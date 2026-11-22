#!/usr/bin/env bash
# Build and install libdragon (preview branch, pinned) into $N64_INST.
#
#   N64_INST=/opt/pak-n64 n64/build_libdragon.sh
#
# Mirrors atyr647/Pak tools/build_libdragon.sh, with one difference: STK needs
# libdragon's *preview* branch, not trunk. preview is where the kernel threads
# (kernel.h -- std::thread is shimmed on top of them), OpenGL 1.1 (GL/gl.h),
# Opus audio and the DSO loader live; trunk, which Pak pins, has none of them.
#
# The compiler comes from Pak's tools/build_n64_toolchain.sh: its versions
# (binutils 2.45, GCC 16.2.0, newlib 4.4.0.20231231) are the ones libdragon's
# own tools/build-toolchain.sh uses at this revision.
set -euo pipefail
N64_INST="${N64_INST:-/opt/pak-n64}"
REV="${LIBDRAGON_REV:-39d0d6096130836a65710da6730290701d060272}"
SRC="${LIBDRAGON_SRC:-${TMPDIR:-/tmp}/stk-libdragon}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

[ -x "$N64_INST/bin/mips64-elf-g++" ] || {
    echo "libdragon: no toolchain at $N64_INST (run Pak's tools/build_n64_toolchain.sh)"; exit 1; }

if [ ! -d "$SRC/.git" ]; then
    git clone -q -b preview https://github.com/DragonMinded/libdragon "$SRC"
fi
cd "$SRC"
git fetch -q origin preview || true
git checkout -q "$REV"

export N64_INST PATH="$N64_INST/bin:$PATH"
make -j"$JOBS" libdragon
make install
make -j"$JOBS" tools
make tools-install
echo "libdragon: preview @ $(git rev-parse --short HEAD) installed to $N64_INST"
