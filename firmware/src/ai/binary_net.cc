/**
 * binary_net.cc — stage-1 binary net, inference through TFLM
 *
 * Same pattern as species_net.cc (M6): a static arena in BSS, no malloc, a
 * budget you can measure. The difference is that the output is a single
 * scalar (the raw pre-sigmoid logit) rather than 179 classes.
 */
#include "ai/binary_net.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "dsp/mel.h"
#include "pico/stdlib.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "../../../models/binary_net_int8.h"

/* Arena size — MEASURED, not guessed (same reasoning as species_net.cc).
 *
 * The first guess (32 KB) was not enough: TFLM asked for 60,160 bytes. Grown
 * temporarily to 96 KB and measured with the `X` command:
 * arena_used_bytes() = 63,876 bytes. We allocate 72 KB, leaving 9,852 bytes
 * (15%) of headroom above the measurement — as in species_net.cc, TFLM's
 * alignment behaviour can shift by a few hundred bytes with the layout.
 *
 * WARNING: IF THE MODEL CHANGES, SO DOES THIS NUMBER. The `X` command prints
 * the bytes actually used on every run. It can be overridden from CMake with
 * -DPB_BINARY_ARENA_BYTES=... */
#ifndef PB_BINARY_ARENA_BYTES
#define PB_BINARY_ARENA_BYTES (72 * 1024)
#endif

alignas(16) static uint8_t s_arena[PB_BINARY_ARENA_BYTES];

/* The same four op types as the species net (CONV_2D 4,
 * DEPTHWISE_CONV_2D 3, FULLY_CONNECTED 1, MEAN 1) — a small model, but built
 * from the same kinds of layer (see build_model in tools/train_binary.py). */
static tflite::MicroMutableOpResolver<4> s_resolver;

alignas(alignof(tflite::MicroInterpreter))
static uint8_t s_interp_memory[sizeof(tflite::MicroInterpreter)];
static tflite::MicroInterpreter *s_interp = nullptr;

static TfLiteTensor *s_input  = nullptr;
static TfLiteTensor *s_output  = nullptr;
static uint32_t      s_time_us = 0;

extern "C" bool pb_binary_net_init(void) {
    if (s_interp) return true;

    const tflite::Model *model = tflite::GetModel(pb_binary_net);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        printf("[!] BINARY NET: schema version %lu, expected %d\n",
               (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    if (s_resolver.AddConv2D()         != kTfLiteOk ||
        s_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
        s_resolver.AddFullyConnected()  != kTfLiteOk ||
        s_resolver.AddMean()            != kTfLiteOk) {
        printf("[!] BINARY NET: op registration failed\n");
        return false;
    }

    s_interp = new (s_interp_memory) tflite::MicroInterpreter(
        model, s_resolver, s_arena, sizeof(s_arena));

    if (s_interp->AllocateTensors() != kTfLiteOk) {
        printf("[!] BINARY NET: AllocateTensors FAILED - an arena of %u bytes was not enough.\n"
               "    Increase PB_BINARY_ARENA_BYTES and try again.\n",
               (unsigned)sizeof(s_arena));
        s_interp = nullptr;
        return false;
    }

    s_input = s_interp->input(0);
    s_output = s_interp->output(0);

    /* The device-side check of the device contract (same reasoning as
     * species_net.cc): if the model changes and the scale drifts away from
     * 1.0, the memcpy would silently feed the net the wrong input. */
    const size_t expected = (size_t)PB_MEL_FRAMES * PB_MEL_BANDS;
    if (s_input->type != kTfLiteInt8 || s_input->bytes != expected) {
        printf("[!] BINARY NET: input tensor mismatch (type %d, %u bytes; "
               "expected int8 %u bytes)\n",
               (int)s_input->type, (unsigned)s_input->bytes,
               (unsigned)expected);
        s_interp = nullptr;
        return false;
    }
    if (s_input->params.scale != 1.0f || s_input->params.zero_point != 0) {
        printf("[!] BINARY NET: input scale %.6f / zero %d - expected 1.0 / 0.\n",
               (double)s_input->params.scale, (int)s_input->params.zero_point);
        s_interp = nullptr;
        return false;
    }
    if (s_output->type != kTfLiteInt8 || s_output->bytes != 1) {
        printf("[!] BINARY NET: output tensor mismatch (%u bytes, expected 1)\n",
               (unsigned)s_output->bytes);
        s_interp = nullptr;
        return false;
    }
    return true;
}

extern "C" size_t pb_binary_net_arena_used(void) {
    return s_interp ? s_interp->arena_used_bytes() : 0;
}

extern "C" size_t pb_binary_net_arena_total(void) { return sizeof(s_arena); }

extern "C" int8_t *pb_binary_net_input(void) {
    return s_input ? s_input->data.int8 : nullptr;
}

extern "C" bool pb_binary_net_run(void) {
    if (!s_interp) return false;
    const uint32_t t0 = time_us_32();
    const TfLiteStatus st = s_interp->Invoke();
    s_time_us = time_us_32() - t0;
    return st == kTfLiteOk;
}

extern "C" uint32_t pb_binary_net_last_time_us(void) { return s_time_us; }

extern "C" int8_t pb_binary_net_output(void) {
    return s_output ? s_output->data.int8[0] : 0;
}

extern "C" float pb_binary_net_output_scale(void) {
    return s_output ? s_output->params.scale : 0.0f;
}

extern "C" int pb_binary_net_output_zero(void) {
    return s_output ? s_output->params.zero_point : 0;
}

extern "C" float pb_binary_net_probability(void) {
    if (!s_output) return 0.0f;
    const float logit = ((float)s_output->data.int8[0] -
                         (float)s_output->params.zero_point) *
                        s_output->params.scale;
    return 1.0f / (1.0f + expf(-logit));
}
