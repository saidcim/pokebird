#!/usr/bin/env python3
"""
sinif_tablosu.py — data/egitim/siniflar.csv -> src/ai/siniflar.h

Model 179 sınıf indeksi üretiyor; cihazın onları isme çevirmesi lazım.
Tablo flash'ta (`const char *const`), RAM'de değil.

Saf stdlib, Python 3.14 yeter:
    python tools/sinif_tablosu.py
"""
from __future__ import annotations

import csv
import pathlib
import sys

KOK = pathlib.Path(__file__).resolve().parent.parent
GIRDI = KOK / "data" / "egitim" / "siniflar.csv"
CIKTI = KOK / "src" / "ai" / "siniflar.h"


def kacir(s: str) -> str:
    """C string literali. Türkçe harfler UTF-8 bayt olarak kalıyor —
    seri terminal UTF-8 okuyor, LVGL tarafı M7'de font seçerken bakacak."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main() -> None:
    if not GIRDI.exists():
        sys.exit(f"{GIRDI} yok — once tools/egitim_kumesi.py calistirin.")

    satirlar = []
    with open(GIRDI, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            satirlar.append((int(s["sinif"]), s["ebird_kodu"], s["turkce_ad"]))
    satirlar.sort()

    beklenen = list(range(len(satirlar)))
    if [s[0] for s in satirlar] != beklenen:
        sys.exit("siniflar.csv'de indeksler 0..N-1 degil — tablo hizasiz olurdu")

    with open(CIKTI, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"""/* Uretilmis dosya — tools/sinif_tablosu.py. ELLE DUZENLEMEYIN.
 * Kaynak: data/egitim/siniflar.csv ({len(satirlar)} sinif)
 *
 * Sinif {len(satirlar) - 1} = negatif / bilinmiyor.
 * Diziler flash'ta (const), RAM maliyeti sifir.
 */
#ifndef POKEBIRD_SINIFLAR_H
#define POKEBIRD_SINIFLAR_H

#define PB_SINIF_SAYISI {len(satirlar)}

static const char *const pb_sinif_kod[PB_SINIF_SAYISI] = {{
""")
        for _, kod, _ad in satirlar:
            f.write(f'  "{kacir(kod)}",\n')
        f.write("};\n\nstatic const char *const pb_sinif_ad[PB_SINIF_SAYISI] = {\n")
        for _, _kod, ad in satirlar:
            f.write(f'  "{kacir(ad)}",\n')
        f.write("};\n\n#endif /* POKEBIRD_SINIFLAR_H */\n")

    print(f"{CIKTI.relative_to(KOK)} yazildi — {len(satirlar)} sinif")


if __name__ == "__main__":
    main()
