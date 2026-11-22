# Platform layer for the N64 build (see n64/stk_n64.cmake).
set(GCC_LIBSTDCXX_SRC "${N64_INST}/build/gcc-16.2.0/libstdc++-v3" CACHE PATH
    "libstdc++ sources (left in place by atyr647/Pak tools/build_n64_toolchain.sh)")

# libstdc++'s thread support, built from GCC's own sources against
# include/bits/gthr-default.h (the prebuilt libstdc++.a has none).
set(LIBSTDCXX_THREAD_SOURCES
    ${GCC_LIBSTDCXX_SRC}/src/c++11/thread.cc
    ${GCC_LIBSTDCXX_SRC}/src/c++11/condition_variable.cc
    ${GCC_LIBSTDCXX_SRC}/src/c++11/mutex.cc)
foreach(f ${LIBSTDCXX_THREAD_SOURCES})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "N64: ${f} not found; set GCC_LIBSTDCXX_SRC")
    endif()
endforeach()
set_source_files_properties(${LIBSTDCXX_THREAD_SOURCES} PROPERTIES
    COMPILE_FLAGS "-std=gnu++17 -I${GCC_LIBSTDCXX_SRC}/libsupc++ -w")

add_library(stk_n64_compat STATIC
    src/gthr_libdragon.c
    src/clock_libdragon.cpp
    src/net_stub.c
    src/ramfs.c
    src/posix_extra.c
    ${LIBSTDCXX_THREAD_SOURCES})
add_library(stk_n64_curl STATIC src/curl_stub.c)
add_library(stk_n64_mbedcrypto STATIC src/mbedtls_stub.c)

option(STK_N64_SCREEN_LOG "Show the log on screen from static initialisation on" OFF)
if(STK_N64_SCREEN_LOG)
    add_compile_definitions(STK_N64_SCREEN_LOG=1)
endif()
