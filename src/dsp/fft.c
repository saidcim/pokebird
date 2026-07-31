#include "dsp/fft.h"

#include <math.h>
#include <string.h>

static float s_re[PB_FFT_SIZE];
static float s_im[PB_FFT_SIZE];
static float s_window[PB_FFT_SIZE];
static int   s_window_ready = 0;

/* Yerinde radix-2 karmaşık FFT. Kısa ve doğrulanabilir olması için sadelik
 * tercih edildi; M3'te CMSIS-DSP devralacak. */
static void fft_inplace(float *re, float *im, uint32_t n) {
    /* bit-ters permütasyon */
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

void pb_fft_spectrum(const int16_t *samples, uint8_t *out, uint32_t n_out,
                     float floor_db) {
    if (!s_window_ready) {
        for (uint32_t i = 0; i < PB_FFT_SIZE; i++) {
            s_window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i /
                                             (float)(PB_FFT_SIZE - 1));
        }
        s_window_ready = 1;
    }

    /* DC'yi çıkar — mikrofonun sabit bir ofseti var (M1'de ~15 LSB ölçüldü)
     * ve çıkarılmazsa bin 0'da yapay bir tepe oluşturuyor. */
    float mean = 0.0f;
    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) mean += (float)samples[i];
    mean /= (float)PB_FFT_SIZE;

    for (uint32_t i = 0; i < PB_FFT_SIZE; i++) {
        s_re[i] = ((float)samples[i] - mean) * s_window[i];
        s_im[i] = 0.0f;
    }
    fft_inplace(s_re, s_im, PB_FFT_SIZE);

    /* Bin'leri logaritmik grupla: kuş sesinde ilgi çekici detay geniş bir
     * frekans aralığına yayılıyor, doğrusal gruplama tizleri eziyor. */
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
