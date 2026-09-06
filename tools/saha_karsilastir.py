#!/usr/bin/env python3
"""
saha_karsilastir.py — M8: cihazin saha gunlugunu BirdNET referansiyla olc.

    python tools/saha_karsilastir.py \
        --oturum saha/20260901_0640_belgrad \
        --birdnet saha/20260901_0640_belgrad/telefon.BirdNET.results.csv \
        --isaret-ses 12.4

NE OLCUYOR
--------------------------------------------------------------------------
Cihaz ile BirdNET AYNI havayi dinledi. Elimizde mutlak dogru (ground truth)
yok -- BirdNET de yaniliyor. O yuzden olculen sey DOGRULUK degil UYUM:

  ortak         : cihaz TUR dedi, BirdNET ayni turu +-pencere icinde duydu
  cihaz_fazla   : cihaz TUR dedi, BirdNET o turu hic duymadi (yanlis alarm adayi)
  birdnet_fazla : BirdNET esik ustunde duydu, cihaz hic demedi (kacirma adayi)

Bu ucu ayri ayri yazdiriyoruz; tek bir yuzdeye indirgemek yanlis olurdu.
"cihaz_fazla" listesindeki her satir, negatif madenciligi (§9f-4) icin
telefondaki kayittan kesilecek bir aday demektir.

ESZAMANLAMA
--------------------------------------------------------------------------
cihaz.csv duvar saatiyle (PC), BirdNET kayit basindan saniyeyle calisiyor.
Kopru: saha_kayit.py'nin ilk ISARET satiri (o an elinizi cirptiniz) ve o
cirpmanin kayittaki saniyesi (--isaret-ses, Audacity'de bakilir).

    ses_saniyesi = (duvar_saati - isaret_duvar_saati) + isaret_ses

Turun sonunda ikinci bir cirpma yaptiysaniz --isaret2-ses ile verin; betik
telefon-PC saat kaymasini olcup dogrusal duzeltir. Vermezseniz kayma
duzeltilmez, yalnizca uyarilirsiniz.

TUR ESLESTIRME
--------------------------------------------------------------------------
Cihaz eBird kodu basiyor, BirdNET bilimsel ad. Kopru
data/birdnet_name_map.csv (M4'te uretildi). BirdNET'in 178 listemizde
OLMAYAN turleri ayri sayilir -- onlar cihazin bilmedigi turler, kacirma
degil KAPSAM DISI.
"""

import argparse
import csv
import datetime as dt
import os
import sys
from collections import defaultdict

# Windows konsolu (cp857/cp1254) Turkce tur adlarindaki bazi karakterleri
# basamayabilir; eski konsolda print() UnicodeEncodeError atar ve SAHADA
# turu yarida keser. Onun yerine bozuk karakteri isaretleyip devam et --
# gunluk dosyalari zaten UTF-8 yaziliyor, kayipsiz.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, "data", "birdnet_ad_haritasi.csv")


def map_yukle():
    """birdnet_bilimsel_ad -> (ebird_kodu, turkce_ad)"""
    if not os.path.exists(MAP):
        sys.exit("eslesme tablosu yok: " + MAP)
    d = {}
    with open(MAP, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            d[s["birdnet_scientific_name"].strip()] = (s["ebird_code"].strip(),
                                                   s["turkish_name"].strip())
    return d


def cihaz_yukle(path):
    """cihaz.csv -> (isaretler[datetime], tur_olaylari[(dt, kod, ad, guven)])"""
    isaretler, olaylar = [], []
    with open(path, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            t = dt.datetime.fromisoformat(s["duvar_saati"])
            if s["kip"] == "ISARET":
                isaretler.append(t)
            elif s["kip"] == "TUR" and s["ebird_code"]:
                olaylar.append((t, s["ebird_code"], s["turkish_name"],
                                float(s["guven_yuzde"])))
    return isaretler, olaylar


def birdnet_yukle(path, threshold, name_map):
    """BirdNET results.csv -> (bilinen[(bas, bit, kod, ad, skor)], kapsam_disi)"""
    bilinen, kapsam_disi = [], defaultdict(float)
    with open(path, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            skor = float(s["Confidence"])
            if skor < threshold:
                continue
            scientific = s["Scientific name"].strip()
            if scientific in name_map:
                code, tr = name_map[scientific]
                bilinen.append((float(s["Start (s)"]), float(s["End (s)"]),
                                code, tr, skor))
            else:
                kapsam_disi[scientific] = max(kapsam_disi[scientific], skor)
    return bilinen, kapsam_disi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", required=True, help="saha_kayit.py'nin dizini")
    ap.add_argument("--birdnet", required=True,
                    help="telefon kaydinin results.csv'si")
    ap.add_argument("--marker-audio", type=float, required=True,
                    help="ILK cirpmanin kayittaki saniyesi")
    ap.add_argument("--marker2-audio", type=float, default=None,
                    help="SON cirpmanin saniyesi (saat kaymasi duzeltmesi)")
    ap.add_argument("--threshold", type=float, default=0.25,
                    help="BirdNET guven esigi (varsayilan 0.25, BirdNET'inki)")
    ap.add_argument("--window", type=float, default=8.0,
                    help="+- kac saniyelik uyum penceresi. 8 = cihazin 3 pencerelik birlestirmesi (PB_DECISION_MIN_WINDOWS) + 5 s ekranda tutma (PB_DECISION_HOLD_MS)")
    args = ap.parse_args()

    cihaz_csv = os.path.join(args.session, "cihaz.csv")
    if not os.path.exists(cihaz_csv):
        sys.exit("bulunamadi: " + cihaz_csv)

    name_map = map_yukle()
    isaretler, olaylar = cihaz_yukle(cihaz_csv)
    if not isaretler:
        sys.exit("cihaz.csv'de ISARET yok -- eszamanlama yapilamaz.\n"
                 "Turda ENTER'a basip cirpmayi unutmussunuz; bu oturum\n"
                 "zaman ekseninde hizalanamaz (kayitlar yine de sakli).")

    t0 = isaretler[0]
    egim = 1.0
    if args.marker2_audio is not None and len(isaretler) >= 2:
        pc_araligi = (isaretler[-1] - t0).total_seconds()
        audio_araligi = args.marker2_audio - args.marker_audio
        if pc_araligi > 60:
            egim = audio_araligi / pc_araligi
            kayma = (egim - 1.0) * pc_araligi
            print("  saat kaymasi: {:+.2f} s / {:.0f} dk (egim {:.6f}) -- duzeltildi"
                  .format(kayma, pc_araligi / 60, egim))
        else:
            print("  [!] iki isaret arasi 60 sn'den kisa, kayma olculemez; egim 1.0")
    elif len(isaretler) >= 2:
        print("  [!] ikinci isaret var ama --isaret2-ses verilmedi; kayma DUZELTILMEDI")

    def audio_saniyesi(t):
        return (t - t0).total_seconds() * egim + args.marker_audio

    bilinen, kapsam_disi = birdnet_yukle(args.birdnet, args.threshold, name_map)

    # --- eslestirme -------------------------------------------------------
    bn_kullanildi = [False] * len(bilinen)
    ortak, cihaz_extra = [], []
    for t, code, name, guven in olaylar:
        ts = audio_saniyesi(t)
        eslesen = None
        for i, (start, end, bkod, bad, skor) in enumerate(bilinen):
            if bkod != code:
                continue
            if start - args.window <= ts <= end + args.window:
                eslesen = i
                break
        if eslesen is None:
            cihaz_extra.append((ts, code, name, guven))
        else:
            bn_kullanildi[eslesen] = True
            ortak.append((ts, code, name, guven, bilinen[eslesen][4]))

    cihazin_dedigi = set(k for _, k, _, _ in olaylar)
    birdnet_extra = [b for i, b in enumerate(bilinen)
                     if not bn_kullanildi[i] and b[2] not in cihazin_dedigi]

    # --- rapor ------------------------------------------------------------
    W = 74
    print()
    print("=" * W)
    print("  SAHA UYUM RAPORU   " + os.path.basename(os.path.normpath(args.session)))
    print("  BirdNET esigi {}  |  uyum penceresi +-{:.0f} s".format(args.threshold, args.window))
    print("=" * W)
    print("  cihaz TUR olayi                    : {}".format(len(olaylar)))
    print("  BirdNET tespiti (bizim 178 icinde) : {}".format(len(bilinen)))
    print("  BirdNET tespiti (kapsam disi tur)  : {} tur".format(len(kapsam_disi)))
    print("-" * W)
    print("  ortak (hemfikir)                   : {}".format(len(ortak)))
    print("  cihaz fazla (yanlis alarm adayi)   : {}".format(len(cihaz_extra)))
    print("  BirdNET fazla (kacirma adayi)      : {}".format(len(birdnet_extra)))
    if olaylar:
        print("\n  cihazin TUR dediklerinin %{:.1f}'i BirdNET'ce dogrulandi"
              .format(100.0 * len(ortak) / len(olaylar)))

    def blok(title, rows):
        print("\n" + "-" * W)
        print("  " + title)
        print("-" * W)
        if not rows:
            print("  (yok)")
        for s in rows:
            print(" ", s)

    blok("ORTAK - cihaz ve BirdNET ayni turu duydu",
         ["{:8.1f}s  {:<9} {:<24} cihaz %{:5.1f}  birdnet {:.2f}"
          .format(ts, code, name, g, b) for ts, code, name, g, b in ortak])

    blok("CIHAZ FAZLA - BirdNET dogrulamadi (negatif madenciligi adayi)",
         ["{:8.1f}s  {:<9} {:<24} cihaz %{:5.1f}".format(ts, code, name, g)
          for ts, code, name, g in cihaz_extra])

    blok("BIRDNET FAZLA - cihaz bu turu hic demedi",
         ["{:8.1f}s  {:<9} {:<24} birdnet {:.2f}".format(start, code, name, skor)
          for start, end, code, name, skor in birdnet_extra])

    if kapsam_disi:
        blok("KAPSAM DISI - 178 turluk listemizde olmayan (kacirma DEGIL)",
             ["{:<34} en yuksek {:.2f}".format(name, skor)
              for name, skor in sorted(kapsam_disi.items(), key=lambda x: -x[1])])

    output = os.path.join(args.session, "uyum.csv")
    with open(output, "w", encoding="utf-8", newline="") as f:
        y = csv.writer(f)
        y.writerow(["class_index", "ses_saniyesi", "ebird_code", "ad",
                    "cihaz_guven_yuzde", "birdnet_skor"])
        for ts, code, name, g, b in ortak:
            y.writerow(["ortak", "{:.1f}".format(ts), code, name, g, b])
        for ts, code, name, g in cihaz_extra:
            y.writerow(["cihaz_fazla", "{:.1f}".format(ts), code, name, g, ""])
        for start, end, code, name, skor in birdnet_extra:
            y.writerow(["birdnet_fazla", "{:.1f}".format(start), code, name, "", skor])
    print("\n  yazildi: " + output + "\n")


if __name__ == "__main__":
    main()
