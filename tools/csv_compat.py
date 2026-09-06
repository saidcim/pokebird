#!/usr/bin/env python3
"""
csv_compat.py — read the pipeline's generated CSVs under either column naming.

The generated intermediates (data/egitim/ornekler.csv, siniflar.csv,
data/segmentler.csv and the BirdNET result files) used to carry Turkish column
names. They now carry English ones.

Those files are large regenerable outputs and are not committed, so an
existing data/ directory from an earlier run still holds the old headers.
Rather than forcing a 13 GB regeneration just to rename columns, every reader
goes through `reader()` here, which maps the legacy names onto the current
ones as it reads.

Only the generated files need this. The two committed CSVs
(data/species_istanbul.csv and data/birdnet_name_map.csv) were migrated in
place and carry the English names directly.
"""
from __future__ import annotations

import csv

# legacy Turkish column name -> current name
LEGACY = {
    "indeks": "index",
    "sinif": "class_index",
    "ebird_kodu": "ebird_code",
    "turkce_ad": "turkish_name",
    "ingilizce_ad": "english_name",
    "bilimsel_ad": "scientific_name",
    "bizim_bilimsel_ad": "our_scientific_name",
    "birdnet_bilimsel_ad": "birdnet_scientific_name",
    "dosya": "file",
    "baslangic": "start",
    "bitis": "end",
    "pencere_ornek": "window_samples",
    "bolum": "split",
    "bulasik": "contaminated",
    "hedef_guven": "target_confidence",
    "en_iyi_tur": "best_species",
    "en_iyi_guven": "best_confidence",
    "en_iyi_skor": "best_score",
    "kus_disi_tur": "non_bird_species",
    "kus_disi_guven": "non_bird_confidence",
    "kaydeden": "recordist",
    "kategori": "category",
    "durum": "status",
    "gerekce": "reason",
    "xc_ab": "xc_count",
    "xc_ab_eu": "xc_count_eu",
    "xc_ab_sa": "xc_count_world",
    "gbif_kayit": "gbif_records",
}
for _m in range(1, 13):
    LEGACY["ay_%02d" % _m] = "month_%02d" % _m


def rename(fieldnames):
    """Map a header row onto the current column names."""
    return [LEGACY.get(h, h) for h in (fieldnames or [])]


def reader(f):
    """csv.DictReader that accepts either naming.

    Pass an already-open text file, exactly as you would to csv.DictReader.
    """
    inner = csv.DictReader(f)
    inner.fieldnames = rename(inner.fieldnames)
    return inner


def rows(path, encoding="utf-8"):
    """Convenience: read a whole CSV into a list of dicts."""
    with open(path, encoding=encoding) as f:
        return list(reader(f))
