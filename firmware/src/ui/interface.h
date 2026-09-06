/**
 * interface.h — the device's two screens and the swipe between them
 *
 * Design: retro/analog, dark theme, 640x172.
 *
 *   SCREEN 0 · LISTEN   left:  the three closest species (name + scientific
 *                              name + confidence bar)
 *                       right: the live spectrogram (written to the panel
 *                              DIRECTLY)
 *   SCREEN 1 · LOG      the species identified so far
 *
 * SCREEN CONTRACT: not one line in this module writes to the panel directly.
 * All drawing goes through LVGL, and `lv_port.c` always pushes vertical
 * slices to the panel at FULL WIDTH (columns 0..171). Multi-row writes into a
 * narrow column band SLIP on this panel. Do not add panel calls here.
 *
 * NOTE — THERE ARE TWO WAYS TO CHANGE SCREEN, deliberately. Swiping depends
 * on the touch controller, and at the time this was written touch had never
 * been confirmed with a finger (a constant 0xDB when idle, which could have
 * been "no touch" or a fault). So screen switching sits behind
 * `pb_ui_set_screen` and can also be driven from the serial console: even if
 * touch turned out to be dead, the UI would stay usable.
 *
 * (Touch has since been confirmed working by hand; the serial path remains as
 * a diagnostic.)
 */
#ifndef POKEBIRD_INTERFACE_H
#define POKEBIRD_INTERFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "ai/decision.h"

#define PB_SCREEN_LISTEN  0
#define PB_SCREEN_LOG     1
#define PB_SCREEN_COUNT   2

/** Everything the UI shows in one update. The caller supplies the class
 *  names: this module does not include `classes.h`, which keeps a second copy
 *  of the 179-entry table out of flash. */
typedef struct {
    pb_decision_mode_t mode;
    const char    *species_name;      /* species to show; NULL if none       */
    float          confidence;        /* 0..1                                */

    const char    *top3_name[3];      /* the vote's top 3; entries may be NULL */
    const char    *top3_latin[3];     /* scientific names; may be NULL       */
    float          top3_probability[3];

    /* The diagnostic counters on the bottom row. They live on screen as well
     * as on the serial console so the pipeline can be checked without one. */
    uint32_t frame_rate;              /* mel frames per second               */
    uint32_t inference;
    uint32_t merged;
    uint32_t overrun;
    float    band_db;
} pb_result_view_t;

/** Build both screens. `pb_lv_init()` must already have been called. */
void pb_ui_create(void);

/**
 * Turn LVGL over, poll for a swipe, and push dirty slices to the panel.
 * Must be called regularly from the main loop.
 */
void pb_ui_tick(void);

/** Refresh the contents of the listening screen. */
void pb_ui_update(const pb_result_view_t *view);

/**
 * Add a detection to the log. If the same species arrives again immediately
 * it does NOT open a new row, it refreshes the top one: the decision rule
 * holds a species on screen for five seconds (`PB_DECISION_HOLD_MS`), and if
 * the same detection kept landing during that time the log would fill up with
 * a single event.
 */
void pb_ui_log_add(const char *name, const char *latin, float confidence);

/** The active screen (PB_SCREEN_*). */
int  pb_ui_screen(void);

/** Change screen. An out-of-range value is ignored. */
void pb_ui_set_screen(int screen);

/** Move to the next screen (wrapping) — the fallback path from serial. */
void pb_ui_next(void);

/* ── Recording (listening) state ──────────────────────────────────────────
 *
 * The device does NOT listen continuously: the record button starts and stops
 * listening, and it is off at boot. The button itself is a FULL-HEIGHT strip
 * in the leftmost 96 pixels of the listening screen — the reason is in
 * screen_listen.c: touch calibration showed the short axis to be unusable, so
 * we only know the HORIZONTAL position of a touch.
 *
 * Actually stopping and starting the pipeline is the CALLER's job: this
 * module never touches hardware, it only holds the state and the appearance.
 * `main.c` checks `pb_ui_recording()` each turn and calls
 * `pb_recognizer_start` / `pb_recognizer_stop`.
 */
bool pb_ui_recording(void);
void pb_ui_set_recording(bool recording);

/** How many times the record button has been pressed — a measurement that
 *  needs no eyes on the screen. */
extern uint32_t pb_button_press;

/* ── Swipe diagnostics — NO EYES NEEDED ───────────────────────────────────
 * Because touch was unverified for a long time, being able to tell why a
 * swipe did not register without looking at the screen was essential. These
 * counters are dumped to the serial console. */
extern uint32_t pb_swipe_touch;    /* valid touch frames read              */
extern uint32_t pb_swipe_begin;    /* finger went down                     */
extern uint32_t pb_swipe_accept;   /* counted as a swipe                   */
extern uint32_t pb_swipe_short;    /* movement stayed below the threshold  */
extern int32_t  pb_swipe_last_dx;  /* last finger movement (ui px)         */
extern int32_t  pb_swipe_last_dy;

#endif /* POKEBIRD_INTERFACE_H */
