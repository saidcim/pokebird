#!/usr/bin/env python3
"""
measure_thresholds.py — measure the CONFIDENCE THRESHOLD for showing a species
name on screen.

    python tools/measure_thresholds.py     # numpy is enough, TensorFlow NOT needed

WHY: the accuracy figures measured elsewhere (top-1 70.40% / top-3 82.20%) are
ACCURACY. The decision rule needs something different: "if the voted
probability p1 is above this value and I print the species name, how often am
I right?" That cannot be derived from accuracy — it has to be measured. The
thresholds in the on-screen decision rule come from here; not one of them is
guessed.

INPUT: models/test_probs.npy — the cache tools/measure_voting.py leaves behind
(the softmax the INT8 model produced for every window in the test set). If it
is missing, run that first (in the .venv-birdnet environment, which has
TensorFlow).

SCOPE, without overclaiming: the two caveats at the top of measure_voting.py
apply here too. The blocks come from consecutive slices of the SAME recording,
and the slices are where BirdNET heard a bird; on the device the windows
OVERLAP at a one-second step, so the errors are more correlated. The precision
figures below are an UPPER BOUND. The thresholds are chosen against them, but
field calibration is still needed.

Output: models/thresholds.txt
"""

import os
import sys
from collections import defaultdict

import numpy as np

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRAIN = csv_compat.resolve(os.path.join(ROOT, "data", "dataset"))
MODELS = os.path.join(ROOT, "models")
CACHE = csv_compat.resolve(os.path.join(MODELS, "test_probs.npy"))

NEGATIVE = 178   # the last class in classes.csv: negative / unknown
WINDOW = 8       # the device's voting window (PB_VOTE_WINDOWS)

# Selection criteria — which precision we are willing to accept is a PRODUCT
# decision, not a number that falls out of the data.
# Enter threshold: how often we are right when we put a species name on screen.
TARGET_PRECISION_ENTER = 0.85
# Exit threshold: the point at which we clear what is shown. A display that
# falls below the voted top-1 accuracy (70.40%) is no better than a plain best
# guess.
TARGET_PRECISION_EXIT = 0.70


def blocks(P, Y, records, n):
    """Average n consecutive windows. The SAME rule as measure_voting.py (the
    mean of the softmax) — choosing another rule would invalidate the accuracy
    figures."""
    p1, pred, target = [], [], []
    for v in records.values():
        for b in range(0, len(v), n):
            group = [k for _, k in v[b:b + n]]
            if len(group) < min(n, 2) and n > 1:
                continue
            avg = P[group].mean(axis=0)
            t = int(np.argmax(avg))
            p1.append(float(avg[t]))
            pred.append(t)
            target.append(int(Y[group[0]]))
    return np.array(p1), np.array(pred), np.array(target)


def table(p1, pred, target, thresholds):
    """For each threshold: coverage, precision, false alarms, misses."""
    bird = target != NEGATIVE
    row = []
    for t in thresholds:
        announced = (p1 >= t) & (pred != NEGATIVE)
        n_announced = int(announced.sum())
        precision = (float((pred[announced] == target[announced]).mean())
                     if n_announced else float("nan"))
        coverage = n_announced / len(p1)
        # The most expensive mistake in the field: mistaking noise for a bird.
        false_alarm = float(announced[~bird].mean()) if (~bird).any() else float("nan")
        miss = float(1.0 - announced[bird].mean()) if bird.any() else float("nan")
        row.append((t, n_announced, coverage, precision, false_alarm, miss))
    return row


def pick(row, target_precision):
    """The SMALLEST threshold that reaches the target precision — no point
    cutting coverage further than necessary."""
    for t, n, coverage, precision, fa, miss in row:
        if n >= 50 and precision >= target_precision:
            return t, coverage, precision, fa
    return None, None, None, None


def main():
    if not os.path.exists(CACHE):
        sys.exit(f"{CACHE} is missing — run tools/measure_voting.py first with "
                 ".venv-birdnet (it produces the cache).")

    P = np.load(CACHE)
    y = np.load(csv_compat.resolve(os.path.join(TRAIN, "labels.npy")))
    with open(csv_compat.resolve(os.path.join(TRAIN, "samples.csv")),
              encoding="utf-8") as f:
        r = list(csv_compat.reader(f))

    idx = np.array([i for i, x in enumerate(r) if x["split"] == "test"])
    if len(P) != len(idx):
        sys.exit(f"the cache has {len(P)} rows and the test set {len(idx)} — "
                 "they are out of step. Re-run measure_voting.py.")
    Y = y[idx]

    records = defaultdict(list)
    for k, i in enumerate(idx):
        records[(r[i]["ebird_code"], r[i]["file"])].append(
            (float(r[i]["start"]), k))
    for v in records.values():
        v.sort()

    thresholds = [round(0.05 * i, 2) for i in range(1, 20)]
    s = []
    s.append("ON-SCREEN DECISION RULE — confidence threshold measurement")
    s.append("(model: models/species_net_int8.tflite, test set, "
             f"{len(idx)} windows / {len(records)} recordings)")
    s.append("coverage = share of blocks where we print a species name - "
             "precision = how often we are right when we do")
    s.append("false alarm = share of negative blocks where we print a name - "
             "miss = share of bird blocks we let through")

    choices = {}
    for n in (1, 3, WINDOW):
        p1, pred, target = blocks(P, Y, records, n)
        s.append("")
        s.append(f"--- {n} window(s) voted ({len(p1)} blocks) "
                 f"------------------------")
        s.append("  thr     blocks  coverage  precision  false-alarm   miss")
        row = table(p1, pred, target, thresholds)
        for t, nd, coverage, precision, fa, miss in row:
            s.append(f"  {t:4.2f}  {nd:6d}   {100*coverage:5.1f}%    "
                     f"{100*precision:5.1f}%    {100*fa:8.1f}%  {100*miss:5.1f}%")
        choices[n] = row

    voted = choices[WINDOW]
    enter = pick(voted, TARGET_PRECISION_ENTER)
    exit_ = pick(voted, TARGET_PRECISION_EXIT)

    s.append("")
    s.append("--- SELECTION -----------------------------------------------")
    s.append(f"enter threshold (precision >= {100*TARGET_PRECISION_ENTER:.0f}%): "
             f"{enter[0]}  -> coverage {100*enter[1]:.1f}%, precision "
             f"{100*enter[2]:.1f}%, false alarm {100*enter[3]:.1f}%"
             if enter[0] is not None else
             "enter threshold: the target precision was NOT REACHED — lower "
             "the target or improve the model")
    s.append(f"exit threshold (precision >= {100*TARGET_PRECISION_EXIT:.0f}%): "
             f"{exit_[0]}  -> coverage {100*exit_[1]:.1f}%, precision "
             f"{100*exit_[2]:.1f}%, false alarm {100*exit_[3]:.1f}%"
             if exit_[0] is not None else
             "exit threshold: the target precision was NOT REACHED")
    s.append("")
    s.append("Hysteresis: the species name appears once p1 reaches the enter "
             "threshold, and stays until p1 falls below the exit threshold.")
    s.append("WARNING: these numbers are an UPPER BOUND — the blocks come from "
             "non-overlapping slices, while on the device the windows overlap "
             "at a one-second step (so the errors are correlated).")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(MODELS, "thresholds.txt"), "w",
              encoding="utf-8") as f:
        f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
