#!/usr/bin/env python3
"""
m4_run.py — M4 veri boru hattını uçtan uca, gözetimsiz çalıştır.

  1. Xeno-canto kayıt sayımı (doğru kalite filtresiyle: q:">C" = A/B)
  2. Nihai tür listesi — hedef tür sayısına en yakın eşiği kendi bulur
  3. Tür başına kayıt indirme

Neden ayrı script: sayım bitmeden nihai listenin kaç tür olacağı bilinmiyor,
indirme de o listeye bağlı. Üçünü elle zincirlemek yerine tek komut.

    python tools/m4_run.py --hedef-tur 120 --adet 60
"""

import argparse
import csv
import os
import subprocess
import sys

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV_YOL = os.path.join(KOK, "data", "species_istanbul.csv")
PY = sys.executable


def calistir(args):
    print(f"\n$ {' '.join(args)}\n", flush=True)
    p = subprocess.run([PY] + args, cwd=KOK)
    if p.returncode != 0:
        sys.exit(f"[!] komut basarisiz (cikis {p.returncode}): {' '.join(args)}")


def dahil_sayisi():
    with open(CSV_YOL, encoding="utf-8") as f:
        return sum(1 for r in csv.DictReader(f) if r["durum"] == "dahil")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hedef-tur", type=int, default=120)
    ap.add_argument("--adet", type=int, default=60)
    ap.add_argument("--yaygin-esik", type=int, default=30)
    args = ap.parse_args()

    # 1. Havuzu tazele (GBIF/eBird önbellekten, saniyeler sürer)
    calistir(["tools/species_list.py", "--aylik"])

    # 2. Sayım + eleme. Nadir tür eşiğini hedefe en yakın sonucu verecek
    #    şekilde ara: sayım önbelleğe alındığı için tekrarlar bedava.
    adaylar = [100, 125, 150, 175, 200, 250, 300]
    en_iyi, en_iyi_fark, en_iyi_sayi = None, 10**9, 0

    for nadir in adaylar:
        calistir(["tools/xc_fetch.py", "--say",
                  "--esik", str(args.yaygin_esik), "--nadir-esik", str(nadir)])
        sayi = dahil_sayisi()
        fark = abs(sayi - args.hedef_tur)
        print(f"\n>>> nadir-esik {nadir} -> {sayi} tur (hedef {args.hedef_tur}, "
              f"fark {fark})\n", flush=True)
        if fark < en_iyi_fark:
            en_iyi, en_iyi_fark, en_iyi_sayi = nadir, fark, sayi
        # Havuz her turda daralıyor; bir sonraki eşik için tazele.
        calistir(["tools/species_list.py", "--aylik"])

    print(f"\n=== SECILEN: nadir-esik {en_iyi} -> {en_iyi_sayi} tur ===\n", flush=True)
    calistir(["tools/xc_fetch.py", "--say",
              "--esik", str(args.yaygin_esik), "--nadir-esik", str(en_iyi)])

    # 3. İndirme
    calistir(["tools/xc_fetch.py", "--indir", "--adet", str(args.adet)])
    print("\n=== M4 VERI HATTI TAMAM ===", flush=True)


if __name__ == "__main__":
    sys.exit(main())
