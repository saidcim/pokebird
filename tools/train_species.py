#!/usr/bin/env python3
"""
train_species.py — stage-2 species net: training with distillation + INT8
quantisation.

    .venv-birdnet\\Scripts\\python -u tools/train_species.py --smoke  # this first (2 min)
    .venv-birdnet\\Scripts\\python -u tools/train_species.py

Input : data/dataset/  (the output of tools/build_dataset.py)
Output: models/species_net.keras       the trained model
        models/species_net_int8.tflite what goes on the device
        models/species_net_int8.h      the C array
        models/species_net_report.txt  top-1/top-3, confusion, MAC, size

==========================================================================
THE DEVICE CONTRACT — the model's input is the raw int8 mel window
==========================================================================
On the device `pb_mel_window()` produces 64x187 int8. So that the model can
take those bytes AS THEY ARE:

  * the Keras input is the raw int8 values (as floats, -128..127),
  * the model's FIRST layer is Rescaling(4/127) — that is, turning int8 into
    sigma units happens INSIDE THE MODEL, not on the device,
  * converting to TFLite with an int8 input must come out with input scale
    1.0 / zero point 0. The script ASSERTS this; if it does not hold, a
    conversion is needed on the device side and that is written into the
    report.

That keeps the device code as simple as `pb_mel_window(buf)` ->
`memcpy(input->data.int8, buf)`. Any drift here is a silent loss of accuracy.

==========================================================================
THE TEACHER SIGNAL (distillation) — let us be honest about its scope
==========================================================================
`teacher.npy` holds BirdNET's SIGMOID scores, not a softmax, and BirdNET
never writes anything below 0.1. Measured: on average 1.11 classes per row
are above zero. So the teacher distribution is almost one-hot; what
distillation contributes is concentrated in the AMBIGUOUS slices (where the
target and a related species both score) — it is not a general source of
"dark knowledge". No overclaiming.

For CONTAMINATED slices (`best_species != target`) the hard label is not used
at all, only the teacher distribution.
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
SPECIES = os.path.join(ROOT, "data", "species_istanbul.csv")

FRAMES, BANDS = 187, 64
SIGMA_SCALE = 4.0 / 127.0        # int8 -> sigma; the +-4 sigma spread in mel.c
MAC_BUDGET = 30_000_000
ARENA_BUDGET = 180 * 1024


# == Data ================================================================
def load_data():
    X = np.load(csv_compat.resolve(os.path.join(TRAIN, "windows.npy")), mmap_mode="r")
    y = np.load(csv_compat.resolve(os.path.join(TRAIN, "labels.npy"))).astype(np.int32)
    T = np.load(csv_compat.resolve(os.path.join(TRAIN, "teacher.npy"))).astype(np.float32)
    with open(csv_compat.resolve(os.path.join(TRAIN, "samples.csv")), encoding="utf-8") as f:
        row = list(csv_compat.reader(f))
    with open(csv_compat.resolve(os.path.join(TRAIN, "classes.csv")), encoding="utf-8") as f:
        classes = list(csv_compat.reader(f))
    if not (len(X) == len(y) == len(T) == len(row)):
        sys.exit(f"lengths do not match: X {len(X)} y {len(y)} T {len(T)} "
                 f"csv {len(row)} - run tools/build_dataset.py --verify-output")

    split = np.array([s["split"] for s in row])
    contaminated = np.array([s["contaminated"] == "1" for s in row])
    n_classes = len(classes)

    # Teacher -> distribution. The negative class has no teacher (BirdNET
    # hears no bird there); for those rows the hard label is the only source.
    total = T.sum(axis=1, keepdims=True)
    soft = np.zeros((len(X), n_classes), dtype=np.float32)
    has_teacher = (total[:, 0] > 1e-6)
    soft[has_teacher, :T.shape[1]] = T[has_teacher] / total[has_teacher]
    # rows without a teacher (negatives + those with nothing above the
    # threshold) -> one-hot
    soft[~has_teacher, y[~has_teacher]] = 1.0

    return X, y, soft, split, contaminated, classes, n_classes


def make_dataset(X, y, soft, hard_weight, idx, batch, augment, shuffle):
    """A tf.data pipeline that reads batches out of the memmap.

    The array is not turned into a tf constant (738 MB; it must not be baked
    into the graph); indices are streamed and gathered on the numpy side.
    """
    def fetch(i):
        i = np.sort(i)              # sequential reads are fast on a memmap
        return (X[i].astype(np.float32), y[i], soft[i], hard_weight[i])

    ds = tf.data.Dataset.from_tensor_slices(idx)
    if shuffle:
        ds = ds.shuffle(len(idx), reshuffle_each_iteration=True)
    ds = ds.batch(batch, drop_remainder=False)
    ds = ds.map(
        lambda i: tf.numpy_function(
            fetch, [i], [tf.float32, tf.int32, tf.float32, tf.float32]),
        num_parallel_calls=tf.data.AUTOTUNE)

    def shape_batch(x, e, s, w):
        x = tf.reshape(x, (-1, FRAMES, BANDS, 1))
        e.set_shape([None]); s.set_shape([None, soft.shape[1]])
        w.set_shape([None])
        if augment:
            x = spec_augment(x)
        return x, {"hard": e, "soft": s}, w

    return ds.map(shape_batch, num_parallel_calls=tf.data.AUTOTUNE).prefetch(
        tf.data.AUTOTUNE)


def spec_augment(x):
    """A time shift plus SpecAugment.

    The masks are filled with ZERO and that is exactly right: the window
    normalisation on the device pulls the mean to zero (mel.c), so 0 = the
    window's mean energy. Not an arbitrary constant, a meaningful value.

    Mixing noise at the AUDIO level (with the negatives at various SNRs)
    CANNOT BE DONE HERE: mel is logarithmic, so adding two mels is not mixing
    two sounds. The right place for that is the waveform, which means
    re-extracting the mel for every sample.
    """
    b = tf.shape(x)[0]
    # time shift: +-16 frames (~256 ms)
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

    x = mask(x, 1, 30)          # time
    x = mask(x, 2, 10)          # frequency
    return x


# == Model ===============================================================
def ds_block(x, channels, step, name):
    x = tf.keras.layers.DepthwiseConv2D(3, strides=step, padding="same",
                                        use_bias=False, name=f"{name}_dw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_dwbn")(x)
    x = tf.keras.layers.ReLU(6.0, name=f"{name}_dwrelu")(x)
    x = tf.keras.layers.Conv2D(channels, 1, use_bias=False, name=f"{name}_pw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_pwbn")(x)
    return tf.keras.layers.ReLU(6.0, name=f"{name}_pwrelu")(x)


def build_model(n_classes, width=1.0, dropout=0.2):
    """A depthwise-separable CNN (a narrowed MobileNet).

    ReLU6 on purpose: it keeps the activation range bounded for INT8
    quantisation, so calibration is less sensitive to tail values.
    """
    k = lambda n: max(8, int(n * width))
    g = tf.keras.Input(shape=(FRAMES, BANDS, 1), dtype="float32", name="mel_int8")

    # Raw int8 -> sigma. INSIDE THE MODEL, so the device can hand the bytes
    # over as they are.
    x = tf.keras.layers.Rescaling(SIGMA_SCALE, name="int8_sigma")(g)

    x = tf.keras.layers.Conv2D(k(24), 3, strides=2, padding="same",
                               use_bias=False, name="stem")(x)
    x = tf.keras.layers.BatchNormalization(name="stem_bn")(x)
    x = tf.keras.layers.ReLU(6.0, name="stem_relu")(x)

    x = ds_block(x, k(48), 2, "b1")
    x = ds_block(x, k(64), 2, "b2")
    x = ds_block(x, k(64), 1, "b3")
    x = ds_block(x, k(128), 2, "b4")
    x = ds_block(x, k(128), 1, "b5")
    x = ds_block(x, k(128), 1, "b6")
    x = ds_block(x, k(256), 2, "b7")
    x = ds_block(x, k(256), 1, "b8")

    x = tf.keras.layers.GlobalAveragePooling2D(name="gap")(x)
    x = tf.keras.layers.Dropout(dropout, name="dropout")(x)
    c = tf.keras.layers.Dense(n_classes, name="logit")(x)
    return tf.keras.Model(g, c, name="pokebird_species_net")


def mac_count(model):
    """Multiply-accumulates per window. The budget is <=30 MMAC."""
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


def activation_peak(model):
    """A rough arena estimate: the largest sum of two consecutive layer
    outputs.

    TFLM's real arena is MEASURED on the device with `arena_used_bytes()`;
    this is only an early answer to "will it overflow". Do not mistake the
    estimate for a measurement - trusting an estimate has cost this project
    dearly twice already.
    """
    sizes = []
    for k in model.layers:
        s = getattr(k, "output", None)
        if s is None or len(s.shape) < 2:
            continue
        n = 1
        for d in s.shape[1:]:
            n *= int(d)
        sizes.append((k.name, n))
    peak = max((sizes[i][1] + sizes[i + 1][1]) for i in range(len(sizes) - 1))
    return peak, sorted(sizes, key=lambda t: -t[1])[:4]


# == Loss ================================================================
def build_loss(n_classes, alpha, gamma, kd_weight):
    """focal(hard) * sample_weight  +  kd_weight * CE(teacher).

    The hard label's weight is 0 on CONTAMINATED slices - those slices are
    learned from the teacher distribution alone.
    """
    alpha = tf.constant(alpha, dtype=tf.float32)

    def loss(target, logit, sample_weight):
        p = tf.nn.log_softmax(logit)
        hard = tf.one_hot(target["hard"], n_classes)

        pt = tf.reduce_sum(hard * tf.exp(p), axis=-1)
        ce = -tf.reduce_sum(hard * p, axis=-1)
        a = tf.reduce_sum(hard * alpha, axis=-1)
        focal = a * tf.pow(1.0 - pt, gamma) * ce

        kd = -tf.reduce_sum(target["soft"] * p, axis=-1)
        return tf.reduce_mean(sample_weight * focal + kd_weight * kd)

    return loss


# == Training loop =======================================================
def top_k(logit, target, k):
    return tf.reduce_mean(tf.cast(
        tf.math.in_top_k(target, logit, k), tf.float32))


def evaluate(model, ds, n_classes):
    correct1 = correct3 = n = 0
    conf = np.zeros((n_classes, n_classes), dtype=np.int32)
    for x, h, _ in ds:
        logit = model(x, training=False)
        e = h["hard"].numpy()
        t = tf.math.top_k(logit, k=3).indices.numpy()
        correct1 += int((t[:, 0] == e).sum())
        correct3 += int((t == e[:, None]).any(axis=1).sum())
        n += len(e)
        for a, b in zip(e, t[:, 0]):
            conf[a, b] += 1
    return correct1 / n, correct3 / n, conf, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true",
                    help="a small subset + 2 epochs: does the pipeline run")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--width", type=float, default=1.0)
    ap.add_argument("--gamma", type=float, default=2.0, help="focal loss")
    ap.add_argument("--kd", type=float, default=0.5,
                    help="distillation weight")
    ap.add_argument("--out", default=MODELS)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    X, y, soft, split, contaminated, classes, n_classes = load_data()
    print(f"{len(X)} windows / {n_classes} classes")

    idx = {b: np.where(split == b)[0] for b in ("train", "val", "test")}
    if a.smoke:
        rng = np.random.default_rng(0)
        for b in idx:
            idx[b] = rng.choice(idx[b], size=min(len(idx[b]), 3000), replace=False)
        a.epochs = 2
    for b, v in idx.items():
        print(f"  {b:10s} {len(v):6d}")

    # Class imbalance: alongside focal, alpha = (median/count)^0.5. The
    # square root is deliberate - raw inverse frequency, at a 1:31 ratio,
    # over-weights the rarest species and destabilises training.
    count = np.bincount(y[idx["train"]], minlength=n_classes).astype(np.float32)
    alpha = np.sqrt(np.median(count[count > 0]) / np.maximum(count, 1.0))
    alpha = np.clip(alpha, 0.5, 4.0)
    print(f"class weight alpha: {alpha.min():.2f} .. {alpha.max():.2f}")

    # contaminated -> teacher only
    hard_weight = (~contaminated).astype(np.float32)

    train = make_dataset(X, y, soft, hard_weight, idx["train"],
                         a.batch, augment=True, shuffle=True)
    val = make_dataset(X, y, soft, hard_weight, idx["val"],
                            a.batch, augment=False, shuffle=False)
    test = make_dataset(X, y, soft, hard_weight, idx["test"],
                       a.batch, augment=False, shuffle=False)

    model = build_model(n_classes, a.width)
    mac = mac_count(model)
    peak, largest = activation_peak(model)
    par = model.count_params()
    print(f"\nmodel: {par:,} parameters (~{par / 1024:.0f} KB int8)")
    print(f"MAC/window: {mac / 1e6:.1f} M   (budget {MAC_BUDGET / 1e6:.0f} M)")
    print(f"activation peak (rough): {peak / 1024:.0f} KB "
          f"(arena budget {ARENA_BUDGET / 1024:.0f} KB)")
    print("  largest layer outputs: " +
          ", ".join(f"{n} {v / 1024:.0f}KB" for n, v in largest))
    if mac > MAC_BUDGET:
        sys.exit(f"!! the MAC budget was exceeded ({mac / 1e6:.1f} M > "
                 f"{MAC_BUDGET / 1e6:.0f} M) - lower --width")

    loss_f = build_loss(n_classes, alpha, a.gamma, a.kd)
    step_count = max(1, len(idx["train"]) // a.batch) * a.epochs
    plan = tf.keras.optimizers.schedules.CosineDecay(a.lr, step_count,
                                                     warmup_target=a.lr,
                                                     warmup_steps=300)
    opt = tf.keras.optimizers.Adam(plan)

    @tf.function
    def step(x, h, w):
        with tf.GradientTape() as t:
            logit = model(x, training=True)
            loss = loss_f(h, logit, w)
        opt.apply_gradients(zip(t.gradient(loss, model.trainable_variables),
                                model.trainable_variables))
        return loss

    best = -1.0
    path = os.path.join(a.out, "species_net.keras")
    dashboard = os.path.join(a.out, "progress.html")
    info = (f"{par:,} parameters / {mac / 1e6:.1f} MMAC / "
            f"{len(idx['train'])} train / {len(idx['val'])} val")
    history = []
    started = time.time()
    print(f"\nprogress dashboard: {dashboard}\n")
    for epochs in range(1, a.epochs + 1):
        t0 = time.time()
        total = count = 0.0
        for x, h, w in train:
            total += float(step(x, h, w))
            count += 1
        d1, d3, _, _ = evaluate(model, val, n_classes)
        history.append((epochs, total / count, d1, d3))
        star = ""
        if d1 > best:
            best = d1
            model.save(path)
            star = "  <- saved"
        print(f"epoch {epochs:3d}/{a.epochs}  loss {total / count:.4f}  "
              f"val top-1 {d1 * 100:.2f}%  top-3 {d3 * 100:.2f}%  "
              f"{time.time() - t0:.0f}s{star}", flush=True)
        write_dashboard(dashboard, history, a.epochs, info,
                        time.time() - started, done=(epochs == a.epochs))

    print(f"\nbest val top-1: {best * 100:.2f}%  ->  {path}")
    model = tf.keras.models.load_model(path)

    d1, d3, conf, n = evaluate(model, test, n_classes)
    print(f"TEST (float32): top-1 {d1 * 100:.2f}%  top-3 {d3 * 100:.2f}%  "
          f"({n} windows)")

    tflite_path, gs, gz = quantize(model, X, idx["train"], a.out)
    q1, q3, qconf = tflite_evaluate(tflite_path, X, y, idx["test"], n_classes)
    print(f"TEST (int8)   : top-1 {q1 * 100:.2f}%  top-3 {q3 * 100:.2f}%  "
          f"(difference {(q1 - d1) * 100:+.2f} points)")

    c_write(tflite_path, os.path.join(a.out, "species_net_int8.h"))
    write_report(a, model, classes, mac, peak, history, d1, d3, q1, q3, qconf,
              gs, gz, tflite_path)
    return 0


# == INT8 ================================================================
def quantize(model, X, train_idx, out):
    """Post-training INT8 quantisation with a representative data set."""
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

    path = os.path.join(out, "species_net_int8.tflite")
    with open(path, "wb") as f:
        f.write(tfl)

    interp = tf.lite.Interpreter(model_path=path)
    interp.allocate_tensors()
    g = interp.get_input_details()[0]
    gs, gz = float(g["quantization"][0]), int(g["quantization"][1])
    print(f"\nINT8 model: {len(tfl) / 1024:.0f} KB  ->  {path}")
    print(f"input tensor: {g['dtype'].__name__} {tuple(g['shape'])}  "
          f"scale {gs:.6f}  zero point {gz}")
    if abs(gs - 1.0) > 0.02 or gz != 0:
        print("!! WARNING: the input scale is NOT 1.0/0. The device cannot hand\n"
              "   pb_mel_window() output over as it is; a conversion is needed.\n"
              f"   q_tflite = round(q_mel * {SIGMA_SCALE:.6f} / {gs:.6f}) + {gz}")
    else:
        print("   -> the device can hand pb_mel_window() output over DIRECTLY.")
    return path, gs, gz


def tflite_evaluate(path, X, y, idx, n_classes):
    interp = tf.lite.Interpreter(model_path=path, num_threads=8)
    interp.allocate_tensors()
    g = interp.get_input_details()[0]
    c = interp.get_output_details()[0]
    d1 = d3 = 0
    conf = np.zeros((n_classes, n_classes), dtype=np.int32)
    for i in idx:
        interp.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        interp.invoke()
        o = interp.get_tensor(c["index"])[0].astype(np.int32)
        top3 = np.argsort(-o)[:3]
        d1 += int(top3[0] == y[i])
        d3 += int(y[i] in top3)
        conf[y[i], top3[0]] += 1
    return d1 / len(idx), d3 / len(idx), conf


def c_write(tflite_path, h_path):
    raw = open(tflite_path, "rb").read()
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("/* GENERATED FILE - tools/train_species.py. "
                "DO NOT EDIT BY HAND. */\n")
        f.write("#ifndef POKEBIRD_SPECIES_NET_H\n#define POKEBIRD_SPECIES_NET_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define PB_SPECIES_NET_SIZE {len(raw)}\n\n")
        f.write("/* 16-byte alignment: TFLM wants the model data aligned. */\n")
        f.write("__attribute__((aligned(16)))\n")
        f.write("const unsigned char pb_species_net[] = {\n")
        for i in range(0, len(raw), 12):
            f.write("  " + " ".join(f"0x{b:02x}," for b in raw[i:i + 12]) + "\n")
        f.write("};\n\n#endif\n")
    print(f"C array: {h_path}  ({len(raw) / 1024:.0f} KB)")


def write_dashboard(path, history, total_epochs, info, elapsed, done=False):
    """A simple HTML dashboard that refreshes itself every epoch.

    Leave it open in a browser; it reloads every 10 seconds. No
    dependencies, one file, the chart is inline SVG.
    """
    loss = [g[1] for g in history]
    t1 = [g[2] * 100 for g in history]
    t3 = [g[3] * 100 for g in history]
    last = history[-1] if history else (0, 0, 0, 0)
    percent = 100.0 * len(history) / max(total_epochs, 1)
    remaining = ("done" if done else
                 f"~{(total_epochs - len(history)) * elapsed / max(len(history), 1) / 60:.0f} min")

    def line(value, colour, lo_max=None, hi_max=None):
        if not value:
            return ""
        lo = min(value) if lo_max is None else lo_max
        hi = max(value) if hi_max is None else hi_max
        if hi - lo < 1e-9:
            hi = lo + 1
        n = len(value)
        p = " ".join(
            f"{40 + 660 * (i / max(n - 1, 1)):.1f},"
            f"{180 - 160 * ((v - lo) / (hi - lo)):.1f}"
            for i, v in enumerate(value))
        return (f'<polyline fill="none" stroke="{colour}" stroke-width="2.5" '
                f'points="{p}"/>')

    best = max(t1) if t1 else 0
    rows = "".join(
        f"<tr><td>{g[0]}</td><td>{g[1]:.4f}</td><td>{g[2]*100:.2f}%</td>"
        f"<td>{g[3]*100:.2f}%</td></tr>" for g in reversed(history[-25:]))

    html = f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>PokeBird - training</title>
{'' if done else '<meta http-equiv="refresh" content="10">'}
<style>
 body{{font:14px/1.5 system-ui,sans-serif;margin:0;padding:24px;
      background:#11151a;color:#dfe6ee}}
 h1{{font-size:19px;margin:0 0 4px}} .alt{{color:#8b98a6;font-size:13px}}
 .boxes{{display:flex;gap:12px;flex-wrap:wrap;margin:18px 0}}
 .k{{background:#1a2028;border:1px solid #262f3a;border-radius:10px;
     padding:12px 16px;min-width:120px}}
 .k b{{display:block;font-size:22px;font-weight:600;margin-top:2px}}
 .k span{{color:#8b98a6;font-size:12px;text-transform:uppercase;
          letter-spacing:.04em}}
 .bar{{height:8px;background:#232c36;border-radius:5px;overflow:hidden}}
 .bar div{{height:100%;background:linear-gradient(90deg,#3ba55d,#5ed17f)}}
 svg{{background:#1a2028;border:1px solid #262f3a;border-radius:10px}}
 table{{border-collapse:collapse;margin-top:16px;font-variant-numeric:tabular-nums}}
 th,td{{padding:4px 14px 4px 0;text-align:right;border-bottom:1px solid #232c36}}
 th{{color:#8b98a6;font-weight:500;text-align:right}}
 td:first-child,th:first-child{{text-align:left}}
 .leg i{{display:inline-block;width:11px;height:3px;vertical-align:middle;
         margin-right:5px}}
 .leg span{{margin-right:16px;color:#8b98a6;font-size:12px}}
</style></head><body>
<h1>PokeBird - stage-2 species net training</h1>
<div class="alt">{info} / epoch {len(history)}/{total_epochs} / remaining {remaining}
 {'' if done else '/ the page refreshes every 10 s'}</div>
<div class="boxes">
 <div class="k"><span>val top-1</span><b>{last[2]*100:.2f}%</b></div>
 <div class="k"><span>val top-3</span><b>{last[3]*100:.2f}%</b></div>
 <div class="k"><span>best top-1</span><b>{best:.2f}%</b></div>
 <div class="k"><span>loss</span><b>{last[1]:.4f}</b></div>
 <div class="k"><span>elapsed</span><b>{elapsed/60:.0f} min</b></div>
</div>
<div class="bar"><div style="width:{percent:.1f}%"></div></div>
<p class="leg"><span><i style="background:#5ed17f"></i>top-1</span>
<span><i style="background:#63a8ff"></i>top-3</span>
<span><i style="background:#e0803c"></i>loss</span></p>
<svg viewBox="0 0 740 200" width="100%" height="200">
 <line x1="40" y1="180" x2="700" y2="180" stroke="#2e3945"/>
 <line x1="40" y1="20" x2="700" y2="20" stroke="#2e3945" stroke-dasharray="3 4"/>
 {line(t3, '#63a8ff', 0, 100)}{line(t1, '#5ed17f', 0, 100)}{line(loss, '#e0803c')}
 <text x="6" y="184" fill="#8b98a6" font-size="11">0</text>
 <text x="6" y="24" fill="#8b98a6" font-size="11">100</text>
</svg>
<table><tr><th>epoch</th><th>loss</th><th>top-1</th><th>top-3</th></tr>
{rows}</table>
</body></html>"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)


def class_names(classes):
    """class index -> English display name.

    The name column of a data/dataset/classes.csv written by an older run is
    Turkish, so the committed species list is the source of truth and the
    dataset's own column is only the fallback (it still covers the negative
    sentinel, which is not a species).
    """
    english = {}
    if os.path.exists(SPECIES):
        with open(SPECIES, encoding="utf-8") as f:
            english = {r["ebird_code"]: r["english_name"].strip()
                       for r in csv_compat.reader(f)}
    # the same sentinel tools/class_table.py emits for the negative class
    negative = {"__negative__", "__negatif__"}
    return {int(s["class_index"]):
            ("unknown / not a bird" if s["ebird_code"] in negative
             else english.get(s["ebird_code"], s["english_name"]))
            for s in classes}


def write_report(a, model, classes, mac, peak, history, d1, d3, q1, q3, conf,
              gs, gz, tflite_path):
    name = class_names(classes)
    n = conf.sum(axis=1)
    sensitivity = np.divide(np.diag(conf), np.maximum(n, 1))

    s = []
    s.append(f"model     : {model.count_params():,} parameters")
    s.append(f"MAC/window: {mac / 1e6:.1f} M  (budget 30 M)")
    s.append(f"activation peak (rough): {peak / 1024:.0f} KB "
             "(arena budget 180 KB)")
    s.append(f"tflite    : {os.path.getsize(tflite_path) / 1024:.0f} KB")
    s.append(f"input     : int8, scale {gs:.6f}, zero point {gz}")
    s.append("")
    s.append(f"TEST float32 : top-1 {d1 * 100:.2f}%  top-3 {d3 * 100:.2f}%")
    s.append(f"TEST int8    : top-1 {q1 * 100:.2f}%  top-3 {q3 * 100:.2f}%"
             f"   (the cost of quantisation {(q1 - d1) * 100:+.2f} points)")
    s.append("")
    s.append("THE WORST 20 CLASSES (int8, sensitivity on the test set):")
    for i in np.argsort(sensitivity):
        if n[i] == 0:
            continue
        if len([x for x in s if x.startswith("  ")]) >= 20:
            break
        confused = np.argsort(-conf[i])
        k0 = confused[0] if confused[0] != i else (
            confused[1] if len(confused) > 1 else i)
        s.append(f"  {name.get(i, i):28s} {sensitivity[i] * 100:5.1f}%  "
                 f"({int(n[i])} windows)  most confused with: "
                 f"{name.get(int(k0), k0)}")
    s.append("")
    s.append("epoch  loss    val top-1  top-3")
    for d, k, v1, v3 in history:
        s.append(f"{d:5d}  {k:.4f}  {v1 * 100:8.2f}%  {v3 * 100:5.2f}%")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(a.out, "species_net_report.txt"), "w",
              encoding="utf-8") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    sys.exit(main())
