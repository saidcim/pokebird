/**
 * recognizer.c — the real-time recognition pipeline, on core 1
 *
 * The loop is the same mel pipeline the `m` command runs, plus two things:
 * one inference per second while the gate is open, and a vote over the last
 * 8 windows.
 *
 * MEASURED TIMING (on the board, 2 August 2026):
 *   mel frame          arrives every 16 ms (hop 384 @ 24 kHz = 62.5 fps)
 *   species net Invoke 190 ms  <- the ring is NOT read at all during this
 *   audio ring         341 ms (8192 samples), overrun threshold 3/4 = 256 ms
 *
 * So audio accumulating during an inference is not lost; afterwards core 1
 * chews through the backlog faster than real time and catches up. The ring
 * was raised from 4096 to 8192 in M6 for exactly this reason (audio_i2s.h).
 */
#include "ai/recognizer.h"

#include <math.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "ai/binary_net.h"
#include "dsp/fft.h"
#include "dsp/gate.h"
#include "dsp/mel.h"
#include "hal/audio_i2s.h"

/* Decision threshold for the stage-1 binary net. MEASURED in
 * tools/train_binary.py: at 0.5 the test set gives 97.90% bird recall and
 * 86.58% negative specificity.
 *
 * A miss (a wrong "not a bird") is expensive — it silently loses a real
 * detection. A let-through (a wrong "bird") is cheap — the species net
 * filters it with its own negative class anyway. That is why the threshold
 * was NOT pulled below 0.5: the measured operating point already favours
 * recall, and model selection used the same criterion (see train_binary.py). */
#define BINARY_THRESHOLD  0.5f

/* Inference step: how many mel frames between window evaluations.
 * 63 frames x 16 ms = 1.008 s — the one-second window step from the plan. */
#define STEP_FRAME   63

/* Gate threshold: the gate must have been open for at least this many of the
 * last STEP_FRAME frames.
 *
 * The point is to never run the expensive work during 90%+ of the time. In a
 * quiet room the gate already opens on only 2-3% of frames, so a 5/63 = 8%
 * threshold rejects silence comfortably. Birdsong lasts seconds, so a real
 * call clears this threshold easily.
 *
 * WARNING: THIS THRESHOLD NEEDS FIELD CALIBRATION (M8). Its current value
 * comes from a quiet-room measurement; in city noise the gate will open far
 * more often and the threshold will lose its selectivity. At that point the
 * stage-1 binary net takes over the job. */
#define GATE_THRESHOLD   5

/* How long before the voting memory goes stale. If no inference runs for this
 * long (the surroundings went quiet) the accumulated probabilities are
 * dropped: mixing a call from 30 seconds ago into the current result would be
 * wrong. */
#define STALE_MS    6000

/* ── Shared state ─────────────────────────────────────────────────────────
 * Core 1 writes, core 0 reads. No lock: the `version` field is written LAST,
 * and core 0 reads it, copies the body, then checks the version again. On a
 * torn read it retries. With a single writer that is sufficient — no spinlock
 * needed. */
static volatile pb_recognizer_state_t s_state;
static volatile bool s_run = false;
static volatile bool s_gate_ignore = false;

/* Voting ring: the softmax probabilities of the last 8 windows.
 * 8 x 179 x 4 = 5,728 bytes. */
static float s_probability[PB_VOTE_WINDOWS][PB_SPECIES_NET_CLASSES];
static uint32_t s_probability_write = 0;
static uint32_t s_probability_count = 0;

/* ── Spectrogram column queue (core 1 -> core 0) ──────────────────────────
 * Single producer / single consumer ring. No lock: the writer only advances
 * `write` and the reader only advances `read`, so the gap between the two
 * indices always stays on the safe side (a slot counted as full is never
 * overwritten). 64 x 64 = 4,096 bytes. */
#define MEL_QUEUE  64
static int8_t s_mel_queue[MEL_QUEUE][PB_MEL_BANDS];
static volatile uint32_t s_mel_write = 0, s_mel_read = 0;

/* Core 1's stack. The Pico SDK default is 4 KB; we never measured how much
 * stack a TFLM Invoke actually uses (it takes scratch buffers from the arena,
 * but kernels still put temporaries on the stack). 8 KB is a safe unmeasured
 * margin — it could be shrunk after a canary measurement. */
static uint32_t s_core1_heap[2048] __attribute__((aligned(8)));

/**
 * Softmax over int8 logits. Scale and zero point come from the model.
 * The maximum is subtracted for overflow safety (the standard trick).
 */
static void softmax(const int8_t *q, float scale, int zero, float *out) {
    int largest = q[0];
    for (int i = 1; i < PB_SPECIES_NET_CLASSES; i++) {
        if (q[i] > largest) largest = q[i];
    }
    float total = 0.0f;
    for (int i = 0; i < PB_SPECIES_NET_CLASSES; i++) {
        const float z = ((float)q[i] - (float)largest) * scale;
        out[i] = expf(z);
        total += out[i];
    }
    (void)zero;  /* the zero point cancels out because we take a difference */
    const float inv = 1.0f / total;
    for (int i = 0; i < PB_SPECIES_NET_CLASSES; i++) out[i] *= inv;
}

/**
 * Average the last `s_probability_count` windows and pick the top 3.
 *
 * Averaging is exactly the method tools/measure_voting.py measured — the
 * 70.40% / 82.20% figures reported there belong to THIS combination rule.
 * Switching to another rule (majority voting, say) would invalidate them.
 */
static void vote(int16_t *top3, float *score3, uint32_t *count) {
    float avg[PB_SPECIES_NET_CLASSES];
    const uint32_t n = s_probability_count < PB_VOTE_WINDOWS
                           ? s_probability_count : PB_VOTE_WINDOWS;
    for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) avg[c] = 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        const float *p = s_probability[k];
        for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) avg[c] += p[c];
    }
    const float inv = n ? 1.0f / (float)n : 0.0f;
    for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) avg[c] *= inv;

    for (int r = 0; r < 3; r++) { top3[r] = -1; score3[r] = -1.0f; }
    for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) {
        for (int r = 0; r < 3; r++) {
            if (avg[c] > score3[r]) {
                for (int j = 2; j > r; j--) {
                    score3[j] = score3[j - 1];
                    top3[j] = top3[j - 1];
                }
                score3[r] = avg[c];
                top3[r] = (int16_t)c;
                break;
            }
        }
    }
    *count = n;
}

static void core1_loop(void) {
    /* Overlapping frame: each turn takes PB_MEL_HOP new samples and shifts
     * the frame left. Reading without overlap would break the 16 ms step. */
    static int16_t frame[PB_FFT_SIZE];
    static int8_t  window[PB_MEL_FRAMES * PB_MEL_BANDS];
    const uint32_t remaining = PB_FFT_SIZE - PB_MEL_HOP;

    pb_mel_reset();
    pb_gate_reset();
    pb_audio_stream_flush();

    uint32_t step = 0;       /* frames since the last inference           */
    uint32_t step_gate = 0;  /* how many of those had the gate open       */
    /* The binary net said "bird" on the previous window, so the species net
     * is reserved for THIS one (the two never run on the same window — see
     * the warning below). */
    bool binary_pending = false;
    absolute_time_t last_inference = get_absolute_time();

    while (s_run) {
        memmove(frame, frame + PB_MEL_HOP, remaining * sizeof(int16_t));
        pb_capture_result_t cap =
            pb_audio_stream_read(frame + remaining, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) continue;
        if (cap.fifo_overrun) s_state.overrun++;

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(frame, power);
        pb_gate_result_t g = pb_gate_update(power);
        pb_mel_push(frame);

        /* Hand the frame to the UI queue (spectrogram). If the queue is full,
         * SKIP — the real-time pipeline does not wait for the UI. */
        {
            const uint32_t write = s_mel_write;
            const uint32_t next = (write + 1u) % MEL_QUEUE;
            if (next != s_mel_read && pb_mel_last_frame(s_mel_queue[write])) {
                __dmb();          /* data must be visible BEFORE the index */
                s_mel_write = next;
            }
        }

        s_state.frame++;
        s_state.gate_now = g.active;
        if (g.active) { s_state.gate_open++; step_gate++; }
        s_state.band_db = g.band_db;
        s_state.base_db = g.floor_db;
        s_state.flux = g.flux;

        if (++step < STEP_FRAME) continue;
        step = 0;
        const uint32_t gate_count = step_gate;
        step_gate = 0;

        /* Drop the voting memory if it has gone stale. */
        if (absolute_time_diff_us(last_inference, get_absolute_time())
                > (int64_t)STALE_MS * 1000) {
            s_probability_count = 0;
            s_probability_write = 0;
        }

        /* Stage-0 gate: in silence the expensive work never runs at all.
         * While `binary_pending` is set the gate does NOT cancel this turn —
         * the binary net said "bird" on the previous window and the species
         * net was RESERVED for this turn; we do not back out. */
        if (!s_gate_ignore && !binary_pending && gate_count < GATE_THRESHOLD) {
            s_state.skipped++;
            continue;
        }

        /* No window until three seconds have accumulated. */
        if (!pb_mel_window(window)) continue;

        /* WARNING: THE BINARY NET AND THE SPECIES NET NEVER BOTH RUN ON THE
         * SAME WINDOW.
         *
         * It was tried: back to back (binary ~69 ms + species net 190 ms =
         * ~259 ms) exceeded the audio ring's 256 ms tolerance and LOCKED THE
         * BOARD UP. The hardware's DMA ring field is 4 bits, so the ring
         * cannot exceed 32 KB (audio_i2s.h) and cannot be grown. The fix is
         * to split the two across SEPARATE windows. The worst case in one
         * turn is then still 190 ms — a tolerance the ring already meets.
         *
         * binary_pending: the binary net said "bird" on the previous window,
         * so this turn runs ONLY the species net, on a fresher 3 s window,
         * which is if anything an advantage rather than a drawback.
         * Otherwise this turn runs ONLY the binary net. */
        if (binary_pending) {
            binary_pending = false;
        } else {
            /* Same device contract: scale 1.0, zero 0 (asserted in
             * binary_net.cc). */
            memcpy(pb_binary_net_input(), window, sizeof(window));
            if (!pb_binary_net_run()) continue;
            s_state.binary_ran++;
            s_state.binary_last_p = pb_binary_net_probability();
            if (s_state.binary_last_p < BINARY_THRESHOLD) {
                s_state.binary_red++;
                continue;
            }
            /* It said "bird": do NOT run the species net on this turn (that
             * would blow the time budget), reserve it for the next window. */
            binary_pending = true;
            continue;
        }

        /* Device contract: input scale 1.0, zero point 0 — NO conversion,
         * a straight copy (asserted in species_net.cc). */
        memcpy(pb_species_net_input(), window, sizeof(window));
        if (!pb_species_net_run()) continue;

        s_state.last_time_us = pb_species_net_last_time_us();
        s_state.inference++;
        last_inference = get_absolute_time();

        softmax(pb_species_net_output(), pb_species_net_output_scale(),
                pb_species_net_output_zero(), s_probability[s_probability_write]);
        s_probability_write = (s_probability_write + 1) % PB_VOTE_WINDOWS;
        if (s_probability_count < PB_VOTE_WINDOWS) s_probability_count++;

        int16_t top3[3];
        float score3[3];
        uint32_t merged;
        vote(top3, score3, &merged);

        for (int r = 0; r < 3; r++) {
            s_state.top3[r] = top3[r];
            s_state.top3_probability[r] = score3[r];
        }
        s_state.merged = merged;
        s_state.valid = true;
        __dmb();                 /* body must be visible BEFORE the version */
        s_state.version++;
    }
}

bool pb_recognizer_start(bool gate_ignore) {
    if (s_run) return true;
    if (!pb_binary_net_init()) return false;
    if (!pb_species_net_init()) return false;

    s_gate_ignore = gate_ignore;

    memset((void *)&s_state, 0, sizeof(s_state));
    s_probability_count = 0;
    s_probability_write = 0;
    s_mel_write = 0;
    s_mel_read = 0;
    s_run = true;

    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_loop, s_core1_heap,
                                      sizeof(s_core1_heap));
    return true;
}

void pb_recognizer_stop(void) {
    if (!s_run) return;
    s_run = false;
    /* Core 1 leaves the loop at most one read timeout (1 s) later. Resetting
     * without waiting could cut a half-finished Invoke, so give it a short
     * margin first. */
    sleep_ms(400);
    multicore_reset_core1();
}

bool pb_recognizer_get_mel(int8_t *out) {
    if (!out) return false;
    const uint32_t read = s_mel_read;
    if (read == s_mel_write) return false;
    __dmb();
    memcpy(out, s_mel_queue[read], PB_MEL_BANDS);
    __dmb();
    s_mel_read = (read + 1u) % MEL_QUEUE;
    return true;
}

void pb_recognizer_read(pb_recognizer_state_t *out) {
    if (!out) return;
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t v0 = s_state.version;
        __dmb();
        memcpy(out, (const void *)&s_state, sizeof(*out));
        __dmb();
        if (s_state.version == v0) return;   /* no torn read */
    }
}
