#!/usr/bin/env python3
"""
validation_set.py — build the ON-DEVICE VALIDATION SET

Why this exists
---------------
The most expensive class of bug in this project is "works on the PC, not on
the device", and the only thing that catches it is running the same input
through both sides and comparing the OUTPUTS. The audio path is deliberately
out of scope: the microphone, mel and the gate are not involved at all, which
keeps the search space narrow. The input is a ready-made window taken straight
from the training set.

What it produces
----------------
    firmware/src/ai/validation_set.h
        N windows (int8) plus the int8 logits the PC produced

The device's `x` command runs the same windows through the model and compares
its logits against this table. The expectation is that they match EXACTLY. If
they do not, the problem is not in mel but in TFLM, CMSIS-NN or the
quantisation.

Usage
-----
    .venv-birdnet\\Scripts\\python tools/validation_set.py
    .venv-birdnet\\Scripts\\python tools/validation_set.py --count 8

Requires TensorFlow, i.e. `.venv-birdnet` (Python 3.11).
"""
from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRAIN = csv_compat.resolve(ROOT / "data" / "dataset")
MODEL = csv_compat.resolve(ROOT / "models" / "species_net_int8.tflite")
OUTPUT = ROOT / "firmware" / "src" / "ai" / "validation_set.h"

FRAMES = 187
BAND = 64


def sample_pick(count: int, seed: int) -> list[int]:
    """Pick `count` rows from the test split.

    The selection comes from the TEST split, i.e. windows never seen in
    training. That is not strictly required for validation (the same input
    gives the same output regardless of split), but it means the same rows can
    also be used to measure accuracy later.

    The negative class (178) is ALWAYS included: it is the model's last class,
    and the path to it (global average -> fully connected) sees a different
    activation range from the others.
    """
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))
    split = []
    with open(csv_compat.resolve(TRAIN / "samples.csv"), encoding="utf-8") as f:
        for row in csv_compat.reader(f):
            split.append(row["split"])
    split = np.array(split)
    if len(split) != len(label):
        sys.exit(f"samples.csv has {len(split)} rows, labels.npy has "
                 f"{len(label)} — they are misaligned")

    test = np.flatnonzero(split == "test")
    rng = np.random.default_rng(seed)

    negative = test[label[test] == 178]
    bird = test[label[test] != 178]
    if len(negative) == 0:
        sys.exit("no negative samples in the test split")

    selection = [int(rng.choice(negative))]
    # The rest come from different classes: two windows of the same class
    # exercise the same code path, whereas variety touches more channel and
    # scale combinations.
    bird_shuffled = rng.permutation(bird)
    seen: set[int] = set()
    for i in bird_shuffled:
        s = int(label[i])
        if s in seen:
            continue
        seen.add(s)
        selection.append(int(i))
        if len(selection) == count:
            break
    selection.sort()
    return selection


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=8,
                    help="number of windows (default 8; 11,968 bytes of "
                         "FLASH each)")
    ap.add_argument("--seed", type=int, default=20260802)
    args = ap.parse_args()

    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow is not available. Run with "
                 ".venv-birdnet\\Scripts\\python.")

    if not MODEL.exists():
        sys.exit(f"{MODEL} is missing — run tools/train_species.py first.")

    selection = sample_pick(args.count, args.seed)
    windows = np.load(csv_compat.resolve(TRAIN / "windows.npy"), mmap_mode="r")
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))

    name = {}
    with open(csv_compat.resolve(TRAIN / "classes.csv"), encoding="utf-8") as f:
        for s in csv_compat.reader(f):
            name[int(s["class_index"])] = (s["ebird_code"],
                                          s.get("english_name", ""))

    # BUILTIN_REF — DO NOT use the default.
    #
    # tf.lite.Interpreter enables the XNNPACK delegate by default, and XNNPACK
    # does not compute int8 bit-for-bit: measured over the same 8 windows it
    # deviates from BUILTIN_REF by up to 2 int8 steps, mean absolute 0.4441.
    # On the first run the device-vs-PC difference came out at exactly that
    # (max 2, mean 0.4441) — so what looked like "the device deviates" was
    # entirely the delegate on the PC side.
    #
    # What defines TFLite's int8 semantics is the reference kernels, and
    # CMSIS-NN aims to be bit-exact with them. That is the correct gold
    # standard.
    interp = tf.lite.Interpreter(
        model_path=str(MODEL),
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    gd = interp.get_input_details()[0]
    cd = interp.get_output_details()[0]

    # The device contract: scale 1.0 / zero 0. The firmware relies on that to
    # bind the input with a memcpy; if it drifts, the validation set is
    # meaningless too.
    if gd["dtype"] != np.int8 or gd["quantization"] != (1.0, 0):
        sys.exit(f"input contract broken: {gd['dtype']} {gd['quantization']}")
    if tuple(gd["shape"]) != (1, FRAMES, BAND, 1):
        sys.exit(f"input shape {gd['shape']}, expected (1,{FRAMES},{BAND},1)")

    output_scale, output_zero = cd["quantization"]
    cls_count = int(cd["shape"][-1])

    inputs = np.empty((len(selection), FRAMES, BAND), dtype=np.int8)
    logits = np.empty((len(selection), cls_count), dtype=np.int8)
    pred = []
    for k, i in enumerate(selection):
        p = np.asarray(windows[i], dtype=np.int8)
        inputs[k] = p
        interp.set_tensor(gd["index"], p.reshape(1, FRAMES, BAND, 1))
        interp.invoke()
        q = interp.get_tensor(cd["index"])[0].astype(np.int8)
        logits[k] = q
        pred.append(int(np.argmax(q.astype(np.int32))))

    correct = sum(1 for k, i in enumerate(selection) if pred[k] == int(label[i]))
    print(f"{len(selection)} windows selected (test split)")
    print(f"PC-side top-1: {correct}/{len(selection)} "
          f"(a low number is NORMAL — per-window accuracy is 58%)")
    for k, i in enumerate(selection):
        g, t = int(label[i]), pred[k]
        print(f"  row {i:6d}  truth {g:3d} {name.get(g, ('?', '?'))[1]:<26s}"
              f"  pred {t:3d} {name.get(t, ('?', '?'))[1]}")

    def as_array(v: np.ndarray) -> str:
        s, row = [], []
        for x in v:
            row.append(f"{int(x):4d}")
            if len(row) == 16:
                s.append("    " + ",".join(row) + ",")
                row = []
        if row:
            s.append("    " + ",".join(row) + ",")
        return "\n".join(s)

    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"""/* GENERATED FILE - tools/validation_set.py. DO NOT EDIT BY HAND.
 *
 * ON-DEVICE VALIDATION SET.
 *
 * {len(selection)} windows were picked from the TEST split of data/egitim/pencereler.npy;
 * the expected logits are the int8 output the PC's TFLite interpreter gives
 * for the same window. The device must produce EXACTLY the same output for
 * the same input.
 *
 * The PC side was run with the REFERENCE kernels (BUILTIN_REF). The default
 * interpreter uses the XNNPACK delegate, which does not compute int8
 * bit-for-bit (measured: up to 2 steps of deviation), so it is not the gold
 * standard here.
 *
 * If they differ, the problem is NOT in the mel pipeline (the audio path is
 * not exercised by this test at all): it is in the TFLM kernels, CMSIS-NN,
 * the arena, or the quantisation.
 *
 * Output quantisation: logit = (q - {int(output_zero)}) * {float(output_scale):.9f}
 */
#ifndef POKEBIRD_VALIDATION_SET_H
#define POKEBIRD_VALIDATION_SET_H

#include <stdint.h>

#define PB_VALIDATION_COUNT   {len(selection)}
#define PB_VALIDATION_FRAMES   {FRAMES}
#define PB_VALIDATION_BANDS   {BAND}
#define PB_VALIDATION_CLASSES  {cls_count}

/* True class index (not used for correctness, only to make the report\n * readable). */
static const int16_t pb_validation_class[PB_VALIDATION_COUNT] = {{
{as_array(np.array([label[i] for i in selection]))}
}};

/* The PC's prediction for the same window (argmax). */
static const int16_t pb_validation_pc_pred[PB_VALIDATION_COUNT] = {{
{as_array(np.array(pred))}
}};

/* Input windows: frames on the outside (oldest to newest), bands on the
 * inside - the same layout pb_mel_window() produces in mel.c. */
static const int8_t pb_validation_input[PB_VALIDATION_COUNT]
                                      [PB_VALIDATION_FRAMES * PB_VALIDATION_BANDS] = {{
""")
        for k in range(len(selection)):
            f.write("  {\n" + as_array(inputs[k].reshape(-1)) + "\n  },\n")
        f.write("};\n\n/* The PC's raw int8 logits. */\nstatic const int8_t "
                "pb_validation_logit[PB_VALIDATION_COUNT][PB_VALIDATION_CLASSES] = {\n")
        for k in range(len(selection)):
            f.write("  {\n" + as_array(logits[k]) + "\n  },\n")
        f.write("};\n\n#endif /* POKEBIRD_VALIDATION_SET_H */\n")

    size = OUTPUT.stat().st_size
    print(f"\nwrote {OUTPUT.relative_to(ROOT)} ({size/1024:.0f} KB of source, "
          f"{len(selection)*FRAMES*BAND/1024:.0f} KB of flash)")


if __name__ == "__main__":
    main()
