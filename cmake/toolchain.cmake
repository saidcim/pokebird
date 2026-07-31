# toolchain.cmake — SDK ve derleyici yollarını tek yerden ayarlar.
#
# Amaç: "cmake -B build -G Ninja" komutunun, kullanıcı hiçbir ortam değişkeni
# ayarlamadan çalışması. Zaten tanımlıysa ortamdaki değere dokunulmaz.

# ── Pico SDK ──────────────────────────────────────────────────────────────
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(_pb_vendored_sdk "${CMAKE_CURRENT_LIST_DIR}/../third_party/pico-sdk")
    if(EXISTS "${_pb_vendored_sdk}/pico_sdk_init.cmake")
        get_filename_component(PICO_SDK_PATH "${_pb_vendored_sdk}" ABSOLUTE)
        set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Pico SDK yolu")
        message(STATUS "Vendor edilmiş Pico SDK kullanılıyor: ${PICO_SDK_PATH}")
    endif()
endif()

# ── ARM GCC ───────────────────────────────────────────────────────────────
# RP2350'nin Cortex-M33'ü için GCC 10+ gerekiyor. PlatformIO'nun
# toolchain-rp2040-earlephilhower paketi GCC 14.3 içeriyor ve zaten kurulu;
# ayrıca bir ARM toolchain indirmeye gerek yok.
if(NOT DEFINED ENV{PICO_TOOLCHAIN_PATH} AND NOT DEFINED PICO_TOOLCHAIN_PATH)
    set(_pb_toolchain_candidates
        "$ENV{USERPROFILE}/.platformio/packages/toolchain-rp2040-earlephilhower"
        "$ENV{HOME}/.platformio/packages/toolchain-rp2040-earlephilhower"
        "$ENV{USERPROFILE}/.pico-sdk/toolchain/14_2_Rel1"
    )
    foreach(_cand ${_pb_toolchain_candidates})
        if(EXISTS "${_cand}/bin/arm-none-eabi-gcc.exe" OR EXISTS "${_cand}/bin/arm-none-eabi-gcc")
            get_filename_component(PICO_TOOLCHAIN_PATH "${_cand}" ABSOLUTE)
            set(PICO_TOOLCHAIN_PATH "${PICO_TOOLCHAIN_PATH}" CACHE PATH "ARM GCC yolu")
            message(STATUS "ARM toolchain: ${PICO_TOOLCHAIN_PATH}")
            break()
        endif()
    endforeach()
    if(NOT DEFINED PICO_TOOLCHAIN_PATH)
        message(WARNING
            "arm-none-eabi-gcc bulunamadı. PATH'te olduğundan emin olun ya da "
            "-DPICO_TOOLCHAIN_PATH=... ile yolunu verin.")
    endif()
endif()
