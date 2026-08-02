/**
 * tanima.h — Gerçek zamanlı tanıma hattı (core 1)
 *
 * ARCHITECTURE §7'deki iş bölümü:
 *   Core 1  ses + yapay zeka   I2S/DMA → mel → kapı → tür ağı → birleştirme
 *   Core 0  arayüz + depolama
 *
 * Core 1 hiç bloklanmadan koşuyor; core 0 sonucu pb_tanima_oku() ile
 * kopyalıyor. Paylaşılan tek şey aşağıdaki durum yapısı.
 *
 * ⚠ Mel halkası (dsp/mel.c) ve ses akışı bu motor çalışırken CORE 1'İN
 * MALIDIR. `m`, `a`, `s`, `r` gibi aynı kaynakları kullanan teşhis
 * komutlarını aynı anda çalıştırmayın — pb_tanima_durdur() ile core 1'i
 * kapatın önce.
 */
#ifndef POKEBIRD_TANIMA_H
#define POKEBIRD_TANIMA_H

#include <stdbool.h>
#include <stdint.h>

#include "ai/tur_agi.h"

/** Birleştirme penceresi. §9k'da ÖLÇÜLDÜ: kazanç 5–8'de doyuyor, 12'de
 *  artmıyor (hatta top-1 düşüyor). 8 pencere ≈ 8 saniyelik gözlem. */
#define PB_BIRLESTIRME_PENCERE  8

typedef struct {
    uint32_t surum;          /* her yeni sonuçta artar — core 0 buna bakar   */

    uint32_t kare;           /* toplam mel karesi                            */
    uint32_t kapi_acik;      /* kapısı açık kare sayısı                      */
    uint32_t cikarim;        /* çalıştırılan çıkarım sayısı                  */
    uint32_t atlanan;        /* kapı kapalı olduğu için atlanan pencere      */
    uint32_t overrun;        /* ses halkası taştı — süreklilik koptu         */
    uint32_t son_sure_us;    /* son Invoke() süresi                          */
    uint32_t birlesen;       /* kaç pencere birleştirildi (≤ 8)              */

    int16_t  ilk3[3];        /* sınıf indeksleri, en iyiden                  */
    float    ilk3_olasilik[3];
    bool     gecerli;        /* en az bir çıkarım yapıldı mı                 */
    bool     kapi_su_an;     /* kapı EN SON karede açık mıydı — arayüz için  */

    /* Kapı ve gürültü tabanı — teşhis için, `m` komutundakilerin aynısı. */
    float    bant_db, taban_db, aki;
} pb_tanima_durum_t;

/**
 * Core 1'i başlat ve tanıma hattını çalıştır.
 * pb_audio_i2s_init() ve pb_mel_init() önce çağrılmış olmalı.
 *
 * @param kapi_yoksay  true ise Aşama-0 kapısı atlanır ve HER saniye çıkarım
 *                     yapılır. Normal çalışma değil, ÖLÇÜM kipi: hattın en
 *                     kötü durumda (kesintisiz çıkarım) ses sürekliliğini
 *                     koruyup korumadığını sınamak için. Sessiz odada kapı
 *                     hiç açılmadığı için (§9c: %2-3) bu kip olmadan çıkarım
 *                     yolu gerçek zamanlı yük altında hiç test edilemez.
 * @return model yüklenemezse false (core 1 başlatılmaz)
 */
bool pb_tanima_baslat(bool kapi_yoksay);

/** Core 1'i durdur ve sıfırla. */
void pb_tanima_durdur(void);

/** Son durumu kopyala. Core 0'dan güvenli. */
void pb_tanima_oku(pb_tanima_durum_t *out);

/**
 * Spektrogram sütunu kuyruğu — core 1 üretir, core 0 tüketir.
 *
 * NEDEN GEREKLİ: motor çalışırken mel halkası CORE 1'İN MALI (yukarıdaki
 * uyarı). Core 0'ın `pb_mel_last_frame()` çağırması yarış demek. Onun yerine
 * core 1 her kareyi buraya bırakıyor, core 0 boşaltıyor.
 *
 * Kuyruk dolarsa core 1 YENİ kareyi atıyor ve hiç beklemiyor: gerçek zamanlı
 * hattın arayüz yüzünden durması, spektrogramda bir sütun kaybetmekten çok
 * daha pahalı. Kuyruk 64 kare ≈ 1 saniye; core 0 saniyede birkaç kez
 * boşalttığı sürece hiç dolmuyor.
 *
 * @param out PB_MEL_BANDS adet int8
 * @return    kuyruk boşsa false
 */
bool pb_tanima_mel_al(int8_t *out);

#endif /* POKEBIRD_TANIMA_H */
