#include "dsp/mel.h"

#include <math.h>
#include <string.h>

#include "dsp/fft.h"

/* ── Filtre bankası ───────────────────────────────────────────────────────
 * Üçgen filtreler seyrek saklanıyor: her filtre için başlangıç bin'i ve
 * ağırlık dizisi. 64 filtre 257 bin'e yayıldığında sıfır olmayan ağırlık
 * sayısı ~550; tam 64×257'lik matris 66 KB ederdi, seyrek hâli ~2 KB. */
#define PB_MEL_WEIGHTS_MAX 768

typedef struct {
    uint16_t start;   /* ilk bin */
    uint16_t count;   /* ağırlık sayısı */
    uint16_t offset;  /* s_weights içindeki konum */
} mel_filter_t;

static mel_filter_t s_filters[PB_MEL_BANDS];
static float        s_weights[PB_MEL_WEIGHTS_MAX];
static uint16_t     s_weight_used = 0;
static bool         s_inited = false;

/* HTK mel ölçeği. librosa'da htk=True karşılığı. */
static float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float mel_to_hz(float m)  { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

void pb_mel_init(void) {
    if (s_inited) return;

    /* Mel ekseninde eşit aralıklı BANDS+2 nokta; her üçlü bir üçgen. */
    float pts[PB_MEL_BANDS + 2];
    const float m0 = hz_to_mel(PB_MEL_FMIN);
    const float m1 = hz_to_mel(PB_MEL_FMAX);
    for (int i = 0; i < PB_MEL_BANDS + 2; i++) {
        float m = m0 + (m1 - m0) * (float)i / (float)(PB_MEL_BANDS + 1);
        /* Hz -> kesirli FFT bin'i */
        pts[i] = mel_to_hz(m) * (float)PB_FFT_SIZE / (float)PB_SAMPLE_RATE;
    }

    s_weight_used = 0;
    for (int f = 0; f < PB_MEL_BANDS; f++) {
        const float left = pts[f], center = pts[f + 1], right = pts[f + 2];

        int k0 = (int)ceilf(left);
        int k1 = (int)floorf(right);
        if (k0 < 0) k0 = 0;
        if (k1 > PB_FFT_POWER_BINS - 1) k1 = PB_FFT_POWER_BINS - 1;
        if (k1 < k0) k1 = k0 - 1;                 /* boş filtre */

        s_filters[f].start  = (uint16_t)k0;
        s_filters[f].offset = s_weight_used;
        s_filters[f].count  = 0;

        for (int k = k0; k <= k1; k++) {
            float w;
            if ((float)k <= center) {
                w = (center > left) ? ((float)k - left) / (center - left) : 0.0f;
            } else {
                w = (right > center) ? (right - (float)k) / (right - center) : 0.0f;
            }
            if (w < 0.0f) w = 0.0f;
            if (s_weight_used < PB_MEL_WEIGHTS_MAX) {
                s_weights[s_weight_used++] = w;
                s_filters[f].count++;
            }
        }
    }
    s_inited = true;
}

/* dB <-> int8 doğrusal eşleme. Aralık mel.h'de gerekçesiyle birlikte. */
static int8_t db_to_q(float db) {
    if (db < PB_MEL_DB_MIN) db = PB_MEL_DB_MIN;
    if (db > PB_MEL_DB_MAX) db = PB_MEL_DB_MAX;
    float t = (db - PB_MEL_DB_MIN) / (PB_MEL_DB_MAX - PB_MEL_DB_MIN);  /* 0..1 */
    int v = (int)lrintf(t * 255.0f) - 128;
    if (v < -128) v = -128;
    if (v > 127) v = 127;
    return (int8_t)v;
}

float pb_mel_q_to_db(int8_t q) {
    float t = ((float)q + 128.0f) / 255.0f;
    return PB_MEL_DB_MIN + t * (PB_MEL_DB_MAX - PB_MEL_DB_MIN);
}

void pb_mel_frame(const int16_t *samples, int8_t *out) {
    pb_mel_init();

    float power[PB_FFT_POWER_BINS];
    pb_fft_power(samples, power);

    for (int f = 0; f < PB_MEL_BANDS; f++) {
        const mel_filter_t *flt = &s_filters[f];
        const float *w = &s_weights[flt->offset];
        float acc = 0.0f;
        for (uint16_t i = 0; i < flt->count; i++) {
            acc += w[i] * power[flt->start + i];
        }
        /* log10'un sıfırda patlamaması için taban ekleniyor; taban değeri
         * PB_MEL_DB_MIN'in biraz altına denk geliyor ki kırpma orada olsun. */
        float db = 10.0f * log10f(acc + 1e-10f);
        out[f] = db_to_q(db);
    }
}

/* ── Halka tamponu ────────────────────────────────────────────────────────
 * 64×187 int8 = 11.7 KB. Yazma konumu döner; okurken en eskiden en yeniye
 * sırayla kopyalanıyor. */
static int8_t   s_ring[PB_MEL_FRAMES][PB_MEL_BANDS];
static uint32_t s_write = 0;      /* sıradaki yazma satırı */
static uint32_t s_total = 0;      /* toplam itilen kare */

void pb_mel_reset(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_write = 0;
    s_total = 0;
}

uint32_t pb_mel_frame_count(void) { return s_total; }

bool pb_mel_last_frame(int8_t *out) {
    if (s_total == 0 || !out) return false;
    const uint32_t son = (s_write + PB_MEL_FRAMES - 1) % PB_MEL_FRAMES;
    memcpy(out, s_ring[son], PB_MEL_BANDS);
    return true;
}

void pb_mel_push(const int16_t *samples) {
    pb_mel_frame(samples, s_ring[s_write]);
    s_write = (s_write + 1) % PB_MEL_FRAMES;
    s_total++;
}

bool pb_mel_window(int8_t *out) {
    if (s_total < PB_MEL_FRAMES) return false;

    /* En eski kare, bir sonraki yazılacak satırdır. */
    const uint32_t ilk = s_write;

    /* Pencere içi ortalama ve standart sapma — dB alanında. */
    float toplam = 0.0f, toplam_kare = 0.0f;
    for (uint32_t r = 0; r < PB_MEL_FRAMES; r++) {
        const int8_t *satir = s_ring[(ilk + r) % PB_MEL_FRAMES];
        for (int b = 0; b < PB_MEL_BANDS; b++) {
            float db = pb_mel_q_to_db(satir[b]);
            toplam += db;
            toplam_kare += db * db;
        }
    }
    const float n = (float)(PB_MEL_FRAMES * PB_MEL_BANDS);
    const float ort = toplam / n;
    float var = toplam_kare / n - ort * ort;
    if (var < 1e-6f) var = 1e-6f;
    const float std = sqrtf(var);

    /* ±4 sigma'yı int8'in tamamına yay. Kırpma nadir ama zararsız: 4 sigma
     * dışındaki değerler zaten aşırı uçlar. */
    const float olcek = 127.0f / (4.0f * std);

    for (uint32_t r = 0; r < PB_MEL_FRAMES; r++) {
        const int8_t *satir = s_ring[(ilk + r) % PB_MEL_FRAMES];
        int8_t *hedef = out + (size_t)r * PB_MEL_BANDS;
        for (int b = 0; b < PB_MEL_BANDS; b++) {
            float z = (pb_mel_q_to_db(satir[b]) - ort) * olcek;
            int v = (int)lrintf(z);
            if (v < -128) v = -128;
            if (v > 127) v = 127;
            hedef[b] = (int8_t)v;
        }
    }
    return true;
}
