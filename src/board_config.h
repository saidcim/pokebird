/**
 * board_config.h — Waveshare RP2350-Touch-LCD-3.49 donanım tanımları
 *
 * TEK DOĞRULUK KAYNAĞI. Bu dosyadaki pin numaraları, Waveshare'in resmi
 * şemasından (RP2350-Touch-LCD-3.49.pdf) net-net çıkarılmıştır; tahmin yok.
 * Donanımla ilgili bir sayı gerekiyorsa buradan alınır, koda gömülmez.
 *
 * MCU : RP2350B (2x Cortex-M33 @ 150 MHz, FPU + DSP)
 * RAM : 520 KB SRAM — PSRAM YOK
 * ROM : 16 MB QSPI flash (PY25Q128HA)
 */
#ifndef POKEBIRD_BOARD_CONFIG_H
#define POKEBIRD_BOARD_CONFIG_H

/* ── Ses: ES8311 codec + analog MEMS mikrofon (MIC1) + NS4150B D-sınıfı amfi ── */
#define PB_PIN_NS_MODE      0   /* NS4150B hoparlör amfi mod/enable            */
#define PB_PIN_I2S_DSDIN    1   /* MCU  -> codec DAC  (ses çalma)              */
#define PB_PIN_I2S_DSOUT    2   /* codec ADC -> MCU   (MİKROFON VERİSİ)        */
#define PB_PIN_I2S_MCLK     3   /* PIO ile üretilir; ES8311 bundan türetir     */
#define PB_PIN_I2S_SCLK     4   /* BCLK — ES8311 master modda üretir           */
#define PB_PIN_I2S_LRCK     5   /* WS   — ES8311 master modda üretir           */

/* ── Paylaşımlı I2C (codec kontrol). IMU ve RTC de bu hatta ama kullanılmıyor ── */
#define PB_PIN_I2C_SDA      6
#define PB_PIN_I2C_SCL      7
#define PB_I2C_INST         i2c1        /* GPIO6/7 -> I2C1                     */
#define PB_I2C_BAUD         400000

/* Kasıtlı olarak kullanılmayan çevre birimleri (bkz. plan §1):
 *   GPIO8/9  IMU_INT1/2 — QMI8658: projeye katkısı yok
 *   GPIO10   RTC_INT    — PCF85063: pil yedeklemesi yok, saati kaybediyor
 * Pinler burada belgelenmiş hâlde duruyor ki ileride yanlışlıkla kullanılmasın. */
#define PB_PIN_IMU_INT1     8   /* KULLANILMIYOR */
#define PB_PIN_IMU_INT2     9   /* KULLANILMIYOR */
#define PB_PIN_RTC_INT      10  /* KULLANILMIYOR */

/* ── Dokunmatik: AXS15231B, LCD ile aynı çip ama AYRI I2C hattı ── */
#define PB_PIN_TP_INT       11
#define PB_PIN_TP_SDA       32
#define PB_PIN_TP_SCL       33
#define PB_TP_I2C_INST      i2c0        /* GPIO32/33 -> I2C0                   */
#define PB_TP_I2C_BAUD      400000
#define PB_TP_I2C_ADDR      0x3B        /* AXS15231B dokunmatik adresi         */

/* ── Boş GPIO'lar: P3 başlığı (12-19) ve J3/J6 başlığı (41-47) ──
 * Harici I2S MEMS mikrofon gerekirse (bkz. plan §10 risk tablosu) buradan. */
#define PB_PIN_FREE_HDR_P3_FIRST   12
#define PB_PIN_FREE_HDR_P3_LAST    19
#define PB_PIN_FREE_HDR_J3_FIRST   41
#define PB_PIN_FREE_HDR_J3_LAST    47

/* ── Ekran: AXS15231B, 172x640 IPS, QSPI ── */
#define PB_PIN_LCD_SCL      20
#define PB_PIN_LCD_D0       21
#define PB_PIN_LCD_D1       22
#define PB_PIN_LCD_D2       23
#define PB_PIN_LCD_D3       24
#define PB_PIN_LCD_CS       25
#define PB_PIN_LCD_RST      34
#define PB_PIN_LCD_TE       35      /* tearing effect — yırtılmayı önlemek için */
#define PB_PIN_LCD_BL       36      /* arka ışık PWM                            */
#define PB_PIN_BL_EN        37      /* arka ışık yükseltici enable              */

#define PB_LCD_NATIVE_W     172
#define PB_LCD_NATIVE_H     640
/* Cihaz yatay tutuluyor: 640x172'lik şerit */
#define PB_LCD_W            640
#define PB_LCD_H            172

/* ── SD kart (SPI modu; SD_D1/D2 mevcut, ileride 4-bit SDIO'ya geçilebilir) ── */
#define PB_PIN_SD_SCLK      26
#define PB_PIN_SD_MOSI      27
#define PB_PIN_SD_MISO      28
#define PB_PIN_SD_D1        29
#define PB_PIN_SD_D2        30
#define PB_PIN_SD_CS        31

/* ── Güç ── */
#define PB_PIN_SYS_OUT      38      /* güç mandalı: durum                      */
#define PB_PIN_SYS_EN       39      /* güç mandalı: kendini kapatmak için      */
#define PB_PIN_BAT_ADC      40      /* pil voltajı (ADC)                       */
#define PB_BAT_ADC_CHANNEL  (PB_PIN_BAT_ADC - 40)   /* RP2350B: ADC0 = GPIO40  */
/* Şemadaki bölücü: R15 100K / R3 200K -> Vbat = Vadc * 1.5 */
#define PB_BAT_DIVIDER      1.5f

/* ── Ses hattı parametreleri (plan §3) ──
 * 24 kHz seçildi: 12 kHz Nyquist, Çalıkuşu/Tırmaşıkkuşu gibi 7-9 kHz'de
 * öten türleri kapsar. 16 kHz bunları kırpardı. */
#define PB_SAMPLE_RATE      24000
#define PB_MCLK_RATE        (PB_SAMPLE_RATE * 256)
#define PB_BITS_PER_SAMPLE  16
#define PB_MIC_GAIN         3       /* ES8311 PGA kademesi; M1'de kalibre edilecek */

#endif /* POKEBIRD_BOARD_CONFIG_H */
