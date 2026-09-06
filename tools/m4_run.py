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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV_PATH = os.path.join(ROOT, "data", "species_istanbul.csv")
PY = sys.executable


def calistir(args):
    print(f"\n$ {' '.join(args)}\n", flush=True)
    p = subprocess.run([PY] + args, cwd=ROOT)
    if p.returncode != 0:
        sys.exit(f"[!] komut basarisiz (cikis {p.returncode}): {' '.join(args)}")


def include_count():
    with open(CSV_PATH, encoding="utf-8") as f:
        return sum(1 for r in csv.DictReader(f) if r["durum"] == "dahil")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target-species", type=int, default=120)
    ap.add_argument("--count", type=int, default=60)
    ap.add_argument("--common-threshold", type=int, default=30)
    args = ap.parse_args()

    # 1. Havuzu tazele (GBIF/eBird önbellekten, saniyeler sürer)
    calistir(["tools/species_list.py", "--aylik"])

    # 2. Sayım + eleme. Nadir tür eşiğini hedefe en yakın sonucu verecek
    #    şekilde ara: sayım önbelleğe alındığı için tekrarlar bedava.
    adaylar = [100, 125, 150, 175, 200, 250, 300]
    best, max_iyi_fark, max_iyi_count = None, 10**9, 0

    for rare in adaylar:
        calistir(["tools/xc_fetch.py", "--say",
                  "--esik", str(args.common_threshold), "--nadir-esik", str(rare)])
        count = include_count()
        fark = abs(count - args.target_species)
        print(f"\n>>> nadir-esik {rare} -> {count} tur (hedef {args.target_species}, "
              f"fark {fark})\n", flush=True)
        if fark < max_iyi_fark:
            best, max_iyi_fark, max_iyi_count = rare, fark, count
        # Havuz her turda daralıyor; bir sonraki eşik için tazele.
        calistir(["tools/species_list.py", "--aylik"])

    print(f"\n=== SECILEN: nadir-esik {best} -> {max_iyi_count} tur ===\n", flush=True)
    calistir(["tools/xc_fetch.py", "--say",
              "--esik", str(args.common_threshold), "--nadir-esik", str(best)])

    # 3. İndirme
    calistir(["tools/xc_fetch.py", "--indir", "--adet", str(args.count)])
    print("\n=== M4 VERI HATTI TAMAM ===", flush=True)


if __name__ == "__main__":
    sys.exit(main())
