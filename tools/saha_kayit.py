#!/usr/bin/env python3
"""
saha_kayit.py — M8 saha turu: cihazin sonuc ekranini duvar saatiyle gunluge yaz.

    python tools/saha_kayit.py --port COM13 --yer belgrad --not "sabah 06:40, ruzgarsiz"

NEDEN BU BETIK VAR
--------------------------------------------------------------------------
Cihazda SD kart da RTC de YOK (M7 adim 4 ve 5 yapilmadi). Tespitler yalniz
RAM'de ve ekranda. Ama sonuc ekrani (src/main.c, cmd_sonuc_ekrani) her karar
DEGISIKLIGINI zaten seri porta basiyor:

    [123456 ms] TUR            grtwoo  Buyuk Agackakan  %72.3

Buradaki ms ACILISTAN beri gecen sure -- gunun saati degil. Referans kaydiyla
(telefon) hizalamak icin bir duvar saati gerekiyor; bu betik her satiri PC'nin
saatiyle damgalayarak o eksigi kapatiyor. Firmware'e tek satir eklemeden.

ESZAMANLAMA (telefon kaydiyla hizalama)
--------------------------------------------------------------------------
Telefon ayri bir cihaz, saati PC'ninkiyle ayni degil. Cozum: ENTER'a basip
ELINIZI CIRPIN. Betik o ani ISARET satiri olarak yazar; sonra kayitta cirpma
sesinin kacinci saniyede oldugunu bulup saha_karsilastir.py'ye verirsiniz.
Turun basinda ve SONUNDA birer cirpma yapin -- ikisi arasindaki fark telefonun
saat kaymasini da olcer.

CIKTI
--------------------------------------------------------------------------
saha/<TARIH>_<yer>/cihaz.log   ham seri akis (her satir duvar saatli)
saha/<TARIH>_<yer>/cihaz.csv   ayristirilmis kararlar + ISARET satirlari
saha/<TARIH>_<yer>/oturum.txt  port, firmware git surumu, kullanicinin note_text
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
    sys.exit("pyserial gerekli:  pip install pyserial")

# Windows konsolu (cp857/cp1254) Turkce tur adlarindaki bazi karakterleri
# basamayabilir; eski konsolda print() UnicodeEncodeError atar ve SAHADA
# turu yarida keser. Onun yerine bozuk karakteri isaretleyip devam et --
# gunluk dosyalari zaten UTF-8 yaziliyor, kayipsiz.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# src/main.c'deki printf ile birebir:
#   printf("  [%lu ms] %-14s %s %s  %%%.1f\n", ms, kip, kod, ad, guven*100)
# kip icinde bosluk olabilen tek deger "SES ALGILANDI"; o yuzden alternatif
# listesi acikca yaziliyor, \S+ ile yakalanamaz.
KIPLER = ("SES ALGILANDI", "dinliyor", "olabilir", "TUR")
ROW = re.compile(
    r"^\s*\[(?P<ms>\d+) ms\]\s+"
    r"(?P<kip>" + "|".join(re.escape(k) for k in KIPLER) + r"|\?)\s+"
    r"(?P<kod>\S+)\s*"
    r"(?P<ad>.*?)\s+"
    r"%(?P<guven>[\d.]+)\s*$"
)


def bekleyen_tus():
    """Enter'a basildi mi? Bloklamadan bak. Windows'ta msvcrt, digerinde select."""
    try:
        import msvcrt
        basildi = False
        while msvcrt.kbhit():
            if msvcrt.getwch() in ("\r", "\n"):
                basildi = True
        return basildi
    except ImportError:
        import select
        if select.select([sys.stdin], [], [], 0)[0]:
            sys.stdin.readline()
            return True
        return False


def git_surum():
    try:
        return subprocess.check_output(
            ["git", "-C", ROOT, "describe", "--always", "--dirty"],
            stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return "bilinmiyor"


def ekrana_gir(ser, log_write):
    """Sonuc ekranina gir. Firmware iki surumde de olabilir:

    - HEAD: main() komut dongusune duser, '> ' istemi gelir -> 'c' gonder.
    - Calisma agacindaki surum: main() dogrudan cmd_sonuc_ekrani()'ne girer,
      istem HIC gelmez -> 'c' gondermek EKRANDAN CIKARIR (bosluk/n disinda
      her tus cikis, src/main.c'deki yorum). O yuzden once dinliyoruz.

    1,5 sn icinde '> ' gorursek istem var demektir."""
    print("[i] firmware yoklaniyor (1,5 sn)...")
    biriken = ""
    last = time.time() + 1.5
    while time.time() < last:
        raw = ser.read(256)
        if raw:
            text = raw.decode("utf-8", errors="replace")
            biriken += text
            log_write(text)
    if "> " in biriken[-200:] or biriken.rstrip().endswith(">"):
        print("[i] komut istemi goruldu -> 'c' gonderiliyor.")
        ser.write(b"c")
        ser.flush()
        return True
    print("[i] istem yok -> cihaz zaten sonuc ekraninda kabul ediliyor.")
    print("    (yanlissa: cihazi RESET'leyip betigi yeniden calistirin)")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="ornek: COM13 veya /dev/ttyACM0")
    ap.add_argument("--place", required=True, help="ornek: belgrad, validebag")
    ap.add_argument("--note", dest="note_text", default="", help="hava, saat, kim vs.")
    ap.add_argument("--directory", default=os.path.join(ROOT, "saha"))
    args = ap.parse_args()

    damga = dt.datetime.now().strftime("%Y%m%d_%H%M")
    session = os.path.join(args.directory, f"{damga}_{args.place}")
    os.makedirs(session, exist_ok=True)

    log_yolu = os.path.join(session, "cihaz.log")
    csv_yolu = os.path.join(session, "cihaz.csv")

    with open(os.path.join(session, "oturum.txt"), "w", encoding="utf-8") as f:
        f.write(f"baslangic : {dt.datetime.now().isoformat(timespec='seconds')}\n")
        f.write(f"yer       : {args.place}\n")
        f.write(f"port      : {args.port}\n")
        f.write(f"firmware  : {git_surum()}\n")
        f.write(f"not       : {args.note_text}\n")

    print(f"\n  oturum: {session}")
    print("  ENTER = ISARET (basarken ELINIZI CIRPIN) · Ctrl+C = bitir\n")

    record_count = {"tur": 0, "isaret": 0}
    row_tamponu = ""

    with open(log_yolu, "w", encoding="utf-8", newline="") as flog, \
         open(csv_yolu, "w", encoding="utf-8", newline="") as fcsv, \
         serial.Serial(args.port, 115200, timeout=0.1) as ser:

        yazar = csv.writer(fcsv)
        yazar.writerow(["duvar_saati", "cihaz_ms", "kip", "ebird_code",
                        "english_name", "confidence_percent"])

        def log_write(text):
            flog.write(text)
            flog.flush()

        ekrana_gir(ser, log_write)

        try:
            while True:
                if bekleyen_tus():
                    simdi = dt.datetime.now().isoformat(timespec="milliseconds")
                    yazar.writerow([simdi, "", "ISARET", "", "", ""])
                    fcsv.flush()
                    log_write(f"\n### ISARET {simdi}\n")
                    record_count["isaret"] += 1
                    print(f"  >>> ISARET {record_count['isaret']} @ {simdi}  (CIRP!)")

                raw = ser.read(512)
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace")
                log_write(text)

                row_tamponu += text
                while "\n" in row_tamponu:
                    row, row_tamponu = row_tamponu.split("\n", 1)
                    m = ROW.match(row.rstrip("\r"))
                    if not m:
                        continue
                    simdi = dt.datetime.now().isoformat(timespec="milliseconds")
                    code = m.group("kod")
                    name = m.group("ad").strip()
                    yazar.writerow([simdi, m.group("ms"), m.group("kip"),
                                    "" if code == "-" else code, name, m.group("guven")])
                    fcsv.flush()
                    if m.group("kip") == "TUR":
                        record_count["tur"] += 1
                        print(f"  [{record_count['tur']:3d}] {name or code}  "
                              f"%{m.group('guven')}")

        except KeyboardInterrupt:
            print(f"\n\n  bitti. TUR olayi: {record_count['tur']}, "
                  f"isaret: {record_count['isaret']}")
            print(f"  {csv_yolu}")


if __name__ == "__main__":
    main()
