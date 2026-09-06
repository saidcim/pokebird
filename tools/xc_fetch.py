#!/usr/bin/env python3
r"""
xc_fetch.py — M4 adım 1b+2: Xeno-canto'dan ses kaydı sayımı ve indirme.

İki iş yapar:

  --say      Havuzdaki her tür için Xeno-canto'da kaç kayıt var, sayar ve
             species_istanbul.csv'yi `xc_kayit` + nihai `durum` sütunlarıyla
             günceller. NİHAİ ~110 TÜRLÜK LİSTE BURADA OLUŞUYOR.

  --indir    Nihai listedeki türlerin kayıtlarını indirir (kalite A/B önce).

NEDEN DARALTMA ÖLÇÜTÜ BU: GBIF kayıt sayısı "kaç kişi görüp bildirdi"yi
ölçüyor, sesle tanınabilirliği değil — su kuşları açıkta ve kolay görülüyor,
orman ötücüleri duyuluyor ama görülmüyor. GBIF eşiğini yükselterek 110'a
inmek Guguk / Sarıasma / Orman Alaca Ağaçkakan gibi tam da hedeflediğimiz
türleri eliyordu (ölçüldü, bkz. species_list.py sonundaki not). Xeno-canto
kayıt sayısı iki soruyu birden yanıtlıyor: tür sesle tanınıyor mu, ve o sesi
öğretecek verimiz var mı.

API ANAHTARI GEREKİYOR:
  Xeno-canto API v2 kapandı; v3 anahtar istiyor. Ücretsiz:
  https://xeno-canto.org/account

  Anahtar üç yerden okunuyor, bu sırayla:
      1. --key parametresi
      2. XC_KEY ortam değişkeni
      3. data/.xc_key dosyası        <- TERCİH EDİLEN

  Dosya yöntemi tercih edilir: anahtar komut geçmişine, ekran görüntüsüne
  ya da sohbet kaydına düşmez. .gitignore'da olduğu için depoya da girmez.
  Oluşturmak için (PowerShell):

      "ANAHTARINIZ" | Out-File -Encoding ascii -NoNewline data\.xc_key

LİSANS — ND KAYITLARI ALINMIYOR:
  Xeno-canto kayıtları Creative Commons ama hepsi aynı değil. Guguk'un ilk
  300 A/B kaydında ölçülen dağılım:
      by-nc-sa 201 · by-nc-nd 34 · by-nc 4 · CC0 2 · by 1 · by-sa 1
  **ND = NoDerivatives**, yani eseri işleyip dağıtmak yasak. Modeli o kayıtla
  eğitmenin türev eser sayılıp sayılmayacağı tartışmalı; %14'lük veri için
  bu riski almaya değmez. Bu yüzden ND lisanslı kayıtlar İNDİRİLMİYOR
  (`--nd-dahil` ile açılabilir, sorumluluk kullanıcıda).

  Kalan lisansların ezici çoğunluğu BY-NC-SA: gayriticari + aynı lisansla
  paylaşım. Projenin kişisel kullanımıyla uyumlu, ticari dağıtımıyla değil —
  BirdNET'in CC BY-NC-SA kısıtıyla aynı sınıftan sorun (ARCHITECTURE §6).

  İndirilen her dosyanın lisansı, kaydedeni ve XC kimliği
  `data/xc/kayitlar.csv`'ye yazılıyor — atıf yükümlülüğü için saklayın.

COĞRAFYA — AVRUPA ÖNCELİKLİ:
  Türkiye kayıtları pratikte yok (ölçüldü: Guguk 1, Kızılgerdan 0, Büyük
  Baştankara 4). Ama Avrupa kayıtları bol ve dünya toplamının ~%90'ı zaten
  Avrupa. Kuş sesinde bölgesel lehçe gerçek bir olgu olduğu için üreme
  bölgesi kayıtları tercih ediliyor: `area:europe`. Avrupa'da yeterli kayıt
  bulunmayan tür için dünya geneline düşülüyor.
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
XC_DIR = os.path.join(DATA, "xc")
WAV_DIR = os.path.join(DATA, "wav")      # xc_convert.py ciktisi
CACHE = os.path.join(DATA, "cache")
CSV_PATH = os.path.join(DATA, "species_istanbul.csv")

API = "https://xeno-canto.org/api/3/recordings"
UA = {"User-Agent": "PokeBird/0.1 (kisisel arastirma projesi)"}

# ── Nihai eleme kuralı ────────────────────────────────────────────────────
#
# Tek eşik ÇALIŞMIYOR, iki yönden de ölçüldü:
#
#   Sadece GBIF (görülme) eşiği: 912'de Guguk / Sarıasma / Orman Alaca
#     Ağaçkakan eleniyor, Flamingo ve martılar kalıyor — GBIF "kaç kişi görüp
#     bildirdi"yi ölçüyor, "ötüyor mu"yu değil.
#
#   Sadece XC (ses verisi) eşiği: 60'ta İbibik (1.680 GBIF, son derece ayırt
#     edici "hüd-hüd" sesi), Kerkenez, Yeşilbaş, Küçük Ağaçkakan eleniyor —
#     İstanbul'da yaygın ama Xeno-canto'da az kaydı olan türler.
#
# Bu yüzden iki boyut birleştiriliyor: İstanbul'da YAYGIN türler düşük ses
# verisiyle de listede kalır (kullanıcı onları gerçekten duyacak), NADİR
# türler ise ancak bol ses verisi varsa girer (yoksa sınıf zaten öğrenilemez).
COMMON_GBIF = 800     # bu kadar İstanbul kaydı olan tür "yaygın" sayılır
VARSAYILAN_THRESHOLD = 30  # yaygın türler için asgari XC A/B kaydı
RARE_THRESHOLD = 100      # yaygın olmayan türler için asgari XC A/B kaydı

# Türev eser yasaklayan lisanslar — eğitim verisine alınmıyor (bkz. başlık).
ND_ISARETI = "-nd"


def nd_mi(lisans_url):
    """Lisans ND (NoDerivatives) mi? URL biçimi:
    https://creativecommons.org/licenses/by-nc-nd/4.0/"""
    if not lisans_url:
        return False           # lisansı bilinmeyen kayıt: metadata'da 'lic' boş
    u = lisans_url.lower()
    if "publicdomain" in u or "zero" in u:
        return False
    code = u.rstrip("/").split("/licenses/")[-1].split("/")[0] if "/licenses/" in u else u
    return "nd" in code.split("-")


def http_json(url, attempt=3):
    for i in range(attempt):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=90) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            govde = e.read(300).decode("utf-8", errors="replace")
            if e.code in (401, 403):
                sys.exit(f"\n[!] Xeno-canto anahtari reddedildi (HTTP {e.code}).\n"
                         f"    {govde}\n"
                         f"    https://xeno-canto.org/account adresinden alin,\n"
                         f"    XC_KEY ortam degiskenine koyun ya da --key ile verin.\n")
            if i == attempt - 1:
                raise
            time.sleep(2 * (i + 1))
        except (urllib.error.URLError, TimeoutError):
            if i == attempt - 1:
                raise
            time.sleep(2 * (i + 1))


def xc_sorgu(key, sorgu, sayfa=1, per_page=1):
    p = urllib.parse.urlencode({"query": sorgu, "key": key,
                                "page": sayfa, "per_page": per_page})
    return http_json(f"{API}?{p}")


def csv_oku():
    if not os.path.exists(CSV_PATH):
        sys.exit(f"[!] {CSV_PATH} yok. Once: python tools/species_list.py")
    with open(CSV_PATH, encoding="utf-8") as f:
        r = csv.DictReader(f)
        return list(r), list(r.fieldnames)


def csv_write(rows, sutunlar):
    with open(CSV_PATH, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=sutunlar, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


# ── Sayım ──────────────────────────────────────────────────────────────────

def komut_count(key, threshold, rare_threshold=RARE_THRESHOLD, common_gbif=COMMON_GBIF):
    rows, sutunlar = csv_oku()
    havuz = [s for s in rows if s["durum"] == "dahil"]
    print(f"Havuzda {len(havuz)} tur. Xeno-canto kayit sayilari cekiliyor...")
    print(f"(Avrupa, kalite A/B. Yaygin tur [GBIF>={COMMON_GBIF}] icin >={threshold} kayit,\n"
          f" nadir tur icin >={RARE_THRESHOLD} kayit gerekiyor)\n")

    os.makedirs(CACHE, exist_ok=True)
    for i, s in enumerate(havuz, 1):
        sci = s["bilimsel_ad"]
        onbellek = os.path.join(CACHE, f"xc_{sci.replace(' ', '_')}.json")

        if os.path.exists(onbellek):
            with open(onbellek, encoding="utf-8") as f:
                d = json.load(f)
        else:
            # Sadece kayıt SAYISI lazım: per_page=1 ile tek kayıt isteyip
            # numRecordings alanını okuyoruz. Tüm sayfaları çekmek gereksiz.
            #
            # Üç sayı: dünya A/B, Avrupa A/B, Avrupa BY-NC-SA. Üçüncüsü
            # lisans payını gösteriyor — negatif lisans filtresi (-lic:) API
            # tarafından desteklenmiyor (400 dönüyor), o yüzden ND'yi burada
            # değil indirme sırasında metadata'dan eliyoruz.
            d = {
                "ab":     xc_sorgu(key, f'sp:"{sci}" q:">C"').get("numRecordings", "0"),
                "ab_eu":  xc_sorgu(key, f'sp:"{sci}" q:">C" area:europe').get("numRecordings", "0"),
                "ab_sa":  xc_sorgu(key, f'sp:"{sci}" q:">C" area:europe lic:"BY-NC-SA"').get("numRecordings", "0"),
            }
            with open(onbellek, "w", encoding="utf-8") as f:
                json.dump(d, f)
            time.sleep(0.34)      # API'ye nazik davran

        s["xc_ab"] = int(d["ab"])
        s["xc_ab_eu"] = int(d["ab_eu"])
        s["xc_ab_sa"] = int(d["ab_sa"])

        if i % 20 == 0 or i == len(havuz):
            print(f"  {i}/{len(havuz)}")

    # Nihai eleme — birleşik kural (gerekçe: dosya başındaki sabitler).
    # Ses verisi ölçütü Avrupa A/B sayısı; Avrupa'da hiç kayıt yoksa
    # (İstanbul'da görülen Asya/Afrika türleri) dünya geneline düşülüyor.
    for s in rows:
        if s["durum"] != "dahil":
            for k in ("xc_ab", "xc_ab_eu", "xc_ab_sa"):
                s.setdefault(k, "")
            continue
        eu = int(s.get("xc_ab_eu") or 0)
        dunya = int(s.get("xc_ab") or 0)
        etkin = eu if eu > 0 else dunya
        common = int(s.get("gbif_kayit") or 0) >= common_gbif
        gereken = threshold if common else rare_threshold

        if etkin < gereken:
            s["durum"] = "elendi"
            s["gerekce"] = (
                f"{'yaygin' if common else 'nadir'} tur, XC'de {etkin} A/B kayit "
                f"(gereken {gereken}; Avrupa {eu}, dunya {dunya})")

    for k in ("xc_ab", "xc_ab_eu", "xc_ab_sa"):
        if k not in sutunlar:
            sutunlar.append(k)
    csv_write(rows, sutunlar)

    nihai = [s for s in rows if s["durum"] == "dahil"]
    print(f"\n{CSV_PATH}")
    print(f"  NIHAI LISTE: {len(nihai)} tur")
    if nihai:
        total_record = sum(int(s["xc_ab_eu"] or 0) or int(s["xc_ab"] or 0) for s in nihai)
        print(f"  Toplam kullanilabilir A/B kayit: {total_record:,}")
        print("\n  En az ses kaydi olan 10 tur (veri riski burada):")
        for s in sorted(nihai, key=lambda x: int(x["xc_ab_eu"] or 0) or int(x["xc_ab"] or 0))[:10]:
            eu, dw = int(s["xc_ab_eu"] or 0), int(s["xc_ab"] or 0)
            print(f"    {eu or dw:5} A/B  {s['turkce_ad'] or s['bilimsel_ad']}"
                  f"{'  (dunya geneli)' if not eu else ''}")
    if len(nihai) > 130:
        print(f"\n  [i] {len(nihai)} tur planin ~110'unun uzerinde; --esik yukseltilebilir.")
    elif len(nihai) < 90:
        print(f"\n  [i] {len(nihai)} tur planin ~110'unun altinda; --esik dusurulebilir.")


# ── İndirme ────────────────────────────────────────────────────────────────

def komut_download(key, species_basina, only_ab, nd_include):
    rows, _ = csv_oku()
    nihai = [s for s in rows if s["durum"] == "dahil"]
    if not nihai:
        sys.exit("[!] Nihai listede tur yok. Once: python tools/xc_fetch.py --say")

    os.makedirs(XC_DIR, exist_ok=True)
    record_csv = os.path.join(XC_DIR, "kayitlar.csv")
    fresh_file = not os.path.exists(record_csv)

    print(f"{len(nihai)} tur, tur basina en fazla {species_basina} kayit indirilecek.\n")
    total_indi = 0

    with open(record_csv, "a", newline="", encoding="utf-8") as kf:
        w = csv.writer(kf)
        if fresh_file:
            w.writerow(["dosya", "bilimsel_ad", "ebird_kodu", "xc_id",
                        "kalite", "lisans", "kaydeden", "ulke", "sure_sn"])

        total_nd_atlandi = 0

        for s in nihai:
            sci = s["bilimsel_ad"]
            species_code = s["ebird_kodu"] or sci.replace(" ", "_")
            target_directory = os.path.join(XC_DIR, species_code)

            # Kotası zaten dolu olan türü hiç sorgulama. Eskiden her tür için
            # boş dizin açılıyordu; "bu tür yeniden iniyor" izlenimi veriyor
            # ve her turda gereksiz API sorgusu yapılıyordu.
            wav_directory = os.path.join(WAV_DIR, species_code)
            mevcut = 0
            if os.path.isdir(wav_directory):
                mevcut += len([f for f in os.listdir(wav_directory) if f.endswith(".wav")])
            if os.path.isdir(target_directory):
                mevcut += len([f for f in os.listdir(target_directory) if f.endswith(".mp3")])
            if mevcut >= species_basina:
                print(f"  {s['turkce_ad'] or sci}: {mevcut} kayit zaten var, atlandi")
                continue

            os.makedirs(target_directory, exist_ok=True)

            quality = ' q:">C"' if only_ab else ""
            # Kademeli gevşetme: en alakalı/en ucuz kayıtlardan başla, tür
            # başına hedef dolmazsa kısıtları sırayla kaldır.
            #
            #   1. Avrupa + 5-120 sn   üreme bölgesi, kısa kayıt. Uzun kayıtlar
            #                          çoğunlukla sessizlik; hem disk hem
            #                          segmentasyon süresi israfı (ölçüldü:
            #                          ortalama 24 sn'ye karşı 32 sn).
            #   2. Avrupa              az kayıtlı türlerde uzunluk filtresi
            #                          havuzu fazla daraltıyor (İbibik 37).
            #   3. dünya geneli        Avrupa'da kaydı olmayan türler için.
            #   0. Avrupa + 5-60 sn    ÖNCE BU: BirdNET nasılsa 3 sn'lik
            #                          dilimlere bölecek, 2 dakikalık kayda
            #                          ihtiyaç yok. Ölçüldü — uzun kayıtlar
            #                          18 MB'a çıkabiliyor ve indirmeyi
            #                          8 kat yavaşlatıyor.
            sorgular = [
                f'sp:"{sci}"{quality} area:europe len:5-60',
                f'sp:"{sci}"{quality} area:europe len:5-120',
                f'sp:"{sci}"{quality} area:europe',
                f'sp:"{sci}"{quality}',
            ]

            alinan, seen = [], set()
            for sorgu in sorgular:
                sayfa = 1
                while len(alinan) < species_basina:
                    d = xc_sorgu(key, sorgu, sayfa=sayfa, per_page=100)
                    records = d.get("recordings", [])
                    if not records:
                        break
                    for k in records:
                        if k.get("id") in seen:
                            continue
                        seen.add(k.get("id"))
                        alinan.append(k)
                    if sayfa >= int(d.get("numPages", 1)):
                        break
                    sayfa += 1
                    time.sleep(0.34)
                if len(alinan) >= species_basina:
                    break

            indi, nd_atlandi = 0, 0
            for k in alinan:
                if indi >= species_basina:
                    break
                if not nd_include and nd_mi(k.get("lic", "")):
                    nd_atlandi += 1
                    continue
                xc_id = k.get("id", "")
                url = k.get("file", "")
                if not url:
                    continue
                path = os.path.join(target_directory, f"XC{xc_id}.mp3")
                # Çevrilmiş WAV'a da bak: xc_convert.py mp3'ü silip WAV
                # bırakıyor. Yalnızca mp3'e bakılırsa çevrilmiş her tür
                # sıfırdan yeniden inerdi (bir turda 26 GB boşa gidiyordu).
                wav_path = os.path.join(WAV_DIR, os.path.basename(target_directory),
                                       f"XC{xc_id}.wav")
                if os.path.exists(path) or os.path.exists(wav_path):
                    indi += 1
                    continue
                try:
                    req = urllib.request.Request(url, headers=UA)
                    with urllib.request.urlopen(req, timeout=120) as r, \
                         open(path, "wb") as out:
                        out.write(r.read())
                except Exception as e:
                    print(f"    [!] XC{xc_id} indirilemedi: {type(e).__name__}")
                    continue
                w.writerow([os.path.relpath(path, ROOT), sci, s["ebird_kodu"], xc_id,
                            k.get("q", ""), k.get("lic", ""), k.get("rec", ""),
                            k.get("cnt", ""), k.get("length", "")])
                indi += 1
                total_indi += 1
                time.sleep(0.1)

            total_nd_atlandi += nd_atlandi
            print(f"  {s['turkce_ad'] or sci}: {indi} kayit"
                  f"{f' (ND atlandi: {nd_atlandi})' if nd_atlandi else ''}")
            kf.flush()

    print(f"\nToplam {total_indi} yeni kayit -> {XC_DIR}")
    if total_nd_atlandi:
        print(f"ND (turev yasak) lisansli {total_nd_atlandi} kayit atlandi.")
    print(f"Lisans ve atif bilgisi: {record_csv}")


KEY_FILE = os.path.join(DATA, ".xc_key")


def key_bul(parametre):
    """Anahtarı üç kaynaktan sırayla ara. Dosya yöntemi tercih edilir:
    anahtar komut geçmişine ya da ekrana düşmez."""
    if parametre:
        return parametre.strip()
    if os.environ.get("XC_KEY"):
        return os.environ["XC_KEY"].strip()
    if os.path.exists(KEY_FILE):
        with open(KEY_FILE, encoding="utf-8-sig") as f:
            return f.read().strip()
    return ""


def main():
    ap = argparse.ArgumentParser(description="PokeBird M4: Xeno-canto")
    ap.add_argument("--key", default="",
                    help="Xeno-canto API anahtari (yoksa XC_KEY ya da data/.xc_key)")
    ap.add_argument("--count", action="store_true", help="kayit sayilarini cek ve nihai listeyi olustur")
    ap.add_argument("--download", action="store_true", help="nihai listedeki turlerin kayitlarini indir")
    ap.add_argument("--threshold", type=int, default=VARSAYILAN_THRESHOLD,
                    help=f"yaygin turler icin asgari A/B kayit (varsayilan {VARSAYILAN_THRESHOLD})")
    ap.add_argument("--rare-threshold", type=int, default=RARE_THRESHOLD,
                    help=f"nadir turler icin asgari A/B kayit (varsayilan {RARE_THRESHOLD})")
    ap.add_argument("--common-gbif", type=int, default=COMMON_GBIF,
                    help=f"bu kadar GBIF kaydi olan tur 'yaygin' sayilir (varsayilan {COMMON_GBIF})")
    ap.add_argument("--count", type=int, default=60, help="tur basina indirilecek kayit (varsayilan 60)")
    ap.add_argument("--all-quality", action="store_true", help="A/B disinda C/D/E kayitlari da indir")
    ap.add_argument("--nd-include", action="store_true",
                    help="ND (turev yasak) lisansli kayitlari da indir — sorumluluk sizde")
    args = ap.parse_args()

    key = key_bul(args.key)
    if not key:
        sys.exit(
            "\n[!] Xeno-canto API anahtari bulunamadi.\n\n"
            "    API v2 kapandi, v3 anahtar istiyor. Ucretsiz almak icin:\n"
            "      1. https://xeno-canto.org/account adresinde hesap acin\n"
            "      2. Ayni sayfadan API anahtarinizi kopyalayin\n"
            "      3. Anahtari dosyaya yazin (PowerShell):\n"
            "           \"ANAHTAR\" | Out-File -Encoding ascii -NoNewline data\\.xc_key\n\n"
            "    Alternatif:  $env:XC_KEY = \"ANAHTAR\"   ya da   --key ANAHTAR\n")

    if args.count:
        komut_count(key, args.threshold, args.rare_threshold, args.common_gbif)
    elif args.download:
        komut_download(key, args.count, not args.all_quality, args.nd_include)
    else:
        ap.print_help()


if __name__ == "__main__":
    sys.exit(main())
