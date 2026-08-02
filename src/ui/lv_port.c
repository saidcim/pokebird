#include "ui/lv_port.h"

#include <stdio.h>

#include "lvgl.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "hal/display/lcd_blit.h"
#include "hal/touch.h"

/* ── Kart bölgesi ve framebuffer'ı — rsvpnano'nun geometrisi ──────────────
 *
 * ÖLÇÜLMÜŞ KISIT (§9n): bu panelde panele **tam genişlikte** (172 sütun)
 * yazmak DÜZ çalışıyor; **dar sütun bandına çok satırlı** yazmak satır başına
 * KAYIYOR. LVGL ise kirli dikdörtgen veriyor ve yatay arayüzde kirli bir
 * dikdörtgen tam olarak dar sütun bandına düşüyor — yazının bozuk
 * görünmesinin sebebi buydu.
 *
 * Çalışan referans (rsvpnano) bu tuzağa hiç düşmüyor çünkü panele HER ZAMAN
 * tam genişlikte satır bantları basıyor:
 *
 *     // DisplayManager::flushScaledFrame — orada ~20 ekranin hepsi boyle
 *     drawBitmap(0, nativeYStart, kPanelNativeWidth, nativeYStart + rows, txBuffer_);
 *
 * Bunu yapabilmesinin bedeli bir framebuffer. Aynısını burada YALNIZCA KART
 * BÖLGESİ için ödüyoruz: kart arayüzün ilk 200 sütunu (= panel satırı
 * 0..199), spektrogram panel satırı 200..639'da ve LVGL oraya hiç dokunmuyor.
 *
 *     200 x 172 x 2 = 68.800 bayt
 *
 * Tam ekran framebuffer'ı (640x172 = 220 KB, plan §5'te reddedilen) DEĞİL;
 * yalnızca kartın kendisi. Yerleşim PANEL yöneliminde tutuluyor
 * (`[panel satırı][panel sütunu]`), böylece panele basarken devrik alma ya da
 * adımlı okuma gerekmiyor: bir satır bandı doğrudan bitişik. */
#define PB_LV_W  200                    /* arayüz genişliği = panel satırı 0..199 */
#define PB_LV_H  PB_LCD_H               /* arayüz yüksekliği = panel sütunu, 172 */

static uint16_t s_kart_fb[PB_LV_W][PB_PANEL_W];

/* ── Çizim tamponu ────────────────────────────────────────────────────────
 * Kısmi (partial) render: LVGL kartı şeritler hâlinde çiziyor ve her şeridi
 * flush_cb'ye veriyor. Ekran 200 sütuna daraldığı için tampon da küçüldü
 * (640x20 = 25,6 KB → 200x20 = 8 KB); framebuffer'ın maliyetinin bir kısmını
 * bu geri kazandırıyor. */
#define LV_STRIP_H  20
static uint16_t s_draw_buf[PB_LV_W * LV_STRIP_H];

/* ── Yön çevrimi ──────────────────────────────────────────────────────────
 * Kartta ölçüldü (`o` komutu). Cihaz USB soketi SAĞDA, yatay tutuluyor:
 *
 *     panel_y = ui_x            (panel Y+  =  fiziksel sol -> sağ)
 *     panel_x = 171 - ui_y      (panel X+  =  fiziksel alt -> üst)
 *
 * Devrik (transpose) kopyası için ikinci bir tampon ayırmıyoruz: panelin bir
 * YATAY satırı, LVGL tamponunun bir DİKEY sütunudur, o da adımlı okumayla
 * doğrudan gönderilebiliyor (bkz. pb_lcd_blit_strided).                   */
/* ── Flush sayaçları — hizalama gerçekten tutuyor mu (§9n) ────────────────
 * Panel sütun aralığını 2 piksele yuvarlıyor. Sütun sayısı TEK olursa panel
 * satır başına bizim gönderdiğimizden bir piksel FAZLA kullanır ve veri her
 * satırda bir piksel kayar — yazının yatay sürüklenmiş görünmesinin birebir
 * imzası. `alan_yuvarla` bunu engellemeli; sayaçlar ENGELLEDİĞİNİ ölçüyor.
 * Göz gerekmiyor: `a`/`u` çıkışında basılıyor. */
uint32_t pb_lv_flush_say;
uint32_t pb_lv_flush_hizasiz;      /* panel_x tek ya da panel_w tek */
uint32_t pb_lv_flush_stride_farkli; /* LVGL'in satır adımı alan genişliği DEĞİL */
uint32_t pb_lv_flush_w_min = 0xFFFFFFFF, pb_lv_flush_w_max;
int32_t  pb_lv_son_y1, pb_lv_son_y2, pb_lv_son_x1, pb_lv_son_x2;
int32_t  pb_lv_son_stride_px, pb_lv_son_alan_w;

static int s_dokum_kalan = 0;

void pb_lv_dokum_iste(int adet) { s_dokum_kalan = adet; }

void pb_lv_flush_sayaclari_sifirla(void)
{
    pb_lv_flush_say = 0;
    pb_lv_flush_hizasiz = 0;
    pb_lv_flush_stride_farkli = 0;
    pb_lv_flush_w_min = 0xFFFFFFFF;
    pb_lv_flush_w_max = 0;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t x1 = area->x1, x2 = area->x2;
    const int32_t y1 = area->y1, y2 = area->y2;
    const int32_t alan_w = x2 - x1 + 1;

    const uint16_t *src = (const uint16_t *)(void *)px_map;

    /* ⚠ SATIR ADIMI ALAN GENİŞLİĞİ DEĞİL — LVGL'e sorulmalı.
     * Devrik okuma `-satir_adimi` ile sütun atlıyor; adım bir piksel bile
     * şaşarsa görüntü her satırda kayar ve yazı yatay sürüklenmiş görünür.
     * LVGL çizim tamponunun adımını `LV_DRAW_BUF_STRIDE_ALIGN`e göre
     * yuvarlayabiliyor, dolayısıyla `x2-x1+1` VARSAYMAK yanlış. */
    int32_t satir_adimi = alan_w;
    lv_draw_buf_t *db = lv_display_get_buf_active(disp);
    if (db && db->header.stride) satir_adimi = (int32_t)(db->header.stride / 2);
    pb_lv_son_stride_px = satir_adimi;
    pb_lv_son_alan_w = alan_w;
    if (satir_adimi != alan_w) pb_lv_flush_stride_farkli++;

    /* Panel dikdörtgeni: ui_y aralığı panel X'e, ui_x aralığı panel Y'ye. */
    const uint32_t panel_x = (uint32_t)(PB_LCD_H - 1 - y2);
    const uint32_t panel_y = (uint32_t)x1;
    const uint32_t panel_w = (uint32_t)(y2 - y1 + 1);
    const uint32_t panel_h = (uint32_t)(x2 - x1 + 1);

    pb_lv_flush_say++;
    if ((panel_x & 1u) || (panel_w & 1u)) pb_lv_flush_hizasiz++;
    if (panel_w < pb_lv_flush_w_min) pb_lv_flush_w_min = panel_w;
    if (panel_w > pb_lv_flush_w_max) pb_lv_flush_w_max = panel_w;
    pb_lv_son_x1 = x1; pb_lv_son_x2 = x2;
    pb_lv_son_y1 = y1; pb_lv_son_y2 = y2;

    /* piksel(panel satırı r, panel sütunu c):
     *     ui_x = x1 + r          -> kaynakta +1 adım
     *     ui_y = y2 - c          -> kaynakta -alan_w adım
     * yani başlangıç, tamponun SON satırının başı. */
    /* ── Göz gerektirmeyen yazı teşhisi ───────────────────────────────────
     * Alanı, sürücünün OKUDUĞU indislemeyle seri porta ASCII olarak döküyor.
     * Terminalde yazı düzgün okunuyorsa hem LVGL'in çizimi hem devrik okuma
     * doğru demektir ve bozulma daha aşağıda. Okunmuyorsa bozulma LVGL'de.
     * Satırlar ui yönünde: dış döngü panel sütunu (= ui y), iç döngü panel
     * satırı (= ui x). */
    if (s_dokum_kalan > 0 && panel_h <= 320 && panel_w <= 180) {
        s_dokum_kalan--;
        printf("#DOKUM ui x(%ld..%ld) y(%ld..%ld) panel %lux%lu adim %ld\n",
               (long)x1, (long)x2, (long)y1, (long)y2,
               (unsigned long)panel_w, (unsigned long)panel_h, (long)satir_adimi);
        for (int32_t c = (int32_t)panel_w - 1; c >= 0; c--) {
            for (uint32_t r = 0; r < panel_h; r++) {
                uint16_t px = src[(size_t)(y2 - y1 - c) * satir_adimi + r];
                /* RGB565 -> kaba parlaklık */
                uint32_t l = ((px >> 11) & 0x1F) + ((px >> 6) & 0x1F) + (px & 0x1F);
                putchar(l < 6 ? '.' : (l < 24 ? '+' : '#'));
            }
            putchar('\n');
        }
        printf("#DOKUM-SON\n");
    }

    /* 1) Kirli dikdörtgeni framebuffer'a işle (devrik, CPU ile).
     *    piksel(panel satırı r, panel sütunu c) = src[(y2-y1-c)*adım + r] */
    for (uint32_t r = 0; r < panel_h; r++) {
        uint16_t *dst = &s_kart_fb[panel_y + r][0];
        for (uint32_t c = 0; c < panel_w; c++) {
            dst[panel_x + c] = src[(size_t)(y2 - y1 - c) * satir_adimi + r];
        }
    }

    /* 2) Panele HER ZAMAN TAM GENİŞLİKTE bas — kayma yalnızca dar sütun
     *    bandında oluyor (§9n, `S` ile ölçüldü). Framebuffer satırları
     *    zaten 172 piksel ve bitişik, o yüzden bu düz bir blit. */
    pb_lcd_blit(0, panel_y, PB_PANEL_W, panel_h, &s_kart_fb[panel_y][0]);

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

    if (ux > PB_LV_W - 1) ux = PB_LV_W - 1;
    if (uy > PB_LV_H - 1) uy = PB_LV_H - 1;

#if PB_TOUCH_AYNALA
    ux = (PB_LV_W - 1) - ux;
    uy = (PB_LV_H - 1) - uy;
#endif

    data->point.x = ux;
    data->point.y = uy;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* NOT: eskiden burada bir `alan_yuvarla` vardı — kirli alanı sola yayıp
 * panel sütun aralığını 2 piksele hizalıyordu. Artık GEREKMİYOR: panele her
 * zaman tam genişlikte (0..171) basıyoruz, o da tanımı gereği hizalı ve
 * kaymayan geometri. LVGL'in kirli dikdörtgeni serbest bırakıldı, böylece
 * gereksiz yeniden çizim de yok. */

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

    lv_display_t *disp = lv_display_create(PB_LV_W, PB_LV_H);
    lv_display_set_flush_cb(disp, flush_cb);
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
