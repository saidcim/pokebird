/**
 * tanima.c — Gerçek zamanlı tanıma hattı, core 1'de
 *
 * Döngü, `m` komutundaki mel hattının aynısı; üzerine iki şey ekliyor:
 * kapı açıksa saniyede bir çıkarım, ve son 8 pencerenin birleştirilmesi.
 *
 * ÖLÇÜLMÜŞ ZAMANLAMA (kart, 2 Ağustos 2026):
 *   mel karesi      16 ms'de bir gelir (hop 384 @ 24 kHz = 62,5 kare/s)
 *   tür ağı Invoke  190 ms  ← bu süre boyunca halka HİÇ okunmuyor
 *   ses halkası     341 ms (8192 örnek), taşma eşiği 3/4 = 256 ms
 *
 * Yani çıkarım sırasında biriken ses kaybolmuyor; çıkarımdan sonra core 1
 * birikmiş kareleri gerçek zamandan hızlı işleyip açığı kapatıyor. Halka
 * M6'da tam bunun için 4096'dan 8192'ye çıkarıldı (audio_i2s.h).
 */
#include "ai/recognizer.h"

#include <math.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "ai/binary_net.h"
#include "dsp/fft.h"
#include "dsp/gate.h"
#include "dsp/mel.h"
#include "hal/audio_i2s.h"

/* Aşama-1 ikili ağın karar eşiği. tools/train_binary.py'de ÖLÇÜLDÜ: eşik 0.5'te
 * test kümesinde kuş-geri-çağırma %97,90, negatif-özgüllük %86,58 (§9o).
 * Kaçırma (yanlış "değil") pahalı — gerçek bir tespiti sessizce kaybediyor;
 * geçirme (yanlış "kuş") ucuz — tür ağı zaten kendi negatif sınıfıyla eliyor.
 * Bu yüzden eşik 0.5'ten AŞAĞI çekilmedi: ölçülen nokta zaten geri-çağırmayı
 * önceliklendiriyor (model seçimi de bu ölçütle yapıldı, bkz. ikili_egit.py). */
#define BINARY_THRESHOLD  0.5f

/* Çıkarım adımı: kaç mel karesinde bir pencere değerlendirilsin.
 * 63 kare × 16 ms = 1,008 s — plandaki 1 saniyelik pencere adımı (§7). */
#define STEP_FRAME   63

/* Kapı eşiği: son ADIM_KARE karenin en az bu kadarında kapı açık olmalı.
 *
 * Amaç ağır işi zamanın %90+'ında hiç çalıştırmamak (plan §3). Sessiz odada
 * kapı zaten karelerin %2–3'ünde açılıyor (§9c ölçümü), yani 5/63 = %8 eşiği
 * sessizliği rahatça eliyor. Kuş ötüşü saniyelerce sürdüğü için gerçek bir
 * ötüşte bu eşik kolayca aşılıyor.
 *
 * ⚠ BU EŞİK SAHADA KALİBRE EDİLMELİ (M8). Şu anki değeri sessiz oda
 * ölçümünden türetildi; şehir gürültüsünde kapı çok daha sık açılacak ve
 * eşik seçiciliğini kaybedecek. O zaman işi Aşama-1 ikili ağı devralacak. */
#define GATE_THRESHOLD   5

/* Birleştirme belleğinin bayatlama süresi. Bu kadar süre çıkarım yapılmazsa
 * (ortam sessizleşti) birikmiş olasılıklar atılıyor: 30 saniye önceki bir
 * ötüşü şimdiki sonuca karıştırmak yanlış olur. */
#define STALE_MS    6000

/* ── Paylaşılan durum ──────────────────────────────────────────────────────
 * Core 1 yazar, core 0 okur. Kilit yok: `surum` alanı en SON yazılıyor ve
 * core 0 onu okuduktan sonra gövdeyi kopyalıyor; kopyaladıktan sonra sürümü
 * tekrar kontrol ediyor. Yırtık okuma olursa tekrar deniyor. Tek yazar
 * olduğu için bu yeterli — spinlock'a gerek yok. */
static volatile pb_recognizer_state_t s_state;
static volatile bool s_run = false;
static volatile bool s_gate_ignore = false;

/* Birleştirme halkası: son 8 pencerenin softmax olasılıkları.
 * 8 × 179 × 4 = 5.728 bayt. */
static float s_probability[PB_VOTE_WINDOWS][PB_SPECIES_NET_CLASSES];
static uint32_t s_probability_write = 0;
static uint32_t s_probability_count = 0;

/* ── Spektrogram sütunu kuyruğu (core 1 -> core 0) ────────────────────────
 * Tek yazar / tek okuyucu halka. Kilit yok: yazar yalnızca `yaz`ı, okuyucu
 * yalnızca `oku`yu ilerletiyor; iki indeks arasındaki mesafe her zaman
 * güvenli tarafta kalıyor (dolu sayılan bir yuva asla üzerine yazılmıyor).
 * 64 x 64 = 4.096 bayt. */
#define MEL_QUEUE  64
static int8_t s_mel_queue[MEL_QUEUE][PB_MEL_BANDS];
static volatile uint32_t s_mel_write = 0, s_mel_read = 0;

/* Core 1'in yığını. Pico SDK'nın varsayılanı 4 KB; TFLM Invoke'un ne kadar
 * yığın kullandığını ölçmedik (scratch tamponlarını arena'dan alıyor ama
 * çekirdek içi geçici diziler yığında). 8 KB, ölçmeden alınmış güvenli bir
 * pay — kanarya ile ölçülüp küçültülebilir. */
static uint32_t s_core1_heap[2048] __attribute__((aligned(8)));

/**
 * int8 logit'lerden softmax. Ölçek/sıfır noktası modelden geliyor.
 * Taşmaya karşı en büyük değer çıkarılıyor (standart numaralı softmax).
 */
static void softmax(const int8_t *q, float scale, int sifir, float *out) {
    int max_big = q[0];
    for (int i = 1; i < PB_SPECIES_NET_CLASSES; i++) {
        if (q[i] > max_big) max_big = q[i];
    }
    float total = 0.0f;
    for (int i = 0; i < PB_SPECIES_NET_CLASSES; i++) {
        const float z = ((float)q[i] - (float)max_big) * scale;
        out[i] = expf(z);
        total += out[i];
    }
    (void)sifir;  /* fark alındığı için sıfır noktası sadeleşiyor */
    const float ters = 1.0f / total;
    for (int i = 0; i < PB_SPECIES_NET_CLASSES; i++) out[i] *= ters;
}

/**
 * Son `s_olasilik_adet` pencerenin ORTALAMASINI al ve ilk 3'ü seç.
 *
 * Ortalama, tools/measure_voting.py'nin ölçtüğü yöntemin aynısı — orada
 * ölçülen %70,40 / %82,20 rakamları bu birleştirmeye ait. Başka bir kural
 * (örn. oy sayma) seçilirse o sayılar geçersiz olur.
 */
static void birlestir(int16_t *top3, float *skor3, uint32_t *count) {
    float avg[PB_SPECIES_NET_CLASSES];
    const uint32_t n = s_probability_count < PB_VOTE_WINDOWS
                           ? s_probability_count : PB_VOTE_WINDOWS;
    for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) avg[c] = 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        const float *p = s_probability[k];
        for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) avg[c] += p[c];
    }
    const float ters = n ? 1.0f / (float)n : 0.0f;
    for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) avg[c] *= ters;

    for (int r = 0; r < 3; r++) { top3[r] = -1; skor3[r] = -1.0f; }
    for (int c = 0; c < PB_SPECIES_NET_CLASSES; c++) {
        for (int r = 0; r < 3; r++) {
            if (avg[c] > skor3[r]) {
                for (int j = 2; j > r; j--) {
                    skor3[j] = skor3[j - 1];
                    top3[j] = top3[j - 1];
                }
                skor3[r] = avg[c];
                top3[r] = (int16_t)c;
                break;
            }
        }
    }
    *count = n;
}

static void core1_dongu(void) {
    /* Örtüşmeli kare: her turda PB_MEL_HOP yeni örnek alınıp kare sola
     * kaydırılıyor. Örtüşmesiz okumak 16 ms'lik adımı bozar. */
    static int16_t frame[PB_FFT_SIZE];
    static int8_t  window[PB_MEL_FRAMES * PB_MEL_BANDS];
    const uint32_t remaining = PB_FFT_SIZE - PB_MEL_HOP;

    pb_mel_reset();
    pb_gate_reset();
    pb_audio_stream_flush();

    uint32_t step = 0;            /* son çıkarımdan beri geçen kare        */
    uint32_t step_gate = 0;       /* o karelerin kaçında kapı açıktı       */
    /* bir önceki pencerede ikili ağ "kuş" dedi, tür ağı BU pencereye ayrılı
     * (aynı pencerede ikisi birden çalışmıyor — bkz. aşağıdaki uyarı) */
    bool binary_pending = false;
    absolute_time_t last_inference = get_absolute_time();

    while (s_run) {
        memmove(frame, frame + PB_MEL_HOP, remaining * sizeof(int16_t));
        pb_capture_result_t cap =
            pb_audio_stream_read(frame + remaining, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) continue;
        if (cap.fifo_overrun) s_state.overrun++;

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(frame, power);
        pb_gate_result_t g = pb_gate_update(power);
        pb_mel_push(frame);

        /* Kareyi arayüz kuyruğuna bırak (spektrogram). Kuyruk doluysa ATLA —
         * gerçek zamanlı hat arayüz için beklemez. */
        {
            const uint32_t write = s_mel_write;
            const uint32_t next = (write + 1u) % MEL_QUEUE;
            if (next != s_mel_read && pb_mel_last_frame(s_mel_queue[write])) {
                __dmb();               /* veri, indeksten ÖNCE görünür olsun */
                s_mel_write = next;
            }
        }

        s_state.frame++;
        s_state.gate_su_an = g.active;
        if (g.active) { s_state.gate_open++; step_gate++; }
        s_state.band_db = g.band_db;
        s_state.base_db = g.floor_db;
        s_state.aki = g.flux;

        if (++step < STEP_FRAME) continue;
        step = 0;
        const uint32_t gate_count = step_gate;
        step_gate = 0;

        /* Birleştirme belleği bayatladıysa temizle. */
        if (absolute_time_diff_us(last_inference, get_absolute_time())
                > (int64_t)STALE_MS * 1000) {
            s_probability_count = 0;
            s_probability_write = 0;
        }

        /* Aşama-0 kapısı: sessizlikte ağır iş HİÇ çalışmıyor. `ikili_beklemede`
         * iken kapı bu turu iptal ETMİYOR — bir önceki pencerede ikili ağ
         * "kuş" dedi ve tür ağı bu tura AYRILDI, geri çekilmiyor. */
        if (!s_gate_ignore && !binary_pending && gate_count < GATE_THRESHOLD) {
            s_state.skipped++;
            continue;
        }

        /* 3 saniye dolmadıysa pencere yok. */
        if (!pb_mel_window(window)) continue;

        /* ⚠ İKİLİ AĞ VE TÜR AĞI AYNI PENCEREDE ASLA İKİSİ BİRDEN ÇALIŞMAZ.
         *
         * Denendi: ikisi art arda (ikili ~69 ms + tür ağı 190 ms = ~259 ms)
         * ses halkasının 256 ms'lik toleransını aştı ve KARTI KİLİTLEDİ
         * (§9o adım 3) — donanımın DMA ring alanı 4 bit, azami 32 KB
         * (audio_i2s.h), büyütülemiyor. Çözüm: ikisini AYRI pencerelere
         * bölmek. Bir turda en kötü durum hâlâ 190 ms — ring'in zaten
         * doğrulanmış toleransı.
         *
         * ikili_beklemede: bir önceki pencerede ikili ağ "kuş" dedi, bu
         * turda SADECE tür ağı çalışır (daha taze bir 3 s pencereyle —
         * bariz bir dezavantaj değil, tam tersi). Aksi hâlde SADECE ikili
         * ağ çalışır. */
        if (binary_pending) {
            binary_pending = false;
        } else {
            /* Aynı cihaz sözleşmesi: ölçek 1.0, sıfır 0 (ikili_agi.cc'de
             * assert ediliyor). */
            memcpy(pb_binary_net_input(), window, sizeof(window));
            if (!pb_binary_net_run()) continue;
            s_state.binary_ran++;
            s_state.binary_last_p = pb_binary_net_probability();
            if (s_state.binary_last_p < BINARY_THRESHOLD) {
                s_state.binary_red++;
                continue;
            }
            /* "Kuş" dedi: tür ağını BU TURDA ÇALIŞTIRMA (zaman bütçesini
             * aşar), bir sonraki pencereye ayır. */
            binary_pending = true;
            continue;
        }

        /* Cihaz sözleşmesi: girdi ölçeği 1.0, sıfır noktası 0 —
         * dönüşüm YOK, doğrudan kopya (§9k, tur_agi.cc'de assert ediliyor). */
        memcpy(pb_species_net_input(), window, sizeof(window));
        if (!pb_species_net_run()) continue;

        s_state.last_time_us = pb_species_net_last_time_us();
        s_state.inference++;
        last_inference = get_absolute_time();

        softmax(pb_species_net_output(), pb_species_net_output_scale(),
                pb_species_net_output_zero(), s_probability[s_probability_write]);
        s_probability_write = (s_probability_write + 1) % PB_VOTE_WINDOWS;
        if (s_probability_count < PB_VOTE_WINDOWS) s_probability_count++;

        int16_t top3[3];
        float skor3[3];
        uint32_t merged;
        birlestir(top3, skor3, &merged);

        for (int r = 0; r < 3; r++) {
            s_state.top3[r] = top3[r];
            s_state.top3_probability[r] = skor3[r];
        }
        s_state.merged = merged;
        s_state.valid = true;
        __dmb();                 /* gövde sürümden ÖNCE görünür olsun */
        s_state.version++;
    }
}

bool pb_recognizer_start(bool gate_ignore) {
    if (s_run) return true;
    if (!pb_binary_net_init()) return false;
    if (!pb_species_net_init()) return false;

    s_gate_ignore = gate_ignore;

    memset((void *)&s_state, 0, sizeof(s_state));
    s_probability_count = 0;
    s_probability_write = 0;
    s_mel_write = 0;
    s_mel_read = 0;
    s_run = true;

    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_dongu, s_core1_heap,
                                      sizeof(s_core1_heap));
    return true;
}

void pb_recognizer_stop(void) {
    if (!s_run) return;
    s_run = false;
    /* Core 1 en fazla bir okuma zaman aşımı (1 s) kadar sonra döngüden
     * çıkar; beklemeden sıfırlamak yarım kalmış bir Invoke'u kesebilir,
     * o yüzden önce kısa bir pay veriliyor. */
    sleep_ms(400);
    multicore_reset_core1();
}

bool pb_recognizer_get_mel(int8_t *out) {
    if (!out) return false;
    const uint32_t read = s_mel_read;
    if (read == s_mel_write) return false;
    __dmb();
    memcpy(out, s_mel_queue[read], PB_MEL_BANDS);
    __dmb();
    s_mel_read = (read + 1u) % MEL_QUEUE;
    return true;
}

void pb_recognizer_read(pb_recognizer_state_t *out) {
    if (!out) return;
    for (int deneme = 0; deneme < 4; deneme++) {
        const uint32_t v0 = s_state.version;
        __dmb();
        memcpy(out, (const void *)&s_state, sizeof(*out));
        __dmb();
        if (s_state.version == v0) return;   /* yırtık okuma yok */
    }
}
