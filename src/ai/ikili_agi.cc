/**
 * ikili_agi.cc — Aşama-1 ikili ağ, TFLM ile çıkarım
 *
 * tur_agi.cc ile aynı desen (M6): statik BSS arena'sı, malloc yok, ölçülebilir
 * bütçe. Fark: çıkış tek skaler (sigmoid öncesi ham logit), 179 sınıf değil.
 */
#include "ai/ikili_agi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "dsp/mel.h"
#include "pico/stdlib.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "../../models/ikili_agi_int8.h"

/* Arena boyutu — ÖLÇÜLDÜ, tahmin değil (tur_agi.cc'deki gerekçenin aynısı).
 *
 * İlk tahmin (32 KB) yetmedi: TFLM "60.160 bayt istiyorum" dedi. 96 KB'a
 * geçici büyütülüp `X` komutuyla ölçüldü: arena_used_bytes() = 63.876 bayt.
 * 72 KB ayrılıyor: ölçülenin üstünde 9.852 bayt (%15) pay — tur_agi.cc'deki
 * gibi TFLM'in hizalama davranışı yerleşime göre birkaç yüz bayt oynayabilir.
 *
 * ⚠ MODEL DEĞİŞİRSE BU SAYI DA DEĞİŞİR. `X` komutu her koşuda kullanılan
 * baytı basıyor. CMake'ten -DPB_IKILI_ARENA_BAYT=... ile ezilebilir. */
#ifndef PB_IKILI_ARENA_BAYT
#define PB_IKILI_ARENA_BAYT (72 * 1024)
#endif

alignas(16) static uint8_t s_arena[PB_IKILI_ARENA_BAYT];

/* Aynı dört op türü tür ağıyla ölçüldü (CONV_2D 4, DEPTHWISE_CONV_2D 3,
 * FULLY_CONNECTED 1, MEAN 1) — küçük model ama aynı katman türlerinden
 * kurulu (bkz. tools/ikili_egit.py model_kur). */
static tflite::MicroMutableOpResolver<4> s_resolver;

alignas(alignof(tflite::MicroInterpreter))
static uint8_t s_interp_bellek[sizeof(tflite::MicroInterpreter)];
static tflite::MicroInterpreter *s_interp = nullptr;

static TfLiteTensor *s_girdi  = nullptr;
static TfLiteTensor *s_cikti  = nullptr;
static uint32_t      s_sure_us = 0;

extern "C" bool pb_ikili_agi_baslat(void) {
    if (s_interp) return true;

    const tflite::Model *model = tflite::GetModel(pb_ikili_agi);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        printf("[!] IKILI AGI: sema surumu %lu, beklenen %d\n",
               (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    if (s_resolver.AddConv2D()         != kTfLiteOk ||
        s_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
        s_resolver.AddFullyConnected()  != kTfLiteOk ||
        s_resolver.AddMean()            != kTfLiteOk) {
        printf("[!] IKILI AGI: op kaydi basarisiz\n");
        return false;
    }

    s_interp = new (s_interp_bellek) tflite::MicroInterpreter(
        model, s_resolver, s_arena, sizeof(s_arena));

    if (s_interp->AllocateTensors() != kTfLiteOk) {
        printf("[!] IKILI AGI: AllocateTensors BASARISIZ — arena %u bayt yetmedi.\n"
               "    PB_IKILI_ARENA_BAYT'i buyutup tekrar deneyin.\n",
               (unsigned)sizeof(s_arena));
        s_interp = nullptr;
        return false;
    }

    s_girdi = s_interp->input(0);
    s_cikti = s_interp->output(0);

    /* Cihaz sözleşmesinin cihaz tarafındaki sağlaması (tur_agi.cc'deki
     * gerekçenin aynısı, §9k): model değişip ölçek 1.0'dan kayarsa memcpy
     * sessizce yanlış girdi verir. */
    const size_t bekleniyor = (size_t)PB_MEL_FRAMES * PB_MEL_BANDS;
    if (s_girdi->type != kTfLiteInt8 || s_girdi->bytes != bekleniyor) {
        printf("[!] IKILI AGI: girdi tensoru uyumsuz (tip %d, %u bayt; "
               "beklenen int8 %u bayt)\n",
               (int)s_girdi->type, (unsigned)s_girdi->bytes,
               (unsigned)bekleniyor);
        s_interp = nullptr;
        return false;
    }
    if (s_girdi->params.scale != 1.0f || s_girdi->params.zero_point != 0) {
        printf("[!] IKILI AGI: girdi olcegi %.6f / sifir %d — 1.0 / 0 bekleniyordu.\n",
               (double)s_girdi->params.scale, (int)s_girdi->params.zero_point);
        s_interp = nullptr;
        return false;
    }
    if (s_cikti->type != kTfLiteInt8 || s_cikti->bytes != 1) {
        printf("[!] IKILI AGI: cikti tensoru uyumsuz (%u bayt, beklenen 1)\n",
               (unsigned)s_cikti->bytes);
        s_interp = nullptr;
        return false;
    }
    return true;
}

extern "C" size_t pb_ikili_agi_arena_kullanilan(void) {
    return s_interp ? s_interp->arena_used_bytes() : 0;
}

extern "C" size_t pb_ikili_agi_arena_toplam(void) { return sizeof(s_arena); }

extern "C" int8_t *pb_ikili_agi_girdi(void) {
    return s_girdi ? s_girdi->data.int8 : nullptr;
}

extern "C" bool pb_ikili_agi_calistir(void) {
    if (!s_interp) return false;
    const uint32_t t0 = time_us_32();
    const TfLiteStatus st = s_interp->Invoke();
    s_sure_us = time_us_32() - t0;
    return st == kTfLiteOk;
}

extern "C" uint32_t pb_ikili_agi_son_sure_us(void) { return s_sure_us; }

extern "C" int8_t pb_ikili_agi_cikti(void) {
    return s_cikti ? s_cikti->data.int8[0] : 0;
}

extern "C" float pb_ikili_agi_cikti_olcek(void) {
    return s_cikti ? s_cikti->params.scale : 0.0f;
}

extern "C" int pb_ikili_agi_cikti_sifir(void) {
    return s_cikti ? s_cikti->params.zero_point : 0;
}

extern "C" float pb_ikili_agi_olasilik(void) {
    if (!s_cikti) return 0.0f;
    const float logit = ((float)s_cikti->data.int8[0] -
                         (float)s_cikti->params.zero_point) *
                        s_cikti->params.scale;
    return 1.0f / (1.0f + expf(-logit));
}
