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

# fpu_emu.c writes $f20-$f31 directly; it must not use them itself
set_source_files_properties(src/fpu_emu.c PROPERTIES COMPILE_FLAGS
    "-DSTK_N64_FPU_EMU_FIXED_REGS -ffixed-$f20 -ffixed-$f21 -ffixed-$f22 -ffixed-$f23 -ffixed-$f24 -ffixed-$f25 -ffixed-$f26 -ffixed-$f27 -ffixed-$f28 -ffixed-$f29 -ffixed-$f30 -ffixed-$f31")
add_library(stk_n64_compat STATIC
    src/gthr_libdragon.c
    src/fpu_emu.c
    src/clock_libdragon.cpp
    src/net_stub.c
    src/ramfs.c
    src/posix_extra.c
    src/romfs_index.c
    ${LIBSTDCXX_THREAD_SOURCES})
add_library(stk_n64_curl STATIC src/curl_stub.c)
add_library(stk_n64_mbedcrypto STATIC src/mbedtls_stub.c)
if(NOT SERVER_ONLY)
    add_library(stk_n64_sdl STATIC src/sdl_n64.c)
    target_include_directories(stk_n64_sdl PRIVATE ${N64_DEPS_PREFIX}/include/SDL2)
    target_link_libraries(stk_n64_sdl stk_n64_compat)
endif()

option(STK_N64_CONSOLE "Show the log on an on-screen console. Costs 600 KB of heap (two 32-bit framebuffers); without it the log goes to the debug channel (ISViewer / USB) only" OFF)
if(STK_N64_CONSOLE)
    add_compile_definitions(STK_N64_CONSOLE=1)
endif()
option(STK_N64_LOG_HEAP "Prefix every log line with the heap in use" OFF)
if(STK_N64_LOG_HEAP)
    add_compile_definitions(STK_N64_LOG_HEAP=1)
endif()
option(STK_N64_SCREEN_LOG "Show the log on screen from static initialisation on" OFF)
if(STK_N64_SCREEN_LOG)
    add_compile_definitions(STK_N64_SCREEN_LOG=1)
endif()

# --wrap=stat (romfs_index.c) is invisible to LTO's symbol resolution
set_source_files_properties(src/romfs_index.c PROPERTIES COMPILE_OPTIONS "-fno-lto")
if(STK_N64_VERIFY_ROMFS_INDEX)
    set_property(SOURCE src/romfs_index.c APPEND PROPERTY
        COMPILE_DEFINITIONS STK_N64_VERIFY_ROMFS_INDEX=1)
endif()
