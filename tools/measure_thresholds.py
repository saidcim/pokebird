#!/usr/bin/env python3
"""
esik_olc.py — Ekranda tur adi gostermek icin GUVEN ESIGINI olc. (M7 adim 2)

    python tools/measure_thresholds.py            # numpy yeter, TensorFlow GEREKMEZ

NEDEN: §9k'da olculen sayilar DOGRULUK (top-1 %70,40 / top-3 %82,20). Karar
kurali icin gereken baska bir sey: "birlestirilmis olasilik p1 su degerin
ustundeyse tur adini yazsam ne kadar sik hakli olurum?" Bu, dogruluktan
turetilemez — olcmek gerekiyor. Ekrandaki karar kuralinin esikleri buradan
geliyor; tahmin edilmis tek sayi yok.

GIRDI: models/test_olasilik.npy — tools/measure_voting.py'nin biraktigi
onbellek (test kumesindeki her pencerenin INT8 modelden cikan softmax'i).
Yoksa once onu calistirin (.venv-birdnet ortami, TensorFlow gerekiyor).

KAPSAM — abartmadan: birlestirme_olc.py'nin basligindaki iki cekince burada
da gecerli. Bloklar AYNI KAYDIN ardisik dilimlerinden ve dilimler BirdNET'in
kus duydugu yerler; cihazda pencereler 1 sn adimla ORTUSUYOR, yani hatalar
daha ilintili. Asagidaki isabet oranlari bir UST SINIR. Esikleri buna gore
SECIYORUZ ama saha kalibrasyonu (M8) hala gerekli.

Cikti: models/thresholds.txt
"""

import csv
import os
import sys
from collections import defaultdict

import numpy as np

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRAIN = os.path.join(ROOT, "data", "egitim")
MODELLER = os.path.join(ROOT, "models")
ONBELLEK = os.path.join(MODELLER, "test_olasilik.npy")

NEGATIVE = 178          # siniflar.csv'nin son sinifi: negatif / bilinmiyor
WINDOW = 8            # cihazdaki birlestirme penceresi (PB_VOTE_WINDOWS)

# Secim olcutleri — hangi isabete razi oldugumuz bir URUN karari, sayilar degil.
# Girme esigi: tur adini ekrana yazdigimizda hakli olma orani.
TARGET_ISABET_GIRIS = 0.85
# Cikma esigi: yazdigimizi silme noktasi. Birlestirilmis top-1 dogrulugunun
# (%70,40) altina dusen bir gosterim artik "en iyi tahmin"den iyi degil.
TARGET_ISABET_CIKIS = 0.70


def bloklar(P, Y, records, n):
    """Ardisik n pencereyi ortala. birlestirme_olc.py ile AYNI kural (softmax
    ortalamasi) — baska bir kural secilirse §9k'daki sayilar gecersiz olur."""
    p1, pred, target = [], [], []
    for v in records.values():
        for b in range(0, len(v), n):
            grup = [k for _, k in v[b:b + n]]
            if len(grup) < min(n, 2) and n > 1:
                continue
            ort = P[grup].mean(axis=0)
            t = int(np.argmax(ort))
            p1.append(float(ort[t]))
            pred.append(t)
            target.append(int(Y[grup[0]]))
    return np.array(p1), np.array(pred), np.array(target)


def tablo(p1, pred, target, esikler):
    """Her esik icin: kapsam, isabet, yanlis alarm, kacirma."""
    bird = target != NEGATIVE
    row = []
    for t in esikler:
        duyuru = (p1 >= t) & (pred != NEGATIVE)
        n_duyuru = int(duyuru.sum())
        isabet = float((pred[duyuru] == target[duyuru]).mean()) if n_duyuru else float("nan")
        kapsam = n_duyuru / len(p1)
        # Sahadaki en pahali hata: gurultuyu kus sanmak.
        yanlis_alarm = float(duyuru[~bird].mean()) if (~bird).any() else float("nan")
        kacirma = float(1.0 - duyuru[bird].mean()) if bird.any() else float("nan")
        row.append((t, n_duyuru, kapsam, isabet, yanlis_alarm, kacirma))
    return row


def pick(row, target_isabet):
    """Hedef isabete ulasan EN KUCUK esik — kapsami gereksiz yere kismayalim."""
    for t, n, kapsam, isabet, ya, kac in row:
        if n >= 50 and isabet >= target_isabet:
            return t, kapsam, isabet, ya
    return None, None, None, None


def main():
    if not os.path.exists(ONBELLEK):
        sys.exit(f"{ONBELLEK} yok — once .venv-birdnet ile "
                 "tools/measure_voting.py calistirin (onbellegi o uretiyor).")

    P = np.load(ONBELLEK)
    y = np.load(os.path.join(TRAIN, "etiket.npy"))
    with open(os.path.join(TRAIN, "ornekler.csv"), encoding="utf-8") as f:
        r = list(csv_compat.reader(f))

    idx = np.array([i for i, x in enumerate(r) if x["split"] == "test"])
    if len(P) != len(idx):
        sys.exit(f"onbellek {len(P)} satir, test kumesi {len(idx)} — "
                 "esitlenmemis. birlestirme_olc.py'yi yeniden calistirin.")
    Y = y[idx]

    records = defaultdict(list)
    for k, i in enumerate(idx):
        records[(r[i]["ebird_code"], r[i]["file"])].append(
            (float(r[i]["start"]), k))
    for v in records.values():
        v.sort()

    esikler = [round(0.05 * i, 2) for i in range(1, 20)]
    s = []
    s.append("EKRAN KARAR KURALI — guven esigi olcumu")
    s.append("(model: models/tur_agi_int8.tflite, test kumesi, "
             f"{len(idx)} pencere / {len(records)} kayit)")
    s.append("kapsam = kac blokta tur adi yazariz - isabet = yazdigimizda "
             "hakli olma orani")
    s.append("yanlis alarm = negatif bloklarin kacinda tur adi yazariz - "
             "kacirma = kus bloklarinin kacini kaciririz")

    secimler = {}
    for n in (1, 3, WINDOW):
        p1, pred, target = bloklar(P, Y, records, n)
        s.append("")
        s.append(f"--- {n} pencere birlestirilmis ({len(p1)} blok) "
                 f"------------------------")
        s.append("  esik    blok  kapsam   isabet  yanlis-alarm  kacirma")
        row = tablo(p1, pred, target, esikler)
        for t, nd, kapsam, isabet, ya, kac in row:
            s.append(f"  {t:4.2f}  {nd:6d}  %{100*kapsam:5.1f}   "
                     f"%{100*isabet:5.1f}   %{100*ya:9.1f}   %{100*kac:5.1f}")
        secimler[n] = row

    satir8 = secimler[WINDOW]
    giris = pick(satir8, TARGET_ISABET_GIRIS)
    cikis = pick(satir8, TARGET_ISABET_CIKIS)

    s.append("")
    s.append("--- SECIM ---------------------------------------------------")
    s.append(f"girme esigi (isabet >= %{100*TARGET_ISABET_GIRIS:.0f}): "
             f"{giris[0]}  -> kapsam %{100*giris[1]:.1f}, isabet "
             f"%{100*giris[2]:.1f}, yanlis alarm %{100*giris[3]:.1f}"
             if giris[0] is not None else
             "girme esigi: hedef isabete ULASILAMADI — hedefi dusurun ya da "
             "modeli iyilestirin")
    s.append(f"cikma esigi (isabet >= %{100*TARGET_ISABET_CIKIS:.0f}): "
             f"{cikis[0]}  -> kapsam %{100*cikis[1]:.1f}, isabet "
             f"%{100*cikis[2]:.1f}, yanlis alarm %{100*cikis[3]:.1f}"
             if cikis[0] is not None else
             "cikma esigi: hedef isabete ULASILAMADI")
    s.append("")
    s.append("Histerezis: p1 >= girme esigine ulasinca tur adi yazilir, "
             "p1 < cikma esigine dusene kadar kalir.")
    s.append("UYARI: bu sayilar UST SINIR — bloklar ortusmeyen dilimlerden, "
             "cihazda pencereler 1 sn adimla ortusuyor (hatalar ilintili).")

    text = "\n".join(s)
    print("\n" + text)
    with open(os.path.join(MODELLER, "esik.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
