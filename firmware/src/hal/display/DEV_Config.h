/**
 * DEV_Config.h — a compatibility shim for Waveshare's display drivers
 *
 * qspi_pio.c, LCD_3in49.c ve Touch.c Waveshare'in RP2350-Touch-LCD-3.49 LVGL
 * example. Those files expect Waveshare's own HAL (DEV_Config.h). Rather
 * than editing the drivers line by line, we satisfy the handful of symbols
 * they need from the project's own definitions here.
 *
 * That keeps the vendor files almost untouched, so if Waveshare publishes an
 * update it is easy to take again.
 */
#ifndef POKEBIRD_DISPLAY_DEV_CONFIG_H
#define POKEBIRD_DISPLAY_DEV_CONFIG_H

#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include "board_config.h"

/* The data types Waveshare's drivers use */
#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

/* Touch is on the same chip as the LCD (AXS15231B) but on a SEPARATE I2C bus */
#define TOUCH_I2C_PORT  PB_TP_I2C_INST
#define TOUCH_SDA_PIN   PB_PIN_TP_SDA
#define TOUCH_SCL_PIN   PB_PIN_TP_SCL
#define TOUCH_INT_PIN   PB_PIN_TP_INT

#define LCD_BL_PIN      PB_PIN_LCD_BL
#define BAT_ADC         PB_PIN_BAT_ADC

#define DEV_Delay_ms(x) sleep_ms(x)
#define DEV_Delay_us(x) sleep_us(x)

/* LCD_3in49.c uses these two globals; they are defined in
 * hal/display/dev_config.c. The short, generic names are kept so the vendor
 * files do not have to change. */
#include "hardware/dma.h"
extern uint dma_tx;
extern dma_channel_config c;

/** Prepare the display's DMA channel — call before LCD_3IN49_Init(). */
void pb_display_dma_init(void);

#endif /* POKEBIRD_DISPLAY_DEV_CONFIG_H */
