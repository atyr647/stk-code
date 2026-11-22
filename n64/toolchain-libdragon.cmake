# CMake toolchain file for the real N64 target: libdragon (preview) on the
# mips64-elf toolchain from atyr647/Pak tools/build_n64_toolchain.sh.
#
#   cmake -S . -B build-n64 -DCMAKE_TOOLCHAIN_FILE=n64/toolchain-libdragon.cmake
#
# Flags are libdragon's own (include/n64.mk, N64_C_AND_CXX_FLAGS) minus its
# -Werror set, which is libdragon's policy for libdragon code, not STK's.
if(NOT DEFINED ENV{N64_INST} AND NOT N64_INST)
    set(N64_INST /opt/pak-n64)
elseif(NOT N64_INST)
    set(N64_INST $ENV{N64_INST})
endif()
set(N64_INST "${N64_INST}" CACHE PATH "libdragon install prefix")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips64)
set(N64 TRUE)

set(CMAKE_C_COMPILER   ${N64_INST}/bin/mips64-elf-gcc)
set(CMAKE_CXX_COMPILER ${N64_INST}/bin/mips64-elf-g++)
set(CMAKE_ASM_COMPILER ${N64_INST}/bin/mips64-elf-gcc)
set(CMAKE_AR           ${N64_INST}/bin/mips64-elf-ar)
set(CMAKE_RANLIB       ${N64_INST}/bin/mips64-elf-ranlib)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(N64_INCLUDEDIR ${N64_INST}/mips64-elf/include)
set(N64_LIBDIR     ${N64_INST}/mips64-elf/lib)

set(N64_FLAGS "-march=vr4300 -mtune=vr4300 -mabi=o64")
string(APPEND N64_FLAGS " -I${N64_INCLUDEDIR}/newlib_overrides -I${N64_INCLUDEDIR} -include ktls.h")
string(APPEND N64_FLAGS " -falign-functions=32 -ffunction-sections -fdata-sections")
string(APPEND N64_FLAGS " -ffast-math -ftrapping-math -fno-associative-math")
string(APPEND N64_FLAGS " -DN64 -DLIBDRAGON_PREVIEW=2 -ftrivial-auto-var-init=pattern")

set(CMAKE_C_FLAGS_INIT   "${N64_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${N64_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${N64_FLAGS}")
# -Oz: GCC's most size-oriented level; on the N64 code is resident in the
# same 8 MB as everything else
# (as cache entries: CMake appends its own "-Os" to *_INIT, and the last -O
# on a command line wins)
set(CMAKE_C_FLAGS_MINSIZEREL   "-Oz -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG" CACHE STRING "")

# Linking: what n64.mk's %.elf rule does.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L${N64_LIBDIR} -Wl,-T${N64_LIBDIR}/n64.ld -Wl,--gc-sections -Wl,--wrap,__do_global_ctors")
set(CMAKE_CXX_STANDARD_LIBRARIES "-ldragon -lc -lm -ldragonsys")
set(CMAKE_C_STANDARD_LIBRARIES   "-ldragon -lc -lm -ldragonsys")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(CMAKE_FIND_ROOT_PATH ${N64_INST}/mips64-elf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
