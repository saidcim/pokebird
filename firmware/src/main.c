/**
 * PokeBird — İstanbul kuş sesi tanıma cihazı
 *
 * M1: mikrofon bring-up.
 *
 * Bu aşamanın amacı bir özellik teslim etmek değil, planın en riskli
 * varsayımını ölçmek: kart üzerindeki analog MEMS mikrofon, aynı küçük PCB'de
 * duran ekran, QSPI hattı ve anahtarlamalı güç kaynağının gürültüsü altında
 * kuş sesi tanımaya yetecek kadar temiz mi?
 *
 * USB seri porttan komut alır:
 *   i  cihaz kimliği ve saat yapılandırmasını yazdır
 *   n  gürültü tabanı ölçümü (sessiz ortamda çalıştırın)
 *   e  EMI taraması: arka ışık kapalı / sabit açık / PWM'li durumlarda gürültü
 *   g  mikrofon kazancını değiştir (0-7)
 *   r  2 saniye kayıt al ve PC'ye aktar (tools/capture_wav.py ile yakalayın)
 *   ?  yardım
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"

#include "board_config.h"
#include "hal/audio_i2s.h"
#include "hal/es8311.h"
#include "hal/i2c_bus.h"
#include "hal/touch.h"
#include "hal/display/LCD_3in49.h"
#include "hal/display/lcd_blit.h"
#include "hal/display/qspi_pio.h"
#include "dsp/fft.h"
#include "dsp/mel.h"
#include "dsp/gate.h"
#include "ui/spectrogram.h"
#include "ui/lv_port.h"
#include "ui/theme.h"
#include "ai/species_net.h"
#include "ai/binary_net.h"
#include "ai/recognizer.h"
#include "ai/decision.h"
#include "ai/classes.h"
#include "ai/validation_set.h"
#include "ai/binary_validation_set.h"
#include "ui/interface.h"
#include "lvgl.h"

void pb_display_dma_init(void);   /* hal/display/dev_config.c */

/* ── Derleme zamanı donanım kontrolleri ───────────────────────────────────
 * Yanlış board seçilirse bu hatalar derlemeyi durdurur. Aksi hâlde kod
 * sessizce derlenir ve hata ancak kartta, GPIO40'ın hiç kıpırdamamasıyla
 * ortaya çıkar — orada bulması çok pahalı. */
static_assert(PICO_RP2350A == 0,
              "RP2350B secilmedi. PICO_BOARD=pokebird_rp2350b olmali "
              "(boards/pokebird_rp2350b.h).");
static_assert(NUM_BANK0_GPIOS >= 48,
              "48 GPIO bekleniyor. BAT_ADC (GPIO40), SD_CS (GPIO31) ve "
              "bos baslik pinleri (41-47) RP2350A'da yok.");
static_assert(PB_PIN_BAT_ADC < NUM_BANK0_GPIOS,
              "BAT_ADC pini GPIO araliginin disinda.");
static_assert(PICO_FLASH_SIZE_BYTES == 16 * 1024 * 1024,
              "16 MB flash bekleniyor (PY25Q128HA).");

/* Karar kuralı negatif sınıfın indeksini sabit olarak biliyor (ai/decision.h);
 * sınıf tablosu yeniden üretilip sınıf sayısı değişirse burada durmalı, çünkü
 * kayması "gürültüyü kuş sanmak" demek ve hiçbir yerde hata vermez. */
static_assert(PB_DECISION_NEGATIVE_CLASS == PB_CLASS_COUNT - 1,
              "Negatif sinif indeksi kaydi: ai/decision.h ile ai/classes.h "
              "uyusmuyor (tools/class_table.py yeniden mi calisti?).");

#define BL_PWM_WRAP     2048
#define ES8311_I2C_ADDR 0x18

/**
 * Yakalama parçası — akıştan tek seferde okunan en büyük öbek.
 *
 * Burada eskiden 2 saniyelik bitişik bir tampon vardı: 24 kHz × 16 bit =
 * 96.000 bayt, tek başına bss'in yarısı. Onu kullanan teşhis komutlarının
 * hiçbirinin 2 saniyeyi bir arada görmesi gerekmiyordu — hepsi ya biriktirici
 * (RMS, tepe, DC) ya da pencere pencere çalışıyor. M3'ün sürekli yakalama
 * halkası geldiğinden beri (hal/audio_i2s.c) veri kesintisiz biçimde parça
 * parça okunabiliyor, bu yüzden tampon parça boyuna indirildi.
 *
 * Kazanç 96.000 → 4.096 bayt. M6'nın TFLM arena'sı (180 KB) ancak bu yer
 * açıldıktan sonra sığıyor.
 */
#define CHUNK_SAMPLES   PB_AUDIO_MAX_READ       /* 2048 ornek = 4096 bayt */
static int16_t s_chunk[CHUNK_SAMPLES];

/* 'r' komutunun kayıt uzunluğu. */
#define CAPTURE_SECONDS 2
#define CAPTURE_SAMPLES (PB_SAMPLE_RATE * CAPTURE_SECONDS)

static const pb_audio_cfg_t s_audio_cfg = {
    .mclk_freq   = PB_MCLK_RATE,
    .sample_freq = PB_SAMPLE_RATE,
    .res_in      = PB_BITS_PER_SAMPLE,
    .res_out     = PB_BITS_PER_SAMPLE,
};

static uint8_t s_mic_gain = PB_MIC_GAIN;

/* â”€â”€ Ekran â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * M1'de ekranı kullanmıyoruz ama arka ışık gürültü ölçümünün bir değişkeni:
 * AP3032 yükseltici ve PWM, mikrofonun hemen yanında anahtarlama yapıyor. */

/**
 * Arka ışık — düz GPIO, PWM YOK.
 *
 * DOĞRULANMIŞ DAVRANIŞ (etkileşimli 'b' testi, kart üzerinde ölçüldü):
 *     BL_EN (GPIO37) = 1  ve  LCD_BL (GPIO36) = 0   ->  IŞIK YANAR
 * Yani LCD_BL aktif-düşük. rsvpnano'daki çalışan sürücü de aynısını söylüyor
 * ("active-low PWM; lower duty is brighter"), Waveshare'in kendi kodu da
 * (pwm_set_chan_level(slice, CHAN_A, 100 - Value)).
 *
 * NEDEN PWM KULLANMIYORUZ:
 * PWM ile duty %0 — elektriksel olarak pinin sürekli LOW olması, yani yukarıda
 * ışığı yakan durumun aynısı — ışığı YAKMIYOR. Düz GPIO ile LOW yakıyor.
 * Demek ki PWM çevre birimi bu pini beklediğimiz gibi sürmüyor (muhtemelen
 * RP2350B'de GPIO36'nın slice eşlemesiyle ilgili). Kök nedeni kovalamak yerine
 * çalıştığı doğrulanmış mekanizmayı kullanıyoruz.
 *
 * MALİYETİ: parlaklık ayarı yok, sadece aç/kapa. Şimdilik önemsiz — M1'deki
 * EMI taraması arka ışığın mikrofona etkisinin +0.2 dB olduğunu gösterdi,
 * yani parlaklığı kısmak için akustik bir gerekçe de yok. Kademeli parlaklık
 * istenirse (M7 ayarlar ekranı) PWM sorunu o zaman ayrıca çözülür.
 */
/**
 * Güç mandalını kilitle.
 *
 * Waveshare'in örneği ekrandan önce `DEV_Module_Init()` çağırıyor; biz onu
 * kendi HAL'imizle çakışmasın diye hiç almadık. İçindeki tek kritik iş
 * SYS_EN'i yüksek tutmak: kart bu mandalla ayakta duruyor, örneğin core1'i
 * `DEV_Digital_Write(SYS_EN, 0)` ile kapanma yapıyor.
 *
 * Panelin mantık/IO beslemesi bu mandalın arkasındaysa, panel kendi
 * taramasını sürdürse bile ana bilgisayar arayüzü beslemesiz kalır ve
 * QSPI'den gelen hiçbir komutu duymaz — gözlediğimiz tabloya birebir uyuyor.
 * Doğrulanmış değil; ucuz ve zararsız olduğu için deniyoruz.
 */
static void power_latch_init(void) {
    gpio_init(PB_PIN_SYS_EN);
    gpio_set_dir(PB_PIN_SYS_EN, GPIO_OUT);
    gpio_put(PB_PIN_SYS_EN, 1);     /* 1 = acik kal. 0 KAPATIR. */
}

static void backlight_init(void) {
    gpio_init(PB_PIN_BL_EN);
    gpio_set_dir(PB_PIN_BL_EN, GPIO_OUT);
    gpio_put(PB_PIN_BL_EN, 1);

    gpio_init(PB_PIN_LCD_BL);
    gpio_set_dir(PB_PIN_LCD_BL, GPIO_OUT);
    gpio_put(PB_PIN_LCD_BL, 0);         /* aktif-düşük: 0 = yanık */
}

static void backlight_set(bool on) {
    gpio_put(PB_PIN_BL_EN, on ? 1 : 0);
    gpio_put(PB_PIN_LCD_BL, on ? 0 : 1);
}

/* ── Ölçüm ───────────────────────────────────────────────────────────────
 * Tam ölçek 16-bit için referans 32768. dBFS = 20*log10(rms/32768). */

typedef struct {
    double  rms;
    double  dbfs;
    int32_t peak;
    double  dc_offset;
} audio_stats_t;

/* log10 için math.h yerine basit bir yaklaşım kullanmıyoruz; SDK'nın
 * optimize edilmiş log10f'i zaten bağlı ve M3'te CMSIS-DSP gelecek. */
#include <math.h>

/**
 * Akış biriktiricisi — istatistik tek geçişte.
 *
 * Eski `compute_stats` İKİ geçişliydi: önce DC ortalamasını buluyor, sonra
 * aynı diziyi ikinci kez tarayıp o ortalamaya göre RMS hesaplıyordu. Bu,
 * örneklerin tamamının bellekte durmasını şart koşuyordu. Parça parça
 * okurken ikinci geçiş için veri yok — parça işlendikten sonra üzerine
 * yenisi yazılıyor.
 *
 * Çözüm varyans özdeşliği:  rms² = sumsq/n − (sum/n)²
 * Böylece ham toplamlar biriktirilip DC ancak sonda çıkarılabiliyor.
 * `double` ile güvenli: 48.000 örnek × 32768² ≈ 5,2e13, double'ın tam sayı
 * kesinliği 9e15'e kadar. Host tarafında iki yol aynı veriyle karşılaştırıldı:
 * karttan alınan gerçek kayıtta sapma 3,6e-14 dB, DC 20000 üzerine ±3 AC gibi
 * fark almayı zorlayan uydurma bir durumda bile 3,6e-8 dB. (Kabul sınırı
 * 0,1 dB idi.)
 */
typedef struct {
    double   sum;
    double   sumsq;
    int32_t  peak;
    uint32_t n;
} stats_acc_t;

static void stats_add(stats_acc_t *a, const int16_t *x, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double v = (double)x[i];
        a->sum   += v;
        a->sumsq += v * v;
        int32_t m = x[i] < 0 ? -(int32_t)x[i] : (int32_t)x[i];
        if (m > a->peak) a->peak = m;
    }
    a->n += n;
}

static audio_stats_t stats_finish(const stats_acc_t *a) {
    audio_stats_t st = { 0 };
    if (a->n == 0) return st;

    double mean = a->sum / (double)a->n;
    double var  = a->sumsq / (double)a->n - mean * mean;
    if (var < 0.0) var = 0.0;        /* yuvarlama sıfırın altına düşürebilir */

    st.dc_offset = mean;
    st.rms       = sqrt(var);
    st.peak      = a->peak;
    st.dbfs      = (st.rms > 0.0) ? 20.0 * log10(st.rms / 32768.0) : -999.0;
    return st;
}

/** Bellekteki küçük bir tampon için kolaylık sarmalayıcısı. */
static audio_stats_t compute_stats(const int16_t *x, uint32_t n) {
    stats_acc_t acc = { 0 };
    stats_add(&acc, x, n);
    return stats_finish(&acc);
}

/**
 * Gürültü tabanını yüzdelik ile ölç.
 *
 * Düz RMS, ölçüm boyunca olan tek bir kapı sesi ya da öksürükle yukarı
 * çekiliyor — oda hiçbir zaman tam sessiz değil. Sinyali kısa pencerelere
 * bölüp pencere RMS'lerinin 10. yüzdeliğini almak, geçici seslere karşı
 * dayanıklı ve "en sessiz an" için çok daha dürüst bir sayı veriyor.
 */
/* Pencere sayısı 64'ten 48'e indi ve pencere uzunluğu parça sınırına hizalandı.
 *
 * Eskiden 2 saniye tek parça okunup 64'e bölünüyordu (pencere 750 örnek).
 * Akışta pencerenin okuma parçasına hizalı olması gerekiyor, yoksa pencereler
 * parça sınırını aşar. 1024 örneklik 48 pencere = 49.152 örnek = 2,048 s.
 *
 * YAN ETKİ — belgelenmeli: yüzdelik indeksi `count/10` olduğu için 64 pencerede
 * 6. eleman (%9,4), 48 pencerede 4. eleman (%8,3) seçiliyor. Yani "10.
 * yüzdelik" biraz kaydı. Sonuç diagnostik; M1'in -36 dBFS tabanıyla
 * karşılaştırma yaparken bu kayma akılda tutulmalı. */
#define NOISE_WINDOWS 48
#define NOISE_WIN_LEN 1024      /* 42,7 ms @ 24 kHz — parça boyunun böleni */

static double window_rms(const int16_t *x, uint32_t n) {
    stats_acc_t acc = { 0 };
    stats_add(&acc, x, n);
    return stats_finish(&acc).rms;
}

/** Sıralayıp 10. yüzdeliği döndür (liste yerinde değiştirilir). */
static double percentile10(double *v, uint32_t count) {
    /* küçükten büyüğe (count küçük, basit ekleme sıralaması yeterli) */
    for (uint32_t i = 1; i < count; i++) {
        double t = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > t) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
    return v[count / 10];
}

static void print_stats(const char *label, const audio_stats_t *st,
                        const pb_capture_result_t *cap) {
    printf("  %-22s RMS %8.1f  %7.1f dBFS  tepe %6ld  DC %8.1f",
           label, st->rms, st->dbfs, (long)st->peak, st->dc_offset);
    if (cap->fifo_overrun) printf("   [!] ORNEK DUSTU");
    if (cap->timed_out)    printf("   [!] SAAT YOK");
    printf("\n");
}

/**
 * Akıştan `total` örnek oku ve yalnızca istatistik biriktir — ham veri
 * saklanmaz, her parça bir sonrakinin üzerine yazılır.
 *
 * TUZAK: flush YALNIZCA döngüden önce, bir kez çağrılıyor. Parça başına
 * `pb_audio_capture` çağırmak cazip görünüyor (imzası tam uyuyor) ama o
 * fonksiyon flush + oku sarmalayıcısı: her çağrıda birikmişi atar. Parça
 * parça çağrılırsa parçalar ARASINDAKİ örnekler düşer ve ölçüm sessizce
 * bozulur — `fifo_overrun` bu kaybı bildirmez, çünkü halka taşmamıştır,
 * biz attırmışızdır.
 */
static pb_capture_result_t stream_stats(uint32_t total, audio_stats_t *out) {
    pb_capture_result_t res = { 0 };
    stats_acc_t acc = { 0 };

    pb_audio_stream_flush();
    while (res.samples < total) {
        uint32_t want = total - res.samples;
        if (want > CHUNK_SAMPLES) want = CHUNK_SAMPLES;

        pb_capture_result_t part = pb_audio_stream_read(s_chunk, want, 1000);
        res.samples      += part.samples;
        res.fifo_overrun |= part.fifo_overrun;
        res.timed_out    |= part.timed_out;
        stats_add(&acc, s_chunk, part.samples);

        if (part.timed_out) break;
    }
    if (out) *out = stats_finish(&acc);
    return res;
}

/* Ölçüm alırken arka ışığı verilen duruma getirip bekle (güç hattı otursun) */
static audio_stats_t measure_with_backlight(bool enable,
                                            pb_capture_result_t *cap_out) {
    backlight_set(enable);
    sleep_ms(250);

    audio_stats_t st;
    pb_capture_result_t cap = stream_stats(PB_SAMPLE_RATE / 2, &st);  /* 0.5 s */
    if (cap_out) *cap_out = cap;
    return st;
}

/* â”€â”€ Komutlar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static void cmd_info(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    printf("\n--- Cihaz ---\n");
    printf("  MCU          RP2350%s @ %lu Hz\n",
           PICO_RP2350A ? "A" : "B", (unsigned long)clock_get_hz(clk_sys));
    printf("  Flash        %d MB\n", PICO_FLASH_SIZE_BYTES / (1024 * 1024));
    printf("  Kart ID      ");
    for (size_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) printf("%02x", id.id[i]);
    printf("\n");
    printf("--- Ses ---\n");
    printf("  MCLK         %lu Hz (PIO, GPIO%d)\n",
           (unsigned long)s_audio_cfg.mclk_freq, PB_PIN_I2S_MCLK);
    printf("  Ornekleme    %lu Hz (ES8311 master, MCLK/256)\n",
           (unsigned long)s_audio_cfg.sample_freq);
    printf("  Mikrofon     analog MEMS -> ES8311 ADC, kazanc %u\n", s_mic_gain);
    printf("  ES8311 ID    0x%04x %s\n", es8311_read_id(),
           pb_i2c_probe(ES8311_I2C_ADDR) ? "(I2C yanit veriyor)" : "(I2C YANIT YOK)");
    printf("\n");
}

static void cmd_noise(void) {
    const uint32_t total = (uint32_t)NOISE_WINDOWS * NOISE_WIN_LEN;

    printf("\nGurultu tabani olcumu (%.2f s). Ortami sessiz tutun...\n",
           (double)total / PB_SAMPLE_RATE);
    backlight_set(false);
    sleep_ms(300);

    /* Pencere pencere oku: her pencere hem genel istatistiğe eklenir hem de
     * kendi RMS'iyle yüzdelik listesine girer. Ham veri saklanmıyor. */
    pb_capture_result_t cap = { 0 };
    stats_acc_t acc = { 0 };
    double rms_list[NOISE_WINDOWS];
    uint32_t count = 0;

    pb_audio_stream_flush();        /* bir KEZ, döngüden önce (bkz. stream_stats) */
    for (uint32_t w = 0; w < NOISE_WINDOWS; w++) {
        pb_capture_result_t part =
            pb_audio_stream_read(s_chunk, NOISE_WIN_LEN, 1000);
        cap.samples      += part.samples;
        cap.fifo_overrun |= part.fifo_overrun;
        cap.timed_out    |= part.timed_out;

        if (part.samples < NOISE_WIN_LEN) break;    /* saat yok — eksik pencere */

        stats_add(&acc, s_chunk, part.samples);
        rms_list[count++] = window_rms(s_chunk, part.samples);
    }
    audio_stats_t st = stats_finish(&acc);

    printf("  Yakalanan    %lu / %lu ornek  (%lu / %d pencere)\n",
           (unsigned long)cap.samples, (unsigned long)total,
           (unsigned long)count, NOISE_WINDOWS);
    print_stats("tum pencere (RMS)", &st, &cap);

    double floor_rms = (count > 0) ? percentile10(rms_list, count) : 0.0;
    double floor_db  = (floor_rms > 0.0)
                     ? 20.0 * log10(floor_rms / 32768.0) : -999.0;
    printf("  %-22s RMS %8.1f  %7.1f dBFS   <- gecici seslere dayanikli\n",
           "gurultu tabani (P10)", floor_rms, floor_db);

    if (cap.timed_out) {
        printf("\n  [!] ES8311 saat uretmiyor. Kontrol: MCLK cikiyor mu, codec\n");
        printf("      I2C'de yanit veriyor mu, master mod register'i yazildi mi.\n");
    } else if (st.rms < 1.0) {
        printf("\n  [!] Sinyal tamamen sifir. Mikrofon yolu acilmamis olabilir\n");
        printf("      (ES8311 REG14 analog mic / PGA ayarlari).\n");
    } else {
        printf("\n  Yorum: %.0f dBFS taban, ", floor_db);
        if (floor_db < -60.0)      printf("iyi — kus sesi tanima icin yeterli.\n");
        else if (floor_db < -45.0) printf("kabul edilebilir, ama EMI taramasi ('e') yapin.\n");
        else                       printf("YUKSEK — ortam sesi mi devre mi, 'g' ile\n"
                                          "         kazanci degistirip bakin (olcum kazancla\n"
                                          "         olcekleniyorsa gurultu akustiktir).\n");
    }
    printf("\n");
}

/* Canlı seviye göstergesi. Mikrofonun gerçekten ses duyduğunu doğrulamanın en
 * pratik yolu: el çırpın, ıslık çalın, konuşun — çubuk anında tepki vermeli. */
static void cmd_level_meter(void) {
    printf("\nCanli seviye. El cirpin / konusun. Cikmak icin bir tusa basin.\n\n");
    /* 2048 ornek = 85 ms. Eskiden 100 ms'ti; parca boyuna indirildi.
     * Canli gosterge oldugu icin her turda en tazeye atlamasi zaten isteniyor,
     * bu yuzden flush+oku sarmalayicisi (pb_audio_capture) burada DOGRU olan. */
    const uint32_t win = CHUNK_SAMPLES;

    while (getchar_timeout_us(0) < 0) {
        pb_capture_result_t cap = pb_audio_capture(s_chunk, win);
        if (cap.samples == 0) break;
        audio_stats_t st = compute_stats(s_chunk, cap.samples);

        int bars = (int)((st.dbfs + 80.0) / 2.0);   /* -80 dBFS -> 0, 0 dBFS -> 40 */
        if (bars < 0) bars = 0;
        if (bars > 40) bars = 40;

        printf("\r  %6.1f dBFS  tepe %5ld  [", st.dbfs, (long)st.peak);
        for (int i = 0; i < 40; i++) putchar(i < bars ? '#' : ' ');
        printf("]");
        stdio_flush();   /* fflush() newlib stdio kilitlerini cekiyor;
                          * SDK'nin kendi flush'i o bagimliligi getirmiyor */
    }
    printf("\n\n");
}

static void cmd_emi_sweep(void) {
    printf("\nEMI taramasi — ekran arka isiginin mikrofona etkisi.\n");
    printf("Her olcum 0.5 s. Ortami sessiz tutun.\n\n");

    pb_capture_result_t cap;
    audio_stats_t off    = measure_with_backlight(false, &cap);
    print_stats("arka isik KAPALI", &off, &cap);

    audio_stats_t full   = measure_with_backlight(true, &cap);
    print_stats("tam acik (PWM yok)", &full, &cap);

    audio_stats_t pwm50  = measure_with_backlight(true, &cap);
    print_stats("PWM %50", &pwm50, &cap);

    audio_stats_t pwm10  = measure_with_backlight(true, &cap);
    print_stats("PWM %10", &pwm10, &cap);

    backlight_set(false);

    double worst = pwm50.dbfs > pwm10.dbfs ? pwm50.dbfs : pwm10.dbfs;
    if (full.dbfs > worst) worst = full.dbfs;
    double delta = worst - off.dbfs;

    printf("\n  En kotu durum, kapaliya gore %+.1f dB.\n", delta);
    if (delta < 3.0) {
        printf("  Arka isik mikrofonu bozmuyor. Ekran acik dinleme sorunsuz.\n");
    } else if (delta < 10.0) {
        printf("  Olcülebilir etki var. PWM frekansini degistirmeyi ya da dinleme\n");
        printf("  aninda parlakligi sabitlemeyi degerlendirin.\n");
    } else {
        printf("  [!] Ciddi girisim. Secenekler: PWM yerine sabit parlaklik,\n");
        printf("      PWM frekansini kaydirma, ya da bos GPIO'lardan (12-19)\n");
        printf("      harici I2S MEMS mikrofon (plan §10 risk tablosu).\n");
    }
    printf("\n");
}

static void cmd_gain(void) {
    printf("\nKazanc (0-7), su an %u. Yeni deger girin: ", s_mic_gain);
    int c = getchar_timeout_us(10 * 1000 * 1000);
    if (c < '0' || c > '7') {
        printf("iptal\n\n");
        return;
    }
    s_mic_gain = (uint8_t)(c - '0');
    es8311_microphone_gain_set((es8311_mic_gain_t)s_mic_gain);
    printf("%u olarak ayarlandi\n\n", s_mic_gain);
}

/* Ham örnekleri PC'ye aktar. Basit ve kendini tanıtan bir çerçeve kullanıyoruz;
 * tools/capture_wav.py bunu WAV'a çeviriyor. */
/**
 * Kayıt artık akış hâlinde: 2 saniye önce belleğe alınıp sonra yazdırılmıyor,
 * parça parça okunup anında aktarılıyor. Üç ayrıntı kritik:
 *
 * 1. SIRA. `tools/capture_wav.py` başlıktaki `samples=N`'i okuyup N örnek
 *    bekliyor (capture_wav.py:112), yani başlık örneklerden ÖNCE gitmeli.
 *    Ama istatistik ancak akış bitince hazır olur. Bu yüzden yeni sıra:
 *    başlık → örnekler → #WAV-END → istatistik. (Araç #WAV-END'de okumayı
 *    bıraktığı için istatistiği o göstermez; seri terminalde görünür.)
 *
 * 2. SAAT KONTROLÜ BAŞLIKTAN ÖNCE. Başlığı yazdıktan sonra çekilmek yok:
 *    `samples=N` sözü verilmiş olur. Bu yüzden ilk parça başlıktan önce
 *    okunuyor; saat yoksa hiç başlık yazmadan çıkıyoruz.
 *
 * 3. GERÇEK ZAMANA YETİŞMEK. Yazdırma okumayla iç içe geçtiği için aktarım
 *    gerçek zamandan yavaş kalırsa halka (170 ms) taşar ve WAV'da kopukluk
 *    olur. Kartta ölçüldü: CDC 276 KB/s, ondalık biçimde gereken 102 KB/s —
 *    2,7 kat pay var. Yine de her parçanın `fifo_overrun`'ı toplanıp sonda
 *    yüksek sesle bildiriliyor: bu projede sessiz bozulma iki kez pahalıya
 *    patladı (lastsession.md §5.10), kopukluk sessizce geçmemeli.
 */
static void cmd_record(void) {
    printf("\nKayit basliyor (%d s)...\n", CAPTURE_SECONDS);

    pb_capture_result_t cap = { 0 };
    stats_acc_t acc = { 0 };

    /* Saat var mı? İlk parçayı başlıktan ÖNCE oku (yukarıdaki 2. madde). */
    pb_audio_stream_flush();        /* bir KEZ; sonrasında sadece stream_read */
    uint32_t first = CAPTURE_SAMPLES < CHUNK_SAMPLES ? CAPTURE_SAMPLES : CHUNK_SAMPLES;
    pb_capture_result_t part = pb_audio_stream_read(s_chunk, first, 1000);
    if (part.samples == 0) {
        printf("Kayit alinamadi (ES8311 saat uretmiyor).\n\n");
        return;
    }

    printf("#WAV-BEGIN rate=%d channels=1 bits=16 samples=%lu\n",
           PB_SAMPLE_RATE, (unsigned long)CAPTURE_SAMPLES);

    /* Satir basina 32 ornek, isaretli ondalik — bicim degismedi, PC tarafi
     * oldugu gibi calisiyor. */
    uint32_t emitted = 0;
    for (;;) {
        cap.samples      += part.samples;
        cap.fifo_overrun |= part.fifo_overrun;
        cap.timed_out    |= part.timed_out;
        stats_add(&acc, s_chunk, part.samples);

        for (uint32_t i = 0; i < part.samples; i++, emitted++) {
            printf("%d%c", s_chunk[i], ((emitted % 32) == 31) ? '\n' : ' ');
        }

        if (part.timed_out || cap.samples >= CAPTURE_SAMPLES) break;

        uint32_t want = CAPTURE_SAMPLES - cap.samples;
        if (want > CHUNK_SAMPLES) want = CHUNK_SAMPLES;
        part = pb_audio_stream_read(s_chunk, want, 1000);
    }
    if (emitted % 32) printf("\n");
    printf("#WAV-END\n");

    audio_stats_t st = stats_finish(&acc);
    print_stats("kayit", &st, &cap);
    if (cap.samples != CAPTURE_SAMPLES) {
        printf("  [!] %lu / %lu ornek gonderildi — PC tarafi EKSIK diyecek.\n",
               (unsigned long)cap.samples, (unsigned long)CAPTURE_SAMPLES);
    }
    if (cap.fifo_overrun) {
        printf("  [!] ORNEK DUSTU: aktarim gercek zamana yetisemedi, kayitta\n");
        printf("      kopukluk var. WAV'i olcum icin KULLANMAYIN.\n");
    }
    printf("\n");
}


/* M2 doğrulaması: mikrofondan gelen ses ekranda akıyor mu.
 * Ekran QSPI'ye tek sütun yazarak güncelleniyor (bkz. ui/spectrogram.c). */
static void cmd_spectrogram(void) {
    printf("\nCanli spektrogram. Cikmak icin bir tusa basin.\n");
    backlight_set(true);
    pb_spec_init();

    uint8_t bins[PB_SPEC_HEIGHT];
    while (getchar_timeout_us(0) < 0) {
        pb_capture_result_t cap = pb_audio_capture(s_chunk, PB_FFT_SIZE);
        if (cap.samples < PB_FFT_SIZE) break;
        /* -75 dBFS taban: M1'de olculen ~-36 dBFS oda gurultusunun altinda,
         * boylece sessizlik siyah kaliyor ama zayif sesler hala goruluyor. */
        pb_fft_spectrum(s_chunk, bins, PB_SPEC_HEIGHT, -75.0f);
        pb_spec_push_column(bins, PB_SPEC_HEIGHT);
    }
    printf("cikildi\n\n");
}

/* Bit-bang yolu asagida tanimli; ekran testi PIO yolunun yanina onu da
 * koyabilsin diye burada bildiriliyor. */
static void bb_pins_setup(void);
static void bb_panel_init(void);
static void bb_ekrani_boya(uint16_t color);

#define PB_INIT_BITBANG 99   /* cmd_display_test icin sanal varyant */

/** Bekleyen seri girisi temizle — onceki adimdan kalan tuslar yanlis
 *  varyanti "kazanan" gostermesin. */
static void drain_stdin(void) {
    while (getchar_timeout_us(0) >= 0) { }
}

/**
 * Ekran testi — hangi panel başlatma dizisi doğru görüntü veriyor?
 *
 * Belirti: ekran yanıyor ama her pikselin farklı renk olduğu, hiç gitmeyen
 * bir karıncalanma var. Bu, GRAM'a kayık veri yazıldığının klasik işareti:
 * en olası sebep piksel biçiminin (COLMOD, 0x3A) hiç ayarlanmamış olması —
 * biz piksel başına 2 bayt yolluyoruz, panel reset varsayılanında başka bir
 * genişlik bekliyor.
 *
 * Seri porttan ekranı göremediğimiz için (bkz. lastsession.md §5.9) üç
 * hipotezi tek firmware'e koyup sırayla deniyoruz; düz renk gördüğünüzde
 * tuşa basıyorsunuz ve cihaz hangi varyantta olduğunu kendisi yazıyor.
 */
static void cmd_display_test(void) {
    printf("\nEkran testi — panel baslatma varyantlari (etkilesimli).\n");
    printf("EKRANA BAKIN. Ekran DUZ RENK doldugunda bir tusa basin.\n");
    printf("Karincalanma devam ediyorsa hicbir sey yapmayin, sonraki varyanta gecer.\n\n");
    backlight_set(true);

    const struct { int varyant; const char *name; } attempts[] = {
        { LCD_3IN49_INIT_FULL,      "satici tablosu + SLPOUT/MADCTL/COLMOD(RGB565)/DISPON" },
        { LCD_3IN49_INIT_MINIMAL,   "sadece DCS kuyrugu (rsvpnano ile ayni)" },
        { LCD_3IN49_INIT_NO_COLMOD, "satici tablosu + SLPOUT/DISPON, COLMOD YOK (kontrol)" },
        { PB_INIT_BITBANG,          "BIT-BANG: PIO/DMA/satici surucusu tamamen devre disi" },
    };

    const struct { const char *name; uint16_t color; } colors[] = {
        { "kirmizi", 0xF800 },
        { "yesil",   0x07E0 },
        { "mavi",    0x001F },
        { "beyaz",   0xFFFF },
    };

    for (size_t v = 0; v < sizeof(attempts) / sizeof(attempts[0]); v++) {
        printf("  [%u] %s\n", (unsigned)(v + 1), attempts[v].name);
        const bool bitbang = (attempts[v].varyant == PB_INIT_BITBANG);
        if (bitbang) {
            pio_sm_set_enabled(qspi.pio, qspi.sm, false);
            bb_pins_setup();
            bb_panel_init();
        } else {
            LCD_3IN49_InitVariant(attempts[v].varyant);
        }
        backlight_set(true);
        drain_stdin();

        for (size_t r = 0; r < sizeof(colors) / sizeof(colors[0]); r++) {
            printf("        %s\n", colors[r].name);
            if (bitbang) bb_ekrani_boya(colors[r].color);
            else         pb_lcd_fill(colors[r].color);

            /* Renk basildiktan sonra 1.5 s bakma suresi */
            for (int t = 0; t < 15; t++) {
                if (getchar_timeout_us(0) >= 0) {
                    printf("\n  >>> DUZGUN CALISAN VARYANT: [%u] %s <<<\n",
                           (unsigned)(v + 1), attempts[v].name);
                    printf("  (o anda ekranda: %s)\n\n", colors[r].name);
                    if (bitbang) {
                        printf("  Bit-bang yolu calisiyor, PIO yolu calismiyor:\n");
                        printf("  hata PIO/DMA tarafinda. Yon karesi atlandi.\n\n");
                        QSPI_PIO_Restore(qspi);   /* pinleri/SM'i PIO'ya geri ver */
                        return;
                    }
                    /* Yon kontrolu: panelin (0,0) kosesine 20x20 beyaz kare.
                     * Cihazi yatay tuttugunuzda karenin nerede goruldugu,
                     * ui/spectrogram.c'deki yon cevirimini dogrular. */
                    pb_lcd_fill(0x0000);
                    static uint16_t frame[20 * 20];
                    for (int i = 0; i < 20 * 20; i++) frame[i] = 0xFFFF;
                    pb_lcd_blit(0, 0, 20, 20, frame);
                    printf("  panel (0,0) konumuna 20x20 beyaz kare cizildi —\n");
                    printf("  cihazi USB soketi ASAGI bakacak sekilde tutun ve\n");
                    printf("  karenin hangi kosede oldugunu soyleyin.\n\n");
                    return;
                }
                sleep_ms(100);
            }
        }
        /* Bit-bang varyanti pinleri SIO'ya alip SM'i kapatiyor. Geri
         * vermezsek BUNDAN SONRAKI her ekran testi sahte bicimde "bozuk"
         * gorunur — bu tuzak bir oturumu yanilti (§9n). */
        if (bitbang) QSPI_PIO_Restore(qspi);
        printf("\n");
    }

    printf("  Hicbir varyantta tus basilmadi — uc dizinin ucu de duzgun\n");
    printf("  goruntu vermedi. Sorun baslatma dizisinde degil, veri yolunda.\n\n");
}

/* ── Bit-bang QSPI — PIO'yu denklemden çıkarmak için ──────────────────────
 * Hattı doğrudan CPU ile, yavaşça sürüyoruz. Amaç PIO programını şüpheli
 * listesinden silmek: bit-bang panele ulaşıyor ama PIO ulaşmıyorsa hata
 * PIO'dadır; ikisi de ulaşmıyorsa hata kabloda/pinde/panelde.
 * Ayrıca bit-bang okuma yapabiliyor — panelin kimliğini sorabiliyoruz,
 * ki bu "panel bizi duyuyor mu" sorusunun tek doğrudan yanıtı. */
#define BB_DELAY() sleep_us(1)      /* ~500 kHz — panel icin fazlasiyla yavas */

static void bb_pins_setup(void) {
    const uint cikislar[] = { PIN_CS, PIN_SCLK, PIN_DIO0 };
    for (size_t i = 0; i < 3; i++) {
        gpio_set_function(cikislar[i], GPIO_FUNC_SIO);
        gpio_set_dir(cikislar[i], GPIO_OUT);
    }
    /* D1..D3 tek hatli fazda kullanilmiyor; panel surerse cakismasin diye giris */
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) {
        gpio_set_function(p, GPIO_FUNC_SIO);
        gpio_set_dir(p, GPIO_IN);
    }
    gpio_put(PIN_CS, 1);
    gpio_put(PIN_SCLK, 0);
}

static void bb_byte(uint8_t v) {
    for (int i = 7; i >= 0; i--) {
        gpio_put(PIN_SCLK, 0);
        gpio_put(PIN_DIO0, (v >> i) & 1);
        BB_DELAY();
        gpio_put(PIN_SCLK, 1);          /* panel yukselen kenarda ornekler */
        BB_DELAY();
    }
    gpio_put(PIN_SCLK, 0);
}

static void bb_cmd(uint8_t cmd, const uint8_t *data, size_t n) {
    gpio_put(PIN_CS, 0);
    BB_DELAY();
    bb_byte(0x02); bb_byte(0x00); bb_byte(cmd); bb_byte(0x00);
    for (size_t i = 0; i < n; i++) bb_byte(data[i]);
    BB_DELAY();
    gpio_put(PIN_CS, 1);
    BB_DELAY();
}

static void bb_read(uint8_t cmd, uint8_t *output, size_t n) {
    gpio_put(PIN_CS, 0);
    BB_DELAY();
    bb_byte(0x03); bb_byte(0x00); bb_byte(cmd); bb_byte(0x00);

    gpio_set_dir(PIN_DIO0, GPIO_IN);
    for (size_t i = 0; i < n; i++) {
        uint8_t v = 0;
        for (int b = 7; b >= 0; b--) {
            gpio_put(PIN_SCLK, 0); BB_DELAY();
            gpio_put(PIN_SCLK, 1); BB_DELAY();
            v |= (uint8_t)(gpio_get(PIN_DIO0) << b);
        }
        output[i] = v;
    }
    gpio_put(PIN_SCLK, 0);
    gpio_set_dir(PIN_DIO0, GPIO_OUT);
    BB_DELAY();
    gpio_put(PIN_CS, 1);
}

/** Dort hat uzerinden bir bayt (gercek QSPI veri fazi). */
static void bb_byte_quad(uint8_t v) {
    for (int half = 0; half < 2; half++) {
        uint8_t nib = half ? (uint8_t)(v & 0x0F) : (uint8_t)(v >> 4);
        gpio_put(PIN_SCLK, 0);
        gpio_put(PIN_DIO0,  nib       & 1);
        gpio_put(PIN_DIO1, (nib >> 1) & 1);
        gpio_put(PIN_DIO2, (nib >> 2) & 1);
        gpio_put(PIN_DIO3, (nib >> 3) & 1);
        gpio_put(PIN_SCLK, 1);
    }
    gpio_put(PIN_SCLK, 0);
}

/**
 * Tum ekrani bit-bang ile tek renge boya.
 *
 * PIO'yu, DMA'yi ve satici surucusunu tamamen devre disi birakan bagimsiz
 * bir yol. Yavas ama her adimi burada gorunur. PIO yolu calismayip bu
 * calisirsa hata PIO tarafindadir; ikisi de calismazsa hata daha asagida.
 */
static void bb_ekrani_boya(uint16_t color) {
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_OUT);

    uint8_t caset[] = { 0x00, 0x00, (PB_PANEL_W - 1) >> 8, (PB_PANEL_W - 1) & 0xFF };
    uint8_t raset[] = { 0x00, 0x00, (PB_PANEL_H - 1) >> 8, (PB_PANEL_H - 1) & 0xFF };
    bb_cmd(0x2A, caset, 4);
    bb_cmd(0x2B, raset, 4);

    /* Piksel yazimi: komut+adres tek hatta (0x32 / 0x002C00), veri dort hatta */
    gpio_put(PIN_CS, 0);
    bb_byte(0x32); bb_byte(0x00); bb_byte(0x2C); bb_byte(0x00);
    uint8_t high = (uint8_t)(color >> 8), low = (uint8_t)(color & 0xFF);
    for (uint32_t i = 0; i < (uint32_t)PB_PANEL_W * PB_PANEL_H; i++) {
        bb_byte_quad(high);
        bb_byte_quad(low);
    }
    gpio_put(PIN_CS, 1);

    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_IN);
}

/** rsvpnano'nun kanitladigi asgari baslatma — tamami bit-bang. */
static void bb_panel_init(void) {
    uint8_t p0 = 0x00, p55 = 0x55;
    bb_cmd(0x11, NULL, 0);  sleep_ms(120);   /* SLPOUT */
    bb_cmd(0x36, &p0,  1);                   /* MADCTL */
    bb_cmd(0x3A, &p55, 1);                   /* COLMOD RGB565 */
    bb_cmd(0x29, NULL, 0);  sleep_ms(120);   /* DISPON */
}

/** TE hattinda 200 ms'de gecis say — panel tariyor mu / emre uydu mu. */
static uint32_t te_transition_say(void) {
    uint32_t s = 0;
    int previous = gpio_get(PB_PIN_LCD_TE);
    absolute_time_t end = make_timeout_time_ms(200);
    while (!time_reached(end)) {
        int now = gpio_get(PB_PIN_LCD_TE);
        if (now != previous) { s++; previous = now; }
    }
    return s;
}

/**
 * M3 — mel + kapı hattını CANLI mikrofonla çalıştır.
 *
 * DSP'nin doğruluğu host testleriyle kanıtlandı (`test/dsp_test` 13/13, ve
 * `tools/mel_reference.py` bağımsız Python referansıyla 64 bandın tamamında
 * sıfır sapma). Burada sınanan başka bir şey: hat gerçek zamanlı olarak,
 * gerçek mikrofonla, kartın üzerinde ayakta kalıyor mu ve kapı gerçek odada
 * mantıklı davranıyor mu.
 *
 * Çıktı tamamen sayısal — ekrana bakmak gerekmiyor.
 */
static void cmd_mel_pipeline(void) {
    printf("\nM3: mel + kapi hatti (canli mikrofon)\n");
    printf("Cikmak icin bir tusa basin.\n\n");

    pb_mel_init();
    pb_mel_reset();
    pb_gate_reset();

    /* Doğru hop için örtüşmeli kare: her turda PB_MEL_HOP yeni örnek alınıp
     * kare sola kaydırılıyor. Örtüşmesiz okumak 16 ms'lik adımı bozar ve
     * 3 saniye 187 kare tutmaz. */
    static int16_t frame[PB_FFT_SIZE];
    const uint32_t remaining = PB_FFT_SIZE - PB_MEL_HOP;

    uint32_t total = 0, open = 0, window_count = 0;
    uint32_t lost_frame = 0, fps = 0;
    absolute_time_t next_report = make_timeout_time_ms(1000);

    /* Bayat veriyle değil şimdiyle başla; döngü İÇİNDE flush YOK — hattın
     * kesintisiz akması sürekli yakalamanın bütün amacı. */
    pb_audio_stream_flush();

    while (getchar_timeout_us(0) < 0) {
        memmove(frame, frame + PB_MEL_HOP, remaining * sizeof(int16_t));
        pb_capture_result_t cap = pb_audio_stream_read(frame + remaining, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) { printf("  yakalama eksik, cikiliyor\n"); break; }
        if (cap.fifo_overrun) lost_frame++;   /* halka sarıldı: süreklilik koptu */

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(frame, power);
        pb_gate_result_t g = pb_gate_update(power);
        if (g.active) open++;

        pb_mel_push(frame);
        total++;
        fps++;

        if (pb_mel_frame_count() >= PB_MEL_FRAMES &&
            pb_mel_frame_count() % PB_MEL_FRAMES == 0) {
            static int8_t window[PB_MEL_BANDS * PB_MEL_FRAMES];
            if (pb_mel_window(window)) window_count++;
        }

        if (time_reached(next_report)) {
            /* kare/s kabul ölçütü: 62.5 (hop 384 @ 24 kHz). Eskiden 57'ydi —
             * bloklayan yakalama kare kaçırıyordu (lastsession.md §9c). */
            printf("  kare %5lu (%lu/s)  kapi %%%3lu  bant %6.1f dB  "
                   "taban %6.1f dB  flux %.3f  pencere %lu  kayip %lu\n",
                   (unsigned long)total, (unsigned long)fps,
                   (unsigned long)(total ? open * 100 / total : 0),
                   (double)g.band_db, (double)g.floor_db, (double)g.flux,
                   (unsigned long)window_count, (unsigned long)lost_frame);
            fps = 0;
            next_report = make_timeout_time_ms(1000);
        }
    }

    printf("\n  toplam kare %lu, kapi acik %lu (%%%lu), tam pencere %lu, "
           "kayip %lu\n\n",
           (unsigned long)total, (unsigned long)open,
           (unsigned long)(total ? open * 100 / total : 0),
           (unsigned long)window_count, (unsigned long)lost_frame);
}

/**
 * DEMO — şimdiye kadar yapılan her şey tek ekranda, aynı anda.
 *
 *   Sol şerit (0..199)   LVGL durum kartı (M2b): başlık, kapı durumu,
 *                        canlı sayılar. Yalnızca kirlenen alan yeniden
 *                        çizildiği için sağ şeride dokunmuyor.
 *   Sağ şerit (200..639) Canlı MEL spektrogramı (M3): ekranda akan şey
 *                        FFT değil, modelin göreceği 64 bant. Doğrudan
 *                        blit ile çiziliyor — LVGL ile bir arada yaşama
 *                        M2b'nin açık kalan 6. maddesiydi, bu demo onu
 *                        kapatıyor.
 *   Ses                  Sürekli yakalama halkasından (M3'ün son işi):
 *                        62.5 kare/s örtüşmeli hop, kare kaçırmadan.
 *   Kapı                 Ses algılayınca durum yazısı yeşillenir; 3 s'lik
 *                        pencere sayacı modelin girdi penceresinin
 *                        birikişini gösterir.
 *
 * Çıkışta sayısal özet de basılıyor: kare/s ölçümü göz gerektirmeden
 * doğrulanabilsin (kabul ölçütü 62/s, eski bloklayan yakalama 57'de
 * kalıyordu).
 */
static void cmd_full_demo(void) {
    printf("\nDEMO: LVGL kart + canli mel spektrogrami + kapi.\n");
    printf("Cihazi USB soketi SAGDA olacak sekilde yatay tutun.\n");
    printf("Cikmak icin bir tusa basin.\n\n");
    backlight_set(true);

    /* Ekranı temizlemek yalnızca kozmetik değil: `pb_lcd_fill` atlama
     * şeridini (lcd_blit.c) bilinen bir hâle getiriyor. Şerit geçersizken
     * konumlandırma 0/1. sütunlara siyah yazar ve iz bırakır — teşhis
     * komutlarından (`z`, `j`, `v`) sonra tam olarak bu olur. */
    pb_lcd_fill(0x0000);
    pb_lv_flush_counters_reset();

    bool touch = pb_lv_init();
    printf("  dokunmatik: %s\n", touch ? "hazir" : "yok (demoya engel degil)");

    /* ── Sol kart ── */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PokeBird");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0C000), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *state = lv_label_create(scr);
    lv_label_set_text(state, "dinliyor...");
    lv_obj_set_style_text_color(state, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, 12, 44);

    lv_obj_t *numbers = lv_label_create(scr);
    lv_label_set_text(numbers, "");
    lv_obj_set_style_text_color(numbers, lv_color_hex(0x8090A0), LV_PART_MAIN);
    lv_obj_align(numbers, LV_ALIGN_TOP_LEFT, 12, 76);

    /* Spektrogramın dilimlerini LVGL'e ÇİZDİRME: arayüz artık tam genişlik
     * (640) ve LVGL varsayılan olarak beş dilimin de sahibi — bildirilmezse
     * kart ile şerit aynı bölgeye yazıp birbirini siler (lv_port.c). */
    pb_lv_set_slice_owner(PB_LVGL_SLICE_MASK_LISTEN);

    /* Kartı çiz, SONRA sağ şeridi spektrograma ver: LVGL'in ilk çizimi kendi
     * dilimlerinin tamamını boyuyor. */
    for (int i = 0; i < 4; i++) { pb_lv_tick(); sleep_ms(5); }
    pb_spec_init();

    /* ── Ses hattı ── */
    pb_mel_init();
    pb_mel_reset();
    pb_gate_reset();

    static int16_t frame[PB_FFT_SIZE];
    memset(frame, 0, sizeof(frame));
    const uint32_t remaining = PB_FFT_SIZE - PB_MEL_HOP;

    uint32_t total = 0, open = 0, window_count = 0, lost = 0;
    uint32_t fps = 0, last_rate = 0;
    /* Kapı kapanınca yazıyı hemen soldurmak yerine kısa bir süre tut:
     * 62 kare/s'de tek karelik açılmalar gözle görülmez. */
    uint32_t gate_hold = 0;
    bool gate_gorunur = false;
    absolute_time_t next_report = make_timeout_time_ms(1000);
    absolute_time_t next_card  = make_timeout_time_ms(250);

    pb_audio_stream_flush();
    drain_stdin();

    while (getchar_timeout_us(0) < 0) {
        memmove(frame, frame + PB_MEL_HOP, remaining * sizeof(int16_t));
        pb_capture_result_t cap = pb_audio_stream_read(frame + remaining, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) { printf("  yakalama eksik, cikiliyor\n"); break; }
        if (cap.fifo_overrun) lost++;

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(frame, power);
        pb_gate_result_t g = pb_gate_update(power);
        if (g.active) { open++; gate_hold = 31; }      /* ~0.5 s görünür kal */
        else if (gate_hold) gate_hold--;

        pb_mel_push(frame);
        total++;
        fps++;

        if (pb_mel_frame_count() >= PB_MEL_FRAMES &&
            pb_mel_frame_count() % PB_MEL_FRAMES == 0) {
            static int8_t window[PB_MEL_BANDS * PB_MEL_FRAMES];
            if (pb_mel_window(window)) window_count++;
        }

        /* Mel karesini spektrogram sütununa çevir. Gösterim penceresi
         * -75..-15 dB: oda tabanı (~-47 dB) koyu, kuş sesi parlak düşer. */
        int8_t mel_q[PB_MEL_BANDS];
        if (pb_mel_last_frame(mel_q)) {
            uint8_t bins[PB_MEL_BANDS];
            for (int b = 0; b < PB_MEL_BANDS; b++) {
                float db = pb_mel_q_to_db(mel_q[b]);
                float v = (db + 75.0f) * (255.0f / 60.0f);
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                bins[b] = (uint8_t)v;
            }
            pb_spec_push_column(bins, PB_MEL_BANDS);
        }

        if (time_reached(next_report)) {
            last_rate = fps;
            fps = 0;
            next_report = make_timeout_time_ms(1000);
        }

        /* Kartı 4 Hz güncelle: her karede güncellemek LVGL'e boş yere
         * çizim çıkarır ve hop bütçesini yer. */
        if (time_reached(next_card)) {
            bool show = g.active || gate_hold > 0;
            if (show != gate_gorunur) {
                gate_gorunur = show;
                lv_label_set_text(state, show ? "SES ALGILANDI" : "dinliyor...");
                lv_obj_set_style_text_color(state,
                    lv_color_hex(show ? 0x40E060 : 0xB0B8C0), LV_PART_MAIN);
            }
            lv_label_set_text_fmt(numbers,
                "%lu kare/s\nkapi %%%lu\nbant %d dB\npencere %lu\nkayip %lu",
                (unsigned long)last_rate,
                (unsigned long)(total ? open * 100 / total : 0),
                (int)g.band_db,
                (unsigned long)window_count,
                (unsigned long)lost);
            next_card = make_timeout_time_ms(250);
        }
        pb_lv_tick();
    }

    printf("\n  toplam kare %lu, son hiz %lu kare/s, kapi acik %%%lu,\n"
           "  tam pencere %lu, kayip %lu\n",
           (unsigned long)total, (unsigned long)last_rate,
           (unsigned long)(total ? open * 100 / total : 0),
           (unsigned long)window_count, (unsigned long)lost);

    /* LVGL flush hizalamasi (§9n) — goz gerektirmeyen olcum.
     * Panel sutun araligini 2 piksele yuvarliyor; sutun sayisi TEK olursa
     * panel satir basina bizim gonderdigimizden bir piksel FAZLA kullanir ve
     * veri her satirda kayar — yazinin yatay suruklenmis gorunmesinin imzasi. */
    printf("  LVGL flush %lu, HIZASIZ %lu, satir adimi != alan_w: %lu\n"
           "  panel_w %lu..%lu, son alan ui x(%ld..%ld) y(%ld..%ld), "
           "adim %ld px / alan_w %ld px\n\n",
           (unsigned long)pb_lv_flush_count, (unsigned long)pb_lv_flush_unaligned,
           (unsigned long)pb_lv_flush_stride_differs,
           (unsigned long)pb_lv_flush_w_min, (unsigned long)pb_lv_flush_w_max,
           (long)pb_lv_last_x1, (long)pb_lv_last_x2,
           (long)pb_lv_last_y1, (long)pb_lv_last_y2,
           (long)pb_lv_last_stride_px, (long)pb_lv_last_area_w);
}

/**
 * LVGL demo — M2b'nin kabul ölçütü.
 *
 * Tek ekranda dört şeyi birden sınıyor: LVGL'in ayağa kalkması, 90° yön
 * çevriminin kısmi render ile doğru çalışması, yazı tipi/tema, ve dokunmatik
 * girişi. Dokunulan nokta ekrana yazıldığı için koordinat eşlemesinin doğru
 * olup olmadığı da doğrudan görülüyor.
 */
static void cmd_ui_demo(void) {
    printf("\nLVGL demo. Cikmak icin bir tusa basin.\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);   /* atlama seridini bilinen hale getirir (lcd_blit.c) */

    bool touch = pb_lv_init();
    printf("  dokunmatik: %s\n", touch ? "hazir" : "YOK (sadece ekran)");

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PokeBird");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0C000), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text(info, "ekrana dokunun");
    lv_obj_set_style_text_color(info, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 12, 44);

    /* Sağ kenara bir çubuk: kısmi render'ın ekranın uzak ucunda da doğru
     * yere düştüğünü gösterir (yön hatası en çok orada belli olur). */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 200, 16);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_bar_set_range(bar, 0, 100);

    int value = 0;
    drain_stdin();
    while (getchar_timeout_us(0) < 0) {
        pb_lv_tick();

        pb_touch_state_t st = pb_touch_read();
        if (st.ok && st.fingers > 0) {
            lv_label_set_text_fmt(info, "dokunus: x=%u  y=%u",
                                  st.p.raw_x, st.p.raw_y);
        }

        value = (value + 1) % 101;
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
        sleep_ms(20);
    }
    printf("cikildi\n\n");
}

/**
 * Yön testi — panelin doğal koordinatları fiziksel olarak nereye düşüyor?
 *
 * Panel 172x640 dikey, arayüz 640x172 yatay. `ui/spectrogram.c` bu çevrimi
 * yapıyor ama hangi köşenin (0,0) olduğu ve aynalama olup olmadığı ancak
 * ekrana bakılarak bilinir.
 *
 * Dört köşeye dört farklı renk basıyoruz. Tek bir bakış hem dönüşü hem
 * aynalamayı belirsizliğe yer bırakmadan söylüyor — tuşa basmak gerekmiyor.
 */
static void cmd_orientation(void) {
    enum { FRAME = 40 };
    static uint16_t row[PB_PANEL_W];

    const struct { uint32_t x, y; uint16_t color; const char *name; } corner[] = {
        { 0,               0,               0xF800, "KIRMIZI = panel (0,0)"         },
        { PB_PANEL_W-FRAME, 0,               0x07E0, "YESIL   = panel (X sonu, 0)"   },
        { 0,               PB_PANEL_H-FRAME, 0x001F, "MAVI    = panel (0, Y sonu)"   },
        { PB_PANEL_W-FRAME, PB_PANEL_H-FRAME, 0xFFE0, "SARI    = panel (X sonu, Y sonu)" },
    };

    printf("\nYon testi — ekranda dort renkli kare var.\n");
    backlight_set(true);

    /* TEK GEÇİŞ — panelin sözleşmesi bu (§9n).
     *
     * Eski hâli önce ekranı doldurup sonra dört kareyi ayrı ayrı blit
     * ediyordu; bu panelde her RAMWR satır 0'a döndüğü için dördü de üst
     * üste biniyor ve yalnızca sonuncusu görünüyordu — ekran hatasının en
     * çok görülen belirtisi buydu. Şimdi 640 satırın her biri yerinde
     * üretilip tek RAMWR akışında yollanıyor: dört köşe de doğru yerde. */
    pb_lcd_column_window(0, PB_PANEL_W - 1);
    pb_lcd_stream_begin(0x2C);
    for (uint32_t y = 0; y < PB_PANEL_H; y++) {
        for (uint32_t x = 0; x < PB_PANEL_W; x++) row[x] = 0x0000;
        for (size_t i = 0; i < sizeof(corner) / sizeof(corner[0]); i++) {
            if (y >= corner[i].y && y < corner[i].y + FRAME) {
                for (uint32_t x = corner[i].x; x < corner[i].x + FRAME; x++) {
                    row[x] = corner[i].color;
                }
            }
        }
        pb_lcd_stream_row(row, PB_PANEL_W);
    }
    pb_lcd_stream_end();
    pb_lcd_cursor_invalidate();

    for (size_t i = 0; i < sizeof(corner) / sizeof(corner[0]); i++) {
        printf("  %s\n", corner[i].name);
    }

    printf("\nCihazi USB soketi ASAGI bakacak sekilde tutun ve her rengin\n");
    printf("hangi kosede oldugunu soyleyin (sol ust / sag ust / sol alt / sag alt).\n\n");
}

/* ── §9n: ekran hatası — QSPI zamanlama ve melez yol teşhisleri ───────────
 *
 * §9n'in birinci hipotezi: `QSPI_WaitIdle`'ın 50 ms'lik zaman aşımı SESSİZ.
 * Zaman aşımına giriyorsa fonksiyon hiçbir şey beklemiyordur, §5.9'un hatası
 * geri gelmiştir (CS, veri hatta çıkmadan yükselir) ve düzeltme kâğıt üstünde
 * kalmıştır. Aşağıdaki `w` bunu ölçüyor — göz gerekmiyor. */

static void qspi_counter_print(void) {
    printf("     WaitIdle cagrisi         : %lu\n",
           (unsigned long)pb_qspi_wait_calls);
    printf("     ZAMAN ASIMI (sessiz hata): %lu%s\n",
           (unsigned long)pb_qspi_wait_timeout,
           pb_qspi_wait_timeout ? "   <<< HIPOTEZ DOGRU: beklemiyor" : "   (0 = beklendi)");
    printf("     girerken SM kapali       : %lu%s\n",
           (unsigned long)pb_qspi_wait_sm_off,
           pb_qspi_wait_sm_off ? "   <<< SM KAPALI" : "");
    printf("     girerken FIFO doluydu    : %lu   (en yuksek seviye %lu/4)\n",
           (unsigned long)pb_qspi_wait_fifo_full,
           (unsigned long)pb_qspi_wait_fifo_max);
    printf("     gercekten bekledi        : %lu   (en cok %lu dongu, en uzun %lu us)\n",
           (unsigned long)pb_qspi_wait_waited,
           (unsigned long)pb_qspi_wait_spin_max,
           (unsigned long)pb_qspi_wait_us_max);
    printf("     CIKARKEN FIFO hala dolu  : %lu%s\n",
           (unsigned long)pb_qspi_wait_residue,
           pb_qspi_wait_residue ? "   <<< BEKLEME ISE YARAMADI" : "   (0 = FIFO bosaldi)");
    printf("     toplam bekleme           : %lu us\n\n",
           (unsigned long)pb_qspi_wait_us_total);
}

/**
 * QSPI zamanlama teşhisi — `QSPI_WaitIdle` gerçekten bekliyor mu? (§9n)
 *
 * GÖZ GEREKMİYOR. Cihaz kendi yanıtını yazıyor. Dört ölçüm:
 *
 *   1) Yalnızca pencere komutları — FIFO'ya CPU yazıyor, DMA yok.
 *   2) Tek satır blit — piksel yolu, DMA'lı.
 *   3) Tam ekran doldurma + geçen süre. PIO'nun kuramsal tabanıyla
 *      karşılaştırılıyor: her WaitIdle zaman aşımına girseydi 640 satır x
 *      4 işlem x 50 ms = ~2 dakika sürerdi, yani süre tek başına da bir kanıt.
 *   4) CS yükseldikten SONRA hat hâlâ kıpırdıyor mu — §5.9'un imzasının
 *      DOĞRUDAN gözlemi. PIO saati kalıntı baytlar CPU'nun örneklemesine
 *      yetecek kadar yavaşlatılıyor; `QSPI_Deselect` döndükten sonra SCLK'te
 *      geçiş varsa veri CS yüksekken hatta çıkıyor demektir.
 *
 * Bu komut PIO durum makinesini KAPALI BIRAKMIYOR; ardından ekran testi
 * çalıştırmak güvenli (bkz. `d`'nin bit-bang varyantı, §9n uyarısı).
 */
static void cmd_qspi_timing(void) {
    printf("\nQSPI zamanlama teshisi — WaitIdle gercekten bekliyor mu? (goz GEREKMEZ)\n");
    printf("======================================================================\n\n");

    printf("0) PIO durumu: sm%u %s, PC %u, sys clk %lu Hz\n\n",
           (unsigned)qspi.sm,
           ((qspi.pio->ctrl >> qspi.sm) & 1u) ? "ETKIN" : "KAPALI (!)",
           (unsigned)pio_sm_get_pc(qspi.pio, qspi.sm),
           (unsigned long)clock_get_hz(clk_sys));

    printf("1) Pencere komutlari — SetWindows, 3 CS islemi, DMA yok\n");
    pb_qspi_counters_reset();
    LCD_3IN49_SetWindows(0, 0, PB_PANEL_W, 1);
    qspi_counter_print();

    printf("2) Tek satir blit — pencere + DMA piksel, 4 CS islemi\n");
    {
        static uint16_t row[PB_PANEL_W];
        for (uint32_t i = 0; i < PB_PANEL_W; i++) row[i] = 0x0000;
        pb_qspi_counters_reset();
        pb_lcd_blit(0, 0, PB_PANEL_W, 1, row);
        qspi_counter_print();
    }

    printf("3) Tam ekran doldurma — 640 satir\n");
    {
        pb_qspi_counters_reset();
        absolute_time_t t0 = get_absolute_time();
        pb_lcd_fill(0x0000);
        int64_t elapsed = absolute_time_diff_us(t0, get_absolute_time());

        /* PIO tabani: tek gecis oldugu icin yalnizca piksel verisi —
         * W*H*2 bayt, bayt basina 4 PIO cevrimi. (§9n duzeltmesinden once
         * satir basina ayri pencere+RAMWR vardi ve taban 440 bayt/satirdi.) */
        uint32_t pio_hz = (uint32_t)(clock_get_hz(clk_sys) / 2);
        uint64_t base_us = (uint64_t)PB_PANEL_W * PB_PANEL_H * 2ull
                            * 4ull * 1000000ull / pio_hz;

        qspi_counter_print();
        printf("     gecen sure               : %lu us\n", (unsigned long)elapsed);
        printf("     PIO tabani (kuramsal)    : %lu us\n", (unsigned long)base_us);
        printf("     her cagri zaman asiminda : ~%lu us olurdu\n\n",
               (unsigned long)((uint64_t)pb_qspi_wait_calls * 50000ull));
    }

    printf("4) CS yukseldikten SONRA hat hala kipirdiyor mu? (§5.9'un imzasi)\n");
    {
        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 1000.0f);   /* ~150 kHz PIO */
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

        pb_qspi_counters_reset();
        QSPI_Select(qspi);
        QSPI_REGISTER_Write(qspi, 0x2a);
        QSPI_DATA_Write(qspi, 0x00);
        QSPI_DATA_Write(qspi, 0x00);
        QSPI_DATA_Write(qspi, (PB_PANEL_W - 1) >> 8);
        QSPI_DATA_Write(qspi, (PB_PANEL_W - 1) & 0xff);
        QSPI_Deselect(qspi);

        /* Deselect dondu, CS yuksek. Hat susmus OLMALI. */
        uint32_t transition = 0;
        int previous = gpio_get(PIN_SCLK);
        absolute_time_t end = make_timeout_time_ms(10);
        while (!time_reached(end)) {
            int now = gpio_get(PIN_SCLK);
            if (now != previous) { transition++; previous = now; }
        }

        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 2.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

        printf("     32 bayt ~150 kHz'te yollandi (islem ~850 us surmeliydi)\n");
        qspi_counter_print();
        printf("     CS yuksekken SCLK gecisi : %lu%s\n\n", (unsigned long)transition,
               transition ? "   <<< VERI CS YUKSEKKEN CIKIYOR — §5.9 GERI GELMIS"
                     : "   (0 = hat susmus, CS zamanlamasi DOGRU)");

        /* Pencereyi tam ekrana geri al; sonraki cizim dogru yere dussun. */
        LCD_3IN49_SetWindows(0, 0, PB_PANEL_W, PB_PANEL_H);
        pb_lcd_cursor_invalidate();   /* panele disaridan yazildi (lcd_blit.h) */
    }
}

/* ── Melez yol testi: pencere komutu ile piksel verisi AYRI yollardan ──────
 *
 * §9n'in ölçülmüş gerçeği: bit-bang düz renkleri doğru basıyor, PIO/DMA yolu
 * basmıyor. Ama `bb_ekrani_boya` HEM pencereyi HEM pikselleri bit-bang ile
 * yolluyor, yani hangisinin düştüğünü ayırmıyor.
 *
 * Belirti ("ekran temizlenmiyor, yalnızca EN SON çizilen kare görünüyor")
 * pencere komutlarının düşmesiyle birebir uyuşuyor: pencere hiç değişmezse
 * her RAMWR yazma imlecini aynı yere döndürür ve her çizim bir öncekinin
 * üstüne biner. Bu test o hipotezi ikiye ayırıyor. */

static void bb_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint8_t caset[] = { (uint8_t)(x >> 8), (uint8_t)x,
                        (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1) };
    uint8_t raset[] = { (uint8_t)(y >> 8), (uint8_t)y,
                        (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1) };
    bb_cmd(0x2A, caset, 4);
    bb_cmd(0x2B, raset, 4);
}

static void bb_pixel(uint16_t color, uint32_t pixel) {
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_OUT);
    gpio_put(PIN_CS, 0);
    bb_byte(0x32); bb_byte(0x00); bb_byte(0x2C); bb_byte(0x00);
    uint8_t high = (uint8_t)(color >> 8), low = (uint8_t)(color & 0xFF);
    for (uint32_t i = 0; i < pixel; i++) { bb_byte_quad(high); bb_byte_quad(low); }
    gpio_put(PIN_CS, 1);
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_IN);
}

static void hybrid_wait(void) {
    printf("      >>> EKRANA BAKIN. Devam icin tusa basin (10 s sonra kendi gecer).\n");
    drain_stdin();
    for (int t = 0; t < 100; t++) {
        if (getchar_timeout_us(0) >= 0) return;
        sleep_ms(100);
    }
}

/* PIO'ya geri don ve saati ayarla.
 * DIKKAT: QSPI_PIO_Restore, program_init uzerinden clkdiv'i 2.0'a geri
 * cekiyor — saat HER geri donusten SONRA yeniden kurulmali. */
static void hybrid_pio_version(float clkdiv) {
    QSPI_PIO_Restore(qspi);
    pio_sm_set_clkdiv(qspi.pio, qspi.sm, clkdiv);
    pio_sm_clkdiv_restart(qspi.pio, qspi.sm);
}

static void hybrid_phase(bool window_pio, bool pixel_pio, float clkdiv) {
    enum { SQ_X = 66, SQ_Y = 300, SQ_W = 40, SQ_H = 40 };

    /* 1) Tum ekrani koyu maviye boya */
    if (window_pio) { hybrid_pio_version(clkdiv); LCD_3IN49_SetWindows(0, 0, PB_PANEL_W, PB_PANEL_H); }
    else             { bb_pins_setup();       bb_window(0, 0, PB_PANEL_W, PB_PANEL_H); }

    if (pixel_pio)  { hybrid_pio_version(clkdiv); pb_lcd_stream_flat(0x001F, (uint32_t)PB_PANEL_W * PB_PANEL_H); }
    else             { bb_pins_setup();       bb_pixel(0x001F, (uint32_t)PB_PANEL_W * PB_PANEL_H); }

    /* 2) Ekranin ORTASINA 40x40 beyaz kare.
     *    Pencere komutu dusuyorsa kare ortada degil, EN USTTE tam genislikte
     *    bir serit olarak cikar — tek bakista ayirt edilir. */
    if (window_pio) { hybrid_pio_version(clkdiv); LCD_3IN49_SetWindows(SQ_X, SQ_Y, SQ_X + SQ_W, SQ_Y + SQ_H); }
    else             { bb_pins_setup();       bb_window(SQ_X, SQ_Y, SQ_W, SQ_H); }

    if (pixel_pio)  { hybrid_pio_version(clkdiv); pb_lcd_stream_flat(0xFFFF, SQ_W * SQ_H); }
    else             { bb_pins_setup();       bb_pixel(0xFFFF, SQ_W * SQ_H); }

    hybrid_pio_version(2.0f);        /* uretim saatiyle birak */
}

/**
 * `y` — melez yol testi. GÖZ GEREKİR, etkileşimli.
 *
 * Dört bileşim, her birinde aynı desen: ekran koyu mavi + ORTADA 40x40 beyaz
 * kare. Sorulacak tek soru: kare ORTADA mı, yoksa EN ÜSTTE geniş bir şerit mi?
 */
static void cmd_hybrid_path(void) {
    printf("\nMelez yol testi — pencere komutu ve piksel verisi ayri yollardan\n");
    printf("================================================================\n\n");
    printf("Her adimda ekran MAVI olmali ve ORTASINDA 40x40 BEYAZ KARE.\n");
    printf("Kare ORTADA ise o bilesim CALISIYOR.\n");
    printf("Kare EN USTTE genis bir SERIT ise pencere komutu panele ULASMIYOR.\n");
    printf("Ekran hic mavi olmuyorsa piksel verisi ulasmiyor.\n\n");

    backlight_set(true);

    const struct { bool window_pio, pixel_pio; float clkdiv; const char *name; } phase[] = {
        { false, false,  2.0f, "pencere BIT-BANG + piksel BIT-BANG   (kontrol: 9n'e gore CALISIYOR)" },
        { false, true,   2.0f, "pencere BIT-BANG + piksel PIO/DMA    (SCLK 37,5 MHz)" },
        { true,  false,  2.0f, "pencere PIO      + piksel BIT-BANG   (SCLK 37,5 MHz)" },
        { true,  true,   2.0f, "pencere PIO      + piksel PIO/DMA    (URETIM YOLU, 37,5 MHz)" },
        { true,  true,  20.0f, "pencere PIO      + piksel PIO/DMA    (ayni yol, SCLK 3,75 MHz)" },
        { true,  true,  80.0f, "pencere PIO      + piksel PIO/DMA    (ayni yol, SCLK 0,94 MHz)" },
    };

    for (size_t i = 0; i < sizeof(phase) / sizeof(phase[0]); i++) {
        printf("  [%u] %s\n", (unsigned)(i + 1), phase[i].name);
        hybrid_phase(phase[i].window_pio, phase[i].pixel_pio, phase[i].clkdiv);
        hybrid_wait();
        printf("\n");
    }

    printf("Bildirin: hangi adimlarda kare ORTADAYDI, hangilerinde SERIT/yoktu.\n");
    printf("  [2] calisip [3] calismiyorsa  -> hata PENCERE komutlarinda.\n");
    printf("  [3] calisip [2] calismiyorsa  -> hata PIKSEL/DMA yolunda.\n");
    printf("  [4] bozuk ama [5]/[6] duzgunse-> hata SAAT HIZI (37,5 MHz fazla).\n");
    printf("  [1] disinda hicbiri calismiyorsa -> PIO yolu bastan asagi bozuk.\n\n");
}

/* ── §9n: satır adresleme testi ────────────────────────────────────────────
 *
 * `y` altı bileşimin ALTISINDA da aynı sonucu verdi (mavi ekran + kenarda
 * beyaz kutu). Yani hata yolda (PIO/bit-bang) da, saat hızında da DEĞİL —
 * gönderilen komutlarda.
 *
 * Panelin ÇALIŞAN iki bağımsız sürücüsü (rsvpnano'nun ESP32 ve RP2350-PIO
 * sürücüleri) **RASET (0x2B) komutunu hiç yollamıyor**: yalnızca CASET (0x2A)
 * ile sütun aralığı ayarlanıyor, satır ise RAMWR (0x2C, sütun penceresinin
 * en üstünden başla) ve RAMWRC (0x3C, kaldığın yerden devam et) ile
 * belirleniyor. Bizim sürücümüz RASET yollayıp satırın oraya gitmesini
 * bekliyor.
 *
 * Bu, gözlenen HER ŞEYİ açıklıyor: tam ekran düz dolgu çalışıyor (zaten
 * satır 0'dan başlıyor), ama her kısmi çizim satır 0'a düşüyor —
 * `pb_lcd_fill`'in 640 satırı hep aynı üst satıra biniyor (ekran
 * "temizlenmiyor"), `o`'nun dört karesi üst üste geliyor (yalnızca sonuncusu
 * görünüyor), `a`'da LVGL'in her parçası tepede birikiyor.
 *
 * `z` bunu doğruluyor: aynı kare, beş farklı yöntemle. */

static void qspi_reg_write(uint8_t reg, const uint8_t *data, size_t n) {
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, reg);
    for (size_t i = 0; i < n; i++) QSPI_DATA_Write(qspi, data[i]);
    QSPI_Deselect(qspi);
}

static void qspi_caset(uint16_t x1, uint16_t x2) {
    uint8_t d[] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    qspi_reg_write(0x2A, d, 4);
}

static void qspi_raset(uint16_t y1, uint16_t y2) {
    uint8_t d[] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    qspi_reg_write(0x2B, d, 4);
}

/** Tüm ekranı tek renge boya — panelin gerçek sözleşmesiyle (CASET + RAMWR).
 *  Bu yolun çalıştığı ölçüldü (`y`'nin altı adımında da ekran masmaviydi). */
static void z_background(uint16_t color) {
    qspi_caset(0, PB_PANEL_W - 1);
    pb_lcd_stream_begin(0x2C);
    pb_lcd_stream_color(color, (uint32_t)PB_PANEL_W * PB_PANEL_H);
    pb_lcd_stream_end();
}

/**
 * `z` — satır adresleme testi. GÖZ GEREKİR, etkileşimli.
 *
 * Beş yöntem, hepsinde aynı hedef: panelin (66,300) konumuna, yani TAM
 * ORTASINA 40x40 beyaz kare. Tek soru: kare ORTADA mı, UÇTA mı?
 */
static void cmd_row_addr(void) {
    enum { SQ_X = 66, SQ_Y = 300, SQ_W = 40, SQ_H = 40 };
    const uint16_t CLR_BLUE = 0x001F, CLR_WHITE = 0xFFFF;

    printf("\nSatir adresleme testi — RASET (0x2B) bu panelde calisiyor mu?\n");
    printf("=============================================================\n\n");
    printf("Her adimda ekran MAVI, uzerinde 40x40 BEYAZ kare olacak.\n");
    printf("Kare panelin TAM ORTASINA cizilmek isteniyor.\n");
    printf("  ORTADA ise  -> o yontem DOGRU\n");
    printf("  UCTA/kenarda ise -> satir adresi yok sayiliyor\n\n");

    backlight_set(true);

    for (int step = 1; step <= 5; step++) {
        switch (step) {
        case 1:
            printf("  [1] SIMDIKI YOL: CASET + RASET + ciplak 0x2C, sonra RAMWR (kontrol)\n");
            z_background(CLR_BLUE);
            LCD_3IN49_SetWindows(SQ_X, SQ_Y, SQ_X + SQ_W, SQ_Y + SQ_H);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 2:
            printf("  [2] CASET + RASET, ciplak 0x2C YOK\n");
            z_background(CLR_BLUE);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            qspi_raset(SQ_Y, SQ_Y + SQ_H - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 3:
            printf("  [3] Sira ters: once RASET sonra CASET\n");
            z_background(CLR_BLUE);
            qspi_raset(SQ_Y, SQ_Y + SQ_H - 1);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 4:
            printf("  [4] REFERANS YOL: yalniz CASET; satir RAMWR'den itibaren\n");
            printf("      sayiliyor — %d satir mavi atlanip sonra beyaz yaziliyor\n", SQ_Y);
            z_background(CLR_BLUE);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_BLUE,  (uint32_t)SQ_Y * SQ_W);   /* atla */
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;

        case 5:
            printf("  [5] REFERANS + RAMWRC: atlama ayri islemde, beyaz 0x3C ile\n");
            printf("      devam ediyor (kalici cozumun ucuz olup olmadigini soyler)\n");
            z_background(CLR_BLUE);
            qspi_caset(SQ_X, SQ_X + SQ_W - 1);
            pb_lcd_stream_begin(0x2C);
            pb_lcd_stream_color(CLR_BLUE, (uint32_t)SQ_Y * SQ_W);
            pb_lcd_stream_end();
            pb_lcd_stream_begin(0x3C);                      /* RAMWRC — devam et */
            pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
            pb_lcd_stream_end();
            break;
        }
        hybrid_wait();
        printf("\n");
    }

    printf("Bildirin: hangi adimlarda kare ORTADAYDI?\n");
    printf("  [4] ortada, [1][2][3] ucta  -> RASET yok sayiliyor. KOK NEDEN BU.\n");
    printf("  [5] de ortada               -> RAMWRC calisiyor, cozum ucuz:\n");
    printf("                                 LVGL akisi bastan sona tek gecis.\n");
    printf("  [5] ucta ama [4] ortada     -> RAMWRC yok, her cizim atlama bedeli oder.\n\n");

    /* Pencereyi tam ekrana geri birak. */
    qspi_caset(0, PB_PANEL_W - 1);
    pb_lcd_cursor_invalidate();
}

/**
 * `j` — imleç konumlandırma testi. GÖZ GEREKİR, etkileşimli.
 *
 * `z` şunu kanıtladı: satırı yalnızca RAMWR (en üste dön) ve RAMWRC (kaldığın
 * yerden devam et) belirliyor. Geriye MİMARİYİ belirleyen tek soru kaldı:
 *
 *   Sütun penceresi DARALTILIRSA satır daha ucuza ilerletilebilir mi, ve
 *   pencere yeniden genişletilince satır KORUNUR mu?
 *
 * Satır, pencere genişliği kadar piksel yazıldıkça ilerliyor. Pencere 1
 * piksel genişse `y` satır ilerletmek `y` piksele mal olur — 172*y yerine.
 * Sonra pencere gerçek aralığa alınıp RAMWRC ile devam edilebiliyorsa
 * elimizde **ucuz bir konumlandırma** var:
 *
 *   ÇALIŞIRSA  `pb_lcd_blit` genel amaçlı kalır; spektrogram ile LVGL aynı
 *              ekranda yaşayabilir (`a` demosu), bedel satır başına 1 piksel.
 *   ÇALIŞMAZSA panel yalnızca yukarıdan aşağı TEK GEÇİŞ çizime izin veriyor
 *              demektir; arayüz katmanı buna göre yeniden kurulmalı.
 *
 * Atlama verisi CLR_RED: neyin üzerine yazıldığı gözle görülsün.
 */
static void cmd_cursor_seek(void) {
    enum { SQ_X = 66, SQ_Y = 300, SQ_W = 40, SQ_H = 40 };
    const uint16_t CLR_BLUE = 0x001F, CLR_WHITE = 0xFFFF, CLR_RED = 0xF800;

    /* Atlama sütunu genişliği ve o genişlikte SQ_Y satır ilerlemek için gereken
     * piksel sayısı. İlk turda 1 piksellik pencere denendi ve kare hem
     * NOKTALI çıktı hem de hedeflenen satırın yarısı kadar ilerledi — bu,
     * panelin sütun aralığını 2 piksele YUVARLADIĞININ klasik imzası
     * (CASET(66,66) fiilen 66..67 oluyor, 300 piksel 300 değil 150 satır
     * ilerletiyor ve RAMWRC tek piksel kaymış hizadan devam ediyor). */
    const struct { uint32_t x1, x2; const char *name; } skip[] = {
        { SQ_X, SQ_X + 1, "2 piksel HIZALI (x1 cift, x2 tek) — ana hipotez" },
        { 0,  1,      "2 piksel HIZALI ama BASKA sutunda (x=0..1)"      },
        { SQ_X, SQ_X,     "1 piksel (ilk turda noktali cikan hal)"          },
    };

    printf("\nImlec konumlandirma testi — dar pencereyle ucuz atlama\n");
    printf("======================================================\n\n");
    printf("Her adimda hedef ayni: MAVI zemin + ORTADA 40x40 BEYAZ kare.\n");
    printf("Atlama verisi CLR_RED — nereye yazildigini gorun.\n\n");

    backlight_set(true);

    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        const uint32_t width = skip[i].x2 - skip[i].x1 + 1;
        z_background(CLR_BLUE);

        printf("  [%u] DAR ATLAMA: %s\n", (unsigned)(i + 1), skip[i].name);
        printf("      pencere %lu..%lu, %lu piksel kirmizi (= %d satir),\n",
               (unsigned long)skip[i].x1, (unsigned long)skip[i].x2,
               (unsigned long)(SQ_Y * width), SQ_Y);
        printf("      sonra pencere %d..%d ve RAMWRC ile beyaz\n", SQ_X, SQ_X + SQ_W - 1);

        qspi_caset(skip[i].x1, skip[i].x2);
        pb_lcd_stream_begin(0x2C);
        pb_lcd_stream_color(CLR_RED, SQ_Y * width);   /* satir 0 -> SQ_Y */
        pb_lcd_stream_end();
        qspi_caset(SQ_X, SQ_X + SQ_W - 1);
        pb_lcd_stream_begin(0x3C);                    /* RAMWRC — satir korunuyor mu? */
        pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
        pb_lcd_stream_end();

        hybrid_wait();
        printf("\n");
    }

    z_background(CLR_BLUE);
    printf("  [4] KONTROL (z[4] ile ayni, dogru oldugu biliniyor):\n");
    printf("      pencere %d..%d, %d satir genis kirmizi atlama, sonra beyaz\n",
           SQ_X, SQ_X + SQ_W - 1, SQ_Y);
    qspi_caset(SQ_X, SQ_X + SQ_W - 1);
    pb_lcd_stream_begin(0x2C);
    pb_lcd_stream_color(CLR_RED, (uint32_t)SQ_Y * SQ_W);
    pb_lcd_stream_color(CLR_WHITE, SQ_W * SQ_H);
    pb_lcd_stream_end();
    hybrid_wait();

    printf("\nBildirin (her adim icin uc sey):\n");
    printf("  a) beyaz kare ORTADA mi, ORTAYA YAKIN mi, UCTA mi?\n");
    printf("  b) kare DUZ mu, NOKTALI mi?\n");
    printf("  c) kirmizi INCE cizgi mi, GENIS blok mu?\n\n");
    printf("  [1] ortada + DUZ  -> panel sutunu 2'ye yuvarliyor, UCUZ\n");
    printf("      KONUMLANDIRMA VAR: y satir ilerletmek 2*y piksel.\n");
    printf("      pb_lcd_blit genel amacli kalir, `a` demosu duzelir.\n");
    printf("  [1] hala noktali/kayik -> dar pencere yolu OLU. Panel yalnizca\n");
    printf("      yukaridan asagi TEK GECIS cizime izin veriyor; arayuz\n");
    printf("      katmani (kart + serit duzeni) ona gore yeniden kurulacak.\n\n");

    qspi_caset(0, PB_PANEL_W - 1);
    pb_lcd_cursor_invalidate();
}

/**
 * `L` — yazı teşhisi. GÖZ GEREKMİYOR.
 *
 * Ekrandaki yazılar bozuk görünüyor ama teşhisin tamamı temiz çıktı:
 * WaitIdle bekliyor, CS zamanlaması doğru, sütun hizalaması tutuyor
 * (HIZASIZ 0), LVGL'in satır adımı alan genişliğine eşit (fark 0). Geriye
 * iki ihtimal kaldı ve bu komut ikisini ayırıyor:
 *
 *   - LVGL yazıyı zaten bozuk çiziyor (yazı tipi/önbellek/bellek sorunu), ya da
 *   - çizim doğru, bozulma bizim gönderme yolumuzda.
 *
 * Tek bir etiket çizdirip flush alanını, **sürücünün okuduğu indislemeyle**,
 * seri porta ASCII olarak döküyor. Terminalde "PokeBird" okunuyorsa LVGL de
 * devrik okuma da sağlam demektir.
 */
static void cmd_text_dump(void) {
    printf("\nYazi teshisi — LVGL'in cizdigi ASCII olarak dokuluyor (goz GEREKMEZ)\n");
    printf("====================================================================\n\n");

    backlight_set(true);
    pb_lcd_fill(0x0000);
    pb_lv_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "PokeBird");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 4, 4);

    /* Once tum ekrani cizdir (dokum kapali), sonra yalnizca etiketi kirlet. */
    for (int i = 0; i < 6; i++) { pb_lv_tick(); sleep_ms(10); }

    printf("Asagidaki dokum ui yoneliminde: her satir bir ui y, her karakter\n");
    printf("bir ui x. Yazi okunuyorsa LVGL ve devrik okuma SAGLAM.\n\n");
    pb_lv_request_dump(1);
    pb_lcd_request_row_dump(140);   /* DMA'ya giden s_row'un kendisi */
    lv_obj_invalidate(label);
    for (int i = 0; i < 6; i++) { pb_lv_tick(); sleep_ms(10); }

    printf("\nBitti.\n\n");
}

/**
 * `S` — dar pencerede çok satırlı yazma KAYIYOR MU? GÖZ GEREKİR.
 *
 * Yazılım yolunun tamamı ölçüldü ve temiz: LVGL'in çizimi, 90° devrik okuma,
 * satır adımı, sütun hizalaması, DMA'ya giden `s_row`'un kendisi — hepsi
 * piksel piksel doğru (`L` komutu). Doğru veri, hizalı pencereyle hatta
 * çıkıyor. Yine de ekranda yazı bozuk.
 *
 * ⚠ Şimdiye kadarki BÜTÜN ekran testleri bu hataya KÖRDÜ: `o`'nun köşeleri,
 * `z`/`j`'nin kareleri, `d`'nin renkleri, `pb_lcd_fill` — hepsi DÜZ RENK.
 * Düz bir blok satır satır kaysa bile yine düz blok görünür. Spektrogram da
 * kör: her itmede TEK satır yazıyor, hata birikemiyor. Bozulan tek şey
 * LVGL'in yazısı ve o da tek yerde: **dar pencereye çok satırlı yazma.**
 *
 * Bu test tam olarak onu sınıyor. Desen düz renk DEĞİL: her satırda aynı iki
 * pikselde beyaz nokta var, yani doğru yazılırsa iki DÜMDÜZ çizgi çıkar,
 * kayma varsa çizgiler EĞİLİR. Kaynak `row_step = 0` ile besleniyor, yani
 * her panel satırı aynı veriyi alıyor — eğilme varsa panelden gelir.
 */
static void kayma_bandi(uint32_t x1, uint32_t width, uint32_t n_pixel,
                        uint16_t color, uint32_t row) {
    static uint16_t pattern[PB_PANEL_W];
    for (uint32_t i = 0; i < PB_PANEL_W; i++) pattern[i] = 0x0000;
    pattern[8]  = color;
    pattern[24] = color;

    pb_lcd_column_window(x1, x1 + width - 1);
    pb_lcd_stream_begin(0x2C);
    for (uint32_t r = 0; r < row; r++) pb_lcd_stream_row(pattern, n_pixel);
    pb_lcd_stream_end();
    pb_lcd_cursor_invalidate();
}

/**
 * `S` — dar pencerede panelin GERÇEK satır adımı kaç piksel? GÖZ GEREKİR.
 *
 * ÖLÇÜLDÜ (kullanıcı gözle): 32 sütunluk pencereye 32 piksel/satır
 * yazınca çizgiler merdiven gibi AŞAĞI kayıyor; tam genişlikte (172) düz.
 * Yani panelin bir satırda tükettiği piksel sayısı, pencere genişliği
 * OLARAK HESAPLADIĞIMIZ değere eşit değil — ve fark yazıyı bozuyor.
 *
 * Bu tur tahmin etmiyor, ÖLÇÜYOR: aynı 32 sütunluk pencereye satır başına
 * 31 / 32 / 33 / 34 piksel yazan dört bant basılıyor. Hangisinin çizgileri
 * DÜMDÜZ çıkarsa panelin gerçek satır adımı odur. Desen bilerek düz renk
 * değil; düz blok kaysa bile düz görünür (§9n'de bütün testleri kör eden şey
 * tam olarak buydu).
 */
static void cmd_stripe_test(void) {
    enum { ROW = 200, WIDTH = 32 };

    const struct { uint32_t x1, n; uint16_t color; const char *name; } band[] = {
        {   8, 31, 0xF800, "KIRMIZI = satir basina 31 piksel" },
        {  40, 32, 0x07E0, "YESIL   = satir basina 32 piksel (su anki varsayim)" },
        {  72, 33, 0x001F, "MAVI    = satir basina 33 piksel" },
        { 104, 34, 0xFFE0, "SARI    = satir basina 34 piksel" },
    };

    printf("\nDar pencere satir adimi olcumu — hangi bant DUZ?\n");
    printf("================================================\n\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);

    /* Referans: tam genislik. Bunun duz oldugu olculdu; "duz nasil gorunur"un
     * karsilastirma olcegi olsun diye duruyor. */
    {
        static uint16_t pattern[PB_PANEL_W];
        for (uint32_t i = 0; i < PB_PANEL_W; i++) pattern[i] = 0x0000;
        pattern[148] = 0xFFFF;
        pattern[164] = 0xFFFF;
        pb_lcd_column_window(0, PB_PANEL_W - 1);
        pb_lcd_stream_begin(0x2C);
        for (uint32_t r = 0; r < ROW; r++) pb_lcd_stream_row(pattern, PB_PANEL_W);
        pb_lcd_stream_end();
        pb_lcd_cursor_invalidate();
    }

    for (size_t i = 0; i < sizeof(band) / sizeof(band[0]); i++) {
        kayma_bandi(band[i].x1, WIDTH, band[i].n, band[i].color, ROW);
        printf("  %s\n", band[i].name);
    }

    printf("\nEkranda BES cift yatay cizgi var (cihaz USB SAGDA, yatay).\n");
    printf("Hepsi ekranin SOL ucundan baslayip ortaya dogru uzaniyor.\n");
    printf("  2 BEYAZ  — tam genislikteki referans, DUZ olmali\n");
    printf("  kirmizi / yesil / mavi / sari — dort aday satir adimi\n\n");
    printf("BILDIRIN: hangi RENK cifti beyazlar gibi DUMDUZ?\n");
    printf("(digerlerinin merdiven gibi yukari ya da asagi kaymasi bekleniyor)\n\n");
    printf("Duz cikan renk, panelin dar penceredeki GERCEK satir adimini verir.\n");
    printf("Sürücü ona göre düzeltilecek — yazinin bozuk gorunmesinin sebebi bu.\n\n");
}

/**
 * Dokunmatik bring-up ve koordinat eşlemesi.
 *
 * Ham değerlerin hangi eksene/yöne karşılık geldiği varsayılmıyor, ekran
 * yönünde olduğu gibi ÖLÇÜLÜYOR: kullanıcıdan dört köşeye sırayla dokunması
 * isteniyor ve cihaz her köşenin ham değerini yazıyor. Dört satırdan eşleme
 * belirsizliğe yer bırakmadan çıkıyor.
 */
static void cmd_touch_probe(void) {
    printf("\nDokunmatik teshisi\n");
    printf("==================\n\n");

    if (!pb_touch_init()) {
        printf("Adres 0x%02x (I2C0, SDA=GPIO%d SCL=GPIO%d) yanit VERMIYOR.\n\n",
               PB_TP_I2C_ADDR, PB_PIN_TP_SDA, PB_PIN_TP_SCL);
        return;
    }
    printf("Adres 0x%02x yanit veriyor.\n\n", PB_TP_I2C_ADDR);

    /* â”€â”€ Once en temel soru: bu hatta gercekten bir sey var mi? â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * Adres taramasi. Yalnizca 0x3B yanit veriyorsa cip gercekten orada.
     * COGU adres yanit veriyorsa hat bozuk (yanlis pin, SDA takili kalmis)
     * ve okudugumuz 0xdb yalnizca gurultudur — protokol varyantlariyla
     * ugrasmak bos emek olur. Karsilastirma icin ES8311'in bulundugu i2c1
     * de taraniyor: o hat saglam oldugunu bildigimiz referans. */
    {
        printf("I2C adres taramasi:\n");
        printf("  i2c0 (dokunmatik, GPIO%d/%d):", PB_PIN_TP_SDA, PB_PIN_TP_SCL);
        int tp_count = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            uint8_t d;
            if (i2c_read_timeout_us(PB_TP_I2C_INST, a, &d, 1, false, 2000) >= 0) {
                printf(" %02x", a);
                tp_count++;
            }
        }
        printf("   (%d adet)\n", tp_count);

        printf("  i2c1 (codec,      GPIO%d/%d):", PB_PIN_I2C_SDA, PB_PIN_I2C_SCL);
        int cd_count = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (pb_i2c_probe(a)) { printf(" %02x", a); cd_count++; }
        }
        printf("   (%d adet)\n", cd_count);

        if (tp_count > 8) {
            printf("  -> i2c0'da COK FAZLA adres yanit veriyor: hat bozuk,\n");
            printf("     okunan 0xdb gurultu. Once pin/kablolama.\n\n");
        } else if (tp_count == 0) {
            printf("  -> i2c0'da HICBIR sey yok.\n\n");
        } else {
            printf("\n");
        }
    }


    /* INT hatti: cip dokunusu ALGILIYOR mu? Bu, okuma protokolunden bagimsiz
     * bir soru. INT kipirdiyorsa cip calisiyordur ve sorun yalnizca okumada. */
    gpio_init(PB_PIN_TP_INT);
    gpio_set_dir(PB_PIN_TP_INT, GPIO_IN);
    gpio_pull_up(PB_PIN_TP_INT);

    printf("Cihazi USB soketi SAGDA olacak sekilde YATAY tutun.\n\n");

    /* ── YONLENDIRMELI KALIBRASYON ────────────────────────────────────────
     *
     * NEDEN: dokunmatik calisiyor ama ekran degistirme YANLIS EKSENDE
     * tetikleniyor (kullanici DIKEY kaydiriyor, kod YATAY kaydirma sayiyor).
     * Hangi bayt ciftinin hangi fiziksel eksen oldugu ve yonun hangi tarafa
     * arttigi TAHMIN EDILEMEZ; dort kenar tek tek olculuyor.
     *
     * Her kenarda ORTANCA aliniyor, ortalama degil: cip ara sira panel disi
     * (~4000) deger veriyor ve ortalama ondan bozulur, ortanca bozulmaz. */
    {
        static uint16_t ox[256], oy[256];
        const char *edge[4] = { "SOL", "SAG", "ALT", "UST" };
        uint16_t mx[4], my[4];

        printf("KALIBRASYON — dort kenar sirayla olculecek.\n");
        printf("Her komutta parmaginizi o kenarin ORTASINA basili tutun.\n\n");

        for (int k = 0; k < 4; k++) {
            /* Pico SDK'nin stdio'su tamponlamiyor; fflush KULLANILMIYOR —
             * newlib'in fflush'i __retarget_lock_* sembollerini cekiyor ve
             * SDK onlari saglamiyor, bag hatasi veriyor. */
            printf("  >> %s kenarina simdi basili tutun", edge[k]);
            for (int g = 3; g > 0; g--) { printf(" %d", g); sleep_ms(700); }

            uint32_t n = 0;
            absolute_time_t end = make_timeout_time_ms(2000);
            while (!time_reached(end) && n < 256) {
                pb_touch_state_t st = pb_touch_read();
                if (st.ok && st.fingers > 0) { ox[n] = st.p.raw_x; oy[n] = st.p.raw_y; n++; }
                sleep_ms(8);
            }
            if (n == 0) { printf("  -> dokunma OKUNAMADI\n"); mx[k] = my[k] = 0xFFFF; continue; }

            /* Ortanca: kucukten buyuge siralayip ortayi al (n kucuk, basit
             * ekleme siralamasi yeter). */
            for (uint32_t i = 1; i < n; i++) {
                uint16_t a = ox[i], b = oy[i];
                uint32_t j = i;
                while (j > 0 && ox[j - 1] > a) { ox[j] = ox[j - 1]; j--; }
                ox[j] = a;
                j = i;
                while (j > 0 && oy[j - 1] > b) { oy[j] = oy[j - 1]; j--; }
                oy[j] = b;
            }
            mx[k] = ox[n / 2];
            my[k] = oy[n / 2];
            printf("  -> ortanca ham_x %4u  ham_y %4u   (%lu ornek, en kucuk/buyuk "
                   "x %u/%u  y %u/%u)\n",
                   mx[k], my[k], (unsigned long)n, ox[0], ox[n - 1], oy[0], oy[n - 1]);
        }

        printf("\n  SONUC:\n");
        printf("    SOL->SAG  ham_x %u -> %u   (degisim %d)\n",
               mx[0], mx[1], (int)mx[1] - (int)mx[0]);
        printf("    SOL->SAG  ham_y %u -> %u   (degisim %d)\n",
               my[0], my[1], (int)my[1] - (int)my[0]);
        printf("    ALT->UST  ham_x %u -> %u   (degisim %d)\n",
               mx[2], mx[3], (int)mx[3] - (int)mx[2]);
        printf("    ALT->UST  ham_y %u -> %u   (degisim %d)\n",
               my[2], my[3], (int)my[3] - (int)my[2]);
        printf("  Hangi ham eksen YATAY'da cok degisiyorsa arayuzun x'i odur;\n");
        printf("  degisimin isareti de yonu verir.\n\n");
    }

    printf("Simdi CANLI AKIS. Kenarlarda gezdirin; her degisiklik yaziliyor.\n");
    printf("INT sutunu dokununca 0'a dusuyorsa cip dokunusu goruyor demektir.\n");
    printf("Cikmak icin bir tusa basin.\n\n");
    printf("  INT  parmak   ham x   ham y   ilk 8 bayt\n");

    /* Adim adim "su koseye dokun" yerine CANLI AKIS: cihaz ne goruyorsa onu
     * yaziyor. Onceki surum kose kose ilerliyordu ve bazi koseleri atliyordu;
     * hatanin dokunmatikte mi kendi durum makinemde mi oldugu ayirt
     * edilemiyordu. Ham baytlari da basiyoruz — parmak sayisinin gercekten
     * bayt 1'de olup olmadigi ancak boyle gorulur. */
    uint16_t previous_x = 0xFFFF, previous_y = 0xFFFF;
    uint8_t  previous_f = 0xFF;
    int      previous_int = -1;
    uint32_t hic_yanit_yok = 0;

    while (getchar_timeout_us(0) < 0) {
        int intp = gpio_get(PB_PIN_TP_INT);
        pb_touch_state_t st = pb_touch_read();
        if (!st.ok) {
            if (++hic_yanit_yok % 100 == 1) printf("  (I2C yanit vermiyor)\n");
            sleep_ms(20);
            continue;
        }

        if (intp != previous_int || st.fingers != previous_f ||
            st.p.raw_x != previous_x || st.p.raw_y != previous_y) {
            uint8_t raw[32];
            pb_touch_last_raw(raw);
            printf("  %3d   %4u    %5u   %5u   ",
                   intp, st.fingers, st.p.raw_x, st.p.raw_y);
            for (int i = 0; i < 8; i++) printf("%02x ", raw[i]);
            printf("\n");
            previous_int = intp;
            previous_f = st.fingers;
            previous_x = st.p.raw_x;
            previous_y = st.p.raw_y;
        }
        sleep_ms(20);
    }
    printf("\ncikildi\n\n");
}

/**
 * Veri yolu teşhisi — QSPI hattında hangi varsayım tutmuyor?
 *
 * Üç başlatma dizisinin üçü de görüntü vermedi, yani sorun panelin register
 * dizisinde değil, baytların panele ulaşmasında. Bu testte veri yolundaki
 * her varsayım tek tek ölçülüyor; çoğu için ekrana bakmak GEREKMİYOR, cihaz
 * sonucu kendisi yazıyor (bkz. lastsession.md §5.9).
 */
static void cmd_datapath_probe(void) {
    printf("\nVeri yolu teshisi\n");
    printf("=================\n\n");

    /* â”€â”€ 1. Dar (8 bit) DMA yazimi bayt seritlerine kopyalaniyor mu? â”€â”€â”€â”€â”€â”€
     * Piksel verisi DMA_SIZE_8 ile PIO TX FIFO'suna yaziliyor. PIO programi
     * OSR'yi SOLA kaydiriyor, yani anlamli bayt bit 31:24'te olmali. Tek
     * baytlik bir yazimin 32 bitin tamamina kopyalanmasina guveniyoruz.
     * Kopyalanmiyorsa bayt bit 7:0'a dusuyor ve panele giden her piksel 0
     * oluyor — tum piksel yolu sessizce olu.
     *
     * Zararsiz, okunabilir bir IO register'ina (watchdog scratch) ayni
     * sekilde tek bayt yazip geri okuyoruz. */
    {
        /* Hedef: kullanilmayan bir DMA kanalinin read_addr register'i. Tam
         * 32 bit okunur-yazilir, tetiklenmedigi surece zararsiz. (Ilk
         * denemede watchdog scratch kullanilmisti; oradan 0 donuyordu, yani
         * hedef yaziya hic izin vermiyordu ve test sonucsuz kalmisti.) */
        int target = dma_claim_unused_channel(true);
        int ch    = dma_claim_unused_channel(true);
        volatile uint32_t *reg = &dma_hw->ch[target].read_addr;
        static uint8_t src = 0xA5;

        /* (a) DMA ile tek bayt */
        *reg = 0;
        dma_channel_config cfg = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&cfg, false);
        channel_config_set_write_increment(&cfg, false);
        dma_channel_configure(ch, &cfg, (void *)reg, &src, 1, true);
        dma_channel_wait_for_finish_blocking(ch);
        uint32_t dma_sonuc = *reg;

        /* (b) CPU ile tek bayt — karsilastirma icin */
        *reg = 0;
        *(volatile uint8_t *)reg = 0xA5;
        uint32_t cpu_result = *reg;

        *reg = 0;
        dma_channel_unclaim(ch);
        dma_channel_unclaim(target);

        printf("1) Dar (8 bit) IO yazimi — 0xA5:\n");
        printf("   DMA ile: 0x%08lx    CPU ile: 0x%08lx\n",
               (unsigned long)dma_sonuc, (unsigned long)cpu_result);
        if (dma_sonuc == 0xA5A5A5A5u) {
            printf("   -> bayt tum seritlere kopyalaniyor. PIO 31:24'ten okuyor,\n");
            printf("      piksel yolu bu yonden SAGLAM.\n\n");
        } else if (dma_sonuc == 0x000000A5u) {
            printf("   -> KOPYALANMIYOR. Bayt 7:0'a dusuyor, PIO ise sola kaydirip\n");
            printf("      31:24'ten okuyor: TUM PIKSEL VERISI SIFIR GIDIYOR.\n");
            printf("      Duzeltme: DMA yazma adresi ((uint8_t*)&pio->txf[sm])+3.\n\n");
        } else {
            printf("   -> beklenmeyen; elle degerlendirin.\n\n");
        }
    }

    /* â”€â”€ 2. PIO state machine calisiyor ve FIFO'yu tuketiyor mu? â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */
    {
        printf("2) PIO durumu (pio0, sm%u):\n", (unsigned)qspi.sm);
        printf("   SM etkin mi: %s\n",
               ((qspi.pio->ctrl >> qspi.sm) & 1u) ? "EVET" : "HAYIR (veri hic cikmaz)");
        printf("   PC: %u\n", (unsigned)pio_sm_get_pc(qspi.pio, qspi.sm));

        pio_sm_clear_fifos(qspi.pio, qspi.sm);
        for (int i = 0; i < 8; i++) {
            if (!pio_sm_is_tx_fifo_full(qspi.pio, qspi.sm)) {
                pio_sm_put(qspi.pio, qspi.sm, 0x0Fu << 24);
            }
        }
        uint32_t lvl_once = pio_sm_get_tx_fifo_level(qspi.pio, qspi.sm);
        sleep_ms(2);
        uint32_t lvl_sonra = pio_sm_get_tx_fifo_level(qspi.pio, qspi.sm);
        printf("   TX FIFO: yazimdan hemen sonra %lu, 2 ms sonra %lu\n",
               (unsigned long)lvl_once, (unsigned long)lvl_sonra);
        printf("   -> %s\n\n", (lvl_sonra == 0)
               ? "FIFO bosaliyor, SM veriyi tuketiyor."
               : "FIFO BOSALMIYOR. SM calismiyor veya saat durmus.");
    }

    /* â”€â”€ 3. PIO pinleri gercekten suruyor mu? â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * Saati calisilamayacak kadar yavaslatip (birkac kHz) pinleri CPU ile
     * ornekliyoruz. Gecis sayisi 0 ise PIO o pini hic surmuyor. */
    {
        printf("3) PIO pinleri suruyor mu (yavas saatte orneklendi):\n");
        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 30000.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

        uint32_t transition_sclk = 0, transition_d0 = 0;
        int previous_s = gpio_get(PIN_SCLK), previous_d = gpio_get(PIN_DIO0);
        absolute_time_t end = make_timeout_time_ms(60);
        while (!time_reached(end)) {
            if (!pio_sm_is_tx_fifo_full(qspi.pio, qspi.sm)) {
                pio_sm_put(qspi.pio, qspi.sm, 0x0Fu << 24);  /* nibble 0 sonra F */
            }
            int s = gpio_get(PIN_SCLK);
            int d = gpio_get(PIN_DIO0);
            if (s != previous_s) { transition_sclk++; previous_s = s; }
            if (d != previous_d) { transition_d0++;  previous_d = d; }
        }
        printf("   SCLK(GPIO%d) gecis: %lu   D0(GPIO%d) gecis: %lu\n",
               PIN_SCLK, (unsigned long)transition_sclk,
               PIN_DIO0, (unsigned long)transition_d0);
        printf("   -> %s\n\n", (transition_sclk > 0 && transition_d0 > 0)
               ? "PIO her iki pini de suruyor."
               : "PIN KIPIRDAMIYOR. Yanlis pin, ezilmis islev ya da olu SM.");

        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 2.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);
        pio_sm_clear_fifos(qspi.pio, qspi.sm);
    }

    /* â”€â”€ 4. Pinler elektriksel olarak saglam mi? â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * Pinleri kisa sureligine duz GPIO yapip surulen seviyeyi geri okuyoruz.
     * Bu test yanlis pin NUMARASINI yakalayamaz (bagli olmayan bir GPIO de
     * yazdiginizi geri okur); kisa devre / takili kalmis pin yakalar. */
    {
        printf("4) Pin surme/geri okuma (kisa devre testi):\n");
        const struct { uint pin; const char *name; } pins[] = {
            { PIN_SCLK, "SCLK" }, { PIN_DIO0, "D0" }, { PIN_DIO1, "D1" },
            { PIN_DIO2, "D2" },   { PIN_DIO3, "D3" }, { PIN_CS,   "CS" },
            { PIN_RST,  "RST" },
        };
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            gpio_set_function(pins[i].pin, GPIO_FUNC_SIO);
            gpio_set_dir(pins[i].pin, GPIO_OUT);
            gpio_put(pins[i].pin, 1); sleep_us(100);
            int high = gpio_get(pins[i].pin);
            gpio_put(pins[i].pin, 0); sleep_us(100);
            int low = gpio_get(pins[i].pin);
            printf("   %-4s GPIO%-2d  1->%d  0->%d  %s\n",
                   pins[i].name, pins[i].pin, high, low,
                   (high == 1 && low == 0) ? "" : "<<< TAKILI KALMIS");
        }
        printf("\n");

        /* Takili kalan pin disaridan mi suruluyor? Cikisi birakip once
         * asagi sonra yukari cekerek olcuyoruz: ikisinde de ayni seviye
         * okunuyorsa pini baska bir sey suruyor demektir. */
        printf("   Serbest birakildiginda (dahili pull ile olculdu):\n");
        const struct { uint pin; const char *name; } free[] = {
            { PIN_RST, "RST(34)" }, { PB_PIN_LCD_TE, "TE(35)" },
            { PB_PIN_LCD_BL, "BL(36)" }, { PB_PIN_BL_EN, "BL_EN(37)" },
        };
        for (size_t i = 0; i < sizeof(free) / sizeof(free[0]); i++) {
            gpio_set_function(free[i].pin, GPIO_FUNC_SIO);
            gpio_set_dir(free[i].pin, GPIO_IN);
            gpio_pull_down(free[i].pin); sleep_ms(2);
            int pd = gpio_get(free[i].pin);
            gpio_pull_up(free[i].pin);   sleep_ms(2);
            int pu = gpio_get(free[i].pin);
            gpio_disable_pulls(free[i].pin);
            const char *yorum = (pd == 0 && pu == 1) ? "serbest (normal)"
                              : (pd == 1 && pu == 1) ? "DISARIDAN YUKSEK SURULUYOR"
                              : (pd == 0 && pu == 0) ? "DISARIDAN DUSUK SURULUYOR"
                                                     : "belirsiz";
            printf("   %-10s pull-down->%d  pull-up->%d   %s\n",
                   free[i].name, pd, pu, yorum);
        }
        printf("\n");
    }

    /* â”€â”€ 5. Komutlar panele ulasiyor mu? (goz gerekir) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * DISPOFF/DISPON ve renk tersleme, piksel verisinden BAGIMSIZ olarak
     * ekranda gorunur bir degisiklik yapar. Karincalanma sonuyor ya da
     * renkleri tersine donuyorsa komut yolu calisiyor demektir ve sorun
     * yalnizca piksel yolundadir. Hicbir sey degismiyorsa panele hicbir
     * sey ulasmiyordur. */
    /* ── 5. Komutlar panele ULASIYOR MU? — TE hatti ile, goz gerekmeden ────
     * Panelin TE (tearing effect) cikisi GPIO35'e bagli. TEON (0x35) komutu
     * panelin her karede bu hatti darbelemesini saglar; TEOFF (0x34) durdurur.
     * Yani TE'yi izleyerek "komut panele ulasti mi" sorusunu ekrana bakmadan,
     * olcerek yanitlayabiliyoruz. Ayrica darbe varsa panel gercekten TARIYOR
     * demektir — ki bu da karincalanmanin panelin kendi GRAM'ini gosterdigini
     * dogrular. */
    {
        printf("5) Komut yolu testi — panelin TE cikisi (GPIO%d) dinleniyor:\n",
               PB_PIN_LCD_TE);

        /* Pinleri PIO'ya geri ver, paneli yeniden baslat */
        QSPI_GPIO_Init(qspi);
        for (uint p = PIN_SCLK; p <= PIN_DIO3; p++) pio_gpio_init(qspi.pio, p);
        LCD_3IN49_InitVariant(LCD_3IN49_INIT_FULL);

        gpio_set_function(PB_PIN_LCD_TE, GPIO_FUNC_SIO);
        gpio_set_dir(PB_PIN_LCD_TE, GPIO_IN);
        gpio_disable_pulls(PB_PIN_LCD_TE);

        /* TE gecislerini 200 ms boyunca say (60 Hz'de ~24 beklenir).
         * Test iki yonlu: TEOFF darbeleri durdurmali, TEON geri getirmeli.
         * Tek yonlu bakmak yaniltici — panel varsayilan olarak da darbeliyor
         * olabilir, nitekim ilk olcumde oyleydi. */
        #define TE_SAY() ({                                            \
            uint32_t _s = 0; int _o = gpio_get(PB_PIN_LCD_TE);         \
            absolute_time_t _b = make_timeout_time_ms(200);            \
            while (!time_reached(_b)) {                                \
                int _n = gpio_get(PB_PIN_LCD_TE);                      \
                if (_n != _o) { _s++; _o = _n; }                       \
            } _s; })

        /* Saat hizini da eleyelim: 37.5 MHz komut icin fazla hizliysa yavas
         * saatte calisir. Iki hizda da olcup karsilastiriyoruz. */
        const struct { float bolen; const char *name; } rates[] = {
            { 2.0f,  "clkdiv 2  (~37.5 MHz)" },
            { 40.0f, "clkdiv 40 (~1.9 MHz)"  },
        };

        for (size_t h = 0; h < sizeof(rates) / sizeof(rates[0]); h++) {
            pio_sm_set_clkdiv(qspi.pio, qspi.sm, rates[h].bolen);
            pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

            uint32_t base = TE_SAY();

            LCD_3IN49_SendSimpleCmd(0x34);          /* TEOFF */
            sleep_ms(20);
            uint32_t off = TE_SAY();

            QSPI_Select(qspi);                      /* TEON */
            QSPI_REGISTER_Write(qspi, 0x35);
            QSPI_DATA_Write(qspi, 0x00);
            QSPI_Deselect(qspi);
            sleep_ms(20);
            uint32_t open = TE_SAY();

            printf("   %s\n", rates[h].name);
            printf("     taban %lu  ->  TEOFF %lu  ->  TEON %lu\n",
                   (unsigned long)base, (unsigned long)off,
                   (unsigned long)open);
            if (base > 4 && off < 4 && open > 4) {
                printf("     -> KOMUTLAR ULASIYOR. Panel emirlere uyuyor.\n");
            } else if (base > 4) {
                printf("     -> panel tariyor ama komutlara UYMUYOR.\n");
            } else {
                printf("     -> TE hic darbelemiyor; panel taramiyor.\n");
            }
        }
        printf("\n");
        pio_sm_set_clkdiv(qspi.pio, qspi.sm, 2.0f);
        pio_sm_clkdiv_restart(qspi.pio, qspi.sm);
        #undef TE_SAY
    }

    /* â”€â”€ 6. Bit-bang: PIO'yu denklemden cikar, panele kimligini sor â”€â”€â”€â”€â”€â”€â”€
     * PIO calisiyor, pinler kipirdiyor, CS zamanlamasi duzeltildi — ama panel
     * hala uymuyor. Geriye iki ihtimal kaliyor: PIO'nun urettigi dalga sekli
     * yanlis, ya da sorun hattin/panelin kendisinde. Bit-bang ikisini ayirir.
     * Ayrica okuma yapabildigi icin panelin kimligini sorabiliyoruz. */
    {
        printf("6) Bit-bang testi (PIO devre disi, ~500 kHz):\n");
        pio_sm_set_enabled(qspi.pio, qspi.sm, false);
        bb_pins_setup();

        gpio_set_function(PB_PIN_LCD_TE, GPIO_FUNC_SIO);
        gpio_set_dir(PB_PIN_LCD_TE, GPIO_IN);
        gpio_disable_pulls(PB_PIN_LCD_TE);

        uint32_t base = te_transition_say();
        bb_cmd(0x34, NULL, 0);                 /* TEOFF */
        sleep_ms(20);
        uint32_t off = te_transition_say();
        uint8_t param = 0x00;
        bb_cmd(0x35, &param, 1);               /* TEON */
        sleep_ms(20);
        uint32_t open = te_transition_say();

        printf("   TE: taban %lu -> TEOFF %lu -> TEON %lu\n",
               (unsigned long)base, (unsigned long)off, (unsigned long)open);
        printf("   -> %s\n", (base > 4 && off < 4 && open > 4)
               ? "BIT-BANG CALISIYOR. Hata PIO dalga seklinde."
               : "bit-bang de etkisiz. Sorun PIO'da degil.");

        /* Panelden oku: 0x04 = RDDID, 0x0A = guc modu, 0x0C = piksel bicimi.
         * Hepsi 0x00 ya da hepsi 0xFF gelirse panel hic yanit vermiyordur
         * (hat sirasiyla asagi ya da yukari cekili kaliyor). */
        const struct { uint8_t reg; const char *name; } reads[] = {
            { 0x04, "RDDID  (uretici/surum/kimlik)" },
            { 0x0A, "RDDPM  (guc modu)" },
            { 0x0C, "RDDCOLMOD (piksel bicimi)" },
        };
        int anlamli = 0;
        for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
            uint8_t buf[6] = {0};
            bb_read(reads[i].reg, buf, sizeof(buf));
            printf("   0x%02x %-28s:", reads[i].reg, reads[i].name);
            for (size_t j = 0; j < sizeof(buf); j++) printf(" %02x", buf[j]);
            printf("\n");
            for (size_t j = 0; j < sizeof(buf); j++) {
                if (buf[j] != 0x00 && buf[j] != 0xFF) anlamli = 1;
            }
        }
        printf("   -> %s\n\n", anlamli
               ? "PANEL YANIT VERIYOR. Veri hatti iki yonlu calisiyor."
               : "PANELDEN HIC YANIT YOK (hep 00 ya da FF). Panel bu hattan\n"
                 "      bizi duymuyor: pin haritasi ya da kablolama yanlis.");

        /* PIO'yu geri ac — SM'i de sifirliyor (FIFO/kaydirma sayaci dahil),
         * eski elle yapilan geri alma bunlari birakiyordu. */
        QSPI_GPIO_Init(qspi);
        QSPI_PIO_Restore(qspi);
    }

    /* â”€â”€ 7. Yedek: gozle komut yolu testi â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * Olculebilir testler sonucsuz kalirsa diye duruyor. */
    {
        printf("7) Komut yolu testi — EKRANA BAKIN.\n");
        printf("   Karincalanmada HERHANGI bir degisiklik (sonme, renklerin\n");
        printf("   tersine donmesi, parlaklik oynamasi) gorurseniz TUSA BASIN.\n\n");

        backlight_set(true);
        drain_stdin();

        const struct { uint8_t cmd; const char *name; } steps[] = {
            { 0x28, "DISPOFF  (ekran sonmeli)" },
            { 0x29, "DISPON   (geri gelmeli)" },
            { 0x21, "INVON    (renkler terslenmeli)" },
            { 0x20, "INVOFF   (geri donmeli)" },
            { 0x28, "DISPOFF  (ekran sonmeli)" },
            { 0x29, "DISPON   (geri gelmeli)" },
        };
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            LCD_3IN49_SendSimpleCmd(steps[i].cmd);
            printf("   0x%02x %s\n", steps[i].cmd, steps[i].name);
            for (int t = 0; t < 20; t++) {
                if (getchar_timeout_us(0) >= 0) {
                    printf("\n   >>> KOMUT YOLU CALISIYOR — degisiklik 0x%02x sonrasi <<<\n",
                           steps[i].cmd);
                    printf("   Komutlar panele ulasiyor; sorun yalnizca piksel yolunda.\n\n");
                    LCD_3IN49_SendSimpleCmd(0x29);
                    return;
                }
                sleep_ms(100);
            }
        }
        LCD_3IN49_SendSimpleCmd(0x29);
        printf("\n   Hicbir degisiklik bildirilmedi — panele HICBIR komut\n");
        printf("   ulasmiyor. Sorun QSPI hattinin kendisinde (pin/CS/saat).\n\n");
    }
}

/**
 * Arka ışık teşhisi — hangi pin kombinasyonu ışığı yakıyor?
 *
 * Reset sonrası (hiçbir kodumuz çalışmadan) ışık yanıyor, bizim kod
 * çalışınca sönüyor. Demek ki iki pinden biri yanlış sürülüyor:
 *   GPIO36 (LCD_BL)  — parlaklık, PWM
 *   GPIO37 (BL_EN)   — yükseltici enable
 * Ama polariteyi ve hangisinin gerçekten enable olduğunu varsaymak yerine
 * dört kombinasyonu tek tek deneyip hangisinde ışık yandığını soruyoruz.
 *
 * Ayrıca GPIO36'yı PWM yerine düz GPIO olarak da deniyoruz: PWM işlevi
 * QSPI_GPIO_Init tarafından ezilmiş olabilir.
 */
static void cmd_backlight_probe(void) {
    /* Kendi kendini raporlayan sürüm: durumları sırayla dener, ışığı
     * gördüğünüzde bir tuşa basın; cihaz o anki durumu yazar. Önceki
     * sürümde adımları göz kararı saymak gerekiyordu ve karışıyordu. */
    printf("\nArka isik teshisi (etkilesimli).\n");
    printf("EKRANA BAKIN. Isik yandigi anda bir tusa basin.\n");
    printf("Her durum 5 saniye. Hicbiri yanmazsa test kendiliginden biter.\n\n");

    gpio_init(PB_PIN_BL_EN);
    gpio_set_dir(PB_PIN_BL_EN, GPIO_OUT);

    const struct { int bl_en; int bl; int pwm_duty; const char *name; } state[] = {
        { 1, 0, -1, "BL_EN=1  LCD_BL=0 (duz GPIO)" },
        { 1, 1, -1, "BL_EN=1  LCD_BL=1 (duz GPIO)" },
        { 0, 0, -1, "BL_EN=0  LCD_BL=0 (duz GPIO)" },
        { 0, 1, -1, "BL_EN=0  LCD_BL=1 (duz GPIO)" },
        { 1, 0,  5, "BL_EN=1  PWM duty %5   (aktif-dusuk ise PARLAK)" },
        { 1, 0, 95, "BL_EN=1  PWM duty %95  (aktif-yuksek ise PARLAK)" },
    };

    for (size_t i = 0; i < sizeof(state) / sizeof(state[0]); i++) {
        gpio_put(PB_PIN_BL_EN, state[i].bl_en);
        if (state[i].pwm_duty < 0) {
            gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_SIO);
            gpio_set_dir(PB_PIN_LCD_BL, GPIO_OUT);
            gpio_put(PB_PIN_LCD_BL, state[i].bl);
        } else {
            gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_PWM);
            pwm_set_gpio_level(PB_PIN_LCD_BL,
                               (uint16_t)((BL_PWM_WRAP - 1) * state[i].pwm_duty / 100));
        }
        printf("  [%u] %s\n", (unsigned)(i + 1), state[i].name);

        for (int t = 0; t < 50; t++) {
            if (getchar_timeout_us(0) >= 0) {
                printf("\n  >>> ISIK YANAN DURUM: [%u] %s <<<\n\n",
                       (unsigned)(i + 1), state[i].name);
                return;
            }
            sleep_ms(100);
        }
    }
    printf("  hicbir durumda tus basilmadi\n\n");
}


/* ── x: cihaz-içi doğrulama seti + arena + çıkarım süresi ──────────────────
 *
 * M6'nın kabul ölçütü bu komut. Üç şeyi birden ölçüyor ve üçü de TAHMİN
 * DEĞİL ÖLÇÜM olsun diye buraya kondu (§9l madde 2, 4, 7):
 *
 *   1. arena_used_bytes()  — belgedeki 141 KB bir tahmindi, bu gerçeği verir
 *   2. Invoke() süresi     — 1 s'lik pencere adımına sığmalı
 *   3. logit karşılaştırma — "PC'de çalışıyor cihazda çalışmıyor"u yakalar
 *
 * Ses yolu bilerek İŞİN DIŞINDA: girdi, eğitim kümesinden gömülmüş hazır bir
 * pencere. Fark çıkarsa mel'e bakmaya gerek yok, hata alanı TFLM/CMSIS-NN/
 * niceleştirme ile sınırlı. Mikrofon da gerekmiyor — kulaklık kuralı (§5.5)
 * bu testi hiç ilgilendirmiyor.
 */
static void cmd_ai_verify(void) {
    printf("\n=== TUR AGI — cihaz ici dogrulama ===\n");

    const uint32_t t_init0 = time_us_32();
    if (!pb_species_net_init()) {
        printf("[!] model baslatilamadi.\n");
        return;
    }
    const uint32_t t_init = time_us_32() - t_init0;

    printf("baslatma      %lu us\n", (unsigned long)t_init);
    printf("arena         %u / %u bayt kullanildi  (%.1f%%)\n",
           (unsigned)pb_species_net_arena_used(),
           (unsigned)pb_species_net_arena_total(),
           100.0 * pb_species_net_arena_used() / pb_species_net_arena_total());
    printf("cikti nicel.  olcek %.9f  sifir %d\n",
           (double)pb_species_net_output_scale(), pb_species_net_output_zero());

    int8_t *input = pb_species_net_input();
    const int8_t *output = pb_species_net_output();

    uint32_t time_min = 0xFFFFFFFFu, time_max = 0, time_total = 0;
    int exact = 0, pred_matches = 0, max_diff = 0;
    long diff_total = 0;
    long diff_count = 0;

    for (int k = 0; k < PB_VALIDATION_COUNT; k++) {
        memcpy(input, pb_validation_input[k],
               (size_t)PB_VALIDATION_FRAMES * PB_VALIDATION_BANDS);
        if (!pb_species_net_run()) {
            printf("[!] pencere %d: Invoke basarisiz\n", k);
            return;
        }
        const uint32_t us = pb_species_net_last_time_us();
        if (us < time_min) time_min = us;
        if (us > time_max) time_max = us;
        time_total += us;

        int diff_max = 0, best = 0;
        for (int c = 0; c < PB_VALIDATION_CLASSES; c++) {
            int d = (int)output[c] - (int)pb_validation_logit[k][c];
            if (d < 0) d = -d;
            if (d > diff_max) diff_max = d;
            diff_total += d;
            diff_count++;
            if (output[c] > output[best]) best = c;
        }
        if (diff_max == 0) exact++;
        if (diff_max > max_diff) max_diff = diff_max;
        if (best == pb_validation_pc_pred[k]) pred_matches++;

        printf("  pencere %d  sinif %3d  cihaz-tahmin %3d  PC-tahmin %3d  "
               "logit max fark %d  %lu us\n",
               k, (int)pb_validation_class[k], best,
               (int)pb_validation_pc_pred[k], diff_max, (unsigned long)us);
    }

    printf("\nsure          min %lu  ort %lu  max %lu us   (hedef < 1.000.000)\n",
           (unsigned long)time_min,
           (unsigned long)(time_total / PB_VALIDATION_COUNT),
           (unsigned long)time_max);
    printf("logit         %d/%d pencere BIREBIR ayni, en buyuk fark %d, "
           "ort mutlak fark %.4f\n",
           exact, PB_VALIDATION_COUNT, max_diff,
           (double)diff_total / (double)diff_count);
    printf("tahmin        %d/%d pencere ayni sinifi sectik\n",
           pred_matches, PB_VALIDATION_COUNT);

    if (exact == PB_VALIDATION_COUNT) {
        printf("\nSONUC: cihaz PC ile BIREBIR ayni. TFLM hatti dogru.\n");
    } else if (pred_matches == PB_VALIDATION_COUNT) {
        printf("\nSONUC: logit'lerde kucuk sapma var ama tahminler ayni.\n"
               "       Sapma 1-2 adimi asiyorsa cekirdek farki arayin.\n");
    } else {
        printf("\n[!] SONUC: cihaz PC'den FARKLI tahmin uretti. TFLM/CMSIS-NN\n"
               "    veya nicelestirme tarafinda sorun var. Ses yolu bu teste\n"
               "    hic girmedi, o yuzden mel'e bakmayin.\n");
    }
}

/* ── X: IKILI AG — cihaz ici dogrulama (M7, §9o adim 3) ───────────────────
 *
 * `x`'in Asama-1 ikili ag karsiligi: ayni pencereleri PC (BUILTIN_REF) ve
 * cihazda modelden gecirip ham int8 logit'i BIREBIR karsilastirir. Ses yolu
 * (mikrofon, mel) bu teste hic girmiyor — fark cikarsa sorun TFLM/CMSIS-NN
 * veya nicelestirme tarafinda demektir.
 */
static void cmd_binary_ai_verify(void) {
    printf("\n=== IKILI AG — cihaz ici dogrulama ===\n");

    const uint32_t t_init0 = time_us_32();
    if (!pb_binary_net_init()) {
        printf("[!] model baslatilamadi.\n");
        return;
    }
    const uint32_t t_init = time_us_32() - t_init0;

    printf("baslatma      %lu us\n", (unsigned long)t_init);
    printf("arena         %u / %u bayt kullanildi  (%.1f%%)\n",
           (unsigned)pb_binary_net_arena_used(),
           (unsigned)pb_binary_net_arena_total(),
           100.0 * pb_binary_net_arena_used() / pb_binary_net_arena_total());
    printf("cikti nicel.  olcek %.9f  sifir %d\n",
           (double)pb_binary_net_output_scale(), pb_binary_net_output_zero());

    int8_t *input = pb_binary_net_input();

    uint32_t time_min = 0xFFFFFFFFu, time_max = 0, time_total = 0;
    int exact = 0, pred_matches = 0, max_diff = 0;

    for (int k = 0; k < PB_BINARY_VALIDATION_COUNT; k++) {
        memcpy(input, pb_binary_validation_input[k],
               (size_t)PB_BINARY_VALIDATION_FRAMES * PB_BINARY_VALIDATION_BANDS);
        if (!pb_binary_net_run()) {
            printf("[!] pencere %d: Invoke basarisiz\n", k);
            return;
        }
        const uint32_t us = pb_binary_net_last_time_us();
        if (us < time_min) time_min = us;
        if (us > time_max) time_max = us;
        time_total += us;

        const int8_t cihaz_logit = pb_binary_net_output();
        int diff = (int)cihaz_logit - (int)pb_binary_validation_logit[k];
        if (diff < 0) diff = -diff;
        if (diff == 0) exact++;
        if (diff > max_diff) max_diff = diff;

        const bool cihaz_pred = pb_binary_net_probability() >= 0.5f;
        const bool pc_pred = pb_binary_validation_logit[k] >=
                               pb_binary_net_output_zero();  /* logit>=0 esdeger */
        if (cihaz_pred == pc_pred) pred_matches++;

        printf("  pencere %d  gercek %-5s  cihaz-logit %4d  PC-logit %4d  "
               "fark %d  p=%.4f  %lu us\n",
               k, pb_binary_validation_truth[k] ? "KUS" : "DEGIL",
               cihaz_logit, pb_binary_validation_logit[k], diff,
               (double)pb_binary_net_probability(), (unsigned long)us);
    }

    printf("\nsure          min %lu  ort %lu  max %lu us\n",
           (unsigned long)time_min,
           (unsigned long)(time_total / PB_BINARY_VALIDATION_COUNT),
           (unsigned long)time_max);
    printf("logit         %d/%d pencere BIREBIR ayni, en buyuk fark %d\n",
           exact, PB_BINARY_VALIDATION_COUNT, max_diff);
    printf("karar         %d/%d pencere PC ile ayni yonde (>=0.5 esigi)\n",
           pred_matches, PB_BINARY_VALIDATION_COUNT);

    if (exact == PB_BINARY_VALIDATION_COUNT) {
        printf("\nSONUC: cihaz PC ile BIREBIR ayni. TFLM hatti dogru.\n");
    } else if (pred_matches == PB_BINARY_VALIDATION_COUNT) {
        printf("\nSONUC: logit'te kucuk sapma var ama karar ayni.\n");
    } else {
        printf("\n[!] SONUC: cihaz PC'den FARKLI karar uretti. TFLM/CMSIS-NN\n"
               "    veya nicelestirme tarafinda sorun var.\n");
    }
}

/* ── k: gerçek zamanlı tanıma (core 1) ────────────────────────────────────
 *
 * M6'nın asıl teslimi. Core 1 sesi okuyup mel çıkarıyor, kapı açılınca
 * saniyede bir tür ağını çalıştırıyor ve son 8 pencereyi birleştiriyor;
 * core 0 (burası) yalnızca sonucu basıyor.
 *
 * ⛔ AKUSTİK TEST İÇİN PC'DEN SES ÇALMAYIN (§5.5): bilgisayarda kulaklık
 * takılı, hoparlörden ses çıkmıyor. Bu komutun kuş sesiyle sınanması
 * gerekiyorsa kullanıcıdan isteyin. Modelin doğru çalıştığı zaten `x`
 * komutuyla mikrofona hiç dokunmadan kanıtlanıyor.
 */
static void cmd_recognize(bool gate_ignore) {
    printf("\n=== GERCEK ZAMANLI TANIMA (core 1) ===\n");
    if (gate_ignore)
        printf("OLCUM KIPI: kapi YOKSAYILIYOR, her saniye cikarim.\n");
    else
        printf("Kapi acilmadikca cikarim CALISMAZ (sessizlikte %%2-3).\n");
    printf("Birlestirme penceresi: %d\n", PB_VOTE_WINDOWS);
    printf("Cikmak icin bir tusa basin.\n\n");

    if (!pb_recognizer_start(gate_ignore)) {
        printf("[!] tanima hatti baslatilamadi.\n");
        return;
    }

    uint32_t seen = 0;
    absolute_time_t next = make_timeout_time_ms(1000);

    while (getchar_timeout_us(0) < 0) {
        pb_recognizer_state_t d;
        pb_recognizer_read(&d);

        if (d.version != seen && d.valid) {
            seen = d.version;
            printf("  [%lu] %lu pencere birlesti, %lu us:\n",
                   (unsigned long)d.inference, (unsigned long)d.merged,
                   (unsigned long)d.last_time_us);
            for (int r = 0; r < 3; r++) {
                const int c = d.top3[r];
                if (c < 0 || c >= PB_CLASS_COUNT) continue;
                printf("      %d. %%%5.1f  %-10s %s\n", r + 1,
                       (double)(d.top3_probability[r] * 100.0f),
                       pb_class_code[c], pb_class_name[c]);
            }
        }

        if (time_reached(next)) {
            printf("  kare %lu  kapi %%%lu  ikili %lu (red %lu, son p=%.2f)  "
                   "cikarim %lu  atlanan %lu  bant %.1f dB  taban %.1f dB  "
                   "overrun %lu\n",
                   (unsigned long)d.frame,
                   (unsigned long)(d.frame ? d.gate_open * 100 / d.frame : 0),
                   (unsigned long)d.binary_ran, (unsigned long)d.binary_red,
                   (double)d.binary_last_p,
                   (unsigned long)d.inference, (unsigned long)d.skipped,
                   (double)d.band_db, (double)d.base_db,
                   (unsigned long)d.overrun);
            next = make_timeout_time_ms(1000);
        }
        sleep_ms(20);
    }

    pb_recognizer_state_t d;
    pb_recognizer_read(&d);
    pb_recognizer_stop();

    printf("\n  toplam kare %lu (%lu kapi acik, %%%lu)\n",
           (unsigned long)d.frame, (unsigned long)d.gate_open,
           (unsigned long)(d.frame ? d.gate_open * 100 / d.frame : 0));
    printf("  cikarim %lu, kapi kapali diye atlanan pencere %lu\n",
           (unsigned long)d.inference, (unsigned long)d.skipped);
    printf("  ikili ag calisti %lu, \"kus degil\" dedigi (tur agi atlandi) %lu\n",
           (unsigned long)d.binary_ran, (unsigned long)d.binary_red);
    printf("  ses halkasi overrun %lu  (0 olmali — degilse cikarim halkadan\n"
           "                            uzun suruyor, audio_i2s.h'ye bakin)\n\n",
           (unsigned long)d.overrun);
}

/* ── c: SONUÇ EKRANI — M7'nin ilk teslimi ─────────────────────────────────
 *
 * `k` ne yapıyorsa aynısı, ama sonuç seri porta değil EKRANA gidiyor:
 * solda tanıma kartı (tür adı + güven + ilk 3 + sayaçlar), sağda canlı
 * spektrogram. Aradaki karar kuralı ai/decision.c'de; eşikleri ölçüldü
 * (models/thresholds.txt, tools/measure_thresholds.py).
 *
 * İŞ BÖLÜMÜ:
 *   core 1  ses -> mel -> kapı -> tür ağı -> birleştirme   (ai/recognizer.c)
 *   core 0  burası: kuyruğu boşalt, karar kuralı, LVGL, QSPI
 *
 * Mel halkası motor çalışırken core 1'in malı (ai/recognizer.h uyarısı); core 0
 * spektrogram sütunlarını `pb_recognizer_get_mel()` kuyruğundan alıyor, mel'e
 * doğrudan dokunmuyor.
 *
 * ⛔ AKUSTİK DOĞRULAMAYI PC'DEN SES ÇALARAK YAPMAYIN (§5.5): bilgisayarda
 * kulaklık takılı. Bu ekranın kuş sesiyle sınanmasını kullanıcıdan isteyin.
 */
static void cmd_result_ekrani(void) {
    printf("\n=== SONUC EKRANI (M7) ===\n");
    printf("Cihazi USB soketi SAGDA olacak sekilde yatay tutun.\n");
    printf("EKRAN 0 dinleme (solda ilk 3 tur, sagda spektrogram)\n");
    printf("EKRAN 1 gunluk  (tam genislik, taninan turlerin listesi)\n");
    printf("Gecis: ekranda YATAY KAYDIRMA, ya da bosluk/'n' tusu.\n");
    printf("Karar kurali: girme %.2f / cikma %.2f, en az %u pencere, "
           "tutma %u ms\n", (double)PB_DECISION_ENTER_THRESHOLD,
           (double)PB_DECISION_EXIT_THRESHOLD, (unsigned)PB_DECISION_MIN_WINDOWS,
           (unsigned)PB_DECISION_HOLD_MS);
    printf("Cikmak icin baska bir tusa basin.\n\n");

    backlight_set(true);
    /* `a` demosundaki gerekçe: fill, lcd_blit'in atlama şeridini bilinen bir
     * hâle getiriyor — teşhis komutlarından sonra iz kalmasın. */
    pb_lcd_fill(0x0000);
    pb_lv_flush_counters_reset();
    pb_lv_init();

    pb_ui_create();
    /* Arayüzü önce çiz, SONRA sağ şeridi spektrograma ver: LVGL'in ilk çizimi
     * sahip olduğu dilimlerin tamamını boyuyor. */
    for (int i = 0; i < 4; i++) { pb_ui_tick(); sleep_ms(5); }
    pb_spec_init();

    pb_mel_init();   /* filtre bankası core 1 başlamadan hazır olsun */

    /* ⚠ HAT AÇILIŞTA ÇALIŞMIYOR — cihaz artık sürekli dinlemiyor
     * (kullanıcının kararı). Dinlemeyi kayıt butonu başlatıyor; bu döngü her
     * turda `pb_ui_recording()`ya bakıp core 1'i gerçekten başlatıp
     * durduruyor. `pb_recognizer_start`/`durdur` bu kullanıma uygun:
     * ikisi de `s_calis` ile korumalı ve başlatma core 1'i sıfırdan kuruyor.
     * ⚠ `pb_recognizer_stop` core 1'in döngüden çıkmasını beklemek için
     * 400 ms bloklanıyor — butona basınca arayüz o kadar takılır. */
    bool line_running = false;

    pb_decision_t decision;
    pb_decision_reset(&decision, to_ms_since_boot(get_absolute_time()));

    uint32_t seen = 0, kare0 = 0, last_rate = 0;
    uint32_t decision_version = decision.version;
    absolute_time_t next_rate  = make_timeout_time_ms(1000);
    absolute_time_t next_card = make_timeout_time_ms(250);
    pb_recognizer_state_t d;
    memset(&d, 0, sizeof(d));

    drain_stdin();
    for (;;) {
        /* Boşluk / 'n' ekran değiştiriyor, başka her tuş çıkıyor. Kaydırmanın
         * yedeği bu: dokunmatik bir kareyi düşürse bile ekran değiştirilebilir
         * kalıyor (arayuz.h'deki gerekçe). */
        const int key = getchar_timeout_us(0);
        if (key >= 0) {
            if (key == ' ' || key == 'n' || key == 'N') pb_ui_next();
            else if (key == 'r' || key == 'R')
                pb_ui_set_recording(!pb_ui_recording());
            else break;
        }

        /* 0) Kayıt durumu değiştiyse hattı gerçekten başlat/durdur. */
        if (pb_ui_recording() != line_running) {
            if (pb_ui_recording()) {
                if (pb_recognizer_start(false)) {
                    line_running = true;
                    pb_decision_reset(&decision, to_ms_since_boot(get_absolute_time()));
                    decision_version = decision.version;
                    seen = 0;
                    printf("  [kayit BASLADI]\n");
                } else {
                    printf("[!] tanima hatti baslatilamadi.\n");
                    pb_ui_set_recording(false);
                }
            } else {
                pb_recognizer_stop();
                line_running = false;
                memset(&d, 0, sizeof(d));
                pb_decision_reset(&decision, to_ms_since_boot(get_absolute_time()));
                decision_version = decision.version;
                printf("  [kayit DURDU]\n");
            }
        }

        if (!line_running) {
            /* Boştayken ekran yalnızca "BOŞTA" gösteriyor; sonuç alanı
             * boşaltılıyor ki durmuş bir tahmin canlıymış gibi durmasın. */
            pb_result_view_t empty;
            memset(&empty, 0, sizeof(empty));
            empty.mode = PB_DECISION_LISTENING;
            pb_ui_update(&empty);
            pb_ui_tick();
            sleep_ms(10);
            continue;
        }

        /* 1) Spektrogram: core 1'in bıraktığı mel sütunlarını boşalt.
         *    Tur başına en fazla 8 sütun — çıkarım sonrası birikmiş kuyruk
         *    tek turda boşaltılmaya çalışılırsa arayüz o turda takılır. */
        int8_t mel_q[PB_MEL_BANDS];
        for (int i = 0; i < 8 && pb_recognizer_get_mel(mel_q); i++) {
            /* ⚠ Günlük ekranındayken spektrogram YAZMAMALI: o ekranda sağdaki
             * iki dilim de LVGL'in (arayuz.c, dilim sahipliği) ve ikisi aynı
             * bölgeye yazarsa birbirlerini siler. Kuyruk yine de boşaltılıyor,
             * yoksa core 1 dolu kuyruğa kare atmaya başlar. */
            if (pb_ui_screen() != PB_SCREEN_LISTEN) continue;

            uint8_t bins[PB_MEL_BANDS];
            for (int b = 0; b < PB_MEL_BANDS; b++) {
                /* `a` demosuyla AYNI gösterim penceresi: -75..-15 dB. */
                float v = (pb_mel_q_to_db(mel_q[b]) + 75.0f) * (255.0f / 60.0f);
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                bins[b] = (uint8_t)v;
            }
            pb_spec_push_column(bins, PB_MEL_BANDS);
        }

        /* 2) Tanıma durumu -> karar kuralı. */
        pb_recognizer_read(&d);
        pb_decision_input_t gi = {
            .now_ms   = to_ms_since_boot(get_absolute_time()),
            .gate_open  = d.gate_now,
            .fresh_result = d.valid && d.version != seen,
            .cls      = d.valid ? d.top3[0] : (int16_t)-1,
            .probability   = d.valid ? d.top3_probability[0] : 0.0f,
            .merged   = d.merged,
        };
        if (gi.fresh_result) seen = d.version;
        pb_decision_update(&decision, &gi);

        /* Ekranda görünen her değişiklik seri porta da düşsün: bu komutun
         * göz gerektirmeyen kaydı bu — kullanıcı ekrana bakarken ben aynı
         * olayları terminalde okuyabiliyorum. */
        if (decision.version != decision_version) {
            decision_version = decision.version;
            const int c = decision.cls;
            printf("  [%lu ms] %-14s %s %s  %%%.1f\n",
                   (unsigned long)gi.now_ms, pb_decision_mode_name(decision.mode),
                   (c >= 0 && c < PB_CLASS_COUNT) ? pb_class_code[c] : "-",
                   (c >= 0 && c < PB_CLASS_COUNT) ? pb_class_name[c] : "",
                   (double)(decision.confidence * 100.0f));
        }

        if (time_reached(next_rate)) {
            last_rate = d.frame - kare0;
            kare0 = d.frame;
            next_rate = make_timeout_time_ms(1000);
        }

        /* Kartı 4 Hz güncelle. Her güncelleme kartın QSPI'ye yeniden basılması
         * (68,8 KB) demek; sonuç ekranında daha hızlısının bir karşılığı yok. */
        if (time_reached(next_card)) {
            pb_result_view_t view;
            memset(&view, 0, sizeof(view));
            view.mode = decision.mode;
            view.confidence = decision.confidence;
            view.species_name = (decision.cls >= 0 && decision.cls < PB_CLASS_COUNT)
                            ? pb_class_name[decision.cls] : NULL;
            if (d.valid) {
                for (int r = 0; r < 3; r++) {
                    const int c = d.top3[r];
                    if (c >= 0 && c < PB_CLASS_COUNT) {
                        view.top3_name[r]    = pb_class_name[c];
                        view.top3_latin[r] = pb_class_latin[c];
                    }
                    view.top3_probability[r] = d.top3_probability[r];
                }
            }
            view.frame_rate = last_rate;
            view.inference = d.inference;
            view.merged = d.merged;
            view.overrun = d.overrun;
            view.band_db = d.band_db;
            pb_ui_update(&view);
            next_card = make_timeout_time_ms(250);
        }

        pb_ui_tick();
        sleep_ms(2);
    }

    pb_recognizer_read(&d);
    pb_recognizer_stop();

    printf("\n  kare %lu (kapi acik %%%lu), cikarim %lu, atlanan %lu, "
           "overrun %lu\n",
           (unsigned long)d.frame,
           (unsigned long)(d.frame ? d.gate_open * 100 / d.frame : 0),
           (unsigned long)d.inference, (unsigned long)d.skipped,
           (unsigned long)d.overrun);
    printf("  LVGL flush %lu, satir adimi != alan_w: %lu, panel_w %lu..%lu, "
           "dilim basimi %lu\n",
           (unsigned long)pb_lv_flush_count,
           (unsigned long)pb_lv_flush_stride_differs,
           (unsigned long)pb_lv_flush_w_min, (unsigned long)pb_lv_flush_w_max,
           (unsigned long)pb_lv_slice_press);
    /* Kaydırma teşhisi — GÖZ GEREKMEZ. Kaydırma çalışmıyorsa hangi aşamada
     * durduğu buradan okunuyor: dokunma hiç gelmiyor mu, geliyor da hareket
     * eşiği mi aşılmıyor, yoksa panel dışı kareler mi düşürülüyor. */
    printf("  kaydirma: dokunma %lu, basla %lu, kabul %lu, kisa/egik %lu, "
           "panel disi %lu, son ham dx %ld (esik 200), ham_y %ld, "
           "buton basim %lu\n\n",
           (unsigned long)pb_swipe_touch, (unsigned long)pb_swipe_begin,
           (unsigned long)pb_swipe_accept, (unsigned long)pb_swipe_short,
           (unsigned long)pb_lv_touch_invalidate,
           (long)pb_swipe_last_dx, (long)pb_swipe_last_dy,
           (unsigned long)pb_button_press);
}

/* ── C: SONUÇ KARTI GÖSTERİM TESTİ (mikrofonsuz) ──────────────────────────
 *
 * NEDEN AYRI BİR KOMUT: `c` ancak gerçek bir kuş sesi duyulursa tür adı
 * yazıyor. Sessiz odada kart hep "dinliyor" gösterir, yani ASIL çizim yolu
 * (uzun tür adı, sarma, güven, ilk 3, renkler) hiç sınanmaz. Bu komut aynı
 * kartı sahte bir sonuç dizisiyle sürüyor: göz testi tek bakışta yapılabilsin.
 *
 * KARAR KURALI BİLEREK DEVREDE DEĞİL — burada sınanan şey ÇİZİM. Kuralın
 * kendisi host testlerinde (test/dsp_test.c, `test_karar`) zaman ilerletilerek
 * sınanıyor; ikisini karıştırmak, bir hata çıktığında hangisinde olduğunu
 * belirsizleştirirdi.
 *
 * Desen düz renk DEĞİL (§9o uyarısı): en uzun tür adı, iki alternatif satırı
 * ve sağda hareketli bir spektrogram deseni var — kayma olursa yazıda görünür.
 */
static void cmd_result_karti_demo(void) {
    /* En UZUN tür adını bul: sarmanın ve kenarların en kötü durumu bu.
     * İndeks sabitlemek yerine aramak, sınıf tablosu yeniden üretilse de
     * testin en kötü durumu göstermeye devam etmesini sağlıyor. */
    int lengthy = 0;
    for (int i = 0; i < PB_CLASS_COUNT; i++) {
        if (strlen(pb_class_name[i]) > strlen(pb_class_name[lengthy])) lengthy = i;
    }
    const int second = (lengthy + 1) % PB_CLASS_COUNT;
    const int third = (lengthy + 2) % PB_CLASS_COUNT;

    printf("\n=== SONUC KARTI GOSTERIM TESTI (mikrofon YOK) ===\n");
    printf("Cihazi USB soketi SAGDA olacak sekilde yatay tutun.\n");
    printf("En uzun tur adi: \"%s\" (%d karakter)\n",
           pb_class_name[lengthy], (int)strlen(pb_class_name[lengthy]));
    printf("Kart dort asamadan gecip basa donuyor. Cikmak icin bir tusa basin.\n\n");

    backlight_set(true);
    pb_lcd_fill(0x0000);
    pb_lv_init();
    pb_ui_create();
    for (int i = 0; i < 4; i++) { pb_ui_tick(); sleep_ms(5); }
    pb_spec_init();

    /* Günlük ekranı da sınanabilsin: boş liste hiçbir çizim sorununu
     * göstermez. Üç sahte kayıt, en uzun ad dâhil. */
    pb_ui_log_add(pb_class_name[lengthy],   pb_class_latin[lengthy],   0.91f);
    pb_ui_log_add(pb_class_name[second], pb_class_latin[second], 0.74f);
    pb_ui_log_add(pb_class_name[third], pb_class_latin[third], 0.63f);

    const struct { pb_decision_mode_t mode; bool species; float confidence; const char *ne; }
    stage[] = {
        { PB_DECISION_LISTENING, false, 0.00f, "dinliyor (tur yok)"      },
        { PB_DECISION_SOUND,      false, 0.00f, "ses algilandi"           },
        { PB_DECISION_UNSURE, true,  0.42f, "belirsiz, kehribar"      },
        { PB_DECISION_SPECIES,      true,  0.91f, "tur adi, sari, en uzun"  },
    };
    const int count = (int)(sizeof(stage) / sizeof(stage[0]));

    int a = 0;
    uint32_t column = 0;
    absolute_time_t next = make_timeout_time_ms(1);
    drain_stdin();

    for (;;) {
        const int key = getchar_timeout_us(0);
        if (key >= 0) {
            if (key == ' ' || key == 'n' || key == 'N') pb_ui_next();
            else break;
        }

        if (time_reached(next)) {
            pb_result_view_t view;
            memset(&view, 0, sizeof(view));
            view.mode = stage[a].mode;
            view.confidence = stage[a].confidence;
            view.species_name = stage[a].species ? pb_class_name[lengthy] : NULL;
            view.top3_name[0] = pb_class_name[lengthy];
            view.top3_name[1] = pb_class_name[second];
            view.top3_name[2] = pb_class_name[third];
            view.top3_latin[0] = pb_class_latin[lengthy];
            view.top3_latin[1] = pb_class_latin[second];
            view.top3_latin[2] = pb_class_latin[third];
            view.top3_probability[0] = stage[a].confidence;
            view.top3_probability[1] = 0.21f;
            view.top3_probability[2] = 0.07f;
            view.frame_rate = 62;
            view.inference = (uint32_t)a + 1;
            view.merged = 8;
            view.overrun = 0;
            view.band_db = -38.0f;
            pb_ui_update(&view);

            printf("  asama %d/%d: %s  [ekran %d]\n", a + 1, count, stage[a].ne,
                   pb_ui_screen());
            a = (a + 1) % count;
            next = make_timeout_time_ms(2500);
        }

        /* Spektrogram şeridi: gerçek bir ötüşe benzeyen desen — heceler,
         * süpüren bir temel frekans, iki harmonik ve gürültü tabanı.
         *
         * Düz renk OLMAMASI şart (§9o): düz blok satır kaymasını gizler.
         * Ama düz olmayan her desen de yetmiyor — tek bir hareketli tepe
         * gerçek spektrograma benzemediği için "doğru görünüyor mu?"
         * sorusuna cevap vermiyordu. Bu desen hem kaymayı gösteriyor hem de
         * gerçek çıktının nasıl görüneceğini. */
        uint8_t bins[PB_MEL_BANDS];
        const uint32_t phase = column % 48;             /* hece ~0,77 s        */
        const bool silent = (phase >= 34);             /* heceler arası       */
        const int base = 13 + (int)(phase < 17 ? phase : 34 - phase);  /* süpürme */
        for (int b = 0; b < PB_MEL_BANDS; b++) {
            int v = 10 + (int)((column * 7u + (uint32_t)b * 13u) % 9u);  /* taban */
            if (!silent) {
                for (int h = 1; h <= 3; h++) {
                    const int center = base * h;
                    if (center >= PB_MEL_BANDS) break;
                    int d = b - center;
                    if (d < 0) d = -d;
                    if (d <= 2) {
                        const int parlak = 255 - d * 70 - (h - 1) * 60;
                        if (parlak > v) v = parlak;
                    }
                }
            }
            bins[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        if (pb_ui_screen() == PB_SCREEN_LISTEN) {
            pb_spec_push_column(bins, PB_MEL_BANDS);
        }
        column++;

        pb_ui_tick();
        sleep_ms(16);
    }
    printf("cikildi\n");
    printf("  kaydirma: dokunma %lu, basla %lu, kabul %lu, kisa/egik %lu, "
           "panel disi %lu, son dx %ld dy %ld, buton basim %lu\n\n",
           (unsigned long)pb_swipe_touch, (unsigned long)pb_swipe_begin,
           (unsigned long)pb_swipe_accept, (unsigned long)pb_swipe_short,
           (unsigned long)pb_lv_touch_invalidate,
           (long)pb_swipe_last_dx, (long)pb_swipe_last_dy,
           (unsigned long)pb_button_press);
}

/* ── F: KART FRAMEBUFFER DÖKÜMÜ — göz GEREKMEZ, teşhisi ikiye böler ───────
 *
 * `C` ekranında yazılar üst üste binmiş görünüyordu. İki ihtimal vardı ve
 * fotoğraftan ayırt edilemiyordu:
 *   (a) LVGL/yerleşim yanlış çiziyor  -> framebuffer'da da bozuk olur
 *   (b) panele giden yol bozuyor      -> framebuffer TEMİZ, ekran bozuk
 *
 * Bu komut kartı iki kez kurup (a) hâlini seri porta ASCII döküyor. Terminalde
 * yazı düzgün okunuyorsa suçlu (b), okunmuyorsa (a) — ve (a) panele hiç
 * bakmadan düzeltilebilir.
 *
 * İKİ AŞAMA BİLEREK: önce kısa içerik ("dinliyor", tür yok), sonra en uzun
 * tür adı. İkinci dökümde birinci aşamanın kalıntısı varsa sorun "eski yazı
 * silinmiyor"dur; kalıntı yoksa ve satırlar üst üste biniyorsa sorun
 * yerleşimdir (etiketler birbirinin alanına taşıyor).
 */
static void cmd_card_fb_dok(void) {
    int lengthy = 0;
    for (int i = 0; i < PB_CLASS_COUNT; i++) {
        if (strlen(pb_class_name[i]) > strlen(pb_class_name[lengthy])) lengthy = i;
    }

    printf("\n=== KART FRAMEBUFFER DOKUMU (goz GEREKMEZ) ===\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);
    pb_lv_init();
    pb_ui_create();

    for (int stage = 0; stage < 2; stage++) {
        pb_result_view_t view;
        memset(&view, 0, sizeof(view));
        if (stage == 0) {
            view.mode = PB_DECISION_LISTENING;
            printf("\n--- asama 1: dinliyor, tur yok ---\n");
        } else {
            view.mode = PB_DECISION_SPECIES;
            view.species_name = pb_class_name[lengthy];
            view.confidence = 0.91f;
            view.top3_name[0] = pb_class_name[lengthy];
            view.top3_name[1] = pb_class_name[(lengthy + 1) % PB_CLASS_COUNT];
            view.top3_name[2] = pb_class_name[(lengthy + 2) % PB_CLASS_COUNT];
            view.top3_probability[0] = 0.91f;
            view.top3_probability[1] = 0.21f;
            view.top3_probability[2] = 0.07f;
            view.frame_rate = 62;
            view.inference = 6;
            view.merged = 8;
            view.band_db = -38.0f;
            printf("\n--- asama 2: en uzun tur adi (\"%s\") ---\n",
                   pb_class_name[lengthy]);
        }
        pb_ui_update(&view);
        for (int i = 0; i < 6; i++) { pb_ui_tick(); sleep_ms(5); }
        pb_lv_dump_card_fb();
    }
    printf("\n");
}

static void print_help(void) {
    printf("\nKomutlar:\n");
    printf("  i  cihaz ve ses yapilandirmasi\n");
    printf("  n  gurultu tabani olcumu\n");
    printf("  e  EMI taramasi (arka isik etkisi)\n");
    printf("  g  mikrofon kazanci (0-7)\n");
    printf("  r  %d s kayit al ve aktar\n", CAPTURE_SECONDS);
    printf("  d  ekran testi (panel baslatma varyantlari)\n");
    printf("  o  yon testi (dort koseye dort renk)\n");
    printf("  t  dokunmatik teshisi ve koordinat esleme\n");
    printf("  m  mel + kapi hatti (M3, canli mikrofon)\n");
    printf("  a  DEMO: LVGL kart + canli mel spektrogrami + kapi\n");
    printf("  b  arka isik teshisi\n");
    printf("  v  QSPI veri yolu teshisi\n");
    printf("  w  QSPI zamanlama teshisi (WaitIdle olcumu, goz GEREKMEZ)\n");
    printf("  y  melez yol testi: pencere ve piksel ayri yollardan (goz gerekir)\n");
    printf("  z  satir adresleme testi: RASET calisiyor mu (goz gerekir)\n");
    printf("  j  imlec konumlandirma testi: dar pencereyle ucuz atlama (goz gerekir)\n");
    printf("  L  yazi teshisi: LVGL cizimini ASCII dok (goz GEREKMEZ)\n");
    printf("  S  dar pencere kayma testi: cizgiler duz mu (goz gerekir)\n");
    printf("  s  canli spektrogram\n");
    printf("  x  TUR AGI: cihaz ici dogrulama + arena + cikarim suresi\n");
    printf("  X  IKILI AGI (Asama-1): cihaz ici dogrulama + arena + cikarim suresi\n");
    printf("  k  gercek zamanli tanima (core 1, seri porta yazar)\n");
    printf("  K  aynisi ama kapi yoksayilir — olcum kipi\n");
    printf("  c  ARAYUZ: dinleme + gunluk ekrani, canli tanima (goz gerekir)\n");
    printf("  C  arayuz gosterim testi, mikrofonsuz (goz gerekir)\n");
    printf("     c/C icinde: KAYDIR ya da bosluk/'n' = ekran degistir\n");
    printf("  F  kart framebuffer dokumu: ASCII, goz GEREKMEZ\n");
    printf("  ?  bu yardim\n\n");
}

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\n========================================\n");
    printf(" PokeBird — M1: mikrofon bring-up\n");
    printf("========================================\n");

    power_latch_init();
    backlight_init();
    pb_i2c_init();

    /* SIRA ÖNEMLİ: ES8311'in dahili PLL'i MCLK olmadan register yazimlarina
     * duzgun tepki vermiyor. Once MCLK, sonra codec yapilandirmasi. */
    if (!pb_audio_mclk_start(&s_audio_cfg)) {
        printf("[!] MCLK baslatilamadi.\n");
    }
    sleep_ms(10);

    if (!pb_i2c_probe(ES8311_I2C_ADDR)) {
        printf("[!] ES8311 I2C adresi 0x%02x yanit vermiyor.\n", ES8311_I2C_ADDR);
        printf("    Hat: SDA=GPIO%d SCL=GPIO%d\n", PB_PIN_I2C_SDA, PB_PIN_I2C_SCL);
    }

    es8311_init(s_audio_cfg);
    es8311_sample_frequency_config((int)s_audio_cfg.mclk_freq, (int)s_audio_cfg.sample_freq);
    es8311_microphone_config();
    es8311_microphone_gain_set((es8311_mic_gain_t)s_mic_gain);

    if (!pb_audio_i2s_init(&s_audio_cfg)) {
        printf("[!] I2S yakalama yolu kurulamadi.\n");
    }

    /* â”€â”€ Ekran â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * QSPI pio0'da, ses pio1'de — state machine çakışması yok.
     *
     * SIRA ZORUNLU (Waveshare örneğindeki sıra):
     *   1. QSPI_GPIO_Init   — CS/RST/PWR_EN pin yönleri
     *   2. QSPI_PIO_Init    — PIO programını yükle (SM'leri KAPALI bırakır)
     *   3. QSPI_4Wrie_Mode  — 4-bit SM'i ETKİNLEŞTİR ve qspi.sm'i ayarla
     *   4. pb_display_dma_init — DREQ doğru SM'e bağlansın diye 3'ten sonra
     *   5. LCD_3IN49_Init   — panel reset + register dizisi
     *
     * 3. adım atlanırsa hiçbir SM çalışmadığı için PIO TX FIFO hiç
     * boşalmıyor ve DMA sonsuza kadar bekliyor — ekran tamamen siyah kalıyor,
     * üstelik ilk çizim çağrısında kilitleniyor. */
    /* PANEL HAZIR OLMA PENCERESİ — silmeyin, ölçülerek kondu.
     *
     * AXS15231B açılıştan sonra bir süre başlatma dizisini kabul etmiyor.
     * Erken başlatılırsa panel kendi başlatılmamış GRAM'ını göstermeye devam
     * ediyor (karıncalanma) ve bu durum KALICI: GPIO34 dışarıdan yüksek
     * tutulduğu için panele donanım reset'i atamıyoruz (§5.11), yani yeniden
     * deneme şansı yok.
     *
     * NEDEN BÖYLE ÖĞRENDİK: `s_capture` temizliği bss'i 218.988'den 127.084'e
     * indirdi. bss'i sıfırlamak (crt0, main'den önce) o kadar kısa sürmeye
     * başladı ki firmware panel başlatmaya ~1 ms daha erken varır oldu ve
     * ekran bozuldu. Yani eski hâl bu pencereyi KIL PAYI geçiyormuş; hata
     * kodda zaten vardı, temizlik yalnızca payı bitirdi. Kartta ölçüldü:
     * 20 ms'lik gecikme yetiyor (500 ms de çalışıyor, 0 çalışmıyor).
     *
     * NEDEN `sleep_ms` DEĞİL: sabit bir uyku yalnızca o anki paya sabit bir
     * miktar ekler; kendinden ÖNCEKİ kod hızlanırsa aynı tuzak yeniden kurulur
     * (bizi buraya tam olarak bu düşürdü). Mutlak alt sınır kısıtın kendisini
     * ifade ediyor ve önceki kodun süresinden bağımsız. USB beklemesi zaten
     * uzun sürdüyse bu satır hiç beklemez, yani normalde bedeli sıfır. */
    while (to_ms_since_boot(get_absolute_time()) < 250) sleep_ms(5);

    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    pb_display_dma_init();
    LCD_3IN49_Init();
    pb_lcd_fill(0x0000);
    printf("Ekran hazir (%dx%d panel).\n", PB_PANEL_W, PB_PANEL_H);

    cmd_info();
    print_help();

    /* Cihaz PC'ye bagli olmadan, elde tasinirken kendiliginden tanima
     * ekranina girsin diye. Once burada baslatiyoruz; ekrandan (bosluk/n/r
     * disinda) bir tusa basilirsa asagidaki komut dongusune duser, PC
     * baglanmissa teshis komutlari yine erisilebilir kalir. */
    cmd_result_ekrani();

    while (true) {
        printf("> ");
        /* getchar_timeout_us(0) HEMEN doner (0 = beklemeden zaman asimi),
         * bu da istemi bos yere dondurur. Bloklayan okuma icin getchar(). */
        int c = getchar();
        if (c < 0) continue;
        switch (c) {
            case 'i': cmd_info();      break;
            case 'n': cmd_noise();     break;
            case 'e': cmd_emi_sweep(); break;
            case 'g': cmd_gain();      break;
            case 'r': cmd_record();    break;
            case 'l': cmd_level_meter(); break;
            case 's': cmd_spectrogram(); break;
            case 'd': cmd_display_test(); break;
            case 'b': cmd_backlight_probe(); break;
            case 'v': cmd_datapath_probe(); break;
            case 'w': cmd_qspi_timing(); break;
            case 'y': cmd_hybrid_path(); break;
            case 'z': cmd_row_addr(); break;
            case 'j': cmd_cursor_seek(); break;
            case 'L': cmd_text_dump(); break;
            case 'S': cmd_stripe_test(); break;
            case 'o': cmd_orientation(); break;
            case 't': cmd_touch_probe(); break;
            case 'u': cmd_ui_demo(); break;
            case 'm': cmd_mel_pipeline(); break;
            case 'a': cmd_full_demo(); break;
            case 'x': cmd_ai_verify(); break;
            case 'X': cmd_binary_ai_verify(); break;
            case 'k': cmd_recognize(false); break;
            case 'K': cmd_recognize(true);  break;
            case 'c': cmd_result_ekrani();   break;
            case 'C': cmd_result_karti_demo(); break;
            case 'F': cmd_card_fb_dok();    break;
            case '?': print_help();    break;
            case '\r': case '\n': printf("\r"); break;
            default:  printf("bilinmeyen komut ('?' yardim)\n"); break;
        }
    }
}
