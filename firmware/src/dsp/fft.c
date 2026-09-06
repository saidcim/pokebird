#include "dsp/fft.h"

#include <math.h>
#include <string.h>

static float s_re[PB_FFT_SIZE];
static float s_im[PB_FFT_SIZE];
static float s_window[PB_FFT_SIZE];
static int   s_window_ready = 0;

/* In-place radix-2 complex FFT. Kept simple so it stays short and
 * tercih edildi; M3'te CMSIS-DSP devralacak. */
static void fft_inplace(float *re, float *im, uint32_t n) {
    /* bit-reversal permutation */
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (uint32_t i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (uint32_t k = 0; k < len / 2; k++) {
                float ur = re[i + k],           ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;  im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;  im[i + k + len / 2] = ui - vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* Hann window — periodic (divided by N), NOT symmetric (N-1).
 * The periodic form is the correct one for spectral analysis with overlapping
 * frames, and it is what the Python side defaults to (scipy/librosa
 * `sym=False`). Since we compare against that reference, the distinction
 * matters. */
static void window_ensure(void) {
    if (s_window_ready) return;
    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) {
        s_window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i /
                                         (float)PB_FFT_SIZE);
    }
    s_window_ready = 1;
}

void pb_fft_power(const int16_t *samples, float *power) {
    window_ensure();

    /* Remove DC — the microphone has a constant offset (measured at about
     * 15 LSB), and left in it produces an artificial peak in bin 0. */
    float mean = 0.0f;
    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) mean += (float)samples[i];
    mean /= (float)PB_FFT_SIZE;

    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) {
        s_re[i] = (((float)samples[i] - mean) / 32768.0f) * s_window[i];
        s_im[i] = 0.0f;
    }
    fft_inplace(s_re, s_im, PB_FFT_SIZE);

    /* Normalise by the window's coherent gain, so a full-scale sine gives a
     * power of 1.0 in its own bin, i.e. 0 dBFS.
     *
     * Derivation: for x[n] = A*sin(...) windowed with Hann, the magnitude in
     * its own bin is |X[k]| ~= A*N/4, so the power is A^2*N^2/16. Bringing
     * that to A^2 needs 16/N^2, which written in terms of the gain
     * sum(w)/N = 0.5 is 4/gain^2.
     *
     * (It was first written as 2/gain^2, which gives the sine's mean square,
     * A^2/2, so a full-scale sine read as -3 dBFS. A host test caught it.) */
    const float gain = 0.5f * (float)PB_FFT_SIZE;
    const float scale = 4.0f / (gain * gain);

    for (uint32_t k = 0; k < PB_FFT_POWER_BINS; k++) {
        float p = s_re[k] * s_re[k] + s_im[k] * s_im[k];
        /* DC and Nyquist have no mirror partner, so they are not doubled. */
        power[k] = (k == 0 || k == PB_FFT_SIZE / 2) ? p * (scale * 0.5f)
                                                    : p * scale;
    }
}

void pb_fft_spectrum(const int16_t *samples, uint8_t *out, uint32_t n_out,
                     float floor_db) {
    window_ensure();

    float mean = 0.0f;
    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) mean += (float)samples[i];
    mean /= (float)PB_FFT_SIZE;

    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) {
        s_re[i] = ((float)samples[i] - mean) * s_window[i];
        s_im[i] = 0.0f;
    }
    fft_inplace(s_re, s_im, PB_FFT_SIZE);

    /* Group the bins logarithmically: in birdsong the interesting detail is
     * spread over a wide frequency range, and linear grouping crushes the
     * high end. */
    const float span = 1.0f / (float)n_out;
    for (uint32_t b = 0; b < n_out; b++) {
        float f0 = powf((float)PB_FFT_BINS, (float)b * span);
        float f1 = powf((float)PB_FFT_BINS, (float)(b + 1) * span);
        uint32_t k0 = (uint32_t)f0;
        uint32_t k1 = (uint32_t)f1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > PB_FFT_BINS) k1 = PB_FFT_BINS;

        float peak = 0.0f;
        for (uint32_t k = k0; k < k1; k++) {
            float m = s_re[k] * s_re[k] + s_im[k] * s_im[k];
            if (m > peak) peak = m;
        }
        float db = (peak > 0.0f)
                     ? 10.0f * log10f(peak / ((float)PB_FFT_SIZE * 32768.0f * 32768.0f))
                     : -200.0f;

        float norm = (db - floor_db) / (0.0f - floor_db);
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        out[b] = (uint8_t)(norm * 255.0f);
    }
}
