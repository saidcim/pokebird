/**
 * audio_i2s.c — I2S capture from the ES8311's microphone (PIO + DMA)
 *
 * The PIO programs were adapted from Waveshare's RP2350-Touch-LCD-3.5 example
 * (MIT); see the header of audio_i2s.pio for details.
 */
#include "hal/audio_i2s.h"

#include <assert.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "audio_i2s.pio.h"
#include "board_config.h"

/* The `wait ... gpio N` instructions inside audio_i2s.pio use ABSOLUTE GPIO
 * numbers and cannot be parameterised from C. If the pins change, the .pio
 * file has to be updated by hand; these checks stop us from silently
 * listening to the wrong pin. */
static_assert(PB_PIN_I2S_SCLK == 4, "audio_i2s.pio waits for BCLK with 'wait gpio 4'");
static_assert(PB_PIN_I2S_LRCK == 5, "audio_i2s.pio waits for LRCK with 'wait gpio 5'");

#define PB_PIO              pio1
#define PB_SM_MCLK          0
#define PB_SM_RX            1

/* ── Ring buffer ──────────────────────────────────────────────────────────
 *
 * This uses DMA's address-wrapping (ring) feature: the low bits of the write
 * address are masked, so the channel returns to the start by itself when it
 * reaches the end of the buffer. That has two requirements — the buffer size
 * must be a power of two, and the buffer must be aligned to its own size.
 *
 * 8192 samples = 32 KB = 341 ms at 24 kHz (raised from 4096 in M6; the
 * reasoning is in audio_i2s.h). DO NOT GROW IT — the hardware's DMA ring
 * field is 4 bits, capping the ring at 32 KB (see the warning in
 * audio_i2s.h). */
#define PB_RING_WORDS       PB_AUDIO_RING_SAMPLES
#define PB_RING_MASK        (PB_RING_WORDS - 1)
#define PB_RING_ADDR_BITS   15                  /* 1<<15 = 32768 bytes — HARDWARE CEILING */

static_assert((PB_RING_WORDS & PB_RING_MASK) == 0, "ring size must be a power of two");
static_assert((1u << PB_RING_ADDR_BITS) == PB_RING_WORDS * sizeof(uint32_t),
              "PB_RING_ADDR_BITS does not match the ring size");

/* DMA writes, the CPU reads: volatile so the compiler does not cache the
 * reads. */
static volatile uint32_t s_ring[PB_RING_WORDS]
    __attribute__((aligned(1u << PB_RING_ADDR_BITS)));

/* The count the control channel writes back into the data channel.
 * Deliberately in RAM: making DMA read from flash (XIP) would be an
 * unnecessary dependency. */
static uint32_t s_reload_words = PB_RING_WORDS;

static uint32_t s_read_idx = 0;
static int      s_dma_data = -1;
static int      s_dma_ctrl = -1;
static bool     s_mclk_running = false;
static bool     s_rx_ready = false;
static bool     s_stream_running = false;

/* ── MCLK ──────────────────────────────────────────────────────────────── */

bool pb_audio_mclk_start(const pb_audio_cfg_t *cfg) {
    if (s_mclk_running) return true;
    if (!cfg || cfg->mclk_freq == 0) return false;

    uint offset = pio_add_program(PB_PIO, &mclk_pio_program);
    mclk_pio_program_init(PB_PIO, PB_SM_MCLK, offset, PB_PIN_I2S_MCLK);

    /* The mclk_pio loop takes 5 instructions, so one MCLK period is
     * 5 x clkdiv system clocks. 150 MHz / (4.8828125 x 5) = 6.144 MHz. */
    float div = ((float)clock_get_hz(clk_sys) / (float)cfg->mclk_freq) / 5.0f;
    pio_sm_set_clkdiv(PB_PIO, PB_SM_MCLK, div);
    pio_sm_set_enabled(PB_PIO, PB_SM_MCLK, true);

    s_mclk_running = true;
    return true;
}

/* ── Capture ───────────────────────────────────────────────────────────── */

bool pb_audio_i2s_init(const pb_audio_cfg_t *cfg) {
    if (!pb_audio_mclk_start(cfg)) return false;
    if (s_rx_ready) return true;

    uint offset = pio_add_program(PB_PIO, &i2s_rx_pio_program);
    i2s_rx_pio_program_init(PB_PIO, PB_SM_RX, offset,
                            PB_PIN_I2S_DSOUT, PB_PIN_I2S_SCLK, PB_PIN_I2S_LRCK);
    /* The RX state machine follows the clock the ES8311 generates; it must
     * not be slowed down by a divider of its own. */
    pio_sm_set_clkdiv(PB_PIO, PB_SM_RX, 1.0f);

    s_dma_data = dma_claim_unused_channel(false);
    s_dma_ctrl = dma_claim_unused_channel(false);
    if (s_dma_data < 0 || s_dma_ctrl < 0) return false;

    /* The RX state machine is started ONCE here and never stopped again.
     *
     * Why: stopping it on every capture and restarting with pio_sm_restart()
     * meant the I2S frame lock had to be re-established each time. Because
     * the LRCK and BCLK edges change almost simultaneously, that re-lock is a
     * race, and the lock sometimes landed in the wrong slot. The result was
     * reading pure noise on a random fraction of measurements (uncorrelated,
     * all 16 bits random — not a simple bit shift).
     *
     * Once the lock is established correctly it maintains itself: the program
     * realigns on LRCK's falling edge every frame. */
    pio_sm_clear_fifos(PB_PIO, PB_SM_RX);
    pio_sm_restart(PB_PIO, PB_SM_RX);
    pio_sm_exec(PB_PIO, PB_SM_RX, pio_encode_jmp(offset));  /* rewind the PC */
    pio_sm_set_enabled(PB_PIO, PB_SM_RX, true);

    s_rx_ready = true;
    return pb_audio_stream_start();
}

/* The RXSTALL bit in PIO FDEBUG: an IN instruction stalled because the RX
 * FIFO was full, meaning a sample was dropped. It is the only real indicator
 * of whether a measurement can be trusted. */
static inline void fdebug_clear_rxstall(void) {
    PB_PIO->fdebug = (1u << PB_SM_RX);
}
static inline bool fdebug_rxstall(void) {
    return (PB_PIO->fdebug & (1u << PB_SM_RX)) != 0;
}

/* ── Continuous capture: self-refreshing DMA ──────────────────────────────
 *
 * Two channels are used:
 *   data     — PIO RX FIFO -> ring buffer, paced by DREQ, wrapping enabled.
 *              When it finishes it CHAINS to the control channel.
 *   control  — writes a single word: it puts the ring size into the TRIGGERING
 *              alias of the data channel's count register
 *              (al1_transfer_count_trig), so the data channel restarts
 *              immediately.
 *
 * The result is a capture that loops forever with no CPU involvement.
 * Because address wrapping brings the write pointer back to the start on its
 * own, the control channel does not need to reset the address as well. The
 * few clock cycles of gap between laps are covered many times over by the
 * PIO's RX FIFO (8 words when joined, about 333 us).
 */

static bool stream_configure(void) {
    dma_channel_config dc = dma_channel_get_default_config((uint)s_dma_data);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, PB_RING_ADDR_BITS);   /* write address wraps */
    channel_config_set_dreq(&dc, pio_get_dreq(PB_PIO, PB_SM_RX, false));
    channel_config_set_chain_to(&dc, (uint)s_dma_ctrl);
    dma_channel_configure((uint)s_dma_data, &dc,
                          (void *)s_ring,                  /* destination */
                          &PB_PIO->rxf[PB_SM_RX],          /* source      */
                          PB_RING_WORDS,
                          false);                          /* do not start yet */

    dma_channel_config cc = dma_channel_get_default_config((uint)s_dma_ctrl);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    channel_config_set_chain_to(&cc, (uint)s_dma_ctrl);     /* to itself = no chain */
    dma_channel_configure((uint)s_dma_ctrl, &cc,
                          &dma_hw->ch[s_dma_data].al1_transfer_count_trig,
                          &s_reload_words,
                          1,
                          false);
    return true;
}

/** Where DMA is currently writing in the ring (word index). */
static inline uint32_t ring_write_index(void) {
    uint32_t off = (uint32_t)((uintptr_t)dma_hw->ch[s_dma_data].write_addr -
                              (uintptr_t)s_ring);
    return (off >> 2) & PB_RING_MASK;
}

bool pb_audio_stream_start(void) {
    if (!s_rx_ready) return false;
    if (s_stream_running) return true;
    if (!stream_configure()) return false;

    /* Drop stale samples: the state machine has been running SINCE STARTUP,
     * so there may be words waiting in the FIFO. The state machine itself is
     * not stopped (see the note above). */
    while (!pio_sm_is_rx_fifo_empty(PB_PIO, PB_SM_RX)) {
        (void)pio_sm_get(PB_PIO, PB_SM_RX);
    }
    fdebug_clear_rxstall();

    s_read_idx = 0;                    /* writing also starts at the buffer head */
    dma_channel_start((uint)s_dma_data);
    s_stream_running = true;
    return true;
}

void pb_audio_stream_stop(void) {
    if (!s_stream_running) return;

    /* Break the chain FIRST. An aborted channel can still fire its chain, so
     * aborting while the chain stands would have the control channel restart
     * the data channel immediately and the stop would do nothing. al1_ctrl is
     * the non-triggering alias, safe to write while running. */
    uint32_t ctrl = dma_hw->ch[s_dma_data].al1_ctrl;
    ctrl &= ~DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS;
    ctrl |= ((uint32_t)s_dma_data << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
    dma_hw->ch[s_dma_data].al1_ctrl = ctrl;

    dma_channel_abort((uint)s_dma_data);
    dma_channel_abort((uint)s_dma_ctrl);
    s_stream_running = false;
}

void pb_audio_stream_flush(void) {
    if (!s_stream_running) return;
    s_read_idx = ring_write_index();
    fdebug_clear_rxstall();
}

uint32_t pb_audio_stream_available(void) {
    if (!s_stream_running) return 0;
    return (ring_write_index() - s_read_idx) & PB_RING_MASK;
}

pb_capture_result_t pb_audio_stream_read(int16_t *dst, uint32_t n_samples,
                                         uint32_t timeout_ms) {
    pb_capture_result_t res = { .samples = 0, .fifo_overrun = false, .timed_out = false };
    if (!s_stream_running || !dst || n_samples == 0 || n_samples > PB_AUDIO_MAX_READ) {
        res.timed_out = true;
        return res;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms ? timeout_ms : 1000);
    uint32_t got = 0;

    while (got < n_samples) {
        uint32_t avail = pb_audio_stream_available();

        if (avail == 0) {
            /* With no clock (the ES8311 not driving BCLK/LRCK) the ring
             * never fills, so we return an error rather than waiting
             * forever. */
            if (time_reached(deadline)) { res.timed_out = true; break; }
            tight_loop_contents();
            continue;
        }

        /* If the consumer has fallen three quarters of a ring behind, the
         * oldest samples are about to be overwritten: jump to the freshest
         * data and report it. Silently returning discontinuous data would
         * silently corrupt the measurement. */
        if (avail > (PB_RING_WORDS - PB_RING_WORDS / 4)) {
            res.fifo_overrun = true;
            s_read_idx = (ring_write_index() - PB_AUDIO_MAX_READ) & PB_RING_MASK;
            avail = PB_AUDIO_MAX_READ;
        }

        uint32_t take = n_samples - got;
        if (take > avail) take = avail;

        /* PIO sends one mono sample per frame; because the autopush
         * threshold is 16 bits, the sample sits in the word's low 16 bits. */
        for (uint32_t i = 0; i < take; i++) {
            dst[got + i] = (int16_t)(s_ring[(s_read_idx + i) & PB_RING_MASK] & 0xFFFFu);
        }
        s_read_idx = (s_read_idx + take) & PB_RING_MASK;
        got += take;
    }

    if (fdebug_rxstall()) {      /* DMA fell behind: the PIO FIFO overflowed */
        res.fifo_overrun = true;
        fdebug_clear_rxstall();
    }
    res.samples = got;
    return res;
}

pb_capture_result_t pb_audio_capture(int16_t *dst, uint32_t n_samples) {
    pb_capture_result_t res = { .samples = 0, .fifo_overrun = false, .timed_out = false };
    if (!dst || n_samples == 0) {
        res.timed_out = true;
        return res;
    }

    pb_audio_stream_flush();

    uint32_t done = 0;
    while (done < n_samples) {
        uint32_t want = n_samples - done;
        if (want > PB_AUDIO_MAX_READ) want = PB_AUDIO_MAX_READ;

        pb_capture_result_t part = pb_audio_stream_read(dst + done, want, 1000);
        res.samples      += part.samples;
        res.fifo_overrun |= part.fifo_overrun;
        res.timed_out    |= part.timed_out;
        done             += part.samples;

        if (part.timed_out) break;
    }
    return res;
}
