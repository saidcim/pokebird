/**
 * tur_agi.h — Aşama-2 tür ağı: TFLM sarmalayıcısı (C arayüzü)
 *
 * Model: models/species_net_int8.h (tools/train_species.py üretiyor, M5 / §9k).
 * 209.107 parametre, 6,6 MMAC/pencere, 179 sınıf (178 tür + negatif).
 *
 * CİHAZ SÖZLEŞMESİ — bu M6'yı ucuza getiren karar:
 *   TFLite girdi tensörü int8, ölçek 1.0, sıfır noktası 0.
 *   Yani pb_mel_window() çıktısı DOĞRUDAN kopyalanıyor, hiçbir dönüşüm yok.
 *   Sözleşme tools/train_species.py içinde assert ediliyor; kayarsa sessiz doğruluk
 *   kaybı olur, o yüzden pb_species_net_init() burada da kontrol ediyor.
 *
 * Girdi düzeni: (1, 187, 64, 1) — kare dışta (eskiden yeniye), bant içte.
 * mel.c'deki pb_mel_window() düzeninin aynısı ve eğitim kümesi de öyle
 * üretildi (tools/build_dataset.py, §9j).
 */
#ifndef POKEBIRD_AI_SPECIES_AGI_H
#define POKEBIRD_AI_SPECIES_AGI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_SPECIES_NET_CLASSES   179        /* 178 tür + negatif (indeks 178) */
#define PB_SPECIES_NET_NEGATIVE 178

/**
 * Yorumlayıcıyı kur ve tensörleri ayır. Bir kez, açılışta.
 * @return başarısızlıkta false; sebep seri porta yazılır.
 */
bool pb_species_net_init(void);

/** Ayrılan arena'nın GERÇEKTEN kullanılan kısmı (bayt). Ölçüm, tahmin değil. */
size_t pb_species_net_arena_used(void);

/** Ayrılmış arena'nın toplam boyutu (bayt). */
size_t pb_species_net_arena_total(void);

/**
 * Girdi tensörünün ham int8 tamponu — PB_MEL_FRAMES*PB_MEL_BANDS bayt.
 * pb_mel_window() doğrudan buraya yazılabilir.
 */
int8_t *pb_species_net_input(void);

/** Bir çıkarım çalıştır. @return TfLiteStatus kOk ise true. */
bool pb_species_net_run(void);

/** Son çıkarımın süresi (mikrosaniye). */
uint32_t pb_species_net_last_time_us(void);

/** Çıkış logit'leri, ham int8 — PB_SPECIES_NET_CLASSES adet. */
const int8_t *pb_species_net_output(void);

/** Çıkış niceleştirme parametreleri (logit = (q - sifir) * olcek). */
float pb_species_net_output_scale(void);
int   pb_species_net_output_zero(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEBIRD_AI_TUR_AGI_H */
