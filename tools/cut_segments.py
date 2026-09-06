#!/usr/bin/env python3
"""
segment_kes.py — segmentler.csv'deki dilimleri dinlenebilir WAV'lara kes.

    python tools/cut_segments.py --adet 10            # rastgele 10 dilim
    python tools/cut_segments.py --tur eurbla --adet 5

NEDEN VAR: segmentasyonun dogru calistigina dair TEK dogrudan gozlem
dinlemektir. Bu projede dolayli olcume fazla guvenmek iki kez pahaliya
patladi (lastsession §5.10) — TE hattini dinleyip "panel sagir" demek ve
dokunmatigi kimse ekrana dokunmadan test etmek. Segmentasyonda ayni hataya
dusmemek icin ornekler kesilip DINLENMELI.

Tum dilimleri kesmek icin degil: ~10 GB eder (bkz. birdnet_ozet.py). Egitim
dilimleri dogrudan orijinal WAV'dan ofsetle okuyacak.

Ses 24 kHz mono 16-bit oldugu icin kesme stdlib `wave` ile yapiliyor;
ffmpeg gerekmiyor, yeniden kodlama yok — baytlar birebir kopyalaniyor.
"""

import argparse
import csv
import os
import random
import wave

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(KOK, "data")


def kes(kaynak, hedef, bas, bit):
    with wave.open(kaynak, "rb") as g:
        hiz = g.getframerate()
        g.setpos(min(int(bas * hiz), g.getnframes()))
        veri = g.readframes(int((bit - bas) * hiz))
        with wave.open(hedef, "wb") as c:
            c.setnchannels(g.getnchannels())
            c.setsampwidth(g.getsampwidth())
            c.setframerate(hiz)
            c.writeframes(veri)
    return len(veri)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--segmentler", default=os.path.join(DATA, "segmentler.csv"))
    ap.add_argument("--wav", default=os.path.join(DATA, "wav"))
    ap.add_argument("--out", default=os.path.join(DATA, "segment_ornek"))
    ap.add_argument("--tur", nargs="*", help="yalnizca bu ebird kodlari")
    ap.add_argument("--esik", type=float, default=0.5, help="en az hedef_guven")
    ap.add_argument("--adet", type=int, default=10)
    ap.add_argument("--tohum", type=int, default=0, help="tekrarlanabilirlik")
    a = ap.parse_args()

    with open(a.segmentler, encoding="utf-8") as f:
        satir = [
            r for r in csv.DictReader(f)
            if float(r["hedef_guven"]) >= a.esik
            and (not a.tur or r["ebird_kodu"] in a.tur)
        ]
    if not satir:
        raise SystemExit(f"esik {a.esik} ustunde dilim yok")

    random.seed(a.tohum)
    secim = random.sample(satir, min(a.adet, len(satir)))
    os.makedirs(a.out, exist_ok=True)

    print(f"{len(satir)} uygun dilimden {len(secim)} tanesi kesiliyor -> {a.out}\n")
    for r in sorted(secim, key=lambda x: (x["ebird_kodu"], x["dosya"])):
        kod, dosya = r["ebird_kodu"], r["dosya"]
        bas, bit = float(r["baslangic"]), float(r["bitis"])
        kaynak = os.path.join(a.wav, kod, dosya + ".wav")
        ad = f"{kod}_{dosya}_{bas:06.1f}.wav"
        kes(kaynak, os.path.join(a.out, ad), bas, bit)
        print(
            f"{ad}\n    hedef {float(r['hedef_guven']):.2f}   "
            f"en iyi: {r['en_iyi_tur']} {float(r['en_iyi_guven']):.2f}"
            + (f"   kus disi: {r['kus_disi_tur']}" if r["kus_disi_tur"] else "")
        )


if __name__ == "__main__":
    main()
