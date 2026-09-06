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
import csv_compat  # noqa: E402
from mel_reference import (  # noqa: E402  — parametrelerin TEK kaynagi
    DB_MAX, DB_MIN, N_FFT, N_MELS, SR, mel_filterbank, power_spectrum,
    quantize_db,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")

# src/dsp/mel.h'den; asagida O DOSYAYA KARSI DOGRULANIYOR.
HOP = 384
FRAMES = 187
FMIN, FMAX = 150.0, 11500.0
WINDOW_SAMPLES = (FRAMES - 1) * HOP + N_FFT      # 71.936 ornek ≈ 3,00 s

NEGATIVE_CODE = "__negative__"

# Pencere ici dB standart sapmasi bunun altindaysa ortada ses yok demektir
# (dijital sessizlik). Cihazdaki normalizasyon var<1e-6 durumunda olcegi
# patlatiyor; boyle bir pencere modele +-127'lik gurultu olarak girer.
SESSIZ_STD_DB = 0.5


# ══ 1. Sabitleri C basligina karsi dogrula ═══════════════════════════════
def sabitleri_verify(title=os.path.join(ROOT, "src", "dsp", "mel.h")):
    """mel.h ile bu betik ayrilirsa DURDUR.

    Bu projede en pahali hata sinifi "sessizce kayan sayi". Cihaz tarafinda
    hop veya bant sayisi degisip burasi guncellenmezse egitim kumesi
    gecersiz olur ve hicbir test bunu soylemez.
    """
    if not os.path.exists(title):
        print(f"!! {title} yok — sabit karsilastirmasi ATLANDI")
        return
    text = open(title, encoding="utf-8").read()

    def bul(name):
        m = re.search(rf"#define\s+{name}\s+\(?(-?[\d.]+)f?\)?", text)
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
                     open(os.path.join(ROOT, "src", "dsp", "fft.h"),
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


def guc_spektrumu_batch(kareler):
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


def mel_penceresi(samples):
    """(>=71936,) int16  ->  (187, 64) int8, ayrica ham dB standart sapmasi.

    Cihazdaki sira (src/dsp/mel.c):
      1. her kare -> guc -> mel -> dB -> int8   (pb_mel_frame)
      2. int8'ler dB'ye GERI cozulur           (pb_mel_q_to_db)
      3. pencere ici ortalama/std, +-4 sigma int8'in tamamina yayilir
    2. adim onemli: ara yuvarlama atlanirsa cikti cihazdakinden kayar.
    """
    x = samples[:WINDOW_SAMPLES]
    kareler = np.lib.stride_tricks.sliding_window_view(x, N_FFT)[::HOP]
    enerji = guc_spektrumu_batch(kareler) @ filtre_bankasi().T
    q = quantize_db(10.0 * np.log10(enerji + 1e-10))            # (187, 64)

    db = DB_MIN + (q.astype(np.float64) + 128.0) / 255.0 * (DB_MAX - DB_MIN)
    ort = db.mean()
    var = max(db.var(), 1e-6)
    std = np.sqrt(var)
    z = np.rint((db - ort) * (127.0 / (4.0 * std)))
    return np.clip(z, -128, 127).astype(np.int8), float(std)


# ══ 3. Saglama — C koduyla karsilastir ══════════════════════════════════
def dsp_test_yolu():
    for candidate in ("test/build/dsp_test.exe", "test/build/dsp_test"):
        y = os.path.join(ROOT, candidate)
        if os.path.exists(y):
            return y
    return None


def checksum():
    """Iki bagimsiz karsilastirma. Ikisi de gecmeden egitim kumesi uretmeyin."""
    sabitleri_verify()
    print("1) Sabitler mel.h/fft.h ile uyusuyor.\n")

    # --- gercek bir dilim bul (yoksa sentetik) ---
    sample = None
    seg = os.path.join(DATA, "segments.csv")
    if os.path.exists(seg):
        with open(seg, encoding="utf-8") as f:
            for r in csv_compat.reader(f):
                if float(r["target_confidence"]) >= 0.9:
                    y = os.path.join(DATA, "wav", r["ebird_code"], r["file"] + ".wav")
                    if os.path.exists(y):
                        s = wav_oku(y)
                        b = int(round(float(r["start"]) * SR))
                        if len(s) >= b + WINDOW_SAMPLES:
                            sample = s[b:b + WINDOW_SAMPLES]
                            print(f"girdi: {r['ebird_kodu']}/{r['dosya']} "
                                  f"@{r['baslangic']}s (guven {r['hedef_guven']})")
                            break
    if sample is None:
        t = np.arange(WINDOW_SAMPLES) / SR
        v = (0.4 * np.sin(2 * np.pi * 1000 * t) + 0.25 * np.sin(2 * np.pi * 4300 * t)
             + 0.1 * ((np.arange(WINDOW_SAMPLES) % 97) / 97.0 - 0.5))
        sample = np.rint(v * 32767).astype(np.int16)
        print("girdi: sentetik (gercek dilim bulunamadi)")

    # --- (a) yiginlanmis yol == mel_reference.power_spectrum ---
    kareler = np.lib.stride_tricks.sliding_window_view(
        sample[:WINDOW_SAMPLES], N_FFT)[::HOP]
    batch = guc_spektrumu_batch(kareler)
    tek = np.array([power_spectrum(k) for k in kareler])
    fark = np.abs(batch - tek).max()
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

    py, _ = mel_penceresi(sample)
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "dilim.s16")
        sample[:WINDOW_SAMPLES].astype("<i2").tofile(raw)
        output = subprocess.run([exe, "--window", raw], capture_output=True,
                               text=True, check=True).stdout

    c = np.zeros((FRAMES, N_MELS), dtype=np.int16)
    for r in csv_compat.reader(output.splitlines()):
        c[int(r["frame"]), int(r["band"])] = int(r["q"])

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
def _row_audio(s):
    """ornekler.csv satiri -> kaynak WAV yolu ve pencere ornekleri."""
    if s["ebird_code"] == NEGATIVE_CODE:
        record = os.path.join(DATA, "negatif", "esc50_kayitlar.csv")
        with open(record, encoding="utf-8") as f:
            for r in csv_compat.reader(f):
                if os.path.basename(r["file"]) == s["file"]:
                    return os.path.join(ROOT, r["file"])
        return None
    return os.path.join(DATA, "wav", s["ebird_code"], s["file"] + ".wav")


def output_verify(out, n):
    """Satir hizasi sagalamasi: dizideki pencere gercekten O satirin sesi mi?

    Sessiz pencereler atlandigi ve dizi sonradan kirpildigi icin `indeks`
    ile dizi satiri arasinda kayma ihtimali var. Kayma olursa her ornek
    yanlis turle etiketlenir ve HICBIR SEY hata vermez — model sadece
    ogrenemez. Bu yuzden kalici bir kip.
    """
    X = np.load(csv_compat.resolve(os.path.join(out, "windows.npy")), mmap_mode="r")
    y = np.load(csv_compat.resolve(os.path.join(out, "labels.npy")))
    with open(csv_compat.resolve(os.path.join(out, "samples.csv")), encoding="utf-8") as f:
        rows = list(csv_compat.reader(f))

    if len(rows) != len(X) or len(y) != len(X):
        print(f"KALDI: uzunluklar tutmuyor — csv {len(rows)}, "
              f"pencereler {len(X)}, etiket {len(y)}")
        return 1

    # --- (1) SIZINTI: bir kayit birden fazla bolumde olmamali ---
    #
    # Bu kontrol bir kere gercek bir hata yakaladi: bulasik dilimleri
    # dogrulamadan EGITIME TASIYAN kural, 290 kaydi iki bolume birden
    # koymustu. Yani sizintiyi onlemek icin yazilmis kod sizinti uretti.
    # Ucuz kontrol, kalici olsun.
    record_split = defaultdict(set)
    kisi_split = defaultdict(set)
    for s in rows:
        if s["ebird_code"] == NEGATIVE_CODE:
            continue
        record_split[(s["ebird_code"], s["file"])].add(s["split"])
        if s["recordist"]:
            kisi_split[(s["ebird_code"], s["recordist"])].add(s["split"])
    bol_record = [k for k, v in record_split.items() if len(v) > 1]
    bol_kisi = [k for k, v in kisi_split.items() if len(v) > 1]
    print(f"sizinti · birden fazla bolumde olan kayit : {len(bol_record)}  (0 olmali)")
    print(f"sizinti · birden fazla bolumde olan kisi  : {len(bol_kisi)}  "
          f"(kaydeden bazinda bolunemeyen turler haric 0)")
    if bol_record:
        for k in bol_record[:5]:
            print(f"   {k[0]}/{k[1]}: {sorted(record_split[k])}")
        print("KALDI: ayni kaydin dilimleri birden fazla bolumde — dogruluk "
              "sahte yukselir (§9i tuzak 1).")
        return 1

    # --- (2) satir hizasi ---
    rng = np.random.default_rng(0)
    selection = rng.choice(len(rows), size=min(n, len(rows)), replace=False)
    ayni = kontrol = 0
    for i in sorted(selection.tolist()):
        s = rows[i]
        if int(s["index"]) != i:
            print(f"KALDI: satir {i} indeks sutununda {s['indeks']} yaziyor")
            return 1
        path = _row_audio(s)
        if not path or not os.path.exists(path):
            continue
        start = int(s["window_samples"])
        p, _ = mel_penceresi(wav_oku(path)[start:start + WINDOW_SAMPLES])
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


def listen(out, n):
    """N satirin kaynak sesini WAV olarak yaz — kulakla dogrulamak icin.

    Bu projede dolayli olcum iki kez yaniltti (§5.10). Segmentasyonda tek
    dogrudan gozlem dinlemekti; egitim kumesinde de oyle: dosya adinda tur,
    bolum ve guven yaziyor, dinleyip etiketle karsilastirin.
    """
    with open(csv_compat.resolve(os.path.join(out, "samples.csv")), encoding="utf-8") as f:
        rows = list(csv_compat.reader(f))
    d = os.path.join(out, "ornek_ses")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(1)
    for i in rng.choice(len(rows), size=min(n, len(rows)), replace=False):
        s = rows[int(i)]
        path = _row_audio(s)
        if not path or not os.path.exists(path):
            continue
        start = int(s["window_samples"])
        audio = wav_oku(path)[start:start + WINDOW_SAMPLES]
        name = (f"{s['indeks']}_{s['ebird_kodu']}_{s['bolum']}"
              f"{'_BULASIK' if s['bulasik'] == '1' else ''}"
              f"_{s['dosya']}_{s['baslangic']}s.wav")
        with wave.open(os.path.join(d, name), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(audio.tobytes())
        print(f"  {name}   hedef_guven {s['hedef_guven']}  "
              f"en_iyi {s['en_iyi_tur']}")
    print(f"\n-> {d}   (dinleyip etiketle karsilastirin)")
    return 0


# ══ 4. Yardimcilar ══════════════════════════════════════════════════════
def wav_oku(path):
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (SR, 1, 2):
            raise ValueError(f"{path}: {w.getframerate()} Hz "
                             f"{w.getnchannels()} kanal {w.getsampwidth() * 8} bit "
                             f"— {SR} Hz mono 16-bit bekleniyordu")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def wav_length(path):
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes()
    except Exception:
        return None


def species_haritasi():
    path = os.path.join(DATA, "birdnet_ad_haritasi.csv")
    if not os.path.exists(path):
        sys.exit(f"ad haritasi yok: {path} — once tools/birdnet_slist.py (§5.16)")
    with open(path, encoding="utf-8") as f:
        r = list(csv_compat.reader(f))
    return ({x["ebird_code"]: x["birdnet_scientific_name"] for x in r},
            {x["ebird_code"]: x["turkish_name"] for x in r},
            {x["ebird_code"]: x["our_scientific_name"] for x in r})


def kaydedenler():
    """dosya adi (XC kimligi) -> kaydeden. Bolmeyi kisiye gore gruplamak icin."""
    path = os.path.join(DATA, "xc", "kayitlar.csv")
    if not os.path.exists(path):
        print(f"!! {path} yok — bolme yalnizca KAYIT bazinda yapilacak "
              f"(kisi bazinda degil)")
        return {}
    with open(path, encoding="utf-8") as f:
        return {os.path.splitext(os.path.basename(r["file"]))[0]: r["recordist"]
                for r in csv_compat.reader(f)}


def ogretmen_oku(path, cls_indeks, name_cls):
    """BirdNET ham sonucu -> {(bas, bit): {sinif_indeksi: guven}}"""
    d = defaultdict(dict)
    if not os.path.exists(path):
        return d
    with open(path, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            i = name_cls.get(r["Scientific name"])
            if i is not None:
                key = (round(float(r["Start (s)"]), 1), round(float(r["End (s)"]), 1))
                d[key][i] = float(r["Confidence"])
    return d


def bolumle(gruplar, val_pay, test_pay):
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
    total = sum(len(v) for v in gruplar.values())
    target = {"egitim": total * (1.0 - val_pay - test_pay),
             "dogrulama": total * val_pay,
             "test": total * test_pay}
    mevcut = dict.fromkeys(target, 0)
    assignment = {}
    for key, slices in sorted(gruplar.items(),
                                    key=lambda kv: (-len(kv[1]), str(kv[0]))):
        b = max(target, key=lambda k: target[k] - mevcut[k])
        assignment[key] = b
        mevcut[b] += len(slices)
    return assignment


def species_bolumle(kisi_gruplari, file_gruplari, val_pay, test_pay):
    """Bir turu bolumle. Once kaydeden bazinda; olmuyorsa kayit bazinda.

    Kaydeden bazinda gruplamak dogrusu (ayni kisi = ayni ekipman, lokasyon,
    arka plan). Ama bir turun kayitlarinin cogu tek kisiye aitse o tur icin
    dogrulama ya da test bos kalabiliyor — o zaman tur hic olculemez hale
    gelir. Boyle turlerde KAYIT bazina dusuluyor (§9i tuzak 1'in asil
    sarti — ayni kaydin dilimleri hep ayni bolumde — her hâlükârda korunur)
    ve hangi turlerde boyle yapildigi rapora yaziliyor.
    """
    assignment = bolumle(kisi_gruplari, val_pay, test_pay)
    count = defaultdict(int)
    for key, idx in kisi_gruplari.items():
        count[assignment[key]] += len(idx)
    if count["dogrulama"] and count["test"]:
        return kisi_gruplari, assignment, False
    if len(file_gruplari) < 3:
        return kisi_gruplari, assignment, False      # bolunecek malzeme yok
    return (file_gruplari,
            bolumle(file_gruplari, val_pay, test_pay), True)


# ══ 5. Ana akis ═════════════════════════════════════════════════════════
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checksum", action="store_true",
                    help="C koduyla birebirligi olc ve cik (once bunu calistirin)")
    ap.add_argument("--threshold", type=float, default=0.25,
                    help="hedef_guven alt siniri (§9e: 0.25'te tur basina 327)")
    ap.add_argument("--val", type=float, default=0.15)
    ap.add_argument("--test", type=float, default=0.10)
    ap.add_argument("--contaminated", choices=("egit", "at"), default="egit",
                    help="en_iyi_tur != hedef olan dilimler: egitimde bayrakli "
                         "tut (varsayilan) ya da tamamen dusur")
    ap.add_argument("--negative", default=os.path.join(DATA, "negatif"))
    ap.add_argument("--negative-window", type=int, default=2,
                    help="her ESC-50 klibinden kac pencere (klip 5 sn)")
    ap.add_argument("--negative-bird-threshold", type=float, default=0.25,
                    help="BirdNET bu guvenle kus duyduysa klip negatiften atilir")
    ap.add_argument("--out", default=os.path.join(DATA, "egitim"))
    ap.add_argument("--verify-output", type=int, metavar="N",
                    help="uretilmis kumeden N satir sec, kaynaktan yeniden "
                         "cikarip diziyle karsilastir (satir hizasi sagalamasi)")
    ap.add_argument("--listen", type=int, metavar="N",
                    help="N satirin kaynak sesini WAV olarak yaz — kulakla "
                         "dogrulamak icin")
    ap.add_argument("--species", nargs="*", help="yalnizca bu ebird kodlari (deneme)")
    ap.add_argument("--limit", type=int, help="en fazla bu kadar dilim (duman testi)")
    a = ap.parse_args()

    if a.checksum:
        return checksum()
    if a.verify_output:
        return output_verify(a.out, a.verify_output)
    if a.listen:
        return listen(a.out, a.listen)

    sabitleri_verify()
    birdnet_name, turkce, _ = species_haritasi()
    kodlar = sorted(birdnet_name)
    cls_indeks = {k: i for i, k in enumerate(kodlar)}
    name_cls = {birdnet_name[k]: cls_indeks[k] for k in kodlar}
    negative_indeks = len(kodlar)
    kisi = kaydedenler()

    # ---- 5.1 dilim listesi ----
    seg = os.path.join(DATA, "segments.csv")
    if not os.path.exists(seg):
        sys.exit(f"{seg} yok — once tools/birdnet_summary.py")

    length = {}
    candidate = []            # (kod, dosya, baslangic, hedef_guven, bulasik, en_iyi..)
    threshold_alti = loss_wav = kisa = 0
    with open(seg, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            code = r["ebird_code"]
            if a.species and code not in a.species:
                continue
            if code not in cls_indeks:
                continue
            if float(r["target_confidence"]) < a.threshold:
                threshold_alti += 1
                continue
            path = os.path.join(DATA, "wav", code, r["file"] + ".wav")
            if path not in length:
                length[path] = wav_length(path)
            n = length[path]
            if n is None:
                loss_wav += 1
                continue
            if n < WINDOW_SAMPLES:
                kisa += 1
                continue
            # Dilim sonu dosyayi asiyorsa pencereyi SOLA kaydir: dilimin sesi
            # yine pencerenin icinde kalir, veri kaybedilmez. Sifir doldurmak
            # yerine bu tercih edildi — sifir bandi mel'de -90 dB'lik yapay
            # bir blok yapar ve pencere normalizasyonunu bozar.
            start = int(round(float(r["start"]) * SR))
            start = max(0, min(start, n - WINDOW_SAMPLES))
            contaminated = int(r["best_species"] != birdnet_name[code])
            if contaminated and a.contaminated == "at":
                continue
            candidate.append((code, r["file"], start, float(r["start"]),
                         float(r["target_confidence"]), contaminated,
                         r["best_species"], float(r["best_confidence"] or 0)))
            if a.limit and len(candidate) >= a.limit:
                break

    print(f"esik {a.threshold}: {len(candidate)} dilim  "
          f"(esik alti {threshold_alti}, wav yok {loss_wav}, kisa dosya {kisa})")
    if not candidate:
        sys.exit("hic dilim kalmadi")

    # ---- 5.2 bolme: tur ici, KAYIT/KISI bazinda ----
    kisi_gr = defaultdict(lambda: defaultdict(list))
    file_gr = defaultdict(lambda: defaultdict(list))
    for i, (code, file, *_r) in enumerate(candidate):
        kisi_gr[code][kisi.get(file) or f"__dosya__{file}"].append(i)
        file_gr[code][file].append(i)

    split_of = {}
    record_bazina_dusen = []
    for code in kisi_gr:
        g, assignment, dustu = species_bolumle(kisi_gr[code], file_gr[code],
                                      a.val, a.test)
        if dustu:
            record_bazina_dusen.append(code)
        for key, idx in g.items():
            for i in idx:
                # Bulasik dilim olcume girmez: dogrulama/test temiz olmali.
                #
                # !! ONEMLI: bu dilimleri EGITIME TASIMAYIN, DUSURUN. Ilk
                # surum tasiyordu ve olculdu: 290 kayit hem egitimde hem
                # dogrulamada cikti — yani §9i tuzak 1'in ta kendisi, hem de
                # onu onlemek icin yazilmis kod yuzunden. Kaydin bir dilimi
                # bile karsi tarafa gecerse bolme bozulur.
                split_of[i] = (None if (candidate[i][5] and assignment[key] != "egitim")
                               else assignment[key])

    # ---- 5.3 negatifler ----
    neg = negative_listesi(a, name_cls)
    total = len(candidate) + len(neg)
    print(f"negatif: {len(neg)} pencere")
    print(f"toplam : {total} pencere  "
          f"({total * FRAMES * N_MELS / 1e6:.0f} MB)")

    # ---- 5.4 cikarim ----
    os.makedirs(a.out, exist_ok=True)
    X = np.lib.format.open_memmap(os.path.join(a.out, "windows.npy"), mode="w+",
                                  dtype=np.int8, shape=(total, FRAMES, N_MELS))
    y = np.zeros(total, dtype=np.int16)
    T = np.zeros((total, len(kodlar)), dtype=np.float16)
    rows = []
    sessiz = 0
    contaminated_dusen = 0
    write = 0

    dosyaya_gore = defaultdict(list)
    for i, s in enumerate(candidate):
        dosyaya_gore[(s[0], s[1])].append(i)

    for sayac, ((code, file), idx) in enumerate(sorted(dosyaya_gore.items()), 1):
        try:
            audio = wav_oku(os.path.join(DATA, "wav", code, file + ".wav"))
        except Exception as e:
            print(f"!! okunamadi {code}/{file}: {e}")
            continue
        ogr = ogretmen_oku(os.path.join(DATA, "birdnet_sonuc", code,
                                        file + ".BirdNET.results.csv"),
                           cls_indeks, name_cls)
        for i in idx:
            if split_of[i] is None:      # olcum kumesindeki bulasik dilim
                contaminated_dusen += 1
                continue
            _, _, start, start_sn, guven, contaminated, best, max_iyi_g = candidate[i]
            p, std = mel_penceresi(audio[start:start + WINDOW_SAMPLES])
            if std < SESSIZ_STD_DB:
                sessiz += 1
                continue
            X[write] = p
            y[write] = cls_indeks[code]
            for j, c in ogr.get((round(start_sn, 1), round(start_sn + 3.0, 1)), {}).items():
                T[write, j] = c
            rows.append({
                "index": write, "class_index": cls_indeks[code], "ebird_code": code,
                "turkish_name": turkce[code], "file": file,
                "start": f"{start_sn:.1f}", "window_samples": start,
                "split": split_of[i], "bulasik": contaminated,
                "target_confidence": f"{guven:.4f}", "best_species": best,
                "best_confidence": f"{max_iyi_g:.4f}",
                "recordist": kisi.get(file, ""),
            })
            write += 1
        if sayac % 500 == 0:
            print(f"  {sayac}/{len(dosyaya_gore)} kayit · {write} pencere", flush=True)

    # negatifler
    for path, ofset, split, kategori in neg:
        try:
            audio = wav_oku(path)
        except Exception as e:
            print(f"!! negatif okunamadi {path}: {e}")
            continue
        if len(audio) < ofset + WINDOW_SAMPLES:
            continue
        p, std = mel_penceresi(audio[ofset:ofset + WINDOW_SAMPLES])
        if std < SESSIZ_STD_DB:
            sessiz += 1
            continue
        X[write] = p
        y[write] = negative_indeks
        rows.append({
            "index": write, "class_index": negative_indeks, "ebird_code": NEGATIVE_CODE,
            "turkish_name": kategori, "file": os.path.basename(path),
            "start": f"{ofset / SR:.1f}", "window_samples": ofset,
            "split": split, "bulasik": 0, "target_confidence": "",
            "best_species": "", "best_confidence": "", "recordist": "ESC-50",
        })
        write += 1

    # Gercek satir sayisina kirp (sessiz pencereler dusuruldu).
    X.flush()
    del X
    if write != total:
        old = np.load(csv_compat.resolve(os.path.join(a.out, "windows.npy")), mmap_mode="r")
        fresh = np.lib.format.open_memmap(
            os.path.join(a.out, "pencereler.tmp.npy"), mode="w+",
            dtype=np.int8, shape=(write, FRAMES, N_MELS))
        for i in range(0, write, 4096):
            last = min(i + 4096, write)
            fresh[i:last] = old[i:last]
        fresh.flush()
        del fresh, old
        os.replace(os.path.join(a.out, "pencereler.tmp.npy"),
                   os.path.join(a.out, "windows.npy"))
    np.save(os.path.join(a.out, "labels.npy"), y[:write])
    np.save(os.path.join(a.out, "teacher.npy"), T[:write])

    with open(csv_compat.resolve(os.path.join(a.out, "samples.csv")), "w", encoding="utf-8",
              newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    with open(csv_compat.resolve(os.path.join(a.out, "classes.csv")), "w", encoding="utf-8",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["class_index", "ebird_code", "turkish_name", "birdnet_scientific_name"])
        for k in kodlar:
            w.writerow([cls_indeks[k], k, turkce[k], birdnet_name[k]])
        w.writerow([negative_indeks, NEGATIVE_CODE, "negatif / bilinmiyor", ""])

    rapor(a, rows, kodlar, turkce, negative_indeks, sessiz, write,
          record_bazina_dusen, contaminated_dusen)
    return 0


def negative_listesi(a, name_cls):
    """ESC-50 kliplerinden (yol, ofset, bolum, kategori) listesi.

    Bolme ESC-50'nin kendi katmanlarina gore: fold 5 test, fold 4 dogrulama,
    1-3 egitim. Katmanlar zaten ayni Freesound kaydindan gelen klipler ayni
    katmanda kalacak sekilde ayrilmis — kayit sizintisina karsi dogru olan bu.
    """
    record = os.path.join(a.negative, "esc50_kayitlar.csv")
    if not os.path.exists(record):
        print(f"\n!! NEGATIF SINIF BOS: {record} yok.\n"
              f"   Once:  python tools/esc50_download.py\n"
              f"   Negatif sinif doldurulmazsa model her sesi bir kusa atar "
              f"(§9f-4). Yine de devam ediliyor.\n")
        return []

    # BirdNET negatiflerde kus duydu mu? (istege bagli ama cok degerli)
    dirty = {}
    result_root = os.path.join(a.negative, "birdnet_sonuc")
    if os.path.isdir(result_root):
        for root, _, files in os.walk(result_root):
            for d in files:
                if not d.endswith(".BirdNET.results.csv"):
                    continue
                highest = 0.0
                with open(os.path.join(root, d), encoding="utf-8") as f:
                    for r in csv_compat.reader(f):
                        if r["Scientific name"] in name_cls:
                            highest = max(highest, float(r["Confidence"]))
                dirty[d.replace(".BirdNET.results.csv", ".wav")] = highest
        print(f"negatif BirdNET taramasi: {len(dirty)} klip okundu")
    else:
        print(f"!! {result_root} yok — negatifler kus icerigine karsi TARANMADI.\n"
              f"   Onerilen:  .venv-birdnet\\Scripts\\python tools/birdnet_run.py "
              f"--girdi data/negatif/wav --out data/negatif/birdnet_sonuc")

    out, elenen = [], 0
    with open(record, encoding="utf-8") as f:
        for r in csv_compat.reader(f):
            name = os.path.basename(r["file"])
            if dirty.get(name, 0.0) >= a.negative_bird_threshold:
                elenen += 1
                continue
            fold = r.get("fold", "")
            split = {"5": "test", "4": "dogrulama"}.get(fold, "egitim")
            path = os.path.join(ROOT, r["file"])
            klip = wav_length(path) or 0
            if klip < WINDOW_SAMPLES:
                continue
            n = max(1, a.negative_window)
            for k in range(n):
                ofset = 0 if n == 1 else int(round(k * (klip - WINDOW_SAMPLES) / (n - 1)))
                out.append((path, ofset, split, r["category"]))
    if elenen:
        print(f"negatif: kus duyulan {elenen} klip elendi "
              f"(esik {a.negative_bird_threshold})")
    return out


def rapor(a, rows, kodlar, turkce, negative_indeks, sessiz, total,
          record_bazina_dusen, contaminated_dusen):
    split_count = defaultdict(int)
    species_split = defaultdict(lambda: defaultdict(int))
    contaminated_count = defaultdict(int)
    for s in rows:
        split_count[s["split"]] += 1
        species_split[s["ebird_code"]][s["split"]] += 1
        if s["bulasik"]:
            contaminated_count[s["ebird_code"]] += 1

    var = [k for k in kodlar if species_split[k]]        # bu kosuda yer alan turler
    yok = [k for k in kodlar if not species_split[k]]

    row = []
    row.append(f"esik {a.threshold} · bulasik={a.contaminated} · bolme belirlenimci")
    row.append(f"toplam pencere : {total}")
    for b in ("egitim", "dogrulama", "test"):
        row.append(f"  {b:10s} {split_count[b]:7d}  "
                     f"%{100 * split_count[b] / max(total, 1):.1f}")
    row.append(f"tur sayisi                 : {len(var)}")
    row.append(f"sessiz diye atilan pencere : {sessiz}")
    row.append(f"bulasik (yalniz egitimde)  : {sum(contaminated_count.values())}")
    row.append(f"bulasik oldugu icin dusen  : {contaminated_dusen}  "
                 f"(dogrulama/test grubuna dusmuslerdi)")
    nb = species_split[NEGATIVE_CODE]
    row.append(f"negatif (sinif {negative_indeks})      : "
                 f"{sum(nb.values())}  (egitim {nb['egitim']}, "
                 f"dog {nb['dogrulama']}, test {nb['test']})")

    if yok:
        row.append(f"\n!! HIC PENCERESI OLMAYAN TUR: {len(yok)}")
        row.append("  " + ", ".join(yok[:20]))
    missing = [k for k in var if not species_split[k]["dogrulama"]]
    if missing:
        row.append(f"\n!! dogrulama kumesi BOS olan tur: {len(missing)}")
        row.append("  " + ", ".join(missing))
    if record_bazina_dusen:
        row.append(f"\nkaydeden bazinda bolunemeyen tur: "
                     f"{len(record_bazina_dusen)} (kayit bazina dusuldu; "
                     f"ayni kaydin dilimleri yine ayni bolumde)")
        row.append("  " + ", ".join(sorted(record_bazina_dusen)))

    zayif = sorted((sum(species_split[k].values()), k) for k in var)
    row.append("\nen az pencereli 12 tur (focal loss + veri artirma bunlari "
                 "hedeflemeli, §9i-4):")
    for n, k in zayif[:12]:
        b = species_split[k]
        row.append(f"  {k:10s} {turkce[k]:26s} {n:5d}  "
                     f"(egitim {b['egitim']}, dog {b['dogrulama']}, test {b['test']}, "
                     f"bulasik {contaminated_count[k]})")
    count = [sum(species_split[k].values()) for k in var]
    row.append(f"\ntur basina pencere: en az {min(count)} · ortanca "
                 f"{int(np.median(count))} · en cok {max(count)}")

    text = "\n".join(row)
    print("\n" + text)
    with open(csv_compat.resolve(os.path.join(a.out, "summary.txt")), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(f"\n-> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
