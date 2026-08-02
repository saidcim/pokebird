# PokeBird — Oturum Devir Teslimi

> Bu dosya, yeni bir Claude oturumunun projeyi sıfırdan anlayıp kaldığı yerden
> devam edebilmesi için yazıldı. Mimari planın tamamı [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
> içinde; burada onun özeti, şu ana kadar yapılanlar, **denenip işe yaramayanlar**
> ve sıradaki adımlar var.
>
> Son güncelleme: 2 Ağustos 2026 (M6 kapanışı).

## ⚠ ÖNCE BUNU OKUYUN

**Durum:** M0 ✅ · M1 ✅ · M2a ✅ · M2b 🔶 (dokunmatik park edildi) · M3 ✅ ·
M4 ✅ · M5 🔶 (Aşama-2 tür ağı bitti; Aşama-1 ve Aşama-3 kaldı) ·
**M6 ✅ (§9m)**

Çalışma ağacı temiz, her şey commit edildi (§10). **Dal `m6`**, `main` değil.

> ### ⛔ EKRAN BOZUK — ve bu M6'DAN GELMİYOR, ÖLÇÜLDÜ
>
> `o` testi (dört köşeye dört renk) kartta şunu veriyor: **ekran
> temizlenmiyor ve yalnızca EN SON çizilen kare görünüyor.** Aynı belirti `a`
> demosunda da var (yazı tipi bozuk, spektrogram yok).
>
> **M6 öncesi `main` derlemesi kartta denendi ve AYNI ŞEKİLDE bozuk.**
> Yani hata M6'dan önce de vardı; bu oturumda yalnızca *fark edildi*.
> Ayrıntı, elenen ihtimaller ve sıradaki adım §9n'de.
>
> **KÖK NEDEN BULUNDU (§9n):** veri yolu hiçbir zaman sorun değildi.
> `QSPI_WaitIdle` sağlam (ölçüldü, zaman aşımı 0), yol ve saat hızı de
> elendi (`y`: altı bileşim de birebir aynı). Asıl sorun: **bu panel
> `0x2B` (RASET) komutunu YOK SAYIYOR.** Panelin çalışan iki bağımsız
> sürücüsü de (rsvpnano ESP32 + RP2350-PIO) RASET'i hiç yollamıyor;
> satır konumu `0x2C` (RAMWR = en üste dön) ve `0x3C` (RAMWRC = kaldığın
> yerden devam) ile belirleniyor. Bizim her kısmi çizimimiz satır 0'a
> düşüyordu.
>
> **`z` ile doğrulandı ve sürücü düzeltildi.** `pb_lcd_fill` tek geçişe indi
> (2560 CS işlemi → 2, 27,2 → 11,8 ms), `pb_lcd_blit` imleç takip ediyor,
> `o` tek geçişte çiziliyor, LVGL'in kirli alanı sola yayılıyor. **Açık
> kalan:** `a` demosunun spektrogramı (LVGL ile sıra alınca imleç kayıyor).
> Onu `j` testinin sonucu belirleyecek — **göz gerekiyor.**
>
> Nasıl gözden kaçmış: §9h'de kullanıcının gözle onayladığı ikili **20 ms**
> sürümüydü; gönderilen **250 ms** sürümüne hiç bakılmadı ve o oturumun
> sonundaki *"yeni oturumda ilk iş `--cmd a` ile 10 saniyelik bir bakış
> atın"* notu yerine getirilmedi. **Göz gerektiren ölçütü ertelemeyin**
> (§5.10'un aynısı, üçüncü kez).

**M3 KAPANDI (1 Ağustos 2026):** Sürekli yakalama (kendini yenileyen DMA
halka tamponu) yazıldı ve kartta doğrulandı: **62–63 kare/s, kayıp 0**
(hedef 62.5; eski bloklayan yakalama 57'de kalıyordu). Ayrıntı §9c.
Ayrıca `a` komutu eklendi: şimdiye kadarki her şeyi tek ekranda çalıştıran
demo — LVGL kart + canlı mel spektrogramı + kapı. **Kullanıcı ekranda
doğruladı:** kart doğru, spektrogram akıyor, ses çıkınca "SES ALGILANDI"
yeşil yanıyor. Bu demo M2b'nin açık 6. maddesini de (spektrogram + LVGL
bir arada) kapattı.

**M4 veri toplama ✅ BİTTİ (§9d).** Eğitim verisi hazır ve doğrulandı:

```
178 tür · 7.111 WAV · 13,2 GB · ~79 saat ses
24 kHz mono 16-bit (30 rastgele dosyada format sağlaması 30/30 doğru)
172 tür tam 40 kayıt · 6 tür 39 · Küçük Kartal 37 (XC'de o kadar var)
```

**M4 BirdNET segmentasyonu ✅ BİTTİ (§9e).** Dört araç yazıldı, 79 saatlik
sesin tamamı tarandı:

```
178 tür · 95.033 dilim tarandı · data/segmentler.csv (79.932 satır)
hedef güven >=0.25: 58.151 dilim = tür başına 327   (kabul ölçütü >=100)
0.25'te 100 dilimin altında kalan yalnızca 6 tür (üçü balıkçıl)
```

**Kullanıcı kararı (2 Ağustos 2026): negatif TOPLAMA askıya alındı.** İlk
doğrulama testi boş odada bilgisayardan kuş sesi çalarak yapılacak; o
senaryoda şehir gürültüsü yok. Odak tür tanımada. Ayrıntı ve *ama*'sı
§9f-4'te — negatif **sınıfı** yine de bir şeyle doldurulmak zorunda.

**`s_capture` temizliği ✅ BİTTİ (§9g)** ve açığa çıkardığı **ekran
gerilemesi de ✅ ÇÖZÜLDÜ (§9h).**

```
bss  218.988  ->  127.084 bayt     (91.904 bayt serbest, hedef <=130.000)
arena (180 KB) ile birlikte 311.404  ->  520 KB SRAM'de ~208 KB pay
```

Beş kabul ölçütünün beşi de karşılandı. Kartta HEAD duruyor, çalışıyor.

**M4 ADIM 4 — EĞİTİM KÜMESİ ✅ BİTTİ (§9j).** Model girdisi üretildi ve
cihazın gördüğü özniteliklerle **ölçülerek** eşleştiği doğrulandı:

```
data/egitim/pencereler.npy   64x187 int8 pencereler
tools/egitim_kumesi.py --saglama   Python penceresi == C penceresi
                                   %99,49 hucre birebir, en buyuk fark 1 int8 adimi
bolme KAYIT+KAYDEDEN bazinda · bulasik dilimler bayrakli, olcume girmiyor
negatif sinif ESC-50'den dolduruldu (kus siniflari cikarildi)
```

**M5 AŞAMA-2 TÜR AĞI ✅ BİTTİ (§9k).** Model eğitildi, INT8'e indirildi ve
cihaza hazır:

```
models/tur_agi_int8.tflite   270 KB · 209.107 parametre · 6,6 MMAC (butce 30)
models/tur_agi_int8.h        firmware'in derleyecegi C dizisi
TEST  INT8  top-1 %58,07  top-3 %74,82        (pencere basina)
      birlestirme 8 pencere -> top-1 %70,40  top-3 %82,20   <- kullanicinin gordugu
girdi olcegi 1.000000 / sifir noktasi 0  ->  cihaz mel penceresini DOGRUDAN besler
```

**M6 ✅ BİTTİ (§9m).** Model cihazda çalışıyor ve PC ile **birebir aynı**:

```
TFLM + CMSIS-NN vendor edildi (cmake/tflm.cmake, surumler sabit)
arena     110.436 bayt OLCULDU   (butce 180 KB; §9k'daki "141 KB" tahmindi)
cikarim   190 ms                 (hedef: 1 s'lik pencere adimina sigmak)
dogrulama 8/8 pencere BIREBIR ayni logit  <- "PC'de calisiyor cihazda
                                             calismiyor" riski KAPANDI
core1     62,6 kare/s, overrun 0, birlestirme 8 pencere
```

**SIRADAKİ İŞ — ekran hatası (§9n).** M6'nın çıktısını kullanıcıya
göstermenin önündeki tek engel bu ve M7'nin tamamı ona bağlı.

Geriye kalan küçük işler: Aşama-1 ikili ağ, Aşama-3 mevsim tablosu (§9k sonu).

---

## 1. Proje nedir

Waveshare **RP2350-Touch-LCD-3.49** kartı üzerinde çalışan, internet gerektirmeyen,
kuş sesinden tür tanıyan cep cihazı. BirdNET'in yaptığı işin İstanbul'a daraltılmış
ve 150 MHz'lik bir mikrodenetleyiciye sığdırılmış hâli.

**Neden daraltma:** BirdNET GLOBAL 6K ~6000 sınıf, 48 kHz girdi, onlarca MB. Bu
cihazda 520 KB SRAM var. İstanbul'a özgü ~110 tür hem sınıf sayısını 50 kat
düşürüyor hem de sınıf başına eğitim verisini artırdığı için küçük modelin
doğruluğunu yükseltiyor.

### Kullanıcıyla kilitlenen kararlar

| Karar | Seçim |
|---|---|
| Tür kapsamı | ~110 tür (yerleşik + düzenli üreyen + yaygın göçmen) |
| Kullanım | Elde taşınan, anlık tanıma; ekran sürekli açık |
| Yazılım yığını | Pico SDK + CMake (C/C++) |
| Model eğitimi | BirdNET öğretmen → küçük modele damıtma (distillation) |
| RTC | **Kullanılmıyor** — pil yedeklemesi yok, saati kaybediyor. Mevsim önceliği için açılışta tarih sorulacak |
| IMU | **Kullanılmıyor** — projeye katkısı yok, gereksiz karmaşıklık |

### Kullanıcının çalışma tarzı hakkında

Kullanıcı bring-up ayrıntılarında uzun süre takılmaktan hoşlanmıyor; asıl
hedef (kuş tanıma) gecikirse bunu açıkça söylüyor. **Kritik yolda olmayan
işler için tur harcamayın** — dokunmatik tam bu yüzden park edildi (§9b).
Kritik yol: M3 → M4 → M5 → M6.

---

## 2. Donanım gerçeği (şematikten doğrulandı)

Waveshare'in resmi şeması (`RP2350-Touch-LCD-3.49.pdf`) PyMuPDF ile çözümlenip
koordinat bazlı okundu. **Tahmin yok.** Kaynak: [`src/board_config.h`](src/board_config.h)

| GPIO | Sinyal | Not |
|---|---|---|
| 0 | `NS_MODE` | NS4150B hoparlör amfi mod/enable |
| 1 | `I2S_DSDIN` | MCU → codec DAC (ses çalma) |
| 2 | `I2S_DSOUT` | codec ADC → MCU (**mikrofon verisi**) |
| 3 | `I2S_MCLK` | PIO ile üretilir |
| 4 | `I2S_SCLK` | BCLK — ES8311 master olarak üretir |
| 5 | `I2S_LRCK` | WS — ES8311 master olarak üretir |
| 6 / 7 | `SDA` / `SCL` | Paylaşımlı I2C1: ES8311 + IMU + RTC |
| 8 / 9 / 10 | IMU_INT1/2, RTC_INT | *kullanılmıyor* |
| 11 | `TP_INT` | Dokunmatik kesme |
| **12–19** | **boş** | P3 başlığı (8 pin) |
| 20–25 | `LCD_SCL`, `LCD_D0..D3`, `LCD_CS` | AXS15231B QSPI |
| 26–31 | `SD_SCLK`, `SD_MOSI`, `SD_MISO`, `SD_D1`, `SD_D2`, `SD_CS` | |
| 32 / 33 | `TP_SDA` / `TP_SCL` | Dokunmatik **ayrı** I2C0 hattı |
| 34–37 | `LCD_RST`, `LCD_TE`, `LCD_BL`, `BL_EN` | |
| 38 / 39 | `SYS_OUT` / `SYS_EN` | Güç mandalı |
| 40 | `BAT_ADC` | Pil voltajı (bölücü 1.5) |
| **41–47** | **boş** | J3/J6 başlığı (7 pin) |

**Kritik kısıtlar:**
- RP2350**B** (48 GPIO), 2× Cortex-M33 @ 150 MHz, FPU + DSP
- **520 KB SRAM, PSRAM YOK** ← en sert kısıt
- 16 MB flash (PY25Q128HA)
- Ekran 172×640; tam framebuffer RGB565'te **220 KB** = SRAM'in %42'si → **tam framebuffer kullanmıyoruz**
- Kart üzerinde analog MEMS mikrofon (`MIC1`) → ES8311 ADC. Harici mikrofon gerekmiyor.
- Hoparlör çıkışı var (NS4150B + MX1.25 konnektör H1)
- **Yazılımdan kontrol edilebilir LED YOK** (tek LED şarj durum LED'i, GPIO'ya bağlı değil)

**Bağımsız teyit:** Waveshare'in resmi board başlığı
(`waveshare_rp2350_touch_lcd_3.49.h`) ve LVGL örneğinin `qspi_pio.h`'si
şematikten çıkardığımız her değerle birebir tutuyor: RP2350A=0, I2C1 6/7,
flash 16 MB, LCD_BL 36, dokunmatik 32/33/11 (adres 0x3B), BAT_ADC 40,
QSPI CS 25 / SCLK 20 / D0-D3 21-24, LCD_RST 34, PWR_EN 37.

**PSRAM yok.** Satıcı örneğindeki `lib/PSRAM/` başka karttan kopyalanmış ölü
kod; `rp_setup_psram()` hiç çağrılmıyor. 520 KB bütçe geçerli.

---

## 3. Geliştirme ortamı — pratik bilgiler

### Firmware derleme

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Çıktı: `build/pokebird.uf2`

> **`-S .`'i atlamayın.** `cmake -B build` kaynak dizin olarak ÇALIŞMA DİZİNİNİ
> kullanıyor. Kabuk yanlışlıkla `third_party/lvgl` içindeyken çalıştırılırsa
> CMake LVGL'i üst proje sanıp yapılandırıyor, ana toolchain hiç okunmuyor ve
> hata "LVGL'in lv_conf.h'yi bulamaması" gibi görünüyor. Bu tuzak bir oturumda
> saatlerce yanlış yerde arattı. Ayrıca `third_party/lvgl/build/` diye başıboş
> bir dizin bırakıyor; görürseniz silin.

### Host tarafı DSP testleri — DSP'ye dokunmadan önce bunları çalıştırın

```bash
cmake -S test -B test/build -G Ninja && cmake --build test/build
./test/build/dsp_test               # 13 test, 0 kaldi
python tools/mel_reference.py       # 64 bandin tamaminda sapma 0.0000 dB
python tools/egitim_kumesi.py --saglama   # PC egitim hatti == cihaz hatti
```

Saniyeler sürüyor ve şimdiden iki gerçek hata yakaladı (§9c). Mikrodenetleyicide
DSP hatası ayıklamak çok pahalı; **DSP değişikliklerini önce burada doğrulayın.**

`dsp_test`'in üçüncü kipi **`--pencere <ham.s16>`**: ham int16 dosyasından
modelin gerçek girdisini (64×187 int8, `pb_mel_window()` çıktısı) döküyor.
`--dump` yalnızca tek kareyi karşılaştırıyordu; kare dizilimi (hop 384) ve
pencere normalizasyonu onun dışında kalıyordu. Eğitim kümesi betiği kendini
buna karşı doğruluyor (§9j).

### Bağımlılıklar

- CMake + Ninja: winlibs mingw64 paketinden, PATH'te
- ARM GCC 14.3: `~/.platformio/packages/toolchain-rp2040-earlephilhower` — [`cmake/toolchain.cmake`](cmake/toolchain.cmake) otomatik buluyor
- Pico SDK 2.3.0 ve LVGL 9.3.0: `third_party/` altına klonlu, **git'e girmiyor**
- Python 3.14, pyserial 3.5, numpy 2.4 kurulu (librosa YOK, gerekmiyor)
- **`.venv-birdnet/`** — BirdNET-Analyzer 2.4.0 + TensorFlow 2.21 için ayrı
  **Python 3.11** ortamı (3.14'te kurulmuyor). Git'e girmiyor. Kurulumdaki
  iki tuzak §5.13 (pip sertifikası) ve §5.14 (model indirme).
  `tools/birdnet_run.py` ve `birdnet_slist.py` **bu ortamın** python'uyla
  çalışır; `birdnet_ozet.py` ve `segment_kes.py` saf stdlib, 3.14 yeter.

Yoksa:
```bash
git clone -b 2.3.0 --depth 1 https://github.com/raspberrypi/pico-sdk.git third_party/pico-sdk
git -C third_party/pico-sdk submodule update --init --depth 1 lib/tinyusb
git clone -b v9.3.0 --depth 1 https://github.com/lvgl/lvgl.git third_party/lvgl
```

### LVGL yapılandırması — iki tuzak

Ayarlar [`src/ui/lv_conf.h`](src/ui/lv_conf.h)'de; şablonun tamamı kopyalanmadı,
yalnızca varsayılandan sapan satırlar var.

- CMake değişkeninin adı **`LV_BUILD_CONF_PATH`**. Belgelerde geçen
  `LV_CONF_PATH`, LVGL'in ürettiği derleyici makrosunun adı; CMake'e verilirse
  sessizce yok sayılır.
- Bir bileşeni kapatırken **bağımlısını da** kapatmak gerekiyor, yoksa LVGL
  `#error` ile duruyor (`LV_USE_TEXTAREA 0` → `LV_USE_SPINBOX 0` şart).
  Demolar/örnekler CMake tarafında ayrı hedefler; `CONFIG_LV_BUILD_DEMOS` ve
  `CONFIG_LV_BUILD_EXAMPLES` OFF yapılmazsa derleniyor ve patlıyorlar.

### Karta yükleme — düğmeye basmadan

Kart çalışırken 1200 baud'da porta bağlanmak onu BOOTSEL'e sokuyor:

```powershell
try { $p = New-Object System.IO.Ports.SerialPort 'COM13',1200,'None',8,'one'; $p.Open(); Start-Sleep -Milliseconds 250; $p.Close() } catch {}
Start-Sleep -Seconds 5
$v = Get-Volume | Where-Object { $_.FileSystemLabel -like 'RP2350*' }
Copy-Item 'C:\Users\hp\Downloads\pokebird\build\pokebird.uf2' "$($v.DriveLetter):\pokebird.uf2" -Force
```

Elle: BOOT + RESET'e bas, önce RESET'i sonra BOOT'u bırak, `.uf2`'yi sürücüye kopyala.

### Cihazla konuşma

Kart USB seri olarak görünüyor: **COM13** (`VID_2E8A PID_0009`).

| Komut | Ne yapar | Göz gerekir mi |
|---|---|---|
| `i` | cihaz ve ses yapılandırması | hayır |
| `n` | gürültü tabanı ölçümü | hayır |
| `e` | EMI taraması (arka ışık etkisi) | hayır |
| `g` | mikrofon kazancı (0–7) | hayır |
| `r` | 2 s kayıt al ve aktar | hayır |
| `l` | canlı seviye | hayır |
| `s` | canlı spektrogram | **evet** |
| `d` | ekran testi: 4 başlatma varyantı | **evet**, etkileşimli |
| `o` | yön testi: 4 köşeye 4 renk | **evet** |
| `b` | arka ışık teşhisi | **evet**, etkileşimli |
| `v` | QSPI veri yolu teşhisi | çoğu adım hayır |
| `w` | **QSPI zamanlama teşhisi** — `QSPI_WaitIdle` ölçümü (§9n) | hayır |
| `y` | **melez yol testi** — pencere ve piksel ayrı yollardan (§9n) | **evet**, etkileşimli |
| `z` | **satır adresleme testi** — RASET çalışıyor mu (§9n) | **evet**, etkileşimli |
| `j` | **imleç konumlandırma testi** — dar pencereyle ucuz atlama (§9n) | **evet**, etkileşimli |
| `t` | dokunmatik teşhisi (canlı akış) | **evet** |
| `u` | LVGL demo ekranı | **evet** |
| `m` | **mel + kapı hattı (M3)** | hayır |
| `a` | **TAM DEMO**: LVGL kart + canlı mel spektrogramı + kapı | **evet** |
| `x` | **tür ağı cihaz-içi doğrulama** + arena + çıkarım süresi (M6) | hayır |
| `k` | **gerçek zamanlı tanıma** (core 1, mikrofon) | hayır |
| `K` | aynısı ama kapı yoksayılır — ölçüm kipi | hayır |

```bash
python tools/capture_wav.py --port COM13 --cmd m --sure 8   # 8 s akit, ozeti al
python tools/capture_wav.py --port COM13 --cmd a --sure 15  # demoyu 15 s calistir
python tools/capture_wav.py --port COM13 --out a.wav        # kayit al
```

`d`, `b`, `v`, `t`, `u` **etkileşimli**: araç çıktıyı canlı akıtır ve klavyeyi
cihaza iletir. Diğerleri toplu okur. `m` ve `a`, `--sure N` verilirse N saniye
akıtıp kendiliğinden çıkar (göz gerektirmeyen doğrulama); `--sure`'siz
etkileşimli çalışırlar.

> Tek seferlik gözlenen tuhaflık: uzun (60 s) bir `--sure` koşusunda çıkış
> tuşu cihaza ulaşmadı ve özet bir sonraki bağlantıda geldi. Tekrarında
> (15 s) temiz çalıştı; kovalanmadı. Tekrar görülürse USB-CDC tarafına bakın.

`v` teşhisinin ilk dört adımı ekrana bakmayı gerektirmez; QSPI hattında bir şey
bozulursa oradan başlayın: dar DMA yazımı, PIO durumu, pinlerin gerçekten
sürülüp sürülmediği, kısa devre kontrolü. Adım 6 PIO'yu tamamen devre dışı
bırakıp paneli **bit-bang** ile sürer — hatanın PIO'da mı yoksa daha aşağıda mı
olduğunu tek adımda ayırır.

---

## 4. Tamamlananlar

### M0 — proje iskeleti ✅

- CMake + Pico SDK iskeleti, `boards/pokebird_rp2350b.h` (SDK'da RP2350B için hazır başlık yok)
- `src/board_config.h` — pin haritası, tek doğruluk kaynağı
- Derleme-zamanı kontrolleri (`static_assert`): yanlış board seçilirse derleme durur.
  GPIO40 ve 41–47 sadece RP2350B'de var; bu sessizce yanlış giderse bulması çok pahalı olurdu.

### M1 — mikrofon bring-up ✅

**Sonuç: mikrofon kuş sesi tanıma için kullanılabilir.**

| Ölçüm | Sonuç |
|---|---|
| ES8311 çip kimliği | 0x1183, I2C yanıt veriyor |
| MCLK | 6.144 MHz (PIO), ES8311 master, 24 kHz örnekleme |
| Yakalama | 48000/48000 örnek, düşen örnek yok |
| Tekrarlanabilirlik | 6 ardışık ölçüm, -36.2 … -36.8 dBFS (±0.3 dB) |
| ~~Arka ışık EMI~~ | ~~en kötü +0.2 dB~~ — **GEÇERSİZ, aşağıya bakın** |
| Kazanç taraması | 4→6→7 kademelerinde +12.2 ve +6.8 dB |
| El çırpma testi | **geçti** |

**Yorum:** Gürültü kazançla ölçekleniyor (ES8311'in 6 dB'lik PGA adımlarıyla
neredeyse birebir), yani PGA'dan **önce** giriyor: akustik kaynaklı, devre
kaynaklı değil. -36 dBFS taban, kullanıcının odasındaki **fan sesi**.

> ### ⚠ EMI SONUCU GEÇERSİZ — YENİDEN ÖLÇÜLMELİ
> M1'deki arka ışık EMI taraması PWM üzerinden yapılmıştı. Sonradan anlaşıldı ki
> **PWM o pini hiç sürmüyor** (§5.8). Yani dört ölçümün dördünde de arka ışık
> büyük olasılıkla KAPALIYDI; test hiçbir zaman "ışık açık" durumunu ölçmedi.
> *"Ekran açıkken dinleme sorunsuz"* sonucu **kanıtlanmış değil**. `e` komutu
> aç/kapa olarak düzeltilip yeniden çalıştırılmalı.

Dosyalar: [`src/hal/audio_i2s.c`](src/hal/audio_i2s.c), [`.pio`](src/hal/audio_i2s.pio),
[`src/hal/es8311.c`](src/hal/es8311.c), [`src/hal/i2c_bus.c`](src/hal/i2c_bus.c)

---

## 5. Denenip İŞE YARAMAYANLAR — tekrar denemeyin

Bu bölüm zaman kazandırmak için var.

### 5.1 SDK'nın kendi derlediği picotool segfault ediyor

Pico SDK, sistemde uygun sürüm bulamazsa picotool'u kaynaktan derliyor. Bu
makinedeki host derleyicisiyle (winlibs GCC 16.1) derlenen picotool 2.3.0
**dosya okuyan her komutta** çöküyor: `uf2 convert`, `info`, `coprodis`.
Belirti: derleme `Access violation` / `[code=3221225477]` ile duruyor.

**Çözüm:** [`cmake/picotool.cmake`](cmake/picotool.cmake) — PlatformIO ile gelen
önceden derlenmiş picotool 2.0.0'ı SDK'ya `IMPORTED` hedef olarak tanıtıyor.
SDK'nın `Findpicotool.cmake` dosyası `if (NOT TARGET picotool)` ile başladığı
için `pico_sdk_init()` öncesinde hedefi tanımlamak indirme/derleme adımını
tamamen atlatıyor.

> Denenmiş ama gereksiz: `PICO_NO_COPRO_DIS=1`. Sadece `coprodis` adımını
> atlatıyor, `uf2 convert` yine çöküyordu.

### 5.2 PIO state machine'i her yakalamada yeniden başlatmak

**M1'de en çok zaman kaybettiren hata.** Ölçümlerin rastgele bir kısmı tamamen
gürültü veriyordu: RMS ~19000, tepe tam ölçek, DC oynak.

Teşhis: `|x| > 16384` oranı tam **%50.1**, ardışık örnek farkının RMS'i
≈ √2 × RMS → örnekler tamamen ilintisiz. Her bit kaydırması denendi, hiçbiri
yapıyı geri getirmedi → basit bit kayması değil, hattı hiç okumuyoruz.

Sebep: her yakalamada `pio_sm_set_enabled(false)` + `pio_sm_restart()`. Bu, I2S
çerçeve kilidini her seferinde yeniden kurmaya zorluyor; LRCK ve BCLK kenarları
neredeyse aynı anda değiştiği için yeniden kilitlenme yarışa dönüşüyor.

**Çözüm:** RX state machine `pb_audio_i2s_init()` içinde **bir kez** başlatılıyor
ve bir daha durdurulmuyor. Yakalama öncesi sadece FIFO boşaltılıyor.

> Kısmen yardımcı olan ama tek başına yetmeyen düzeltme: `.pio` içinde LRCK
> kenarından sonra doğrudan `wait 1 gpio 4` yerine önce `wait 0 gpio 4`.
> WAIT komutu kenar değil **seviye** tetikli.

### 5.3 `getchar_timeout_us(0)` bloklamıyor

`0` "beklemeden zaman aşımı" demek. Bloklayan okuma için düz `getchar()`.

### 5.4 `fflush(stdout)` bağlanmıyor

Newlib'in tüm stdio kilit makinesini çekiyor (`__retarget_lock_acquire_recursive`
vb. tanımsız), Pico SDK'nın minimal printf'i onları sağlamıyor. SDK'nın kendi
`stdio_flush()` fonksiyonu kullanılmalı.

### 5.5 PC'den ses çalıp cihazın duymasını beklemek — GEÇERSİZ, HÂLÂ GEÇERSİZ

`[Console]::Beep` ile test yapıldı, "mikrofon çalışıyor" sanıldı. **Ama
bilgisayarda kulaklık takılıydı, hoparlörden ses çıkmadı.** Görülen artış
odadaki başka bir sesti. Doğru test: kullanıcının el çırpması (geçti).

> ### ⛔ KURAL — kullanıcı 2 Ağustos 2026'da tekrar hatırlattı
>
> **Bilgisayarda kulaklık takılı ve öyle kalacak.** Bu yüzden:
>
> **PC'den ses çalıp cihazın mikrofonuyla duymasını bekleyen HİÇBİR test
> kurmayın.** Ne `Console::Beep`, ne WAV çalma, ne ffplay, ne tarayıcıdan
> kuş sesi. Hoparlörden ses çıkmıyor; test sessizce "başarısız" değil,
> sessizce **anlamsız** olur — cihaz hiçbir şey duymaz ve siz bunu kod
> hatası sanıp saatlerce yanlış yerde ararsınız.
>
> **Akustik bir test gerekiyorsa KULLANICIDAN İSTEYİN.** Ne çalacağını,
> ne kadar süre, cihazı nereye tutacağını açıkça yazın; sonucu o size
> bildirsin. Elle yapılan test geçerli, otomatik olan değil.
>
> Bu, §5.10'un ("dolaylı ölçüme fazla güvenmeyin") akustik hâli: burada
> ölçüm dolaylı bile değil, **hiç yapılmıyor.**

### 5.6 Ev dizini kazara bir git deposu

`C:\Users\hp` bir git deposu (commit'i yok). `git add -A` oradan çalıştırılırsa
tüm ev dizinini taramaya kalkıyor ve zaman aşımına uğruyor.

### 5.7 Düz RMS ile gürültü tabanı ölçmek yanıltıcı

Oda hiçbir zaman tam sessiz değil; tek bir kapı sesi düz RMS'i yukarı çekiyor.
Gürültü tabanı artık kısa pencere RMS'lerinin **10. yüzdeliği**.

### 5.8 Arka ışığı PWM ile sürmek

`gpio_set_function(GPIO36, GPIO_FUNC_PWM)` + duty %0 — yani pinin sürekli LOW
olması — ışığı **yakmıyor**. Aynı elektriksel durum düz GPIO ile (`GPIO_FUNC_SIO`,
çıkış, LOW) **yakıyor**. Kartta ölçüldü. Kök neden aranmadı; arka ışık düz
GPIO'ya alındı (aç/kapa).

**Tuzak:** Bu hata M1'in EMI ölçümünü de sessizce geçersiz kıldı (§4).

### 5.9 CS'i PIO daha veriyi hatta çıkarmadan yükseltmek — **ekranın çalışmama sebebi**

Ekran hiç görüntü vermiyordu: panel 60 Hz tarıyor, arka ışık yanıyor, ama her
pikselin farklı renk olduğu, hiç değişmeyen bir karıncalanma vardı — panel kendi
başlatılmamış GRAM'ını gösteriyordu ve bizden gelen **hiçbir şeyi kabul
etmiyordu**.

Sebep [`qspi_pio.c`](src/hal/display/qspi_pio.c) içindeki `QSPI_Deselect`:

```c
void QSPI_Deselect(pio_qspi_t qspi){
    gpio_put(qspi.pin_cs,1);       // <-- PIO hala kaydiriyor!
}
```

`pio_sm_put_blocking()` yalnızca FIFO **dolu** iken bekler; veriyi FIFO'ya koyar
koymaz döner. `dma_channel_is_busy()` de yanlışa döndüğünde baytlar FIFO'ya
yazılmıştır, **hatta çıkmış değildir**. Dolayısıyla CS, komut daha kablodayken
yükseliyordu ve panel her işlemi yarıda kesilmiş görüyordu.

**Çözüm:** CS yükseltilmeden önce PIO'nun `TXSTALL` bayrağı bekleniyor
(`QSPI_WaitIdle`). Bekleme `QSPI_Deselect`'in içine konuldu ki satıcı dosyaları
dahil her çağrı yeri tek düzeltmeden faydalansın.

> Bu hata Waveshare'in kendi örneğinde de var; `QSPI_REGISTER_Write` sonundaki
> `// WAIT_TIME();` yorum satırı, satıcının da sorunu sezip yetersiz bir çözüm
> denediğini gösteriyor (2 nop zaten yetmezdi).

Aynı turda düzeltilen ikinci eksik: satıcının başlatma tablosu `0x11` (SLPOUT)
ve `0x29` (DISPON) yorum satırıyla bitiyordu, `0x3A` (COLMOD) hiç yoktu. Bunlar
eklendi — **ama tek başlarına yetmiyordu**: COLMOD'suz kontrol varyantı da
düzeldikten sonra çalıştı, yani panelin varsayılan piksel biçimi zaten
RGB565'miş. Yine de açıkça ayarlamak doğru.

### 5.10 Ekran hata ayıklamasında tahmin yürütmek — ve dolaylı ölçüme fazla güvenmek

Ekran tarafında beş hata üst üste çıktı ve hiçbirini seri porttan göremedim;
hepsi ancak kullanıcı ekrana bakınca ortaya çıktı.

**İşe yarayan yöntem:** *kendi kendini raporlayan* test yazmak. Arka ışık
sorununu çözen şey, durumları sırayla deneyip "ışığı gördüğünüzde tuşa basın"
diyen `b` komutu oldu. Yön sorununu çözen şey `o` komutunun dört köşeye dört
farklı renk basması oldu — tek bakışta hem dönüş hem aynalama belli oluyor.

**Ama gözle doğrulamayı fazla ertelemeyin.** CS hatası aranırken göz
gerektirmeyen iki dolaylı test yazıldı (TE hattını dinlemek, panelden register
okumak). İkisi de "panel sağır" dedi — **ikisi de yanlıştı**: TE bu panelde
koşulsuz darbeliyor ve kullanılan okuma opcode'u yanıt getirmiyor. Dolaylı
ölçüm de en az tahmin kadar yanıltabilir; hipotezi doğrudan sınayan tek gözlem
(ekrana bakmak) bir tur daha erken alınmalıydı.

**Aynı hata dokunmatikte tekrarlandı:** protokol testlerinin hepsi kimse ekrana
dokunmadan çalıştırıldı, dolayısıyla "boşta 0xDB geliyor" bulgusu hiçbir şey
kanıtlamadı (§9b).

### 5.11 GPIO34 (LCD_RST) aşağı çekilemiyor — ölçüldü, çözülmedi

`v` teşhisi: GPIO34 çıkış olarak 0 sürülüp geri okunduğunda **1** okunuyor;
serbest bırakılıp dahili pull-down ile de 1 kalıyor. Pin dışarıdan sert şekilde
yüksek tutuluyor ve **panele donanım reset'i atamıyoruz**. Pin numarası doğru
(Waveshare'in `qspi_pio.h`'si de `PIN_RST 34` diyor). Ekran resetsiz çalıştığı
için peşine düşülmedi.

Dokunmatik boşta hep `0xDB` döndürüyor; **muhtemel bağlantı burada** — AXS15231B
tek çip olarak hem ekranı hem dokunmatiği sürüyor, dokunmatik motoru resetsiz
uyanmıyor olabilir. Dokunmatiğe dönülürse ilk bakılacak yer bu.

### 5.12 PowerShell ile kaynak dosya düzenlemek — kodlamayı bozuyor

`Get-Content` (varsayılan kodlama) + `Set-Content -Encoding utf8` ile bir blok
silindi; Windows PowerShell 5.1 dosyayı **cp1254** olarak okuyup UTF-8 yazdığı
için tüm Türkçe karakterler **çift kodlandı** (`—` → `â€”`). Fark edildi ve
satır bazında onarıldı, derleme doğrulandı.

**Kaynak dosyaları Edit aracıyla düzenleyin, PowerShell ile değil.**

### 5.13 Yeni bir venv'in pip'i hiçbir şey indiremiyor — sertifika

`py -3.11 -m venv` ile açılan ortamın pip'i (24.0) her indirmede
`CERTIFICATE_VERIFY_FAILED: self-signed certificate in certificate chain`
veriyor. Ağda araya giren bir sertifika var. Sistemdeki Python 3.14'ün pip'i
(25.x) aynı adresi sorunsuz indiriyor — **fark truststore**: yeni pip
Windows sertifika deposunu kullanıyor, 24.0 yalnızca certifi'ye bakıyor.

```bash
python -m pip install --use-feature=truststore --upgrade pip
```

Bir kez pip yükseltilince gerisi normal çalışıyor. `pip.ini` aramayın, yok.

### 5.14 BirdNET modelini kendi adresinden indirmek

`ensure_model_exists()` 214 MB'lık `V2.4.zip`'i `tuc.cloud`'dan çekiyor.
Ölçüldü: tek bağlantı **20–113 kB/s**, ve iki denemede de **95 MB'de
takıldı** (`requests` timeout'u ateşlemeden). Sunucu Range destekliyor (206)
ama 12 paralel parça denemesinde 12 bağlantının hepsi 0 bayt kaldı —
eşzamanlılığı kısıtlıyor.

**Çözüm:** aynı V2.4 dosyaları GitHub'da deponun **`v1.5.1`** etiketinde
duruyor (2.x'te depodan çıkarılıp tuc.cloud'a taşınmışlar). 36 dosya,
`raw.githubusercontent.com`'dan dakikalar içinde iniyor. Her dosyayı git
ağacındaki boyutuna karşı doğrulayın; `birdnet_analyzer.utils.check_birdnet_files()`
listenin tamamını istiyor (TFJS parçaları dahil), biri eksikse yeniden
indirmeye kalkıyor.

### 5.15 BirdNET'in kendi süreç havuzu kilitleniyor

`analyze(threads=N)` içeride `multiprocessing.Pool` açıyor. `threads=14`
ile 40 dosyalık bir türde **37 dosyadan sonra kilitlendi**: 16 süreç ayakta,
toplam CPU 962 sn'de sabit, kalan 3 dosya hiç işlenmedi. Aynı üç dosya
`threads=1` ile sorunsuz bitti (21,4 / 0,7 / 10,9 sn) — yani dosyalarda
sorun yok, havuzda var (Windows + TensorFlow).

**Çözüm:** paralellik `tools/birdnet_run.py` içinde kuruluyor — N bağımsız
süreç, her biri kendi tür kümesini `threads=1` ile işliyor, her birinin
ayrı günlüğü var. Ölçülen: **124 dosya/dk** (10 işçi).

### 5.16 Tür eşlemesini bilimsel adla yapmak — sessiz veri kaybı

Özet betiği hedef türü bilimsel adla arıyordu. BirdNET iki türde **eski cins
adında** kalmış:

| | bizde | BirdNET |
|---|---|---|
| Küçük Karga (`eurjac`) | `Coloeus monedula` | `Corvus monedula` |
| Ak Karınlı Ebabil (`alpswi1`) | `Tachymarptis melba` | `Apus melba` |

Kendi adımızla arasaydık bu iki türün güveni **her dilimde 0** çıkardı ve
ikisi de sessizce eğitim dışı kalırdı — hiçbir hata mesajı vermeden.
Eşleme artık **eBird kodu** üzerinden, BirdNET'in kendi
`eBird_taxonomy_codes_2024E.json`'ıyla kuruluyor ve sonuç
`data/birdnet_ad_haritasi.csv`'ye yazılıyor (görünür olsun diye).

> **Ders:** dış bir modelle isim üzerinden eşleşiyorsanız, eşleşmeyenleri
> saydırın. "178/178 bulundu" demek yetmez; *neyle* bulunduğunu da yazdırın.

---

### 5.17 Belleği küçültmek zamanlama payını da yiyebilir

`s_capture` temizliği (96 KB → 4 KB) ekranı bozdu. Sebep bellek bozulması
**değildi** — kanarya testi bunu eledi. bss küçülünce crt0'ın sıfırlaması
kısaldı, firmware panel başlatmaya ~1 ms erken vardı ve AXS15231B'nin hazır
olma penceresini kaçırdı. Panele donanım reset'i atamadığımız için (§5.11)
durum kalıcı oldu.

**İki ders:**

1. **Bellek değişikliği bir zamanlama değişikliğidir.** "Sadece tampon
   küçülttüm, mantığa dokunmadım" güvenli demek değil. Açılış sırasındaki
   herhangi bir işi kısaltmak, ondan sonra gelen ve zamanlamaya duyarlı
   donanımı vurabilir.
2. **Zamanlama kısıtını `sleep_ms` ile değil mutlak alt sınırla ifade edin.**
   `sleep_ms(N)` o anki paya N ekler; kendinden önceki kod hızlanınca aynı
   tuzak yeniden kurulur. `while (to_ms_since_boot(...) < N) ...` kısıtın
   kendisini söyler ve önceki kodun süresinden bağımsızdır.

Teşhis yolu da kayda değer: üç koşuluk A/B (eski / yeni / **yeni mantık +
eski yerleşim**) mantığı yerleşimden ayırdı ve tek başına "hata mantıkta
değil" sonucunu verdi. Bunu bir kez daha kullanın — ucuz ve kesin.

---

## 6. Açık konular / borçlar

| Konu | Durum |
|---|---|
| **EMI ölçümü geçersiz** | M1'deki tarama PWM ile yapıldı, ışık hep kapalıydı. `e` komutu aç/kapa olarak düzeltilip yeniden ölçülmeli (§4). |
| ~~**`s_capture` (96 KB)**~~ | ✅ **ÇÖZÜLDÜ (§9g).** 4 KB'lık `s_chunk`'a indi, bss 218.988 → 127.084. Arena'nın önü açık. |
| ~~**Ekran gerilemesi**~~ | ✅ **ÇÖZÜLDÜ (§9h).** Panel hazır olma penceresi; ekran başlatması açılıştan ≥250 ms sonraya alındı. Bellek kazancı korundu. |
| **EMI ölçümü — borç neredeyse kapandı** | `e` komutu artık gerçekten aç/kapa ölçüyor (arka ışık düz GPIO'da): **kapalıya göre +0,4 dB**, yani arka ışık mikrofonu bozmuyor. Kalan tek eksik: son üç ölçüm hâlâ `PWM %50` / `PWM %10` diye etiketleniyor, oysa üçü de aynı "açık" durumu. Etiketler düzeltilip yeniden çalıştırılırsa §4'teki "GEÇERSİZ" uyarısı kaldırılabilir. Küçük iş. |
| **⛔ EKRAN BOZUK** | `o` testinde yalnızca en son çizilen kare görünüyor; `a` demosunda yazı tipi bozuk. **M6 öncesi derlemede de aynı** — gerileme değil, gözden kaçmış bir hata. Bit-bang yolu çalışıyor, PIO/DMA yolu çalışmıyor. Elenen ihtimaller ve sıradaki adım §9n. **M7'nin tamamı buna bağlı.** |
| **Dokunmatik park edildi** | Kritik yolda değil. Kaldığı yer §9b. |
| **PWM GPIO36'yı sürmüyor** | Kök neden bulunmadı; arka ışık düz GPIO. Parlaklık ayarı gerekirse (M7) çözülmeli. |
| **GPIO34 (LCD_RST) aşağı çekilemiyor** | Ölçüldü, kök neden aranmadı. Bkz. §5.11. |
| `SYS_EN` (GPIO39) | Açılışta 1'e çekiliyor (`power_latch_init`). Waveshare'in `DEV_Module_Init()`'inden alınan tek iş. Pil ile çalışırken güç mandalı için doğru olan bu. |
| Ekran teşhis iskelesi | `v` komutu, `d`'nin varyantları ve `main.c`'deki bit-bang yolu duruyor. **Korunmalı** — QSPI bir daha bozulursa en hızlı yol bunlar. |
| **es8311.c lisansı** | "ESPRESSIF MIT License" standart MIT DEĞİL; kullanımı Espressif ürünleriyle sınırlı. Kişisel kullanımda pratik sorun yok. **Dağıtım öncesi** veri sayfasından kendi sürücümüz yazılmalı. |
| **BirdNET lisansı** | CC BY-NC-SA 4.0. Damıtılan model türev sayılabilir → ticari kullanımı kısıtlar. Ticari yol için damıtmasız varyant gerekir. ARCHITECTURE §6. |
| **ESC-50 lisansı** | CC BY-NC 3.0 (K. J. Piczak). Negatif sınıfın kaynağı (§9j). Kişisel kullanımla uyumlu, **ticari dağıtımla değil** — XC ve BirdNET ile aynı sınıftan kısıt. Atıf `data/negatif/esc50_kayitlar.csv`'de. Ticari yol açılacaksa negatifler M8'de cihazın kendi kayıtlarıyla değiştirilebilir. |
| Mikrofon kazancı | Şu an 3 (varsayılan). Saha koşullarında kalibre edilmeli. |
| CMSIS-DSP | `src/dsp/fft.c` hâlâ kendi radix-2 FFT'si. Doğru ama yavaş; hız gerekirse `arm_rfft_fast_f32` devralabilir. Host testleri değişikliği anında doğrular. |
| LCD_TE yırtılma önleme | M2b'de planlanmıştı, yapılmadı. |
| SD kart | Hiç dokunulmadı. M7'de. |

---

## 7. Mimari özet (ayrıntı: docs/ARCHITECTURE.md)

```
Core 1 (ses + yapay zeka, gerçek zamanlı)
  I2S/PIO+DMA → mel öznitelik → Kapı(VAD) → Aşama-1 ikili → Aşama-2 tür
  → zamansal birleştirme + mevsim önceliği
              ↓ FIFO (tespit olayları)
Core 0 (arayüz + depolama)
  LVGL → AXS15231B QSPI+DMA │ dokunmatik │ SD │ pil │ güç
```

**Ses hattı:** 24 kHz / 16-bit mono, 3 s pencere 1 s adımla, FFT 512, hop 384
(16 ms) → 187 kare, 64 mel bandı, 150 Hz – 11.5 kHz, log-mel + normalizasyon.
24 kHz seçildi çünkü 16 kHz'in 8 kHz Nyquist'i Çalıkuşu/Tırmaşıkkuşu gibi
7–9 kHz'de öten türleri kırpardı.

**Bellek hilesi:** 3 s ham ses tutulmuyor (144 KB olurdu). Mel kareleri artımlı
hesaplanıp 64×187 halka tamponda tutuluyor (int8 → 12 KB). ~130 KB tasarruf.

**Model piramidi:**
| Aşama | Görev | Boyut |
|---|---|---|
| 0 Kapı | enerji + spektral akı | ~0 (DSP) ✅ yapıldı |
| 1 İkili | kuş mu değil mi | ~15 KB int8 |
| 2 Tür | 110 tür + bilinmiyor | ~300–400 KB int8 |
| 3 Birleştirme | zamansal oylama + mevsim önceliği | ~2 KB tablo |

Hedef: ≤30 MMAC/pencere, tensor arena ≤180 KB.

**Doğruluk beklentisi:** temiz kayıtlarda top-1 %65–75, top-3 %85–90. Arayüz bu
yüzden tek cevap değil **ilk 3 tahmin** gösterecek.

**Bellek bütçesi (520 KB):** arena 180 + LVGL 26 + mel 12 + ham ses 24 +
DMA 8 + FatFS 10 + tablo 20 + yığın/heap 80 = **~360 KB**, ~160 KB pay.

### Şu anki gerçek kullanım (2 Ağustos 2026, `arm-none-eabi-size` ile ölçüldü)

```
text 463.016 (flash, 16 MB'de sorun değil)
bss  127.084 (520 KB SRAM'de)          <- s_capture temizliginden SONRA
```

bss'teki en büyük altı nesne (`nm --size-sort -S`):

| Nesne | bayt |
|---|---|
| `s_draw_buf` — LVGL çizim tamponu (640×20 px) | 25.600 |
| `work_mem_int` — LVGL bellek havuzu | 24.576 |
| `s_ring` — sürekli yakalama halkası (16 KB'a hizalı) | 16.384 |
| `pencere.0` / `pencere.2` / mel `s_ring` — mel halkaları (64×187 int8) | 3 × 11.968 |
| **`s_chunk`** — teşhis yakalama parçası | **4.096** |

TFLM arena'sı (180 KB) eklendiğinde 127.084 + 184.320 = **311.404 bayt**,
520 KB SRAM'de yığın/heap için ~208 KB pay kalıyor. Plan §7'nin öngördüğü
~360 KB bütçenin altında.

---

## 8. Yol haritası

| # | Aşama | Durum |
|---|---|---|
| M0 | İskelet, derleme zinciri | ✅ |
| M1 | Mikrofon bring-up + SNR | ✅ |
| M2a | Ekran sürücüsü + canlı spektrogram | ✅ (§9a) |
| **M2b** | **LVGL entegrasyonu + dokunmatik** | **🔶 LVGL + spektrogram birlikte çalışıyor (demo); dokunmatik park, TE yapılmadı (§9b)** |
| M3 | DSP hattı: mel + kapı + sürekli yakalama | ✅ (§9c) |
| M4 | Veri boru hattı + tür listesi (PC tarafı) | ✅ veri (178 tür, 7.111 WAV) · segmentasyon (§9e) · **eğitim kümesi (§9j)** · negatif saha turu M8'e ertelendi (§9f-4) |
| M5 | Model eğitimi + damıtma + INT8 | 🔶 **Aşama-2 tür ağı ✅ (§9k)** · Aşama-1 ikili ağ ve Aşama-3 mevsim tablosu kaldı |
| M6 | TFLM entegrasyonu, gerçek zamanlı çıkarım (core1) | ✅ (§9m) — arena 110 KB, çıkarım 190 ms, doğrulama 8/8 birebir |
| **—** | **EKRAN HATASI** | **⛔ M6 öncesinden geliyor, M7'yi tıkıyor (§9n)** |
| M7 | Sonradan işleme, tarih ekranı, tam arayüz, günlük, pil | |
| M8 | Saha kalibrasyonu | |

---

## 9a. M2a — ekran ✅ TAMAMLANDI

Waveshare'in **bu karta ait** LVGL örneğinden alındı
(`files.waveshare.com/wiki/RP2350-Touch-LCD-3.49/RP2350-Touch-LCD-3.49-LVGL.zip`):
`qspi.pio`, `qspi_pio.c`, `LCD_3in49.c` (AXS15231B). Satıcı dosyaları neredeyse
dokunulmadan duruyor; bekledikleri semboller
[`DEV_Config.h`](src/hal/display/DEV_Config.h) uyum katmanından ve
[`dev_config.c`](src/hal/display/dev_config.c)'deki DMA globallerinden geliyor.

- **QSPI pio0'da, ses pio1'de** — state machine çakışması yok
- `src/ui/spectrogram.c`: tek sütun yazan kaydırmalı spektrogram; tam
  framebuffer (220 KB) yerine sütun başına 344 bayt

**Doğrulandı:** panel renkleri basıyor (dört başlatma varyantının dördü de,
bit-bang dahil), yön ölçüldü, **canlı spektrogram kullanıcı tarafından doğru
çalışırken görüldü**.

### Ekranın çalışmasını engelleyen beş hata

| # | Hata | Çözüm |
|---|---|---|
| 1 | Ekran başlatma bloğu `main()`'e hiç eklenmemişti | Elle eklendi; düzenleme sonrası `grep` ile doğrulanıyor |
| 2 | Başlatma sırası eksikti | `QSPI_GPIO_Init → QSPI_PIO_Init → QSPI_4Wrie_Mode → pb_display_dma_init → LCD_3IN49_Init`. 3. adım PIO SM'i etkinleştiriyor; atlanırsa TX FIFO hiç boşalmıyor ve **DMA sonsuza kadar bekliyor** |
| 3 | Koordinat sistemi ters | Sürücü panelin doğal yönünde: **X 0–171, Y 0–639** |
| 4 | Arka ışık PWM ile sürülemiyor | Düz GPIO'ya alındı (§5.8) |
| 5 | **CS, PIO veriyi çıkarmadan yükseliyordu** | `QSPI_WaitIdle()` (§5.9) — asıl sebep buydu |

Ayrıca: `LCD_3IN49_DisplayWindows()` verilen tamponu **tam ekran framebuffer**
sanıyor, küçük tampon verilince sınır dışını okuyor. Kendi
[`pb_lcd_blit()`](src/hal/display/lcd_blit.c)'imiz yazıldı: düz tampon,
kapsayıcı w/h, panelin beklediği **big-endian RGB565** çevrimi dahil.

### 5. hata aranırken ölçümle ELENEN ihtimaller — tekrar bakmayın

| İhtimal | Sonuç |
|---|---|
| Dar (8 bit) DMA yazımı bayt şeritlerine kopyalanmıyor | Kopyalanıyor (`0xa5a5a5a5`) |
| PIO çalışmıyor / FIFO boşalmıyor | Çalışıyor, boşalıyor |
| Pinler sürülmüyor / kısa devre | SCLK ve D0 kıpırdıyor, 20–25 temiz |
| Pin haritası yanlış | Waveshare'in `qspi_pio.h`'siyle birebir aynı |
| `qspi.pio` / `qspi_pio.c` bozulmuş | Orijinalle **byte-identical** |
| SCLK çok hızlı | 37.5 MHz ve 1.9 MHz'de aynı sonuç |
| Panel register dizisi yanlış | Dört varyantın dördü de düzeltmeden sonra çalıştı |
| Sistem saati | `set_sys_clock` çağrısı yok; varsayılan 150 MHz, satıcıyla aynı |

### Arka ışık — kartta ölçüldü, kesin

```
BL_EN (GPIO37) = 1   ve   LCD_BL (GPIO36) = 0   ->   IŞIK YANAR
```

LCD_BL **aktif-düşük**. Üç kaynak doğruluyor: etkileşimli `b` testi, `rsvpnano`
sürücüsü, ve Waveshare'in `pwm_set_chan_level(slice, CHAN_A, 100 - Value)` kodu.

### Yön — kartta ölçüldü (`o` komutu)

Cihaz **USB soketi AŞAĞI** tutulduğunda:

```
panel X 0->171  =  fiziksel SOL -> SAĞ
panel Y 0->639  =  fiziksel ÜST -> ALT
```

Panelin doğal yönü, dönüş/aynalama olmadan dikey. Arayüz yatay olduğu için cihaz
90° sola çevriliyor ve **USB soketi SAĞDA** kalıyor:

```
panel Y+  =  fiziksel SOL -> SAĞ   -> arayüzün x'i (zaman)
panel X+  =  fiziksel ALT -> ÜST   -> arayüzün y'si, TERS
```

Son satır [`ui/spectrogram.c`](src/ui/spectrogram.c)'de düzeltme gerektirdi:
frekans ekseni ters çevriliyordu. Ters tutuş (USB solda) istenirse **iki şey
birden** dönmeli: `write_ui_column`'daki ny ve bin eşlemesi.

---

## 9b. M2b — LVGL + dokunmatik 🔶

| # | İş | Durum |
|---|---|---|
| 1 | LVGL v9.3 vendor + yapılandırma + derleme | ✅ |
| 2 | Ekran sürücüsü: kısmi render + 90° yön çevrimi | ✅ **kullanıcı doğruladı** — yazılar düz, düzen doğru |
| 3 | Dokunmatik sürücüsü (zaman aşımlı), çip 0x3B'de ACK veriyor | ✅ |
| 4 | Dokunmatik koordinat eşlemesi | ⏸ **PARK EDİLDİ** |
| 5 | LCD_TE ile yırtılma önleme | ⏳ yapılmadı |
| 6 | Spektrogramın LVGL ile birlikte yaşaması | ✅ `a` demosu kanıtladı: LVGL sol şeritte (0..199), spektrogram sağda (200..639) doğrudan blit; LVGL yalnızca kirlenen alanı çizdiği için çakışmıyor. Sıra önemli: LVGL'in İLK çizimi tam ekran, `pb_spec_init` ondan SONRA çağrılmalı. `pb_lv_init` artık çift çağrıya dayanıklı. |

### Yön çevrimi ek tampon olmadan

LVGL yatay (640×172), panel dikey (172×640). Devrik (transpose) kopyası ikinci
bir tampon kadar RAM ister — bizde yok. Bunun yerine
[`pb_lcd_blit_strided()`](src/hal/display/lcd_blit.c) kaynağı **devrik sırayla**
okuyor: panelin bir yatay satırı, LVGL tamponunun bir dikey sütunudur. Negatif
adım aynalamayı da hallediyor. **Ek tampon maliyeti sıfır.**

Çizim tamponu: **tek** tampon, 640×20 px = 25 KB. Plan iki tampon öngörüyordu;
flush'ımız bloklayan DMA ile çalışıp hemen `flush_ready` dediği için ikinci
tampon paralel çizim sağlamaz, sadece şerit sayısını iki katına çıkarırdı.
Aynı bütçe tek parça kullanılıyor.

### ⏸ Dokunmatik neden park edildi

Kritik yolda değil: kuş tanımaya giden yol M3→M6, dokunmatik arayüz süsü.
Bring-up'ı tur başına bir "ekrana bakıp söyle" gerektiriyordu ve asıl amacı
geciktiriyordu. **Kullanıcı bunu açıkça belirtti.** Kod ağaçta duruyor:

- Çip 0x3B'de ACK veriyor, hat sağlam. i2c0 taramasında yalnızca `3b`;
  karşılaştırma i2c1: `18 51 6b` (codec, RTC, IMU) — tarama doğru çalışıyor.
- Okuma protokolü rsvpnano'ya göre düzeltildi: komut dizisinin **7. baytı
  okunacak bayt sayısı**. Waveshare `0x0E` yazıp 32 bayt okuyor (uyuşmuyor);
  çalışan sürücü `0x08` yazıp 8 bayt okuyor. Bizde artık 8 bayt.
- I2C hat kurtarma eklendi (SCL darbeleyip takılı slave'i serbest bırakma).
- **Çözülmemiş:** boşta paket sabit `0xDB` geliyor. Bunun "dokunma yok"
  demek mi yoksa hata mı olduğu **belirlenemedi** — tüm testler kimse ekrana
  dokunmadan çalıştırıldı (§5.10'daki hatanın tekrarı).

**Devam edilecekse ilk iş:** `--cmd t` çalıştırıp ekrana dokunmak. Satırlar
akıyorsa protokol tamam, sadece eşleme ölçülecek ([`lv_port.c`](src/ui/lv_port.c)
içindeki `PB_TOUCH_AYNALA`). Hep `db` kalıyorsa sıradaki şüpheli §5.11.

> **Ders:** rsvpnano'nun sürücüsü "hazır" gibi görünüyor ama ESP32-S3/Arduino
> (`Wire`, ESP-IDF `spi_master`). Taşınabilir olan yalnızca **protokol**; taşıma
> katmanı her hâlükârda Pico SDK'ya yeniden yazılıyor. Kullanıcı bunu sordu,
> cevap bu.

### Satıcının Touch.c'si neden alınmadı

`i2c_write_blocking`/`i2c_read_blocking` kullanıyor, **zaman aşımı yok**:
dokunmatik ACK vermezse tüm cihaz kilitleniyor. Ses hattı gerçek zamanlı,
göze alınamaz. Protokolü korunarak zaman aşımlı hâli
[`src/hal/touch.c`](src/hal/touch.c)'ye taşındı, satıcı dosyaları **silindi**.

`pb_touch_read()` 8 ms önbellekli: LVGL'in giriş sürücüsü ve arayüz kodu aynı
karede ayrı ayrı okuyordu; iç içe geçen komut+okuma dizisi çipi bozabilir.

---

## 9c. M3 — mel + kapı + sürekli yakalama ✅ TAMAMLANDI

### Yapılanlar

| Dosya | Ne |
|---|---|
| [`src/dsp/fft.c`](src/dsp/fft.c) | `pb_fft_power()` ayrıldı: periyodik Hann, DC giderimi, tam ölçekli sinüs = 0 dBFS |
| [`src/dsp/mel.c`](src/dsp/mel.c) | 64 bant HTK mel (150 Hz–11.5 kHz), seyrek filtre bankası, int8 halka tamponu 64×187 = 11.7 KB |
| [`src/dsp/gate.c`](src/dsp/gate.c) | 2–10 kHz enerji + spektral akı, uyarlamalı taban |
| [`test/dsp_test.c`](test/dsp_test.c) | 13 host testi |
| [`tools/mel_reference.py`](tools/mel_reference.py) | bağımsız numpy referansı |

### Doğrulama — planın kabul ölçütü karşılandı

```bash
cmake -S test -B test/build -G Ninja && cmake --build test/build
./test/build/dsp_test          # 13 test, 0 kaldi
python tools/mel_reference.py  # 64 bandin tamaminda sapma 0.0000 dB
```

Python referansı numpy ile **sıfırdan** yazıldı, C ile ortak kod yok. 64 bandın
tamamında int8 adımında bile sapma yok.

**Referansla uyum için kritik iki ayrıntı** (değiştirilirse model sessizce
kötüleşir):
- Filtre bankası **HTK** mel, **alan normalizasyonu YOK** →
  `librosa.filters.mel(sr=24000, n_fft=512, n_mels=64, fmin=150, fmax=11500, htk=True, norm=None)`
- Pencere **periyodik** Hann (`sym=False`), simetrik değil

### Host testleri iki gerçek hata yakaladı

Kartta bulunması pahalı olurdu, saniyeler içinde çıktılar:

1. **FFT ölçeği 2× yanlıştı.** `2/kazanç²` sinüsün ortalama-karesini veriyordu,
   tam ölçekli sinüs -3 dBFS okunuyordu. Doğrusu `4/kazanç²`. Bu, tüm mel
   değerlerini sessizce kaydırırdı.
2. **Kapı, yakalaması gereken anı kaçırıyordu.** Spektral akının şekil vektörü
   yalnızca enerji varken güncelleniyordu; sessizlikten sonraki ilk sesli karede
   karşılaştıracak önceki şekil olmadığı için akı sıfır çıkıyor ve kapı
   açılmıyordu. Artık sessiz karelerde şekil düzgün dağılım kabul ediliyor.

### Kartta canlı ölçüm (`--cmd m`, sessiz oda)

```
kare 452  kapi %3  bant -45.5 dB  taban -47.0 dB  aki 0.425  pencere 2
toplam kare 506, kapi acik 15 (%2), tam pencere 2
```

- Kapı sessiz odada **%2–3** açılıyor — plan §3'ün istediği davranış
  (ağır iş zamanın %90+'ında hiç çalışmayacak).
- Taban 2–10 kHz bandında ~-47 dB; M1'de ölçülen ~-36 dBFS geniş bant oda
  gürültüsüyle tutarlı.

### Sürekli yakalama — kendini yenileyen DMA halka tamponu ✅

Eski `pb_audio_capture` bloklayandı: her çağrı FIFO'yu boşaltıp sıfırdan DMA
kuruyordu; çağrılar arasında gelen örnekler PIO'nun 8 kelimelik FIFO'sunu
taşırıp düşüyordu → ~57 kare/s (62.5 yerine).

**Çözüm ([`src/hal/audio_i2s.c`](src/hal/audio_i2s.c)):** klasik ping-pong
yerine tek halka + iki DMA kanalı:

- **Veri kanalı:** PIO RX → 4096 örneklik halka (16 KB, 24 kHz'de 170 ms).
  DMA'nın **adres sarma (ring)** özelliği kullanılıyor — yazma adresi tampon
  sonunda kendiliğinden başa dönüyor. Şartları: boyut ikinin kuvveti VE tampon
  kendi boyutuna hizalı (`aligned(16384)`).
- **Kontrol kanalı:** veri kanalı bitince zincirle tetikleniyor, tek iş yapıyor:
  veri kanalının `al1_transfer_count_trig` register'ına sayacı yeniden yazmak.
  Bu, kanalı anında yeniden başlatıyor. CPU hiç karışmıyor.
- İki tur arasındaki birkaç çevrimlik boşluğu PIO RX FIFO'su (~333 µs) kapatıyor.

API: `pb_audio_stream_read` (kesintisiz, gerçek zamanlı hat için),
`pb_audio_stream_flush` (canlı göstergeler en tazeye atlar),
`pb_audio_capture` (eski imza korunarak flush+oku sarmalayıcısı oldu —
teşhis komutları değişmedi).

**Tüketici geride kalırsa:** halkanın 3/4'ünden fazlası birikmişse okuma en
tazeye atlar ve `fifo_overrun` ile bildirir — sessizce süreksiz veri dönmez.

**Durdururken tuzak:** önce zincir kırılmalı (`al1_ctrl`'de chain_to'yu kendine
çevir), sonra abort. İptal edilen kanal zinciri tetikleyebiliyor; sıra ters
olursa kontrol kanalı veri kanalını hemen yeniden başlatıyor.

**Kartta ölçüldü (1 Ağustos 2026):**
```
m komutu:  62-63 kare/s, kayip 0            (hedef 62.5, eskiden 57)
a demosu:  63 kare/s, kayip 0 — LVGL + dokunmatik + spektrogram yüküyle
           15 s'de 940 kare = 62.7/s, matematik birebir
```

Kapı davranışı değişmedi (sessiz odada %0–3). Kullanıcı demoyu ekranda
doğruladı: kart + akan mel spektrogramı + "SES ALGILANDI" yeşil.

---

## 9d. M4 — veri boru hattı ✅ TAMAMLANDI

### Yapılanlar

| Dosya | Ne |
|---|---|
| [`tools/species_list.py`](tools/species_list.py) | GBIF + eBird'den İstanbul tür havuzu, **anahtarsız** |
| [`tools/xc_fetch.py`](tools/xc_fetch.py) | Xeno-canto sayım (`--say`) + indirme (`--indir`) |
| [`tools/xc_convert.py`](tools/xc_convert.py) | mp3 → 24 kHz mono WAV, tür başına kota |
| [`tools/m4_run.py`](tools/m4_run.py) | sayım → eşik arama → indirme zinciri, gözetimsiz |
| `data/species_istanbul.csv` | tür tablosu + aylık dağılım (git'e giriyor) |
| `data/wav/<ebird_kodu>/XC*.wav` | eğitim verisi (git'e girmiyor) |
| `data/xc/kayitlar.csv` | **lisans + kaydeden + XC kimliği — atıf için saklayın** |

### Nihai veri kümesi (ölçüldü, tamamlandı)

```
178 tür · 7.111 WAV · 13,2 GB · ~79 saat ses · ortalama 39,9 sn/kayıt
24 kHz mono 16-bit — format sağlaması 30/30 doğru
172 tür tam 40 · 6 tür 39 · booeag1 (Küçük Kartal) 37
```

39'da kalan 6 tür: kotanın son kaydı ND lisanslı ya da bozuk çıkmış.
Önemsiz — eğitim için 39 ile 40 arasında fark yok.

**Bozuk indirmeler:** toplam 7 mp3 indirme sırasında bozulmuştu (ffprobe
açamıyor: *"Failed to find two consecutive MPEG audio frames"*). Silindiler.
Yeni indirme yapılırsa `ffprobe` ile sağlama yapmakta fayda var.

**Tür sayısı neden 178 (plan ~110 diyordu):** kalite filtresi düzeltilince
(aşağıda) kullanılabilir A/B kayıt sayısı 7 kat arttı, en yüksek nadir-tür
eşiği bile 178 türü geçiriyor. Cihaz tarafında sorun değil — sınıf sayısı
yalnızca son katmanı büyütüyor (178 sınıf ≈ 22,8 KB int8; 110 olsaydı
14 KB). Tensor arena sınıf sayısından bağımsız, mevsim tablosu 2,1 KB, tür
isimleri flash'ta. **Bedel bellekte değil doğrulukta:** 178 sınıf daha zor
bir problem. Eğitimden sonra karışıklık matrisine bakıp zayıf sınıfları
budamak kolay; veri elde olduğu için sonradan indirmek gerekmeyecek.

### Veri kaynakları — hangisi anahtar istiyor

| Kaynak | Anahtar | Not |
|---|---|---|
| GBIF occurrence | **hayır** | İstanbul'da 576.380 kuş kaydı, 389 tür |
| eBird taksonomi (`ref/taxonomy`) | **hayır** | `locale=tr` ile **Türkçe adlar** geliyor |
| eBird bölge listesi (`product/spplist`) | evet (403) | GBIF ile ikame edildi, gerek kalmadı |
| Xeno-canto v2 | — | **KAPANDI** (404, "no longer available") |
| Xeno-canto v3 | **evet** (401) | ücretsiz: xeno-canto.org/account |

> **GADM tuzağı:** İstanbul = `TUR.40_1`. İlk denemede `TUR.35_1` kullanıldı,
> o **Gümüşhane**; sorgu sessizce 3.325 kayıt döndürdü (doğrusu 576.380).
> İl kodunu `api.gbif.org/v1/geocode/gadm/TUR/subdivisions` ile doğrulayın.

### ⚠ ÖLÇÜLDÜ: GBIF kayıt sayısıyla 110'a inmeyin

Havuz 268 tür, plan ~110 diyor. Eşiği yükseltmek **yanlış türleri eliyor**:
eşik 912'de Guguk (839), Sarıasma (817), Orman Alaca Ağaçkakan (835), Bahçe
Tırmaşıkkuşu (837) eleniyor — tam da sesle tanınacak orman ötücüleri; yerine
Flamingo (912), Kuğu (838), martılar kalıyor.

Sebep: GBIF kayıt sayısı *"kaç kişi görüp bildirdi"*yi ölçüyor, *"ötüyor mu"*yu
değil. Su kuşları açıkta ve kolay görülüyor; orman ötücüleri duyuluyor ama
görülmüyor. **Doğru daraltma ölçütü Xeno-canto ses kaydı sayısı** — hem sesle
tanınabilirliği hem eğitim verisi mevcudiyetini aynı anda ölçer (plan §6 da
bunu diyor).

### Aylık dağılım doğrulandı

268 türün tamamı için İstanbul aylık kayıt dağılımı çekildi (Aşama 3 mevsim
önceliğinin ham verisi). Bilinen göç desenleriyle karşılaştırılarak sınandı:

```
          Oca Sub Mar Nis May Haz Tem Agu Eyl Eki Kas Ara
   +@.          Guguk          yalnizca Mart-Mayis      ✓ yaz gocmeni
  @@+..+:       Leylek         Mart-Nisan zirve         ✓ yaz gocmeni
   =@+=-+.      Ebabil         Nisan-Agustos            ✓ yaz gocmeni
*+@#=...-@%#    Kizilgerdan    kis yuksek, yaz bos      ✓ kis ziyaretcisi
==#%@=-=**==    Serce          12 ay sabit              ✓ yerlesik
```

### ⚠ M4'te yapılan HATALAR — tekrarlamayın

**1. `q:"<C"` "C'den DÜŞÜK kalite" demek (D/E), A/B değil.**
A/B kayıt isterken tam tersini sorguladım. Ölçüm (Büyük Baştankara, Avrupa):

```
filtresiz  9070  |  q:"<C"  703 (hepsi D)  |  q:">C"  4995 (A 947 + B 4048)
```

Deneme indirmesinde kalite sütununun `D` göstermesiyle yakalandı. **Doğrusu
`q:">C"` ya da `q_gt:C`.** Bu hata tüm sayımları ve dolayısıyla tür listesini
bozmuştu; sayım önbelleği ve inen dosyalar silinip baştan alındı.
*Ders: indirilen ilk dosyanın metadata'sına bak — filtre çalışmıyor olabilir.*

**2. Dosya boyutu tahmin edildi, ölçülmedi — 9 kat yanlış.**
XC metadata'sındaki `length` alanından süreyi okuyup 128 kbps varsaydım,
0,4 MB/dosya çıktı. **Gerçek: 3,7 MB/dosya.** 2,5 GB diye onay alınan iş
38,8 GB'a gidiyordu; disk %99 dolu olduğu için 26,6 GB'da durduruldu.
*Ders: birkaç dosya indirip ölç, tahmin etme. Ve indirme öncesi `df` bak.*

**3. `m4_run.py` hedefi tutturamayınca yine de indirmeye geçti.**
120 tür hedeflenmişti, eşik araması 178'de kaldı ve durup sormadan indirdi.
*Ders: gözetimsiz zincirde hedef sapması varsa dur.*

**4. `xc_fetch` yalnızca mp3'e bakıyordu.**
`xc_convert` mp3'ü silip WAV bıraktığı için, çevrilmiş türler "inmemiş"
sayılıp baştan inecekti (bir turda 26 GB boşa). Artık WAV'a da bakıyor,
ayrıca kotası dolu türü hiç sorgulamıyor.

**5. Uzun kayıtlar indirmeyi 9 kat yavaşlattı.**
`len:5-120` filtresi 2 dakikalık yüksek bitrate kayıtlara izin veriyordu;
ölçülen dosyalar 2,4 / 5,1 / **18,1 MB**. Hız 0,09 dosya/sn'ye düşmüştü
(5,5 saatlik iş). Kademeye `len:5-60` basamağı eklendi → 0,83 dosya/sn.
**Gerekçe:** BirdNET nasılsa 3 sn'lik dilimlere bölecek, 2 dakikalık kayda
ihtiyaç yok.

**6. Arka plan süreçleri bu ortamda yaşamıyor.**
`nohup ... &` ile başlatılan işler shell kapanınca ölüyor; harness'ın
`run_in_background`'u da uzun işlerde öldü. İkisinde de Python çıktıyı
tamponladığı için **log 0 bayt kaldı ve ölüm sebebi kayboldu**. Çözüm:
uzun işleri `timeout 500 python -u ...` ile **önplanda parça parça**
çalıştırmak. `-u` olmadan ölüm sebebi bir daha görünmez.

### Boyut gerçekleri (ölçüldü, tahmin değil)

```
7.244 kayıt = 78,9 saat ses · ortalama 39 sn, medyan 31 sn
mp3 26,6 GB   ->   24 kHz mono WAV 13,6 GB
```

WAV'ın küçük çıkmasının sebebi kayıtların çoğunun kısa olması. **Tek tek
dosyalarda tersi oluyor** (16 sn'lik kayıt: 0,36 MB mp3 → 0,77 MB WAV).
Uzun kayıt ağırlıklı bir kümede WAV daha büyük olurdu — bu yüzden ölçüldü.

### Lisans ve coğrafya kararları

- **ND (NoDerivatives) kayıtlar İNDİRİLMİYOR.** Guguk'un 300 kaydında
  dağılım: `by-nc-sa` 201 · `by-nc-nd` **34** · `by-nc` 4 · CC0 2 · diğer 2.
  Modeli ND kayıtla eğitmenin türev eser sayılması tartışmalı; %14 veri için
  risk alınmadı. `--nd-dahil` ile açılır.
- Kalanın çoğu **BY-NC-SA**: kişisel kullanımla uyumlu, **ticari dağıtımla
  değil** — BirdNET'in CC BY-NC-SA kısıtıyla aynı sınıftan sorun.
- API **negatif lisans filtresi desteklemiyor** (`-lic:` → HTTP 400);
  eleme indirme sırasında metadata'dan yapılıyor.
- Coğrafya `area:europe`. **Türkiye kayıtları pratikte yok** (Guguk 1,
  Kızılgerdan 0, Büyük Baştankara 4). Kuş sesinde bölgesel lehçe gerçek
  olduğu için üreme bölgesi tercih edildi; Avrupa'da kaydı olmayan tür için
  dünya geneline düşülüyor.

---

## 9e. M4 adım 3 — BirdNET segmentasyonu ✅ TAMAMLANDI

### Araçlar

| Dosya | Ne | Hangi python |
|---|---|---|
| [`tools/birdnet_slist.py`](tools/birdnet_slist.py) | tür listesi **ve** `ebird_kodu → BirdNET etiketi` haritası | `.venv-birdnet` |
| [`tools/birdnet_run.py`](tools/birdnet_run.py) | 7.111 kaydı 3 sn'lik dilimlere ayırıp skorları yazar | `.venv-birdnet` |
| [`tools/birdnet_ozet.py`](tools/birdnet_ozet.py) | sonuçları tek segment tablosuna indirir, zayıf türleri bildirir | 3.14 (stdlib) |
| [`tools/segment_kes.py`](tools/segment_kes.py) | dinleyip doğrulamak için örnek dilim keser | 3.14 (stdlib) |

```bash
.venv-birdnet\Scripts\python -u tools/birdnet_slist.py
.venv-birdnet\Scripts\python -u tools/birdnet_run.py --isci 10
python tools/birdnet_ozet.py
python tools/segment_kes.py --adet 10
```

`birdnet_run.py` **yeniden başlatılabilir** — yarıda kesilirse aynı komut
kaldığı yerden devam eder (`skip_existing_results`). Bu oturumda işe yaradı:
koşu %62'de kesildi, aynı komutla tamamlandı.

### Planın üç maddesi ölçümle değişti

**1. `--slist` VERİLMİYOR.** Plan 178 türlük liste vermeyi öngörüyordu.
Kurulu sürümde tür listesi filtresi **çıkarımdan sonra** uygulanıyor
(`analyze/utils.py:689`) — yani hiç hız kazandırmıyor, sadece satır eliyor.
Listesiz koşunca aynı sürede iki şey fazladan geliyor:

```
0.0-3.0  Engine                   0.2877   <- kus disi sinif
3.0-5.4  Corvus cornix  (hedef)   0.5201
3.0-5.4  Corvus corone  (akraba)  0.4368   <- karisma sinyali
```

`Engine`/`Human vocal`/`Dog`/`Siren` gibi kuş dışı sınıflar **negatif
madenciliği için kanal uyumlu negatif** demek (§9f-4); akraba tür skoru da
bulaşık dilimi ayıklamaya yarıyor. 178 türe süzme özet aşamasında yapılıyor.

**2. BirdNET'in kendi süreç havuzu kullanılmıyor** — kilitleniyor, §5.15.
Paralellik 10 bağımsız süreçte, ölçülen hız **124 dosya/dk**.

**3. Ses kesilmiyor, DİZİN çıkarılıyor.** Plan `birdnet_analyzer.segments`
ile dilimleri ayrı WAV'lara kesmeyi öngörüyordu. Tür başına ~400 dilim ×
178 tür × 144 KB ≈ **10 GB** eder; diskte 28 GB kalmıştı. Dizin birkaç MB
ve eğitim zaten orijinal WAV'dan istediği ofsetten mel çıkarabiliyor.
Kesme yalnızca dinlenecek örnekler için (`segment_kes.py`).

### segmentler.csv — eğitimin okuyacağı tablo

| Sütun | Ne |
|---|---|
| `ebird_kodu`, `dosya` | hangi türün hangi kaydı |
| `baslangic`, `bitis` | dilimin saniye cinsinden yeri |
| `hedef_guven` | dizinin türü bu dilimde ne kadar güvenle duyuldu |
| `en_iyi_tur`, `en_iyi_guven` | dilimin en yüksek skorlu türü — hedef değilse **bulaşık dilim** |
| `kus_disi_tur`, `kus_disi_guven` | Engine / Human vocal / Dog / Siren... |

`hedef_guven = 0` "sessizlik" demek değil: BirdNET 0.1'in altını hiç
yazmıyor. Dilimin kaydın neresine düştüğü `baslangic`'ta duruyor.

### ⚠ Bu türde iki sessiz hata vardı — biri yakalandı

Tür eşlemesi bilimsel adla yapılırsa **iki tür sessizce eğitim dışı kalıyor**
(Küçük Karga, Ak Karınlı Ebabil). Ayrıntı ve çözüm §5.16. Eşleme artık eBird
kodu üzerinden ve `data/birdnet_ad_haritasi.csv`'de görünür.

### Elimizde ne var (envanter)

```
data/species_istanbul.csv     178 tür "dahil" + 90 tür "elendi" (gerekçeli)
                              sütunlar: ebird_kodu, bilimsel_ad, turkce_ad,
                              ingilizce_ad, gbif_kayit, xc_ab/xc_ab_eu/xc_ab_sa,
                              ay_01..ay_12, durum, gerekce
data/wav/<ebird_kodu>/XC*.wav tür başına ≤40 kayıt, 24 kHz mono 16-bit
data/xc/kayitlar.csv          her dosyanın lisansı + kaydedeni + XC kimliği
data/.xc_key                  XC API anahtarı (git'e girmiyor)
```

Kontrol komutu:
```bash
python -c "import os;print(sum(len(os.listdir('data/wav/'+d)) for d in os.listdir('data/wav')),'WAV')"
```

### Neden segmentasyon şart

Xeno-canto kayıtlarının büyük kısmı sessizlik, arka plan türü ve kaydedenin
konuşması. 40 kaydın tamamını "bu tür" diye etiketlemek modeli sessizliği ve
yanlış türü ezberlemeye iter. BirdNET her 3 saniyelik dilimde hangi tür var,
onu söylüyor — plan §6 bunu *"veri kalitesini dramatik biçimde artırır"*
diye işaretliyor.

Ayrıca BirdNET'in **çıkış olasılıkları öğretmen sinyali** olarak saklanacak:
M5'teki damıtma (distillation) bunları kullanacak. Yani sadece "hangi dilim"
değil, "BirdNET ne kadar emin" de kaydedilmeli.

### Kurulum — nasıl çözüldü

Makinede **Python 3.11 ve 3.14** var; BirdNET-Analyzer 2.4.0 3.11'e kuruldu:

```bash
py -3.11 -m venv .venv-birdnet
.venv-birdnet\Scripts\python -m pip install --use-feature=truststore --upgrade pip
.venv-birdnet\Scripts\python -m pip install birdnet-analyzer
```

İkinci satır şart — yoksa venv'in pip'i sertifika hatasıyla hiçbir şey
indiremiyor (§5.13). Model (214 MB) paketin kendi adresinden inmiyor,
GitHub'dan alındı (§5.14). Kurulum doğrulaması:

```bash
.venv-birdnet\Scripts\python -c "from birdnet_analyzer.utils import check_birdnet_files; print(check_birdnet_files())"
```

**`--slist` biçimi doğrulandı** (plan "DOĞRULANMADI" diyordu): paketin
`labels/V2.4/*.txt` dosyalarında satırlar `Bilimsel ad_İngilizce ad`.
Ama İngilizce adı kendi CSV'mizden yazmayın — modelin kendi etiket
dosyasıyla birebir tutmazsa BirdNET satırı sessizce yok sayar. Eşleme
`eBird_taxonomy_codes_2024E.json` üzerinden yapılıyor (§5.16).

**`--min_conf` 0.25 değil 0.1:** yumuşak etiket topluyoruz. Düşük güvenli
dilimler de damıtma için bilgi taşıyor; eşiği eğitim tarafında yükseltmek
kolay, atılan veriyi geri getirmek için 79 saatlik analizi tekrarlamak
gerekir.

### ⚠ 24 kHz meselesi — bilerek böyle

BirdNET 48 kHz bekliyor, bizim WAV'lar 24 kHz. BirdNET kendi yeniden
örneklemesini yapar; yukarı örnekleme bilgi eklemez ama **bu tutarlılık
istenen bir şey**: cihazın mikrofonu da 12 kHz üstünü hiç duymuyor
(24 kHz örnekleme, Nyquist 12 kHz). BirdNET'i cihazın gerçekten duyacağı
bantla besliyoruz, dolayısıyla öğretmen sinyali cihazın görebileceği
dünyayla uyumlu oluyor.

Bedeli: 9–11 kHz'de öten türlerde (Çalıkuşu, Tırmaşıkkuşu) BirdNET biraz
daha az emin olabilir. Kabul edilmiş bir bedel — orijinal mp3'ler silindi,
geri dönüş yok. Bu türlerde tespit sayısı çok düşük çıkarsa `--min_conf`
onlar için ayrıca düşürülebilir.

### Sonuç — ölçüldü (2 Ağustos 2026)

```
178 tür · 7.111 kayıt · 95.033 dilim (3 sn) tarandı
79.932 dilim en az bir tespit aldı (%84)
data/segmentler.csv  ·  79.932 satır

hedef güven >= 0.10 : 63.059 dilim   tür başına 354
hedef güven >= 0.25 : 58.151 dilim   tür başına 327   <- hedef >=100 idi
hedef güven >= 0.50 : 52.415 dilim   tür başına 294
```

Koşu: 10 işçi, **~245 dosya/dk**, toplam ~50 dk (yarıda kesilip devam etti).

**0.25 eşiğinde 100 dilimin altında kalan 6 tür** — M5'te sınıf dengesizliği
olarak karşımıza çıkacak, focal loss ve veri artırma bunları hedefleyecek:

| Tür | Dilim | Hedef en yüksek |
|---|---|---|
| Alaca Balıkçıl (`squher1`) | 25 | %30 |
| Küçük Ak Balıkçıl (`litegr`) | 50 | %32 |
| Büyük Ak Balıkçıl (`greegr`) | 53 | %36 |
| Kara Karınlı Kumkuşu (`dunlin`) | 83 | %67 |
| Çaprazgaga (`redcro`) | 88 | %45 |
| Yalıçapkını (`comkin1`) | 89 | %58 |

Altısının üçü **balıkçıl**. "Hedef en yüksek" %30–36 demek: tespit alan
dilimlerin üçte ikisinde kayıttaki baskın ses başka bir kuş. Sebep tahmin
değil, desen belli — balıkçıllar az ötüyor ve XC kayıtları sulak alanda,
yani başka türlerle dolu. Bu türlerde ya eşik ayrıca düşürülmeli ya da
`en_iyi_tur` sütununa bakıp bulaşık dilimler ayıklanmalı.

> ⚠ **Kuş dışı ses içeren dilim yalnızca 204** (95.033 içinde binde 2).
> Bir ara bunu "negatif madenciliği için bedava kaynak" diye not etmiştim;
> **ölçünce öyle çıkmadı**. XC kayıtları temiz kayıtlar, içlerinde araba ve
> insan sesi neredeyse yok. Negatif sınıfı için gerçek kaynak lazım
> (§9f-4).
>
> İkinci aday: hiç tespit almayan **15.101 dilim** (95.033 − 79.932).
> Bunlar sessizlik ve belirsiz arka plan. Kullanılabilir ama **riskli** —
> içlerinde eşiğin altında kalmış zayıf kuş sesi olabilir, negatif diye
> öğretilirse Aşama-1 gerçek kuşu reddetmeyi öğrenir.

### Kabul ölçütü — durum

| Ölçüt | Durum |
|---|---|
| Tür başına ≥100 segment (3 sn) | ✅ **327/tür** (0.25 eşiğinde) — yukarıdaki sayılar |
| Zayıf türler not edilmeli | ✅ `birdnet_ozet.py` 0.25 eşiğinde 100 altını listeliyor |
| BirdNET güven skorları saklanmalı (öğretmen sinyali) | ✅ `segmentler.csv` + kayıt başına ham CSV |
| **Rastgele segmentler DİNLENMELİ** | ✅ **kullanıcı 10 örneği dinledi: onunda da kuş sesi duyuluyor** |

**Dinleme yapıldı ve geçti.** Bu projede dolaylı ölçüme fazla güvenmek iki
kez pahalıya patladı (§5.10); segmentasyonda tek doğrudan gözlem dinlemekti,
o yüzden atlanmadı.

`data/segment_ornek/` altındaki 10 örnek kullanıcıya gönderildi ve dinlendi:
**onunda da kuş sesi duyuluyor.** Objektif sağlamalar da geçmişti — dilimler
tam 3,00 sn, 24 kHz, RMS -25…-43 dBFS (sessizlik değil) ve onunda da hedef
tür dilimin en yüksek skorlu türü.

> Kapsamı olduğu gibi yazalım: doğrulanan **"dilimde kuş sesi var"**.
> Türün doğruluğu tek tek teyit edilmedi (10 farklı tür, kulaktan ayırt
> etmek uzmanlık ister). Segmentasyonun asıl işi olan *sessizlik ve boşluk
> ayıklama* için bu yeterli; tür doğruluğu zaten M5 sonrası karışıklık
> matrisinde ölçülecek.

```bash
python tools/segment_kes.py --adet 10        # rastgele 10 dilim kes
python tools/segment_kes.py --tur eurbla --esik 0.9 --adet 5
```

Dosya adı `<kod>_<XCkimligi>_<baslangic>.wav`; ekrana hedef güveni ve
dilimin en yüksek skorlu türü de basılıyor, dinlerken karşılaştırın.

---

## 9f. Sonraki adımlar (ARCHITECTURE §6)

1. ✅ Tür listesi — 178 tür
2. ✅ Kayıt indirme + 24 kHz mono WAV dönüşümü — **7.111 dosya, doğrulandı**
3. ✅ **BirdNET segmentasyonu** — §9e, `data/segmentler.csv`
4. ⏸/✅ **Negatif: SAHA TURU ertelendi, SINIF dolduruldu (§9j).** ESC-50
   indirildi ve kuş sınıfları çıkarıldı; ayrıntı ve ölçülen tuzak §9j'de.
   Aşağıdaki metin kararın gerekçesi olarak duruyor.

   Plan *"atlanırsa cihaz sahada kullanılamaz"* diyor. Kullanıcı yine de
   erteledi ve **gerekçesi sağlam**: ilk doğrulama testi *boş bir odada
   bilgisayardan kuş sesi çalarak* yapılacak. O senaryoda şehir gürültüsü
   yok, dolayısıyla negatifler o testi hiç etkilemiyor. Öncelik tür tanımayı
   çalıştırmak. Saha kaydı turu (ezan, vapur, simitçi, trafik) **M8 saha
   kalibrasyonuna** kaydı.

   > **AMA ikisini karıştırmayın:** *toplama* askıya alındı; **negatif
   > sınıfı yine de doldurulmak zorunda.** Aşama-1'in "kuş değil" ve
   > Aşama-2'nin "bilinmiyor" sınıflarına bir şey konmazsa model her sesi
   > bir kuşa atar — boş odada bile, kendi nefesinizi bile.
   >
   > **ÖLÇÜLDÜ, tahmin değil:** kendi kayıtlarımızdan çıkan kuş dışı dilim
   > sayısı **204** (95.033 içinde). Bir ara bunu yeterli bir kaynak sandım;
   > **değil**. XC kayıtları temiz, içlerinde şehir sesi yok.
   >
   > Askıya alma kararını bozmayan çözüm: **ESC-50 indirmek** (879 MB,
   > anahtar gerekmiyor, aşağıdaki tabloda). Bu bir *indirme*, saha turu
   > değil — kullanıcının ertelediği şey dışarı çıkmaktı. `chirping_birds`
   > sınıfını çıkarmayı unutmayın. İstanbul'a özgü sesler (ezan, vapur,
   > simitçi) M8'e kalıyor; onlar doğruluğu artırır ama ilk çalışan sürüm
   > için şart değil.

   Saha turuna dönüldüğünde araştırılmış kaynaklar (anahtarsız):
   | Kaynak | Lisans | Not |
   |---|---|---|
   | **ESC-50** (github.com/karolpiczak/ESC-50) | CC BY-NC | 2000 klip, 50 sınıf: korna, siren, motor, yağmur, rüzgâr, köpek. 879 MB. **`chirping_birds` sınıfı ÇIKARILMALI** — negatife kuş sesi karışırsa Aşama 1 bozulur |
   | Zenodo şehir sesi setleri | CC BY-4.0 | "Isolated urban sound database", "STeLiN-US". NC kısıtı yok, lisans daha temiz |
   | Freesound | anahtar gerekli | İkinci bir API anahtarı istememek için kaçınıldı |

   **En değerli negatifler hazır sette YOK:** ezan, vapur düdüğü, simitçi,
   İstanbul trafiği, martı gürültüsü. Bunlar **cihazın kendisiyle**
   kaydedilmeli (`r` komutu, `python tools/capture_wav.py --port COM13
   --out ezan.wav`). Avantajı çift: plan "İstanbul'a özgü" diyor **ve**
   kanal uyumu birebir oluyor — aynı mikrofon, aynı ES8311 kazancı, aynı
   DSP zinciri. Alan kaydı için cihazı yanınıza alın.

   Bu negatifler iki yerde kullanılacak: Aşama 1'in negatif sınıfı ve
   Aşama 2'nin "bilinmiyor" sınıfı.

5. ⏳ **Veri artırma** — zaman kaydırma, pitch/tempo, negatiflerle çeşitli
   SNR'lerde gürültü karıştırma, SpecAugment, oda/mesafe simülasyonu.
   **Dikkat:** artırma mel'den SONRA (SpecAugment) ya da ham sesten önce
   yapılmalı; ham seste yapılıyorsa pencere `tools/egitim_kumesi.py`'nin
   `mel_penceresi()`'nden geçmeli, elle mel yazılmamalı (§9j).
6. ⏳ **M5**: damıtma ile eğitim (BirdNET yumuşak çıktıları öğretmen —
   `ogretmen.npy` hazır), focal loss (sınıf dengesizliği), INT8
   niceleştirme, doğruluk raporu

### M5'e girerken hatırlanacak kısıtlar

- Girdi **64×187 int8** (mel penceresi) — cihazdaki `pb_mel_window()` çıktısı
- Hedef **≤30 MMAC/pencere**, tensor arena **≤180 KB**
- Bellek bütçesi **ölçüldü ve yer açıldı** (§9g): bss 127.084, arena ile
  birlikte 311.404 → ~208 KB pay. Model boyutu bu rakama göre seçilebilir.
- Mel parametreleri değiştirilirse cihaz tarafı da değişmeli: HTK mel,
  alan normalizasyonu YOK, periyodik Hann (§9c). Bu iki ayrıntı tutmazsa
  model sessizce kötü çalışır.

---

## 9g. `s_capture` temizliği ✅ TAMAMLANDI (M6'nın önünü açtı)

> **Kullanıcı kararı (2 Ağustos 2026):** eğitim kümesinden ÖNCE yapıldı.
> Gerekçe: M5'te seçilecek model boyutu tensor arena bütçesine bağlı; bütçe
> artık ölçülmüş bir rakam, M5'e gerçek sayıyla girilebilir.

### Başlangıç durumu (değişiklikten önce)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
~/.platformio/packages/toolchain-rp2040-earlephilhower/bin/arm-none-eabi-size.exe build/pokebird.elf
```

```
   text     data      bss
 461952        0   218988      <- 2 Agustos 2026'da olculdu
```

> §7'de bss "206 KB" yazıyordu; **gerçek 218.988 bayt (213,9 KB)**. Kaymış.
> Karar vermeden önce `size` çalıştırın, belgedeki sayıya güvenmeyin.

bss'in içindekiler:

| | bayt |
|---|---|
| **`s_capture`** (main.c:70, 2 s @ 24 kHz) | **96.000** |
| sürekli yakalama halkası (16 KB, hizalı) | 16.384 |
| LVGL çizim tamponu (640×20 px) | ~25.600 |
| mel halkası (64×187 int8) | 11.968 |
| kalanı | ~69.000 |

TFLM arena'sı **180 KB**. 218.988 + 184.320 = 403.308 → 520 KB'a sığıyor
gibi ama yığın/heap payı kalmıyor.

### SONUÇ — ölçüldü

```
bss  218.988  ->  127.084 bayt        (91.904 serbest = tam 96.000 - 4.096)
text 461.952  ->  463.016 bayt        (+1.064; akis dongulerinin bedeli)
arena ile birlikte 311.404  ->  ~208 KB yigin/heap payi
```

Kabul ölçütü ≤130.000 idi; **127.084** ile karşılandı.

### Yapılan: 96 KB'lık tamponu 4 KB'lık parçaya indirmek

`s_capture` tek bir 2 saniyelik bitişik tampon. **Onu kullanan hiçbir teşhis
komutunun aslında 2 saniyeyi bir arada görmesi gerekmiyor** — hepsi ya
biriktirici (RMS, tepe, DC) ya da pencere pencere çalışıyor. Sürekli yakalama
halkası (M3, §9c) geldiğinden beri veriyi parça parça okumak mümkün.

```c
/* main.c:70 — yerine */
#define CHUNK_SAMPLES 2048          /* = PB_AUDIO_MAX_READ, 4 KB */
static int16_t s_chunk[CHUNK_SAMPLES];
```

Gerçekleşen kazanç: **96.000 → 4.096 bayt.**

### Kullanan yerler — ne yapıldı

| Komut | Ne istiyordu | Ne oldu |
|---|---|---|
| `e` EMI | 0,5 s istatistik | `stream_stats()` yardımcısı — parça parça oku, biriktir |
| `n` gürültü | 2 s istatistik + pencere yüzdelik | pencere = okuma birimi (1024 örnek × 48) |
| `l` seviye | 100 ms istatistik | 85 ms'e (2048) indi; canlı gösterge olduğu için `pb_audio_capture` **doğru** olan |
| `r` kayıt | 2 s'yi PC'ye aktar | parça parça akıtılıyor, biçim değişmedi |
| `s` spektrogram | 512 örnek | sadece tampon adı değişti |

Yeni yardımcılar (`src/main.c`): `stats_acc_t` + `stats_add` + `stats_finish`
(tek geçişli istatistik), `stream_stats` (oku-ve-biriktir döngüsü),
`window_rms`, `percentile10`.

### ⚠ Dört tuzak — hepsi gerçekti, nasıl geçildi

**1. `pb_audio_capture`'ı parça başına ÇAĞIRMAYIN.** O fonksiyon flush + read
sarmalayıcısı; her çağrıda birikmişi atar, parçalar arasındaki örnekler
sessizce düşerdi. Uygulanan: `pb_audio_stream_flush()` **bir kez**, sonra
`pb_audio_stream_read()` döngüsü. `stream_stats`'ın başındaki yorum bunu
anlatıyor ki bir sonraki okuyan aynı tuzağa düşmesin.

> Tek istisna `l` (canlı seviye): orada her turda en tazeye atlamak zaten
> **isteniyor**, o yüzden `pb_audio_capture` bilerek korundu.

**2. `compute_stats` İKİ geçişliydi.** Varyans özdeşliğiyle tek geçişe
çevrildi (`rms² = sumsq/n − (sum/n)²`). **Host tarafında ölçülerek
doğrulandı** (kabul sınırı 0,1 dB idi):

```
karttan alinan gercek kayit (before.wav, DC 14.3):
  ESKI (2 gecis) RMS 447.149577093   YENI (1 gecis) RMS 447.149577093
  sapma 3.6e-14 dB
zor durum (DC 20000 uzerine +-3 AC, 80 dB fark alma):
  sapma 3.6e-08 dB
```

**3. `noise_floor_dbfs` 64 pencereye bölüyordu** (750 örnek, parça sınırına
hizasız). 48 pencere × 1024 örnek = 49.152 örnek = **2,048 s** yapıldı.
Yüzdelik indeksi `count/10` olduğu için seçilen eleman 6/64 (%9,4) yerine
4/48 (%8,3) — yani biraz daha sessiz bir pencere. Ölçülen etki **0,6 dB**
(aşağıdaki kabul ölçütü 3), yönü de bu kaymayla tutarlı. M1'in -36 dBFS
tabanıyla karşılaştırma yapılırken akılda tutun.

**4. `r` komutunun çerçevesi.** Yeni sıra: başlık → örnekler → `#WAV-END` →
istatistik. Buna **beşinci bir ayrıntı** eklendi: başlık yazıldıktan sonra
`samples=N` sözü verilmiş oluyor, geri dönüş yok. Bu yüzden **ilk parça
başlıktan ÖNCE** okunuyor; saat yoksa hiç başlık yazılmadan çıkılıyor.

> Yan etki: `capture_wav.py` `#WAV-END`'de okumayı bıraktığı için istatistik
> satırını **artık göstermiyor** (seri terminalde görünüyor). Kayıt özeti
> aracın kendi hesabından geliyor, bilgi kaybı yok.

### ⚠ BEŞİNCİ tuzak — belgede yoktu, ölçümle bulundu

Yazdırma artık okumayla iç içe geçiyor. Aktarım gerçek zamandan yavaş kalırsa
halka (170 ms) taşar ve WAV'da kopukluk olur — **tam da kabul ölçütü 2'nin
yasakladığı şey.** `main.c`'deki yorum "2 saniye için ~250 KB metin, USB
CDC'de birkaç saniye sürer" diyordu; bu ~83 KB/s demek olurdu ve gereken
102 KB/s'nin altında kalırdı. O rakama güvenip ondalık biçimi terk etmek
(base64/ikili) planlanmıştı.

**Ölçünce öyle çıkmadı** (eski firmware, `r`, gövde 208.327 bayt):

```
saf aktarim 0,74 s  ->  276 KB/s        gereken: 24000 x 4,34 = 102 KB/s
                                        pay: 2,7 kat
```

Yani **ondalık biçim rahatça yetiyor** ve protokolü değiştirmeye gerek yok:
`tools/capture_wav.py` hiç dokunulmadan çalışıyor. Yeni firmware'de ölçülen:

```
basliktan #WAV-END'e 1,98 s   (48.000 ornek = 2,00 s -> tam gercek zaman)
komuttan #WAV-END'e  2,08 s   (eski: 2,85 s -> %27 daha hizli,
                               cunku yakalama ve aktarim artik ust uste biniyor)
```

Yine de pay sonsuz değil: PC tarafı 170 ms'den uzun takılırsa halka taşar.
Bu yüzden her parçanın `fifo_overrun`'ı toplanıp sonda **yüksek sesle**
bildiriliyor ("ORNEK DUSTU ... WAV'i olcum icin KULLANMAYIN"). Bu projede
sessiz bozulma iki kez pahalıya patladı (§5.10); kopukluk sessizce geçmemeli.

> **Ders (yine aynısı):** kaynak kodundaki bir yorumdaki performans tahmini
> ölçüm değildir. Buradaki tahmin 3,3 kat yanlıştı ve gereksiz bir protokol
> değişikliğine yol açacaktı. §9d-2'deki "dosya boyutu 9 kat yanlış"ın aynısı.

### Kabul ölçütü — durum

| # | Ölçüt | Sonuç |
|---|---|---|
| 1 | bss ≤ 130.000 bayt | ✅ **127.084** (218.988'den) |
| 2 | `r` çalışıyor, WAV süreksizlik içermiyor | ✅ objektif · 🔶 dinleme aşağıda |
| 3 | `--cmd n` tabanı öncesiyle ±1 dB içinde | ✅ **0,6 dB** fark |
| 4 | `--cmd m` 62–63 kare/s, kayıp 0 | ✅ **63 kare/s, kayıp 0** |
| 5 | `--cmd a` tam demo ekranda çalışıyor | ✅ (önce düştü, §9h'de çözüldü) |

> ⚠ **Bu satırı bir ara "✅ 63 kare/s" diye yazmıştım — YANLIŞTI.** Ölçülen
> şey yalnızca ses hattının kare hızıydı; ölçütün asıl kısmı olan "ekranda
> çalışıyor" hiç doğrulanmamıştı. Kullanıcı ekrana bakınca panelin
> karıncalanma gösterdiği ortaya çıktı. *Ders, §5.10'un aynısı: göz
> gerektiren bir ölçütü göz gerektirmeyen bir ölçümle geçmiş saymayın.*

**Ölçüt 3 — aynı odada, aynı oturumda, önce/sonra:**

```
ONCE (eski firmware, 64 pencere)  -37.2  -37.8  -37.4  dBFS   ort -37.47
SONRA (yeni firmware, 48 pencere) -38.1  -38.5  -37.6  dBFS   ort -38.07
fark 0,6 dB — sinir 1 dB. Yonu de 4. tuzaktaki yuzdelik kaymasiyla tutarli.
```

> Bu karşılaştırma için **eski firmware kasten geri yüklendi**: belgedeki eski
> sayılarla değil, aynı gün aynı odada ölçülen sayılarla karşılaştırmak için.
> Oda ölçüm sırasında değişti (tam pencere RMS önce -18…-29 dB, sonra
> -36…-37 dB) — P10 metriği tam da bunun için var ve gerçekten dayandı.

**Ölçüt 2 — süreksizlik ölçümü.** Kopukluk olsaydı **parça sınırında** (her
2048 örnekte, yani 85 ms'de bir) görünürdü. Ardışık örnek farkları:

```
                    medyan  %99.9   maks
after.wav genel        110    563    820
  parca sinirlari (23 nokta)  maks 329   ort 123.9
  parca ortalari  (22 nokta)  maks 327   ort 101.9
```

Parça sınırları parça ortalarından **ayırt edilemiyor** (329 vs 327) ve ikisi
de sinyalin kendi maksimumunun (820) altında. Ayrıca cihazın kendi raporunda
`[!]` yok: ne `ORNEK DUSTU` ne eksik örnek. 48.000/48.000 gönderildi.

🔶 **Kullanıcı doğrulaması bekliyor (2 madde).** Bu projede dolaylı ölçüm iki
kez yanılttı (§5.10), o yüzden objektif sağlamalar yeterli sayılmadı:
- `after.wav` dinlenecek (before.wav ile birlikte gönderildi)
- `a` demosu ekranda görülecek (`--sure 25` ile 2 kez çalıştırıldı)

### Bu turda ayrıca görülen

- **`e` (EMI) komutu artık gerçekten aç/kapa ölçüyor.** §6'daki "EMI ölçümü
  geçersiz" borcu PWM'den kaynaklanıyordu; arka ışık düz GPIO'ya alındığından
  beri `measure_with_backlight(false/true)` gerçek bir kapalı/açık
  karşılaştırması yapıyor. Ölçülen: **kapalıya göre +0,4 dB.** *Ama borç tam
  kapanmadı:* dört ölçümün son üçü hâlâ `PWM %50` / `PWM %10` diye
  etiketleniyor, oysa üçü de aynı "açık" durumu. Etiketler düzeltilip
  yeniden çalıştırılırsa borç kapanır — küçük iş, bu turun kapsamı değildi.
- `l` komutu `--sure` desteklemiyor (yalnızca `m` ve `a` destekliyor);
  `--cmd l` ile çalıştırılırsa **hiç bitmez** ve portu tutar. Canlı akıştığı
  için aracın 4 sn'lik sessizlik çıkışı hiç tetiklenmiyor.

### Bundan sonra

§9h çözüldü. Sıradaki iş: eğitim kümesi → M5 (§9i).

---

## 9h. Ekran gerilemesi ✅ ÇÖZÜLDÜ — panel hazır olma penceresi

### Belirti

`a` demosunda panel **karıncalanma** gösteriyor: her pikselin farklı renk
olduğu, değişmeyen bir kar deseni — §5.9'daki "panel kendi başlatılmamış
GRAM'ını gösteriyor" tablosunun aynısı. Tek istisna sol alt köşede küçük bir
koyu kutu ve içinde LVGL'in yazıları okunuyor, yani **LVGL'in çizdiği alanın
bir kısmı panele ulaşıyor**, gerisi ulaşmıyor.

Ses tarafı bu sırada tamamen sağlam: 63 kare/s, kayıp 0.

### Üç koşuluk A/B — kanıt

Kullanıcı üç ikiliyi de ekranda gördü. Bu tablo teşhisin tamamı:

| İkili | Mantık | bss | Ekran |
|---|---|---|---|
| `old` (commit `5a5bdf0`) | eski | 218.988 | ✅ **düzgün** |
| `new` (commit `1ddf465`, HEAD) | yeni | 127.084 | ❌ **bozuk** |
| `probe` (yeni mantık + eski yerleşim) | **yeni** | 218.988 | ✅ **düzgün** |

`probe` = HEAD'in kodu, tek fark `s_chunk`'ın **48000 elemanlı** bırakılması
(yalnızca ilk 2048'i kullanılıyor). Yani:

> **Mantık değişikliklerim ekranı bozmuyor. Bozan şey yalnızca bss
> yerleşiminin kayması.** Kodda zaten var olan gizli bir sınır dışı yazma,
> eskiden zararsız bir yere düşerken şimdi canlı bir tamponu eziyor.

`probe`'u yeniden üretmek (tek satır, commit etmeyin):

```c
/* src/main.c, s_chunk tanimi */
static int16_t s_chunk[48000];   /* yerine: s_chunk[CHUNK_SAMPLES] */
```

### Bellek haritası — kurbanı daraltan asıl ipucu

`nm --size-sort -S -td build/pokebird.elf`, bss, adrese göre:

```
                       PROBE (calisan)        YENI (bozuk)
s_ring (ses, 16 KB)    536887296              536887296     ] ayni
work_mem_int (LVGL)    536904464              536904464     ] adres
blok.4                 536929840              536929840     ]
pencere.0 (mel)        536937392              536937392     ]
pencere.2 (mel)        536949360              536949360     ]
s_chunk                536961368  96000       536961368  4096
  (344 bayt bosluk = s_row, lcd_blit.c'nin satir tamponu)
s_draw_buf (LVGL)      537057712              536965808     ] 91.904
s_ring (mel)           537088508              536996604     ] bayt
s_weights              537100824              537008920     ] kaydi
```

Nesne **sırası iki derlemede de aynı**. `s_chunk`'tan öncekiler aynı adreste,
sonrakiler blok hâlinde 91.904 bayt kaydı. Bundan çıkan sonuç dar:

- Kaydıran nesnelerin **birbirine göre** konumu değişmedi → aralarındaki bir
  taşma iki derlemede de aynı davranırdı. **Eleyin.**
- Değişen tek ilişki: `s_chunk`'tan **ÖNCEKİ** nesnelerle **SONRAKİLER**
  arasındaki mesafe. Önceki bir tampondan ileri taşan bir yazma, probe'da
  96.000 baytlık `s_chunk`'ın ortasına zararsızca düşüyor; yeni yerleşimde
  4 KB'ı aşıp **`s_row` ve `s_draw_buf`'a** (LVGL çizim tamponu) ulaşıyor.

**Şüpheliler — `s_chunk`'tan önce duran ve `a` demosunun kullandığı nesneler:**
`pencere.0` / `pencere.2` (mel halkaları, 64×187 = 11.968 bayt, hemen
`s_chunk`'ın önünde), `work_mem_int` (LVGL havuzu), `s_ring` (ses halkası).
Mel hattı demoda sürekli çalıştığı için **mel halkaları birinci şüpheli**.

**`s_chunk`'ın kendisi taşmıyor — elendi.** Yeni firmware yüklendikten sonra
gönderilen İLK komut `a` idi; `s_chunk`'ı kullanan hiçbir komut (`n`, `r`,
`e`, `l`, `s`) hiç çalışmamıştı. Kurban `s_chunk` olabilir ama fail o değil.

### Kanarya testi — bellek bozulmasını ELEDİ

`s_chunk` 96.000 baytlık bir "dedektör şeridi" olarak `0xA5` ile dolduruldu
(`pencere.2`'nin hemen arkasında duruyor), demo 12 s çalıştırıldı, sonra
taranarak raporlandı. Sonuç: **bozulma yok, şerit hiç yazılmamış.**

Yani ortada sınır dışı yazma **yoktu**. Yukarıdaki "kurban" teorisi yanlıştı;
bellek haritası doğru okunmuştu ama yanlış mekanizmaya bağlanmıştı.

> `v` teşhisi bu turda da yalan söyledi: çalışan ve bozuk ikilide çıktısı
> **birebir aynı** ("panel komutlara uymuyor", register okumaları hep 00).
> §5.10'un tarif ettiği tuzağın aynısı — bu paneldeki TE ve okuma testleri
> hiçbir şey kanıtlamıyor, gösterge olarak kullanmayın.

### ✅ GERÇEK KÖK NEDEN — panel hazır olma penceresi (zamanlama)

AXS15231B **açılıştan sonra kısa bir süre başlatma dizisini kabul etmiyor.**
Erken başlatılırsa panel kendi başlatılmamış GRAM'ını göstermeye devam ediyor
(karıncalanma) ve bu durum **KALICI**: GPIO34 dışarıdan yüksek tutulduğu için
panele donanım reset'i atamıyoruz (§5.11), yani yeniden deneme şansı yok.

bss 218.988 → 127.084 inince crt0'ın bss sıfırlaması kısaldı ve firmware panel
başlatmaya **~1 ms daha erken** varmaya başladı. Kartta ölçüldü:

| Gecikme | Ekran |
|---|---|
| yok | ❌ bozuk |
| 20 ms | ✅ düzgün |
| 500 ms | ✅ düzgün |

> **Eski hâl bu pencereyi KIL PAYI geçiyormuş.** `s_capture` temizliği bir
> hata *yaratmadı*, var olan payı bitirdi. Hata kodda zaten duruyordu ve
> M6'da TFLM arena'sı yerleşimi kaydırdığında çok daha pahalı bir yerde
> patlayacaktı.

### Düzeltme — neden `sleep_ms` değil

```c
/* main.c, QSPI_GPIO_Init'ten hemen once */
while (to_ms_since_boot(get_absolute_time()) < 250) sleep_ms(5);
```

Sabit uyku yalnızca o anki paya sabit bir miktar ekler; kendinden **önceki**
kod hızlanırsa aynı tuzak yeniden kurulur — bizi buraya tam olarak bu düşürdü.
Mutlak alt sınır kısıtın kendisini ifade ediyor ve önceki kodun süresinden
bağımsız. USB beklemesi zaten uzun sürdüyse hiç beklemiyor, normal koşulda
bedeli sıfır. Ölçülen eşik ≤20 ms; 250 ms bilerek cömert.

**Bu satırı silmeyin ve `sleep_ms`'e çevirmeyin.**

### Kabul ölçütü — durum

| Ölçüt | Sonuç |
|---|---|
| `--cmd a` ekranda düzgün | ✅ kullanıcı doğruladı |
| bss ≤ 130.000 | ✅ **127.084** korundu |
| `--cmd m` 63 kare/s, kayıp 0 | ✅ |
| `--cmd r` sürekliliği bozulmamış | ✅ sınır maks 649 vs genel maks 2073 |

> Küçük bir dürüstlük notu: kullanıcının gözle onayladığı ikili **20 ms**
> sürümüydü. Gönderilen sürüm 250 ms alt sınırı, yani kesinlikle daha
> korumalı, ve göz gerektirmeyen ölçütlerin hepsi onda çalıştırıldı.
> Yeni oturumda ilk iş olarak `--cmd a` ile 10 saniyelik bir bakış atın.

---

## 9i. M4 adım 4 planı — eğitim kümesi (✅ yapıldı, sonuç §9j'de)

> Bu bölüm işe başlamadan önce yazılmış **plandı**. Ne çıktığı, dört tuzağın
> her birine ne olduğu ve planın hangi maddesinin ölçümle değiştiği §9j'de.
> Firmware tarafı bitti; bu iş tamamen **PC tarafı**, karta gerek yok.

### Elde ne var

```
data/segmentler.csv   79.932 satir — hangi kaydin hangi 3 sn'lik diliminde
                      hangi tur, ne guvenle duyuldu (BirdNET yumusak etiketi)
data/wav/<kod>/*.wav  7.111 kayit, 178 tur, 24 kHz mono 16-bit
data/species_istanbul.csv   tur tablosu + aylik dagilim (mevsim onceligi icin)
data/xc/kayitlar.csv        lisans + atif
```

Eşikler (§9e): güven ≥0.25'te **58.151 dilim, tür başına 327**.

### Yapılacak: `tools/egitim_kumesi.py`

`segmentler.csv` + WAV'lardan model girdisi üretmek. Cihazdaki
`pb_mel_window()` çıktısıyla **birebir aynı** olmak zorunda:

- girdi **64×187 int8**, 3 s pencere
- FFT 512, hop 384, 24 kHz
- mel **HTK**, 150 Hz–11.5 kHz, **alan normalizasyonu YOK**
- pencere **periyodik** Hann (`sym=False`)

> Bu dört ayrıntıdan biri tutmazsa model PC'de iyi, cihazda kötü çalışır ve
> sebebi **hiçbir yerde hata olarak görünmez.** `tools/mel_reference.py`
> zaten bu parametrelerin numpy referansı — eğitim kümesi onu kullanmalı,
> yeniden yazmamalı. Sağlaması: aynı 3 sn'lik sesi hem cihazdan (`--cmd m`
> ya da `s`) hem betikten geçirip mel matrislerini karşılaştırın.

### ⚠ Dört tuzak

**1. Bölmeyi DİLİM bazında yapmayın, KAYIT bazında yapın.** Aynı XC kaydından
çıkan dilimler hem eğitimde hem doğrulamada olursa model kaydı ezberler,
doğruluk sahte yükselir. Bölme `dosya` sütununa göre olmalı; aynı kaydın tüm
dilimleri aynı kümeye. İdealde **kaydeden kişiye** göre de ayırın (aynı kişi
aynı ekipman/lokasyon).

**2. Bulaşık dilimleri ayıklayın.** `en_iyi_tur != hedef` olan dilimde
kayıttaki baskın ses başka bir kuş. Özellikle 6 zayıf türde (§9e) tespit alan
dilimlerin üçte ikisi böyle. Ya bu dilimleri atın ya da yumuşak etiketle
eğitin — ama **sessizce hedef etiketiyle eğitmeyin.**

**3. Negatif sınıfı doldurulmak ZORUNDA (§9f-4).** Toplama ertelendi, sınıf
ertelenmedi. Kendi kayıtlarımızdan çıkan kuş dışı dilim **yalnızca 204** —
ölçüldü, yetmiyor. Çözüm: **ESC-50 indirin** (879 MB, anahtarsız,
github.com/karolpiczak/ESC-50), **`chirping_birds` sınıfını ÇIKARIN.** Bu bir
indirme, saha turu değil; kullanıcının ertelediği şey dışarı çıkmaktı.

**4. Sınıf dengesizliği.** 6 tür 100 dilimin altında (§9e tablosu). Focal loss
+ veri artırma bunları hedeflemeli; ayrıca o türlerde eşiği ayrıca düşürmek
seçenek.

### Sonra M5

Damıtma (BirdNET yumuşak çıktıları öğretmen), INT8 niceleştirme, karışıklık
matrisi. Kısıtlar: **≤30 MMAC/pencere**, **tensor arena ≤180 KB** — bütçe
artık ölçülü (§7: bss 127.084, arena ile ~208 KB pay).

---

## 9j. M4 adım 4 — eğitim kümesi ✅ TAMAMLANDI

### Araçlar

| Dosya | Ne | Hangi python |
|---|---|---|
| [`tools/egitim_kumesi.py`](tools/egitim_kumesi.py) | `segmentler.csv` + WAV → 64×187 int8 pencereler, bölme, öğretmen sinyali | 3.14 (numpy) |
| [`tools/esc50_indir.py`](tools/esc50_indir.py) | ESC-50 indir, kuş sınıflarını çıkar, 24 kHz mono WAV'a çevir | 3.14 (stdlib) |
| `test/dsp_test.c --pencere` | **C tarafının tam pencere dökümü** — Python'un karşılaştırıldığı referans | — |

```bash
python tools/egitim_kumesi.py --saglama          # ONCE BU (cihazla birebirlik)
python tools/esc50_indir.py                      # negatif ses kaynagi
.venv-birdnet\Scripts\python tools/birdnet_run.py \
    --girdi data/negatif/wav --out data/negatif/birdnet_sonuc
python tools/egitim_kumesi.py                    # kumeyi uret
python tools/egitim_kumesi.py --dogrula-cikti 40 # sizinti + satir hizasi
python tools/egitim_kumesi.py --dinle 8          # kulakla dogrulama
```

### Sonuç — ölçüldü (2 Ağustos 2026)

```
61.111 pencere · 178 tur + negatif sinif · 738 MB
  egitim     45.764  %74,9
  dogrulama   9.080  %14,9
  test        6.267  %10,3
negatif (sinif 178)  3.489   (ESC-50, egitim 2.088 / dog 708 / test 693)
tur basina pencere: en az 22 · ortanca 317 · en cok 675
178 turun 178'inde dogrulama kumesi DOLU
```

`data/egitim/` içeriği:

| Dosya | Ne |
|---|---|
| `pencereler.npy` | **(61111, 187, 64) int8** — modelin girdisi, cihazdakiyle aynı |
| `etiket.npy` | (N,) int16 sınıf indeksi; **178 = negatif** |
| `ogretmen.npy` | (N, 178) float16 — BirdNET yumuşak skorları, **damıtma için** |
| `ornekler.csv` | satır başına kaynak, `bolum`, `bulasik`, güven, kaydeden |
| `siniflar.csv` | indeks → eBird kodu / Türkçe ad |
| `ozet.txt` | koşunun raporu (zayıf türler dahil) |

Öğretmen vektörü ölçüldü: kuş satırlarında hedef sınıfın ortalama skoru
**0,838**, medyan 0,966; satır başına sıfırdan büyük sınıf sayısı 1,11.
Negatif satırlarda tamamen sıfır (BirdNET orada kuş duymuyor — zaten şartı bu).

### ✅ Cihazla birebirlik — bu işin tek gerçek riski, ölçüldü

Plan §9i *"bu dört ayrıntıdan biri tutmazsa model PC'de iyi cihazda kötü
çalışır ve sebebi hiçbir yerde hata olarak görünmez"* diyordu. Üç katmanlı
sağlama kuruldu, üçü de geçiyor:

```
1) Sabitler mel.h/fft.h'den okunup karsilastiriliyor   (biri kayarsa betik CALISMIYOR)
2) Yiginlanmis guc spektrumu == mel_reference.power_spectrum   fark 0.000e+00
3) Python penceresi == C penceresi (dsp_test --pencere)
     birebir ayni hucre  %99,49
     en buyuk fark       1 int8 adimi
     ortalama mutlak     0,0051
```

Mel parametreleri **yeniden yazılmadı**: `mel_reference.py`'nin filtre
bankası ve güç spektrumu doğrudan import ediliyor, hızlandırılmış (yığınlanmış)
yol ona karşı sıfır farkla doğrulanıyor.

> **`--dump` yetmiyordu.** Var olan tek karşılaştırma tek bir mel karesiydi;
> kare dizilimi (hop 384) ve pencere içi normalizasyon onun dışında kalıyordu
> — yani modelin gerçek girdisinin yarısı hiç doğrulanmamıştı.
> `dsp_test --pencere` bu yüzden eklendi.

**Neden %100 değil:** C, pencere ortalama/varyansını `float` (32 bit) ile tek
geçişte biriktiriyor ([`mel.c:144`](src/dsp/mel.c)). 11.968 değerin kare
toplamı float32'nin kesin tamsayı aralığını aşıyor ve `E[x²]−E[x]²` farkında
sadeleşme var; ölçek ~1e-4 göreli kayıyor, yuvarlama sınırındaki hücreler bir
adım oynuyor. **Python tarafı float64 ile daha doğru olan.** 1 adım, int8'in
±4σ'lık aralığında 0,03σ — önemsiz. Firmware'i bunun için değiştirmeye gerek
yok, ama bilinsin.

### ⚠ Dört tuzağa ne oldu

**1. Bölme KAYIT bazında — ve bu tuzağa RAĞMEN sızıntı üretildi, ölçümle yakalandı.**

Bölme `dosya` + `kaydeden` bazında yapıldı. Ama *bulaşık dilimleri doğrulamadan
eğitime taşıyan* kural, aynı kaydın bir kısmını eğitime bir kısmını doğrulamaya
koydu: **290 kayıt iki bölümde birden.** Yani §9i tuzak 1'in ta kendisi, hem de
onu önlemek için yazılmış kod yüzünden.

Doğrusu: ölçüm kümesine düşen bulaşık dilimler **taşınmaz, düşürülür** (bu
koşuda 522 dilim). Kontrol `--dogrula-cikti` içine **kalıcı** olarak konuldu:

```
sizinti · birden fazla bolumde olan kayit : 0   (0 olmali)
sizinti · birden fazla bolumde olan kisi  : 1   (hoocro1, asagida)
```

> **Ders:** bir tuzağı bildiğinizi ve önlem aldığınızı sanmak, önlemin
> çalıştığını ölçmenin yerine geçmiyor. Bu kontrol yazılmasaydı sızıntı
> M5'te doğruluğu sahte yükseltecek ve kimse fark etmeyecekti.

**Bölme algoritması da ölçümle değişti.** İlk sürüm grupları karıştırıp önce
test kotasını dolduruyordu. Ölçüldü: Karatavuk'un 573 diliminin **399'u tek bir
kaydedene** ait; o grup teste düşünce bölme **%11 / %21 / %68** oldu. Doğrusu
büyükten küçüğe gidip her grubu *hedefinden en çok geride olan* bölüme vermek —
kaydeden bazında gruplamayı bozmadan oranları tutturuyor ve **belirlenimci**
(tohum gerekmiyor).

**`hoocro1` (Leş Kargası) istisnası:** kayıtlarının neredeyse tamamı tek kişiye
ait; kaydeden bazında bölününce doğrulama boş kalıyordu, o tür için **kayıt**
bazına düşüldü. Kayıt bazlı garanti (aynı kaydın tüm dilimleri aynı bölümde)
orada da korunuyor. Raporda adıyla yazıyor.

**2. Bulaşık dilimler.** `en_iyi_tur != hedef` olan dilim `bulasik=1` ile
işaretli. Doğrulama/teste **hiç girmiyor** (522'si bu yüzden düşürüldü),
eğitimde 1.363 tanesi bayrağıyla ve tam öğretmen vektörüyle duruyor — yani
"yumuşak etiketle eğit" seçeneği açık, "sessizce hedef etiketiyle eğit"
seçeneği için ekstra emek gerekiyor. `--bulasik at` ile tamamen düşürülür.

**3. Negatif sınıf — `chirping_birds` çıkarmak YETMİYOR.**

ESC-50 indirildi (2.000 klip, 44.1 kHz → 24 kHz mono, kuş WAV'larıyla aynı
ffmpeg zinciri). Sonra 1.960 klip **BirdNET'ten geçirildi** ve ölçüldü:

```
51 klipte kendi 178 turumuzden biri >=0.25 guvenle duyuluyor
  crow            26     <- en yuksekleri Corvus frugilegus 1.00, Pica pica 0.98
  brushing_teeth   4
  hen              3
  sheep            3
  rooster          2 ...
```

ESC-50'nin **`crow` sınıfı bizim hedef türlerimiz.** Negatife konsaydı model
kargayı reddetmeyi öğrenirdi — ve bu, hiçbir testte hata olarak görünmezdi.
Plan yalnızca `chirping_birds`i söylüyordu; **yetmiyor.**

İki katmanlı çözüm, ikisi de ölçülmüş:
- **Sınıf bazında:** `chirping_birds`, `crow`, `hen`, `rooster` çıkarıldı
  (2.000 → 1.840 klip, 46 kategori).
- **Klip bazında:** kalanlardan BirdNET'in ≥0.25 güvenle kuş duyduğu **20 klip**
  daha elendi.

Negatif bölmesi ESC-50'nin **kendi 5 katmanına** göre (fold 5 test, 4 doğrulama,
1–3 eğitim) — o katmanlar zaten aynı Freesound kaydından kesilmiş klipler aynı
katmanda kalsın diye ayrılmış. Tek istisna ölçüldü: `209698` numaralı kaynak
`clock_alarm` ve `clock_tick` olarak iki katmana bölünmüş (1.406 kaynağın 1'i,
doğrulama/test arasında — eğitimi etkilemiyor).

> **Saha turu hâlâ M8'de.** Ezan, vapur, simitçi, İstanbul trafiği yok; onlar
> cihazın kendi mikrofonuyla kaydedilecek (§9f-4). Yapılan şey sınıfı
> doldurmaktı, saha kalibrasyonu değil.

**4. Sınıf dengesizliği.** En zayıf tür Alaca Balıkçıl **22 pencere** (18/2/2),
en güçlüsü 675. Oran 1:31. `ozet.txt`'te en zayıf 12 tür listeleniyor.
Erguvani Balıkçıl'a dikkat: 100 pencerenin **27'si bulaşık** — o türde
kayıtların çoğunda baskın ses başka bir kuş.

### Diğer ölçülmüş kararlar

- **Dilim sonu dosyayı aşınca pencere SOLA kaydırılıyor**, sıfırla
  doldurulmuyor. Sıfır bandı mel'de -90 dB'lik yapay bir blok yapar ve pencere
  normalizasyonunu bozardı. Dilimin sesi yine pencerenin içinde kalıyor.
- **Sessiz pencereler düşürülüyor** (dB standart sapması < 0,5): 156 tane.
  Dijital sessizlikte cihazın normalizasyonu ölçeği patlatıyor ve modele
  ±127'lik gürültü olarak giriyor.
- **Ara int8 yuvarlaması atlanmadı.** Cihaz her kareyi önce -90…0 dB
  aralığında int8'e sıkıştırıyor, normalizasyonu *o* değerlerden yapıyor.
  Bu ara adım atlansa çıktı sessizce kayardı.
- Ses kesilmiyor, orijinal WAV'dan istenen ofsetten okunuyor (§9e'deki
  kararın devamı) — 10 GB'lık dilim dosyası üretilmedi.

### Kabul ölçütü — durum

| Ölçüt | Sonuç |
|---|---|
| Çıktı cihazdaki `pb_mel_window()` ile aynı | ✅ %99,49 birebir, en büyük fark 1 int8 adımı |
| Bölme kayıt bazında, sızıntı yok | ✅ **0 kayıt** iki bölümde (ölçüldü, bir kez de yakaladı) |
| Bulaşık dilimler sessizce hedefle eğitilmiyor | ✅ bayraklı; ölçüm kümesine hiç girmiyor |
| Negatif sınıf dolu | ✅ 3.489 pencere, kuş içerenler iki katmanda elendi |
| Zayıf türler bildiriliyor | ✅ `ozet.txt` |
| Satır hizası (dizi ↔ csv) | ✅ 40 satır kaynaktan yeniden üretildi, 40'ı birebir |
| **Rastgele pencereler DİNLENMELİ** | ✅ **kullanıcı 8 örneği dinledi ve onayladı** |

Dinleme yapıldı ve geçti. 8 örnek (6 kuş + 2 negatif) kullanıcıya gönderildi;
kuş örneklerinde ses duyuluyor, **iki negatif örnekte kuş duyulmuyor** —
kullanıcı ikisini de teyit etti. Bu projede dolaylı ölçüm iki kez pahalıya
patladı (§5.10), o yüzden atlanmadı: negatife kuş karışması Aşama-1'i bozan
sessiz hatadır ve objektif sağlamalar (BirdNET taraması) onu ancak BirdNET'in
duyduğu kadar yakalar.

### M5'e taşınan kısıtlar

- Girdi hazır: `pencereler.npy` (61111, 187, 64) int8, `etiket.npy`,
  `ogretmen.npy` (damıtma öğretmeni), bölümler `ornekler.csv`'de.
- **Veri artırma ham seste yapılacaksa** pencere `egitim_kumesi.py`'nin
  `mel_penceresi()`'nden geçmeli — elle mel yazmayın, cihazla birebirlik
  oradan geliyor. SpecAugment mel'den sonra, sorun değil.
- Focal loss + artırma en zayıf 12 türü hedeflesin (`ozet.txt`).
- Model boyutu: **≤30 MMAC/pencere**, tensor arena **≤180 KB** (§7).
- Sınıf 178 = negatif; Aşama-1'in "kuş değil"i ve Aşama-2'nin "bilinmiyor"u
  aynı havuzdan besleniyor.

---

## 9k. 🔵 M5 — Aşama-2 tür ağı: damıtma + INT8

### Araç

[`tools/egit.py`](tools/egit.py) — `.venv-birdnet` (Python 3.11 + TF 2.21) ile
çalışır, 3.14 ortamında TensorFlow yok.

```bash
.venv-birdnet\Scripts\python -u tools/egit.py --duman   # 2 dk, hat calisiyor mu
.venv-birdnet\Scripts\python -u tools/egit.py --devir 60
```

Çıktılar `models/` altında: `tur_agi.keras`, `tur_agi_int8.tflite`,
`tur_agi_int8.h` (firmware'in derleyeceği C dizisi), `rapor.txt`,
`ilerleme.html` (canlı pano, 10 sn'de bir kendini yeniler).

### ⚠ GPU kullanılamıyor — ölçüldü, alternatif değerlendirildi

Makinede **RTX 4050** var ama **TensorFlow ≥2.11 native Windows'ta GPU
desteklemiyor** (TF'in kendi uyarısı). Seçenekler ve neden CPU seçildi:

| Yol | Neden seçilmedi |
|---|---|
| WSL2 + CUDA | Sıfırdan TF+CUDA kurulumu; saatler, kritik yolda değil |
| PyTorch + CUDA (Windows'ta çalışır) | Çıktı **TFLite int8** olmak zorunda; ONNX→TF→TFLite zinciri bu projedeki en pahalı hata sınıfını (sessiz sapma) davet ediyor |
| tensorflow-directml | TF 2.10'da donmuş, ölü |

**CPU (20 çekirdek) ile ölçülen: 100 sn/devir**, 60 devir ≈ 1 saat 40 dk.
Kabul edilebilir. Model büyütülecekse GPU tekrar değerlendirilmeli.

### Cihaz sözleşmesi — modelin girdisi HAM int8 mel penceresi

Bu, M6'yı ucuza getiren tek karar. Keras girdisi ham int8 değerleri
(-128..127 float olarak), modelin **ilk katmanı** `Rescaling(4/127)` — yani
int8→sigma çevrimi **modelin içinde**, cihazda değil. TFLite'a `int8`
girdiyle dönüştürülünce ölçüldü:

```
girdi tensoru: int8 (1, 187, 64, 1)   olcek 1.000000   sifir noktasi 0
```

Yani M6'da cihaz kodu şu kadar: `pb_mel_window(buf)` →
`memcpy(input->data.int8, buf, 187*64)`. Betik bu ölçeği **assert ediyor**;
tutmazsa rapora gereken dönüşümü yazıyor. Kayarsa sessiz doğruluk kaybı olur.

### Model — bütçenin neresindeyiz

Derinlemesine ayrılabilir CNN (daraltılmış MobileNet, ARCHITECTURE §4),
aktivasyon **ReLU6** (INT8'de aralığı sınırlı tutuyor, kalibrasyon kuyruk
değerlerine daha az duyarlı):

```
209.107 parametre  (~204 KB int8, plan 300-400 KB diyordu)
MAC/pencere  6,6 M      butce 30 M      <- %78 bos pay var
aktivasyon tepesi (kaba) 141 KB   arena butcesi 180 KB
tflite dosyasi 270 KB
```

> **MAC'te bol pay bilerek bırakıldı.** Doğruluk yetersiz çıkarsa
> `--genislik 1.5` ile model büyütülebilir; betik MAC bütçesini aşarsa
> **duruyor**. Asıl sıkışık olan MAC değil **arena**: en büyük katman
> girişteki 94×32×24 (70 KB) ve arena ardışık iki aktivasyonu birden
> tutuyor. Model büyütülürse ilk katmanın kanal sayısına dikkat.
>
> **141 KB bir TAHMİN, ölçüm değil.** Gerçek arena M6'da
> `arena_used_bytes()` ile ölçülecek. Bu projede tahmine güvenmek iki kez
> pahalıya patladı (§9d-2, §9g); bu sayıyı ölçüm sanmayın.

### Kayıp — üç tuzağın karşılığı

```
kayip = ornek_agirligi * focal(sert etiket) + 0,5 * CE(ogretmen dagilimi)
```

- **Bulaşık dilimlerde `ornek_agirligi = 0`** — sert etiket hiç kullanılmıyor,
  o dilimler yalnızca BirdNET'in yumuşak dağılımından öğreniliyor. §9i
  tuzak 2'nin istediği tam olarak bu.
- **Focal loss** (γ=2) + sınıf ağırlığı `α = √(ortanca/sayı)`, [0,5 … 4]
  aralığına kırpılmış. Ölçülen aralık **0,50 … 3,66**. Karekök bilerek:
  ham ters frekans 1:31'lik oranda en zayıf türü aşırı ağırlıklandırıp
  eğitimi dengesizleştiriyordu.
- **Öğretmen dağılımının kapsamı abartılmamalı:** BirdNET sigmoid skorları
  yazıyor, softmax değil, ve 0.1 altını hiç yazmıyor. Ölçüldü: satır başına
  sıfırdan büyük sınıf **1,11**. Yani damıtmanın katkısı *belirsiz*
  dilimlerde toplanıyor (hedef + akraba türün birlikte skor aldığı yerler),
  genel bir "karanlık bilgi" kaynağı değil.

### Veri artırma — ve NEDEN gürültü karıştırma burada YOK

Uygulanan: zaman kaydırma (±16 kare ≈ 256 ms) + SpecAugment (zaman ve
frekans maskeleri).

Maskeler **sıfırla** dolduruluyor ve bu tam olarak doğru olan: cihazdaki
pencere normalizasyonu ortalamayı sıfıra çekiyor ([`mel.c`](src/dsp/mel.c)),
yani 0 = pencerenin ortalama enerjisi. Rastgele bir sabit değil, anlamlı
bir değer.

> **Plan §6-5'teki "negatiflerle çeşitli SNR'lerde gürültü karıştırma"
> BURADA YAPILAMAZ.** Mel logaritmik: iki mel matrisini toplamak iki sesi
> karıştırmak değil. Doğru yeri dalga formu, o da her artırılmış örnek için
> yeniden mel çıkarmak demek. M8 saha kayıtlarıyla birlikte yapılacak —
> asıl değerini de orada verecek zaten (gerçek İstanbul gürültüsüyle).

### ✅ SONUÇ — 60 devir, ölçüldü

```
TEST (hic dokunulmamis 6.267 pencere)
  float32  top-1 %58,05   top-3 %75,01
  INT8     top-1 %58,07   top-3 %74,82      <- nicelestirme bedeli ~0
tflite 270 KB · girdi olcegi 1.000000 / sifir noktasi 0
```

**INT8'in bedeli sıfır çıktı** (+0,02 / −0,19 puan). ReLU6 + temsilî veri
kümesiyle kalibrasyon işini görmüş; bu, plandaki "niceleştirme öncesi/sonrası
fark raporlanır" maddesinin cevabı.

### ⚠ Ama asıl sayı bu değil — zamansal birleştirme ölçüldü

`egit.py`'nin bildirdiği rakam **tek 3 saniyelik pencere** başına. Cihaz öyle
çalışmıyor: adım 1 sn ve Aşama-3 ardışık pencereleri birleştiriyor.
[`tools/birlestirme_olc.py`](tools/birlestirme_olc.py) bunu ölçüyor
(`models/birlestirme.txt`):

| birleştirilen pencere | top-1 | top-3 |
|---|---|---|
| 1 (ham) | %58,07 | %74,82 |
| 2 | %64,21 | %79,63 |
| 3 | %67,58 | %81,18 |
| 5 | %69,29 | %81,98 |
| **8** | **%70,40** | **%82,20** |
| 12 | %69,71 | %82,33 |

**Kazanç 5–8 pencerede doyuyor**, 12'de artık artmıyor (hatta top-1 düşüyor).
Aşama-3'ün penceresi buna göre seçilmeli: **~8 pencere.** Daha uzunu bedava
değil, gecikme getiriyor ve karşılığını vermiyor.

Böylece top-1 **%70,4** ile ARCHITECTURE §4'ün %65–75 bandının içinde;
top-3 **%82,2** ile %85–90 bandının biraz altında. Üstelik plan **~110 tür**
varsayıyordu, bizde **179** var.

> **Bu sayılar bir ÜST SINIR tahmini, dürüst yazalım.** Birleştirme test
> kümesindeki aynı kaydın ardışık dilimleri üzerinden yapıldı. Cihaz 1 sn
> adımla daha çok örtüşen pencere görecek → hataları daha ilintili → gerçek
> kazanç bir miktar daha düşük. Ayrıca bu dilimler BirdNET'in kuş duyduğu
> dilimler, yani "kuş sürekli ötüyor" varsayımı burada geçerli. Kesin cevap
> M8 saha testinde.

### Saha açısından iki kritik oran (pencere başına)

```
negatifi kus sanma : %8,37     <- sahada en pahali hata
kusu negatif sanma : %1,40
```

Negatifi kuş sanma %8,37 tek pencerede yüksek görünüyor ama üç şey onu
bastıracak: birleştirme, **Aşama-0 kapısı** (sessiz odada zamanın yalnızca
%2–3'ünde açılıyor, §9c) ve henüz yazılmamış **Aşama-1 ikili ağ**.

### ⚠ Sınıf budama ölçüldü — sanıldığı kadar işe yaramıyor

Karışıklık listesindeki en zayıf türleri atmak cazip görünüyordu. **İlk
ölçümüm daireseldi** (en zayıf türleri *test* kümesine bakarak seçip yine
testte ölçmek) ve %65,5 gibi şişik bir sayı verdi. Doğru yol — seçimi
**doğrulamada**, ölçümü **testte** yapmak:

| çıkarılan tür | kalan sınıf | top-1 | top-3 |
|---|---|---|---|
| 0 | 179 | %58,07 | %74,82 |
| 20 | 159 | %59,65 | %76,62 |
| 40 | 139 | %61,66 | %78,18 |

**40 tür feda edip 3,6 puan.** Kötü takas. §9d'de "sonradan budamak kolay"
diye not düşülmüştü; ölçüldü, kolay ama getirisi küçük. Budama yapılacaksa
en sona bırakılmalı.

### Asıl darboğaz: ezberleme, kapasite değil

```
devir 15: kayip 0,65  dogrulama top-1 %52,2
devir 60: kayip 0,20  dogrulama top-1 %56,8      <- kayip 3 kat dustu, dogruluk +4,6 puan
```

Eğitim kaybı düşmeye devam ederken doğrulama doğruluğu 15. devirden sonra
neredeyse yatay. Bu **aşırı öğrenme** imzası. Dolayısıyla:

- **Modeli büyütmek muhtemelen işe yaramaz** (MAC'in %78'i boş olsa da).
  Kapasite eksik değil, genelleme eksik.
- İşe yarayacak olan: **daha güçlü veri artırma** (mixup, daha agresif
  SpecAugment), ağırlık sönümü, ve M8'de gerçek gürültüyle SNR karıştırma.
- Ya da **tür başına daha çok kayıt** — M4'te kota 40'tı, yükseltilebilir
  (ama 13 GB daha indirme demek).

### Karışıklıklar akustik olarak ANLAMLI — iyiye işaret

En çok karışan çiftler (`models/birlestirme.txt`):

```
Serce -> Agac Sercesi · Cilikusu -> Surmeli Cilikusu
Benekli Sinekkapan -> Kucuk Sinekkapan · Benekli Bulbul -> Bulbul
Sutavugu -> Sakarmeke · Ibibik -> Guguk
```

Model gürültü ezberlemiyor; **gerçekten benzer öten türleri** karıştırıyor.
Bu, mimarinin sağlam olduğunu söyleyen bir işaret — ve top-3 gösteriminin
neden doğru tasarım kararı olduğunu da gösteriyor: bu çiftlerin ikisi de
ilk üçte olacak.

### Bundan sonra

1. **M6'ya geçmek** — en büyük bilinmeyen artık model değil, "PC'de çalışıyor
   cihazda çalışmıyor" riski. Gerçek arena ölçümü, çıkarım süresi, cihaz-içi
   doğrulama seti (ARCHITECTURE §6 adım 9).
2. **Aşama-1 ikili ağ** (~15 KB): aynı veriden iki sınıf (178 tür → "kuş",
   negatif → "kuş değil"). Küçük iş, negatifi kuş sanma oranını düşürür.
3. **Aşama-3 mevsim tablosu**: `species_istanbul.csv`'deki `ay_01..ay_12`'den
   178×12 log-öncelik, ±2.0 logit tavanı. Birleştirme penceresi **8**.
4. Doğruluk iterasyonu (artırma güçlendirme) M6'dan sonra — çünkü asıl
   ölçüt boş odadaki saha testi, ve o testi ancak cihaz çalışınca yapabiliriz.

---

## 9l. 🔵 SIRADAKİ İŞ — M6: TFLM entegrasyonu, cihazda çıkarım

> Bu iş **karta dokunuyor.** Kart COM13'te, HEAD kartta duruyor ve çalışıyor
> (bss 127.084, ekran düzgün, ses 63 kare/s kayıp 0).

### Elde ne var

```
models/tur_agi_int8.h        C dizisi, 270 KB, 16 bayt hizali, PB_TUR_AGI_BOYUT
models/tur_agi_int8.tflite   ayni model
models/rapor.txt             dogruluk + en kotu 20 sinif
models/birlestirme.txt       birlestirme tablosu + en cok karisan ciftler
data/egitim/siniflar.csv     sinif indeksi -> eBird kodu / Turkce ad (179 satir)
```

### Yapılacaklar

1. **TFLM vendor** — `third_party/tflite-micro` (git'e girmiyor, `third_party/`
   deseni zaten öyle). CMSIS-NN çekirdekleriyle derlenmeli, yoksa M33'te
   referans çekirdekler çok yavaş.
2. **Arena'yı ÖLÇ.** §9k'daki **141 KB bir tahmin, ölçüm değil.** Gerçeği
   `interpreter.arena_used_bytes()` ile ölçün, bütçe 180 KB. Bu projede
   tahmine güvenmek iki kez pahalıya patladı (§9d-2, §9g).
3. **Girdiyi bağla — en kolay kısım, bilerek öyle tasarlandı.**
   ```c
   int8_t pencere[PB_MEL_FRAMES * PB_MEL_BANDS];
   if (pb_mel_window(pencere))
       memcpy(input->data.int8, pencere, sizeof(pencere));
   ```
   Dönüşüm YOK: TFLite girdi ölçeği 1.0, sıfır noktası 0 (§9k'da assert
   ediliyor). Kare sırası eskiden yeniye, bant içte — `mel.c`'deki düzenin
   aynısı, eğitim kümesi de öyle üretildi.
4. **Çıkarım süresini ölç.** Hedef: 1 s'lik pencere adımına sığmak. 6,6 MMAC
   @150 MHz CMSIS-NN ile ~0,3–0,5 s bekleniyor (ARCHITECTURE §4) — **ölçün.**
5. **Core 1'e taşı.** Ses + çıkarım core1'de, arayüz core0'da (§7). Kapı
   (Aşama-0) açılmadıkça çıkarım hiç çalışmamalı — sessiz odada zamanın
   %2–3'ü (§9c).
6. **Zamansal birleştirme: 8 pencere.** Ölçüldü, 12'de artık artmıyor (§9k).
7. **⚠ CİHAZ-İÇİ DOĞRULAMA SETİ — bu adımı atlamayın.** ARCHITECTURE §6
   adım 9. "PC'de çalışıyor cihazda çalışmıyor" sınıfını yakalayan **tek**
   şey bu. En ucuz hâli: `data/egitim/`den birkaç pencereyi C dizisi olarak
   gömüp cihazda çıkarım yapmak ve **logit'leri PC'deki TFLite çıktısıyla
   karşılaştırmak.** Aynı girdi → aynı çıktı olmalı; olmuyorsa mel'e,
   arena'ya ya da niceleştirmeye bakın. Ses yolunu işin içine katmadan
   sınayın ki hata alanı dar kalsın.

### ⛔ M6'da akustik test: PC'den ses ÇALMAYIN

**Bilgisayarda kulaklık takılı (§5.5).** Cihazın mikrofonuyla duymasını
bekleyen otomatik test kurmayın — hoparlörden ses çıkmıyor, test anlamsız
olur ve sonucu kod hatası sanarsınız.

M6'nın doğrulaması zaten **ses gerektirmiyor**: madde 7'deki cihaz-içi
doğrulama seti gömülü pencereleri kullanıyor, mikrofonu hiç işin içine
katmıyor — hata alanı bu yüzden dar. Çıkarım doğruluğunu böyle sınayın.

Gerçekten akustik bir teste ihtiyaç olursa **kullanıcıdan isteyin**: ne
çalacağını, kaç saniye, cihazı nereye tutacağını yazın; sonucu o bildirsin.
İlk saha doğrulaması (boş odada bilgisayardan kuş sesi çalmak) da böyle,
**kullanıcının elleriyle** yapılacak.

### Beklenen tuzaklar

- **Bellek değişikliği bir zamanlama değişikliğidir (§5.17).** Arena 180 KB
  eklenince bss yerleşimi kayacak. `main.c`'deki panel hazır olma penceresi
  (`while (to_ms_since_boot(...) < 250)`) **tam bunun için var — silmeyin,
  `sleep_ms`'e çevirmeyin.** Ekran bozulursa ilk bakılacak yer orası.
- Arena'yı `static` bir dizi olarak bss'e koyun, malloc'la değil; 520 KB'de
  heap parçalanması istemiyoruz.
- `nm --size-sort -S -td build/pokebird.elf` ile bss'i her adımda ölçün;
  hedef bss + arena ≤ ~311 KB (§7'de hesaplanmış pay).
- Ekran/ses gerilemesi olursa **üç koşuluk A/B** (eski / yeni / yeni mantık +
  eski yerleşim) mantığı yerleşimden ayırır — §5.17'de bir kez işe yaradı.

---

## 9m. M6 — TFLM entegrasyonu, cihazda çıkarım ✅ TAMAMLANDI

### Ne yapıldı

| Dosya | Ne |
|---|---|
| [`cmake/tflm.cmake`](cmake/tflm.cmake) | TFLM + CMSIS-NN derleme kuralları, sürümler sabitlenmiş |
| [`src/ai/tur_agi.cc`](src/ai/tur_agi.cc) | TFLM sarmalayıcısı, arena, cihaz sözleşmesi sağlaması |
| [`src/ai/tflm_port.cc`](src/ai/tflm_port.cc) | `DebugLog`, `micro_time`, **`abort()` ezmesi** |
| [`src/ai/tanima.c`](src/ai/tanima.c) | core 1 gerçek zamanlı hat + 8 pencere birleştirme |
| [`tools/dogrulama_seti.py`](tools/dogrulama_seti.py) | cihaz-içi doğrulama seti üreteci |
| [`tools/sinif_tablosu.py`](tools/sinif_tablosu.py) | `siniflar.csv` → `src/ai/siniflar.h` |

Yeni komutlar: **`x`** (doğrulama + arena + süre), **`k`** (gerçek zamanlı
tanıma), **`K`** (aynısı ama kapı yoksayılır — ölçüm kipi).

### TFLM'i vendor etmek — TFLM'in kendi Makefile'ı Windows'ta ÇALIŞMIYOR

`tools/make/Makefile` wget/unzip/md5sum ve POSIX kabuğu bekliyor;
`create_tflm_tree.py` de onu çağırıyor. Kaynak listesi bu yüzden CMake'e
taşındı — **uydurulmadı**, TFLM'in kendi `tools/make/sources.inc` dosyasından
alındı. Sürümler de TFLM'in kendi indirme betiklerinden:

```
tflite-micro  330b1747c9d51c0e394f51a2a34ff42deb9b95f5   (29 Tem 2026)
flatbuffers   v25.9.23  + tools/make/flatbuffers.patch    <- YAMA SART
gemmlowp      719139ce755a0f31cbf1c37f7f98adcc7fc9f425    (yalniz baslik)
ruy           d37128311b445e758136b8602d1bbd2a755e115d    (yalniz baslik)
CMSIS-NN      4ab83cc3cc98fb85ed6dafb55e8ca02f1628dcae
```

Kurulum (hiçbiri git'e girmiyor):

```bash
git clone --depth 1 https://github.com/tensorflow/tflite-micro.git third_party/tflite-micro
cd third_party && mkdir .dl && cd .dl
curl -sSL -o fb.zip       https://github.com/google/flatbuffers/archive/refs/tags/v25.9.23.zip
curl -sSL -o gemmlowp.zip https://github.com/google/gemmlowp/archive/719139ce755a0f31cbf1c37f7f98adcc7fc9f425.zip
curl -sSL -o ruy.zip      https://github.com/google/ruy/archive/d37128311b445e758136b8602d1bbd2a755e115d.zip
curl -sSL -o cmsisnn.zip  https://github.com/ARM-software/CMSIS-NN/archive/4ab83cc3cc98fb85ed6dafb55e8ca02f1628dcae.zip
for z in fb gemmlowp ruy cmsisnn; do unzip -q $z.zip; done
# klasorleri flatbuffers/ gemmlowp/ ruy/ cmsis-nn/ olarak third_party/ altina tasi
cd ../flatbuffers && patch -p1 < ../tflite-micro/tensorflow/lite/micro/tools/make/flatbuffers.patch
```

CMSIS Core başlıkları ayrıca indirilmedi: CMSIS-NN kendi kendine yetiyor
(`Internal/arm_nn_compiler.h` yalnızca derleyicinin `arm_acle.h`'sini istiyor).

### ⚠ Üç derleme tuzağı — üçü de gerçekti

**1. `-DCMSIS_NN` tanımlanmazsa ODR çakışması.** `kernels/conv.h` gibi
başlıklar `CMSIS_NN` yoksa `Register_CONV_2D_INT8()`'i **kendisi `inline`
tanımlıyor**; `cmsis_nn/conv.cc`'deki gerçek tanımla çakışıyor. Derleme
hatası olarak çıkıyor — iyi ki öyle. Tanım **PUBLIC** olmak zorunda: aynı
başlıkları `micro_mutable_op_resolver.h` üzerinden bizim kodumuz da görüyor.

**2. TFLM'in `abort()`'u newlib malloc'unu çekiyor — §5.4'ün ikinci kapısı.**
TFLM birkaç yerde `abort()` çağırıyor (micro_utils.cc, reduce_common.cc,
quantization_util.cc). newlib'in abort'u `raise()` → `signal()` → `malloc()`
zincirini açıyor, malloc da `__retarget_lock_acquire_recursive` /
`__lock___malloc_recursive_mutex` istiyor ve Pico SDK bunları sağlamıyor:

```
libg.a(libc_a-mlock.o): undefined reference to `__retarget_lock_acquire_recursive'
```

Kilit saplamaları yazmak yerine **`abort()`'un kendisi ezildi**
(`tflm_port.cc` → `panic()`). Newlib malloc/signal hiç bağlanmıyor (520 KB'lik
bir cihazda heap'i kazara canlandırmak istemiyoruz) ve hata sessiz kilitlenme
yerine seri porta yazılan bir panic oluyor.

> **Teşhis yöntemi kayda değer:** hangi nesnenin malloc'u çektiğini `nm` ile
> bulamadım (kimse doğrudan çağırmıyordu). `-Wl,-y,<sembol>` linker
> bayrağı her sembolü kimin tanımlayıp kimin referans verdiğini yazdırıyor;
> zinciri üç turda ortaya çıkardı. Aynı sınıf hata tekrar çıkarsa bu bayrak.

**3. Üretilmiş `models/tur_agi_int8.h` ile kendi başlığımın include guard'ı
çakıştı.** İkisi de `POKEBIRD_TUR_AGI_H` kullanıyordu; kendi başlığım önce
dahil edildiği için model dizisi **sessizce hiç dahil edilmedi** ve
`pb_tur_agi` "tanımsız" çıktı. Bizimki `POKEBIRD_AI_TUR_AGI_H` oldu.

### ✅ ARENA — ölçüldü (§9l madde 2)

```
arena_used_bytes()  110.436 bayt      §9k tahmini 141 KB (%28 fazla)
ayrilan             122.880 (120 KB)  butce 180 KB
```

Ayrılan boyut ölçülene göre seçildi: %11 pay. 180 KB'da bırakmak 61 KB'ı
boşuna tutardı. Model değişirse sayı da değişir — `x` komutu her koşuda
kullanılan baytı basıyor, yetmezse `AllocateTensors` sebebini yazıp duruyor.
`cmake -DPB_TFLM_ARENA_BAYT=...` ile ezilebilir.

### ✅ ÇIKARIM SÜRESİ — ölçüldü (§9l madde 4)

```
190 ms / pencere   (min 189.559  ort 189.686  max 189.815 us)
hedef 1 s'lik pencere adimi  ->  %19 doluluk, 5 kat pay
```

ARCHITECTURE §4 CMSIS-NN ile 0,3–0,5 s bekliyordu; ölçülen daha iyi.

### ✅ CİHAZ-İÇİ DOĞRULAMA SETİ — 8/8 BİREBİR (§9l madde 7)

M6'nın en kritik maddesi ve tek gerçek riski. 8 pencere (test bölümünden,
biri negatif sınıf) `src/ai/dogrulama_seti.h`'ye gömüldü; `x` komutu bunları
modelden geçirip logit'leri PC'nin çıktısıyla karşılaştırıyor.

```
logit   8/8 pencere BIREBIR ayni, en buyuk fark 0, ort mutlak fark 0.0000
tahmin  8/8 pencere ayni sinifi sectik
```

Ses yolu bu teste **hiç girmiyor** (girdi hazır pencere), o yüzden hata alanı
TFLM/CMSIS-NN/niceleştirme ile sınırlı. Kulaklık kuralı (§5.5) bu testi
ilgilendirmiyor.

> ### ⚠ İLK KOŞU "SAPMA VAR" DEDİ — SUÇLU PC'YDİ, CİHAZ DEĞİL
>
> İlk ölçüm 8/8 tahmin doğru ama logit'lerde **en büyük 2 adım, ortalama
> mutlak 0,4441** sapma gösterdi. "Cihazda küçük bir sapma var" diye
> yazmak üzereydim. Önce PC'nin kendi iki çekirdek setini karşılaştırdım:
>
> ```
> XNNPACK vs BUILTIN_REF (ayni 8 pencere, ikisi de PC'de):
>   en buyuk fark 2   ort mutlak 0,4441      <- BIREBIR AYNI SAYILAR
> ```
>
> Yani sapmanın tamamı `tf.lite.Interpreter`'ın **varsayılan olarak
> devreye soktuğu XNNPACK delegesinden** geliyordu; XNNPACK int8'i
> bit-birebir hesaplamıyor. Altın standart referans çekirdekler
> (`OpResolverType.BUILTIN_REF`) — TFLite'ın int8 tanımını onlar veriyor ve
> CMSIS-NN onlarla birebir olmayı hedefliyor. Referans o şekilde yeniden
> üretilince cihaz **8/8 birebir** çıktı.
>
> **Ders:** karşılaştırdığınız "referans"ın kendisi bir yaklaşım olabilir.
> Cihazı suçlamadan önce referansı iki farklı yolla üretip aralarındaki
> farka bakın — ölçüm ikiye bölünmezse yanlış tarafta hata ararsınız.

### ✅ CORE 1'E TAŞINDI + BİRLEŞTİRME 8 (§9l madde 5, 6)

`k` komutu: core 1 sesi okuyor, mel çıkarıyor, kapı açılınca saniyede bir
çıkarım yapıyor ve son 8 pencereyi birleştiriyor; core 0 yalnızca basıyor.
Birleştirme kuralı `tools/birlestirme_olc.py`'ninkiyle **aynı** (softmax
ortalaması) — başka bir kural seçilse §9k'daki %70,40 / %82,20 geçersiz olurdu.

### ⚠ SES HALKASI 4096 → 8192 BÜYÜTÜLDÜ — ölçüme dayalı, şart

Çıkarım 190 ms sürüyor ve o süre boyunca core 1 halkayı **hiç okumuyor**.
Okuma yolu, birikmiş miktar halkanın 3/4'ünü aşınca en tazeye atlıyor; yani
eski 4096'lık halkanın gerçek toleransı 170 değil **128 ms**'ti. Her çıkarımda
ses hattı kopar ve 3 saniyelik pencerenin ortasında süreksizlik olurdu —
üstelik hiçbir yerde hata olarak görünmeden.

```
8192 ornek = 32 KB, tolerans 3/4 x 341 = 256 ms   (cikarimin 1,35 kati)
```

`PB_AUDIO_MAX_READ` halka boyutundan **koparıldı** (eskiden RING/2 idi):
teşhis tamponu `s_chunk` onunla boyutlanıyor ve §9g'de 96 KB'dan 4 KB'a
indirilen kazanç geri gidecekti.

Kartta ölçülen (kapı yoksayılarak, yani her saniye çıkarım — en kötü durum):

```
1252 kare / 20 s = 62,6 kare/s      (gercek zaman 62,5)
overrun 0                            <- halka yetiyor
17 cikarim, her biri 190 ms, birlestirme 8 pencere
```

> Aşama-1 ikili ağ eklenince toplam çıkarım süresi artacak; halka toleransı
> o zaman **yeniden ölçülmeli**. `k` komutu overrun'ı basıyor.

### Bellek — ölçüldü

```
text 463.056 -> 944.992   (model 277 KB + dogrulama seti 94 KB + TFLM kodu)
bss  127.084 -> 295.784   (arena 120 KB + halka +16 KB + birlestirme 5,7 KB
                           + core1 yigini 8 KB)
520 KB SRAM'de yigin/heap payi ~231 KB
```

Flash 16 MB'de sorun değil. §9l "bss + arena ≤ ~311 KB" diyordu; 295.784.

### Kabul ölçütü — durum

| # | Ölçüt | Sonuç |
|---|---|---|
| 1 | TFLM CMSIS-NN çekirdekleriyle derleniyor | ✅ |
| 2 | Arena ÖLÇÜLDÜ, ≤180 KB | ✅ **110.436** ölçüldü, 120 KB ayrıldı |
| 3 | Girdi dönüşümsüz bağlandı | ✅ `memcpy`, ölçek 1.0 cihazda da assert ediliyor |
| 4 | Çıkarım 1 s'lik adıma sığıyor | ✅ **190 ms** |
| 5 | Core 1'de, kapı açılmadıkça çalışmıyor | ✅ `k` komutu; kapı 14 fırsatın 5'ini eledi |
| 6 | Birleştirme penceresi 8 | ✅ ve kural ölçümle aynı |
| 7 | **Cihaz-içi doğrulama seti** | ✅ **8/8 BİREBİR** |
| — | Ekranda gösterim | ⛔ **ekran bozuk, ama M6'dan değil — §9n** |

Kabul ölçütlerinin **hiçbiri ekrana bakmayı gerektirmedi**; M6 baştan öyle
tasarlanmıştı (§9l) ve bu, ekran hatası ortaya çıkınca işe yaradı.

---

## 9n. ⛔ EKRAN BOZUK — M6 ÖNCESİNDEN GELİYOR (açık iş)

### Belirti (kullanıcı gözle doğruladı)

- **`o` (dört köşeye dört renk):** ekran temizlenmiyor ve yalnızca **EN SON**
  çizilen kare (sarı) görünüyor. Kırmızı/yeşil/mavi yok, `pb_lcd_fill(0x0000)`
  hiç etki etmiyor.
- **`a` (tam demo):** panel çalışıyor ama yazı tipi bozuk okunmuyor,
  spektrogram yok, zemin beyaz. Yeşil "ses algılandı" yazısı **sesle tepki
  veriyor** — yani ses hattı ve demo mantığı sağlam, sorun çizimde.

### Ölçümle ELENENLER — tekrar bakmayın

| İhtimal | Nasıl elendi |
|---|---|
| **M6 gerilemesi** | M6 öncesi `main` derlemesi (bss 127.084, text 463.056) kartta denendi: **aynı şekilde bozuk** |
| **bss boyutu / yerleşimi** | Probe: aynı kod, `-DPB_TFLM_ARENA_BAYT=4096` → bss 295.784 → 177.000, **flash birebir aynı**. Hiç değişmedi |
| **Panel ya da kablolama** | `d` teşhisinin **bit-bang** varyantı (PIO/DMA tamamen devre dışı) düz renkleri **doğru** basıyor |
| **§5.9'un düzeltmesi kaybolmuş** | `QSPI_WaitIdle` yerinde, `QSPI_Deselect` onu çağırıyor |

### Buradan çıkan

Bit-bang çalışıyor, PIO/DMA yolu çalışmıyor → hata **PIO/DMA tarafında**
(§9a'daki tablonun tam olarak bu satırı). "Yalnızca en son yazılan görünüyor"
§5.9'un imzası: CS, veri hatta çıkmadan yükseliyor. Düzeltme kodda duruyor
ama görünüşe göre **yetmiyor**.

### ✅ ÖLÇÜLDÜ — `QSPI_WaitIdle` sağlam, CS zamanlaması DOĞRU (§9n adım 1–2)

Yeni `w` komutu (**göz gerekmiyor**, `python tools/capture_wav.py --port COM13
--cmd w`) `QSPI_WaitIdle`'ı sayaçlarla ölçüyor: kaç çağrı, kaçı zaman aşımına
girdi, girerken/çıkarken TX FIFO doluydu mu, kaç döngü döndü, kaç µs sürdü.
Sayaçlar `qspi_pio.c`'de, `pb_qspi_wait_*`.

| Ölçüm | Pencere komutları (3 CS) | Tek satır blit (4 CS) | Tam ekran (2560 CS) |
|---|---|---|---|
| **zaman aşımı** | **0** | **0** | **0** |
| girerken SM kapalı | 0 | 0 | 0 |
| çıkarken FIFO hâlâ dolu | 0 | 0 | 0 |
| geçen süre | — | — | 27,2 ms (PIO tabanı 15,0 ms) |

Süre tek başına da kanıt: her çağrı zaman aşımına girseydi tam ekran doldurma
2560 × 50 ms = **~128 saniye** sürerdi. 27 ms sürüyor.

**Asıl kanıt `w`'nin 4. adımı — mekanizmanın doğrudan gözlemi.** PIO saati
150 kHz'e indirilip 32 baytlık bir CASET yollanıyor; bu hızda kalıntı baytlar
CPU'nun örnekleyebileceği kadar yavaş çıkar:

```
girerken FIFO doluydu    : 1   (seviye 4/4)   <- bekleme GEREKLIYDI
gercekten bekledi        : 1   (1147 dongu, 144 us)  <- ve BEKLEDI
CIKARKEN FIFO hala dolu  : 0
CS yuksekken SCLK gecisi : 0   <- CS yukseldikten SONRA hat SUSMUS
```

> **§5.9 geri gelmemiş.** CS, veri hatta çıktıktan sonra yükseliyor. Baytlar
> yongadan çıkıyor, FIFO boşalıyor, SM sağlıklı. §9n'in 1. ve 2. maddesinin
> ikisinin de yanıtı **HAYIR**. Bu hipotezi tekrar denemeyin.

### Buradan sonrası — geriye ne kaldı

Veri yolu zamanlaması doğruysa sorun **ne gönderildiğinde** ya da panelin onu
nasıl yorumladığında. İki aday kaldı ve `y` komutu ikisini de tek oturumda
ayırıyor:

1. **Pencere komutları (CASET/RASET) panele geçmiyor, piksel verisi geçiyor.**
   Belirtiyle birebir uyuşuyor: pencere hiç değişmezse her RAMWR yazma
   imlecini aynı yere döndürür, her çizim bir öncekinin üstüne biner →
   *"ekran temizlenmiyor, yalnızca EN SON çizilen görünüyor"*. `a` demosunda
   da aynısı: LVGL'in son çizdiği yeşil etiket görünüyor, gerisi üst üste
   binmiş durumda.
2. **SCLK hızı.** Üretim `clkdiv 2.0` → bayt başına 4 PIO çevrimi →
   **SCLK 37,5 MHz**. AXS15231B'nin üst sınırı bu civarda. Bit-bang ~500 kHz'te
   çalışıyor — yani "bit-bang çalışıyor, PIO çalışmıyor" farkı yol farkı değil
   **hız** farkı da olabilir.

### ✅ `y` ÇALIŞTIRILDI — altı adımın ALTISI da aynı (kullanıcı gözle)

> **"Bütün adımlarda masmavi ekran + sol alt köşede beyaz kutu vardı."**

Bu iki şeyi birden söylüyor:

- **Yol ve saat hızı elendi.** Bit-bang, PIO/DMA, 37,5 / 3,75 / 0,94 MHz —
  hepsi *birebir aynı*. Hata bunların hiçbirinde değil. Bu ekseni kapatın.
- **Tam ekran düz dolgu ÇALIŞIYOR** (ekran masmavi oldu, altı adımda da).
  Bozuk olan yalnızca **konumlandırma**: panelin tam ortasına istenen 40x40
  kare uca düştü — ve **şerit değil, kutu** olarak. Yani sütun aralığı
  (CASET, 40 piksel genişlik) uygulanmış, **satır yok sayılmış**.

### ✅ KÖK NEDEN (iki bağımsız kaynakla) — bu panel RASET'i yok sayıyor

`rsvpnano`'daki **çalışan** iki sürücü de aynı şeyi yapıyor:

```
rsvpnano-main/src/drivers/display/axs15231b/axs15231b.cpp          (ESP32)
rsvpnano-main/src/drivers/display/axs15231b_pio/axs15231b_pio.cpp  (RP2350 + PIO)
```

**İkisi de `0x2B` (RASET) komutunu HİÇ yollamıyor.** İkisinde de tek pencere
komutu `setColumnWindow()` → `0x2A` (CASET). Satır konumu komutla değil,
yazma sırasıyla belirleniyor — RP2350 sürücüsünden birebir:

```cpp
setColumnWindow(context, x, x + width - 1);
// RAMWR (0x2C) resets the panel's write pointer; RAMWRC (0x3C) continues an
pioPushSingleLineByte(0x32);
pioPushSingleLineByte(0x00);
pioPushSingleLineByte(y == 0 ? 0x2C : 0x3C);
```

Yani panelin sözleşmesi:

| Komut | Ne yapar |
|---|---|
| `0x2A` CASET | sütun aralığını ayarlar — **çalışıyor** |
| `0x2B` RASET | **yok sayılıyor** |
| `0x2C` RAMWR | imleci sütun penceresinin **en üstüne** alır |
| `0x3C` RAMWRC | bir önceki yazmanın **bittiği yerden devam** eder |

Bizim sürücümüz RASET yollayıp satırın oraya gitmesini bekliyor. Gitmiyor:
**her RAMWR satır 0'a dönüyor.**

### Bu, gözlenen HER ŞEYİ açıklıyor

| Belirti | Açıklama |
|---|---|
| `pb_lcd_fill` "ekranı temizlemiyor" | 640 satırın 640'ı da aynı **üst satıra** yazılıyor |
| `o`'da yalnızca sarı görünüyor | dört kare üst üste biniyor, sonuncusu kazanıyor |
| `a`'da yazı bozuk, zemin beyaz | LVGL'in her kısmi çizimi tepede birikiyor |
| yeşil "ses algılandı" tepki veriyor | her karede **en son** çizilen o |
| bit-bang düz renk çalışıyor (§9n) | tam ekran zaten satır 0'dan başlıyor — RASET'e ihtiyaç yok |
| tüm veri yolu ölçümleri temiz | veri yolu hiçbir zaman sorun değildi |

**Neden şimdiye kadar fark edilmedi:** kabul ölçütlerinin hepsi ya tam ekran
düz dolguydu ya da göz gerektirmeyen sayaçlardı. Hata sürücüde **baştan beri**
duruyordu; §5.9 ve §9h gerçek ve ayrı hatalardı, bu üçüncüsü onların altında
kalmış.

### 🔵 SIRADAKİ ADIM — `z` satır adresleme testi (GÖZ GEREKİR, kullanıcı çalıştırır)

`python tools/capture_wav.py --port COM13 --cmd z` — beş adım, hepsinde aynı
hedef: panelin **tam ortasına** 40x40 beyaz kare. Tek soru: kare **ORTADA** mı,
**UÇTA** mı?

| Adım | Yöntem | Beklenen |
|---|---|---|
| 1 | şimdiki yol: CASET + RASET + çıplak `0x2C` | uçta |
| 2 | CASET + RASET, çıplak `0x2C` yok | uçta |
| 3 | sıra ters: önce RASET sonra CASET | uçta |
| 4 | **referans yol**: yalnız CASET, 300 satır atlanıp yazılıyor | **ortada** |
| 5 | referans + `0x3C` RAMWRC ile devam | ortada (kalıcı çözüm ucuzsa) |

[4] ortada ve [1][2][3] uçtaysa kök neden doğrulanmış olur. [5] de ortadaysa
RAMWRC çalışıyor demektir ve çözüm ucuz: LVGL akışı baştan sona tek geçiş,
atlama bedeli yok. [5] uçtaysa her çizim atlama bedeli öder (bkz. aşağısı).

### ✅ `z` ÇALIŞTIRILDI — kök neden DOĞRULANDI (kullanıcı gözle)

> **"[1] [2] ve [3]'te sol köşede beyaz kutu; [4] [5] ortada."**

| Adım | Yöntem | Sonuç |
|---|---|---|
| 1 | CASET + RASET + çıplak `0x2C` (şimdiki sürücü) | ❌ uçta |
| 2 | CASET + RASET, çıplak `0x2C` yok | ❌ uçta |
| 3 | önce RASET sonra CASET | ❌ uçta |
| 4 | **yalnız CASET**, satır RAMWR'den sayılıyor | ✅ **ortada** |
| 5 | yalnız CASET + `0x3C` RAMWRC ile devam | ✅ **ortada** |

İki sonuç birden: **RASET yok sayılıyor** (kök neden kesin) ve **RAMWRC
çalışıyor** — yani ardışık yazımlar atlama bedeli ödemeden zincirlenebiliyor.

### ✅ SÜRÜCÜ DÜZELTİLDİ

[`lcd_blit.c`](src/hal/display/lcd_blit.c) panelin gerçek sözleşmesine göre
yeniden yazıldı. Artık `0x2B` hiç yollanmıyor; sürücü **yazma imlecini takip
ediyor** (`s_imlec_*`): hedef satır imlecin durduğu yerse `0x3C` ile bedava
devam ediyor, değilse `0x2C` + atlama.

| Değişiklik | Etki |
|---|---|
| `pb_lcd_fill` tek geçişe indi | 2560 CS işlemi → **2**; 27,2 ms → **11,8 ms** (ölçüldü, kuramsal taban 11,74) |
| `pb_lcd_blit` / `_strided` imleç kullanıyor | ardışık çizim bedava; rastgele erişim atlama ödüyor |
| `o` (yön testi) tek geçişe çevrildi | dört köşe artık **aynı RAMWR akışında** üretiliyor |
| LVGL `alan_yuvarla` (yeni) | kirli alanın `x1`i 0'a sabitlendi → `panel_y` hep 0 → atlama yok |
| `pb_lcd_sutun_penceresi` / `pb_lcd_akis_basla` | dışarıdan çağrılınca imleci **kendiliğinden** geçersiz kılıyor |

> LVGL'de `x2`ye bilerek dokunulmadı: onu da tam genişliğe çekmek her
> yenilemede panelin tamamını çizdirir ve `a` demosunda spektrogram şeridini
> siler. Kirli alan yalnızca **sola** büyütülüyor.

### ⚠ AÇIK KALAN — `a` demosunun spektrogramı

`ui/spectrogram.c` panel satırı `200+k`'ya yazıyor ve her karede LVGL flush'ı
ile sıra alıyor. LVGL yazınca imleç kayıyor, sonraki spektrogram yazımı imleci
tutturamıyor ve `(200+k)*172` piksellik atlama hem pahalı hem **kartı siliyor**.
Ardışık kalması da mümkün değil: her itme 2 satır yazıp 1 satır ilerliyor
(veri + "şimdi" imleci), yani her karede **1 satır geri** gitmek gerekiyor —
panel geri gitmeye izin vermiyor.

Bunu `j` testinin sonucu belirliyor (aşağıda). `o`, `d`, `u` ve `pb_lcd_fill`
şu anki hâliyle doğru; **`a` hâlâ bozuk** ve öyle olduğu biliniyor.

### 🔵 SIRADAKİ ADIM — `j` imleç konumlandırma testi (GÖZ GEREKİR)

`python tools/capture_wav.py --port COM13 --cmd j` — üç adım, mimariyi
belirleyen tek soru: **sütun penceresi daraltılırsa satır daha ucuza
ilerletilebilir mi, ve pencere yeniden genişleyince satır korunur mu?**

Satır, pencere genişliği kadar piksel yazıldıkça ilerliyor. Pencere 1 piksel
genişse `y` satır ilerletmek `172*y` yerine **`y`** piksele mal olur.

| Adım | Yöntem | Beklenen (çalışıyorsa) |
|---|---|---|
| 1 | dar atlama aynı sütunda (x=66), sonra CASET genişlet + RAMWRC | ortada + **ince** kırmızı çizgi |
| 2 | dar atlama başka sütunda (x=0), sonra CASET 66..105 + RAMWRC | ortada + ince kırmızı çizgi kenarda |
| 3 | kontrol: `z[4]` ile aynı (geniş atlama) | ortada + **geniş** kırmızı blok |

- **[1] ve [2] ortadaysa** → ucuz konumlandırma var. `pb_lcd_blit` genel amaçlı
  kalır, spektrogram ile LVGL aynı ekranda yaşar, bedel satır başına 1 piksel.
- **[1]/[2] uçtaysa** → pencere değişince satır sıfırlanıyor. Panel yalnızca
  yukarıdan aşağı **tek geçiş** çizime izin veriyor; arayüz katmanı (`a`
  demosunun kart + şerit düzeni) buna göre yeniden kurulmalı.

Aynı turda **`o` da bakılmalı**: dört köşe artık dört ayrı köşede olmalı.
Bu, §9n'in baş belirtisinin gerçekten kapandığının gözle onayı.

### Çözüm tasarımı — `z` doğrularsa (yapıldı, yukarıda)

`pb_lcd_blit(x, y, w, h)` artık RASET'e güvenemez. İki seçenek:

1. **Atlamalı** (her zaman doğru, bedeli var): CASET `x..x+w-1`, `0x2C`, sonra
   `y*w` piksel atlama verisi, sonra gerçek veri. En kötü durum (y=600,
   w=172) 103.200 piksel atlama = tam ekranın kendisi kadar. Küçük
   dikdörtgenlerde ucuz (`w` dar olduğu için), tam genişlikte pahalı.
2. **RAMWRC ile akış** (ucuz, sıra kısıtı var): panelin bir karesi baştan sona
   **tek geçişte, yukarıdan aşağı** yazılır; ilk parça `0x2C`, gerisi `0x3C`.
   LVGL'in flush'ı zaten bu sırayla geliyor (§9b'deki 90° çevrim korunur).
   `ui/spectrogram.c` ve `lv_port.c` bu sözleşmeye uydurulmalı.

Doğrusu 2, 1'i de yedek olarak bırakmak. Ama **önce `z` ile ölçün** —
`pb_lcd_akis_basla/renk/bitir` üçlüsü ([lcd_blit.h](src/hal/display/lcd_blit.h))
bu sözleşmeyi ifade etmek için zaten eklendi.

### (geçersiz kaldı) `y` melez yol testi — ne yapıyordu

`python tools/capture_wav.py --port COM13 --cmd y` — etkileşimli, altı adım.
Her adımda ekran **mavi** olmalı ve **ortasında 40x40 beyaz kare**. Pencere
komutu düşerse kare ortada değil, **en üstte tam genişlikte bir şerit** çıkar.

| Adım | Pencere | Piksel | SCLK |
|---|---|---|---|
| 1 | bit-bang | bit-bang | — (kontrol: §9n'e göre çalışıyor) |
| 2 | bit-bang | PIO/DMA | 37,5 MHz |
| 3 | PIO | bit-bang | 37,5 MHz |
| 4 | PIO | PIO/DMA | 37,5 MHz (**üretim yolu**) |
| 5 | PIO | PIO/DMA | 3,75 MHz |
| 6 | PIO | PIO/DMA | 0,94 MHz |

Okuma: [2] çalışıp [3] çalışmıyorsa hata **pencere komutlarında**; tersi ise
**piksel/DMA yolunda**; [4] bozuk ama [5]/[6] düzgünse hata **saat hızında**
ve düzeltme tek satır (`qspi.pio`'nun clkdiv'i).

### ✅ Yan olarak düzeltildi — bit-bang testi artık PIO'yu geri veriyor

`d`'nin bit-bang varyantı ve `v`'nin 6. adımı pinleri SIO'ya alıp SM'i
kapatıyor, geri vermiyordu; **ondan sonraki her ekran testi sahte biçimde
"bozuk" görünüyordu**. Yeni `QSPI_PIO_Restore()` pinleri, SM'i, FIFO'ları ve
kaydırma sayacını geri alıyor (`pio_add_program`'ı TEKRAR çağırmadan — PIO
komut belleği 32 komut, her çağrı 2 komut daha yakardı). İki ekran testi
arasında artık kartı yeniden başlatmak gerekmiyor.

`w` PIO'yu kapalı bırakmıyor, ardından ekran testi çalıştırmak güvenli.

> **Bu hatayı ararken göz gerektiren testi ertelemeyin.** §9h'de tam olarak
> bu yapıldı (20 ms'lik ikili onaylandı, gönderilen 250 ms'lik sürüme hiç
> bakılmadı) ve hata bir oturum boyunca sessizce durdu.

### Eğer `y` de sonuçsuz kalırsa — sırada ne var

- **Firmware kaynağı `dba7a01` ile `main` arasında BİREBİR AYNI** (ölçüldü:
  `git diff dba7a01 main -- src/ CMakeLists.txt boards/ cmake/` boş). Yani
  `main` = §9h'nin sonunda gönderilen 250 ms'lik ikili, ve kullanıcının gözle
  onayladığı ikili 250 ms'lik **değil, 20 ms'likti**. Aradaki tek fark o satır.
  Ucuz A/B: `dba7a01`'i 250 → 20 yapıp derleyin ve baktırın.
- Son çare: bit-bang yolu çalıştığına göre ekran oradan sürülebilir — yavaş
  ama M7'yi açar.

---

## 10. Depo düzeni ve git durumu

```
boards/     Pico SDK board tanımı (pokebird_rp2350b.h)
cmake/      toolchain.cmake, picotool.cmake, pico_sdk_import.cmake
docs/       ARCHITECTURE.md (tam plan)
src/
  board_config.h    ← pin haritası, TEK doğruluk kaynağı
  main.c            ← seri komut kabuğu + tüm teşhis komutları
  hal/
    audio_i2s(.c/.h/.pio), es8311(.c/.h), i2c_bus(.c/.h)
    touch(.c/.h)                    ← kendi dokunmatik sürücümüz
    display/  qspi.pio, qspi_pio(.c/.h), LCD_3in49(.c/.h),
              DEV_Config.h, dev_config.c, lcd_blit(.c/.h)
  dsp/        fft(.c/.h), mel(.c/.h), gate(.c/.h)
  ai/         tur_agi(.cc/.h)     ← TFLM sarmalayicisi + arena
              tflm_port.cc        ← DebugLog / micro_time / abort() ezmesi
              tanima(.c/.h)       ← core1 gercek zamanli hat + birlestirme
              dogrulama_seti.h    ← URETILMIS (GIRIYOR, 94 KB flash)
              siniflar.h          ← URETILMIS (GIRIYOR)
  ui/         spectrogram(.c/.h), lv_conf.h, lv_port(.c/.h)
test/       CMakeLists.txt, dsp_test.c      ← host tarafı DSP testleri
tools/      capture_wav.py, mel_reference.py,
            species_list.py, xc_fetch.py,
            xc_convert.py, m4_run.py        ← M4 veri boru hattı
            birdnet_slist.py, birdnet_run.py,
            birdnet_ozet.py, segment_kes.py ← M4 adım 3: segmentasyon
            esc50_indir.py, egitim_kumesi.py ← M4 adım 4: eğitim kümesi
            egit.py, birlestirme_olc.py     ← M5: eğitim + değerlendirme
            dogrulama_seti.py               ← M6: cihaz-içi doğrulama seti
            sinif_tablosu.py                ← M6: sınıf adları -> siniflar.h
models/     tur_agi_int8.h                  ← C dizisi (GİRİYOR, firmware derliyor)
            rapor.txt, birlestirme.txt      ← doğruluk kayıtları (GİRİYOR)
            tur_agi.keras, *.tflite         ← girmiyor, üretilebilir
            ilerleme.html                   ← canlı eğitim panosu (girmiyor)
data/       species_istanbul.csv            ← tür tablosu (git'e giriyor)
            birdnet_ad_haritasi.csv         ← kod→BirdNET adı (GİRİYOR, §5.16)
            .xc_key                         ← XC API anahtarı (GİRMİYOR)
            wav/                            ← eğitim verisi (girmiyor)
            xc/kayitlar.csv                 ← lisans/atıf kaydı (girmiyor)
            cache/                          ← API önbelleği (girmiyor)
            birdnet_sonuc/                  ← kayıt başına sonuç CSV (girmiyor)
            birdnet_log/                    ← işçi günlükleri (girmiyor)
            segmentler.csv                  ← dilim dizini (girmiyor, üretilebilir)
            egitim/                         ← EĞİTİM KÜMESİ (girmiyor, 739 MB)
                pencereler.npy · etiket.npy · ogretmen.npy
                ornekler.csv · siniflar.csv · ozet.txt
            negatif/                        ← ESC-50 (girmiyor)
                wav/<kategori>/*.wav · esc50_kayitlar.csv
                birdnet_sonuc/              ← negatiflerin kuş taraması
.venv-birdnet/  BirdNET 3.11 ortamı + model (girmiyor, ~1 GB)
third_party/  pico-sdk/, lvgl/              (git'e girmiyor)
              tflite-micro/, flatbuffers/, gemmlowp/, ruy/, cmsis-nn/
                                            ← M6, girmiyor; kurulum §9m'de
rsvpnano/     kullanıcının kopyası           (git'e girmiyor)
```

### Git

**Dal `m6`** (uzak depo yok, çalışma ağacı temiz). `main` M5 sonunda duruyor;
`m6` ekran hatası ayıklanırken kontrol grubu olarak kullanıldı ve o yüzden
henüz birleştirilmedi. Kontrol derlemesi: `git checkout main && cmake -S . -B build-head ...`

M6 oturumunun commit'leri (2 Ağustos 2026):

| Commit | Ne |
|---|---|
| `ce79542` | **M6: TFLM + CMSIS-NN entegrasyonu** — `cmake/tflm.cmake`, `src/ai/`, `x`/`k`/`K` komutları, ses halkası 8192, cihaz-içi doğrulama seti (§9m) |

Bu oturumun (2 Ağustos 2026) commit'leri:

| Commit | Ne |
|---|---|
| `ae9ae90` | **M4 adım 3: BirdNET segmentasyon boru hattı** — dört araç, üç ölçülmüş karar, bir sessiz hata (§9e, §5.13–5.16) |
| `1ddf465` | **`s_capture` temizliği** — teşhis komutları akışa çevrildi, bss 218.988 → 127.084 (§9g) |
| `b69145e` | lastsession.md: commit hash'i yazıldı |
| `7db6961` | lastsession.md: §9h ekran gerilemesi — üç koşuluk A/B, bellek haritası |
| `dba7a01` | **Panel hazır olma penceresi** — ekran başlatması açılıştan ≥250 ms sonra (§9h kök neden) |
| `9ec1654` | lastsession.md: §9h çözüldü, §9i eğitim kümesi planı, §5.17 dersi |
| `928e331` | lastsession.md: commit hash'i yazıldı |
| `3762dd4` | **M4 adım 4: eğitim kümesi** — `egitim_kumesi.py`, `esc50_indir.py`, `dsp_test --pencere`; üç katmanlı birebirlik sağlaması, sızıntı kontrolü, ESC-50 kuş sınıfları (§9j) |
| `eb29fe7` | **M5: eğitim hattı** — `egit.py`, damıtma + focal + INT8, cihaz sözleşmesi (girdi ölçeği 1.0), HTML ilerleme panosu (§9k) |
| `96c5094` | **M5 sonuçları** — tür ağı eğitildi, INT8 bedeli ~0, `birlestirme_olc.py` ile birleştirme ölçüldü; iki kendi hatam düzeltildi (dairesel budama ölçümü, kapasite≠darboğaz) |

> HEAD sağlam: bss 127.084, ekran çalışıyor, ses hattı 63 kare/s kayıp 0.
> Kartta HEAD duruyor.

Önceki oturumlardan (eskiden yeniye): `7273768` ekran çalışıyor (§5.9) ·
`68ba8b2` spektrogram yönü · `768cda9` M3 mel + kapı · `9e7e2dc` dokunmatik
sürücüsü · `30c6ac9` M2b LVGL · `cfcc0c4` teşhis komutları · `a7ca72b` M3
sürekli yakalama · `6b27ab6` `a` demosu · `5bedba4`/`9deba1b`/`e7bc464`
M4 hazırlığı · `038230e` M4 veri toplama tamamlandı.

Firmware tarafındaki commit sıralaması **her commit derlenebilir kalsın**
diye seçilmişti: DSP dosyaları CMakeLists'e eklenmeden önce commit'lendiği
için ara commit'lerde derlemeye girmiyorlar ve firmware bozulmuyor.

### rsvpnano referansı

Kullanıcının bu panelde çalıştırdığı kitap okuma uygulaması (ESP32-S3/Arduino).
İki kopya: `C:\Users\hp\rsvpnano` (asıl) ve `pokebird/rsvpnano/` (gitignore'da).

Değerli dosyalar:
- `src/display/axs15231b.cpp` — panelin asgari init dizisi + arka ışık
- `src/input/TouchHandler.cpp` — dokunmatik protokolü (8 baytlık okuma buradan)

Satıcı LVGL örneği ayrıca `C:\Users\hp\AppData\Local\Temp\lcd\ext\` altına
açılmıştı (geçici dizin, silinmiş olabilir).
