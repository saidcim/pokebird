# PokeBird

Waveshare **RP2350-Touch-LCD-3.49** üzerinde çalışan, internet gerektirmeyen kuş sesi tür tanıma cihazı. İstanbul'da yaşayan ~110 kuş türüne odaklanır.

Tam mimari planı: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)

## Durum

| Aşama | Durum |
|---|---|
| **M0** — proje iskeleti, derleme zinciri | ✅ tamam |
| M1 — mikrofon bring-up + SNR ölçümü | sırada |
| M2 — ekran + LVGL + canlı spektrogram | |
| M3 — DSP hattı (mel + kapı) | |
| M4 — veri boru hattı, tür listesi | |
| M5 — model eğitimi + damıtma + INT8 | |
| M6 — TFLM entegrasyonu, gerçek zamanlı çıkarım | |
| M7 — sonradan işleme, tam arayüz | |
| M8 — saha kalibrasyonu | |

## Donanım

| | |
|---|---|
| MCU | RP2350**B** — 2× Cortex-M33 @ 150 MHz, FPU + DSP |
| Bellek | 520 KB SRAM (**PSRAM yok**), 16 MB flash |
| Ekran | 172×640 IPS, AXS15231B, QSPI + dokunmatik |
| Ses | ES8311 codec, kart üzerinde analog MEMS mikrofon, NS4150B hoparlör amfisi |
| Depolama | microSD |

Pin haritası [`src/board_config.h`](src/board_config.h) içinde ve Waveshare'in resmi şemasından net-net doğrulanmıştır. **Donanımla ilgili her sayı oradan alınır.**

## Derleme

Gereksinimler (üçü de büyük olasılıkla zaten kurulu):

- CMake ≥ 3.13 ve Ninja
- ARM GCC — PlatformIO'nun `toolchain-rp2040-earlephilhower` paketi (GCC 14.3) otomatik bulunur
- Pico SDK — `third_party/pico-sdk` altına klonlanır (aşağıya bakın)

```bash
git clone -b 2.3.0 --depth 1 https://github.com/raspberrypi/pico-sdk.git third_party/pico-sdk
git -C third_party/pico-sdk submodule update --init --depth 1 lib/tinyusb
```

Sonra:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

Çıktı: `build/pokebird.uf2`

Yollar [`cmake/toolchain.cmake`](cmake/toolchain.cmake) tarafından otomatik bulunur; ortam değişkeni ayarlamanız gerekmez. Farklı bir kurulumunuz varsa `-DPICO_SDK_PATH=...` veya `-DPICO_TOOLCHAIN_PATH=...` verebilirsiniz.

## Karta yükleme

1. **BOOT** ve **RESET** tuşlarına birlikte basın
2. Önce **RESET**'i, sonra **BOOT**'u bırakın
3. Bilgisayarda çıkan sürücüye `build/pokebird.uf2` dosyasını kopyalayın

Kart yeniden başlar. Çalıştığını iki yerden görürsünüz: ekran arka ışığı nefes alır gibi kısılıp açılır, ve USB seri portta kart bilgileri yazar.

## Bilinen ortam sorunu: picotool

Pico SDK, sistemde uygun sürüm bulamazsa picotool'u kaynaktan derler. Bu makinedeki host derleyicisiyle (winlibs GCC 16.1) derlenen picotool 2.3.0, dosya okuyan her komutta segfault ediyor — `uf2 convert` ve `coprodis` dahil. Derleme "Access violation" ile durur.

[`cmake/picotool.cmake`](cmake/picotool.cmake) bunu, PlatformIO ile gelen önceden derlenmiş picotool'u SDK'ya tanıtarak çözer. Çalışan bir picotool bulamazsa sessizce çekilir ve SDK her zamanki davranışına döner.

## Dizin yapısı

```
boards/     Pico SDK board tanımı (SDK'da RP2350B için hazır başlık yok)
cmake/      Derleme zinciri yardımcıları
src/        Cihaz kodu — board_config.h, hal/, dsp/, ml/, data/, ui/
models/     Niceleştirilmiş INT8 modeller (C dizisi olarak)
tools/      PC tarafı: veri toplama, eğitim, dönüştürme (Python)
test/       Host tarafı DSP birim testleri
```

## Lisans notu

Model eğitiminde BirdNET öğretmen olarak kullanılacak. BirdNET **CC BY-NC-SA 4.0** ile dağıtılıyor; damıtılan model türev eser sayılabilir, bu da **ticari kullanımı kısıtlar**. Kişisel/araştırma kullanımı için sorun yok. Ayrıntı ve ticari yol için mimari planın §6 bölümüne bakın.
