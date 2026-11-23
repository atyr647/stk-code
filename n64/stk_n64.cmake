# Included by CMakeLists.txt when n64/toolchain-libdragon.cmake is in use.
#
# Everything STK looks for on a PC is satisfied here with an N64 equivalent,
# so the main build files need no N64-specific branches:
#   zlib          -> real, built by n64/build_n64_deps.sh
#   pthread       -> n64/compat: libstdc++ threads on libdragon's kernel
#   curl, mbedtls -> n64/compat stubs (no network hardware; see stk_n64_net.h)
set(STK_N64_DIR ${PROJECT_SOURCE_DIR}/n64)
set(N64_DEPS_PREFIX "/opt/n64deps" CACHE PATH "prefix from n64/build_n64_deps.sh")

# Before every target, so lib/* see it too: our <bits/gthr-default.h> and the
# socket headers must win over the toolchain's.
include_directories(BEFORE ${STK_N64_DIR}/compat/include)
add_definitions(-D_GLIBCXX_HAS_GTHREADS=1)
# smaller preallocated Bullet pools (lib/bullet: btDefaultCollisionConfiguration,
# btAxisSweep3) -- 4.5 MB of manifold pool alone otherwise
add_definitions(-DBULLET_SMALL_MEMORY)
# STK's own memory-for-nothing trades (src/physics/triangle_mesh.*)
add_definitions(-DSTK_LOW_MEMORY)
# lib/mcpp (track-script preprocessor): its buffers are sized from NBUFF --
# 64 KB by default, i.e. 320 KB of static data plus 256 KB stack frames. STK's
# scripts are small files with short lines; mcpp documents 0x1000/0x4000 as
# valid sizes.
add_definitions(-DNBUFF=0x2000)
# lib/irrlicht: every mesh scene node keeps its own copy of each SMaterial,
# and each copy carries 8 texture layers. Only layers 0-1 are ever filled
# (.spm/.b3d materials have at most two textures); the rest is the desktop
# renderers' business. 2 layers: ~110 bytes less per material copy.
add_definitions(-D_IRR_MATERIAL_MAX_TEXTURES_=2)
# Track scripts run from AngelScript bytecode precompiled by a host build
# (n64/tools/export_collision.sh writes tracks/*/n64_script.asb): no script
# compiler (AS_NO_COMPILER halves AngelScript) and no preprocessor (mcpp).
add_definitions(-DSTK_SCRIPT_BYTECODE_ONLY -DAS_NO_COMPILER)
# lib/irrlicht: switch off loaders/writers nothing on the N64 uses
# (IrrCompileConfig.h's own NO_* switches). ZIP stays: addons install from it.
add_definitions(-DNO_IRR_COMPILE_WITH_BMP_LOADER_ -DNO_IRR_COMPILE_WITH_BMP_WRITER_
                -DNO_IRR_COMPILE_WITH_JPG_WRITER_ -DNO_IRR_COMPILE_WITH_PNG_WRITER_
                -DNO__IRR_COMPILE_WITH_TAR_ARCHIVE_LOADER_
                -DNO__IRR_COMPILE_WITH_WAD_ARCHIVE_LOADER_)

add_subdirectory(${STK_N64_DIR}/compat ${CMAKE_BINARY_DIR}/n64compat)

set(ZLIB_INCLUDE_DIR ${N64_DEPS_PREFIX}/include CACHE PATH "" FORCE)
set(ZLIB_LIBRARY ${N64_DEPS_PREFIX}/lib/libz.a CACHE FILEPATH "" FORCE)
set(CURL_INCLUDE_DIR ${STK_N64_DIR}/compat/include CACHE PATH "" FORCE)
set(CURL_LIBRARY stk_n64_curl CACHE STRING "" FORCE)
set(CURL_LIBRARY_RELEASE stk_n64_curl CACHE STRING "" FORCE)
set(MBEDTLS_INCLUDE_DIRS ${STK_N64_DIR}/compat/include CACHE PATH "" FORCE)
set(MBEDCRYPTO_LIBRARY stk_n64_mbedcrypto CACHE STRING "" FORCE)
set(PTHREAD_LIBRARY stk_n64_compat CACHE STRING "" FORCE)
set(USE_CRYPTO_OPENSSL OFF CACHE BOOL "" FORCE)
set(USE_DNS_C ON CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_WIIUSE OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_ENET OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_MCPP OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_ANGELSCRIPT OFF CACHE BOOL "" FORCE)

# lib/enet probes the socket API with try-compiles; answer for n64/compat,
# which provides all of these (as clean failures where they touch a network).
foreach(v HAS_FCNTL HAS_POLL HAS_GETADDRINFO HAS_GETNAMEINFO HAS_INET_PTON
          HAS_INET_NTOP HAS_MSGHDR_FLAGS HAS_SOCKLEN_T)
    set(${v} 1 CACHE INTERNAL "provided by n64/compat")
endforeach()
# check_type_size caches under HAVE_<var>, with <var> holding the size
set(HAVE_HAS_SOCKLEN_T TRUE CACHE INTERNAL "provided by n64/compat")
set(HAS_SOCKLEN_T 4 CACHE INTERNAL "provided by n64/compat")

# FindThreads (used by lib/angelscript) cannot probe a cross target and falls
# back to "-lpthreads". Threads come from n64/compat (linked explicitly), i.e.
# "in libc" as far as CMake is concerned.
set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "threads via n64/compat")
set(THREADS_PREFER_PTHREAD_FLAG OFF)
