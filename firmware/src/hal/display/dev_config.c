/**
 * dev_config.c — Waveshare ekran sürücülerinin beklediği paylaşımlı DMA durumu
 *
 * LCD_3in49.c, Waveshare'in DEV_Config.c dosyasında tanımlanan iki global
 * değişkeni kullanıyor: `dma_tx` (kanal numarası) ve `c` (kanal yapılandırması).
 * Waveshare'in DEV_Config.c'sinin tamamını almak yerine (I2C, ADC, RTC, IMU
 * kurulumu da içeriyor ve bizim kendi HAL'imizle çakışır) sadece bu ikisini
 * burada tanımlıyoruz.
 *
 * Bu isimler bilerek kısa/genel bırakıldı — satıcı dosyalarını değiştirmemek
 * için. Yeni kod bunları kullanmamalı.
 */
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "qspi_pio.h"

uint dma_tx;
dma_channel_config c;

extern pio_qspi_t qspi;   /* qspi_pio.c içinde tanımlı */

/**
 * Ekranın DMA kanalını hazırla. LCD_3IN49_Init() öncesinde çağrılmalı.
 *
 * 8-bit aktarım: QSPI PIO programı bayt bayt besleniyor. Okuma adresi artıyor
 * (tampondan), yazma adresi sabit (PIO TX FIFO).
 */
void pb_display_dma_init(void) {
    dma_tx = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
}
