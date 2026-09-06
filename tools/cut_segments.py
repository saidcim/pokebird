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

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")


def cut(source, target, start, end):
    with wave.open(source, "rb") as g:
        rate = g.getframerate()
        g.setpos(min(int(start * rate), g.getnframes()))
        data = g.readframes(int((end - start) * rate))
        with wave.open(target, "wb") as c:
            c.setnchannels(g.getnchannels())
            c.setsampwidth(g.getsampwidth())
            c.setframerate(rate)
            c.writeframes(data)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--segments", default=os.path.join(DATA, "segmentler.csv"))
    ap.add_argument("--wav", default=os.path.join(DATA, "wav"))
    ap.add_argument("--out", default=os.path.join(DATA, "segment_ornek"))
    ap.add_argument("--species", nargs="*", help="yalnizca bu ebird kodlari")
    ap.add_argument("--threshold", type=float, default=0.5, help="en az hedef_guven")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--seed", type=int, default=0, help="tekrarlanabilirlik")
    a = ap.parse_args()

    with open(a.segments, encoding="utf-8") as f:
        row = [
            r for r in csv_compat.reader(f)
            if float(r["target_confidence"]) >= a.threshold
            and (not a.species or r["ebird_code"] in a.species)
        ]
    if not row:
        raise SystemExit(f"esik {a.threshold} ustunde dilim yok")

    random.seed(a.seed)
    selection = random.sample(row, min(a.count, len(row)))
    os.makedirs(a.out, exist_ok=True)

    print(f"{len(row)} uygun dilimden {len(selection)} tanesi kesiliyor -> {a.out}\n")
    for r in sorted(selection, key=lambda x: (x["ebird_code"], x["file"])):
        code, file = r["ebird_code"], r["file"]
        start, end = float(r["start"]), float(r["end"])
        source = os.path.join(a.wav, code, file + ".wav")
        name = f"{code}_{file}_{start:06.1f}.wav"
        cut(source, os.path.join(a.out, name), start, end)
        print(
            f"{name}\n    hedef {float(r['hedef_guven']):.2f}   "
            f"en iyi: {r['en_iyi_tur']} {float(r['en_iyi_guven']):.2f}"
            + (f"   kus disi: {r['kus_disi_tur']}" if r["non_bird_species"] else "")
        )


if __name__ == "__main__":
    main()
