# picotool.cmake — finds a working picotool and points the SDK at it.
#
# WHY THIS IS NEEDED:
# If it cannot find a suitable version on the system, the Pico SDK builds
# picotool from source. On this machine the only usable host compiler is
# winlibs GCC 16.1, and the picotool 2.3.0 it produces segfaults on EVERY
# command that reads a file ("Access violation") — both `uf2 convert` and
# `coprodis`. picotool's own `version` command works, so the problem is in the
# ELF/file handling path.
#
# THE FIX:
# Use the pre-built picotool that ships with PlatformIO. Tested: `uf2 convert`
# and `coprodis` both work.
#
# The SDK's Findpicotool.cmake begins with `if (NOT TARGET picotool)`, so
# defining our own imported target before pico_sdk_init() skips the
# download/build step entirely.
#
# This file is a convenience layer: if no working picotool is found it backs
# out quietly and the SDK falls back to its usual behaviour.

if(NOT TARGET picotool)
    set(_pb_picotool_candidates
        "$ENV{USERPROFILE}/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool.exe"
        "$ENV{HOME}/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool"
        "$ENV{USERPROFILE}/.pico-sdk/picotool/2.3.0/picotool/picotool.exe"
    )

    foreach(_cand ${_pb_picotool_candidates})
        if(EXISTS "${_cand}")
            # Existing is not enough — verify that it actually runs.
            execute_process(
                COMMAND "${_cand}" version
                RESULT_VARIABLE _pb_rc
                OUTPUT_VARIABLE _pb_out
                ERROR_QUIET
            )
            if(_pb_rc EQUAL 0)
                add_executable(picotool IMPORTED GLOBAL)
                set_property(TARGET picotool PROPERTY IMPORTED_LOCATION "${_cand}")
                string(STRIP "${_pb_out}" _pb_out)
                message(STATUS "picotool (prebuilt): ${_cand} - ${_pb_out}")
                break()
            endif()
        endif()
    endforeach()

    if(NOT TARGET picotool)
        message(STATUS "No prebuilt picotool found; the SDK will build one from "
                       "source. If the build fails with an 'Access violation', "
                       "see cmake/picotool.cmake.")
    endif()
endif()
