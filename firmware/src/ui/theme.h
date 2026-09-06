/**
 * theme.h — the UI's colours, fonts and shared drawing helpers
 *
 * The colours are taken EXACTLY from the design (retro/analog, dark theme).
 * The design's pixel values are the device's pixels directly: the screen is
 * 640x172 and the design draws at 640x172, so there is no scaling in between.
 *
 * WARNING: the fonts under firmware/src/ui/fonts/ are GENERATED files — do
 * not edit them by hand, regenerate them with
 * `python tools/generate_fonts.py`. The design's Oswald + Space Mono pairing
 * lives on Google Fonts; to avoid a download, two of Windows' own fonts that
 * fill the same roles were chosen instead (the reasoning is in
 * generate_fonts.py):
 *
 *     Oswald      -> Liberation Sans Narrow Bold
 *     Space Mono  -> DejaVu Sans Mono (oblique)
 *
 * These fonts do carry the accented Latin letters (verified during
 * generation). Species names are drawn in full, with no ASCII folding.
 */
#ifndef POKEBIRD_THEME_H
#define POKEBIRD_THEME_H

#include "lvgl.h"

/* ── Fonts (firmware/src/ui/fonts/, generated) ───────────────────────────── */
extern const lv_font_t pb_font_name_18;    /* species name — narrow, bold     */
extern const lv_font_t pb_font_bold_13;    /* headings, percentages, log rows */
extern const lv_font_t pb_font_narrow_11;  /* secondary information           */
extern const lv_font_t pb_font_mono_10;    /* scientific name, counters (italic) */

/* ── Colours — from the design ───────────────────────────────────────────── */
#define PB_COLOR_BACKGROUND  0x0B0908   /* screen background                  */
#define PB_COLOR_TEXT        0xE8E2D6   /* primary text                       */
#define PB_COLOR_MUTED       0x8A8072   /* secondary text                     */
#define PB_COLOR_FAINT       0x6F675C   /* tertiary text                      */
#define PB_COLOR_LATIN       0x7A7263   /* scientific name                    */
#define PB_COLOR_LINE        0x2A2620   /* separator                          */
#define PB_COLOR_ROW         0x211D18   /* row underline / bar bed            */
#define PB_COLOR_BORDER      0x3A332A   /* badge border, inactive dot         */
#define PB_COLOR_INACTIVE    0x5F5849   /* inactive bar fill                  */

#define PB_COLOR_ACCENT      0xFFB020   /* amber — the general accent         */
#define PB_COLOR_PEAK        0x7BD88F   /* green — highest-scoring species    */
#define PB_COLOR_RECORD      0xE86A5A   /* red — the "listening" dot          */

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define PB_SCREEN_W   640
#define PB_SCREEN_H   172

/* The spectrogram uses the TWO right-hand slices (128 px each); LVGL owns the
 * three on the left. The design asked for 236 px — 256 is the nearest value
 * that lands on a slice boundary, and landing on one is essential: if LVGL
 * and the spectrogram shared a slice they would erase each other (lv_port.c). */
#define PB_SPEC_SLICE_COUNT  2
#define PB_LVGL_SLICE_MASK_LISTEN  0x07u  /* slices 0,1,2 -> ui x 0..383      */
#define PB_LVGL_SLICE_MASK_ALL     0x1Fu  /* all five belong to LVGL          */

#define PB_LEFT_W     384   /* left column of the listening screen            */
#define PB_MARGIN     18    /* left/right inner padding                       */

/* ── Shared drawing helpers (theme.c) ────────────────────────────────────── */

/** An empty screen object with background, padding and border all zeroed. */
lv_obj_t *pb_screen_new(void);

/** A label positioned from its top-left corner. */
lv_obj_t *pb_label(lv_obj_t *par, const lv_font_t *f, uint32_t color,
                   int32_t x, int32_t y);

/** A flat-colour rectangle — bar, separator line, dot. `radius` rounds it. */
lv_obj_t *pb_box(lv_obj_t *par, int32_t x, int32_t y, int32_t w, int32_t h,
                 uint32_t color, int32_t radius);

/**
 * Write text to a label ONLY IF IT CHANGED.
 *
 * `lv_label_set_text` dirties the object even when the text is identical, and
 * a dirty area means that slice gets pushed over QSPI again (44 KB). The
 * screen updates at 4 Hz, so the cost of a pointless push is real.
 *
 * @return true if the text changed (the caller may want to update the colour
 *         as well).
 */
bool pb_write(lv_obj_t *o, char *last, uint32_t n, const char *text);

/** The page dots at bottom centre — the only hint that there are two screens. */
void pb_page_dots(lv_obj_t *par, int active);

#endif /* POKEBIRD_THEME_H */
