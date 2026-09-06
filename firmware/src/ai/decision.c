/**
 * decision.c — threshold + hysteresis + hold. The reasoning and the measured
 * numbers live in decision.h.
 *
 * THE WHOLE RULE (deliberately small — being readable is half of being right):
 *
 *   when a new vote arrives
 *     is the candidate valid?  class is not the negative one, and at least
 *                              MIN_WINDOWS windows were voted
 *       p >= ENTER                             -> show SPECIES (may switch class)
 *       p >= EXIT and the same class is up     -> keep the mode, refresh support
 *       p >= EXIT and nothing is on screen     -> show UNSURE
 *       otherwise                              -> no support
 *
 *   every turn
 *     a display unsupported for HOLD_MS is cleared
 *     with nothing on show, the mode is SOUND if the gate opened recently,
 *     otherwise LISTENING
 *
 * NOTE — the "same class is already up" branch IS the hysteresis: replacing
 * the species on screen costs the ENTER threshold, while merely staying costs
 * only EXIT. That stops the text from flickering while confidence wanders
 * around the threshold. A DIFFERENT species arriving with a confidence
 * between EXIT and ENTER does NOT take over; the old one stays until its hold
 * expires. That is deliberate: showing slightly stale information beats
 * flickering the screen with the second-best guess.
 */
#include "ai/decision.h"

static void show(pb_decision_t *d, pb_decision_mode_t mode, int16_t cls,
                 float confidence, uint32_t now_ms) {
    if (d->mode != mode || d->cls != cls) {
        d->enter_ms = now_ms;
        d->version++;
    }
    d->mode = mode;
    d->cls = cls;
    d->confidence = confidence;
    d->last_support_ms = now_ms;
}

void pb_decision_reset(pb_decision_t *d, uint32_t now_ms) {
    if (!d) return;
    d->mode = PB_DECISION_LISTENING;
    d->cls = -1;
    d->confidence = 0.0f;
    /* "Already well in the past": at boot neither the sound indicator nor a
     * display hold should look live by accident. The differences are computed
     * unsigned, so this stays correct across wraparound too. */
    d->last_gate_ms = now_ms - (PB_DECISION_SOUND_HOLD_MS + 1u);
    d->last_support_ms = now_ms - (PB_DECISION_HOLD_MS + 1u);
    d->enter_ms = now_ms;
    d->version = 0;
}

void pb_decision_update(pb_decision_t *d, const pb_decision_input_t *in) {
    if (!d || !in) return;

    if (in->gate_open) d->last_gate_ms = in->now_ms;

    if (in->fresh_result) {
        const bool candidate =
            in->cls >= 0 &&
            in->cls != PB_DECISION_NEGATIVE_CLASS &&
            in->merged >= PB_DECISION_MIN_WINDOWS;
        const bool showing =
            (d->mode == PB_DECISION_SPECIES || d->mode == PB_DECISION_UNSURE);

        if (candidate && in->probability >= PB_DECISION_ENTER_THRESHOLD) {
            show(d, PB_DECISION_SPECIES, in->cls, in->probability, in->now_ms);
        } else if (candidate && in->probability >= PB_DECISION_EXIT_THRESHOLD &&
                   showing && in->cls == d->cls) {
            /* Hysteresis: the species on screen keeps its mode for as long as
             * it stays above the exit threshold — SPECIES stays SPECIES. */
            show(d, d->mode, d->cls, in->probability, in->now_ms);
        } else if (candidate && in->probability >= PB_DECISION_EXIT_THRESHOLD &&
                   !showing) {
            show(d, PB_DECISION_UNSURE, in->cls, in->probability, in->now_ms);
        }
        /* otherwise: no support, let the hold below decide */
    }

    if (d->mode == PB_DECISION_SPECIES || d->mode == PB_DECISION_UNSURE) {
        if (in->now_ms - d->last_support_ms > PB_DECISION_HOLD_MS) {
            d->cls = -1;
            d->confidence = 0.0f;
            d->mode = PB_DECISION_LISTENING;  /* the block below may raise this to SOUND */
            d->version++;
        }
    }

    if (d->mode != PB_DECISION_SPECIES && d->mode != PB_DECISION_UNSURE) {
        const pb_decision_mode_t fresh =
            (in->now_ms - d->last_gate_ms <= PB_DECISION_SOUND_HOLD_MS)
                ? PB_DECISION_SOUND : PB_DECISION_LISTENING;
        if (fresh != d->mode) {
            d->mode = fresh;
            d->version++;
        }
    }
}

const char *pb_decision_mode_name(pb_decision_mode_t mode) {
    switch (mode) {
        case PB_DECISION_LISTENING: return "listening";
        case PB_DECISION_SOUND:     return "SOUND DETECTED";
        case PB_DECISION_UNSURE:    return "maybe";
        case PB_DECISION_SPECIES:   return "SPECIES";
    }
    return "?";
}
