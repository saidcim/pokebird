#!/usr/bin/env python3
"""
ikili_dogrulama_seti.py — Asama-1 ikili ag CIHAZ-ICI DOGRULAMA SETI (M7)

tools/validation_set.py'nin (M6) ikili ag karsiligi — ayni gerekce: mikrofon/
mel hattini hic karistirmadan, ayni pencereyi PC ve cihazda ayni modelden
gecirip int8 logit'leri BIREBIR karsilastirmak. "PC'de calisiyor cihazda
calismiyor" hata sinifini yakalayan tek yontem bu.

Cikti: src/ai/binary_validation_set.h

Secim: yarisi kus (farkli turlerden), yarisi negatif (ESC-50) — ikili agin
HER IKI ucta da dogru calistigini gormek icin. dogrulama_seti.py'deki
BUILTIN_REF uyarisi ayni sekilde geçerli: varsayilan XNNPACK int8'i
bit-birebir hesaplamiyor.

Kullanim:
    .venv-birdnet\\Scripts\\python tools/binary_validation_set.py
"""
from __future__ import annotations

import csv
import pathlib
import sys

import numpy as np

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRAIN = ROOT / "data" / "egitim"
MODEL = csv_compat.resolve(ROOT / "models" / "binary_net_int8.tflite")
OUTPUT = ROOT / "src" / "ai" / "ikili_dogrulama_seti.h"

KARE, BAND = 187, 64
NEGATIVE_CLS = 178
COUNT_HER_TARAF = 4   # 4 kus + 4 negatif = 8 pencere


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

    selection_negative = list(rng.choice(negative, size=COUNT_HER_TARAF, replace=False))

    # Kus tarafinda cesitlilik: farkli turlerden.
    bird_karisik = rng.permutation(bird)
    seen: set[int] = set()
    selection_bird = []
    for i in bird_karisik:
        s = int(label[i])
        if s in seen:
            continue
        seen.add(s)
        selection_bird.append(int(i))
        if len(selection_bird) == COUNT_HER_TARAF:
            break

    selection = sorted(int(i) for i in selection_negative) + sorted(selection_bird)
    return selection


def main() -> None:
    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow yok. .venv-birdnet\\Scripts\\python ile calistirin.")

    if not MODEL.exists():
        sys.exit(f"{MODEL} yok — once tools/train_binary.py calistirin.")

    selection = sample_pick(20260803)
    windows = np.load(csv_compat.resolve(TRAIN / "windows.npy"), mmap_mode="r")
    label = np.load(csv_compat.resolve(TRAIN / "labels.npy"))

    # BUILTIN_REF — dogrulama_seti.py'deki uyarinin aynisi: XNNPACK int8'i
    # bit-birebir hesaplamiyor (olculdu, M6 §9l), altin standart REF cekirdek.
    interp = tf.lite.Interpreter(
        model_path=str(MODEL),
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    gd = interp.get_input_details()[0]
    cd = interp.get_output_details()[0]

    if gd["dtype"] != np.int8 or gd["quantization"] != (1.0, 0):
        sys.exit(f"girdi sozlesmesi bozuk: {gd['dtype']} {gd['quantization']}")
    if tuple(gd["shape"]) != (1, KARE, BAND, 1):
        sys.exit(f"girdi sekli {gd['shape']}, beklenen (1,{KARE},{BAND},1)")

    output_scale, output_sifir = cd["quantization"]

    girdiler = np.empty((len(selection), KARE, BAND), dtype=np.int8)
    logitler = np.empty(len(selection), dtype=np.int8)
    truth_ikili = np.empty(len(selection), dtype=np.int32)
    for k, i in enumerate(selection):
        p = np.asarray(windows[i], dtype=np.int8)
        girdiler[k] = p
        interp.set_tensor(gd["index"], p.reshape(1, KARE, BAND, 1))
        interp.invoke()
        q = int(interp.get_tensor(cd["index"])[0][0])
        logitler[k] = q
        truth_ikili[k] = 0 if int(label[i]) == NEGATIVE_CLS else 1

    print(f"{len(selection)} pencere secildi (test bolumu, {COUNT_HER_TARAF} kus + "
          f"{COUNT_HER_TARAF} negatif)")
    for k, i in enumerate(selection):
        p = 1.0 / (1.0 + np.exp(-(float(logitler[k]) - output_sifir) * output_scale))
        print(f"  satir {i:6d}  gercek {'KUS' if truth_ikili[k] else 'DEGIL':5s}"
              f"  logit(int8) {int(logitler[k]):4d}  p={p:.4f}")

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
        f.write(f"""/* Uretilmis dosya — tools/binary_validation_set.py. ELLE DUZENLEMEYIN.
 *
 * ASAMA-1 IKILI AG CIHAZ-ICI DOGRULAMA SETI (M7).
 *
 * {len(selection)} pencere data/egitim/pencereler.npy'nin TEST bolumunden secildi
 * ({COUNT_HER_TARAF} kus + {COUNT_HER_TARAF} negatif); beklenen logit PC'deki
 * TFLite REFERANS cekirdek (BUILTIN_REF) ciktisi. Cihaz BIREBIR ayni
 * uretmeli — varsayilan XNNPACK delegesi int8'i bit-birebir hesaplamiyor
 * (M6 §9l'de olculdu), o yuzden BUILTIN_REF kullanildi.
 *
 * Cikti nicelestirmesi: logit = (q - {int(output_sifir)}) * {float(output_scale):.9f}
 * (sigmoid ONCESI ham deger; p = sigmoid(logit))
 */
#ifndef POKEBIRD_BINARY_VALIDATION_SET_H
#define POKEBIRD_BINARY_VALIDATION_SET_H

#include <stdint.h>

#define PB_BINARY_VALIDATION_COUNT  {len(selection)}
#define PB_BINARY_VALIDATION_FRAMES  {KARE}
#define PB_BINARY_VALIDATION_BANDS  {BAND}

/* Gercek ikili etiket: 1 = KUS, 0 = DEGIL. */
static const int16_t pb_binary_validation_truth[PB_BINARY_VALIDATION_COUNT] = {{
{dizi(truth_ikili)}
}};

/* PC REFERANS cekirdegin urettigi ham int8 logit (sigmoid oncesi). */
static const int8_t pb_binary_validation_logit[PB_BINARY_VALIDATION_COUNT] = {{
{dizi(logitler)}
}};

/* Girdi pencereleri: kare disar (eskiden yeniye), bant icerde. */
static const int8_t pb_binary_validation_input[PB_BINARY_VALIDATION_COUNT]
                                            [PB_BINARY_VALIDATION_FRAMES * PB_BINARY_VALIDATION_BANDS] = {{
""")
        for k in range(len(selection)):
            f.write("  {\n" + dizi(girdiler[k].reshape(-1)) + "\n  },\n")
        f.write("};\n\n#endif\n")

    print(f"\nyazildi: {OUTPUT}")


if __name__ == "__main__":
    main()
