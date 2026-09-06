#include "dsp/gate.h"

#include <math.h>
#include <string.h>

#include "dsp/fft.h"
#include "dsp/mel.h"      /* PB_SAMPLE_RATE */

/* The band bounds as bin indices — all integer arithmetic, because the size
 * of s_prev has to be known at compile time. */
#define BIN_LO ((PB_GATE_F_LO * PB_FFT_SIZE) / PB_SAMPLE_RATE)
#define BIN_HI ((PB_GATE_F_HI * PB_FFT_SIZE) / PB_SAMPLE_RATE)

/* Floor tracking rates (dB per frame). With 16 ms frames:
 *   falling  0.50 dB/frame -> ~30 dB/s, settles onto silence quickly
 *   rising   0.02 dB/frame -> ~1.2 dB/s, a long call cannot drag the floor up
 * The asymmetry is deliberate; the reasoning is in gate.h. */
#define FLOOR_DOWN_DB 0.50f
#define FLOOR_UP_DB   0.02f

/* Gate threshold: how many dB above the floor. 6 dB is a factor of four in
 * energy. */
#define TRIGGER_DB    6.0f

/* Flux threshold. It is computed over the normalised band shape, so it is
 * dimensionless; 0.15 sits well above the noise on quiet recordings. */
#define TRIGGER_FLUX  0.15f

#define NBANDS (BIN_HI - BIN_LO + 1)

static float s_floor_db;
static bool  s_have_floor;
static float s_prev[NBANDS];
static bool  s_have_prev;

void pb_gate_reset(void) {
    s_floor_db = 0.0f;
    /* A separate flag: 0 dB is a legitimate value and cannot serve as a
     * sentinel. */
    s_have_floor = false;
    s_have_prev = false;
    memset(s_prev, 0, sizeof(s_prev));
}

pb_gate_result_t pb_gate_update(const float *power) {
    pb_gate_result_t r;
    memset(&r, 0, sizeof(r));

    /* Band energy */
    float total = 0.0f;
    for (int k = BIN_LO; k <= BIN_HI; k++) total += power[k];
    r.band_db = 10.0f * log10f(total + 1e-12f);

    /* Spectral flux: the band shape is normalised and the POSITIVE
     * differences between consecutive frames are summed. Looking only at
     * increases matters — sound stopping also produces a large difference,
     * but what we care about is the onset.
     *
     * The shape vector is UPDATED ON EVERY FRAME, silent ones included. It
     * used to be updated only when there was energy, which meant the first
     * loud frame after a silence had no previous shape to compare against and
     * the flux came out zero — so the gate missed exactly the moment it was
     * supposed to catch. A host test ("a sudden tone opens the gate") caught
     * this.
     *
     * During silence the shape is taken to be a flat distribution, so when
     * energy gathers into one band the flux naturally rises. */
    float current[NBANDS];
    const float flat = 1.0f / (float)NBANDS;
    if (total > 1e-12f) {
        const float inv = 1.0f / total;
        for (int i = 0; i < NBANDS; i++) current[i] = power[BIN_LO + i] * inv;
    } else {
        for (int i = 0; i < NBANDS; i++) current[i] = flat;
    }

    float flux = 0.0f;
    if (s_have_prev) {
        for (int i = 0; i < NBANDS; i++) {
            float d = current[i] - s_prev[i];
            if (d > 0.0f) flux += d;
        }
    }
    memcpy(s_prev, current, sizeof(s_prev));
    s_have_prev = true;
    r.flux = flux;

    /* Adaptive floor */
    if (!s_have_floor) {
        s_floor_db = r.band_db;          /* first frame: settle immediately */
        s_have_floor = true;
    } else if (r.band_db < s_floor_db) {
        s_floor_db -= FLOOR_DOWN_DB;
        if (s_floor_db < r.band_db) s_floor_db = r.band_db;
    } else {
        s_floor_db += FLOOR_UP_DB;
        if (s_floor_db > r.band_db) s_floor_db = r.band_db;
    }
    r.floor_db = s_floor_db;

    /* Both criteria are required: a steady drone raises the energy but not
     * the flux, while birdsong raises both. */
    r.active = (r.band_db > s_floor_db + TRIGGER_DB) && (r.flux > TRIGGER_FLUX);
    return r;
}
