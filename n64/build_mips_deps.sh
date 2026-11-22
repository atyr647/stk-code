#!/bin/bash
# Cross-compile SuperTuxKart's third-party dependencies as static libraries
# for big-endian MIPS / VR4300, using n64/toolchain-mips-vr4300.cmake.
#
# Usage: n64/build_mips_deps.sh <dir with dependency source checkouts> <prefix>
# Expected source dirs: zlib libpng jpeg freetype harfbuzz ogg vorbis SDL2
#                       mbedtls curl libsamplerate
set -e
SRC="$(realpath "$1")"
PREFIX="$(realpath -m "$2")"
TC="$(realpath "$(dirname "$0")")/toolchain-mips-vr4300.cmake"
export MIPS_DEPS_PREFIX="$PREFIX"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$PREFIX"

build() {
    local name="$1"; shift
    echo "=== $name ==="
    cmake -S "$SRC/$name" -B "$SRC/build-$name" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_POLICY_DEFAULT_CMP0074=NEW \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" \
        > "$SRC/build-$name.log" 2>&1
    cmake --build "$SRC/build-$name" -j"$JOBS" >> "$SRC/build-$name.log" 2>&1
    cmake --install "$SRC/build-$name" >> "$SRC/build-$name.log" 2>&1
}

build zlib     -DZLIB_BUILD_EXAMPLES=OFF
rm -f "$PREFIX"/lib/libz.so*
build libpng   -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
build jpeg     -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF
build freetype -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_BROTLI=ON \
               -DFT_DISABLE_HARFBUZZ=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON
build harfbuzz -DHB_HAVE_FREETYPE=ON -DHB_BUILD_SUBSET=OFF -DHB_HAVE_GLIB=OFF \
               -DHB_HAVE_ICU=OFF
build ogg      -DINSTALL_DOCS=OFF
build vorbis
# SDL2: no window-system / audio-server backends exist on the N64 anyway;
# keep only the dummy/offscreen drivers so the library builds headless.
build SDL2     -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
               -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF \
               -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF \
               -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBUDEV=OFF \
               -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_RPATH=OFF
build mbedtls  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
build curl     -DBUILD_CURL_EXE=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
               -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF \
               -DUSE_LIBIDN2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DBUILD_TESTING=OFF \
               -DENABLE_CURL_MANUAL=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF
build libsamplerate -DLIBSAMPLERATE_EXAMPLES=OFF -DBUILD_TESTING=OFF
echo "All dependencies installed to $PREFIX"
