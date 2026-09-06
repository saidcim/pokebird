#include "ui/lv_port.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "hal/display/lcd_blit.h"
#include "hal/touch.h"

/* ── The SLICE (slab) path — the UI is now FULL WIDTH, 640x172 ────────────
 *
 * PREVIOUSLY: LVGL drew only the left 200 columns and pushed them to the
 * panel through a 200x172 card framebuffer (68.8 KB). The reason: writing
 * **multiple rows into a narrow column band** SLIPS by one row on this panel,
 * and the only geometry that holds is a **full-width (172 column) row band**.
 *
 * A full-screen framebuffer was rejected for size — 640x172x2 = 220 KB.
 * MEASURED (arm-none-eabi-nm, before this change): bss ends at 0x2005f400,
 * leaving about 131 KB free in main SRAM. Even giving back the card
 * framebuffer (68.8 KB) and the draw buffer (8 KB), 220 KB DOES NOT FIT. So
 * the framebuffer path cannot simply be widened to full screen.
 *
 * THE WAY OUT — the panel's contract RELAXED after that was written:
 * `skip_with_strip` (lcd_blit.c) made cursor positioning GENERAL PURPOSE and
 * INVISIBLE. The panel's real requirement is now only this:
 *
 *     "push exactly 172 columns wide; the starting row is free"
 *
 * In UI coordinates that requirement means a **FULL-HEIGHT VERTICAL SLICE**
 * (ui_y 0..171 always included, the ui_x range free). So if we cut the screen
 * into vertical slices and draw each separately, we get full width without
 * ever violating the panel's contract.
 *
 *     640 = 5 slices x 128 pixels
 *     slice framebuffer  128 x 172 x 2 =  44,032 bytes
 *     draw buffer        128 x  43 x 2 =  11,008 bytes
 *     ------------------------------------------------
 *     total                               55,040 bytes
 *
 * The old path was 68,800 + 8,000 = 76,800 bytes. So going from 200 to 640
 * columns actually REDUCES RAM by 21.7 KB.
 *
 * Slices are pushed in ASCENDING order: because the panel cursor walks
 * forward, the repositioning between them is either free (RAMWRC) or 2*y
 * pixels via `skip_with_strip` — measurably small and invisible. */
#define PB_LV_W          PB_LCD_W        /* 640 — UI width                   */
#define PB_LV_H          PB_LCD_H        /* 172 — UI height                  */

#define PB_SLICE_W       128
#define PB_SLICE_COUNT  (PB_LV_W / PB_SLICE_W)   /* 5 */

/* Held in panel orientation ([panel row][panel column]) so pushing to the
 * panel needs neither a transpose nor a strided read: a row band is
 * contiguous as it stands. */
static uint16_t s_slice_fb[PB_SLICE_W][PB_PANEL_W];

/* 172 = 4 x 43, so a slice is drawn in exactly four strips with no remainder
 * and no short final strip. */
#define LV_STRIP_H  43
static uint16_t s_draw_buf[PB_SLICE_W * LV_STRIP_H];

static lv_display_t *s_disp;

/* Which slices LVGL draws. The spectrogram writes to the panel DIRECTLY (it
 * has its own fast column path at 62 Hz); LVGL must not push its slices or
 * the two writers would erase each other in the same region. The screens
 * declare this themselves. */
static uint32_t s_lvgl_slices = (1u << PB_SLICE_COUNT) - 1u;

/* Slices that need redrawing, fed from LVGL's own invalidation (the event
 * hook below): only the slice a changed label falls into gets pushed. The
 * card updates at 4 Hz and each slice is 44 KB of QSPI, so pushing all of
 * them would cost three times as much for nothing. */
static uint32_t s_dirty;
static bool     s_drawing;             /* so we do not count our own invalidate */
static int32_t  s_active_slice = -1;    /* which slice flush_cb is writing into  */

/* ── Orientation mapping ──────────────────────────────────────────────────
 * Measured on the board (the `o` command). The device is held in landscape
 * with the USB socket on the RIGHT:
 *
 *     panel_y = ui_x            (panel Y+  =  physical left -> right)
 *     panel_x = 171 - ui_y      (panel X+  =  physical bottom -> top)
 */

/* ── Flush counters — does the alignment really hold ──────────────────────
 * On the slice path the panel window is ALWAYS 0..171, so it is aligned by
 * construction; the counters keep measuring it anyway so a regression cannot
 * slip through silently. */
uint32_t pb_lv_flush_count;
uint32_t pb_lv_flush_unaligned;
uint32_t pb_lv_flush_stride_differs;
uint32_t pb_lv_flush_w_min = 0xFFFFFFFF, pb_lv_flush_w_max;
int32_t  pb_lv_last_y1, pb_lv_last_y2, pb_lv_last_x1, pb_lv_last_x2;
int32_t  pb_lv_last_stride_px, pb_lv_last_area_w;

uint32_t pb_lv_slice_press;             /* slices pushed to the panel */

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

/* ── The invalidation hook — which slice got dirty ────────────────────────
 * When LVGL invalidates a label we see the area here and only mark the slice
 * mask; we do NOT touch the area itself (LVGL should run its own queue
 * normally). The actual drawing happens slice by slice in `pb_lv_tick`. */
static void invalidate_cb(lv_event_t *e)
{
    if (s_drawing) return;          /* our own slice request — do not count it */

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
    const int32_t area_w = x2 - x1 + 1;

    const uint16_t *src = (const uint16_t *)(void *)px_map;

    /* THE ROW STRIDE IS NOT THE AREA WIDTH — LVGL has to be asked. LVGL may
     * round the draw buffer's stride up to `LV_DRAW_BUF_STRIDE_ALIGN`, so
     * ASSUMING `x2-x1+1` is wrong (a whole debugging round went to this). */
    int32_t row_step = area_w;
    lv_draw_buf_t *db = lv_display_get_buf_active(disp);
    if (db && db->header.stride) row_step = (int32_t)(db->header.stride / 2);
    pb_lv_last_stride_px = row_step;
    pb_lv_last_area_w = area_w;
    if (row_step != area_w) pb_lv_flush_stride_differs++;

    pb_lv_flush_count++;
    pb_lv_last_x1 = x1; pb_lv_last_x2 = x2;
    pb_lv_last_y1 = y1; pb_lv_last_y2 = y2;

    /* ── CLIP to the active slice ─────────────────────────────────────────
     * `lv_refr_now` draws not only the slice we asked for but also whatever
     * areas LVGL had queued of its own. Anything falling outside the slice is
     * DROPPED here: no content is lost, because the slice that area belongs
     * to is marked in the mask and gets drawn in full when its turn comes.
     * Writing without clipping would overrun the framebuffer. */
    if (s_active_slice < 0) { lv_display_flush_ready(disp); return; }

    const int32_t slice_x0 = s_active_slice * PB_SLICE_W;
    int32_t xb = x1 > slice_x0 ? x1 : slice_x0;
    int32_t xs = x2 < slice_x0 + PB_SLICE_W - 1 ? x2 : slice_x0 + PB_SLICE_W - 1;

    if (xb > xs) { lv_display_flush_ready(disp); return; }

    const uint32_t panel_w = (uint32_t)(y2 - y1 + 1);
    if ((uint32_t)(PB_LCD_H - 1 - y2) & 1u) pb_lv_flush_unaligned++;
    if (panel_w < pb_lv_flush_w_min) pb_lv_flush_w_min = panel_w;
    if (panel_w > pb_lv_flush_w_max) pb_lv_flush_w_max = panel_w;

    /* ── Text diagnostics that need no eyes ───────────────────────────────
     * Dumps the area to the serial console as ASCII, using the same indexing
     * the driver READS with. If the text is legible in the terminal then both
     * LVGL's drawing and the transposed read are correct and the corruption
     * is further down; if it is not, the problem is on the LVGL side. */
    if (s_dump_remaining > 0 && (xs - xb) < 180) {
        s_dump_remaining--;
        printf("#DUMP ui x(%ld..%ld) y(%ld..%ld) slice %ld stride %ld\n",
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
        printf("#DUMP-END\n");
    }

    /* Transposed write: pixel(panel row ui_x, panel column 171-ui_y).
     * The outer loop is ui_y, so the SOURCE is read sequentially (the draw
     * buffer is row-major); at the destination the column is fixed and the
     * row strides. */
    for (int32_t y = y1; y <= y2; y++) {
        const uint16_t *s = &src[(size_t)(y - y1) * row_step + (xb - x1)];
        const uint32_t pc = (uint32_t)(PB_LCD_H - 1 - y);
        for (int32_t x = xb; x <= xs; x++) {
            s_slice_fb[x - slice_x0][pc] = *s++;
        }
    }

    lv_display_flush_ready(disp);
}

/* ── Touch ────────────────────────────────────────────────────────────────
 *
 * THE MAPPING WAS TAKEN FROM A WORKING DRIVER (rsvpnano's
 * axs15231b_touch.cpp — same panel, same chip, working in the field):
 *
 *     rawLongAxis  = bytes 2,3   ->  panel Y axis (0..639), INVERTED
 *     rawShortAxis = bytes 4,5   ->  panel X axis (0..171)
 *     physicalX = rawShort
 *     physicalY = panelHeight - 1 - rawLong
 *
 * Since our orientation mapping is panel_y = ui_x and panel_x = 171 - ui_y:
 *
 *     ui_x = 639 - raw_long
 *     ui_y = 171 - raw_short
 *
 * THE OLD CODE WAS WRONG: it clipped the long axis (0..639) to `PB_LV_W - 1`,
 * that is 199 — so every touch on the right two thirds of the screen piled up
 * against the left edge. It also mirrored both axes, where the working driver
 * mirrors only the long one. Because touch had never been tried with a
 * finger, the bug had gone unnoticed.
 *
 * Out-of-range values are REJECTED, not CLIPPED: the working driver's
 * reasoning applies unchanged — clipping a corrupt packet to the edge turns
 * corruption into "a plausible touch near the edge" and produces silently
 * wrong behaviour.
 *
 * CONFIRMED BY HAND (`t`, with a finger): touch WORKS. The earlier "constant
 * 0xDB when idle" alarm was a false one — the working driver also treats a
 * finger-count byte above 4 as "no touch", so the junk packet when idle is
 * expected behaviour.
 *
 * A MEASURED ODDITY: on a double tap, and sometimes mid-swipe, the
 * coordinates jump from ~300 to ~4000. The coordinate is 12 bits (max 4095),
 * so that value is off-panel — a corrupt frame, or one belonging to a second
 * finger. The 8-byte packet carries a single point (as does the working
 * driver), so the correct response is to DROP that frame. How often it
 * happens is measured by `pb_lv_touch_invalidate`, and the swipe detector is
 * itself tolerant of a dropped frame (interface.c, FINGER_RELEASE_MS). */
#define PB_TOUCH_TOLERANCE 8

uint32_t pb_lv_touch_invalidate;

bool pb_lv_touch_get(int32_t *ux, int32_t *uy)
{
    pb_touch_state_t st = pb_touch_read();
    if (!st.ok || st.fingers == 0) return false;

    const uint32_t long_axis  = st.p.raw_x;   /* bytes 2,3 — panel Y (0..639) */
    const uint32_t short_axis = st.p.raw_y;   /* bytes 4,5 — panel X (0..171) */

    if (long_axis >= (uint32_t)PB_LCD_W + PB_TOUCH_TOLERANCE ||
        short_axis >= (uint32_t)PB_LCD_H + PB_TOUCH_TOLERANCE) {
        pb_lv_touch_invalidate++;
        return false;
    }

    int32_t x = (int32_t)PB_LCD_W - 1 - (int32_t)long_axis;
    int32_t y = (int32_t)PB_LCD_H - 1 - (int32_t)short_axis;

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

void pb_lv_set_slice_owner(uint32_t mask)
{
    s_lvgl_slices = mask & ((1u << PB_SLICE_COUNT) - 1u);
}

void pb_lv_invalidate_all(void)
{
    s_dirty = (1u << PB_SLICE_COUNT) - 1u;
}

/** Draw one slice and push it to the panel. */
static void draw_slice(int32_t d)
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

    /* Full width (172 columns), row band [d*128 .. d*128+127].
     * The only geometry that matches the panel's contract exactly. */
    pb_lcd_blit(0, (uint32_t)(d * PB_SLICE_W), PB_PANEL_W, PB_SLICE_W,
                &s_slice_fb[0][0]);
    pb_lv_slice_press++;
}

void pb_lv_dump_card_fb(void)
{
    /* Draws and dumps each LVGL slice in turn. The output is in UI
     * orientation: 172 rows x 128 columns per slice, left to right. It reads
     * the dump with the inverse of the mapping the driver WRITES with, so it
     * exercises the mapping too. */
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

        printf("#CARDFB slice %ld  ui x %ld..%ld  (%d rows x %d columns)\n",
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
        printf("#CARDFB-END\n");
    }
}

/* LVGL's time base. In v9 this is supplied at run time rather than by macro. */
static uint32_t tick_cb(void)
{
    return to_ms_since_boot(get_absolute_time());
}

bool pb_lv_init(void)
{
    /* Set up once: if two commands are run back to back (say 'u' then 'a'), a
     * second lv_init plus a second display would corrupt LVGL. */
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

    /* Push the slices that are dirty AND ours, in ascending order. Ascending
     * matters: the panel cursor walks forward, so there is never a backward
     * jump. */
    uint32_t pending = s_dirty & s_lvgl_slices;
    if (!pending) return;

    s_dirty &= ~s_lvgl_slices;

    for (int32_t d = 0; d < PB_SLICE_COUNT; d++) {
        if ((pending >> d) & 1u) draw_slice(d);
    }
}
