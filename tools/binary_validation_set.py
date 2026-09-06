#!/usr/bin/env python3
"""
binary_validation_set.py — the stage-1 binary net's ON-DEVICE VALIDATION SET

The binary net's counterpart to tools/validation_set.py, for the same reason:
without involving the microphone or the mel pipeline at all, run the same
window through the same model on the PC and on the device and compare the int8
logits EXACTLY. That is the only method that catches the "works on the PC, not
on the device" class of bug.

Output: firmware/src/ai/binary_validation_set.h

Selection: half birds (from different species) and half negatives (ESC-50), so
that the binary net is seen working at BOTH ends. The BUILTIN_REF warning in
validation_set.py applies here too: the default XNNPACK delegate does not
compute int8 bit-for-bit.

Usage:
    .venv-birdnet\\Scripts\\python tools/binary_validation_set.py
"""
from __future__ import annotations

import pathlib
import sys

import numpy as np

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRAIN = ROOT / "data" / "egitim"
MODEL = csv_compat.resolve(ROOT / "models" / "binary_net_int8.tflite")
OUTPUT = ROOT / "firmware" / "src" / "ai" / "binary_validation_set.h"

FRAMES, BAND = 187, 64
NEGATIVE_CLS = 178
PER_SIDE = 4   # 4 bird + 4 negative = 8 windows


def sample_pick(seed: int) -> list[int]:
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))
    split = []
    with open(csv_compat.resolve(TRAIN / "samples.csv"), encoding="utf-8") as f:
        for row in csv_compat.reader(f):
            split.append(row["split"])
    split = np.array(split)

    test = np.flatnonzero(split == "test")
    rng = np.random.default_rng(seed)

    negative = test[label[test] == NEGATIVE_CLS]
    bird = test[label[test] != NEGATIVE_CLS]

    selection_negative = list(rng.choice(negative, size=PER_SIDE, replace=False))

    # Variety on the bird side: from different species.
    bird_shuffled = rng.permutation(bird)
    seen: set[int] = set()
    selection_bird = []
    for i in bird_shuffled:
        s = int(label[i])
        if s in seen:
            continue
        seen.add(s)
        selection_bird.append(int(i))
        if len(selection_bird) == PER_SIDE:
            break

    selection = sorted(int(i) for i in selection_negative) + sorted(selection_bird)
    return selection


def main() -> None:
    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow is not available. Run with "
                 ".venv-birdnet\\Scripts\\python.")

    if not MODEL.exists():
        sys.exit(f"{MODEL} is missing — run tools/train_binary.py first.")

    selection = sample_pick(20260803)
    windows = np.load(csv_compat.resolve(TRAIN / "windows.npy"), mmap_mode="r")
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))

    # BUILTIN_REF — the same warning as in validation_set.py: XNNPACK does not
    # compute int8 bit-for-bit (measured), so the reference kernels are the
    # gold standard.
    interp = tf.lite.Interpreter(
        model_path=str(MODEL),
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    gd = interp.get_input_details()[0]
    cd = interp.get_output_details()[0]

    if gd["dtype"] != np.int8 or gd["quantization"] != (1.0, 0):
        sys.exit(f"input contract broken: {gd['dtype']} {gd['quantization']}")
    if tuple(gd["shape"]) != (1, FRAMES, BAND, 1):
        sys.exit(f"input shape {gd['shape']}, expected (1,{FRAMES},{BAND},1)")

    output_scale, output_zero = cd["quantization"]

    inputs = np.empty((len(selection), FRAMES, BAND), dtype=np.int8)
    logits = np.empty(len(selection), dtype=np.int8)
    truth = np.empty(len(selection), dtype=np.int32)
    for k, i in enumerate(selection):
        p = np.asarray(windows[i], dtype=np.int8)
        inputs[k] = p
        interp.set_tensor(gd["index"], p.reshape(1, FRAMES, BAND, 1))
        interp.invoke()
        q = int(interp.get_tensor(cd["index"])[0][0])
        logits[k] = q
        truth[k] = 0 if int(label[i]) == NEGATIVE_CLS else 1

    print(f"{len(selection)} windows selected (test split, {PER_SIDE} bird + "
          f"{PER_SIDE} negative)")
    for k, i in enumerate(selection):
        p = 1.0 / (1.0 + np.exp(-(float(logits[k]) - output_zero) * output_scale))
        print(f"  row {i:6d}  truth {'BIRD' if truth[k] else 'NOT':5s}"
              f"  logit(int8) {int(logits[k]):4d}  p={p:.4f}")

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
        f.write(f"""/* GENERATED FILE - tools/binary_validation_set.py. DO NOT EDIT BY HAND.
 *
 * ON-DEVICE VALIDATION SET FOR THE STAGE-1 BINARY NET.
 *
 * {len(selection)} windows were picked from the TEST split of data/egitim/pencereler.npy
 * ({PER_SIDE} bird + {PER_SIDE} negative); the expected logit is the output of the PC's TFLite
 * REFERENCE kernels (BUILTIN_REF). The device must produce EXACTLY the same
 * value - the default XNNPACK delegate does not compute int8 bit-for-bit
 * (measured), which is why BUILTIN_REF is used.
 *
 * Output quantisation: logit = (q - {int(output_zero)}) * {float(output_scale):.9f}
 * (the raw PRE-sigmoid value; p = sigmoid(logit))
 */
#ifndef POKEBIRD_BINARY_VALIDATION_SET_H
#define POKEBIRD_BINARY_VALIDATION_SET_H

#include <stdint.h>

#define PB_BINARY_VALIDATION_COUNT  {len(selection)}
#define PB_BINARY_VALIDATION_FRAMES  {FRAMES}
#define PB_BINARY_VALIDATION_BANDS  {BAND}

/* True binary label: 1 = BIRD, 0 = NOT. */
static const int16_t pb_binary_validation_truth[PB_BINARY_VALIDATION_COUNT] = {{
{as_array(truth)}
}};

/* The raw int8 logit produced by the PC's REFERENCE kernels (pre-sigmoid). */
static const int8_t pb_binary_validation_logit[PB_BINARY_VALIDATION_COUNT] = {{
{as_array(logits)}
}};

/* Input windows: frames on the outside (oldest to newest), bands inside. */
static const int8_t pb_binary_validation_input[PB_BINARY_VALIDATION_COUNT]
                                            [PB_BINARY_VALIDATION_FRAMES * PB_BINARY_VALIDATION_BANDS] = {{
""")
        for k in range(len(selection)):
            f.write("  {\n" + as_array(inputs[k].reshape(-1)) + "\n  },\n")
        f.write("};\n\n#endif\n")

    print(f"\nwrote {OUTPUT}")


if __name__ == "__main__":
    main()
