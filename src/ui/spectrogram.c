#include "ui/spectrogram.h"

#include <string.h>

#include "hal/display/LCD_3in49.h"

static uint16_t s_column[PB_SPEC_HEIGHT];
static uint32_t s_write_x = 0;

/**
 * Genliği renge çevir — koyu mavi → camgöbeği → sarı → beyaz.
 *
 * Doğrusal gri tonlama kuş sesi için kötü: ilgilendiğimiz detay üst
 * genliklerde toplanıyor ve gri tonlamada ayırt edilemiyor. Renk geçişi hem
 * zayıf harmonikleri hem güçlü temel frekansı aynı anda okunur kılıyor.
 */
static uint16_t amplitude_to_rgb565(uint8_t v) {
    uint8_t r, g, b;
    if (v < 64) {                 /* siyah -> koyu mavi */
        r = 0;  g = 0;            b = (uint8_t)(v * 2);
    } else if (v < 128) {         /* koyu mavi -> camgöbeği */
        r = 0;  g = (uint8_t)((v - 64) * 4); b = 128 + (uint8_t)((v - 64) * 2);
    } else if (v < 192) {         /* camgöbeği -> sarı */
        r = (uint8_t)((v - 128) * 4); g = 255; b = (uint8_t)(255 - (v - 128) * 4);
    } else {                      /* sarı -> beyaz */
        r = 255; g = 255;         b = (uint8_t)((v - 192) * 4);
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void pb_spec_init(void) {
    memset(s_column, 0, sizeof(s_column));
    for (uint32_t x = PB_SPEC_X0; x <= PB_SPEC_X1; x++) {
        LCD_3IN49_DisplayWindows(x, 0, x, PB_SPEC_HEIGHT - 1, s_column);
    }
    s_write_x = 0;
}

void pb_spec_push_column(const uint8_t *bins, uint32_t n_bins) {
    if (!bins || n_bins == 0) return;

    /* Düşük frekanslar ekranın ALTINDA olacak şekilde çiz — spektrogram
     * geleneği bu ve kuş sesine bakarken beklenen yön. */
    for (uint32_t y = 0; y < PB_SPEC_HEIGHT; y++) {
        uint32_t bin = ((PB_SPEC_HEIGHT - 1 - y) * n_bins) / PB_SPEC_HEIGHT;
        s_column[y] = amplitude_to_rgb565(bins[bin]);
    }

    uint32_t x = PB_SPEC_X0 + s_write_x;
    LCD_3IN49_DisplayWindows(x, 0, x, PB_SPEC_HEIGHT - 1, s_column);

    /* Bir sonraki sütunu beyaz bir imleçle işaretle: nerede yazdığımız
     * görünsün, yoksa şerit nerede "şimdi" belli olmuyor. */
    uint32_t nx = PB_SPEC_X0 + ((s_write_x + 1) % PB_SPEC_WIDTH);
    static uint16_t cursor[PB_SPEC_HEIGHT];
    for (uint32_t y = 0; y < PB_SPEC_HEIGHT; y++) cursor[y] = 0x4208;  /* koyu gri */
    LCD_3IN49_DisplayWindows(nx, 0, nx, PB_SPEC_HEIGHT - 1, cursor);

    s_write_x = (s_write_x + 1) % PB_SPEC_WIDTH;
}
