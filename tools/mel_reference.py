#!/usr/bin/env python3
"""
mel_reference.py — Cihazdaki mel hattını bağımsız bir uygulamayla karşılaştır.

Plan §M3'ün kabul ölçütü: "Cihazdaki mel, Python'daki mel ile ≈aynı".

Buradaki uygulama numpy ile SIFIRDAN yazıldı; C tarafıyla ortak kod yok.
Amaç bu: iki bağımsız yol aynı sayıyı veriyorsa ölçekleme, pencereleme,
bin indeksleme ve filtre bankası kurulumunda hata olma ihtimali çok düşer.
Aynı formülleri paylaşıyor olmaları kaçınılmaz (aynı şeyi hesaplıyorlar),
ama uygulama hataları ortak değil.

Kullanım:
    cmake -S test -B test/build -G Ninja && cmake --build test/build
    ./test/build/dsp_test --dump > /tmp/mel_c.csv
    python tools/mel_reference.py /tmp/mel_c.csv

Argümansız çalıştırılırsa dsp_test'i kendisi bulup çalıştırmayı dener.

librosa karşılığı (kurulu değilse gerekmiyor, formüller aşağıda):
    librosa.filters.mel(sr=24000, n_fft=512, n_mels=64,
                        fmin=150, fmax=11500, htk=True, norm=None)
"""

import csv
import io
import subprocess
import sys
from pathlib import Path

import numpy as np

SR = 24000
N_FFT = 512
N_MELS = 64
FMIN = 150.0
FMAX = 11500.0
DB_MIN = -90.0
DB_MAX = 0.0

# Farkın kabul edilebilir üst sınırı. int8 saklama adımı
# (DB_MAX-DB_MIN)/255 = 0.353 dB; bir adımlık yuvarlama farkı normal.
TOLERANS_DB = 0.60


def hz_to_mel(hz):
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def mel_to_hz(m):
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)


def mel_filterbank():
    """HTK mel, alan normalizasyonu YOK (librosa htk=True, norm=None)."""
    pts_mel = np.linspace(hz_to_mel(FMIN), hz_to_mel(FMAX), N_MELS + 2)
    pts_bin = mel_to_hz(pts_mel) * N_FFT / SR

    n_bins = N_FFT // 2 + 1
    fb = np.zeros((N_MELS, n_bins), dtype=np.float64)
    k = np.arange(n_bins, dtype=np.float64)
    for i in range(N_MELS):
        left, center, right = pts_bin[i], pts_bin[i + 1], pts_bin[i + 2]
        if center > left:
            up = (k - left) / (center - left)
            fb[i] += np.where((k >= np.ceil(left)) & (k <= center), up, 0.0)
        if right > center:
            down = (right - k) / (right - center)
            fb[i] += np.where((k > center) & (k <= np.floor(right)), down, 0.0)
    return np.clip(fb, 0.0, None)


def power_spectrum(samples_i16):
    """C tarafındaki pb_fft_power ile aynı sözleşme.

    - DC çıkarılır (mikrofonun sabit ofseti var)
    - periyodik Hann (sym=False)
    - tam ölçekli sinüs kendi bin'inde 1.0 güç -> 0 dBFS
    """
    x = samples_i16.astype(np.float64)
    x = (x - x.mean()) / 32768.0
    w = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / N_FFT)
    X = np.fft.rfft(x * w)
    p = np.abs(X) ** 2

    gain = 0.5 * N_FFT
    scale = 4.0 / (gain * gain)
    p *= scale
    p[0] *= 0.5          # DC ve Nyquist'in negatif frekans eşi yok
    p[-1] *= 0.5
    return p


def quantize_db(db):
    db = np.clip(db, DB_MIN, DB_MAX)
    t = (db - DB_MIN) / (DB_MAX - DB_MIN)
    return np.clip(np.rint(t * 255.0) - 128, -128, 127).astype(np.int16)


def test_signal():
    """dsp_test.c içindeki dump_frame() ile BİREBİR aynı girdi."""
    i = np.arange(N_FFT)
    t = i / SR
    v = (0.40 * np.sin(2 * np.pi * 1000.0 * t)
         + 0.25 * np.sin(2 * np.pi * 4300.0 * t)
         + 0.10 * ((i % 97) / 97.0 - 0.5))
    return np.rint(v * 32767.0).astype(np.int16)


def reference_mel():
    p = power_spectrum(test_signal())
    energy = mel_filterbank() @ p
    db = 10.0 * np.log10(energy + 1e-10)
    return db, quantize_db(db)


def read_c_output(text):
    rows = list(csv.DictReader(io.StringIO(text)))
    if not rows:
        sys.exit("dsp_test --dump ciktisi bos.")
    q = np.array([int(r["q"]) for r in rows], dtype=np.int16)
    db = np.array([float(r["db"]) for r in rows], dtype=np.float64)
    return q, db


def main():
    if len(sys.argv) > 1:
        text = Path(sys.argv[1]).read_text()
    else:
        exe = None
        for candidate in ("test/build/dsp_test.exe", "test/build/dsp_test"):
            if Path(candidate).exists():
                # Mutlak yol: Windows'ta gorece yol PATH'te aranir ve bulunamaz.
                exe = str(Path(candidate).resolve())
                break
        if exe is None:
            sys.exit("dsp_test bulunamadi. Once:\n"
                     "  cmake -S test -B test/build -G Ninja\n"
                     "  cmake --build test/build")
        text = subprocess.run([exe, "--dump"], capture_output=True,
                              text=True, check=True).stdout

    c_q, c_db = read_c_output(text)
    ref_db, ref_q = reference_mel()

    if len(c_q) != N_MELS:
        sys.exit(f"Bant sayisi uyusmuyor: C {len(c_q)}, referans {N_MELS}")

    # int8 saklamasindan cozulmus dB uzerinden karsilastir; ham dB'ler
    # kirpma araligi disinda ayrilabilir ve bu bilgi zaten saklanmiyor.
    ref_db_q = DB_MIN + (ref_q.astype(np.float64) + 128.0) / 255.0 * (DB_MAX - DB_MIN)
    fark = np.abs(c_db - ref_db_q)
    q_fark = np.abs(c_q.astype(int) - ref_q.astype(int))

    print(f"{'bant':>4} {'C (dB)':>9} {'ref (dB)':>9} {'fark':>7}  {'C q':>5} {'ref q':>6}")
    for b in range(N_MELS):
        marker = "  <<<" if fark[b] > TOLERANS_DB else ""
        print(f"{b:>4} {c_db[b]:>9.3f} {ref_db_q[b]:>9.3f} {fark[b]:>7.3f} "
              f"{c_q[b]:>5} {ref_q[b]:>6}{marker}")

    print()
    print(f"en buyuk fark : {fark.max():.4f} dB  (tolerans {TOLERANS_DB})")
    print(f"ortalama fark : {fark.mean():.4f} dB")
    print(f"int8 adiminda en buyuk sapma: {q_fark.max()}")

    if fark.max() > TOLERANS_DB:
        print("\nKALDI: mel hatti referansla uyusmuyor.")
        return 1
    print("\nGECTI: cihazdaki mel, bagimsiz referansla uyusuyor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
