# PokeBird — İstanbul Kuş Sesi Tanıma Cihazı: Mimari Temel

## Context

**Amaç:** Waveshare RP2350-Touch-LCD-3.49 üzerinde, internet bağlantısı olmadan çalışan, kuş sesinden tür tanıyan cep cihazı. BirdNET'in yaptığı işin, tek bir şehre (İstanbul) daraltılmış ve 150 MHz'lik bir mikrodenetleyiciye sığdırılmış hâli.

**Neden daraltma gerekiyor:** BirdNET GLOBAL 6K modeli ~6000 sınıf, 48 kHz girdi ve onlarca MB'lik bir ağ. Bu cihazda 520 KB SRAM ve 150 MHz iki çekirdek var — BirdNET'i olduğu gibi çalıştırmak fiziksel olarak mümkün değil. İstanbul'a özgü ~110 türlük bir liste, hem sınıf sayısını 50 kat düşürüyor hem de sınıf başına düşen eğitim verisini artırdığı için küçük modelin doğruluğunu yükseltiyor.

**Bu doküman ne:** Üzerine inşa edeceğimiz temel. Donanım gerçekleri (şematikten doğrulanmış), bellek/hesap bütçesi, sinyal işleme ve model mimarisi, veri boru hattı, arayüz ve aşamalı teslim planı.

**Proje durumu:** `C:\Users\hp\Downloads\pokebird` boş bir git deposu. Sıfırdan başlıyoruz.

**Kilitlenen kararlar (kullanıcı onayı ile):**
| Karar | Seçim |
|---|---|
| Tür kapsamı | ~110 tür (yerleşik + düzenli üreyen + yaygın göçmen) |
| Kullanım | Elde taşınan, anlık tanıma; ekran sürekli açık |
| Yazılım yığını | Pico SDK + CMake (C/C++) |
| Model eğitimi | BirdNET öğretmen → küçük modele damıtma (distillation) |

---

## 1. Donanım Temeli (şematikten doğrulandı)

Waveshare şeması (`RP2350-Touch-LCD-3.49.pdf`) çözümlenerek çıkarıldı. **Bu tablo projenin `board_config.h` dosyasının kaynağıdır.**

| GPIO | Sinyal | Not |
|---|---|---|
| 0 | `NS_MODE` | NS4150B hoparlör amfi mod/enable |
| 1 | `I2S_DSDIN` | MCU → codec (DAC, ses çalma) |
| 2 | `I2S_DSOUT` | codec → MCU (**ADC, mikrofon verisi**) |
| 3 | `I2S_MCLK` | PIO ile üretilecek |
| 4 | `I2S_SCLK` | BCLK |
| 5 | `I2S_LRCK` | WS |
| 6 / 7 | `SDA` / `SCL` | **Paylaşımlı I2C**: ES8311 codec + QMI8658 IMU + PCF85063 RTC |
| 8 / 9 | `IMU_INT1` / `IMU_INT2` | *kullanılmayacak* |
| 10 | `RTC_INT` | *kullanılmayacak* |
| 11 | `TP_INT` | Dokunmatik kesme |
| **12–19** | **boş** | P3 başlığına çıkmış (8 pin) |
| 20–25 | `LCD_SCL`, `LCD_D0..D3`, `LCD_CS` | AXS15231B QSPI |
| 26–31 | `SD_SCLK`, `SD_MOSI`, `SD_MISO`, `SD_D1`, `SD_D2`, `SD_CS` | 4-bit SDIO da mümkün |
| 32 / 33 | `TP_SDA` / `TP_SCL` | Dokunmatik **ayrı** I2C hattı |
| 34 / 35 / 36 / 37 | `LCD_RST`, `LCD_TE`, `LCD_BL`, `BL_EN` | |
| 38 / 39 | `SYS_OUT` / `SYS_EN` | Güç mandalı (latch) |
| 40 | `BAT_ADC` | Pil voltajı |
| **41–47** | **boş** | J3/J6 başlığı (7 pin) |

**Kritik donanım gerçekleri:**
- **RP2350B**: 2× Cortex-M33 @ 150 MHz, FPU + DSP eklentisi (CMSIS-DSP/NN kullanılabilir).
- **520 KB SRAM, 16 MB flash (PY25Q128HA). PSRAM YOK.** — En sert kısıt bu.
- **Mikrofon kart üzerinde var**: analog MEMS mikrofon (`MIC1`) → ES8311 `MIC1P/MIC1N` girişi. Harici mikrofon eklemeye gerek yok.
- **Hoparlör çıkışı var**: NS4150B D-sınıfı amfi + MX1.25 konnektör (H1). Referans kuş sesi çalma özelliği mümkün.
- **Kasıtlı olarak kullanılmayan çevre birimleri:** IMU (QMI8658) ve RTC (PCF85063). IMU'nun bu projeye katkısı yok, sadece karmaşıklık ekler. RTC'nin pil yedeklemesi yok — cihaz kapanınca saati kaybediyor, dolayısıyla güvenilir bir tarih kaynağı değil (tarih yönetimi için bkz. §4). Ekranı uyandırma/uyutma işi kartın kendi güç tuşuyla yapılacak.
- **Ekran 172×640** — dar ve uzun. Yatay çevrildiğinde 640×172'lik bir "şerit". Tam çerçeve arabelleği RGB565'te **220 KB** eder; SRAM'in %42'si. **Tam framebuffer kullanmayacağız** (bkz. §5).

**Yeniden kullanılacak hazır kod (sıfırdan yazmayacağız):**
`github.com/waveshareteam/RP2350-Touch-LCD-3.5` → `examples/C/03_ES8311/` altında çalışan bir **ES8311 sürücüsü + PIO tabanlı I2S** var; içinde `read_pio` (mikrofon yakalama), `mclk_pio` (MCLK üretimi), mikrofon kazanç ayarı ve 16-bit/24 kHz yapılandırma bulunuyor. Kardeş karta ait olduğu için **sadece pin tanımları değişecek** (`PICO_AUDIO_*` makroları → yukarıdaki GPIO 1–5). Bu, projenin en riskli parçasını hazır çözüyor.

---

## 2. Sistem Mimarisi

```
┌─ Core 1 (ses + yapay zeka, gerçek zamanlı) ──────────────────┐
│  I2S/PIO+DMA → halka tampon → mel öznitelik çıkarımı         │
│      → Kapı (VAD)  → Aşama-1 ikili ağ  → Aşama-2 tür ağı     │
│      → zamansal birleştirme + mevsim önceliği                │
└──────────────────── FIFO (tespit olayları) ──────────────────┘
┌─ Core 0 (arayüz + depolama, gerçek zamanlı değil) ───────────┐
│  LVGL → AXS15231B QSPI+DMA │ dokunmatik │ SD kart │ pil │ güç │
└──────────────────────────────────────────────────────────────┘
```

**Neden bu ayrım:** Ses yakalama bir örnek bile kaçıramaz. LVGL'in çizim döngüsü ve SD kart yazma işlemleri onlarca milisaniye bloklayabilir. İki çekirdeği ayırmak, arayüzün ses hattını asla aksatmamasını garantiler. Çekirdekler arası tek yön: Core 1 → Core 0, sadece tespit olayları (küçük struct'lar) `pico_multicore` FIFO + kilitsiz halka tampon ile.

**Katmanlar:**
| Katman | Sorumluluk |
|---|---|
| `hal/` | Pico SDK sarmalayıcıları: i2s, i2c, qspi_lcd, touch, sd, power |
| `dsp/` | Halka tampon, pencereleme, RFFT, mel filtre bankası, log + normalizasyon |
| `ml/` | TFLM çalıştırıcı, kapı mantığı, iki aşamalı çıkarım, sonradan işleme |
| `data/` | Tür tablosu, mevsim öncelikleri, tespit günlüğü, yaşam listesi |
| `ui/` | LVGL ekranları, tema, Türkçe yazı tipi |
| `app/` | Görev döngüleri, durum makinesi, ayarların kalıcılığı |

---

## 3. Ses ve Sinyal İşleme Hattı

**Örnekleme hızı: 24 kHz** (16 kHz değil). Gerekçe: 16 kHz → 8 kHz Nyquist, İstanbul'da bulunan Çalıkuşu (*Regulus regulus*), Ağaç Tırmaşıkkuşu (*Certhia*) ve bazı ötleğenlerin sesi 7–9 kHz bandında; 8 kHz tavan bunları kırpar. 24 kHz → 12 kHz Nyquist tümünü kapsar. Ayrıca Waveshare sürücüsünün varsayılanı da 24 kHz — ekstra iş yok.

| Parametre | Değer |
|---|---|
| Örnekleme | 24 kHz, 16-bit, mono |
| Analiz penceresi | 3.0 s, 1.0 s adımla (%66 örtüşme) |
| FFT | 512 nokta, Hann |
| Adım (hop) | 384 örnek (16 ms) → 3 s'de 187 kare |
| Mel bantları | 64, aralık 150 Hz – 11.5 kHz |
| Çıktı | log-mel, pencere içi ortalama/varyans normalizasyonu, int8 |

**Kritik bellek hilesi:** 3 saniyelik ham sesi tutmayacağız (144 KB olurdu). Mel kareleri **artımlı** hesaplanıp 64×187'lik bir halka tamponda tutulacak (int8 → 12 KB). Ham ses için sadece ~0.5 s'lik kısa bir halka (24 KB) tutulur — bu da isteğe bağlı WAV kaydı ve gürültü tahmini içindir. Bu tek karar SRAM'de ~130 KB kazandırıyor.

**Kapı (gate) — sürekli çalışan ucuz filtre:** 2–10 kHz bandındaki enerji + spektral akı (flux), uyarlamalı gürültü tabanına göre. Şehirde cihazın çoğu zaman "hiçbir şey yok" durumunda olacağı için bu kapı, ağır işlemin %90+ oranında hiç çalışmamasını sağlar → pil ömrü.

---

## 4. Model Mimarisi: Üç Aşamalı Piramit

Tek büyük 110-sınıflı ağ yerine kademeli yapı. Gerekçe: DrongoNet çalışmasının ([arXiv 2607.19721](https://arxiv.org/html/2607.19721)) temel bulgusu, "ses var mı" ile "hangi tür" sorularının maliyetlerinin çok farklı olduğu — ucuz olanı önce sormak, hem doğruluğu hem pil ömrünü artırıyor.

| Aşama | Görev | Boyut | Ne zaman çalışır |
|---|---|---|---|
| **0. Kapı** | Enerji + spektral akı | ~0 KB (DSP) | Sürekli |
| **1. İkili ağ** | Kuş sesi mi, değil mi | ~15 KB int8 | Kapı tetiklenince |
| **2. Tür ağı** | 110 tür + "bilinmiyor" | ~300–400 KB int8 | Aşama 1 "kuş" derse |
| **3. Birleştirme** | Zamansal oylama + mevsim önceliği | ~2 KB tablo | Her tespitte |

**Aşama 2 ağı:** derinlemesine ayrılabilir (depthwise-separable) evrişimli CNN — daraltılmış MobileNet mantığı. Girdi 64×187 int8. Hedef bütçe: **≤30 MMAC/pencere**. CMSIS-NN ile M33 @150 MHz'de bu ~0.3–0.5 s eder; 1 s'lik pencere adımına rahat sığar.

**Aşama 3 — mevsim önceliği (küçük iş, büyük kazanç):** eBird'den İstanbul'un aylık tür görülme oranları çıkarılıp 110×12'lik bir tabloya (1.3 KB) niceleştirilir. Logit'lere ay bazlı bir log-öncelik eklenir; böylece Ocak ayında Arı Kuşu tahmini otomatik olarak bastırılır. **Önemli koruma:** öncelik etkisi bir tavanla sınırlanır (ör. ±2.0 logit) ve ayarlardan kapatılabilir — yoksa cihaz gerçek bir nadir gözlemi asla raporlayamaz.

**Tarih nereden geliyor — RTC kullanmıyoruz.** Karttaki PCF85063 RTC'nin pil yedeklemesi olmadığı için cihaz kapanınca saati kaybediyor; güvenilmez bir kaynak. Bunun yerine:
- Açılışta tek ekranlık bir **tarih onay** adımı: LittleFS'te saklanan son bilinen tarih varsayılan olarak gelir, kullanıcı ya "Onayla"ya basar ya da tekerlekten değiştirir. Günlük kullanımda tek dokunuş.
- Öncelik için **sadece ay** gerekiyor — gün/saat hassasiyeti önemsiz, dolayısıyla bir-iki gün kayma hiçbir şeyi bozmaz.
- Oturum içi saat, açılıştan itibaren MCU zamanlayıcısıyla sayılır (günlük kayıtlarındaki saat damgaları için yeterli). Kapanışta güncel tarih LittleFS'e yazılır.
- Kullanıcı isterse tarihi hiç girmeden geçebilir → mevsim önceliği o oturum için devre dışı kalır, cihaz yine çalışır.

**Zamansal birleştirme:** softmax'ın üstel hareketli ortalaması + "son n pencereden k tanesi eşiğin üstünde" kuralı. Tek pencerelik gürültü kaynaklı yanlış pozitifleri büyük ölçüde eler.

**Gerçekçi doğruluk beklentisi (baştan söylemek önemli):** Temiz tek-tür kayıtlarında top-1 %65–75, top-3 %85–90 bandı hedeflenebilir. Gerçek şehir ortamında (trafik, ezan, martı gürültüsü, insan sesi) bu belirgin düşer. Bu yüzden arayüz **tek bir cevap değil, ilk 3 tahmini güven yüzdeleriyle** gösterecek ve düşük güvende açıkça "emin değil" diyecek. Bunu bir kusur değil, tasarım kararı olarak ele alıyoruz.

---

## 5. Bellek Bütçesi (520 KB SRAM)

| Bileşen | Tahmini |
|---|---|
| TFLM tensor arena (Aşama 2) | 180 KB |
| LVGL çizim tamponları (2 × 640×20 px RGB565) | 26 KB |
| Mel halka tamponu | 12 KB |
| Ham ses halka tamponu (0.5 s) | 24 KB |
| I2S DMA tamponları | 8 KB |
| FatFS + SD tamponları | 10 KB |
| Tür tablosu + günlük + ayarlar | 20 KB |
| Yığınlar (2 çekirdek) + heap + LVGL nesneleri | 80 KB |
| **Toplam** | **~360 KB** — ~160 KB pay kalıyor |

**Ekran stratejisi (tam framebuffer YOK):** LVGL, kısmi (partial) render modunda iki küçük tamponla çalışacak; çizim biterken QSPI'ye DMA ile aktarılırken diğer tampon doldurulacak. `LCD_TE` (tearing effect) sinyali yırtılmayı önlemek için kullanılacak. Kaydırmalı spektrogram, LVGL canvas yerine doğrudan bir sütun-itme (column-push) rutiniyle çizilecek — her karede sadece 1 piksellik yeni sütun QSPI'ye gider, bu çok ucuz.

**Flash yerleşimi (16 MB):**
| Bölge | Boyut |
|---|---|
| Firmware | ~900 KB |
| Modeller (Aşama 1 + 2) | ~450 KB |
| Tür meta verisi (TR/EN/Latince ad, aylık öncelik) | ~60 KB |
| Türkçe yazı tipi + ikonlar | ~400 KB |
| LittleFS (ayarlar, son bilinen tarih, yaşam listesi, günlük) | 2 MB |
| Boş / OTA payı | ~12 MB |

**SD kart (opsiyonel ama önerilir):** kuş fotoğrafları, referans ses kayıtları (hoparlörden çalmak için), ham WAV kayıt arşivi, CSV tespit günlüğü. Cihaz SD olmadan da tam çalışır — sadece fotoğraf/ses çalma devre dışı kalır.

---

## 6. Veri ve Eğitim Boru Hattı (PC tarafı)

`tools/` altında Python; cihaz koduna hiç karışmaz.

1. **Tür listesi kesinleştirme** — eBird İstanbul (TR-34) taksonu + Avibase kontrol listesi kesiştirilir; ses çıkarmayan/ayırt edilemeyen ve Xeno-canto'da <30 kaydı olan türler elenir → nihai ~110 tür. Çıktı: `data/species_istanbul.csv` (bilimsel ad, Türkçe ad, İngilizce ad, eBird kodu).
2. **Kayıt toplama** — Xeno-canto API'sinden CC lisanslı kayıtlar indirilir; kalite A/B önceliklendirilir.
3. **BirdNET ile otomatik segmentasyon + yumuşak etiketleme** — BirdNET-Analyzer her kayıt üzerinde çalıştırılır; hangi 3 saniyelik dilimde gerçekten hedef tür var, bu tespit edilir (Xeno-canto kayıtlarının büyük kısmı sessizlik ve arka plan türü içerir — bu adım veri kalitesini dramatik biçimde artırır). BirdNET'in çıkış olasılıkları öğretmen sinyali olarak saklanır.
4. **Negatif madenciliği (İstanbul'a özgü, kritik)** — trafik, korna, ezan, vapur düdüğü, insan konuşması, köpek/kedi, rüzgâr, yağmur, inşaat. Bunlar hem Aşama 1'in negatif sınıfı hem de Aşama 2'nin "bilinmiyor" sınıfı olur. **Bu adım atlanırsa cihaz sahada kullanılamaz** — şehir gürültüsünü sürekli kuş sanar.
5. **Veri artırma** — zaman kaydırma, pitch/tempo, gürültü karıştırma (yukarıdaki negatiflerle, çeşitli SNR'lerde), SpecAugment, oda/mesafe simülasyonu.
6. **Damıtma ile eğitim** — küçük CNN, hem gerçek etiketlerle hem BirdNET'in yumuşak çıktılarıyla eğitilir. Sınıf dengesizliği için focal loss.
7. **Niceleştirme (INT8) + doğrulama** — TFLite post-training quantization, temsilî veri kümesiyle. Niceleştirme öncesi/sonrası doğruluk farkı raporlanır.
8. **C dizisine dönüştürme** — `xxd -i` benzeri; `models/` altına.
9. **Cihaz-içi doğrulama seti** — SD karttan WAV çalıp cihazın kendi çıktısını PC'deki referansla karşılaştıran bir test modu. Bu, "PC'de çalışıyor ama cihazda çalışmıyor" sınıfı hataları yakalar.

**Lisans uyarısı (şimdiden bilinmeli):** BirdNET modelleri **CC BY-NC-SA 4.0** ile dağıtılıyor. BirdNET'i öğretmen olarak kullanıp damıtılan model, türev eser sayılabilir — bu da **ticari kullanımı kısıtlar** ve aynı lisansla paylaşım (share-alike) yükümlülüğü doğurabilir. Kişisel/hobi/araştırma kullanımı için sorun yok. İleride ticarileştirme düşünülüyorsa, adım 3'ün yalnızca segmentasyon için kullanılıp yumuşak etiketlerin (damıtmanın) atlandığı bir varyant gerekir. Xeno-canto kayıtları da kayıt bazında farklı CC lisanslarına sahip; atıf dosyası (`ATTRIBUTION.md`) otomatik üretilecek.

---

## 7. Arayüz Tasarımı (640×172 yatay)

Ekran alışılmadık bir şerit — bunu kusur değil, avantaj olarak kullanacağız: solda sabit "kimlik kartı", sağda akan spektrogram.

**Ekran 1 — Dinleme (ana ekran)**
```
┌────────────────────┬───────────────────────────────────────┐
│  KIZILGERDAN       │  ▁▂▅█▇▄▂▁  (kaydırmalı spektrogram)   │
│  Erithacus rub.    │                                        │
│  ● %87   14:32     │  ▬▬▬▬▬▬▬░░░  seviye                   │
│  2. Serçe %6       │                                        │
└────────────────────┴───────────────────────────────────────┘
```
Güven düşükse kart "Dinliyor…" veya "Emin değil — 3 aday" durumuna geçer.

**Ekran 2 — Tespit detayı:** SD'den kuş fotoğrafı, TR/EN/Latince ad, güven, saat, "▶ Referans sesi çal" düğmesi (hoparlör). *Not: çalma sırasında dinleme duraklatılır — yoksa cihaz kendi sesini duyar.*

**Ekran 3 — Günlük:** bugünün tespitleri (saat + tür + güven), gün listesi / yaşam listesi sekmeleri, SD'ye CSV dışa aktarma.

**Ekran 4 — Ayarlar:** hassasiyet eşiği, mevsim önceliği aç/kapa, mikrofon kazancı, ekran parlaklığı, dil (TR/EN), ham WAV kaydı aç/kapa, tarih.

**Ekran 0 — Tarih onayı (açılışta):** Tek satır: `Bugün: 31 Temmuz 2026` + [Onayla] [Değiştir] [Atla]. Varsayılan, LittleFS'teki son bilinen tarih. Amaç mevsim önceliği (§4); "Atla" denirse öncelik o oturumda devre dışı. Günlük kullanımda tek dokunuşluk bir adım.

Ekranlar arası geçiş dokunmatik yatay kaydırma ile. Ekranı açma/kapama kartın kendi güç tuşuyla — ek bir uyandırma mekanizması yok.

---

## 8. Depo Yapısı

```
pokebird/
├─ CMakeLists.txt              # PICO_BOARD=pico2, PICO_PLATFORM=rp2350
├─ src/
│  ├─ main.c                   # core0 giriş; core1'i başlatır
│  ├─ board_config.h           # §1'deki pin tablosu — TEK doğruluk kaynağı
│  ├─ hal/                     # i2s_mic, es8311, axs15231b, touch, sdcard, rtc, power
│  ├─ dsp/                     # ringbuf, window, mel, gate
│  ├─ ml/                      # tflm_runner, pipeline, prior, aggregate
│  ├─ data/                    # species_table, detection_log, settings
│  └─ ui/                      # screen_date, screen_listen, screen_detail, screen_log, screen_settings, theme
├─ models/                     # gate_int8.c/h, species_int8.c/h
├─ assets/                     # fonts (TR karakter setli), icons
├─ third_party/                # pico-tflmicro, CMSIS-DSP/NN, lvgl, FatFS
├─ tools/                      # Python: veri toplama, eğitim, niceleştirme, dönüştürme
└─ test/                       # host tarafı DSP birim testleri + cihaz-içi WAV testi
```

---

## 9. Aşamalı Teslim

Her aşama kendi başına doğrulanabilir. **Sıralama kasıtlı: en riskli parça (mikrofon) en başta.**

| # | Aşama | Çıktı / Doğrulama |
|---|---|---|
| **M0** | İskelet: CMake + Pico SDK, LED yanıp söner, USB seri log | Kart programlanabiliyor |
| **M1** | **Mikrofon bring-up** (Waveshare ES8311 örneği uyarlanır) | 5 s ses kaydedip SD'ye WAV yazar; PC'de dinlenip SNR ölçülür. **Kart içi EMI/ekran gürültüsü burada ölçülür.** |
| **M2** | Ekran + dokunmatik + LVGL, kısmi render, canlı spektrogram | Mikrofondan gelen ses ekranda akıyor |
| **M3** | DSP hattı: mel + kapı, host tarafı testlerle bit-uyumluluk | Cihazdaki mel, Python'daki mel ile ≈aynı |
| **M4** | Veri boru hattı + tür listesi kesinleştirme (PC) | `species_istanbul.csv` + indirilmiş/segmentlenmiş veri kümesi |
| **M5** | Model eğitimi + damıtma + INT8 + PC'de doğruluk raporu | Karışıklık matrisi, top-1/top-3 metrikleri |
| **M6** | TFLM entegrasyonu, iki aşamalı çıkarım, core1'de | Cihaz gerçek zamanlı tür söylüyor; gecikme ölçülüyor |
| **M7** | Sonradan işleme (öncelik + oylama), tarih onay ekranı, tam arayüz, günlük, pil | Sahada kullanılabilir cihaz |
| **M8** | Saha kalibrasyonu: gerçek İstanbul kayıtlarıyla eşik ayarı | Yanlış pozitif oranı kabul edilebilir |

---

## 10. Riskler ve Karşı Önlemler

| Risk | Etki | Önlem |
|---|---|---|
| **Kart içi mikrofon SNR'ı kötü** (ekran/QSPI/anahtarlamalı güç kaynağı gürültüsü) | Projeyi bitirebilir | **M1'de ölç.** Kötüyse: LCD parlaklık PWM frekansını kaydır, dinleme sırasında ekran yenilemeyi azalt, son çare olarak boş GPIO'lardan (12–19) harici I2S MEMS mikrofon (INMP441/ICS-43434) ekle — kart bunu destekliyor |
| ES8311 pinleri 3.5 kartından farklı | Orta | `board_config.h`'de tek noktadan tanımlı; §1 tablosu şematikten doğrulandı |
| MCLK/master-slave saat yapılandırması tutmaz | Orta | ES8311 SCLK'yi MCLK olarak kullanabilir (register seçeneği); PIO MCLK zaten Waveshare örneğinde çalışıyor |
| 110 sınıfta doğruluk beklentinin altında | Yüksek olasılık | Arayüz baştan ilk-3 gösterecek; kademeli düşüş: listeyi 60'a indirip yeniden eğitmek sadece model + CSV değişikliği, kod değişmez |
| Bazı türlerin sesi neredeyse ayırt edilemez (ör. bazı ötleğenler, martı türleri) | Orta | Bu türler "tür grubu" olarak birleştirilir (ör. "Gümüş/Karabaş martı grubu") — yanlış kesinlik vermekten iyidir |
| Tensor arena SRAM'e sığmaz | Orta | Model mimarisi bütçeye göre tasarlanıyor (§4); erken stride-2 ile aktivasyonlar küçük tutulur; M5'te arena boyutu ölçülüp M6 öncesi doğrulanır |
| BirdNET lisansı ticari kullanımı kısıtlar | Düşük (hobi için) | §6'da belgelendi; ticari yol için damıtmasız varyant tanımlı |

---

## 11. Doğrulama Yaklaşımı

- **Host tarafı birim testleri** (`test/`): DSP fonksiyonları (pencereleme, RFFT, mel filtre bankası) PC'de derlenip Python referansına karşı karşılaştırılır. Mikrodenetleyicide DSP hatası ayıklamak çok pahalı — bu testler zaman kazandırır.
- **Cihaz-içi WAV testi:** SD karttaki etiketli doğrulama kliplerini mikrofon yerine hattın girişine besleyen bir test modu. Cihazın çıktısı PC'deki referansla karşılaştırılır → "PC'de çalışıyor, cihazda çalışmıyor" hatalarını yakalar.
- **Gecikme ve bellek ölçümü:** her aşamanın süresi (mel, kapı, aşama 1, aşama 2) USB seri üzerinden raporlanır; TFLM arena kullanımı `arena_used_bytes()` ile loglanır.
- **Saha testi (M8):** Belgrad Ormanı / Validebağ Korusu / Büyükçekmece gibi eBird sıcak noktalarında kayıt + cihaz çıktısı karşılaştırması. Aynı anda telefonla BirdNET çalıştırıp iki cihazın hemfikir olduğu oran ölçülür — pratik ve hızlı bir referans.
- **Pil ömrü:** ekran açık sürekli dinleme senaryosunda ölçülür; hedef ≥4 saat.

---

## Kaynaklar

- [RP2350-Touch-LCD-3.49 — Waveshare Wiki](https://www.waveshare.com/wiki/RP2350-Touch-LCD-3.49) (şematik PDF'i buradan çözümlendi)
- [waveshareteam/RP2350-Touch-LCD-3.5](https://github.com/waveshareteam/RP2350-Touch-LCD-3.5) — yeniden kullanılacak ES8311 + PIO I2S sürücüsü
- [DrongoNet: Ultra-Compact CNN Architectures for Bird Audio Detection on Microcontrollers](https://arxiv.org/html/2607.19721) — kademeli mimari ve bellek bütçesi gerekçesi
- [eBird — İstanbul (TR-34)](https://ebird.org/region/TR-34) — tür listesi ve aylık görülme oranları
- [BirdNET-Analyzer](https://github.com/birdnet-team/BirdNET-Analyzer) — öğretmen model
