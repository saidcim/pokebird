#include "ui/lv_port.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "hal/display/lcd_blit.h"
#include "hal/touch.h"

/* ── DİLİM (slab) yolu — arayüz artık TAM GENİŞLİK, 640x172 ───────────────
 *
 * ESKİDEN: LVGL yalnızca sol 200 sütunu çiziyordu ve 200x172'lik bir kart
 * framebuffer'ı (68,8 KB) üzerinden panele basıyordu. Sebep §9n'de yazılı:
 * bu panele **dar sütun bandına çok satırlı** yazmak satır başına KAYIYOR,
 * sağlam olan tek geometri **tam genişlikte (172 sütun) satır bandı**.
 *
 * Tam ekran framebuffer'ı bu yüzden reddedilmişti — 640x172x2 = 220 KB.
 * ÖLÇÜLDÜ (arm-none-eabi-nm, bu değişiklikten önce): bss 0x2005f400'de
 * bitiyor, ana SRAM'de ~131 KB boş var. Kart framebuffer'ı (68,8 KB) ve çizim
 * tamponu (8 KB) geri verilse bile 220 KB SIĞMIYOR. Yani framebuffer yolu
 * genişletilerek tam ekrana çıkılamaz.
 *
 * ÇIKIŞ YOLU — panelin sözleşmesi §9n yazıldığından beri GEVŞEDİ:
 * `serit_ile_atla` (lcd_blit.c) imleç konumlandırmayı GENEL AMAÇLI ve
 * GÖRÜNMEZ hâle getirdi. Panelin gerçek şartı artık yalnızca şu:
 *
 *     "tam 172 sütun genişliğinde bas; satır başlangıcı serbest"
 *
 * Arayüz koordinatlarında bu şart, **tam yükseklikte DİKEY BİR DİLİM**
 * demek (ui_y 0..171 hep dâhil, ui_x aralığı serbest). Yani ekranı dikey
 * dilimlere bölüp her dilimi ayrı çizersek hem tam genişliğe çıkıyoruz hem
 * de panelin sözleşmesini hiç ihlal etmiyoruz.
 *
 *     640 = 5 dilim x 128 piksel
 *     dilim framebuffer'ı  128 x 172 x 2 =  44.032 bayt
 *     çizim tamponu        128 x  43 x 2 =  11.008 bayt
 *     ------------------------------------------------
 *     toplam                                55.040 bayt
 *
 * Eski yol 68.800 + 8.000 = 76.800 bayttı. Yani arayüz 200'den 640 sütuna
 * çıkarken RAM **21,7 KB AZALIYOR**.
 *
 * Dilimler ARTAN sırada basılıyor: panel imleci ileri doğru yürüdüğü için
 * aradaki konumlanma ya bedava (RAMWRC) ya da `serit_ile_atla` ile 2*y
 * piksel — ölçülebilir biçimde küçük ve görünmez.                          */
#define PB_LV_W          PB_LCD_W        /* 640 — arayüz genişliği           */
#define PB_LV_H          PB_LCD_H        /* 172 — arayüz yüksekliği          */

#define PB_SLICE_W       128
#define PB_SLICE_COUNT  (PB_LV_W / PB_SLICE_W)   /* 5 */

/* Panel yöneliminde tutuluyor ([panel satırı][panel sütunu]) ki panele
 * basarken devrik alma ya da adımlı okuma gerekmesin: bir satır bandı
 * doğrudan bitişik. */
static uint16_t s_slice_fb[PB_SLICE_W][PB_PANEL_W];

/* 172 = 4 x 43, yani bir dilim tam olarak dört şeritte çiziliyor; artık
 * kalmıyor ve son şerit kısa olmuyor. */
#define LV_STRIP_H  43
static uint16_t s_draw_buf[PB_SLICE_W * LV_STRIP_H];

static lv_display_t *s_disp;

/* Hangi dilimleri LVGL çiziyor. Spektrogram panele DOĞRUDAN yazıyor (kendi
 * hızlı sütun yolu var, 62 Hz); onun dilimlerini LVGL basmamalı yoksa iki
 * yazan aynı bölgede birbirini siler. Ekranlar bunu kendileri bildiriyor. */
static uint32_t s_lvgl_slices = (1u << PB_SLICE_COUNT) - 1u;

/* Yeniden çizilmesi gereken dilimler. LVGL'in kendi geçersizleştirmesinden
 * besleniyor (aşağıdaki olay kancası): hangi etiket değiştiyse yalnızca onun
 * düştüğü dilim basılıyor. Kart 4 Hz güncelleniyor ve her dilim 44 KB QSPI
 * demek — hepsini basmak boşuna 3 kat maliyet olurdu. */
static uint32_t s_dirty;
static bool     s_drawing;              /* kendi invalidate'imizi saymamak için */
static int32_t  s_active_slice = -1;     /* flush_cb hangi dilime yazıyor       */

/* ── Yön çevrimi ──────────────────────────────────────────────────────────
 * Kartta ölçüldü (`o` komutu). Cihaz USB soketi SAĞDA, yatay tutuluyor:
 *
 *     panel_y = ui_x            (panel Y+  =  fiziksel sol -> sağ)
 *     panel_x = 171 - ui_y      (panel X+  =  fiziksel alt -> üst)
 */

/* ── Flush sayaçları — hizalama gerçekten tutuyor mu (§9n) ────────────────
 * Dilim yolunda panel penceresi HER ZAMAN 0..171, yani tanımı gereği hizalı;
 * sayaçlar bunu ölçmeye devam ediyor ki bir gerileme sessizce geçmesin. */
uint32_t pb_lv_flush_count;
uint32_t pb_lv_flush_unaligned;
uint32_t pb_lv_flush_stride_differs;
uint32_t pb_lv_flush_w_min = 0xFFFFFFFF, pb_lv_flush_w_max;
int32_t  pb_lv_last_y1, pb_lv_last_y2, pb_lv_last_x1, pb_lv_last_x2;
int32_t  pb_lv_last_stride_px, pb_lv_last_area_w;

uint32_t pb_lv_slice_press;             /* panele basılan dilim sayısı */

static int s_dump_remaining = 0;

void pb_lv_request_dump(int count) { s_dump_remaining = count; }

void pb_lv_flush_counters_reset(void)
{
    pb_lv_flush_count = 0;
    pb_lv_flush_unaligned = 0;
    pb_lv_flush_stride_differs = 0;
    pb_lv_flush_w_min = 0xFFFFFFFF;
    pb_lv_flush_w_max = 0;
    pb_lv_slice_press = 0;
}

/* ── Geçersizleştirme kancası — hangi dilim kirlendi ──────────────────────
 * LVGL bir etiketi geçersizleştirdiğinde alanı burada görüyoruz ve yalnızca
 * dilim maskesini işaretliyoruz; alana DOKUNMUYORUZ (LVGL kendi kuyruğunu
 * normal işletsin). Gerçek çizim `pb_lv_tick`te dilim dilim yapılıyor. */
static void invalidate_cb(lv_event_t *e)
{
    if (s_drawing) return;              /* kendi dilim isteğimiz — saymayalım */

    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    if (!a) return;

    int32_t x1 = a->x1 < 0 ? 0 : a->x1;
    int32_t x2 = a->x2 > PB_LV_W - 1 ? PB_LV_W - 1 : a->x2;
    if (x1 > x2) return;

    for (int32_t d = x1 / PB_SLICE_W; d <= x2 / PB_SLICE_W; d++) {
        s_dirty |= (1u << d);
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t x1 = area->x1, x2 = area->x2;
    const int32_t y1 = area->y1, y2 = area->y2;
    const int32_t alan_w = x2 - x1 + 1;

    const uint16_t *src = (const uint16_t *)(void *)px_map;

    /* ⚠ SATIR ADIMI ALAN GENİŞLİĞİ DEĞİL — LVGL'e sorulmalı. LVGL çizim
     * tamponunun adımını `LV_DRAW_BUF_STRIDE_ALIGN`e göre yuvarlayabiliyor,
     * dolayısıyla `x2-x1+1` VARSAYMAK yanlış (§9n'de bir tur buna gitti). */
    int32_t row_step = alan_w;
    lv_draw_buf_t *db = lv_display_get_buf_active(disp);
    if (db && db->header.stride) row_step = (int32_t)(db->header.stride / 2);
    pb_lv_last_stride_px = row_step;
    pb_lv_last_area_w = alan_w;
    if (row_step != alan_w) pb_lv_flush_stride_differs++;

    pb_lv_flush_count++;
    pb_lv_last_x1 = x1; pb_lv_last_x2 = x2;
    pb_lv_last_y1 = y1; pb_lv_last_y2 = y2;

    /* ── Etkin dilime KIRP ─────────────────────────────────────────────────
     * `lv_refr_now` yalnızca bizim istediğimiz dilimi değil, LVGL'in o an
     * kuyruğunda bekleyen kendi alanlarını da çiziyor. Dilim dışına düşen
     * her şey burada DÜŞÜRÜLÜYOR: içeriği kaybetmiyoruz, çünkü o alanın
     * düştüğü dilim maskede işaretli ve sırası gelince tamamı çiziliyor.
     * Kırpmadan yazmak framebuffer'ın dışına taşardı. */
    if (s_active_slice < 0) { lv_display_flush_ready(disp); return; }

    const int32_t slice_x0 = s_active_slice * PB_SLICE_W;
    int32_t xb = x1 > slice_x0 ? x1 : slice_x0;
    int32_t xs = x2 < slice_x0 + PB_SLICE_W - 1 ? x2 : slice_x0 + PB_SLICE_W - 1;

    if (xb > xs) { lv_display_flush_ready(disp); return; }

    const uint32_t panel_w = (uint32_t)(y2 - y1 + 1);
    if ((uint32_t)(PB_LCD_H - 1 - y2) & 1u) pb_lv_flush_unaligned++;
    if (panel_w < pb_lv_flush_w_min) pb_lv_flush_w_min = panel_w;
    if (panel_w > pb_lv_flush_w_max) pb_lv_flush_w_max = panel_w;

    /* ── Göz gerektirmeyen yazı teşhisi ───────────────────────────────────
     * Alanı, sürücünün OKUDUĞU indislemeyle seri porta ASCII olarak döküyor.
     * Terminalde yazı düzgün okunuyorsa hem LVGL'in çizimi hem devrik okuma
     * doğru demektir ve bozulma daha aşağıda; okunmuyorsa LVGL tarafında. */
    if (s_dump_remaining > 0 && (xs - xb) < 180) {
        s_dump_remaining--;
        printf("#DOKUM ui x(%ld..%ld) y(%ld..%ld) dilim %ld adim %ld\n",
               (long)xb, (long)xs, (long)y1, (long)y2,
               (long)s_active_slice, (long)row_step);
        for (int32_t y = y1; y <= y2; y++) {
            const uint16_t *s = &src[(size_t)(y - y1) * row_step + (xb - x1)];
            for (int32_t x = xb; x <= xs; x++) {
                const uint16_t px = *s++;
                const uint32_t l = ((px >> 11) & 0x1F) + ((px >> 6) & 0x1F) + (px & 0x1F);
                putchar(l < 6 ? '.' : (l < 24 ? '+' : '#'));
            }
            putchar('\n');
        }
        printf("#DOKUM-SON\n");
    }

    /* Devrik yaz: piksel(panel satırı ui_x, panel sütunu 171-ui_y).
     * Dış döngü ui_y olduğu için KAYNAK ardışık okunuyor (çizim tamponu
     * satır sıralı); hedefte sütun sabit, satır atlıyor. */
    for (int32_t y = y1; y <= y2; y++) {
        const uint16_t *s = &src[(size_t)(y - y1) * row_step + (xb - x1)];
        const uint32_t pc = (uint32_t)(PB_LCD_H - 1 - y);
        for (int32_t x = xb; x <= xs; x++) {
            s_slice_fb[x - slice_x0][pc] = *s++;
        }
    }

    lv_display_flush_ready(disp);
}

/* ── Dokunmatik ───────────────────────────────────────────────────────────
 *
 * EŞLEME ÇALIŞAN SÜRÜCÜDEN ALINDI (rsvpnano/src/drivers/touch/axs15231b_touch
 * /axs15231b_touch.cpp — aynı panel, aynı çip, sahada çalışıyor):
 *
 *     rawLongAxis  = bayt 2,3   ->  panel Y ekseni (0..639), TERS
 *     rawShortAxis = bayt 4,5   ->  panel X ekseni (0..171)
 *     physicalX = rawShort
 *     physicalY = panelHeight - 1 - rawLong
 *
 * Bizim yön çevrimimiz panel_y = ui_x ve panel_x = 171 - ui_y olduğuna göre:
 *
 *     ui_x = 639 - raw_long
 *     ui_y = 171 - raw_short
 *
 * ⚠ ESKİ KOD YANLIŞTI: uzun ekseni (0..639) `PB_LV_W - 1`e, yani 199'a
 * kırpıyordu — ekranın sağ üçte ikisine yapılan her dokunuş sol kenara
 * yığılıyordu. Aynalama da iki ekseni birden çeviriyordu; çalışan sürücü
 * yalnızca uzun ekseni çeviriyor. Dokunmatik hiç parmakla denenmediği için
 * (§9b) bu hata bugüne kadar ortaya çıkmamıştı.
 *
 * Sınır dışı değerler KIRPILMIYOR, REDDEDİLİYOR: çalışan sürücünün gerekçesi
 * aynen geçerli — bozuk paketi kenara kırpmak, bozulmayı "kenarda makul bir
 * dokunuş"a çeviriyor ve sessizce yanlış davranış üretiyor.
 *
 * ✅ KULLANICI DOĞRULADI (`t`, parmakla): dokunmatik ÇALIŞIYOR. §9b'deki
 * "boşta sabit 0xDB" alarmı yanlıştı — çalışan sürücü de parmak sayısı
 * baytını 4'ten büyük görünce "dokunma yok" sayıyor, yani boştaki çöp paket
 * beklenen davranış.
 *
 * ⚠ ÖLÇÜLEN TUHAFLIK: çift dokunuşta ve bazen kaydırma sırasında koordinatlar
 * ~300'den ~4000'e sıçrıyor. Koordinat 12 bit (azami 4095), yani bu değer
 * panelin dışı — bozuk ya da ikinci parmağa ait bir kare. 8 baytlık paket tek
 * nokta taşıyor (çalışan sürücü de öyle), dolayısıyla doğru davranış o kareyi
 * DÜŞÜRMEK. Ne sıklıkta olduğu `pb_lv_touch_invalidate` ile ölçülüyor;
 * kaydırma algılayıcısı da düşen kareye dayanıklı (arayuz.c,
 * PARMAK_BIRAKMA_MS). */
#define PB_TOUCH_TOLERANCE 8

uint32_t pb_lv_touch_invalidate;

bool pb_lv_touch_get(int32_t *ux, int32_t *uy)
{
    pb_touch_state_t st = pb_touch_read();
    if (!st.ok || st.fingers == 0) return false;

    const uint32_t lengthy  = st.p.raw_x;      /* bayt 2,3 — panel Y (0..639) */
    const uint32_t brief  = st.p.raw_y;      /* bayt 4,5 — panel X (0..171) */

    if (lengthy >= (uint32_t)PB_LCD_W + PB_TOUCH_TOLERANCE ||
        brief >= (uint32_t)PB_LCD_H + PB_TOUCH_TOLERANCE) {
        pb_lv_touch_invalidate++;
        return false;
    }

    int32_t x = (int32_t)PB_LCD_W - 1 - (int32_t)lengthy;
    int32_t y = (int32_t)PB_LCD_H - 1 - (int32_t)brief;

    if (x < 0) x = 0; else if (x > PB_LV_W - 1) x = PB_LV_W - 1;
    if (y < 0) y = 0; else if (y > PB_LV_H - 1) y = PB_LV_H - 1;

    *ux = x;
    *uy = y;
    return true;
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int32_t x, y;
    if (!pb_lv_touch_get(&x, &y)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

void pb_lv_set_slice_owner(uint32_t maske)
{
    s_lvgl_slices = maske & ((1u << PB_SLICE_COUNT) - 1u);
}

void pb_lv_invalidate_all(void)
{
    s_dirty = (1u << PB_SLICE_COUNT) - 1u;
}

/** Bir dilimi çiz ve panele bas. */
static void dilimi_head(int32_t d)
{
    s_active_slice = d;

    lv_area_t a = {
        .x1 = d * PB_SLICE_W,
        .y1 = 0,
        .x2 = d * PB_SLICE_W + PB_SLICE_W - 1,
        .y2 = PB_LV_H - 1,
    };

    s_drawing = true;
    lv_obj_invalidate_area(lv_screen_active(), &a);
    lv_refr_now(s_disp);
    s_drawing = false;

    s_active_slice = -1;

    /* Tam genişlik (172 sütun), satır bandı [d*128 .. d*128+127].
     * Panelin sözleşmesine birebir uyan tek geometri (§9n). */
    pb_lcd_blit(0, (uint32_t)(d * PB_SLICE_W), PB_PANEL_W, PB_SLICE_W,
                &s_slice_fb[0][0]);
    pb_lv_slice_press++;
}

void pb_lv_dump_card_fb(void)
{
    /* Her LVGL dilimini sırayla çizip döküyor. Çıktı ARAYÜZ yöneliminde:
     * dilim başına 172 satır x 128 sütun, soldan sağa. Dökümü sürücünün
     * YAZDIĞI eşlemenin tersiyle okuyor, yani eşlemeyi de sınıyor. */
    for (int32_t d = 0; d < PB_SLICE_COUNT; d++) {
        if (!((s_lvgl_slices >> d) & 1u)) continue;

        s_active_slice = d;
        lv_area_t a = { .x1 = d * PB_SLICE_W, .y1 = 0,
                        .x2 = d * PB_SLICE_W + PB_SLICE_W - 1, .y2 = PB_LV_H - 1 };
        s_drawing = true;
        lv_obj_invalidate_area(lv_screen_active(), &a);
        lv_refr_now(s_disp);
        s_drawing = false;
        s_active_slice = -1;

        printf("#KARTFB dilim %ld  ui x %ld..%ld  (%d satir x %d sutun)\n",
               (long)d, (long)(d * PB_SLICE_W),
               (long)(d * PB_SLICE_W + PB_SLICE_W - 1), PB_LV_H, PB_SLICE_W);
        for (int32_t uy = 0; uy < PB_LV_H; uy++) {
            for (int32_t ux = 0; ux < PB_SLICE_W; ux++) {
                const uint16_t px = s_slice_fb[ux][PB_LV_H - 1 - uy];
                const uint32_t l = ((px >> 11) & 0x1F) + ((px >> 6) & 0x1F) + (px & 0x1F);
                putchar(l < 6 ? '.' : (l < 24 ? '+' : '#'));
            }
            putchar('\n');
        }
        printf("#KARTFB-SON\n");
    }
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

    s_disp = lv_display_create(PB_LV_W, PB_LV_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(s_disp, invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    pb_lv_invalidate_all();

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

    /* Kirli ve BİZİM olan dilimleri artan sırada bas. Artan sıra önemli:
     * panel imleci ileri yürüyor, geri atlama olmuyor (§9n). */
    uint32_t is = s_dirty & s_lvgl_slices;
    if (!is) return;

    s_dirty &= ~s_lvgl_slices;

    for (int32_t d = 0; d < PB_SLICE_COUNT; d++) {
        if ((is >> d) & 1u) dilimi_head(d);
    }
}
