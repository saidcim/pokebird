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

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARITA = os.path.join(KOK, "data", "birdnet_ad_haritasi.csv")


def harita_yukle():
    """birdnet_bilimsel_ad -> (ebird_kodu, turkce_ad)"""
    if not os.path.exists(HARITA):
        sys.exit("eslesme tablosu yok: " + HARITA)
    d = {}
    with open(HARITA, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            d[s["birdnet_bilimsel_ad"].strip()] = (s["ebird_kodu"].strip(),
                                                   s["turkce_ad"].strip())
    return d


def cihaz_yukle(yol):
    """cihaz.csv -> (isaretler[datetime], tur_olaylari[(dt, kod, ad, guven)])"""
    isaretler, olaylar = [], []
    with open(yol, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            t = dt.datetime.fromisoformat(s["duvar_saati"])
            if s["kip"] == "ISARET":
                isaretler.append(t)
            elif s["kip"] == "TUR" and s["ebird_kodu"]:
                olaylar.append((t, s["ebird_kodu"], s["turkce_ad"],
                                float(s["guven_yuzde"])))
    return isaretler, olaylar


def birdnet_yukle(yol, esik, harita):
    """BirdNET results.csv -> (bilinen[(bas, bit, kod, ad, skor)], kapsam_disi)"""
    bilinen, kapsam_disi = [], defaultdict(float)
    with open(yol, encoding="utf-8") as f:
        for s in csv.DictReader(f):
            skor = float(s["Confidence"])
            if skor < esik:
                continue
            bilimsel = s["Scientific name"].strip()
            if bilimsel in harita:
                kod, tr = harita[bilimsel]
                bilinen.append((float(s["Start (s)"]), float(s["End (s)"]),
                                kod, tr, skor))
            else:
                kapsam_disi[bilimsel] = max(kapsam_disi[bilimsel], skor)
    return bilinen, kapsam_disi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oturum", required=True, help="saha_kayit.py'nin dizini")
    ap.add_argument("--birdnet", required=True,
                    help="telefon kaydinin results.csv'si")
    ap.add_argument("--isaret-ses", type=float, required=True,
                    help="ILK cirpmanin kayittaki saniyesi")
    ap.add_argument("--isaret2-ses", type=float, default=None,
                    help="SON cirpmanin saniyesi (saat kaymasi duzeltmesi)")
    ap.add_argument("--esik", type=float, default=0.25,
                    help="BirdNET guven esigi (varsayilan 0.25, BirdNET'inki)")
    ap.add_argument("--pencere", type=float, default=8.0,
                    help="+- kac saniyelik uyum penceresi. 8 = cihazin 3 pencerelik birlestirmesi (PB_DECISION_MIN_WINDOWS) + 5 s ekranda tutma (PB_DECISION_HOLD_MS)")
    args = ap.parse_args()

    cihaz_csv = os.path.join(args.oturum, "cihaz.csv")
    if not os.path.exists(cihaz_csv):
        sys.exit("bulunamadi: " + cihaz_csv)

    harita = harita_yukle()
    isaretler, olaylar = cihaz_yukle(cihaz_csv)
    if not isaretler:
        sys.exit("cihaz.csv'de ISARET yok -- eszamanlama yapilamaz.\n"
                 "Turda ENTER'a basip cirpmayi unutmussunuz; bu oturum\n"
                 "zaman ekseninde hizalanamaz (kayitlar yine de sakli).")

    t0 = isaretler[0]
    egim = 1.0
    if args.isaret2_ses is not None and len(isaretler) >= 2:
        pc_araligi = (isaretler[-1] - t0).total_seconds()
        ses_araligi = args.isaret2_ses - args.isaret_ses
        if pc_araligi > 60:
            egim = ses_araligi / pc_araligi
            kayma = (egim - 1.0) * pc_araligi
            print("  saat kaymasi: {:+.2f} s / {:.0f} dk (egim {:.6f}) -- duzeltildi"
                  .format(kayma, pc_araligi / 60, egim))
        else:
            print("  [!] iki isaret arasi 60 sn'den kisa, kayma olculemez; egim 1.0")
    elif len(isaretler) >= 2:
        print("  [!] ikinci isaret var ama --isaret2-ses verilmedi; kayma DUZELTILMEDI")

    def ses_saniyesi(t):
        return (t - t0).total_seconds() * egim + args.isaret_ses

    bilinen, kapsam_disi = birdnet_yukle(args.birdnet, args.esik, harita)

    # --- eslestirme -------------------------------------------------------
    bn_kullanildi = [False] * len(bilinen)
    ortak, cihaz_fazla = [], []
    for t, kod, ad, guven in olaylar:
        ts = ses_saniyesi(t)
        eslesen = None
        for i, (bas, bit, bkod, bad, skor) in enumerate(bilinen):
            if bkod != kod:
                continue
            if bas - args.pencere <= ts <= bit + args.pencere:
                eslesen = i
                break
        if eslesen is None:
            cihaz_fazla.append((ts, kod, ad, guven))
        else:
            bn_kullanildi[eslesen] = True
            ortak.append((ts, kod, ad, guven, bilinen[eslesen][4]))

    cihazin_dedigi = set(k for _, k, _, _ in olaylar)
    birdnet_fazla = [b for i, b in enumerate(bilinen)
                     if not bn_kullanildi[i] and b[2] not in cihazin_dedigi]

    # --- rapor ------------------------------------------------------------
    W = 74
    print()
    print("=" * W)
    print("  SAHA UYUM RAPORU   " + os.path.basename(os.path.normpath(args.oturum)))
    print("  BirdNET esigi {}  |  uyum penceresi +-{:.0f} s".format(args.esik, args.pencere))
    print("=" * W)
    print("  cihaz TUR olayi                    : {}".format(len(olaylar)))
    print("  BirdNET tespiti (bizim 178 icinde) : {}".format(len(bilinen)))
    print("  BirdNET tespiti (kapsam disi tur)  : {} tur".format(len(kapsam_disi)))
    print("-" * W)
    print("  ortak (hemfikir)                   : {}".format(len(ortak)))
    print("  cihaz fazla (yanlis alarm adayi)   : {}".format(len(cihaz_fazla)))
    print("  BirdNET fazla (kacirma adayi)      : {}".format(len(birdnet_fazla)))
    if olaylar:
        print("\n  cihazin TUR dediklerinin %{:.1f}'i BirdNET'ce dogrulandi"
              .format(100.0 * len(ortak) / len(olaylar)))

    def blok(baslik, satirlar):
        print("\n" + "-" * W)
        print("  " + baslik)
        print("-" * W)
        if not satirlar:
            print("  (yok)")
        for s in satirlar:
            print(" ", s)

    blok("ORTAK - cihaz ve BirdNET ayni turu duydu",
         ["{:8.1f}s  {:<9} {:<24} cihaz %{:5.1f}  birdnet {:.2f}"
          .format(ts, kod, ad, g, b) for ts, kod, ad, g, b in ortak])

    blok("CIHAZ FAZLA - BirdNET dogrulamadi (negatif madenciligi adayi)",
         ["{:8.1f}s  {:<9} {:<24} cihaz %{:5.1f}".format(ts, kod, ad, g)
          for ts, kod, ad, g in cihaz_fazla])

    blok("BIRDNET FAZLA - cihaz bu turu hic demedi",
         ["{:8.1f}s  {:<9} {:<24} birdnet {:.2f}".format(bas, kod, ad, skor)
          for bas, bit, kod, ad, skor in birdnet_fazla])

    if kapsam_disi:
        blok("KAPSAM DISI - 178 turluk listemizde olmayan (kacirma DEGIL)",
             ["{:<34} en yuksek {:.2f}".format(ad, skor)
              for ad, skor in sorted(kapsam_disi.items(), key=lambda x: -x[1])])

    cikti = os.path.join(args.oturum, "uyum.csv")
    with open(cikti, "w", encoding="utf-8", newline="") as f:
        y = csv.writer(f)
        y.writerow(["sinif", "ses_saniyesi", "ebird_kodu", "ad",
                    "cihaz_guven_yuzde", "birdnet_skor"])
        for ts, kod, ad, g, b in ortak:
            y.writerow(["ortak", "{:.1f}".format(ts), kod, ad, g, b])
        for ts, kod, ad, g in cihaz_fazla:
            y.writerow(["cihaz_fazla", "{:.1f}".format(ts), kod, ad, g, ""])
        for bas, bit, kod, ad, skor in birdnet_fazla:
            y.writerow(["birdnet_fazla", "{:.1f}".format(bas), kod, ad, "", skor])
    print("\n  yazildi: " + cikti + "\n")


if __name__ == "__main__":
    main()
