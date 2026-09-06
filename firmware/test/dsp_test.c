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

#include "ai/decision.h"
#include "dsp/fft.h"
#include "dsp/gate.h"
#include "dsp/mel.h"
#include "ui/text.h"

static int g_fail = 0;
static int g_run  = 0;

static void check(bool ok, const char *name, const char *detail) {
    g_run++;
    if (ok) {
        printf("  [gecti] %s\n", name);
    } else {
        g_fail++;
        printf("  [KALDI] %s — %s\n", name, detail);
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

    int peak = 0;
    for (int k = 1; k < PB_FFT_POWER_BINS; k++) {
        if (power[k] > power[peak]) peak = k;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "tepe bin %d, beklenen %d", peak, bin);
    check(peak == bin, "tepe dogru bin'de", msg);

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

    const float frequencies[] = { 500.0f, 1000.0f, 3000.0f, 8000.0f };
    int previous_peak = -1;
    bool artan = true;

    for (size_t i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); i++) {
        int16_t buf[PB_FFT_SIZE];
        sine(buf, PB_FFT_SIZE, frequencies[i], 0.9f);

        int8_t mel[PB_MEL_BANDS];
        pb_mel_frame(buf, mel);

        int peak = 0;
        for (int b = 1; b < PB_MEL_BANDS; b++) if (mel[b] > mel[peak]) peak = b;

        printf("        %6.0f Hz -> bant %2d (%.1f dB)\n",
               (double)frequencies[i], peak, (double)pb_mel_q_to_db(mel[peak]));

        if (previous_peak >= 0 && peak <= previous_peak) artan = false;
        previous_peak = peak;
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

    bool all_base = true;
    for (int b = 0; b < PB_MEL_BANDS; b++) if (mel[b] != -128) all_base = false;
    check(all_base, "sessizlik tabana oturuyor", "bazi bantlar taban degil");
}

/* ── Halka tamponu ve pencere normalizasyonu ─────────────────────────────── */
static void test_mel_window(void) {
    printf("Mel halka tamponu:\n");
    pb_mel_reset();

    int8_t *window = malloc((size_t)PB_MEL_BANDS * PB_MEL_FRAMES);
    check(!pb_mel_window(window), "dolmadan pencere vermiyor", "erken true dondu");

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
    check(pb_mel_window(window), "pencere hazir", "false dondu");

    /* Normalizasyon sonrası ortalama ~0 olmalı. */
    double total = 0.0;
    const int n = PB_MEL_BANDS * PB_MEL_FRAMES;
    for (int i = 0; i < n; i++) total += window[i];
    double avg = total / n;
    snprintf(msg, sizeof(msg), "ortalama %.2f", avg);
    check(fabs(avg) < 8.0, "normalize pencerenin ortalamasi ~0", msg);

    /* Halka gerçekten dönmeli: 187 kare daha itip taşmadığını görelim. */
    for (int f = 0; f < PB_MEL_FRAMES; f++) {
        sine(buf, PB_FFT_SIZE, 2000.0f, 0.5f);
        pb_mel_push(buf);
    }
    snprintf(msg, sizeof(msg), "kare sayisi %u", pb_mel_frame_count());
    check(pb_mel_frame_count() == 2 * PB_MEL_FRAMES, "halka donuyor", msg);

    free(window);
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
    snprintf(msg, sizeof(msg), "bant %.1f dB, taban %.1f dB, flux %.3f",
             (double)r.band_db, (double)r.floor_db, (double)r.flux);
    check(r.active, "ani ton kapiyi aciyor", msg);
    printf("        %s\n", msg);
}

/* ── Karar kuralı (ai/decision.c) ────────────────────────────────────────────
 * Ekranın zıplamaması bu kurala bağlı ve kuralın tamamı ZAMANA bağlı:
 * histerezis, tutma süresi, ses göstergesinin sönmesi. Kartta sınamak için
 * gerçek saniyeler beklemek gerekirdi; burada zamanı elle ilerletiyoruz. */
/** Yeni bir birleştirme sonucu gelmiş gibi bir adım ilerlet. */
static void result(pb_decision_t *k, uint32_t ms, int16_t cls, float p,
                  uint32_t merged) {
    pb_decision_input_t g = { .now_ms = ms, .gate_open = true,
                           .fresh_result = true, .cls = cls,
                           .probability = p, .merged = merged };
    pb_decision_update(k, &g);
}

/** Yeni sonuç YOKken bir adım ilerlet (zaman aşımları burada işliyor). */
static void empty(pb_decision_t *k, uint32_t ms, bool gate) {
    pb_decision_input_t g = { .now_ms = ms, .gate_open = gate,
                           .fresh_result = false, .cls = -1,
                           .probability = 0.0f, .merged = 0 };
    pb_decision_update(k, &g);
}

static void test_decision(void) {
    printf("Karar kurali (esik + histerezis + tutma):\n");
    pb_decision_t k;
    char msg[160];

    /* 1) Açılış: kapı hiç açılmadıysa "dinliyor". */
    pb_decision_reset(&k, 10000);
    empty(&k, 10000, false);
    check(k.mode == PB_DECISION_LISTENING && k.cls < 0,
          "acilista dinliyor", "baska bir kipte basladi");

    /* 2) Kapı açılınca ses göstergesi, kapanınca SES_TUT_MS sonra sönüyor. */
    empty(&k, 10100, true);
    const bool ses_yandi = (k.mode == PB_DECISION_SOUND);
    empty(&k, 10100 + PB_DECISION_SOUND_HOLD_MS + 1, false);
    check(ses_yandi && k.mode == PB_DECISION_LISTENING,
          "kapi acilip kapaninca ses gostergesi sonuyor",
          ses_yandi ? "sonmedi" : "hic yanmadi");

    /* 3) Çıkma eşiğiyle girme eşiği arasındaki güven TÜR yazdırmıyor. */
    pb_decision_reset(&k, 20000);
    result(&k, 20000, 5, 0.5f * (PB_DECISION_ENTER_THRESHOLD + PB_DECISION_EXIT_THRESHOLD),
          PB_DECISION_MIN_WINDOWS);
    check(k.mode == PB_DECISION_UNSURE && k.cls == 5,
          "esikler arasi guven = belirsiz", "kip yanlis");

    /* 4) Girme eşiği aşılınca TÜR. */
    result(&k, 21000, 5, PB_DECISION_ENTER_THRESHOLD + 0.05f, PB_DECISION_MIN_WINDOWS);
    check(k.mode == PB_DECISION_SPECIES && k.cls == 5, "girme esigi -> TUR",
          "TUR'a gecmedi");

    /* 5) HİSTEREZİS: güven girme eşiğinin ALTINA düşse de, çıkma eşiğinin
     *    üstünde kaldığı sürece aynı tür TUR olarak kalıyor. Ekranın
     *    zıplamamasını sağlayan madde bu. */
    for (uint32_t t = 22000; t <= 25000; t += 1000) {
        result(&k, t, 5, PB_DECISION_EXIT_THRESHOLD + 0.02f, PB_DECISION_MIN_WINDOWS);
    }
    snprintf(msg, sizeof(msg), "kip %d sinif %d", (int)k.mode, (int)k.cls);
    check(k.mode == PB_DECISION_SPECIES && k.cls == 5,
          "girme esiginin altinda ama cikma esiginin ustunde: TUR kaliyor", msg);

    /* 6) Başka bir tür ancak GİRME eşiğiyle yerini alabiliyor. */
    result(&k, 26000, 9, PB_DECISION_EXIT_THRESHOLD + 0.02f, PB_DECISION_MIN_WINDOWS);
    const bool degismedi = (k.cls == 5);
    result(&k, 27000, 9, PB_DECISION_ENTER_THRESHOLD + 0.05f, PB_DECISION_MIN_WINDOWS);
    check(degismedi && k.cls == 9,
          "yeni tur yalnizca girme esigiyle yer aliyor",
          degismedi ? "girme esiginde de degismedi" : "cikma esiginde degisti");

    /* 7) Destek kesilince TUT_MS boyunca ekranda kalıyor, sonra siliniyor. */
    empty(&k, 27000 + PB_DECISION_HOLD_MS - 100, false);
    const bool duruyor = (k.mode == PB_DECISION_SPECIES && k.cls == 9);
    empty(&k, 27000 + PB_DECISION_HOLD_MS + 100, false);
    snprintf(msg, sizeof(msg), "kip %d sinif %d", (int)k.mode, (int)k.cls);
    check(duruyor && k.mode == PB_DECISION_LISTENING && k.cls < 0,
          "destek kesilince tutma suresi sonunda siliniyor",
          duruyor ? msg : "tutma suresi dolmadan silindi");

    /* 8) Negatif sınıf hiçbir güvende tür adı yazdırmıyor — sahadaki en
     *    pahalı hata "gürültüyü kuş sanmak" (models/thresholds.txt). */
    pb_decision_reset(&k, 40000);
    result(&k, 40000, PB_DECISION_NEGATIVE_CLASS, 0.99f, PB_DECISION_MIN_WINDOWS);
    check(k.cls < 0 && k.mode != PB_DECISION_SPECIES,
          "negatif sinif ekrana tur yazdirmiyor", "negatif sinif gosterildi");

    /* 9) Yeterince pencere birleşmediyse karar yok: ölçüldü, 1 pencerede
     *    isabet %70,1, 3 pencerede %84,9 (models/thresholds.txt). */
    pb_decision_reset(&k, 50000);
    result(&k, 50000, 5, 0.95f, PB_DECISION_MIN_WINDOWS - 1);
    check(k.cls < 0, "az pencereyle karar verilmiyor", "erken karar verdi");
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

/**
 * Ham int16 dosyasından TAM bir model penceresi (64×187 int8) dök.
 *
 * Neden gerekli: `--dump` yalnızca tek kareyi karşılaştırıyor, yani filtre
 * bankasını ve dB→int8 eşlemesini doğruluyor. Modelin gerçek girdisi ise
 * `pb_mel_window()`'un çıktısı — kare dizilimi (hop 384) ve pencere içi
 * ortalama/varyans normalizasyonu buna ek olarak devreye giriyor. Eğitim
 * kümesini üreten Python bunları da birebir tutturmak zorunda; tutmazsa
 * model PC'de iyi cihazda kötü çalışır ve hiçbir yerde hata görünmez
 * (lastsession.md §9i).
 *
 * Girdi: 24 kHz mono, ham little-endian int16, en az 187*384+128 = 71.936
 * örnek. Kare r = örnek[r*384 .. r*384+512).
 */
static int dump_window(const char *yol) {
    enum { REQUIRED = (PB_MEL_FRAMES - 1) * PB_MEL_HOP + PB_FFT_SIZE };

    FILE *f = fopen(yol, "rb");
    if (!f) { fprintf(stderr, "acilamadi: %s\n", yol); return 2; }

    int16_t *s = (int16_t *)malloc(sizeof(int16_t) * REQUIRED);
    if (!s) { fclose(f); fprintf(stderr, "bellek yok\n"); return 2; }

    size_t okunan = fread(s, sizeof(int16_t), REQUIRED, f);
    fclose(f);
    if (okunan < REQUIRED) {
        fprintf(stderr, "kisa dosya: %zu ornek, gereken %d\n", okunan, REQUIRED);
        free(s);
        return 2;
    }

    pb_mel_reset();
    for (int r = 0; r < PB_MEL_FRAMES; r++) pb_mel_push(s + (size_t)r * PB_MEL_HOP);

    int8_t *window = (int8_t *)malloc((size_t)PB_MEL_FRAMES * PB_MEL_BANDS);
    if (!window || !pb_mel_window(window)) {
        fprintf(stderr, "pencere olusmadi\n");
        free(window); free(s);
        return 2;
    }

    printf("kare,bant,q\n");
    for (int r = 0; r < PB_MEL_FRAMES; r++) {
        for (int b = 0; b < PB_MEL_BANDS; b++) {
            printf("%d,%d,%d\n", r, b, window[(size_t)r * PB_MEL_BANDS + b]);
        }
    }
    free(window);
    free(s);
    return 0;
}

/* ── Uppercasing ─────────────────────────────────────────────
 *
 * WHY THIS IS TESTED: the UI shows species names in UPPER CASE. The
 * conversion is not plain toupper() — it has to leave multi-byte sequences
 * intact, and it must never cut one in half at the buffer edge.
 *
 * REGRESSION: this function used to implement Turkish casing, where `i`
 * uppercases to the dotted `İ`. Correct for Turkish, wrong for the English
 * species names the device now shows — it rendered "Common Nightingale" as
 * "COMMON NİGHTİNGALE". The first two cases below pin that down.
 */
static void expect_text(const char *input, const char *beklenen) {
    char out[64];
    pb_text_upper(input, out, sizeof(out));
    char detail[160];
    snprintf(detail, sizeof(detail), "\"%s\" -> \"%s\", expected \"%s\"",
             input, out, beklenen);
    check(strcmp(out, beklenen) == 0, input, detail);
}

static void test_text_upper(void) {
    printf("Uppercasing\n");

    /* ASCII `i` must stay a plain `I` — the regression described above. */
    expect_text("Common Nightingale", "COMMON NIGHTINGALE");
    expect_text("Eurasian Blackbird", "EURASIAN BLACKBIRD");

    /* Plain ASCII, and a hyphenated name. */
    expect_text("European Robin", "EUROPEAN ROBIN");
    expect_text("Greater White-fronted Goose", "GREATER WHITE-FRONTED GOOSE");

    /* Scientific names are shown as-is elsewhere, but must survive this. */
    expect_text("Anser albifrons", "ANSER ALBIFRONS");

    /* Multi-byte letters the fonts carry still uppercase correctly. */
    expect_text("çğöşü", "ÇĞÖŞÜ");
    expect_text("ı", "I");            /* dotless ı -> I: two bytes collapse to one */

    /* Edge cases. */
    expect_text("", "");

    /* Buffer overflow must not leave half a UTF-8 sequence behind.
     * A 5-byte buffer holds "Ç" (2) + "Ç" (2) + terminator = exactly full. */
    char small[5];
    pb_text_upper("çççç", small, sizeof(small));
    check(strcmp(small, "ÇÇ") == 0, "overflow leaves no partial UTF-8", small);

    /* NULL input must not crash; it yields an empty string. */
    char nl[8] = "xxx";
    pb_text_upper(NULL, nl, sizeof(nl));
    check(nl[0] == '\0', "NULL input yields empty string", nl);

    printf("\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump_frame();
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--pencere") == 0) {
        return dump_window(argv[2]);
    }

    printf("PokeBird DSP testleri\n=====================\n\n");
    test_fft_scale();
    test_mel_peak();
    test_mel_silence();
    test_mel_window();
    test_gate();
    test_decision();
    test_text_upper();

    printf("\n%d test, %d kaldi\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
