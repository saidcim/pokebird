# tflm.cmake — TensorFlow Lite for Microcontrollers + CMSIS-NN
#
# TFLM's own build system (tensorflow/lite/micro/tools/make/Makefile) does NOT
# work on Windows: it expects wget/unzip/md5sum and its target list depends on
# a POSIX shell. So the source list was moved here. The list was not invented;
# it was taken from TFLM's own `tools/make/sources.inc` — the same
# MICROLITE_CC_BASE_SRCS + TFL_CC_SRCS + MICROLITE_CC_KERNEL_SRCS.
#
# The version pins come from TFLM's own download scripts
# (tools/make/third_party_downloads.inc and ext_libs/cmsis_nn_download.sh):
#
#   tflite-micro  330b1747c9d51c0e394f51a2a34ff42deb9b95f5
#   flatbuffers   v25.9.23  + tools/make/flatbuffers.patch  <- PATCH REQUIRED
#   gemmlowp      719139ce755a0f31cbf1c37f7f98adcc7fc9f425  (headers only)
#   ruy           d37128311b445e758136b8602d1bbd2a755e115d  (headers only)
#   CMSIS-NN      4ab83cc3cc98fb85ed6dafb55e8ca02f1628dcae
#
# The setup commands are in the README; none of this is committed
# (.gitignore).

set(PB_TFLM_DIR   ${CMAKE_CURRENT_LIST_DIR}/../third_party/tflite-micro)
set(PB_FB_DIR     ${CMAKE_CURRENT_LIST_DIR}/../third_party/flatbuffers)
set(PB_GEMM_DIR   ${CMAKE_CURRENT_LIST_DIR}/../third_party/gemmlowp)
set(PB_RUY_DIR    ${CMAKE_CURRENT_LIST_DIR}/../third_party/ruy)
set(PB_CMSISNN_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/cmsis-nn)

foreach(_d ${PB_TFLM_DIR} ${PB_FB_DIR} ${PB_GEMM_DIR} ${PB_RUY_DIR} ${PB_CMSISNN_DIR})
    if(NOT EXISTS ${_d})
        message(FATAL_ERROR "Missing TFLM dependency: ${_d}\n"
                            "The setup steps are in README.md.")
    endif()
endforeach()

# Without the flatbuffers patch the build proceeds but TFLM takes a code path
# that allocates dynamically. Caught here so it cannot pass silently.
file(READ ${PB_FB_DIR}/include/flatbuffers/base.h _pb_fb_base LIMIT 800)
if(NOT _pb_fb_base MATCHES "FLATBUFFERS_LOCALE_INDEPENDENT 0")
    message(FATAL_ERROR
        "The flatbuffers patch has not been applied. Run:\n"
        "  cd third_party/flatbuffers && patch -p1 < "
        "../tflite-micro/tensorflow/lite/micro/tools/make/flatbuffers.patch")
endif()

# ── Source list ───────────────────────────────────────────────────────────
# MICROLITE_CC_BASE_SRCS from sources.inc
file(GLOB _tflm_base
    ${PB_TFLM_DIR}/tensorflow/lite/micro/*.cc
    ${PB_TFLM_DIR}/tensorflow/lite/micro/arena_allocator/*.cc
    ${PB_TFLM_DIR}/tensorflow/lite/micro/memory_planner/*.cc
    ${PB_TFLM_DIR}/tensorflow/lite/micro/tflite_bridge/*.cc
)

# TFL_CC_SRCS from sources.inc (every .cc under tensorflow/ except lite/micro
# and lite/experimental). array.cc is deliberately excluded in a
# TF_LITE_STATIC_MEMORY build — sources.inc does the same.
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

# The kernels. All of them are compiled and the LINKER drops the unused ones
# (-ffunction-sections/-fdata-sections plus --gc-sections are already on).
# Our model uses only CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED / MEAN,
# but pruning the list turns into a silent "op not found" the moment another
# network is added; the only cost of keeping it is build time.
file(GLOB _tflm_kernels ${PB_TFLM_DIR}/tensorflow/lite/micro/kernels/*.cc)
file(GLOB _tflm_kernels_cmsis ${PB_TFLM_DIR}/tensorflow/lite/micro/kernels/cmsis_nn/*.cc)

# Where a kernel has a CMSIS-NN version, the reference version must be
# removed from the list; specialize_files.py does this job in TFLM's Makefile.
# Defining the same symbol twice is a linker error — NOT a silent "slow
# kernel" situation but an outright failure, which is preferable.
foreach(_opt ${_tflm_kernels_cmsis})
    get_filename_component(_n ${_opt} NAME)
    list(REMOVE_ITEM _tflm_kernels ${PB_TFLM_DIR}/tensorflow/lite/micro/kernels/${_n})
endforeach()
list(APPEND _tflm_kernels ${_tflm_kernels_cmsis})

set(_tflm_srcs ${_tflm_base} ${_tflm_kernels})

# Tests and test scaffolding. kernel_runner/fake_micro_context/
# mock_micro_graph exist only for TFLM's own kernel tests, and
# test_helpers.cc GENERATES flatbuffers at run time (dynamic allocation).
list(FILTER _tflm_srcs EXCLUDE REGEX "_test\\.cc$")
list(FILTER _tflm_srcs EXCLUDE REGEX
     "/(test_helpers|test_helper_custom_ops|kernel_runner|fake_micro_context|mock_micro_graph)\\.cc$")

# Our own implementations replace debug_log.cc and micro_time.cc
# (firmware/src/ai/tflm_port.cc). The reference debug_log.cc calls
# vfprintf(stderr, ...), which drags in newlib's stdio locking machinery that
# the Pico SDK's minimal printf does not provide.
list(FILTER _tflm_srcs EXCLUDE REGEX "/(debug_log|micro_time)\\.cc$")

# CMSIS-NN: the float kernels are disabled (ARM_NN_ENABLE_F16/F32 = 0), so
# their sources are not compiled either — the same filter as cmsis_nn.inc.
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
    TF_LITE_STATIC_MEMORY          # required in every microcontroller build
    TF_LITE_DISABLE_X86_NEON
    ARM_NN_ENABLE_F16=0
    ARM_NN_ENABLE_F32=0
    # Without CMSIS_NN, headers such as kernels/conv.h define
    # Register_CONV_2D_INT8() themselves as `inline`, clashing with the real
    # definition in cmsis_nn/conv.cc. This must be PUBLIC: firmware/src/ai/
    # species_net.cc sees the same headers through
    # micro_mutable_op_resolver.h, and if the definitions disagree it is an
    # ODR violation and the clash stays silent.
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
    # Vendor code: our own warnings are nothing but noise here.
    -w
)

# Inherit the Pico SDK's compiler flags (mcpu, float-abi, sysroot).
# pico_stdlib is an INTERFACE target carrying only headers and compile flags;
# the real linking happens in the parent target.
target_link_libraries(tflm PUBLIC pico_base_headers hardware_structs)
