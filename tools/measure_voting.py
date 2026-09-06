#!/usr/bin/env python3
"""
birlestirme_olc.py — Zamansal birlestirmenin dogruluga katkisini olc. (M5)

    .venv-birdnet\\Scripts\\python -u tools/measure_voting.py

NEDEN BU OLCUM: `egit.py`'nin bildirdigi top-1/top-3 TEK 3 saniyelik pencere
basina. Cihaz oyle calismiyor — pencere 3 sn ama adim 1 sn, ve Asama-3 ardisik
pencerelerin softmax'ini birlestiriyor (ARCHITECTURE §4). Kullanicinin ekranda
gordugu sayi bu birlestirilmis sonuc. Pencere basina dogrulugu optimize etmek,
yanlis sayiyi kovalamak olabilir; once dogru sayiya bakalim.

KAPSAM — abartmadan yazalim. Buradaki birlestirme, test kumesindeki AYNI
KAYDIN ardisik dilimleri uzerinden yapiliyor. Cihazdakiyle iki farki var:
  * cihaz 1 sn adimla daha cok ortusen pencere goruyor -> hatalari daha
    ILINTILI, yani gercek kazanc buradakinden bir miktar DUSUK olur;
  * buradaki dilimler BirdNET'in kus duydugu dilimler, yani kus surekli
    otuyor varsayimi bu kumede gecerli.
Dolayisiyla asagidaki sayilar bir UST SINIR tahmini. Kesin cevap M8'deki
saha testinde.

Cikti: models/voting.txt
"""

import csv
import os
import sys
from collections import defaultdict

import numpy as np
import tensorflow as tf

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRAIN = os.path.join(ROOT, "data", "egitim")
MODELS = os.path.join(ROOT, "models")
TFLITE = csv_compat.resolve(os.path.join(MODELS, "species_net_int8.tflite"))
CACHE = csv_compat.resolve(os.path.join(MODELS, "test_probs.npy"))


def olasiliklar(idx, X):
    """Test penceresi basina softmax. Bir kez hesaplanip onbellege alinir."""
    if os.path.exists(CACHE):
        P = np.load(CACHE)
        if len(P) == len(idx):
            print(f"onbellekten okundu: {CACHE}")
            return P
    it = tf.lite.Interpreter(model_path=TFLITE, num_threads=10)
    it.allocate_tensors()
    g, c = it.get_input_details()[0], it.get_output_details()[0]
    scale, sifir = c["quantization"]
    P = np.zeros((len(idx), c["shape"][-1]), dtype=np.float32)
    for k, i in enumerate(idx):
        it.set_tensor(g["index"], X[i].reshape(g["shape"]).astype(np.int8))
        it.invoke()
        raw = (it.get_tensor(c["index"])[0].astype(np.float32) - sifir) * scale
        e = np.exp(raw - raw.max())
        P[k] = e / e.sum()
        if k % 1000 == 0:
            print(f"  {k}/{len(idx)}", flush=True)
    np.save(CACHE, P)
    return P


def main():
    if not os.path.exists(TFLITE):
        sys.exit(f"{TFLITE} yok — once tools/train_species.py")
    X = np.load(csv_compat.resolve(os.path.join(TRAIN, "windows.npy")), mmap_mode="r")
    y = np.load(csv_compat.resolve(os.path.join(TRAIN, "labels.npy")))
    with open(csv_compat.resolve(os.path.join(TRAIN, "samples.csv")), encoding="utf-8") as f:
        r = list(csv_compat.reader(f))
    name = {int(s["class_index"]): s["turkish_name"] for s in
          csv_compat.reader(open(csv_compat.resolve(os.path.join(TRAIN, "classes.csv")),
                              encoding="utf-8"))}

    idx = np.array([i for i, x in enumerate(r) if x["split"] == "test"])
    P = olasiliklar(idx, X)
    Y = y[idx]

    # Ayni kaydin dilimlerini zaman sirasina diz.
    record = defaultdict(list)
    for k, i in enumerate(idx):
        record[(r[i]["ebird_code"], r[i]["file"])].append(
            (float(r[i]["start"]), k))
    for v in record.values():
        v.sort()

    s = []
    s.append("ZAMANSAL BIRLESTIRME — kac ardisik pencere oylanirsa ne oluyor")
    s.append("(secim yok, olcum test kumesinde; ust sinir tahmini, bkz. betik "
             "basligi)")
    s.append("")
    s.append(" pencere   ornek    top-1     top-3")
    for n in (1, 2, 3, 5, 8, 12):
        d1 = d3 = count = 0
        for (_, _), v in record.items():
            for b in range(0, len(v), n):
                grup = [k for _, k in v[b:b + n]]
                if len(grup) < min(n, 2) and n > 1:
                    continue
                ort = P[grup].mean(axis=0)
                ilk3 = np.argsort(-ort)[:3]
                target = Y[grup[0]]
                d1 += int(ilk3[0] == target)
                d3 += int(target in ilk3)
                count += 1
        s.append(f" {n:>7}  {count:>6}   %{100 * d1 / count:5.2f}   "
                 f"%{100 * d3 / count:5.2f}")

    # Negatif sinif ayri: sahada en pahali hata "gurultuyu kus sanmak".
    neg = 178
    bird = Y != neg
    ilk = np.argmax(P, axis=1)
    s.append("")
    s.append(f"negatifi kus sanma orani (pencere basina): "
             f"%{100 * (ilk[~bird] != neg).mean():.2f}")
    s.append(f"kusu negatif sanma orani  (pencere basina): "
             f"%{100 * (ilk[bird] == neg).mean():.2f}")
    s.append("")
    s.append("En cok karisan 15 cift (pencere basina, test):")
    cift = defaultdict(int)
    for h, t in zip(Y, ilk):
        if h != t:
            cift[(int(h), int(t))] += 1
    for (h, t), n in sorted(cift.items(), key=lambda x: -x[1])[:15]:
        s.append(f"  {name.get(h, h):26s} -> {name.get(t, t):26s} {n:4d}")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(MODELS, "birlestirme.txt"), "w",
              encoding="utf-8") as f:
        f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
