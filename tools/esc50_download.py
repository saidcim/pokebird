#!/usr/bin/env python3
"""
esc50_download.py — the audio source for the negative class: download ESC-50,
weed the birds out, convert to 24 kHz mono WAV.

    python tools/esc50_download.py
    python tools/esc50_download.py --only-convert   # if the zip is already in

Output: data/negative/wav/<category>/<file>.wav  (24 kHz mono 16-bit, 5 s)
        data/negative/esc50_records.csv          — licence/attribution +
                                                   category

--------------------------------------------------------------------------
WHY THIS WORK WAS NOT PUT OFF
--------------------------------------------------------------------------
COLLECTING negatives out in the field (the call to prayer, a ferry, a simit
seller) was deferred by the user's own decision. But the negative CLASS was
not deferred: if nothing is put into stage 1's "not a bird" and stage 2's
"unknown" class, the model assigns every sound to some bird. The number of
non-bird slices our own XC recordings yield was MEASURED: 204 out of 95,033.
Not enough. ESC-50 is a *download*, not a field trip.

--------------------------------------------------------------------------
!! THE chirping_birds CLASS IS REMOVED
--------------------------------------------------------------------------
One of ESC-50's 50 classes is bird song. If a bird gets into the negatives,
stage 1 learns to reject a real bird — a silent and expensive mistake. This
script drops that class; it is also worth running what remains through
BirdNET:

    .venv-birdnet\\Scripts\\python tools/birdnet_run.py \\
        --inp data/negative/wav --out data/negative/birdnet_result

If tools/build_dataset.py finds those results, it also drops the clips where
a bird was heard.

Licence: ESC-50 is CC BY-NC 3.0 (K. J. Piczak). Compatible with personal use
but not with commercial distribution — the same class of restriction as
BirdNET and Xeno-canto.
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

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEG = csv_compat.resolve(os.path.join(ROOT, "data", "negative"))
ZIP = os.path.join(NEG, "ESC-50-master.zip")
URL = "https://github.com/karolpiczak/ESC-50/archive/refs/heads/master.zip"

# !! CLASSES THAT CONTAIN BIRD SOUND - these must not go into the negatives.
#
# The plan named only `chirping_birds`. MEASURED, and that is not enough:
# running the 1,960 clips through BirdNET, one of our own 178 species was
# heard at >=0.25 confidence in 51 clips, and 26 of those came from the
# `crow` class - the highest being Corvus frugilegus at 1.00 and Pica pica at
# 0.98. ESC-50's "crow" class IS one of our TARGET species. Putting it in the
# negatives would teach the model to reject a crow. hen/rooster are bird-like
# too: one of our 178 fired on three of them.
EXCLUDED = {"chirping_birds", "crow", "hen", "rooster"}
TARGET_SR = 24000


def download(url, target, retries=5):
    """A resumable download.

    The lesson from earlier: do not trust a single request to pull a large
    file down, the server can drop it halfway without reporting an error. We
    resume with a Range header.
    """
    partial = target + ".part"
    for attempt in range(1, retries + 1):
        have = os.path.getsize(partial) if os.path.exists(partial) else 0
        req = urllib.request.Request(url,
                                     headers={"User-Agent": "pokebird/1.0"})
        if have:
            req.add_header("Range", f"bytes={have}-")
        try:
            with urllib.request.urlopen(req, timeout=60) as y:
                total = int(y.headers.get("Content-Length") or 0) + have
                mode = "ab" if have and y.status == 206 else "wb"
                if mode == "wb":
                    have = 0
                t0, last = time.time(), time.time()
                with open(partial, mode) as f:
                    while True:
                        block = y.read(1 << 20)
                        if not block:
                            break
                        f.write(block)
                        have += len(block)
                        if time.time() - last > 3:
                            last = time.time()
                            rate = have / max(time.time() - t0, 1e-6) / 1e6
                            percent = (f" {100 * have / total:.0f}%"
                                       if total else "")
                            print(f"  {have / 1e6:7.1f} MB{percent}  "
                                  f"{rate:.1f} MB/s", flush=True)
            if total and have < total:
                print(f"  came up short ({have}/{total}), resuming", flush=True)
                continue
            os.replace(partial, target)
            return
        except Exception as e:
            print(f"  attempt {attempt}/{retries} was cut off: {e}", flush=True)
            time.sleep(3)
    sys.exit("the download failed - check the network and run again "
             "(the file resumes where it left off)")


def convert(source, target):
    """44.1 kHz -> 24 kHz mono 16-bit. The SAME chain as the bird WAVs
    (ffmpeg)."""
    r = subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-i", source,
         "-ac", "1", "-ar", str(TARGET_SR), "-sample_fmt", "s16", target],
        capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only-convert", action="store_true",
                    help="the zip is already here; only extract and convert")
    ap.add_argument("--zip-delete", action="store_true",
                    help="delete the zip once conversion is done (tight disk)")
    a = ap.parse_args()

    os.makedirs(NEG, exist_ok=True)

    if not a.only_convert or not os.path.exists(ZIP):
        if os.path.exists(ZIP):
            print(f"the zip is already here: {ZIP} "
                  f"({os.path.getsize(ZIP) / 1e6:.0f} MB)")
        else:
            print(f"downloading ESC-50 -> {ZIP}")
            download(URL, ZIP)
            print(f"  done: {os.path.getsize(ZIP) / 1e6:.0f} MB")

    # ---- metadata + audio ----
    raw = os.path.join(NEG, "raw")
    os.makedirs(raw, exist_ok=True)
    with zipfile.ZipFile(ZIP) as z:
        names = z.namelist()
        meta = next(n for n in names if n.endswith("meta/esc50.csv"))
        with z.open(meta) as f:
            row = list(csv_compat.reader(l.decode("utf-8") for l in f))
        wavs = [n for n in names if n.endswith(".wav") and "/audio/" in n]
        print(f"{len(wavs)} wavs in the zip, {len(row)} metadata rows")

        by_name = {r["filename"]: r for r in row}
        selected = []
        for n in wavs:
            name = os.path.basename(n)
            r = by_name.get(name)
            if r is None:
                print(f"!! not in the metadata, skipped: {name}")
                continue
            if r["category"] in EXCLUDED:
                continue
            selected.append((n, name, r))

        dropped = len(wavs) - len(selected)
        print(f"dropped (bird): {dropped}  /  kept: {len(selected)}")
        if dropped == 0:
            sys.exit("!! nothing was filtered out at all - the category name "
                     "may have changed, which would let birds into the "
                     "negatives. Stopped.")

        for n, name, r in selected:
            h = os.path.join(raw, name)
            if not os.path.exists(h):
                with z.open(n) as src, open(h, "wb") as dst:
                    shutil.copyfileobj(src, dst)

    # ---- conversion ----
    target_root = os.path.join(NEG, "wav")
    # Clean out bird-class directories left over from an earlier run: if
    # EXCLUDED has grown, the old files stay on disk and, though they no
    # longer appear in esc50_records.csv, they are confusing.
    for k in EXCLUDED:
        d = os.path.join(target_root, k)
        if os.path.isdir(d):
            shutil.rmtree(d)
            print(f"removed a bird class left over from an earlier run: {k}")
    record = []
    converted = error = skipped = 0
    for i, (_, name, r) in enumerate(selected, 1):
        d = os.path.join(target_root, r["category"])
        os.makedirs(d, exist_ok=True)
        target = os.path.join(d, name)
        if os.path.exists(target) and os.path.getsize(target) > 1000:
            skipped += 1
        else:
            ok, err = convert(os.path.join(raw, name), target)
            if ok:
                converted += 1
            else:
                error += 1
                print(f"!! could not convert {name}: {err}")
                continue
        record.append({
            "file": os.path.relpath(target, ROOT),
            "category": r["category"],
            "esc50_file": name,
            # the fold and the source file are needed to PREVENT LEAKAGE:
            # several clips are cut from the same Freesound recording. The 5
            # folds ESC-50 ships were separated for exactly this; split along
            # them.
            "fold": r.get("fold", ""),
            "source": r.get("src_file", ""),
            "licence": "CC BY-NC 3.0 (ESC-50, K. J. Piczak)",
        })
        if i % 200 == 0:
            print(f"  {i}/{len(selected)}", flush=True)

    with open(os.path.join(NEG, "esc50_records.csv"), "w",
              encoding="utf-8", newline="") as f:
        y = csv.DictWriter(f, fieldnames=list(record[0].keys()))
        y.writeheader()
        y.writerows(record)

    # ---- sanity check: is the format really 24 kHz mono ----
    import wave
    import random
    sample = random.Random(0).sample(record, min(20, len(record)))
    correct = 0
    for k in sample:
        with wave.open(os.path.join(ROOT, k["file"]), "rb") as w:
            if ((w.getframerate(), w.getnchannels(), w.getsampwidth())
                    == (TARGET_SR, 1, 2)):
                correct += 1

    print(f"\nconverted {converted} / already there {skipped} / "
          f"errors {error}")
    print(f"format check : {correct}/{len(sample)} correct "
          "(24 kHz mono 16-bit)")
    print(f"categories   : {len(set(k['category'] for k in record))}")
    print(f"-> {target_root}")
    if correct != len(sample):
        sys.exit("!! the format check failed")

    shutil.rmtree(raw, ignore_errors=True)
    if a.zip_delete and os.path.exists(ZIP):
        os.remove(ZIP)
        print("the zip was deleted")


if __name__ == "__main__":
    main()
