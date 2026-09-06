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
#include "ai/decision.h"

static void show(pb_decision_t *k, pb_decision_mode_t mode, int16_t cls,
                   float confidence, uint32_t now_ms) {
    if (k->mode != mode || k->cls != cls) {
        k->enter_ms = now_ms;
        k->version++;
    }
    k->mode = mode;
    k->cls = cls;
    k->confidence = confidence;
    k->last_support_ms = now_ms;
}

void pb_decision_reset(pb_decision_t *k, uint32_t now_ms) {
    if (!k) return;
    k->mode = PB_DECISION_LISTENING;
    k->cls = -1;
    k->confidence = 0.0f;
    /* "Çoktan geçmişte": açılışta ne ses göstergesi ne de bir gösterim
     * tutması yanlışlıkla canlı görünsün. Fark hesapları işaretsiz olduğu
     * için taşma da doğru çalışıyor. */
    k->last_gate_ms = now_ms - (PB_DECISION_SOUND_HOLD_MS + 1u);
    k->last_support_ms = now_ms - (PB_DECISION_HOLD_MS + 1u);
    k->enter_ms = now_ms;
    k->version = 0;
}

void pb_decision_update(pb_decision_t *k, const pb_decision_input_t *g) {
    if (!k || !g) return;

    if (g->gate_open) k->last_gate_ms = g->now_ms;

    if (g->fresh_result) {
        const bool candidate =
            g->cls >= 0 &&
            g->cls != PB_DECISION_NEGATIVE_CLASS &&
            g->merged >= PB_DECISION_MIN_WINDOWS;
        const bool gosteriliyor =
            (k->mode == PB_DECISION_SPECIES || k->mode == PB_DECISION_UNSURE);

        if (candidate && g->probability >= PB_DECISION_ENTER_THRESHOLD) {
            show(k, PB_DECISION_SPECIES, g->cls, g->probability, g->now_ms);
        } else if (candidate && g->probability >= PB_DECISION_EXIT_THRESHOLD &&
                   gosteriliyor && g->cls == k->cls) {
            /* Histerezis: ekrandaki tür, çıkma eşiğinin üstünde kaldığı
             * sürece kipini korur — TÜR ise TÜR kalır. */
            show(k, k->mode, k->cls, g->probability, g->now_ms);
        } else if (candidate && g->probability >= PB_DECISION_EXIT_THRESHOLD && !gosteriliyor) {
            show(k, PB_DECISION_UNSURE, g->cls, g->probability, g->now_ms);
        }
        /* aksi hâlde: destek yok, aşağıdaki tutma süresi karar versin */
    }

    if (k->mode == PB_DECISION_SPECIES || k->mode == PB_DECISION_UNSURE) {
        if (g->now_ms - k->last_support_ms > PB_DECISION_HOLD_MS) {
            k->cls = -1;
            k->confidence = 0.0f;
            k->mode = PB_DECISION_LISTENING;   /* aşağıdaki satır SES'e yükseltebilir */
            k->version++;
        }
    }

    if (k->mode != PB_DECISION_SPECIES && k->mode != PB_DECISION_UNSURE) {
        const pb_decision_mode_t fresh =
            (g->now_ms - k->last_gate_ms <= PB_DECISION_SOUND_HOLD_MS)
                ? PB_DECISION_SOUND : PB_DECISION_LISTENING;
        if (fresh != k->mode) {
            k->mode = fresh;
            k->version++;
        }
    }
}

const char *pb_decision_mode_name(pb_decision_mode_t mode) {
    switch (mode) {
        case PB_DECISION_LISTENING:  return "dinliyor";
        case PB_DECISION_SOUND:       return "SES ALGILANDI";
        case PB_DECISION_UNSURE:  return "olabilir";
        case PB_DECISION_SPECIES:       return "TUR";
    }
    return "?";
}
