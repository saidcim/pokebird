#!/usr/bin/env python3
"""
build_dataset.py — build the model input from segments.csv and the WAVs.

    python tools/build_dataset.py --checksum   # RUN THIS FIRST
    python tools/build_dataset.py

Output (data/dataset/):
    windows.npy   (N, 187, 64) int8    the model's input, the same as on the
                                       device
    labels.npy    (N,)         int16   the class index (178 = negative)
    teacher.npy   (N, 178)     float16 BirdNET's soft scores (distillation)
    samples.csv   the source, split and contamination flag of every row
    classes.csv   index -> eBird code / English name
    summary.txt   the report for this run

==========================================================================
1. MATCHING THE DEVICE EXACTLY — the only real risk in this script
==========================================================================
Whatever `pb_mel_window()` produces on the device, this has to produce the
same thing. If it does not, the model is good on the PC and bad on the
device, and the reason shows up as an error NOWHERE.

Hence:

  * The mel parameters were NOT REWRITTEN. `tools/mel_reference.py` is
    already the numpy reference for them (HTK mel, NO area normalisation,
    a periodic Hann, FFT 512) and it matched the C to 0.0000 dB. The
    accelerated (batched) path here is verified against it.
  * The frame layout and the window normalisation were carried over exactly
    from `firmware/src/dsp/mel.c` — INCLUDING THE INTERMEDIATE int8
    ROUNDING. The device first squeezes each frame into int8 over a -90..0 dB
    range and normalises from THOSE values. Skipping that step makes the
    output drift silently.
  * `--checksum` runs the same 3 seconds through both this script and the C
    code and compares the 64x187 matrices (firmware/test/dsp_test --window).
  * The constants are read out of `firmware/src/dsp/mel.h` and compared; if
    one of them changes, the script refuses to run.

==========================================================================
2. THE SPLIT IS PER RECORDING — NOT per slice
==========================================================================
If slices of the same XC recording land in both training and validation, the
model memorises the recording and the accuracy rises falsely. The split goes
by the `file` column; and beyond that, every recording by the same RECORDIST
goes into the same split (same equipment, same location, same background).
For the negatives, ESC-50's own 5 folds are used — they were separated for
exactly this reason.

==========================================================================
3. CONTAMINATED SLICES
==========================================================================
In a slice where `best_species != target`, the dominant sound in the
recording is a different bird. These:
  * NEVER enter validation or test (the measurement has to be clean),
  * stay in training with a `contaminated=1` flag and come with the full
    teacher vector — so the option of "train on the soft label" is open.
Training them silently under the target label would take extra work; the
default DOES NOT do that. `--contaminated drop` throws them out entirely.

==========================================================================
4. THE NEGATIVE CLASS
==========================================================================
Collecting was deferred, the CLASS was not. The source is ESC-50 (see
tools/esc50_download.py, with `chirping_birds` removed). If the negative
clips were also run through BirdNET (data/negative/birdnet_result), the ones
with a bird in them are filtered out on top of that.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile
import wave
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csv_compat  # noqa: E402
from mel_reference import (  # noqa: E402  - the ONE source of the parameters
    DB_MAX, DB_MIN, N_FFT, N_MELS, SR, mel_filterbank, power_spectrum,
    quantize_db,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")

# From firmware/src/dsp/mel.h; VERIFIED AGAINST THAT FILE below.
HOP = 384
FRAMES = 187
FMIN, FMAX = 150.0, 11500.0
WINDOW_SAMPLES = (FRAMES - 1) * HOP + N_FFT      # 71,936 samples ~ 3.00 s

NEGATIVE_CODE = "__negative__"
NEGATIVE_DIR = csv_compat.resolve(os.path.join(DATA, "negative"))

# If the dB standard deviation inside a window is below this, there is no
# sound there (digital silence). The normalisation on the device blows the
# scale up when the variance is < 1e-6; such a window reaches the model as
# +-127 noise.
SILENT_STD_DB = 0.5


# == 1. Verify the constants against the C header =========================
def verify_constants(header=os.path.join(ROOT, "firmware", "src", "dsp",
                                         "mel.h")):
    """STOP if this script and mel.h drift apart.

    The most expensive class of bug in this project is "a number that shifts
    silently". If the hop or the band count changes on the device side and
    this is not updated, the training set is invalid and no test says so.
    """
    if not os.path.exists(header):
        print(f"!! {header} is missing - the constant check was SKIPPED")
        return
    text = open(header, encoding="utf-8").read()

    def find(name):
        m = re.search(rf"#define\s+{name}\s+\(?(-?[\d.]+)f?\)?", text)
        return float(m.group(1)) if m else None

    expected = {
        "PB_SAMPLE_RATE": SR, "PB_MEL_BANDS": N_MELS, "PB_MEL_HOP": HOP,
        "PB_MEL_FRAMES": FRAMES, "PB_MEL_FMIN": FMIN, "PB_MEL_FMAX": FMAX,
        "PB_MEL_DB_MIN": DB_MIN, "PB_MEL_DB_MAX": DB_MAX,
    }
    drift = [f"  {k}: mel.h {find(k)} != this script {v}"
             for k, v in expected.items()
             if find(k) is not None and find(k) != v]
    # the FFT size lives in a separate header
    fftm = re.search(r"#define\s+PB_FFT_SIZE\s+(\d+)",
                     open(os.path.join(ROOT, "firmware", "src", "dsp",
                                       "fft.h"), encoding="utf-8").read())
    if fftm and int(fftm.group(1)) != N_FFT:
        drift.append(f"  PB_FFT_SIZE: fft.h {fftm.group(1)} != this script "
                     f"{N_FFT}")

    if drift:
        sys.exit("!! the device constants do not match this script - the "
                 "training set would be invalid:\n" + "\n".join(drift))


# == 2. The mel window - the exact counterpart of pb_mel_window() =========
_FB = None
_HANN = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / N_FFT)


def filter_bank():
    global _FB
    if _FB is None:
        _FB = mel_filterbank()      # mel_reference.py - the ONE source
    return _FB


def power_spectrum_batch(frames):
    """The batched form of mel_reference.power_spectrum. (F, 512) -> (F, 257)

    Calling it one frame at a time is far too slow for 187 frames x 58,000
    slices. That it gives the same result is measured by `--checksum`; if the
    two drift apart, that check fails.
    """
    x = frames.astype(np.float64)
    x = (x - x.mean(axis=1, keepdims=True)) / 32768.0
    p = np.abs(np.fft.rfft(x * _HANN, axis=1)) ** 2
    gain = 0.5 * N_FFT
    p *= 4.0 / (gain * gain)
    p[:, 0] *= 0.5              # DC and Nyquist have no negative-frequency twin
    p[:, -1] *= 0.5
    return p


def mel_window(samples):
    """(>=71936,) int16  ->  (187, 64) int8, plus the raw dB standard
    deviation.

    The order on the device (firmware/src/dsp/mel.c):
      1. each frame -> power -> mel -> dB -> int8   (pb_mel_frame)
      2. the int8s are decoded BACK to dB           (pb_mel_q_to_db)
      3. the mean/std inside the window; +-4 sigma is spread over the whole
         int8 range
    Step 2 matters: skip the intermediate rounding and the output drifts away
    from the device's.
    """
    x = samples[:WINDOW_SAMPLES]
    frames = np.lib.stride_tricks.sliding_window_view(x, N_FFT)[::HOP]
    energy = power_spectrum_batch(frames) @ filter_bank().T
    q = quantize_db(10.0 * np.log10(energy + 1e-10))            # (187, 64)

    db = DB_MIN + (q.astype(np.float64) + 128.0) / 255.0 * (DB_MAX - DB_MIN)
    mean = db.mean()
    var = max(db.var(), 1e-6)
    std = np.sqrt(var)
    z = np.rint((db - mean) * (127.0 / (4.0 * std)))
    return np.clip(z, -128, 127).astype(np.int8), float(std)


# == 3. Checksum - compare against the C code ============================
def dsp_test_path():
    for candidate in ("firmware/test/build/dsp_test.exe",
                      "firmware/test/build/dsp_test"):
        y = os.path.join(ROOT, candidate)
        if os.path.exists(y):
            return y
    return None


def checksum():
    """Two independent comparisons. Do not build a training set until both
    pass."""
    verify_constants()
    print("1) The constants agree with mel.h/fft.h.\n")

    # --- find a real slice (a synthetic one if there is none) ---
    sample = None
    seg = csv_compat.resolve(os.path.join(DATA, "segments.csv"))
    if os.path.exists(seg):
        with open(seg, encoding="utf-8") as f:
            for r in csv_compat.reader(f):
                if float(r["target_confidence"]) >= 0.9:
                    y = os.path.join(DATA, "wav", r["ebird_code"], r["file"] + ".wav")
                    if os.path.exists(y):
                        s = read_wav(y)
                        b = int(round(float(r["start"]) * SR))
                        if len(s) >= b + WINDOW_SAMPLES:
                            sample = s[b:b + WINDOW_SAMPLES]
                            print(f"input: {r['ebird_code']}/{r['file']} "
                                  f"@{r['start']}s "
                                  f"(confidence {r['target_confidence']})")
                            break
    if sample is None:
        t = np.arange(WINDOW_SAMPLES) / SR
        v = (0.4 * np.sin(2 * np.pi * 1000 * t) + 0.25 * np.sin(2 * np.pi * 4300 * t)
             + 0.1 * ((np.arange(WINDOW_SAMPLES) % 97) / 97.0 - 0.5))
        sample = np.rint(v * 32767).astype(np.int16)
        print("input: synthetic (no real slice was found)")

    # --- (a) the batched path == mel_reference.power_spectrum ---
    frames = np.lib.stride_tricks.sliding_window_view(
        sample[:WINDOW_SAMPLES], N_FFT)[::HOP]
    batch = power_spectrum_batch(frames)
    one_by_one = np.array([power_spectrum(k) for k in frames])
    difference = np.abs(batch - one_by_one).max()
    print("\n2) The batched power spectrum vs "
          "mel_reference.power_spectrum")
    print(f"   largest absolute difference: {difference:.3e}   "
          "(expected ~0)")
    if difference > 1e-12:
        print("   FAILED: the accelerated path has drifted from the "
              "reference.")
        return 1

    # --- (b) the Python window == the C window ---
    exe = dsp_test_path()
    if exe is None:
        print("\n3) dsp_test was not found, the C comparison was SKIPPED. "
              "First:\n"
              "   cmake -S firmware/test -B firmware/test/build -G Ninja && "
              "cmake --build firmware/test/build")
        return 1

    py, _ = mel_window(sample)
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "slice.s16")
        sample[:WINDOW_SAMPLES].astype("<i2").tofile(raw)
        output = subprocess.run([exe, "--window", raw], capture_output=True,
                               text=True, check=True).stdout

    c = np.zeros((FRAMES, N_MELS), dtype=np.int16)
    for r in csv_compat.reader(output.splitlines()):
        c[int(r["frame"]), int(r["band"])] = int(r["q"])

    d = np.abs(c.astype(int) - py.astype(int))
    identical = (d == 0).mean()
    print("\n3) The Python window vs the C window (dsp_test --window)")
    print(f"   cells that match exactly : {identical * 100:.2f}%")
    print(f"   largest difference       : {d.max()} int8 steps")
    print(f"   mean absolute            : {d.mean():.4f}")
    print(f"   value range              : C [{c.min()}, {c.max()}]  "
          f"Python [{py.min()}, {py.max()}]")

    # A 1-step difference is EXPECTED: the C accumulates the window's
    # mean/variance in float32 in a single pass (mel.c). The sum of squares
    # of 11,968 values exceeds float32's exact-integer range, and there is
    # cancellation in the E[x^2]-E[x]^2 difference too; the scale shifts by
    # about 1e-4 relative and cells on a rounding boundary move by one step.
    # Over int8's +-4 sigma range, one step is 0.03 sigma - immaterial. The
    # Python side, in float64, is the more CORRECT one; there is no need to
    # change the C.
    if d.max() > 1:
        print("\n   FAILED: there is a difference larger than one step, so "
              "rounding is not the cause.")
        return 1
    if d.mean() > 0.05:
        print("\n   FAILED: one-step differences are too widespread (>5%).")
        return 1
    print("\nPASSED: the training set matches the features the device "
          "sees.")
    return 0


# == 3b. Verify the dataset that was produced ============================
def _row_audio(s):
    """A samples.csv row -> the path of its source WAV."""
    if s["ebird_code"] == NEGATIVE_CODE:
        record = csv_compat.resolve(os.path.join(NEGATIVE_DIR,
                                                 "esc50_records.csv"))
        with open(record, encoding="utf-8") as f:
            for r in csv_compat.reader(f):
                if os.path.basename(r["file"]) == s["file"]:
                    return os.path.join(ROOT, r["file"])
        return None
    return os.path.join(DATA, "wav", s["ebird_code"], s["file"] + ".wav")


def output_verify(out, n):
    """A row-alignment check: is the window in the array really the audio of
    THAT row?

    Because silent windows are skipped and the array is trimmed afterwards,
    the `index` column and the array row could drift apart. If they do, every
    sample carries the wrong species label and NOTHING reports an error - the
    model simply fails to learn. Hence a permanent mode.
    """
    X = np.load(csv_compat.resolve(os.path.join(out, "windows.npy")), mmap_mode="r")
    y = np.load(csv_compat.resolve(os.path.join(out, "labels.npy")))
    with open(csv_compat.resolve(os.path.join(out, "samples.csv")), encoding="utf-8") as f:
        rows = list(csv_compat.reader(f))

    if len(rows) != len(X) or len(y) != len(X):
        print(f"FAILED: the lengths do not match - csv {len(rows)}, "
              f"windows {len(X)}, labels {len(y)}")
        return 1

    # --- (1) LEAKAGE: a recording must not be in more than one split ---
    #
    # This check once caught a real bug: the rule that MOVED contaminated
    # slices out of validation and INTO TRAINING had put 290 recordings into
    # two splits at once. That is, code written to prevent leakage produced
    # leakage. A cheap check; keep it permanently.
    record_split = defaultdict(set)
    person_split = defaultdict(set)
    for s in rows:
        if s["ebird_code"] == NEGATIVE_CODE:
            continue
        record_split[(s["ebird_code"], s["file"])].add(s["split"])
        if s["recordist"]:
            person_split[(s["ebird_code"], s["recordist"])].add(s["split"])
    split_records = [k for k, v in record_split.items() if len(v) > 1]
    split_people = [k for k, v in person_split.items() if len(v) > 1]
    print(f"leakage / recordings in more than one split : "
          f"{len(split_records)}  (must be 0)")
    print(f"leakage / recordists in more than one split : "
          f"{len(split_people)}  (0 apart from species that cannot be split "
          "by recordist)")
    if split_records:
        for k in split_records[:5]:
            print(f"   {k[0]}/{k[1]}: {sorted(record_split[k])}")
        print("FAILED: slices of the same recording are in more than one "
              "split - the accuracy would rise falsely.")
        return 1

    # --- (2) row alignment ---
    rng = np.random.default_rng(0)
    selection = rng.choice(len(rows), size=min(n, len(rows)), replace=False)
    identical = checked = 0
    for i in sorted(selection.tolist()):
        s = rows[i]
        if int(s["index"]) != i:
            print(f"FAILED: row {i} has {s['index']} in its index column")
            return 1
        path = _row_audio(s)
        if not path or not os.path.exists(path):
            continue
        start = int(s["window_samples"])
        p, _ = mel_window(read_wav(path)[start:start + WINDOW_SAMPLES])
        checked += 1
        if np.array_equal(p, X[i]):
            identical += 1
        else:
            d = np.abs(p.astype(int) - X[i].astype(int))
            print(f"!! row {i} ({s['ebird_code']}/{s['file']}@{s['start']}) "
                  f"did not match - largest difference {d.max()}")

    print(f"{checked} rows re-extracted from their source / "
          f"identical: {identical}")
    if checked == 0:
        print("FAILED: no row could be verified (are the source files "
              "missing?)")
        return 1
    if identical != checked:
        print("FAILED: the array and the csv rows are not aligned.")
        return 1
    print("PASSED: every row's window is reproduced exactly from its own "
          "source.")
    return 0


def listen(out, n):
    """Write the source audio of N rows out as WAVs - to check by ear.

    Indirect measurement has misled this project twice. In segmentation the
    only direct observation was listening, and it is the same for the
    training set: the file name carries the species, the split and the
    confidence, so listen and compare against the label.
    """
    with open(csv_compat.resolve(os.path.join(out, "samples.csv")), encoding="utf-8") as f:
        rows = list(csv_compat.reader(f))
    d = os.path.join(out, "sample_audio")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(1)
    for i in rng.choice(len(rows), size=min(n, len(rows)), replace=False):
        s = rows[int(i)]
        path = _row_audio(s)
        if not path or not os.path.exists(path):
            continue
        start = int(s["window_samples"])
        audio = read_wav(path)[start:start + WINDOW_SAMPLES]
        name = (f"{s['index']}_{s['ebird_code']}_{s['split']}"
              f"{'_CONTAMINATED' if s['contaminated'] == '1' else ''}"
              f"_{s['file']}_{s['start']}s.wav")
        with wave.open(os.path.join(d, name), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(audio.tobytes())
        print(f"  {name}   target_confidence {s['target_confidence']}  "
              f"best {s['best_species']}")
    print(f"\n-> {d}   (listen, and compare against the label)")
    return 0


# == 4. Helpers ==========================================================
def read_wav(path):
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (SR, 1, 2):
            raise ValueError(f"{path}: {w.getframerate()} Hz "
                             f"{w.getnchannels()} channels "
                             f"{w.getsampwidth() * 8} bit "
                             f"- expected {SR} Hz mono 16-bit")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def wav_length(path):
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes()
    except Exception:
        return None


def species_map():
    path = os.path.join(DATA, "birdnet_name_map.csv")
    if not os.path.exists(path):
        sys.exit(f"the name map is missing: {path} - run "
                 "tools/birdnet_slist.py first")
    with open(path, encoding="utf-8") as f:
        r = list(csv_compat.reader(f))
    return ({x["ebird_code"]: x["birdnet_scientific_name"] for x in r},
            {x["ebird_code"]: x["english_name"] for x in r},
            {x["ebird_code"]: x["our_scientific_name"] for x in r})


def recordists():
    """file name (the XC id) -> recordist. To group the split by person."""
    path = csv_compat.resolve(os.path.join(DATA, "xc", "records.csv"))
    if not os.path.exists(path):
        print(f"!! {path} is missing - the split will be done PER RECORDING "
              "only (not per person)")
        return {}
    with open(path, encoding="utf-8") as f:
        return {os.path.splitext(os.path.basename(r["file"]))[0]: r["recordist"]
                for r in csv_compat.reader(f)}


def read_teacher(path, cls_index, name_cls):
    """A raw BirdNET result -> {(start, end): {class index: confidence}}"""
    d = defaultdict(dict)
    if not os.path.exists(path):
        return d
    with open(path, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            i = name_cls.get(r["Scientific name"])
            if i is not None:
                key = (round(float(r["Start (s)"]), 1), round(float(r["End (s)"]), 1))
                d[key][i] = float(r["Confidence"])
    return d


def assign_splits(groups, val_share, test_share):
    """Distribute groups (recording/person) over the splits - NOT per slice.

    It starts from the largest group and gives each one to whichever split is
    furthest behind its target at that moment. Deterministic: no seed is
    needed, the same input produces the same split.

    WHY NOT "fill in order": the first attempt shuffled the groups and filled
    the test quota first. Measured - 399 of the Eurasian Blackbird's 573
    slices belong to a SINGLE recordist; when that group landed in test the
    split came out 11%/21%/68%. Largest-first plus close-the-biggest-gap puts
    that same group into training and the ratios hold. It solves the problem
    without breaking the grouping by recordist.
    """
    total = sum(len(v) for v in groups.values())
    target = {"train": total * (1.0 - val_share - test_share),
              "val": total * val_share,
              "test": total * test_share}
    so_far = dict.fromkeys(target, 0)
    assignment = {}
    for key, slices in sorted(groups.items(),
                              key=lambda kv: (-len(kv[1]), str(kv[0]))):
        b = max(target, key=lambda k: target[k] - so_far[k])
        assignment[key] = b
        so_far[b] += len(slices)
    return assignment


def split_species(person_groups, file_groups, val_share, test_share):
    """Split one species. By recordist first; per recording if that fails.

    Grouping by recordist is the correct thing (same person = same
    equipment, location, background). But if most of a species' recordings
    belong to one person, its validation or test split can come out empty -
    and then the species cannot be measured at all. For such species it falls
    back to PER RECORDING (the essential condition - slices of one recording
    always in the same split - is preserved either way) and the report says
    which species that happened to.
    """
    assignment = assign_splits(person_groups, val_share, test_share)
    count = defaultdict(int)
    for key, idx in person_groups.items():
        count[assignment[key]] += len(idx)
    if count["val"] and count["test"]:
        return person_groups, assignment, False
    if len(file_groups) < 3:
        return person_groups, assignment, False   # nothing left to split
    return (file_groups,
            assign_splits(file_groups, val_share, test_share), True)


# == 5. Main flow ========================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checksum", action="store_true",
                    help="measure the match against the C code and exit "
                         "(run this first)")
    ap.add_argument("--threshold", type=float, default=0.25,
                    help="the lower bound on target_confidence (at 0.25 that "
                         "is 327 slices per species)")
    ap.add_argument("--val", type=float, default=0.15)
    ap.add_argument("--test", type=float, default=0.10)
    ap.add_argument("--contaminated", choices=("train", "drop"),
                    default="train",
                    help="slices where best_species != target: keep them in "
                         "training with a flag (default) or drop them "
                         "entirely")
    ap.add_argument("--negative", default=NEGATIVE_DIR)
    ap.add_argument("--negative-window", type=int, default=2,
                    help="how many windows per ESC-50 clip (a clip is 5 s)")
    ap.add_argument("--negative-bird-threshold", type=float, default=0.25,
                    help="if BirdNET heard a bird at this confidence, the "
                         "clip is thrown out of the negatives")
    ap.add_argument("--out", default=csv_compat.resolve(
        os.path.join(DATA, "dataset")))
    ap.add_argument("--verify-output", type=int, metavar="N",
                    help="pick N rows out of the produced set, re-extract "
                         "them from the source and compare against the array "
                         "(a row-alignment check)")
    ap.add_argument("--listen", type=int, metavar="N",
                    help="write the source audio of N rows out as WAVs - to "
                         "check by ear")
    ap.add_argument("--species", nargs="*",
                    help="only these eBird codes (for experiments)")
    ap.add_argument("--limit", type=int,
                    help="at most this many slices (a smoke test)")
    a = ap.parse_args()

    if a.checksum:
        return checksum()
    if a.verify_output:
        return output_verify(a.out, a.verify_output)
    if a.listen:
        return listen(a.out, a.listen)

    verify_constants()
    birdnet_name, english, _ = species_map()
    codes = sorted(birdnet_name)
    cls_index = {k: i for i, k in enumerate(codes)}
    name_cls = {birdnet_name[k]: cls_index[k] for k in codes}
    negative_index = len(codes)
    person = recordists()

    # ---- 5.1 the slice list ----
    seg = csv_compat.resolve(os.path.join(DATA, "segments.csv"))
    if not os.path.exists(seg):
        sys.exit(f"{seg} is missing - run tools/birdnet_summary.py first")

    length = {}
    # (code, file, start sample, start s, confidence, contaminated, best..)
    candidate = []
    below_threshold = missing_wav = too_short = 0
    with open(seg, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            code = r["ebird_code"]
            if a.species and code not in a.species:
                continue
            if code not in cls_index:
                continue
            if float(r["target_confidence"]) < a.threshold:
                below_threshold += 1
                continue
            path = os.path.join(DATA, "wav", code, r["file"] + ".wav")
            if path not in length:
                length[path] = wav_length(path)
            n = length[path]
            if n is None:
                missing_wav += 1
                continue
            if n < WINDOW_SAMPLES:
                too_short += 1
                continue
            # If the end of the slice runs past the end of the file, shift the
            # window LEFT: the slice's audio still falls inside the window and
            # no data is lost. This was preferred over zero padding - a band
            # of zeros becomes an artificial -90 dB block in the mel and
            # breaks the window normalisation.
            start = int(round(float(r["start"]) * SR))
            start = max(0, min(start, n - WINDOW_SAMPLES))
            contaminated = int(r["best_species"] != birdnet_name[code])
            if contaminated and a.contaminated == "drop":
                continue
            candidate.append((code, r["file"], start, float(r["start"]),
                         float(r["target_confidence"]), contaminated,
                         r["best_species"], float(r["best_confidence"] or 0)))
            if a.limit and len(candidate) >= a.limit:
                break

    print(f"threshold {a.threshold}: {len(candidate)} slices  "
          f"(below threshold {below_threshold}, no wav {missing_wav}, "
          f"file too short {too_short})")
    if not candidate:
        sys.exit("no slices are left")

    # ---- 5.2 the split: within a species, PER RECORDING/PERSON ----
    person_gr = defaultdict(lambda: defaultdict(list))
    file_gr = defaultdict(lambda: defaultdict(list))
    for i, (code, file, *_r) in enumerate(candidate):
        person_gr[code][person.get(file) or f"__file__{file}"].append(i)
        file_gr[code][file].append(i)

    split_of = {}
    fell_back_to_records = []
    for code in person_gr:
        g, assignment, fell_back = split_species(person_gr[code],
                                                 file_gr[code], a.val, a.test)
        if fell_back:
            fell_back_to_records.append(code)
        for key, idx in g.items():
            for i in idx:
                # A contaminated slice does not enter the measurement:
                # validation and test have to be clean.
                #
                # !! IMPORTANT: DO NOT MOVE these slices INTO TRAINING, DROP
                # them. The first version moved them, and it was measured:
                # 290 recordings came out in both training and validation -
                # exactly the leakage the code was written to prevent. If even
                # one slice of a recording crosses over, the split is broken.
                split_of[i] = (None if (candidate[i][5]
                                        and assignment[key] != "train")
                               else assignment[key])

    # ---- 5.3 the negatives ----
    neg = negative_list(a, name_cls)
    total = len(candidate) + len(neg)
    print(f"negative: {len(neg)} windows")
    print(f"total   : {total} windows  "
          f"({total * FRAMES * N_MELS / 1e6:.0f} MB)")

    # ---- 5.4 extraction ----
    os.makedirs(a.out, exist_ok=True)
    X = np.lib.format.open_memmap(os.path.join(a.out, "windows.npy"), mode="w+",
                                  dtype=np.int8, shape=(total, FRAMES, N_MELS))
    y = np.zeros(total, dtype=np.int16)
    T = np.zeros((total, len(codes)), dtype=np.float16)
    rows = []
    silent = 0
    contaminated_dropped = 0
    write = 0

    by_file = defaultdict(list)
    for i, s in enumerate(candidate):
        by_file[(s[0], s[1])].append(i)

    for counter, ((code, file), idx) in enumerate(sorted(by_file.items()), 1):
        try:
            audio = read_wav(os.path.join(DATA, "wav", code, file + ".wav"))
        except Exception as e:
            print(f"!! could not read {code}/{file}: {e}")
            continue
        teacher = read_teacher(
            os.path.join(csv_compat.resolve(
                os.path.join(DATA, "birdnet_result")), code,
                file + ".BirdNET.results.csv"),
            cls_index, name_cls)
        for i in idx:
            # a contaminated slice that fell into the measurement set
            if split_of[i] is None:
                contaminated_dropped += 1
                continue
            (_, _, start, start_s, confidence, contaminated, best,
             best_conf) = candidate[i]
            p, std = mel_window(audio[start:start + WINDOW_SAMPLES])
            if std < SILENT_STD_DB:
                silent += 1
                continue
            X[write] = p
            y[write] = cls_index[code]
            for j, c in teacher.get(
                    (round(start_s, 1), round(start_s + 3.0, 1)), {}).items():
                T[write, j] = c
            rows.append({
                "index": write, "class_index": cls_index[code],
                "ebird_code": code,
                "english_name": english[code], "file": file,
                "start": f"{start_s:.1f}", "window_samples": start,
                "split": split_of[i], "contaminated": contaminated,
                "target_confidence": f"{confidence:.4f}",
                "best_species": best,
                "best_confidence": f"{best_conf:.4f}",
                "recordist": person.get(file, ""),
            })
            write += 1
        if counter % 500 == 0:
            print(f"  {counter}/{len(by_file)} recordings / {write} windows",
                  flush=True)

    # the negatives
    for path, offset, split, category in neg:
        try:
            audio = read_wav(path)
        except Exception as e:
            print(f"!! could not read the negative {path}: {e}")
            continue
        if len(audio) < offset + WINDOW_SAMPLES:
            continue
        p, std = mel_window(audio[offset:offset + WINDOW_SAMPLES])
        if std < SILENT_STD_DB:
            silent += 1
            continue
        X[write] = p
        y[write] = negative_index
        rows.append({
            "index": write, "class_index": negative_index,
            "ebird_code": NEGATIVE_CODE,
            "english_name": category, "file": os.path.basename(path),
            "start": f"{offset / SR:.1f}", "window_samples": offset,
            "split": split, "contaminated": 0, "target_confidence": "",
            "best_species": "", "best_confidence": "", "recordist": "ESC-50",
        })
        write += 1

    # Trim to the real row count (silent windows were dropped).
    X.flush()
    del X
    if write != total:
        old = np.load(csv_compat.resolve(os.path.join(a.out, "windows.npy")), mmap_mode="r")
        fresh = np.lib.format.open_memmap(
            os.path.join(a.out, "windows.tmp.npy"), mode="w+",
            dtype=np.int8, shape=(write, FRAMES, N_MELS))
        for i in range(0, write, 4096):
            last = min(i + 4096, write)
            fresh[i:last] = old[i:last]
        fresh.flush()
        del fresh, old
        os.replace(os.path.join(a.out, "windows.tmp.npy"),
                   os.path.join(a.out, "windows.npy"))
    np.save(os.path.join(a.out, "labels.npy"), y[:write])
    np.save(os.path.join(a.out, "teacher.npy"), T[:write])

    with open(csv_compat.resolve(os.path.join(a.out, "samples.csv")), "w", encoding="utf-8",
              newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    with open(csv_compat.resolve(os.path.join(a.out, "classes.csv")), "w", encoding="utf-8",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["class_index", "ebird_code", "english_name",
                    "birdnet_scientific_name"])
        for k in codes:
            w.writerow([cls_index[k], k, english[k], birdnet_name[k]])
        w.writerow([negative_index, NEGATIVE_CODE, "unknown / not a bird", ""])

    report(a, rows, codes, english, negative_index, silent, write,
           fell_back_to_records, contaminated_dropped)
    return 0


def negative_list(a, name_cls):
    """A list of (path, offset, split, category) over the ESC-50 clips.

    The split follows ESC-50's own folds: fold 5 is test, fold 4 is
    validation, 1-3 are training. The folds are already separated so that
    clips from the same Freesound recording stay in the same fold - which is
    the right thing against recording leakage.
    """
    record = csv_compat.resolve(os.path.join(a.negative,
                                             "esc50_records.csv"))
    if not os.path.exists(record):
        print(f"\n!! THE NEGATIVE CLASS IS EMPTY: {record} is missing.\n"
              f"   First:  python tools/esc50_download.py\n"
              f"   Without a negative class the model assigns every sound to "
              f"some bird. Continuing anyway.\n")
        return []

    # Did BirdNET hear a bird in the negatives? (optional but very valuable)
    dirty = {}
    result_root = csv_compat.resolve(os.path.join(a.negative,
                                                  "birdnet_result"))
    if os.path.isdir(result_root):
        for root, _, files in os.walk(result_root):
            for d in files:
                if not d.endswith(".BirdNET.results.csv"):
                    continue
                highest = 0.0
                with open(os.path.join(root, d), encoding="utf-8") as f:
                    for r in csv_compat.reader(f):
                        if r["Scientific name"] in name_cls:
                            highest = max(highest, float(r["Confidence"]))
                dirty[d.replace(".BirdNET.results.csv", ".wav")] = highest
        print(f"negative BirdNET scan: {len(dirty)} clips read")
    else:
        print(f"!! {result_root} is missing - the negatives were NOT SCANNED "
              "for bird content.\n"
              "   Suggested:  .venv-birdnet\\Scripts\\python "
              "tools/birdnet_run.py "
              "--inp data/negative/wav --out data/negative/birdnet_result")

    out, filtered = [], 0
    with open(record, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            name = os.path.basename(r["file"])
            if dirty.get(name, 0.0) >= a.negative_bird_threshold:
                filtered += 1
                continue
            fold = r.get("fold", "")
            split = {"5": "test", "4": "val"}.get(fold, "train")
            path = os.path.join(ROOT, r["file"])
            clip = wav_length(path) or 0
            if clip < WINDOW_SAMPLES:
                continue
            n = max(1, a.negative_window)
            for k in range(n):
                offset = (0 if n == 1 else
                          int(round(k * (clip - WINDOW_SAMPLES) / (n - 1))))
                out.append((path, offset, split, r["category"]))
    if filtered:
        print(f"negative: {filtered} clips with a bird in them were filtered "
              f"out (threshold {a.negative_bird_threshold})")
    return out


def report(a, rows, codes, english, negative_index, silent, total,
           fell_back_to_records, contaminated_dropped):
    split_count = defaultdict(int)
    species_split = defaultdict(lambda: defaultdict(int))
    contaminated_count = defaultdict(int)
    for s in rows:
        split_count[s["split"]] += 1
        species_split[s["ebird_code"]][s["split"]] += 1
        if s["contaminated"]:
            contaminated_count[s["ebird_code"]] += 1

    present = [k for k in codes if species_split[k]]   # species in this run
    absent = [k for k in codes if not species_split[k]]

    row = []
    row.append(f"threshold {a.threshold} / contaminated={a.contaminated} / "
               "the split is deterministic")
    row.append(f"total windows : {total}")
    for b in ("train", "val", "test"):
        row.append(f"  {b:10s} {split_count[b]:7d}  "
                   f"{100 * split_count[b] / max(total, 1):.1f}%")
    row.append(f"species                        : {len(present)}")
    row.append(f"windows dropped as silent      : {silent}")
    row.append(f"contaminated (training only)   : "
               f"{sum(contaminated_count.values())}")
    row.append(f"dropped for being contaminated : {contaminated_dropped}  "
               "(they had fallen into the validation/test group)")
    nb = species_split[NEGATIVE_CODE]
    row.append(f"negative (class {negative_index})            : "
               f"{sum(nb.values())}  (train {nb['train']}, "
               f"val {nb['val']}, test {nb['test']})")

    if absent:
        row.append(f"\n!! SPECIES WITH NO WINDOWS AT ALL: {len(absent)}")
        row.append("  " + ", ".join(absent[:20]))
    missing = [k for k in present if not species_split[k]["val"]]
    if missing:
        row.append(f"\n!! species with an EMPTY validation set: "
                   f"{len(missing)}")
        row.append("  " + ", ".join(missing))
    if fell_back_to_records:
        row.append(f"\nspecies that could not be split by recordist: "
                   f"{len(fell_back_to_records)} (fell back to per recording; "
                   "slices of one recording are still in the same split)")
        row.append("  " + ", ".join(sorted(fell_back_to_records)))

    weak = sorted((sum(species_split[k].values()), k) for k in present)
    row.append("\nthe 12 species with the fewest windows (the focal loss and "
               "the augmentation should target these):")
    for n, k in weak[:12]:
        b = species_split[k]
        row.append(f"  {k:10s} {english[k]:26s} {n:5d}  "
                   f"(train {b['train']}, val {b['val']}, test {b['test']}, "
                   f"contaminated {contaminated_count[k]})")
    count = [sum(species_split[k].values()) for k in present]
    row.append(f"\nwindows per species: min {min(count)} / median "
               f"{int(np.median(count))} / max {max(count)}")

    text = "\n".join(row)
    print("\n" + text)
    with open(csv_compat.resolve(os.path.join(a.out, "summary.txt")), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(f"\n-> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
