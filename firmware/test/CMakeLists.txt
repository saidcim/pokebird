# Host-side DSP tests.
#
# WHY A SEPARATE PROJECT: the main CMakeLists forces the ARM cross compiler.
# These tests run on the PC with the host compiler and give an answer in
# seconds without going near the microcontroller. Debugging a DSP fault on the
# microcontroller is very expensive; these tests remove that cost up front.
#
#   cmake -S firmware/test -B firmware/test/build -G Ninja
#   cmake --build firmware/test/build
#   ./firmware/test/build/dsp_test
cmake_minimum_required(VERSION 3.13)
project(pokebird_dsp_test C)

set(CMAKE_C_STANDARD 11)

add_executable(dsp_test
    dsp_test.c
    ../src/dsp/fft.c
    ../src/dsp/mel.c
    ../src/dsp/gate.c
    # The decision rule (ai/decision.c) is hardware-free: hysteresis and hold
    # times can only be tested by advancing time, which is expensive on the
    # board.
    ../src/ai/decision.c
    # Uppercasing is hardware-free too, and it must not cut a UTF-8 sequence
    # in half at the buffer edge - a real risk that is hard to spot on
    # screen.
    ../src/ui/text.c
)

target_include_directories(dsp_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)

# M_PI comes from POSIX; MSVC and some libcs need it requested explicitly.
target_compile_definitions(dsp_test PRIVATE _USE_MATH_DEFINES)

find_library(M_LIB m)
if(M_LIB)
    target_link_libraries(dsp_test PRIVATE ${M_LIB})
endif()

target_compile_options(dsp_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
