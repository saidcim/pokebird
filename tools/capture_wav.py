#!/usr/bin/env python3
"""
capture_wav.py — PokeBird M1: cihazdan ses kaydı al, WAV'a çevir, analiz et.

Cihazın USB seri portuna bağlanır, 'r' komutunu gönderir, #WAV-BEGIN /
#WAV-END arasındaki örnekleri toplayıp WAV dosyası yazar ve gürültü tabanı
ile spektrum özetini basar.

Kullanım:
    python tools/capture_wav.py --port COM13 --out kayit.wav
    python tools/capture_wav.py --port COM13 --cmd n     # sadece ölçüm
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
    sys.exit("pyserial gerekli:  pip install pyserial")

BEGIN_RE = re.compile(r"#WAV-BEGIN\s+rate=(\d+)\s+channels=(\d+)\s+bits=(\d+)\s+samples=(\d+)")


def read_until_prompt(ser, timeout=5.0):
    """Cihaz '> ' istemini basana kadar oku; ara çıktıyı döndür."""
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
    """Etkileşimli teşhis komutları ('d', 'b') için canlı akış.

    Bu testler ekrana bakıp bir tuşa basmayı gerektiriyor: cihaz durumları
    sırayla deniyor, siz doğru olanı gördüğünüzde tuşa basıyorsunuz ve cihaz
    hangi durumda olduğunu kendisi yazıyor (bkz. lastsession.md §5.9).
    Bu yüzden çıktıyı sonda toplu basmak yerine canlı akıtıp klavyeyi
    cihaza iletiyoruz.
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
    print("Ekrana bakin. Istenen goruntuyu gordugunuzde bir tusa basin.")
    print("Cikmak icin Ctrl+C.\n")

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
                # Test bitince cihaz istemde bekler; 4 sn sessizlikte cik.
                if time.time() - last_data > 4.0:
                    break
                time.sleep(0.02)
    except KeyboardInterrupt:
        print("\niptal edildi")
    print()


def capture(ser, timeout=60.0):
    """'r' gönder, örnekleri topla. (rate, samples) döndürür."""
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
                print(f"  akis basladi: {expected} ornek @ {rate} Hz")
            else:
                # Basliktan onceki tanisal ciktiyi goster
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    if line.strip():
                        print("  " + line.strip())
                continue

        if "#WAV-END" in buf:
            body, _ = buf.split("#WAV-END", 1)
            samples.extend(int(t) for t in body.split() if t.lstrip("-").isdigit())
            break

        # Tam satirlari isle, kalani tamponda tut
        if "\n" in buf:
            body, buf = buf.rsplit("\n", 1)
            samples.extend(int(t) for t in body.split() if t.lstrip("-").isdigit())

    if rate is None:
        raise RuntimeError("Cihaz #WAV-BEGIN gondermedi. Dogru port mu?")
    if len(samples) < expected:
        print(f"  [!] {len(samples)}/{expected} ornek alindi (eksik)")
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
    Basit bant enerjisi analizi — numpy'siz, bagimlilik eklememek icin.
    Sinyalin ortasindan bir pencere alip oktav bantlarinda RMS hesaplar.
    """
    if len(samples) < n:
        return []
    start = (len(samples) - n) // 2
    win = samples[start:start + n]
    mean = sum(win) / n
    # Hann penceresi
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
    ap = argparse.ArgumentParser(description="PokeBird M1 ses yakalama")
    ap.add_argument("--port", required=True, help="ornek: COM13 veya /dev/ttyACM0")
    ap.add_argument("--out", default="kayit.wav")
    ap.add_argument("--cmd", default="r",
                    choices=["r", "n", "e", "i", "l", "d", "s", "b", "v", "o",
                             "t", "u", "m", "a", "x", "k", "K", "w", "y", "z"],
                    help="r=kayit al, n=gurultu, e=EMI taramasi, i=bilgi, "
                         "l=canli seviye, d=ekran testi, b=arka isik, "
                         "v=QSPI veri yolu teshisi, s=spektrogram (Ctrl+C ile cik), "
                         "m=mel+kapi hatti, a=tam demo, "
                         "w=QSPI zamanlama teshisi (goz gerekmez), "
                         "y=melez yol testi (goz gerekir, etkilesimli), "
                         "z=satir adresleme testi (goz gerekir, etkilesimli), "
                         "x=tur agi cihaz ici dogrulama, k=gercek zamanli tanima")
    ap.add_argument("--sure", type=float, default=0.0,
                    help="m/a icin: bu kadar saniye akit, sonra cihazdan cik")
    ap.add_argument("--spectrum", action="store_true",
                    help="bant enerjisi analizi (yavas, numpy'siz DFT)")
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.1) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()

        if args.cmd in ("l", "s"):
            # Canli seviye: cihaz surekli yaziyor, Ctrl+C ile cikilir.
            ser.write(args.cmd.encode())
            ser.flush()
            print("Canli seviye — el cirpin / konusun. Cikmak icin Ctrl+C.\n")
            try:
                while True:
                    data = ser.read(256).decode("utf-8", errors="replace")
                    if data:
                        sys.stdout.write(data)
                        sys.stdout.flush()
                    else:
                        time.sleep(0.02)
            except KeyboardInterrupt:
                ser.write(b" ")     # cihazdaki dongunun cikis kosulu
                print("\n")
            return

        if args.cmd in ("d", "b", "v", "t", "u", "y", "z") or (args.cmd in ("m", "a", "k", "K") and args.sure <= 0):
            # Etkilesimli teshis: canli akis + klavyeyi cihaza ilet.
            run_interactive(ser, args.cmd)
            return

        if args.cmd in ("m", "a", "k", "K"):
            # Zamanli akis: --sure kadar canli bas, sonra cikis tusu gonder
            # ve ozeti al. Goz gerektirmeyen dogrulama icin.
            ser.write(args.cmd.encode())
            ser.flush()
            bitis = time.time() + args.sure
            while time.time() < bitis:
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

        print(f"{args.port} uzerinden kayit aliniyor...")
        rate, samples = capture(ser)

    if not samples:
        sys.exit("Ornek alinamadi.")

    write_wav(args.out, rate, samples)
    dur = len(samples) / rate

    mean = sum(samples) / len(samples)
    rms = math.sqrt(sum((s - mean) ** 2 for s in samples) / len(samples))
    peak = max(abs(s) for s in samples)
    dbfs = 20 * math.log10(rms / 32768.0) if rms > 0 else -999

    print(f"\n  yazildi : {args.out}  ({dur:.2f} s, {rate} Hz, {len(samples)} ornek)")
    print(f"  RMS     : {rms:.1f}  ({dbfs:.1f} dBFS)")
    print(f"  tepe    : {peak}   DC kaymasi: {mean:.1f}")

    if peak >= 32700:
        print("  [!] Kirpma var — mikrofon kazancini dusurun ('g' komutu).")
    if rms < 1.0:
        print("  [!] Sinyal yok denecek kadar dusuk — mikrofon yolu acilmamis olabilir.")

    if args.spectrum:
        print("\n  Bant enerjisi:")
        for lo, hi, db in dft_band_energy(samples, rate):
            bar = "#" * max(0, int((db + 100) / 3))
            print(f"    {lo:5d}-{hi:5d} Hz  {db:7.1f} dBFS  {bar}")


if __name__ == "__main__":
    main()
