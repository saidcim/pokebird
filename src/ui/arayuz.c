#include "ui/arayuz.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "hal/touch.h"
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
 * NEDEN LVGL'İN KENDİ HAREKET ALGILAMASI DEĞİL: çalışmadığında NEDEN
 * çalışmadığını ekrana bakmadan görebilmek gerekiyor. Algılama ham noktanın
 * üstünde duruyor ve her aşaması ayrı sayaçla ölçülüyor.
 *
 * ⚠ HAM EKSENDE ÖLÇÜLÜYOR, TÜRETİLMİŞ PİKSELDE DEĞİL — ve bu ölçüme dayalı
 * bir karar. `t` kalibrasyonu (kullanıcı, dört kenar, ortanca):
 *
 *     SOL -> SAG (yatay):   ham_x 432 -> 3     degisim -429
 *     ALT -> UST (dikey):   ham_x 560 -> 449   degisim -111
 *                           ham_y 107 -> 93    degisim  -14
 *
 * Buradan çıkan üç şey:
 *   1. Yatay eksen ham_x ve SOLDAN SAĞA AZALIYOR.
 *   2. ham_y kullanılamaz: ekranın tamamı boyunca yalnızca 14 birim
 *      değişiyor (kasanın çıkıntısı üst/alt kenara dokunmayı engelliyor
 *      olmalı). Bu yüzden DİKEY ORAN KISITI KALDIRILDI — güvenilmeyen bir
 *      sayıyla bölmek, elemekten daha kötü.
 *   3. Asıl hata buydu: dikey kaydırmada ham_x 111 birim kayıyor ve eski
 *      eşik 90'dı, yani dikey hareket yatay kaydırma sayılıyordu.
 *
 * Eşik ikisinin ARASINA konuldu: kazara kayma 111, bilinçli kaydırma ~429.
 * 200 ikisinden de rahat uzakta.
 *
 * Ham->piksel ölçeği KALİBRE EDİLMEDİ (sol kenar 432 okuyor, 639 değil) ama
 * gerekmiyor: ekranda dokunulacak bir şey yok, yalnızca kaydırma var.       */
#define KAYDIRMA_ESIK_HAM  200
#define KAYDIRMA_AZAMI_MS  1200
#define PARMAK_BIRAKMA_MS    80    /* bu kadar okumasız kalınca "kalktı"    */

/* Uzun eksenin makul üst sınırı. Çip ara sıra ~4000 veriyor (12 bit, panel
 * dışı); o kareler düşürülüyor. Ölçülen en büyük gerçek değer 560. */
#define HAM_AZAMI          1000

/* ── Kayıt butonu — dokunuşun YATAY yeri yeterli ──────────────────────────
 *
 * Buton ekranın sol 96 pikselinde ve TAM YÜKSEKLİKTE (ekran_dinleme.c'deki
 * gerekçe: kalibrasyon kısa ekseni kullanılamaz gösterdi, bir dokunuşun
 * dikey yerini bilemiyoruz). Dolayısıyla vuruş testi tek boyutlu.
 *
 * Kalibrasyon: sol kenar ham_x ~432, sağ kenar ~3, yani 640 piksel ~429 ham
 * birim (piksel başına ~0,67). Buton ui_x 0..95 -> ham_x 432..~368.
 * Eşik 370 seçildi; içerik 96'dan başlıyor ve ilk pikselleri zaten boşluk,
 * yani sınırın birkaç piksel kayması bir şeyi bozmuyor.
 *
 * ⚠ Ölçek KABA. Ekrana ikinci bir dokunmatik hedef eklenirse bu yetmez;
 * önce ham->piksel eşlemesi düzgün kalibre edilmeli. */
#define BUTON_HAM_ESIK     370
#define BASMA_AZAMI_MS     800     /* bundan uzun basış "basma" sayılmıyor */

uint32_t pb_kaydirma_dokunma;
uint32_t pb_kaydirma_basla;
uint32_t pb_kaydirma_kabul;
uint32_t pb_kaydirma_kisa;
uint32_t pb_buton_basim;
int32_t  pb_kaydirma_son_dx;
int32_t  pb_kaydirma_son_dy;

/* Cihaz artık SÜREKLİ DİNLEMİYOR — kullanıcının kararı. Dinlemeyi kayıt
 * butonu başlatıyor; açılışta kapalı. */
static bool s_kayitta;

static bool     s_basili;
static int32_t  s_bas_ham, s_son_ham;
static uint32_t s_bas_ms, s_son_dokunma_ms;
static bool     s_bu_dokunusta_kaydirildi;

/** Kaydırmayı uygula. `d` ham eksendeki değişim.
 *
 * ham_x soldan sağa AZALDIĞI için (kalibrasyon), sağdan sola kaydırma —
 * yani sayfa çevirme yönü, "sonraki ekran" — ham_x'i ARTIRIYOR. */
static void kaydirmayi_uygula(int32_t d)
{
    pb_kaydirma_kabul++;
    pb_arayuz_ekran_ayarla(d > 0 ? s_aktif + 1 : s_aktif - 1);
}

static void kaydirma_bitir(uint32_t simdi)
{
    if (!s_basili) return;
    s_basili = false;

    if (s_bu_dokunusta_kaydirildi) return;

    const int32_t d = s_son_ham - s_bas_ham;
    pb_kaydirma_son_dx = d;

    const int32_t ad = d < 0 ? -d : d;
    const uint32_t sure = simdi - s_bas_ms;

    /* Kaydırma mı, butona basma mı? Parmak fazla gezmediyse ve dokunuş
     * butonun şeridinde başladıysa BASMA. Kaydırma testinden ÖNCE bakılıyor
     * çünkü eşiği aşmayan her dokunuş zaten kaydırma değil. */
    if (ad < KAYDIRMA_ESIK_HAM) {
        if (s_aktif == PB_EKRAN_DINLEME && s_bas_ham >= BUTON_HAM_ESIK &&
            sure <= BASMA_AZAMI_MS) {
            pb_buton_basim++;
            pb_arayuz_kayit_ayarla(!s_kayitta);
        } else {
            pb_kaydirma_kisa++;
        }
        return;
    }

    if (sure > KAYDIRMA_AZAMI_MS) { pb_kaydirma_kisa++; return; }

    kaydirmayi_uygula(d);
}

static void kaydirma_yokla(void)
{
    const uint32_t simdi = to_ms_since_boot(get_absolute_time());

    /* Ham noktayı doğrudan okuyoruz: eşik ham eksende ölçüldü (yukarıdaki
     * kalibrasyon) ve ham->piksel ölçeği kalibre edilmiş değil. */
    pb_touch_state_t st = pb_touch_read();
    const bool gecerli = st.ok && st.fingers > 0 && st.p.raw_x < HAM_AZAMI;

    if (gecerli) {
        const int32_t ham = (int32_t)st.p.raw_x;
        pb_kaydirma_dokunma++;
        pb_kaydirma_son_dy = (int32_t)st.p.raw_y;   /* teşhis için ham kısa eksen */
        s_son_dokunma_ms = simdi;

        if (!s_basili) {
            s_basili = true;
            s_bu_dokunusta_kaydirildi = false;
            s_bas_ham = s_son_ham = ham;
            s_bas_ms = simdi;
            pb_kaydirma_basla++;
            return;
        }

        s_son_ham = ham;

        /* Parmak henüz kalkmadan eşiği aştıysa hemen geç: kullanıcı
         * parmağını kaldırana kadar beklemek "tepki vermiyor" hissi veriyor. */
        if (!s_bu_dokunusta_kaydirildi) {
            const int32_t d = s_son_ham - s_bas_ham;
            const int32_t ad = d < 0 ? -d : d;
            if (ad >= KAYDIRMA_ESIK_HAM && simdi - s_bas_ms <= KAYDIRMA_AZAMI_MS) {
                pb_kaydirma_son_dx = d;
                s_bu_dokunusta_kaydirildi = true;
                kaydirmayi_uygula(d);
            }
        }
        return;
    }

    /* Okuma yok (ya da panel dışı kare). Dokunmatik ara ara kare düşürüyor;
     * hemen "kalktı" demeden kısa bir pencere bekleniyor, yoksa tek kayıp
     * kare kaydırmayı böler. */
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

    /* Açılışta BOŞTA — cihaz sürekli dinlemiyor. */
    s_kayitta = false;
    pb_ekran_dinleme_kayit_ayarla(false);

    s_son_gunluk_ad[0] = '\0';
}

int pb_arayuz_ekran(void) { return s_aktif; }

bool pb_arayuz_kayitta(void) { return s_kayitta; }

void pb_arayuz_kayit_ayarla(bool kayitta)
{
    if (!s_kuruldu || kayitta == s_kayitta) return;
    s_kayitta = kayitta;
    pb_ekran_dinleme_kayit_ayarla(kayitta);

    /* Kayıt durdurulunca şerit olduğu gibi kalıyor (son duyulanın kaydı) ama
     * yeni kayıtta karışmasın diye başlarken temizleniyor. */
    if (kayitta && s_aktif == PB_EKRAN_DINLEME) pb_spec_init();
}

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
