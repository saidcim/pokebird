/**
 * mel.h — log-mel feature extraction and its ring buffer
 *
 * The parameters:
 *   24 kHz mono · FFT 512 (Hann) · hop 384 (16 ms) · 64 mel bands
 *   150 Hz - 11.5 kHz · 3 s window = 187 frames
 *
 * THE CRITICAL MEMORY DECISION: three seconds of raw audio is NOT KEPT (that
 * would be 144 KB). Mel frames are computed INCREMENTALLY as audio arrives
 * and held in a 64x187 int8 ring buffer — 12 KB. This single decision saves
 * about 130 KB.
 *
 * MATCHING THE REFERENCE: the filter bank is built on the HTK mel scale and
 * WITHOUT area normalisation. The exact Python equivalent is:
 *
 *     librosa.filters.mel(sr=24000, n_fft=512, n_mels=64,
 *                         fmin=150, fmax=11500, htk=True, norm=None)
 *
 * The window is a periodic Hann (`sym=False`). If either of these details
 * fails to match, the features on the device diverge from the ones used in
 * training and the model quietly performs badly — the most expensive class of
 * bug to debug.
 */
#ifndef POKEBIRD_MEL_H
#define POKEBIRD_MEL_H

#include <stdbool.h>
#include <stdint.h>

#define PB_SAMPLE_RATE   24000
#define PB_MEL_BANDS     64
#define PB_MEL_HOP       384          /* 16 ms */
#define PB_MEL_FRAMES    187          /* 3 s / 16 ms */
#define PB_MEL_FMIN      150.0f
#define PB_MEL_FMAX      11500.0f

/* int8 storage: log power in dB is mapped linearly onto this range.
 * The -90 dBFS floor is well below the ~-36 dBFS of measured room noise, and
 * the 0 dBFS ceiling is full scale. The resolution is about 0.35 dB — ample
 * for birdsong. */
#define PB_MEL_DB_MIN    (-90.0f)
#define PB_MEL_DB_MAX    (0.0f)

/** Build the filter bank. Once, before any other call. */
void pb_mel_init(void);

/**
 * Produce one log-mel frame (leaves the ring buffer alone).
 * @param samples PB_FFT_SIZE int16 values
 * @param out     PB_MEL_BANDS int8 values; the dB->int8 mapping is above
 */
void pb_mel_frame(const int16_t *samples, int8_t *out);

/** Compute the frame and push it into the ring buffer. */
void pb_mel_push(const int16_t *samples);

/** Reset the ring buffer. */
void pb_mel_reset(void);

/** Total frames pushed into the buffer (used to tell when 3 s is full). */
uint32_t pb_mel_frame_count(void);

/**
 * Copy the int8 values of the most recently pushed frame — for the live
 * display. Does NOT recompute the mel frame; it reads from the ring.
 * @param out PB_MEL_BANDS int8 values
 * @return    false if no frame has been pushed yet
 */
bool pb_mel_last_frame(int8_t *out);

/**
 * Return the last three seconds, normalised as model input.
 *
 * Per-window mean/variance normalisation happens here: recording level and
 * microphone gain vary from device to device, and feeding the model absolute
 * dB values would push that variability straight into accuracy.
 *
 * @param out PB_MEL_BANDS * PB_MEL_FRAMES int8 values, frames oldest to newest
 * @return    false if 187 frames have not accumulated yet (nothing is written)
 */
bool pb_mel_window(int8_t *out);

/** Convert an int8 storage value back to dB — for tests and diagnostics. */
float pb_mel_q_to_db(int8_t q);

#endif /* POKEBIRD_MEL_H */
