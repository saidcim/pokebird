/**
 * dsp_test.c — host-side tests for the DSP pipeline
 *
 * The DSP functions are compiled on the PC and compared against a Python
 * reference. Debugging a DSP fault on the microcontroller is very expensive;
 * these tests remove that cost.
 *
 * Two modes:
 *   dsp_test           run the tests and print pass/fail
 *   dsp_test --dump    print a single mel frame as CSV (for comparison
 *                      against Python; see tools/mel_reference.py)
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai/decision.h"
#include "dsp/fft.h"
#include "dsp/gate.h"
#include "dsp/mel.h"
#include "ui/text.h"

static int g_fail = 0;
static int g_run  = 0;

static void check(bool ok, const char *name, const char *detail) {
    g_run++;
    if (ok) {
        printf("  [pass] %s\n", name);
    } else {
        g_fail++;
        printf("  [FAIL] %s - %s\n", name, detail);
    }
}

/** Generate a sine of amplitude `amp` relative to full scale. */
static void sine(int16_t *out, int n, float hz, float amp) {
    for (int i = 0; i < n; i++) {
        float v = amp * 32767.0f * sinf(2.0f * (float)M_PI * hz * (float)i /
                                        (float)PB_SAMPLE_RATE);
        out[i] = (int16_t)lrintf(v);
    }
}

/* ── FFT scaling ──────────────────────────────────────────────────────────
 * A full-scale sine sitting on a bin centre must give a power of about 1.0 in
 * its own bin. If that does not hold, every dB reading shifts, and mel shifts
 * with it. */
static void test_fft_scale(void) {
    printf("FFT scaling:\n");

    const int bin = 64;
    const float hz = (float)bin * PB_SAMPLE_RATE / PB_FFT_SIZE;
    int16_t buf[PB_FFT_SIZE];
    sine(buf, PB_FFT_SIZE, hz, 1.0f);

    float power[PB_FFT_POWER_BINS];
    pb_fft_power(buf, power);

    int peak = 0;
    for (int k = 1; k < PB_FFT_POWER_BINS; k++) {
        if (power[k] > power[peak]) peak = k;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "peak bin %d, expected %d", peak, bin);
    check(peak == bin, "peak is in the right bin", msg);

    snprintf(msg, sizeof(msg), "power %.4f, expected ~1.0", (double)power[bin]);
    check(power[bin] > 0.85f && power[bin] < 1.15f,
          "full-scale sine gives ~1.0 power", msg);

    /* Half amplitude -> -6 dB */
    sine(buf, PB_FFT_SIZE, hz, 0.5f);
    pb_fft_power(buf, power);
    float db = 10.0f * log10f(power[bin]);
    snprintf(msg, sizeof(msg), "%.2f dB, expected ~-6", (double)db);
    check(db > -7.0f && db < -5.0f, "half amplitude is -6 dB", msg);
}

/* ── Mel filter bank ──────────────────────────────────────────────────────
 * The band centres must rise with frequency, and the peak band must follow
 * the input frequency. */
static void test_mel_peak(void) {
    printf("Mel band mapping:\n");
    pb_mel_init();

    const float frequencies[] = { 500.0f, 1000.0f, 3000.0f, 8000.0f };
    int previous_peak = -1;
    bool rising = true;

    for (size_t i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); i++) {
        int16_t buf[PB_FFT_SIZE];
        sine(buf, PB_FFT_SIZE, frequencies[i], 0.9f);

        int8_t mel[PB_MEL_BANDS];
        pb_mel_frame(buf, mel);

        int peak = 0;
        for (int b = 1; b < PB_MEL_BANDS; b++) if (mel[b] > mel[peak]) peak = b;

        printf("        %6.0f Hz -> bant %2d (%.1f dB)\n",
               (double)frequencies[i], peak, (double)pb_mel_q_to_db(mel[peak]));

        if (previous_peak >= 0 && peak <= previous_peak) rising = false;
        previous_peak = peak;
    }
    check(rising, "peak band rises with frequency", "monotonicity broken");
}

/* Silence must settle onto the floor; otherwise the gate fires constantly. */
static void test_mel_silence(void) {
    printf("Mel silence:\n");
    int16_t buf[PB_FFT_SIZE];
    memset(buf, 0, sizeof(buf));

    int8_t mel[PB_MEL_BANDS];
    pb_mel_frame(buf, mel);

    bool all_base = true;
    for (int b = 0; b < PB_MEL_BANDS; b++) if (mel[b] != -128) all_base = false;
    check(all_base, "silence settles on the floor", "some bands are not at the floor");
}

/* ── Ring buffer and window normalisation ────────────────────────────────── */
static void test_mel_window(void) {
    printf("Mel ring buffer:\n");
    pb_mel_reset();

    int8_t *window = malloc((size_t)PB_MEL_BANDS * PB_MEL_FRAMES);
    check(!pb_mel_window(window), "no window before it is full", "returned true early");

    int16_t buf[PB_FFT_SIZE];
    for (int f = 0; f < PB_MEL_FRAMES; f++) {
        /* A tone that changes from frame to frame: normalisation would be
         * meaningless on a constant input (zero variance). */
        sine(buf, PB_FFT_SIZE, 1000.0f + 20.0f * (float)f, 0.5f);
        pb_mel_push(buf);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "frame count %u", pb_mel_frame_count());
    check(pb_mel_frame_count() == PB_MEL_FRAMES, "187 frames accumulated", msg);
    check(pb_mel_window(window), "window is ready", "returned false");

    /* After normalisation the mean should be about 0. */
    double total = 0.0;
    const int n = PB_MEL_BANDS * PB_MEL_FRAMES;
    for (int i = 0; i < n; i++) total += window[i];
    double avg = total / n;
    snprintf(msg, sizeof(msg), "mean %.2f", avg);
    check(fabs(avg) < 8.0, "normalised window has a mean of ~0", msg);

    /* The ring must really wrap: push 187 more frames and check it does not
     * overflow. */
    for (int f = 0; f < PB_MEL_FRAMES; f++) {
        sine(buf, PB_FFT_SIZE, 2000.0f, 0.5f);
        pb_mel_push(buf);
    }
    snprintf(msg, sizeof(msg), "frame count %u", pb_mel_frame_count());
    check(pb_mel_frame_count() == 2 * PB_MEL_FRAMES, "the ring wraps", msg);

    free(window);
}

/* ── Gate ─────────────────────────────────────────────────────────────────
 * Silence and a steady drone must NOT open the gate; a sudden tone MUST. */
static void test_gate(void) {
    printf("Gate (VAD):\n");
    int16_t buf[PB_FFT_SIZE];
    float power[PB_FFT_POWER_BINS];

    /* 1) Silence */
    pb_gate_reset();
    bool opened_in_silence = false;
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 40; i++) {
        pb_fft_power(buf, power);
        if (pb_gate_update(power).active) opened_in_silence = true;
    }
    check(!opened_in_silence, "does not open in silence", "the gate fired");

    /* 2) A steady drone: high energy but an unchanging shape */
    pb_gate_reset();
    bool opened_on_drone = false;
    for (int i = 0; i < 60; i++) {
        sine(buf, PB_FFT_SIZE, 5000.0f, 0.3f);
        pb_fft_power(buf, power);
        pb_gate_result_t r = pb_gate_update(power);
        if (i > 20 && r.active) opened_on_drone = true;   /* let the floor settle */
    }
    check(!opened_on_drone, "does not open on a steady drone", "the gate fired");

    /* 3) A sudden tone after silence */
    pb_gate_reset();
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 40; i++) { pb_fft_power(buf, power); pb_gate_update(power); }

    sine(buf, PB_FFT_SIZE, 4000.0f, 0.5f);
    pb_fft_power(buf, power);
    pb_gate_result_t r = pb_gate_update(power);
    char msg[160];
    snprintf(msg, sizeof(msg), "band %.1f dB, floor %.1f dB, flux %.3f",
             (double)r.band_db, (double)r.floor_db, (double)r.flux);
    check(r.active, "a sudden tone opens the gate", msg);
    printf("        %s\n", msg);
}

/* ── The decision rule (ai/decision.c) ────────────────────────────────────
 * Whether the screen stays steady depends on this rule, and the whole rule
 * depends on TIME: hysteresis, the hold, the sound indicator fading. Testing
 * it on the board would mean waiting real seconds; here we advance time by
 * hand. */
/** Advance one step as though a new vote result had arrived. */
static void result(pb_decision_t *k, uint32_t ms, int16_t cls, float p,
                  uint32_t merged) {
    pb_decision_input_t g = { .now_ms = ms, .gate_open = true,
                           .fresh_result = true, .cls = cls,
                           .probability = p, .merged = merged };
    pb_decision_update(k, &g);
}

/** Advance one step with NO new result (this is where the timeouts run). */
static void empty(pb_decision_t *k, uint32_t ms, bool gate) {
    pb_decision_input_t g = { .now_ms = ms, .gate_open = gate,
                           .fresh_result = false, .cls = -1,
                           .probability = 0.0f, .merged = 0 };
    pb_decision_update(k, &g);
}

static void test_decision(void) {
    printf("Decision rule (threshold + hysteresis + hold):\n");
    pb_decision_t k;
    char msg[160];

    /* 1) At startup, with the gate never opened, the mode is "listening". */
    pb_decision_reset(&k, 10000);
    empty(&k, 10000, false);
    check(k.mode == PB_DECISION_LISTENING && k.cls < 0,
          "listening at startup", "started in another mode");

    /* 2) Opening the gate lights the sound indicator; closing it fades after
     *    SOUND_HOLD_MS. */
    empty(&k, 10100, true);
    const bool sound_lit = (k.mode == PB_DECISION_SOUND);
    empty(&k, 10100 + PB_DECISION_SOUND_HOLD_MS + 1, false);
    check(sound_lit && k.mode == PB_DECISION_LISTENING,
          "the sound indicator fades after the gate opens and closes",
          sound_lit ? "it did not fade" : "it never lit");

    /* 3) A confidence between the exit and enter thresholds does not print a
     *    species. */
    pb_decision_reset(&k, 20000);
    result(&k, 20000, 5, 0.5f * (PB_DECISION_ENTER_THRESHOLD + PB_DECISION_EXIT_THRESHOLD),
          PB_DECISION_MIN_WINDOWS);
    check(k.mode == PB_DECISION_UNSURE && k.cls == 5,
          "confidence between the thresholds = unsure", "wrong mode");

    /* 4) Crossing the enter threshold gives SPECIES. */
    result(&k, 21000, 5, PB_DECISION_ENTER_THRESHOLD + 0.05f, PB_DECISION_MIN_WINDOWS);
    check(k.mode == PB_DECISION_SPECIES && k.cls == 5,
          "enter threshold -> SPECIES", "did not switch to SPECIES");

    /* 5) HYSTERESIS: even when the confidence falls BELOW the enter
     *    threshold, the same species stays as SPECIES for as long as it
     *    remains above the exit threshold. This is the clause that keeps the
     *    screen from jumping. */
    for (uint32_t t = 22000; t <= 25000; t += 1000) {
        result(&k, t, 5, PB_DECISION_EXIT_THRESHOLD + 0.02f, PB_DECISION_MIN_WINDOWS);
    }
    snprintf(msg, sizeof(msg), "mode %d class %d", (int)k.mode, (int)k.cls);
    check(k.mode == PB_DECISION_SPECIES && k.cls == 5,
          "below enter but above exit: SPECIES is kept", msg);

    /* 6) A different species can only take over via the ENTER threshold. */
    result(&k, 26000, 9, PB_DECISION_EXIT_THRESHOLD + 0.02f, PB_DECISION_MIN_WINDOWS);
    const bool unchanged = (k.cls == 5);
    result(&k, 27000, 9, PB_DECISION_ENTER_THRESHOLD + 0.05f, PB_DECISION_MIN_WINDOWS);
    check(unchanged && k.cls == 9,
          "a new species takes over only at the enter threshold",
          unchanged ? "it did not change at the enter threshold either"
                    : "it changed at the exit threshold");

    /* 7) When support stops it stays on screen for HOLD_MS, then clears. */
    empty(&k, 27000 + PB_DECISION_HOLD_MS - 100, false);
    const bool still_up = (k.mode == PB_DECISION_SPECIES && k.cls == 9);
    empty(&k, 27000 + PB_DECISION_HOLD_MS + 100, false);
    snprintf(msg, sizeof(msg), "mode %d class %d", (int)k.mode, (int)k.cls);
    check(still_up && k.mode == PB_DECISION_LISTENING && k.cls < 0,
          "clears at the end of the hold once support stops",
          still_up ? msg : "cleared before the hold expired");

    /* 8) The negative class never prints a species name at any confidence —
     *    the most expensive mistake in the field is "mistaking noise for a
     *    bird" (models/thresholds.txt). */
    pb_decision_reset(&k, 40000);
    result(&k, 40000, PB_DECISION_NEGATIVE_CLASS, 0.99f, PB_DECISION_MIN_WINDOWS);
    check(k.cls < 0 && k.mode != PB_DECISION_SPECIES,
          "the negative class prints no species", "the negative class was shown");

    /* 9) With too few windows voted there is no decision: measured, the
     *    precision is 70.1% at one window and 84.9% at three
     *    (models/thresholds.txt). */
    pb_decision_reset(&k, 50000);
    result(&k, 50000, 5, 0.95f, PB_DECISION_MIN_WINDOWS - 1);
    check(k.cls < 0, "no decision with too few windows", "decided too early");
}

/** A single frame as CSV, for comparison against the Python reference. */
static void dump_frame(void) {
    int16_t buf[PB_FFT_SIZE];
    /* A deterministic, reproducible input: two tones plus a sawtooth. */
    for (int i = 0; i < PB_FFT_SIZE; i++) {
        float t = (float)i / (float)PB_SAMPLE_RATE;
        float v = 0.40f * sinf(2.0f * (float)M_PI * 1000.0f * t)
                + 0.25f * sinf(2.0f * (float)M_PI * 4300.0f * t)
                + 0.10f * ((float)(i % 97) / 97.0f - 0.5f);
        buf[i] = (int16_t)lrintf(v * 32767.0f);
    }
    int8_t mel[PB_MEL_BANDS];
    pb_mel_frame(buf, mel);

    printf("band,q,db\n");
    for (int b = 0; b < PB_MEL_BANDS; b++) {
        printf("%d,%d,%.4f\n", b, mel[b], (double)pb_mel_q_to_db(mel[b]));
    }
}

/**
 * Dump a COMPLETE model window (64x187 int8) from a raw int16 file.
 *
 * Why this is needed: `--dump` compares a single frame, which verifies the
 * filter bank and the dB->int8 mapping. The model's real input is the output
 * of `pb_mel_window()`, which additionally involves the frame layout (hop
 * 384) and the per-window mean/variance normalisation. The Python that
 * generates the training set has to match those exactly as well; if it does
 * not, the model is good on the PC and poor on the device, with no error
 * visible anywhere.
 *
 * Input: 24 kHz mono, raw little-endian int16, at least
 * 187*384+128 = 71,936 samples. Frame r = sample[r*384 .. r*384+512).
 */
static int dump_window(const char *path) {
    enum { REQUIRED = (PB_MEL_FRAMES - 1) * PB_MEL_HOP + PB_FFT_SIZE };

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "could not open: %s\n", path); return 2; }

    int16_t *s = (int16_t *)malloc(sizeof(int16_t) * REQUIRED);
    if (!s) { fclose(f); fprintf(stderr, "out of memory\n"); return 2; }

    size_t got = fread(s, sizeof(int16_t), REQUIRED, f);
    fclose(f);
    if (got < REQUIRED) {
        fprintf(stderr, "file too short: %zu samples, need %d\n", got, REQUIRED);
        free(s);
        return 2;
    }

    pb_mel_reset();
    for (int r = 0; r < PB_MEL_FRAMES; r++) pb_mel_push(s + (size_t)r * PB_MEL_HOP);

    int8_t *window = (int8_t *)malloc((size_t)PB_MEL_FRAMES * PB_MEL_BANDS);
    if (!window || !pb_mel_window(window)) {
        fprintf(stderr, "the window could not be formed\n");
        free(window); free(s);
        return 2;
    }

    printf("frame,band,q\n");
    for (int r = 0; r < PB_MEL_FRAMES; r++) {
        for (int b = 0; b < PB_MEL_BANDS; b++) {
            printf("%d,%d,%d\n", r, b, window[(size_t)r * PB_MEL_BANDS + b]);
        }
    }
    free(window);
    free(s);
    return 0;
}

/* ── Uppercasing ─────────────────────────────────────────────
 *
 * WHY THIS IS TESTED: the UI shows species names in UPPER CASE. The
 * conversion is not plain toupper() — it has to leave multi-byte sequences
 * intact, and it must never cut one in half at the buffer edge.
 *
 * REGRESSION: this function used to implement Turkish casing, where `i`
 * uppercases to the dotted `İ`. Correct for Turkish, wrong for the English
 * species names the device now shows — it rendered "Common Nightingale" as
 * "COMMON NİGHTİNGALE". The first two cases below pin that down.
 */
static void expect_text(const char *input, const char *beklenen) {
    char out[64];
    pb_text_upper(input, out, sizeof(out));
    char detail[160];
    snprintf(detail, sizeof(detail), "\"%s\" -> \"%s\", expected \"%s\"",
             input, out, beklenen);
    check(strcmp(out, beklenen) == 0, input, detail);
}

static void test_text_upper(void) {
    printf("Uppercasing\n");

    /* ASCII `i` must stay a plain `I` — the regression described above. */
    expect_text("Common Nightingale", "COMMON NIGHTINGALE");
    expect_text("Eurasian Blackbird", "EURASIAN BLACKBIRD");

    /* Plain ASCII, and a hyphenated name. */
    expect_text("European Robin", "EUROPEAN ROBIN");
    expect_text("Greater White-fronted Goose", "GREATER WHITE-FRONTED GOOSE");

    /* Scientific names are shown as-is elsewhere, but must survive this. */
    expect_text("Anser albifrons", "ANSER ALBIFRONS");

    /* Multi-byte letters the fonts carry still uppercase correctly. */
    expect_text("çğöşü", "ÇĞÖŞÜ");
    expect_text("ı", "I");            /* dotless ı -> I: two bytes collapse to one */

    /* Edge cases. */
    expect_text("", "");

    /* Buffer overflow must not leave half a UTF-8 sequence behind.
     * A 5-byte buffer holds "Ç" (2) + "Ç" (2) + terminator = exactly full. */
    char small[5];
    pb_text_upper("çççç", small, sizeof(small));
    check(strcmp(small, "ÇÇ") == 0, "overflow leaves no partial UTF-8", small);

    /* NULL input must not crash; it yields an empty string. */
    char nl[8] = "xxx";
    pb_text_upper(NULL, nl, sizeof(nl));
    check(nl[0] == '\0', "NULL input yields empty string", nl);

    printf("\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump_frame();
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--pencere") == 0) {
        return dump_window(argv[2]);
    }

    printf("PokeBird DSP testleri\n=====================\n\n");
    test_fft_scale();
    test_mel_peak();
    test_mel_silence();
    test_mel_window();
    test_gate();
    test_decision();
    test_text_upper();

    printf("\n%d tests, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
