#include "ui/arayuz.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ui/ekran_dinleme.h"
#include "ui/ekran_gunluk.h"
#include "ui/lv_port.h"
#include "ui/spectrogram.h"
#include "ui/tema.h"

static lv_obj_t *s_ekran[PB_EKRAN_SAYISI];
static int       s_aktif = PB_EKRAN_DINLEME;
static bool      s_kuruldu;

/* Günlük satırlarının ("3 dk önce") ve sayaçların tazelenme sıklığı. */
#define TAZELE_MS 1000
static uint32_t s_son_tazele;

/* Son görünüm — günlük ekranının sayaç satırı buradan besleniyor. */
static uint32_t s_kare_hiz, s_cikarim, s_overrun;

/* Günlüğe aynı türü tekrar tekrar yazmamak için. */
static char s_son_gunluk_ad[48];

/* ── Kaydırma algılayıcısı ────────────────────────────────────────────────
 *
 * NEDEN LVGL'İN KENDİ HAREKET ALGILAMASI DEĞİL: dokunmatik bugüne kadar hiç
 * parmakla denenmedi (§9b — boşta sabit 0xDB geliyor ve bunun "dokunma yok"
 * mu bozuk paket mi olduğu belirlenemedi). Çalışmazsa NEDEN çalışmadığını
 * ekrana bakmadan görebilmek gerekiyor; bu yüzden algılama ham noktanın
 * üstünde duruyor ve her aşaması ayrı sayaçla ölçülüyor.
 *
 * Eşikler: ekran 640 piksel geniş, 90 piksel yaklaşık yedide biri — kazayla
 * aşılmayacak, bilerek yapılan bir kaydırmada rahat aşılacak kadar. Dikey
 * kısıt, listeye dokunup parmağı kaydıran kullanıcının ekran değiştirmesini
 * engelliyor.                                                              */
#define KAYDIRMA_ESIK_PX   90
#define KAYDIRMA_DIKEY_PAY  2      /* |dx| > 2*|dy| olmalı */
#define KAYDIRMA_AZAMI_MS  1200
#define PARMAK_BIRAKMA_MS    80    /* bu kadar okumasız kalınca "kalktı"    */

uint32_t pb_kaydirma_dokunma;
uint32_t pb_kaydirma_basla;
uint32_t pb_kaydirma_kabul;
uint32_t pb_kaydirma_kisa;
int32_t  pb_kaydirma_son_dx;
int32_t  pb_kaydirma_son_dy;

static bool     s_basili;
static int32_t  s_bas_x, s_bas_y, s_son_x, s_son_y;
static uint32_t s_bas_ms, s_son_dokunma_ms;
static bool     s_bu_dokunusta_kaydirildi;

static void kaydirma_bitir(uint32_t simdi)
{
    if (!s_basili) return;
    s_basili = false;

    if (s_bu_dokunusta_kaydirildi) return;

    const int32_t dx = s_son_x - s_bas_x;
    const int32_t dy = s_son_y - s_bas_y;
    pb_kaydirma_son_dx = dx;
    pb_kaydirma_son_dy = dy;

    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;

    if (simdi - s_bas_ms > KAYDIRMA_AZAMI_MS) { pb_kaydirma_kisa++; return; }
    if (adx < KAYDIRMA_ESIK_PX)               { pb_kaydirma_kisa++; return; }
    if (adx <= KAYDIRMA_DIKEY_PAY * ady)      { pb_kaydirma_kisa++; return; }

    pb_kaydirma_kabul++;
    /* Sağdan sola kaydırma = "sonraki ekran" (sayfa çevirme yönü). */
    pb_arayuz_ekran_ayarla(dx < 0 ? s_aktif + 1 : s_aktif - 1);
}

static void kaydirma_yokla(void)
{
    const uint32_t simdi = to_ms_since_boot(get_absolute_time());

    int32_t x, y;
    if (pb_lv_dokunma_al(&x, &y)) {
        pb_kaydirma_dokunma++;
        s_son_dokunma_ms = simdi;

        if (!s_basili) {
            s_basili = true;
            s_bu_dokunusta_kaydirildi = false;
            s_bas_x = s_son_x = x;
            s_bas_y = s_son_y = y;
            s_bas_ms = simdi;
            pb_kaydirma_basla++;
            return;
        }

        s_son_x = x;
        s_son_y = y;

        /* Parmak henüz kalkmadan eşiği aştıysa hemen geç: kullanıcı
         * parmağını kaldırana kadar beklemek "tepki vermiyor" hissi veriyor. */
        if (!s_bu_dokunusta_kaydirildi) {
            const int32_t dx = s_son_x - s_bas_x;
            const int32_t dy = s_son_y - s_bas_y;
            const int32_t adx = dx < 0 ? -dx : dx;
            const int32_t ady = dy < 0 ? -dy : dy;
            if (adx >= KAYDIRMA_ESIK_PX && adx > KAYDIRMA_DIKEY_PAY * ady &&
                simdi - s_bas_ms <= KAYDIRMA_AZAMI_MS) {
                pb_kaydirma_son_dx = dx;
                pb_kaydirma_son_dy = dy;
                pb_kaydirma_kabul++;
                s_bu_dokunusta_kaydirildi = true;
                pb_arayuz_ekran_ayarla(dx < 0 ? s_aktif + 1 : s_aktif - 1);
            }
        }
        return;
    }

    /* Okuma yok. Dokunmatik ara ara kare düşürüyor; hemen "kalktı" demeden
     * kısa bir pencere bekleniyor, yoksa tek kayıp kare kaydırmayı böler. */
    if (s_basili && (simdi - s_son_dokunma_ms) > PARMAK_BIRAKMA_MS) {
        kaydirma_bitir(simdi);
    }
}

void pb_arayuz_olustur(void)
{
    if (s_kuruldu) return;
    s_kuruldu = true;

    s_ekran[PB_EKRAN_DINLEME] = pb_ekran_dinleme_olustur();
    s_ekran[PB_EKRAN_GUNLUK]  = pb_ekran_gunluk_olustur();

    s_aktif = PB_EKRAN_DINLEME;
    lv_screen_load(s_ekran[s_aktif]);
    pb_lv_dilim_sahibi_ayarla(PB_LVGL_DILIM_MASKE_DINLEME);
    pb_lv_tumunu_kirlet();

    s_son_gunluk_ad[0] = '\0';
}

int pb_arayuz_ekran(void) { return s_aktif; }

void pb_arayuz_ekran_ayarla(int ekran)
{
    if (!s_kuruldu) return;
    if (ekran < 0 || ekran >= PB_EKRAN_SAYISI) return;
    if (ekran == s_aktif) return;

    s_aktif = ekran;

    /* ⚠ GEÇİŞ ANİMASYONU YOK — bilerek. `lv_screen_load_anim` her karede tüm
     * ekranı geçersizleştirir; bizde bir kare, sahip olunan her dilimin
     * panele yeniden basılması (dilim başına 44 KB QSPI) demek. 60 Hz'de
     * imkânsız, 10 Hz'de de takılarak akan bir animasyon anlık geçişten
     * kötü görünürdü. */
    lv_screen_load(s_ekran[s_aktif]);

    if (s_aktif == PB_EKRAN_GUNLUK) {
        /* Günlük tam genişlik: sağdaki iki dilim de LVGL'in oluyor. */
        pb_lv_dilim_sahibi_ayarla(PB_LVGL_DILIM_MASKE_TAM);
        pb_ekran_gunluk_tazele(s_kare_hiz, s_cikarim, s_overrun);
    } else {
        pb_lv_dilim_sahibi_ayarla(PB_LVGL_DILIM_MASKE_DINLEME);
        /* Spektrogram bölgesini günlük ekranı boyamıştı; şeridi sıfırla ki
         * altında eski yazı kalmasın. Sütunlar akmaya devam edip dolduracak. */
        pb_spec_init();
    }

    pb_lv_tumunu_kirlet();
}

void pb_arayuz_sonraki(void)
{
    pb_arayuz_ekran_ayarla((s_aktif + 1) % PB_EKRAN_SAYISI);
}

void pb_arayuz_gunluge_ekle(const char *ad, const char *latin, float guven)
{
    pb_ekran_gunluk_ekle(ad, latin, guven);
}

void pb_arayuz_guncelle(const pb_sonuc_gorunum_t *g)
{
    if (!g || !s_kuruldu) return;

    s_kare_hiz = g->kare_hiz;
    s_cikarim  = g->cikarim;
    s_overrun  = g->overrun;

    pb_ekran_dinleme_guncelle(g);

    /* Karar "TANINDI" dediğinde günlüğe yaz — ama yalnızca tür DEĞİŞTİYSE.
     * Karar kuralı bir türü PB_KARAR_TUT_MS (5 s) boyunca ekranda tutuyor ve
     * bu fonksiyon 4 Hz çağrılıyor, yani aynı tespit ~20 kez düşüyor.
     * `ekran_gunluk` aynı türü üst üste görünce zaten yeni satır açmıyor;
     * buradaki kontrol o çağrıyı hiç yapmamak için. Kip TUR'dan çıkınca
     * sıfırlanıyor ki aynı tür ikinci kez duyulduğunda yeniden yazılsın. */
    if (g->kip == PB_KARAR_TUR && g->tur_ad) {
        if (strncmp(s_son_gunluk_ad, g->tur_ad, sizeof(s_son_gunluk_ad) - 1) != 0) {
            snprintf(s_son_gunluk_ad, sizeof(s_son_gunluk_ad), "%s", g->tur_ad);
            pb_ekran_gunluk_ekle(g->tur_ad, g->ilk3_latin[0], g->guven);
        }
    } else {
        s_son_gunluk_ad[0] = '\0';
    }
}

void pb_arayuz_tick(void)
{
    if (!s_kuruldu) { pb_lv_tick(); return; }

    kaydirma_yokla();

    const uint32_t simdi = to_ms_since_boot(get_absolute_time());
    if (s_aktif == PB_EKRAN_GUNLUK && (simdi - s_son_tazele) >= TAZELE_MS) {
        s_son_tazele = simdi;
        pb_ekran_gunluk_tazele(s_kare_hiz, s_cikarim, s_overrun);
    }

    pb_lv_tick();
}
