#!/usr/bin/env bash
# libstdc++ compiled with -mlong-calls, for code paging (n64/compat/src/pager.c):
# paged code runs at 0xC0800000 and calls the resident C library at
# 0x80000000, beyond a jal's 256 MB reach. Rebuilt from the toolchain's own
# GCC build tree (Pak's tools/build_n64_toolchain.sh), installed into
# <prefix>/lib/longcalls; the installed toolchain is left untouched.
#
#   n64/build_libstdcxx_longcalls.sh <prefix>
set -euo pipefail
PREFIX="$(realpath -m "$1")"
GCC_BUILD="${GCC_BUILD:-/opt/pak-n64/build/build-gcc}"
JOBS="${JOBS:-$(nproc)}"
export PATH="${N64_INST:-/opt/pak-n64}/bin:$PATH"
cd "$GCC_BUILD/mips64-elf"
rm -rf libstdc++-v3-longcalls
cp -a libstdc++-v3 libstdc++-v3-longcalls
cd libstdc++-v3-longcalls
make clean > /dev/null 2>&1 || true
# generated assembly (the ios_failure typeinfo rewrite) survives make clean
find . -name '*.s' -delete
make -j"$JOBS" CXXFLAGS='-g -O2 -mlong-calls' CFLAGS='-g -O2 -mlong-calls' > build.log 2>&1 ||
    { tail -30 build.log; exit 1; }
# every member must call through registers (readelf needs a real file)
for lib in src/.libs/libstdc++.a libsupc++/.libs/libsupc++.a; do
    for m in $(mips64-elf-ar t "$lib"); do
        mips64-elf-ar p "$lib" "$m" > member.o
        if mips64-elf-readelf -r member.o | grep -q R_MIPS_26; then
            echo "libstdc++: $m has direct calls"; exit 1
        fi
    done
done
mkdir -p "$PREFIX/lib/longcalls"
cp src/.libs/libstdc++.a libsupc++/.libs/libsupc++.a "$PREFIX/lib/longcalls/"
echo "libstdc++ (long calls) -> $PREFIX/lib/longcalls"
