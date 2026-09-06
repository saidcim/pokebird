/**
 * species_net.cc — stage-2 species net, inference through TFLM
 *
 * The arena is a static array in BSS; malloc is never used. On a 520 KB
 * device we do not want heap fragmentation, and being measurable with
 * `arm-none-eabi-size` makes the budget easy to track.
 */
#include "ai/species_net.h"

#include <cstdio>
#include <cstring>

#include "dsp/mel.h"
#include "pico/stdlib.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* The generated model array. This header DEFINES the array itself (it is not
 * static), so it must be included from this translation unit only. */
#include "../../../models/species_net_int8.h"

/* Arena size — MEASURED, not guessed.
 *
 * Measured on the board with the `x` command: arena_used_bytes() = 110,436
 * bytes. An earlier "141 KB" figure was an estimate and 28% too high; the
 * budget at the time was 180 KB.
 *
 * We allocate 120 KB, leaving 12,444 bytes (11%) of headroom above the
 * measurement. The headroom is not zero because TFLM's alignment behaviour
 * depends on buffer addresses and can move by a few hundred bytes when the
 * layout changes; leaving it at 180 KB, on the other hand, would have wasted
 * 61 KB.
 *
 * WARNING: IF THE MODEL CHANGES, SO DOES THIS NUMBER. The `x` command prints
 * the bytes used on every run; check it after loading a new model. If it is
 * not enough, AllocateTensors fails and says why — it does not fail silently.
 *
 * Can be overridden from CMake with -DPB_TFLM_ARENA_BYTES=... (useful for
 * temporarily growing it while measuring a new model). */
#ifndef PB_TFLM_ARENA_BYTES
#define PB_TFLM_ARENA_BYTES (120 * 1024)
#endif

/* 16-byte alignment is the minimum TFLM asks for, and it is also enough for
 * CMSIS-NN's vector accesses. */
alignas(16) static uint8_t s_arena[PB_TFLM_ARENA_BYTES];

/* The MicroMutableOpResolver template parameter is the NUMBER OF
 * REGISTRATIONS. These are the four ops the model actually uses (measured:
 * CONV_2D 9, DEPTHWISE_CONV_2D 8, FULLY_CONNECTED 1, MEAN 1). AllOpsResolver
 * is deliberately not used: it drags 100+ kernels into the link and bloats
 * flash for nothing. */
static tflite::MicroMutableOpResolver<4> s_resolver;

/* MicroInterpreter has no default constructor, so it is built with placement
 * new to give it static lifetime and keep the heap untouched. */
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
        printf("[!] SPECIES NET: schema version %lu, expected %d\n",
               (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    if (s_resolver.AddConv2D()         != kTfLiteOk ||
        s_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
        s_resolver.AddFullyConnected()  != kTfLiteOk ||
        s_resolver.AddMean()            != kTfLiteOk) {
        printf("[!] SPECIES NET: op registration failed\n");
        return false;
    }

    s_interp = new (s_interp_memory) tflite::MicroInterpreter(
        model, s_resolver, s_arena, sizeof(s_arena));

    if (s_interp->AllocateTensors() != kTfLiteOk) {
        printf("[!] SPECIES NET: AllocateTensors FAILED - an arena of %u bytes "
               "was not enough.\n"
               "    Increase PB_TFLM_ARENA_BYTES and try again.\n",
               (unsigned)sizeof(s_arena));
        s_interp = nullptr;
        return false;
    }

    s_input = s_interp->input(0);
    s_output = s_interp->output(0);

    /* ── The DEVICE-side check of the device contract ─────────────────────
     * The training-time assert lives on the PC side. We repeat the same check
     * here: if the model is retrained and the .h changes so that the scale
     * drifts away from 1.0, the memcpy would silently feed the net the wrong
     * input and accuracy would drop with no error message at all. That class
     * of bug has cost this project dearly twice. */
    const size_t expected = (size_t)PB_MEL_FRAMES * PB_MEL_BANDS;
    if (s_input->type != kTfLiteInt8 || s_input->bytes != expected) {
        printf("[!] SPECIES NET: input tensor mismatch (type %d, %u bytes; "
               "expected int8 %u bytes)\n",
               (int)s_input->type, (unsigned)s_input->bytes,
               (unsigned)expected);
        s_interp = nullptr;
        return false;
    }
    if (s_input->params.scale != 1.0f || s_input->params.zero_point != 0) {
        printf("[!] SPECIES NET: input scale %.6f / zero %d - expected 1.0 / 0.\n"
               "    The model has changed: a conversion is now needed instead "
               "of a plain memcpy.\n",
               (double)s_input->params.scale, (int)s_input->params.zero_point);
        s_interp = nullptr;
        return false;
    }
    if (s_output->type != kTfLiteInt8 || s_output->bytes != PB_SPECIES_NET_CLASSES) {
        printf("[!] SPECIES NET: output tensor mismatch (%u classes, expected %d)\n",
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
