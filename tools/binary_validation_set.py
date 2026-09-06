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

KOK = pathlib.Path(__file__).resolve().parent.parent
EGITIM = KOK / "data" / "egitim"
MODEL = KOK / "models" / "ikili_agi_int8.tflite"
CIKTI = KOK / "src" / "ai" / "ikili_dogrulama_seti.h"

KARE, BANT = 187, 64
NEGATIF_SINIF = 178
ADET_HER_TARAF = 4   # 4 kus + 4 negatif = 8 pencere


def ornek_sec(tohum: int) -> list[int]:
    etiket = np.load(EGITIM / "etiket.npy")
    bolum = []
    with open(EGITIM / "ornekler.csv", encoding="utf-8") as f:
        for satir in csv.DictReader(f):
            bolum.append(satir["bolum"])
    bolum = np.array(bolum)

    test = np.flatnonzero(bolum == "test")
    rng = np.random.default_rng(tohum)

    negatif = test[etiket[test] == NEGATIF_SINIF]
    kus = test[etiket[test] != NEGATIF_SINIF]

    secim_negatif = list(rng.choice(negatif, size=ADET_HER_TARAF, replace=False))

    # Kus tarafinda cesitlilik: farkli turlerden.
    kus_karisik = rng.permutation(kus)
    gorulen: set[int] = set()
    secim_kus = []
    for i in kus_karisik:
        s = int(etiket[i])
        if s in gorulen:
            continue
        gorulen.add(s)
        secim_kus.append(int(i))
        if len(secim_kus) == ADET_HER_TARAF:
            break

    secim = sorted(int(i) for i in secim_negatif) + sorted(secim_kus)
    return secim


def main() -> None:
    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow yok. .venv-birdnet\\Scripts\\python ile calistirin.")

    if not MODEL.exists():
        sys.exit(f"{MODEL} yok — once tools/train_binary.py calistirin.")

    secim = ornek_sec(20260803)
    pencereler = np.load(EGITIM / "pencereler.npy", mmap_mode="r")
    etiket = np.load(EGITIM / "etiket.npy")

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
    if tuple(gd["shape"]) != (1, KARE, BANT, 1):
        sys.exit(f"girdi sekli {gd['shape']}, beklenen (1,{KARE},{BANT},1)")

    cikti_olcek, cikti_sifir = cd["quantization"]

    girdiler = np.empty((len(secim), KARE, BANT), dtype=np.int8)
    logitler = np.empty(len(secim), dtype=np.int8)
    gercek_ikili = np.empty(len(secim), dtype=np.int32)
    for k, i in enumerate(secim):
        p = np.asarray(pencereler[i], dtype=np.int8)
        girdiler[k] = p
        interp.set_tensor(gd["index"], p.reshape(1, KARE, BANT, 1))
        interp.invoke()
        q = int(interp.get_tensor(cd["index"])[0][0])
        logitler[k] = q
        gercek_ikili[k] = 0 if int(etiket[i]) == NEGATIF_SINIF else 1

    print(f"{len(secim)} pencere secildi (test bolumu, {ADET_HER_TARAF} kus + "
          f"{ADET_HER_TARAF} negatif)")
    for k, i in enumerate(secim):
        p = 1.0 / (1.0 + np.exp(-(float(logitler[k]) - cikti_sifir) * cikti_olcek))
        print(f"  satir {i:6d}  gercek {'KUS' if gercek_ikili[k] else 'DEGIL':5s}"
              f"  logit(int8) {int(logitler[k]):4d}  p={p:.4f}")

    def dizi(v: np.ndarray) -> str:
        s, satir = [], []
        for x in v:
            satir.append(f"{int(x):4d}")
            if len(satir) == 16:
                s.append("    " + ",".join(satir) + ",")
                satir = []
        if satir:
            s.append("    " + ",".join(satir) + ",")
        return "\n".join(s)

    with open(CIKTI, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"""/* Uretilmis dosya — tools/binary_validation_set.py. ELLE DUZENLEMEYIN.
 *
 * ASAMA-1 IKILI AG CIHAZ-ICI DOGRULAMA SETI (M7).
 *
 * {len(secim)} pencere data/egitim/pencereler.npy'nin TEST bolumunden secildi
 * ({ADET_HER_TARAF} kus + {ADET_HER_TARAF} negatif); beklenen logit PC'deki
 * TFLite REFERANS cekirdek (BUILTIN_REF) ciktisi. Cihaz BIREBIR ayni
 * uretmeli — varsayilan XNNPACK delegesi int8'i bit-birebir hesaplamiyor
 * (M6 §9l'de olculdu), o yuzden BUILTIN_REF kullanildi.
 *
 * Cikti nicelestirmesi: logit = (q - {int(cikti_sifir)}) * {float(cikti_olcek):.9f}
 * (sigmoid ONCESI ham deger; p = sigmoid(logit))
 */
#ifndef POKEBIRD_BINARY_VALIDATION_SET_H
#define POKEBIRD_BINARY_VALIDATION_SET_H

#include <stdint.h>

#define PB_BINARY_VALIDATION_COUNT  {len(secim)}
#define PB_BINARY_VALIDATION_FRAMES  {KARE}
#define PB_BINARY_VALIDATION_BANDS  {BANT}

/* Gercek ikili etiket: 1 = KUS, 0 = DEGIL. */
static const int16_t pb_binary_validation_truth[PB_BINARY_VALIDATION_COUNT] = {{
{dizi(gercek_ikili)}
}};

/* PC REFERANS cekirdegin urettigi ham int8 logit (sigmoid oncesi). */
static const int8_t pb_binary_validation_logit[PB_BINARY_VALIDATION_COUNT] = {{
{dizi(logitler)}
}};

/* Girdi pencereleri: kare disar (eskiden yeniye), bant icerde. */
static const int8_t pb_binary_validation_input[PB_BINARY_VALIDATION_COUNT]
                                            [PB_BINARY_VALIDATION_FRAMES * PB_BINARY_VALIDATION_BANDS] = {{
""")
        for k in range(len(secim)):
            f.write("  {\n" + dizi(girdiler[k].reshape(-1)) + "\n  },\n")
        f.write("};\n\n#endif\n")

    print(f"\nyazildi: {CIKTI}")


if __name__ == "__main__":
    main()
