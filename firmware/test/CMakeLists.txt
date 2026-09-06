# Host tarafı DSP testleri.
#
# NEDEN AYRI PROJE: ana CMakeLists ARM çapraz derleyicisini zorluyor. Bu
# testler PC'de, host derleyicisiyle çalışır — mikrodenetleyiciye gitmeden
# saniyeler içinde sonuç verir. Mikrodenetleyicide DSP hatası ayıklamak çok
# pahalı (plan §11); testler o maliyeti baştan siler.
#
#   cmake -S test -B test/build -G Ninja
#   cmake --build test/build
#   ./test/build/dsp_test
cmake_minimum_required(VERSION 3.13)
project(pokebird_dsp_test C)

set(CMAKE_C_STANDARD 11)

add_executable(dsp_test
    dsp_test.c
    ../src/dsp/fft.c
    ../src/dsp/mel.c
    ../src/dsp/gate.c
    # Karar kurali (ai/decision.c) donanimsiz: histerezis ve tutma sureleri
    # ancak zaman ilerletilerek sinanabilir, kartta bunu yapmak pahali.
    ../src/ai/decision.c
    # Türkçe büyütme donanımsız: `i`->`İ` ve `ı`->`I` bayt uzunluğunu
    # değiştiriyor, tampon sınırı gerçek bir risk ve hatayı ekranda ancak
    # Türkçe okuyan biri yakalar.
    ../src/ui/text.c
)

target_include_directories(dsp_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)

# M_PI POSIX'ten geliyor; MSVC ve bazı libc'lerde açıkça istemek gerekiyor.
target_compile_definitions(dsp_test PRIVATE _USE_MATH_DEFINES)

find_library(M_LIB m)
if(M_LIB)
    target_link_libraries(dsp_test PRIVATE ${M_LIB})
endif()

target_compile_options(dsp_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
