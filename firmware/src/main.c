/**
 * PokeBird — a bird-song recognition device for Istanbul
 *
 * This file is the firmware entry point and the diagnostic console.
 *
 * Beyond running the device, it carries the bring-up and measurement
 * commands the project was built with. The riskiest assumption in the plan
 * was whether the board's analog MEMS microphone is clean enough for bird
 * recognition given the noise of the display, the QSPI bus and the switching
 * regulator all sharing the same small PCB — most of these commands exist to
 * measure exactly that, and they were kept because they are what makes a
 * fault diagnosable without a logic analyser.
 *
 * Commands arrive over the USB serial port; press `?` for the full list.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"

#include "board_config.h"
#include "hal/audio_i2s.h"
#include "hal/es8311.h"
#include "hal/i2c_bus.h"
#include "hal/touch.h"
#include "hal/display/LCD_3in49.h"
#include "hal/display/lcd_blit.h"
#include "hal/display/qspi_pio.h"
#include "dsp/fft.h"
#include "dsp/mel.h"
#include "dsp/gate.h"
#include "ui/spectrogram.h"
#include "ui/lv_port.h"
#include "ui/theme.h"
#include "ai/species_net.h"
#include "ai/binary_net.h"
#include "ai/recognizer.h"
#include "ai/decision.h"
#include "ai/classes.h"
#include "ai/validation_set.h"
#include "ai/binary_validation_set.h"
#include "ui/interface.h"
#include "lvgl.h"

void pb_display_dma_init(void);   /* hal/display/dev_config.c */

/* ── Compile-time hardware checks ─────────────────────────────────────────
 * If the wrong board is selected these stop the build. Otherwise the code
 * compiles silently and the fault only shows up on the board, as GPIO40 never
 * moving — which is very expensive to find there. */
static_assert(PICO_RP2350A == 0,
              "RP2350B not selected. PICO_BOARD must be pokebird_rp2350b "
              "(firmware/boards/pokebird_rp2350b.h).");
static_assert(NUM_BANK0_GPIOS >= 48,
              "48 GPIOs expected. BAT_ADC (GPIO40), SD_CS (GPIO31) and the "
              "free header pins (41-47) do not exist on the RP2350A.");
static_assert(PB_PIN_BAT_ADC < NUM_BANK0_GPIOS,
              "The BAT_ADC pin is outside the GPIO range.");
static_assert(PICO_FLASH_SIZE_BYTES == 16 * 1024 * 1024,
              "16 MB of flash expected (PY25Q128HA).");

/* The decision rule knows the negative class's index as a constant
 * (ai/decision.h). If the class table is regenerated and the class count
 * changes, the build must stop here: a shift would mean "mistaking noise for
 * a bird", and it would raise no error anywhere. */
static_assert(PB_DECISION_NEGATIVE_CLASS == PB_CLASS_COUNT - 1,
              "Negative class index drift: ai/decision.h and ai/classes.h "
              "disagree (did tools/class_table.py run again?).");

#define BL_PWM_WRAP     2048
#define ES8311_I2C_ADDR 0x18

/**
 * Capture chunk — the largest block read from the stream at once.
 *
 * There used to be a contiguous two-second buffer here: 24 kHz x 16 bit =
 * 96,000 bytes, half of bss on its own. None of the diagnostic commands using
 * it actually needed to see two seconds at once — they are all either
 * accumulators (RMS, peak, DC) or work window by window. Since the continuous
 * capture ring arrived (hal/audio_i2s.c) the data can be read chunk by chunk
 * without interruption, so the buffer was cut to one chunk.
 *
 * That saved 96,000 -> 4,096 bytes. The TFLM arena only fits after this space
 * was freed.
 */
#define CHUNK_SAMPLES   PB_AUDIO_MAX_READ       /* 2048 samples = 4096 bytes */
static int16_t s_chunk[CHUNK_SAMPLES];

/* Recording length for the 'r' command. */
#define CAPTURE_SECONDS 2
#define CAPTURE_SAMPLES (PB_SAMPLE_RATE * CAPTURE_SECONDS)

static const pb_audio_cfg_t s_audio_cfg = {
    .mclk_freq   = PB_MCLK_RATE,
    .sample_freq = PB_SAMPLE_RATE,
    .res_in      = PB_BITS_PER_SAMPLE,
    .res_out     = PB_BITS_PER_SAMPLE,
};

static uint8_t s_mic_gain = PB_MIC_GAIN;

/* ── Display ──────────────────────────────────────────────────────────────
 * The backlight is a variable in the noise measurement: the AP3032 boost
 * converter and its PWM switch right next to the microphone. */

/**
 * Backlight — plain GPIO, NO PWM.
 *
 * VERIFIED BEHAVIOUR (the interactive 'b' test, measured on the board):
 *     BL_EN (GPIO37) = 1  and  LCD_BL (GPIO36) = 0   ->  THE LIGHT COMES ON
 * So LCD_BL is active-low. The working driver in rsvpnano says the same
 * ("active-low PWM; lower duty is brighter"), and so does Waveshare's own
 * code (pwm_set_chan_level(slice, CHAN_A, 100 - Value)).
 *
 * WHY WE DO NOT USE PWM:
 * With PWM at 0% duty — electrically the pin held permanently LOW, the same
 * condition that lights the backlight above — the light does NOT come on.
 * With plain GPIO, LOW does light it. So the PWM peripheral is not driving
 * this pin the way we expect (probably something about GPIO36's slice mapping
 * on the RP2350B). Rather than chase the root cause we use the mechanism
 * verified to work.
 *
 * THE COST: no brightness control, only on/off. That is unimportant for now —
 * the EMI sweep measured the backlight's effect on the microphone at +0.2 dB,
 * so there is no acoustic reason to dim it either. If graduated brightness is
 * ever wanted, the PWM problem can be solved separately then.
 */
/**
 * Latch the power rail on.
 *
 * Waveshare's example calls `DEV_Module_Init()` before touching the display;
 * we never took it, to avoid clashing with our own HAL. The one critical
 * thing it does is hold SYS_EN high: the board stays powered through this
 * latch, and the example powers itself down with
 * `DEV_Digital_Write(SYS_EN, 0)`.
 *
 * If the panel's logic/IO supply sits behind this latch, then even while the
 * panel keeps scanning, its host interface would be unpowered and would hear
 * no QSPI command at all — which matches the symptoms we saw exactly. That is
 * unproven, but it is cheap and harmless, so we do it.
 */
static void power_latch_init(void) {
    gpio_init(PB_PIN_SYS_EN);
    gpio_set_dir(PB_PIN_SYS_EN, GPIO_OUT);
    gpio_put(PB_PIN_SYS_EN, 1);     /* 1 = stay on. 0 POWERS OFF. */
}

static void backlight_init(void) {
    gpio_init(PB_PIN_BL_EN);
    gpio_set_dir(PB_PIN_BL_EN, GPIO_OUT);
    gpio_put(PB_PIN_BL_EN, 1);

    gpio_init(PB_PIN_LCD_BL);
    gpio_set_dir(PB_PIN_LCD_BL, GPIO_OUT);
    gpio_put(PB_PIN_LCD_BL, 0);         /* active-low: 0 = lit */
}

static void backlight_set(bool on) {
    gpio_put(PB_PIN_BL_EN, on ? 1 : 0);
    gpio_put(PB_PIN_LCD_BL, on ? 0 : 1);
}

/* ── Measurement ──────────────────────────────────────────────────────────
 * The full-scale 16-bit reference is 32768. dBFS = 20*log10(rms/32768). */

typedef struct {
    double  rms;
    double  dbfs;
    int32_t peak;
    double  dc_offset;
} audio_stats_t;

/* We do not roll our own approximation for log10; the SDK's optimised
 * log10f is already linked in. */
#include <math.h>

/**
 * Streaming accumulator — statistics in a single pass.
 *
 * The old `compute_stats` took TWO passes: first it found the DC mean, then
 * it walked the same array again computing RMS relative to that mean. That
 * required every sample to stay in memory. Reading chunk by chunk there is no
 * data for a second pass — once a chunk is processed the next one overwrites
 * it.
 *
 * The fix is the variance identity:  rms^2 = sumsq/n - (sum/n)^2
 * so the raw sums can be accumulated and DC removed only at the end.
 *
 * `double` makes this safe: 48,000 samples x 32768^2 is about 5.2e13, and
 * double is exact for integers up to 9e15. The two approaches were compared
 * on the host with identical data: on a real recording from the board the
 * deviation was 3.6e-14 dB, and even in a contrived worst case (DC of 20000
 * with +/-3 of AC) it was 3.6e-8 dB. The acceptance limit was 0.1 dB.
 */
typedef struct {
    double   sum;
    double   sumsq;
    int32_t  peak;
    uint32_t n;
} stats_acc_t;

static void stats_add(stats_acc_t *a, const int16_t *x, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double v = (double)x[i];
        a->sum   += v;
        a->sumsq += v * v;
        int32_t m = x[i] < 0 ? -(int32_t)x[i] : (int32_t)x[i];
        if (m > a->peak) a->peak = m;
    }
    a->n += n;
}

static audio_stats_t stats_finish(const stats_acc_t *a) {
    audio_stats_t st = { 0 };
    if (a->n == 0) return st;

    double mean = a->sum / (double)a->n;
    double var  = a->sumsq / (double)a->n - mean * mean;
    if (var < 0.0) var = 0.0;        /* rounding can push it below zero */

    st.dc_offset = mean;
    st.rms       = sqrt(var);
    st.peak      = a->peak;
    st.dbfs      = (st.rms > 0.0) ? 20.0 * log10(st.rms / 32768.0) : -999.0;
    return st;
}

/** Convenience wrapper for a small in-memory buffer. */
static audio_stats_t compute_stats(const int16_t *x, uint32_t n) {
    stats_acc_t acc = { 0 };
    stats_add(&acc, x, n);
    return stats_finish(&acc);
}

/**
 * Measure the noise floor with a percentile.
 *
 * A plain RMS is dragged upwards by a single door slam or cough during the
 * measurement — a room is never completely silent. Splitting the signal into
 * short windows and taking the 10th percentile of the window RMS values is
 * robust against transients and gives a far more honest number for "the
 * quietest moment".
 */
/* The window count went from 64 to 48 and the window length was aligned to
 * the chunk size.
 *
 * Two seconds used to be read in one go and split into 64 (750 samples per
 * window). On the stream the window has to align with the read chunk,
 * otherwise windows straddle a chunk boundary. 48 windows of 1024 samples =
 * 49,152 samples = 2.048 s.
 *
 * A SIDE EFFECT worth documenting: the percentile index is `count/10`, so
 * with 64 windows it picked element 6 (9.4%) and with 48 it picks element 4
 * (8.3%). The "10th percentile" therefore shifted slightly. The result is
 * diagnostic, but keep the shift in mind when comparing against the earlier
 * -36 dBFS floor. */
#define NOISE_WINDOWS 48
#define NOISE_WIN_LEN 1024      /* 42.7 ms @ 24 kHz — divides the chunk size */

static double window_rms(const int16_t *x, uint32_t n) {
    stats_acc_t acc = { 0 };
    stats_add(&acc, x, n);
    return stats_finish(&acc).rms;
}

/** Sort and return the 10th percentile (the list is modified in place). */
static double percentile10(double *v, uint32_t count) {
    /* ascending (count is small, a simple insertion sort is enough) */
    for (uint32_t i = 1; i < count; i++) {
        double t = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > t) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
    return v[count / 10];
}

static void print_stats(const char *label, const audio_stats_t *st,
                        const pb_capture_result_t *cap) {
    printf("  %-22s RMS %8.1f  %7.1f dBFS  peak %6ld  DC %8.1f",
           label, st->rms, st->dbfs, (long)st->peak, st->dc_offset);
    if (cap->fifo_overrun) printf("   [!] SAMPLES DROPPED");
    if (cap->timed_out)    printf("   [!] NO CLOCK");
    printf("\n");
}

/**
 * Read `total` samples from the stream and accumulate statistics only — the
 * raw data is not kept, each chunk overwrites the last.
 *
 * A TRAP: flush is called ONCE, before the loop. Calling `pb_audio_capture`
 * per chunk looks tempting (the signature fits perfectly) but that function
 * is a flush+read wrapper: it discards the backlog on every call. Called
 * chunk by chunk it would drop the samples BETWEEN chunks and silently
 * corrupt the measurement — and `fifo_overrun` would not report the loss,
 * because the ring never overflowed, we threw the data away ourselves.
 */
static pb_capture_result_t stream_stats(uint32_t total, audio_stats_t *out) {
    pb_capture_result_t res = { 0 };
    stats_acc_t acc = { 0 };

    pb_audio_stream_flush();
    while (res.samples < total) {
        uint32_t want = total - res.samples;
        if (want > CHUNK_SAMPLES) want = CHUNK_SAMPLES;

        pb_capture_result_t part = pb_audio_stream_read(s_chunk, want, 1000);
        res.samples      += part.samples;
        res.fifo_overrun |= part.fifo_overrun;
        res.timed_out    |= part.timed_out;
        stats_add(&acc, s_chunk, part.samples);

        if (part.timed_out) break;
    }
    if (out) *out = stats_finish(&acc);
    return res;
}

/* Set the backlight to the given state and wait for the power rail to
 * settle before measuring. */
static audio_stats_t measure_with_backlight(bool enable,
                                            pb_capture_result_t *cap_out) {
    backlight_set(enable);
    sleep_ms(250);

    audio_stats_t st;
    pb_capture_result_t cap = stream_stats(PB_SAMPLE_RATE / 2, &st);  /* 0.5 s */
    if (cap_out) *cap_out = cap;
    return st;
}

/* ── Commands ─────────────────────────────────────────────────────────────── */

static void cmd_info(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    printf("\n--- Device ---\n");
    printf("  MCU          RP2350%s @ %lu Hz\n",
           PICO_RP2350A ? "A" : "B", (unsigned long)clock_get_hz(clk_sys));
    printf("  Flash        %d MB\n", PICO_FLASH_SIZE_BYTES / (1024 * 1024));
    printf("  Board ID     ");
    for (size_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) printf("%02x", id.id[i]);
    printf("\n");
    printf("--- Audio ---\n");
    printf("  MCLK         %lu Hz (PIO, GPIO%d)\n",
           (unsigned long)s_audio_cfg.mclk_freq, PB_PIN_I2S_MCLK);
    printf("  Sample rate  %lu Hz (ES8311 master, MCLK/256)\n",
           (unsigned long)s_audio_cfg.sample_freq);
    printf("  Microphone   analog MEMS -> ES8311 ADC, gain %u\n", s_mic_gain);
    printf("  ES8311 ID    0x%04x %s\n", es8311_read_id(),
           pb_i2c_probe(ES8311_I2C_ADDR) ? "(I2C responds)" : "(NO I2C RESPONSE)");
    printf("\n");
}

static void cmd_noise(void) {
    const uint32_t total = (uint32_t)NOISE_WINDOWS * NOISE_WIN_LEN;

    printf("\nNoise floor measurement (%.2f s). Keep the room quiet...\n",
           (double)total / PB_SAMPLE_RATE);
    backlight_set(false);
    sleep_ms(300);

    /* Read window by window: each window feeds both the overall statistics
     * and, via its own RMS, the percentile list. No raw data is kept. */
    pb_capture_result_t cap = { 0 };
    stats_acc_t acc = { 0 };
    double rms_list[NOISE_WINDOWS];
    uint32_t count = 0;

    pb_audio_stream_flush();     /* ONCE, before the loop (see stream_stats) */
    for (uint32_t w = 0; w < NOISE_WINDOWS; w++) {
        pb_capture_result_t part =
            pb_audio_stream_read(s_chunk, NOISE_WIN_LEN, 1000);
        cap.samples      += part.samples;
        cap.fifo_overrun |= part.fifo_overrun;
        cap.timed_out    |= part.timed_out;

        if (part.samples < NOISE_WIN_LEN) break;   /* no clock — short window */

        stats_add(&acc, s_chunk, part.samples);
        rms_list[count++] = window_rms(s_chunk, part.samples);
    }
    audio_stats_t st = stats_finish(&acc);

    printf("  Captured     %lu / %lu samples  (%lu / %d windows)\n",
           (unsigned long)cap.samples, (unsigned long)total,
           (unsigned long)count, NOISE_WINDOWS);
    print_stats("all windows (RMS)", &st, &cap);

    double floor_rms = (count > 0) ? percentile10(rms_list, count) : 0.0;
    double floor_db  = (floor_rms > 0.0)
                     ? 20.0 * log10(floor_rms / 32768.0) : -999.0;
    printf("  %-22s RMS %8.1f  %7.1f dBFS   <- robust to transients\n",
           "noise floor (P10)", floor_rms, floor_db);

    if (cap.timed_out) {
        printf("\n  [!] The ES8311 is not producing a clock. Check: is MCLK\n");
        printf("      running, does the codec answer on I2C, was the master\n");
        printf("      mode register written?\n");
    } else if (st.rms < 1.0) {
        printf("\n  [!] The signal is exactly zero. The microphone path may not\n");
        printf("      be enabled (ES8311 REG14 analog mic / PGA settings).\n");
    } else {
        printf("\n  Reading: a %.0f dBFS floor, ", floor_db);
        if (floor_db < -60.0)      printf("good - enough for bird recognition.\n");
        else if (floor_db < -45.0) printf("acceptable, but run the EMI sweep ('e').\n");
        else                       printf("HIGH - room noise or circuit noise? Change\n"
                                          "         the gain with 'g' and look again (if the\n"
                                          "         reading scales with gain, it is acoustic).\n");
    }
    printf("\n");
}

/* Live level meter. The most practical way to confirm the microphone really
 * hears something: clap, whistle or talk — the bar should react instantly. */
static void cmd_level_meter(void) {
    printf("\nLive level. Clap or talk. Press any key to exit.\n\n");
    /* 2048 samples = 85 ms. It used to be 100 ms; it was cut to the chunk
     * size. Being a live display, jumping to the freshest data each turn is
     * exactly what we want, so the flush+read wrapper (pb_audio_capture) is
     * the RIGHT choice here. */
    const uint32_t win = CHUNK_SAMPLES;

    while (getchar_timeout_us(0) < 0) {
        pb_capture_result_t cap = pb_audio_capture(s_chunk, win);
        if (cap.samples == 0) break;
        audio_stats_t st = compute_stats(s_chunk, cap.samples);

        int bars = (int)((st.dbfs + 80.0) / 2.0);   /* -80 dBFS -> 0, 0 dBFS -> 40 */
        if (bars < 0) bars = 0;
        if (bars > 40) bars = 40;

        printf("\r  %6.1f dBFS  peak %5ld  [", st.dbfs, (long)st.peak);
        for (int i = 0; i < 40; i++) putchar(i < bars ? '#' : ' ');
        printf("]");
        stdio_flush();   /* fflush() drags in newlib's stdio locks; the SDK's
                          * own flush does not bring that dependency */
    }
    printf("\n\n");
}

static void cmd_emi_sweep(void) {
    printf("\nEMI sweep - the backlight's effect on the microphone.\n");
    printf("Each measurement is 0.5 s. Keep the room quiet.\n\n");

    pb_capture_result_t cap;
    audio_stats_t off    = measure_with_backlight(false, &cap);
    print_stats("backlight OFF", &off, &cap);

    audio_stats_t full   = measure_with_backlight(true, &cap);
    print_stats("full on (no PWM)", &full, &cap);

    audio_stats_t pwm50  = measure_with_backlight(true, &cap);
    print_stats("PWM %50", &pwm50, &cap);

    audio_stats_t pwm10  = measure_with_backlight(true, &cap);
    print_stats("PWM %10", &pwm10, &cap);

    backlight_set(false);

    double worst = pwm50.dbfs > pwm10.dbfs ? pwm50.dbfs : pwm10.dbfs;
    if (full.dbfs > worst) worst = full.dbfs;
    double delta = worst - off.dbfs;

    printf("\n  Worst case, relative to off: %+.1f dB.\n", delta);
    if (delta < 3.0) {
        printf("  The backlight does not disturb the microphone. Listening\n"
               "  with the screen on is fine.\n");
    } else if (delta < 10.0) {
        printf("  There is a measurable effect. Consider changing the PWM\n");
        printf("  frequency, or fixing the brightness while listening.\n");
    } else {
        printf("  [!] Serious interference. Options: a fixed brightness instead\n");
        printf("      of PWM, shifting the PWM frequency, or an external I2S\n");
        printf("      MEMS microphone on the free GPIOs (12-19).\n");
    }
    printf("\n");
}

static void cmd_gain(void) {
    printf("\nGain (0-7), currently %u. Enter a new value: ", s_mic_gain);
    int c = getchar_timeout_us(10 * 1000 * 1000);
    if (c < '0' || c > '7') {
        printf("cancelled\n\n");
        return;
    }
    s_mic_gain = (uint8_t)(c - '0');
    es8311_microphone_gain_set((es8311_mic_gain_t)s_mic_gain);
    printf("set to %u\n\n", s_mic_gain);
}

/* Stream raw samples to the PC. The framing is simple and self-describing;
 * tools/capture_wav.py turns it into a WAV. */
/**
 * Recording is streamed: rather than buffering two seconds and then printing
 * them, chunks are read and forwarded immediately. Three details matter:
 *
 * 1. ORDER. `tools/capture_wav.py` reads `samples=N` from the header and then
 *    waits for N samples, so the header must go out BEFORE the samples. But
 *    the statistics are only ready once the stream ends. Hence the order:
 *    header -> samples -> #WAV-END -> statistics. (The tool stops reading at
 *    #WAV-END so it never shows the statistics; they appear in the serial
 *    terminal.)
 *
 * 2. THE CLOCK CHECK COMES BEFORE THE HEADER. There is no backing out after
 *    printing the header: `samples=N` is a promise. So the first chunk is
 *    read before the header, and if there is no clock we leave without
 *    printing one at all.
 *
 * 3. KEEPING UP WITH REAL TIME. Because printing is interleaved with reading,
 *    a transfer slower than real time would overflow the ring and leave a gap
 *    in the WAV. Measured on the board: CDC does 276 KB/s and the decimal
 *    format needs 102 KB/s — 2.7x of headroom. Even so, every chunk's
 *    `fifo_overrun` is accumulated and reported loudly at the end: silent
 *    corruption has cost this project dearly twice, and a gap must not pass
 *    unnoticed.
 */
static void cmd_record(void) {
    printf("\nRecording (%d s)...\n", CAPTURE_SECONDS);

    pb_capture_result_t cap = { 0 };
    stats_acc_t acc = { 0 };

    /* Is there a clock? Read the first chunk BEFORE the header (point 2). */
    pb_audio_stream_flush();     /* ONCE; only stream_read from here on */
    uint32_t first = CAPTURE_SAMPLES < CHUNK_SAMPLES ? CAPTURE_SAMPLES : CHUNK_SAMPLES;
    pb_capture_result_t part = pb_audio_stream_read(s_chunk, first, 1000);
    if (part.samples == 0) {
        printf("Could not record (the ES8311 is not producing a clock).\n\n");
        return;
    }

    printf("#WAV-BEGIN rate=%d channels=1 bits=16 samples=%lu\n",
           PB_SAMPLE_RATE, (unsigned long)CAPTURE_SAMPLES);

    /* 32 samples per line, signed decimal — the format is unchanged, so the
     * PC side works as it is. */
    uint32_t emitted = 0;
    for (;;) {
        cap.samples      += part.samples;
        cap.fifo_overrun |= part.fifo_overrun;
        cap.timed_out    |= part.timed_out;
        stats_add(&acc, s_chunk, part.samples);

        for (uint32_t i = 0; i < part.samples; i++, emitted++) {
            printf("%d%c", s_chunk[i], ((emitted % 32) == 31) ? '\n' : ' ');
        }

        if (part.timed_out || cap.samples >= CAPTURE_SAMPLES) break;

        uint32_t want = CAPTURE_SAMPLES - cap.samples;
        if (want > CHUNK_SAMPLES) want = CHUNK_SAMPLES;
        part = pb_audio_stream_read(s_chunk, want, 1000);
    }
    if (emitted % 32) printf("\n");
    printf("#WAV-END\n");

    audio_stats_t st = stats_finish(&acc);
    print_stats("recording", &st, &cap);
    if (cap.samples != CAPTURE_SAMPLES) {
        printf("  [!] sent %lu / %lu samples - the PC side will report a SHORTFALL.\n",
               (unsigned long)cap.samples, (unsigned long)CAPTURE_SAMPLES);
    }
    if (cap.fifo_overrun) {
        printf("  [!] SAMPLES DROPPED: the transfer could not keep up with real\n");
        printf("      time, so there is a gap in the recording. Do NOT use this\n");
        printf("      WAV for measurement.\n");
    }
    printf("\n");
}


/* Does audio from the microphone actually scroll across the screen? The
 * display updates by writing a single column to QSPI (see
 * ui/spectrogram.c). */
static void cmd_spectrogram(void) {
    printf("\nLive spectrogram. Press any key to exit.\n");
    backlight_set(true);
    pb_spec_init();

    uint8_t bins[PB_SPEC_HEIGHT];
    while (getchar_timeout_us(0) < 0) {
        pb_capture_result_t cap = pb_audio_capture(s_chunk, PB_FFT_SIZE);
        if (cap.samples < PB_FFT_SIZE) break;
        /* A -75 dBFS floor: below the ~-36 dBFS of measured room noise, so
         * silence stays black while faint sounds are still visible. */
        pb_fft_spectrum(s_chunk, bins, PB_SPEC_HEIGHT, -75.0f);
        pb_spec_push_column(bins, PB_SPEC_HEIGHT);
    }
    printf("exited\n\n");
}

/* The bit-bang path is defined below; it is declared here so the display
 * test can offer it alongside the PIO path. */
static void bb_pins_setup(void);
static void bb_panel_init(void);
static void bb_fill_screen(uint16_t color);

#define PB_INIT_BITBANG 99   /* a virtual variant for cmd_display_test */

/** Drain pending serial input, so keys left over from the previous step do
 *  not make the wrong variant look like the winner. */
static void drain_stdin(void) {
    while (getchar_timeout_us(0) >= 0) { }
}

/**
 * Display test — which panel init sequence produces a correct image?
 *
 * The symptom: the screen lights up but shows permanent snow, every pixel a
 * different colour. That is the classic sign of data written to GRAM at the
 * wrong width. The likeliest cause is the pixel format (COLMOD, 0x3A) never
 * being set — we send two bytes per pixel while the panel, at its reset
 * default, expects something else.
 *
 * Since the screen cannot be seen over the serial console, the hypotheses are
 * all compiled into one firmware and tried in turn: when you see a flat
 * colour you press a key, and the device reports which variant it was on.
 */
static void cmd_display_test(void) {
    printf("\nDisplay test - panel init variants (interactive).\n");
    printf("WATCH THE SCREEN. Press a key when it fills with a FLAT COLOUR.\n");
    printf("If the snow continues, do nothing and it moves to the next variant.\n\n");
    backlight_set(true);

    const struct { int variant; const char *name; } attempts[] = {
        { LCD_3IN49_INIT_FULL,      "vendor table + SLPOUT/MADCTL/COLMOD(RGB565)/DISPON" },
        { LCD_3IN49_INIT_MINIMAL,   "DCS queue only (same as the reference driver)" },
        { LCD_3IN49_INIT_NO_COLMOD, "vendor table + SLPOUT/DISPON, NO COLMOD (control)" },
        { PB_INIT_BITBANG,          "BIT-BANG: PIO/DMA/vendor driver fully bypassed" },
    };

    const struct { const char *name; uint16_t color; } colors[] = {
        { "red",   0xF800 },
        { "green", 0x07E0 },
        { "blue",  0x001F },
        { "white", 0xFFFF },
    };

    for (size_t v = 0; v < sizeof(attempts) / sizeof(attempts[0]); v++) {
        printf("  [%u] %s\n", (unsigned)(v + 1), attempts[v].name);
        const bool bitbang = (attempts[v].variant == PB_INIT_BITBANG);
        if (bitbang) {
            pio_sm_set_enabled(qspi.pio, qspi.sm, false);
            bb_pins_setup();
            bb_panel_init();
        } else {
            LCD_3IN49_InitVariant(attempts[v].variant);
        }
        backlight_set(true);
        drain_stdin();

        for (size_t r = 0; r < sizeof(colors) / sizeof(colors[0]); r++) {
            printf("        %s\n", colors[r].name);
            if (bitbang) bb_fill_screen(colors[r].color);
            else         pb_lcd_fill(colors[r].color);

            /* 1.5 s to look at the screen after the colour is pushed */
            for (int t = 0; t < 15; t++) {
                if (getchar_timeout_us(0) >= 0) {
                    printf("\n  >>> THE VARIANT THAT WORKS: [%u] %s <<<\n",
                           (unsigned)(v + 1), attempts[v].name);
                    printf("  (on screen at the time: %s)\n\n", colors[r].name);
                    if (bitbang) {
                        printf("  The bit-bang path works and the PIO path does not:\n");
                        printf("  the fault is on the PIO/DMA side. Orientation square\n");
                        printf("  skipped.\n\n");
                        QSPI_PIO_Restore(qspi);   /* hand the pins/SM back to PIO */
                        return;
                    }
                    /* Orientation check: a 20x20 white square at the panel's
                     * (0,0). Where that square appears when the device is held
                     * in landscape confirms the mapping in
                     * ui/spectrogram.c. */
                    pb_lcd_fill(0x0000);
                    static uint16_t frame[20 * 20];
                    for (int i = 0; i < 20 * 20; i++) frame[i] = 0xFFFF;
                    pb_lcd_blit(0, 0, 20, 20, frame);
                    printf("  drew a 20x20 white square at panel (0,0) -\n");
                    printf("  hold the device with the USB socket pointing DOWN\n");
                    printf("  and note which corner the square is in.\n\n");
                    return;
                }
                sleep_ms(100);
            }
        }
        /* The bit-bang variant takes the pins to SIO and disables the SM. If
         * we do not hand them back, EVERY subsequent display test looks
         * spuriously "broken" — this trap cost a whole debugging session. */
        if (bitbang) QSPI_PIO_Restore(qspi);
        printf("\n");
    }

    printf("  No key was pressed on any variant - none of the sequences gave\n");
    printf("  a correct image. The problem is not the init sequence, it is the\n");
    printf("  data path.\n\n");
}

/* ── Bit-bang QSPI — for taking PIO out of the equation ───────────────────
 * The bus is driven slowly, directly by the CPU. The point is to strike the
 * PIO program off the suspect list: if bit-bang reaches the panel and PIO
 * does not, the fault is in PIO; if neither reaches it, the fault is in the
 * wiring, the pins or the panel.
 * Bit-bang can also READ, which lets us ask the panel for its identity — the
 * only direct answer to "does the panel hear us at all". */
#define BB_DELAY() sleep_us(1)      /* ~500 kHz — far slower than the panel needs */

static void bb_pins_setup(void) {
    const uint outputs[] = { PIN_CS, PIN_SCLK, PIN_DIO0 };
    for (size_t i = 0; i < 3; i++) {
        gpio_set_function(outputs[i], GPIO_FUNC_SIO);
        gpio_set_dir(outputs[i], GPIO_OUT);
    }
    /* D1..D3 are unused in the single-lane phase; left as inputs so they do
     * not clash if the panel drives them */
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) {
        gpio_set_function(p, GPIO_FUNC_SIO);
        gpio_set_dir(p, GPIO_IN);
    }
    gpio_put(PIN_CS, 1);
    gpio_put(PIN_SCLK, 0);
}

static void bb_byte(uint8_t v) {
    for (int i = 7; i >= 0; i--) {
        gpio_put(PIN_SCLK, 0);
        gpio_put(PIN_DIO0, (v >> i) & 1);
        BB_DELAY();
        gpio_put(PIN_SCLK, 1);          /* the panel samples on the rising edge */
        BB_DELAY();
    }
    gpio_put(PIN_SCLK, 0);
}

static void bb_cmd(uint8_t cmd, const uint8_t *data, size_t n) {
    gpio_put(PIN_CS, 0);
    BB_DELAY();
    bb_byte(0x02); bb_byte(0x00); bb_byte(cmd); bb_byte(0x00);
    for (size_t i = 0; i < n; i++) bb_byte(data[i]);
    BB_DELAY();
    gpio_put(PIN_CS, 1);
    BB_DELAY();
}

static void bb_read(uint8_t cmd, uint8_t *output, size_t n) {
    gpio_put(PIN_CS, 0);
    BB_DELAY();
    bb_byte(0x03); bb_byte(0x00); bb_byte(cmd); bb_byte(0x00);

    gpio_set_dir(PIN_DIO0, GPIO_IN);
    for (size_t i = 0; i < n; i++) {
        uint8_t v = 0;
        for (int b = 7; b >= 0; b--) {
            gpio_put(PIN_SCLK, 0); BB_DELAY();
            gpio_put(PIN_SCLK, 1); BB_DELAY();
            v |= (uint8_t)(gpio_get(PIN_DIO0) << b);
        }
        output[i] = v;
    }
    gpio_put(PIN_SCLK, 0);
    gpio_set_dir(PIN_DIO0, GPIO_OUT);
    BB_DELAY();
    gpio_put(PIN_CS, 1);
}

/** One byte across all four lanes (the real QSPI data phase). */
static void bb_byte_quad(uint8_t v) {
    for (int half = 0; half < 2; half++) {
        uint8_t nib = half ? (uint8_t)(v & 0x0F) : (uint8_t)(v >> 4);
        gpio_put(PIN_SCLK, 0);
        gpio_put(PIN_DIO0,  nib       & 1);
        gpio_put(PIN_DIO1, (nib >> 1) & 1);
        gpio_put(PIN_DIO2, (nib >> 2) & 1);
        gpio_put(PIN_DIO3, (nib >> 3) & 1);
        gpio_put(PIN_SCLK, 1);
    }
    gpio_put(PIN_SCLK, 0);
}

/**
 * Fill the whole screen with one colour by bit-bang.
 *
 * An independent path that bypasses PIO, DMA and the vendor driver entirely.
 * It is slow, but every step of it is visible here. If this works while the
 * PIO path does not, the fault is on the PIO side; if neither works, the
 * fault is further down.
 */
static void bb_fill_screen(uint16_t color) {
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_OUT);

    uint8_t caset[] = { 0x00, 0x00, (PB_PANEL_W - 1) >> 8, (PB_PANEL_W - 1) & 0xFF };
    uint8_t raset[] = { 0x00, 0x00, (PB_PANEL_H - 1) >> 8, (PB_PANEL_H - 1) & 0xFF };
    bb_cmd(0x2A, caset, 4);
    bb_cmd(0x2B, raset, 4);

    /* Pixel write: command+address on one lane (0x32 / 0x002C00), data on
     * all four */
    gpio_put(PIN_CS, 0);
    bb_byte(0x32); bb_byte(0x00); bb_byte(0x2C); bb_byte(0x00);
    uint8_t high = (uint8_t)(color >> 8), low = (uint8_t)(color & 0xFF);
    for (uint32_t i = 0; i < (uint32_t)PB_PANEL_W * PB_PANEL_H; i++) {
        bb_byte_quad(high);
        bb_byte_quad(low);
    }
    gpio_put(PIN_CS, 1);

    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_IN);
}

/** The minimal init proven by the reference driver — all of it bit-banged. */
static void bb_panel_init(void) {
    uint8_t p0 = 0x00, p55 = 0x55;
    bb_cmd(0x11, NULL, 0);  sleep_ms(120);   /* SLPOUT */
    bb_cmd(0x36, &p0,  1);                   /* MADCTL */
    bb_cmd(0x3A, &p55, 1);                   /* COLMOD RGB565 */
    bb_cmd(0x29, NULL, 0);  sleep_ms(120);   /* DISPON */
}

/** Count transitions on the TE line over 200 ms — is the panel scanning,
 *  and did it obey the command? */
static uint32_t te_transition_count(void) {
    uint32_t s = 0;
    int previous = gpio_get(PB_PIN_LCD_TE);
    absolute_time_t end = make_timeout_time_ms(200);
    while (!time_reached(end)) {
        int now = gpio_get(PB_PIN_LCD_TE);
        if (now != previous) { s++; previous = now; }
    }
    return s;
}

/**
 * Run the mel + gate pipeline on the LIVE microphone.
 *
 * The DSP's correctness is proven by the host tests, and by
 * `tools/mel_reference.py` matching an independent Python reference with zero
 * deviation across all 64 bands. What is tested here is something else: does
 * the pipeline keep up in real time, with a real microphone, on the board —
 * and does the gate behave sensibly in a real room?
 *
 * The output is entirely numeric; there is no need to look at the screen.
 */
static void cmd_mel_pipeline(void) {
    printf("\nmel + gate pipeline (live microphone)\n");
    printf("Press any key to exit.\n\n");

    pb_mel_init();
    pb_mel_reset();
    pb_gate_reset();

    /* An overlapping frame, for the correct hop: each turn takes PB_MEL_HOP
     * new samples and shifts the frame left. Reading without overlap would
     * break the 16 ms step, and three seconds would not come to 187 frames. */
    static int16_t frame[PB_FFT_SIZE];
    const uint32_t remaining = PB_FFT_SIZE - PB_MEL_HOP;

    uint32_t total = 0, open = 0, window_count = 0;
    uint32_t lost_frame = 0, fps = 0;
    absolute_time_t next_report = make_timeout_time_ms(1000);

    /* Start from now rather than from stale data; there is NO flush INSIDE
     * the loop — an uninterrupted pipeline is the entire point of continuous
     * capture. */
    pb_audio_stream_flush();

    while (getchar_timeout_us(0) < 0) {
        memmove(frame, frame + PB_MEL_HOP, remaining * sizeof(int16_t));
        pb_capture_result_t cap = pb_audio_stream_read(frame + remaining, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) { printf("  capture came up short, exiting\n"); break; }
        if (cap.fifo_overrun) lost_frame++;   /* the ring wrapped: continuity broke */

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(frame, power);
        pb_gate_result_t g = pb_gate_update(power);
        if (g.active) open++;

        pb_mel_push(frame);
        total++;
        fps++;

        if (pb_mel_frame_count() >= PB_MEL_FRAMES &&
            pb_mel_frame_count() % PB_MEL_FRAMES == 0) {
            static int8_t window[PB_MEL_BANDS * PB_MEL_FRAMES];
            if (pb_mel_window(window)) window_count++;
        }

        if (time_reached(next_report)) {
            /* The fps acceptance criterion is 62.5 (hop 384 @ 24 kHz). It
             * used to be 57 — the old blocking capture was dropping
             * frames. */
            printf("  frame %5lu (%lu/s)  gate %%%3lu  band %6.1f dB  "
                   "floor %6.1f dB  flux %.3f  windows %lu  lost %lu\n",
                   (unsigned long)total, (unsigned long)fps,
                   (unsigned long)(total ? open * 100 / total : 0),
                   (double)g.band_db, (double)g.floor_db, (double)g.flux,
                   (unsigned long)window_count, (unsigned long)lost_frame);
            fps = 0;
            next_report = make_timeout_time_ms(1000);
        }
    }

    printf("\n  total frames %lu, gate open %lu (%%%lu), full windows %lu, "
           "lost %lu\n\n",
           (unsigned long)total, (unsigned long)open,
           (unsigned long)(total ? open * 100 / total : 0),
           (unsigned long)window_count, (unsigned long)lost_frame);
}

/**
 * DEMO — everything built so far, on one screen at once.
 *
 *   Left strip    An LVGL status card: title, gate state, live counters.
 *                 Only the dirty area is redrawn, so it never touches the
 *                 right strip.
 *   Right strip   The live MEL spectrogram. What scrolls across the screen is
 *                 not an FFT but the 64 bands the model will see. It is drawn
 *                 by direct blit — coexisting with LVGL was the last open
 *                 question of the UI work, and this demo closes it.
 *   Audio         From the continuous capture ring: a 62.5 fps overlapping
 *                 hop, with no dropped frames.
 *   Gate          The status text turns green when sound is detected, and the
 *                 3 s window counter shows the model's input window filling
 *                 up.
 *
 * A numeric summary is printed on exit, so the fps measurement can be
 * verified without looking at the screen (the acceptance criterion is 62/s;
 * the old blocking capture stalled at 57).
 */
static void cmd_full_demo(void) {
    printf("\nDEMO: LVGL card + live mel spectrogram + gate.\n");
    printf("Hold the device in landscape with the USB socket on the RIGHT.\n");
    printf("Press any key to exit.\n\n");
    backlight_set(true);

    /* Clearing the screen is not merely cosmetic: `pb_lcd_fill` puts the
     * skip strip (lcd_blit.c) into a known state. While the strip is invalid,
     * positioning writes black into columns 0 and 1 and leaves a trail —
     * which is exactly what happens after the diagnostic commands. */
    pb_lcd_fill(0x0000);
    pb_lv_flush_counters_reset();

    bool touch = pb_lv_init();
    printf("  touch: %s\n", touch ? "ready" : "absent (does not block the demo)");

    /* ── Left card ── */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PokeBird");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0C000), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *state = lv_label_create(scr);
    lv_label_set_text(state, "listening...");
    lv_obj_set_style_text_color(state, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, 12, 44);

    lv_obj_t *numbers = lv_label_create(scr);
    lv_label_set_text(numbers, "");
    lv_obj_set_style_text_color(numbers, lv_color_hex(0x8090A0), LV_PART_MAIN);
    lv_obj_align(numbers, LV_ALIGN_TOP_LEFT, 12, 76);

    /* Do NOT let LVGL draw the spectrogram's slices: the UI is full width
     * (640) and LVGL owns all five slices by default, so without this the
     * card and the strip would write to the same region and erase each other
     * (lv_port.c). */
    pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_LISTEN);

    /* Draw the card FIRST, then hand the right strip to the spectrogram:
     * LVGL's first draw paints all of its own slices. */
    for (int i = 0; i < 4; i++) { pb_lv_tick(); sleep_ms(5); }
    pb_spec_init();

    /* ── Audio pipeline ── */
    pb_mel_init();
    pb_mel_reset();
    pb_gate_reset();

    static int16_t frame[PB_FFT_SIZE];
    memset(frame, 0, sizeof(frame));
    const uint32_t remaining = PB_FFT_SIZE - PB_MEL_HOP;

    uint32_t total = 0, open = 0, window_count = 0, lost = 0;
    uint32_t fps = 0, last_rate = 0;
    /* Rather than fading the text the moment the gate closes, hold it
     * briefly: at 62 fps a single-frame opening is invisible. */
    uint32_t gate_hold = 0;
    bool gate_visible = false;
    absolute_time_t next_report = make_timeout_time_ms(1000);
    absolute_time_t next_card  = make_timeout_time_ms(250);

    pb_audio_stream_flush();
    drain_stdin();

    while (getchar_timeout_us(0) < 0) {
        memmove(frame, frame + PB_MEL_HOP, remaining * sizeof(int16_t));
        pb_capture_result_t cap = pb_audio_stream_read(frame + remaining, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) { printf("  capture came up short, exiting\n"); break; }
        if (cap.fifo_overrun) lost++;

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(frame, power);
        pb_gate_result_t g = pb_gate_update(power);
        if (g.active) { open++; gate_hold = 31; }   /* stay visible ~0.5 s */
        else if (gate_hold) gate_hold--;

        pb_mel_push(frame);
        total++;
        fps++;

        if (pb_mel_frame_count() >= PB_MEL_FRAMES &&
            pb_mel_frame_count() % PB_MEL_FRAMES == 0) {
            static int8_t window[PB_MEL_BANDS * PB_MEL_FRAMES];
            if (pb_mel_window(window)) window_count++;
        }

        /* Turn the mel frame into a spectrogram column. The display window
         * is -75..-15 dB, so the room floor (about -47 dB) lands dark and
         * birdsong lands bright. */
        int8_t mel_q[PB_MEL_BANDS];
        if (pb_mel_last_frame(mel_q)) {
            uint8_t bins[PB_MEL_BANDS];
            for (int b = 0; b < PB_MEL_BANDS; b++) {
                float db = pb_mel_q_to_db(mel_q[b]);
                float v = (db + 75.0f) * (255.0f / 60.0f);
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                bins[b] = (uint8_t)v;
            }
            pb_spec_push_column(bins, PB_MEL_BANDS);
        }

        if (time_reached(next_report)) {
            last_rate = fps;
            fps = 0;
            next_report = make_timeout_time_ms(1000);
        }

        /* Update the card at 4 Hz: updating every frame would give LVGL
         * pointless drawing to do and eat into the hop budget. */
        if (time_reached(next_card)) {
            bool show = g.active || gate_hold > 0;
            if (show != gate_visible) {
                gate_visible = show;
                lv_label_set_text(state, show ? "SOUND DETECTED" : "listening...");
                lv_obj_set_style_text_color(state,
                    lv_color_hex(show ? 0x40E060 : 0xB0B8C0), LV_PART_MAIN);
            }
            lv_label_set_text_fmt(numbers,
                "%lu fps\ngate %%%lu\nband %d dB\nwindows %lu\nlost %lu",
                (unsigned long)last_rate,
                (unsigned long)(total ? open * 100 / total : 0),
                (int)g.band_db,
                (unsigned long)window_count,
                (unsigned long)lost);
            next_card = make_timeout_time_ms(250);
        }
        pb_lv_tick();
    }

    printf("\n  total frames %lu, last rate %lu fps, gate open %%%lu,\n"
           "  full windows %lu, lost %lu\n",
           (unsigned long)total, (unsigned long)last_rate,
           (unsigned long)(total ? open * 100 / total : 0),
           (unsigned long)window_count, (unsigned long)lost);

    /* LVGL flush alignment — a measurement that needs no eyes.
     * The panel rounds a column range to 2 pixels, so with an ODD column
     * count it uses one pixel MORE per row than we send, and the data shifts
     * on every row — the signature of horizontally smeared text. */
    printf("  LVGL flush %lu, UNALIGNED %lu, row stride != area_w: %lu\n"
           "  panel_w %lu..%lu, last area ui x(%ld..%ld) y(%ld..%ld), "
           "stride %ld px / area_w %ld px\n\n",
           (unsigned long)pb_lv_flush_count, (unsigned long)pb_lv_flush_unaligned,
           (unsigned long)pb_lv_flush_stride_differs,
           (unsigned long)pb_lv_flush_w_min, (unsigned long)pb_lv_flush_w_max,
           (long)pb_lv_last_x1, (long)pb_lv_last_x2,
           (long)pb_lv_last_y1, (long)pb_lv_last_y2,
           (long)pb_lv_last_stride_px, (long)pb_lv_last_area_w);
}

/**
 * LVGL demo — the UI acceptance test.
 *
 * It exercises four things on one screen: LVGL coming up, the 90-degree
 * rotation working correctly with partial rendering, the fonts and theme, and
 * touch input. The touched point is printed on screen, so whether the
 * coordinate mapping is right can be seen directly.
 */
static void cmd_ui_demo(void) {
    printf("\nLVGL demo. Press any key to exit.\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);   /* puts the skip strip into a known state (lcd_blit.c) */

    bool touch = pb_lv_init();
    printf("  touch: %s\n", touch ? "ready" : "ABSENT (display only)");

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PokeBird");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0C000), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text(info, "touch the screen");
    lv_obj_set_style_text_color(info, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 12, 44);

    /* A bar at the right edge: it shows that partial rendering lands
     * correctly at the far end of the screen too, which is where an
     * orientation error shows up most clearly. */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 200, 16);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_bar_set_range(bar, 0, 100);

    int value = 0;
    drain_stdin();
    while (getchar_timeout_us(0) < 0) {
        pb_lv_tick();

        pb_touch_state_t st = pb_touch_read();
        if (st.ok && st.fingers > 0) {
            lv_label_set_text_fmt(info, "touch: x=%u  y=%u",
                                  st.p.raw_x, st.p.raw_y);
        }

        value = (value + 1) % 101;
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
        sleep_ms(20);
    }
    printf("exited\n\n");
}

/**
 * Orientation test — where do the panel's native coordinates land
 * physically?
 *
 * The panel is 172x640 portrait and the UI is 640x172 landscape.
 * `ui/spectrogram.c` performs that conversion, but which corner is (0,0), and
 * whether there is any mirroring, can only be known by looking at the screen.
 *
 * Four different colours are pushed to the four corners. A single glance
 * settles both the rotation and the mirroring unambiguously — no key press
 * needed.
 */
static void cmd_orientation(void) {
    enum { FRAME = 40 };
    static uint16_t row[PB_PANEL_W];

    const struct { uint32_t x, y; uint16_t color; const char *name; } corner[] = {
        { 0,               0,               0xF800, "RED    = panel (0,0)"          },
        { PB_PANEL_W-FRAME, 0,               0x07E0, "GREEN  = panel (X end, 0)"     },
        { 0,               PB_PANEL_H-FRAME, 0x001F, "BLUE   = panel (0, Y end)"     },
        { PB_PANEL_W-FRAME, PB_PANEL_H-FRAME, 0xFFE0, "YELLOW = panel (X end, Y end)" },
    };

    printf("\nOrientation test - four coloured squares on screen.\n");
    backlight_set(true);

    /* A SINGLE PASS — this is the panel's contract.
     *
     * The old version filled the screen and then blitted the four squares
     * separately. On this panel every RAMWR returns to row 0, so all four
     * landed on top of each other and only the last was visible — the most
     * frequently seen symptom of the display bug. Now each of the 640 rows is
     * generated in place and sent in one RAMWR stream, and all four corners
     * land correctly. */
    pb_lcd_column_window(0, PB_PANEL_W - 1);
    pb_lcd_stream_begin(0x2C);
    for (uint32_t y = 0; y < PB_PANEL_H; y++) {
        for (uint32_t x = 0; x < PB_PANEL_W; x++) row[x] = 0x0000;
        for (size_t i = 0; i < sizeof(corner) / sizeof(corner[0]); i++) {
            if (y >= corner[i].y && y < corner[i].y + FRAME) {
                for (uint32_t x = corner[i].x; x < corner[i].x + FRAME; x++) {
                    row[x] = corner[i].color;
                }
            }
        }
        pb_lcd_stream_row(row, PB_PANEL_W);
    }
    pb_lcd_stream_end();
    pb_lcd_cursor_invalidate();

    for (size_t i = 0; i < sizeof(corner) / sizeof(corner[0]); i++) {
        printf("  %s\n", corner[i].name);
    }

    printf("\nHold the device with the USB socket pointing DOWN and note which\n");
    printf("corner each colour is in (top-left / top-right / bottom-left /\n");
    printf("bottom-right).\n\n");
}

/* ── The display bug: QSPI timing and hybrid-path diagnostics ─────────────
 *
 * The first hypothesis was that `QSPI_WaitIdle`'s 50 ms timeout is SILENT. If
 * it is timing out then the function is not waiting for anything, the old
 * bug has returned (CS rises before the data reaches the bus), and the fix
 * only ever existed on paper. The `w` command below measures that — no eyes
 * required. */

static void qspi_counter_print(void) {
    printf("     WaitIdle calls           : %lu\n",
           (unsigned long)pb_qspi_wait_calls);
    printf("     TIMEOUTS (silent failure): %lu%s\n",
           (unsigned long)pb_qspi_wait_timeout,
           pb_qspi_wait_timeout ? "   <<< HYPOTHESIS CONFIRMED: not waiting"
                                : "   (0 = it did wait)");
    printf("     SM off on entry          : %lu%s\n",
           (unsigned long)pb_qspi_wait_sm_off,
           pb_qspi_wait_sm_off ? "   <<< SM IS OFF" : "");
    printf("     FIFO full on entry       : %lu   (peak level %lu/4)\n",
           (unsigned long)pb_qspi_wait_fifo_full,
           (unsigned long)pb_qspi_wait_fifo_max);
    printf("     actually waited          : %lu   (max %lu spins, longest %lu us)\n",
           (unsigned long)pb_qspi_wait_waited,
           (unsigned long)pb_qspi_wait_spin_max,
           (unsigned long)pb_qspi_wait_us_max);
    printf("     FIFO still full on exit  : %lu%s\n",
           (unsigned long)pb_qspi_wait_residue,
           pb_qspi_wait_residue ? "   <<< THE WAIT DID NOT HELP"
                                : "   (0 = the FIFO drained)");
    printf("     total wait               : %lu us\n\n",
           (unsigned long)pb_qspi_wait_us_total);
}

/**
 * QSPI timing diagnostic — is `QSPI_WaitIdle` actually waiting?
 *
 * NO EYES REQUIRED. The device prints its own answer. Four measurements:
 *
 *   1) Window commands only — the CPU writes the FIFO, no DMA.
 *   2) A single-row blit — the pixel path, with DMA.
 *   3) A full-screen fill plus elapsed time, compared against PIO's
 *      theoretical floor. If every WaitIdle were timing out, 640 rows x 4
 *      operations x 50 ms would take about two minutes, so the duration alone
 *      is evidence.
 *   4) Is the bus still moving AFTER CS rises — a DIRECT observation of the
 *      old bug's signature. The PIO clock is slowed enough for the CPU to
 *      sample any residual bytes; if SCLK transitions after `QSPI_Deselect`
 *      returns, data is reaching the bus while CS is high.
 *
 * This command does NOT leave the PIO state machine disabled, so running a
 * display test afterwards is safe (unlike `d`'s bit-bang variant).
 */
static void cmd_qspi_timing(void) {
    printf("\nQSPI timing diagnostic - is WaitIdle really waiting? (NO EYES NEEDED)\n");
    printf("=====================================================================\n\n");

    printf("0) PIO state: sm%u %s, PC %u, sys clk %lu Hz\n\n",
           (unsigned)qspi.sm,
           ((qspi.pio->ctrl >> qspi.sm) & 1u) ? "ENABLED" : "DISABLED (!)",
           (unsigned)pio_sm_get_pc(qspi.pio, qspi.sm),
           (unsigned long)clock_get_hz(clk_sys));

    printf("1) Window commands - SetWindows, 3 CS transactions, no DMA\n");
    pb_qspi_counters_reset();
    LCD_3IN49_SetWindows(0, 0, PB_PANEL_W, 1);
    qspi_counter_print();

    printf("2) Single-row blit - window + DMA pixels, 4 CS transactions\n");
    {
        static uint16_t row[PB_PANEL_W];
        for (uint32_t i = 0; i < PB_PANEL_W; i++) row[i] = 0x0000;
        pb_qspi_counters_reset();
        pb_lcd_blit(0, 0, PB_PANEL_W, 1, row);
        qspi_counter_print();
    }

    printf("3) Full-screen fill - 640 rows\n");
    {
        pb_qspi_counters_reset();
        absolute_time_t t0 = get_absolute_time();
        pb_lcd_fill(0x0000);
        int64_t elapsed = absolute_time_diff_us(t0, get_absolute_time());

        /* The PIO floor: being a single pass, this is pixel data only —
         * W*H*2 bytes at 4 PIO cycles per byte. (Before the fix there was a
         * separate window+RAMWR per row and the floor was 440 bytes/row.) */
        uint32_t pio_hz = (uint32_t)(clock_get_hz(clk_sys) / 2);
        uint64_t base_us = (uint64_t)PB_PANEL_W * PB_PANEL_H * 2ull
                            * 4ull * 1000000ull / pio_hz;

        qspi_counter_print();
        printf("     elapsed                  : %lu us\n", (unsigned long)elapsed);
        printf("     PIO floor (theoretical)  : %lu us\n", (unsigned long)base_us);
        printf("     if every call timed out  : ~%lu us\n\n",
               (unsigned long)((uint64_t)pb_qspi_wait_calls * 50000ull));
    }

    printf("4) Is the bus still moving AFTER CS rises? (the old bug's signature)\n");
    {
        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 1000.0f);   /* ~150 kHz PIO */
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

        pb_qspi_counters_reset();
        QSPI_Select(qspi);
        QSPI_REGISTER_Write(qspi, 0x2a);
        QSPI_DATA_Write(qspi, 0x00);
        QSPI_DATA_Write(qspi, 0x00);
        QSPI_DATA_Write(qspi, (PB_PANEL_W - 1) >> 8);
        QSPI_DATA_Write(qspi, (PB_PANEL_W - 1) & 0xff);
        QSPI_Deselect(qspi);

        /* Deselect has returned and CS is high. The bus MUST be quiet. */
        uint32_t transition = 0;
        int previous = gpio_get(PIN_SCLK);
        absolute_time_t end = make_timeout_time_ms(10);
        while (!time_reached(end)) {
            int now = gpio_get(PIN_SCLK);
            if (now != previous) { transition++; previous = now; }
        }

        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 2.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

        printf("     32 bytes sent at ~150 kHz (the transaction should take ~850 us)\n");
        qspi_counter_print();
        printf("     SCLK transitions, CS high: %lu%s\n\n", (unsigned long)transition,
               transition ? "   <<< DATA REACHES THE BUS WITH CS HIGH - the bug is back"
                     : "   (0 = the bus is quiet, CS timing is CORRECT)");

        /* Restore the full-screen window so the next draw lands correctly. */
        LCD_3IN49_SetWindows(0, 0, PB_PANEL_W, PB_PANEL_H);
        pb_lcd_cursor_invalidate();   /* the panel was written from outside (lcd_blit.h) */
    }
}

/* ── Hybrid path test: the window command and the pixel data by SEPARATE
 *    routes ──────────────────────────────────────────────────────────────
 *
 * The measured fact: bit-bang pushes flat colours correctly and the PIO/DMA
 * path does not. But `bb_fill_screen` sends BOTH the window AND the pixels by
 * bit-bang, so it does not separate which one is failing.
 *
 * The symptom ("the screen does not clear, only the LAST square drawn is
 * visible") matches the window commands being dropped exactly: if the window
 * never changes, every RAMWR returns the write cursor to the same place and
 * each draw lands on top of the last. This test splits that hypothesis in
 * two. */

static void bb_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint8_t caset[] = { (uint8_t)(x >> 8), (uint8_t)x,
                        (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1) };
    uint8_t raset[] = { (uint8_t)(y >> 8), (uint8_t)y,
                        (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1) };
    bb_cmd(0x2A, caset, 4);
    bb_cmd(0x2B, raset, 4);
}

static void bb_pixel(uint16_t color, uint32_t pixel) {
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_OUT);
    gpio_put(PIN_CS, 0);
    bb_byte(0x32); bb_byte(0x00); bb_byte(0x2C); bb_byte(0x00);
    uint8_t high = (uint8_t)(color >> 8), low = (uint8_t)(color & 0xFF);
    for (uint32_t i = 0; i < pixel; i++) { bb_byte_quad(high); bb_byte_quad(low); }
    gpio_put(PIN_CS, 1);
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_IN);
}

static void hybrid_wait(void) {
    printf("      >>> WATCH THE SCREEN. Press a key to continue (auto after 10 s).\n");
    drain_stdin();
    for (int t = 0; t < 100; t++) {
        if (getchar_timeout_us(0) >= 0) return;
        sleep_ms(100);
    }
}

/* Return to PIO and set the clock.
 * NOTE: QSPI_PIO_Restore resets clkdiv to 2.0 via program_init, so the clock
 * must be set again AFTER every restore. */
static void hybrid_pio_version(float clkdiv) {
    QSPI_PIO_Restore(qspi);
    pio_sm_set_clkdiv(qspi.pio, qspi.sm, clkdiv);
    pio_sm_clkdiv_restart(qspi.pio, qspi.sm);
}

static void hybrid_phase(bool window_pio, bool pixel_pio, float clkdiv) {
    enum { SQ_X = 66, SQ_Y = 300, SQ_W = 40, SQ_H = 40 };

    /* 1) Fill the whole screen with dark blue */
    if (window_pio) { hybrid_pio_version(clkdiv); LCD_3IN49_SetWindows(0, 0, PB_PANEL_W, PB_PANEL_H); }
    else             { bb_pins_setup();       bb_window(0, 0, PB_PANEL_W, PB_PANEL_H); }

    if (pixel_pio)  { hybrid_pio_version(clkdiv); pb_lcd_stream_flat(0x001F, (uint32_t)PB_PANEL_W * PB_PANEL_H); }
    else             { bb_pins_setup();       bb_pixel(0x001F, (uint32_t)PB_PANEL_W * PB_PANEL_H); }

    /* 2) A 40x40 white square in the MIDDLE of the screen.
     *    If the window command is being dropped, the square appears not in
     *    the middle but as a full-width strip at the TOP — distinguishable at
     *    a glance. */
    if (window_pio) { hybrid_pio_version(clkdiv); LCD_3IN49_SetWindows(SQ_X, SQ_Y, SQ_X + SQ_W, SQ_Y + SQ_H); }
    else             { bb_pins_setup();       bb_window(SQ_X, SQ_Y, SQ_W, SQ_H); }

    if (pixel_pio)  { hybrid_pio_version(clkdiv); pb_lcd_stream_flat(0xFFFF, SQ_W * SQ_H); }
    else             { bb_pins_setup();       bb_pixel(0xFFFF, SQ_W * SQ_H); }

    hybrid_pio_version(2.0f);        /* leave it at the production clock */
}

/**
 * `y` — the hybrid path test. EYES REQUIRED, interactive.
 *
 * Several combinations, each drawing the same pattern: a dark blue screen
 * with a 40x40 white square in the MIDDLE. There is one question to answer:
 * is the square in the MIDDLE, or is it a wide strip at the TOP?
 */
static void cmd_hybrid_path(void) {
    printf("\nHybrid path test - window command and pixel data by separate routes\n");
    printf("==================================================================\n\n");
    printf("At each step the screen should be BLUE with a 40x40 WHITE SQUARE\n");
    printf("in the MIDDLE.\n");
    printf("Square in the MIDDLE   -> that combination WORKS.\n");
    printf("Wide STRIP at the TOP  -> the window command is not REACHING the panel.\n");
    printf("Screen never goes blue -> the pixel data is not reaching it.\n\n");

    backlight_set(true);

    const struct { bool window_pio, pixel_pio; float clkdiv; const char *name; } phase[] = {
        { false, false,  2.0f, "window BIT-BANG + pixels BIT-BANG  (control: known to WORK)" },
        { false, true,   2.0f, "window BIT-BANG + pixels PIO/DMA   (SCLK 37.5 MHz)" },
        { true,  false,  2.0f, "window PIO      + pixels BIT-BANG  (SCLK 37.5 MHz)" },
        { true,  true,   2.0f, "window PIO      + pixels PIO/DMA   (PRODUCTION PATH, 37.5 MHz)" },
        { true,  true,  20.0f, "window PIO      + pixels PIO/DMA   (same path, SCLK 3.75 MHz)" },
        { true,  true,  80.0f, "window PIO      + pixels PIO/DMA   (same path, SCLK 0.94 MHz)" },
    };

    for (size_t i = 0; i < sizeof(phase) / sizeof(phase[0]); i++) {
        printf("  [%u] %s\n", (unsigned)(i + 1), phase[i].name);
        hybrid_phase(phase[i].window_pio, phase[i].pixel_pio, phase[i].clkdiv);
        hybrid_wait();
        printf("\n");
    }

    printf("Report which steps had the square in the MIDDLE and which had a\n");
    printf("STRIP or nothing.\n");
    printf("  [2] works, [3] does not   -> the fault is in the WINDOW commands.\n");
    printf("  [3] works, [2] does not   -> the fault is in the PIXEL/DMA path.\n");
    printf("  [4] broken, [5]/[6] fine  -> the fault is CLOCK SPEED (37.5 MHz is\n");
    printf("                               too fast).\n");
    printf("  nothing but [1] works     -> the PIO path is broken throughout.\n\n");
}

/* ── Row addressing test ──────────────────────────────────────────────────
 *
 * `y` gave the same result in ALL six combinations (blue screen with a white
 * box at the edge). So the fault is NOT in the route (PIO or bit-bang) and
 * NOT in the clock speed — it is in the commands we send.
 *
 * The panel's two independent WORKING drivers (rsvpnano's ESP32 and RP2350-PIO
 * drivers) **never send the RASET (0x2B) command at all**: only the column
 * range is set, with CASET (0x2A), and the row is determined by RAMWR (0x2C,
 * start from the top of the column window) and RAMWRC (0x3C, continue where
 * you left off). Our driver was sending RASET and expecting the row to go
 * there.
 *
 * That explains EVERYTHING observed: a full-screen flat fill works (it starts
 * at row 0 anyway), but every partial draw lands on row 0 — `pb_lcd_fill`'s
 * 640 rows all pile onto the same top row (the screen "does not clear"), `o`'s
 * four squares land on top of each other (only the last is visible), and in
 * `a` every LVGL fragment accumulates at the top.
 *
 * `z` confirms this: the same square, drawn five different ways. */

static void qspi_reg_write(uint8_t reg, const uint8_t *data, size_t n) {
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, reg);
    for (size_t i = 0; i < n; i++) QSPI_DATA_Write(qspi, data[i]);
    QSPI_Deselect(qspi);
}

static void qspi_caset(uint16_t x1, uint16_t x2) {
    uint8_t d[] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    qspi_reg_write(0x2A, d, 4);
}

static void qspi_raset(uint16_t y1, uint16_t y2) {
    uint8_t d[] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    qspi_reg_write(0x2B, d, 4);
}

/** Fill the whole screen with one colour, using the panel's real contract
 *  (CASET + RAMWR). This path is measured to work — the screen was solid blue
 *  in all six steps of `y`. */
static void z_background(uint16_t color) {
    qspi_caset(0, PB_PANEL_W - 1);
    pb_lcd_stream_begin(0x2C);
    pb_lcd_stream_color(color, (uint32_t)PB_PANEL_W * PB_PANEL_H);
    pb_lcd_stream_end();
}

/**
 * `z` — the row addressing test. EYES REQUIRED, interactive.
 *
 * Five methods, all with the same target: a 40x40 white square at the panel's
 * (66,300), i.e. dead centre. One question: is the square in the MIDDLE or at
 * the EDGE?
 */
static void cmd_row_addr(void) {
    enum { SQ_X = 66, SQ_Y = 300, SQ_W = 40, SQ_H = 40 };
    const uint16_t CLR_BLUE = 0x001F, CLR_WHITE = 0xFFFF;

    printf("\nRow addressing test - does RASET (0x2B) work on this panel?\n");
    printf("===========================================================\n\n");
    printf("At each step the screen is BLUE with a 40x40 WHITE square on it.\n");
    printf("The square is meant to land dead centre on the panel.\n");
    printf("  in the MIDDLE   -> that method is CORRECT\n");
    printf("  at the EDGE     -> the row address is being ignored\n\n");

    backlight_set(true);

    for (int step = 1; step <= 5; step++) {
        switch (step) {
        case 1:
            printf("  [1] CURRENT PATH: CASET + RASET + bare 0x2C, then RAMWR (control)\n");
            z_background(CLR_BLUE);
            LCD_3IN49_SetWindows(SQ_X, SQ_Y, SQ_X + SQ_W, SQ_Y + SQ_H);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 2:
            printf("  [2] CASET + RASET, NO bare 0x2C\n");
            z_background(CLR_BLUE);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            qspi_raset(SQ_Y, SQ_Y + SQ_H - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 3:
            printf("  [3] Reversed order: RASET first, then CASET\n");
            z_background(CLR_BLUE);
            qspi_raset(SQ_Y, SQ_Y + SQ_H - 1);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 4:
            printf("  [4] REFERENCE PATH: CASET only; the row is counted from\n");
            printf("      RAMWR - skip %d rows of blue, then write white\n", SQ_Y);
            z_background(CLR_BLUE);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_BLUE,  (uint32_t)SQ_Y * SQ_W);   /* skip */
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 5:
            printf("  [5] REFERENCE + RAMWRC: the skip is a separate transaction and\n");
            printf("      the white continues with 0x3C (this tells us whether the\n");
            printf("      permanent fix is cheap)\n");
            z_background(CLR_BLUE);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_BLUE, (uint32_t)SQ_Y * SQ_W);
            pb_lcd_stream_end();
            pb_lcd_stream_begin(0x3C);                      /* RAMWRC — continue */
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;
        }
        hybrid_wait();
        printf("\n");
    }

    printf("Report which steps had the square in the MIDDLE.\n");
    printf("  [4] centre, [1][2][3] edge -> RASET is ignored. THAT IS THE ROOT CAUSE.\n");
    printf("  [5] centre too             -> RAMWRC works and the fix is cheap:\n");
    printf("                                the LVGL stream is one pass end to end.\n");
    printf("  [5] edge but [4] centre    -> no RAMWRC, so every draw pays the skip.\n\n");

    /* Leave the window restored to full screen. */
    qspi_caset(0, PB_PANEL_W - 1);
    pb_lcd_cursor_invalidate();
}

/**
 * `j` — the cursor positioning test. EYES REQUIRED, interactive.
 *
 * `z` proved that the row is determined only by RAMWR (return to the top) and
 * RAMWRC (continue where you left off). One question remains, and it decides
 * the ARCHITECTURE:
 *
 *   If the column window is NARROWED, can the row be advanced more cheaply,
 *   and is the row PRESERVED when the window is widened again?
 *
 * The row advances as pixels are written, at the width of the window. With a
 * window one pixel wide, advancing `y` rows costs `y` pixels instead of
 * 172*y. If the window can then be set to the real range and writing continued
 * with RAMWRC, we have **cheap positioning**:
 *
 *   IF IT WORKS   `pb_lcd_blit` stays general purpose, and the spectrogram and
 *                 LVGL can share a screen (the `a` demo) at a cost of one
 *                 pixel per row.
 *   IF IT DOES NOT  the panel only permits SINGLE-PASS drawing from top to
 *                 bottom, and the UI layer has to be rebuilt around that.
 *
 * The skip data is CLR_RED, so what it writes over is visible.
 */
static void cmd_cursor_seek(void) {
    enum { SQ_X = 66, SQ_Y = 300, SQ_W = 40, SQ_H = 40 };
    const uint16_t CLR_BLUE = 0x001F, CLR_WHITE = 0xFFFF, CLR_RED = 0xF800;

    /* The skip column's width, and the pixel count needed to advance SQ_Y
     * rows at that width. The first attempt used a 1-pixel window: the square
     * came out DOTTED and advanced only half the intended rows — the classic
     * signature of the panel ROUNDING the column range to 2 pixels
     * (CASET(66,66) is effectively 66..67, so 300 pixels advance 150 rows
     * rather than 300, and RAMWRC continues one pixel out of alignment). */
    const struct { uint32_t x1, x2; const char *name; } skip[] = {
        { SQ_X, SQ_X + 1, "2 pixels ALIGNED (x1 even, x2 odd) - main hypothesis" },
        { 0,  1,          "2 pixels ALIGNED but in a DIFFERENT column (x=0..1)" },
        { SQ_X, SQ_X,     "1 pixel (the case that came out dotted)"             },
    };

    printf("\nCursor positioning test - cheap skipping with a narrow window\n");
    printf("============================================================\n\n");
    printf("The target is the same each step: BLUE background + a 40x40 WHITE\n");
    printf("square in the MIDDLE. The skip data is RED, so you can see where it\n");
    printf("is written.\n\n");

    backlight_set(true);

    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        const uint32_t width = skip[i].x2 - skip[i].x1 + 1;
        z_background(CLR_BLUE);

        printf("  [%u] NARROW SKIP: %s\n", (unsigned)(i + 1), skip[i].name);
        printf("      window %lu..%lu, %lu red pixels (= %d rows),\n",
               (unsigned long)skip[i].x1, (unsigned long)skip[i].x2,
               (unsigned long)(SQ_Y * width), SQ_Y);
        printf("      then window %d..%d and white via RAMWRC\n", SQ_X, SQ_X + SQ_W - 1);

        qspi_caset(skip[i].x1, skip[i].x2);
        pb_lcd_stream_begin(0x2C);
        pb_lcd_stream_color(CLR_RED, SQ_Y * width);   /* row 0 -> SQ_Y */
        pb_lcd_stream_end();
        qspi_caset(SQ_X, SQ_X + SQ_W - 1);
        pb_lcd_stream_begin(0x3C);                 /* RAMWRC — is the row preserved? */
        pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
        pb_lcd_stream_end();

        hybrid_wait();
        printf("\n");
    }

    z_background(CLR_BLUE);
    printf("  [4] CONTROL (same as z[4], known to be correct):\n");
    printf("      window %d..%d, a %d-row wide red skip, then white\n",
           SQ_X, SQ_X + SQ_W - 1, SQ_Y);
    qspi_caset(SQ_X, SQ_X + SQ_W - 1);
    pb_lcd_stream_begin(0x2C);
    pb_lcd_stream_color(CLR_RED, (uint32_t)SQ_Y * SQ_W);
    pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
    pb_lcd_stream_end();
    hybrid_wait();

    printf("\nReport three things for each step:\n");
    printf("  a) is the white square in the MIDDLE, NEAR the middle, or at the EDGE?\n");
    printf("  b) is the square SOLID or DOTTED?\n");
    printf("  c) is the red a THIN line or a WIDE block?\n\n");
    printf("  [1] centre + SOLID -> the panel rounds columns to 2, so CHEAP\n");
    printf("      POSITIONING EXISTS: advancing y rows costs 2*y pixels.\n");
    printf("      pb_lcd_blit stays general purpose and the `a` demo is fixed.\n");
    printf("  [1] still dotted/shifted -> the narrow-window path is DEAD. The\n");
    printf("      panel only permits SINGLE-PASS drawing top to bottom, and the\n");
    printf("      UI layer has to be rebuilt around that.\n\n");

    qspi_caset(0, PB_PANEL_W - 1);
    pb_lcd_cursor_invalidate();
}

/**
 * `L` — text diagnostic. NO EYES REQUIRED.
 *
 * The text on screen looks corrupt, but every diagnostic came back clean:
 * WaitIdle waits, the CS timing is right, the column alignment holds
 * (UNALIGNED 0), and LVGL's row stride equals the area width (difference 0).
 * Two possibilities remain, and this command separates them:
 *
 *   - LVGL is already drawing the text corrupt (a font, cache or memory
 *     problem), or
 *   - the drawing is correct and the corruption is in our send path.
 *
 * It draws a single label and dumps the flush area to the serial console as
 * ASCII, **using the indexing the driver reads with**. If "PokeBird" is
 * legible in the terminal, then both LVGL and the transposed read are sound.
 */
static void cmd_text_dump(void) {
    printf("\nText diagnostic - what LVGL drew, dumped as ASCII (NO EYES NEEDED)\n");
    printf("=================================================================\n\n");

    backlight_set(true);
    pb_lcd_fill(0x0000);
    pb_lv_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "PokeBird");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 4, 4);

    /* Draw the whole screen first (with the dump off), then dirty only the
     * label. */
    for (int i = 0; i < 6; i++) { pb_lv_tick(); sleep_ms(10); }

    printf("The dump below is in UI orientation: each line is one ui y and each\n");
    printf("character one ui x. If the text is legible, LVGL and the transposed\n");
    printf("read are both SOUND.\n\n");
    pb_lv_request_dump(1);
    pb_lcd_request_row_dump(140);   /* s_row itself, as handed to DMA */
    lv_obj_invalidate(label);
    for (int i = 0; i < 6; i++) { pb_lv_tick(); sleep_ms(10); }

    printf("\nDone.\n\n");
}

/**
 * Does a multi-row write into a NARROW WINDOW slip? EYES REQUIRED.
 *
 * The entire software path has been measured and is clean: LVGL's drawing,
 * the 90-degree transposed read, the row stride, the column alignment, and
 * `s_row` itself as handed to DMA — all correct pixel for pixel (the `L`
 * command). Correct data goes out with an aligned window. And yet the text on
 * screen is corrupt.
 *
 * EVERY display test so far was BLIND to this bug: `o`'s corners, `z`/`j`'s
 * squares, `d`'s colours, `pb_lcd_fill` — all FLAT COLOUR. A flat block still
 * looks like a flat block even if it slips row by row. The spectrogram is
 * blind too: it writes ONE row per push, so the error cannot accumulate. The
 * only thing that breaks is LVGL's text, and it breaks in one place: **a
 * multi-row write into a narrow window.**
 *
 * This test exercises exactly that. The pattern is deliberately NOT a flat
 * colour: every row has a white dot at the same two pixels, so a correct
 * write gives two DEAD STRAIGHT lines and a slip makes them SLANT. The source
 * is fed with `row_step = 0`, so every panel row gets identical data — any
 * slant therefore comes from the panel.
 */
static void shift_band(uint32_t x1, uint32_t width, uint32_t n_pixel,
                        uint16_t color, uint32_t row) {
    static uint16_t pattern[PB_PANEL_W];
    for (uint32_t i = 0; i < PB_PANEL_W; i++) pattern[i] = 0x0000;
    pattern[8]  = color;
    pattern[24] = color;

    pb_lcd_column_window(x1, x1 + width - 1);
    pb_lcd_stream_begin(0x2C);
    for (uint32_t r = 0; r < row; r++) pb_lcd_stream_row(pattern, n_pixel);
    pb_lcd_stream_end();
    pb_lcd_cursor_invalidate();
}

/**
 * `S` — how many pixels is the panel's REAL row step in a narrow window?
 * EYES REQUIRED.
 *
 * MEASURED by eye: writing 32 pixels per row into a 32-column window makes
 * the lines slip DOWNWARDS like a staircase, while at full width (172) they
 * are straight. So the number of pixels the panel consumes per row is not
 * equal to the window width WE COMPUTE — and the difference is what corrupts
 * the text.
 *
 * This round does not guess, it MEASURES: four bands are drawn into the same
 * 32-column window at 31 / 32 / 33 / 34 pixels per row. Whichever band's
 * lines come out DEAD STRAIGHT gives the panel's real row step. The pattern
 * is deliberately not a flat colour; a flat block looks flat even when it
 * slips, which is exactly what blinded every earlier test.
 */
static void cmd_stripe_test(void) {
    enum { ROW = 200, WIDTH = 32 };

    const struct { uint32_t x1, n; uint16_t color; const char *name; } band[] = {
        {   8, 31, 0xF800, "RED    = 31 pixels per row" },
        {  40, 32, 0x07E0, "GREEN  = 32 pixels per row (the current assumption)" },
        {  72, 33, 0x001F, "BLUE   = 33 pixels per row" },
        { 104, 34, 0xFFE0, "YELLOW = 34 pixels per row" },
    };

    printf("\nNarrow-window row step measurement - which band is STRAIGHT?\n");
    printf("===========================================================\n\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);

    /* A reference at full width. This is measured to be straight, and it is
     * here to give a comparison for what "straight" looks like. */
    {
        static uint16_t pattern[PB_PANEL_W];
        for (uint32_t i = 0; i < PB_PANEL_W; i++) pattern[i] = 0x0000;
        pattern[148] = 0xFFFF;
        pattern[164] = 0xFFFF;
        pb_lcd_column_window(0, PB_PANEL_W - 1);
        pb_lcd_stream_begin(0x2C);
        for (uint32_t r = 0; r < ROW; r++) pb_lcd_stream_row(pattern, PB_PANEL_W);
        pb_lcd_stream_end();
        pb_lcd_cursor_invalidate();
    }

    for (size_t i = 0; i < sizeof(band) / sizeof(band[0]); i++) {
        shift_band(band[i].x1, WIDTH, band[i].n, band[i].color, ROW);
        printf("  %s\n", band[i].name);
    }

    printf("\nThere are FIVE pairs of horizontal lines on screen (device in\n");
    printf("landscape, USB on the RIGHT). All start at the LEFT edge and run\n");
    printf("towards the middle.\n");
    printf("  2 WHITE  - the full-width reference, should be STRAIGHT\n");
    printf("  red / green / blue / yellow - four candidate row steps\n\n");
    printf("REPORT: which COLOUR pair is as DEAD STRAIGHT as the white ones?\n");
    printf("(the others are expected to slip up or down like a staircase)\n\n");
    printf("The straight one gives the panel's REAL row step in a narrow window,\n");
    printf("and the driver is corrected accordingly - that is why the text looks\n");
    printf("corrupt.\n\n");
}

/**
 * Touch bring-up and coordinate mapping.
 *
 * Which axis and direction the raw values correspond to is not assumed but
 * MEASURED, exactly as the screen orientation was: the user is asked to touch
 * each of the four edges in turn and the device prints the raw value for
 * each. Four lines settle the mapping unambiguously.
 */
static void cmd_touch_probe(void) {
    printf("\nTouch diagnostic\n");
    printf("================\n\n");

    if (!pb_touch_init()) {
        printf("Address 0x%02x (I2C0, SDA=GPIO%d SCL=GPIO%d) does NOT respond.\n\n",
               PB_TP_I2C_ADDR, PB_PIN_TP_SDA, PB_PIN_TP_SCL);
        return;
    }
    printf("Address 0x%02x responds.\n\n", PB_TP_I2C_ADDR);

    /* ── First, the most basic question: is anything really on this bus? ──
     * An address scan. If only 0x3B responds, the chip really is there. If
     * MOST addresses respond, the bus is broken (wrong pin, SDA stuck) and
     * the 0xdb we read is just noise — chasing protocol variants would be
     * wasted effort. i2c1, where the ES8311 lives, is scanned for comparison:
     * that bus is a reference we know to be sound. */
    {
        printf("I2C address scan:\n");
        printf("  i2c0 (touch, GPIO%d/%d):", PB_PIN_TP_SDA, PB_PIN_TP_SCL);
        int tp_count = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            uint8_t d;
            if (i2c_read_timeout_us(PB_TP_I2C_INST, a, &d, 1, false, 2000) >= 0) {
                printf(" %02x", a);
                tp_count++;
            }
        }
        printf("   (%d found)\n", tp_count);

        printf("  i2c1 (codec, GPIO%d/%d):", PB_PIN_I2C_SDA, PB_PIN_I2C_SCL);
        int cd_count = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (pb_i2c_probe(a)) { printf(" %02x", a); cd_count++; }
        }
        printf("   (%d found)\n", cd_count);

        if (tp_count > 8) {
            printf("  -> FAR TOO MANY addresses respond on i2c0: the bus is\n");
            printf("     broken and the 0xdb is noise. Check pins/wiring first.\n\n");
        } else if (tp_count == 0) {
            printf("  -> NOTHING at all on i2c0.\n\n");
        } else {
            printf("\n");
        }
    }


    /* The INT line: does the chip DETECT the touch? That is a question
     * independent of the read protocol. If INT moves, the chip is working and
     * the problem is only in the reading. */
    gpio_init(PB_PIN_TP_INT);
    gpio_set_dir(PB_PIN_TP_INT, GPIO_IN);
    gpio_pull_up(PB_PIN_TP_INT);

    printf("Hold the device in LANDSCAPE with the USB socket on the RIGHT.\n\n");

    /* ── GUIDED CALIBRATION ───────────────────────────────────────────────
     *
     * WHY: touch works, but the screen change fires on the WRONG AXIS (the
     * user swipes VERTICALLY and the code counts a HORIZONTAL swipe). Which
     * byte pair is which physical axis, and which way the value increases,
     * CANNOT BE GUESSED; all four edges are measured one at a time.
     *
     * The MEDIAN is taken at each edge, not the mean: the chip occasionally
     * emits an off-panel value (~4000), which corrupts a mean but not a
     * median. */
    {
        static uint16_t ox[256], oy[256];
        const char *edge[4] = { "LEFT", "RIGHT", "BOTTOM", "TOP" };
        uint16_t mx[4], my[4];

        printf("CALIBRATION - the four edges are measured in turn.\n");
        printf("At each prompt, hold your finger on the MIDDLE of that edge.\n\n");

        for (int k = 0; k < 4; k++) {
            /* The Pico SDK's stdio does not buffer, and fflush is NOT used:
             * newlib's fflush drags in the __retarget_lock_* symbols, which
             * the SDK does not provide, and the link fails. */
            printf("  >> hold on the %s edge now", edge[k]);
            for (int g = 3; g > 0; g--) { printf(" %d", g); sleep_ms(700); }

            uint32_t n = 0;
            absolute_time_t end = make_timeout_time_ms(2000);
            while (!time_reached(end) && n < 256) {
                pb_touch_state_t st = pb_touch_read();
                if (st.ok && st.fingers > 0) { ox[n] = st.p.raw_x; oy[n] = st.p.raw_y; n++; }
                sleep_ms(8);
            }
            if (n == 0) { printf("  -> touch could NOT BE READ\n"); mx[k] = my[k] = 0xFFFF; continue; }

            /* Median: sort ascending and take the middle (n is small, a
             * simple insertion sort is enough). */
            for (uint32_t i = 1; i < n; i++) {
                uint16_t a = ox[i], b = oy[i];
                uint32_t j = i;
                while (j > 0 && ox[j - 1] > a) { ox[j] = ox[j - 1]; j--; }
                ox[j] = a;
                j = i;
                while (j > 0 && oy[j - 1] > b) { oy[j] = oy[j - 1]; j--; }
                oy[j] = b;
            }
            mx[k] = ox[n / 2];
            my[k] = oy[n / 2];
            printf("  -> median raw_x %4u  raw_y %4u   (%lu samples, min/max "
                   "x %u/%u  y %u/%u)\n",
                   mx[k], my[k], (unsigned long)n, ox[0], ox[n - 1], oy[0], oy[n - 1]);
        }

        printf("\n  RESULT:\n");
        printf("    LEFT->RIGHT   raw_x %u -> %u   (change %d)\n",
               mx[0], mx[1], (int)mx[1] - (int)mx[0]);
        printf("    LEFT->RIGHT   raw_y %u -> %u   (change %d)\n",
               my[0], my[1], (int)my[1] - (int)my[0]);
        printf("    BOTTOM->TOP   raw_x %u -> %u   (change %d)\n",
               mx[2], mx[3], (int)mx[3] - (int)mx[2]);
        printf("    BOTTOM->TOP   raw_y %u -> %u   (change %d)\n",
               my[2], my[3], (int)my[3] - (int)my[2]);
        printf("  Whichever raw axis changes most HORIZONTALLY is the UI's x,\n");
        printf("  and the sign of the change gives the direction.\n\n");
    }

    printf("Now a LIVE STREAM. Move around the edges; every change is printed.\n");
    printf("If the INT column drops to 0 on touch, the chip is seeing it.\n");
    printf("Press any key to exit.\n\n");
    printf("  INT  fingers   raw x   raw y   first 8 bytes\n");

    /* A LIVE STREAM rather than a step-by-step "touch this corner": the
     * device prints whatever it sees. The previous version walked corner by
     * corner and skipped some, so it was impossible to tell whether the fault
     * was in the touch controller or in our own state machine. The raw bytes
     * are printed too — that is the only way to see whether the finger count
     * really is in byte 1. */
    uint16_t previous_x = 0xFFFF, previous_y = 0xFFFF;
    uint8_t  previous_f = 0xFF;
    int      previous_int = -1;
    uint32_t no_response = 0;

    while (getchar_timeout_us(0) < 0) {
        int intp = gpio_get(PB_PIN_TP_INT);
        pb_touch_state_t st = pb_touch_read();
        if (!st.ok) {
            if (++no_response % 100 == 1) printf("  (I2C is not responding)\n");
            sleep_ms(20);
            continue;
        }

        if (intp != previous_int || st.fingers != previous_f ||
            st.p.raw_x != previous_x || st.p.raw_y != previous_y) {
            uint8_t raw[32];
            pb_touch_last_raw(raw);
            printf("  %3d   %4u    %5u   %5u   ",
                   intp, st.fingers, st.p.raw_x, st.p.raw_y);
            for (int i = 0; i < 8; i++) printf("%02x ", raw[i]);
            printf("\n");
            previous_int = intp;
            previous_f = st.fingers;
            previous_x = st.p.raw_x;
            previous_y = st.p.raw_y;
        }
        sleep_ms(20);
    }
    printf("\nexited\n\n");
}

/**
 * Data path diagnostic — which assumption about the QSPI bus does not hold?
 *
 * All three init sequences failed to produce an image, so the problem is not
 * the panel's register sequence but the bytes reaching the panel. This test
 * measures every assumption on the data path one at a time; for most of them
 * NO EYES are required, because the device prints the result itself.
 */
static void cmd_datapath_probe(void) {
    printf("\nData path diagnostic\n");
    printf("====================\n\n");

    /* ── 1. Is a narrow (8-bit) DMA write replicated across byte lanes? ───
     * Pixel data is written to the PIO TX FIFO with DMA_SIZE_8. The PIO
     * program shifts the OSR LEFT, so the significant byte must be in bits
     * 31:24. We rely on a single-byte write being replicated across all 32
     * bits. If it is not, the byte lands in bits 7:0 and every pixel reaching
     * the panel is 0 — the whole pixel path is silently dead.
     *
     * We write a single byte the same way to a harmless, readable IO register
     * and read it back. */
    {
        /* The target is an unused DMA channel's read_addr register: fully
         * 32-bit readable and writable, and harmless as long as it is not
         * triggered. (The first attempt used the watchdog scratch register,
         * which read back 0 — the target was not accepting the write at all
         * and the test was inconclusive.) */
        int target = dma_claim_unused_channel(true);
        int ch    = dma_claim_unused_channel(true);
        volatile uint32_t *reg = &dma_hw->ch[target].read_addr;
        static uint8_t src = 0xA5;

        /* (a) a single byte via DMA */
        *reg = 0;
        dma_channel_config cfg = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&cfg, false);
        channel_config_set_write_increment(&cfg, false);
        dma_channel_configure(ch, &cfg, (void *)reg, &src, 1, true);
        dma_channel_wait_for_finish_blocking(ch);
        uint32_t dma_result = *reg;

        /* (b) a single byte via the CPU, for comparison */
        *reg = 0;
        *(volatile uint8_t *)reg = 0xA5;
        uint32_t cpu_result = *reg;

        *reg = 0;
        dma_channel_unclaim(ch);
        dma_channel_unclaim(target);

        printf("1) Narrow (8-bit) IO write - 0xA5:\n");
        printf("   via DMA: 0x%08lx    via CPU: 0x%08lx\n",
               (unsigned long)dma_result, (unsigned long)cpu_result);
        if (dma_result == 0xA5A5A5A5u) {
            printf("   -> the byte is replicated across all lanes. PIO reads from\n");
            printf("      31:24, so the pixel path is SOUND in this respect.\n\n");
        } else if (dma_result == 0x000000A5u) {
            printf("   -> NOT REPLICATED. The byte lands in 7:0 while PIO shifts\n");
            printf("      left and reads 31:24: ALL PIXEL DATA GOES OUT AS ZERO.\n");
            printf("      Fix: DMA write address ((uint8_t*)&pio->txf[sm])+3.\n\n");
        } else {
            printf("   -> unexpected; evaluate by hand.\n\n");
        }
    }

    /* ── 2. Is the PIO state machine running and draining the FIFO? ────── */
    {
        printf("2) PIO state (pio0, sm%u):\n", (unsigned)qspi.sm);
        printf("   SM enabled: %s\n",
               ((qspi.pio->ctrl >> qspi.sm) & 1u) ? "YES" : "NO (no data will ever go out)");
        printf("   PC: %u\n", (unsigned)pio_sm_get_pc(qspi.pio, qspi.sm));

        pio_sm_clear_fifos(qspi.pio, qspi.sm);
        for (int i = 0; i < 8; i++) {
            if (!pio_sm_is_tx_fifo_full(qspi.pio, qspi.sm)) {
                pio_sm_put(qspi.pio, qspi.sm, 0x0Fu << 24);
            }
        }
        uint32_t lvl_before = pio_sm_get_tx_fifo_level(qspi.pio, qspi.sm);
        sleep_ms(2);
        uint32_t lvl_after = pio_sm_get_tx_fifo_level(qspi.pio, qspi.sm);
        printf("   TX FIFO: right after writing %lu, after 2 ms %lu\n",
               (unsigned long)lvl_before, (unsigned long)lvl_after);
        printf("   -> %s\n\n", (lvl_after == 0)
               ? "the FIFO drains, the SM is consuming data."
               : "the FIFO does NOT drain. The SM is stopped or the clock is dead.");
    }

    /* ── 3. Is PIO actually driving the pins? ─────────────────────────────
     * The clock is slowed far below working speed (a few kHz) and the pins
     * are sampled by the CPU. A transition count of 0 means PIO is not
     * driving that pin at all. */
    {
        printf("3) Is PIO driving the pins (sampled at a slow clock):\n");
        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 30000.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

        uint32_t transition_sclk = 0, transition_d0 = 0;
        int previous_s = gpio_get(PIN_SCLK), previous_d = gpio_get(PIN_DIO0);
        absolute_time_t end = make_timeout_time_ms(60);
        while (!time_reached(end)) {
            if (!pio_sm_is_tx_fifo_full(qspi.pio, qspi.sm)) {
                pio_sm_put(qspi.pio, qspi.sm, 0x0Fu << 24);  /* nibble 0 then F */
            }
            int s = gpio_get(PIN_SCLK);
            int d = gpio_get(PIN_DIO0);
            if (s != previous_s) { transition_sclk++; previous_s = s; }
            if (d != previous_d) { transition_d0++;  previous_d = d; }
        }
        printf("   SCLK(GPIO%d) transitions: %lu   D0(GPIO%d): %lu\n",
               PIN_SCLK, (unsigned long)transition_sclk,
               PIN_DIO0, (unsigned long)transition_d0);
        printf("   -> %s\n\n", (transition_sclk > 0 && transition_d0 > 0)
               ? "PIO is driving both pins."
               : "THE PINS DO NOT MOVE. Wrong pin, overridden function, or a dead SM.");

        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 2.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);
        pio_sm_clear_fifos(qspi.pio, qspi.sm);
    }

    /* ── 4. Are the pins electrically sound? ──────────────────────────────
     * The pins are briefly made plain GPIOs and the driven level is read
     * back. This test cannot catch a wrong pin NUMBER (an unconnected GPIO
     * also reads back what you wrote); it catches shorts and stuck pins. */
    {
        printf("4) Pin drive/readback (short-circuit test):\n");
        const struct { uint pin; const char *name; } pins[] = {
            { PIN_SCLK, "SCLK" }, { PIN_DIO0, "D0" }, { PIN_DIO1, "D1" },
            { PIN_DIO2, "D2" },   { PIN_DIO3, "D3" }, { PIN_CS,   "CS" },
            { PIN_RST,  "RST" },
        };
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            gpio_set_function(pins[i].pin, GPIO_FUNC_SIO);
            gpio_set_dir(pins[i].pin, GPIO_OUT);
            gpio_put(pins[i].pin, 1); sleep_us(100);
            int high = gpio_get(pins[i].pin);
            gpio_put(pins[i].pin, 0); sleep_us(100);
            int low = gpio_get(pins[i].pin);
            printf("   %-4s GPIO%-2d  1->%d  0->%d  %s\n",
                   pins[i].name, pins[i].pin, high, low,
                   (high == 1 && low == 0) ? "" : "<<< STUCK");
        }
        printf("\n");

        /* Is a stuck pin being driven from outside? We release the output
         * and pull it down, then up: if the same level is read both times,
         * something else is driving the pin. */
        printf("   When released (measured with the internal pulls):\n");
        const struct { uint pin; const char *name; } free[] = {
            { PIN_RST, "RST(34)" }, { PB_PIN_LCD_TE, "TE(35)" },
            { PB_PIN_LCD_BL, "BL(36)" }, { PB_PIN_BL_EN, "BL_EN(37)" },
        };
        for (size_t i = 0; i < sizeof(free) / sizeof(free[0]); i++) {
            gpio_set_function(free[i].pin, GPIO_FUNC_SIO);
            gpio_set_dir(free[i].pin, GPIO_IN);
            gpio_pull_down(free[i].pin); sleep_ms(2);
            int pd = gpio_get(free[i].pin);
            gpio_pull_up(free[i].pin);   sleep_ms(2);
            int pu = gpio_get(free[i].pin);
            gpio_disable_pulls(free[i].pin);
            const char *note = (pd == 0 && pu == 1) ? "free (normal)"
                             : (pd == 1 && pu == 1) ? "DRIVEN HIGH FROM OUTSIDE"
                             : (pd == 0 && pu == 0) ? "DRIVEN LOW FROM OUTSIDE"
                                                    : "inconclusive";
            printf("   %-10s pull-down->%d  pull-up->%d   %s\n",
                   free[i].name, pd, pu, note);
        }
        printf("\n");
    }

    /* ── 5. Do the commands REACH the panel? — via the TE line, no eyes ───
     * The panel's TE (tearing effect) output is wired to GPIO35. The TEON
     * (0x35) command makes the panel pulse that line every frame, and TEOFF
     * (0x34) stops it. So by watching TE we can answer "did the command reach
     * the panel" by measurement rather than by looking at the screen. And if
     * there are pulses, the panel really is SCANNING — which also confirms
     * that the snow is the panel showing its own GRAM. */
    {
        printf("5) Command path test - listening to the panel's TE output (GPIO%d):\n",
               PB_PIN_LCD_TE);

        /* Hand the pins back to PIO and restart the panel */
        QSPI_GPIO_Init(qspi);
        for (uint p = PIN_SCLK; p <= PIN_DIO3; p++) pio_gpio_init(qspi.pio, p);
        LCD_3IN49_InitVariant(LCD_3IN49_INIT_FULL);

        gpio_set_function(PB_PIN_LCD_TE, GPIO_FUNC_SIO);
        gpio_set_dir(PB_PIN_LCD_TE, GPIO_IN);
        gpio_disable_pulls(PB_PIN_LCD_TE);

        /* Count TE transitions over 200 ms (about 24 expected at 60 Hz).
         * The test is two-way: TEOFF must stop the pulses and TEON must bring
         * them back. Looking only one way is misleading — the panel may pulse
         * by default anyway, as it did on the first measurement. */
        #define TE_COUNT() ({                                            \
            uint32_t _s = 0; int _o = gpio_get(PB_PIN_LCD_TE);         \
            absolute_time_t _b = make_timeout_time_ms(200);            \
            while (!time_reached(_b)) {                                \
                int _n = gpio_get(PB_PIN_LCD_TE);                      \
                if (_n != _o) { _s++; _o = _n; }                       \
            } _s; })

        /* Rule out clock speed too: if 37.5 MHz is too fast for commands, a
         * slow clock will work. We measure at both and compare. */
        const struct { float divider; const char *name; } rates[] = {
            { 2.0f,  "clkdiv 2  (~37.5 MHz)" },
            { 40.0f, "clkdiv 40 (~1.9 MHz)"  },
        };

        for (size_t h = 0; h < sizeof(rates) / sizeof(rates[0]); h++) {
            pio_sm_set_clkdiv(qspi.pio, qspi.sm, rates[h].divider);
            pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

            uint32_t base = TE_COUNT();

            LCD_3IN49_SendSimpleCmd(0x34);          /* TEOFF */
            sleep_ms(20);
            uint32_t off = TE_COUNT();

            QSPI_Select(qspi);                      /* TEON */
            QSPI_REGISTER_Write(qspi, 0x35);
            QSPI_DATA_Write(qspi, 0x00);
            QSPI_Deselect(qspi);
            sleep_ms(20);
            uint32_t open = TE_COUNT();

            printf("   %s\n", rates[h].name);
            printf("     baseline %lu  ->  TEOFF %lu  ->  TEON %lu\n",
                   (unsigned long)base, (unsigned long)off,
                   (unsigned long)open);
            if (base > 4 && off < 4 && open > 4) {
                printf("     -> COMMANDS ARE REACHING IT. The panel obeys.\n");
            } else if (base > 4) {
                printf("     -> the panel scans but does NOT obey commands.\n");
            } else {
                printf("     -> TE never pulses; the panel is not scanning.\n");
            }
        }
        printf("\n");
        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 2.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);
        #undef TE_COUNT
    }

    /* ── 6. Bit-bang: take PIO out of the equation, ask the panel its ID ──
     * PIO runs, the pins move, the CS timing was fixed — and the panel still
     * does not obey. Two possibilities remain: the waveform PIO produces is
     * wrong, or the problem is in the bus or the panel itself. Bit-bang
     * separates them. And because it can read, we can ask the panel its
     * identity. */
    {
        printf("6) Bit-bang test (PIO disabled, ~500 kHz):\n");
        pio_sm_set_enabled(qspi.pio, qspi.sm, false);
        bb_pins_setup();

        gpio_set_function(PB_PIN_LCD_TE, GPIO_FUNC_SIO);
        gpio_set_dir(PB_PIN_LCD_TE, GPIO_IN);
        gpio_disable_pulls(PB_PIN_LCD_TE);

        uint32_t base = te_transition_count();
        bb_cmd(0x34, NULL, 0);                 /* TEOFF */
        sleep_ms(20);
        uint32_t off = te_transition_count();
        uint8_t param = 0x00;
        bb_cmd(0x35, &param, 1);               /* TEON */
        sleep_ms(20);
        uint32_t open = te_transition_count();

        printf("   TE: baseline %lu -> TEOFF %lu -> TEON %lu\n",
               (unsigned long)base, (unsigned long)off, (unsigned long)open);
        printf("   -> %s\n", (base > 4 && off < 4 && open > 4)
               ? "BIT-BANG WORKS. The fault is in the PIO waveform."
               : "bit-bang has no effect either. The problem is not in PIO.");

        /* Read from the panel: 0x04 = RDDID, 0x0A = power mode, 0x0C = pixel
         * format. If everything comes back 0x00, or everything 0xFF, the
         * panel is not answering at all (the bus is stuck low or high
         * respectively). */
        const struct { uint8_t reg; const char *name; } reads[] = {
            { 0x04, "RDDID  (manufacturer/version/id)" },
            { 0x0A, "RDDPM  (power mode)" },
            { 0x0C, "RDDCOLMOD (piksel bicimi)" },
        };
        int meaningful = 0;
        for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
            uint8_t buf[6] = {0};
            bb_read(reads[i].reg, buf, sizeof(buf));
            printf("   0x%02x %-28s:", reads[i].reg, reads[i].name);
            for (size_t j = 0; j < sizeof(buf); j++) printf(" %02x", buf[j]);
            printf("\n");
            for (size_t j = 0; j < sizeof(buf); j++) {
                if (buf[j] != 0x00 && buf[j] != 0xFF) meaningful = 1;
            }
        }
        printf("   -> %s\n\n", meaningful
               ? "THE PANEL RESPONDS. The data line works in both directions."
               : "NO RESPONSE AT ALL (always 00 or FF). The panel does not hear\n"
                 "      us on this bus: the pin map or the wiring is wrong.");

        /* Re-enable PIO — this also resets the SM (FIFO and shift counter
         * included), which the old manual restore left behind. */
        QSPI_GPIO_Init(qspi);
        QSPI_PIO_Restore(qspi);
    }

    /* ── 7. Fallback: the command path test by eye ────────────────────────
     * Kept in case the measurable tests come back inconclusive. */
    {
        printf("7) Command path test - WATCH THE SCREEN.\n");
        printf("   If you see ANY change in the snow (it going dark, colours\n");
        printf("   inverting, brightness shifting), PRESS A KEY.\n\n");

        backlight_set(true);
        drain_stdin();

        const struct { uint8_t cmd; const char *name; } steps[] = {
            { 0x28, "DISPOFF  (the screen should go dark)" },
            { 0x29, "DISPON   (it should come back)" },
            { 0x21, "INVON    (colours should invert)" },
            { 0x20, "INVOFF   (they should revert)" },
            { 0x28, "DISPOFF  (the screen should go dark)" },
            { 0x29, "DISPON   (it should come back)" },
        };
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            LCD_3IN49_SendSimpleCmd(steps[i].cmd);
            printf("   0x%02x %s\n", steps[i].cmd, steps[i].name);
            for (int t = 0; t < 20; t++) {
                if (getchar_timeout_us(0) >= 0) {
                    printf("\n   >>> THE COMMAND PATH WORKS - change after 0x%02x <<<\n",
                           steps[i].cmd);
                    printf("   Commands reach the panel; the problem is only in the\n");
                    printf("   pixel path.\n\n");
                    LCD_3IN49_SendSimpleCmd(0x29);
                    return;
                }
                sleep_ms(100);
            }
        }
        LCD_3IN49_SendSimpleCmd(0x29);
        printf("\n   No change was reported - NO command is reaching the panel.\n");
        printf("   The problem is in the QSPI bus itself (pins/CS/clock).\n\n");
    }
}

/**
 * Backlight diagnostic — which pin combination lights it?
 *
 * After a reset (before any of our code runs) the light is on, and it goes
 * out once our code runs. So one of the two pins is being driven wrongly:
 *   GPIO36 (LCD_BL)  — brightness, PWM
 *   GPIO37 (BL_EN)   — boost converter enable
 * Rather than assuming the polarity, or which one is really the enable, we
 * try each combination in turn and ask which one lit the backlight.
 *
 * GPIO36 is also tried as a plain GPIO rather than PWM: the PWM function may
 * have been overridden by QSPI_GPIO_Init.
 */
static void cmd_backlight_probe(void) {
    /* A self-reporting version: it steps through the states, and when you
     * see the light you press a key and the device prints the state it was
     * in. The previous version required counting the steps by eye, which was
     * easy to lose track of. */
    printf("\nBacklight diagnostic (interactive).\n");
    printf("WATCH THE SCREEN. Press a key the moment the light comes on.\n");
    printf("Each state lasts 5 seconds. If none light it, the test ends by itself.\n\n");

    gpio_init(PB_PIN_BL_EN);
    gpio_set_dir(PB_PIN_BL_EN, GPIO_OUT);

    const struct { int bl_en; int bl; int pwm_duty; const char *name; } state[] = {
        { 1, 0, -1, "BL_EN=1  LCD_BL=0 (plain GPIO)" },
        { 1, 1, -1, "BL_EN=1  LCD_BL=1 (plain GPIO)" },
        { 0, 0, -1, "BL_EN=0  LCD_BL=0 (plain GPIO)" },
        { 0, 1, -1, "BL_EN=0  LCD_BL=1 (plain GPIO)" },
        { 1, 0,  5, "BL_EN=1  PWM duty 5%%   (BRIGHT if active-low)" },
        { 1, 0, 95, "BL_EN=1  PWM duty 95%%  (BRIGHT if active-high)" },
    };

    for (size_t i = 0; i < sizeof(state) / sizeof(state[0]); i++) {
        gpio_put(PB_PIN_BL_EN, state[i].bl_en);
        if (state[i].pwm_duty < 0) {
            gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_SIO);
            gpio_set_dir(PB_PIN_LCD_BL, GPIO_OUT);
            gpio_put(PB_PIN_LCD_BL, state[i].bl);
        } else {
            gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_PWM);
            pwm_set_gpio_level(PB_PIN_LCD_BL,
                               (uint16_t)((BL_PWM_WRAP - 1) * state[i].pwm_duty / 100));
        }
        printf("  [%u] %s\n", (unsigned)(i + 1), state[i].name);

        for (int t = 0; t < 50; t++) {
            if (getchar_timeout_us(0) >= 0) {
                printf("\n  >>> THE STATE THAT LIT IT: [%u] %s <<<\n\n",
                       (unsigned)(i + 1), state[i].name);
                return;
            }
            sleep_ms(100);
        }
    }
    printf("  no key was pressed in any state\n\n");
}


/* ── x: the on-device validation set + arena + inference time ─────────────
 *
 * This command is the acceptance test for the inference work. It measures
 * three things at once, all of them MEASUREMENTS rather than estimates:
 *
 *   1. arena_used_bytes()  — the documented 141 KB was an estimate; this is
 *                            the real figure
 *   2. Invoke() duration   — it has to fit inside the 1 s window step
 *   3. logit comparison    — catches "works on the PC, not on the device"
 *
 * The audio path is deliberately OUT OF SCOPE: the input is a ready-made
 * window embedded from the training set. If there is a discrepancy there is
 * no need to look at mel; the fault is confined to TFLM, CMSIS-NN or the
 * quantisation. No microphone is needed either.
 */
static void cmd_ai_verify(void) {
    printf("\n=== SPECIES NET - on-device validation ===\n");

    const uint32_t t_init0 = time_us_32();
    if (!pb_species_net_init()) {
        printf("[!] the model could not be started.\n");
        return;
    }
    const uint32_t t_init = time_us_32() - t_init0;

    printf("startup       %lu us\n", (unsigned long)t_init);
    printf("arena         %u / %u bytes used  (%.1f%%)\n",
           (unsigned)pb_species_net_arena_used(),
           (unsigned)pb_species_net_arena_total(),
           100.0 * pb_species_net_arena_used() / pb_species_net_arena_total());
    printf("output quant. scale %.9f  zero %d\n",
           (double)pb_species_net_output_scale(), pb_species_net_output_zero());

    int8_t *input = pb_species_net_input();
    const int8_t *output = pb_species_net_output();

    uint32_t time_min = 0xFFFFFFFFu, time_max = 0, time_total = 0;
    int exact = 0, pred_matches = 0, max_diff = 0;
    long diff_total = 0;
    long diff_count = 0;

    for (int k = 0; k < PB_VALIDATION_COUNT; k++) {
        memcpy(input, pb_validation_input[k],
               (size_t)PB_VALIDATION_FRAMES * PB_VALIDATION_BANDS);
        if (!pb_species_net_run()) {
            printf("[!] window %d: Invoke failed\n", k);
            return;
        }
        const uint32_t us = pb_species_net_last_time_us();
        if (us < time_min) time_min = us;
        if (us > time_max) time_max = us;
        time_total += us;

        int diff_max = 0, best = 0;
        for (int c = 0; c < PB_VALIDATION_CLASSES; c++) {
            int d = (int)output[c] - (int)pb_validation_logit[k][c];
            if (d < 0) d = -d;
            if (d > diff_max) diff_max = d;
            diff_total += d;
            diff_count++;
            if (output[c] > output[best]) best = c;
        }
        if (diff_max == 0) exact++;
        if (diff_max > max_diff) max_diff = diff_max;
        if (best == pb_validation_pc_pred[k]) pred_matches++;

        printf("  window %d  class %3d  device-pred %3d  PC-pred %3d  "
               "max logit diff %d  %lu us\n",
               k, (int)pb_validation_class[k], best,
               (int)pb_validation_pc_pred[k], diff_max, (unsigned long)us);
    }

    printf("\ntime          min %lu  avg %lu  max %lu us   (target < 1,000,000)\n",
           (unsigned long)time_min,
           (unsigned long)(time_total / PB_VALIDATION_COUNT),
           (unsigned long)time_max);
    printf("logits        %d/%d windows EXACTLY equal, largest diff %d, "
           "mean absolute diff %.4f\n",
           exact, PB_VALIDATION_COUNT, max_diff,
           (double)diff_total / (double)diff_count);
    printf("prediction    %d/%d windows chose the same class\n",
           pred_matches, PB_VALIDATION_COUNT);

    if (exact == PB_VALIDATION_COUNT) {
        printf("\nRESULT: the device matches the PC EXACTLY. The TFLM path is correct.\n");
    } else if (pred_matches == PB_VALIDATION_COUNT) {
        printf("\nRESULT: the logits deviate slightly but the predictions match.\n"
               "        If the deviation exceeds 1-2 steps, look for a kernel\n"
               "        difference.\n");
    } else {
        printf("\n[!] RESULT: the device produced a DIFFERENT prediction from the\n"
               "    PC. Something is wrong in TFLM/CMSIS-NN or the quantisation.\n"
               "    The audio path was never exercised here, so do not look at\n"
               "    mel.\n");
    }
}

/* ── X: BINARY NET — on-device validation ─────────────────────────────────
 *
 * The stage-1 binary net's counterpart to `x`: it runs the same windows
 * through the model on the PC (BUILTIN_REF) and on the device and compares
 * the raw int8 logit EXACTLY. The audio path (microphone, mel) is never
 * exercised here — a discrepancy means the fault is in TFLM/CMSIS-NN or the
 * quantisation.
 */
static void cmd_binary_ai_verify(void) {
    printf("\n=== BINARY NET - on-device validation ===\n");

    const uint32_t t_init0 = time_us_32();
    if (!pb_binary_net_init()) {
        printf("[!] the model could not be started.\n");
        return;
    }
    const uint32_t t_init = time_us_32() - t_init0;

    printf("startup       %lu us\n", (unsigned long)t_init);
    printf("arena         %u / %u bytes used  (%.1f%%)\n",
           (unsigned)pb_binary_net_arena_used(),
           (unsigned)pb_binary_net_arena_total(),
           100.0 * pb_binary_net_arena_used() / pb_binary_net_arena_total());
    printf("output quant. scale %.9f  zero %d\n",
           (double)pb_binary_net_output_scale(), pb_binary_net_output_zero());

    int8_t *input = pb_binary_net_input();

    uint32_t time_min = 0xFFFFFFFFu, time_max = 0, time_total = 0;
    int exact = 0, pred_matches = 0, max_diff = 0;

    for (int k = 0; k < PB_BINARY_VALIDATION_COUNT; k++) {
        memcpy(input, pb_binary_validation_input[k],
               (size_t)PB_BINARY_VALIDATION_FRAMES * PB_BINARY_VALIDATION_BANDS);
        if (!pb_binary_net_run()) {
            printf("[!] window %d: Invoke failed\n", k);
            return;
        }
        const uint32_t us = pb_binary_net_last_time_us();
        if (us < time_min) time_min = us;
        if (us > time_max) time_max = us;
        time_total += us;

        const int8_t device_logit = pb_binary_net_output();
        int diff = (int)device_logit - (int)pb_binary_validation_logit[k];
        if (diff < 0) diff = -diff;
        if (diff == 0) exact++;
        if (diff > max_diff) max_diff = diff;

        const bool device_pred = pb_binary_net_probability() >= 0.5f;
        const bool pc_pred = pb_binary_validation_logit[k] >=
                               pb_binary_net_output_zero();  /* equivalent to logit>=0 */
        if (device_pred == pc_pred) pred_matches++;

        printf("  window %d  truth %-6s  device-logit %4d  PC-logit %4d  "
               "diff %d  p=%.4f  %lu us\n",
               k, pb_binary_validation_truth[k] ? "BIRD" : "NOT",
               device_logit, pb_binary_validation_logit[k], diff,
               (double)pb_binary_net_probability(), (unsigned long)us);
    }

    printf("\ntime          min %lu  avg %lu  max %lu us\n",
           (unsigned long)time_min,
           (unsigned long)(time_total / PB_BINARY_VALIDATION_COUNT),
           (unsigned long)time_max);
    printf("logits        %d/%d windows EXACTLY equal, largest diff %d\n",
           exact, PB_BINARY_VALIDATION_COUNT, max_diff);
    printf("decision      %d/%d windows agree with the PC (>=0.5 threshold)\n",
           pred_matches, PB_BINARY_VALIDATION_COUNT);

    if (exact == PB_BINARY_VALIDATION_COUNT) {
        printf("\nRESULT: the device matches the PC EXACTLY. The TFLM path is correct.\n");
    } else if (pred_matches == PB_BINARY_VALIDATION_COUNT) {
        printf("\nRESULT: the logit deviates slightly but the decision matches.\n");
    } else {
        printf("\n[!] RESULT: the device produced a DIFFERENT decision from the PC.\n"
               "    Something is wrong in TFLM/CMSIS-NN or the quantisation.\n");
    }
}

/* ── k: real-time recognition (core 1) ────────────────────────────────────
 *
 * Core 1 reads audio and extracts mel, runs the species net once a second
 * while the gate is open, and votes over the last 8 windows; core 0 (here)
 * only prints the result.
 *
 * NOTE ON ACOUSTIC TESTING: the development machine has headphones plugged
 * in, so nothing comes out of its speakers. Testing this command with real
 * birdsong has to be done by hand. That the model itself is correct is
 * already proven by the `x` command, which never touches the microphone.
 */
static void cmd_recognize(bool gate_ignore) {
    printf("\n=== REAL-TIME RECOGNITION (core 1) ===\n");
    if (gate_ignore)
        printf("MEASUREMENT MODE: the gate is IGNORED, inference every second.\n");
    else
        printf("No inference runs unless the gate opens (2-3%% in silence).\n");
    printf("Voting window: %d\n", PB_VOTE_WINDOWS);
    printf("Press any key to exit.\n\n");

    if (!pb_recognizer_start(gate_ignore)) {
        printf("[!] the recognition pipeline could not be started.\n");
        return;
    }

    uint32_t seen = 0;
    absolute_time_t next = make_timeout_time_ms(1000);

    while (getchar_timeout_us(0) < 0) {
        pb_recognizer_state_t d;
        pb_recognizer_read(&d);

        if (d.version != seen && d.valid) {
            seen = d.version;
            printf("  [%lu] %lu windows voted, %lu us:\n",
                   (unsigned long)d.inference, (unsigned long)d.merged,
                   (unsigned long)d.last_time_us);
            for (int r = 0; r < 3; r++) {
                const int c = d.top3[r];
                if (c < 0 || c >= PB_CLASS_COUNT) continue;
                printf("      %d. %%%5.1f  %-10s %s\n", r + 1,
                       (double)(d.top3_probability[r] * 100.0f),
                       pb_class_code[c], pb_class_name[c]);
            }
        }

        if (time_reached(next)) {
            printf("  frames %lu  gate %%%lu  binary %lu (rejected %lu, last p=%.2f)  "
                   "inference %lu  skipped %lu  band %.1f dB  floor %.1f dB  "
                   "overrun %lu\n",
                   (unsigned long)d.frame,
                   (unsigned long)(d.frame ? d.gate_open * 100 / d.frame : 0),
                   (unsigned long)d.binary_ran, (unsigned long)d.binary_red,
                   (double)d.binary_last_p,
                   (unsigned long)d.inference, (unsigned long)d.skipped,
                   (double)d.band_db, (double)d.base_db,
                   (unsigned long)d.overrun);
            next = make_timeout_time_ms(1000);
        }
        sleep_ms(20);
    }

    pb_recognizer_state_t d;
    pb_recognizer_read(&d);
    pb_recognizer_stop();

    printf("\n  total frames %lu (%lu with the gate open, %%%lu)\n",
           (unsigned long)d.frame, (unsigned long)d.gate_open,
           (unsigned long)(d.frame ? d.gate_open * 100 / d.frame : 0));
    printf("  inferences %lu, windows skipped for a closed gate %lu\n",
           (unsigned long)d.inference, (unsigned long)d.skipped);
    printf("  binary net ran %lu, said \"not a bird\" (species net skipped) %lu\n",
           (unsigned long)d.binary_ran, (unsigned long)d.binary_red);
    printf("  audio ring overrun %lu  (should be 0; if not, inference is taking\n"
           "                          longer than the ring, see audio_i2s.h)\n\n",
           (unsigned long)d.overrun);
}

/* ── c: THE RESULT SCREEN ─────────────────────────────────────────────────
 *
 * The same thing `k` does, except the result goes to the SCREEN rather than
 * the serial console: the recognition card on the left (species name +
 * confidence + top 3 + counters) and the live spectrogram on the right. The
 * decision rule in between lives in ai/decision.c, and its thresholds were
 * measured (models/thresholds.txt, tools/measure_thresholds.py).
 *
 * THE SPLIT OF WORK:
 *   core 1  audio -> mel -> gate -> species net -> vote  (ai/recognizer.c)
 *   core 0  here: drain the queue, decision rule, LVGL, QSPI
 *
 * While the engine runs the mel ring belongs to core 1 (see the warning in
 * ai/recognizer.h); core 0 takes spectrogram columns from the
 * `pb_recognizer_get_mel()` queue and never touches mel directly.
 *
 * NOTE ON ACOUSTIC TESTING: the development machine has headphones plugged
 * in, so testing this screen with real birdsong has to be done by hand.
 */
static void cmd_result_screen(void) {
    printf("\n=== RESULT SCREEN ===\n");
    printf("Hold the device in landscape with the USB socket on the RIGHT.\n");
    printf("SCREEN 0 listen (top 3 species on the left, spectrogram on the right)\n");
    printf("SCREEN 1 log    (full width, the species identified so far)\n");
    printf("To switch: SWIPE HORIZONTALLY on the screen, or press space or 'n'.\n");
    printf("Decision rule: enter %.2f / exit %.2f, at least %u windows, "
           "hold %u ms\n", (double)PB_DECISION_ENTER_THRESHOLD,
           (double)PB_DECISION_EXIT_THRESHOLD, (unsigned)PB_DECISION_MIN_WINDOWS,
           (unsigned)PB_DECISION_HOLD_MS);
    printf("Press any other key to exit.\n\n");

    backlight_set(true);
    /* Same reasoning as the `a` demo: the fill puts lcd_blit's skip strip
     * into a known state, so no trail is left after the diagnostic
     * commands. */
    pb_lcd_fill(0x0000);
    pb_lv_flush_counters_reset();
    pb_lv_init();

    pb_ui_create();
    /* Draw the UI FIRST, then hand the right strip to the spectrogram:
     * LVGL's first draw paints all of the slices it owns. */
    for (int i = 0; i < 4; i++) { pb_ui_tick(); sleep_ms(5); }
    pb_spec_init();

    pb_mel_init();   /* the filter bank must be ready before core 1 starts */

    /* ⚠ HAT AÇILIŞTA ÇALIŞMIYOR — cihaz artık sürekli dinlemiyor
     * (kullanıcının kararı). Dinlemeyi kayıt butonu başlatıyor; bu döngü her
     * turda `pb_ui_recording()`ya bakıp core 1'i gerçekten başlatıp
     * durduruyor. `pb_recognizer_start`/`durdur` bu kullanıma uygun:
     * ikisi de `s_calis` ile korumalı ve başlatma core 1'i sıfırdan kuruyor.
     * ⚠ `pb_recognizer_stop` core 1'in döngüden çıkmasını beklemek için
     * 400 ms bloklanıyor — butona basınca arayüz o kadar takılır. */
    bool line_running = false;

    pb_decision_t decision;
    pb_decision_reset(&decision, to_ms_since_boot(get_absolute_time()));

    uint32_t seen = 0, kare0 = 0, last_rate = 0;
    uint32_t decision_version = decision.version;
    absolute_time_t next_rate  = make_timeout_time_ms(1000);
    absolute_time_t next_card = make_timeout_time_ms(250);
    pb_recognizer_state_t d;
    memset(&d, 0, sizeof(d));

    drain_stdin();
    for (;;) {
        /* Boşluk / 'n' ekran değiştiriyor, başka her tuş çıkıyor. Kaydırmanın
         * yedeği bu: dokunmatik bir kareyi düşürse bile ekran değiştirilebilir
         * kalıyor (arayuz.h'deki gerekçe). */
        const int key = getchar_timeout_us(0);
        if (key >= 0) {
            if (key == ' ' || key == 'n' || key == 'N') pb_ui_next();
            else if (key == 'r' || key == 'R')
                pb_ui_set_recording(!pb_ui_recording());
            else break;
        }

        /* 0) Kayıt durumu değiştiyse hattı gerçekten başlat/durdur. */
        if (pb_ui_recording() != line_running) {
            if (pb_ui_recording()) {
                if (pb_recognizer_start(false)) {
                    line_running = true;
                    pb_decision_reset(&decision, to_ms_since_boot(get_absolute_time()));
                    decision_version = decision.version;
                    seen = 0;
                    printf("  [kayit BASLADI]\n");
                } else {
                    printf("[!] the recognition pipeline could not be started.\n");
                    pb_ui_set_recording(false);
                }
            } else {
                pb_recognizer_stop();
                line_running = false;
                memset(&d, 0, sizeof(d));
                pb_decision_reset(&decision, to_ms_since_boot(get_absolute_time()));
                decision_version = decision.version;
                printf("  [kayit DURDU]\n");
            }
        }

        if (!line_running) {
            /* Boştayken ekran yalnızca "BOŞTA" gösteriyor; sonuç alanı
             * boşaltılıyor ki durmuş bir tahmin canlıymış gibi durmasın. */
            pb_result_view_t empty;
            memset(&empty, 0, sizeof(empty));
            empty.mode = PB_DECISION_LISTENING;
            pb_ui_update(&empty);
            pb_ui_tick();
            sleep_ms(10);
            continue;
        }

        /* 1) Spektrogram: core 1'in bıraktığı mel sütunlarını boşalt.
         *    Tur başına en fazla 8 sütun — çıkarım sonrası birikmiş kuyruk
         *    tek turda boşaltılmaya çalışılırsa arayüz o turda takılır. */
        int8_t mel_q[PB_MEL_BANDS];
        for (int i = 0; i < 8 && pb_recognizer_get_mel(mel_q); i++) {
            /* ⚠ Günlük ekranındayken spektrogram YAZMAMALI: o ekranda sağdaki
             * iki dilim de LVGL'in (arayuz.c, dilim sahipliği) ve ikisi aynı
             * bölgeye yazarsa birbirlerini siler. Kuyruk yine de boşaltılıyor,
             * yoksa core 1 dolu kuyruğa kare atmaya başlar. */
            if (pb_ui_screen() != PB_SCREEN_LISTEN) continue;

            uint8_t bins[PB_MEL_BANDS];
            for (int b = 0; b < PB_MEL_BANDS; b++) {
                /* `a` demosuyla AYNI gösterim penceresi: -75..-15 dB. */
                float v = (pb_mel_q_to_db(mel_q[b]) + 75.0f) * (255.0f / 60.0f);
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                bins[b] = (uint8_t)v;
            }
            pb_spec_push_column(bins, PB_MEL_BANDS);
        }

        /* 2) Tanıma durumu -> karar kuralı. */
        pb_recognizer_read(&d);
        pb_decision_input_t gi = {
            .now_ms   = to_ms_since_boot(get_absolute_time()),
            .gate_open  = d.gate_now,
            .fresh_result = d.valid && d.version != seen,
            .cls      = d.valid ? d.top3[0] : (int16_t)-1,
            .probability   = d.valid ? d.top3_probability[0] : 0.0f,
            .merged   = d.merged,
        };
        if (gi.fresh_result) seen = d.version;
        pb_decision_update(&decision, &gi);

        /* Ekranda görünen her değişiklik seri porta da düşsün: bu komutun
         * göz gerektirmeyen kaydı bu — kullanıcı ekrana bakarken ben aynı
         * olayları terminalde okuyabiliyorum. */
        if (decision.version != decision_version) {
            decision_version = decision.version;
            const int c = decision.cls;
            printf("  [%lu ms] %-14s %s %s  %%%.1f\n",
                   (unsigned long)gi.now_ms, pb_decision_mode_name(decision.mode),
                   (c >= 0 && c < PB_CLASS_COUNT) ? pb_class_code[c] : "-",
                   (c >= 0 && c < PB_CLASS_COUNT) ? pb_class_name[c] : "",
                   (double)(decision.confidence * 100.0f));
        }

        if (time_reached(next_rate)) {
            last_rate = d.frame - kare0;
            kare0 = d.frame;
            next_rate = make_timeout_time_ms(1000);
        }

        /* Kartı 4 Hz güncelle. Her güncelleme kartın QSPI'ye yeniden basılması
         * (68,8 KB) demek; sonuç ekranında daha hızlısının bir karşılığı yok. */
        if (time_reached(next_card)) {
            pb_result_view_t view;
            memset(&view, 0, sizeof(view));
            view.mode = decision.mode;
            view.confidence = decision.confidence;
            view.species_name = (decision.cls >= 0 && decision.cls < PB_CLASS_COUNT)
                            ? pb_class_name[decision.cls] : NULL;
            if (d.valid) {
                for (int r = 0; r < 3; r++) {
                    const int c = d.top3[r];
                    if (c >= 0 && c < PB_CLASS_COUNT) {
                        view.top3_name[r]    = pb_class_name[c];
                        view.top3_latin[r] = pb_class_latin[c];
                    }
                    view.top3_probability[r] = d.top3_probability[r];
                }
            }
            view.frame_rate = last_rate;
            view.inference = d.inference;
            view.merged = d.merged;
            view.overrun = d.overrun;
            view.band_db = d.band_db;
            pb_ui_update(&view);
            next_card = make_timeout_time_ms(250);
        }

        pb_ui_tick();
        sleep_ms(2);
    }

    pb_recognizer_read(&d);
    pb_recognizer_stop();

    printf("\n  kare %lu (kapi acik %%%lu), cikarim %lu, atlanan %lu, "
           "overrun %lu\n",
           (unsigned long)d.frame,
           (unsigned long)(d.frame ? d.gate_open * 100 / d.frame : 0),
           (unsigned long)d.inference, (unsigned long)d.skipped,
           (unsigned long)d.overrun);
    printf("  LVGL flush %lu, satir adimi != alan_w: %lu, panel_w %lu..%lu, "
           "dilim basimi %lu\n",
           (unsigned long)pb_lv_flush_count,
           (unsigned long)pb_lv_flush_stride_differs,
           (unsigned long)pb_lv_flush_w_min, (unsigned long)pb_lv_flush_w_max,
           (unsigned long)pb_lv_slice_press);
    /* Kaydırma teşhisi — GÖZ GEREKMEZ. Kaydırma çalışmıyorsa hangi aşamada
     * durduğu buradan okunuyor: dokunma hiç gelmiyor mu, geliyor da hareket
     * eşiği mi aşılmıyor, yoksa panel dışı kareler mi düşürülüyor. */
    printf("  kaydirma: dokunma %lu, basla %lu, kabul %lu, kisa/egik %lu, "
           "panel disi %lu, son ham dx %ld (esik 200), ham_y %ld, "
           "buton basim %lu\n\n",
           (unsigned long)pb_swipe_touch, (unsigned long)pb_swipe_begin,
           (unsigned long)pb_swipe_accept, (unsigned long)pb_swipe_short,
           (unsigned long)pb_lv_touch_invalidate,
           (long)pb_swipe_last_dx, (long)pb_swipe_last_dy,
           (unsigned long)pb_button_press);
}

/* ── C: SONUÇ KARTI GÖSTERİM TESTİ (mikrofonsuz) ──────────────────────────
 *
 * NEDEN AYRI BİR KOMUT: `c` ancak gerçek bir kuş sesi duyulursa tür adı
 * yazıyor. Sessiz odada kart hep "dinliyor" gösterir, yani ASIL çizim yolu
 * (uzun tür adı, sarma, güven, ilk 3, renkler) hiç sınanmaz. Bu komut aynı
 * kartı sahte bir sonuç dizisiyle sürüyor: göz testi tek bakışta yapılabilsin.
 *
 * KARAR KURALI BİLEREK DEVREDE DEĞİL — burada sınanan şey ÇİZİM. Kuralın
 * kendisi host testlerinde (test/dsp_test.c, `test_karar`) zaman ilerletilerek
 * sınanıyor; ikisini karıştırmak, bir hata çıktığında hangisinde olduğunu
 * belirsizleştirirdi.
 *
 * Desen düz renk DEĞİL (§9o uyarısı): en uzun tür adı, iki alternatif satırı
 * ve sağda hareketli bir spektrogram deseni var — kayma olursa yazıda görünür.
 */
static void cmd_result_card_demo(void) {
    /* En UZUN tür adını bul: sarmanın ve kenarların en kötü durumu bu.
     * İndeks sabitlemek yerine aramak, sınıf tablosu yeniden üretilse de
     * testin en kötü durumu göstermeye devam etmesini sağlıyor. */
    int lengthy = 0;
    for (int i = 0; i < PB_CLASS_COUNT; i++) {
        if (strlen(pb_class_name[i]) > strlen(pb_class_name[lengthy])) lengthy = i;
    }
    const int second = (lengthy + 1) % PB_CLASS_COUNT;
    const int third = (lengthy + 2) % PB_CLASS_COUNT;

    printf("\n=== SONUC KARTI GOSTERIM TESTI (mikrofon YOK) ===\n");
    printf("Cihazi USB soketi SAGDA olacak sekilde yatay tutun.\n");
    printf("En uzun tur adi: \"%s\" (%d karakter)\n",
           pb_class_name[lengthy], (int)strlen(pb_class_name[lengthy]));
    printf("Kart dort asamadan gecip basa donuyor. Cikmak icin bir tusa basin.\n\n");

    backlight_set(true);
    pb_lcd_fill(0x0000);
    pb_lv_init();
    pb_ui_create();
    for (int i = 0; i < 4; i++) { pb_ui_tick(); sleep_ms(5); }
    pb_spec_init();

    /* Günlük ekranı da sınanabilsin: boş liste hiçbir çizim sorununu
     * göstermez. Üç sahte kayıt, en uzun ad dâhil. */
    pb_ui_log_add(pb_class_name[lengthy],   pb_class_latin[lengthy],   0.91f);
    pb_ui_log_add(pb_class_name[second], pb_class_latin[second], 0.74f);
    pb_ui_log_add(pb_class_name[third], pb_class_latin[third], 0.63f);

    const struct { pb_decision_mode_t mode; bool species; float confidence; const char *ne; }
    stage[] = {
        { PB_DECISION_LISTENING, false, 0.00f, "dinliyor (tur yok)"      },
        { PB_DECISION_SOUND,      false, 0.00f, "ses algilandi"           },
        { PB_DECISION_UNSURE, true,  0.42f, "belirsiz, kehribar"      },
        { PB_DECISION_SPECIES,      true,  0.91f, "tur adi, sari, en uzun"  },
    };
    const int count = (int)(sizeof(stage) / sizeof(stage[0]));

    int a = 0;
    uint32_t column = 0;
    absolute_time_t next = make_timeout_time_ms(1);
    drain_stdin();

    for (;;) {
        const int key = getchar_timeout_us(0);
        if (key >= 0) {
            if (key == ' ' || key == 'n' || key == 'N') pb_ui_next();
            else break;
        }

        if (time_reached(next)) {
            pb_result_view_t view;
            memset(&view, 0, sizeof(view));
            view.mode = stage[a].mode;
            view.confidence = stage[a].confidence;
            view.species_name = stage[a].species ? pb_class_name[lengthy] : NULL;
            view.top3_name[0] = pb_class_name[lengthy];
            view.top3_name[1] = pb_class_name[second];
            view.top3_name[2] = pb_class_name[third];
            view.top3_latin[0] = pb_class_latin[lengthy];
            view.top3_latin[1] = pb_class_latin[second];
            view.top3_latin[2] = pb_class_latin[third];
            view.top3_probability[0] = stage[a].confidence;
            view.top3_probability[1] = 0.21f;
            view.top3_probability[2] = 0.07f;
            view.frame_rate = 62;
            view.inference = (uint32_t)a + 1;
            view.merged = 8;
            view.overrun = 0;
            view.band_db = -38.0f;
            pb_ui_update(&view);

            printf("  asama %d/%d: %s  [ekran %d]\n", a + 1, count, stage[a].ne,
                   pb_ui_screen());
            a = (a + 1) % count;
            next = make_timeout_time_ms(2500);
        }

        /* Spektrogram şeridi: gerçek bir ötüşe benzeyen desen — heceler,
         * süpüren bir temel frekans, iki harmonik ve gürültü tabanı.
         *
         * Düz renk OLMAMASI şart (§9o): düz blok satır kaymasını gizler.
         * Ama düz olmayan her desen de yetmiyor — tek bir hareketli tepe
         * gerçek spektrograma benzemediği için "doğru görünüyor mu?"
         * sorusuna cevap vermiyordu. Bu desen hem kaymayı gösteriyor hem de
         * gerçek çıktının nasıl görüneceğini. */
        uint8_t bins[PB_MEL_BANDS];
        const uint32_t phase = column % 48;             /* hece ~0,77 s        */
        const bool silent = (phase >= 34);             /* heceler arası       */
        const int base = 13 + (int)(phase < 17 ? phase : 34 - phase);  /* süpürme */
        for (int b = 0; b < PB_MEL_BANDS; b++) {
            int v = 10 + (int)((column * 7u + (uint32_t)b * 13u) % 9u);  /* taban */
            if (!silent) {
                for (int h = 1; h <= 3; h++) {
                    const int center = base * h;
                    if (center >= PB_MEL_BANDS) break;
                    int d = b - center;
                    if (d < 0) d = -d;
                    if (d <= 2) {
                        const int parlak = 255 - d * 70 - (h - 1) * 60;
                        if (parlak > v) v = parlak;
                    }
                }
            }
            bins[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        if (pb_ui_screen() == PB_SCREEN_LISTEN) {
            pb_spec_push_column(bins, PB_MEL_BANDS);
        }
        column++;

        pb_ui_tick();
        sleep_ms(16);
    }
    printf("cikildi\n");
    printf("  kaydirma: dokunma %lu, basla %lu, kabul %lu, kisa/egik %lu, "
           "panel disi %lu, son dx %ld dy %ld, buton basim %lu\n\n",
           (unsigned long)pb_swipe_touch, (unsigned long)pb_swipe_begin,
           (unsigned long)pb_swipe_accept, (unsigned long)pb_swipe_short,
           (unsigned long)pb_lv_touch_invalidate,
           (long)pb_swipe_last_dx, (long)pb_swipe_last_dy,
           (unsigned long)pb_button_press);
}

/* ── F: KART FRAMEBUFFER DÖKÜMÜ — göz GEREKMEZ, teşhisi ikiye böler ───────
 *
 * `C` ekranında yazılar üst üste binmiş görünüyordu. İki ihtimal vardı ve
 * fotoğraftan ayırt edilemiyordu:
 *   (a) LVGL/yerleşim yanlış çiziyor  -> framebuffer'da da bozuk olur
 *   (b) panele giden yol bozuyor      -> framebuffer TEMİZ, ekran bozuk
 *
 * Bu komut kartı iki kez kurup (a) hâlini seri porta ASCII döküyor. Terminalde
 * yazı düzgün okunuyorsa suçlu (b), okunmuyorsa (a) — ve (a) panele hiç
 * bakmadan düzeltilebilir.
 *
 * İKİ AŞAMA BİLEREK: önce kısa içerik ("dinliyor", tür yok), sonra en uzun
 * tür adı. İkinci dökümde birinci aşamanın kalıntısı varsa sorun "eski yazı
 * silinmiyor"dur; kalıntı yoksa ve satırlar üst üste biniyorsa sorun
 * yerleşimdir (etiketler birbirinin alanına taşıyor).
 */
static void cmd_card_fb_dump(void) {
    int lengthy = 0;
    for (int i = 0; i < PB_CLASS_COUNT; i++) {
        if (strlen(pb_class_name[i]) > strlen(pb_class_name[lengthy])) lengthy = i;
    }

    printf("\n=== KART FRAMEBUFFER DOKUMU (goz GEREKMEZ) ===\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);
    pb_lv_init();
    pb_ui_create();

    for (int stage = 0; stage < 2; stage++) {
        pb_result_view_t view;
        memset(&view, 0, sizeof(view));
        if (stage == 0) {
            view.mode = PB_DECISION_LISTENING;
            printf("\n--- asama 1: dinliyor, tur yok ---\n");
        } else {
            view.mode = PB_DECISION_SPECIES;
            view.species_name = pb_class_name[lengthy];
            view.confidence = 0.91f;
            view.top3_name[0] = pb_class_name[lengthy];
            view.top3_name[1] = pb_class_name[(lengthy + 1) % PB_CLASS_COUNT];
            view.top3_name[2] = pb_class_name[(lengthy + 2) % PB_CLASS_COUNT];
            view.top3_probability[0] = 0.91f;
            view.top3_probability[1] = 0.21f;
            view.top3_probability[2] = 0.07f;
            view.frame_rate = 62;
            view.inference = 6;
            view.merged = 8;
            view.band_db = -38.0f;
            printf("\n--- asama 2: en uzun tur adi (\"%s\") ---\n",
                   pb_class_name[lengthy]);
        }
        pb_ui_update(&view);
        for (int i = 0; i < 6; i++) { pb_ui_tick(); sleep_ms(5); }
        pb_lv_dump_card_fb();
    }
    printf("\n");
}

static void print_help(void) {
    printf("\nKomutlar:\n");
    printf("  i  cihaz ve ses yapilandirmasi\n");
    printf("  n  gurultu tabani olcumu\n");
    printf("  e  EMI taramasi (arka isik etkisi)\n");
    printf("  g  mikrofon kazanci (0-7)\n");
    printf("  r  %d s kayit al ve aktar\n", CAPTURE_SECONDS);
    printf("  d  ekran testi (panel baslatma varyantlari)\n");
    printf("  o  yon testi (dort koseye dort renk)\n");
    printf("  t  dokunmatik teshisi ve koordinat esleme\n");
    printf("  m  mel + kapi hatti (M3, canli mikrofon)\n");
    printf("  a  DEMO: LVGL kart + canli mel spektrogrami + kapi\n");
    printf("  b  arka isik teshisi\n");
    printf("  v  QSPI veri yolu teshisi\n");
    printf("  w  QSPI zamanlama teshisi (WaitIdle olcumu, goz GEREKMEZ)\n");
    printf("  y  melez yol testi: pencere ve piksel ayri yollardan (goz gerekir)\n");
    printf("  z  satir adresleme testi: RASET calisiyor mu (goz gerekir)\n");
    printf("  j  imlec konumlandirma testi: dar pencereyle ucuz atlama (goz gerekir)\n");
    printf("  L  yazi teshisi: LVGL cizimini ASCII dok (goz GEREKMEZ)\n");
    printf("  S  dar pencere kayma testi: cizgiler duz mu (goz gerekir)\n");
    printf("  s  canli spektrogram\n");
    printf("  x  TUR AGI: cihaz ici dogrulama + arena + cikarim suresi\n");
    printf("  X  IKILI AGI (Asama-1): cihaz ici dogrulama + arena + cikarim suresi\n");
    printf("  k  gercek zamanli tanima (core 1, seri porta yazar)\n");
    printf("  K  aynisi ama kapi yoksayilir — olcum kipi\n");
    printf("  c  ARAYUZ: dinleme + gunluk ekrani, canli tanima (goz gerekir)\n");
    printf("  C  arayuz gosterim testi, mikrofonsuz (goz gerekir)\n");
    printf("     c/C icinde: KAYDIR ya da bosluk/'n' = ekran degistir\n");
    printf("  F  kart framebuffer dokumu: ASCII, goz GEREKMEZ\n");
    printf("  ?  bu yardim\n\n");
}

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\n========================================\n");
    printf(" PokeBird — M1: mikrofon bring-up\n");
    printf("========================================\n");

    power_latch_init();
    backlight_init();
    pb_i2c_init();

    /* SIRA ÖNEMLİ: ES8311'in dahili PLL'i MCLK olmadan register yazimlarina
     * duzgun tepki vermiyor. Once MCLK, sonra codec yapilandirmasi. */
    if (!pb_audio_mclk_start(&s_audio_cfg)) {
        printf("[!] MCLK baslatilamadi.\n");
    }
    sleep_ms(10);

    if (!pb_i2c_probe(ES8311_I2C_ADDR)) {
        printf("[!] ES8311 I2C adresi 0x%02x yanit vermiyor.\n", ES8311_I2C_ADDR);
        printf("    Hat: SDA=GPIO%d SCL=GPIO%d\n", PB_PIN_I2C_SDA, PB_PIN_I2C_SCL);
    }

    es8311_init(s_audio_cfg);
    es8311_sample_frequency_config((int)s_audio_cfg.mclk_freq, (int)s_audio_cfg.sample_freq);
    es8311_microphone_config();
    es8311_microphone_gain_set((es8311_mic_gain_t)s_mic_gain);

    if (!pb_audio_i2s_init(&s_audio_cfg)) {
        printf("[!] I2S yakalama yolu kurulamadi.\n");
    }

    /* ── Ekran ──────────────────────────────────────────────────────────
     * QSPI pio0'da, ses pio1'de — state machine çakışması yok.
     *
     * SIRA ZORUNLU (Waveshare örneğindeki sıra):
     *   1. QSPI_GPIO_Init   — CS/RST/PWR_EN pin yönleri
     *   2. QSPI_PIO_Init    — PIO programını yükle (SM'leri KAPALI bırakır)
     *   3. QSPI_4Wrie_Mode  — 4-bit SM'i ETKİNLEŞTİR ve qspi.sm'i ayarla
     *   4. pb_display_dma_init — DREQ doğru SM'e bağlansın diye 3'ten sonra
     *   5. LCD_3IN49_Init   — panel reset + register dizisi
     *
     * 3. adım atlanırsa hiçbir SM çalışmadığı için PIO TX FIFO hiç
     * boşalmıyor ve DMA sonsuza kadar bekliyor — ekran tamamen siyah kalıyor,
     * üstelik ilk çizim çağrısında kilitleniyor. */
    /* PANEL HAZIR OLMA PENCERESİ — silmeyin, ölçülerek kondu.
     *
     * AXS15231B açılıştan sonra bir süre başlatma dizisini kabul etmiyor.
     * Erken başlatılırsa panel kendi başlatılmamış GRAM'ını göstermeye devam
     * ediyor (karıncalanma) ve bu durum KALICI: GPIO34 dışarıdan yüksek
     * tutulduğu için panele donanım reset'i atamıyoruz (§5.11), yani yeniden
     * deneme şansı yok.
     *
     * NEDEN BÖYLE ÖĞRENDİK: `s_capture` temizliği bss'i 218.988'den 127.084'e
     * indirdi. bss'i sıfırlamak (crt0, main'den önce) o kadar kısa sürmeye
     * başladı ki firmware panel başlatmaya ~1 ms daha erken varır oldu ve
     * ekran bozuldu. Yani eski hâl bu pencereyi KIL PAYI geçiyormuş; hata
     * kodda zaten vardı, temizlik yalnızca payı bitirdi. Kartta ölçüldü:
     * 20 ms'lik gecikme yetiyor (500 ms de çalışıyor, 0 çalışmıyor).
     *
     * NEDEN `sleep_ms` DEĞİL: sabit bir uyku yalnızca o anki paya sabit bir
     * miktar ekler; kendinden ÖNCEKİ kod hızlanırsa aynı tuzak yeniden kurulur
     * (bizi buraya tam olarak bu düşürdü). Mutlak alt sınır kısıtın kendisini
     * ifade ediyor ve önceki kodun süresinden bağımsız. USB beklemesi zaten
     * uzun sürdüyse bu satır hiç beklemez, yani normalde bedeli sıfır. */
    while (to_ms_since_boot(get_absolute_time()) < 250) sleep_ms(5);

    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    pb_display_dma_init();
    LCD_3IN49_Init();
    pb_lcd_fill(0x0000);
    printf("Ekran hazir (%dx%d panel).\n", PB_PANEL_W, PB_PANEL_H);

    cmd_info();
    print_help();

    /* Cihaz PC'ye bagli olmadan, elde tasinirken kendiliginden tanima
     * ekranina girsin diye. Once burada baslatiyoruz; ekrandan (bosluk/n/r
     * disinda) bir tusa basilirsa asagidaki komut dongusune duser, PC
     * baglanmissa teshis komutlari yine erisilebilir kalir. */
    cmd_result_screen();

    while (true) {
        printf("> ");
        /* getchar_timeout_us(0) HEMEN doner (0 = beklemeden zaman asimi),
         * bu da istemi bos yere dondurur. Bloklayan okuma icin getchar(). */
        int c = getchar();
        if (c < 0) continue;
        switch (c) {
            case 'i': cmd_info();      break;
            case 'n': cmd_noise();     break;
            case 'e': cmd_emi_sweep(); break;
            case 'g': cmd_gain();      break;
            case 'r': cmd_record();    break;
            case 'l': cmd_level_meter(); break;
            case 's': cmd_spectrogram(); break;
            case 'd': cmd_display_test(); break;
            case 'b': cmd_backlight_probe(); break;
            case 'v': cmd_datapath_probe(); break;
            case 'w': cmd_qspi_timing(); break;
            case 'y': cmd_hybrid_path(); break;
            case 'z': cmd_row_addr(); break;
            case 'j': cmd_cursor_seek(); break;
            case 'L': cmd_text_dump(); break;
            case 'S': cmd_stripe_test(); break;
            case 'o': cmd_orientation(); break;
            case 't': cmd_touch_probe(); break;
            case 'u': cmd_ui_demo(); break;
            case 'm': cmd_mel_pipeline(); break;
            case 'a': cmd_full_demo(); break;
            case 'x': cmd_ai_verify(); break;
            case 'X': cmd_binary_ai_verify(); break;
            case 'k': cmd_recognize(false); break;
            case 'K': cmd_recognize(true);  break;
            case 'c': cmd_result_screen();   break;
            case 'C': cmd_result_card_demo(); break;
            case 'F': cmd_card_fb_dump();    break;
            case '?': print_help();    break;
            case '\r': case '\n': printf("\r"); break;
            default:  printf("bilinmeyen komut ('?' yardim)\n"); break;
        }
    }
}
