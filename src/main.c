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

#define BL_PWM_WRAP     2048
#define ES8311_I2C_ADDR 0x18

/* 2 saniyelik yakalama tamponu. 24 kHz x 16 bit = 96 KB; 520 KB SRAM'de rahat.
 * Gürültü tabanı ve SNR ölçümü için 2 saniye fazlasıyla yeterli. */
#define CAPTURE_SECONDS 2
#define CAPTURE_SAMPLES (PB_SAMPLE_RATE * CAPTURE_SECONDS)
static int16_t s_capture[CAPTURE_SAMPLES];

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

static audio_stats_t compute_stats(const int16_t *x, uint32_t n) {
    audio_stats_t st = { 0 };
    if (n == 0) return st;

    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) sum += (double)x[i];
    st.dc_offset = sum / (double)n;

    double sumsq = 0.0;
    int32_t peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        double v = (double)x[i] - st.dc_offset;   /* DC'yi çıkararak gerçek gürültü */
        sumsq += v * v;
        int32_t a = x[i] < 0 ? -(int32_t)x[i] : (int32_t)x[i];
        if (a > peak) peak = a;
    }
    st.rms  = sqrt(sumsq / (double)n);
    st.peak = peak;
    st.dbfs = (st.rms > 0.0) ? 20.0 * log10(st.rms / 32768.0) : -999.0;
    return st;
}

/**
 * Gürültü tabanını yüzdelik ile ölç.
 *
 * Düz RMS, ölçüm boyunca olan tek bir kapı sesi ya da öksürükle yukarı
 * çekiliyor — oda hiçbir zaman tam sessiz değil. Sinyali kısa pencerelere
 * bölüp pencere RMS'lerinin 10. yüzdeliğini almak, geçici seslere karşı
 * dayanıklı ve "en sessiz an" için çok daha dürüst bir sayı veriyor.
 */
#define NOISE_WINDOWS 64

static double noise_floor_dbfs(const int16_t *x, uint32_t n, double *out_rms) {
    uint32_t w = n / NOISE_WINDOWS;
    if (w < 16) w = n, n = w;   /* çok kısa sinyal: tek pencere */

    double rms_list[NOISE_WINDOWS];
    uint32_t count = 0;
    for (uint32_t k = 0; k + w <= n && count < NOISE_WINDOWS; k += w, count++) {
        double sum = 0.0;
        for (uint32_t i = 0; i < w; i++) sum += (double)x[k + i];
        double mean = sum / (double)w;
        double sq = 0.0;
        for (uint32_t i = 0; i < w; i++) {
            double v = (double)x[k + i] - mean;
            sq += v * v;
        }
        rms_list[count] = sqrt(sq / (double)w);
    }
    if (count == 0) { if (out_rms) *out_rms = 0.0; return -999.0; }

    /* küçükten büyüğe sırala (n küçük, basit ekleme sıralaması yeterli) */
    for (uint32_t i = 1; i < count; i++) {
        double v = rms_list[i];
        uint32_t j = i;
        while (j > 0 && rms_list[j - 1] > v) { rms_list[j] = rms_list[j - 1]; j--; }
        rms_list[j] = v;
    }
    double p10 = rms_list[count / 10];
    if (out_rms) *out_rms = p10;
    return (p10 > 0.0) ? 20.0 * log10(p10 / 32768.0) : -999.0;
}

static void print_stats(const char *label, const audio_stats_t *st,
                        const pb_capture_result_t *cap) {
    printf("  %-22s RMS %8.1f  %7.1f dBFS  tepe %6ld  DC %8.1f",
           label, st->rms, st->dbfs, (long)st->peak, st->dc_offset);
    if (cap->fifo_overrun) printf("   [!] ORNEK DUSTU");
    if (cap->timed_out)    printf("   [!] SAAT YOK");
    printf("\n");
}

/* Ölçüm alırken arka ışığı verilen duruma getirip bekle (güç hattı otursun) */
static audio_stats_t measure_with_backlight(bool enable,
                                            pb_capture_result_t *cap_out) {
    backlight_set(enable);
    sleep_ms(250);
    pb_capture_result_t cap = pb_audio_capture(s_capture, PB_SAMPLE_RATE / 2); /* 0.5 s */
    if (cap_out) *cap_out = cap;
    return compute_stats(s_capture, cap.samples);
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
    printf("\nGurultu tabani olcumu (%d s). Ortami sessiz tutun...\n", CAPTURE_SECONDS);
    backlight_set(false);
    sleep_ms(300);

    pb_capture_result_t cap = pb_audio_capture(s_capture, CAPTURE_SAMPLES);
    audio_stats_t st = compute_stats(s_capture, cap.samples);

    printf("  Yakalanan    %lu / %lu ornek\n",
           (unsigned long)cap.samples, (unsigned long)CAPTURE_SAMPLES);
    print_stats("tum pencere (RMS)", &st, &cap);

    double floor_rms;
    double floor_db = noise_floor_dbfs(s_capture, cap.samples, &floor_rms);
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
    const uint32_t win = PB_SAMPLE_RATE / 10;   /* 100 ms */

    while (getchar_timeout_us(0) < 0) {
        pb_capture_result_t cap = pb_audio_capture(s_capture, win);
        if (cap.samples == 0) break;
        audio_stats_t st = compute_stats(s_capture, cap.samples);

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
static void cmd_record(void) {
    printf("\nKayit basliyor (%d s)...\n", CAPTURE_SECONDS);
    pb_capture_result_t cap = pb_audio_capture(s_capture, CAPTURE_SAMPLES);
    audio_stats_t st = compute_stats(s_capture, cap.samples);
    print_stats("kayit", &st, &cap);

    if (cap.samples == 0) {
        printf("Kayit alinamadi.\n\n");
        return;
    }

    printf("#WAV-BEGIN rate=%d channels=1 bits=16 samples=%lu\n",
           PB_SAMPLE_RATE, (unsigned long)cap.samples);
    /* Satir basina 32 ornek, isaretli ondalik. Basit ve hata ayiklamasi kolay;
     * 2 saniye icin ~250 KB metin, USB CDC'de birkac saniye surer. */
    for (uint32_t i = 0; i < cap.samples; i++) {
        printf("%d%c", s_capture[i], ((i % 32) == 31) ? '\n' : ' ');
    }
    if (cap.samples % 32) printf("\n");
    printf("#WAV-END\n\n");
}


/* M2 doğrulaması: mikrofondan gelen ses ekranda akıyor mu.
 * Ekran QSPI'ye tek sütun yazarak güncelleniyor (bkz. ui/spectrogram.c). */
static void cmd_spectrogram(void) {
    printf("\nCanli spektrogram. Cikmak icin bir tusa basin.\n");
    backlight_set(true);
    pb_spec_init();

    uint8_t bins[PB_SPEC_HEIGHT];
    while (getchar_timeout_us(0) < 0) {
        pb_capture_result_t cap = pb_audio_capture(s_capture, PB_FFT_SIZE);
        if (cap.samples < PB_FFT_SIZE) break;
        /* -75 dBFS taban: M1'de olculen ~-36 dBFS oda gurultusunun altinda,
         * boylece sessizlik siyah kaliyor ama zayif sesler hala goruluyor. */
        pb_fft_spectrum(s_capture, bins, PB_SPEC_HEIGHT, -75.0f);
        pb_spec_push_column(bins, PB_SPEC_HEIGHT);
    }
    printf("cikildi\n\n");
}

/* Bit-bang yolu asagida tanimli; ekran testi PIO yolunun yanina onu da
 * koyabilsin diye burada bildiriliyor. */
static void bb_pins_setup(void);
static void bb_panel_init(void);
static void bb_ekrani_boya(uint16_t renk);

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

    const struct { int varyant; const char *ad; } denemeler[] = {
        { LCD_3IN49_INIT_FULL,      "satici tablosu + SLPOUT/MADCTL/COLMOD(RGB565)/DISPON" },
        { LCD_3IN49_INIT_MINIMAL,   "sadece DCS kuyrugu (rsvpnano ile ayni)" },
        { LCD_3IN49_INIT_NO_COLMOD, "satici tablosu + SLPOUT/DISPON, COLMOD YOK (kontrol)" },
        { PB_INIT_BITBANG,          "BIT-BANG: PIO/DMA/satici surucusu tamamen devre disi" },
    };

    const struct { const char *ad; uint16_t renk; } renkler[] = {
        { "kirmizi", 0xF800 },
        { "yesil",   0x07E0 },
        { "mavi",    0x001F },
        { "beyaz",   0xFFFF },
    };

    for (size_t v = 0; v < sizeof(denemeler) / sizeof(denemeler[0]); v++) {
        printf("  [%u] %s\n", (unsigned)(v + 1), denemeler[v].ad);
        const bool bitbang = (denemeler[v].varyant == PB_INIT_BITBANG);
        if (bitbang) {
            pio_sm_set_enabled(qspi.pio, qspi.sm, false);
            bb_pins_setup();
            bb_panel_init();
        } else {
            LCD_3IN49_InitVariant(denemeler[v].varyant);
        }
        backlight_set(true);
        drain_stdin();

        for (size_t r = 0; r < sizeof(renkler) / sizeof(renkler[0]); r++) {
            printf("        %s\n", renkler[r].ad);
            if (bitbang) bb_ekrani_boya(renkler[r].renk);
            else         pb_lcd_fill(renkler[r].renk);

            /* Renk basildiktan sonra 1.5 s bakma suresi */
            for (int t = 0; t < 15; t++) {
                if (getchar_timeout_us(0) >= 0) {
                    printf("\n  >>> DUZGUN CALISAN VARYANT: [%u] %s <<<\n",
                           (unsigned)(v + 1), denemeler[v].ad);
                    printf("  (o anda ekranda: %s)\n\n", renkler[r].ad);
                    if (bitbang) {
                        printf("  Bit-bang yolu calisiyor, PIO yolu calismiyor:\n");
                        printf("  hata PIO/DMA tarafinda. Yon karesi atlandi.\n\n");
                        return;
                    }
                    /* Yon kontrolu: panelin (0,0) kosesine 20x20 beyaz kare.
                     * Cihazi yatay tuttugunuzda karenin nerede goruldugu,
                     * ui/spectrogram.c'deki yon cevirimini dogrular. */
                    pb_lcd_fill(0x0000);
                    static uint16_t kare[20 * 20];
                    for (int i = 0; i < 20 * 20; i++) kare[i] = 0xFFFF;
                    pb_lcd_blit(0, 0, 20, 20, kare);
                    printf("  panel (0,0) konumuna 20x20 beyaz kare cizildi —\n");
                    printf("  cihazi USB soketi ASAGI bakacak sekilde tutun ve\n");
                    printf("  karenin hangi kosede oldugunu soyleyin.\n\n");
                    return;
                }
                sleep_ms(100);
            }
        }
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
#define BB_GECIKME() sleep_us(1)      /* ~500 kHz — panel icin fazlasiyla yavas */

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
        BB_GECIKME();
        gpio_put(PIN_SCLK, 1);          /* panel yukselen kenarda ornekler */
        BB_GECIKME();
    }
    gpio_put(PIN_SCLK, 0);
}

static void bb_cmd(uint8_t cmd, const uint8_t *veri, size_t n) {
    gpio_put(PIN_CS, 0);
    BB_GECIKME();
    bb_byte(0x02); bb_byte(0x00); bb_byte(cmd); bb_byte(0x00);
    for (size_t i = 0; i < n; i++) bb_byte(veri[i]);
    BB_GECIKME();
    gpio_put(PIN_CS, 1);
    BB_GECIKME();
}

static void bb_read(uint8_t cmd, uint8_t *cikti, size_t n) {
    gpio_put(PIN_CS, 0);
    BB_GECIKME();
    bb_byte(0x03); bb_byte(0x00); bb_byte(cmd); bb_byte(0x00);

    gpio_set_dir(PIN_DIO0, GPIO_IN);
    for (size_t i = 0; i < n; i++) {
        uint8_t v = 0;
        for (int b = 7; b >= 0; b--) {
            gpio_put(PIN_SCLK, 0); BB_GECIKME();
            gpio_put(PIN_SCLK, 1); BB_GECIKME();
            v |= (uint8_t)(gpio_get(PIN_DIO0) << b);
        }
        cikti[i] = v;
    }
    gpio_put(PIN_SCLK, 0);
    gpio_set_dir(PIN_DIO0, GPIO_OUT);
    BB_GECIKME();
    gpio_put(PIN_CS, 1);
}

/** Dort hat uzerinden bir bayt (gercek QSPI veri fazi). */
static void bb_byte_quad(uint8_t v) {
    for (int yari = 0; yari < 2; yari++) {
        uint8_t nib = yari ? (uint8_t)(v & 0x0F) : (uint8_t)(v >> 4);
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
static void bb_ekrani_boya(uint16_t renk) {
    for (uint p = PIN_DIO1; p <= PIN_DIO3; p++) gpio_set_dir(p, GPIO_OUT);

    uint8_t caset[] = { 0x00, 0x00, (PB_PANEL_W - 1) >> 8, (PB_PANEL_W - 1) & 0xFF };
    uint8_t raset[] = { 0x00, 0x00, (PB_PANEL_H - 1) >> 8, (PB_PANEL_H - 1) & 0xFF };
    bb_cmd(0x2A, caset, 4);
    bb_cmd(0x2B, raset, 4);

    /* Piksel yazimi: komut+adres tek hatta (0x32 / 0x002C00), veri dort hatta */
    gpio_put(PIN_CS, 0);
    bb_byte(0x32); bb_byte(0x00); bb_byte(0x2C); bb_byte(0x00);
    uint8_t yuksek = (uint8_t)(renk >> 8), dusuk = (uint8_t)(renk & 0xFF);
    for (uint32_t i = 0; i < (uint32_t)PB_PANEL_W * PB_PANEL_H; i++) {
        bb_byte_quad(yuksek);
        bb_byte_quad(dusuk);
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
static uint32_t te_gecis_say(void) {
    uint32_t s = 0;
    int onceki = gpio_get(PB_PIN_LCD_TE);
    absolute_time_t bitis = make_timeout_time_ms(200);
    while (!time_reached(bitis)) {
        int simdi = gpio_get(PB_PIN_LCD_TE);
        if (simdi != onceki) { s++; onceki = simdi; }
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
    static int16_t kare[PB_FFT_SIZE];
    const uint32_t kalan = PB_FFT_SIZE - PB_MEL_HOP;

    uint32_t toplam = 0, acik = 0, pencere_sayisi = 0;
    uint32_t kayip_kare = 0, saniye_kare = 0;
    absolute_time_t sonraki_rapor = make_timeout_time_ms(1000);

    /* Bayat veriyle değil şimdiyle başla; döngü İÇİNDE flush YOK — hattın
     * kesintisiz akması sürekli yakalamanın bütün amacı. */
    pb_audio_stream_flush();

    while (getchar_timeout_us(0) < 0) {
        memmove(kare, kare + PB_MEL_HOP, kalan * sizeof(int16_t));
        pb_capture_result_t cap = pb_audio_stream_read(kare + kalan, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) { printf("  yakalama eksik, cikiliyor\n"); break; }
        if (cap.fifo_overrun) kayip_kare++;   /* halka sarıldı: süreklilik koptu */

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(kare, power);
        pb_gate_result_t g = pb_gate_update(power);
        if (g.active) acik++;

        pb_mel_push(kare);
        toplam++;
        saniye_kare++;

        if (pb_mel_frame_count() >= PB_MEL_FRAMES &&
            pb_mel_frame_count() % PB_MEL_FRAMES == 0) {
            static int8_t pencere[PB_MEL_BANDS * PB_MEL_FRAMES];
            if (pb_mel_window(pencere)) pencere_sayisi++;
        }

        if (time_reached(sonraki_rapor)) {
            /* kare/s kabul ölçütü: 62.5 (hop 384 @ 24 kHz). Eskiden 57'ydi —
             * bloklayan yakalama kare kaçırıyordu (lastsession.md §9c). */
            printf("  kare %5lu (%lu/s)  kapi %%%3lu  bant %6.1f dB  "
                   "taban %6.1f dB  aki %.3f  pencere %lu  kayip %lu\n",
                   (unsigned long)toplam, (unsigned long)saniye_kare,
                   (unsigned long)(toplam ? acik * 100 / toplam : 0),
                   (double)g.band_db, (double)g.floor_db, (double)g.flux,
                   (unsigned long)pencere_sayisi, (unsigned long)kayip_kare);
            saniye_kare = 0;
            sonraki_rapor = make_timeout_time_ms(1000);
        }
    }

    printf("\n  toplam kare %lu, kapi acik %lu (%%%lu), tam pencere %lu, "
           "kayip %lu\n\n",
           (unsigned long)toplam, (unsigned long)acik,
           (unsigned long)(toplam ? acik * 100 / toplam : 0),
           (unsigned long)pencere_sayisi, (unsigned long)kayip_kare);
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

    bool dokunmatik = pb_lv_init();
    printf("  dokunmatik: %s\n", dokunmatik ? "hazir" : "yok (demoya engel degil)");

    /* ── Sol kart ── */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *baslik = lv_label_create(scr);
    lv_label_set_text(baslik, "PokeBird");
    lv_obj_set_style_text_color(baslik, lv_color_hex(0xF0C000), LV_PART_MAIN);
    lv_obj_set_style_text_font(baslik, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(baslik, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *durum = lv_label_create(scr);
    lv_label_set_text(durum, "dinliyor...");
    lv_obj_set_style_text_color(durum, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(durum, LV_ALIGN_TOP_LEFT, 12, 44);

    lv_obj_t *sayilar = lv_label_create(scr);
    lv_label_set_text(sayilar, "");
    lv_obj_set_style_text_color(sayilar, lv_color_hex(0x8090A0), LV_PART_MAIN);
    lv_obj_align(sayilar, LV_ALIGN_TOP_LEFT, 12, 76);

    /* Kartı çiz, SONRA sağ şeridi spektrograma ver: LVGL'in ilk çizimi tam
     * ekran, spektrogram alanını da boyuyor — sıra ters olursa şerit silinir. */
    for (int i = 0; i < 4; i++) { pb_lv_tick(); sleep_ms(5); }
    pb_spec_init();

    /* ── Ses hattı ── */
    pb_mel_init();
    pb_mel_reset();
    pb_gate_reset();

    static int16_t kare[PB_FFT_SIZE];
    memset(kare, 0, sizeof(kare));
    const uint32_t kalan = PB_FFT_SIZE - PB_MEL_HOP;

    uint32_t toplam = 0, acik = 0, pencere_sayisi = 0, kayip = 0;
    uint32_t saniye_kare = 0, son_hiz = 0;
    /* Kapı kapanınca yazıyı hemen soldurmak yerine kısa bir süre tut:
     * 62 kare/s'de tek karelik açılmalar gözle görülmez. */
    uint32_t kapi_tut = 0;
    bool kapi_gorunur = false;
    absolute_time_t sonraki_rapor = make_timeout_time_ms(1000);
    absolute_time_t sonraki_kart  = make_timeout_time_ms(250);

    pb_audio_stream_flush();
    drain_stdin();

    while (getchar_timeout_us(0) < 0) {
        memmove(kare, kare + PB_MEL_HOP, kalan * sizeof(int16_t));
        pb_capture_result_t cap = pb_audio_stream_read(kare + kalan, PB_MEL_HOP, 1000);
        if (cap.samples < PB_MEL_HOP) { printf("  yakalama eksik, cikiliyor\n"); break; }
        if (cap.fifo_overrun) kayip++;

        float power[PB_FFT_POWER_BINS];
        pb_fft_power(kare, power);
        pb_gate_result_t g = pb_gate_update(power);
        if (g.active) { acik++; kapi_tut = 31; }      /* ~0.5 s görünür kal */
        else if (kapi_tut) kapi_tut--;

        pb_mel_push(kare);
        toplam++;
        saniye_kare++;

        if (pb_mel_frame_count() >= PB_MEL_FRAMES &&
            pb_mel_frame_count() % PB_MEL_FRAMES == 0) {
            static int8_t pencere[PB_MEL_BANDS * PB_MEL_FRAMES];
            if (pb_mel_window(pencere)) pencere_sayisi++;
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

        if (time_reached(sonraki_rapor)) {
            son_hiz = saniye_kare;
            saniye_kare = 0;
            sonraki_rapor = make_timeout_time_ms(1000);
        }

        /* Kartı 4 Hz güncelle: her karede güncellemek LVGL'e boş yere
         * çizim çıkarır ve hop bütçesini yer. */
        if (time_reached(sonraki_kart)) {
            bool goster = g.active || kapi_tut > 0;
            if (goster != kapi_gorunur) {
                kapi_gorunur = goster;
                lv_label_set_text(durum, goster ? "SES ALGILANDI" : "dinliyor...");
                lv_obj_set_style_text_color(durum,
                    lv_color_hex(goster ? 0x40E060 : 0xB0B8C0), LV_PART_MAIN);
            }
            lv_label_set_text_fmt(sayilar,
                "%lu kare/s\nkapi %%%lu\nbant %d dB\npencere %lu\nkayip %lu",
                (unsigned long)son_hiz,
                (unsigned long)(toplam ? acik * 100 / toplam : 0),
                (int)g.band_db,
                (unsigned long)pencere_sayisi,
                (unsigned long)kayip);
            sonraki_kart = make_timeout_time_ms(250);
        }
        pb_lv_tick();
    }

    printf("\n  toplam kare %lu, son hiz %lu kare/s, kapi acik %%%lu,\n"
           "  tam pencere %lu, kayip %lu\n\n",
           (unsigned long)toplam, (unsigned long)son_hiz,
           (unsigned long)(toplam ? acik * 100 / toplam : 0),
           (unsigned long)pencere_sayisi, (unsigned long)kayip);
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

    bool dokunmatik = pb_lv_init();
    printf("  dokunmatik: %s\n", dokunmatik ? "hazir" : "YOK (sadece ekran)");

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *baslik = lv_label_create(scr);
    lv_label_set_text(baslik, "PokeBird");
    lv_obj_set_style_text_color(baslik, lv_color_hex(0xF0C000), LV_PART_MAIN);
    lv_obj_set_style_text_font(baslik, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(baslik, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *bilgi = lv_label_create(scr);
    lv_label_set_text(bilgi, "ekrana dokunun");
    lv_obj_set_style_text_color(bilgi, lv_color_hex(0xB0B8C0), LV_PART_MAIN);
    lv_obj_align(bilgi, LV_ALIGN_TOP_LEFT, 12, 44);

    /* Sağ kenara bir çubuk: kısmi render'ın ekranın uzak ucunda da doğru
     * yere düştüğünü gösterir (yön hatası en çok orada belli olur). */
    lv_obj_t *cubuk = lv_bar_create(scr);
    lv_obj_set_size(cubuk, 200, 16);
    lv_obj_align(cubuk, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_bar_set_range(cubuk, 0, 100);

    int deger = 0;
    drain_stdin();
    while (getchar_timeout_us(0) < 0) {
        pb_lv_tick();

        pb_touch_state_t st = pb_touch_read();
        if (st.ok && st.fingers > 0) {
            lv_label_set_text_fmt(bilgi, "dokunus: x=%u  y=%u",
                                  st.p.raw_x, st.p.raw_y);
        }

        deger = (deger + 1) % 101;
        lv_bar_set_value(cubuk, deger, LV_ANIM_OFF);
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
    enum { KARE = 40 };
    static uint16_t blok[KARE * KARE];

    const struct { uint32_t x, y; uint16_t renk; const char *ad; } kose[] = {
        { 0,               0,               0xF800, "KIRMIZI = panel (0,0)"         },
        { PB_PANEL_W-KARE, 0,               0x07E0, "YESIL   = panel (X sonu, 0)"   },
        { 0,               PB_PANEL_H-KARE, 0x001F, "MAVI    = panel (0, Y sonu)"   },
        { PB_PANEL_W-KARE, PB_PANEL_H-KARE, 0xFFE0, "SARI    = panel (X sonu, Y sonu)" },
    };

    printf("\nYon testi — ekranda dort renkli kare var.\n");
    backlight_set(true);
    pb_lcd_fill(0x0000);

    for (size_t i = 0; i < sizeof(kose) / sizeof(kose[0]); i++) {
        for (int p = 0; p < KARE * KARE; p++) blok[p] = kose[i].renk;
        pb_lcd_blit(kose[i].x, kose[i].y, KARE, KARE, blok);
        printf("  %s\n", kose[i].ad);
    }

    printf("\nCihazi USB soketi ASAGI bakacak sekilde tutun ve her rengin\n");
    printf("hangi kosede oldugunu soyleyin (sol ust / sag ust / sol alt / sag alt).\n\n");
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
        int tp_sayi = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            uint8_t d;
            if (i2c_read_timeout_us(PB_TP_I2C_INST, a, &d, 1, false, 2000) >= 0) {
                printf(" %02x", a);
                tp_sayi++;
            }
        }
        printf("   (%d adet)\n", tp_sayi);

        printf("  i2c1 (codec,      GPIO%d/%d):", PB_PIN_I2C_SDA, PB_PIN_I2C_SCL);
        int cd_sayi = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (pb_i2c_probe(a)) { printf(" %02x", a); cd_sayi++; }
        }
        printf("   (%d adet)\n", cd_sayi);

        if (tp_sayi > 8) {
            printf("  -> i2c0'da COK FAZLA adres yanit veriyor: hat bozuk,\n");
            printf("     okunan 0xdb gurultu. Once pin/kablolama.\n\n");
        } else if (tp_sayi == 0) {
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

    printf("Cihazi USB soketi SAGDA olacak sekilde YATAY tutun.\n");
    printf("Kenarlarda ve koselerde gezdirin; her degisiklik yaziliyor.\n");
    printf("INT sutunu dokununca 0'a dusuyorsa cip dokunusu goruyor demektir.\n");
    printf("Cikmak icin bir tusa basin.\n\n");
    printf("  INT  parmak   ham x   ham y   ilk 8 bayt\n");

    /* Adim adim "su koseye dokun" yerine CANLI AKIS: cihaz ne goruyorsa onu
     * yaziyor. Onceki surum kose kose ilerliyordu ve bazi koseleri atliyordu;
     * hatanin dokunmatikte mi kendi durum makinemde mi oldugu ayirt
     * edilemiyordu. Ham baytlari da basiyoruz — parmak sayisinin gercekten
     * bayt 1'de olup olmadigi ancak boyle gorulur. */
    uint16_t onceki_x = 0xFFFF, onceki_y = 0xFFFF;
    uint8_t  onceki_f = 0xFF;
    int      onceki_int = -1;
    uint32_t hic_yanit_yok = 0;

    while (getchar_timeout_us(0) < 0) {
        int intp = gpio_get(PB_PIN_TP_INT);
        pb_touch_state_t st = pb_touch_read();
        if (!st.ok) {
            if (++hic_yanit_yok % 100 == 1) printf("  (I2C yanit vermiyor)\n");
            sleep_ms(20);
            continue;
        }

        if (intp != onceki_int || st.fingers != onceki_f ||
            st.p.raw_x != onceki_x || st.p.raw_y != onceki_y) {
            uint8_t ham[32];
            pb_touch_last_raw(ham);
            printf("  %3d   %4u    %5u   %5u   ",
                   intp, st.fingers, st.p.raw_x, st.p.raw_y);
            for (int i = 0; i < 8; i++) printf("%02x ", ham[i]);
            printf("\n");
            onceki_int = intp;
            onceki_f = st.fingers;
            onceki_x = st.p.raw_x;
            onceki_y = st.p.raw_y;
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
        int hedef = dma_claim_unused_channel(true);
        int ch    = dma_claim_unused_channel(true);
        volatile uint32_t *reg = &dma_hw->ch[hedef].read_addr;
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
        uint32_t cpu_sonuc = *reg;

        *reg = 0;
        dma_channel_unclaim(ch);
        dma_channel_unclaim(hedef);

        printf("1) Dar (8 bit) IO yazimi — 0xA5:\n");
        printf("   DMA ile: 0x%08lx    CPU ile: 0x%08lx\n",
               (unsigned long)dma_sonuc, (unsigned long)cpu_sonuc);
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

        uint32_t gecis_sclk = 0, gecis_d0 = 0;
        int onceki_s = gpio_get(PIN_SCLK), onceki_d = gpio_get(PIN_DIO0);
        absolute_time_t bitis = make_timeout_time_ms(60);
        while (!time_reached(bitis)) {
            if (!pio_sm_is_tx_fifo_full(qspi.pio, qspi.sm)) {
                pio_sm_put(qspi.pio, qspi.sm, 0x0Fu << 24);  /* nibble 0 sonra F */
            }
            int s = gpio_get(PIN_SCLK);
            int d = gpio_get(PIN_DIO0);
            if (s != onceki_s) { gecis_sclk++; onceki_s = s; }
            if (d != onceki_d) { gecis_d0++;  onceki_d = d; }
        }
        printf("   SCLK(GPIO%d) gecis: %lu   D0(GPIO%d) gecis: %lu\n",
               PIN_SCLK, (unsigned long)gecis_sclk,
               PIN_DIO0, (unsigned long)gecis_d0);
        printf("   -> %s\n\n", (gecis_sclk > 0 && gecis_d0 > 0)
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
        const struct { uint pin; const char *ad; } pinler[] = {
            { PIN_SCLK, "SCLK" }, { PIN_DIO0, "D0" }, { PIN_DIO1, "D1" },
            { PIN_DIO2, "D2" },   { PIN_DIO3, "D3" }, { PIN_CS,   "CS" },
            { PIN_RST,  "RST" },
        };
        for (size_t i = 0; i < sizeof(pinler) / sizeof(pinler[0]); i++) {
            gpio_set_function(pinler[i].pin, GPIO_FUNC_SIO);
            gpio_set_dir(pinler[i].pin, GPIO_OUT);
            gpio_put(pinler[i].pin, 1); sleep_us(100);
            int yuksek = gpio_get(pinler[i].pin);
            gpio_put(pinler[i].pin, 0); sleep_us(100);
            int dusuk = gpio_get(pinler[i].pin);
            printf("   %-4s GPIO%-2d  1->%d  0->%d  %s\n",
                   pinler[i].ad, pinler[i].pin, yuksek, dusuk,
                   (yuksek == 1 && dusuk == 0) ? "" : "<<< TAKILI KALMIS");
        }
        printf("\n");

        /* Takili kalan pin disaridan mi suruluyor? Cikisi birakip once
         * asagi sonra yukari cekerek olcuyoruz: ikisinde de ayni seviye
         * okunuyorsa pini baska bir sey suruyor demektir. */
        printf("   Serbest birakildiginda (dahili pull ile olculdu):\n");
        const struct { uint pin; const char *ad; } serbest[] = {
            { PIN_RST, "RST(34)" }, { PB_PIN_LCD_TE, "TE(35)" },
            { PB_PIN_LCD_BL, "BL(36)" }, { PB_PIN_BL_EN, "BL_EN(37)" },
        };
        for (size_t i = 0; i < sizeof(serbest) / sizeof(serbest[0]); i++) {
            gpio_set_function(serbest[i].pin, GPIO_FUNC_SIO);
            gpio_set_dir(serbest[i].pin, GPIO_IN);
            gpio_pull_down(serbest[i].pin); sleep_ms(2);
            int pd = gpio_get(serbest[i].pin);
            gpio_pull_up(serbest[i].pin);   sleep_ms(2);
            int pu = gpio_get(serbest[i].pin);
            gpio_disable_pulls(serbest[i].pin);
            const char *yorum = (pd == 0 && pu == 1) ? "serbest (normal)"
                              : (pd == 1 && pu == 1) ? "DISARIDAN YUKSEK SURULUYOR"
                              : (pd == 0 && pu == 0) ? "DISARIDAN DUSUK SURULUYOR"
                                                     : "belirsiz";
            printf("   %-10s pull-down->%d  pull-up->%d   %s\n",
                   serbest[i].ad, pd, pu, yorum);
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
        const struct { float bolen; const char *ad; } hizlar[] = {
            { 2.0f,  "clkdiv 2  (~37.5 MHz)" },
            { 40.0f, "clkdiv 40 (~1.9 MHz)"  },
        };

        for (size_t h = 0; h < sizeof(hizlar) / sizeof(hizlar[0]); h++) {
            pio_sm_set_clkdiv(qspi.pio, qspi.sm, hizlar[h].bolen);
            pio_sm_clkdiv_restart(qspi.pio, qspi.sm);

            uint32_t taban = TE_SAY();

            LCD_3IN49_SendSimpleCmd(0x34);          /* TEOFF */
            sleep_ms(20);
            uint32_t kapali = TE_SAY();

            QSPI_Select(qspi);                      /* TEON */
            QSPI_REGISTER_Write(qspi, 0x35);
            QSPI_DATA_Write(qspi, 0x00);
            QSPI_Deselect(qspi);
            sleep_ms(20);
            uint32_t acik = TE_SAY();

            printf("   %s\n", hizlar[h].ad);
            printf("     taban %lu  ->  TEOFF %lu  ->  TEON %lu\n",
                   (unsigned long)taban, (unsigned long)kapali,
                   (unsigned long)acik);
            if (taban > 4 && kapali < 4 && acik > 4) {
                printf("     -> KOMUTLAR ULASIYOR. Panel emirlere uyuyor.\n");
            } else if (taban > 4) {
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

        uint32_t taban = te_gecis_say();
        bb_cmd(0x34, NULL, 0);                 /* TEOFF */
        sleep_ms(20);
        uint32_t kapali = te_gecis_say();
        uint8_t param = 0x00;
        bb_cmd(0x35, &param, 1);               /* TEON */
        sleep_ms(20);
        uint32_t acik = te_gecis_say();

        printf("   TE: taban %lu -> TEOFF %lu -> TEON %lu\n",
               (unsigned long)taban, (unsigned long)kapali, (unsigned long)acik);
        printf("   -> %s\n", (taban > 4 && kapali < 4 && acik > 4)
               ? "BIT-BANG CALISIYOR. Hata PIO dalga seklinde."
               : "bit-bang de etkisiz. Sorun PIO'da degil.");

        /* Panelden oku: 0x04 = RDDID, 0x0A = guc modu, 0x0C = piksel bicimi.
         * Hepsi 0x00 ya da hepsi 0xFF gelirse panel hic yanit vermiyordur
         * (hat sirasiyla asagi ya da yukari cekili kaliyor). */
        const struct { uint8_t reg; const char *ad; } okumalar[] = {
            { 0x04, "RDDID  (uretici/surum/kimlik)" },
            { 0x0A, "RDDPM  (guc modu)" },
            { 0x0C, "RDDCOLMOD (piksel bicimi)" },
        };
        int anlamli = 0;
        for (size_t i = 0; i < sizeof(okumalar) / sizeof(okumalar[0]); i++) {
            uint8_t buf[6] = {0};
            bb_read(okumalar[i].reg, buf, sizeof(buf));
            printf("   0x%02x %-28s:", okumalar[i].reg, okumalar[i].ad);
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

        /* PIO'yu geri ac */
        QSPI_GPIO_Init(qspi);
        for (uint p = PIN_SCLK; p <= PIN_DIO3; p++) pio_gpio_init(qspi.pio, p);
        pio_sm_set_consecutive_pindirs(qspi.pio, qspi.sm, PIN_SCLK, 1, true);
        pio_sm_set_consecutive_pindirs(qspi.pio, qspi.sm, PIN_DIO0, 4, true);
        pio_sm_set_enabled(qspi.pio, qspi.sm, true);
    }

    /* â”€â”€ 7. Yedek: gozle komut yolu testi â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
     * Olculebilir testler sonucsuz kalirsa diye duruyor. */
    {
        printf("7) Komut yolu testi — EKRANA BAKIN.\n");
        printf("   Karincalanmada HERHANGI bir degisiklik (sonme, renklerin\n");
        printf("   tersine donmesi, parlaklik oynamasi) gorurseniz TUSA BASIN.\n\n");

        backlight_set(true);
        drain_stdin();

        const struct { uint8_t cmd; const char *ad; } adimlar[] = {
            { 0x28, "DISPOFF  (ekran sonmeli)" },
            { 0x29, "DISPON   (geri gelmeli)" },
            { 0x21, "INVON    (renkler terslenmeli)" },
            { 0x20, "INVOFF   (geri donmeli)" },
            { 0x28, "DISPOFF  (ekran sonmeli)" },
            { 0x29, "DISPON   (geri gelmeli)" },
        };
        for (size_t i = 0; i < sizeof(adimlar) / sizeof(adimlar[0]); i++) {
            LCD_3IN49_SendSimpleCmd(adimlar[i].cmd);
            printf("   0x%02x %s\n", adimlar[i].cmd, adimlar[i].ad);
            for (int t = 0; t < 20; t++) {
                if (getchar_timeout_us(0) >= 0) {
                    printf("\n   >>> KOMUT YOLU CALISIYOR — degisiklik 0x%02x sonrasi <<<\n",
                           adimlar[i].cmd);
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

    const struct { int en; int bl; int pwm_duty; const char *ad; } durum[] = {
        { 1, 0, -1, "BL_EN=1  LCD_BL=0 (duz GPIO)" },
        { 1, 1, -1, "BL_EN=1  LCD_BL=1 (duz GPIO)" },
        { 0, 0, -1, "BL_EN=0  LCD_BL=0 (duz GPIO)" },
        { 0, 1, -1, "BL_EN=0  LCD_BL=1 (duz GPIO)" },
        { 1, 0,  5, "BL_EN=1  PWM duty %5   (aktif-dusuk ise PARLAK)" },
        { 1, 0, 95, "BL_EN=1  PWM duty %95  (aktif-yuksek ise PARLAK)" },
    };

    for (size_t i = 0; i < sizeof(durum) / sizeof(durum[0]); i++) {
        gpio_put(PB_PIN_BL_EN, durum[i].en);
        if (durum[i].pwm_duty < 0) {
            gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_SIO);
            gpio_set_dir(PB_PIN_LCD_BL, GPIO_OUT);
            gpio_put(PB_PIN_LCD_BL, durum[i].bl);
        } else {
            gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_PWM);
            pwm_set_gpio_level(PB_PIN_LCD_BL,
                               (uint16_t)((BL_PWM_WRAP - 1) * durum[i].pwm_duty / 100));
        }
        printf("  [%u] %s\n", (unsigned)(i + 1), durum[i].ad);

        for (int t = 0; t < 50; t++) {
            if (getchar_timeout_us(0) >= 0) {
                printf("\n  >>> ISIK YANAN DURUM: [%u] %s <<<\n\n",
                       (unsigned)(i + 1), durum[i].ad);
                return;
            }
            sleep_ms(100);
        }
    }
    printf("  hicbir durumda tus basilmadi\n\n");
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
    printf("  s  canli spektrogram\n");
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
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    pb_display_dma_init();
    LCD_3IN49_Init();
    pb_lcd_fill(0x0000);
    printf("Ekran hazir (%dx%d panel).\n", PB_PANEL_W, PB_PANEL_H);

    cmd_info();
    print_help();

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
            case 'o': cmd_orientation(); break;
            case 't': cmd_touch_probe(); break;
            case 'u': cmd_ui_demo(); break;
            case 'm': cmd_mel_pipeline(); break;
            case 'a': cmd_full_demo(); break;
            case '?': print_help();    break;
            case '\r': case '\n': printf("\r"); break;
            default:  printf("bilinmeyen komut ('?' yardim)\n"); break;
        }
    }
}
