#!/bin/sh
# Kalan turleri indir -> WAV'a cevir -> mp3'leri sil. Iki tur:
# indirme sirasinda mp3 birikmesin diye arada bir cevirim yapiliyor.
set -e
cd "$(dirname "$0")/.."
python tools/xc_fetch.py --indir --adet 40
python tools/xc_convert.py --adet 40
echo "=== M4 INDIRME + DONUSUM TAMAM ==="
