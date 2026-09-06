#!/usr/bin/env python3
"""
cut_segments.py — cut the slices listed in segments.csv into playable WAVs.

    python tools/cut_segments.py --count 10           # 10 random slices
    python tools/cut_segments.py --species eurbla --count 5

WHY THIS EXISTS: the ONLY direct evidence that the segmentation works is
listening to it. Trusting indirect measurement too far has cost this project
dearly twice — declaring the panel deaf from the TE line alone, and testing
touch without anyone ever touching the screen. To avoid the same mistake in
segmentation, samples must be cut and HEARD.

It is not meant for cutting every slice: that would be about 10 GB (see
birdnet_summary.py). Training reads its slices straight from the original WAV
at an offset.

Because the audio is 24 kHz mono 16-bit, the cutting is done with the stdlib
`wave` module — no ffmpeg, no re-encoding, the bytes are copied through
unchanged.
"""

import argparse
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
    ap.add_argument("--segments", default=os.path.join(DATA, "segments.csv"))
    ap.add_argument("--wav", default=os.path.join(DATA, "wav"))
    ap.add_argument("--out", default=os.path.join(DATA, "segment_samples"))
    ap.add_argument("--species", nargs="*", help="only these eBird codes")
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="minimum target confidence")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--seed", type=int, default=0, help="for reproducibility")
    a = ap.parse_args()

    with open(csv_compat.resolve(a.segments), encoding="utf-8") as f:
        row = [
            r for r in csv_compat.reader(f)
            if float(r["target_confidence"]) >= a.threshold
            and (not a.species or r["ebird_code"] in a.species)
        ]
    if not row:
        raise SystemExit(f"no slices above threshold {a.threshold}")

    random.seed(a.seed)
    selection = random.sample(row, min(a.count, len(row)))
    os.makedirs(a.out, exist_ok=True)

    print(f"cutting {len(selection)} of {len(row)} eligible slices -> {a.out}\n")
    for r in sorted(selection, key=lambda x: (x["ebird_code"], x["file"])):
        code, file = r["ebird_code"], r["file"]
        start, end = float(r["start"]), float(r["end"])
        source = os.path.join(a.wav, code, file + ".wav")
        name = f"{code}_{file}_{start:06.1f}.wav"
        cut(source, os.path.join(a.out, name), start, end)
        print(
            f"{name}\n    target {float(r['target_confidence']):.2f}   "
            f"best: {r['best_species']} {float(r['best_confidence']):.2f}"
            + (f"   non-bird: {r['non_bird_species']}"
               if r["non_bird_species"] else "")
        )


if __name__ == "__main__":
    main()
