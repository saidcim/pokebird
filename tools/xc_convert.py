#!/usr/bin/env python3
"""
xc_convert.py — convert the downloaded Xeno-canto mp3s to the device's format.

    mp3 (various rates/channels)  ->  24 kHz mono 16-bit WAV

WHY: the device's audio path is 24 kHz mono (board_config.h, PB_SAMPLE_RATE).
The training data has to be in the same format, otherwise a silent mismatch
sits between training and inference. Doing the conversion now rather than
later also halves the disk usage (measured: 26.6 GB of mp3 -> 13.6 GB of WAV;
the recordings average 39 s, which is why WAV comes out smaller — for long
recordings it would be the other way round).

At most `--count` recordings are kept per species and the rest are DISCARDED.
It keeps the first N in the order Xeno-canto returned them (that order is by
quality and relevance), not the longest ones.

Converted mp3s are DELETED by default (`--mp3-keep` preserves them) to stop
the disk filling up. The source file is still on Xeno-canto and can be
downloaded again if needed.

    python tools/xc_convert.py --count 40
"""

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
XC_DIR = os.path.join(DATA, "xc")
WAV_DIR = os.path.join(DATA, "wav")
SAMPLE_RATE = 24000


def have_ffmpeg():
    return shutil.which("ffmpeg") is not None


def convert(job):
    mp3, wav, delete_mp3 = job
    os.makedirs(os.path.dirname(wav), exist_ok=True)
    p = subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", mp3,
         "-ac", "1", "-ar", str(SAMPLE_RATE), "-c:a", "pcm_s16le", wav],
        capture_output=True, text=True)
    if p.returncode != 0:
        return (mp3, False, (p.stderr or "").strip()[:120])
    if delete_mp3:
        try:
            os.remove(mp3)
        except OSError:
            pass
    return (mp3, True, "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=40,
                    help="recordings to keep per species (default 40)")
    ap.add_argument("--mp3-keep", action="store_true",
                    help="do not delete converted mp3s (doubles disk usage)")
    ap.add_argument("--threads", type=int, default=4,
                    help="number of concurrent ffmpeg processes")
    args = ap.parse_args()

    if not have_ffmpeg():
        sys.exit("[!] ffmpeg not found. https://ffmpeg.org/download.html")
    if not os.path.isdir(XC_DIR):
        sys.exit(f"[!] {XC_DIR} does not exist. First run: "
                 f"python tools/xc_fetch.py --download")

    jobs, discarded = [], 0
    for species in sorted(os.listdir(XC_DIR)):
        species_dir = os.path.join(XC_DIR, species)
        if not os.path.isdir(species_dir):
            continue
        mp3s = sorted(f for f in os.listdir(species_dir) if f.endswith(".mp3"))

        # Count what is already converted, so this can be re-run safely.
        wav_species = os.path.join(WAV_DIR, species)
        existing = len([f for f in os.listdir(wav_species)
                        if f.endswith(".wav")]) if os.path.isdir(wav_species) else 0

        room = max(0, args.count - existing)
        for f in mp3s[:room]:
            jobs.append((os.path.join(species_dir, f),
                         os.path.join(wav_species, f[:-4] + ".wav"),
                         not args.mp3_keep))
        # mp3s beyond the quota: they will not be converted, so do not let
        # them take up space.
        for f in mp3s[room:]:
            if not args.mp3_keep:
                try:
                    os.remove(os.path.join(species_dir, f))
                    discarded += 1
                except OSError:
                    pass

    if discarded:
        print(f"deleted {discarded:,} surplus mp3s "
              f"(quota {args.count} per species)")
    if not jobs:
        print("No new files to convert.")
        return

    print(f"converting {len(jobs):,} files -> 24 kHz mono WAV\n", flush=True)
    ok = error = 0
    with ThreadPoolExecutor(max_workers=args.threads) as ex:
        for i, (mp3, success, message) in enumerate(ex.map(convert, jobs), 1):
            if success:
                ok += 1
            else:
                error += 1
                print(f"  [!] {os.path.basename(mp3)}: {message}")
            if i % 250 == 0 or i == len(jobs):
                print(f"  {i:,}/{len(jobs):,}", flush=True)

    # Remove species directories that are now empty
    for species in os.listdir(XC_DIR):
        d = os.path.join(XC_DIR, species)
        if os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)

    size = sum(os.path.getsize(os.path.join(r, f))
               for r, _, fs in os.walk(WAV_DIR) for f in fs if f.endswith(".wav"))
    count = sum(1 for r, _, fs in os.walk(WAV_DIR) for f in fs if f.endswith(".wav"))
    print(f"\nConverted {ok:,}, errors {error}")
    print(f"{WAV_DIR}: {count:,} WAVs, {size/1e9:.1f} GB")


if __name__ == "__main__":
    sys.exit(main())
