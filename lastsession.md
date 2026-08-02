# PokeBird — Oturum Devir Teslimi

> Bu dosya, yeni bir Claude oturumunun projeyi sıfırdan anlayıp kaldığı yerden
> devam edebilmesi için yazıldı. Mimari planın tamamı [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
> içinde; burada onun özeti, şu ana kadar yapılanlar, **denenip işe yaramayanlar**
> ve sıradaki adımlar var.
>
> Son güncelleme: 2 Ağustos 2026.

## ⚠ ÖNCE BUNU OKUYUN

**Durum:** M0 ✅ · M1 ✅ · M2a ✅ · M2b 🔶 (dokunmatik park edildi) · M3 ✅

Çalışma ağacı temiz, her şey commit edildi (§10).

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

**`s_capture` temizliği ✅ BİTTİ (§9g)** — ama **bir GERİLEME açığa çıkardı
(§9h). ÖNCE ONU OKUYUN.**

```
bss  218.988  ->  127.084 bayt     (91.904 bayt serbest, hedef <=130.000)
arena (180 KB) ile birlikte 311.404  ->  520 KB SRAM'de ~208 KB pay
```

Kabul ölçütünün 4'ü geçti (bellek, kayıt sürekliliği, gürültü tabanı, kare
hızı). **5. ölçüt DÜŞTÜ: `a` demosunda EKRAN BOZUK** — panel karıncalanma
gösteriyor (§5.9'daki tabloyla aynı).

> ### 🔴 KARTTA HEAD DEĞİL, TEŞHİS İKİLİSİ (`probe`) DURUYOR
> Oturum kapanırken kartta **`probe`** firmware'i kaldı: HEAD'in kodu, tek
> farkı `s_chunk`'ın 48000 elemanlı bırakılması (§9h). **Ekranı çalışıyor**,
> ama bss'i 218.988 — yani bellek kazancı o ikilide YOK ve **hiçbir commit'e
> karşılık gelmiyor.**
>
> HEAD'i (bss 127.084) derleyip yüklerseniz **ekran karıncalanır** — bu
> beklenen, teşhisi konmuş durum, yeni bir sürpriz değil. Kök neden
> `s_capture` temizliği DEĞİL; o temizlik kodda zaten var olan gizli bir
> hatayı görünür yaptı. Kanıt ve yöntem §9h'de.

**SIRADAKİ İŞ — §9h'deki sınır dışı yazmayı bulmak.** Ondan sonrası eğitim
kümesi (§9f-5) → M5; `data/segmentler.csv` hazır bekliyor.

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
./test/build/dsp_test          # 13 test, 0 kaldi
python tools/mel_reference.py  # 64 bandin tamaminda sapma 0.0000 dB
```

Saniyeler sürüyor ve şimdiden iki gerçek hata yakaladı (§9c). Mikrodenetleyicide
DSP hatası ayıklamak çok pahalı; **DSP değişikliklerini önce burada doğrulayın.**

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
| `t` | dokunmatik teşhisi (canlı akış) | **evet** |
| `u` | LVGL demo ekranı | **evet** |
| `m` | **mel + kapı hattı (M3)** | hayır |
| `a` | **TAM DEMO**: LVGL kart + canlı mel spektrogramı + kapı | **evet** |

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

### 5.5 PC hoparlöründen bip çalarak akustik test — GEÇERSİZ

`[Console]::Beep` ile test yapıldı, "mikrofon çalışıyor" sanıldı. **Ama
bilgisayarda kulaklık takılıydı, hoparlörden ses çıkmadı.** Görülen artış
odadaki başka bir sesti. Doğru test: kullanıcının el çırpması (geçti).

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

## 6. Açık konular / borçlar

| Konu | Durum |
|---|---|
| **EMI ölçümü geçersiz** | M1'deki tarama PWM ile yapıldı, ışık hep kapalıydı. `e` komutu aç/kapa olarak düzeltilip yeniden ölçülmeli (§4). |
| ~~**`s_capture` (96 KB)**~~ | ✅ **ÇÖZÜLDÜ (§9g).** 4 KB'lık `s_chunk`'a indi, bss 218.988 → 127.084. Arena'nın önü açık. |
| 🔴 **Ekran gerilemesi** | **SIRADAKİ İŞ — §9h.** HEAD'in firmware'inde `a` demosunda panel karıncalanıyor. Kök neden bellek yerleşimine bağlı gizli bir sınır dışı yazma; üç koşuluk A/B ile kanıtlandı. Bellek kazancından vazgeçilmemeli. |
| **Dokunmatik park edildi** | Kritik yolda değil. Kaldığı yer §9b. |
| **PWM GPIO36'yı sürmüyor** | Kök neden bulunmadı; arka ışık düz GPIO. Parlaklık ayarı gerekirse (M7) çözülmeli. |
| **GPIO34 (LCD_RST) aşağı çekilemiyor** | Ölçüldü, kök neden aranmadı. Bkz. §5.11. |
| `SYS_EN` (GPIO39) | Açılışta 1'e çekiliyor (`power_latch_init`). Waveshare'in `DEV_Module_Init()`'inden alınan tek iş. Pil ile çalışırken güç mandalı için doğru olan bu. |
| Ekran teşhis iskelesi | `v` komutu, `d`'nin varyantları ve `main.c`'deki bit-bang yolu duruyor. **Korunmalı** — QSPI bir daha bozulursa en hızlı yol bunlar. |
| **es8311.c lisansı** | "ESPRESSIF MIT License" standart MIT DEĞİL; kullanımı Espressif ürünleriyle sınırlı. Kişisel kullanımda pratik sorun yok. **Dağıtım öncesi** veri sayfasından kendi sürücümüz yazılmalı. |
| **BirdNET lisansı** | CC BY-NC-SA 4.0. Damıtılan model türev sayılabilir → ticari kullanımı kısıtlar. Ticari yol için damıtmasız varyant gerekir. ARCHITECTURE §6. |
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
| **M4** | **Veri boru hattı + tür listesi (PC tarafı)** | **🔶 veri ✅ (178 tür, 7.111 WAV) · segmentasyon ✅ (§9e) · negatifler kaldı (§9f-4)** |
| M5 | Model eğitimi + damıtma + INT8 | |
| M6 | TFLM entegrasyonu, gerçek zamanlı çıkarım (core1) | |
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
4. ⏸ **Negatif TOPLAMA — KULLANICI KARARIYLA ASKIYA ALINDI (2 Ağustos 2026)**

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
   SNR'lerde gürültü karıştırma, SpecAugment, oda/mesafe simülasyonu
6. ⏳ **M5**: damıtma ile eğitim (BirdNET yumuşak çıktıları öğretmen),
   focal loss (sınıf dengesizliği), INT8 niceleştirme, doğruluk raporu

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
| 5 | `--cmd a` tam demo ekranda çalışıyor | ❌ **DÜŞTÜ — ekran bozuk, §9h** (ses hattı 63 kare/s, kayıp 0) |

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

Önce §9h (ekran gerilemesi), sonra eğitim kümesi (§9f-5) → M5.

---

## 9h. 🔴 SIRADAKİ İŞ — bellek yerleşimine bağlı ekran gerilemesi

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

### Sonraki oturumda izlenecek yol

1. **Kurbanı doğrulayın (kanarya).** `s_draw_buf`'ın önüne ve arkasına bilinen
   desenli guard dizileri koyun (ör. 64 bayt `0xA5`), `pb_lv_tick` sonrası
   kontrol edip bozulanı seri porta yazdırın. Hangi tamponun, hangi yönden ve
   kaç bayt ezildiğini doğrudan söyler. Bu, göz gerektirmeyen bir test —
   §5.10'un aksine burada dolaylı ölçüm meşru, çünkü *bellek* ölçüyoruz.
2. **Eşiği ikiye bölün.** `s_chunk`'ı 48000 → 24000 → 12000 → 8000 → 4096
   yapıp ekranın hangi boyutta bozulmaya başladığını bulun. Kırılma noktası,
   taşmanın kaç bayt olduğunu doğrudan verir (kabaca: bozulmanın başladığı
   boyut ≈ taşma miktarı).
3. Şüpheli tamponları yazan kodu okuyun: `src/dsp/mel.c` (halka indeksleme),
   `src/ui/spectrogram.c`, `src/ui/lv_port.c` (kısmi render + devrik blit —
   negatif adımlı `pb_lcd_blit_strided` sınır hesabı burada kolay kaçar).
4. **Bellek kazancından VAZGEÇMEYİN.** Hata gerçek ve zaten oradaydı; TFLM
   arena'sı (180 KB) geldiğinde yerleşimi nasılsa yine kaydıracak ve aynı
   çökme M6'nın ortasında, çok daha pahalı bir yerde çıkacaktı. **Şimdi
   bulmak ucuz.** `s_capture` temizliği bu hatayı *yaratmadı*, ortaya çıkardı.

### Kabul ölçütü

- Kanarya testi hangi tamponun ezildiğini söylüyor ve kök neden bulunuyor
- `s_chunk[CHUNK_SAMPLES]` (4 KB) hâliyle `--cmd a` **ekranda düzgün**
  (kullanıcı bakacak — bu ölçüt göz gerektiriyor, ölçümle geçilmiş sayılmaz)
- bss ≤ 130.000 korunuyor
- `--cmd m` 63 kare/s kayıp 0, `--cmd r` sürekliliği bozulmamış

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
  ui/         spectrogram(.c/.h), lv_conf.h, lv_port(.c/.h)
test/       CMakeLists.txt, dsp_test.c      ← host tarafı DSP testleri
tools/      capture_wav.py, mel_reference.py,
            species_list.py, xc_fetch.py,
            xc_convert.py, m4_run.py        ← M4 veri boru hattı
            birdnet_slist.py, birdnet_run.py,
            birdnet_ozet.py, segment_kes.py ← M4 adım 3: segmentasyon
data/       species_istanbul.csv            ← tür tablosu (git'e giriyor)
            birdnet_ad_haritasi.csv         ← kod→BirdNET adı (GİRİYOR, §5.16)
            .xc_key                         ← XC API anahtarı (GİRMİYOR)
            wav/                            ← eğitim verisi (girmiyor)
            xc/kayitlar.csv                 ← lisans/atıf kaydı (girmiyor)
            cache/                          ← API önbelleği (girmiyor)
            birdnet_sonuc/                  ← kayıt başına sonuç CSV (girmiyor)
            birdnet_log/                    ← işçi günlükleri (girmiyor)
            segmentler.csv                  ← dilim dizini (girmiyor, üretilebilir)
.venv-birdnet/  BirdNET 3.11 ortamı + model (girmiyor, ~1 GB)
third_party/  pico-sdk/, lvgl/              (ikisi de git'e girmiyor)
rsvpnano/     kullanıcının kopyası           (git'e girmiyor)
```

### Git

Dal `main`, uzak depo yok, çalışma ağacı temiz.

Bu oturumun (2 Ağustos 2026) commit'leri:

| Commit | Ne |
|---|---|
| `ae9ae90` | **M4 adım 3: BirdNET segmentasyon boru hattı** — dört araç, üç ölçülmüş karar, bir sessiz hata (§9e, §5.13–5.16) |
| `1ddf465` | **`s_capture` temizliği** — teşhis komutları akışa çevrildi, bss 218.988 → 127.084 (§9g) |
| `b69145e` | lastsession.md: commit hash'i yazıldı |
| *(bu tur)* | lastsession.md: **§9h ekran gerilemesi** — üç koşuluk A/B, bellek haritası, izlenecek yol |

> ⚠ **HEAD derlenebiliyor ve ses tarafı sağlam, ama `a` demosunda ekran
> bozuk (§9h).** Bu bilinerek commit'lendi: hata `s_capture` temizliğinin
> yarattığı bir şey değil, açığa çıkardığı gerçek bir hata. Geri almak onu
> yeniden gizlerdi ve TFLM arena'sı geldiğinde M6'nın ortasında patlardı.

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
