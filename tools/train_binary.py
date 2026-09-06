#!/usr/bin/env python3
"""
train_binary.py — stage-1 binary net (bird / no bird): training + INT8
quantisation.

    .venv-birdnet\\Scripts\\python -u tools/train_binary.py --smoke   # this first
    .venv-birdnet\\Scripts\\python -u tools/train_binary.py

Input : data/dataset/  (the output of tools/build_dataset.py — the SAME data
        the species net uses)
Output: models/binary_net.keras
        models/binary_net_int8.tflite
        models/binary_net_int8.h
        models/binary_net_report.txt

==========================================================================
WHY THERE IS NO SEPARATE DATA SET
==========================================================================
data/dataset/labels.npy already has 179 classes: 0..177 are bird species and
178 is "__negative__" (ESC-50 with the bird classes removed). All stage 1
needs is that label reduced to a binary one: class != 178 -> BIRD (1),
class == 178 -> NOT (0). The same windows.npy (64x187 int8 mel) is used as
the INPUT, under the same contract as the device's pb_mel_window() output.

CONTAMINATED WINDOWS (best_species != target) were a problem for the species
net (a wrong hard label) but they are NOT a problem HERE: a contaminated
window is still BIRD SONG, it is only that BirdNET's best guess is a
different species. Stage 1 asks "bird or not", not WHICH — so contaminated
windows go into training at full weight (they are not dropped the way the
species net drops them).

THE TEACHER SIGNAL (teacher.npy, the BirdNET sigmoid) IS NOT USED: that
distribution is per SPECIES and does not carry over to the binary question
(a species can have a low BirdNET score and still be a BIRD). A plain
weighted BCE is enough.

==========================================================================
CLASS IMBALANCE — MEASURED
==========================================================================
57,622 bird windows / 3,489 negatives = 16.5:1. The loss function weights the
negative class by that ratio (pos_weight = birds/negatives, changeable with
--weight).

==========================================================================
THRESHOLD CHOICE — a miss (false negative) is expensive, a pass-through
(false positive) is cheap
==========================================================================
If stage 1 says "not", stage 2 (the species net) never runs — so a wrong
"not" SILENTLY LOSES A REAL BIRD DETECTION. If it says "bird" and is wrong,
the cost is only a wasted stage-2 inference (stage 2 has a negative class of
its own and will say "unknown"). That is why the default decision threshold
is NOT 0.5 — it is measured and written into the report.
"""

import argparse
import os
import sys
import time

import numpy as np

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import tensorflow as tf  # noqa: E402

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRAIN = csv_compat.resolve(os.path.join(ROOT, "data", "dataset"))
MODELS = os.path.join(ROOT, "models")

FRAMES, BANDS = 187, 64
SIGMA_SCALE = 4.0 / 127.0
NEGATIVE_CLS = 178
MAC_BUDGET = 5_000_000       # far below the species net's 30M - it runs often
SIZE_BUDGET = 15 * 1024


def load_data():
    X = np.load(csv_compat.resolve(os.path.join(TRAIN, "windows.npy")), mmap_mode="r")
    y_species = np.load(csv_compat.resolve(os.path.join(TRAIN, "labels.npy"))).astype(np.int32)
    with open(csv_compat.resolve(os.path.join(TRAIN, "samples.csv")), encoding="utf-8") as f:
        row = list(csv_compat.reader(f))
    if not (len(X) == len(y_species) == len(row)):
        sys.exit(f"lengths do not match: X {len(X)} y {len(y_species)} "
                 f"csv {len(row)}")

    split = np.array([s["split"] for s in row])
    y = (y_species != NEGATIVE_CLS).astype(np.int32)   # 1=bird, 0=not
    return X, y, split


def make_dataset(X, y, class_weight, idx, batch, augment, shuffle):
    def fetch(i):
        i = np.sort(i)
        return (X[i].astype(np.float32), y[i].astype(np.float32),
                class_weight[i])

    ds = tf.data.Dataset.from_tensor_slices(idx)
    if shuffle:
        ds = ds.shuffle(len(idx), reshuffle_each_iteration=True)
    ds = ds.batch(batch, drop_remainder=False)
    ds = ds.map(
        lambda i: tf.numpy_function(fetch, [i],
                                    [tf.float32, tf.float32, tf.float32]),
        num_parallel_calls=tf.data.AUTOTUNE)

    def shape_batch(x, e, w):
        x = tf.reshape(x, (-1, FRAMES, BANDS, 1))
        e.set_shape([None]); w.set_shape([None])
        if augment:
            x = spec_augment(x)
        return x, e, w

    return ds.map(shape_batch, num_parallel_calls=tf.data.AUTOTUNE).prefetch(
        tf.data.AUTOTUNE)


def spec_augment(x):
    """The same as train_species.py: a time shift plus SpecAugment, with 0 as
    the mask value."""
    b = tf.shape(x)[0]
    k = tf.random.uniform([], -16, 17, dtype=tf.int32)
    x = tf.roll(x, shift=k, axis=1)

    def mask(x, axis, max_extra):
        length = tf.shape(x)[axis]
        width = tf.random.uniform([b, 1], 0, max_extra, dtype=tf.int32)
        start = tf.random.uniform([b, 1], 0, length - max_extra, dtype=tf.int32)
        r = tf.reshape(tf.range(length), [1, -1])
        m = tf.cast((r < start) | (r >= start + width), x.dtype)
        shape = [b, 1, 1, 1]
        shape[axis] = length
        return x * tf.reshape(m, shape)

    x = mask(x, 1, 30)
    x = mask(x, 2, 10)
    return x


def ds_block(x, channels, step, name):
    x = tf.keras.layers.DepthwiseConv2D(3, strides=step, padding="same",
                                        use_bias=False, name=f"{name}_dw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_dwbn")(x)
    x = tf.keras.layers.ReLU(6.0, name=f"{name}_dwrelu")(x)
    x = tf.keras.layers.Conv2D(channels, 1, use_bias=False, name=f"{name}_pw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_pwbn")(x)
    return tf.keras.layers.ReLU(6.0, name=f"{name}_pwrelu")(x)


def build_model(width=1.0):
    """A small depthwise-separable CNN - far narrower and shorter than the
    species net's 8 blocks. The budget is 15 KB int8, 1/18th of the species
    net's ~270 KB."""
    k = lambda n: max(4, int(n * width))
    g = tf.keras.Input(shape=(FRAMES, BANDS, 1), dtype="float32", name="mel_int8")
    x = tf.keras.layers.Rescaling(SIGMA_SCALE, name="int8_sigma")(g)

    x = tf.keras.layers.Conv2D(k(16), 3, strides=2, padding="same",
                               use_bias=False, name="stem")(x)
    x = tf.keras.layers.BatchNormalization(name="stem_bn")(x)
    x = tf.keras.layers.ReLU(6.0, name="stem_relu")(x)

    x = ds_block(x, k(32), 2, "b1")
    x = ds_block(x, k(48), 2, "b2")
    x = ds_block(x, k(64), 2, "b3")

    x = tf.keras.layers.GlobalAveragePooling2D(name="gap")(x)
    c = tf.keras.layers.Dense(1, name="logit")(x)
    return tf.keras.Model(g, c, name="pokebird_binary_net")


def mac_count(model):
    total = 0
    for k in model.layers:
        c = getattr(k, "output", None)
        if c is None:
            continue
        s = c.shape
        if isinstance(k, tf.keras.layers.Conv2D):
            h, w, f = s[1], s[2], s[3]
            gk = k.kernel_size[0] * k.kernel_size[1]
            total += h * w * f * gk * k.input.shape[-1]
        elif isinstance(k, tf.keras.layers.DepthwiseConv2D):
            h, w, f = s[1], s[2], s[3]
            total += h * w * f * k.kernel_size[0] * k.kernel_size[1]
        elif isinstance(k, tf.keras.layers.Dense):
            total += k.input.shape[-1] * s[-1]
    return int(total)


def build_loss(pos_weight):
    """Weighted BCE. pos_weight is the multiplier given to the NEGATIVE class
    (0) - the per-negative-sample weight that undoes the 16.5:1 imbalance."""
    pos_weight = tf.constant(float(pos_weight), dtype=tf.float32)

    def loss(y, logit, w):
        ce = tf.nn.sigmoid_cross_entropy_with_logits(labels=y, logits=logit[:, 0])
        weight = tf.where(y > 0.5, tf.ones_like(y), tf.fill(tf.shape(y), pos_weight))
        return tf.reduce_mean(ce * weight * w)
    return loss


def evaluate(model, ds, threshold=0.5):
    correct = total = 0
    tp = fp = tn = fn = 0
    for x, y, _ in ds:
        logit = model(x, training=False)
        p = tf.sigmoid(logit[:, 0]).numpy()
        e = y.numpy()
        pred = (p >= threshold).astype(np.int32)
        correct += int((pred == e).sum())
        total += len(e)
        tp += int(((pred == 1) & (e == 1)).sum())
        fp += int(((pred == 1) & (e == 0)).sum())
        tn += int(((pred == 0) & (e == 0)).sum())
        fn += int(((pred == 0) & (e == 1)).sum())
    return correct / total, tp, fp, tn, fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--width", type=float, default=1.0)
    ap.add_argument("--weight", type=float, default=0.0,
                    help="negative-class weight; 0 = use the measured 16.5")
    ap.add_argument("--out", default=MODELS)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    X, y, split = load_data()
    print(f"{len(X)} windows  bird {int(y.sum())}  not {int((1 - y).sum())}")

    idx = {b: np.where(split == b)[0] for b in ("train", "val", "test")}
    if a.smoke:
        rng = np.random.default_rng(0)
        for b in idx:
            idx[b] = rng.choice(idx[b], size=min(len(idx[b]), 3000), replace=False)
        a.epochs = 2
    for b, v in idx.items():
        print(f"  {b:10s} {len(v):6d}  bird {100*y[v].mean():.1f}%")

    pos_weight = a.weight if a.weight > 0 else (
        (y[idx["train"]] == 1).sum() / max((y[idx["train"]] == 0).sum(), 1))
    print(f"negative-class weight: {pos_weight:.2f}")

    sample_weight = np.ones(len(X), dtype=np.float32)

    train = make_dataset(X, y, sample_weight, idx["train"], a.batch,
                         augment=True, shuffle=True)
    val = make_dataset(X, y, sample_weight, idx["val"], a.batch,
                            augment=False, shuffle=False)
    test = make_dataset(X, y, sample_weight, idx["test"], a.batch,
                       augment=False, shuffle=False)

    model = build_model(a.width)
    mac = mac_count(model)
    par = model.count_params()
    print(f"\nmodel: {par:,} parameters (~{par / 1024:.1f} KB int8)")
    print(f"MAC/window: {mac / 1e6:.2f} M  (budget {MAC_BUDGET / 1e6:.0f} M)")
    if mac > MAC_BUDGET:
        sys.exit(f"!! the MAC budget was exceeded "
                 f"({mac/1e6:.1f}M > {MAC_BUDGET/1e6:.0f}M) - lower --width")
    if par > SIZE_BUDGET:
        print(f"!! WARNING: {par} parameters > the {SIZE_BUDGET} byte budget "
              "(the int8 size is measured on the tflite; that is the verdict)")

    loss_f = build_loss(pos_weight)
    step_count = max(1, len(idx["train"]) // a.batch) * a.epochs
    plan = tf.keras.optimizers.schedules.CosineDecay(
        a.lr, step_count, warmup_target=a.lr, warmup_steps=200)
    opt = tf.keras.optimizers.Adam(plan)

    @tf.function
    def step(x, e, w):
        with tf.GradientTape() as t:
            logit = model(x, training=True)
            loss = loss_f(e, logit, w)
        opt.apply_gradients(zip(t.gradient(loss, model.trainable_variables),
                                model.trainable_variables))
        return loss

    best = -1.0
    path = os.path.join(a.out, "binary_net.keras")
    history = []
    for epochs in range(1, a.epochs + 1):
        t0 = time.time()
        total = count = 0.0
        for x, e, w in train:
            total += float(step(x, e, w))
            count += 1
        acc, tp, fp, tn, fn = evaluate(model, val)
        recall = tp / max(tp + fn, 1)          # recall on the bird class
        specificity = tn / max(tn + fp, 1)     # correct rejection of negatives
        history.append((epochs, total / count, acc, recall, specificity))
        star = ""
        score = recall  # not missing birds is the real priority
        if score > best:
            best = score
            model.save(path)
            star = "  <- saved"
        print(f"epoch {epochs:3d}/{a.epochs}  loss {total/count:.4f}  "
              f"val acc {acc*100:.2f}%  bird-recall {recall*100:.2f}%  "
              f"negative-specificity {specificity*100:.2f}%  "
              f"{time.time()-t0:.0f}s{star}",
              flush=True)

    print(f"\nbest (val bird-recall): {best*100:.2f}%  ->  {path}")
    model = tf.keras.models.load_model(path)

    acc, tp, fp, tn, fn = evaluate(model, test)
    print(f"TEST (float32) threshold 0.5: acc {acc*100:.2f}%  "
          f"bird-recall {100*tp/max(tp+fn,1):.2f}%  "
          f"negative-specificity {100*tn/max(tn+fp,1):.2f}%  "
          f"(tp {tp} fp {fp} tn {tn} fn {fn})")

    tflite_path, gs, gz = quantize(model, X, idx["train"], a.out)
    q_acc, qtp, qfp, qtn, qfn = tflite_evaluate(tflite_path, X, y, idx["test"])
    print(f"TEST (int8)    threshold 0.5: acc {q_acc*100:.2f}%  "
          f"bird-recall {100*qtp/max(qtp+qfn,1):.2f}%  "
          f"negative-specificity {100*qtn/max(qtn+qfp,1):.2f}%  "
          f"(difference {(q_acc-acc)*100:+.2f} points)")

    c_write(tflite_path, os.path.join(a.out, "binary_net_int8.h"))
    write_report(a, model, mac, history, acc, tp, fp, tn, fn,
              q_acc, qtp, qfp, qtn, qfn, gs, gz, tflite_path)
    return 0


def quantize(model, X, train_idx, out):
    rng = np.random.default_rng(0)
    sample = np.sort(rng.choice(train_idx, size=min(500, len(train_idx)),
                               replace=False))

    def representative():
        for i in sample:
            yield [X[i].reshape(1, FRAMES, BANDS, 1).astype(np.float32)]

    d = tf.lite.TFLiteConverter.from_keras_model(model)
    d.optimizations = [tf.lite.Optimize.DEFAULT]
    d.representative_dataset = representative
    d.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    d.inference_input_type = tf.int8
    d.inference_output_type = tf.int8
    tfl = d.convert()

    path = os.path.join(out, "binary_net_int8.tflite")
    with open(path, "wb") as f:
        f.write(tfl)

    interp = tf.lite.Interpreter(model_path=path)
    interp.allocate_tensors()
    g = interp.get_input_details()[0]
    c = interp.get_output_details()[0]
    gs, gz = float(g["quantization"][0]), int(g["quantization"][1])
    print(f"\nINT8 model: {len(tfl)/1024:.1f} KB  "
          f"(budget {SIZE_BUDGET/1024:.0f} KB)  -> {path}")
    print(f"input tensor: {g['dtype'].__name__} {tuple(g['shape'])}  "
          f"scale {gs:.6f}  zero point {gz}")
    if abs(gs - 1.0) > 0.02 or gz != 0:
        print("!! WARNING: the input scale is NOT 1.0/0. The device cannot hand\n"
              "   pb_mel_window() output over as it is; a conversion is needed.\n"
              f"   q_tflite = round(q_mel * {SIGMA_SCALE:.6f} / {gs:.6f}) + {gz}")
    else:
        print("   -> the device can hand pb_mel_window() output over DIRECTLY.")
    cs, cz = float(c["quantization"][0]), int(c["quantization"][1])
    print(f"output tensor: {c['dtype'].__name__}  scale {cs:.6f}  "
          f"zero point {cz}  (the raw pre-sigmoid logit)")
    if len(tfl) > SIZE_BUDGET:
        print(f"!! WARNING: {len(tfl)/1024:.1f} KB > the "
              f"{SIZE_BUDGET/1024:.0f} KB budget")
    return path, gs, gz


def tflite_evaluate(path, X, y, idx, threshold=0.5):
    interp = tf.lite.Interpreter(model_path=path, num_threads=8)
    interp.allocate_tensors()
    g = interp.get_input_details()[0]
    c = interp.get_output_details()[0]
    cs, cz = c["quantization"]
    tp = fp = tn = fn = 0
    for i in idx:
        interp.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        interp.invoke()
        q = int(interp.get_tensor(c["index"])[0][0])
        logit = (q - cz) * cs
        p = 1.0 / (1.0 + np.exp(-logit))
        pred = int(p >= threshold)
        e = int(y[i])
        tp += int(pred == 1 and e == 1)
        fp += int(pred == 1 and e == 0)
        tn += int(pred == 0 and e == 0)
        fn += int(pred == 0 and e == 1)
    n = tp + fp + tn + fn
    return (tp + tn) / max(n, 1), tp, fp, tn, fn


def c_write(tflite_path, h_path):
    raw = open(tflite_path, "rb").read()
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("/* GENERATED FILE - tools/train_binary.py. "
                "DO NOT EDIT BY HAND. */\n")
        f.write("#ifndef POKEBIRD_BINARY_NET_H\n#define POKEBIRD_BINARY_NET_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define PB_BINARY_NET_SIZE {len(raw)}\n\n")
        f.write("__attribute__((aligned(16)))\n")
        f.write("const unsigned char pb_binary_net[] = {\n")
        for i in range(0, len(raw), 12):
            f.write("  " + " ".join(f"0x{b:02x}," for b in raw[i:i + 12]) + "\n")
        f.write("};\n\n#endif\n")
    print(f"C array: {h_path}  ({len(raw)/1024:.1f} KB)")


def write_report(a, model, mac, history, acc, tp, fp, tn, fn,
              q_acc, qtp, qfp, qtn, qfn, gs, gz, tflite_path):
    s = []
    s.append(f"model     : {model.count_params():,} parameters")
    s.append(f"MAC/window: {mac/1e6:.2f} M  (budget {MAC_BUDGET/1e6:.0f} M)")
    s.append(f"tflite    : {os.path.getsize(tflite_path)/1024:.1f} KB  "
             f"(budget {SIZE_BUDGET/1024:.0f} KB)")
    s.append(f"input     : int8, scale {gs:.6f}, zero point {gz}")
    s.append("")
    s.append(f"TEST float32 threshold 0.5: acc {acc*100:.2f}%  "
             f"bird-recall {100*tp/max(tp+fn,1):.2f}%  "
             f"negative-specificity {100*tn/max(tn+fp,1):.2f}%")
    s.append(f"  tp {tp}  fp {fp}  tn {tn}  fn {fn}")
    s.append(f"TEST int8    threshold 0.5: acc {q_acc*100:.2f}%  "
             f"bird-recall {100*qtp/max(qtp+qfn,1):.2f}%  "
             f"negative-specificity {100*qtn/max(qtn+qfp,1):.2f}%"
             f"   (the cost of quantisation {(q_acc-acc)*100:+.2f} points)")
    s.append(f"  tp {qtp}  fp {qfp}  tn {qtn}  fn {qfn}")
    s.append("")
    s.append("epoch  loss    acc        bird-recall  negative-specificity")
    for d, k, ac, gc, oz in history:
        s.append(f"{d:5d}  {k:.4f}  {ac*100:6.2f}%  {gc*100:11.2f}%  "
                 f"{oz*100:20.2f}%")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(a.out, "binary_net_report.txt"), "w",
              encoding="utf-8") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    sys.exit(main())
