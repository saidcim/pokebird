#!/usr/bin/env python3
"""
field_log.py — field trip: write the device's result screen to a log with a
wall clock beside it.

    python tools/field_log.py --port COM13 --place belgrad --note "06:40, no wind"

WHY THIS SCRIPT EXISTS
--------------------------------------------------------------------------
The device has neither an SD card nor an RTC. Detections live only in RAM and
on the screen. But the result screen (firmware/src/main.c, cmd_result_screen)
already prints every CHANGE of decision to the serial port:

    [123456 ms] SPECIES        grtwoo  Great Spotted Woodpecker  72.3%

The ms there is the time since STARTUP, not the time of day. Lining the log up
with a reference recording (a phone) needs a wall clock, and this script fills
that gap by stamping every line with the PC's clock. Without adding a single
line to the firmware.

SYNCHRONISATION (lining up with the phone recording)
--------------------------------------------------------------------------
The phone is a separate device and its clock is not the PC's. The fix: press
ENTER and CLAP YOUR HANDS. The script writes that moment as a MARKER row;
afterwards you find which second the clap falls on in the recording and hand
that to field_compare.py. Clap once at the start of the trip and once at the
END — the difference between the two also measures the phone's clock drift.

OUTPUT
--------------------------------------------------------------------------
field/<DATE>_<place>/device.log      the raw serial stream, wall-clocked
field/<DATE>_<place>/device.csv      the parsed decisions + MARKER rows
field/<DATE>_<place>/session.txt     port, firmware git revision, your note
"""

import argparse
import csv
import datetime as dt
import os
import re
import subprocess
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

# An old Windows console cannot always print the accented letters in a species
# name; print() then raises UnicodeEncodeError and would cut the trip short IN
# THE FIELD. Mark the bad character and carry on instead - the log files are
# written as UTF-8 either way, so nothing is lost.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Exactly the printf in firmware/src/main.c:
#   printf("  [%lu ms] %-14s %s %s  %%%.1f\n", ms, mode, code, name, conf*100)
# The mode strings come from pb_decision_mode_name() in
# firmware/src/ai/decision.c. "SOUND DETECTED" is the only one with a space in
# it, which is why they are listed explicitly rather than matched with \S+.
MODES = ("SOUND DETECTED", "listening", "maybe", "SPECIES")
ROW = re.compile(
    r"^\s*\[(?P<ms>\d+) ms\]\s+"
    r"(?P<mode>" + "|".join(re.escape(k) for k in MODES) + r"|\?)\s+"
    r"(?P<code>\S+)\s*"
    r"(?P<name>.*?)\s+"
    r"%(?P<confidence>[\d.]+)\s*$"
)


def key_waiting():
    """Was Enter pressed? Check without blocking. msvcrt on Windows, select
    elsewhere."""
    try:
        import msvcrt
        pressed = False
        while msvcrt.kbhit():
            if msvcrt.getwch() in ("\r", "\n"):
                pressed = True
        return pressed
    except ImportError:
        import select
        if select.select([sys.stdin], [], [], 0)[0]:
            sys.stdin.readline()
            return True
        return False


def git_revision():
    try:
        return subprocess.check_output(
            ["git", "-C", ROOT, "describe", "--always", "--dirty"],
            stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return "unknown"


def enter_screen(ser, log_write):
    """Get into the result screen. The firmware can be in either of two
    states:

    - it drops into main()'s command loop and a '> ' prompt appears -> send
      'c'.
    - it goes straight into cmd_result_screen() and the prompt NEVER comes ->
      sending 'c' would LEAVE THE SCREEN (any key other than space or 'n'
      exits, see the comment in main.c). So listen first.

    If '> ' shows up within 1.5 s, there is a prompt."""
    print("[i] probing the firmware (1.5 s)...")
    collected = ""
    last = time.time() + 1.5
    while time.time() < last:
        raw = ser.read(256)
        if raw:
            text = raw.decode("utf-8", errors="replace")
            collected += text
            log_write(text)
    if "> " in collected[-200:] or collected.rstrip().endswith(">"):
        print("[i] the command prompt appeared -> sending 'c'.")
        ser.write(b"c")
        ser.flush()
        return True
    print("[i] no prompt -> assuming the device is already on the result "
          "screen.")
    print("    (if that is wrong: RESET the device and run the script again)")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True,
                    help="for example COM13 or /dev/ttyACM0")
    ap.add_argument("--place", required=True,
                    help="for example belgrad, validebag")
    ap.add_argument("--note", dest="note_text", default="",
                    help="weather, time, who, and so on")
    ap.add_argument("--directory", default=os.path.join(ROOT, "field"))
    args = ap.parse_args()

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M")
    session = os.path.join(args.directory, f"{stamp}_{args.place}")
    os.makedirs(session, exist_ok=True)

    log_path = os.path.join(session, "device.log")
    csv_path = os.path.join(session, "device.csv")

    with open(os.path.join(session, "session.txt"), "w",
              encoding="utf-8") as f:
        f.write(f"start    : {dt.datetime.now().isoformat(timespec='seconds')}\n")
        f.write(f"place    : {args.place}\n")
        f.write(f"port     : {args.port}\n")
        f.write(f"firmware : {git_revision()}\n")
        f.write(f"note     : {args.note_text}\n")

    print(f"\n  session: {session}")
    print("  ENTER = MARKER (CLAP YOUR HANDS as you press) / Ctrl+C = finish\n")

    counts = {"species": 0, "marker": 0}
    line_buffer = ""

    with open(log_path, "w", encoding="utf-8", newline="") as flog, \
         open(csv_path, "w", encoding="utf-8", newline="") as fcsv, \
         serial.Serial(args.port, 115200, timeout=0.1) as ser:

        writer = csv.writer(fcsv)
        writer.writerow(["wall_clock", "device_ms", "mode", "ebird_code",
                         "name", "confidence_percent"])

        def log_write(text):
            flog.write(text)
            flog.flush()

        enter_screen(ser, log_write)

        try:
            while True:
                if key_waiting():
                    now = dt.datetime.now().isoformat(timespec="milliseconds")
                    writer.writerow([now, "", "MARKER", "", "", ""])
                    fcsv.flush()
                    log_write(f"\n### MARKER {now}\n")
                    counts["marker"] += 1
                    print(f"  >>> MARKER {counts['marker']} @ {now}  (CLAP!)")

                raw = ser.read(512)
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace")
                log_write(text)

                line_buffer += text
                while "\n" in line_buffer:
                    row, line_buffer = line_buffer.split("\n", 1)
                    m = ROW.match(row.rstrip("\r"))
                    if not m:
                        continue
                    now = dt.datetime.now().isoformat(timespec="milliseconds")
                    code = m.group("code")
                    name = m.group("name").strip()
                    writer.writerow([now, m.group("ms"), m.group("mode"),
                                     "" if code == "-" else code, name,
                                     m.group("confidence")])
                    fcsv.flush()
                    if m.group("mode") == "SPECIES":
                        counts["species"] += 1
                        print(f"  [{counts['species']:3d}] {name or code}  "
                              f"{m.group('confidence')}%")

        except KeyboardInterrupt:
            print(f"\n\n  done. SPECIES events: {counts['species']}, "
                  f"markers: {counts['marker']}")
            print(f"  {csv_path}")


if __name__ == "__main__":
    main()
