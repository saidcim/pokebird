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
saha/<TARIH>_<yer>/oturum.txt  port, firmware git surumu, kullanicinin notu
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

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# src/main.c'deki printf ile birebir:
#   printf("  [%lu ms] %-14s %s %s  %%%.1f\n", ms, kip, kod, ad, guven*100)
# kip icinde bosluk olabilen tek deger "SES ALGILANDI"; o yuzden alternatif
# listesi acikca yaziliyor, \S+ ile yakalanamaz.
KIPLER = ("SES ALGILANDI", "dinliyor", "olabilir", "TUR")
SATIR = re.compile(
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
            ["git", "-C", KOK, "describe", "--always", "--dirty"],
            stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return "bilinmiyor"


def ekrana_gir(ser, log_yaz):
    """Sonuc ekranina gir. Firmware iki surumde de olabilir:

    - HEAD: main() komut dongusune duser, '> ' istemi gelir -> 'c' gonder.
    - Calisma agacindaki surum: main() dogrudan cmd_sonuc_ekrani()'ne girer,
      istem HIC gelmez -> 'c' gondermek EKRANDAN CIKARIR (bosluk/n disinda
      her tus cikis, src/main.c'deki yorum). O yuzden once dinliyoruz.

    1,5 sn icinde '> ' gorursek istem var demektir."""
    print("[i] firmware yoklaniyor (1,5 sn)...")
    biriken = ""
    son = time.time() + 1.5
    while time.time() < son:
        ham = ser.read(256)
        if ham:
            metin = ham.decode("utf-8", errors="replace")
            biriken += metin
            log_yaz(metin)
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
    ap.add_argument("--yer", required=True, help="ornek: belgrad, validebag")
    ap.add_argument("--not", dest="notu", default="", help="hava, saat, kim vs.")
    ap.add_argument("--dizin", default=os.path.join(KOK, "saha"))
    args = ap.parse_args()

    damga = dt.datetime.now().strftime("%Y%m%d_%H%M")
    oturum = os.path.join(args.dizin, f"{damga}_{args.yer}")
    os.makedirs(oturum, exist_ok=True)

    log_yolu = os.path.join(oturum, "cihaz.log")
    csv_yolu = os.path.join(oturum, "cihaz.csv")

    with open(os.path.join(oturum, "oturum.txt"), "w", encoding="utf-8") as f:
        f.write(f"baslangic : {dt.datetime.now().isoformat(timespec='seconds')}\n")
        f.write(f"yer       : {args.yer}\n")
        f.write(f"port      : {args.port}\n")
        f.write(f"firmware  : {git_surum()}\n")
        f.write(f"not       : {args.notu}\n")

    print(f"\n  oturum: {oturum}")
    print("  ENTER = ISARET (basarken ELINIZI CIRPIN) · Ctrl+C = bitir\n")

    kayit_sayisi = {"tur": 0, "isaret": 0}
    satir_tamponu = ""

    with open(log_yolu, "w", encoding="utf-8", newline="") as flog, \
         open(csv_yolu, "w", encoding="utf-8", newline="") as fcsv, \
         serial.Serial(args.port, 115200, timeout=0.1) as ser:

        yazar = csv.writer(fcsv)
        yazar.writerow(["duvar_saati", "cihaz_ms", "kip", "ebird_kodu",
                        "turkce_ad", "guven_yuzde"])

        def log_yaz(metin):
            flog.write(metin)
            flog.flush()

        ekrana_gir(ser, log_yaz)

        try:
            while True:
                if bekleyen_tus():
                    simdi = dt.datetime.now().isoformat(timespec="milliseconds")
                    yazar.writerow([simdi, "", "ISARET", "", "", ""])
                    fcsv.flush()
                    log_yaz(f"\n### ISARET {simdi}\n")
                    kayit_sayisi["isaret"] += 1
                    print(f"  >>> ISARET {kayit_sayisi['isaret']} @ {simdi}  (CIRP!)")

                ham = ser.read(512)
                if not ham:
                    continue
                metin = ham.decode("utf-8", errors="replace")
                log_yaz(metin)

                satir_tamponu += metin
                while "\n" in satir_tamponu:
                    satir, satir_tamponu = satir_tamponu.split("\n", 1)
                    m = SATIR.match(satir.rstrip("\r"))
                    if not m:
                        continue
                    simdi = dt.datetime.now().isoformat(timespec="milliseconds")
                    kod = m.group("kod")
                    ad = m.group("ad").strip()
                    yazar.writerow([simdi, m.group("ms"), m.group("kip"),
                                    "" if kod == "-" else kod, ad, m.group("guven")])
                    fcsv.flush()
                    if m.group("kip") == "TUR":
                        kayit_sayisi["tur"] += 1
                        print(f"  [{kayit_sayisi['tur']:3d}] {ad or kod}  "
                              f"%{m.group('guven')}")

        except KeyboardInterrupt:
            print(f"\n\n  bitti. TUR olayi: {kayit_sayisi['tur']}, "
                  f"isaret: {kayit_sayisi['isaret']}")
            print(f"  {csv_yolu}")


if __name__ == "__main__":
    main()
