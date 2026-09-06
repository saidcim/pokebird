/**
 * gate.h — stage 0: the cheap "is anything worth listening to" gate
 *
 * In a city the device hears nothing interesting for the vast majority of the
 * time. Running the expensive work (mel plus two neural nets) on every frame
 * would waste both battery and CPU. This gate looks at two cheap criteria:
 *
 *   1. The energy in the 2-10 kHz band, relative to an ADAPTIVE noise floor.
 *      A fixed threshold does not work: room, street and park noise differ by
 *      tens of dB (a room floor of -36 dBFS was measured early on, fan
 *      included).
 *   2. Spectral flux — how fast the shape of the band is changing. A steady
 *      drone (air conditioning, traffic) raises the energy but not the flux;
 *      birdsong raises both.
 *
 * The floor is tracked with one-sided rates: it falls quickly towards silence
 * and rises slowly towards noise. The other way round, a long call would drag
 * the floor up onto itself and hide the bird from the gate.
 */
#ifndef POKEBIRD_GATE_H
#define POKEBIRD_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* Integers: the bin bounds must be compile-time constants, otherwise the
 * band buffer becomes "variably modified at file scope" and cannot be
 * allocated statically. */
#define PB_GATE_F_LO 2000
#define PB_GATE_F_HI 10000

typedef struct {
    bool  active;      /**< is the gate open */
    float band_db;     /**< band energy on this frame (dBFS) */
    float floor_db;    /**< the tracked noise floor (dBFS) */
    float flux;        /**< spectral flux (0..) */
} pb_gate_result_t;

/** Reset the state. The floor catches up quickly over the first frames. */
void pb_gate_reset(void);

/**
 * Evaluate one frame.
 * @param power the output of pb_fft_power(), PB_FFT_POWER_BINS values
 */
pb_gate_result_t pb_gate_update(const float *power);

#endif /* POKEBIRD_GATE_H */
