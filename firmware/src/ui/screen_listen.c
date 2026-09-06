#include "ui/screen_listen.h"

#include <stdio.h>
#include <string.h>

#include "ui/text.h"
#include "ui/theme.h"

/* ── Layout — rebuilt around the RECORD BUTTON ────────────────────────────
 *
 * The device no longer listens continuously; the user starts it. The screen
 * follows: a full-height record button on the left, one PROMINENT result in
 * the middle, and the 2nd and 3rd guesses compacted to one line each below.
 *
 * WHY THE BUTTON IS A FULL-HEIGHT STRIP — this is measured, not arbitrary.
 * Touch calibration showed the short axis to be UNUSABLE: it moves by only 14
 * units across the whole screen (the case lip presumably blocks touches near
 * the top and bottom edges). So we cannot know the VERTICAL position of a
 * touch, only the HORIZONTAL one. A full-height strip is the only button
 * shape that can be hit reliably from horizontal position alone.
 *
 *     x   0..95    BUTTON
 *     x  96..383   content
 *     x 384..639   spectrogram (LVGL does not touch it)
 *
 *     y   8   status row
 *     y  32   1st guess: name (+ confidence on the right)
 *     y  54   scientific name
 *     y  74   confidence bar
 *     y  98   2nd guess  (one line)
 *     y 116   3rd guess  (one line)
 *     y 162   page dots (theme.c)
 */
#define BUTTON_W      96
#define INNER_X         102                 /* left edge of the content        */
#define INNER_RIGHT       378                 /* right edge of the content       */

#define NAME_W         200
#define NAME_H         21
#define LATIN_H      13
#define PERCENT_X      306
#define PERCENT_W      (INNER_RIGHT - PERCENT_X)

#define BAR_Y      74
#define BAR_H      6
#define BAR_W      (INNER_RIGHT - INNER_X)

#define BOTTOM_Y0       98
#define BOTTOM_STEP     18
#define BOTTOM_NAME_W     206
#define BOTTOM_PERCENT_X  312

static lv_obj_t *s_screen;
static lv_obj_t *s_dot, *s_state, *s_empty;
static lv_obj_t *s_name, *s_latin, *s_percent, *s_bar, *s_bar_bed, *s_divider;
static lv_obj_t *s_bottom[2], *s_bottom_percent[2];

/* Button parts */
static lv_obj_t *s_button, *s_button_marker, *s_button_text;

static char s_last_state[32], s_last_name[80], s_last_latin[64], s_last_percent[12];
static char s_last_bottom[2][64], s_last_bottom_percent[2][12];
static int32_t s_last_bar_w = -1;
static uint32_t s_last_color = 0xFFFFFFFFu;
static const lv_font_t *s_last_font;
static int s_last_list = -1;
static int s_last_record = -1;

/** Show or hide the whole result area (used when there is no candidate). */
static void result_show(bool show)
{
    lv_obj_t *all[] = { s_name, s_latin, s_percent, s_bar, s_bar_bed,
                          s_divider, s_bottom[0], s_bottom[1],
                          s_bottom_percent[0], s_bottom_percent[1] };
    for (uint32_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (show) lv_obj_clear_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void button_build(void)
{
    /* Outer frame — a full-height strip. */
    s_button = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_button);
    lv_obj_set_pos(s_button, 10, 14);
    lv_obj_set_size(s_button, BUTTON_W - 22, PB_SCREEN_H - 28);
    lv_obj_set_style_radius(s_button, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_button, LV_OBJ_FLAG_SCROLLABLE);

    /* The marker: a circle when idle (record), a square while listening
     * (stop). Rather than swapping shapes we change the RADIUS — one object,
     * one draw. */
    s_button_marker = lv_obj_create(s_button);
    lv_obj_remove_style_all(s_button_marker);
    lv_obj_set_size(s_button_marker, 26, 26);
    lv_obj_align(s_button_marker, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_opa(s_button_marker, LV_OPA_COVER, LV_PART_MAIN);

    s_button_text = lv_label_create(s_button);
    lv_obj_set_style_text_font(s_button_text, &pb_font_bold_13, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_button_text, 2, LV_PART_MAIN);
    lv_obj_align(s_button_text, LV_ALIGN_TOP_MID, 0, 76);
    lv_label_set_text(s_button_text, "RECORD");
}

lv_obj_t *pb_screen_listen_create(void)
{
    s_screen = pb_screen_new();

    button_build();

    /* A thin separator between the button and the content. */
    pb_box(s_screen, BUTTON_W - 2, 14, 1, PB_SCREEN_H - 28, PB_COLOR_ROW, 0);

    /* ── Status row ──
     * The dot does NOT blink via an LVGL animation: an animation would
     * invalidate it every frame and push the slice to the panel at 60 Hz. Its
     * colour changes with the mode, which is enough. */
    s_dot = pb_box(s_screen, INNER_X, 11, 7, 7, PB_COLOR_FAINT, 4);
    s_state = pb_label(s_screen, &pb_font_bold_13, PB_COLOR_MUTED, INNER_X + 14, 6);
    lv_obj_set_style_text_letter_space(s_state, 3, LV_PART_MAIN);

    /* ── 1st guess — the prominent result ── */
    s_name = pb_label(s_screen, &pb_font_name_18, PB_COLOR_TEXT, INNER_X, 30);
    lv_obj_set_size(s_name, NAME_W, NAME_H);
    /* A height is set as well: with a width alone `DOTS` does not truncate,
     * and a long name wraps onto a second line and overlaps what is below
     * it. */
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);

    s_latin = pb_label(s_screen, &pb_font_mono_10, PB_COLOR_LATIN, INNER_X + 1, 54);
    lv_obj_set_size(s_latin, NAME_W, LATIN_H);
    lv_label_set_long_mode(s_latin, LV_LABEL_LONG_MODE_DOTS);

    s_percent = pb_label(s_screen, &pb_font_name_18, PB_COLOR_ACCENT, PERCENT_X, 30);
    lv_obj_set_width(s_percent, PERCENT_W);
    lv_obj_set_style_text_align(s_percent, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s_bar_bed = pb_box(s_screen, INNER_X, BAR_Y, BAR_W, BAR_H,
                            PB_COLOR_ROW, 3);
    s_bar = pb_box(s_screen, INNER_X, BAR_Y, 0, BAR_H, PB_COLOR_ACCENT, 3);

    s_divider = pb_box(s_screen, INNER_X, 90, BAR_W, 1, PB_COLOR_ROW, 0);

    /* ── 2nd and 3rd guesses — one compact line each ──
     * The UI is meant to show the top 3, not a single answer. Top-1 is 70.4%
     * but top-3 is 82.2%, so these two rows genuinely carry information —
     * they just must not compete visually with the first. */
    for (int i = 0; i < 2; i++) {
        const int32_t y = BOTTOM_Y0 + i * BOTTOM_STEP;
        s_bottom[i] = pb_label(s_screen, &pb_font_narrow_11, PB_COLOR_MUTED, INNER_X, y);
        lv_obj_set_size(s_bottom[i], BOTTOM_NAME_W, 14);
        lv_label_set_long_mode(s_bottom[i], LV_LABEL_LONG_MODE_DOTS);

        s_bottom_percent[i] = pb_label(s_screen, &pb_font_narrow_11, PB_COLOR_FAINT,
                                   BOTTOM_PERCENT_X, y);
        lv_obj_set_width(s_bottom_percent[i], INNER_RIGHT - BOTTOM_PERCENT_X);
        lv_obj_set_style_text_align(s_bottom_percent[i], LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        s_last_bottom[i][0] = s_last_bottom_percent[i][0] = '\0';
    }

    /* Empty state — showing three blank rows when there is no result would
     * be meaningless. */
    s_empty = pb_label(s_screen, &pb_font_narrow_11, PB_COLOR_FAINT, INNER_X, 74);
    lv_obj_set_width(s_empty, BAR_W);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* The thin separator against the spectrogram (the design's border-left). */
    pb_box(s_screen, PB_LEFT_W - 1, 10, 1, PB_SCREEN_H - 28, PB_COLOR_LINE, 0);

    pb_page_dots(s_screen, PB_SCREEN_LISTEN);

    s_last_state[0] = s_last_name[0] = s_last_latin[0] = s_last_percent[0] = '\0';
    s_last_font = &pb_font_name_18;
    return s_screen;
}

void pb_screen_listen_set_recording(bool recording)
{
    if (!s_button || (int)recording == s_last_record) return;
    s_last_record = (int)recording;

    /* Listening: red fill + SQUARE (stop). Idle: hollow frame + CIRCLE
     * (record). Colour alone is not enough — on a small screen in daylight
     * the difference in shape reads far more reliably.
     *
     * The marker is red in BOTH states: an invitation to press when idle, a
     * "recording" warning while listening. The distinction is carried by the
     * SHAPE and the FRAME — a pale grey circle made the button look
     * disabled. */
    const uint32_t frame = recording ? PB_COLOR_RECORD : PB_COLOR_BORDER;
    const uint32_t text    = recording ? PB_COLOR_RECORD : PB_COLOR_MUTED;

    lv_obj_set_style_border_color(s_button, lv_color_hex(frame), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(PB_COLOR_BACKGROUND), LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_button_marker, lv_color_hex(PB_COLOR_RECORD),
                              LV_PART_MAIN);
    lv_obj_set_style_radius(s_button_marker, recording ? 3 : 13, LV_PART_MAIN);

    lv_obj_set_style_text_color(s_button_text, lv_color_hex(text), LV_PART_MAIN);
    lv_label_set_text(s_button_text, recording ? "STOP" : "RECORD");
}

void pb_screen_listen_update(const pb_result_view_t *view)
{
    if (!view || !s_screen) return;

    const bool species  = (view->mode == PB_DECISION_SPECIES);
    const bool unsure   = (view->mode == PB_DECISION_UNSURE);
    const bool sound    = (view->mode == PB_DECISION_SOUND);
    const bool recording  = (s_last_record == 1);

    /* ── Status row ── */
    const char *state = !recording ? "IDLE"
                      : species ? "IDENTIFIED"
                      : unsure ? "MAYBE..."
                      : sound ? "SOUND DETECTED"
                            : "LISTENING";
    const uint32_t accent = !recording ? PB_COLOR_BORDER
                         : species ? PB_COLOR_PEAK
                         : unsure ? PB_COLOR_ACCENT
                         : sound ? PB_COLOR_PEAK
                               : PB_COLOR_RECORD;

    pb_write(s_state, s_last_state, sizeof(s_last_state), state);
    if (accent != s_last_color) {
        s_last_color = accent;
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(accent), LV_PART_MAIN);
    }

    /* ── Is there a result at all ── */
    const int list = (view->top3_name[0] != NULL) ? 1 : 0;
    if (list != s_last_list) {
        s_last_list = list;
        result_show(list == 1);
        if (list) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (!list) {
        lv_label_set_text(s_empty,
            recording ? "listening..." : "press the button to record");
        return;
    }

    /* ── 1st guess ── */
    char buf[80];
    pb_text_upper(view->top3_name[0], buf, sizeof(buf));

    /* Rather than TRUNCATE a name that does not fit, SHRINK it first: a few
     * of the 178 species do not fit at 18 px, and one size down they read in
     * full. */
    const lv_font_t *font =
        (lv_text_get_width(buf, (uint32_t)strlen(buf), &pb_font_name_18, 0) > NAME_W)
            ? &pb_font_bold_13 : &pb_font_name_18;
    if (font != s_last_font) {
        s_last_font = font;
        lv_obj_set_style_text_font(s_name, font, LV_PART_MAIN);
    }
    pb_write(s_name, s_last_name, sizeof(s_last_name), buf);
    pb_write(s_latin, s_last_latin, sizeof(s_last_latin),
           view->top3_latin[0] ? view->top3_latin[0] : "");

    char y[12];
    snprintf(y, sizeof(y), "%d%%", (int)(view->top3_probability[0] * 100.0f + 0.5f));
    pb_write(s_percent, s_last_percent, sizeof(s_last_percent), y);

    /* Accent colour only when the decision rule is actually claiming
     * something. Otherwise neutral — the screen must not imply a match. */
    const uint32_t name_color = species ? PB_COLOR_PEAK
                           : unsure ? PB_COLOR_ACCENT : PB_COLOR_TEXT;
    lv_obj_set_style_text_color(s_name, lv_color_hex(name_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_percent, lv_color_hex(name_color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(name_color), LV_PART_MAIN);

    int32_t w = (int32_t)(view->top3_probability[0] * (float)BAR_W + 0.5f);
    if (w > BAR_W) w = BAR_W;
    if (w != s_last_bar_w) { s_last_bar_w = w; lv_obj_set_width(s_bar, w); }

    /* ── 2nd and 3rd guesses ── */
    for (int i = 0; i < 2; i++) {
        const char *raw = view->top3_name[i + 1];
        char row[80];
        if (raw) {
            char b2[64];
            pb_text_upper(raw, b2, sizeof(b2));
            snprintf(row, sizeof(row), "%d. %s", i + 2, b2);
        } else {
            row[0] = '\0';
        }
        pb_write(s_bottom[i], s_last_bottom[i], sizeof(s_last_bottom[i]), row);

        char y2[12];
        if (raw) snprintf(y2, sizeof(y2), "%d%%",
                          (int)(view->top3_probability[i + 1] * 100.0f + 0.5f));
        else     y2[0] = '\0';
        pb_write(s_bottom_percent[i], s_last_bottom_percent[i], sizeof(s_last_bottom_percent[i]), y2);
    }
}
