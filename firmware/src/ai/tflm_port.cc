/**
 * tflm_port.cc — TFLM'in platformdan beklediği iki fonksiyon
 *
 * TFLM iki şeyi hedefe bırakıyor: günlük çıktısı (DebugLog) ve profilleme
 * saati (micro_time). Referans uyarlamaları CMake tarafında listeden
 * çıkarıldı (cmake/tflm.cmake), yerine bunlar geçiyor.
 *
 * NEDEN kendi DebugLog'umuz: referans sürüm `vfprintf(stderr, ...)` çağırıyor.
 * Bu, newlib'in tüm stdio kilit makinesini bağlamaya çalışıyor
 * (__retarget_lock_acquire_recursive vb.) ve Pico SDK'nın minimal printf'i
 * onları sağlamıyor — lastsession.md §5.4'te aynı tuzağa `fflush(stdout)`
 * yüzünden bir kez düşülmüştü. `vprintf` Pico SDK'nın kendi printf'ine
 * gidiyor ve USB CDC'ye çıkıyor; zaten teşhis çıktısının tamamı oradan akıyor.
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
 * abort() — newlib'inkini BİLEREK eziyoruz.
 *
 * TFLM birkaç yerde abort() çağırıyor (micro_utils.cc, reduce_common.cc,
 * quantization_util.cc — TFLITE_ABORT / TFLITE_DCHECK). newlib'in abort'u
 * raise() → signal() → malloc() zincirini çekiyor; malloc da newlib'in
 * kilit makinesini (__retarget_lock_acquire_recursive,
 * __lock___malloc_recursive_mutex) istiyor ve Pico SDK onları sağlamıyor.
 * Belirti bir LİNK hatası olarak çıkıyor — lastsession.md §5.4'teki
 * `fflush(stdout)` tuzağının aynısı, sadece başka bir kapıdan.
 *
 * Kilit saplamaları yazmak yerine abort'un kendisini eziyoruz. İki kazanç:
 * newlib malloc/signal hiç bağlanmıyor (520 KB'lik bir cihazda heap'i kazara
 * canlandırmak istemiyoruz) ve hata sessiz bir kilitlenme yerine seri porta
 * yazılmış bir panic oluyor.
 */
extern "C" __attribute__((noreturn)) void abort(void) {
    panic("TFLM abort()");
}

namespace tflite {

/* Mikrosaniye. time_us_32() 32 bitte ~71 dakikada sarıyor; katman başına
 * ölçüm için fazlasıyla yeterli, tek çıkarım milisaniye mertebesinde. */
uint32_t ticks_per_second() { return 1000000; }

uint32_t GetCurrentTimeTicks() { return time_us_32(); }

}  // namespace tflite
