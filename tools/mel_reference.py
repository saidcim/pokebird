#!/usr/bin/env python3
"""
mel_reference.py — compare the device's mel pipeline against an independent
implementation.

The acceptance criterion: the mel on the device matches the mel in Python.

The implementation here was written FROM SCRATCH with numpy; it shares no code
with the C side. That is the point: if two independent paths produce the same
numbers, the chance of an error in the scaling, the windowing, the bin
indexing or the filter bank construction drops sharply. They inevitably share
the same formulas (they compute the same thing), but they do not share
implementation mistakes.

Usage:
    cmake -S firmware/test -B firmware/test/build -G Ninja
    cmake --build firmware/test/build
    ./firmware/test/build/dsp_test --dump > mel_c.csv
    python tools/mel_reference.py mel_c.csv

Run without an argument and it tries to find and run dsp_test itself.

The librosa equivalent (not required if it is not installed; the formulas are
below):
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

# The acceptable upper bound on the difference. The int8 storage step is
# (DB_MAX-DB_MIN)/255 = 0.353 dB, so a one-step rounding difference is normal.
TOLERANCE_DB = 0.60


def hz_to_mel(hz):
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def mel_to_hz(m):
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)


def mel_filterbank():
    """HTK mel, with NO area normalisation (librosa htk=True, norm=None)."""
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
    """The same contract as pb_fft_power on the C side.

    - DC is removed (the microphone has a constant offset)
    - periodic Hann (sym=False)
    - a full-scale sine gives 1.0 power in its own bin -> 0 dBFS
    """
    x = samples_i16.astype(np.float64)
    x = (x - x.mean()) / 32768.0
    w = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / N_FFT)
    X = np.fft.rfft(x * w)
    p = np.abs(X) ** 2

    gain = 0.5 * N_FFT
    scale = 4.0 / (gain * gain)
    p *= scale
    p[0] *= 0.5          # DC and Nyquist have no negative-frequency partner
    p[-1] *= 0.5
    return p


def quantize_db(db):
    db = np.clip(db, DB_MIN, DB_MAX)
    t = (db - DB_MIN) / (DB_MAX - DB_MIN)
    return np.clip(np.rint(t * 255.0) - 128, -128, 127).astype(np.int16)


def test_signal():
    """EXACTLY the same input as dump_frame() in dsp_test.c."""
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
        sys.exit("the output of dsp_test --dump is empty.")
    q = np.array([int(r["q"]) for r in rows], dtype=np.int16)
    db = np.array([float(r["db"]) for r in rows], dtype=np.float64)
    return q, db


def main():
    if len(sys.argv) > 1:
        text = Path(sys.argv[1]).read_text()
    else:
        exe = None
        for candidate in ("firmware/test/build/dsp_test.exe",
                          "firmware/test/build/dsp_test"):
            if Path(candidate).exists():
                # Absolute path: on Windows a relative one is looked up on
                # PATH and would not be found.
                exe = str(Path(candidate).resolve())
                break
        if exe is None:
            sys.exit("dsp_test not found. First run:\n"
                     "  cmake -S firmware/test -B firmware/test/build -G Ninja\n"
                     "  cmake --build firmware/test/build")
        text = subprocess.run([exe, "--dump"], capture_output=True,
                              text=True, check=True).stdout

    c_q, c_db = read_c_output(text)
    ref_db, ref_q = reference_mel()

    if len(c_q) != N_MELS:
        sys.exit(f"band count mismatch: C {len(c_q)}, reference {N_MELS}")

    # Compare on the dB decoded back from int8 storage: the raw dB values can
    # diverge outside the clipping range, and that information is not stored
    # anyway.
    ref_db_q = DB_MIN + (ref_q.astype(np.float64) + 128.0) / 255.0 * (DB_MAX - DB_MIN)
    diff = np.abs(c_db - ref_db_q)
    q_diff = np.abs(c_q.astype(int) - ref_q.astype(int))

    print(f"{'band':>4} {'C (dB)':>9} {'ref (dB)':>9} {'diff':>7}  "
          f"{'C q':>5} {'ref q':>6}")
    for b in range(N_MELS):
        marker = "  <<<" if diff[b] > TOLERANCE_DB else ""
        print(f"{b:>4} {c_db[b]:>9.3f} {ref_db_q[b]:>9.3f} {diff[b]:>7.3f} "
              f"{c_q[b]:>5} {ref_q[b]:>6}{marker}")

    print()
    print(f"largest difference : {diff.max():.4f} dB  "
          f"(tolerance {TOLERANCE_DB})")
    print(f"mean difference    : {diff.mean():.4f} dB")
    print(f"largest deviation in int8 steps: {q_diff.max()}")

    if diff.max() > TOLERANCE_DB:
        print("\nFAILED: the mel pipeline does not match the reference.")
        return 1
    print("\nPASSED: the device's mel matches the independent reference.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
