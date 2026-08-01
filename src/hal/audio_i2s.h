/**
 * audio_i2s.h — ES8311 mikrofonundan I2S yakalama (PIO + DMA)
 *
 * Saat mimarisi: RP2350 sadece MCLK üretir; ES8311 I2S master olarak BCLK ve
 * LRCK'yi ondan türetir. Bu katman saati takip eder, sürmez.
 */
#ifndef POKEBIRD_AUDIO_I2S_H
#define POKEBIRD_AUDIO_I2S_H

#include <stdbool.h>
#include <stdint.h>

/**
 * ES8311 sürücüsünün beklediği yapılandırma.
 *
 * Alan adları Waveshare'in orijinal `pico_audio_t` yapısıyla bilerek aynı
 * tutuldu; böylece es8311.c'nin register dizileri satır satır değiştirilmeden
 * kullanılabiliyor (bkz. es8311.c başlığındaki atıf notu).
 */
typedef struct {
    uint32_t mclk_freq;     /* Hz — RP2350'nin PIO ile ürettiği MCLK        */
    uint32_t sample_freq;   /* Hz — ES8311'in MCLK'ten türeteceği LRCK      */
    uint8_t  res_in;        /* bit — ADC (mikrofon) çözünürlüğü             */
    uint8_t  res_out;       /* bit — DAC (hoparlör) çözünürlüğü             */
} pb_audio_cfg_t;

/** Yakalama sırasında oluşan sorunlar — ölçümün güvenilirliğini gösterir. */
typedef struct {
    uint32_t samples;       /* gerçekten okunan örnek sayısı                */
    bool     fifo_overrun;  /* true ise örnek DÜŞTÜ, ölçüm güvenilmez       */
    bool     timed_out;     /* saat gelmedi (ES8311 BCLK/LRCK üretmiyor)    */
} pb_capture_result_t;

/** Halka tamponunun kapasitesi (örnek). 24 kHz'de ~170 ms. */
#define PB_AUDIO_RING_SAMPLES  4096

/** Tek çağrıda okunabilecek en büyük öbek — halkanın yarısı. */
#define PB_AUDIO_MAX_READ      (PB_AUDIO_RING_SAMPLES / 2)

/**
 * MCLK'i başlat, I2S yakalama yolunu kur ve sürekli yakalamayı başlat.
 * ES8311 I2C üzerinden ayrı olarak yapılandırılmalıdır (es8311_init).
 * MCLK'in ES8311 yapılandırılmadan ÖNCE çalışıyor olması gerekir — codec'in
 * dahili PLL'i MCLK olmadan register yazımlarına düzgün tepki vermez.
 */
bool pb_audio_i2s_init(const pb_audio_cfg_t *cfg);

/** MCLK'i tek başına başlat (ES8311 yapılandırmasından önce çağrılır). */
bool pb_audio_mclk_start(const pb_audio_cfg_t *cfg);

/* ── Sürekli yakalama ──────────────────────────────────────────────────────
 *
 * DMA hiç durmadan halka tamponunu doldurur; tüketici kendi hızında okur.
 * Okuma ile bir sonraki okuma arasında geçen sürede örnek KAYBOLMAZ — işlem
 * süresi halkanın kapasitesini (170 ms) aşmadığı sürece.
 *
 * Neden böyle: eski `pb_audio_capture` her çağrıda FIFO'yu boşaltıp sıfırdan
 * DMA başlatıyordu. Aradaki işlem süresi boyunca gelen örnekler PIO'nun 8
 * kelimelik FIFO'sunu taşırıp düşüyordu; mel hattı bu yüzden gerçek zamanın
 * ancak %91'inde koşabiliyordu (62.5 yerine ~57 kare/s).
 */

/** Sürekli yakalamayı başlat. `pb_audio_i2s_init` zaten çağırıyor. */
bool pb_audio_stream_start(void);

/** Sürekli yakalamayı durdur (DMA zinciri kırılır, PIO çalışmaya devam eder). */
void pb_audio_stream_stop(void);

/**
 * Birikmiş örnekleri at, en tazeden devam et.
 * Canlı göstergeler (seviye, spektrogram) için: gecikmiş veriyi göstermek
 * yerine güncel olana atlarlar. Mel hattı bunu ÇAĞIRMAZ — sürekliliğe
 * ihtiyacı var.
 */
void pb_audio_stream_flush(void);

/** Halkada okunmayı bekleyen örnek sayısı. */
uint32_t pb_audio_stream_available(void);

/**
 * Akıştan `n_samples` örnek oku (en fazla PB_AUDIO_MAX_READ).
 * Yeterli örnek birikene kadar bekler; `timeout_ms` içinde birikmezse
 * `timed_out` ile döner (saat yok demektir).
 *
 * Tüketici halkanın kapasitesi kadar geride kalırsa en eski örnekler
 * yazıcı tarafından ezilir: bu durumda okuma en tazeye atlar ve
 * `fifo_overrun` ile bildirir — sessizce bozuk veri döndürmez.
 */
pb_capture_result_t pb_audio_stream_read(int16_t *dst, uint32_t n_samples,
                                         uint32_t timeout_ms);

/**
 * Kolaylık sarmalayıcısı: birikmişi at, ardından `n_samples` örneği
 * kesintisiz oku. `n_samples` halkadan büyük olabilir — okuma gerçek zamandan
 * hızlı olduğu için halka dolup taşmaz.
 *
 * Teşhis komutları için; gerçek zamanlı hat `pb_audio_stream_read` kullanır.
 */
pb_capture_result_t pb_audio_capture(int16_t *dst, uint32_t n_samples);

#endif /* POKEBIRD_AUDIO_I2S_H */
