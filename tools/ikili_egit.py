#!/usr/bin/env python3
"""
ikili_egit.py — Asama-1 ikili ag (kus var/yok): egitim + INT8 nicelestirme. (M7)

    .venv-birdnet\\Scripts\\python -u tools/ikili_egit.py --duman     # once bu
    .venv-birdnet\\Scripts\\python -u tools/ikili_egit.py

Girdi : data/egitim/  (tools/egitim_kumesi.py ciktisi — tur agiyla AYNI veri)
Cikti : models/ikili_agi.keras
        models/ikili_agi_int8.tflite
        models/ikili_agi_int8.h
        models/ikili_rapor.txt

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

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EGITIM = os.path.join(KOK, "data", "egitim")
MODELLER = os.path.join(KOK, "models")

FRAMES, BANDS = 187, 64
SIGMA_OLCEK = 4.0 / 127.0
NEGATIF_SINIF = 178
MAC_BUTCE = 5_000_000       # tur aginin 30M'inin cok altinda — sik calisacak
BOYUT_BUTCE = 15 * 1024     # ARCHITECTURE §4


def veri_yukle():
    X = np.load(os.path.join(EGITIM, "pencereler.npy"), mmap_mode="r")
    y_tur = np.load(os.path.join(EGITIM, "etiket.npy")).astype(np.int32)
    with open(os.path.join(EGITIM, "ornekler.csv"), encoding="utf-8") as f:
        satir = list(csv.DictReader(f))
    if not (len(X) == len(y_tur) == len(satir)):
        sys.exit(f"uzunluklar tutmuyor: X {len(X)} y {len(y_tur)} "
                 f"csv {len(satir)}")

    bolum = np.array([s["bolum"] for s in satir])
    y = (y_tur != NEGATIF_SINIF).astype(np.int32)   # 1=kus, 0=degil
    return X, y, bolum


def veri_kumesi(X, y, sirket_agirlik, idx, yigin, artir, karistir):
    def getir(i):
        i = np.sort(i)
        return (X[i].astype(np.float32), y[i].astype(np.float32),
                sirket_agirlik[i])

    ds = tf.data.Dataset.from_tensor_slices(idx)
    if karistir:
        ds = ds.shuffle(len(idx), reshuffle_each_iteration=True)
    ds = ds.batch(yigin, drop_remainder=False)
    ds = ds.map(
        lambda i: tf.numpy_function(getir, [i],
                                    [tf.float32, tf.float32, tf.float32]),
        num_parallel_calls=tf.data.AUTOTUNE)

    def bicim(x, e, w):
        x = tf.reshape(x, (-1, FRAMES, BANDS, 1))
        e.set_shape([None]); w.set_shape([None])
        if artir:
            x = artirma(x)
        return x, e, w

    return ds.map(bicim, num_parallel_calls=tf.data.AUTOTUNE).prefetch(
        tf.data.AUTOTUNE)


def artirma(x):
    """egit.py ile ayni: zaman kaydirma + SpecAugment, maske degeri 0."""
    b = tf.shape(x)[0]
    k = tf.random.uniform([], -16, 17, dtype=tf.int32)
    x = tf.roll(x, shift=k, axis=1)

    def maskele(x, eksen, en_fazla):
        boy = tf.shape(x)[eksen]
        genislik = tf.random.uniform([b, 1], 0, en_fazla, dtype=tf.int32)
        bas = tf.random.uniform([b, 1], 0, boy - en_fazla, dtype=tf.int32)
        r = tf.reshape(tf.range(boy), [1, -1])
        m = tf.cast((r < bas) | (r >= bas + genislik), x.dtype)
        sekil = [b, 1, 1, 1]
        sekil[eksen] = boy
        return x * tf.reshape(m, sekil)

    x = maskele(x, 1, 30)
    x = maskele(x, 2, 10)
    return x


def ds_blok(x, kanal, adim, ad):
    x = tf.keras.layers.DepthwiseConv2D(3, strides=adim, padding="same",
                                        use_bias=False, name=f"{ad}_dw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{ad}_dwbn")(x)
    x = tf.keras.layers.ReLU(6.0, name=f"{ad}_dwrelu")(x)
    x = tf.keras.layers.Conv2D(kanal, 1, use_bias=False, name=f"{ad}_pw")(x)
    x = tf.keras.layers.BatchNormalization(name=f"{ad}_pwbn")(x)
    return tf.keras.layers.ReLU(6.0, name=f"{ad}_pwrelu")(x)


def model_kur(genislik=1.0):
    """Kucuk derinlemesine ayrilabilir CNN — tur aginin 8 blogundan cok daha
    dar/kisa. Butce 15 KB int8; tur aginin ~270 KB'inin 1/18'i."""
    k = lambda n: max(4, int(n * genislik))
    g = tf.keras.Input(shape=(FRAMES, BANDS, 1), dtype="float32", name="mel_int8")
    x = tf.keras.layers.Rescaling(SIGMA_OLCEK, name="int8_sigma")(g)

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


def mac_say(model):
    toplam = 0
    for k in model.layers:
        c = getattr(k, "output", None)
        if c is None:
            continue
        s = c.shape
        if isinstance(k, tf.keras.layers.Conv2D):
            h, w, f = s[1], s[2], s[3]
            gk = k.kernel_size[0] * k.kernel_size[1]
            toplam += h * w * f * gk * k.input.shape[-1]
        elif isinstance(k, tf.keras.layers.DepthwiseConv2D):
            h, w, f = s[1], s[2], s[3]
            toplam += h * w * f * k.kernel_size[0] * k.kernel_size[1]
        elif isinstance(k, tf.keras.layers.Dense):
            toplam += k.input.shape[-1] * s[-1]
    return int(toplam)


def kayip_kur(pos_agirlik):
    """Agirlikli BCE. pos_agirlik: NEGATIF sinifina (0) verilen carpan —
    16,5:1 dengesizligi tersine cevirmek icin negatif ornek basina agirlik."""
    pos_agirlik = tf.constant(float(pos_agirlik), dtype=tf.float32)

    def kayip(y, logit, w):
        ce = tf.nn.sigmoid_cross_entropy_with_logits(labels=y, logits=logit[:, 0])
        agirlik = tf.where(y > 0.5, tf.ones_like(y), tf.fill(tf.shape(y), pos_agirlik))
        return tf.reduce_mean(ce * agirlik * w)
    return kayip


def degerlendir(model, ds, esik=0.5):
    dogru = toplam = 0
    tp = fp = tn = fn = 0
    for x, y, _ in ds:
        logit = model(x, training=False)
        p = tf.sigmoid(logit[:, 0]).numpy()
        e = y.numpy()
        tahmin = (p >= esik).astype(np.int32)
        dogru += int((tahmin == e).sum())
        toplam += len(e)
        tp += int(((tahmin == 1) & (e == 1)).sum())
        fp += int(((tahmin == 1) & (e == 0)).sum())
        tn += int(((tahmin == 0) & (e == 0)).sum())
        fn += int(((tahmin == 0) & (e == 1)).sum())
    return dogru / toplam, tp, fp, tn, fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duman", action="store_true")
    ap.add_argument("--devir", type=int, default=40)
    ap.add_argument("--yigin", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--genislik", type=float, default=1.0)
    ap.add_argument("--agirlik", type=float, default=0.0,
                    help="negatif sinif agirligi; 0 = olculen 16,5 kullan")
    ap.add_argument("--out", default=MODELLER)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    X, y, bolum = veri_yukle()
    print(f"{len(X)} pencere  kus {int(y.sum())}  degil {int((1 - y).sum())}")

    idx = {b: np.where(bolum == b)[0] for b in ("egitim", "dogrulama", "test")}
    if a.duman:
        rng = np.random.default_rng(0)
        for b in idx:
            idx[b] = rng.choice(idx[b], size=min(len(idx[b]), 3000), replace=False)
        a.devir = 2
    for b, v in idx.items():
        print(f"  {b:10s} {len(v):6d}  kus %{100*y[v].mean():.1f}")

    pos_agirlik = a.agirlik if a.agirlik > 0 else (
        (y[idx["egitim"]] == 1).sum() / max((y[idx["egitim"]] == 0).sum(), 1))
    print(f"negatif sinif agirligi: {pos_agirlik:.2f}")

    ornek_agirlik = np.ones(len(X), dtype=np.float32)

    egitim = veri_kumesi(X, y, ornek_agirlik, idx["egitim"], a.yigin,
                         artir=True, karistir=True)
    dogrulama = veri_kumesi(X, y, ornek_agirlik, idx["dogrulama"], a.yigin,
                            artir=False, karistir=False)
    test = veri_kumesi(X, y, ornek_agirlik, idx["test"], a.yigin,
                       artir=False, karistir=False)

    model = model_kur(a.genislik)
    mac = mac_say(model)
    par = model.count_params()
    print(f"\nmodel: {par:,} parametre (~{par / 1024:.1f} KB int8)")
    print(f"MAC/pencere: {mac / 1e6:.2f} M  (butce {MAC_BUTCE / 1e6:.0f} M)")
    if mac > MAC_BUTCE:
        sys.exit(f"!! MAC butcesi asildi ({mac/1e6:.1f}M > {MAC_BUTCE/1e6:.0f}M) "
                 "— --genislik dusurun")
    if par > BOYUT_BUTCE:
        print(f"!! DIKKAT: {par} parametre > {BOYUT_BUTCE} bayt butcesi "
              "(int8 boyut tflite'ta olculecek, kesin karar orada)")

    kayip_f = kayip_kur(pos_agirlik)
    adim_sayisi = max(1, len(idx["egitim"]) // a.yigin) * a.devir
    plan = tf.keras.optimizers.schedules.CosineDecay(
        a.lr, adim_sayisi, warmup_target=a.lr, warmup_steps=200)
    opt = tf.keras.optimizers.Adam(plan)

    @tf.function
    def adim(x, e, w):
        with tf.GradientTape() as t:
            logit = model(x, training=True)
            kayip = kayip_f(e, logit, w)
        opt.apply_gradients(zip(t.gradient(kayip, model.trainable_variables),
                                model.trainable_variables))
        return kayip

    en_iyi = -1.0
    yol = os.path.join(a.out, "ikili_agi.keras")
    gecmis = []
    basladi = time.time()
    for devir in range(1, a.devir + 1):
        t0 = time.time()
        toplam = adet = 0.0
        for x, e, w in egitim:
            toplam += float(adim(x, e, w))
            adet += 1
        acc, tp, fp, tn, fn = degerlendir(model, dogrulama)
        geri_cagirma = tp / max(tp + fn, 1)   # recall kus sinifi
        ozgulluk = tn / max(tn + fp, 1)       # negatifi doGru red
        gecmis.append((devir, toplam / adet, acc, geri_cagirma, ozgulluk))
        yildiz = ""
        skor = geri_cagirma  # kus kacirmamak asil oncelik
        if skor > en_iyi:
            en_iyi = skor
            model.save(yol)
            yildiz = "  <- kaydedildi"
        print(f"devir {devir:3d}/{a.devir}  kayip {toplam/adet:.4f}  "
              f"dogrulama acc %{acc*100:.2f}  kus-geri-cagirma %{geri_cagirma*100:.2f}  "
              f"negatif-ozgulluk %{ozgulluk*100:.2f}  {time.time()-t0:.0f}sn{yildiz}",
              flush=True)

    print(f"\nen iyi (dogrulama kus-geri-cagirma): %{en_iyi*100:.2f}  ->  {yol}")
    model = tf.keras.models.load_model(yol)

    acc, tp, fp, tn, fn = degerlendir(model, test)
    print(f"TEST (float32) esik 0.5: acc %{acc*100:.2f}  "
          f"kus-geri-cagirma %{100*tp/max(tp+fn,1):.2f}  "
          f"negatif-ozgulluk %{100*tn/max(tn+fp,1):.2f}  "
          f"(tp {tp} fp {fp} tn {tn} fn {fn})")

    tflite_yol, gs, gz = niceleştir(model, X, idx["egitim"], a.out)
    q_acc, qtp, qfp, qtn, qfn = tflite_degerlendir(tflite_yol, X, y, idx["test"])
    print(f"TEST (int8)    esik 0.5: acc %{q_acc*100:.2f}  "
          f"kus-geri-cagirma %{100*qtp/max(qtp+qfn,1):.2f}  "
          f"negatif-ozgulluk %{100*qtn/max(qtn+qfp,1):.2f}  "
          f"(fark {(q_acc-acc)*100:+.2f} puan)")

    c_yaz(tflite_yol, os.path.join(a.out, "ikili_agi_int8.h"))
    rapor_yaz(a, model, mac, gecmis, acc, tp, fp, tn, fn,
              q_acc, qtp, qfp, qtn, qfn, gs, gz, tflite_yol)
    return 0


def niceleştir(model, X, egitim_idx, out):
    rng = np.random.default_rng(0)
    ornek = np.sort(rng.choice(egitim_idx, size=min(500, len(egitim_idx)),
                               replace=False))

    def temsili():
        for i in ornek:
            yield [X[i].reshape(1, FRAMES, BANDS, 1).astype(np.float32)]

    d = tf.lite.TFLiteConverter.from_keras_model(model)
    d.optimizations = [tf.lite.Optimize.DEFAULT]
    d.representative_dataset = temsili
    d.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    d.inference_input_type = tf.int8
    d.inference_output_type = tf.int8
    tfl = d.convert()

    yol = os.path.join(out, "ikili_agi_int8.tflite")
    with open(yol, "wb") as f:
        f.write(tfl)

    yorum = tf.lite.Interpreter(model_path=yol)
    yorum.allocate_tensors()
    g = yorum.get_input_details()[0]
    c = yorum.get_output_details()[0]
    gs, gz = float(g["quantization"][0]), int(g["quantization"][1])
    print(f"\nINT8 model: {len(tfl)/1024:.1f} KB  (butce {BOYUT_BUTCE/1024:.0f} KB)  -> {yol}")
    print(f"girdi tensoru: {g['dtype'].__name__} {tuple(g['shape'])}  "
          f"olcek {gs:.6f}  sifir noktasi {gz}")
    if abs(gs - 1.0) > 0.02 or gz != 0:
        print("!! DIKKAT: girdi olcegi 1.0/0 DEGIL. Cihaz pb_mel_window()\n"
              "   ciktisini oldugu gibi veremez; donusum gerekir.\n"
              f"   q_tflite = round(q_mel * {SIGMA_OLCEK:.6f} / {gs:.6f}) + {gz}")
    else:
        print("   -> cihaz pb_mel_window() ciktisini DOGRUDAN verebilir.")
    cs, cz = float(c["quantization"][0]), int(c["quantization"][1])
    print(f"cikti tensoru: {c['dtype'].__name__}  olcek {cs:.6f}  sifir noktasi {cz}  "
          "(sigmoid oncesi ham logit)")
    if len(tfl) > BOYUT_BUTCE:
        print(f"!! DIKKAT: {len(tfl)/1024:.1f} KB > {BOYUT_BUTCE/1024:.0f} KB butcesi")
    return yol, gs, gz


def tflite_degerlendir(yol, X, y, idx, esik=0.5):
    yorum = tf.lite.Interpreter(model_path=yol, num_threads=8)
    yorum.allocate_tensors()
    g = yorum.get_input_details()[0]
    c = yorum.get_output_details()[0]
    cs, cz = c["quantization"]
    tp = fp = tn = fn = 0
    for i in idx:
        yorum.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        yorum.invoke()
        q = int(yorum.get_tensor(c["index"])[0][0])
        logit = (q - cz) * cs
        p = 1.0 / (1.0 + np.exp(-logit))
        tahmin = int(p >= esik)
        e = int(y[i])
        tp += int(tahmin == 1 and e == 1)
        fp += int(tahmin == 1 and e == 0)
        tn += int(tahmin == 0 and e == 0)
        fn += int(tahmin == 0 and e == 1)
    n = tp + fp + tn + fn
    return (tp + tn) / max(n, 1), tp, fp, tn, fn


def c_yaz(tflite_yol, h_yol):
    ham = open(tflite_yol, "rb").read()
    with open(h_yol, "w", encoding="utf-8") as f:
        f.write("/* Uretilmis dosya — tools/ikili_egit.py. ELLE DUZENLEMEYIN. */\n")
        f.write("#ifndef POKEBIRD_IKILI_AGI_H\n#define POKEBIRD_IKILI_AGI_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define PB_IKILI_AGI_BOYUT {len(ham)}\n\n")
        f.write("__attribute__((aligned(16)))\n")
        f.write("const unsigned char pb_ikili_agi[] = {\n")
        for i in range(0, len(ham), 12):
            f.write("  " + " ".join(f"0x{b:02x}," for b in ham[i:i + 12]) + "\n")
        f.write("};\n\n#endif\n")
    print(f"C dizisi: {h_yol}  ({len(ham)/1024:.1f} KB)")


def rapor_yaz(a, model, mac, gecmis, acc, tp, fp, tn, fn,
              q_acc, qtp, qfp, qtn, qfn, gs, gz, tflite_yol):
    s = []
    s.append(f"model      : {model.count_params():,} parametre")
    s.append(f"MAC/pencere: {mac/1e6:.2f} M  (butce {MAC_BUTCE/1e6:.0f} M)")
    s.append(f"tflite     : {os.path.getsize(tflite_yol)/1024:.1f} KB  (butce {BOYUT_BUTCE/1024:.0f} KB)")
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
    for d, k, ac, gc, oz in gecmis:
        s.append(f"{d:5d}  {k:.4f}  %{ac*100:6.2f}  %{gc*100:16.2f}  %{oz*100:16.2f}")

    metin = "\n".join(s)
    print("\n" + metin)
    with open(os.path.join(a.out, "ikili_rapor.txt"), "w", encoding="utf-8") as f:
        f.write(metin + "\n")


if __name__ == "__main__":
    sys.exit(main())
