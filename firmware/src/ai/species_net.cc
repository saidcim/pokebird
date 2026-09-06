/**
 * tur_agi.cc — Aşama-2 tür ağı, TFLM ile çıkarım
 *
 * Arena BSS'te statik bir dizi; malloc kullanılmıyor. 520 KB'lik bir cihazda
 * yığın parçalanması istemiyoruz ve `arm-none-eabi-size` ile ölçülebilir
 * olması bütçeyi takip etmeyi kolaylaştırıyor (lastsession.md §9l).
 */
#include "ai/species_net.h"

#include <cstdio>
#include <cstring>

#include "dsp/mel.h"
#include "pico/stdlib.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* Üretilmiş model dizisi. Bu başlık dizinin KENDİSİNİ tanımlıyor (static
 * değil), o yüzden yalnızca bu çeviri biriminden dahil edilmeli. */
#include "../../../models/species_net_int8.h"

/* Arena boyutu — ÖLÇÜLDÜ, tahmin değil.
 *
 * Kartta `x` komutuyla ölçülen: arena_used_bytes() = 110.436 bayt.
 * §9k'daki "141 KB" bir tahmindi ve %28 fazlaydı; bütçe 180 KB idi.
 *
 * 120 KB ayrılıyor: ölçülenin üstünde 12.444 bayt (%11) pay var. Pay sıfır
 * bırakılmıyor çünkü TFLM'in hizalama davranışı tampon adreslerine bağlı ve
 * yerleşim değişince birkaç yüz bayt oynayabiliyor; 180 KB'da bırakmak ise
 * 61 KB'ı boşuna tutardı.
 *
 * ⚠ MODEL DEĞİŞİRSE BU SAYI DA DEĞİŞİR. `x` komutu her koşuda kullanılan
 * baytı basıyor; yeni model yüklendiğinde oraya bakın. Yetmezse
 * AllocateTensors başarısız oluyor ve sebebini yazıyor — sessiz kalmıyor.
 *
 * CMake'ten -DPB_TFLM_ARENA_BAYT=... ile ezilebilir (yeni model ölçerken
 * geçici olarak büyütmek için). */
#ifndef PB_TFLM_ARENA_BYTES
#define PB_TFLM_ARENA_BYTES (120 * 1024)
#endif

/* 16 bayt hizalama TFLM'in istediği asgari; buradaki 16'lık hizalama
 * CMSIS-NN'in vektör erişimleri için de yeterli. */
alignas(16) static uint8_t s_arena[PB_TFLM_ARENA_BYTES];

/* MicroMutableOpResolver şablon parametresi KAYIT SAYISI. Modelin gerçekten
 * kullandığı dört op (tools ile ölçüldü: CONV_2D 9, DEPTHWISE_CONV_2D 8,
 * FULLY_CONNECTED 1, MEAN 1). AllOpsResolver kullanılmıyor: 100+ çekirdeği
 * linkere bağlar ve flash'ı gereksiz şişirir. */
static tflite::MicroMutableOpResolver<4> s_resolver;

/* MicroInterpreter'ın varsayılan kurucusu yok; placement new ile kuruluyor
 * ki statik ömürlü olsun ve heap'e dokunulmasın. */
alignas(alignof(tflite::MicroInterpreter))
static uint8_t s_interp_memory[sizeof(tflite::MicroInterpreter)];
static tflite::MicroInterpreter *s_interp = nullptr;

static TfLiteTensor *s_input  = nullptr;
static TfLiteTensor *s_output  = nullptr;
static uint32_t      s_time_us = 0;

extern "C" bool pb_species_net_init(void) {
    if (s_interp) return true;

    const tflite::Model *model = tflite::GetModel(pb_species_net);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        printf("[!] TUR AGI: sema surumu %lu, beklenen %d\n",
               (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    if (s_resolver.AddConv2D()         != kTfLiteOk ||
        s_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
        s_resolver.AddFullyConnected()  != kTfLiteOk ||
        s_resolver.AddMean()            != kTfLiteOk) {
        printf("[!] TUR AGI: op kaydi basarisiz\n");
        return false;
    }

    s_interp = new (s_interp_memory) tflite::MicroInterpreter(
        model, s_resolver, s_arena, sizeof(s_arena));

    if (s_interp->AllocateTensors() != kTfLiteOk) {
        printf("[!] TUR AGI: AllocateTensors BASARISIZ — arena %u bayt yetmedi.\n"
               "    PB_TFLM_ARENA_BYTES'i buyutup tekrar deneyin.\n",
               (unsigned)sizeof(s_arena));
        s_interp = nullptr;
        return false;
    }

    s_input = s_interp->input(0);
    s_output = s_interp->output(0);

    /* ── Cihaz sözleşmesinin cihaz TARAFINDAKİ sağlaması ──────────────────
     * §9k'daki assert PC tarafındaydı. Aynı kontrolü burada da yapıyoruz:
     * model yeniden eğitilip .h değişirse ve ölçek 1.0'dan kayarsa,
     * memcpy sessizce yanlış girdi verir ve doğruluk hiçbir hata mesajı
     * olmadan düşer. Bu projede o sınıf hata iki kez pahalıya patladı. */
    const size_t waiting = (size_t)PB_MEL_FRAMES * PB_MEL_BANDS;
    if (s_input->type != kTfLiteInt8 || s_input->bytes != waiting) {
        printf("[!] TUR AGI: girdi tensoru uyumsuz (tip %d, %u bayt; "
               "beklenen int8 %u bayt)\n",
               (int)s_input->type, (unsigned)s_input->bytes,
               (unsigned)waiting);
        s_interp = nullptr;
        return false;
    }
    if (s_input->params.scale != 1.0f || s_input->params.zero_point != 0) {
        printf("[!] TUR AGI: girdi olcegi %.6f / sifir %d — 1.0 / 0 bekleniyordu.\n"
               "    Model degismis: memcpy yerine donusum gerekiyor (§9k).\n",
               (double)s_input->params.scale, (int)s_input->params.zero_point);
        s_interp = nullptr;
        return false;
    }
    if (s_output->type != kTfLiteInt8 || s_output->bytes != PB_SPECIES_NET_CLASSES) {
        printf("[!] TUR AGI: cikti tensoru uyumsuz (%u sinif, beklenen %d)\n",
               (unsigned)s_output->bytes, PB_SPECIES_NET_CLASSES);
        s_interp = nullptr;
        return false;
    }
    return true;
}

extern "C" size_t pb_species_net_arena_used(void) {
    return s_interp ? s_interp->arena_used_bytes() : 0;
}

extern "C" size_t pb_species_net_arena_total(void) { return sizeof(s_arena); }

extern "C" int8_t *pb_species_net_input(void) {
    return s_input ? s_input->data.int8 : nullptr;
}

extern "C" bool pb_species_net_run(void) {
    if (!s_interp) return false;
    const uint32_t t0 = time_us_32();
    const TfLiteStatus st = s_interp->Invoke();
    s_time_us = time_us_32() - t0;
    return st == kTfLiteOk;
}

extern "C" uint32_t pb_species_net_last_time_us(void) { return s_time_us; }

extern "C" const int8_t *pb_species_net_output(void) {
    return s_output ? s_output->data.int8 : nullptr;
}

extern "C" float pb_species_net_output_scale(void) {
    return s_output ? s_output->params.scale : 0.0f;
}

extern "C" int pb_species_net_output_zero(void) {
    return s_output ? s_output->params.zero_point : 0;
}
