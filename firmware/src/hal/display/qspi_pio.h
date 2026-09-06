/*****************************************************************************
* | File      	:   qspi_pio.h
* | Author      :   Waveshare Team
* | Function    :   QSPI Interface Functions
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2025-03-20
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of theex Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#ifndef _QSPI_PIO_H_
#define _QSPI_PIO_H_

#include "qspi.pio.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

/* POKEBIRD: pinler board_config.h'den geliyor — tek doğruluk kaynağı orası.
 * Waveshare'in sabit değerleri board_config.h ile birebir aynı (şematikten
 * bağımsız olarak doğrulandı), yine de tek yerden yönetilsin. */
#include "board_config.h"

#define PIN_CS      PB_PIN_LCD_CS
#define PIN_SCLK    PB_PIN_LCD_SCL
#define PIN_DIO0    PB_PIN_LCD_D0
#define PIN_DIO1    PB_PIN_LCD_D1
#define PIN_DIO2    PB_PIN_LCD_D2
#define PIN_DIO3    PB_PIN_LCD_D3
#define PIN_PWR_EN  PB_PIN_BL_EN
#define PIN_RST     PB_PIN_LCD_RST

#define WAIT_TIME() for(int i=0;i<2;i++) __asm__ volatile("nop");

typedef struct pio_qspi {
    PIO pio;
    uint8_t sm;
    uint8_t sm_4wire;
    uint8_t sm_1wire;
    uint8_t pin_cs;
    uint8_t pin_sclk;
    uint8_t pin_dio0;
    uint8_t pin_dio1;
    uint8_t pin_dio2;
    uint8_t pin_dio3;
    uint8_t pin_pwr_en;
    uint8_t pin_rst;
} pio_qspi_t;

extern pio_qspi_t qspi;

/* POKEBIRD teshis sayaclari — QSPI_WaitIdle gercekten bekliyor mu (§9n).
 * `w` komutu okuyor; sifirlamak icin pb_qspi_counters_reset(). */
extern volatile uint32_t pb_qspi_wait_calls;       /* toplam cagri            */
extern volatile uint32_t pb_qspi_wait_timeout;        /* zaman asimi (SESSIZ hata)*/
extern volatile uint32_t pb_qspi_wait_sm_off;   /* girerken SM etkin degil */
extern volatile uint32_t pb_qspi_wait_fifo_full;   /* girerken FIFO bos degil */
extern volatile uint32_t pb_qspi_wait_residue;     /* cikarken FIFO bos degil */
extern volatile uint32_t pb_qspi_wait_waited;     /* gercekten bekledigi     */
extern volatile uint32_t pb_qspi_wait_fifo_max;
extern volatile uint32_t pb_qspi_wait_spin_max;
extern volatile uint32_t pb_qspi_wait_us_max;
extern volatile uint32_t pb_qspi_wait_us_total;
void pb_qspi_counters_reset(void);

void QSPI_GPIO_Init(pio_qspi_t qspi);
void QSPI_Select(pio_qspi_t qspi);
void QSPI_Deselect(pio_qspi_t qspi);
void QSPI_WaitIdle(pio_qspi_t qspi);
void QSPI_PIO_Init(pio_qspi_t qspi);
void QSPI_PIO_Restore(pio_qspi_t qspi);   /* bit-bang testinden sonra geri al */
void QSPI_1Wrie_Mode(pio_qspi_t *qspi);
void QSPI_4Wrie_Mode(pio_qspi_t *qspi);
void QSPI_DATA_Write(pio_qspi_t qspi, uint32_t val);
void QSPI_REGISTER_Write(pio_qspi_t qspi, uint32_t addr);
void QSPI_Pixel_Write(pio_qspi_t qspi, uint32_t addr);

#endif // _QSPI_PIO_H
