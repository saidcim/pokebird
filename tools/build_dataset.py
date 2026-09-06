#!/usr/bin/env python3
"""
egitim_kumesi.py — segmentler.csv + WAV'lardan model girdisi uret. (M4 adim 4)

    python tools/build_dataset.py --saglama     # ONCE BUNU CALISTIRIN
    python tools/build_dataset.py

Cikti (data/egitim/):
    pencereler.npy   (N, 187, 64) int8   — modelin girdisi, cihazdakiyle ayni
    etiket.npy       (N,)         int16  — sinif indeksi (178 = negatif)
    ogretmen.npy     (N, 178)     float16— BirdNET yumusak skorlari (damitma)
    ornekler.csv     satir basina kaynak, bolum, bulasik bayragi
    siniflar.csv     indeks -> ebird_kodu / turkce ad
    ozet.txt         bu kosunun raporu

==========================================================================
1. CIHAZLA BIREBIRLIK — bu betigin tek gercek riski
==========================================================================
Cihazdaki `pb_mel_window()` ne uretiyorsa buranin da ayni seyi uretmesi
lazim. Tutmazsa model PC'de iyi, cihazda kotu calisir ve sebep HICBIR YERDE
hata olarak gorunmez (lastsession.md §9i).

Bu yuzden:

  * Mel parametreleri YENIDEN YAZILMADI. `tools/mel_reference.py` zaten bu
    parametrelerin numpy referansi (HTK mel, alan normalizasyonu YOK,
    periyodik Hann, FFT 512) ve M3'te C ile 0.0000 dB sapmayla eslesti.
    Buradaki hizlandirilmis (yiginlanmis) yol ona karsi dogrulaniyor.
  * Kare dizilimi ve pencere normalizasyonu `src/dsp/mel.c`'den birebir
    tasindi — ARADAKI int8 YUVARLAMASI DAHIL. Cihaz her kareyi once
    -90..0 dB araliginda int8'e sikistiriyor, normalizasyonu O
    degerlerden yapiyor. Bu ara adim atlanirsa cikti sessizce kayar.
  * `--saglama` ayni 3 saniyeyi hem buradan hem C kodundan gecirip
    64x187'lik matrisleri karsilastirir (test/dsp_test --pencere).
  * Sabitler `src/dsp/mel.h`'den okunup karsilastiriliyor; biri degisirse
    betik calismayi reddediyor.

==========================================================================
2. BOLME KAYIT BAZINDA — dilim bazinda DEGIL
==========================================================================
Ayni XC kaydinin dilimleri hem egitimde hem dogrulamada olursa model kaydi
ezberler ve dogruluk sahte yukselir. Bolme `dosya` sutununa gore; ustelik
ayni KAYDEDEN kisinin butun kayitlari ayni bolumde (ayni ekipman, ayni
lokasyon, ayni arka plan). Negatiflerde ESC-50'nin kendi 5 katmani
kullaniliyor — zaten sizinti olmasin diye ayrilmislar.

==========================================================================
3. BULASIK DILIMLER
==========================================================================
`en_iyi_tur != hedef` olan dilimde kayittaki baskin ses baska bir kus.
Bunlar:
  * dogrulama ve teste HIC girmiyor (olcum temiz olmali),
  * egitimde `bulasik=1` bayragiyla duruyor ve tam ogretmen vektoru
    yaninda geliyor — yani "yumusak etiketle egit" secenegi acik.
Sessizce hedef etiketiyle egitmek icin ekstra emek gerekir; varsayilan
bunu YAPMIYOR. `--bulasik at` derseniz tamamen dusurulur.

==========================================================================
4. NEGATIF SINIF
==========================================================================
Toplama M8'e ertelendi, SINIF ertelenmedi. Kaynak ESC-50 (bkz.
tools/esc50_download.py, `chirping_birds` cikarilmis). Negatif klipler
BirdNET'ten de gecirildiyse (data/negatif/birdnet_sonuc) icinde kus
duyulanlar ayrica eleniyor.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile
import wave
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mel_reference import (  # noqa: E402  — parametrelerin TEK kaynagi
    DB_MAX, DB_MIN, N_FFT, N_MELS, SR, mel_filterbank, power_spectrum,
    quantize_db,
)

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(KOK, "data")

# src/dsp/mel.h'den; asagida O DOSYAYA KARSI DOGRULANIYOR.
HOP = 384
FRAMES = 187
FMIN, FMAX = 150.0, 11500.0
PENCERE_ORNEK = (FRAMES - 1) * HOP + N_FFT      # 71.936 ornek ≈ 3,00 s

NEGATIF_AD = "__negatif__"

# Pencere ici dB standart sapmasi bunun altindaysa ortada ses yok demektir
# (dijital sessizlik). Cihazdaki normalizasyon var<1e-6 durumunda olcegi
# patlatiyor; boyle bir pencere modele +-127'lik gurultu olarak girer.
SESSIZ_STD_DB = 0.5


# ══ 1. Sabitleri C basligina karsi dogrula ═══════════════════════════════
def sabitleri_dogrula(baslik=os.path.join(KOK, "src", "dsp", "mel.h")):
    """mel.h ile bu betik ayrilirsa DURDUR.

    Bu projede en pahali hata sinifi "sessizce kayan sayi". Cihaz tarafinda
    hop veya bant sayisi degisip burasi guncellenmezse egitim kumesi
    gecersiz olur ve hicbir test bunu soylemez.
    """
    if not os.path.exists(baslik):
        print(f"!! {baslik} yok — sabit karsilastirmasi ATLANDI")
        return
    metin = open(baslik, encoding="utf-8").read()

    def bul(ad):
        m = re.search(rf"#define\s+{ad}\s+\(?(-?[\d.]+)f?\)?", metin)
        return float(m.group(1)) if m else None

    beklenen = {
        "PB_SAMPLE_RATE": SR, "PB_MEL_BANDS": N_MELS, "PB_MEL_HOP": HOP,
        "PB_MEL_FRAMES": FRAMES, "PB_MEL_FMIN": FMIN, "PB_MEL_FMAX": FMAX,
        "PB_MEL_DB_MIN": DB_MIN, "PB_MEL_DB_MAX": DB_MAX,
    }
    sapma = [f"  {k}: mel.h {bul(k)} != betik {v}"
             for k, v in beklenen.items() if bul(k) is not None and bul(k) != v]
    # FFT boyutu ayri baslikta
    fftm = re.search(r"#define\s+PB_FFT_SIZE\s+(\d+)",
                     open(os.path.join(KOK, "src", "dsp", "fft.h"),
                          encoding="utf-8").read())
    if fftm and int(fftm.group(1)) != N_FFT:
        sapma.append(f"  PB_FFT_SIZE: fft.h {fftm.group(1)} != betik {N_FFT}")

    if sapma:
        sys.exit("!! Cihaz sabitleri betikle uyusmuyor — egitim kumesi "
                 "gecersiz olurdu:\n" + "\n".join(sapma))


# ══ 2. Mel penceresi — cihazdaki pb_mel_window()'un birebir karsiligi ════
_FB = None
_HANN = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / N_FFT)


def filtre_bankasi():
    global _FB
    if _FB is None:
        _FB = mel_filterbank()          # mel_reference.py — TEK kaynak
    return _FB


def guc_spektrumu_yigin(kareler):
    """mel_reference.power_spectrum'un yiginlanmis hali. (F, 512) -> (F, 257)

    Tek tek cagirmak 187 kare x 58 bin dilim icin cok yavas. Ayni sonucu
    verdigi `--saglama`'da olculuyor; ayrilirlarsa saglama kalir.
    """
    x = kareler.astype(np.float64)
    x = (x - x.mean(axis=1, keepdims=True)) / 32768.0
    p = np.abs(np.fft.rfft(x * _HANN, axis=1)) ** 2
    gain = 0.5 * N_FFT
    p *= 4.0 / (gain * gain)
    p[:, 0] *= 0.5              # DC ve Nyquist'in negatif frekans esi yok
    p[:, -1] *= 0.5
    return p


def mel_penceresi(ornekler):
    """(>=71936,) int16  ->  (187, 64) int8, ayrica ham dB standart sapmasi.

    Cihazdaki sira (src/dsp/mel.c):
      1. her kare -> guc -> mel -> dB -> int8   (pb_mel_frame)
      2. int8'ler dB'ye GERI cozulur           (pb_mel_q_to_db)
      3. pencere ici ortalama/std, +-4 sigma int8'in tamamina yayilir
    2. adim onemli: ara yuvarlama atlanirsa cikti cihazdakinden kayar.
    """
    x = ornekler[:PENCERE_ORNEK]
    kareler = np.lib.stride_tricks.sliding_window_view(x, N_FFT)[::HOP]
    enerji = guc_spektrumu_yigin(kareler) @ filtre_bankasi().T
    q = quantize_db(10.0 * np.log10(enerji + 1e-10))            # (187, 64)

    db = DB_MIN + (q.astype(np.float64) + 128.0) / 255.0 * (DB_MAX - DB_MIN)
    ort = db.mean()
    var = max(db.var(), 1e-6)
    std = np.sqrt(var)
    z = np.rint((db - ort) * (127.0 / (4.0 * std)))
    return np.clip(z, -128, 127).astype(np.int8), float(std)


# ══ 3. Saglama — C koduyla karsilastir ══════════════════════════════════
def dsp_test_yolu():
    for aday in ("test/build/dsp_test.exe", "test/build/dsp_test"):
        y = os.path.join(KOK, aday)
        if os.path.exists(y):
            return y
    return None


def saglama():
    """Iki bagimsiz karsilastirma. Ikisi de gecmeden egitim kumesi uretmeyin."""
    sabitleri_dogrula()
    print("1) Sabitler mel.h/fft.h ile uyusuyor.\n")

    # --- gercek bir dilim bul (yoksa sentetik) ---
    ornek = None
    seg = os.path.join(DATA, "segmentler.csv")
    if os.path.exists(seg):
        with open(seg, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if float(r["hedef_guven"]) >= 0.9:
                    y = os.path.join(DATA, "wav", r["ebird_kodu"], r["dosya"] + ".wav")
                    if os.path.exists(y):
                        s = wav_oku(y)
                        b = int(round(float(r["baslangic"]) * SR))
                        if len(s) >= b + PENCERE_ORNEK:
                            ornek = s[b:b + PENCERE_ORNEK]
                            print(f"girdi: {r['ebird_kodu']}/{r['dosya']} "
                                  f"@{r['baslangic']}s (guven {r['hedef_guven']})")
                            break
    if ornek is None:
        t = np.arange(PENCERE_ORNEK) / SR
        v = (0.4 * np.sin(2 * np.pi * 1000 * t) + 0.25 * np.sin(2 * np.pi * 4300 * t)
             + 0.1 * ((np.arange(PENCERE_ORNEK) % 97) / 97.0 - 0.5))
        ornek = np.rint(v * 32767).astype(np.int16)
        print("girdi: sentetik (gercek dilim bulunamadi)")

    # --- (a) yiginlanmis yol == mel_reference.power_spectrum ---
    kareler = np.lib.stride_tricks.sliding_window_view(
        ornek[:PENCERE_ORNEK], N_FFT)[::HOP]
    yigin = guc_spektrumu_yigin(kareler)
    tek = np.array([power_spectrum(k) for k in kareler])
    fark = np.abs(yigin - tek).max()
    print(f"\n2) Yiginlanmis guc spektrumu vs mel_reference.power_spectrum")
    print(f"   en buyuk mutlak fark: {fark:.3e}   (beklenen ~0)")
    if fark > 1e-12:
        print("   KALDI: hizlandirilmis yol referanstan ayrilmis.")
        return 1

    # --- (b) Python penceresi == C penceresi ---
    exe = dsp_test_yolu()
    if exe is None:
        print("\n3) dsp_test bulunamadi, C karsilastirmasi ATLANDI. Once:\n"
              "   cmake -S test -B test/build -G Ninja && cmake --build test/build")
        return 1

    py, _ = mel_penceresi(ornek)
    with tempfile.TemporaryDirectory() as d:
        ham = os.path.join(d, "dilim.s16")
        ornek[:PENCERE_ORNEK].astype("<i2").tofile(ham)
        cikti = subprocess.run([exe, "--pencere", ham], capture_output=True,
                               text=True, check=True).stdout

    c = np.zeros((FRAMES, N_MELS), dtype=np.int16)
    for r in csv.DictReader(cikti.splitlines()):
        c[int(r["kare"]), int(r["bant"])] = int(r["q"])

    d = np.abs(c.astype(int) - py.astype(int))
    esit = (d == 0).mean()
    print(f"\n3) Python penceresi vs C penceresi (dsp_test --pencere)")
    print(f"   birebir ayni hucre : %{esit * 100:.2f}")
    print(f"   en buyuk fark      : {d.max()} int8 adimi")
    print(f"   ortalama mutlak    : {d.mean():.4f}")
    print(f"   deger araligi      : C [{c.min()}, {c.max()}]  "
          f"Python [{py.min()}, {py.max()}]")

    # 1 adimlik fark BEKLENEN: C, pencere ortalama/varyansini float32 ile tek
    # gecişte biriktiriyor (mel.c:144). 11.968 degerin kare toplami float32'nin
    # kesin tamsayi araligini asiyor, E[x^2]-E[x]^2 farkinda da sadelesme var;
    # olcek ~1e-4 goreli kayiyor ve yuvarlama sinirindaki hucreler 1 adim
    # oynuyor. int8'in +-4 sigmalik araliginda 1 adim = 0,03 sigma — onemsiz.
    # Python tarafi float64 ile daha DOGRU olan; C'yi degistirmeye gerek yok.
    if d.max() > 1:
        print("\n   KALDI: 1 adimdan buyuk fark var, sebep yuvarlama degil.")
        return 1
    if d.mean() > 0.05:
        print("\n   KALDI: 1 adimlik farklar cok yaygin (>%5).")
        return 1
    print("\nGECTI: egitim kumesi cihazin gordugu ozniteliklerle uyusuyor.")
    return 0


# ══ 3b. Uretilen kumeyi dogrula ═════════════════════════════════════════
def _satir_ses(s):
    """ornekler.csv satiri -> kaynak WAV yolu ve pencere ornekleri."""
    if s["ebird_kodu"] == NEGATIF_AD:
        kayit = os.path.join(DATA, "negatif", "esc50_kayitlar.csv")
        with open(kayit, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if os.path.basename(r["dosya"]) == s["dosya"]:
                    return os.path.join(KOK, r["dosya"])
        return None
    return os.path.join(DATA, "wav", s["ebird_kodu"], s["dosya"] + ".wav")


def cikti_dogrula(out, n):
    """Satir hizasi sagalamasi: dizideki pencere gercekten O satirin sesi mi?

    Sessiz pencereler atlandigi ve dizi sonradan kirpildigi icin `indeks`
    ile dizi satiri arasinda kayma ihtimali var. Kayma olursa her ornek
    yanlis turle etiketlenir ve HICBIR SEY hata vermez — model sadece
    ogrenemez. Bu yuzden kalici bir kip.
    """
    X = np.load(os.path.join(out, "pencereler.npy"), mmap_mode="r")
    y = np.load(os.path.join(out, "etiket.npy"))
    with open(os.path.join(out, "ornekler.csv"), encoding="utf-8") as f:
        satirlar = list(csv.DictReader(f))

    if len(satirlar) != len(X) or len(y) != len(X):
        print(f"KALDI: uzunluklar tutmuyor — csv {len(satirlar)}, "
              f"pencereler {len(X)}, etiket {len(y)}")
        return 1

    # --- (1) SIZINTI: bir kayit birden fazla bolumde olmamali ---
    #
    # Bu kontrol bir kere gercek bir hata yakaladi: bulasik dilimleri
    # dogrulamadan EGITIME TASIYAN kural, 290 kaydi iki bolume birden
    # koymustu. Yani sizintiyi onlemek icin yazilmis kod sizinti uretti.
    # Ucuz kontrol, kalici olsun.
    kayit_bolum = defaultdict(set)
    kisi_bolum = defaultdict(set)
    for s in satirlar:
        if s["ebird_kodu"] == NEGATIF_AD:
            continue
        kayit_bolum[(s["ebird_kodu"], s["dosya"])].add(s["bolum"])
        if s["kaydeden"]:
            kisi_bolum[(s["ebird_kodu"], s["kaydeden"])].add(s["bolum"])
    bol_kayit = [k for k, v in kayit_bolum.items() if len(v) > 1]
    bol_kisi = [k for k, v in kisi_bolum.items() if len(v) > 1]
    print(f"sizinti · birden fazla bolumde olan kayit : {len(bol_kayit)}  (0 olmali)")
    print(f"sizinti · birden fazla bolumde olan kisi  : {len(bol_kisi)}  "
          f"(kaydeden bazinda bolunemeyen turler haric 0)")
    if bol_kayit:
        for k in bol_kayit[:5]:
            print(f"   {k[0]}/{k[1]}: {sorted(kayit_bolum[k])}")
        print("KALDI: ayni kaydin dilimleri birden fazla bolumde — dogruluk "
              "sahte yukselir (§9i tuzak 1).")
        return 1

    # --- (2) satir hizasi ---
    rng = np.random.default_rng(0)
    secim = rng.choice(len(satirlar), size=min(n, len(satirlar)), replace=False)
    ayni = kontrol = 0
    for i in sorted(secim.tolist()):
        s = satirlar[i]
        if int(s["indeks"]) != i:
            print(f"KALDI: satir {i} indeks sutununda {s['indeks']} yaziyor")
            return 1
        yol = _satir_ses(s)
        if not yol or not os.path.exists(yol):
            continue
        bas = int(s["pencere_ornek"])
        p, _ = mel_penceresi(wav_oku(yol)[bas:bas + PENCERE_ORNEK])
        kontrol += 1
        if np.array_equal(p, X[i]):
            ayni += 1
        else:
            d = np.abs(p.astype(int) - X[i].astype(int))
            print(f"!! satir {i} ({s['ebird_kodu']}/{s['dosya']}@{s['baslangic']}) "
                  f"tutmadi — en buyuk fark {d.max()}")

    print(f"{kontrol} satir kaynaktan yeniden cikarildi · birebir ayni: {ayni}")
    if kontrol == 0:
        print("KALDI: hicbir satir dogrulanamadi (kaynak dosyalar yok?)")
        return 1
    if ayni != kontrol:
        print("KALDI: dizi ile csv satirlari hizali degil.")
        return 1
    print("GECTI: her satirin penceresi kendi kaynagindan birebir uretiliyor.")
    return 0


def dinle(out, n):
    """N satirin kaynak sesini WAV olarak yaz — kulakla dogrulamak icin.

    Bu projede dolayli olcum iki kez yaniltti (§5.10). Segmentasyonda tek
    dogrudan gozlem dinlemekti; egitim kumesinde de oyle: dosya adinda tur,
    bolum ve guven yaziyor, dinleyip etiketle karsilastirin.
    """
    with open(os.path.join(out, "ornekler.csv"), encoding="utf-8") as f:
        satirlar = list(csv.DictReader(f))
    d = os.path.join(out, "ornek_ses")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(1)
    for i in rng.choice(len(satirlar), size=min(n, len(satirlar)), replace=False):
        s = satirlar[int(i)]
        yol = _satir_ses(s)
        if not yol or not os.path.exists(yol):
            continue
        bas = int(s["pencere_ornek"])
        ses = wav_oku(yol)[bas:bas + PENCERE_ORNEK]
        ad = (f"{s['indeks']}_{s['ebird_kodu']}_{s['bolum']}"
              f"{'_BULASIK' if s['bulasik'] == '1' else ''}"
              f"_{s['dosya']}_{s['baslangic']}s.wav")
        with wave.open(os.path.join(d, ad), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(ses.tobytes())
        print(f"  {ad}   hedef_guven {s['hedef_guven']}  "
              f"en_iyi {s['en_iyi_tur']}")
    print(f"\n-> {d}   (dinleyip etiketle karsilastirin)")
    return 0


# ══ 4. Yardimcilar ══════════════════════════════════════════════════════
def wav_oku(yol):
    with wave.open(yol, "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (SR, 1, 2):
            raise ValueError(f"{yol}: {w.getframerate()} Hz "
                             f"{w.getnchannels()} kanal {w.getsampwidth() * 8} bit "
                             f"— {SR} Hz mono 16-bit bekleniyordu")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def wav_uzunluk(yol):
    try:
        with wave.open(yol, "rb") as w:
            return w.getnframes()
    except Exception:
        return None


def tur_haritasi():
    yol = os.path.join(DATA, "birdnet_ad_haritasi.csv")
    if not os.path.exists(yol):
        sys.exit(f"ad haritasi yok: {yol} — once tools/birdnet_slist.py (§5.16)")
    with open(yol, encoding="utf-8") as f:
        r = list(csv.DictReader(f))
    return ({x["ebird_kodu"]: x["birdnet_bilimsel_ad"] for x in r},
            {x["ebird_kodu"]: x["turkce_ad"] for x in r},
            {x["ebird_kodu"]: x["bizim_bilimsel_ad"] for x in r})


def kaydedenler():
    """dosya adi (XC kimligi) -> kaydeden. Bolmeyi kisiye gore gruplamak icin."""
    yol = os.path.join(DATA, "xc", "kayitlar.csv")
    if not os.path.exists(yol):
        print(f"!! {yol} yok — bolme yalnizca KAYIT bazinda yapilacak "
              f"(kisi bazinda degil)")
        return {}
    with open(yol, encoding="utf-8") as f:
        return {os.path.splitext(os.path.basename(r["dosya"]))[0]: r["kaydeden"]
                for r in csv.DictReader(f)}


def ogretmen_oku(yol, sinif_indeks, ad_sinif):
    """BirdNET ham sonucu -> {(bas, bit): {sinif_indeksi: guven}}"""
    d = defaultdict(dict)
    if not os.path.exists(yol):
        return d
    with open(yol, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            i = ad_sinif.get(r["Scientific name"])
            if i is not None:
                anahtar = (round(float(r["Start (s)"]), 1), round(float(r["End (s)"]), 1))
                d[anahtar][i] = float(r["Confidence"])
    return d


def bolumle(gruplar, dogrulama_pay, test_pay):
    """Gruplari (kayit/kisi) bolumlere dagit — DILIM bazinda DEGIL.

    En buyuk gruptan baslayip her grubu, o an hedefinden en cok geride olan
    bolume verir. Belirlenimci: tohum gerekmiyor, ayni girdi ayni bolmeyi
    uretir.

    NEDEN "sirayla doldur" DEGIL: ilk deneme gruplari karistirip once test
    kotasini dolduruyordu. Olculdu — eurbla'nin 573 diliminin 399'u TEK bir
    kaydedene ait; o grup teste dusunce bolme %11/%21/%68 oldu. Buyukten
    kucuge + en buyuk acigi kapat kurali ayni grubu egitime koyuyor ve
    oranlar tutuyor. Kaydeden bazinda gruplamayi bozmadan cozuyor.
    """
    toplam = sum(len(v) for v in gruplar.values())
    hedef = {"egitim": toplam * (1.0 - dogrulama_pay - test_pay),
             "dogrulama": toplam * dogrulama_pay,
             "test": toplam * test_pay}
    mevcut = dict.fromkeys(hedef, 0)
    atama = {}
    for anahtar, dilimler in sorted(gruplar.items(),
                                    key=lambda kv: (-len(kv[1]), str(kv[0]))):
        b = max(hedef, key=lambda k: hedef[k] - mevcut[k])
        atama[anahtar] = b
        mevcut[b] += len(dilimler)
    return atama


def tur_bolumle(kisi_gruplari, dosya_gruplari, dogrulama_pay, test_pay):
    """Bir turu bolumle. Once kaydeden bazinda; olmuyorsa kayit bazinda.

    Kaydeden bazinda gruplamak dogrusu (ayni kisi = ayni ekipman, lokasyon,
    arka plan). Ama bir turun kayitlarinin cogu tek kisiye aitse o tur icin
    dogrulama ya da test bos kalabiliyor — o zaman tur hic olculemez hale
    gelir. Boyle turlerde KAYIT bazina dusuluyor (§9i tuzak 1'in asil
    sarti — ayni kaydin dilimleri hep ayni bolumde — her hâlükârda korunur)
    ve hangi turlerde boyle yapildigi rapora yaziliyor.
    """
    atama = bolumle(kisi_gruplari, dogrulama_pay, test_pay)
    say = defaultdict(int)
    for anahtar, idx in kisi_gruplari.items():
        say[atama[anahtar]] += len(idx)
    if say["dogrulama"] and say["test"]:
        return kisi_gruplari, atama, False
    if len(dosya_gruplari) < 3:
        return kisi_gruplari, atama, False      # bolunecek malzeme yok
    return (dosya_gruplari,
            bolumle(dosya_gruplari, dogrulama_pay, test_pay), True)


# ══ 5. Ana akis ═════════════════════════════════════════════════════════
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--saglama", action="store_true",
                    help="C koduyla birebirligi olc ve cik (once bunu calistirin)")
    ap.add_argument("--esik", type=float, default=0.25,
                    help="hedef_guven alt siniri (§9e: 0.25'te tur basina 327)")
    ap.add_argument("--dogrulama", type=float, default=0.15)
    ap.add_argument("--test", type=float, default=0.10)
    ap.add_argument("--bulasik", choices=("egit", "at"), default="egit",
                    help="en_iyi_tur != hedef olan dilimler: egitimde bayrakli "
                         "tut (varsayilan) ya da tamamen dusur")
    ap.add_argument("--negatif", default=os.path.join(DATA, "negatif"))
    ap.add_argument("--negatif-pencere", type=int, default=2,
                    help="her ESC-50 klibinden kac pencere (klip 5 sn)")
    ap.add_argument("--negatif-kus-esik", type=float, default=0.25,
                    help="BirdNET bu guvenle kus duyduysa klip negatiften atilir")
    ap.add_argument("--out", default=os.path.join(DATA, "egitim"))
    ap.add_argument("--dogrula-cikti", type=int, metavar="N",
                    help="uretilmis kumeden N satir sec, kaynaktan yeniden "
                         "cikarip diziyle karsilastir (satir hizasi sagalamasi)")
    ap.add_argument("--dinle", type=int, metavar="N",
                    help="N satirin kaynak sesini WAV olarak yaz — kulakla "
                         "dogrulamak icin")
    ap.add_argument("--tur", nargs="*", help="yalnizca bu ebird kodlari (deneme)")
    ap.add_argument("--sinir", type=int, help="en fazla bu kadar dilim (duman testi)")
    a = ap.parse_args()

    if a.saglama:
        return saglama()
    if a.dogrula_cikti:
        return cikti_dogrula(a.out, a.dogrula_cikti)
    if a.dinle:
        return dinle(a.out, a.dinle)

    sabitleri_dogrula()
    birdnet_ad, turkce, _ = tur_haritasi()
    kodlar = sorted(birdnet_ad)
    sinif_indeks = {k: i for i, k in enumerate(kodlar)}
    ad_sinif = {birdnet_ad[k]: sinif_indeks[k] for k in kodlar}
    negatif_indeks = len(kodlar)
    kisi = kaydedenler()

    # ---- 5.1 dilim listesi ----
    seg = os.path.join(DATA, "segmentler.csv")
    if not os.path.exists(seg):
        sys.exit(f"{seg} yok — once tools/birdnet_summary.py")

    uzunluk = {}
    aday = []            # (kod, dosya, baslangic, hedef_guven, bulasik, en_iyi..)
    esik_alti = kayip_wav = kisa = 0
    with open(seg, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            kod = r["ebird_kodu"]
            if a.tur and kod not in a.tur:
                continue
            if kod not in sinif_indeks:
                continue
            if float(r["hedef_guven"]) < a.esik:
                esik_alti += 1
                continue
            yol = os.path.join(DATA, "wav", kod, r["dosya"] + ".wav")
            if yol not in uzunluk:
                uzunluk[yol] = wav_uzunluk(yol)
            n = uzunluk[yol]
            if n is None:
                kayip_wav += 1
                continue
            if n < PENCERE_ORNEK:
                kisa += 1
                continue
            # Dilim sonu dosyayi asiyorsa pencereyi SOLA kaydir: dilimin sesi
            # yine pencerenin icinde kalir, veri kaybedilmez. Sifir doldurmak
            # yerine bu tercih edildi — sifir bandi mel'de -90 dB'lik yapay
            # bir blok yapar ve pencere normalizasyonunu bozar.
            bas = int(round(float(r["baslangic"]) * SR))
            bas = max(0, min(bas, n - PENCERE_ORNEK))
            bulasik = int(r["en_iyi_tur"] != birdnet_ad[kod])
            if bulasik and a.bulasik == "at":
                continue
            aday.append((kod, r["dosya"], bas, float(r["baslangic"]),
                         float(r["hedef_guven"]), bulasik,
                         r["en_iyi_tur"], float(r["en_iyi_guven"] or 0)))
            if a.sinir and len(aday) >= a.sinir:
                break

    print(f"esik {a.esik}: {len(aday)} dilim  "
          f"(esik alti {esik_alti}, wav yok {kayip_wav}, kisa dosya {kisa})")
    if not aday:
        sys.exit("hic dilim kalmadi")

    # ---- 5.2 bolme: tur ici, KAYIT/KISI bazinda ----
    kisi_gr = defaultdict(lambda: defaultdict(list))
    dosya_gr = defaultdict(lambda: defaultdict(list))
    for i, (kod, dosya, *_r) in enumerate(aday):
        kisi_gr[kod][kisi.get(dosya) or f"__dosya__{dosya}"].append(i)
        dosya_gr[kod][dosya].append(i)

    bolum_of = {}
    kayit_bazina_dusen = []
    for kod in kisi_gr:
        g, atama, dustu = tur_bolumle(kisi_gr[kod], dosya_gr[kod],
                                      a.dogrulama, a.test)
        if dustu:
            kayit_bazina_dusen.append(kod)
        for anahtar, idx in g.items():
            for i in idx:
                # Bulasik dilim olcume girmez: dogrulama/test temiz olmali.
                #
                # !! ONEMLI: bu dilimleri EGITIME TASIMAYIN, DUSURUN. Ilk
                # surum tasiyordu ve olculdu: 290 kayit hem egitimde hem
                # dogrulamada cikti — yani §9i tuzak 1'in ta kendisi, hem de
                # onu onlemek icin yazilmis kod yuzunden. Kaydin bir dilimi
                # bile karsi tarafa gecerse bolme bozulur.
                bolum_of[i] = (None if (aday[i][5] and atama[anahtar] != "egitim")
                               else atama[anahtar])

    # ---- 5.3 negatifler ----
    neg = negatif_listesi(a, ad_sinif)
    toplam = len(aday) + len(neg)
    print(f"negatif: {len(neg)} pencere")
    print(f"toplam : {toplam} pencere  "
          f"({toplam * FRAMES * N_MELS / 1e6:.0f} MB)")

    # ---- 5.4 cikarim ----
    os.makedirs(a.out, exist_ok=True)
    X = np.lib.format.open_memmap(os.path.join(a.out, "pencereler.npy"), mode="w+",
                                  dtype=np.int8, shape=(toplam, FRAMES, N_MELS))
    y = np.zeros(toplam, dtype=np.int16)
    T = np.zeros((toplam, len(kodlar)), dtype=np.float16)
    satirlar = []
    sessiz = 0
    bulasik_dusen = 0
    yaz = 0

    dosyaya_gore = defaultdict(list)
    for i, s in enumerate(aday):
        dosyaya_gore[(s[0], s[1])].append(i)

    for sayac, ((kod, dosya), idx) in enumerate(sorted(dosyaya_gore.items()), 1):
        try:
            ses = wav_oku(os.path.join(DATA, "wav", kod, dosya + ".wav"))
        except Exception as e:
            print(f"!! okunamadi {kod}/{dosya}: {e}")
            continue
        ogr = ogretmen_oku(os.path.join(DATA, "birdnet_sonuc", kod,
                                        dosya + ".BirdNET.results.csv"),
                           sinif_indeks, ad_sinif)
        for i in idx:
            if bolum_of[i] is None:      # olcum kumesindeki bulasik dilim
                bulasik_dusen += 1
                continue
            _, _, bas, bas_sn, guven, bulasik, en_iyi, en_iyi_g = aday[i]
            p, std = mel_penceresi(ses[bas:bas + PENCERE_ORNEK])
            if std < SESSIZ_STD_DB:
                sessiz += 1
                continue
            X[yaz] = p
            y[yaz] = sinif_indeks[kod]
            for j, c in ogr.get((round(bas_sn, 1), round(bas_sn + 3.0, 1)), {}).items():
                T[yaz, j] = c
            satirlar.append({
                "indeks": yaz, "sinif": sinif_indeks[kod], "ebird_kodu": kod,
                "turkce_ad": turkce[kod], "dosya": dosya,
                "baslangic": f"{bas_sn:.1f}", "pencere_ornek": bas,
                "bolum": bolum_of[i], "bulasik": bulasik,
                "hedef_guven": f"{guven:.4f}", "en_iyi_tur": en_iyi,
                "en_iyi_guven": f"{en_iyi_g:.4f}",
                "kaydeden": kisi.get(dosya, ""),
            })
            yaz += 1
        if sayac % 500 == 0:
            print(f"  {sayac}/{len(dosyaya_gore)} kayit · {yaz} pencere", flush=True)

    # negatifler
    for yol, ofset, bolum, kategori in neg:
        try:
            ses = wav_oku(yol)
        except Exception as e:
            print(f"!! negatif okunamadi {yol}: {e}")
            continue
        if len(ses) < ofset + PENCERE_ORNEK:
            continue
        p, std = mel_penceresi(ses[ofset:ofset + PENCERE_ORNEK])
        if std < SESSIZ_STD_DB:
            sessiz += 1
            continue
        X[yaz] = p
        y[yaz] = negatif_indeks
        satirlar.append({
            "indeks": yaz, "sinif": negatif_indeks, "ebird_kodu": NEGATIF_AD,
            "turkce_ad": kategori, "dosya": os.path.basename(yol),
            "baslangic": f"{ofset / SR:.1f}", "pencere_ornek": ofset,
            "bolum": bolum, "bulasik": 0, "hedef_guven": "",
            "en_iyi_tur": "", "en_iyi_guven": "", "kaydeden": "ESC-50",
        })
        yaz += 1

    # Gercek satir sayisina kirp (sessiz pencereler dusuruldu).
    X.flush()
    del X
    if yaz != toplam:
        eski = np.load(os.path.join(a.out, "pencereler.npy"), mmap_mode="r")
        yeni = np.lib.format.open_memmap(
            os.path.join(a.out, "pencereler.tmp.npy"), mode="w+",
            dtype=np.int8, shape=(yaz, FRAMES, N_MELS))
        for i in range(0, yaz, 4096):
            son = min(i + 4096, yaz)
            yeni[i:son] = eski[i:son]
        yeni.flush()
        del yeni, eski
        os.replace(os.path.join(a.out, "pencereler.tmp.npy"),
                   os.path.join(a.out, "pencereler.npy"))
    np.save(os.path.join(a.out, "etiket.npy"), y[:yaz])
    np.save(os.path.join(a.out, "ogretmen.npy"), T[:yaz])

    with open(os.path.join(a.out, "ornekler.csv"), "w", encoding="utf-8",
              newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(satirlar[0].keys()))
        w.writeheader()
        w.writerows(satirlar)

    with open(os.path.join(a.out, "siniflar.csv"), "w", encoding="utf-8",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["sinif", "ebird_kodu", "turkce_ad", "birdnet_bilimsel_ad"])
        for k in kodlar:
            w.writerow([sinif_indeks[k], k, turkce[k], birdnet_ad[k]])
        w.writerow([negatif_indeks, NEGATIF_AD, "negatif / bilinmiyor", ""])

    rapor(a, satirlar, kodlar, turkce, negatif_indeks, sessiz, yaz,
          kayit_bazina_dusen, bulasik_dusen)
    return 0


def negatif_listesi(a, ad_sinif):
    """ESC-50 kliplerinden (yol, ofset, bolum, kategori) listesi.

    Bolme ESC-50'nin kendi katmanlarina gore: fold 5 test, fold 4 dogrulama,
    1-3 egitim. Katmanlar zaten ayni Freesound kaydindan gelen klipler ayni
    katmanda kalacak sekilde ayrilmis — kayit sizintisina karsi dogru olan bu.
    """
    kayit = os.path.join(a.negatif, "esc50_kayitlar.csv")
    if not os.path.exists(kayit):
        print(f"\n!! NEGATIF SINIF BOS: {kayit} yok.\n"
              f"   Once:  python tools/esc50_download.py\n"
              f"   Negatif sinif doldurulmazsa model her sesi bir kusa atar "
              f"(§9f-4). Yine de devam ediliyor.\n")
        return []

    # BirdNET negatiflerde kus duydu mu? (istege bagli ama cok degerli)
    kirli = {}
    sonuc_kok = os.path.join(a.negatif, "birdnet_sonuc")
    if os.path.isdir(sonuc_kok):
        for kok, _, dosyalar in os.walk(sonuc_kok):
            for d in dosyalar:
                if not d.endswith(".BirdNET.results.csv"):
                    continue
                en = 0.0
                with open(os.path.join(kok, d), encoding="utf-8") as f:
                    for r in csv.DictReader(f):
                        if r["Scientific name"] in ad_sinif:
                            en = max(en, float(r["Confidence"]))
                kirli[d.replace(".BirdNET.results.csv", ".wav")] = en
        print(f"negatif BirdNET taramasi: {len(kirli)} klip okundu")
    else:
        print(f"!! {sonuc_kok} yok — negatifler kus icerigine karsi TARANMADI.\n"
              f"   Onerilen:  .venv-birdnet\\Scripts\\python tools/birdnet_run.py "
              f"--girdi data/negatif/wav --out data/negatif/birdnet_sonuc")

    out, elenen = [], 0
    with open(kayit, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            ad = os.path.basename(r["dosya"])
            if kirli.get(ad, 0.0) >= a.negatif_kus_esik:
                elenen += 1
                continue
            fold = r.get("fold", "")
            bolum = {"5": "test", "4": "dogrulama"}.get(fold, "egitim")
            yol = os.path.join(KOK, r["dosya"])
            klip = wav_uzunluk(yol) or 0
            if klip < PENCERE_ORNEK:
                continue
            n = max(1, a.negatif_pencere)
            for k in range(n):
                ofset = 0 if n == 1 else int(round(k * (klip - PENCERE_ORNEK) / (n - 1)))
                out.append((yol, ofset, bolum, r["kategori"]))
    if elenen:
        print(f"negatif: kus duyulan {elenen} klip elendi "
              f"(esik {a.negatif_kus_esik})")
    return out


def rapor(a, satirlar, kodlar, turkce, negatif_indeks, sessiz, toplam,
          kayit_bazina_dusen, bulasik_dusen):
    bolum_say = defaultdict(int)
    tur_bolum = defaultdict(lambda: defaultdict(int))
    bulasik_say = defaultdict(int)
    for s in satirlar:
        bolum_say[s["bolum"]] += 1
        tur_bolum[s["ebird_kodu"]][s["bolum"]] += 1
        if s["bulasik"]:
            bulasik_say[s["ebird_kodu"]] += 1

    var = [k for k in kodlar if tur_bolum[k]]        # bu kosuda yer alan turler
    yok = [k for k in kodlar if not tur_bolum[k]]

    satir = []
    satir.append(f"esik {a.esik} · bulasik={a.bulasik} · bolme belirlenimci")
    satir.append(f"toplam pencere : {toplam}")
    for b in ("egitim", "dogrulama", "test"):
        satir.append(f"  {b:10s} {bolum_say[b]:7d}  "
                     f"%{100 * bolum_say[b] / max(toplam, 1):.1f}")
    satir.append(f"tur sayisi                 : {len(var)}")
    satir.append(f"sessiz diye atilan pencere : {sessiz}")
    satir.append(f"bulasik (yalniz egitimde)  : {sum(bulasik_say.values())}")
    satir.append(f"bulasik oldugu icin dusen  : {bulasik_dusen}  "
                 f"(dogrulama/test grubuna dusmuslerdi)")
    nb = tur_bolum[NEGATIF_AD]
    satir.append(f"negatif (sinif {negatif_indeks})      : "
                 f"{sum(nb.values())}  (egitim {nb['egitim']}, "
                 f"dog {nb['dogrulama']}, test {nb['test']})")

    if yok:
        satir.append(f"\n!! HIC PENCERESI OLMAYAN TUR: {len(yok)}")
        satir.append("  " + ", ".join(yok[:20]))
    eksik = [k for k in var if not tur_bolum[k]["dogrulama"]]
    if eksik:
        satir.append(f"\n!! dogrulama kumesi BOS olan tur: {len(eksik)}")
        satir.append("  " + ", ".join(eksik))
    if kayit_bazina_dusen:
        satir.append(f"\nkaydeden bazinda bolunemeyen tur: "
                     f"{len(kayit_bazina_dusen)} (kayit bazina dusuldu; "
                     f"ayni kaydin dilimleri yine ayni bolumde)")
        satir.append("  " + ", ".join(sorted(kayit_bazina_dusen)))

    zayif = sorted((sum(tur_bolum[k].values()), k) for k in var)
    satir.append("\nen az pencereli 12 tur (focal loss + veri artirma bunlari "
                 "hedeflemeli, §9i-4):")
    for n, k in zayif[:12]:
        b = tur_bolum[k]
        satir.append(f"  {k:10s} {turkce[k]:26s} {n:5d}  "
                     f"(egitim {b['egitim']}, dog {b['dogrulama']}, test {b['test']}, "
                     f"bulasik {bulasik_say[k]})")
    say = [sum(tur_bolum[k].values()) for k in var]
    satir.append(f"\ntur basina pencere: en az {min(say)} · ortanca "
                 f"{int(np.median(say))} · en cok {max(say)}")

    metin = "\n".join(satir)
    print("\n" + metin)
    with open(os.path.join(a.out, "ozet.txt"), "w", encoding="utf-8") as f:
        f.write(metin + "\n")
    print(f"\n-> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
