#!/usr/bin/env python3
"""
birdnet_slist.py — species_istanbul.csv'den BirdNET tür listesi üret.

BirdNET'e `--slist` verilmezse 6.522 türün tamamını arar ve bizim 178 türle
ilgisi olmayan tespitler döker. Liste verilince model yalnızca o türlerin
skorlarını üretir; hem hızlanır hem de yanlış tür etiketleme riski düşer.

BİÇİM (paketin labels/V2.4/*.txt dosyalarından doğrulandı):

    Bilimsel ad_İngilizce ad          ör. "Corvus cornix_Hooded Crow"

Satır BirdNET'in etiket dosyasındaki hâliyle BİREBİR eşleşmeli. Eşleşmezse
BirdNET o satırı sessizce yok sayar — bu yüzden İngilizce adı kendi CSV'mizden
yazmıyoruz. Bunun yerine BirdNET'in kendi `eBird_taxonomy_codes_2024E.json`
dosyasıyla `ebird_kodu -> etiket` eşlemesi yapıp sonucu Labels.txt'ye karşı
DOĞRULUYORUZ. Eşleşmeyen tür varsa yazdırılır; sessiz kayıp olmaz.

    python tools/birdnet_slist.py
"""

import argparse
import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
VENV_PKG = os.path.join(
    ROOT, ".venv-birdnet", "Lib", "site-packages", "birdnet_analyzer"
)


def batch_dizini(verilen):
    if verilen:
        return verilen
    if os.path.isdir(VENV_PKG):
        return VENV_PKG
    try:
        import birdnet_analyzer

        return os.path.dirname(os.path.abspath(birdnet_analyzer.__file__))
    except ImportError:
        sys.exit("birdnet_analyzer bulunamadi; --paket ile dizinini verin")


def etiketleri_oku(pkg):
    """Modelin gercekten kullandigi etiket listesi."""
    path = os.path.join(pkg, "checkpoints", "V2.4", "BirdNET_GLOBAL_6K_V2.4_Labels.txt")
    if not os.path.exists(path):
        sys.exit(
            f"Etiket dosyasi yok: {path}\n"
            "Model henuz inmemis. Once bir kez analyze calistirin "
            "(ilk calistirmada V2.4.zip iniyor)."
        )
    with open(path, encoding="utf-8") as f:
        return [s.strip() for s in f if s.strip()]


def kodlari_oku(pkg):
    path = os.path.join(pkg, "eBird_taxonomy_codes_2024E.json")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--batch", help="birdnet_analyzer paket dizini")
    ap.add_argument("--csv", default=os.path.join(DATA, "species_istanbul.csv"))
    ap.add_argument("--out", default=os.path.join(DATA, "birdnet_slist.txt"))
    ap.add_argument(
        "--name-map", default=os.path.join(DATA, "birdnet_ad_haritasi.csv"),
        help="ebird_kodu -> BirdNET etiketi; birdnet_ozet.py bunu okur"
    )
    a = ap.parse_args()

    pkg = batch_dizini(a.batch)
    labels = etiketleri_oku(pkg)
    kodlar = kodlari_oku(pkg)

    # Bilimsel ada gore ikincil arama: eBird kodu tutmazsa (taksonomi surumu
    # farkli olabilir) tur adindan yakalamayi dene.
    scientific = {}
    for et in labels:
        scientific.setdefault(et.split("_", 1)[0], et)

    with open(a.csv, encoding="utf-8") as f:
        rows = [r for r in csv.DictReader(f) if r["durum"] == "dahil"]

    label_subset = set(labels)
    secilen, missing, farkli = [], [], []
    for r in rows:
        code, name = r["ebird_kodu"], r["bilimsel_ad"]
        et = kodlar.get(code)
        source = "kod"
        if et not in label_subset:
            et = scientific.get(name)
            source = "bilimsel ad"
        if et is None:
            missing.append((code, name, r["turkce_ad"]))
            continue
        bn_name = et.split("_", 1)[0]
        if bn_name != name:
            farkli.append((code, name, bn_name, r["turkce_ad"]))
        secilen.append((et, code, source, name, bn_name, r["turkce_ad"]))

    with open(a.out, "w", encoding="utf-8") as f:
        for s in secilen:
            f.write(s[0] + "\n")

    with open(a.name_map, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ebird_kodu", "bizim_bilimsel_ad", "birdnet_bilimsel_ad", "turkce_ad"])
        for _, code, _, name, bn_name, tr in secilen:
            w.writerow([code, name, bn_name, tr])

    name_ile = sum(1 for s in secilen if s[2] == "bilimsel ad")
    print(f"CSV'de 'dahil' tur : {len(rows)}")
    print(f"BirdNET'te bulunan : {len(secilen)}  (bilimsel adla kurtarilan {name_ile})")
    print(f"tur listesi        : {a.out}")
    print(f"ad haritasi        : {a.name_map}")

    if farkli:
        # SESSIZ HATA KAYNAGI: ozet betigi hedef turu bilimsel adla ariyor.
        # BirdNET eski cins adini kullanan bu turlerde arama tutmaz ve o turun
        # skoru her dilimde 0 gorunur — tur sessizce egitim disi kalir.
        print(f"\n!! BirdNET FARKLI BILIMSEL AD kullaniyor ({len(farkli)} tur).")
        print("   Eslesme eBird koduyla kuruldu; ad haritasi bu yuzden var.")
        for code, name, bn_name, tr in farkli:
            print(f"   {code:10s} {tr:22s} bizde '{name}'  ->  BirdNET '{bn_name}'")

    if missing:
        print(f"\n!! BirdNET'te YOK ({len(missing)} tur) — bu turler egitilemez:")
        for code, name, tr in missing:
            print(f"   {code:10s} {name:30s} {tr}")


if __name__ == "__main__":
    main()
