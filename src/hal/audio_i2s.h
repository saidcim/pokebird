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
    uint32_t samples;       /* gerçekten yakalanan örnek sayısı             */
    bool     fifo_overrun;  /* true ise örnek DÜŞTÜ, ölçüm güvenilmez       */
    bool     timed_out;     /* saat gelmedi (ES8311 BCLK/LRCK üretmiyor)    */
} pb_capture_result_t;

/**
 * MCLK'i başlat ve I2S yakalama yolunu kur.
 * ES8311 I2C üzerinden ayrı olarak yapılandırılmalıdır (es8311_init).
 * MCLK'in ES8311 yapılandırılmadan ÖNCE çalışıyor olması gerekir — codec'in
 * dahili PLL'i MCLK olmadan register yazımlarına düzgün tepki vermez.
 */
bool pb_audio_i2s_init(const pb_audio_cfg_t *cfg);

/** MCLK'i tek başına başlat (ES8311 yapılandırmasından önce çağrılır). */
bool pb_audio_mclk_start(const pb_audio_cfg_t *cfg);

/**
 * `n_samples` adet mono 16-bit örnek yakala (bloklayan).
 * ES8311 mono olduğu için her I2S çerçevesinin sol kanalı kullanılır.
 */
pb_capture_result_t pb_audio_capture(int16_t *dst, uint32_t n_samples);

#endif /* POKEBIRD_AUDIO_I2S_H */
