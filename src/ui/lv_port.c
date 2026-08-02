#include "ui/lv_port.h"

#include "lvgl.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "hal/display/lcd_blit.h"
#include "hal/touch.h"

/* ── Çizim tamponu ────────────────────────────────────────────────────────
 * Kısmi (partial) render: LVGL ekranı yatay şeritler hâlinde çiziyor ve her
 * şeridi flush_cb'ye veriyor. TAM FRAMEBUFFER YOK — 640x172 RGB565 220 KB
 * ederdi, 520 KB SRAM'in %42'si (plan §5).
 *
 * Plan §M2b iki tampon öngörüyordu (2 x 640x10 = 25.6 KB). TEK tampon
 * kullanıyoruz, aynı bütçeyle iki katı yükseklikte: flush'ımız bloklayan
 * DMA ile çalışıp hemen `flush_ready` diyor, dolayısıyla ikinci tampon
 * hiçbir zaman paralel çizim sağlamaz — sadece şerit sayısını iki katına
 * çıkarırdı. Aynı 25 KB'ı tek parça kullanmak flush çağrısını yarıya
 * indiriyor.                                                              */
#define LV_STRIP_H  20
static uint16_t s_draw_buf[PB_LCD_W * LV_STRIP_H];

/* ── Yön çevrimi ──────────────────────────────────────────────────────────
 * Kartta ölçüldü (`o` komutu). Cihaz USB soketi SAĞDA, yatay tutuluyor:
 *
 *     panel_y = ui_x            (panel Y+  =  fiziksel sol -> sağ)
 *     panel_x = 171 - ui_y      (panel X+  =  fiziksel alt -> üst)
 *
 * Devrik (transpose) kopyası için ikinci bir tampon ayırmıyoruz: panelin bir
 * YATAY satırı, LVGL tamponunun bir DİKEY sütunudur, o da adımlı okumayla
 * doğrudan gönderilebiliyor (bkz. pb_lcd_blit_strided).                   */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t x1 = area->x1, x2 = area->x2;
    const int32_t y1 = area->y1, y2 = area->y2;
    const int32_t alan_w = x2 - x1 + 1;          /* LVGL tamponunun satır uzunluğu */

    const uint16_t *src = (const uint16_t *)(void *)px_map;

    /* Panel dikdörtgeni: ui_y aralığı panel X'e, ui_x aralığı panel Y'ye. */
    const uint32_t panel_x = (uint32_t)(PB_LCD_H - 1 - y2);
    const uint32_t panel_y = (uint32_t)x1;
    const uint32_t panel_w = (uint32_t)(y2 - y1 + 1);
    const uint32_t panel_h = (uint32_t)(x2 - x1 + 1);

    /* piksel(panel satırı r, panel sütunu c):
     *     ui_x = x1 + r          -> kaynakta +1 adım
     *     ui_y = y2 - c          -> kaynakta -alan_w adım
     * yani başlangıç, tamponun SON satırının başı. */
    pb_lcd_blit_strided(panel_x, panel_y, panel_w, panel_h,
                        src + (size_t)(y2 - y1) * alan_w,
                        -alan_w,   /* sütun adımı */
                        1);        /* satır adımı */

    lv_display_flush_ready(disp);
}

/* ── Dokunmatik ───────────────────────────────────────────────────────────
 * Çip koordinatları zaten yatay veriyor: ham x uzun eksen (0..640), ham y
 * kısa eksen (0..172) — arayüzün ekseniyle aynı düzen.
 *
 * AÇIK KALAN: 180° aynalama gerekiyor mu? Cihazı hangi yönde tuttuğumuza
 * bağlı ve ekran yönünde olduğu gibi ÖLÇÜLMELİ (`t` komutu). Ölçülene kadar
 * aynalama yok kabul ediliyor; yanlışsa dokunuşlar ters köşeye düşer.     */
#define PB_TOUCH_AYNALA 0

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    pb_touch_state_t st = pb_touch_read();

    if (!st.ok || st.fingers == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int32_t ux = (int32_t)st.p.raw_x;
    int32_t uy = (int32_t)st.p.raw_y;

    if (ux > PB_LCD_W - 1) ux = PB_LCD_W - 1;
    if (uy > PB_LCD_H - 1) uy = PB_LCD_H - 1;

#if PB_TOUCH_AYNALA
    ux = (PB_LCD_W - 1) - ux;
    uy = (PB_LCD_H - 1) - uy;
#endif

    data->point.x = ux;
    data->point.y = uy;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* ── Kirli alanı arayüzün SOL KENARINA yay ────────────────────────────────
 *
 * Panel RASET'i (0x2B) yok sayıyor (§9n): bir yazma ancak sütun penceresinin
 * EN ÜST satırından (RAMWR) ya da imlecin durduğu yerden (RAMWRC)
 * başlayabiliyor. Ara satırlara gitmenin tek yolu üzerini yazmak.
 *
 * `flush_cb`'de panel_y = area->x1. `x1`i 0'a sabitlemek panel_y'yi HER
 * ZAMAN 0 yapıyor: her flush RAMWR ile sütun penceresinin tepesinden başlar,
 * atlama bedeli de üzerine yazma da olmaz.
 *
 * `x2`ye DOKUNMUYORUZ — bilerek. Onu da tam genişliğe çekmek LVGL'e her
 * yenilemede panelin tamamını çizdirir ve `a` demosunda spektrogram şeridini
 * (arayüz x 200..639) siler. Kirli alan yalnızca sola doğru büyütülüyor. */
static void alan_yuvarla(lv_event_t *e)
{
    lv_area_t *alan = (lv_area_t *)lv_event_get_param(e);
    if (alan) alan->x1 = 0;
}

/* LVGL'in zaman tabanı. v9'da makro değil, çalışma anında veriliyor. */
static uint32_t tick_cb(void)
{
    return to_ms_since_boot(get_absolute_time());
}

bool pb_lv_init(void)
{
    /* Bir kez kur: iki komut arka arkaya çağırırsa (örn. 'u' sonra 'a')
     * ikinci lv_init + ikinci display oluşturmak LVGL'i bozar. */
    static bool s_inited = false;
    static bool s_touch_ok = false;
    if (s_inited) return s_touch_ok;
    s_inited = true;

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(PB_LCD_W, PB_LCD_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, alan_yuvarla, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_set_buffers(disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_touch_ok = pb_touch_init();
    if (s_touch_ok) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, indev_read_cb);
    }
    return s_touch_ok;
}

void pb_lv_tick(void)
{
    lv_timer_handler();
}
