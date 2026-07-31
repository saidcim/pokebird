# PokeBird — Oturum Devir Teslimi

> Bu dosya, yeni bir Claude oturumunun projeyi sıfırdan anlayıp kaldığı yerden
> devam edebilmesi için yazıldı. Mimari planın tamamı [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
> içinde; burada onun özeti, şu ana kadar yapılanlar, **denenip işe yaramayanlar**
> ve sıradaki adımlar var.
>
> Son güncelleme: 31 Temmuz 2026 — M1 tamamlandı, sırada M2.

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

---

## 3. Geliştirme ortamı — pratik bilgiler

### Derleme

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Çıktı: `build/pokebird.uf2`

- CMake + Ninja: winlibs mingw64 paketinden, PATH'te
- ARM GCC 14.3: `~/.platformio/packages/toolchain-rp2040-earlephilhower` — [`cmake/toolchain.cmake`](cmake/toolchain.cmake) otomatik buluyor
- Pico SDK 2.3.0: `third_party/pico-sdk` altına klonlu (git'e girmiyor)

SDK yoksa:
```bash
git clone -b 2.3.0 --depth 1 https://github.com/raspberrypi/pico-sdk.git third_party/pico-sdk
git -C third_party/pico-sdk submodule update --init --depth 1 lib/tinyusb
```

### Karta yükleme — düğmeye basmadan

Kart çalışırken 1200 baud'da porta bağlanmak onu BOOTSEL'e sokuyor, sonra
`.uf2` sürücüye kopyalanıyor. PowerShell ile:

```powershell
try { $p = New-Object System.IO.Ports.SerialPort 'COM13',1200,'None',8,'one'; $p.Open(); Start-Sleep -Milliseconds 250; $p.Close() } catch {}
Start-Sleep -Seconds 4
$v = Get-Volume | Where-Object { $_.FileSystemLabel -like 'RP2350*' }
Copy-Item 'C:\Users\hp\Downloads\pokebird\build\pokebird.uf2' "$($v.DriveLetter):\pokebird.uf2" -Force
```

Elle: BOOT + RESET'e bas, önce RESET'i sonra BOOT'u bırak, `.uf2`'yi sürücüye kopyala.

### Cihazla konuşma

Kart USB seri olarak görünüyor: **COM13** (`VID_2E8A PID_0009`).

```bash
python tools/capture_wav.py --port COM13 --cmd i    # bilgi
python tools/capture_wav.py --port COM13 --cmd n    # gurultu tabani
python tools/capture_wav.py --port COM13 --cmd e    # EMI taramasi
python tools/capture_wav.py --port COM13 --cmd l    # canli seviye
python tools/capture_wav.py --port COM13 --out a.wav  # kayit al
```

pyserial 3.5 kurulu. Python 3.14 varsayılan.

---

## 4. Tamamlananlar

### M0 — proje iskeleti ✅

- CMake + Pico SDK iskeleti, `boards/pokebird_rp2350b.h` (SDK'da RP2350B için hazır başlık yok)
- `src/board_config.h` — pin haritası, tek doğruluk kaynağı
- Derleme-zamanı kontrolleri (`static_assert`): yanlış board seçilirse derleme durur.
  GPIO40 ve 41–47 sadece RP2350B'de var; bu sessizce yanlış giderse bulması çok pahalı olurdu.
- Canlılık göstergesi olarak ekran arka ışığı (LED olmadığı için)

### M1 — mikrofon bring-up ✅

**Sonuç: mikrofon kuş sesi tanıma için kullanılabilir.**

| Ölçüm | Sonuç |
|---|---|
| ES8311 çip kimliği | 0x1183, I2C yanıt veriyor |
| MCLK | 6.144 MHz (PIO), ES8311 master, 24 kHz örnekleme |
| Yakalama | 48000/48000 örnek, düşen örnek yok |
| Tekrarlanabilirlik | 6 ardışık ölçüm, -36.2 … -36.8 dBFS (±0.3 dB) |
| **Arka ışık EMI** | kapalı -36.8, tam açık -37.3, PWM %50 -36.9, PWM %10 -36.6 → **en kötü +0.2 dB** |
| Kazanç taraması | 4→6→7 kademelerinde +12.2 ve +6.8 dB |
| El çırpma testi | **geçti** — mikrofon sese tepki veriyor |

**Yorum:** Gürültü kazançla ölçekleniyor (ES8311'in 6 dB'lik PGA adımlarıyla
neredeyse birebir), yani PGA'dan **önce** giriyor: akustik kaynaklı, devre
kaynaklı değil. -36 dBFS taban, kullanıcının odasındaki **fan sesi**.
Arka ışık farkı +0.2 dB, yani ölçüm gürültüsü seviyesinde.

→ **Planın en büyük riski (kart içi mikrofon SNR'ı) kapandı.** Harici I2S
mikrofon ihtimali masadan kalktı; boş GPIO'lar (12–19, 41–47) yedekte duruyor.

Dosyalar: [`src/hal/audio_i2s.c`](src/hal/audio_i2s.c), [`.pio`](src/hal/audio_i2s.pio),
[`src/hal/es8311.c`](src/hal/es8311.c), [`src/hal/i2c_bus.c`](src/hal/i2c_bus.c)

---

## 5. Denenip İŞE YARAMAYANLAR — tekrar denemeyin

Bu bölüm zaman kazandırmak için var.

### 5.1 SDK'nın kendi derlediği picotool segfault ediyor

Pico SDK, sistemde uygun sürüm bulamazsa picotool'u kaynaktan derliyor. Bu
makinedeki host derleyicisiyle (winlibs GCC 16.1) derlenen picotool 2.3.0
**dosya okuyan her komutta** çöküyor: `uf2 convert`, `info`, `coprodis`.
`version` komutu çalışıyor, yani sorun ELF/dosya işleme yolunda.

Belirti: derleme `Access violation` / `[code=3221225477]` ile duruyor.

**Çözüm:** [`cmake/picotool.cmake`](cmake/picotool.cmake) — PlatformIO ile gelen
önceden derlenmiş picotool 2.0.0'ı SDK'ya `IMPORTED` hedef olarak tanıtıyor.
SDK'nın `Findpicotool.cmake` dosyası `if (NOT TARGET picotool)` ile başladığı
için `pico_sdk_init()` öncesinde hedefi tanımlamak indirme/derleme adımını
tamamen atlatıyor. picotool 2.0.0 hem `uf2 convert` hem `coprodis` yapıyor.

> Denenmiş ama gereksiz: `PICO_NO_COPRO_DIS=1`. Sadece `coprodis` adımını
> atlatıyor, `uf2 convert` yine çöküyordu.

### 5.2 PIO state machine'i her yakalamada yeniden başlatmak

**En çok zaman kaybettiren hata.** Ölçümlerin rastgele bir kısmı tamamen
gürültü veriyordu: RMS ~19000, tepe tam ölçek (32768), DC oynak.

Teşhis: ham örnekler çözümlendi. `|x| > 16384` oranı tam **%50.1**, ardışık
örnek farkının RMS'i ≈ √2 × RMS → örnekler tamamen ilintisiz, 16 bitin tamamı
rastgele. Her olası bit kaydırması (`<<1..3`, `>>1..3`) denendi, hiçbiri yapıyı
geri getirmedi → **basit bit kayması değil**, hattı hiç okumuyoruz.

Sebep: her yakalamada `pio_sm_set_enabled(false)` + `pio_sm_restart()` + tekrar
enable. Bu, I2S çerçeve kilidini her seferinde yeniden kurmaya zorluyor. LRCK ve
BCLK kenarları neredeyse aynı anda değiştiği için yeniden kilitlenme bir yarışa
dönüşüyor ve kilit bazen yanlış yuvaya oturuyor.

**Çözüm:** RX state machine `pb_audio_i2s_init()` içinde **bir kez** başlatılıyor
ve bir daha durdurulmuyor. Yakalama öncesi sadece FIFO boşaltılıyor. Kilit bir kez
doğru kurulunca kendiliğinden korunuyor (program her çerçevede LRCK düşen
kenarında yeniden hizalanıyor).

> Kısmen yardımcı olan ama tek başına yetmeyen düzeltme: `.pio` içinde LRCK
> kenarından sonra doğrudan `wait 1 gpio 4` yerine önce `wait 0 gpio 4` yapmak.
> WAIT komutu kenar değil **seviye** tetikli; BCLK zaten yüksekse anında geçiyordu.
> Bu düzeltme korundu ama asıl sorun yeniden başlatmaydı.

### 5.3 `getchar_timeout_us(0)` bloklamıyor

Komut döngüsü boşa dönüp ekranı "bilinmeyen komut" ile dolduruyordu. `0`
"beklemeden zaman aşımı" demek. Bloklayan okuma için düz `getchar()`.

### 5.4 `fflush(stdout)` bağlanmıyor

Newlib'in tüm stdio kilit makinesini çekiyor (`__retarget_lock_acquire_recursive`
vb. tanımsız), Pico SDK'nın minimal printf'i onları sağlamıyor. SDK'nın kendi
`stdio_flush()` fonksiyonu kullanılmalı.

### 5.5 PC hoparlöründen bip çalarak akustik test — GEÇERSİZ

`[Console]::Beep` ile test yapıldı, RMS 588 → 1393 çıktı ve "mikrofon çalışıyor"
sanıldı. **Ama bilgisayarda kulaklık takılıydı, hoparlörden ses çıkmadı.**
Görülen artış odadaki başka bir sesti. Sonuç atıldı.

Doğru test: kullanıcının el çırpması (yapıldı, geçti).

### 5.6 Ev dizini kazara bir git deposu

`C:\Users\hp` bir git deposu (commit'i yok, muhtemelen kazara). `git add -A`
oradan çalıştırılırsa tüm ev dizinini taramaya kalkıyor ve zaman aşımına uğruyor.
**pokebird artık kendi deposu** (`git init`, `main` dalı).

### 5.7 Düz RMS ile gürültü tabanı ölçmek yanıltıcı

Oda hiçbir zaman tam sessiz değil; tek bir kapı sesi düz RMS'i yukarı çekiyor
(kazanç taramasında tepe 18666 ve 32768 görüldü). Gürültü tabanı artık kısa
pencere RMS'lerinin **10. yüzdeliği** olarak hesaplanıyor.

---

## 6. Açık konular / borçlar

| Konu | Durum |
|---|---|
| **es8311.c lisansı** | "ESPRESSIF MIT License" standart MIT DEĞİL; kullanımı Espressif ürünleriyle sınırlı, bizimki RP2350. Kişisel kullanımda pratik sorun yok. **Dağıtım/ticarileştirme öncesi** veri sayfasından kendi sürücümüz yazılmalı (~20 register + MCLK bölücü tablosu). Dosya başında belgelendi. |
| **BirdNET lisansı** | CC BY-NC-SA 4.0. Damıtılan model türev sayılabilir → ticari kullanımı kısıtlar. Ticari yol için damıtmasız (yalnızca segmentasyon) varyant gerekir. Bkz. ARCHITECTURE §6. |
| Mikrofon kazancı | Şu an 3 (varsayılan). Saha koşullarında kalibre edilmeli. |
| `pb_audio_capture` bloklayan | M3'te çift tamponlu sürekli yakalamaya (ping-pong DMA) dönecek. |
| SD kart | Hiç dokunulmadı. M2 veya M7'de. |

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
hesaplanıp 64×187 halka tamponda tutuluyor (int8 → 12 KB). Ham ses için sadece
~0.5 s halka. ~130 KB tasarruf.

**Model piramidi:**
| Aşama | Görev | Boyut |
|---|---|---|
| 0 Kapı | enerji + spektral akı | ~0 (DSP) |
| 1 İkili | kuş mu değil mi | ~15 KB int8 |
| 2 Tür | 110 tür + bilinmiyor | ~300–400 KB int8 |
| 3 Birleştirme | zamansal oylama + mevsim önceliği | ~2 KB tablo |

Hedef: ≤30 MMAC/pencere, tensor arena ≤180 KB.

**Doğruluk beklentisi:** temiz kayıtlarda top-1 %65–75, top-3 %85–90. Şehir
gürültüsünde düşer. Arayüz bu yüzden tek cevap değil **ilk 3 tahmin** gösterecek.

**Bellek bütçesi (520 KB):** arena 180 + LVGL 26 + mel 12 + ham ses 24 +
DMA 8 + FatFS 10 + tablo 20 + yığın/heap 80 = **~360 KB**, ~160 KB pay.

---

## 8. Yol haritası — nerede kaldık

| # | Aşama | Durum |
|---|---|---|
| M0 | İskelet, derleme zinciri | ✅ |
| M1 | Mikrofon bring-up + SNR | ✅ |
| **M2a** | **Ekran sürücüsü + canlı spektrogram** | **🔶 kod hazır, ekran görsel olarak doğrulanmadı** |
| M2b | LVGL entegrasyonu + dokunmatik | |
| M3 | DSP hattı: mel + kapı, host testleriyle doğrulama | |
| M4 | Veri boru hattı + tür listesi (PC tarafı) | |
| M5 | Model eğitimi + damıtma + INT8 | |
| M6 | TFLM entegrasyonu, gerçek zamanlı çıkarım (core1) | |
| M7 | Sonradan işleme, tarih ekranı, tam arayüz, günlük, pil | |
| M8 | Saha kalibrasyonu | |

### M2a — yapılanlar (kod hazır, görsel doğrulama bekliyor)

Waveshare'in **bu karta ait** LVGL örneğinden alındı
(`files.waveshare.com/wiki/RP2350-Touch-LCD-3.49/RP2350-Touch-LCD-3.49-LVGL.zip`):
`qspi.pio`, `qspi_pio.c`, `LCD_3in49.c` (AXS15231B), `Touch.c`. Satıcı dosyaları
neredeyse dokunulmadan duruyor; bekledikleri semboller
[`src/hal/display/DEV_Config.h`](src/hal/display/DEV_Config.h) uyum katmanından
ve [`dev_config.c`](src/hal/display/dev_config.c)'deki DMA globallerinden geliyor.

- **QSPI pio0'da, ses pio1'de** — state machine çakışması yok
- `src/dsp/fft.c`: geçici radix-2 FFT (M3'te CMSIS-DSP devralacak)
- `src/ui/spectrogram.c`: tek sütun yazan kaydırmalı spektrogram; tam
  framebuffer (220 KB) yerine sütun başına 344 bayt
- Seri komut: `s` → canlı spektrogram

**Doğrulanan:** cihaz açılıyor, `LCD_3IN49_Init()` kilitlenmiyor, `s` komutu
çalışıyor ve cihaz ayakta kalıyor. **Doğrulanmayan:** ekranda gerçekten
doğru görüntü var mı — buna insan gözü gerekiyor. Yön (172×640 dikey paneli
640×172 yatay kullanmak) ilk denemede tutmayabilir.

### Bu arada doğrulanan iki şey

- **Pin haritası bağımsız teyit edildi.** Waveshare'in resmi board başlığı
  (`waveshare_rp2350_touch_lcd_3.49.h`) şematikten çıkardığımız her değerle
  birebir tutuyor: RP2350A=0, I2C1 6/7, flash 16 MB, LCD_BL 36, dokunmatik
  32/33/11 (adres 0x3B), BAT_ADC 40, QSPI CS 25 / SCLK 20 / D0-D3 21-24.
- **PSRAM yok.** Örnekteki `lib/PSRAM/` başka karttan kopyalanmış ölü kod;
  `rp_setup_psram()` hiç çağrılmıyor. 520 KB bütçe geçerli.

> Waveshare örneğinden **kopyalanmayan** hata: `malloc(172*640)` ile 110 KB
> ayırıp LVGL'e 110080 *piksel* (=220 KB) olduğunu söylüyor. Tampon taşması.

### M2b kapsamı

1. **AXS15231B QSPI sürücüsü** — init dizisi Waveshare'in 3.49 LVGL örneğinden
   (`files.waveshare.com/wiki/RP2350-Touch-LCD-3.49/RP2350-Touch-LCD-3.49-LVGL.zip`)
2. **Dokunmatik** — AXS15231B, ayrı I2C0 hattı (GPIO32/33), adres 0x3B, INT GPIO11
3. **LVGL v9**, kısmi render, iki küçük tampon (2 × 640×20 px RGB565 = 26 KB).
   **Tam framebuffer YOK.** `LCD_TE` ile yırtılma önleme.
4. **Canlı spektrogram** — LVGL canvas yerine doğrudan sütun-itme; her karede
   tek piksel sütun QSPI'ye gider.

M2'nin doğrulaması: mikrofondan gelen ses ekranda akıyor.

---

## 9. Depo düzeni

```
boards/     Pico SDK board tanımı
cmake/      toolchain.cmake, picotool.cmake, pico_sdk_import.cmake
docs/       ARCHITECTURE.md (tam plan)
src/
  board_config.h   ← pin haritası, TEK doğruluk kaynağı
  main.c           ← şu an M1 test uygulaması
  hal/             audio_i2s(.c/.h/.pio), es8311(.c/.h), i2c_bus(.c/.h)
tools/      capture_wav.py
third_party/pico-sdk/   (git'e girmiyor)
```

Git: `main` dalı, dört commit (M0 ×2, M1 ×2). Uzak depo yok.
