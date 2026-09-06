#include "ui/interface.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "hal/touch.h"
#include "ui/screen_listen.h"
#include "ui/screen_log.h"
#include "ui/lv_port.h"
#include "ui/spectrogram.h"
#include "ui/theme.h"

static lv_obj_t *s_screen[PB_SCREEN_COUNT];
static int       s_active = PB_SCREEN_LISTEN;
static bool      s_built;

/* How often the log rows ("3 min ago") and the counters are refreshed. */
#define REFRESH_MS 1000
static uint32_t s_last_refresh;

/* The last view — the log screen's counter row is fed from here. */
static uint32_t s_frame_rate, s_inference, s_overrun;

/* Stops the same species being written to the log over and over. */
static char s_last_log_name[48];

/* ── Swipe detection ──────────────────────────────────────────────────────
 *
 * WHY NOT LVGL'S OWN GESTURE DETECTION: when it does not work, we need to see
 * WHY without looking at the screen. This detector sits directly on the raw
 * point and every stage of it is counted separately.
 *
 * IT IS MEASURED ON THE RAW AXIS, NOT IN DERIVED PIXELS — and that is a
 * measurement-driven decision. The `t` calibration (by hand, four edges,
 * median):
 *
 *     LEFT -> RIGHT (horizontal):  raw_x 432 -> 3     change -429
 *     BOTTOM -> TOP (vertical):    raw_x 560 -> 449   change -111
 *                                  raw_y 107 -> 93    change  -14
 *
 * Three things follow:
 *   1. The horizontal axis is raw_x, and it DECREASES from left to right.
 *   2. raw_y is unusable: it moves by only 14 units across the whole screen
 *      (the case lip presumably blocks touches near the top and bottom
 *      edges). So the VERTICAL RATIO CONSTRAINT WAS REMOVED — dividing by a
 *      number you do not trust is worse than dropping it.
 *   3. This was the actual bug: a vertical swipe moves raw_x by 111 units and
 *      the old threshold was 90, so vertical movement counted as a horizontal
 *      swipe.
 *
 * The threshold now sits BETWEEN the two: accidental drift is 111, a
 * deliberate swipe about 429. 200 is comfortably clear of both.
 *
 * The raw->pixel scale is NOT calibrated (the left edge reads 432, not 639),
 * and it does not need to be: there is nothing on screen to touch, only the
 * swipe. */
#define SWIPE_THRESHOLD_RAW  200
#define SWIPE_MAX_MS  1200
#define FINGER_RELEASE_MS    80    /* this long without a read = lifted     */

/* A sane upper bound for the long axis. The chip occasionally returns ~4000
 * (12-bit, off-panel); those frames are dropped. The largest real value
 * measured is 560. */
#define RAW_MAX          1000

/* ── The record button — the HORIZONTAL position of a touch is enough ─────
 *
 * The button occupies the left 96 pixels of the screen at FULL HEIGHT (the
 * reasoning is in screen_listen.c: calibration showed the short axis to be
 * unusable, so we cannot know the vertical position of a touch). The hit test
 * is therefore one-dimensional.
 *
 * Calibration: the left edge reads raw_x ~432 and the right edge ~3, so 640
 * pixels span ~429 raw units (about 0.67 per pixel). The button, ui_x 0..95,
 * maps to raw_x 432..~368. The threshold was set at 370; the content starts
 * at 96 and its first pixels are padding anyway, so the boundary shifting by
 * a few pixels breaks nothing.
 *
 * WARNING: this scale is COARSE. It will not do if a second touch target is
 * ever added to the screen; the raw->pixel mapping would need calibrating
 * properly first. */
#define BUTTON_RAW_THRESHOLD     370
#define PRESS_MAX_MS     800     /* a press longer than this does not count */

uint32_t pb_swipe_touch;
uint32_t pb_swipe_begin;
uint32_t pb_swipe_accept;
uint32_t pb_swipe_short;
uint32_t pb_button_press;
int32_t  pb_swipe_last_dx;
int32_t  pb_swipe_last_dy;

/* The device does NOT listen continuously. The record button starts
 * listening; it is off at boot. */
static bool s_recording;

static bool     s_pressed;
static int32_t  s_head_raw, s_last_raw;
static uint32_t s_head_ms, s_last_touch_ms;
static bool     s_swiped_this_touch;

/** Apply a swipe. `d` is the change along the raw axis.
 *
 * Because raw_x DECREASES from left to right (see the calibration), swiping
 * right to left — the page-turn direction, "next screen" — INCREASES raw_x. */
static void apply_swipe(int32_t d)
{
    pb_swipe_accept++;
    pb_ui_set_screen(d > 0 ? s_active + 1 : s_active - 1);
}

static void swipe_finish(uint32_t now)
{
    if (!s_pressed) return;
    s_pressed = false;

    if (s_swiped_this_touch) return;

    const int32_t d = s_last_raw - s_head_raw;
    pb_swipe_last_dx = d;

    const int32_t dist = d < 0 ? -d : d;
    const uint32_t held = now - s_head_ms;

    /* Swipe or button press? If the finger barely moved and the touch began
     * inside the button's strip, it is a PRESS. This is checked BEFORE the
     * swipe test, because any touch that fails the threshold is not a swipe
     * anyway. */
    if (dist < SWIPE_THRESHOLD_RAW) {
        if (s_active == PB_SCREEN_LISTEN && s_head_raw >= BUTTON_RAW_THRESHOLD &&
            held <= PRESS_MAX_MS) {
            pb_button_press++;
            printf("  [button] ACCEPT start_raw=%ld dist=%ld held=%lums\n",
                   (long)s_head_raw, (long)dist, (unsigned long)held);
            pb_ui_set_recording(!s_recording);
        } else {
            pb_swipe_short++;
            /* Diagnostic for the button-reliability measurement: print the
             * raw start value of every rejected touch. The margin between the
             * threshold (370) and the button's geometric right edge (~368) is
             * only about 2 raw units; this line separates "the margin really
             * is too tight" from some other cause (duration, wrong screen). */
            printf("  [button] REJECT start_raw=%ld dist=%ld held=%lums screen=%d "
                   "(needs raw>=%d, held<=%dms)\n",
                   (long)s_head_raw, (long)dist, (unsigned long)held, s_active,
                   BUTTON_RAW_THRESHOLD, PRESS_MAX_MS);
        }
        return;
    }

    if (held > SWIPE_MAX_MS) { pb_swipe_short++; return; }

    apply_swipe(d);
}

static void poll_swipe(void)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    /* We read the raw point directly: the threshold was measured on the raw
     * axis (see the calibration above) and the raw->pixel scale is not
     * calibrated. */
    pb_touch_state_t st = pb_touch_read();
    const bool valid = st.ok && st.fingers > 0 && st.p.raw_x < RAW_MAX;

    if (valid) {
        const int32_t raw = (int32_t)st.p.raw_x;
        pb_swipe_touch++;
        pb_swipe_last_dy = (int32_t)st.p.raw_y;   /* raw short axis, for diagnostics */
        s_last_touch_ms = now;

        if (!s_pressed) {
            s_pressed = true;
            s_swiped_this_touch = false;
            s_head_raw = s_last_raw = raw;
            s_head_ms = now;
            pb_swipe_begin++;
            return;
        }

        s_last_raw = raw;

        /* If the threshold is crossed before the finger lifts, switch
         * immediately: waiting for the lift feels unresponsive. */
        if (!s_swiped_this_touch) {
            const int32_t d = s_last_raw - s_head_raw;
            const int32_t dist = d < 0 ? -d : d;
            if (dist >= SWIPE_THRESHOLD_RAW && now - s_head_ms <= SWIPE_MAX_MS) {
                pb_swipe_last_dx = d;
                s_swiped_this_touch = true;
                apply_swipe(d);
            }
        }
        return;
    }

    /* No reading (or an off-panel frame). The touch controller drops the odd
     * frame, so rather than declaring a lift immediately we wait a short
     * window; otherwise a single lost frame would cut a swipe in half. */
    if (s_pressed && (now - s_last_touch_ms) > FINGER_RELEASE_MS) {
        swipe_finish(now);
    }
}

void pb_ui_create(void)
{
    if (s_built) return;
    s_built = true;

    s_screen[PB_SCREEN_LISTEN] = pb_screen_listen_create();
    s_screen[PB_SCREEN_LOG]  = pb_screen_log_create();

    s_active = PB_SCREEN_LISTEN;
    lv_screen_load(s_screen[s_active]);
    pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_LISTEN);
    pb_lv_invalidate_all();

    /* IDLE at boot — the device does not listen continuously. */
    s_recording = false;
    pb_screen_listen_set_recording(false);

    s_last_log_name[0] = '\0';
}

int pb_ui_screen(void) { return s_active; }

bool pb_ui_recording(void) { return s_recording; }

void pb_ui_set_recording(bool recording)
{
    if (!s_built || recording == s_recording) return;
    s_recording = recording;
    pb_screen_listen_set_recording(recording);

    /* When recording stops the strip is left as it is (a record of what was
     * last heard), but it is cleared when starting so a new recording is not
     * confused with the old one. */
    if (recording && s_active == PB_SCREEN_LISTEN) pb_spec_init();
}

void pb_ui_set_screen(int screen)
{
    if (!s_built) return;
    if (screen < 0 || screen >= PB_SCREEN_COUNT) return;
    if (screen == s_active) return;

    s_active = screen;

    /* NO TRANSITION ANIMATION, deliberately. `lv_screen_load_anim`
     * invalidates the whole screen every frame, and for us one frame means
     * re-pushing every owned slice to the panel (44 KB of QSPI per slice).
     * That is impossible at 60 Hz, and a stuttering animation at 10 Hz would
     * look worse than an instant switch. */
    lv_screen_load(s_screen[s_active]);

    if (s_active == PB_SCREEN_LOG) {
        /* The log is full width: the two right-hand slices become LVGL's. */
        pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_ALL);
        pb_screen_log_refresh(s_frame_rate, s_inference, s_overrun);
    } else {
        pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_LISTEN);
        /* The log screen painted over the spectrogram region; reset the
         * strip so no old text shows through. The columns will scroll back in
         * and fill it. */
        pb_spec_init();
    }

    pb_lv_invalidate_all();
}

void pb_ui_next(void)
{
    pb_ui_set_screen((s_active + 1) % PB_SCREEN_COUNT);
}

void pb_ui_log_add(const char *name, const char *latin, float confidence)
{
    pb_screen_log_add(name, latin, confidence);
}

void pb_ui_update(const pb_result_view_t *view)
{
    if (!view || !s_built) return;

    s_frame_rate = view->frame_rate;
    s_inference  = view->inference;
    s_overrun    = view->overrun;

    pb_screen_listen_update(view);

    /* Write to the log when the decision says IDENTIFIED — but only if the
     * species CHANGED. The decision rule holds a species on screen for
     * PB_DECISION_HOLD_MS (5 s) and this function is called at 4 Hz, so the
     * same detection arrives about 20 times. screen_log already refuses to
     * open a new row for a repeated species; this check is here to avoid
     * making the call at all. It resets when the mode leaves SPECIES, so the
     * same species is logged again if it is heard a second time. */
    if (view->mode == PB_DECISION_SPECIES && view->species_name) {
        if (strncmp(s_last_log_name, view->species_name, sizeof(s_last_log_name) - 1) != 0) {
            snprintf(s_last_log_name, sizeof(s_last_log_name), "%s", view->species_name);
            pb_screen_log_add(view->species_name, view->top3_latin[0], view->confidence);
        }
    } else {
        s_last_log_name[0] = '\0';
    }
}

void pb_ui_tick(void)
{
    if (!s_built) { pb_lv_tick(); return; }

    poll_swipe();

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_active == PB_SCREEN_LOG && (now - s_last_refresh) >= REFRESH_MS) {
        s_last_refresh = now;
        pb_screen_log_refresh(s_frame_rate, s_inference, s_overrun);
    }

    pb_lv_tick();
}
