/**
 * PokeBird — İstanbul kuş sesi tanıma cihazı
 * M0: iskelet. Kartın programlanabildiğini ve derleme zincirinin doğru
 * yapılandırıldığını kanıtlar.
 *
 * Kartta yazılımdan kontrol edilebilen bir LED yok (tek LED, şarj durum
 * LED'i ve GPIO'ya bağlı değil). Bu yüzden canlılık göstergesi olarak ekran
 * arka ışığını nefes alır gibi kısıp açıyoruz — aynı zamanda BL_EN yükseltici
 * ve LCD_BL PWM hattının çalıştığını da doğruluyor.
 */
#include <assert.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"

#include "board_config.h"

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

#define BL_PWM_WRAP 2048

static void backlight_init(void) {
    /* Arka ışık yükselticisini (AP3032) etkinleştir */
    gpio_init(PB_PIN_BL_EN);
    gpio_set_dir(PB_PIN_BL_EN, GPIO_OUT);
    gpio_put(PB_PIN_BL_EN, 1);

    /* Parlaklık PWM'i.
     * NOT (plan §10): PWM frekansı mikrofon SNR'ını etkileyebilir. Duyulabilir
     * banda (<20 kHz) girmemesi için kasten yüksek tutuldu; M1'de ölçülecek. */
    gpio_set_function(PB_PIN_LCD_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PB_PIN_LCD_BL);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, BL_PWM_WRAP - 1);
    pwm_config_set_clkdiv(&cfg, 1.0f);   /* ~150 MHz / 2048 ≈ 73 kHz */
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level(PB_PIN_LCD_BL, 0);
}

static void backlight_set(uint16_t level) {
    if (level >= BL_PWM_WRAP) level = BL_PWM_WRAP - 1;
    pwm_set_gpio_level(PB_PIN_LCD_BL, level);
}

static void print_banner(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    printf("\n");
    printf("========================================\n");
    printf(" PokeBird — Istanbul kus sesi tanima\n");
    printf(" M0: iskelet\n");
    printf("========================================\n");
    printf(" MCU           : RP2350%s\n", PICO_RP2350A ? "A" : "B (48 GPIO)");
    printf(" Sistem saati  : %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
    printf(" Flash         : %d MB\n", PICO_FLASH_SIZE_BYTES / (1024 * 1024));
    printf(" Ekran         : %dx%d (yatay)\n", PB_LCD_W, PB_LCD_H);
    printf(" Ornekleme     : %d Hz, %d-bit mono\n", PB_SAMPLE_RATE, PB_BITS_PER_SAMPLE);
    printf(" Kart ID       : ");
    for (size_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        printf("%02x", id.id[i]);
    }
    printf("\n");
    printf("========================================\n\n");
}

int main(void) {
    stdio_init_all();

    /* USB CDC'nin sayılması için kısa bir pay — bağlanmazsa yine de devam et,
     * cihaz her zaman USB'ye takılı olmayacak. */
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
        sleep_ms(100);
    }

    print_banner();
    backlight_init();

    uint32_t tick = 0;
    while (true) {
        /* Nefes alan arka ışık: üçgen dalga, ~2 saniyelik döngü */
        uint32_t phase = tick % 100;
        uint32_t bright = (phase < 50) ? phase : (100 - phase);
        backlight_set((uint16_t)(bright * (BL_PWM_WRAP / 50)));

        if (tick % 50 == 0) {
            printf("[%6lu s] calisiyor\n", (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
        }

        tick++;
        sleep_ms(20);
    }
}
