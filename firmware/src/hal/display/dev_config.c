/**
 * dev_config.c — the shared DMA state Waveshare's display drivers expect
 *
 * LCD_3in49.c uses two globals defined in Waveshare's DEV_Config.c: `dma_tx`
 * (the channel number) and `c` (the channel configuration). Rather than
 * taking all of Waveshare's DEV_Config.c (which also sets up I2C, ADC, RTC
 * and the IMU, and would clash with our own HAL), we define just those two
 * here.
 *
 * The names are deliberately left short and generic so the vendor files do
 * not have to be modified. New code should not use them.
 */
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "qspi_pio.h"

uint dma_tx;
dma_channel_config c;

extern pio_qspi_t qspi;   /* defined in qspi_pio.c */

/**
 * Prepare the display's DMA channel. Must be called before LCD_3IN49_Init().
 *
 * 8-bit transfers: the QSPI PIO program is fed byte by byte. The read address
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
