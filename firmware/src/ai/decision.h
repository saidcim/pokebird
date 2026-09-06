/**
 * decision.h — the rule that decides what goes on the screen (M7 step 2)
 *
 * WHY THIS IS A SEPARATE MODULE: the voting stage (recognizer.c) produces a
 * fresh probability distribution every second. Wire that straight to the
 * screen and the text jumps once a second and becomes unreadable. A decision
 * rule belongs in between: threshold + hysteresis + hold.
 *
 * This file is DELIBERATELY hardware-free — stdint/stdbool only. That lets
 * the host tests (firmware/test/dsp_test.c) exercise it by advancing time by
 * hand; debugging hysteresis on the board would be expensive.
 *
 * ── THE THRESHOLDS WERE MEASURED, NOT GUESSED ────────────────────────────
 *
 * `tools/measure_thresholds.py` swept the p1 distribution of the 8-window
 * vote over the test set (6,267 windows / 1,288 recordings, a split never
 * touched during training). Output: models/thresholds.txt. In that table,
 * coverage = the share of blocks where we print a species name, precision =
 * how often we are right when we do print one, and false alarm = the share of
 * negative blocks where we print a name anyway:
 *
 *     threshold  coverage  precision  false alarm
 *       0.35      59.7%      71.7%       4.2%
 *       0.45      49.9%      79.0%       3.6%
 *       0.50      45.1%      81.8%       3.0%
 *       0.60      35.6%      85.4%       2.7%   <- enter threshold
 *       0.70      28.6%      88.5%       2.4%
 *       0.80      20.3%      91.7%       1.5%
 *
 * WARNING: these numbers are an UPPER BOUND. The blocks come from
 * non-overlapping slices, and those slices are where BirdNET heard a bird. On
 * the device the windows overlap at a one-second step, so the errors are more
 * correlated than this. Field calibration (M8) is still needed; when it
 * happens, re-run `tools/measure_thresholds.py` and update the three
 * constants below.
 */
#ifndef POKEBIRD_DECISION_H
#define POKEBIRD_DECISION_H

#include <stdbool.h>
#include <stdint.h>

/** Threshold for PUTTING A SPECIES NAME ON SCREEN. Measured: precision 85.4%
 *  · coverage 35.6% · false alarm 2.7% (models/thresholds.txt, 8 windows). */
#define PB_DECISION_ENTER_THRESHOLD   0.60f

/** Threshold for CLEARING what is shown — the lower end of the hysteresis.
 *  Measured: precision 71.7%, the same level as the voted top-1 accuracy
 *  (70.40%). Below this a displayed name is no better than a plain best
 *  guess, so it is not worth showing. */
#define PB_DECISION_EXIT_THRESHOLD   0.35f

/** Minimum number of voted windows before deciding anything. Measured: at the
 *  0.60 threshold, precision is 70.1% with one window, 84.9% with three and
 *  85.4% with eight. The whole gain lands in the first three windows, so
 *  three is both fast and sufficient. */
#define PB_DECISION_MIN_WINDOWS  3u

/** How long an unsupported display stays on screen. When the sound stops the
 *  gate closes and inference halts, but the user still needs time to read the
 *  name.
 *
 *  5 s was chosen to sit below STALE_MS in recognizer.c (6 s, when the voting
 *  memory goes stale): a species cleared from the screen goes away BEFORE the
 *  voting memory is flushed, so the confusing state of "cleared from the
 *  screen but still supported by memory" never occurs. */
#define PB_DECISION_HOLD_MS       5000u

/** How long the "sound present" indicator lingers after the gate closes. At
 *  62 fps a single-frame flicker is invisible, so it is held briefly — the
 *  same ~0.5 s hold the `a` demo uses. */
#define PB_DECISION_SOUND_HOLD_MS   700u

/** Index of the negative / unknown class (the last class in classes.h).
 *  main.c static_asserts this against PB_CLASS_COUNT so that regenerating the
 *  class table cannot silently shift it. */
#define PB_DECISION_NEGATIVE_CLASS 178

typedef enum {
    PB_DECISION_LISTENING = 0, /* quiet — the gate is closed                 */
    PB_DECISION_SOUND,         /* gate open, no species to show yet          */
    PB_DECISION_UNSURE,        /* between the thresholds: "maybe"            */
    PB_DECISION_SPECIES        /* above the enter threshold: a species name  */
} pb_decision_mode_t;

typedef struct {
    uint32_t now_ms;       /* current time (to_ms_since_boot)             */
    bool     gate_open;    /* is the stage-0 gate open right now           */
    bool     fresh_result; /* did a new vote arrive on this call            */
    int16_t  cls;          /* voted top-1 class index                       */
    float    probability;  /* that class's voted probability (0..1)         */
    uint32_t merged;       /* how many windows were voted together          */
} pb_decision_input_t;

typedef struct {
    pb_decision_mode_t mode;
    int16_t  cls;              /* class on show; -1 when mode < UNSURE      */
    float    confidence;       /* its most recently supported probability   */

    /* ── internal state ── */
    uint32_t last_gate_ms;     /* when the gate was last seen open          */
    uint32_t last_support_ms;  /* when the display was last supported       */
    uint32_t enter_ms;         /* when we entered this display (for stats)  */
    uint32_t version;          /* bumped whenever what is on screen changes */
} pb_decision_t;

/**
 * Reset the state.
 *
 * `now_ms` is required because the timeouts work on absolute timestamps: a
 * "the gate was last open at..." stamp initialised to zero would read as
 * "sound present" for any mode started more than 700 ms after boot.
 */
void pb_decision_reset(pb_decision_t *d, uint32_t now_ms);

/**
 * Advance the rule by one step. Called on every UI turn, and it must be
 * called even when no new vote arrived, because the timeouts (hold, sound
 * decay) are driven from here.
 */
void pb_decision_update(pb_decision_t *d, const pb_decision_input_t *in);

/** Fixed status text for a mode — kept in one place so the UI and the serial
 *  console use the same words. */
const char *pb_decision_mode_name(pb_decision_mode_t mode);

#endif /* POKEBIRD_DECISION_H */
