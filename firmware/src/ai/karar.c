/**
 * karar.c — Eşik + histerezis + tutma. Gerekçeler ve ölçülen sayılar karar.h'de.
 *
 * KURALIN TAMAMI (bilerek küçük — okunabilir olması, doğru olmasının yarısı):
 *
 *   yeni bir birleştirme geldiğinde
 *     aday geçerli mi?  sınıf negatif değil ve en az MIN_PENCERE pencere
 *       p >= GIRIS                         -> TÜR göster (sınıfı değiştirebilir)
 *       p >= CIKIS ve aynı sınıf zaten ekranda -> kip korunur, destek tazelenir
 *       p >= CIKIS ve ekranda bir şey yok  -> BELİRSİZ göster
 *       aksi hâlde                          -> destek yok
 *
 *   her turda
 *     gösterim TUT_MS boyunca desteklenmediyse silinir
 *     gösterim yoksa kip = kapı yakın zamanda açıldıysa SES, değilse DİNLİYOR
 *
 * DİKKAT — "aynı sınıf zaten ekranda" dalı histerezisin kendisi: ekrandaki
 * türün yerini almak GIRIS eşiği ister, kalması ise yalnızca CIKIS eşiği.
 * Böylece güven eşiğin etrafında salınırken yazı zıplamıyor. Farklı bir tür
 * CIKIS ile GIRIS arasında bir güvenle gelirse gösterimi DEĞİŞTİRMİYOR;
 * eskisi tutma süresi dolana kadar kalıyor. Bilinçli: ekranı ikinci en iyi
 * tahminle titretmektense biraz eski bilgi göstermek yeğ.
 */
#include "ai/karar.h"

static void goster(pb_karar_t *k, pb_karar_kip_t kip, int16_t sinif,
                   float guven, uint32_t simdi_ms) {
    if (k->kip != kip || k->sinif != sinif) {
        k->giris_ms = simdi_ms;
        k->surum++;
    }
    k->kip = kip;
    k->sinif = sinif;
    k->guven = guven;
    k->son_destek_ms = simdi_ms;
}

void pb_karar_sifirla(pb_karar_t *k, uint32_t simdi_ms) {
    if (!k) return;
    k->kip = PB_KARAR_DINLIYOR;
    k->sinif = -1;
    k->guven = 0.0f;
    /* "Çoktan geçmişte": açılışta ne ses göstergesi ne de bir gösterim
     * tutması yanlışlıkla canlı görünsün. Fark hesapları işaretsiz olduğu
     * için taşma da doğru çalışıyor. */
    k->son_kapi_ms = simdi_ms - (PB_KARAR_SES_TUT_MS + 1u);
    k->son_destek_ms = simdi_ms - (PB_KARAR_TUT_MS + 1u);
    k->giris_ms = simdi_ms;
    k->surum = 0;
}

void pb_karar_guncelle(pb_karar_t *k, const pb_karar_girdi_t *g) {
    if (!k || !g) return;

    if (g->kapi_acik) k->son_kapi_ms = g->simdi_ms;

    if (g->yeni_sonuc) {
        const bool aday =
            g->sinif >= 0 &&
            g->sinif != PB_KARAR_NEGATIF_SINIF &&
            g->birlesen >= PB_KARAR_MIN_PENCERE;
        const bool gosteriliyor =
            (k->kip == PB_KARAR_TUR || k->kip == PB_KARAR_BELIRSIZ);

        if (aday && g->olasilik >= PB_KARAR_GIRIS_ESIK) {
            goster(k, PB_KARAR_TUR, g->sinif, g->olasilik, g->simdi_ms);
        } else if (aday && g->olasilik >= PB_KARAR_CIKIS_ESIK &&
                   gosteriliyor && g->sinif == k->sinif) {
            /* Histerezis: ekrandaki tür, çıkma eşiğinin üstünde kaldığı
             * sürece kipini korur — TÜR ise TÜR kalır. */
            goster(k, k->kip, k->sinif, g->olasilik, g->simdi_ms);
        } else if (aday && g->olasilik >= PB_KARAR_CIKIS_ESIK && !gosteriliyor) {
            goster(k, PB_KARAR_BELIRSIZ, g->sinif, g->olasilik, g->simdi_ms);
        }
        /* aksi hâlde: destek yok, aşağıdaki tutma süresi karar versin */
    }

    if (k->kip == PB_KARAR_TUR || k->kip == PB_KARAR_BELIRSIZ) {
        if (g->simdi_ms - k->son_destek_ms > PB_KARAR_TUT_MS) {
            k->sinif = -1;
            k->guven = 0.0f;
            k->kip = PB_KARAR_DINLIYOR;   /* aşağıdaki satır SES'e yükseltebilir */
            k->surum++;
        }
    }

    if (k->kip != PB_KARAR_TUR && k->kip != PB_KARAR_BELIRSIZ) {
        const pb_karar_kip_t yeni =
            (g->simdi_ms - k->son_kapi_ms <= PB_KARAR_SES_TUT_MS)
                ? PB_KARAR_SES : PB_KARAR_DINLIYOR;
        if (yeni != k->kip) {
            k->kip = yeni;
            k->surum++;
        }
    }
}

const char *pb_karar_kip_ad(pb_karar_kip_t kip) {
    switch (kip) {
        case PB_KARAR_DINLIYOR:  return "dinliyor";
        case PB_KARAR_SES:       return "SES ALGILANDI";
        case PB_KARAR_BELIRSIZ:  return "olabilir";
        case PB_KARAR_TUR:       return "TUR";
    }
    return "?";
}
