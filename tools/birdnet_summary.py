#!/usr/bin/env python3
"""
birdnet_summary.py — boil the BirdNET results down into a single slice table
and measure whether it is ready for training.

    python tools/birdnet_summary.py

Input : data/birdnet_result/<ebird_code>/XC*.BirdNET.results.csv
Output: data/segments.csv  — the slice index (the table training reads)
        a summary plus a list of weak species, on screen

--------------------------------------------------------------------------
WHY THE AUDIO IS NOT CUT, ONLY INDEXED
--------------------------------------------------------------------------
The plan was to cut the slices into separate WAVs with
`birdnet_analyzer.segments`. We do not cut them: ~400 slices per species x
178 species x 144 KB is about 10 GB, and only 28 GB of disk was free. The
index (start/end plus score) is a few MB, and the training pipeline can
already extract the mel from whatever offset it wants while reading. The
handful of samples needed to listen and check are cut by
tools/cut_segments.py.

--------------------------------------------------------------------------
WHAT A SLICE HOLDS
--------------------------------------------------------------------------
target_confidence  how confidently the index's species was heard in this
                   slice (0 = it stayed below the threshold; BirdNET never
                   writes anything below 0.1)
best_species       the highest-scoring species in the slice - if that is not
                   the target, the recording is full of another bird at that
                   point (a contaminated slice)
non_bird_*         BirdNET's non-bird classes (Engine, Human vocal, Dog,
                   Siren...). These are valuable for negative mining: the
                   channel match is exact, because they come from the very
                   same recordings.
"""

import argparse
import csv
import os
import sys
import wave
from collections import defaultdict

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
WAV_DIR = os.path.join(DATA, "wav")
RESULT_DIR = csv_compat.resolve(os.path.join(DATA, "birdnet_result"))

# Taken out of the label file: the classes in "Name_Name" form that have no
# scientific name. Gryllus/Miogryllus (crickets) were left out of this set -
# they are a real animal sound, not noise.
NON_BIRD = {
    "Dog", "Engine", "Environmental", "Fireworks", "Gun",
    "Human non-vocal", "Human vocal", "Human whistle",
    "Noise", "Power tools", "Siren",
}

THRESHOLDS = (0.1, 0.25, 0.5)


def species_map(map_path):
    """eBird code -> (the scientific name BirdNET uses, the English name)

    !! DO NOT USE THE NAME FROM OUR OWN CSV. BirdNET is stuck on the old
    genus for two species: the Western Jackdaw is 'Coloeus monedula' for us
    and 'Corvus monedula' in BirdNET; the Alpine Swift is 'Tachymarptis
    melba' for us and 'Apus melba' in BirdNET. Looking the target species up
    under our own name would give those two a confidence of 0 in every slice,
    and both would silently drop out of training.

    The map is produced from the eBird code by tools/birdnet_slist.py.
    """
    if not os.path.exists(map_path):
        sys.exit(
            f"the name map is missing: {map_path}\n"
            "run this first:  .venv-birdnet\\Scripts\\python "
            "tools/birdnet_slist.py"
        )
    with open(map_path, encoding="utf-8") as f:
        return {
            r["ebird_code"]: (r["birdnet_scientific_name"], r["english_name"])
            for r in csv_compat.reader(f)
        }


def wav_duration(path):
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return None


def read_file(path):
    """a result CSV -> {(start, end): [(scientific_name, confidence), ...]}"""
    slice = defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            slice[(float(r["Start (s)"]), float(r["End (s)"]))].append(
                (r["Scientific name"], float(r["Confidence"]))
            )
    return slice


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--result", default=RESULT_DIR)
    ap.add_argument("--wav", default=WAV_DIR)
    ap.add_argument("--name-map",
                    default=os.path.join(DATA, "birdnet_name_map.csv"))
    ap.add_argument("--out", default=os.path.join(DATA, "segments.csv"))
    ap.add_argument(
        "--weak-threshold", type=int, default=100,
        help="species with fewer slices than this are reported as weak"
    )
    a = ap.parse_args()

    if not os.path.isdir(a.result):
        sys.exit(f"the result directory is missing: {a.result} - run "
                 "tools/birdnet_run.py first")

    # code -> (BirdNET scientific name, English name)
    code_name = species_map(a.name_map)

    species = sorted(d for d in os.listdir(a.result)
                    if os.path.isdir(os.path.join(a.result, d)))

    count = {e: defaultdict(int) for e in THRESHOLDS}
    target_best = defaultdict(int)    # slices where the target scored highest
    detected = defaultdict(int)       # slices with at least one detection
    total_slice = defaultdict(int)    # every 3 s slice in the recordings
    non_bird_count = defaultdict(int)
    missing_file = defaultdict(int)
    row = 0

    with open(a.out, "w", encoding="utf-8", newline="") as f:
        y = csv.writer(f)
        y.writerow([
            "ebird_code", "file", "start", "end", "target_confidence",
            "best_species", "best_confidence", "non_bird_species", "non_bird_confidence",
        ])

        for code in species:
            if code not in code_name:
                print(f"!! {code} is not 'included' in "
                      "species_istanbul.csv, skipped")
                continue
            target_name = code_name[code][0]
            sdir = os.path.join(a.result, code)
            wdir = os.path.join(a.wav, code)

            for wf in sorted(os.listdir(wdir)) if os.path.isdir(wdir) else []:
                if not wf.endswith(".wav"):
                    continue
                stem = wf[:-4]
                sf = os.path.join(sdir, stem + ".BirdNET.results.csv")
                if not os.path.exists(sf):
                    missing_file[code] += 1
                    continue

                duration = wav_duration(os.path.join(wdir, wf))
                if duration:
                    total_slice[code] += max(1, int(duration // 3) + (duration % 3 > 0))

                for (start, end), pred in sorted(read_file(sf).items()):
                    detected[code] += 1
                    d = dict(pred)
                    target = d.get(target_name, 0.0)
                    best_species, best = max(pred, key=lambda x: x[1])
                    nb = [(n, c) for n, c in pred if n in NON_BIRD]
                    nb_species, nb_conf = (max(nb, key=lambda x: x[1])
                                           if nb else ("", 0.0))
                    if nb:
                        non_bird_count[code] += 1
                    if best_species == target_name:
                        target_best[code] += 1
                    for e in THRESHOLDS:
                        if target >= e:
                            count[e][code] += 1

                    y.writerow([
                        code, stem, f"{start:.1f}", f"{end:.1f}",
                        f"{target:.4f}", best_species, f"{best:.4f}",
                        nb_species, f"{nb_conf:.4f}" if nb else "",
                    ])
                    row += 1

    # ---------------- summary ----------------
    print(f"\n{len(species)} species / {row} slice rows -> {a.out}")
    for e in THRESHOLDS:
        t = sum(count[e].values())
        print(f"  target confidence >= {e}: {t} slices  "
              f"({t / max(len(species), 1):.0f}/species)")
    print(f"  slices with at least one detection: {sum(detected.values())}")
    print(f"  total slices in the recordings    : {sum(total_slice.values())}")
    print(f"  slices holding a non-bird sound   : "
          f"{sum(non_bird_count.values())}  (for negative mining)")

    if missing_file:
        n = sum(missing_file.values())
        print(f"\n!! {n} WAVs have no result (across {len(missing_file)} "
              "species) - run birdnet_run.py again (it resumes where it "
              "left off)")
        for k, v in sorted(missing_file.items(), key=lambda x: -x[1])[:10]:
            print(f"   {k:10s} {v}")

    weak = sorted(
        ((count[0.25][k], k) for k in species if k in code_name),
    )
    few = [(n, k) for n, k in weak if n < a.weak_threshold]
    print(f"\nspecies below {a.weak_threshold} slices at the 0.25 threshold: "
          f"{len(few)}")
    print("(this is the class imbalance the focal loss and the augmentation "
          "in tools/train_species.py target)")
    for n, k in few[:25]:
        name = code_name[k][1]
        share = target_best[k] / detected[k] if detected[k] else 0
        print(f"   {k:10s} {name:28s} {n:5d} slices   "
              f"target scored highest: {share * 100:.0f}%")


if __name__ == "__main__":
    main()
