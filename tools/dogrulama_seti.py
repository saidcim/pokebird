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
    src/ai/dogrulama_seti.h    N pencere (int8) + PC'nin ürettiği int8 logit'ler

Cihazdaki `x` komutu aynı pencereleri modelden geçirip logit'leri bu tabloyla
karşılaştırıyor. Beklenen: BİREBİR aynı. Aynı değilse sorun mel'de değil,
TFLM/CMSIS-NN/niceleştirme tarafındadır.

Kullanım
--------
    .venv-birdnet\\Scripts\\python tools/dogrulama_seti.py
    .venv-birdnet\\Scripts\\python tools/dogrulama_seti.py --adet 8

TensorFlow gerektiriyor, yani `.venv-birdnet` (Python 3.11).
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import sys

import numpy as np

KOK = pathlib.Path(__file__).resolve().parent.parent
EGITIM = KOK / "data" / "egitim"
MODEL = KOK / "models" / "tur_agi_int8.tflite"
CIKTI = KOK / "src" / "ai" / "dogrulama_seti.h"

KARE = 187
BANT = 64


def ornek_sec(adet: int, tohum: int) -> list[int]:
    """Test bölümünden `adet` satır seç.

    Seçim TEST bölümünden: eğitimde görülmemiş pencereler. Doğrulama
    açısından şart değil (aynı girdi → aynı çıktı, bölümden bağımsız) ama
    aynı satırları ileride doğruluk ölçmek için de kullanabilelim diye.

    Negatif sınıf (178) MUTLAKA içeride: modelin son sınıfı ve o sınıfa giden
    yol (global ortalama → tam bağlı) diğerlerinden farklı bir aktivasyon
    aralığı görüyor.
    """
    etiket = np.load(EGITIM / "etiket.npy")
    bolum = []
    with open(EGITIM / "ornekler.csv", encoding="utf-8") as f:
        for satir in csv.DictReader(f):
            bolum.append(satir["bolum"])
    bolum = np.array(bolum)
    if len(bolum) != len(etiket):
        sys.exit(f"ornekler.csv {len(bolum)} satir, etiket.npy {len(etiket)} — hizasiz")

    test = np.flatnonzero(bolum == "test")
    rng = np.random.default_rng(tohum)

    negatif = test[etiket[test] == 178]
    kus = test[etiket[test] != 178]
    if len(negatif) == 0:
        sys.exit("test bolumunde negatif ornek yok")

    secim = [int(rng.choice(negatif))]
    # Kalanı farklı sınıflardan: aynı sınıfın iki penceresi aynı kod yolunu
    # sınıyor, çeşitlilik daha çok kanal/ölçek kombinasyonuna dokunuyor.
    kus_karisik = rng.permutation(kus)
    gorulen: set[int] = set()
    for i in kus_karisik:
        s = int(etiket[i])
        if s in gorulen:
            continue
        gorulen.add(s)
        secim.append(int(i))
        if len(secim) == adet:
            break
    secim.sort()
    return secim


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--adet", type=int, default=8,
                    help="pencere sayisi (varsayilan 8; her biri 11.968 bayt FLASH)")
    ap.add_argument("--tohum", type=int, default=20260802)
    args = ap.parse_args()

    try:
        import tensorflow as tf
    except ImportError:
        sys.exit("TensorFlow yok. .venv-birdnet\\Scripts\\python ile calistirin.")

    if not MODEL.exists():
        sys.exit(f"{MODEL} yok — once tools/egit.py calistirin.")

    secim = ornek_sec(args.adet, args.tohum)
    pencereler = np.load(EGITIM / "pencereler.npy", mmap_mode="r")
    etiket = np.load(EGITIM / "etiket.npy")

    ad = {}
    with open(EGITIM / "siniflar.csv", encoding="utf-8") as f:
        for s in csv.DictReader(f):
            ad[int(s["sinif"])] = (s["ebird_kodu"], s["turkce_ad"])

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
    if tuple(gd["shape"]) != (1, KARE, BANT, 1):
        sys.exit(f"girdi sekli {gd['shape']}, beklenen (1,{KARE},{BANT},1)")

    cikti_olcek, cikti_sifir = cd["quantization"]
    sinif_sayisi = int(cd["shape"][-1])

    girdiler = np.empty((len(secim), KARE, BANT), dtype=np.int8)
    logitler = np.empty((len(secim), sinif_sayisi), dtype=np.int8)
    tahmin = []
    for k, i in enumerate(secim):
        p = np.asarray(pencereler[i], dtype=np.int8)
        girdiler[k] = p
        interp.set_tensor(gd["index"], p.reshape(1, KARE, BANT, 1))
        interp.invoke()
        q = interp.get_tensor(cd["index"])[0].astype(np.int8)
        logitler[k] = q
        tahmin.append(int(np.argmax(q.astype(np.int32))))

    dogru = sum(1 for k, i in enumerate(secim) if tahmin[k] == int(etiket[i]))
    print(f"{len(secim)} pencere secildi (test bolumu)")
    print(f"PC tarafi top-1: {dogru}/{len(secim)} "
          f"(dusuk olmasi NORMAL — pencere basina dogruluk %58)")
    for k, i in enumerate(secim):
        g, t = int(etiket[i]), tahmin[k]
        print(f"  satir {i:6d}  gercek {g:3d} {ad.get(g, ('?', '?'))[1]:<24s}"
              f"  tahmin {t:3d} {ad.get(t, ('?', '?'))[1]}")

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
        f.write(f"""/* Uretilmis dosya — tools/dogrulama_seti.py. ELLE DUZENLEMEYIN.
 *
 * CIHAZ-ICI DOGRULAMA SETI (M6 §9l madde 7).
 *
 * {len(secim)} pencere data/egitim/pencereler.npy'nin TEST bolumunden secildi;
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
 * Cikti nicelestirmesi: logit = (q - {int(cikti_sifir)}) * {float(cikti_olcek):.9f}
 */
#ifndef POKEBIRD_DOGRULAMA_SETI_H
#define POKEBIRD_DOGRULAMA_SETI_H

#include <stdint.h>

#define PB_DOGRULAMA_ADET   {len(secim)}
#define PB_DOGRULAMA_KARE   {KARE}
#define PB_DOGRULAMA_BANT   {BANT}
#define PB_DOGRULAMA_SINIF  {sinif_sayisi}

/* Gercek sinif indeksi (dogruluk icin degil, raporu okunur kilmak icin). */
static const int16_t pb_dogrulama_sinif[PB_DOGRULAMA_ADET] = {{
{dizi(np.array([etiket[i] for i in secim]))}
}};

/* PC'nin ayni pencereye verdigi tahmin (argmax). */
static const int16_t pb_dogrulama_pc_tahmin[PB_DOGRULAMA_ADET] = {{
{dizi(np.array(tahmin))}
}};

/* Girdi pencereleri: kare disar (eskiden yeniye), bant icerde —
 * mel.c'deki pb_mel_window() duzeninin aynisi. */
static const int8_t pb_dogrulama_girdi[PB_DOGRULAMA_ADET]
                                      [PB_DOGRULAMA_KARE * PB_DOGRULAMA_BANT] = {{
""")
        for k in range(len(secim)):
            f.write("  {\n" + dizi(girdiler[k].reshape(-1)) + "\n  },\n")
        f.write("};\n\n/* PC'nin ham int8 logit'leri. */\nstatic const int8_t "
                "pb_dogrulama_logit[PB_DOGRULAMA_ADET][PB_DOGRULAMA_SINIF] = {\n")
        for k in range(len(secim)):
            f.write("  {\n" + dizi(logitler[k]) + "\n  },\n")
        f.write("};\n\n#endif /* POKEBIRD_DOGRULAMA_SETI_H */\n")

    boyut = CIKTI.stat().st_size
    print(f"\n{CIKTI.relative_to(KOK)} yazildi ({boyut/1024:.0f} KB kaynak, "
          f"{len(secim)*KARE*BANT/1024:.0f} KB flash)")


if __name__ == "__main__":
    main()
