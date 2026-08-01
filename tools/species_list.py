#!/usr/bin/env python3
"""
species_list.py — M4 adım 1: İstanbul tür listesini kesinleştir.

Çıktı: data/species_istanbul.csv  (bilimsel ad, Türkçe ad, İngilizce ad,
eBird kodu, GBIF kayıt sayısı, aylık dağılım, dahil/elendi + gerekçe).

VERİ KAYNAKLARI — ikisi de API ANAHTARI GEREKTİRMİYOR:

  GBIF occurrence facet'i   İstanbul'da (GADM TUR.40_1) hangi kuş türünden
                            kaç kayıt var. eBird'ün kendi veri kümesi (EOD)
                            GBIF içinde olduğu için bu, eBird bölge listesinin
                            pratik karşılığı. eBird'ün kendi API'si bölge
                            listesi için anahtar istiyor (403), GBIF istemiyor.

  eBird taksonomisi         Bilimsel ad -> Türkçe ad + İngilizce ad + eBird
                            kodu. locale=tr ile Türkçe adlar anahtarsız geliyor.

NEDEN GBIF KAYIT SAYISI EŞİĞİ: Plan §6 "Xeno-canto'da <30 kaydı olan türler
elenir" diyor. Xeno-canto API'si artık anahtar istiyor (v2 kapandı), o filtre
`xc_fetch.py`'ye kaldı. Buradaki GBIF eşiği farklı bir soruyu yanıtlıyor:
"bu tür İstanbul'da gerçekten görülüyor mu, yoksa tek seferlik bir kayıt mı".
İki filtre birbirinin yerine geçmez, ikisi de gerekli.

Kullanım:
    python tools/species_list.py                 # tür listesi (hızlı)
    python tools/species_list.py --aylik         # + aylık dağılım (yavaş, ~390 sorgu)
    python tools/species_list.py --esik 50       # kayıt sayısı eşiğini değiştir
"""

import argparse
import csv
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(KOK, "data")
CACHE = os.path.join(DATA, "cache")

GBIF = "https://api.gbif.org/v1"
EBIRD = "https://api.ebird.org/v2"
AVES_TAXON_KEY = 212          # GBIF: sınıf Aves
ISTANBUL_GADM = "TUR.40_1"    # GADM il kodu — TUR.35_1 Gümüşhane'dir, karıştırmayın

UA = {"User-Agent": "PokeBird/0.1 (kisisel arastirma projesi)"}


# ── Elenen türler ─────────────────────────────────────────────────────────
#
# Bu liste ELDE tutuluyor çünkü otomatik bir ölçütü yok. Her satırın gerekçesi
# yanında; yeni bir tür eklerken gerekçe de yazın, yoksa altı ay sonra neden
# elendiği bilinmiyor.

# Evcil / kafesten kaçmış / yerleşik olmayan egzotikler: İstanbul'da kaydı var
# ama "vahşi İstanbul kuşu" değiller. Modelin bunları öğrenmesi sınıf bütçesi
# israfı olurdu.
EGZOTIK = {
    "Psittacula krameri":    "kafesten kacmis papagan (yerlesik ama egzotik)",
    "Myiopsitta monachus":   "kafesten kacmis papagan",
    "Melopsittacus undulatus": "muhabbet kusu, kafes kusu",
    "Nymphicus hollandicus": "sultan papagani, kafes kusu",
    "Serinus canaria":       "kanarya, kafes kusu",
    "Gallus gallus":         "evcil tavuk",
    "Anser anser domesticus": "evcil kaz",
    "Cairina moschata":      "evcil misk ordegi",
    "Pavo cristatus":        "tavus kusu, evcil",
    "Numida meleagris":      "beckusu, evcil",
    "Phasianus colchicus":   "salinmis av kusu",
    "Estrilda astrild":      "kafesten kacmis ispinoz",
    "Euplectes afer":        "kafesten kacmis dokumaci",
    "Columba livia domestica": "evcil guvercin",
}

# Sesle tanımanın anlamsız/imkânsız olduğu türler. Dikkat: "sessiz kuş" diye
# bir şey yok — mesele MİKROFONLA AYIRT EDİLEBİLİRLİK.
SESLE_AYIRT_EDILEMEZ = {
    # Denizde, kıyıdan yüzlerce metre uzakta ve genelde sessiz uçan türler
    "Calonectris diomedea": "acik denizde, karadan ses duyulmaz",
    "Puffinus yelkouan":    "acik denizde, karadan ses duyulmaz",
    "Hydrobates pelagicus": "acik denizde, karadan ses duyulmaz",
}


def cache_get(ad, uretici):
    """API sonucunu diske al. Script tekrar çalıştırılabilir olsun ve API
    gereksiz yere dövülmesin; 390 tür sorgusu yeniden çekilirse dakikalar."""
    os.makedirs(CACHE, exist_ok=True)
    yol = os.path.join(CACHE, ad)
    if os.path.exists(yol):
        with open(yol, "r", encoding="utf-8") as f:
            return json.load(f)
    veri = uretici()
    with open(yol, "w", encoding="utf-8") as f:
        json.dump(veri, f, ensure_ascii=False)
    return veri


def http_json(url, deneme=3):
    for i in range(deneme):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=90) as r:
                return json.loads(r.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
            if i == deneme - 1:
                raise
            time.sleep(2 * (i + 1))


# ── GBIF ───────────────────────────────────────────────────────────────────

def gbif_istanbul_turleri():
    """İstanbul'daki kuş türleri ve kayıt sayıları: {speciesKey: sayi}."""
    url = (f"{GBIF}/occurrence/search?gadmGid={ISTANBUL_GADM}"
           f"&taxonKey={AVES_TAXON_KEY}&limit=0&hasCoordinate=true"
           f"&facet=speciesKey&facetLimit=1200")
    d = http_json(url)
    sayimlar = d.get("facets", [{}])[0].get("counts", [])
    return d["count"], {c["name"]: c["count"] for c in sayimlar}


def gbif_tur_adi(species_key):
    d = http_json(f"{GBIF}/species/{species_key}")
    return d.get("canonicalName") or d.get("scientificName", "")


def gbif_aylik(species_key):
    """Türün İstanbul'daki aylık kayıt dağılımı — mevsim önceliği (Aşama 3)
    tablosunun ham verisi. 12 elemanlı liste, Ocak=indeks 0."""
    url = (f"{GBIF}/occurrence/search?gadmGid={ISTANBUL_GADM}"
           f"&taxonKey={AVES_TAXON_KEY}&speciesKey={species_key}"
           f"&limit=0&hasCoordinate=true&facet=month&facetLimit=12")
    d = http_json(url)
    aylar = [0] * 12
    for c in d.get("facets", [{}])[0].get("counts", []):
        try:
            ay = int(c["name"])
            if 1 <= ay <= 12:
                aylar[ay - 1] = c["count"]
        except (ValueError, KeyError):
            pass
    return aylar


# ── eBird ──────────────────────────────────────────────────────────────────

def ebird_taksonomi():
    """Bilimsel ad -> (eBird kodu, Türkçe ad, İngilizce ad).

    Türkçe ve İngilizce adlar için taksonomi İKİ KEZ çekiliyor: API locale
    başına tek ad döndürüyor, ikisini birden veren bir uç nokta yok."""
    def cek(locale):
        url = f"{EBIRD}/ref/taxonomy/ebird?fmt=json&cat=species"
        if locale:
            url += f"&locale={locale}"
        return http_json(url)

    tr = cache_get("ebird_tax_tr.json", lambda: cek("tr"))
    en = cache_get("ebird_tax_en.json", lambda: cek(None))

    en_ad = {t["speciesCode"]: t.get("comName", "") for t in en}
    tablo = {}
    for t in tr:
        sci = t.get("sciName", "")
        kod = t.get("speciesCode", "")
        if sci:
            tablo[sci] = (kod, t.get("comName", ""), en_ad.get(kod, ""))
    return tablo


# ── Ana akış ───────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="PokeBird M4: Istanbul tur listesi")
    ap.add_argument("--esik", type=int, default=30,
                    help="asgari GBIF kayit sayisi (varsayilan 30)")
    ap.add_argument("--aylik", action="store_true",
                    help="aylik dagilimi da cek (tur basina 1 sorgu, yavas)")
    ap.add_argument("--out", default=os.path.join(DATA, "species_istanbul.csv"))
    args = ap.parse_args()

    os.makedirs(DATA, exist_ok=True)

    print(f"GBIF: Istanbul ({ISTANBUL_GADM}) kus kayitlari cekiliyor...")
    toplam, sayimlar = cache_get("gbif_istanbul_facet.json", gbif_istanbul_turleri)
    print(f"  {toplam:,} kayit, {len(sayimlar)} farkli tur")

    print("eBird taksonomisi (TR + EN) cekiliyor...")
    taksonomi = ebird_taksonomi()
    print(f"  {len(taksonomi)} tur")

    print("Tur adlari cozumleniyor...")
    adlar = cache_get("gbif_species_names.json",
                      lambda: {k: gbif_tur_adi(k) for k in sayimlar})

    satirlar = []
    for key, sayi in sorted(sayimlar.items(), key=lambda kv: -kv[1]):
        sci = adlar.get(key, "")
        if not sci:
            continue

        kod, tr_ad, en_ad = taksonomi.get(sci, ("", "", ""))

        durum, gerekce = "dahil", ""
        if sci in EGZOTIK:
            durum, gerekce = "elendi", EGZOTIK[sci]
        elif sci in SESLE_AYIRT_EDILEMEZ:
            durum, gerekce = "elendi", SESLE_AYIRT_EDILEMEZ[sci]
        elif sayi < args.esik:
            durum, gerekce = "elendi", f"Istanbul'da yalnizca {sayi} kayit (esik {args.esik})"
        elif not kod:
            # eBird taksonomisinde yoksa ya alt tür ya da eskimiş bir ad.
            # Sessizce dahil etmek yanlış: Xeno-canto sorgusu da tutmaz.
            durum, gerekce = "elendi", "eBird taksonomisinde eslesmedi"

        satirlar.append({
            "ebird_kodu": kod, "bilimsel_ad": sci, "turkce_ad": tr_ad,
            "ingilizce_ad": en_ad, "gbif_kayit": sayi, "gbif_species_key": key,
            "durum": durum, "gerekce": gerekce,
        })

    if args.aylik:
        dahil = [s for s in satirlar if s["durum"] == "dahil"]
        print(f"Aylik dagilim cekiliyor ({len(dahil)} tur)...")
        for i, s in enumerate(dahil, 1):
            key = s["gbif_species_key"]
            aylar = cache_get(f"aylik_{key}.json", lambda k=key: gbif_aylik(k))
            for ay in range(12):
                s[f"ay_{ay+1:02d}"] = aylar[ay]
            if i % 25 == 0 or i == len(dahil):
                print(f"  {i}/{len(dahil)}")

    sutunlar = ["ebird_kodu", "bilimsel_ad", "turkce_ad", "ingilizce_ad",
                "gbif_kayit", "gbif_species_key", "durum", "gerekce"]
    if args.aylik:
        sutunlar += [f"ay_{a:02d}" for a in range(1, 13)]

    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=sutunlar, extrasaction="ignore")
        w.writeheader()
        for s in satirlar:
            w.writerow(s)

    dahil = sum(1 for s in satirlar if s["durum"] == "dahil")
    elendi = len(satirlar) - dahil
    print(f"\n{args.out}")
    print(f"  dahil  {dahil} tur")
    print(f"  elendi {elendi} tur")

    if dahil:
        print("\nEn cok kaydi olan 10 tur:")
        for s in [x for x in satirlar if x["durum"] == "dahil"][:10]:
            print(f"  {s['gbif_kayit']:7,}  {s['turkce_ad'] or s['bilimsel_ad']}")

    # Plan ~110 tür öngörüyor. Buradaki liste HAVUZ; nihai daraltmayı
    # xc_fetch.py yapacak.
    #
    # ESIGI YUKSELTEREK 110'A INMEYIN — olculdu, yanlis turleri eliyor:
    # esik 912'ye cikarilinca Guguk (839 kayit), Sarıasma (817), Orman Alaca
    # Agackakan (835), Bahce Tirmasikkusu (837) gibi TAM DA sesle taninacak
    # orman oturuleri eleniyor; yerine Flamingo (912), Kugu (838), martilar
    # kaliyor. Cunku GBIF kayit sayisi "kac kisi gorup bildirdi"yi olcuyor,
    # "otuyor mu / sesi ayirt edilebilir mi"yi degil: su kuslari acikta ve
    # kolay goruluyor, orman oturuleri duyuluyor ama gorulmuyor.
    #
    # Dogru daraltma olcutu Xeno-canto ses kaydi sayisi: hem sesle
    # taninabilirligi hem egitim verisi mevcudiyetini ayni anda olcer.
    if dahil > 300:
        print(f"\n[!] {dahil} tur beklenenden fazla — GBIF sorgusu genislemis olabilir.")
    elif dahil < 150:
        print(f"\n[!] {dahil} tur, havuz icin dusuk. --esik dusurmeyi dusunun.")
    else:
        print(f"\nBu bir HAVUZ listesi. Nihai ~110 ture daraltma xc_fetch.py'de,\n"
              f"Xeno-canto ses kaydi sayisina gore yapilacak (bkz. dosya basligi).")


if __name__ == "__main__":
    sys.exit(main())
