#!/usr/bin/env python3
"""
m4_run.py — run the data pipeline end to end, unattended.

  1. Count Xeno-canto recordings (with the right quality filter: q:">C" = A/B)
  2. Build the final species list — it finds the threshold closest to the
     target species count by itself
  3. Download recordings per species

Why this is a separate script: until the counting pass finishes, nobody knows
how many species the final list will hold, and the download depends on that
list. Rather than chaining the three by hand, this is one command.

    python tools/m4_run.py --target-species 120 --count 60
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV_PATH = os.path.join(ROOT, "data", "species_istanbul.csv")
PY = sys.executable


def run(args):
    print(f"\n$ {' '.join(args)}\n", flush=True)
    p = subprocess.run([PY] + args, cwd=ROOT)
    if p.returncode != 0:
        sys.exit(f"[!] command failed (exit {p.returncode}): {' '.join(args)}")


def include_count():
    with open(CSV_PATH, encoding="utf-8") as f:
        return sum(1 for r in csv_compat.reader(f) if r["status"] == "included")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target-species", type=int, default=120)
    ap.add_argument("--count", type=int, default=60)
    ap.add_argument("--common-threshold", type=int, default=30)
    args = ap.parse_args()

    # 1. Refresh the pool (from the GBIF/eBird cache; takes seconds)
    run(["tools/species_list.py", "--monthly"])

    # 2. Count and filter. Search for the rare-species threshold that lands
    #    closest to the target: the counts are cached, so repeats are free.
    candidates = [100, 125, 150, 175, 200, 250, 300]
    best, best_diff, best_count = None, 10**9, 0

    for rare in candidates:
        run(["tools/xc_fetch.py", "--survey",
             "--threshold", str(args.common_threshold),
             "--rare-threshold", str(rare)])
        count = include_count()
        diff = abs(count - args.target_species)
        print(f"\n>>> rare-threshold {rare} -> {count} species "
              f"(target {args.target_species}, off by {diff})\n", flush=True)
        if diff < best_diff:
            best, best_diff, best_count = rare, diff, count
        # The pool narrows each round; refresh it for the next threshold.
        run(["tools/species_list.py", "--monthly"])

    print(f"\n=== CHOSEN: rare-threshold {best} -> {best_count} species ===\n",
          flush=True)
    run(["tools/xc_fetch.py", "--survey",
         "--threshold", str(args.common_threshold),
         "--rare-threshold", str(best)])

    # 3. Download
    run(["tools/xc_fetch.py", "--download", "--per-species", str(args.count)])
    print("\n=== DATA PIPELINE COMPLETE ===", flush=True)


if __name__ == "__main__":
    sys.exit(main())
