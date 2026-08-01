#!/usr/bin/env python3
"""
xc_convert.py — İndirilen Xeno-canto mp3'lerini cihaz formatına çevir.

    mp3 (çeşitli oran/kanal)  ->  24 kHz mono 16-bit WAV

NEDEN: Cihazın ses hattı 24 kHz mono (board_config.h · PB_SAMPLE_RATE).
Eğitim verisi de aynı formatta olmalı, yoksa eğitimle çıkarım arasında sessiz
bir uyumsuzluk kalır. Nasılsa yapılacak dönüşümü şimdi yapmak diski de
yarıya indiriyor (ölçüldü: 26,6 GB mp3 -> 13,6 GB WAV; kayıtların ortalaması
39 sn olduğu için WAV daha küçük çıkıyor — uzun kayıtlarda tersi olurdu).

Tür başına en fazla `--adet` kayıt tutulur, fazlası ATILIR: en uzun kayıtlar
değil, XC'nin döndürdüğü sırayla ilk N tutulur (o sıra kalite/alaka sırası).

Dönüşen mp3 varsayılan olarak SİLİNİR (`--mp3-sakla` ile korunur) — disk
şişmesin diye. Kaynak dosya XC'de duruyor, gerekirse yeniden inebilir.

    python tools/xc_convert.py --adet 40
"""

import argparse
import csv
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(KOK, "data")
XC_DIR = os.path.join(DATA, "xc")
WAV_DIR = os.path.join(DATA, "wav")
ORNEKLEME = 24000


def ffmpeg_var():
    return shutil.which("ffmpeg") is not None


def cevir(is_):
    mp3, wav, mp3_sil = is_
    os.makedirs(os.path.dirname(wav), exist_ok=True)
    p = subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", mp3,
         "-ac", "1", "-ar", str(ORNEKLEME), "-c:a", "pcm_s16le", wav],
        capture_output=True, text=True)
    if p.returncode != 0:
        return (mp3, False, (p.stderr or "").strip()[:120])
    if mp3_sil:
        try:
            os.remove(mp3)
        except OSError:
            pass
    return (mp3, True, "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adet", type=int, default=40,
                    help="tur basina tutulacak kayit (varsayilan 40)")
    ap.add_argument("--mp3-sakla", action="store_true",
                    help="donusen mp3'leri silme (disk iki katina cikar)")
    ap.add_argument("--is-parcacigi", type=int, default=4,
                    help="es zamanli ffmpeg sayisi")
    args = ap.parse_args()

    if not ffmpeg_var():
        sys.exit("[!] ffmpeg bulunamadi. https://ffmpeg.org/download.html")
    if not os.path.isdir(XC_DIR):
        sys.exit(f"[!] {XC_DIR} yok. Once: python tools/xc_fetch.py --indir")

    isler, atilan = [], 0
    for tur in sorted(os.listdir(XC_DIR)):
        tur_yolu = os.path.join(XC_DIR, tur)
        if not os.path.isdir(tur_yolu):
            continue
        mp3ler = sorted(f for f in os.listdir(tur_yolu) if f.endswith(".mp3"))

        # Zaten çevrilmiş olanları say: tekrar çalıştırılabilir olsun.
        wav_tur = os.path.join(WAV_DIR, tur)
        mevcut = len([f for f in os.listdir(wav_tur)
                      if f.endswith(".wav")]) if os.path.isdir(wav_tur) else 0

        yer = max(0, args.adet - mevcut)
        for f in mp3ler[:yer]:
            isler.append((os.path.join(tur_yolu, f),
                          os.path.join(wav_tur, f[:-4] + ".wav"),
                          not args.mp3_sakla))
        # Kotanın üstündeki mp3'ler: çevrilmeyecek, yer kaplamasın.
        for f in mp3ler[yer:]:
            if not args.mp3_sakla:
                try:
                    os.remove(os.path.join(tur_yolu, f))
                    atilan += 1
                except OSError:
                    pass

    if atilan:
        print(f"{atilan:,} fazla mp3 silindi (tur basina kota {args.adet})")
    if not isler:
        print("Cevrilecek yeni dosya yok.")
        return

    print(f"{len(isler):,} dosya cevriliyor -> 24 kHz mono WAV\n", flush=True)
    ok = hata = 0
    with ThreadPoolExecutor(max_workers=args.is_parcacigi) as ex:
        for i, (mp3, basarili, mesaj) in enumerate(ex.map(cevir, isler), 1):
            if basarili:
                ok += 1
            else:
                hata += 1
                print(f"  [!] {os.path.basename(mp3)}: {mesaj}")
            if i % 250 == 0 or i == len(isler):
                print(f"  {i:,}/{len(isler):,}", flush=True)

    # Boşalan tür dizinlerini temizle
    for tur in os.listdir(XC_DIR):
        d = os.path.join(XC_DIR, tur)
        if os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)

    boyut = sum(os.path.getsize(os.path.join(r, f))
                for r, _, fs in os.walk(WAV_DIR) for f in fs if f.endswith(".wav"))
    sayi = sum(1 for r, _, fs in os.walk(WAV_DIR) for f in fs if f.endswith(".wav"))
    print(f"\nCevrildi {ok:,}, hata {hata}")
    print(f"{WAV_DIR}: {sayi:,} WAV, {boyut/1e9:.1f} GB")


if __name__ == "__main__":
    sys.exit(main())
