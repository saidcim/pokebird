#!/usr/bin/env python3
"""
birdnet_slist.py — build a BirdNET species list from species_istanbul.csv.

Without `--slist`, BirdNET searches all 6,522 of its species and produces
detections that have nothing to do with our 178. Given a list, the model only
scores those species: it is faster, and the risk of mislabelling drops.

THE FORMAT (verified against the package's labels/V2.4/*.txt files):

    Scientific name_English name     e.g. "Corvus cornix_Hooded Crow"

The line has to match BirdNET's own label file EXACTLY. If it does not,
BirdNET ignores that line silently — which is why the English name is not
taken from our own CSV. Instead the eBird code is mapped to a label through
BirdNET's own `eBird_taxonomy_codes_2024E.json`, and the result is VERIFIED
against Labels.txt. Any species that fails to match is printed; nothing is
lost silently.

    python tools/birdnet_slist.py
"""

import argparse
import csv
import json
import os
import sys

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
VENV_PKG = os.path.join(
    ROOT, ".venv-birdnet", "Lib", "site-packages", "birdnet_analyzer"
)


def package_dir(given):
    if given:
        return given
    if os.path.isdir(VENV_PKG):
        return VENV_PKG
    try:
        import birdnet_analyzer

        return os.path.dirname(os.path.abspath(birdnet_analyzer.__file__))
    except ImportError:
        sys.exit("birdnet_analyzer not found; pass its directory with --package")


def read_labels(pkg):
    """The label list the model actually uses."""
    path = os.path.join(pkg, "checkpoints", "V2.4",
                        "BirdNET_GLOBAL_6K_V2.4_Labels.txt")
    if not os.path.exists(path):
        sys.exit(
            f"Label file missing: {path}\n"
            "The model has not been downloaded yet. Run analyze once first "
            "(V2.4.zip is fetched on the first run)."
        )
    with open(path, encoding="utf-8") as f:
        return [s.strip() for s in f if s.strip()]


def read_codes(pkg):
    path = os.path.join(pkg, "eBird_taxonomy_codes_2024E.json")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", help="the birdnet_analyzer package directory")
    ap.add_argument("--csv", default=os.path.join(DATA, "species_istanbul.csv"))
    ap.add_argument("--out", default=os.path.join(DATA, "birdnet_slist.txt"))
    ap.add_argument(
        "--name-map", default=os.path.join(DATA, "birdnet_name_map.csv"),
        help="eBird code -> BirdNET label; birdnet_summary.py reads this"
    )
    a = ap.parse_args()

    pkg = package_dir(a.package)
    labels = read_labels(pkg)
    codes = read_codes(pkg)

    # Secondary lookup by scientific name: if the eBird code does not match
    # (the taxonomy version may differ), try to catch it by species name.
    scientific = {}
    for label in labels:
        scientific.setdefault(label.split("_", 1)[0], label)

    with open(a.csv, encoding="utf-8") as f:
        rows = [r for r in csv_compat.reader(f) if r["status"] == "included"]

    label_subset = set(labels)
    selected, missing, differing = [], [], []
    for r in rows:
        code, name = r["ebird_code"], r["scientific_name"]
        label = codes.get(code)
        source = "code"
        if label not in label_subset:
            label = scientific.get(name)
            source = "scientific name"
        if label is None:
            missing.append((code, name, r["english_name"]))
            continue
        bn_name = label.split("_", 1)[0]
        if bn_name != name:
            differing.append((code, name, bn_name, r["english_name"]))
        selected.append((label, code, source, name, bn_name, r["english_name"]))

    with open(a.out, "w", encoding="utf-8") as f:
        for s in selected:
            f.write(s[0] + "\n")

    with open(a.name_map, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ebird_code", "our_scientific_name",
                    "birdnet_scientific_name", "english_name"])
        for _, code, _, name, bn_name, eng in selected:
            w.writerow([code, name, bn_name, eng])

    by_name = sum(1 for s in selected if s[2] == "scientific name")
    print(f"species marked 'included' : {len(rows)}")
    print(f"found in BirdNET          : {len(selected)}  "
          f"({by_name} rescued by scientific name)")
    print(f"species list              : {a.out}")
    print(f"name map                  : {a.name_map}")

    if differing:
        # A SOURCE OF SILENT FAILURE: the summary script looks the target
        # species up by scientific name. For species where BirdNET uses an
        # older genus name that lookup misses, the species scores 0 on every
        # slice, and it silently drops out of training.
        print(f"\n!! BirdNET uses a DIFFERENT SCIENTIFIC NAME for "
              f"{len(differing)} species.")
        print("   The match was made by eBird code; that is why the name map "
              "exists.")
        for code, name, bn_name, eng in differing:
            print(f"   {code:10s} {eng:26s} ours '{name}'  ->  "
                  f"BirdNET '{bn_name}'")

    if missing:
        print(f"\n!! NOT IN BirdNET ({len(missing)} species) — these cannot "
              f"be trained:")
        for code, name, eng in missing:
            print(f"   {code:10s} {name:30s} {eng}")


if __name__ == "__main__":
    main()
