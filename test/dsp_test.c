/**
 * dsp_test.c — DSP hattının host tarafı testleri
 *
 * Plan §11: "DSP fonksiyonları PC'de derlenip Python referansına karşı
 * karşılaştırılır. Mikrodenetleyicide DSP hatası ayıklamak çok pahalı."
 *
 * İki kip:
 *   dsp_test           testleri çalıştır, geçti/kaldı bas
 *   dsp_test --dump    tek bir mel karesini CSV olarak bas (Python ile
 *                      karşılaştırmak için; bkz. tools/mel_reference.py)
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/fft.h"
#include "dsp/gate.h"
#include "dsp/mel.h"

static int g_fail = 0;
static int g_run  = 0;

static void check(bool ok, const char *ad, const char *ayrinti) {
    g_run++;
    if (ok) {
        printf("  [gecti] %s\n", ad);
    } else {
        g_fail++;
        printf("  [KALDI] %s — %s\n", ad, ayrinti);
    }
}

/** Tam ölçeğe göre `amp` genlikli sinüs üret. */
static void sine(int16_t *out, int n, float hz, float amp) {
    for (int i = 0; i < n; i++) {
        float v = amp * 32767.0f * sinf(2.0f * (float)M_PI * hz * (float)i /
                                        (float)PB_SAMPLE_RATE);
        out[i] = (int16_t)lrintf(v);
    }
}

/* ── FFT ölçeklemesi ──────────────────────────────────────────────────────
 * Bin merkezine oturan tam ölçekli sinüs, kendi bin'inde ~1.0 güç vermeli.
 * Bu tutmazsa bütün dB okumaları kayar ve mel de kayar. */
static void test_fft_scale(void) {
    printf("FFT olcekleme:\n");

    const int bin = 64;
    const float hz = (float)bin * PB_SAMPLE_RATE / PB_FFT_SIZE;
    int16_t buf[PB_FFT_SIZE];
    sine(buf, PB_FFT_SIZE, hz, 1.0f);

    float power[PB_FFT_POWER_BINS];
    pb_fft_power(buf, power);

    int tepe = 0;
    for (int k = 1; k < PB_FFT_POWER_BINS; k++) {
        if (power[k] > power[tepe]) tepe = k;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "tepe bin %d, beklenen %d", tepe, bin);
    check(tepe == bin, "tepe dogru bin'de", msg);

    snprintf(msg, sizeof(msg), "guc %.4f, beklenen ~1.0", (double)power[bin]);
    check(power[bin] > 0.85f && power[bin] < 1.15f, "tam olcek sinus ~1.0 guc", msg);

    /* Yarı genlik -> -6 dB */
    sine(buf, PB_FFT_SIZE, hz, 0.5f);
    pb_fft_power(buf, power);
    float db = 10.0f * log10f(power[bin]);
    snprintf(msg, sizeof(msg), "%.2f dB, beklenen ~-6", (double)db);
    check(db > -7.0f && db < -5.0f, "yari genlik -6 dB", msg);
}

/* ── Mel filtre bankası ───────────────────────────────────────────────────
 * Bant merkezlerinin frekansla artması ve tepe bandın giriş frekansını
 * takip etmesi gerekiyor. */
static void test_mel_peak(void) {
    printf("Mel bant esleme:\n");
    pb_mel_init();

    const float frekanslar[] = { 500.0f, 1000.0f, 3000.0f, 8000.0f };
    int onceki_tepe = -1;
    bool artan = true;

    for (size_t i = 0; i < sizeof(frekanslar) / sizeof(frekanslar[0]); i++) {
        int16_t buf[PB_FFT_SIZE];
        sine(buf, PB_FFT_SIZE, frekanslar[i], 0.9f);

        int8_t mel[PB_MEL_BANDS];
        pb_mel_frame(buf, mel);

        int tepe = 0;
        for (int b = 1; b < PB_MEL_BANDS; b++) if (mel[b] > mel[tepe]) tepe = b;

        printf("        %6.0f Hz -> bant %2d (%.1f dB)\n",
               (double)frekanslar[i], tepe, (double)pb_mel_q_to_db(mel[tepe]));

        if (onceki_tepe >= 0 && tepe <= onceki_tepe) artan = false;
        onceki_tepe = tepe;
    }
    check(artan, "tepe bant frekansla artiyor", "monotonluk bozuldu");
}

/* Sessizlik tabana oturmalı; aksi hâlde kapı sürekli tetiklenir. */
static void test_mel_silence(void) {
    printf("Mel sessizlik:\n");
    int16_t buf[PB_FFT_SIZE];
    memset(buf, 0, sizeof(buf));

    int8_t mel[PB_MEL_BANDS];
    pb_mel_frame(buf, mel);

    bool hepsi_taban = true;
    for (int b = 0; b < PB_MEL_BANDS; b++) if (mel[b] != -128) hepsi_taban = false;
    check(hepsi_taban, "sessizlik tabana oturuyor", "bazi bantlar taban degil");
}

/* ── Halka tamponu ve pencere normalizasyonu ─────────────────────────────── */
static void test_mel_window(void) {
    printf("Mel halka tamponu:\n");
    pb_mel_reset();

    int8_t *pencere = malloc((size_t)PB_MEL_BANDS * PB_MEL_FRAMES);
    check(!pb_mel_window(pencere), "dolmadan pencere vermiyor", "erken true dondu");

    int16_t buf[PB_FFT_SIZE];
    for (int f = 0; f < PB_MEL_FRAMES; f++) {
        /* Kareden kareye değişen bir ton: normalizasyon sabit girdide
         * anlamsız olurdu (varyans sıfır). */
        sine(buf, PB_FFT_SIZE, 1000.0f + 20.0f * (float)f, 0.5f);
        pb_mel_push(buf);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "kare sayisi %u", pb_mel_frame_count());
    check(pb_mel_frame_count() == PB_MEL_FRAMES, "187 kare biriktti", msg);
    check(pb_mel_window(pencere), "pencere hazir", "false dondu");

    /* Normalizasyon sonrası ortalama ~0 olmalı. */
    double toplam = 0.0;
    const int n = PB_MEL_BANDS * PB_MEL_FRAMES;
    for (int i = 0; i < n; i++) toplam += pencere[i];
    double ort = toplam / n;
    snprintf(msg, sizeof(msg), "ortalama %.2f", ort);
    check(fabs(ort) < 8.0, "normalize pencerenin ortalamasi ~0", msg);

    /* Halka gerçekten dönmeli: 187 kare daha itip taşmadığını görelim. */
    for (int f = 0; f < PB_MEL_FRAMES; f++) {
        sine(buf, PB_FFT_SIZE, 2000.0f, 0.5f);
        pb_mel_push(buf);
    }
    snprintf(msg, sizeof(msg), "kare sayisi %u", pb_mel_frame_count());
    check(pb_mel_frame_count() == 2 * PB_MEL_FRAMES, "halka donuyor", msg);

    free(pencere);
}

/* ── Kapı ─────────────────────────────────────────────────────────────────
 * Sessizlik ve sabit uğultu kapıyı AÇMAMALI; ani bir ton AÇMALI. */
static void test_gate(void) {
    printf("Kapi (VAD):\n");
    int16_t buf[PB_FFT_SIZE];
    float power[PB_FFT_POWER_BINS];

    /* 1) Sessizlik */
    pb_gate_reset();
    bool sessizlikte_acildi = false;
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 40; i++) {
        pb_fft_power(buf, power);
        if (pb_gate_update(power).active) sessizlikte_acildi = true;
    }
    check(!sessizlikte_acildi, "sessizlikte acilmiyor", "kapi tetiklendi");

    /* 2) Sabit ugultu: enerji yuksek ama sekli degismiyor */
    pb_gate_reset();
    bool ugultuda_acildi = false;
    for (int i = 0; i < 60; i++) {
        sine(buf, PB_FFT_SIZE, 5000.0f, 0.3f);
        pb_fft_power(buf, power);
        pb_gate_result_t r = pb_gate_update(power);
        if (i > 20 && r.active) ugultuda_acildi = true;   /* taban otursun */
    }
    check(!ugultuda_acildi, "sabit ugultuda acilmiyor", "kapi tetiklendi");

    /* 3) Sessizlikten sonra ani ton */
    pb_gate_reset();
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 40; i++) { pb_fft_power(buf, power); pb_gate_update(power); }

    sine(buf, PB_FFT_SIZE, 4000.0f, 0.5f);
    pb_fft_power(buf, power);
    pb_gate_result_t r = pb_gate_update(power);
    char msg[160];
    snprintf(msg, sizeof(msg), "bant %.1f dB, taban %.1f dB, aki %.3f",
             (double)r.band_db, (double)r.floor_db, (double)r.flux);
    check(r.active, "ani ton kapiyi aciyor", msg);
    printf("        %s\n", msg);
}

/** Python referansıyla karşılaştırmak için tek kare CSV. */
static void dump_frame(void) {
    int16_t buf[PB_FFT_SIZE];
    /* Belirlenimci, tekrar üretilebilir bir girdi: iki ton + testere. */
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

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump_frame();
        return 0;
    }

    printf("PokeBird DSP testleri\n=====================\n\n");
    test_fft_scale();
    test_mel_peak();
    test_mel_silence();
    test_mel_window();
    test_gate();

    printf("\n%d test, %d kaldi\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
