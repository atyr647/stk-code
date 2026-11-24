#!/usr/bin/env bash
# Libraries the STK client (graphics, GUI text, sound) needs on the N64,
# built with the libdragon toolchain into the prefix n64/build_n64_deps.sh
# made (zlib):
#
#   n64/build_n64_client_deps.sh <source dir> <prefix>
#
# <source dir> holds one checkout per library (the same versions
# n64/build_mips_deps.sh uses): libpng jpeg freetype harfbuzz ogg vorbis
# libsamplerate.
set -euo pipefail
SRC="$(realpath "$1")"; PREFIX="$(realpath -m "$2")"
TC="$(realpath "$(dirname "$0")")/toolchain-libdragon.cmake"
JOBS="${JOBS:-$(nproc)}"
export N64_DEPS_PREFIX="$PREFIX"
# TARGET=<name>: build only that target (and install by hand, see jpeg)
build() {
    local name="$1"; shift
    echo "=== $name ==="
    rm -rf "$SRC/n64build-$name"
    cmake -S "$SRC/$name" -B "$SRC/n64build-$name" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_POLICY_DEFAULT_CMP0074=NEW -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF "$@" \
        > "$SRC/n64build-$name.log" 2>&1 || { tail -30 "$SRC/n64build-$name.log"; exit 1; }
    cmake --build "$SRC/n64build-$name" -j"$JOBS" ${TARGET:+--target "$TARGET"} \
        >> "$SRC/n64build-$name.log" 2>&1 ||
        { grep -E 'error|Error' "$SRC/n64build-$name.log" | head -20; exit 1; }
    [ -n "${TARGET:-}" ] ||
        cmake --install "$SRC/n64build-$name" >> "$SRC/n64build-$name.log" 2>&1
}
build libpng   -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
               -DPNG_HARDWARE_OPTIMIZATIONS=OFF -DZLIB_ROOT="$PREFIX"
# libpng names its static library liblibpng16_static.a for this target;
# pkg-config users link -lpng16
ln -sf liblibpng16_static.a "$PREFIX/lib/libpng16.a"
ln -sf liblibpng16_static.a "$PREFIX/lib/libpng.a"
# libjpeg-turbo always builds its command-line tools, which cannot link
# without an OS: the library alone, installed by hand
TARGET=jpeg-static build jpeg -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
               -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF
cp "$SRC/n64build-jpeg/libjpeg.a" "$PREFIX/lib/"
cp "$SRC/jpeg/jpeglib.h" "$SRC/jpeg/jmorecfg.h" "$SRC/jpeg/jerror.h" \
   "$SRC/n64build-jpeg/jconfig.h" "$PREFIX/include/"
build freetype -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_BROTLI=ON \
               -DFT_DISABLE_HARFBUZZ=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON
# newlib declares pthreads and mmap but has neither: harfbuzz without
# threading primitives (STK shapes text on one thread) and without mmap
build harfbuzz -DHB_HAVE_FREETYPE=ON -DHB_BUILD_SUBSET=OFF -DHB_HAVE_GLIB=OFF \
               -DHB_HAVE_ICU=OFF -DHB_HAVE_GOBJECT=OFF -DHB_BUILD_UTILS=OFF \
               -DCMAKE_DISABLE_FIND_PACKAGE_Threads=ON \
               "-DCMAKE_CXX_FLAGS=-DHB_NO_MT -DHB_NO_MMAP"
build ogg      -DINSTALL_DOCS=OFF
build vorbis
build libsamplerate -DLIBSAMPLERATE_EXAMPLES=OFF
echo "client libraries -> $PREFIX"
# SDL2: headers only. STK, Irrlicht's SDL device and MojoAL are compiled
# against the real SDL2 API; the functions they use are implemented on
# libdragon in n64/compat (sdl_n64.c). SDL picks SDL_config_minimal.h for an
# unknown platform.
mkdir -p "$PREFIX/include/SDL2"
cp "$SRC"/SDL2/include/*.h "$PREFIX/include/SDL2/"
echo "SDL2 headers -> $PREFIX/include/SDL2"
