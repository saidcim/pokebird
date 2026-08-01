#include "ui/spectrogram.h"

#include <string.h>

#include "hal/display/lcd_blit.h"

/* ── Yön çevirimi ─────────────────────────────────────────────────────────
 * Panel doğal olarak 172 geniş x 640 yüksek (dikey). Arayüz ise cihazı yatay
 * tutuyor: 640 geniş x 172 yüksek.
 *
 * KARTTA ÖLÇÜLDÜ (`o` komutu — dört köşeye dört renk, cihaz USB soketi
 * AŞAĞI bakacak şekilde tutuldu):
 *
 *     panel X 0->171  =  fiziksel SOL -> SAĞ
 *     panel Y 0->639  =  fiziksel ÜST -> ALT
 *
 * Yatay kullanım için cihaz 90° sola çevriliyor; o konumda **USB soketi
 * SAĞDA** kalıyor ve eksenler şöyle düşüyor:
 *
 *     panel Y+  =  fiziksel SOL -> SAĞ      -> arayüzün x'i (zaman)
 *     panel X+  =  fiziksel ALT -> ÜST      -> arayüzün y'si, TERS
 *
 *   arayüz (ux, uy)  ->  panel (nx, ny)
 *   ux 0..639 (sol->sağ, zaman)   ->  ny = ux
 *   uy 0..171 (üst->alt, frekans) ->  nx = 171 - uy
 *
 * Yani arayüzdeki bir DİKEY SÜTUN, panelde bir YATAY SATIR oluyor — tek
 * blit çağrısıyla, 172 piksel. İlk denememde bunu ters kurmuştum: sürücüye
 * 639'a kadar X değeri verdim, panelin X ekseni ise sadece 0..171. Pencere
 * adresi geçersiz olduğu için ekran tamamen siyah kaldı.
 *
 * Cihazı ters yönde (USB solda) tutmak isterseniz iki şey birden dönmeli:
 * hem `write_ui_column`'daki ny, hem aşağıdaki bin eşlemesi.               */

static uint16_t s_column[PB_SPEC_HEIGHT];
static uint32_t s_write_x = 0;

/**
 * Genliği renge çevir — siyah → koyu mavi → camgöbeği → sarı → beyaz.
 *
 * Doğrusal gri tonlama kuş sesi için kötü: ilgilendiğimiz detay üst
 * genliklerde toplanıyor ve gri tonlamada ayırt edilemiyor. Renk geçişi hem
 * zayıf harmonikleri hem güçlü temel frekansı aynı anda okunur kılıyor.
 */
static uint16_t amplitude_to_rgb565(uint8_t v) {
    uint8_t r, g, b;
    if (v < 64) {
        r = 0;   g = 0;                        b = (uint8_t)(v * 2);
    } else if (v < 128) {
        r = 0;   g = (uint8_t)((v - 64) * 4);  b = (uint8_t)(128 + (v - 64) * 2);
    } else if (v < 192) {
        r = (uint8_t)((v - 128) * 4); g = 255; b = (uint8_t)(255 - (v - 128) * 4);
    } else {
        r = 255; g = 255;                      b = (uint8_t)((v - 192) * 4);
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** Arayüz sütununu (ux) panele yaz: panelde ny=ux satırı, nx=0..171.
 *  Tampon indeksi doğrudan panel X'i; arayüzün y'si ters olduğu için
 *  çeviriyi dolduran taraf (pb_spec_push_column) yapıyor. */
static void write_ui_column(uint32_t ux, const uint16_t *col) {
    pb_lcd_blit(0, ux, PB_SPEC_HEIGHT, 1, col);
}

void pb_spec_init(void) {
    memset(s_column, 0, sizeof(s_column));
    for (uint32_t ux = PB_SPEC_X0; ux <= PB_SPEC_X1; ux++) {
        write_ui_column(ux, s_column);
    }
    s_write_x = 0;
}

void pb_spec_push_column(const uint8_t *bins, uint32_t n_bins) {
    if (!bins || n_bins == 0) return;

    /* Düşük frekanslar ekranın ALTINDA — spektrogram geleneği bu.
     * Tampon indeksi panel X; yatay tutuşta panel X+ fiziksel olarak
     * AŞAĞIDAN YUKARIYA gidiyor (yukarıdaki ölçüm). Yani düşük frekans
     * nx=0'da olmalı: bin doğrudan indeksle artıyor, ters çevirme YOK. */
    for (uint32_t nx = 0; nx < PB_SPEC_HEIGHT; nx++) {
        uint32_t bin = (nx * n_bins) / PB_SPEC_HEIGHT;
        s_column[nx] = amplitude_to_rgb565(bins[bin]);
    }
    write_ui_column(PB_SPEC_X0 + s_write_x, s_column);

    /* Bir sonraki sütunu koyu gri imleçle işaretle: şeridin "şimdi"si
     * belli olmazsa kayan görüntü okunmuyor. */
    static uint16_t cursor[PB_SPEC_HEIGHT];
    for (uint32_t nx = 0; nx < PB_SPEC_HEIGHT; nx++) cursor[nx] = 0x4208;
    write_ui_column(PB_SPEC_X0 + ((s_write_x + 1) % PB_SPEC_WIDTH), cursor);

    s_write_x = (s_write_x + 1) % PB_SPEC_WIDTH;
}
