# CMake toolchain file: big-endian MIPS, tuned for the N64's NEC VR4300 CPU.
#
# This uses the stock Debian/Ubuntu "mips-linux-gnu" cross compiler, which
# targets a hosted Linux/glibc environment.  It is a stepping stone: it tells us
# whether SuperTuxKart's code compiles and links for the N64's instruction set
# and byte order.  A real N64 build needs a bare-metal toolchain (libdragon's
# mips64-elf + newlib) instead; see n64/README.md.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)

set(CMAKE_C_COMPILER   mips-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER mips-linux-gnu-g++)

# -march=vr4300 restricts code generation to the MIPS III subset the VR4300
# implements; -mfix4300 works around the VR4300 multiply errata (same as
# libdragon); -EB = big-endian, like the N64.
set(N64_CPU_FLAGS "-march=vr4300 -mtune=vr4300 -mfix4300 -EB")
set(CMAKE_C_FLAGS_INIT   "${N64_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${N64_CPU_FLAGS}")

# The distro's glibc/crt objects are built for MIPS32r2, which is not a
# superset of MIPS III, so ld refuses to merge them with VR4300 objects.  This
# is a host-side test link only (run under qemu-mips), so allow the mismatch.
# The bare-metal libdragon toolchain has no such problem.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--no-warn-mismatch")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--no-warn-mismatch")

if(DEFINED ENV{MIPS_DEPS_PREFIX})
    set(CMAKE_FIND_ROOT_PATH "$ENV{MIPS_DEPS_PREFIX}" /usr/mips-linux-gnu)
    # Some dependency CMake configs reference libraries by bare name (-lz).
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -L$ENV{MIPS_DEPS_PREFIX}/lib")
else()
    set(CMAKE_FIND_ROOT_PATH /usr/mips-linux-gnu)
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
