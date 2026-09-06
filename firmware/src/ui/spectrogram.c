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
 * Genliği renge çevir — SICAK rampa: ekran zemini → köz → kehribar → beyaz.
 *
 * Doğrusal gri tonlama kuş sesi için kötü: ilgilendiğimiz detay üst
 * genliklerde toplanıyor ve gri tonlamada ayırt edilemiyor. Parlaklığı
 * boydan boya artan bir rampa hem zayıf harmonikleri hem güçlü temel
 * frekansı aynı anda okunur kılıyor. Bu gerekçe DEĞİŞMEDİ.
 *
 * ⚠ RENKLER DEĞİŞTİ (eski: siyah → koyu mavi → camgöbeği → sarı → beyaz).
 * Sebep önizlemede görüldü: arayüzün kalanı sıcak ve koyu (kehribar/yeşil,
 * zemin #0B0908), spektrogram ise elektrik mavisiydi. Yan yana iki ayrı
 * ürün gibi duruyordu ve x=384'te sert bir dikey dikiş bırakıyordu.
 *
 * İki şey birden çözülüyor:
 *   · rampa tasarımın kehribarına (#FFB020) oturuyor
 *   · TABAN, ekran zemininin TA KENDİSİ (#0B0908) — sessizlik arayüzün
 *     zeminiyle aynı renk olduğu için dikiş kayboluyor
 */
typedef struct { uint8_t v, r, g, b; } durak_t;

/* Parlaklık boydan boya artıyor; ara renkler tasarımın kehribarından geçiyor. */
static const durak_t RAMPA[] = {
    {   0, 0x0B, 0x09, 0x08 },   /* ekran zemini — sessizlik              */
    {  56, 0x2E, 0x18, 0x0A },   /* köz                                    */
    { 128, 0x8A, 0x3F, 0x0C },   /* kızıl kehribar                         */
    { 190, 0xFF, 0xB0, 0x20 },   /* PB_COLOR_ACCENT — tasarımın vurgusu      */
    { 255, 0xFF, 0xF2, 0xCC },   /* sıcak beyaz — tepe                     */
};

static uint16_t amplitude_to_rgb565(uint8_t v) {
    const uint32_t n = sizeof(RAMPA) / sizeof(RAMPA[0]);

    uint32_t i = 0;
    while (i + 2 < n && v > RAMPA[i + 1].v) i++;

    const durak_t *a = &RAMPA[i], *b = &RAMPA[i + 1];
    const int araligi = (int)b->v - (int)a->v;
    const int t = araligi > 0 ? ((int)v - (int)a->v) * 255 / araligi : 0;

    const uint8_t r = (uint8_t)(a->r + ((int)b->r - (int)a->r) * t / 255);
    const uint8_t g = (uint8_t)(a->g + ((int)b->g - (int)a->g) * t / 255);
    const uint8_t bl = (uint8_t)(a->b + ((int)b->b - (int)a->b) * t / 255);

    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
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

    /* Bir sonraki sütunu imleçle işaretle: şeridin "şimdi"si belli olmazsa
     * kayan görüntü okunmuyor. Renk PB_COLOR_BORDER (0x3A332A) — arayüzün
     * ayraç rengiyle aynı; eski koyu gri sıcak paletin içinde yabancı
     * duruyordu. */
    static uint16_t cursor[PB_SPEC_HEIGHT];
    for (uint32_t nx = 0; nx < PB_SPEC_HEIGHT; nx++) cursor[nx] = 0x3985;
    write_ui_column(PB_SPEC_X0 + ((s_write_x + 1) % PB_SPEC_WIDTH), cursor);

    s_write_x = (s_write_x + 1) % PB_SPEC_WIDTH;
}
