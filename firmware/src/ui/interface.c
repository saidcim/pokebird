#include "ui/interface.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "hal/touch.h"
#include "ui/screen_listen.h"
#include "ui/screen_log.h"
#include "ui/lv_port.h"
#include "ui/spectrogram.h"
#include "ui/theme.h"

static lv_obj_t *s_screen[PB_SCREEN_COUNT];
static int       s_active = PB_SCREEN_LISTEN;
static bool      s_built;

/* Günlük satırlarının ("3 dk önce") ve sayaçların tazelenme sıklığı. */
#define REFRESH_MS 1000
static uint32_t s_last_refresh;

/* Son görünüm — günlük ekranının sayaç satırı buradan besleniyor. */
static uint32_t s_frame_rate, s_inference, s_overrun;

/* Günlüğe aynı türü tekrar tekrar yazmamak için. */
static char s_last_log_name[48];

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
#define SWIPE_THRESHOLD_RAW  200
#define SWIPE_MAX_MS  1200
#define PARMAK_BIRAKMA_MS    80    /* bu kadar okumasız kalınca "kalktı"    */

/* Uzun eksenin makul üst sınırı. Çip ara sıra ~4000 veriyor (12 bit, panel
 * dışı); o kareler düşürülüyor. Ölçülen en büyük gerçek değer 560. */
#define RAW_MAX          1000

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
#define BUTTON_RAW_THRESHOLD     370
#define PRESS_MAX_MS     800     /* bundan uzun basış "basma" sayılmıyor */

uint32_t pb_swipe_touch;
uint32_t pb_swipe_begin;
uint32_t pb_swipe_accept;
uint32_t pb_swipe_short;
uint32_t pb_button_press;
int32_t  pb_swipe_last_dx;
int32_t  pb_swipe_last_dy;

/* Cihaz artık SÜREKLİ DİNLEMİYOR — kullanıcının kararı. Dinlemeyi kayıt
 * butonu başlatıyor; açılışta kapalı. */
static bool s_recording;

static bool     s_pressed;
static int32_t  s_head_raw, s_last_raw;
static uint32_t s_head_ms, s_last_touch_ms;
static bool     s_bu_touch_swiped;

/** Kaydırmayı uygula. `d` ham eksendeki değişim.
 *
 * ham_x soldan sağa AZALDIĞI için (kalibrasyon), sağdan sola kaydırma —
 * yani sayfa çevirme yönü, "sonraki ekran" — ham_x'i ARTIRIYOR. */
static void kaydirmayi_uygula(int32_t d)
{
    pb_swipe_accept++;
    pb_ui_set_screen(d > 0 ? s_active + 1 : s_active - 1);
}

static void swipe_finish(uint32_t now)
{
    if (!s_pressed) return;
    s_pressed = false;

    if (s_bu_touch_swiped) return;

    const int32_t d = s_last_raw - s_head_raw;
    pb_swipe_last_dx = d;

    const int32_t name = d < 0 ? -d : d;
    const uint32_t time = now - s_head_ms;

    /* Kaydırma mı, butona basma mı? Parmak fazla gezmediyse ve dokunuş
     * butonun şeridinde başladıysa BASMA. Kaydırma testinden ÖNCE bakılıyor
     * çünkü eşiği aşmayan her dokunuş zaten kaydırma değil. */
    if (name < SWIPE_THRESHOLD_RAW) {
        if (s_active == PB_SCREEN_LISTEN && s_head_raw >= BUTTON_RAW_THRESHOLD &&
            time <= PRESS_MAX_MS) {
            pb_button_press++;
            printf("  [buton] KABUL  bas_ham=%ld ad=%ld sure=%lums\n",
                   (long)s_head_raw, (long)name, (unsigned long)time);
            pb_ui_set_recording(!s_recording);
        } else {
            pb_swipe_short++;
            /* Teşhis (§9s buton güvenilirliği ölçümü): reddedilen her
             * dokunuşun ham başlangıç değerini yazdır. Eşik (370) ile
             * butonun geometrik sağ kenarı (~368) arasında pay yalnızca
             * ~2 ham birim; bu satır o payın gerçekten yetersiz mi yoksa
             * başka bir sebep mi (süre, ekran) olduğunu ayırt ediyor. */
            printf("  [buton] RED    bas_ham=%ld ad=%ld sure=%lums ekran=%d "
                   "(esik ham>=%d, sure<=%dms)\n",
                   (long)s_head_raw, (long)name, (unsigned long)time, s_active,
                   BUTTON_RAW_THRESHOLD, PRESS_MAX_MS);
        }
        return;
    }

    if (time > SWIPE_MAX_MS) { pb_swipe_short++; return; }

    kaydirmayi_uygula(d);
}

static void swipe_yokla(void)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    /* Ham noktayı doğrudan okuyoruz: eşik ham eksende ölçüldü (yukarıdaki
     * kalibrasyon) ve ham->piksel ölçeği kalibre edilmiş değil. */
    pb_touch_state_t st = pb_touch_read();
    const bool valid = st.ok && st.fingers > 0 && st.p.raw_x < RAW_MAX;

    if (valid) {
        const int32_t raw = (int32_t)st.p.raw_x;
        pb_swipe_touch++;
        pb_swipe_last_dy = (int32_t)st.p.raw_y;   /* teşhis için ham kısa eksen */
        s_last_touch_ms = now;

        if (!s_pressed) {
            s_pressed = true;
            s_bu_touch_swiped = false;
            s_head_raw = s_last_raw = raw;
            s_head_ms = now;
            pb_swipe_begin++;
            return;
        }

        s_last_raw = raw;

        /* Parmak henüz kalkmadan eşiği aştıysa hemen geç: kullanıcı
         * parmağını kaldırana kadar beklemek "tepki vermiyor" hissi veriyor. */
        if (!s_bu_touch_swiped) {
            const int32_t d = s_last_raw - s_head_raw;
            const int32_t name = d < 0 ? -d : d;
            if (name >= SWIPE_THRESHOLD_RAW && now - s_head_ms <= SWIPE_MAX_MS) {
                pb_swipe_last_dx = d;
                s_bu_touch_swiped = true;
                kaydirmayi_uygula(d);
            }
        }
        return;
    }

    /* Okuma yok (ya da panel dışı kare). Dokunmatik ara ara kare düşürüyor;
     * hemen "kalktı" demeden kısa bir pencere bekleniyor, yoksa tek kayıp
     * kare kaydırmayı böler. */
    if (s_pressed && (now - s_last_touch_ms) > PARMAK_BIRAKMA_MS) {
        swipe_finish(now);
    }
}

void pb_ui_create(void)
{
    if (s_built) return;
    s_built = true;

    s_screen[PB_SCREEN_LISTEN] = pb_screen_listen_create();
    s_screen[PB_SCREEN_LOG]  = pb_screen_log_create();

    s_active = PB_SCREEN_LISTEN;
    lv_screen_load(s_screen[s_active]);
    pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_LISTEN);
    pb_lv_invalidate_all();

    /* Açılışta BOŞTA — cihaz sürekli dinlemiyor. */
    s_recording = false;
    pb_screen_listen_set_recording(false);

    s_last_log_name[0] = '\0';
}

int pb_ui_screen(void) { return s_active; }

bool pb_ui_recording(void) { return s_recording; }

void pb_ui_set_recording(bool recording)
{
    if (!s_built || recording == s_recording) return;
    s_recording = recording;
    pb_screen_listen_set_recording(recording);

    /* Kayıt durdurulunca şerit olduğu gibi kalıyor (son duyulanın kaydı) ama
     * yeni kayıtta karışmasın diye başlarken temizleniyor. */
    if (recording && s_active == PB_SCREEN_LISTEN) pb_spec_init();
}

void pb_ui_set_screen(int screen)
{
    if (!s_built) return;
    if (screen < 0 || screen >= PB_SCREEN_COUNT) return;
    if (screen == s_active) return;

    s_active = screen;

    /* ⚠ GEÇİŞ ANİMASYONU YOK — bilerek. `lv_screen_load_anim` her karede tüm
     * ekranı geçersizleştirir; bizde bir kare, sahip olunan her dilimin
     * panele yeniden basılması (dilim başına 44 KB QSPI) demek. 60 Hz'de
     * imkânsız, 10 Hz'de de takılarak akan bir animasyon anlık geçişten
     * kötü görünürdü. */
    lv_screen_load(s_screen[s_active]);

    if (s_active == PB_SCREEN_LOG) {
        /* Günlük tam genişlik: sağdaki iki dilim de LVGL'in oluyor. */
        pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_ALL);
        pb_screen_log_refresh(s_frame_rate, s_inference, s_overrun);
    } else {
        pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_LISTEN);
        /* Spektrogram bölgesini günlük ekranı boyamıştı; şeridi sıfırla ki
         * altında eski yazı kalmasın. Sütunlar akmaya devam edip dolduracak. */
        pb_spec_init();
    }

    pb_lv_invalidate_all();
}

void pb_ui_next(void)
{
    pb_ui_set_screen((s_active + 1) % PB_SCREEN_COUNT);
}

void pb_ui_log_add(const char *name, const char *latin, float confidence)
{
    pb_screen_log_add(name, latin, confidence);
}

void pb_ui_update(const pb_result_view_t *g)
{
    if (!g || !s_built) return;

    s_frame_rate = g->frame_rate;
    s_inference  = g->inference;
    s_overrun  = g->overrun;

    pb_screen_listen_update(g);

    /* Karar "TANINDI" dediğinde günlüğe yaz — ama yalnızca tür DEĞİŞTİYSE.
     * Karar kuralı bir türü PB_DECISION_HOLD_MS (5 s) boyunca ekranda tutuyor ve
     * bu fonksiyon 4 Hz çağrılıyor, yani aynı tespit ~20 kez düşüyor.
     * `ekran_gunluk` aynı türü üst üste görünce zaten yeni satır açmıyor;
     * buradaki kontrol o çağrıyı hiç yapmamak için. Kip TUR'dan çıkınca
     * sıfırlanıyor ki aynı tür ikinci kez duyulduğunda yeniden yazılsın. */
    if (g->mode == PB_DECISION_SPECIES && g->species_name) {
        if (strncmp(s_last_log_name, g->species_name, sizeof(s_last_log_name) - 1) != 0) {
            snprintf(s_last_log_name, sizeof(s_last_log_name), "%s", g->species_name);
            pb_screen_log_add(g->species_name, g->top3_latin[0], g->confidence);
        }
    } else {
        s_last_log_name[0] = '\0';
    }
}

void pb_ui_tick(void)
{
    if (!s_built) { pb_lv_tick(); return; }

    swipe_yokla();

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_active == PB_SCREEN_LOG && (now - s_last_refresh) >= REFRESH_MS) {
        s_last_refresh = now;
        pb_screen_log_refresh(s_frame_rate, s_inference, s_overrun);
    }

    pb_lv_tick();
}
