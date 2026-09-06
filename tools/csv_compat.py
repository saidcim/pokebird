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
    # In the GENERATED files this column held the display name, which is
    # now English. In the two committed CSVs the Turkish name is a column
    # of its own and keeps its own header, so it is unaffected.
    "turkce_ad": "english_name",
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
    "esc50_dosya": "esc50_file",
    "kaynak": "source",
    "lisans": "licence",
}
for _m in range(1, 13):
    LEGACY["ay_%02d" % _m] = "month_%02d" % _m


def rename(fieldnames):
    """Map a header row onto the current column names."""
    return [LEGACY.get(h, h) for h in (fieldnames or [])]


# legacy value -> current value, for the `split` column. Unlike the headers
# above these are DATA, so they cannot be fixed by renaming a field; `reader()`
# rewrites them as it reads.
LEGACY_SPLIT = {
    "egitim": "train",
    "dogrulama": "val",
    "test": "test",
}


def reader(f):
    """csv.DictReader that accepts either naming.

    Pass an already-open text file, exactly as you would to csv.DictReader.
    Legacy `split` values are normalised to train/val/test on the way out.
    """
    inner = csv.DictReader(f)
    inner.fieldnames = rename(inner.fieldnames)
    for row in inner:
        if "split" in row:
            row["split"] = LEGACY_SPLIT.get(row["split"], row["split"])
        yield row


def rows(path, encoding="utf-8"):
    """Convenience: read a whole CSV into a list of dicts."""
    with open(path, encoding=encoding) as f:
        return list(reader(f))


# ── Generated artifact names ──────────────────────────────────────────────
#
# The pipeline's generated files and directories were named in Turkish. They
# now have English names, but an existing data/ or models/ directory from an
# earlier run still holds the old ones, so `resolve()` falls back to the legacy
# name when the current one is absent. Writers always use the current name.

import os as _os

LEGACY_FILES = {
    "windows.npy": "pencereler.npy",
    "labels.npy": "etiket.npy",
    "teacher.npy": "ogretmen.npy",
    "samples.csv": "ornekler.csv",
    "classes.csv": "siniflar.csv",
    "summary.txt": "ozet.txt",
    "segments.csv": "segmentler.csv",
    "test_probs.npy": "test_olasilik.npy",
    "progress.html": "ilerleme.html",
    "species_net_int8.tflite": "tur_agi_int8.tflite",
    "binary_net_int8.tflite": "ikili_agi_int8.tflite",
    "species_net.keras": "tur_agi.keras",
    "binary_net.keras": "ikili_agi.keras",
    # directories, resolved the same way
    "dataset": "egitim",
    "negative": "negatif",
    "birdnet_result": "birdnet_sonuc",
    "segment_samples": "segment_ornek",
    "esc50_records.csv": "esc50_kayitlar.csv",
}


def resolve(path):
    """Return `path`, or its legacy-named equivalent if only that exists.

    Accepts str or pathlib.Path and returns the same type it was given.
    """
    p = str(path)
    if _os.path.exists(p):
        return path
    base = _os.path.basename(p)
    legacy = LEGACY_FILES.get(base)
    if not legacy:
        return path
    alt = _os.path.join(_os.path.dirname(p), legacy)
    if _os.path.exists(alt):
        return type(path)(alt) if not isinstance(path, str) else alt
    return path
