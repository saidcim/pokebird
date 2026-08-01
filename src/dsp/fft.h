/**
 * fft.h — Basit gerçek-değerli FFT ve spektrum yardımcıları
 *
 * GEÇİCİ: M3'te CMSIS-DSP'nin arm_rfft_fast_f32'si ile değiştirilecek.
 * Burada M2'nin (ekranda canlı spektrogram) tek başına doğrulanabilmesi için
 * bağımlılıksız, sade bir radix-2 uygulaması var. Hız kritik değil; asıl
 * mel öznitelik hattı M3'te gelecek.
 */
#ifndef POKEBIRD_FFT_H
#define POKEBIRD_FFT_H

#include <stdint.h>

#define PB_FFT_SIZE 512
#define PB_FFT_BINS (PB_FFT_SIZE / 2)
/** Gerçek girdi için benzersiz bin sayısı: 0..N/2 dahil. */
#define PB_FFT_POWER_BINS (PB_FFT_SIZE / 2 + 1)

/**
 * Hann pencereli güç spektrumu.
 *
 * Hem canlı spektrogram hem mel öznitelik hattı bunu kullanıyor — tek kaynak
 * olsun ki ikisi aynı pencereyi ve aynı DC giderimini görsün.
 *
 * Ölçek: girdi tam ölçeğe (32768) bölünüyor, sonra pencerenin tutarlı
 * kazancına göre normalize ediliyor. Böylece tam ölçekli sinüs, kendi
 * bin'inde 1.0'a yakın güç veriyor ve dBFS okumaları anlamlı oluyor.
 *
 * @param samples PB_FFT_SIZE adet int16
 * @param power   çıkış, PB_FFT_POWER_BINS adet
 */
void pb_fft_power(const int16_t *samples, float *power);

/**
 * `PB_FFT_SIZE` adet int16 örnekten log-ölçekli genlik spektrumu üret.
 *
 * @param samples  giriş, PB_FFT_SIZE adet
 * @param out      çıkış, n_out adet, 0..255
 * @param n_out    istenen bant sayısı (bin'ler logaritmik gruplanır)
 * @param floor_db gürültü tabanı (dBFS); bunun altı 0'a kırpılır
 */
void pb_fft_spectrum(const int16_t *samples, uint8_t *out, uint32_t n_out,
                     float floor_db);

#endif /* POKEBIRD_FFT_H */
