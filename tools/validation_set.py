#!/usr/bin/env python3
"""
dogrulama_seti.py — CİHAZ-İÇİ DOĞRULAMA SETİ üretici (M6, §9l madde 7)

Neden var
---------
Bu projedeki en pahalı hata sınıfı "PC'de çalışıyor cihazda çalışmıyor" ve
onu yakalayan tek şey aynı girdiyi iki tarafta da çalıştırıp ÇIKTILARI
karşılaştırmak. Ses yolu bilerek işin dışında: mikrofon, mel, kapı hiç
karışmıyor ki hata alanı dar kalsın. Girdi doğrudan eğitim kümesinden
alınmış hazır bir pencere.

Ne üretir
---------
    src/ai/validation_set.h    N pencere (int8) + PC'nin ürettiği int8 logit'ler

Cihazdaki `x` komutu aynı pencereleri modelden geçirip logit'leri bu tabloyla
karşılaştırıyor. Beklenen: BİREBİR aynı. Aynı değilse sorun mel'de değil,
TFLM/CMSIS-NN/niceleştirme tarafındadır.

Kullanım
--------
    .venv-birdnet\\Scripts\\python tools/validation_set.py
    .venv-birdnet\\Scripts\\python tools/validation_set.py --adet 8

TensorFlow gerektiriyor, yani `.venv-birdnet` (Python 3.11).
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import sys

import numpy as np

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRAIN = ROOT / "data" / "egitim"
MODEL = csv_compat.resolve(ROOT / "models" / "species_net_int8.tflite")
OUTPUT = ROOT / "src" / "ai" / "dogrulama_seti.h"

KARE = 187
BAND = 64


def sample_pick(count: int, seed: int) -> list[int]:
    """Test bölümünden `adet` satır seç.

    Seçim TEST bölümünden: eğitimde görülmemiş pencereler. Doğrulama
    açısından şart değil (aynı girdi → aynı çıktı, bölümden bağımsız) ama
    aynı satırları ileride doğruluk ölçmek için de kullanabilelim diye.

    Negatif sınıf (178) MUTLAKA içeride: modelin son sınıfı ve o sınıfa giden
    yol (global ortalama → tam bağlı) diğerlerinden farklı bir aktivasyon
    aralığı görüyor.
    """
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))
    split = []
    with open(csv_compat.resolve(TRAIN / "samples.csv"), encoding="utf-8") as f:
        for row in csv_compat.reader(f):
            split.append(row["split"])
    split = np.array(split)
    if len(split) != len(label):
        sys.exit(f"ornekler.csv {len(split)} satir, etiket.npy {len(label)} — hizasiz")

    test = np.flatnonzero(split == "test")
    rng = np.random.default_rng(seed)

    negative = test[label[test] == 178]
    bird = test[label[test] != 178]
    if len(negative) == 0:
        sys.exit("test bolumunde negatif ornek yok")

    selection = [int(rng.choice(negative))]
    # Kalanı farklı sınıflardan: aynı sınıfın iki penceresi aynı kod yolunu
    # sınıyor, çeşitlilik daha çok kanal/ölçek kombinasyonuna dokunuyor.
    bird_karisik = rng.permutation(bird)
    seen: set[int] = set()
    for i in bird_karisik:
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
                    help="pencere sayisi (varsayilan 8; her biri 11.968 bayt FLASH)")
    ap.add_argument("--seed", type=int, default=20260802)
    args = ap.parse_args()

    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow yok. .venv-birdnet\\Scripts\\python ile calistirin.")

    if not MODEL.exists():
        sys.exit(f"{MODEL} yok — once tools/train_species.py calistirin.")

    selection = sample_pick(args.count, args.seed)
    windows = np.load(csv_compat.resolve(TRAIN / "windows.npy"), mmap_mode="r")
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))

    name = {}
    with open(csv_compat.resolve(TRAIN / "classes.csv"), encoding="utf-8") as f:
        for s in csv_compat.reader(f):
            name[int(s["class_index"])] = (s["ebird_code"], s["turkish_name"])

    # ⚠ BUILTIN_REF — varsayılanı KULLANMAYIN.
    #
    # tf.lite.Interpreter varsayılanda XNNPACK delegesini devreye sokuyor.
    # XNNPACK int8'i bit-birebir hesaplamıyor: ölçüldü, aynı 8 pencerede
    # BUILTIN_REF'e göre en büyük 2 int8 adımı, ortalama mutlak 0,4441 fark
    # veriyor. İlk koşuda cihaz-PC farkı da tam olarak bu çıkmıştı (max 2,
    # ort 0,4441) — yani "cihazda sapma var" sanılan şeyin tamamı PC
    # tarafındaki delegeydi.
    #
    # TFLite'ın int8 tanımını veren şey referans çekirdekler; CMSIS-NN de
    # onlarla bit-birebir olmayı hedefliyor. Doğru altın standart bu.
    interp = tf.lite.Interpreter(
        model_path=str(MODEL),
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    gd = interp.get_input_details()[0]
    cd = interp.get_output_details()[0]

    # Cihaz sözleşmesi (§9k): ölçek 1.0 / sıfır 0. Firmware bunu memcpy ile
    # bağlıyor; kayarsa doğrulama seti de anlamsız olur.
    if gd["dtype"] != np.int8 or gd["quantization"] != (1.0, 0):
        sys.exit(f"girdi sozlesmesi bozuk: {gd['dtype']} {gd['quantization']}")
    if tuple(gd["shape"]) != (1, KARE, BAND, 1):
        sys.exit(f"girdi sekli {gd['shape']}, beklenen (1,{KARE},{BAND},1)")

    output_scale, output_sifir = cd["quantization"]
    cls_count = int(cd["shape"][-1])

    girdiler = np.empty((len(selection), KARE, BAND), dtype=np.int8)
    logitler = np.empty((len(selection), cls_count), dtype=np.int8)
    pred = []
    for k, i in enumerate(selection):
        p = np.asarray(windows[i], dtype=np.int8)
        girdiler[k] = p
        interp.set_tensor(gd["index"], p.reshape(1, KARE, BAND, 1))
        interp.invoke()
        q = interp.get_tensor(cd["index"])[0].astype(np.int8)
        logitler[k] = q
        pred.append(int(np.argmax(q.astype(np.int32))))

    dogru = sum(1 for k, i in enumerate(selection) if pred[k] == int(label[i]))
    print(f"{len(selection)} pencere secildi (test bolumu)")
    print(f"PC tarafi top-1: {dogru}/{len(selection)} "
          f"(dusuk olmasi NORMAL — pencere basina dogruluk %58)")
    for k, i in enumerate(selection):
        g, t = int(label[i]), pred[k]
        print(f"  satir {i:6d}  gercek {g:3d} {name.get(g, ('?', '?'))[1]:<24s}"
              f"  tahmin {t:3d} {name.get(t, ('?', '?'))[1]}")

    def dizi(v: np.ndarray) -> str:
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
        f.write(f"""/* Uretilmis dosya — tools/validation_set.py. ELLE DUZENLEMEYIN.
 *
 * CIHAZ-ICI DOGRULAMA SETI (M6 §9l madde 7).
 *
 * {len(selection)} pencere data/egitim/pencereler.npy'nin TEST bolumunden secildi;
 * beklenen logit'ler PC'deki TFLite yorumlayicisinin ayni pencereye verdigi
 * int8 ciktisi. Cihaz ayni girdiye BIREBIR ayni cikti vermeli.
 *
 * PC tarafi REFERANS cekirdeklerle (BUILTIN_REF) calistirildi. Varsayilan
 * yorumlayici XNNPACK delegesini kullaniyor ve int8'i bit-birebir
 * hesaplamiyor (olculdu: en buyuk 2 adim sapma) — altin standart o degil.
 *
 * Fark cikarsa sorun mel hattinda DEGIL (ses yolu bu teste hic girmiyor):
 * TFLM cekirdekleri, CMSIS-NN, arena ya da nicelestirme tarafindadir.
 *
 * Cikti nicelestirmesi: logit = (q - {int(output_sifir)}) * {float(output_scale):.9f}
 */
#ifndef POKEBIRD_VALIDATION_SET_H
#define POKEBIRD_VALIDATION_SET_H

#include <stdint.h>

#define PB_VALIDATION_COUNT   {len(selection)}
#define PB_VALIDATION_FRAMES   {KARE}
#define PB_VALIDATION_BANDS   {BAND}
#define PB_VALIDATION_CLASSES  {cls_count}

/* Gercek sinif indeksi (dogruluk icin degil, raporu okunur kilmak icin). */
static const int16_t pb_validation_class[PB_VALIDATION_COUNT] = {{
{dizi(np.array([label[i] for i in selection]))}
}};

/* PC'nin ayni pencereye verdigi tahmin (argmax). */
static const int16_t pb_validation_pc_pred[PB_VALIDATION_COUNT] = {{
{dizi(np.array(pred))}
}};

/* Girdi pencereleri: kare disar (eskiden yeniye), bant icerde —
 * mel.c'deki pb_mel_window() duzeninin aynisi. */
static const int8_t pb_validation_input[PB_VALIDATION_COUNT]
                                      [PB_VALIDATION_FRAMES * PB_VALIDATION_BANDS] = {{
""")
        for k in range(len(selection)):
            f.write("  {\n" + dizi(girdiler[k].reshape(-1)) + "\n  },\n")
        f.write("};\n\n/* PC'nin ham int8 logit'leri. */\nstatic const int8_t "
                "pb_validation_logit[PB_VALIDATION_COUNT][PB_VALIDATION_CLASSES] = {\n")
        for k in range(len(selection)):
            f.write("  {\n" + dizi(logitler[k]) + "\n  },\n")
        f.write("};\n\n#endif /* POKEBIRD_VALIDATION_SET_H */\n")

    size = OUTPUT.stat().st_size
    print(f"\n{OUTPUT.relative_to(ROOT)} yazildi ({size/1024:.0f} KB kaynak, "
          f"{len(selection)*KARE*BAND/1024:.0f} KB flash)")


if __name__ == "__main__":
    main()
