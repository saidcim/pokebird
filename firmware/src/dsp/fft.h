/**
 * fft.h — a simple real-valued FFT and spectrum helpers
 *
 * This is a plain, dependency-free radix-2 implementation, written so the
 * live on-screen spectrogram could be verified on its own. Speed is not
 * critical here. It could be replaced with CMSIS-DSP's arm_rfft_fast_f32.
 */
#ifndef POKEBIRD_FFT_H
#define POKEBIRD_FFT_H

#include <stdint.h>

#define PB_FFT_SIZE 512
#define PB_FFT_BINS (PB_FFT_SIZE / 2)
/** Unique bins for a real input: 0..N/2 inclusive. */
#define PB_FFT_POWER_BINS (PB_FFT_SIZE / 2 + 1)

/**
 * Hann-windowed power spectrum.
 *
 * Both the live spectrogram and the mel feature pipeline use this, so there
 * is a single source and the two see the same window and the same DC removal.
 *
 * Scaling: the input is divided by full scale (32768), then normalised by the
 * window's coherent gain. A full-scale sine therefore gives a power close to
 * 1.0 in its own bin, which makes the dBFS readings meaningful.
 *
 * @param samples PB_FFT_SIZE int16 values
 * @param power   output, PB_FFT_POWER_BINS values
 */
void pb_fft_power(const int16_t *samples, float *power);

/**
 * Produce a log-scaled magnitude spectrum from `PB_FFT_SIZE` int16 samples.
 *
 * @param samples  input, PB_FFT_SIZE values
 * @param out      output, n_out values, 0..255
 * @param n_out    number of bands wanted (bins are grouped logarithmically)
 * @param floor_db noise floor (dBFS); anything below it clips to 0
 */
void pb_fft_spectrum(const int16_t *samples, uint8_t *out, uint32_t n_out,
                     float floor_db);

#endif /* POKEBIRD_FFT_H */
