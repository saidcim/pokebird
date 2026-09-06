#!/usr/bin/env python3
"""
birdnet_run.py — data/wav altindaki her kaydi BirdNET ile 3 sn'lik dilimlere
ayirip tur skorlarini yazar. (M4 adim 3)

    .venv-birdnet\\Scripts\\python -u tools/birdnet_run.py --isci 10

BU BETIK VENV'IN PYTHON'UYLA CALISIR. Makinedeki varsayilan Python 3.14,
BirdNET-Analyzer 3.11 istiyor; ayri sanal ortam bu yuzden var.

--------------------------------------------------------------------------
NEDEN --slist YOK
--------------------------------------------------------------------------
Plan 178 turluk bir liste vermeyi ongoruyordu. Kurulu surumde tur listesi
filtresi CIKARIMDAN SONRA uygulaniyor (analyze/utils.py:689) — yani liste
vermek hicbir hiz kazandirmiyor, sadece satir eliyor. Listesiz calistirmak
ayni surede daha cok bilgi birakiyor; ilk olcum kayitta bunu dogruladi:

    0.0-3.0  Engine                   0.2877   <- kus disi sinif
    3.0-5.4  Corvus cornix  (hedef)   0.5201
    3.0-5.4  Corvus corone  (akraba)  0.4368   <- karisma sinyali

`Engine` gibi kus disi siniflar Asama-1 kapisi ve negatif madenciligi icin
dogrudan degerli; akraba tur skoru da bulasik dilimi ayiklamaya yariyor.
178 ture suzme tools/birdnet_summary.py'de yapiliyor.

--------------------------------------------------------------------------
NEDEN --min_conf 0.1
--------------------------------------------------------------------------
Varsayilan 0.25. Bizim isimiz yalnizca "hangi dilim" degil; M5'te damitma
(distillation) icin BirdNET'in YUMUSAK skorlari ogretmen sinyali olacak.
Dusuk guvenli dilim de bilgi tasir; esigi egitimde yukseltmek kolay, atilan
veriyi geri getirmek icin 79 saatlik analizi tekrarlamak gerekir.

--------------------------------------------------------------------------
!! BIRDNET'IN KENDI SUREC HAVUZU KULLANILMIYOR — KILITLENIYOR
--------------------------------------------------------------------------
`analyze(threads=14)` iceride multiprocessing.Pool aciyor. Bu makinede
(Windows + TensorFlow) 40 dosyalik bir turde 37 dosyadan sonra KILITLENDI:
16 surec ayakta, CPU 962 sn'de sabit, kalan 3 dosya hic islenmedi. Ayni uc
dosya threads=1 ile sorunsuz bitti (21.4 / 0.7 / 10.9 sn), yani dosyalarda
sorun yok — havuzda var.

Bu yuzden paralellik BURADA kuruluyor: N bagimsiz surec, her biri kendi tur
kumesini threads=1 ile isliyor. Surecler birbirinden habersiz oldugu icin
kilitlenecek ortak nokta kalmiyor; biri olurse digerleri devam eder ve
gunlugunde sebebi gorunur (M4'te ogrenilen ders: sebebi gorunmeyen arka plan
isi zaman kaybettiriyor).

--------------------------------------------------------------------------
YENIDEN BASLATILABILIR
--------------------------------------------------------------------------
skip_existing_results=True: sonucu olan dosya atlanir. Yarida kesilirse ayni
komut kaldigi yerden devam eder.
"""

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
WAV_DIR = os.path.join(DATA, "wav")
RESULT_DIR = os.path.join(DATA, "birdnet_sonuc")
LOG_DIR = os.path.join(DATA, "birdnet_log")


def species_boyutlari(root):
    """tur -> toplam bayt. Is dagitimini dosya sayisina degil sese gore yap."""
    d = {}
    for t in sorted(os.listdir(root)):
        p = os.path.join(root, t)
        if os.path.isdir(p):
            d[t] = sum(
                os.path.getsize(os.path.join(p, f)) for f in os.listdir(p)
            )
    return d


def kumelere_bol(boyutlar, n):
    """En buyukten baslayip en bos kumeye at (LPT) — kumeler dengeli biter."""
    kumeler = [[] for _ in range(n)]
    yuk = [0] * n
    for t, b in sorted(boyutlar.items(), key=lambda x: -x[1]):
        i = yuk.index(min(yuk))
        kumeler[i].append(t)
        yuk[i] += b
    return kumeler, yuk


def result_count(root, species):
    """Yalnizca kapsamdaki turlerin sonuc CSV'lerini say.

    Dizindeki her seyi saymak yaniltiyor: BirdNET her cikti dizinine bir de
    analiz parametre dosyasi yaziyor, ayrica onceki kosulardan baska turlerin
    sonuclari duruyor. Ilk denemede sayac 120 hedefe karsi 164 gostermisti.
    """
    n = 0
    for t in species:
        d = os.path.join(root, t)
        if os.path.isdir(d):
            n += sum(1 for f in os.listdir(d) if f.endswith(".BirdNET.results.csv"))
    return n


def workers_calis(species, inp_root, output_root, min_conf, batch):
    from birdnet_analyzer.analyze.core import analyze

    for i, t in enumerate(species, 1):
        g = os.path.join(inp_root, t)
        c = os.path.join(output_root, t)
        os.makedirs(c, exist_ok=True)
        n = len(os.listdir(g))
        t0 = time.time()
        analyze(
            g,
            c,
            min_conf=min_conf,
            overlap=0.0,
            rtype="csv",
            merge_consecutive=1,  # 1 = birlestirme KAPALI, her 3 sn ayri satir
            skip_existing_results=True,
            threads=1,  # havuz yok, bkz. yukaridaki not
            batch_size=batch,
            combine_results=False,
        )
        print(
            f"[{i}/{len(species)}] {t}  {n} dosya  {time.time() - t0:.0f} sn",
            flush=True,
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inp", default=WAV_DIR)
    ap.add_argument("--out", default=RESULT_DIR)
    ap.add_argument("--min-conf", type=float, default=0.1)
    ap.add_argument("--batch", type=int, default=8, help="batch_size")
    ap.add_argument("--workers", type=int, default=10, help="es zamanli surec")
    ap.add_argument("--species", nargs="*", help="yalnizca bu ebird kodlari")
    ap.add_argument("--sub-process", action="store_true", help="ic kullanim")
    a = ap.parse_args()

    if not os.path.isdir(a.inp):
        sys.exit(f"girdi dizini yok: {a.inp}")

    boyutlar = species_boyutlari(a.inp)
    if a.species:
        boyutlar = {t: boyutlar[t] for t in a.species}

    # --- isci kipi: verilen turleri tek surecte, tek is parcaciginda isle ---
    if a.sub_process or a.workers <= 1:
        workers_calis(sorted(boyutlar), a.inp, a.out, a.min_conf, a.batch)
        return

    # --- ana kip: isi bol, N alt surec baslat, ilerlemeyi bildir ---
    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    kumeler, yuk = kumelere_bol(boyutlar, a.workers)
    target = sum(len(os.listdir(os.path.join(a.inp, t))) for t in boyutlar)

    print(f"{len(boyutlar)} tur, {target} dosya, {sum(yuk) / 2**30:.1f} GB ses")
    print(f"{a.workers} isci; kume basina {min(yuk) / 2**30:.1f}-{max(yuk) / 2**30:.1f} GB")

    proc, loglar = [], []
    for i, k in enumerate(kumeler):
        if not k:
            continue
        log = os.path.join(LOG_DIR, f"isci_{i}.log")
        loglar.append(log)
        f = open(log, "w", encoding="utf-8")
        proc.append(
            (
                subprocess.Popen(
                    [sys.executable, "-u", os.path.abspath(__file__),
                     "--alt-surec", "--girdi", a.inp, "--out", a.out,
                     "--min-conf", str(a.min_conf), "--yigin", str(a.batch),
                     "--tur", *k],
                    stdout=f, stderr=subprocess.STDOUT,
                ),
                f,
            )
        )
    print(f"gunlukler: {LOG_DIR}\\isci_*.log", flush=True)

    t0 = time.time()
    # Hiz, bu kosuda EKLENEN sonuclardan hesaplaniyor: yarida kesilmis bir isi
    # surdururken hazir sonuclari hiza saymak kalan sureyi sifira yuvarlardi.
    basla = result_count(a.out, boyutlar)
    while True:
        biten = sum(1 for p, _ in proc if p.poll() is not None)
        n = result_count(a.out, boyutlar)
        gecen = time.time() - t0
        rate = (n - basla) / gecen if gecen > 0 else 0
        kalan = (target - n) / rate / 60 if rate > 0 else 0
        print(
            f"{time.strftime('%H:%M:%S')}  {n}/{target} sonuc  "
            f"{rate * 60:.0f} dosya/dk  kalan ~{kalan:.0f} dk  "
            f"biten isci {biten}/{len(proc)}",
            flush=True,
        )
        if biten == len(proc):
            break
        time.sleep(60)

    for p, f in proc:
        f.close()
    code = [p.returncode for p, _ in proc]
    print(
        f"\nbitti: {result_count(a.out, boyutlar)}/{target} sonuc, "
        f"{(time.time() - t0) / 60:.0f} dk"
    )
    if any(code):
        print(f"!! sifir olmayan cikis kodlari: {code} — gunluklere bakin")


if __name__ == "__main__":
    main()
