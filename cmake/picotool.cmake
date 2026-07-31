# picotool.cmake — çalışan bir picotool bulup SDK'ya tanıtır.
#
# NEDEN GEREKLİ:
# Pico SDK, sistemde uygun sürüm bulamazsa picotool'u kaynaktan derliyor.
# Bu makinede kullanılabilir tek host derleyicisi winlibs GCC 16.1; onunla
# derlenen picotool 2.3.0, dosya okuyan HER komutta segfault ediyor
# ("Access violation"): hem `uf2 convert` hem `coprodis`. picotool'un kendi
# `version` komutu çalışıyor, yani sorun ELF/dosya işleme yolunda.
#
# ÇÖZÜM:
# PlatformIO ile birlikte gelen, önceden derlenmiş picotool'u kullan. Test
# edildi: `uf2 convert` ve `coprodis` sorunsuz çalışıyor.
#
# SDK'nın Findpicotool.cmake dosyası `if (NOT TARGET picotool)` ile başlıyor;
# bu yüzden pico_sdk_init() öncesinde kendi imported hedefimizi tanımlamak,
# indirme/derleme adımını tamamen atlatıyor.
#
# Bu dosya bir kolaylık katmanı: çalışan picotool bulamazsa sessizce çekilir
# ve SDK her zamanki davranışına döner.

if(NOT TARGET picotool)
    set(_pb_picotool_candidates
        "$ENV{USERPROFILE}/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool.exe"
        "$ENV{HOME}/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool"
        "$ENV{USERPROFILE}/.pico-sdk/picotool/2.3.0/picotool/picotool.exe"
    )

    foreach(_cand ${_pb_picotool_candidates})
        if(EXISTS "${_cand}")
            # Sadece var olması yetmez — gerçekten çalıştığını doğrula.
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
                message(STATUS "picotool (hazir): ${_cand} — ${_pb_out}")
                break()
            endif()
        endif()
    endforeach()

    if(NOT TARGET picotool)
        message(STATUS "Hazir picotool bulunamadi; SDK kaynaktan derleyecek. "
                       "Derleme 'Access violation' ile duserse cmake/picotool.cmake'e bakin.")
    endif()
endif()
