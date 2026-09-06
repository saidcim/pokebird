/**
 * DEV_Config.h — Waveshare ekran sürücüleri için uyum katmanı
 *
 * qspi_pio.c, LCD_3in49.c ve Touch.c Waveshare'in RP2350-Touch-LCD-3.49 LVGL
 * örneğinden alındı. O dosyalar Waveshare'in kendi HAL'ini (DEV_Config.h)
 * bekliyor. Sürücüleri satır satır değiştirmek yerine, bekledikleri birkaç
 * sembolü burada projenin kendi tanımlarından karşılıyoruz.
 *
 * Böylece satıcı dosyaları neredeyse dokunulmamış kalıyor ve Waveshare bir
 * güncelleme yayınlarsa yeniden almak kolay oluyor.
 */
#ifndef POKEBIRD_DISPLAY_DEV_CONFIG_H
#define POKEBIRD_DISPLAY_DEV_CONFIG_H

#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include "board_config.h"

/* Waveshare sürücülerinin kullandığı veri tipleri */
#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

/* Dokunmatik, LCD ile aynı çipte (AXS15231B) ama AYRI I2C hattında */
#define TOUCH_I2C_PORT  PB_TP_I2C_INST
#define TOUCH_SDA_PIN   PB_PIN_TP_SDA
#define TOUCH_SCL_PIN   PB_PIN_TP_SCL
#define TOUCH_INT_PIN   PB_PIN_TP_INT

#define LCD_BL_PIN      PB_PIN_LCD_BL
#define BAT_ADC         PB_PIN_BAT_ADC

#define DEV_Delay_ms(x) sleep_ms(x)
#define DEV_Delay_us(x) sleep_us(x)

/* LCD_3in49.c bu iki globali kullanıyor; hal/display/dev_config.c'de tanımlı.
 * Kısa/genel isimler satıcı dosyalarını değiştirmemek için korundu. */
#include "hardware/dma.h"
extern uint dma_tx;
extern dma_channel_config c;

/** Ekranın DMA kanalını hazırla — LCD_3IN49_Init() öncesinde çağrılmalı. */
void pb_display_dma_init(void);

#endif /* POKEBIRD_DISPLAY_DEV_CONFIG_H */
