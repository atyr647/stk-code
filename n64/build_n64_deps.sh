#!/usr/bin/env bash
# Third-party libraries STK needs on the N64, built with the libdragon
# toolchain into a prefix:
#
#   n64/build_n64_deps.sh <zlib source dir> <prefix>
#
# Only zlib is real: everything network-facing (curl, mbedtls) is provided by
# the stub libraries in n64/compat, which compile as part of the STK build.
set -euo pipefail
ZSRC="$(realpath "$1")"; PREFIX="$(realpath -m "$2")"
N64_INST="${N64_INST:-/opt/pak-n64}"
CC="$N64_INST/bin/mips64-elf-gcc"; AR="$N64_INST/bin/mips64-elf-ar"
CFLAGS="-march=vr4300 -mtune=vr4300 -mabi=o64 -Os -ffunction-sections -fdata-sections -DN64 -DZ_HAVE_UNISTD_H"
B="$PREFIX/build-zlib"; mkdir -p "$B" "$PREFIX/lib" "$PREFIX/include"
# zlib's CMake renames zconf.h out of the way in-tree; accept either name
ZCONF="$ZSRC/zconf.h"; [ -f "$ZCONF" ] || ZCONF="$ZSRC/zconf.h.included"
cp "$ZCONF" "$B/zconf.h"
for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback \
         inffast inflate inftrees trees uncompr zutil; do
    "$CC" $CFLAGS -I"$B" -c "$ZSRC/$f.c" -o "$B/$f.o"
done
"$AR" rcs "$PREFIX/lib/libz.a" "$B"/*.o
cp "$ZSRC/zlib.h" "$B/zconf.h" "$PREFIX/include/"
# for pkg-config users (freetype2.pc and libpng.pc require zlib)
mkdir -p "$PREFIX/lib/pkgconfig"
cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<PC
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib compression library
Version: 1.3
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
PC
echo "zlib -> $PREFIX"
