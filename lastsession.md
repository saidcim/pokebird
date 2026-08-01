# PokeBird — Oturum Devir Teslimi

> Bu dosya, yeni bir Claude oturumunun projeyi sıfırdan anlayıp kaldığı yerden
> devam edebilmesi için yazıldı. Mimari planın tamamı [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
> içinde; burada onun özeti, şu ana kadar yapılanlar, **denenip işe yaramayanlar**
> ve sıradaki adımlar var.
>
> Son güncelleme: 1 Ağustos 2026.

## ⚠ ÖNCE BUNU OKUYUN

**Durum:** M0 ✅ · M1 ✅ · M2a ✅ · M2b 🔶 (dokunmatik park edildi) · M3 🔶

Çalışma ağacı temiz, her şey commit edildi (§10).

**Sıradaki iş — M3'ün kalanı:** `pb_audio_capture` bloklayan olduğu için DSP
hattı gerçek zamanın **%91'inde** koşuyor (57 kare/s, olması gereken 62.5) ve
kare kaçırıyor. Çift tamponlu sürekli yakalamaya (ping-pong DMA) geçilmeli.
Model eklenmeden çözülmeli, yoksa gerçek zamanlı bütçe baştan açık verir.
Ayrıntı §9c.

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

```bash
python tools/capture_wav.py --port COM13 --cmd m
python tools/capture_wav.py --port COM13 --out a.wav   # kayit al
```

`d`, `b`, `v`, `t`, `u` **etkileşimli**: araç çıktıyı canlı akıtır ve klavyeyi
cihaza iletir. Diğerleri toplu okur.

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

---

## 6. Açık konular / borçlar

| Konu | Durum |
|---|---|
| **`pb_audio_capture` bloklayan** | **M3'ün kalan işi.** Hat gerçek zamanın %91'inde koşuyor, kare kaçıyor. Ping-pong DMA'ya geçilmeli. Bkz. §9c. |
| **EMI ölçümü geçersiz** | M1'deki tarama PWM ile yapıldı, ışık hep kapalıydı. `e` komutu aç/kapa olarak düzeltilip yeniden ölçülmeli (§4). |
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

### Şu anki gerçek kullanım

```
text 449 KB (flash, 16 MB'de sorun değil)
bss  189 KB (520 KB SRAM'de)
```

bss'in içinde `s_capture` (M1 test tamponu, ~96 KB) var — ping-pong DMA'ya
geçilince kalkacak. TFLM arena'sı (180 KB) ondan sonra gelecek. **Arena
eklenmeden önce s_capture'ın kalkması gerekiyor**, yoksa bütçe taşar.

---

## 8. Yol haritası

| # | Aşama | Durum |
|---|---|---|
| M0 | İskelet, derleme zinciri | ✅ |
| M1 | Mikrofon bring-up + SNR | ✅ |
| M2a | Ekran sürücüsü + canlı spektrogram | ✅ (§9a) |
| **M2b** | **LVGL entegrasyonu + dokunmatik** | **🔶 LVGL çalışıyor; dokunmatik park, TE yapılmadı (§9b)** |
| **M3** | **DSP hattı: mel + kapı** | **🔶 mel+kapı doğrulandı; ping-pong DMA kaldı (§9c)** |
| M4 | Veri boru hattı + tür listesi (PC tarafı) | |
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
| 6 | Spektrogramın LVGL ile birlikte yaşaması | ⏳ yapılmadı |

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

## 9c. M3 — mel + kapı (BURADA KALDIK)

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

### ⚠ KALAN İŞ: kare hızı %9 eksik

Ölçülen ~57 kare/s, olması gereken 62.5 (hop 384 @ 24 kHz). Hat gerçek zamanın
%91'inde koşuyor, kareler kaçıyor. Sebep `pb_audio_capture`'ın bloklayan olması.

**Yapılacak:** çift tamponlu sürekli yakalama (ping-pong DMA). Yan faydası:
`s_capture` (~96 KB) kalkar ve TFLM arena'sına (180 KB) yer açılır — arena
eklenmeden bu şart.

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
tools/      capture_wav.py, mel_reference.py
third_party/  pico-sdk/, lvgl/              (ikisi de git'e girmiyor)
rsvpnano/     kullanıcının kopyası           (git'e girmiyor)
```

### Git

Dal `main`, uzak depo yok, çalışma ağacı temiz. Bu oturumun commit'leri
(eskiden yeniye):

| Commit | Ne |
|---|---|
| `7273768` | **Ekran çalışıyor: CS, PIO veriyi hatta çıkarmadan yükseliyordu** (§5.9) |
| `68ba8b2` | Spektrogram: frekans ekseni yönü kartta ölçülüp düzeltildi |
| `768cda9` | **M3: mel öznitelik hattı + kapı, host testleriyle doğrulandı** |
| `9e7e2dc` | Dokunmatik: zaman aşımlı kendi sürücümüz, satıcı sürücüsü kaldırıldı |
| `30c6ac9` | M2b: LVGL v9.3 entegrasyonu, 90° yön çevrimi ek tampon olmadan |
| `cfcc0c4` | Teşhis komutları, derleme hedefleri ve etkileşimli araç kipi |

Sıralama **her commit derlenebilir kalsın** diye seçildi: DSP dosyaları
CMakeLists'e eklenmeden önce commit'lendiği için ara commit'lerde derlemeye
girmiyorlar ve firmware bozulmuyor. `CMakeLists.txt` ve `main.c` tek parça
halinde son commit'te — birbirine bağlı oldukları için etkileşimli staging
olmadan bölünemezlerdi.

### rsvpnano referansı

Kullanıcının bu panelde çalıştırdığı kitap okuma uygulaması (ESP32-S3/Arduino).
İki kopya: `C:\Users\hp\rsvpnano` (asıl) ve `pokebird/rsvpnano/` (gitignore'da).

Değerli dosyalar:
- `src/display/axs15231b.cpp` — panelin asgari init dizisi + arka ışık
- `src/input/TouchHandler.cpp` — dokunmatik protokolü (8 baytlık okuma buradan)

Satıcı LVGL örneği ayrıca `C:\Users\hp\AppData\Local\Temp\lcd\ext\` altına
açılmıştı (geçici dizin, silinmiş olabilir).
