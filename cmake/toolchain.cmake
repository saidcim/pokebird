# toolchain.cmake — sets the SDK and compiler paths from one place.
#
# The goal is for "cmake -B build -G Ninja" to work without the user setting
# any environment variables. If a value is already defined, the environment's
# value is left alone.

# ── Pico SDK ──────────────────────────────────────────────────────────────
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(_pb_vendored_sdk "${CMAKE_CURRENT_LIST_DIR}/../third_party/pico-sdk")
    if(EXISTS "${_pb_vendored_sdk}/pico_sdk_init.cmake")
        get_filename_component(PICO_SDK_PATH "${_pb_vendored_sdk}" ABSOLUTE)
        set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Pico SDK path")
        message(STATUS "Using the vendored Pico SDK: ${PICO_SDK_PATH}")
    endif()
endif()

# ── ARM GCC ───────────────────────────────────────────────────────────────
# The RP2350's Cortex-M33 needs GCC 10 or newer. PlatformIO's
# toolchain-rp2040-earlephilhower package contains GCC 14.3 and is usually
# already installed, so there is no need to download a separate ARM
# toolchain.
if(NOT DEFINED ENV{PICO_TOOLCHAIN_PATH} AND NOT DEFINED PICO_TOOLCHAIN_PATH)
    set(_pb_toolchain_candidates
        "$ENV{USERPROFILE}/.platformio/packages/toolchain-rp2040-earlephilhower"
        "$ENV{HOME}/.platformio/packages/toolchain-rp2040-earlephilhower"
        "$ENV{USERPROFILE}/.pico-sdk/toolchain/14_2_Rel1"
    )
    foreach(_cand ${_pb_toolchain_candidates})
        if(EXISTS "${_cand}/bin/arm-none-eabi-gcc.exe" OR EXISTS "${_cand}/bin/arm-none-eabi-gcc")
            get_filename_component(PICO_TOOLCHAIN_PATH "${_cand}" ABSOLUTE)
            set(PICO_TOOLCHAIN_PATH "${PICO_TOOLCHAIN_PATH}" CACHE PATH "ARM GCC path")
            message(STATUS "ARM toolchain: ${PICO_TOOLCHAIN_PATH}")
            break()
        endif()
    endforeach()
    if(NOT DEFINED PICO_TOOLCHAIN_PATH)
        message(WARNING
            "arm-none-eabi-gcc not found. Make sure it is on PATH, or pass "
            "its location with -DPICO_TOOLCHAIN_PATH=...")
    endif()
endif()
