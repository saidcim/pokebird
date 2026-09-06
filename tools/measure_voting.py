#!/usr/bin/env python3
"""
measure_voting.py — measure what temporal voting contributes to accuracy.

    .venv-birdnet\\Scripts\\python -u tools/measure_voting.py

WHY MEASURE THIS: the top-1/top-3 that train_species.py reports is PER SINGLE
3-second window. The device does not work that way — the window is 3 s but the
step is 1 s, and the voting stage combines the softmax of consecutive windows.
The number the user sees on screen is that combined result. Optimising
per-window accuracy could mean chasing the wrong number; look at the right one
first.

SCOPE, without overclaiming. The voting here is done over consecutive slices
of the SAME RECORDING in the test set. It differs from the device in two ways:
  * the device sees more overlapping windows at a 1 s step, so its errors are
    more CORRELATED and the real gain is somewhat LOWER than this;
  * the slices here are the ones where BirdNET heard a bird, so the assumption
    that the bird is singing continuously holds in this set.
The numbers below are therefore an UPPER BOUND. The definitive answer comes
from a field test.

Output: models/voting.txt
"""

import os
import sys
from collections import defaultdict

import numpy as np
import tensorflow as tf

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRAIN = csv_compat.resolve(os.path.join(ROOT, "data", "dataset"))
SPECIES = os.path.join(ROOT, "data", "species_istanbul.csv")
MODELS = os.path.join(ROOT, "models")
TFLITE = csv_compat.resolve(os.path.join(MODELS, "species_net_int8.tflite"))
CACHE = csv_compat.resolve(os.path.join(MODELS, "test_probs.npy"))


def class_names():
    """class index -> English display name.

    The name column of a data/dataset/classes.csv written by an older run is
    Turkish, so the committed species list is the source of truth and the
    dataset's own column is only the fallback.
    """
    english = {}
    if os.path.exists(SPECIES):
        with open(SPECIES, encoding="utf-8") as f:
            english = {r["ebird_code"]: r["english_name"].strip()
                       for r in csv_compat.reader(f)}
    negative = {"__negative__", "__negatif__"}   # the negative-class sentinel
    out = {}
    with open(csv_compat.resolve(os.path.join(TRAIN, "classes.csv")),
              encoding="utf-8") as f:
        for s in csv_compat.reader(f):
            code = s["ebird_code"]
            out[int(s["class_index"])] = (
                "unknown / not a bird" if code in negative
                else english.get(code, s.get("english_name", code)))
    return out


def probabilities(idx, X):
    """The softmax per test window. Computed once and cached."""
    if os.path.exists(CACHE):
        P = np.load(CACHE)
        if len(P) == len(idx):
            print(f"read from cache: {CACHE}")
            return P
    it = tf.lite.Interpreter(model_path=TFLITE, num_threads=10)
    it.allocate_tensors()
    g, c = it.get_input_details()[0], it.get_output_details()[0]
    scale, zero = c["quantization"]
    P = np.zeros((len(idx), c["shape"][-1]), dtype=np.float32)
    for k, i in enumerate(idx):
        it.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        it.invoke()
        raw = (it.get_tensor(c["index"])[0].astype(np.float32) - zero) * scale
        e = np.exp(raw - raw.max())
        P[k] = e / e.sum()
        if k % 1000 == 0:
            print(f"  {k}/{len(idx)}", flush=True)
    np.save(CACHE, P)
    return P


def main():
    if not os.path.exists(TFLITE):
        sys.exit(f"{TFLITE} is missing — run tools/train_species.py first")
    X = np.load(csv_compat.resolve(os.path.join(TRAIN, "windows.npy")), mmap_mode="r")
    y = np.load(csv_compat.resolve(os.path.join(TRAIN, "labels.npy")))
    with open(csv_compat.resolve(os.path.join(TRAIN, "samples.csv")), encoding="utf-8") as f:
        r = list(csv_compat.reader(f))
    name = class_names()

    idx = np.array([i for i, x in enumerate(r) if x["split"] == "test"])
    P = probabilities(idx, X)
    Y = y[idx]

    # Put the slices of the same recording in time order.
    record = defaultdict(list)
    for k, i in enumerate(idx):
        record[(r[i]["ebird_code"], r[i]["file"])].append(
            (float(r[i]["start"]), k))
    for v in record.values():
        v.sort()

    s = []
    s.append("TEMPORAL VOTING — what happens as more consecutive windows vote")
    s.append("(no selection; measured on the test set. An upper bound — see the "
             "header of this script)")
    s.append("")
    s.append(" windows  samples    top-1     top-3")
    for n in (1, 2, 3, 5, 8, 12):
        d1 = d3 = count = 0
        for (_, _), v in record.items():
            for b in range(0, len(v), n):
                group = [k for _, k in v[b:b + n]]
                if len(group) < min(n, 2) and n > 1:
                    continue
                avg = P[group].mean(axis=0)
                top3 = np.argsort(-avg)[:3]
                target = Y[group[0]]
                d1 += int(top3[0] == target)
                d3 += int(target in top3)
                count += 1
        s.append(f" {n:>7}  {count:>7}   {100 * d1 / count:5.2f}%   "
                 f"{100 * d3 / count:5.2f}%")

    # The negative class separately: in the field the most expensive mistake
    # is mistaking noise for a bird.
    neg = 178
    bird = Y != neg
    top = np.argmax(P, axis=1)
    s.append("")
    s.append(f"negative mistaken for a bird (per window): "
             f"{100 * (top[~bird] != neg).mean():.2f}%")
    s.append(f"bird mistaken for negative   (per window): "
             f"{100 * (top[bird] == neg).mean():.2f}%")
    s.append("")
    s.append("The 15 most confused pairs (per window, test set):")
    pairs = defaultdict(int)
    for h, t in zip(Y, top):
        if h != t:
            pairs[(int(h), int(t))] += 1
    for (h, t), n in sorted(pairs.items(), key=lambda x: -x[1])[:15]:
        s.append(f"  {name.get(h, h):26s} -> {name.get(t, t):26s} {n:4d}")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(MODELS, "voting.txt"), "w",
              encoding="utf-8") as f:
        f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
