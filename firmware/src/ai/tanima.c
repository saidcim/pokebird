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
#include "ai/tanima.h"

#include <math.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "ai/ikili_agi.h"
#include "dsp/fft.h"
#include "dsp/gate.h"
#include "dsp/mel.h"
#include "hal/audio_i2s.h"

/* Aşama-1 ikili ağın karar eşiği. tools/ikili_egit.py'de ÖLÇÜLDÜ: eşik 0.5'te
 * test kümesinde kuş-geri-çağırma %97,90, negatif-özgüllük %86,58 (§9o).
 * Kaçırma (yanlış "değil") pahalı — gerçek bir tespiti sessizce kaybediyor;
 * geçirme (yanlış "kuş") ucuz — tür ağı zaten kendi negatif sınıfıyla eliyor.
 * Bu yüzden eşik 0.5'ten AŞAĞI çekilmedi: ölçülen nokta zaten geri-çağırmayı
 * önceliklendiriyor (model seçimi de bu ölçütle yapıldı, bkz. ikili_egit.py). */
#define IKILI_ESIK  0.5f

/* Çıkarım adımı: kaç mel karesinde bir pencere değerlendirilsin.
 * 63 kare × 16 ms = 1,008 s — plandaki 1 saniyelik pencere adımı (§7). */
#define ADIM_KARE   63

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
#define KAPI_ESIK   5

/* Birleştirme belleğinin bayatlama süresi. Bu kadar süre çıkarım yapılmazsa
 * (ortam sessizleşti) birikmiş olasılıklar atılıyor: 30 saniye önceki bir
 * ötüşü şimdiki sonuca karıştırmak yanlış olur. */
#define BAYAT_MS    6000

/* ── Paylaşılan durum ──────────────────────────────────────────────────────
 * Core 1 yazar, core 0 okur. Kilit yok: `surum` alanı en SON yazılıyor ve
 * core 0 onu okuduktan sonra gövdeyi kopyalıyor; kopyaladıktan sonra sürümü
 * tekrar kontrol ediyor. Yırtık okuma olursa tekrar deniyor. Tek yazar
 * olduğu için bu yeterli — spinlock'a gerek yok. */
static volatile pb_tanima_durum_t s_durum;
static volatile bool s_calis = false;
static volatile bool s_kapi_yoksay = false;

/* Birleştirme halkası: son 8 pencerenin softmax olasılıkları.
 * 8 × 179 × 4 = 5.728 bayt. */
static float s_olasilik[PB_BIRLESTIRME_PENCERE][PB_TUR_AGI_SINIF];
static uint32_t s_olasilik_yaz = 0;
static uint32_t s_olasilik_adet = 0;

/* ── Spektrogram sütunu kuyruğu (core 1 -> core 0) ────────────────────────
 * Tek yazar / tek okuyucu halka. Kilit yok: yazar yalnızca `yaz`ı, okuyucu
 * yalnızca `oku`yu ilerletiyor; iki indeks arasındaki mesafe her zaman
 * güvenli tarafta kalıyor (dolu sayılan bir yuva asla üzerine yazılmıyor).
 * 64 x 64 = 4.096 bayt. */
#define MEL_KUYRUK  64
static int8_t s_mel_kuyruk[MEL_KUYRUK][PB_MEL_BANDS];
static volatile uint32_t s_mel_yaz = 0, s_mel_oku = 0;

/* Core 1'in yığını. Pico SDK'nın varsayılanı 4 KB; TFLM Invoke'un ne kadar
 * yığın kullandığını ölçmedik (scratch tamponlarını arena'dan alıyor ama
 * çekirdek içi geçici diziler yığında). 8 KB, ölçmeden alınmış güvenli bir
 * pay — kanarya ile ölçülüp küçültülebilir. */
static uint32_t s_core1_yigin[2048] __attribute__((aligned(8)));

/**
 * int8 logit'lerden softmax. Ölçek/sıfır noktası modelden geliyor.
 * Taşmaya karşı en büyük değer çıkarılıyor (standart numaralı softmax).
 */
static void softmax(const int8_t *q, float olcek, int sifir, float *out) {
    int en_buyuk = q[0];
    for (int i = 1; i < PB_TUR_AGI_SINIF; i++) {
        if (q[i] > en_buyuk) en_buyuk = q[i];
    }
    float toplam = 0.0f;
    for (int i = 0; i < PB_TUR_AGI_SINIF; i++) {
        const float z = ((float)q[i] - (float)en_buyuk) * olcek;
        out[i] = expf(z);
        toplam += out[i];
    }
    (void)sifir;  /* fark alındığı için sıfır noktası sadeleşiyor */
    const float ters = 1.0f / toplam;
    for (int i = 0; i < PB_TUR_AGI_SINIF; i++) out[i] *= ters;
}

/**
 * Son `s_olasilik_adet` pencerenin ORTALAMASINI al ve ilk 3'ü seç.
 *
 * Ortalama, tools/birlestirme_olc.py'nin ölçtüğü yöntemin aynısı — orada
 * ölçülen %70,40 / %82,20 rakamları bu birleştirmeye ait. Başka bir kural
 * (örn. oy sayma) seçilirse o sayılar geçersiz olur.
 */
static void birlestir(int16_t *ilk3, float *skor3, uint32_t *adet) {
    float ort[PB_TUR_AGI_SINIF];
    const uint32_t n = s_olasilik_adet < PB_BIRLESTIRME_PENCERE
                           ? s_olasilik_adet : PB_BIRLESTIRME_PENCERE;
    for (int c = 0; c < PB_TUR_AGI_SINIF; c++) ort[c] = 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        const float *p = s_olasilik[k];
        for (int c = 0; c < PB_TUR_AGI_SINIF; c++) ort[c] += p[c];
    }
    const float ters = n ? 1.0f / (float)n : 0.0f;
    for (int c = 0; c < PB_TUR_AGI_SINIF; c++) ort[c] *= ters;

    for (int r = 0; r < 3; r++) { ilk3[r] = -1; skor3[r] = -1.0f; }
    for (int c = 0; c < PB_TUR_AGI_SINIF; c++) {
        for (int r = 0; r < 3; r++) {
            if (ort[c] > skor3[r]) {
                for (int j = 2; j > r; j--) {
                    skor3[j] = skor3[j - 1];
                    ilk3[j] = ilk3[j - 1];
                }
                skor3[r] = ort[c];
                ilk3[r] = (int16_t)c;
                break;
            }
        }
    }
    *adet = n;
}

static void core1_dongu(void) {
    /* Örtüşmeli kare: her turda PB_MEL_HOP yeni örnek alınıp kare sola
     * kaydırılıyor. Örtüşmesiz okumak 16 ms'lik adımı bozar. */
    static int16_t kare[PB_FFT_SIZE];
    static int8_t  pencere[PB_MEL_FRAMES * PB_MEL_BANDS];
    const uint32_t kalan = PB_FFT_SIZE - PB_MEL_HOP;

    pb_mel_reset();
    pb_gate_reset();
    pb_audio_stream_flush();

    uint32_t adim = 0;            /* son çıkarımdan beri geçen kare        */
    uint32_t adim_kapi = 0;       /* o karelerin kaçında kapı açıktı       */
    /* bir önceki pencerede ikili ağ "kuş" dedi, tür ağı BU pencereye ayrılı
     * (aynı pencerede ikisi birden çalışmıyor — bkz. aşağıdaki uyarı) */
    bool ikili_beklemede = false;
    absolute_time_t son_cikarim = get_absolute_time();

    while (s_calis) {
        memmove(kare, kare + PB_MEL_HOP, kalan * sizeof(int16_t));
        pb_capture_result_t cap =
            pb_audio_stream_read(kare + kalan, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) continue;
        if (cap.fifo_overrun) s_durum.overrun++;

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(kare, power);
        pb_gate_result_t g = pb_gate_update(power);
        pb_mel_push(kare);

        /* Kareyi arayüz kuyruğuna bırak (spektrogram). Kuyruk doluysa ATLA —
         * gerçek zamanlı hat arayüz için beklemez. */
        {
            const uint32_t yaz = s_mel_yaz;
            const uint32_t sonraki = (yaz + 1u) % MEL_KUYRUK;
            if (sonraki != s_mel_oku && pb_mel_last_frame(s_mel_kuyruk[yaz])) {
                __dmb();               /* veri, indeksten ÖNCE görünür olsun */
                s_mel_yaz = sonraki;
            }
        }

        s_durum.kare++;
        s_durum.kapi_su_an = g.active;
        if (g.active) { s_durum.kapi_acik++; adim_kapi++; }
        s_durum.bant_db = g.band_db;
        s_durum.taban_db = g.floor_db;
        s_durum.aki = g.flux;

        if (++adim < ADIM_KARE) continue;
        adim = 0;
        const uint32_t kapi_sayisi = adim_kapi;
        adim_kapi = 0;

        /* Birleştirme belleği bayatladıysa temizle. */
        if (absolute_time_diff_us(son_cikarim, get_absolute_time())
                > (int64_t)BAYAT_MS * 1000) {
            s_olasilik_adet = 0;
            s_olasilik_yaz = 0;
        }

        /* Aşama-0 kapısı: sessizlikte ağır iş HİÇ çalışmıyor. `ikili_beklemede`
         * iken kapı bu turu iptal ETMİYOR — bir önceki pencerede ikili ağ
         * "kuş" dedi ve tür ağı bu tura AYRILDI, geri çekilmiyor. */
        if (!s_kapi_yoksay && !ikili_beklemede && kapi_sayisi < KAPI_ESIK) {
            s_durum.atlanan++;
            continue;
        }

        /* 3 saniye dolmadıysa pencere yok. */
        if (!pb_mel_window(pencere)) continue;

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
        if (ikili_beklemede) {
            ikili_beklemede = false;
        } else {
            /* Aynı cihaz sözleşmesi: ölçek 1.0, sıfır 0 (ikili_agi.cc'de
             * assert ediliyor). */
            memcpy(pb_ikili_agi_girdi(), pencere, sizeof(pencere));
            if (!pb_ikili_agi_calistir()) continue;
            s_durum.ikili_calisti++;
            s_durum.ikili_son_p = pb_ikili_agi_olasilik();
            if (s_durum.ikili_son_p < IKILI_ESIK) {
                s_durum.ikili_red++;
                continue;
            }
            /* "Kuş" dedi: tür ağını BU TURDA ÇALIŞTIRMA (zaman bütçesini
             * aşar), bir sonraki pencereye ayır. */
            ikili_beklemede = true;
            continue;
        }

        /* Cihaz sözleşmesi: girdi ölçeği 1.0, sıfır noktası 0 —
         * dönüşüm YOK, doğrudan kopya (§9k, tur_agi.cc'de assert ediliyor). */
        memcpy(pb_tur_agi_girdi(), pencere, sizeof(pencere));
        if (!pb_tur_agi_calistir()) continue;

        s_durum.son_sure_us = pb_tur_agi_son_sure_us();
        s_durum.cikarim++;
        son_cikarim = get_absolute_time();

        softmax(pb_tur_agi_cikti(), pb_tur_agi_cikti_olcek(),
                pb_tur_agi_cikti_sifir(), s_olasilik[s_olasilik_yaz]);
        s_olasilik_yaz = (s_olasilik_yaz + 1) % PB_BIRLESTIRME_PENCERE;
        if (s_olasilik_adet < PB_BIRLESTIRME_PENCERE) s_olasilik_adet++;

        int16_t ilk3[3];
        float skor3[3];
        uint32_t birlesen;
        birlestir(ilk3, skor3, &birlesen);

        for (int r = 0; r < 3; r++) {
            s_durum.ilk3[r] = ilk3[r];
            s_durum.ilk3_olasilik[r] = skor3[r];
        }
        s_durum.birlesen = birlesen;
        s_durum.gecerli = true;
        __dmb();                 /* gövde sürümden ÖNCE görünür olsun */
        s_durum.surum++;
    }
}

bool pb_tanima_baslat(bool kapi_yoksay) {
    if (s_calis) return true;
    if (!pb_ikili_agi_baslat()) return false;
    if (!pb_tur_agi_baslat()) return false;

    s_kapi_yoksay = kapi_yoksay;

    memset((void *)&s_durum, 0, sizeof(s_durum));
    s_olasilik_adet = 0;
    s_olasilik_yaz = 0;
    s_mel_yaz = 0;
    s_mel_oku = 0;
    s_calis = true;

    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_dongu, s_core1_yigin,
                                      sizeof(s_core1_yigin));
    return true;
}

void pb_tanima_durdur(void) {
    if (!s_calis) return;
    s_calis = false;
    /* Core 1 en fazla bir okuma zaman aşımı (1 s) kadar sonra döngüden
     * çıkar; beklemeden sıfırlamak yarım kalmış bir Invoke'u kesebilir,
     * o yüzden önce kısa bir pay veriliyor. */
    sleep_ms(400);
    multicore_reset_core1();
}

bool pb_tanima_mel_al(int8_t *out) {
    if (!out) return false;
    const uint32_t oku = s_mel_oku;
    if (oku == s_mel_yaz) return false;
    __dmb();
    memcpy(out, s_mel_kuyruk[oku], PB_MEL_BANDS);
    __dmb();
    s_mel_oku = (oku + 1u) % MEL_KUYRUK;
    return true;
}

void pb_tanima_oku(pb_tanima_durum_t *out) {
    if (!out) return;
    for (int deneme = 0; deneme < 4; deneme++) {
        const uint32_t v0 = s_durum.surum;
        __dmb();
        memcpy(out, (const void *)&s_durum, sizeof(*out));
        __dmb();
        if (s_durum.surum == v0) return;   /* yırtık okuma yok */
    }
}
