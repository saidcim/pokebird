#!/usr/bin/env python3
"""
egit.py — Asama-2 tur agi: damitma ile egitim + INT8 niceleştirme. (M5)

    .venv-birdnet\\Scripts\\python -u tools/train_species.py --duman     # once bu (2 dk)
    .venv-birdnet\\Scripts\\python -u tools/train_species.py

Girdi : data/egitim/  (tools/build_dataset.py ciktisi)
Cikti : models/tur_agi.keras          egitilmis model
        models/tur_agi_int8.tflite    cihaza gidecek olan
        models/species_net_int8.h         C dizisi
        models/species_net_report.txt              top-1/top-3, karisiklik, MAC, boyut

==========================================================================
CIHAZ SOZLESMESI — model girdisi ham int8 mel penceresidir
==========================================================================
Cihazda `pb_mel_window()` 64x187 int8 uretiyor. Model o baytlari OLDUGU GIBI
alsin diye:

  * Keras girdisi ham int8 degerleri (float olarak -128..127),
  * modelin ILK katmani Rescaling(4/127) — yani int8'i sigma birimine cevirme
    isi MODELIN ICINDE, cihazda degil,
  * TFLite'a int8 girdiyle donusturulunce girdi olcegi 1.0 / sifir noktasi 0
    cikmali. Betik bunu ASSERT ediyor; tutmazsa cihaz tarafinda donusum
    gerekir ve rapora yaziliyor.

Boylece M6'da cihaz kodu `pb_mel_window(buf)` -> `memcpy(input->data.int8, buf)`
kadar basit kaliyor. Kayma olursa sessiz dogruluk kaybi olur.

==========================================================================
OGRETMEN SINYALI (damitma) — kapsamini bilerek yazalim
==========================================================================
`ogretmen.npy` BirdNET'in SIGMOID skorlari, softmax degil, ve BirdNET 0.1'in
altini hic yazmiyor. Olculdu: satir basina sifirdan buyuk sinif ortalama
1,11. Yani ogretmen dagilimi neredeyse tek-sicak; damitmanin katkisi
BELIRSIZ dilimlerde toplaniyor (hedef ile akraba turun birlikte skor aldigi
yerler) — genel bir "karanlik bilgi" kaynagi degil. Abartmayalim.

BULASIK dilimler (`en_iyi_tur != hedef`) icin sert etiket HIC kullanilmiyor,
yalnizca ogretmen dagilimi kullaniliyor — §9i tuzak 2'nin istedigi bu.
"""

import argparse
import csv
import os
import sys
import time

import numpy as np

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import tensorflow as tf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRAIN = os.path.join(ROOT, "data", "egitim")
MODELLER = os.path.join(ROOT, "models")

FRAMES, BANDS = 187, 64
SIGMA_SCALE = 4.0 / 127.0        # int8 -> sigma; mel.c'deki +-4 sigma yayilimi
MAC_BUDGET = 30_000_000           # ARCHITECTURE §4
ARENA_BUDGET = 180 * 1024         # ARCHITECTURE §5


# ══ Veri ════════════════════════════════════════════════════════════════
def data_yukle():
    X = np.load(os.path.join(TRAIN, "pencereler.npy"), mmap_mode="r")
    y = np.load(os.path.join(TRAIN, "etiket.npy")).astype(np.int32)
    T = np.load(os.path.join(TRAIN, "ogretmen.npy")).astype(np.float32)
    with open(os.path.join(TRAIN, "ornekler.csv"), encoding="utf-8") as f:
        row = list(csv.DictReader(f))
    with open(os.path.join(TRAIN, "siniflar.csv"), encoding="utf-8") as f:
        classes = list(csv.DictReader(f))
    if not (len(X) == len(y) == len(T) == len(row)):
        sys.exit(f"uzunluklar tutmuyor: X {len(X)} y {len(y)} T {len(T)} "
                 f"csv {len(row)} — tools/build_dataset.py --dogrula-cikti")

    split = np.array([s["bolum"] for s in row])
    contaminated = np.array([s["bulasik"] == "1" for s in row])
    n_classes = len(classes)

    # Ogretmen -> dagilim. Negatif sinifin ogretmeni yok (BirdNET orada kus
    # duymuyor); onlarda sert etiket tek kaynak.
    total = T.sum(axis=1, keepdims=True)
    yumusak = np.zeros((len(X), n_classes), dtype=np.float32)
    var = (total[:, 0] > 1e-6)
    yumusak[var, :T.shape[1]] = T[var] / total[var]
    # ogretmensiz satirlar (negatifler + esigi geceni olmayanlar) -> tek sicak
    yumusak[~var, y[~var]] = 1.0

    return X, y, yumusak, split, contaminated, classes, n_classes


def make_dataset(X, y, yumusak, sert_weight, idx, batch, augment, shuffle):
    """memmap'ten yigin okuyan tf.data hatti.

    Diziyi tf sabitine cevirmiyoruz (738 MB, graf icine gomulmemeli);
    indeksleri akitip numpy tarafinda topluyoruz.
    """
    def getir(i):
        i = np.sort(i)                       # memmap'te sirali okuma hizli
        return (X[i].astype(np.float32), y[i], yumusak[i], sert_weight[i])

    ds = tf.data.Dataset.from_tensor_slices(idx)
    if shuffle:
        ds = ds.shuffle(len(idx), reshuffle_each_iteration=True)
    ds = ds.batch(batch, drop_remainder=False)
    ds = ds.map(
        lambda i: tf.numpy_function(
            getir, [i], [tf.float32, tf.int32, tf.float32, tf.float32]),
        num_parallel_calls=tf.data.AUTOTUNE)

    def bicim(x, e, s, w):
        x = tf.reshape(x, (-1, FRAMES, BANDS, 1))
        e.set_shape([None]); s.set_shape([None, yumusak.shape[1]])
        w.set_shape([None])
        if augment:
            x = artirma(x)
        return x, {"sert": e, "yumusak": s}, w

    return ds.map(bicim, num_parallel_calls=tf.data.AUTOTUNE).prefetch(
        tf.data.AUTOTUNE)


def artirma(x):
    """Zaman kaydirma + SpecAugment.

    Maskeleri SIFIRLA dolduruyoruz ve bu tam olarak dogru olan: cihazdaki
    pencere normalizasyonu ortalamayi sifira cekiyor (mel.c), yani 0 =
    pencerenin ortalama enerjisi. Sabit bir sayi degil, anlamli bir deger.

    Ses seviyesinde gurultu karistirma (negatiflerle cesitli SNR'lerde)
    BURADA YAPILAMAZ: mel logaritmik: iki mel'i toplamak iki sesi
    karistirmak degil. Dogru yeri dalga formu; o da her ornekte yeniden mel
    cikarmak demek. M8 saha turuyla birlikte yapilacak.
    """
    b = tf.shape(x)[0]
    # zaman kaydirma: +-16 kare (~256 ms)
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

    x = maskele(x, 1, 30)       # zaman
    x = maskele(x, 2, 10)       # frekans
    return x


# ══ Model ═══════════════════════════════════════════════════════════════
def ds_blok(x, kanal, step, name):
    x = tf.keras.layers.DepthwiseConv2D(3, strides=step, padding="same",
                                        use_bias=False, name=f"{name}_dw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_dwbn")(x)
    x = tf.keras.layers.ReLU(6.0, name=f"{name}_dwrelu")(x)
    x = tf.keras.layers.Conv2D(kanal, 1, use_bias=False, name=f"{name}_pw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{name}_pwbn")(x)
    return tf.keras.layers.ReLU(6.0, name=f"{name}_pwrelu")(x)


def model_kur(n_classes, width=1.0, dropout=0.2):
    """Derinlemesine ayrilabilir CNN (daraltilmis MobileNet), ARCHITECTURE §4.

    ReLU6 bilerek: INT8 niceleştirmede aktivasyon araligini sinirli tutuyor,
    kalibrasyon kuyruk degerlerine daha az duyarli oluyor.
    """
    k = lambda n: max(8, int(n * width))
    g = tf.keras.Input(shape=(FRAMES, BANDS, 1), dtype="float32", name="mel_int8")

    # Ham int8 -> sigma. Cihaz baytlari oldugu gibi versin diye MODELIN ICINDE.
    x = tf.keras.layers.Rescaling(SIGMA_SCALE, name="int8_sigma")(g)

    x = tf.keras.layers.Conv2D(k(24), 3, strides=2, padding="same",
                               use_bias=False, name="giris")(x)
    x = tf.keras.layers.BatchNormalization(name="giris_bn")(x)
    x = tf.keras.layers.ReLU(6.0, name="giris_relu")(x)

    x = ds_blok(x, k(48), 2, "b1")
    x = ds_blok(x, k(64), 2, "b2")
    x = ds_blok(x, k(64), 1, "b3")
    x = ds_blok(x, k(128), 2, "b4")
    x = ds_blok(x, k(128), 1, "b5")
    x = ds_blok(x, k(128), 1, "b6")
    x = ds_blok(x, k(256), 2, "b7")
    x = ds_blok(x, k(256), 1, "b8")

    x = tf.keras.layers.GlobalAveragePooling2D(name="gap")(x)
    x = tf.keras.layers.Dropout(dropout, name="dropout")(x)
    c = tf.keras.layers.Dense(n_classes, name="logit")(x)
    return tf.keras.Model(g, c, name="pokebird_tur_agi")


def mac_count(model):
    """Pencere basina carpma-toplama. Butce ARCHITECTURE §4: <=30 MMAC."""
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


def aktivasyon_tepe(model):
    """Kaba arena tahmini: ardisik iki katman ciktisinin en buyuk toplami.

    TFLM'in gercek arena'si M6'da `arena_used_bytes()` ile OLCULECEK; bu
    yalnizca "tasar mi" sorusunun erken cevabi. Tahmini olcum sanmayin —
    bu projede tahmine guvenmek iki kez pahaliya patladi (§9d-2, §9g).
    """
    boy = []
    for k in model.layers:
        s = getattr(k, "output", None)
        if s is None or len(s.shape) < 2:
            continue
        n = 1
        for d in s.shape[1:]:
            n *= int(d)
        boy.append((k.name, n))
    tepe = max((boy[i][1] + boy[i + 1][1]) for i in range(len(boy) - 1))
    return tepe, sorted(boy, key=lambda t: -t[1])[:4]


# ══ Kayip ═══════════════════════════════════════════════════════════════
def loss_kur(n_classes, alfa, gama, kd_weight):
    """focal(sert) * ornek_agirligi  +  kd_agirlik * CE(ogretmen).

    Sert etiketin agirligi BULASIK dilimlerde 0 — o dilimler yalnizca
    ogretmen dagilimindan ogreniliyor (§9i tuzak 2).
    """
    alfa = tf.constant(alfa, dtype=tf.float32)

    def loss(target, logit, sample_weight):
        p = tf.nn.log_softmax(logit)
        sert = tf.one_hot(target["sert"], n_classes)

        pt = tf.reduce_sum(sert * tf.exp(p), axis=-1)
        ce = -tf.reduce_sum(sert * p, axis=-1)
        a = tf.reduce_sum(sert * alfa, axis=-1)
        focal = a * tf.pow(1.0 - pt, gama) * ce

        kd = -tf.reduce_sum(target["yumusak"] * p, axis=-1)
        return tf.reduce_mean(sample_weight * focal + kd_weight * kd)

    return loss


# ══ Egitim dongusu ══════════════════════════════════════════════════════
def ilk_k(logit, target, k):
    return tf.reduce_mean(tf.cast(
        tf.math.in_top_k(target, logit, k), tf.float32))


def degerlendir(model, ds, n_classes):
    dogru1 = dogru3 = n = 0
    kar = np.zeros((n_classes, n_classes), dtype=np.int32)
    for x, h, _ in ds:
        logit = model(x, training=False)
        e = h["sert"].numpy()
        t = tf.math.top_k(logit, k=3).indices.numpy()
        dogru1 += int((t[:, 0] == e).sum())
        dogru3 += int((t == e[:, None]).any(axis=1).sum())
        n += len(e)
        for a, b in zip(e, t[:, 0]):
            kar[a, b] += 1
    return dogru1 / n, dogru3 / n, kar, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true",
                    help="kucuk alt kume + 2 devir: hat calisiyor mu")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--width", type=float, default=1.0)
    ap.add_argument("--gamma", type=float, default=2.0, help="focal loss")
    ap.add_argument("--kd", type=float, default=0.5, help="damitma agirligi")
    ap.add_argument("--out", default=MODELLER)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    X, y, yumusak, split, contaminated, classes, n_classes = data_yukle()
    print(f"{len(X)} pencere · {n_classes} sinif")

    idx = {b: np.where(split == b)[0] for b in ("egitim", "dogrulama", "test")}
    if a.smoke:
        rng = np.random.default_rng(0)
        for b in idx:
            idx[b] = rng.choice(idx[b], size=min(len(idx[b]), 3000), replace=False)
        a.epochs = 2
    for b, v in idx.items():
        print(f"  {b:10s} {len(v):6d}")

    # Sinif dengesizligi: focal'in yaninda alfa = (ortanca/sayi)^0.5.
    # Karekok bilerek — ham ters frekans 1:31'lik oranda en zayif turu
    # asiri agirliklandirip egitimi dengesizlestiriyor.
    count = np.bincount(y[idx["egitim"]], minlength=n_classes).astype(np.float32)
    alfa = np.sqrt(np.median(count[count > 0]) / np.maximum(count, 1.0))
    alfa = np.clip(alfa, 0.5, 4.0)
    print(f"sinif agirligi alfa: {alfa.min():.2f} .. {alfa.max():.2f}")

    sert_weight = (~contaminated).astype(np.float32)   # bulasik -> yalniz ogretmen

    train = make_dataset(X, y, yumusak, sert_weight, idx["egitim"],
                         a.batch, augment=True, shuffle=True)
    val = make_dataset(X, y, yumusak, sert_weight, idx["dogrulama"],
                            a.batch, augment=False, shuffle=False)
    test = make_dataset(X, y, yumusak, sert_weight, idx["test"],
                       a.batch, augment=False, shuffle=False)

    model = model_kur(n_classes, a.width)
    mac = mac_count(model)
    tepe, buyukler = aktivasyon_tepe(model)
    par = model.count_params()
    print(f"\nmodel: {par:,} parametre (~{par / 1024:.0f} KB int8)")
    print(f"MAC/pencere: {mac / 1e6:.1f} M   (butce {MAC_BUDGET / 1e6:.0f} M)")
    print(f"aktivasyon tepesi (kaba): {tepe / 1024:.0f} KB "
          f"(arena butcesi {ARENA_BUDGET / 1024:.0f} KB)")
    print("  en buyuk katman ciktilari: " +
          ", ".join(f"{n} {v / 1024:.0f}KB" for n, v in buyukler))
    if mac > MAC_BUDGET:
        sys.exit(f"!! MAC butcesi asildi ({mac / 1e6:.1f} M > "
                 f"{MAC_BUDGET / 1e6:.0f} M) — --genislik dusurun")

    loss_f = loss_kur(n_classes, alfa, a.gamma, a.kd)
    step_count = max(1, len(idx["egitim"]) // a.batch) * a.epochs
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
    path = os.path.join(a.out, "tur_agi.keras")
    pano = os.path.join(a.out, "ilerleme.html")
    bilgi = (f"{par:,} parametre · {mac / 1e6:.1f} MMAC · "
             f"{len(idx['egitim'])} egitim / {len(idx['dogrulama'])} dogrulama")
    history = []
    basladi = time.time()
    print(f"\nilerleme panosu: {pano}\n")
    for epochs in range(1, a.epochs + 1):
        t0 = time.time()
        total = count = 0.0
        for x, h, w in train:
            total += float(step(x, h, w))
            count += 1
        d1, d3, _, _ = degerlendir(model, val, n_classes)
        history.append((epochs, total / count, d1, d3))
        yildiz = ""
        if d1 > best:
            best = d1
            model.save(path)
            yildiz = "  <- kaydedildi"
        print(f"devir {epochs:3d}/{a.epochs}  kayip {total / count:.4f}  "
              f"dogrulama top-1 %{d1 * 100:.2f}  top-3 %{d3 * 100:.2f}  "
              f"{time.time() - t0:.0f} sn{yildiz}", flush=True)
        pano_write(pano, history, a.epochs, bilgi, time.time() - basladi,
                 bitti=(epochs == a.epochs))

    print(f"\nen iyi dogrulama top-1: %{best * 100:.2f}  ->  {path}")
    model = tf.keras.models.load_model(path)

    d1, d3, kar, n = degerlendir(model, test, n_classes)
    print(f"TEST (float32): top-1 %{d1 * 100:.2f}  top-3 %{d3 * 100:.2f}  "
          f"({n} pencere)")

    tflite_path, gs, gz = quantize(model, X, idx["egitim"], a.out)
    q1, q3, qkar = tflite_degerlendir(tflite_path, X, y, idx["test"], n_classes)
    print(f"TEST (int8)   : top-1 %{q1 * 100:.2f}  top-3 %{q3 * 100:.2f}  "
          f"(fark {(q1 - d1) * 100:+.2f} puan)")

    c_write(tflite_path, os.path.join(a.out, "tur_agi_int8.h"))
    rapor_write(a, model, classes, mac, tepe, history, d1, d3, q1, q3, qkar,
              gs, gz, tflite_path)
    return 0


# ══ INT8 ════════════════════════════════════════════════════════════════
def quantize(model, X, train_idx, out):
    """Egitim sonrasi INT8 niceleştirme, temsili veri kumesiyle."""
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

    path = os.path.join(out, "tur_agi_int8.tflite")
    with open(path, "wb") as f:
        f.write(tfl)

    note = tf.lite.Interpreter(model_path=path)
    note.allocate_tensors()
    g = note.get_input_details()[0]
    gs, gz = float(g["quantization"][0]), int(g["quantization"][1])
    print(f"\nINT8 model: {len(tfl) / 1024:.0f} KB  ->  {path}")
    print(f"girdi tensoru: {g['dtype'].__name__} {tuple(g['shape'])}  "
          f"olcek {gs:.6f}  sifir noktasi {gz}")
    if abs(gs - 1.0) > 0.02 or gz != 0:
        print("!! DIKKAT: girdi olcegi 1.0/0 DEGIL. Cihaz pb_mel_window()\n"
              "   ciktisini oldugu gibi veremez; M6'da donusum gerekir.\n"
              f"   q_tflite = round(q_mel * {SIGMA_SCALE:.6f} / {gs:.6f}) + {gz}")
    else:
        print("   -> cihaz pb_mel_window() ciktisini DOGRUDAN verebilir.")
    return path, gs, gz


def tflite_degerlendir(path, X, y, idx, n_classes):
    note = tf.lite.Interpreter(model_path=path, num_threads=8)
    note.allocate_tensors()
    g = note.get_input_details()[0]
    c = note.get_output_details()[0]
    d1 = d3 = 0
    kar = np.zeros((n_classes, n_classes), dtype=np.int32)
    for i in idx:
        note.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        note.invoke()
        o = note.get_tensor(c["index"])[0].astype(np.int32)
        ilk3 = np.argsort(-o)[:3]
        d1 += int(ilk3[0] == y[i])
        d3 += int(y[i] in ilk3)
        kar[y[i], ilk3[0]] += 1
    return d1 / len(idx), d3 / len(idx), kar


def c_write(tflite_path, h_path):
    raw = open(tflite_path, "rb").read()
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("/* Uretilmis dosya — tools/train_species.py. ELLE DUZENLEMEYIN. */\n")
        f.write("#ifndef POKEBIRD_SPECIES_NET_H\n#define POKEBIRD_SPECIES_NET_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define PB_TUR_AGI_BOYUT {len(raw)}\n\n")
        f.write("/* 16 bayt hizalama: TFLM model verisinin hizali olmasini "
                "istiyor. */\n")
        f.write("__attribute__((aligned(16)))\n")
        f.write("const unsigned char pb_species_net[] = {\n")
        for i in range(0, len(raw), 12):
            f.write("  " + " ".join(f"0x{b:02x}," for b in raw[i:i + 12]) + "\n")
        f.write("};\n\n#endif\n")
    print(f"C dizisi: {h_path}  ({len(raw) / 1024:.0f} KB)")


def pano_write(path, history, total_epochs, bilgi, gecen, bitti=False):
    """Her devirde kendini yenileyen basit HTML pano.

    Tarayicida acik birakin; 10 saniyede bir kendini yeniliyor. Bagimlilik
    yok, tek dosya, grafik satir ici SVG.
    """
    d = [g[0] for g in history]
    loss = [g[1] for g in history]
    t1 = [g[2] * 100 for g in history]
    t3 = [g[3] * 100 for g in history]
    last = history[-1] if history else (0, 0, 0, 0)
    percent = 100.0 * len(history) / max(total_epochs, 1)
    kalan = ("bitti" if bitti else
             f"~{(total_epochs - len(history)) * gecen / max(len(history), 1) / 60:.0f} dk")

    def cizgi(value, renk, max_az=None, max_cok=None):
        if not value:
            return ""
        lo = min(value) if max_az is None else max_az
        hi = max(value) if max_cok is None else max_cok
        if hi - lo < 1e-9:
            hi = lo + 1
        n = len(value)
        p = " ".join(
            f"{40 + 660 * (i / max(n - 1, 1)):.1f},"
            f"{180 - 160 * ((v - lo) / (hi - lo)):.1f}"
            for i, v in enumerate(value))
        return (f'<polyline fill="none" stroke="{renk}" stroke-width="2.5" '
                f'points="{p}"/>')

    best = max(t1) if t1 else 0
    rows = "".join(
        f"<tr><td>{g[0]}</td><td>{g[1]:.4f}</td><td>%{g[2]*100:.2f}</td>"
        f"<td>%{g[3]*100:.2f}</td></tr>" for g in reversed(history[-25:]))

    html = f"""<!doctype html><html lang="tr"><head><meta charset="utf-8">
<title>PokeBird — egitim</title>
{'' if bitti else '<meta http-equiv="refresh" content="10">'}
<style>
 body{{font:14px/1.5 system-ui,sans-serif;margin:0;padding:24px;
      background:#11151a;color:#dfe6ee}}
 h1{{font-size:19px;margin:0 0 4px}} .alt{{color:#8b98a6;font-size:13px}}
 .kutular{{display:flex;gap:12px;flex-wrap:wrap;margin:18px 0}}
 .k{{background:#1a2028;border:1px solid #262f3a;border-radius:10px;
     padding:12px 16px;min-width:120px}}
 .k b{{display:block;font-size:22px;font-weight:600;margin-top:2px}}
 .k span{{color:#8b98a6;font-size:12px;text-transform:uppercase;
          letter-spacing:.04em}}
 .cubuk{{height:8px;background:#232c36;border-radius:5px;overflow:hidden}}
 .cubuk div{{height:100%;background:linear-gradient(90deg,#3ba55d,#5ed17f)}}
 svg{{background:#1a2028;border:1px solid #262f3a;border-radius:10px}}
 table{{border-collapse:collapse;margin-top:16px;font-variant-numeric:tabular-nums}}
 th,td{{padding:4px 14px 4px 0;text-align:right;border-bottom:1px solid #232c36}}
 th{{color:#8b98a6;font-weight:500;text-align:right}}
 td:first-child,th:first-child{{text-align:left}}
 .lej i{{display:inline-block;width:11px;height:3px;vertical-align:middle;
         margin-right:5px}}
 .lej span{{margin-right:16px;color:#8b98a6;font-size:12px}}
</style></head><body>
<h1>PokeBird — Asama-2 tur agi egitimi</h1>
<div class="alt">{bilgi} · devir {len(history)}/{total_epochs} · kalan {kalan}
 {'' if bitti else '· sayfa 10 sn`de bir yenileniyor'}</div>
<div class="kutular">
 <div class="k"><span>dogrulama top-1</span><b>%{last[2]*100:.2f}</b></div>
 <div class="k"><span>dogrulama top-3</span><b>%{last[3]*100:.2f}</b></div>
 <div class="k"><span>en iyi top-1</span><b>%{best:.2f}</b></div>
 <div class="k"><span>kayip</span><b>{last[1]:.4f}</b></div>
 <div class="k"><span>gecen</span><b>{gecen/60:.0f} dk</b></div>
</div>
<div class="cubuk"><div style="width:{percent:.1f}%"></div></div>
<p class="lej"><span><i style="background:#5ed17f"></i>top-1</span>
<span><i style="background:#63a8ff"></i>top-3</span>
<span><i style="background:#e0803c"></i>kayip</span></p>
<svg viewBox="0 0 740 200" width="100%" height="200">
 <line x1="40" y1="180" x2="700" y2="180" stroke="#2e3945"/>
 <line x1="40" y1="20" x2="700" y2="20" stroke="#2e3945" stroke-dasharray="3 4"/>
 {cizgi(t3, '#63a8ff', 0, 100)}{cizgi(t1, '#5ed17f', 0, 100)}{cizgi(loss, '#e0803c')}
 <text x="6" y="184" fill="#8b98a6" font-size="11">0</text>
 <text x="6" y="24" fill="#8b98a6" font-size="11">100</text>
</svg>
<table><tr><th>devir</th><th>kayip</th><th>top-1</th><th>top-3</th></tr>
{rows}</table>
</body></html>"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)


def rapor_write(a, model, classes, mac, tepe, history, d1, d3, q1, q3, kar,
              gs, gz, tflite_path):
    name = {int(s["class_index"]): s["turkish_name"] for s in classes}
    n = kar.sum(axis=1)
    duyarlilik = np.divide(np.diag(kar), np.maximum(n, 1))

    s = []
    s.append(f"model     : {model.count_params():,} parametre")
    s.append(f"MAC/pencere: {mac / 1e6:.1f} M  (butce 30 M)")
    s.append(f"aktivasyon tepesi (kaba): {tepe / 1024:.0f} KB (arena butcesi 180 KB)")
    s.append(f"tflite    : {os.path.getsize(tflite_path) / 1024:.0f} KB")
    s.append(f"girdi     : int8, olcek {gs:.6f}, sifir noktasi {gz}")
    s.append("")
    s.append(f"TEST float32 : top-1 %{d1 * 100:.2f}  top-3 %{d3 * 100:.2f}")
    s.append(f"TEST int8    : top-1 %{q1 * 100:.2f}  top-3 %{q3 * 100:.2f}"
             f"   (nicelestirme bedeli {(q1 - d1) * 100:+.2f} puan)")
    s.append("")
    s.append("EN KOTU 20 SINIF (int8, test kumesi duyarliligi):")
    for i in np.argsort(duyarlilik):
        if n[i] == 0:
            continue
        if len([x for x in s if x.startswith("  ")]) >= 20:
            break
        karisan = np.argsort(-kar[i])
        k0 = karisan[0] if karisan[0] != i else (karisan[1] if len(karisan) > 1 else i)
        s.append(f"  {name.get(i, i):28s} %{duyarlilik[i] * 100:5.1f}  "
                 f"({int(n[i])} pencere)  en cok karistigi: {name.get(int(k0), k0)}")
    s.append("")
    s.append("devir  kayip   dogrulama top-1  top-3")
    for d, k, v1, v3 in history:
        s.append(f"{d:5d}  {k:.4f}  %{v1 * 100:12.2f}  %{v3 * 100:5.2f}")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(a.out, "rapor.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    sys.exit(main())
