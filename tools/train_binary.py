#!/usr/bin/env python3
"""
ikili_egit.py — Asama-1 ikili ag (kus var/yok): egitim + INT8 nicelestirme. (M7)

    .venv-birdnet\\Scripts\\python -u tools/train_binary.py --duman     # once bu
    .venv-birdnet\\Scripts\\python -u tools/train_binary.py

Girdi : data/egitim/  (tools/build_dataset.py ciktisi — tur agiyla AYNI veri)
Cikti : models/ikili_agi.keras
        models/ikili_agi_int8.tflite
        models/binary_net_int8.h
        models/binary_net_report.txt

==========================================================================
NEDEN AYRI VERI KUMESI YOK
==========================================================================
data/egitim/etiket.npy zaten 179 sinifli: 0..177 kus turleri, 178 =
"__negatif__" (ESC-50, kus siniflari cikarilmis — §9j). Asama-1'in ihtiyaci
olan tek sey bu etiketin ikiliye indirgenmesi: sinif != 178 -> KUS (1),
sinif == 178 -> DEGIL (0). Ayni pencereler.npy (64x187 int8 mel) GIRDI
olarak kullaniliyor, cihazdaki pb_mel_window() ciktisiyla ayni sozlesme.

BULASIK PENCERELER (en_iyi_tur != hedef) tur agi icin sorunluydu (yanlis
sert etiket) ama BURADA SORUN DEGIL: bulasik bir pencere hala KUS SESI,
sadece BirdNET'in en iyi tahmini farkli bir tur. Asama-1 "kus mu degil mi"
sorusuna bakiyor, TUR'e degil — o yuzden bulasik pencereler de tam agirlikla
egitime giriyor (tur agindaki gibi disari birakilmiyor).

OGRETMEN SINYALI (ogretmen.npy, BirdNET sigmoid) KULLANILMIYOR: o dagilim
TUR bazinda, ikili soruya dogrudan tasinmiyor (bir turun BirdNET skoru
dusuk olabilir ama yine de KUS'tur). Duz agirlikli BCE yeterli.

==========================================================================
SINIF DENGESIZLIGI — OLCULDU
==========================================================================
57.622 kus penceresi / 3.489 negatif = 16,5:1. Kayip fonksiyonu negatif
sinifi bu oranla agirlikliyor (pos_agirlik = kus/negatif, --agirlik ile
degistirilebilir).

==========================================================================
ESIK SECIMI — kacirma (false negative) pahali, gecirme (false positive) ucuz
==========================================================================
Asama-1 "degil" derse Asama-2 (tur agi) hic calismiyor — yanlis "degil"
GERCEK BIR KUS TESPITINI SESSIZCE KAYBEDER. "kus" derse ve yanlissa,
bedel yalnizca bosa harcanan bir Asama-2 cikarimi (Asama-2'nin kendi
negatif sinifi zaten var, o da "bilinmiyor" der). Bu yuzden varsayilan
karar esigi 0.5 DEGIL — tools/ikili_esik_olc.py ile olculup rapora yazilir.
"""

import argparse
import csv
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
TRAIN = os.path.join(ROOT, "data", "egitim")
MODELS = os.path.join(ROOT, "models")

FRAMES, BANDS = 187, 64
SIGMA_SCALE = 4.0 / 127.0
NEGATIVE_CLS = 178
MAC_BUDGET = 5_000_000       # tur aginin 30M'inin cok altinda — sik calisacak
SIZE_BUDGET = 15 * 1024     # ARCHITECTURE §4


def data_yukle():
    X = np.load(csv_compat.resolve(os.path.join(TRAIN, "windows.npy")), mmap_mode="r")
    y_species = np.load(csv_compat.resolve(os.path.join(TRAIN, "labels.npy"))).astype(np.int32)
    with open(csv_compat.resolve(os.path.join(TRAIN, "samples.csv")), encoding="utf-8") as f:
        row = list(csv_compat.reader(f))
    if not (len(X) == len(y_species) == len(row)):
        sys.exit(f"uzunluklar tutmuyor: X {len(X)} y {len(y_species)} "
                 f"csv {len(row)}")

    split = np.array([s["split"] for s in row])
    y = (y_species != NEGATIVE_CLS).astype(np.int32)   # 1=kus, 0=degil
    return X, y, split


def make_dataset(X, y, class_weight, idx, batch, augment, shuffle):
    def getir(i):
        i = np.sort(i)
        return (X[i].astype(np.float32), y[i].astype(np.float32),
                class_weight[i])

    ds = tf.data.Dataset.from_tensor_slices(idx)
    if shuffle:
        ds = ds.shuffle(len(idx), reshuffle_each_iteration=True)
    ds = ds.batch(batch, drop_remainder=False)
    ds = ds.map(
        lambda i: tf.numpy_function(getir, [i],
                                    [tf.float32, tf.float32, tf.float32]),
        num_parallel_calls=tf.data.AUTOTUNE)

    def bicim(x, e, w):
        x = tf.reshape(x, (-1, FRAMES, BANDS, 1))
        e.set_shape([None]); w.set_shape([None])
        if augment:
            x = artirma(x)
        return x, e, w

    return ds.map(bicim, num_parallel_calls=tf.data.AUTOTUNE).prefetch(
        tf.data.AUTOTUNE)


def artirma(x):
    """egit.py ile ayni: zaman kaydirma + SpecAugment, maske degeri 0."""
    b = tf.shape(x)[0]
    k = tf.random.uniform([], -16, 17, dtype=tf.int32)
    x = tf.roll(x, shift=k, axis=1)

    def maskele(x, eksen, max_extra):
        boy = tf.shape(x)[eksen]
        width = tf.random.uniform([b, 1], 0, max_extra, dtype=tf.int32)
        start = tf.random.uniform([b, 1], 0, boy - max_extra, dtype=tf.int32)
        r = tf.reshape(tf.range(boy), [1, -1])
        m = tf.cast((r < start) | (r >= start + width), x.dtype)
        sekil = [b, 1, 1, 1]
        sekil[eksen] = boy
        return x * tf.reshape(m, sekil)

    x = maskele(x, 1, 30)
    x = maskele(x, 2, 10)
    return x


def ds_blok(x, kanal, step, name):
    x = tf.keras.layers.DepthwiseConv2D(3, strides=step, padding="same",
                                        use_bias=False, name=f"{name}_dw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_dwbn")(x)
    x = tf.keras.layers.ReLU(6.0, name=f"{name}_dwrelu")(x)
    x = tf.keras.layers.Conv2D(kanal, 1, use_bias=False, name=f"{name}_pw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_pwbn")(x)
    return tf.keras.layers.ReLU(6.0, name=f"{name}_pwrelu")(x)


def model_kur(width=1.0):
    """Kucuk derinlemesine ayrilabilir CNN — tur aginin 8 blogundan cok daha
    dar/kisa. Butce 15 KB int8; tur aginin ~270 KB'inin 1/18'i."""
    k = lambda n: max(4, int(n * width))
    g = tf.keras.Input(shape=(FRAMES, BANDS, 1), dtype="float32", name="mel_int8")
    x = tf.keras.layers.Rescaling(SIGMA_SCALE, name="int8_sigma")(g)

    x = tf.keras.layers.Conv2D(k(16), 3, strides=2, padding="same",
                               use_bias=False, name="giris")(x)
    x = tf.keras.layers.BatchNormalization(name="giris_bn")(x)
    x = tf.keras.layers.ReLU(6.0, name="giris_relu")(x)

    x = ds_blok(x, k(32), 2, "b1")
    x = ds_blok(x, k(48), 2, "b2")
    x = ds_blok(x, k(64), 2, "b3")

    x = tf.keras.layers.GlobalAveragePooling2D(name="gap")(x)
    c = tf.keras.layers.Dense(1, name="logit")(x)
    return tf.keras.Model(g, c, name="pokebird_ikili_agi")


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


def loss_kur(pos_weight):
    """Agirlikli BCE. pos_agirlik: NEGATIF sinifina (0) verilen carpan —
    16,5:1 dengesizligi tersine cevirmek icin negatif ornek basina agirlik."""
    pos_weight = tf.constant(float(pos_weight), dtype=tf.float32)

    def loss(y, logit, w):
        ce = tf.nn.sigmoid_cross_entropy_with_logits(labels=y, logits=logit[:, 0])
        weight = tf.where(y > 0.5, tf.ones_like(y), tf.fill(tf.shape(y), pos_weight))
        return tf.reduce_mean(ce * weight * w)
    return loss


def degerlendir(model, ds, threshold=0.5):
    dogru = total = 0
    tp = fp = tn = fn = 0
    for x, y, _ in ds:
        logit = model(x, training=False)
        p = tf.sigmoid(logit[:, 0]).numpy()
        e = y.numpy()
        pred = (p >= threshold).astype(np.int32)
        dogru += int((pred == e).sum())
        total += len(e)
        tp += int(((pred == 1) & (e == 1)).sum())
        fp += int(((pred == 1) & (e == 0)).sum())
        tn += int(((pred == 0) & (e == 0)).sum())
        fn += int(((pred == 0) & (e == 1)).sum())
    return dogru / total, tp, fp, tn, fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--width", type=float, default=1.0)
    ap.add_argument("--weight", type=float, default=0.0,
                    help="negatif sinif agirligi; 0 = olculen 16,5 kullan")
    ap.add_argument("--out", default=MODELS)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    X, y, split = data_yukle()
    print(f"{len(X)} pencere  kus {int(y.sum())}  degil {int((1 - y).sum())}")

    idx = {b: np.where(split == b)[0] for b in ("egitim", "dogrulama", "test")}
    if a.smoke:
        rng = np.random.default_rng(0)
        for b in idx:
            idx[b] = rng.choice(idx[b], size=min(len(idx[b]), 3000), replace=False)
        a.epochs = 2
    for b, v in idx.items():
        print(f"  {b:10s} {len(v):6d}  kus %{100*y[v].mean():.1f}")

    pos_weight = a.weight if a.weight > 0 else (
        (y[idx["egitim"]] == 1).sum() / max((y[idx["egitim"]] == 0).sum(), 1))
    print(f"negatif sinif agirligi: {pos_weight:.2f}")

    sample_weight = np.ones(len(X), dtype=np.float32)

    train = make_dataset(X, y, sample_weight, idx["egitim"], a.batch,
                         augment=True, shuffle=True)
    val = make_dataset(X, y, sample_weight, idx["dogrulama"], a.batch,
                            augment=False, shuffle=False)
    test = make_dataset(X, y, sample_weight, idx["test"], a.batch,
                       augment=False, shuffle=False)

    model = model_kur(a.width)
    mac = mac_count(model)
    par = model.count_params()
    print(f"\nmodel: {par:,} parametre (~{par / 1024:.1f} KB int8)")
    print(f"MAC/pencere: {mac / 1e6:.2f} M  (butce {MAC_BUDGET / 1e6:.0f} M)")
    if mac > MAC_BUDGET:
        sys.exit(f"!! MAC butcesi asildi ({mac/1e6:.1f}M > {MAC_BUDGET/1e6:.0f}M) "
                 "— --genislik dusurun")
    if par > SIZE_BUDGET:
        print(f"!! DIKKAT: {par} parametre > {SIZE_BUDGET} bayt butcesi "
              "(int8 boyut tflite'ta olculecek, kesin karar orada)")

    loss_f = loss_kur(pos_weight)
    step_count = max(1, len(idx["egitim"]) // a.batch) * a.epochs
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
    basladi = time.time()
    for epochs in range(1, a.epochs + 1):
        t0 = time.time()
        total = count = 0.0
        for x, e, w in train:
            total += float(step(x, e, w))
            count += 1
        acc, tp, fp, tn, fn = degerlendir(model, val)
        geri_cagirma = tp / max(tp + fn, 1)   # recall kus sinifi
        ozgulluk = tn / max(tn + fp, 1)       # negatifi doGru red
        history.append((epochs, total / count, acc, geri_cagirma, ozgulluk))
        yildiz = ""
        skor = geri_cagirma  # kus kacirmamak asil oncelik
        if skor > best:
            best = skor
            model.save(path)
            yildiz = "  <- kaydedildi"
        print(f"devir {epochs:3d}/{a.epochs}  kayip {total/count:.4f}  "
              f"dogrulama acc %{acc*100:.2f}  kus-geri-cagirma %{geri_cagirma*100:.2f}  "
              f"negatif-ozgulluk %{ozgulluk*100:.2f}  {time.time()-t0:.0f}sn{yildiz}",
              flush=True)

    print(f"\nen iyi (dogrulama kus-geri-cagirma): %{best*100:.2f}  ->  {path}")
    model = tf.keras.models.load_model(path)

    acc, tp, fp, tn, fn = degerlendir(model, test)
    print(f"TEST (float32) esik 0.5: acc %{acc*100:.2f}  "
          f"kus-geri-cagirma %{100*tp/max(tp+fn,1):.2f}  "
          f"negatif-ozgulluk %{100*tn/max(tn+fp,1):.2f}  "
          f"(tp {tp} fp {fp} tn {tn} fn {fn})")

    tflite_path, gs, gz = quantize(model, X, idx["egitim"], a.out)
    q_acc, qtp, qfp, qtn, qfn = tflite_degerlendir(tflite_path, X, y, idx["test"])
    print(f"TEST (int8)    esik 0.5: acc %{q_acc*100:.2f}  "
          f"kus-geri-cagirma %{100*qtp/max(qtp+qfn,1):.2f}  "
          f"negatif-ozgulluk %{100*qtn/max(qtn+qfp,1):.2f}  "
          f"(fark {(q_acc-acc)*100:+.2f} puan)")

    c_write(tflite_path, os.path.join(a.out, "ikili_agi_int8.h"))
    rapor_write(a, model, mac, history, acc, tp, fp, tn, fn,
              q_acc, qtp, qfp, qtn, qfn, gs, gz, tflite_path)
    return 0


def quantize(model, X, train_idx, out):
    rng = np.random.default_rng(0)
    sample = np.sort(rng.choice(train_idx, size=min(500, len(train_idx)),
                               replace=False))

    def temsili():
        for i in sample:
            yield [X[i].reshape(1, FRAMES, BANDS, 1).astype(np.float32)]

    d = tf.lite.TFLiteConverter.from_keras_model(model)
    d.optimizations = [tf.lite.Optimize.DEFAULT]
    d.representative_dataset = temsili
    d.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    d.inference_input_type = tf.int8
    d.inference_output_type = tf.int8
    tfl = d.convert()

    path = os.path.join(out, "binary_net_int8.tflite")
    with open(path, "wb") as f:
        f.write(tfl)

    note = tf.lite.Interpreter(model_path=path)
    note.allocate_tensors()
    g = note.get_input_details()[0]
    c = note.get_output_details()[0]
    gs, gz = float(g["quantization"][0]), int(g["quantization"][1])
    print(f"\nINT8 model: {len(tfl)/1024:.1f} KB  (butce {SIZE_BUDGET/1024:.0f} KB)  -> {path}")
    print(f"girdi tensoru: {g['dtype'].__name__} {tuple(g['shape'])}  "
          f"olcek {gs:.6f}  sifir noktasi {gz}")
    if abs(gs - 1.0) > 0.02 or gz != 0:
        print("!! DIKKAT: girdi olcegi 1.0/0 DEGIL. Cihaz pb_mel_window()\n"
              "   ciktisini oldugu gibi veremez; donusum gerekir.\n"
              f"   q_tflite = round(q_mel * {SIGMA_SCALE:.6f} / {gs:.6f}) + {gz}")
    else:
        print("   -> cihaz pb_mel_window() ciktisini DOGRUDAN verebilir.")
    cs, cz = float(c["quantization"][0]), int(c["quantization"][1])
    print(f"cikti tensoru: {c['dtype'].__name__}  olcek {cs:.6f}  sifir noktasi {cz}  "
          "(sigmoid oncesi ham logit)")
    if len(tfl) > SIZE_BUDGET:
        print(f"!! DIKKAT: {len(tfl)/1024:.1f} KB > {SIZE_BUDGET/1024:.0f} KB butcesi")
    return path, gs, gz


def tflite_degerlendir(path, X, y, idx, threshold=0.5):
    note = tf.lite.Interpreter(model_path=path, num_threads=8)
    note.allocate_tensors()
    g = note.get_input_details()[0]
    c = note.get_output_details()[0]
    cs, cz = c["quantization"]
    tp = fp = tn = fn = 0
    for i in idx:
        note.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        note.invoke()
        q = int(note.get_tensor(c["index"])[0][0])
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
        f.write("/* Uretilmis dosya — tools/train_binary.py. ELLE DUZENLEMEYIN. */\n")
        f.write("#ifndef POKEBIRD_BINARY_NET_H\n#define POKEBIRD_BINARY_NET_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define PB_IKILI_AGI_BOYUT {len(raw)}\n\n")
        f.write("__attribute__((aligned(16)))\n")
        f.write("const unsigned char pb_binary_net[] = {\n")
        for i in range(0, len(raw), 12):
            f.write("  " + " ".join(f"0x{b:02x}," for b in raw[i:i + 12]) + "\n")
        f.write("};\n\n#endif\n")
    print(f"C dizisi: {h_path}  ({len(raw)/1024:.1f} KB)")


def rapor_write(a, model, mac, history, acc, tp, fp, tn, fn,
              q_acc, qtp, qfp, qtn, qfn, gs, gz, tflite_path):
    s = []
    s.append(f"model      : {model.count_params():,} parametre")
    s.append(f"MAC/pencere: {mac/1e6:.2f} M  (butce {MAC_BUDGET/1e6:.0f} M)")
    s.append(f"tflite     : {os.path.getsize(tflite_path)/1024:.1f} KB  (butce {SIZE_BUDGET/1024:.0f} KB)")
    s.append(f"girdi      : int8, olcek {gs:.6f}, sifir noktasi {gz}")
    s.append("")
    s.append(f"TEST float32 esik 0.5: acc %{acc*100:.2f}  "
             f"kus-geri-cagirma %{100*tp/max(tp+fn,1):.2f}  "
             f"negatif-ozgulluk %{100*tn/max(tn+fp,1):.2f}")
    s.append(f"  tp {tp}  fp {fp}  tn {tn}  fn {fn}")
    s.append(f"TEST int8    esik 0.5: acc %{q_acc*100:.2f}  "
             f"kus-geri-cagirma %{100*qtp/max(qtp+qfn,1):.2f}  "
             f"negatif-ozgulluk %{100*qtn/max(qtn+qfp,1):.2f}"
             f"   (nicelestirme bedeli {(q_acc-acc)*100:+.2f} puan)")
    s.append(f"  tp {qtp}  fp {qfp}  tn {qtn}  fn {qfn}")
    s.append("")
    s.append("devir  kayip   acc     kus-geri-cagirma  negatif-ozgulluk")
    for d, k, ac, gc, oz in history:
        s.append(f"{d:5d}  {k:.4f}  %{ac*100:6.2f}  %{gc*100:16.2f}  %{oz*100:16.2f}")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(a.out, "ikili_rapor.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    sys.exit(main())
