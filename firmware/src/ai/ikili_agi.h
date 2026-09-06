/**
 * ikili_agi.h — Aşama-1 ikili ağ: kuş sesi mi, değil mi (TFLM sarmalayıcısı)
 *
 * Model: models/ikili_agi_int8.h (tools/ikili_egit.py üretiyor, M7 §9o).
 * 7.217 parametre, 1,44 MMAC/pencere, TEK çıkış (sigmoid öncesi ham logit).
 *
 * Aynı cihaz sözleşmesi tür ağıyla birebir aynı (tools/egit.py, §9k):
 * girdi tensörü int8, ölçek 1.0, sıfır noktası 0 — pb_mel_window() çıktısı
 * dönüşümsüz kopyalanabiliyor. pb_ikili_agi_baslat() bunu ölçüp doğruluyor.
 *
 * Girdi düzeni tur_agi.h ile aynı: (1, 187, 64, 1), kare dışta, bant içte.
 *
 * NEDEN AYRI ARENA: tür ağıyla aynı anda çalışmıyorlar (kapı -> ikili ağ ->
 * "kuş" derse tür ağı), ama iki ayrı MicroInterpreter aynı statik arena'yı
 * paylaşmak TFLM'in AllocateTensors çağrısını iki kez, karışık sırayla
 * gerektirir — kırılgan. Ayrı arena çok daha küçük (ölçülecek, ~15-30 KB
 * bekleniyor, tür ağının 120 KB'ının yanında önemsiz).
 */
#ifndef POKEBIRD_AI_IKILI_AGI_H
#define POKEBIRD_AI_IKILI_AGI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Yorumlayıcıyı kur ve tensörleri ayır. Bir kez, açılışta.
 * @return başarısızlıkta false; sebep seri porta yazılır.
 */
bool pb_ikili_agi_baslat(void);

/** Ayrılan arena'nın GERÇEKTEN kullanılan kısmı (bayt). Ölçüm, tahmin değil. */
size_t pb_ikili_agi_arena_kullanilan(void);

/** Ayrılmış arena'nın toplam boyutu (bayt). */
size_t pb_ikili_agi_arena_toplam(void);

/** Girdi tensörünün ham int8 tamponu — PB_MEL_FRAMES*PB_MEL_BANDS bayt. */
int8_t *pb_ikili_agi_girdi(void);

/** Bir çıkarım çalıştır. @return TfLiteStatus kOk ise true. */
bool pb_ikili_agi_calistir(void);

/** Son çıkarımın süresi (mikrosaniye). */
uint32_t pb_ikili_agi_son_sure_us(void);

/** Çıkış logit'i, ham int8 (sigmoid öncesi, TEK değer). */
int8_t pb_ikili_agi_cikti(void);

/** Çıkış niceleştirme parametreleri (logit = (q - sifir) * olcek). */
float pb_ikili_agi_cikti_olcek(void);
int   pb_ikili_agi_cikti_sifir(void);

/**
 * Son çıkarımın "kuş" olasılığı (sigmoid uygulanmış, 0..1).
 * pb_ikili_agi_calistir() sonrası çağrılmalı.
 */
float pb_ikili_agi_olasilik(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEBIRD_AI_IKILI_AGI_H */
