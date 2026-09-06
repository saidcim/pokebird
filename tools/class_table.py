#!/usr/bin/env python3
"""
sinif_tablosu.py — data/egitim/siniflar.csv -> src/ai/classes.h

Model 179 sınıf indeksi üretiyor; cihazın onları isme çevirmesi lazım.
Tablo flash'ta (`const char *const`), RAM'de değil.

Saf stdlib, Python 3.14 yeter:
    python tools/class_table.py
"""
from __future__ import annotations

import csv
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
INP = ROOT / "data" / "egitim" / "siniflar.csv"
OUTPUT = ROOT / "src" / "ai" / "siniflar.h"


def kacir(s: str) -> str:
    """C string literali. Türkçe harfler UTF-8 bayt olarak kalıyor — seri
    terminal UTF-8 okuyor, arayüzün yazı tipleri de (tools/generate_fonts.py ile
    üretiliyor) Türkçe harfleri içeriyor, yani ekrana da olduğu gibi
    basılabiliyorlar."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main() -> None:
    if not INP.exists():
        sys.exit(f"{INP} yok — once tools/build_dataset.py calistirin.")

    rows = []
    with open(INP, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            rows.append((int(s["class_index"]), s["ebird_code"], s["turkish_name"],
                             s.get("birdnet_scientific_name", "")))
    rows.sort()

    beklenen = list(range(len(rows)))
    if [s[0] for s in rows] != beklenen:
        sys.exit("siniflar.csv'de indeksler 0..N-1 degil — tablo hizasiz olurdu")

    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"""/* Uretilmis dosya — tools/class_table.py. ELLE DUZENLEMEYIN.
 * Kaynak: data/egitim/siniflar.csv ({len(rows)} sinif)
 *
 * Sinif {len(rows) - 1} = negatif / bilinmiyor.
 * Diziler flash'ta (const), RAM maliyeti sifir.
 */
#ifndef POKEBIRD_CLASSES_H
#define POKEBIRD_CLASSES_H

#define PB_CLASS_COUNT {len(rows)}

static const char *const pb_class_code[PB_CLASS_COUNT] = {{
""")
        for _, code, _name, _lat in rows:
            f.write(f'  "{kacir(code)}",\n')
        f.write("};\n\nstatic const char *const pb_class_name[PB_CLASS_COUNT] = {\n")
        for _, _code, name, _lat in rows:
            f.write(f'  "{kacir(name)}",\n')
        # Bilimsel adlar: arayüz tür adının altında gösteriyor (tasarımdaki
        # italik satır). Flash'ta duruyor, RAM maliyeti sifir.
        f.write("};\n\nstatic const char *const pb_class_latin[PB_CLASS_COUNT] = {\n")
        for _, _code, _name, lat in rows:
            f.write(f'  "{kacir(lat)}",\n')
        f.write("};\n\n#endif /* POKEBIRD_CLASSES_H */\n")

    print(f"{OUTPUT.relative_to(ROOT)} yazildi — {len(rows)} sinif")


if __name__ == "__main__":
    main()
