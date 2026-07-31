#include "lcd_blit.h"

#include "DEV_Config.h"
#include "LCD_3in49.h"
#include "qspi_pio.h"
#include "hardware/dma.h"

/* Bayt sırası çevrilmiş satır tamponu. Panelin bir satırı en fazla PB_PANEL_W
 * piksel; sütun yazarken de aynı tampon kullanılıyor (h=1). */
static uint16_t s_row[PB_PANEL_W];

/**
 * Pencereyi ayarla.
 *
 * Waveshare'in SetWindows'u bitiş koordinatını DIŞLAYICI kabul ediyor:
 * register'a (Xend-1) yazıyor. Yani son piksel Xend-1. Bu fonksiyona
 * kapsayıcı w/h veriyoruz ve dönüşümü burada yapıyoruz — çağıran tarafta
 * bir eksik/bir fazla hatası yapmak kolay.
 */
static void set_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    LCD_3IN49_SetWindows(x, y, x + w, y + h);
}

void pb_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint16_t *buf) {
    if (!buf || w == 0 || h == 0) return;
    if (x >= PB_PANEL_W || y >= PB_PANEL_H) return;
    if (x + w > PB_PANEL_W) w = PB_PANEL_W - x;
    if (y + h > PB_PANEL_H) h = PB_PANEL_H - y;

    set_window(x, y, w, h);

    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);           /* RAMWR */
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

    for (uint32_t row = 0; row < h; row++) {
        const uint16_t *src = buf + (size_t)row * w;
        for (uint32_t i = 0; i < w; i++) {
            /* Panel big-endian RGB565 istiyor */
            s_row[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
        }
        dma_channel_configure(dma_tx, &c,
                              &qspi.pio->txf[qspi.sm],
                              s_row,
                              w * 2,          /* bayt sayısı (8-bit aktarım) */
                              true);
        while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
    }

    QSPI_Deselect(qspi);
}

void pb_lcd_fill(uint16_t color) {
    uint16_t line[PB_PANEL_W];
    for (uint32_t i = 0; i < PB_PANEL_W; i++) line[i] = color;
    for (uint32_t y = 0; y < PB_PANEL_H; y++) {
        pb_lcd_blit(0, y, PB_PANEL_W, 1, line);
    }
}
