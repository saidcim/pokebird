/*
 * pokebird_rp2350b.h — Waveshare RP2350-Touch-LCD-3.49 için Pico SDK board tanımı
 *
 * SDK'da bu kart için hazır bir başlık yok. RP2350B (48 GPIO) varyantını ve
 * 16 MB flash'ı doğru seçebilmek için kendi board dosyamızı tanımlıyoruz.
 *
 * Pin ayrıntıları src/board_config.h içinde; burada yalnızca SDK'nın kendi
 * başlatma kodunun ihtiyaç duyduğu tanımlar var.
 *
 * -----------------------------------------------------
 * NOT: BU BAŞLIK ASSEMBLER TARAFINDAN DA OKUNUR,
 *      SADECE ÖNİŞLEMCİ DİREKTİFİ İÇEREBİLİR.
 * -----------------------------------------------------
 */
#ifndef _BOARDS_POKEBIRD_RP2350B_H
#define _BOARDS_POKEBIRD_RP2350B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// Kart tespiti için
#define POKEBIRD_RP2350B

// --- RP2350 VARYANTI ---
// 0 = RP2350B (QFN-80, 48 GPIO). Bu kart B varyantını kullanıyor; GPIO40+
// (BAT_ADC) yalnızca B'de var, dolayısıyla bu satır zorunlu.
#define PICO_RP2350A 0

// --- FLASH: PY25Q128HA, 128 Mbit = 16 MB ---
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- LED YOK ---
// Karttaki tek LED (LED1) ETA6098 şarj durum LED'i; GPIO'ya bağlı değil,
// yazılımdan kontrol edilemez. PICO_DEFAULT_LED_PIN bilerek tanımlanmadı —
// canlılık göstergesi olarak ekran arka ışığı kullanılıyor (bkz. src/main.c).

// --- UART: hata ayıklama için boş P3 başlığı pinlerine ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 12
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 13
#endif

// --- I2C: ES8311 codec kontrol hattı (GPIO6/7 -> I2C1) ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- SPI: SD kart (GPIO26/27/28 -> SPI1) ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 26
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 27
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 28
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 31
#endif

// --- Dahili flash (QSPI) ---
#ifndef PICO_BOOT_STAGE2_CHOOSE_W25Q080
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#endif
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

// SDK'nın varsayılan RAM/stack ayarları 520 KB SRAM için uygun.

#endif /* _BOARDS_POKEBIRD_RP2350B_H */
