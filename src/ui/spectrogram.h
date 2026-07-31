/**
 * spectrogram.h — Kaydırmalı canlı spektrogram
 *
 * LVGL canvas kullanmıyoruz. Gerekçe (plan §5): 640x172 tam framebuffer
 * RGB565'te 220 KB eder, 520 KB SRAM'in %42'si. Bunun yerine her karede
 * yalnızca TEK piksel sütunu üretip QSPI'ye gönderiyoruz — bellek maliyeti
 * bir sütun (172 piksel = 344 bayt), CPU maliyeti neredeyse sıfır.
 *
 * Ekran donanımsal olarak dikey kaydırmayı desteklemediği için sütunları
 * dairesel yazıyoruz: yeni sütun en eski sütunun üzerine biniyor ve yazma
 * konumu sağa doğru ilerliyor. Klasik "kayan şerit" görünümü, kaydırma
 * maliyeti olmadan.
 */
#ifndef POKEBIRD_SPECTROGRAM_H
#define POKEBIRD_SPECTROGRAM_H

#include <stdint.h>

/** Ekranın spektrograma ayrılan bölgesi (sağ taraf; sol taraf kimlik kartı) */
#define PB_SPEC_X0      200
#define PB_SPEC_X1      639
#define PB_SPEC_WIDTH   (PB_SPEC_X1 - PB_SPEC_X0 + 1)
#define PB_SPEC_HEIGHT  172

/** Bölgeyi temizle ve yazma konumunu başa al. */
void pb_spec_init(void);

/**
 * Bir zaman dilimini tek sütun olarak çiz.
 * @param bins    her biri 0..255 arası genlik, düşük frekanstan yükseğe
 * @param n_bins  bin sayısı; ekran yüksekliğine ölçeklenir
 */
void pb_spec_push_column(const uint8_t *bins, uint32_t n_bins);

#endif /* POKEBIRD_SPECTROGRAM_H */
