/**
 * species_net.h — stage-2 species net: the TFLM wrapper (C interface)
 *
 * Model: models/species_net_int8.h (produced by tools/train_species.py, M5).
 * 209,107 parameters, 6.6 MMAC per window, 179 classes (178 species plus a
 * negative class).
 *
 * THE DEVICE CONTRACT — the decision that made M6 cheap:
 *   the TFLite input tensor is int8, scale 1.0, zero point 0.
 *   So the output of pb_mel_window() is copied in DIRECTLY, with no
 *   conversion whatsoever.
 *   The contract is asserted inside tools/train_species.py; if it ever
 *   drifted the result would be a silent loss of accuracy, so
 *   pb_species_net_init() checks it here as well.
 *
 * Input layout: (1, 187, 64, 1) — frames on the outside (oldest to newest),
 * bands on the inside. This is the same layout pb_mel_window() produces in
 * mel.c, and the training set was generated the same way
 * (tools/build_dataset.py).
 */
#ifndef POKEBIRD_AI_SPECIES_NET_H
#define POKEBIRD_AI_SPECIES_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_SPECIES_NET_CLASSES   179  /* 178 species + negative (index 178) */
#define PB_SPECIES_NET_NEGATIVE  178

/**
 * Build the interpreter and allocate tensors. Once, at startup.
 * @return false on failure; the reason is printed to the serial console.
 */
bool pb_species_net_init(void);

/** How much of the arena is ACTUALLY used (bytes). Measured, not estimated. */
size_t pb_species_net_arena_used(void);

/** Total size of the allocated arena (bytes). */
size_t pb_species_net_arena_total(void);

/**
 * Raw int8 buffer of the input tensor — PB_MEL_FRAMES*PB_MEL_BANDS bytes.
 * pb_mel_window() can write straight into it.
 */
int8_t *pb_species_net_input(void);

/** Run one inference. @return true when TfLiteStatus is kOk. */
bool pb_species_net_run(void);

/** Duration of the last inference (microseconds). */
uint32_t pb_species_net_last_time_us(void);

/** Output logits, raw int8 — PB_SPECIES_NET_CLASSES of them. */
const int8_t *pb_species_net_output(void);

/** Output quantisation parameters (logit = (q - zero) * scale). */
float pb_species_net_output_scale(void);
int   pb_species_net_output_zero(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEBIRD_AI_SPECIES_NET_H */
