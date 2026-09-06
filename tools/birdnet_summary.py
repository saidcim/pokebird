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

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
WAV_DIR = os.path.join(DATA, "wav")
RESULT_DIR = os.path.join(DATA, "birdnet_sonuc")

# Etiket dosyasindan cikarildi: "Ad_Ad" bicimindeki, bilimsel adi olmayan
# siniflar. Gryllus/Miogryllus (cirtlak) disarida birakildi — onlar gercek
# canli sesi, gurultu degil.
BIRD_DISI = {
    "Dog", "Engine", "Environmental", "Fireworks", "Gun",
    "Human non-vocal", "Human vocal", "Human whistle",
    "Noise", "Power tools", "Siren",
}

ESIKLER = (0.1, 0.25, 0.5)


def species_haritasi(map_yolu):
    """ebird_kodu -> (BirdNET'in kullandigi bilimsel ad, turkce ad)

    !! BIZIM CSV'DEKI ADI KULLANMAYIN. BirdNET iki turde eski cins adinda
    kalmis: Kucuk Karga bizde 'Coloeus monedula', BirdNET'te 'Corvus
    monedula'; Ak Karinli Ebabil bizde 'Tachymarptis melba', BirdNET'te
    'Apus melba'. Hedef turu kendi adimizla arasaydik bu iki turun guveni
    her dilimde 0 cikar, ikisi de sessizce egitim disi kalirdi.

    Harita eBird kodu uzerinden tools/birdnet_slist.py tarafindan uretiliyor.
    """
    if not os.path.exists(map_yolu):
        sys.exit(
            f"ad haritasi yok: {map_yolu}\n"
            "once calistirin:  .venv-birdnet\\Scripts\\python tools/birdnet_slist.py"
        )
    with open(map_yolu, encoding="utf-8") as f:
        return {
            r["ebird_code"]: (r["birdnet_scientific_name"], r["turkish_name"])
            for r in csv_compat.reader(f)
        }


def wav_suresi(path):
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return None


def file_oku(path):
    """sonuc CSV -> {(bas, bit): [(bilimsel_ad, guven), ...]}"""
    slice = defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            slice[(float(r["Start (s)"]), float(r["End (s)"]))].append(
                (r["Scientific name"], float(r["Confidence"]))
            )
    return slice


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--result", default=RESULT_DIR)
    ap.add_argument("--wav", default=WAV_DIR)
    ap.add_argument("--name-map", default=os.path.join(DATA, "birdnet_ad_haritasi.csv"))
    ap.add_argument("--out", default=os.path.join(DATA, "segmentler.csv"))
    ap.add_argument(
        "--zayif-threshold", type=int, default=100,
        help="bu sayidan az dilimi olan turler zayif diye bildirilir"
    )
    a = ap.parse_args()

    if not os.path.isdir(a.result):
        sys.exit(f"sonuc dizini yok: {a.result} — once tools/birdnet_run.py")

    code_name = species_haritasi(a.name_map)  # kod -> (BirdNET bilimsel adi, turkce ad)

    species = sorted(d for d in os.listdir(a.result)
                    if os.path.isdir(os.path.join(a.result, d)))

    count = {e: defaultdict(int) for e in ESIKLER}
    target_max_iyi = defaultdict(int)   # hedefin en yuksek skor oldugu dilim
    tespitli = defaultdict(int)       # en az bir tespit alan dilim
    total_slice = defaultdict(int)   # kayitlardaki tum 3 sn'lik dilimler
    bird_disi_count = defaultdict(int)
    missing_file = defaultdict(int)
    row = 0

    with open(a.out, "w", encoding="utf-8", newline="") as f:
        y = csv.writer(f)
        y.writerow([
            "ebird_code", "file", "start", "end", "target_confidence",
            "best_species", "best_confidence", "non_bird_species", "non_bird_confidence",
        ])

        for code in species:
            if code not in code_name:
                print(f"!! {code} species_istanbul.csv'de 'dahil' degil, atlandi")
                continue
            target_name = code_name[code][0]
            sdir = os.path.join(a.result, code)
            wdir = os.path.join(a.wav, code)

            for wf in sorted(os.listdir(wdir)) if os.path.isdir(wdir) else []:
                if not wf.endswith(".wav"):
                    continue
                temel = wf[:-4]
                sf = os.path.join(sdir, temel + ".BirdNET.results.csv")
                if not os.path.exists(sf):
                    missing_file[code] += 1
                    continue

                duration = wav_suresi(os.path.join(wdir, wf))
                if duration:
                    total_slice[code] += max(1, int(duration // 3) + (duration % 3 > 0))

                for (start, end), pred in sorted(file_oku(sf).items()):
                    tespitli[code] += 1
                    d = dict(pred)
                    target = d.get(target_name, 0.0)
                    max_iyi_species, best = max(pred, key=lambda x: x[1])
                    kd = [(n, c) for n, c in pred if n in BIRD_DISI]
                    kd_species, kd_guven = max(kd, key=lambda x: x[1]) if kd else ("", 0.0)
                    if kd:
                        bird_disi_count[code] += 1
                    if max_iyi_species == target_name:
                        target_max_iyi[code] += 1
                    for e in ESIKLER:
                        if target >= e:
                            count[e][code] += 1

                    y.writerow([
                        code, temel, f"{start:.1f}", f"{end:.1f}", f"{target:.4f}",
                        max_iyi_species, f"{best:.4f}", kd_species,
                        f"{kd_guven:.4f}" if kd else "",
                    ])
                    row += 1

    # ---------------- ozet ----------------
    print(f"\n{len(species)} tur · {row} dilim satiri -> {a.out}")
    for e in ESIKLER:
        t = sum(count[e].values())
        print(f"  hedef guven >= {e}: {t} dilim  ({t / max(len(species), 1):.0f}/tur)")
    print(f"  en az bir tespit alan dilim : {sum(tespitli.values())}")
    print(f"  kayitlardaki toplam dilim   : {sum(total_slice.values())}")
    print(f"  kus disi ses iceren dilim   : {sum(bird_disi_count.values())}"
          f"  (negatif madenciligi icin, §9f-4)")

    if missing_file:
        n = sum(missing_file.values())
        print(f"\n!! {n} WAV'in sonucu yok ({len(missing_file)} turde)"
              f" — birdnet_run.py'yi tekrar calistirin (kaldigi yerden devam eder)")
        for k, v in sorted(missing_file.items(), key=lambda x: -x[1])[:10]:
            print(f"   {k:10s} {v}")

    zayif = sorted(
        ((count[0.25][k], k) for k in species if k in code_name),
    )
    az = [(n, k) for n, k in zayif if n < a.zayif_threshold]
    print(f"\n0.25 esiginde {a.zayif_threshold} dilimin altinda kalan tur: {len(az)}")
    print("(M5'te sinif dengesizligi — focal loss ve veri artirma bunlari hedefleyecek)")
    for n, k in az[:25]:
        tr = code_name[k][1]
        pay = target_max_iyi[k] / tespitli[k] if tespitli[k] else 0
        print(f"   {k:10s} {tr:28s} {n:5d} dilim   hedef en yuksek: %{pay * 100:.0f}")


if __name__ == "__main__":
    main()
