#!/usr/bin/env python3
"""
field_compare.py — measure the device's field log against a BirdNET reference.

    python tools/field_compare.py \\
        --session field/20260901_0640_belgrad \\
        --birdnet field/20260901_0640_belgrad/phone.BirdNET.results.csv \\
        --marker-audio 12.4

WHAT IT MEASURES
--------------------------------------------------------------------------
The device and BirdNET listened to the SAME air. There is no ground truth --
BirdNET is wrong sometimes too. So what is measured is not ACCURACY but
AGREEMENT:

  shared        : the device said SPECIES and BirdNET heard the same species
                  within the window
  device_extra  : the device said SPECIES and BirdNET never heard it
                  (a false-alarm candidate)
  birdnet_extra : BirdNET heard it above the threshold and the device never
                  said it (a miss candidate)

The three are printed separately; reducing them to a single percentage would
be wrong. Every row in "device_extra" is a candidate to cut out of the phone
recording for negative mining.

SYNCHRONISATION
--------------------------------------------------------------------------
device.csv runs on the PC's wall clock, BirdNET on seconds from the start of
the recording. The bridge is field_log.py's first MARKER row (the moment you
clapped) and the second that clap falls on in the recording (--marker-audio,
read off in Audacity).

    audio_second = (wall_clock - marker_wall_clock) + marker_audio

If you clapped a second time at the end of the trip, pass --marker2-audio; the
script then measures the phone-to-PC clock drift and corrects it linearly.
Without it the drift is not corrected, only warned about.

MATCHING SPECIES
--------------------------------------------------------------------------
The device prints an eBird code, BirdNET a scientific name. The bridge is
data/birdnet_name_map.csv. BirdNET species that are NOT in our 178 are counted
separately -- those are species the device does not know, so they are OUT OF
SCOPE rather than misses.
"""

import argparse
import csv
import datetime as dt
import os
import sys
from collections import defaultdict

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

# An old Windows console cannot always print the accented letters in a species
# name; print() then raises UnicodeEncodeError. Mark the bad character and
# carry on -- the files are written as UTF-8 either way.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, "data", "birdnet_name_map.csv")


def load_map():
    """BirdNET scientific name -> (eBird code, English name)"""
    if not os.path.exists(MAP):
        sys.exit("the name map is missing: " + MAP)
    d = {}
    with open(MAP, encoding="utf-8") as f:
        for s in csv_compat.reader(f):
            d[s["birdnet_scientific_name"].strip()] = (
                s["ebird_code"].strip(), s["english_name"].strip())
    return d


def load_device(path):
    """device.csv -> (markers[datetime], species events[(dt, code, name, conf)])"""
    markers, events = [], []
    with open(path, encoding="utf-8") as f:
        for s in csv_compat.reader(f):
            t = dt.datetime.fromisoformat(s["wall_clock"])
            if s["mode"] == "MARKER":
                markers.append(t)
            elif s["mode"] == "SPECIES" and s["ebird_code"]:
                events.append((t, s["ebird_code"], s["name"],
                               float(s["confidence_percent"])))
    return markers, events


def load_birdnet(path, threshold, name_map):
    """A BirdNET results.csv -> (known[(start, end, code, name, score)],
    out_of_scope)"""
    known, out_of_scope = [], defaultdict(float)
    with open(path, encoding="utf-8") as f:
        for s in csv_compat.reader(f):
            score = float(s["Confidence"])
            if score < threshold:
                continue
            scientific = s["Scientific name"].strip()
            if scientific in name_map:
                code, name = name_map[scientific]
                known.append((float(s["Start (s)"]), float(s["End (s)"]),
                              code, name, score))
            else:
                out_of_scope[scientific] = max(out_of_scope[scientific], score)
    return known, out_of_scope


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", required=True,
                    help="the directory field_log.py wrote")
    ap.add_argument("--birdnet", required=True,
                    help="the results.csv of the phone recording")
    ap.add_argument("--marker-audio", type=float, required=True,
                    help="the second the FIRST clap falls on in the recording")
    ap.add_argument("--marker2-audio", type=float, default=None,
                    help="the second of the LAST clap (clock-drift correction)")
    ap.add_argument("--threshold", type=float, default=0.25,
                    help="BirdNET confidence threshold (default 0.25, its own)")
    ap.add_argument("--window", type=float, default=8.0,
                    help="the +- agreement window in seconds. 8 = the device's "
                         "3-window vote (PB_DECISION_MIN_WINDOWS) plus the 5 s "
                         "it holds a name on screen (PB_DECISION_HOLD_MS)")
    args = ap.parse_args()

    device_csv = os.path.join(args.session, "device.csv")
    if not os.path.exists(device_csv):
        sys.exit("not found: " + device_csv)

    name_map = load_map()
    markers, events = load_device(device_csv)
    if not markers:
        sys.exit("there is no MARKER in device.csv -- synchronisation is "
                 "impossible.\nThe ENTER-and-clap was missed on the trip; "
                 "this session cannot be\nlined up in time (the logs are "
                 "still kept).")

    t0 = markers[0]
    slope = 1.0
    if args.marker2_audio is not None and len(markers) >= 2:
        pc_span = (markers[-1] - t0).total_seconds()
        audio_span = args.marker2_audio - args.marker_audio
        if pc_span > 60:
            slope = audio_span / pc_span
            drift = (slope - 1.0) * pc_span
            print("  clock drift: {:+.2f} s over {:.0f} min (slope {:.6f}) "
                  "-- corrected".format(drift, pc_span / 60, slope))
        else:
            print("  [!] the two markers are less than 60 s apart, the drift "
                  "cannot be measured; slope 1.0")
    elif len(markers) >= 2:
        print("  [!] there is a second marker but no --marker2-audio; the "
              "drift was NOT CORRECTED")

    def audio_second(t):
        return (t - t0).total_seconds() * slope + args.marker_audio

    known, out_of_scope = load_birdnet(args.birdnet, args.threshold, name_map)

    # --- matching ---------------------------------------------------------
    bn_used = [False] * len(known)
    shared, device_extra = [], []
    for t, code, name, confidence in events:
        ts = audio_second(t)
        matched = None
        for i, (start, end, bcode, bname, score) in enumerate(known):
            if bcode != code:
                continue
            if start - args.window <= ts <= end + args.window:
                matched = i
                break
        if matched is None:
            device_extra.append((ts, code, name, confidence))
        else:
            bn_used[matched] = True
            shared.append((ts, code, name, confidence, known[matched][4]))

    device_said = set(k for _, k, _, _ in events)
    birdnet_extra = [b for i, b in enumerate(known)
                     if not bn_used[i] and b[2] not in device_said]

    # --- report -----------------------------------------------------------
    W = 74
    print()
    print("=" * W)
    print("  FIELD AGREEMENT REPORT   "
          + os.path.basename(os.path.normpath(args.session)))
    print("  BirdNET threshold {}  |  agreement window +-{:.0f} s"
          .format(args.threshold, args.window))
    print("=" * W)
    print("  device SPECIES events              : {}".format(len(events)))
    print("  BirdNET detections (in our 178)    : {}".format(len(known)))
    print("  BirdNET detections (out of scope)  : {} species"
          .format(len(out_of_scope)))
    print("-" * W)
    print("  shared (they agree)                : {}".format(len(shared)))
    print("  device extra (false-alarm candidate): {}".format(len(device_extra)))
    print("  BirdNET extra (miss candidate)     : {}".format(len(birdnet_extra)))
    if events:
        print("\n  {:.1f}% of what the device called was confirmed by BirdNET"
              .format(100.0 * len(shared) / len(events)))

    def block(title, rows):
        print("\n" + "-" * W)
        print("  " + title)
        print("-" * W)
        if not rows:
            print("  (none)")
        for s in rows:
            print(" ", s)

    block("SHARED - the device and BirdNET heard the same species",
          ["{:8.1f}s  {:<9} {:<24} device {:5.1f}%  birdnet {:.2f}"
           .format(ts, code, name, g, b) for ts, code, name, g, b in shared])

    block("DEVICE EXTRA - BirdNET did not confirm it "
          "(a negative-mining candidate)",
          ["{:8.1f}s  {:<9} {:<24} device {:5.1f}%".format(ts, code, name, g)
           for ts, code, name, g in device_extra])

    block("BIRDNET EXTRA - the device never called this species",
          ["{:8.1f}s  {:<9} {:<24} birdnet {:.2f}"
           .format(start, code, name, score)
           for start, end, code, name, score in birdnet_extra])

    if out_of_scope:
        block("OUT OF SCOPE - not in our list of 178 (NOT a miss)",
              ["{:<34} highest {:.2f}".format(name, score)
               for name, score in sorted(out_of_scope.items(),
                                         key=lambda x: -x[1])])

    output = os.path.join(args.session, "agreement.csv")
    with open(output, "w", encoding="utf-8", newline="") as f:
        y = csv.writer(f)
        y.writerow(["bucket", "audio_second", "ebird_code", "name",
                    "device_confidence_percent", "birdnet_score"])
        for ts, code, name, g, b in shared:
            y.writerow(["shared", "{:.1f}".format(ts), code, name, g, b])
        for ts, code, name, g in device_extra:
            y.writerow(["device_extra", "{:.1f}".format(ts), code, name, g, ""])
        for start, end, code, name, score in birdnet_extra:
            y.writerow(["birdnet_extra", "{:.1f}".format(start), code, name,
                        "", score])
    print("\n  wrote: " + output + "\n")


if __name__ == "__main__":
    main()
