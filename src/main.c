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
#include "hardware/pwm.h"

#include "board_config.h"
#include "hal/audio_i2s.h"
#include "hal/es8311.h"
#include "hal/i2c_bus.h"
#include "hal/display/LCD_3in49.h"
#include "hal/display/lcd_blit.h"
#include "hal/display/qspi_pio.h"
#include "dsp/fft.h"
#include "ui/spectrogram.h"

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

/* ── Ekran ────────────────────────────────────────────────────────────────
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

/* ── Komutlar ────────────────────────────────────────────────────────────── */

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

/**
 * Ekran testi — "hiçbir şey görünmüyor" durumunu ayrıştırır.
 *
 * Arka ışık ayrı, panel ayrı bir sorun olabilir. Bu test önce arka ışığı
 * yakıp beyaz basıyor: ekran beyaz oluyorsa QSPI ve panel init'i çalışıyor
 * demektir ve sorun çizim mantığındadır. Hiçbir şey olmuyorsa sorun daha
 * aşağıda: arka ışık ya da panel başlatma.
 */
static void cmd_display_test(void) {
    printf("\nEkran testi. Her renk 2 saniye.\n");
    backlight_set(true);

    const struct { const char *ad; uint16_t renk; } adimlar[] = {
        { "beyaz",     0xFFFF },
        { "kirmizi",   0xF800 },
        { "yesil",     0x07E0 },
        { "mavi",      0x001F },
        { "siyah",     0x0000 },
    };

    for (size_t i = 0; i < sizeof(adimlar) / sizeof(adimlar[0]); i++) {
        printf("  %s\n", adimlar[i].ad);
        pb_lcd_fill(adimlar[i].renk);
        sleep_ms(2000);
    }

    /* Yön kontrolü: panelin SOL UST kosesine kucuk beyaz bir kare.
     * Cihazi yatay tuttugunuzda karenin nerede oldugu, yon cevirimimizin
     * dogru olup olmadigini soyler. */
    static uint16_t kare[20 * 20];
    for (int i = 0; i < 20 * 20; i++) kare[i] = 0xFFFF;
    pb_lcd_blit(0, 0, 20, 20, kare);
    printf("  panel (0,0) konumuna 20x20 beyaz kare cizildi\n\n");
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
    printf("  ?  bu yardim\n\n");
}

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\n========================================\n");
    printf(" PokeBird — M1: mikrofon bring-up\n");
    printf("========================================\n");

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

    /* ── Ekran ──────────────────────────────────────────────────────────
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
            case '?': print_help();    break;
            case '\r': case '\n': printf("\r"); break;
            default:  printf("bilinmeyen komut ('?' yardim)\n"); break;
        }
    }
}
