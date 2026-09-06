/**
 * recognizer.h — the real-time recognition pipeline (core 1)
 *
 * The split of work from docs/ARCHITECTURE.md:
 *   Core 1  audio + inference   I2S/DMA -> mel -> gate -> species net -> vote
 *   Core 0  UI + storage
 *
 * Core 1 runs without ever blocking; core 0 copies the result out with
 * pb_recognizer_read(). The state struct below is the only shared thing.
 *
 * WARNING: while this engine is running, the mel ring (dsp/mel.c) and the
 * audio stream BELONG TO CORE 1. Do not run the diagnostic commands that use
 * the same resources (`m`, `a`, `s`, `r`) at the same time — stop core 1 with
 * pb_recognizer_stop() first.
 */
#ifndef POKEBIRD_RECOGNIZER_H
#define POKEBIRD_RECOGNIZER_H

#include <stdbool.h>
#include <stdint.h>

#include "ai/species_net.h"

/** Voting window. MEASURED: the gain saturates at 5-8 windows and does not
 *  improve at 12 (top-1 actually drops). 8 windows is about 8 seconds of
 *  listening. */
#define PB_VOTE_WINDOWS  8

typedef struct {
    uint32_t version;        /* bumped on every new result — core 0 watches this */

    uint32_t frame;          /* total mel frames                                */
    uint32_t gate_open;      /* frames where the gate was open                  */
    uint32_t inference;      /* inferences run (species net)                    */
    uint32_t skipped;        /* windows skipped because the gate was shut       */
    uint32_t binary_ran;     /* windows where the stage-1 binary net ran        */
    uint32_t binary_red;     /* windows the binary net rejected as "not a bird" */
    float    binary_last_p;  /* last binary net output — P(bird), 0..1          */
    uint32_t overrun;        /* the audio ring overflowed — continuity broke    */
    uint32_t last_time_us;   /* duration of the last Invoke()                   */
    uint32_t merged;         /* how many windows were voted together (<= 8)     */

    int16_t  top3[3];        /* class indices, best first                       */
    float    top3_probability[3];
    bool     valid;          /* has at least one inference run                  */
    bool     gate_now;       /* was the gate open on the LATEST frame (for UI)  */

    /* Gate and noise floor — diagnostics, the same values the `m` command
     * reports. */
    float    band_db, base_db, flux;
} pb_recognizer_state_t;

/**
 * Start core 1 and run the recognition pipeline.
 * pb_audio_i2s_init() and pb_mel_init() must already have been called.
 *
 * @param gate_ignore  when true the stage-0 gate is bypassed and inference
 *                     runs EVERY second. This is not normal operation, it is
 *                     MEASUREMENT mode: it checks whether the pipeline holds
 *                     audio continuity under the worst case of back-to-back
 *                     inference. In a quiet room the gate almost never opens
 *                     (2-3% of frames), so without this mode the inference
 *                     path never gets tested under real-time load at all.
 * @return false if the model could not be loaded (core 1 is not started)
 */
bool pb_recognizer_start(bool gate_ignore);

/** Stop core 1 and reset. */
void pb_recognizer_stop(void);

/** Copy out the latest state. Safe to call from core 0. */
void pb_recognizer_read(pb_recognizer_state_t *out);

/**
 * Spectrogram column queue — core 1 produces, core 0 consumes.
 *
 * WHY THIS EXISTS: while the engine runs, the mel ring BELONGS TO CORE 1 (see
 * the warning above), so core 0 calling pb_mel_last_frame() would be a race.
 * Instead core 1 drops each frame in here and core 0 drains it.
 *
 * If the queue is full, core 1 DISCARDS the new frame and never waits:
 * stalling the real-time pipeline for the sake of the UI would cost far more
 * than losing one spectrogram column. The queue holds 64 frames, about one
 * second, so it never fills as long as core 0 drains it a few times a second.
 *
 * @param out PB_MEL_BANDS int8 values
 * @return    false when the queue is empty
 */
bool pb_recognizer_get_mel(int8_t *out);

#endif /* POKEBIRD_RECOGNIZER_H */
