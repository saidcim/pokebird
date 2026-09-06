#!/usr/bin/env python3
"""
capture_wav.py — record audio from the device, turn it into a WAV, analyse it.

Connects to the device's USB serial port, sends the 'r' command, collects the
samples between #WAV-BEGIN and #WAV-END, writes a WAV file and prints the
noise floor plus a spectrum summary.

Usage:
    python tools/capture_wav.py --port COM13 --out recording.wav
    python tools/capture_wav.py --port COM13 --cmd n     # measurement only
"""

import argparse
import math
import re
import struct
import sys
import time
import wave

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

BEGIN_RE = re.compile(r"#WAV-BEGIN\s+rate=(\d+)\s+channels=(\d+)\s+bits=(\d+)\s+samples=(\d+)")


def read_until_prompt(ser, timeout=5.0):
    """Read until the device prints its '> ' prompt; return what came in
    between."""
    out, deadline = [], time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(4096).decode("utf-8", errors="replace")
        if chunk:
            out.append(chunk)
            if "".join(out).rstrip().endswith(">"):
                break
        else:
            time.sleep(0.02)
    return "".join(out)


def run_interactive(ser, cmd):
    """A live stream for the interactive diagnostic commands ('d', 'b').

    Those tests need someone to look at the screen and press a key: the
    device walks through its states in turn, you press a key when you see the
    right one, and the device itself reports which state it was in. So rather
    than printing everything at the end, the output is streamed live and the
    keyboard is forwarded to the device.
    """
    try:
        import msvcrt                      # Windows
        def key_pressed():
            return msvcrt.getch() if msvcrt.kbhit() else None
    except ImportError:                    # POSIX
        import select, termios, tty
        tty.setcbreak(sys.stdin.fileno())
        def key_pressed():
            if select.select([sys.stdin], [], [], 0)[0]:
                return sys.stdin.read(1).encode()
            return None

    ser.write(cmd.encode())
    ser.flush()
    if cmd in ("c", "C"):
        # UI commands: space or 'n' changes screen, any other key exits.
        # Swiping works on the touchscreen too; this is the fallback.
        print("Look at the screen. SWIPE HORIZONTALLY ON IT, or change")
        print("screen with space / 'n'. Press any other key to leave.")
    else:
        print("Look at the screen. Press a key when you see what you want.")
    print("Ctrl+C to quit.\n")

    last_data = time.time()
    try:
        while True:
            chunk = ser.read(256).decode("utf-8", errors="replace")
            if chunk:
                sys.stdout.write(chunk)
                sys.stdout.flush()
                last_data = time.time()
            key = key_pressed()
            if key:
                if key in (b"\x03", b"\x04"):
                    raise KeyboardInterrupt
                ser.write(key)
                ser.flush()
            if not chunk:
                # When the test ends the device waits at its prompt; leave
                # after 4 s of silence.
                if time.time() - last_data > 4.0:
                    break
                time.sleep(0.02)
    except KeyboardInterrupt:
        print("\ncancelled")
    print()


def capture(ser, timeout=60.0):
    """Send 'r' and collect the samples. Returns (rate, samples)."""
    ser.write(b"r")
    ser.flush()

    rate, expected, samples = None, 0, []
    buf, deadline = "", time.time() + timeout

    while time.time() < deadline:
        chunk = ser.read(8192).decode("utf-8", errors="replace")
        if not chunk:
            time.sleep(0.01)
            continue
        buf += chunk

        if rate is None:
            m = BEGIN_RE.search(buf)
            if m:
                rate, _ch, _bits, expected = (int(m.group(1)), int(m.group(2)),
                                              int(m.group(3)), int(m.group(4)))
                buf = buf[m.end():]
                print(f"  stream started: {expected} samples @ {rate} Hz")
            else:
                # show the diagnostic output that precedes the header
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    if line.strip():
                        print("  " + line.strip())
                continue

        if "#WAV-END" in buf:
            body, _ = buf.split("#WAV-END", 1)
            samples.extend(int(t) for t in body.split() if t.lstrip("-").isdigit())
            break

        # process whole lines, keep the remainder buffered
        if "\n" in buf:
            body, buf = buf.rsplit("\n", 1)
            samples.extend(int(t) for t in body.split() if t.lstrip("-").isdigit())

    if rate is None:
        raise RuntimeError("the device sent no #WAV-BEGIN. Is that the right "
                           "port?")
    if len(samples) < expected:
        print(f"  [!] got {len(samples)}/{expected} samples (short)")
    return rate, samples


def write_wav(path, rate, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h",
                                  *(max(-32768, min(32767, s)) for s in samples)))


def dft_band_energy(samples, rate, n=2048):
    """
    A simple band-energy analysis - without numpy, so as not to add a
    dependency. Takes a window from the middle of the signal and computes the
    RMS in octave bands.
    """
    if len(samples) < n:
        return []
    start = (len(samples) - n) // 2
    win = samples[start:start + n]
    mean = sum(win) / n
    # Hann window
    x = [(v - mean) * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)))
         for i, v in enumerate(win)]

    bands = [(20, 100), (100, 300), (300, 1000), (1000, 3000),
             (3000, 6000), (6000, 9000), (9000, 12000)]
    out = []
    for lo, hi in bands:
        k_lo = max(1, int(lo * n / rate))
        k_hi = min(n // 2, int(hi * n / rate))
        if k_hi <= k_lo:
            continue
        energy = 0.0
        for k in range(k_lo, k_hi):
            re_ = im = 0.0
            step = 2 * math.pi * k / n
            for i, v in enumerate(x):
                re_ += v * math.cos(step * i)
                im -= v * math.sin(step * i)
            energy += (re_ * re_ + im * im)
        rms = math.sqrt(energy) / n
        dbfs = 20 * math.log10(rms / 32768.0) if rms > 0 else -999
        out.append((lo, hi, dbfs))
    return out


def main():
    ap = argparse.ArgumentParser(description="PokeBird audio capture")
    ap.add_argument("--port", required=True,
                    help="for example COM13 or /dev/ttyACM0")
    ap.add_argument("--out", default="recording.wav")
    ap.add_argument("--cmd", default="r",
                    choices=["r", "n", "e", "i", "l", "d", "s", "b", "v", "o",
                             "t", "u", "m", "a", "c", "C", "F", "x", "X", "k",
                             "K", "w", "y", "z", "j", "L", "S"],
                    help="r=record, n=noise floor, e=EMI sweep, i=info, "
                         "l=live level, d=display test, b=backlight, "
                         "v=QSPI bus diagnostic, s=spectrogram (Ctrl+C to "
                         "quit), m=mel + gate pipeline, a=full demo, "
                         "w=QSPI timing diagnostic (no eyes needed), "
                         "y=hybrid path test (needs eyes, interactive), "
                         "z=row addressing test (needs eyes, interactive), "
                         "j=cursor placement test (needs eyes, interactive), "
                         "x=species-net on-device validation, "
                         "k=real-time recognition, "
                         "c=RESULT SCREEN: recognition card + spectrogram "
                         "(needs eyes)")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="for m/a/c/k: stream this many seconds, then leave "
                         "the device")
    ap.add_argument("--spectrum", action="store_true",
                    help="band-energy analysis (slow, a DFT without numpy)")
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.1) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()

        if args.cmd in ("l", "s"):
            # Live level: the device keeps printing; Ctrl+C leaves.
            ser.write(args.cmd.encode())
            ser.flush()
            print("Live level - clap or talk. Ctrl+C to quit.\n")
            try:
                while True:
                    data = ser.read(256).decode("utf-8", errors="replace")
                    if data:
                        sys.stdout.write(data)
                        sys.stdout.flush()
                    else:
                        time.sleep(0.02)
            except KeyboardInterrupt:
                ser.write(b" ")     # the exit condition of the loop on the device
                print("\n")
            return

        if args.cmd in ("d", "b", "v", "t", "u", "y", "z", "j", "C") or (args.cmd in ("m", "a", "c", "k", "K") and args.duration <= 0):
            # Interactive diagnostic: stream live and forward the keyboard.
            run_interactive(ser, args.cmd)
            return

        if args.cmd in ("m", "a", "c", "k", "K"):
            # Timed stream: print live for --duration, then send the exit
            # key and collect the summary. For verification that needs no
            # eyes.
            ser.write(args.cmd.encode())
            ser.flush()
            deadline = time.time() + args.duration
            while time.time() < deadline:
                data = ser.read(4096).decode("utf-8", errors="replace")
                if data:
                    sys.stdout.write(data)
                    sys.stdout.flush()
                else:
                    time.sleep(0.02)
            ser.write(b"q")
            print(read_until_prompt(ser, timeout=10.0))
            return

        if args.cmd != "r":
            ser.write(args.cmd.encode())
            ser.flush()
            print(read_until_prompt(ser, timeout=20.0))
            return

        print(f"recording over {args.port}...")
        rate, samples = capture(ser)

    if not samples:
        sys.exit("no samples were received.")

    write_wav(args.out, rate, samples)
    dur = len(samples) / rate

    mean = sum(samples) / len(samples)
    rms = math.sqrt(sum((s - mean) ** 2 for s in samples) / len(samples))
    peak = max(abs(s) for s in samples)
    dbfs = 20 * math.log10(rms / 32768.0) if rms > 0 else -999

    print(f"\n  wrote : {args.out}  ({dur:.2f} s, {rate} Hz, "
          f"{len(samples)} samples)")
    print(f"  RMS   : {rms:.1f}  ({dbfs:.1f} dBFS)")
    print(f"  peak  : {peak}   DC offset: {mean:.1f}")

    if peak >= 32700:
        print("  [!] clipping - turn the microphone gain down ('g').")
    if rms < 1.0:
        print("  [!] the signal is next to nothing - the microphone path may "
              "not be open.")

    if args.spectrum:
        print("\n  Band energy:")
        for lo, hi, db in dft_band_energy(samples, rate):
            bar = "#" * max(0, int((db + 100) / 3))
            print(f"    {lo:5d}-{hi:5d} Hz  {db:7.1f} dBFS  {bar}")


if __name__ == "__main__":
    main()
