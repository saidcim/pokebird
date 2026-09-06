/**
 * lv_port.h — the layer binding LVGL to PokeBird's hardware
 *
 * Registers the display (QSPI panel) and input (capacitive touch) drivers
 * with LVGL. The UI is LANDSCAPE (640x172) and the panel is PORTRAIT
 * (172x640); the 90-degree rotation between them happens during flush — see
 * lv_port.c for the details.
 *
 * The UI is FULL WIDTH (640) and the screen is pushed as five VERTICAL
 * SLICES; the reasoning and the measurements are at the top of lv_port.c. A
 * full-screen framebuffer (220 KB) still does NOT FIT — the slice path
 * replaces it and is cheaper.
 */
#ifndef POKEBIRD_LV_PORT_H
#define POKEBIRD_LV_PORT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Start LVGL, the display and touch.
 *
 * The display hardware (QSPI + panel) must already be initialised BEFORE this
 * call.
 * @return true if touch responds; the display works even when it returns
 *         false.
 */
bool pb_lv_init(void);

/** Turn LVGL's timer over. Must be called regularly from the main loop. */
void pb_lv_tick(void);

/* Flush counters — does the panel's column alignment actually hold?
 * The panel rounds a column range to 2 pixels; with an odd column count the
 * data shifts by one pixel on every row and text looks horizontally smeared.
 * A measurement that needs no eyes: printed when `a` and `u` exit. */
extern uint32_t pb_lv_flush_count;
extern uint32_t pb_lv_flush_unaligned;
extern uint32_t pb_lv_flush_stride_differs;
extern uint32_t pb_lv_flush_w_min, pb_lv_flush_w_max;
extern int32_t  pb_lv_last_x1, pb_lv_last_x2, pb_lv_last_y1, pb_lv_last_y2;
extern int32_t  pb_lv_last_stride_px, pb_lv_last_area_w;
extern uint32_t pb_lv_slice_press;      /* slices pushed to the panel */
void pb_lv_flush_counters_reset(void);

/**
 * Declare which vertical slices LVGL draws (bit d = slice d, 128 px).
 *
 * The spectrogram writes to the panel DIRECTLY (62 Hz, its own fast column
 * path). If LVGL also pushed that region the two would erase each other. So
 * each screen declares ownership according to its own layout: the listening
 * screen leaves the two right-hand slices to the spectrogram, and the log
 * screen takes all five.
 */
void pb_lv_set_slice_owner(uint32_t mask);

/** Dirty every slice — for a full redraw when switching screens. */
void pb_lv_invalidate_all(void);

/**
 * Read the raw touch point in UI coordinates (0..639, 0..171).
 *
 * The mapping was taken from a working driver (rsvpnano's
 * axs15231b_touch.cpp); the details, and the clipping bug in the old code,
 * are written up in lv_port.c. The swipe detector reads this directly rather
 * than going through LVGL's input layer: because touch went unverified for so
 * long, being able to dump the raw data to the serial console was essential.
 *
 * @return true if there is a touch, writing `ux`/`uy`; false otherwise.
 */
bool pb_lv_touch_get(int32_t *ux, int32_t *uy);

/** Touch frames rejected for falling outside the panel — the observed "jump
 *  to ~4000" is counted here. A measurement that needs no eyes. */
extern uint32_t pb_lv_touch_invalidate;

/**
 * Dump the next `count` flush areas to the serial console as ASCII — NO EYES
 * NEEDED.
 *
 * The dump is produced with the same indexing the driver READS with, so it
 * exercises both LVGL's drawing and the 90-degree transposed read at once. If
 * the text is legible in the terminal, the corruption is further down (panel
 * or bus); if it is not, the problem is on the LVGL side.
 */
void pb_lv_request_dump(int count);

/**
 * Dump the ENTIRE card framebuffer to the serial console as ASCII — NO EYES
 * NEEDED.
 *
 * `pb_lv_request_dump` shows a single flush AREA; this shows the card's full
 * current state. The difference between the two is decisive in diagnosis:
 *
 *   dump is legible     -> LVGL, the layout and the transposed write are all
 *                          CORRECT, so the corruption is on the path to the
 *                          panel (cursor, window, DMA)
 *   dump is illegible   -> the corruption is on the LVGL/layout side and can
 *                          be fixed without looking at the panel at all
 *
 * The output is in UI orientation and SLICE BY SLICE: for each slice LVGL
 * owns, 172 rows x 128 columns, left to right and top to bottom — the same
 * arrangement you should see looking at the screen, in five pieces.
 */
void pb_lv_dump_card_fb(void);

#endif /* POKEBIRD_LV_PORT_H */
