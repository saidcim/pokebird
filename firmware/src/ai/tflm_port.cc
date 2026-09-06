/**
 * tflm_port.cc — the two functions TFLM expects the platform to provide
 *
 * TFLM leaves two things to the target: log output (DebugLog) and the
 * profiling clock (micro_time). The reference implementations are excluded on
 * the CMake side (cmake/tflm.cmake) and these take their place.
 *
 * WHY OUR OWN DebugLog: the reference version calls `vfprintf(stderr, ...)`.
 * That drags in newlib's entire stdio locking machinery
 * (__retarget_lock_acquire_recursive and friends), which the Pico SDK's
 * minimal printf does not provide — the same trap was hit once before via
 * `fflush(stdout)`. `vprintf` goes to the Pico SDK's own printf and out over
 * USB CDC, which is where all the diagnostic output already flows.
 */
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "tensorflow/lite/micro/debug_log.h"
#include "tensorflow/lite/micro/micro_time.h"

extern "C" void DebugLog(const char* format, va_list args) {
  vprintf(format, args);
}

extern "C" int DebugVsnprintf(char* buffer, size_t buf_size, const char* format,
                              va_list vlist) {
  return vsnprintf(buffer, buf_size, format, vlist);
}

/**
 * abort() — we override newlib's DELIBERATELY.
 *
 * TFLM calls abort() in several places (micro_utils.cc, reduce_common.cc,
 * quantization_util.cc — TFLITE_ABORT / TFLITE_DCHECK). newlib's abort pulls
 * in the raise() -> signal() -> malloc() chain, and malloc in turn wants
 * newlib's locking machinery (__retarget_lock_acquire_recursive,
 * __lock___malloc_recursive_mutex), which the Pico SDK does not provide. The
 * symptom is a LINK error — the same `fflush(stdout)` trap as before, just
 * through a different door.
 *
 * Rather than writing lock stubs we override abort itself. Two benefits:
 * newlib's malloc/signal never get linked in (on a 520 KB device we do not
 * want to resurrect the heap by accident), and a failure becomes a panic
 * printed to the serial console instead of a silent lockup.
 */
extern "C" __attribute__((noreturn)) void abort(void) {
    panic("TFLM abort()");
}

namespace tflite {

/* Microseconds. time_us_32() wraps after about 71 minutes at 32 bits, which
 * is more than enough for per-layer measurement: a single inference is on the
 * order of milliseconds. */
uint32_t ticks_per_second() { return 1000000; }

uint32_t GetCurrentTimeTicks() { return time_us_32(); }

}  // namespace tflite
