/**
 * karar.h — Ekrana ne yazılacağına karar veren kural (M7 adım 2)
 *
 * NEDEN AYRI BİR MODÜL: birleştirme (tanima.c) her saniye yeni bir olasılık
 * dağılımı üretiyor. Onu doğrudan ekrana bağlarsak yazı her saniye zıplar ve
 * okunmaz. Arada bir karar kuralı gerekiyor: eşik + histerezis + tutma.
 *
 * Bu dosya BİLEREK donanımsız: yalnızca stdint/stdbool. Böylece host tarafı
 * testlerde (test/dsp_test.c) zaman ilerletilerek sınanabiliyor — histerezisin
 * kartta hata ayıklanması pahalı olurdu.
 *
 * ── EŞİKLER ÖLÇÜLDÜ, TAHMİN EDİLMEDİ ─────────────────────────────────────
 *
 * `tools/esik_olc.py` test kümesinde (6.267 pencere / 1.288 kayıt, hiç
 * dokunulmamış bölüm) 8 pencerelik birleştirmenin p1 dağılımını taradı.
 * Çıktı: models/esik.txt. Ölçülen tablo (kapsam = kaç blokta tür adı yazarız,
 * isabet = yazdığımızda haklı olma oranı, yanlış alarm = negatif blokların
 * kaçında tür adı yazarız):
 *
 *     eşik   kapsam   isabet   yanlış alarm
 *     0,35    %59,7    %71,7      %4,2
 *     0,45    %49,9    %79,0      %3,6
 *     0,50    %45,1    %81,8      %3,0
 *     0,60    %35,6    %85,4      %2,7      <- girme eşiği
 *     0,70    %28,6    %88,5      %2,4
 *     0,80    %20,3    %91,7      %1,5
 *
 * ⚠ Bu sayılar bir ÜST SINIR: bloklar örtüşmeyen dilimlerden ve dilimler
 * BirdNET'in kuş duyduğu yerler; cihazda pencereler 1 sn adımla örtüşüyor,
 * yani hatalar daha ilintili. Saha kalibrasyonu (M8) hâlâ gerekli — o zaman
 * `tools/esik_olc.py` yeniden çalıştırılıp bu üç sabit güncellenir.
 */
#ifndef POKEBIRD_KARAR_H
#define POKEBIRD_KARAR_H

#include <stdbool.h>
#include <stdint.h>

/** Tür adını EKRANA YAZMA eşiği. Ölçüldü: isabet %85,4 · kapsam %35,6 ·
 *  yanlış alarm %2,7 (models/esik.txt, 8 pencere). */
#define PB_KARAR_GIRIS_ESIK   0.60f

/** Yazılanı SİLME eşiği — histerezisin alt ucu. Ölçüldü: isabet %71,7, yani
 *  birleştirilmiş top-1 doğruluğuyla (%70,40, §9k) aynı seviye. Buranın
 *  altındaki bir gösterim artık "en iyi tahmin"den daha iyi değil. */
#define PB_KARAR_CIKIS_ESIK   0.35f

/** Karar için gereken en az birleştirilmiş pencere sayısı. Ölçüldü: 0,60
 *  eşiğinde isabet 1 pencerede %70,1, 3 pencerede %84,9, 8 pencerede %85,4.
 *  Yani kazancın tamamı ilk üç pencerede; 3 hem hızlı hem yeterli. */
#define PB_KARAR_MIN_PENCERE  3u

/** Desteklenmeyen bir gösterimin ekranda kalma süresi. Ses kesilince kapı
 *  kapanır ve çıkarım durur; kullanıcının yazıyı okuyacak zamanı olmalı.
 *  5 s, tanima.c'deki BAYAT_MS'in (6 s, birleştirme belleğinin bayatlaması)
 *  altında seçildi: ekrandan silinen tür, birleştirme belleği temizlenmeden
 *  önce gider — yani "silindi ama hâlâ o türü destekleyen bellek var" hâli
 *  oluşmaz. */
#define PB_KARAR_TUT_MS       5000u

/** Kapı kapandıktan sonra "ses var" göstergesinin sönme süresi. 62 kare/s'de
 *  tek karelik açılıp kapanmalar gözle görülmez; `a` demosundaki ~0,5 s'lik
 *  tutmanın aynısı. */
#define PB_KARAR_SES_TUT_MS   700u

/** Negatif / bilinmiyor sınıfının indeksi (siniflar.h'de son sınıf).
 *  main.c bunu PB_SINIF_SAYISI'na karşı static_assert ile doğruluyor: sınıf
 *  tablosu yeniden üretilirse burası sessizce kaymasın. */
#define PB_KARAR_NEGATIF_SINIF 178

typedef enum {
    PB_KARAR_DINLIYOR = 0,  /* sessiz — kapı kapalı                        */
    PB_KARAR_SES,           /* kapı açık, henüz gösterilecek bir tür yok   */
    PB_KARAR_BELIRSIZ,      /* eşikler arasında: "olabilir"                */
    PB_KARAR_TUR            /* girme eşiğinin üstünde: tür adı             */
} pb_karar_kip_t;

typedef struct {
    uint32_t simdi_ms;      /* şu an (to_ms_since_boot)                    */
    bool     kapi_acik;     /* Aşama-0 kapısı bu an açık mı                */
    bool     yeni_sonuc;    /* bu çağrıda yeni bir birleştirme geldi mi    */
    int16_t  sinif;         /* birleştirilmiş top-1 sınıf indeksi          */
    float    olasilik;      /* o sınıfın birleştirilmiş olasılığı (0..1)   */
    uint32_t birlesen;      /* kaç pencere birleştirildi                   */
} pb_karar_girdi_t;

typedef struct {
    pb_karar_kip_t kip;
    int16_t  sinif;         /* gösterilen sınıf; kip < BELIRSIZ ise -1     */
    float    guven;         /* gösterilen sınıfın son desteklenen olasılığı*/

    /* ── iç durum ── */
    uint32_t son_kapi_ms;   /* kapının en son açık görüldüğü an            */
    uint32_t son_destek_ms; /* gösterimin en son desteklendiği an          */
    uint32_t giris_ms;      /* gösterime geçilen an (günlük/istatistik)    */
    uint32_t surum;         /* ekranda görünen şey her değiştiğinde artar  */
} pb_karar_t;

/**
 * Durumu başlangıç hâline al.
 *
 * `simdi_ms` isteniyor çünkü zaman aşımları mutlak zaman damgalarıyla
 * çalışıyor: sıfırdan başlatılan bir "kapı en son şu an açıktı" damgası,
 * açılıştan 700 ms sonra başlatılan bir kip için "ses var" anlamına gelirdi.
 */
void pb_karar_sifirla(pb_karar_t *k, uint32_t simdi_ms);

/**
 * Kuralı bir adım ilerlet. Her arayüz turunda çağrılır; yeni bir birleştirme
 * olmasa bile çağrılmalı, çünkü zaman aşımları (tutma, ses sönmesi) burada
 * işliyor.
 */
void pb_karar_guncelle(pb_karar_t *k, const pb_karar_girdi_t *g);

/** Kip için sabit durum yazısı — arayüz ve seri port aynı sözcükleri
 *  kullansın diye tek yerde. */
const char *pb_karar_kip_ad(pb_karar_kip_t kip);

#endif /* POKEBIRD_KARAR_H */
