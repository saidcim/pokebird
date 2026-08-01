#!/bin/sh
# Kalan turleri indir -> WAV'a cevir. python -u: cikti tamponlanmasin,
# surec beklenmedik sekilde olurse log'da nerede kaldigi gorunsun.
cd "$(dirname "$0")/.."
echo "[$(date +%H:%M:%S)] indirme basliyor"
python -u tools/xc_fetch.py --indir --adet 40 || echo "[!] indirme hata verdi: $?"
echo "[$(date +%H:%M:%S)] donusum basliyor"
python -u tools/xc_convert.py --adet 40 || echo "[!] donusum hata verdi: $?"
echo "[$(date +%H:%M:%S)] === M4 INDIRME + DONUSUM TAMAM ==="
