# tflm.cmake — TensorFlow Lite for Microcontrollers + CMSIS-NN
#
# TFLM'in kendi derleme sistemi (tensorflow/lite/micro/tools/make/Makefile)
# Windows'ta ÇALIŞMIYOR: wget/unzip/md5sum bekliyor ve hedef listesi POSIX
# kabuğuna bağlı. Bu yüzden kaynak listesi buraya taşındı. Liste uydurulmadı,
# TFLM'in kendi `tools/make/sources.inc` dosyasından alındı — oradaki
# MICROLITE_CC_BASE_SRCS + TFL_CC_SRCS + MICROLITE_CC_KERNEL_SRCS'in aynısı.
#
# Sürüm sabitlemeleri de TFLM'in kendi indirme betiklerinden geliyor
# (tools/make/third_party_downloads.inc ve ext_libs/cmsis_nn_download.sh):
#
#   tflite-micro  330b1747c9d51c0e394f51a2a34ff42deb9b95f5  (29 Tem 2026)
#   flatbuffers   v25.9.23  + tools/make/flatbuffers.patch   <- YAMA ŞART
#   gemmlowp      719139ce755a0f31cbf1c37f7f98adcc7fc9f425   (yalnızca başlık)
#   ruy           d37128311b445e758136b8602d1bbd2a755e115d   (yalnızca başlık)
#   CMSIS-NN      4ab83cc3cc98fb85ed6dafb55e8ca02f1628dcae
#
# Kurulum komutları README'de; hiçbiri git'e girmiyor (.gitignore).

set(PB_TFLM_DIR   ${CMAKE_CURRENT_LIST_DIR}/../third_party/tflite-micro)
set(PB_FB_DIR     ${CMAKE_CURRENT_LIST_DIR}/../third_party/flatbuffers)
set(PB_GEMM_DIR   ${CMAKE_CURRENT_LIST_DIR}/../third_party/gemmlowp)
set(PB_RUY_DIR    ${CMAKE_CURRENT_LIST_DIR}/../third_party/ruy)
set(PB_CMSISNN_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/cmsis-nn)

foreach(_d ${PB_TFLM_DIR} ${PB_FB_DIR} ${PB_GEMM_DIR} ${PB_RUY_DIR} ${PB_CMSISNN_DIR})
    if(NOT EXISTS ${_d})
        message(FATAL_ERROR "TFLM bağımlılığı eksik: ${_d}\n"
                            "Kurulum adımları README.md'de (M6 bölümü).")
    endif()
endforeach()

# flatbuffers yaması uygulanmadıysa derleme ilerler ama TFLM dinamik ayırma
# yapan bir kod yoluna girer. Sessiz kalmasın diye burada yakalanıyor.
file(READ ${PB_FB_DIR}/include/flatbuffers/base.h _pb_fb_base LIMIT 800)
if(NOT _pb_fb_base MATCHES "FLATBUFFERS_LOCALE_INDEPENDENT 0")
    message(FATAL_ERROR
        "flatbuffers yaması uygulanmamış. Şunu çalıştırın:\n"
        "  cd third_party/flatbuffers && patch -p1 < "
        "../tflite-micro/tensorflow/lite/micro/tools/make/flatbuffers.patch")
endif()

# ── Kaynak listesi ────────────────────────────────────────────────────────
# sources.inc'teki MICROLITE_CC_BASE_SRCS
file(GLOB _tflm_base
    ${PB_TFLM_DIR}/tensorflow/lite/micro/*.cc
    ${PB_TFLM_DIR}/tensorflow/lite/micro/arena_allocator/*.cc
    ${PB_TFLM_DIR}/tensorflow/lite/micro/memory_planner/*.cc
    ${PB_TFLM_DIR}/tensorflow/lite/micro/tflite_bridge/*.cc
)

# sources.inc'teki TFL_CC_SRCS (tensorflow/ altında lite/micro ve
# lite/experimental hariç her .cc). array.cc, TF_LITE_STATIC_MEMORY
# derlemesinde bilerek dışarıda — sources.inc de öyle yapıyor.
list(APPEND _tflm_base
    ${PB_TFLM_DIR}/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc
    ${PB_TFLM_DIR}/tensorflow/compiler/mlir/lite/schema/schema_utils.cc
    ${PB_TFLM_DIR}/tensorflow/lite/core/api/flatbuffer_conversions.cc
    ${PB_TFLM_DIR}/tensorflow/lite/core/api/tensor_utils.cc
    ${PB_TFLM_DIR}/tensorflow/lite/core/c/common.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/common.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/portable_tensor_utils.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/quantization_util.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/reference/portable_tensor_utils.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/runtime_shape.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/tensor_ctypes.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/internal/tensor_utils.cc
    ${PB_TFLM_DIR}/tensorflow/lite/kernels/kernel_util.cc
)

# Çekirdekler. Hepsi derleniyor, kullanılmayanları LİNKER atıyor
# (-ffunction-sections/-fdata-sections + --gc-sections zaten açık).
# Modelimiz yalnızca CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED / MEAN
# kullanıyor ama listeyi budamak ileride Aşama-1 ağı geldiğinde sessiz bir
# "op not found" hatasına dönüşür; bedeli sadece derleme süresi.
file(GLOB _tflm_kernels ${PB_TFLM_DIR}/tensorflow/lite/micro/kernels/*.cc)
file(GLOB _tflm_kernels_cmsis ${PB_TFLM_DIR}/tensorflow/lite/micro/kernels/cmsis_nn/*.cc)

# CMSIS-NN sürümü olan çekirdeğin referans sürümü listeden çıkarılmalı;
# TFLM'in Makefile'ında bu işi specialize_files.py yapıyor. Aynı sembolü iki
# kez tanımlamak linker hatası verir — sessiz bir "yavaş çekirdek" durumu
# DEĞİL, doğrudan patlar. İyi de olur.
foreach(_opt ${_tflm_kernels_cmsis})
    get_filename_component(_n ${_opt} NAME)
    list(REMOVE_ITEM _tflm_kernels ${PB_TFLM_DIR}/tensorflow/lite/micro/kernels/${_n})
endforeach()
list(APPEND _tflm_kernels ${_tflm_kernels_cmsis})

set(_tflm_srcs ${_tflm_base} ${_tflm_kernels})

# Testler ve test iskelesi. kernel_runner/fake_micro_context/mock_micro_graph
# yalnızca TFLM'in kendi çekirdek testleri için var; test_helpers.cc ise
# çalışma zamanında flatbuffer ÜRETİYOR (dinamik ayırma).
list(FILTER _tflm_srcs EXCLUDE REGEX "_test\\.cc$")
list(FILTER _tflm_srcs EXCLUDE REGEX
     "/(test_helpers|test_helper_custom_ops|kernel_runner|fake_micro_context|mock_micro_graph)\\.cc$")

# debug_log.cc ve micro_time.cc'nin yerine kendi uyarlamamız geçiyor
# (src/ai/tflm_port.cc). Referans debug_log.cc vfprintf(stderr,...) çağırıyor;
# newlib'in stdio kilit makinesini çeker ve Pico SDK'nın minimal printf'i
# onları sağlamıyor (bkz. lastsession.md §5.4).
list(FILTER _tflm_srcs EXCLUDE REGEX "/(debug_log|micro_time)\\.cc$")

# CMSIS-NN: float çekirdekleri kapalı (ARM_NN_ENABLE_F16/F32 = 0), o yüzden
# kaynakları da derlenmiyor — cmsis_nn.inc'teki filtrenin aynısı.
file(GLOB_RECURSE _cmsisnn_srcs ${PB_CMSISNN_DIR}/Source/*.c)
list(FILTER _cmsisnn_srcs EXCLUDE REGEX "_f16\\.c$")
list(FILTER _cmsisnn_srcs EXCLUDE REGEX "_f32\\.c$")
list(FILTER _cmsisnn_srcs EXCLUDE REGEX "arm_nntables_flt\\.c$")

add_library(tflm STATIC ${_tflm_srcs} ${_cmsisnn_srcs})

target_include_directories(tflm SYSTEM PUBLIC
    ${PB_TFLM_DIR}
    ${PB_FB_DIR}/include
    ${PB_GEMM_DIR}
    ${PB_RUY_DIR}
    ${PB_CMSISNN_DIR}
    ${PB_CMSISNN_DIR}/Include
)

target_compile_definitions(tflm PUBLIC
    TF_LITE_STATIC_MEMORY          # tüm mikrodenetleyici derlemelerinde şart
    TF_LITE_DISABLE_X86_NEON
    ARM_NN_ENABLE_F16=0
    ARM_NN_ENABLE_F32=0
    # CMSIS_NN olmadan kernels/conv.h gibi başlıklar Register_CONV_2D_INT8()'i
    # `inline` olarak KENDİSİ tanımlıyor ve cmsis_nn/conv.cc'nin gerçek
    # tanımıyla çakışıyor. PUBLIC olmak zorunda: bu başlıkları
    # micro_mutable_op_resolver.h üzerinden src/ai/tur_agi.cc de görüyor,
    # tanımlar uyuşmazsa ODR ihlali olur ve çakışma sessiz kalır.
    CMSIS_NN
)

target_compile_options(tflm PRIVATE
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -ffunction-sections
    -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    # Vendor kodu: kendi uyarılarımız burada gürültüden başka bir şey değil.
    -w
)

# Pico SDK'nın derleyici bayraklarını (mcpu, float-abi, sysroot) miras al.
# pico_stdlib INTERFACE hedefi; yalnızca başlık/derleme bayrağı taşıyor,
# gerçek bağlama üst hedefte yapılıyor.
target_link_libraries(tflm PUBLIC pico_base_headers hardware_structs)
