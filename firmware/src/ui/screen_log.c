#include "ui/screen_log.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ui/text.h"
#include "ui/theme.h"

/* ── Yerleşim (tam 640) ───────────────────────────────────────────────────
 *     y   8   başlık "BUGÜN · TODAY"          sağda kayıt sayısı
 *     y  30   ayraç
 *     y  38   1. kayıt  \
 *     y  76   2. kayıt   >  satır yüksekliği 38 (ad + bilimsel ad alt alta)
 *     y 114   3. kayıt  /
 *     y 150   sayaçlar (göz gerektirmeyen doğrulama)
 *     y 162   sayfa noktaları
 */
#define ROW_Y0    38
#define ROW_STEP  38
#define VISIBLE_ROWS  3

#define LEFT         20
#define RIGHT         620
#define TIME_X      LEFT
#define TIME_W      86
#define NAME_X        (LEFT + 96)
#define NAME_W        300
#define BAR_X     440
#define BAR_W     120
#define PERCENT_X     572
#define PERCENT_W     48

/* ── Kayıt halkası ────────────────────────────────────────────────────────
 * SD kart günlüğü gelene kadar (M7 adım 5) tespitler RAM'de duruyor. Sekiz
 * yeter: ekranda üçü görünüyor, kalanı "kaydırınca daha fazlası" için değil,
 * yalnızca en yenisinin doğru seçilebilmesi için. Maliyet ~800 bayt. */
#define RING 8

typedef struct {
    char     name[48];
    char     latin[40];
    float    confidence;
    uint32_t ms;            /* açılıştan bu yana */
    bool     full;
} record_t;

static record_t s_record[RING];
static uint32_t s_head;          /* en yeni kaydın indeksi + 1 (mod HALKA) */
static uint32_t s_total;

typedef struct {
    lv_obj_t *time, *name, *latin, *bar, *percent;
    char last_time[24], last_name[64], last_latin[48], last_percent[12];
    int32_t last_bar_w;
} row_t;

static lv_obj_t *s_screen, *s_count, *s_empty, *s_counter;
static row_t   s_row[VISIBLE_ROWS];
static char      s_last_count[24], s_last_counter[64];

static void row_kur(row_t *s, int32_t y)
{
    s->time = pb_label(s_screen, &pb_font_mono_10, PB_COLOR_FAINT, TIME_X, y + 4);
    lv_obj_set_width(s->time, TIME_W);

    /* ⚠ Yükseklik de veriliyor — yalnızca genişlikle `DOTS` kesmiyor, uzun ad
     * iki satıra sarıp bilimsel adın üstüne biniyor (bkz. ekran_dinleme.c). */
    s->name = pb_label(s_screen, &pb_font_bold_13, PB_COLOR_TEXT, NAME_X, y);
    lv_obj_set_size(s->name, NAME_W, 16);
    lv_label_set_long_mode(s->name, LV_LABEL_LONG_MODE_DOTS);

    s->latin = pb_label(s_screen, &pb_font_mono_10, PB_COLOR_LATIN, NAME_X + 1, y + 16);
    lv_obj_set_size(s->latin, NAME_W, 13);
    lv_label_set_long_mode(s->latin, LV_LABEL_LONG_MODE_DOTS);

    pb_box(s_screen, BAR_X, y + 8, BAR_W, 5, PB_COLOR_ROW, 3);
    s->bar = pb_box(s_screen, BAR_X, y + 8, 0, 5, PB_COLOR_ACCENT, 3);

    s->percent = pb_label(s_screen, &pb_font_bold_13, PB_COLOR_ACCENT, PERCENT_X, y + 1);
    lv_obj_set_width(s->percent, PERCENT_W);
    lv_obj_set_style_text_align(s->percent, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s->last_time[0] = s->last_name[0] = s->last_latin[0] = s->last_percent[0] = '\0';
    s->last_bar_w = -1;
}

lv_obj_t *pb_screen_log_create(void)
{
    s_screen = pb_screen_new();

    lv_obj_t *title = pb_label(s_screen, &pb_font_bold_13, PB_COLOR_TEXT, LEFT, 8);
    lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
    lv_label_set_text(title, "TODAY");

    s_count = pb_label(s_screen, &pb_font_narrow_11, PB_COLOR_FAINT,
                       RIGHT - 120, 10);
    lv_obj_set_width(s_count, 120);
    lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    pb_box(s_screen, LEFT, 30, RIGHT - LEFT, 1, PB_COLOR_LINE, 0);

    for (int i = 0; i < VISIBLE_ROWS; i++) {
        row_kur(&s_row[i], ROW_Y0 + i * ROW_STEP);
    }

    /* Boş durum — cihaz yeni açıldığında ekranın anlamsız görünmemesi için. */
    s_empty = pb_label(s_screen, &pb_font_narrow_11, PB_COLOR_FAINT, LEFT, 84);
    lv_obj_set_width(s_empty, RIGHT - LEFT);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_empty, "No entries yet · listening");

    /* Sayaçlar tasarımda YOK; buraya konuldu çünkü §9p'de bilerek ekrana
     * yazılmışlardı (göz gerektirmeyen doğrulama). Dinleme ekranını
     * kalabalıklaştırmamak için ikinci ekranın altına alındılar. */
    s_counter = pb_label(s_screen, &pb_font_mono_10, PB_COLOR_BORDER, LEFT, 150);
    lv_obj_set_width(s_counter, 400);

    pb_page_dots(s_screen, 1);

    s_last_count[0] = s_last_counter[0] = '\0';
    return s_screen;
}

void pb_screen_log_add(const char *name, const char *latin, float confidence)
{
    if (!name || !name[0]) return;

    const uint32_t now = to_ms_since_boot(get_absolute_time());

    /* Aynı tür üst üste gelirse yeni satır AÇMA, en üsttekini tazele: karar
     * kuralı bir türü PB_DECISION_HOLD_MS boyunca ekranda tutuyor ve o süre
     * boyunca aynı tespit tekrar tekrar düşerse günlük tek olayla dolardı. */
    if (s_total > 0) {
        record_t *top = &s_record[(s_head + RING - 1) % RING];
        if (strncmp(top->name, name, sizeof(top->name) - 1) == 0) {
            if (confidence > top->confidence) top->confidence = confidence;
            top->ms = now;
            return;
        }
    }

    record_t *k = &s_record[s_head];
    snprintf(k->name, sizeof(k->name), "%s", name);
    snprintf(k->latin, sizeof(k->latin), "%s", latin ? latin : "");
    k->confidence = confidence;
    k->ms = now;
    k->full = true;

    s_head = (s_head + 1) % RING;
    if (s_total < 0xFFFFFFFFu) s_total++;
}

/** "az önce" / "3 dk önce" / "2 sa önce" — RTC yok, açılıştan bu yana. */
static void time_write(uint32_t elapsed_ms, char *out, uint32_t n)
{
    const uint32_t sn = elapsed_ms / 1000u;
    if (sn < 60u)        snprintf(out, n, "just now");
    else if (sn < 3600u) snprintf(out, n, "%lu min ago", (unsigned long)(sn / 60u));
    else                 snprintf(out, n, "%lu hr ago", (unsigned long)(sn / 3600u));
}

void pb_screen_log_refresh(uint32_t frame_rate, uint32_t inference, uint32_t overrun)
{
    if (!s_screen) return;

    const uint32_t now = to_ms_since_boot(get_absolute_time());

    char buf[80];
    snprintf(buf, sizeof(buf), "%lu entries", (unsigned long)s_total);
    pb_write(s_count, s_last_count, sizeof(s_last_count), buf);

    if (s_total == 0) {
        lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < VISIBLE_ROWS; i++) {
        row_t *s = &s_row[i];
        /* i=0 en yeni: halkada bir geri. */
        const record_t *k = &s_record[(s_head + RING - 1 - (uint32_t)i) % RING];

        if (!k->full || (uint32_t)i >= s_total) {
            pb_write(s->time,  s->last_time,  sizeof(s->last_time),  "");
            pb_write(s->name,    s->last_name,    sizeof(s->last_name),    "");
            pb_write(s->latin, s->last_latin, sizeof(s->last_latin), "");
            pb_write(s->percent, s->last_percent, sizeof(s->last_percent), "");
            if (s->last_bar_w != 0) { s->last_bar_w = 0; lv_obj_set_width(s->bar, 0); }
            continue;
        }

        time_write(now - k->ms, buf, sizeof(buf));
        pb_write(s->time, s->last_time, sizeof(s->last_time), buf);

        pb_text_upper(k->name, buf, sizeof(buf));
        pb_write(s->name, s->last_name, sizeof(s->last_name), buf);

        pb_write(s->latin, s->last_latin, sizeof(s->last_latin), k->latin);

        snprintf(buf, sizeof(buf), "%d%%", (int)(k->confidence * 100.0f + 0.5f));
        pb_write(s->percent, s->last_percent, sizeof(s->last_percent), buf);

        int32_t w = (int32_t)(k->confidence * (float)BAR_W + 0.5f);
        if (w > BAR_W) w = BAR_W;
        if (w != s->last_bar_w) { s->last_bar_w = w; lv_obj_set_width(s->bar, w); }
    }

    snprintf(buf, sizeof(buf), "%lu fps · inference %lu · overrun %lu",
             (unsigned long)frame_rate, (unsigned long)inference,
             (unsigned long)overrun);
    pb_write(s_counter, s_last_counter, sizeof(s_last_counter), buf);
}
