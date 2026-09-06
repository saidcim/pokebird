#!/usr/bin/env python3
"""
birdnet_run.py — split every recording under data/wav into 3-second slices
with BirdNET and write the species scores.

    .venv-birdnet\\Scripts\\python -u tools/birdnet_run.py --workers 10

THIS SCRIPT RUNS WITH THE VENV'S PYTHON. The default Python on this machine
is 3.14 and BirdNET-Analyzer wants 3.11; that is why the separate virtualenv
exists.

--------------------------------------------------------------------------
WHY THERE IS NO --slist
--------------------------------------------------------------------------
The plan was to pass a list of the 178 species. In the installed version the
species-list filter is applied AFTER INFERENCE (analyze/utils.py:689) — so
passing a list buys no speed at all, it only drops rows. Running without one
leaves more information behind in the same time, and the first measurement
run confirmed it:

    0.0-3.0  Engine                   0.2877   <- a non-bird class
    3.0-5.4  Corvus cornix  (target)  0.5201
    3.0-5.4  Corvus corone  (relative) 0.4368  <- a confusion signal

Non-bird classes like `Engine` are directly useful for the stage-1 gate and
for negative mining; the score of a related species helps weed out
contaminated slices. Filtering down to the 178 happens in
tools/birdnet_summary.py.

--------------------------------------------------------------------------
WHY --min_conf 0.1
--------------------------------------------------------------------------
The default is 0.25. Our job is not only "which slice": BirdNET's SOFT scores
become the teacher signal for distillation in the species net. A
low-confidence slice still carries information; raising the threshold during
training is easy, whereas getting discarded data back means repeating a
79-hour analysis.

--------------------------------------------------------------------------
!! BIRDNET'S OWN PROCESS POOL IS NOT USED — IT DEADLOCKS
--------------------------------------------------------------------------
`analyze(threads=14)` opens a multiprocessing.Pool internally. On this
machine (Windows + TensorFlow) it DEADLOCKED after 37 of the 40 files in one
species: 16 processes alive, CPU stuck at 962 s, the remaining 3 files never
processed. Those same three files finished without trouble at threads=1
(21.4 / 0.7 / 10.9 s), so the files are fine — the pool is not.

So the parallelism is built HERE: N independent processes, each handling its
own set of species with threads=1. Since the processes know nothing about
each other there is no shared point left to deadlock on; if one dies the rest
carry on and the reason is visible in its log (a lesson learned the hard way:
background work whose failure is invisible costs time).

--------------------------------------------------------------------------
RESTARTABLE
--------------------------------------------------------------------------
skip_existing_results=True: a file that already has a result is skipped. If
the run is interrupted, the same command picks up where it left off.
"""

import argparse
import os
import subprocess
import sys
import time

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
WAV_DIR = os.path.join(DATA, "wav")
RESULT_DIR = csv_compat.resolve(os.path.join(DATA, "birdnet_result"))
LOG_DIR = os.path.join(DATA, "birdnet_log")


def species_sizes(root):
    """species -> total bytes. Share the work out by audio, not file count."""
    d = {}
    for t in sorted(os.listdir(root)):
        p = os.path.join(root, t)
        if os.path.isdir(p):
            d[t] = sum(
                os.path.getsize(os.path.join(p, f)) for f in os.listdir(p)
            )
    return d


def split_into_sets(sizes, n):
    """Largest first, onto the emptiest set (LPT) - the sets come out even."""
    sets = [[] for _ in range(n)]
    load = [0] * n
    for t, b in sorted(sizes.items(), key=lambda x: -x[1]):
        i = load.index(min(load))
        sets[i].append(t)
        load[i] += b
    return sets, load


def result_count(root, species):
    """Count the result CSVs of the species in scope, and only those.

    Counting everything in the directory misleads: BirdNET also writes an
    analysis-parameters file into each output directory, and results from
    other species are left over from earlier runs. On the first attempt the
    counter read 164 against a target of 120.
    """
    n = 0
    for t in species:
        d = os.path.join(root, t)
        if os.path.isdir(d):
            n += sum(1 for f in os.listdir(d) if f.endswith(".BirdNET.results.csv"))
    return n


def run_worker(species, inp_root, output_root, min_conf, batch):
    from birdnet_analyzer.analyze.core import analyze

    for i, t in enumerate(species, 1):
        g = os.path.join(inp_root, t)
        c = os.path.join(output_root, t)
        os.makedirs(c, exist_ok=True)
        n = len(os.listdir(g))
        t0 = time.time()
        analyze(
            g,
            c,
            min_conf=min_conf,
            overlap=0.0,
            rtype="csv",
            merge_consecutive=1,  # 1 = merging OFF, one row per 3 s
            skip_existing_results=True,
            threads=1,  # no pool, see the note above
            batch_size=batch,
            combine_results=False,
        )
        print(
            f"[{i}/{len(species)}] {t}  {n} files  {time.time() - t0:.0f}s",
            flush=True,
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inp", default=WAV_DIR)
    ap.add_argument("--out", default=RESULT_DIR)
    ap.add_argument("--min-conf", type=float, default=0.1)
    ap.add_argument("--batch", type=int, default=8, help="batch_size")
    ap.add_argument("--workers", type=int, default=10,
                    help="concurrent processes")
    ap.add_argument("--species", nargs="*", help="only these eBird codes")
    ap.add_argument("--sub-process", action="store_true", help="internal use")
    a = ap.parse_args()

    if not os.path.isdir(a.inp):
        sys.exit(f"the input directory is missing: {a.inp}")

    sizes = species_sizes(a.inp)
    if a.species:
        sizes = {t: sizes[t] for t in a.species}

    # --- worker mode: handle the given species in one process, one thread ---
    if a.sub_process or a.workers <= 1:
        run_worker(sorted(sizes), a.inp, a.out, a.min_conf, a.batch)
        return

    # --- main mode: split the work, start N subprocesses, report progress ---
    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    sets, load = split_into_sets(sizes, a.workers)
    target = sum(len(os.listdir(os.path.join(a.inp, t))) for t in sizes)

    print(f"{len(sizes)} species, {target} files, {sum(load) / 2**30:.1f} GB audio")
    print(f"{a.workers} workers; {min(load) / 2**30:.1f}-"
          f"{max(load) / 2**30:.1f} GB per set")

    proc, logs = [], []
    for i, k in enumerate(sets):
        if not k:
            continue
        log = os.path.join(LOG_DIR, f"worker_{i}.log")
        logs.append(log)
        f = open(log, "w", encoding="utf-8")
        proc.append(
            (
                subprocess.Popen(
                    [sys.executable, "-u", os.path.abspath(__file__),
                     "--sub-process", "--inp", a.inp, "--out", a.out,
                     "--min-conf", str(a.min_conf), "--batch", str(a.batch),
                     "--species", *k],
                    stdout=f, stderr=subprocess.STDOUT,
                ),
                f,
            )
        )
    print(f"logs: {LOG_DIR}\\worker_*.log", flush=True)

    t0 = time.time()
    # The rate is computed from the results ADDED IN THIS RUN: while
    # resuming an interrupted job, counting the results that were already
    # there would round the time remaining down to zero.
    at_start = result_count(a.out, sizes)
    while True:
        finished = sum(1 for p, _ in proc if p.poll() is not None)
        n = result_count(a.out, sizes)
        elapsed = time.time() - t0
        rate = (n - at_start) / elapsed if elapsed > 0 else 0
        remaining = (target - n) / rate / 60 if rate > 0 else 0
        print(
            f"{time.strftime('%H:%M:%S')}  {n}/{target} results  "
            f"{rate * 60:.0f} files/min  ~{remaining:.0f} min left  "
            f"workers finished {finished}/{len(proc)}",
            flush=True,
        )
        if finished == len(proc):
            break
        time.sleep(60)

    for p, f in proc:
        f.close()
    code = [p.returncode for p, _ in proc]
    print(
        f"\ndone: {result_count(a.out, sizes)}/{target} results, "
        f"{(time.time() - t0) / 60:.0f} min"
    )
    if any(code):
        print(f"!! non-zero exit codes: {code} - check the logs")


if __name__ == "__main__":
    main()
