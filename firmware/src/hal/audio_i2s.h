/**
 * audio_i2s.h — I2S capture from the ES8311's microphone (PIO + DMA)
 *
 * Clock architecture: the RP2350 only produces MCLK; the ES8311 is the I2S
 * master and derives BCLK and LRCK from it. This layer follows the clock, it
 * does not drive it.
 */
#ifndef POKEBIRD_AUDIO_I2S_H
#define POKEBIRD_AUDIO_I2S_H

#include <stdbool.h>
#include <stdint.h>

/**
 * The configuration the ES8311 driver expects.
 *
 * The field names are deliberately identical to Waveshare's original
 * `pico_audio_t` struct, so es8311.c's register sequences can be used
 * line for line without modification (see the attribution note at the top of
 * es8311.c).
 */
typedef struct {
    uint32_t mclk_freq;     /* Hz — the MCLK the RP2350 generates with PIO  */
    uint32_t sample_freq;   /* Hz — the LRCK the ES8311 derives from MCLK   */
    uint8_t  res_in;        /* bits — ADC (microphone) resolution           */
    uint8_t  res_out;       /* bits — DAC (speaker) resolution              */
} pb_audio_cfg_t;

/** Problems encountered during capture — they tell you whether a measurement
 *  can be trusted. */
typedef struct {
    uint32_t samples;       /* samples actually read                        */
    bool     fifo_overrun;  /* true means samples were DROPPED, untrustworthy */
    bool     timed_out;     /* no clock (the ES8311 is not driving BCLK/LRCK) */
} pb_capture_result_t;

/**
 * Ring buffer capacity, in samples. About 341 ms at 24 kHz.
 *
 * RAISED FROM 4096 IN M6 — A MEASUREMENT-DRIVEN DECISION (170 ms -> 256 ms of
 * tolerance, giving 1.35x headroom over the species net's 190 ms inference).
 *
 * 8192 (32 KB) IS THE HARDWARE'S ABSOLUTE CEILING — DO NOT TRY TO RAISE IT.
 * When the stage-1 binary net was added in M7 the total inference time became
 * 259 ms and crossed the 256 ms threshold. "Grow the ring to 16384" was tried
 * and it LOCKED THE BOARD UP COMPLETELY. The root cause: the RP2350's DMA
 * `RING_SIZE` field is 4 bits (dma.h: `DMA_CHx_CTRL_TRIG_RING_SIZE_BITS`,
 * MSB 11 LSB 8), so the largest representable value is 15 and the maximum
 * ring is **2^15 = 32,768 bytes = 8192 samples**. Setting
 * `PB_RING_ADDR_BITS 16` made the hardware register take a silently wrong
 * value and the DMA collapsed.
 *
 * The correct fix is never to run the binary net and the species net on the
 * SAME window (see recognizer.c, binary_pending) — the worst-case inference
 * time then stays at 190 ms, which this ring handles without growing at all.
 */
#define PB_AUDIO_RING_SAMPLES  8192

/**
 * The largest chunk that can be read in one call.
 *
 * Deliberately NOT tied to the ring size (it used to be RING/2): the
 * diagnostic commands size their capture buffer `s_chunk` from this constant,
 * so growing the ring would have grown that too — this is the buffer that was
 * cut from 96 KB to 4 KB.
 */
#define PB_AUDIO_MAX_READ      2048

/**
 * Start MCLK, set up the I2S capture path and begin continuous capture.
 * The ES8311 must be configured separately over I2C (es8311_init). MCLK has
 * to be running BEFORE the ES8311 is configured — the codec's internal PLL
 * does not respond properly to register writes without it.
 */
bool pb_audio_i2s_init(const pb_audio_cfg_t *cfg);

/** Start MCLK on its own (called before configuring the ES8311). */
bool pb_audio_mclk_start(const pb_audio_cfg_t *cfg);

/* ── Continuous capture ────────────────────────────────────────────────────
 *
 * DMA fills the ring buffer without ever stopping and the consumer reads at
 * its own pace. No samples are LOST between one read and the next, as long as
 * the processing time stays within the ring's capacity.
 *
 * Why it works this way: the old `pb_audio_capture` flushed the FIFO and
 * restarted DMA from scratch on every call. Samples arriving during the
 * processing gap overflowed the PIO's 8-word FIFO and were dropped, which is
 * why the mel pipeline could only run at 91% of real time (about 57 fps
 * instead of 62.5).
 */

/** Start continuous capture. `pb_audio_i2s_init` already calls this. */
bool pb_audio_stream_start(void);

/** Stop continuous capture (the DMA chain breaks, PIO keeps running). */
void pb_audio_stream_stop(void);

/**
 * Discard whatever has accumulated and continue from the freshest sample.
 * For live indicators (level, spectrogram): rather than showing stale data
 * they jump to the current point. The mel pipeline does NOT call this — it
 * needs continuity.
 */
void pb_audio_stream_flush(void);

/** How many samples are waiting to be read in the ring. */
uint32_t pb_audio_stream_available(void);

/**
 * Read `n_samples` samples from the stream (at most PB_AUDIO_MAX_READ).
 * Waits until enough samples accumulate; if they do not within `timeout_ms`
 * it returns with `timed_out` set (which means there is no clock).
 *
 * If the consumer falls a full ring behind, the oldest samples are overwritten
 * by the writer. In that case the read jumps to the freshest data and reports
 * `fifo_overrun` — it never silently returns corrupt data.
 */
pb_capture_result_t pb_audio_stream_read(int16_t *dst, uint32_t n_samples,
                                         uint32_t timeout_ms);

/**
 * Convenience wrapper: discard the backlog, then read `n_samples` samples
 * without interruption. `n_samples` may exceed the ring size — because
 * reading is faster than real time, the ring never overflows.
 *
 * For the diagnostic commands; the real-time pipeline uses
 * `pb_audio_stream_read`.
 */
pb_capture_result_t pb_audio_capture(int16_t *dst, uint32_t n_samples);

#endif /* POKEBIRD_AUDIO_I2S_H */
