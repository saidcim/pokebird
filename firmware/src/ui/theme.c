#include "ui/theme.h"

#include <stdbool.h>
#include <string.h>

lv_obj_t *pb_screen_new(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(PB_COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(scr, 0, LV_PART_MAIN);
    /* Swiping is NOT LVGL's own scrolling — we drive the screen change
     * ourselves (interface.c). Letting objects scroll would break the
     * layout. */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *pb_label(lv_obj_t *par, const lv_font_t *f, uint32_t color,
                    int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *pb_box(lv_obj_t *par, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    return o;
}

bool pb_write(lv_obj_t *o, char *last, uint32_t n, const char *text)
{
    if (!o || !last || n == 0) return false;
    if (strncmp(last, text, n - 1) == 0) return false;
    /* Done by hand rather than with strncpy: this guarantees termination. */
    uint32_t i = 0;
    for (; i + 1 < n && text[i]; i++) last[i] = text[i];
    last[i] = '\0';
    lv_label_set_text(o, last);
    return true;
}

void pb_page_dots(lv_obj_t *par, int active)
{
    /* The device has no other controls: these two dots are the only way to
     * tell the user a second screen exists. The design showed them on the log
     * screen only; they are on both here, because otherwise nothing on the
     * listening screen hints that it can be swiped. */
    const int32_t w = 16, h = 3, gap = 6;
    const int32_t total = 2 * w + gap;
    const int32_t x0 = (PB_SCREEN_W - total) / 2;
    const int32_t y  = PB_SCREEN_H - 10;

    for (int i = 0; i < 2; i++) {
        pb_box(par, x0 + i * (w + gap), y, w, h,
                i == active ? PB_COLOR_ACCENT : PB_COLOR_BORDER, 2);
    }
}
