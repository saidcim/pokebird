/**
 * binary_net.h — stage-1 binary net: is this birdsong or not (TFLM wrapper)
 *
 * Model: models/binary_net_int8.h (produced by tools/train_binary.py, M7).
 * 7,217 parameters, 1.44 MMAC per window, a SINGLE output (the raw logit,
 * before the sigmoid).
 *
 * The device contract is identical to the species net's: the input tensor is
 * int8 with scale 1.0 and zero point 0, so the output of pb_mel_window() can
 * be copied in with no conversion at all. pb_binary_net_init() measures this
 * and verifies it.
 *
 * The input layout matches species_net.h: (1, 187, 64, 1), frames on the
 * outside, bands on the inside.
 *
 * WHY A SEPARATE ARENA: the two nets never run at the same time (gate ->
 * binary net -> species net if it says "bird"), but having two
 * MicroInterpreters share one static arena would mean calling TFLM's
 * AllocateTensors twice in an interleaved order, which is fragile. A separate
 * arena is far smaller — on the order of 15-30 KB, negligible next to the
 * species net's 120 KB.
 */
#ifndef POKEBIRD_AI_BINARY_NET_H
#define POKEBIRD_AI_BINARY_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build the interpreter and allocate tensors. Once, at startup.
 * @return false on failure; the reason is printed to the serial console.
 */
bool pb_binary_net_init(void);

/** How much of the arena is ACTUALLY used (bytes). Measured, not estimated. */
size_t pb_binary_net_arena_used(void);

/** Total size of the allocated arena (bytes). */
size_t pb_binary_net_arena_total(void);

/** Raw int8 buffer of the input tensor — PB_MEL_FRAMES*PB_MEL_BANDS bytes. */
int8_t *pb_binary_net_input(void);

/** Run one inference. @return true when TfLiteStatus is kOk. */
bool pb_binary_net_run(void);

/** Duration of the last inference (microseconds). */
uint32_t pb_binary_net_last_time_us(void);

/** Output logit, raw int8 (pre-sigmoid, a SINGLE value). */
int8_t pb_binary_net_output(void);

/** Output quantisation parameters (logit = (q - zero) * scale). */
float pb_binary_net_output_scale(void);
int   pb_binary_net_output_zero(void);

/**
 * P(bird) for the last inference, with the sigmoid applied (0..1).
 * Call after pb_binary_net_run().
 */
float pb_binary_net_probability(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEBIRD_AI_BINARY_NET_H */
