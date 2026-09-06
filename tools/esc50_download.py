#!/usr/bin/env python3
"""
esc50_indir.py — Negatif sinifin ses kaynagi: ESC-50 indir, kusu ayikla,
24 kHz mono WAV'a cevir. (M4 adim 4'un on kosulu, lastsession.md §9f-4)

    python tools/esc50_download.py
    python tools/esc50_download.py --sadece-cevir     # zip zaten indiyse

Cikti: data/negatif/wav/<kategori>/<dosya>.wav   (24 kHz mono 16-bit, 5 sn)
       data/negatif/esc50_kayitlar.csv           — lisans/atif + kategori

--------------------------------------------------------------------------
NEDEN BU IS ERTELENMEDI
--------------------------------------------------------------------------
Negatif TOPLAMA (saha turu: ezan, vapur, simitci) kullanici karariyla M8'e
ertelendi. Ama negatif SINIFI ertelenmedi: Asama-1'in "kus degil" ve
Asama-2'nin "bilinmiyor" sinifina hicbir sey konmazsa model her sesi bir
kusa atar. Kendi XC kayitlarimizdan cikan kus disi dilim sayisi OLCULDU:
95.033 icinde 204. Yetmiyor. ESC-50 bir *indirme*, saha turu degil.

--------------------------------------------------------------------------
!! chirping_birds SINIFI CIKARILIYOR
--------------------------------------------------------------------------
ESC-50'nin 50 sinifindan biri kus sesi. Negatife kus karisirsa Asama-1
gercek kusu reddetmeyi ogrenir — sessiz ve pahali bir hata. Bu betik o
sinifi cikariyor; ayrica kalan kliplerin BirdNET'ten gecirilmesi onerilir:

    .venv-birdnet\\Scripts\\python tools/birdnet_run.py \\
        --girdi data/negatif/wav --out data/negatif/birdnet_sonuc

tools/build_dataset.py o sonuclari bulursa kus duyulan klipleri de eler.

Lisans: ESC-50 CC BY-NC 3.0 (K. J. Piczak). Kisisel kullanimla uyumlu,
ticari dagitimla degil — BirdNET ve XC ile ayni siniftan kisit.
"""

import argparse
import csv
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEG = os.path.join(KOK, "data", "negatif")
ZIP = os.path.join(NEG, "ESC-50-master.zip")
URL = "https://github.com/karolpiczak/ESC-50/archive/refs/heads/master.zip"

# !! KUS SESI ICEREN SINIFLAR — negatife girmemeli.
#
# Plan yalnizca `chirping_birds`i soyluyordu. OLCULDU, yetmiyor: 1960 klip
# BirdNET'ten gecirildiginde 51 klipte kendi 178 turumuzden biri >=0.25
# guvenle duyuldu ve 26'si `crow` sinifindan cikti — en yuksekleri Corvus
# frugilegus 1.00 ve Pica pica 0.98. Yani ESC-50'nin "karga" sinifi bizim
# HEDEF turlerimiz. Negatife konsaydi model kargayi reddetmeyi ogrenirdi.
# hen/rooster de bird-like: uclerinde 178 turumuzden biri tetiklendi.
CIKARILAN = {"chirping_birds", "crow", "hen", "rooster"}
HEDEF_SR = 24000


def indir(url, hedef, tekrar=5):
    """Kaldigi yerden devam eden indirme.

    §5.14'un dersi: buyuk dosyayi tek istekle cekmeye guvenmeyin, sunucu
    ortada birakabiliyor ve hata vermiyor. Range ile devam ediyoruz.
    """
    gecici = hedef + ".parca"
    for deneme in range(1, tekrar + 1):
        var = os.path.getsize(gecici) if os.path.exists(gecici) else 0
        istek = urllib.request.Request(url, headers={"User-Agent": "pokebird/1.0"})
        if var:
            istek.add_header("Range", f"bytes={var}-")
        try:
            with urllib.request.urlopen(istek, timeout=60) as y:
                toplam = int(y.headers.get("Content-Length") or 0) + var
                kip = "ab" if var and y.status == 206 else "wb"
                if kip == "wb":
                    var = 0
                t0, son = time.time(), time.time()
                with open(gecici, kip) as f:
                    while True:
                        blok = y.read(1 << 20)
                        if not blok:
                            break
                        f.write(blok)
                        var += len(blok)
                        if time.time() - son > 3:
                            son = time.time()
                            hiz = var / max(time.time() - t0, 1e-6) / 1e6
                            yuzde = f" %{100 * var / toplam:.0f}" if toplam else ""
                            print(f"  {var / 1e6:7.1f} MB{yuzde}  {hiz:.1f} MB/s",
                                  flush=True)
            if toplam and var < toplam:
                print(f"  eksik kaldi ({var}/{toplam}), devam ediliyor", flush=True)
                continue
            os.replace(gecici, hedef)
            return
        except Exception as e:
            print(f"  deneme {deneme}/{tekrar} kesildi: {e}", flush=True)
            time.sleep(3)
    sys.exit("indirme basarisiz — agi kontrol edip tekrar calistirin "
             "(dosya kaldigi yerden devam eder)")


def cevir(kaynak, hedef):
    """44.1 kHz -> 24 kHz mono 16-bit. Kus WAV'lariyla AYNI zincir (ffmpeg)."""
    r = subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-i", kaynak,
         "-ac", "1", "-ar", str(HEDEF_SR), "-sample_fmt", "s16", hedef],
        capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sadece-cevir", action="store_true",
                    help="zip elde, yalnizca cikar ve cevir")
    ap.add_argument("--zip-sil", action="store_true",
                    help="cevrim bitince zip'i sil (disk dar)")
    a = ap.parse_args()

    os.makedirs(NEG, exist_ok=True)

    if not a.sadece_cevir or not os.path.exists(ZIP):
        if os.path.exists(ZIP):
            print(f"zip zaten var: {ZIP} ({os.path.getsize(ZIP) / 1e6:.0f} MB)")
        else:
            print(f"ESC-50 indiriliyor -> {ZIP}")
            indir(URL, ZIP)
            print(f"  bitti: {os.path.getsize(ZIP) / 1e6:.0f} MB")

    # ---- meta + ses ----
    ham = os.path.join(NEG, "ham")
    os.makedirs(ham, exist_ok=True)
    with zipfile.ZipFile(ZIP) as z:
        adlar = z.namelist()
        meta = next(n for n in adlar if n.endswith("meta/esc50.csv"))
        with z.open(meta) as f:
            satir = list(csv.DictReader(l.decode("utf-8") for l in f))
        wavlar = [n for n in adlar if n.endswith(".wav") and "/audio/" in n]
        print(f"zip icinde {len(wavlar)} wav, meta {len(satir)} satir")

        kategori = {r["filename"]: r for r in satir}
        secilen = []
        for n in wavlar:
            ad = os.path.basename(n)
            r = kategori.get(ad)
            if r is None:
                print(f"!! metada yok, atlandi: {ad}")
                continue
            if r["category"] in CIKARILAN:
                continue
            secilen.append((n, ad, r))

        atilan = len(wavlar) - len(secilen)
        print(f"cikarilan (kus): {atilan}  ·  kalan: {len(secilen)}")
        if atilan == 0:
            sys.exit("!! chirping_birds hic elenmedi — kategori adi degismis "
                     "olabilir, negatife kus karisir. Durduruldu.")

        for n, ad, r in secilen:
            h = os.path.join(ham, ad)
            if not os.path.exists(h):
                with z.open(n) as src, open(h, "wb") as dst:
                    shutil.copyfileobj(src, dst)

    # ---- cevrim ----
    hedef_kok = os.path.join(NEG, "wav")
    # Onceki kosudan kalan kus sinifi dizinlerini temizle: CIKARILAN
    # buyuduyse eski dosyalar diskte kalir ve esc50_kayitlar.csv'de
    # gorunmese de kafa karistirir.
    for k in CIKARILAN:
        d = os.path.join(hedef_kok, k)
        if os.path.isdir(d):
            shutil.rmtree(d)
            print(f"onceki kosudan kalan kus sinifi silindi: {k}")
    kayit = []
    cevrilen = hata = atlanan = 0
    for i, (_, ad, r) in enumerate(secilen, 1):
        d = os.path.join(hedef_kok, r["category"])
        os.makedirs(d, exist_ok=True)
        hedef = os.path.join(d, ad)
        if os.path.exists(hedef) and os.path.getsize(hedef) > 1000:
            atlanan += 1
        else:
            ok, err = cevir(os.path.join(ham, ad), hedef)
            if ok:
                cevrilen += 1
            else:
                hata += 1
                print(f"!! cevrilemedi {ad}: {err}")
                continue
        kayit.append({
            "dosya": os.path.relpath(hedef, KOK),
            "kategori": r["category"],
            "esc50_dosya": ad,
            # fold ve kaynak dosya SIZINTIYI ONLEMEK icin lazim: ayni Freesound
            # kaydindan kesilmis birden fazla klip var. ESC-50'nin kendi 5
            # katmani tam bunun icin ayrilmis; bolmeyi ona gore yapin.
            "fold": r.get("fold", ""),
            "kaynak": r.get("src_file", ""),
            "lisans": "CC BY-NC 3.0 (ESC-50, K. J. Piczak)",
        })
        if i % 200 == 0:
            print(f"  {i}/{len(secilen)}", flush=True)

    with open(os.path.join(NEG, "esc50_kayitlar.csv"), "w",
              encoding="utf-8", newline="") as f:
        y = csv.DictWriter(f, fieldnames=list(kayit[0].keys()))
        y.writeheader()
        y.writerows(kayit)

    # ---- sagalama: format gercekten 24 kHz mono mu ----
    import wave
    import random
    ornek = random.Random(0).sample(kayit, min(20, len(kayit)))
    dogru = 0
    for k in ornek:
        with wave.open(os.path.join(KOK, k["dosya"]), "rb") as w:
            if (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (HEDEF_SR, 1, 2):
                dogru += 1

    print(f"\ncevrildi {cevrilen} · zaten vardi {atlanan} · hata {hata}")
    print(f"format sagalamasi: {dogru}/{len(ornek)} dogru (24 kHz mono 16-bit)")
    print(f"kategori sayisi  : {len(set(k['kategori'] for k in kayit))}")
    print(f"-> {hedef_kok}")
    if dogru != len(ornek):
        sys.exit("!! format sagalamasi kaldi")

    shutil.rmtree(ham, ignore_errors=True)
    if a.zip_sil and os.path.exists(ZIP):
        os.remove(ZIP)
        print("zip silindi")


if __name__ == "__main__":
    main()
