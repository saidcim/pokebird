#!/usr/bin/env python3
"""
birdnet_ozet.py — BirdNET sonuclarini tek bir segment tablosuna indir ve
egitime hazir olup olmadigini olc. (M4 adim 3, ikinci yari)

    python tools/birdnet_summary.py

Girdi : data/birdnet_sonuc/<ebird_kodu>/XC*.BirdNET.results.csv
Cikti : data/segmentler.csv        — dilim dizini (egitimin okuyacagi tablo)
        ekrana ozet + zayif tur listesi

--------------------------------------------------------------------------
NEDEN SES KESILMIYOR, DIZIN CIKARILIYOR
--------------------------------------------------------------------------
Plan `birdnet_analyzer.segments` ile dilimleri ayri WAV'lara kesmeyi
ongoruyordu. Kesmiyoruz: tur basina ~400 dilim × 178 tur × 144 KB ≈ 10 GB
eder ve diskte 28 GB kalmisti. Dizin (baslangic/bitis + skor) birkac MB;
egitim kaydi zaten okurken istedigi ofsetten mel'i cikarabiliyor. Dinleyip
dogrulamak icin gereken az sayida ornegi tools/cut_segments.py kesiyor.

--------------------------------------------------------------------------
DILIMDE NE TUTULUYOR
--------------------------------------------------------------------------
hedef_guven     dizinin turu bu dilimde ne kadar guvenle duyuldu (0 = esigin
                altinda kaldi; BirdNET 0.1'in altini hic yazmiyor)
en_iyi_tur      dilimde en yuksek skoru alan tur — hedef degilse kayit o
                dilimde baska bir kusla dolu demektir (bulasik dilim)
kus_disi_*      BirdNET'in kus olmayan siniflari (Engine, Human vocal, Dog,
                Siren...). Bunlar negatif madenciligi (§9f-4) icin degerli:
                kanal uyumu birebir, cunku ayni kayitlardan geliyorlar.
"""

import argparse
import csv
import os
import sys
import wave
from collections import defaultdict

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(KOK, "data")
WAV_DIR = os.path.join(DATA, "wav")
SONUC_DIR = os.path.join(DATA, "birdnet_sonuc")

# Etiket dosyasindan cikarildi: "Ad_Ad" bicimindeki, bilimsel adi olmayan
# siniflar. Gryllus/Miogryllus (cirtlak) disarida birakildi — onlar gercek
# canli sesi, gurultu degil.
KUS_DISI = {
    "Dog", "Engine", "Environmental", "Fireworks", "Gun",
    "Human non-vocal", "Human vocal", "Human whistle",
    "Noise", "Power tools", "Siren",
}

ESIKLER = (0.1, 0.25, 0.5)


def tur_haritasi(harita_yolu):
    """ebird_kodu -> (BirdNET'in kullandigi bilimsel ad, turkce ad)

    !! BIZIM CSV'DEKI ADI KULLANMAYIN. BirdNET iki turde eski cins adinda
    kalmis: Kucuk Karga bizde 'Coloeus monedula', BirdNET'te 'Corvus
    monedula'; Ak Karinli Ebabil bizde 'Tachymarptis melba', BirdNET'te
    'Apus melba'. Hedef turu kendi adimizla arasaydik bu iki turun guveni
    her dilimde 0 cikar, ikisi de sessizce egitim disi kalirdi.

    Harita eBird kodu uzerinden tools/birdnet_slist.py tarafindan uretiliyor.
    """
    if not os.path.exists(harita_yolu):
        sys.exit(
            f"ad haritasi yok: {harita_yolu}\n"
            "once calistirin:  .venv-birdnet\\Scripts\\python tools/birdnet_slist.py"
        )
    with open(harita_yolu, encoding="utf-8") as f:
        return {
            r["ebird_kodu"]: (r["birdnet_bilimsel_ad"], r["turkce_ad"])
            for r in csv.DictReader(f)
        }


def wav_suresi(yol):
    try:
        with wave.open(yol, "rb") as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return None


def dosya_oku(yol):
    """sonuc CSV -> {(bas, bit): [(bilimsel_ad, guven), ...]}"""
    dilim = defaultdict(list)
    with open(yol, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            dilim[(float(r["Start (s)"]), float(r["End (s)"]))].append(
                (r["Scientific name"], float(r["Confidence"]))
            )
    return dilim


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sonuc", default=SONUC_DIR)
    ap.add_argument("--wav", default=WAV_DIR)
    ap.add_argument("--harita", default=os.path.join(DATA, "birdnet_ad_haritasi.csv"))
    ap.add_argument("--out", default=os.path.join(DATA, "segmentler.csv"))
    ap.add_argument(
        "--zayif-esik", type=int, default=100,
        help="bu sayidan az dilimi olan turler zayif diye bildirilir"
    )
    a = ap.parse_args()

    if not os.path.isdir(a.sonuc):
        sys.exit(f"sonuc dizini yok: {a.sonuc} — once tools/birdnet_run.py")

    kod_ad = tur_haritasi(a.harita)  # kod -> (BirdNET bilimsel adi, turkce ad)

    turler = sorted(d for d in os.listdir(a.sonuc)
                    if os.path.isdir(os.path.join(a.sonuc, d)))

    say = {e: defaultdict(int) for e in ESIKLER}
    hedef_en_iyi = defaultdict(int)   # hedefin en yuksek skor oldugu dilim
    tespitli = defaultdict(int)       # en az bir tespit alan dilim
    toplam_dilim = defaultdict(int)   # kayitlardaki tum 3 sn'lik dilimler
    kus_disi_say = defaultdict(int)
    eksik_dosya = defaultdict(int)
    satir = 0

    with open(a.out, "w", encoding="utf-8", newline="") as f:
        y = csv.writer(f)
        y.writerow([
            "ebird_kodu", "dosya", "baslangic", "bitis", "hedef_guven",
            "en_iyi_tur", "en_iyi_guven", "kus_disi_tur", "kus_disi_guven",
        ])

        for kod in turler:
            if kod not in kod_ad:
                print(f"!! {kod} species_istanbul.csv'de 'dahil' degil, atlandi")
                continue
            hedef_ad = kod_ad[kod][0]
            sdir = os.path.join(a.sonuc, kod)
            wdir = os.path.join(a.wav, kod)

            for wf in sorted(os.listdir(wdir)) if os.path.isdir(wdir) else []:
                if not wf.endswith(".wav"):
                    continue
                temel = wf[:-4]
                sf = os.path.join(sdir, temel + ".BirdNET.results.csv")
                if not os.path.exists(sf):
                    eksik_dosya[kod] += 1
                    continue

                sure = wav_suresi(os.path.join(wdir, wf))
                if sure:
                    toplam_dilim[kod] += max(1, int(sure // 3) + (sure % 3 > 0))

                for (bas, bit), tahmin in sorted(dosya_oku(sf).items()):
                    tespitli[kod] += 1
                    d = dict(tahmin)
                    hedef = d.get(hedef_ad, 0.0)
                    en_iyi_tur, en_iyi = max(tahmin, key=lambda x: x[1])
                    kd = [(n, c) for n, c in tahmin if n in KUS_DISI]
                    kd_tur, kd_guven = max(kd, key=lambda x: x[1]) if kd else ("", 0.0)
                    if kd:
                        kus_disi_say[kod] += 1
                    if en_iyi_tur == hedef_ad:
                        hedef_en_iyi[kod] += 1
                    for e in ESIKLER:
                        if hedef >= e:
                            say[e][kod] += 1

                    y.writerow([
                        kod, temel, f"{bas:.1f}", f"{bit:.1f}", f"{hedef:.4f}",
                        en_iyi_tur, f"{en_iyi:.4f}", kd_tur,
                        f"{kd_guven:.4f}" if kd else "",
                    ])
                    satir += 1

    # ---------------- ozet ----------------
    print(f"\n{len(turler)} tur · {satir} dilim satiri -> {a.out}")
    for e in ESIKLER:
        t = sum(say[e].values())
        print(f"  hedef guven >= {e}: {t} dilim  ({t / max(len(turler), 1):.0f}/tur)")
    print(f"  en az bir tespit alan dilim : {sum(tespitli.values())}")
    print(f"  kayitlardaki toplam dilim   : {sum(toplam_dilim.values())}")
    print(f"  kus disi ses iceren dilim   : {sum(kus_disi_say.values())}"
          f"  (negatif madenciligi icin, §9f-4)")

    if eksik_dosya:
        n = sum(eksik_dosya.values())
        print(f"\n!! {n} WAV'in sonucu yok ({len(eksik_dosya)} turde)"
              f" — birdnet_run.py'yi tekrar calistirin (kaldigi yerden devam eder)")
        for k, v in sorted(eksik_dosya.items(), key=lambda x: -x[1])[:10]:
            print(f"   {k:10s} {v}")

    zayif = sorted(
        ((say[0.25][k], k) for k in turler if k in kod_ad),
    )
    az = [(n, k) for n, k in zayif if n < a.zayif_esik]
    print(f"\n0.25 esiginde {a.zayif_esik} dilimin altinda kalan tur: {len(az)}")
    print("(M5'te sinif dengesizligi — focal loss ve veri artirma bunlari hedefleyecek)")
    for n, k in az[:25]:
        tr = kod_ad[k][1]
        pay = hedef_en_iyi[k] / tespitli[k] if tespitli[k] else 0
        print(f"   {k:10s} {tr:28s} {n:5d} dilim   hedef en yuksek: %{pay * 100:.0f}")


if __name__ == "__main__":
    main()
