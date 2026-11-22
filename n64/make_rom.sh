#!/usr/bin/env bash
# Pack an STK N64 build into a .z64 ROM.
#
#   n64/make_rom.sh <build dir> <downscaled assets dir> <out.z64> [args file]
#
#   build dir        CMake build made with n64/toolchain-libdragon.cmake
#   assets dir       output of n64/tools/stk_n64_assets.py
#   args file        command line for the run (one argument per line), copied
#                    to rom:/data/n64_args.txt; see n64/compat/src/n64_main.cpp
#
# Environment:
#   N64_INST         libdragon install (default /opt/pak-n64)
#   STK_N64_TRACKS   comma list: only these tracks go in the ROM (default all)
#   STK_N64_KARTS    comma list: only these karts (default all)
#   STK_N64_SERVER   1 = the build is SERVER_ONLY: images, fonts, shaders and
#                    audio are not loaded by such a build, so they stay out
#
# The steps are those of libdragon's n64.mk %.z64 rule: n64sym, strip,
# n64elfcompress, then n64tool with the ELF, its symbols and the DragonFS
# image (rom:/).
set -euo pipefail
BUILD="$(realpath "$1")"; ASSETS="$(realpath "$2")"; OUT="$(realpath -m "$3")"
ARGS="${4:-}"
export N64_INST="${N64_INST:-/opt/pak-n64}"
BIN="$N64_INST/bin"
STK="$(realpath "$(dirname "$0")/..")"
ELF="$BUILD/bin/supertuxkart"
[ -f "$ELF" ] || ELF="$BUILD/bin/supertuxkart.elf"
WORK="$BUILD/rom"
rm -rf "$WORK"; mkdir -p "$WORK/fs/data"

# --- rom:/data ---------------------------------------------------------------
EXCLUDE=(--exclude '*.xcf' --exclude '*.blend' --exclude '*.svg' --exclude '*.icns'
         --exclude 'po/' --exclude 'supertuxkart_*.png' --exclude '*.desktop'
         --exclude '*.plist' --exclude 'n64_budget_report.json')
if [ "${STK_N64_SERVER:-0}" = 1 ]; then
    EXCLUDE+=(--exclude '*.png' --exclude '*.jpg' --exclude '*.jpeg'
              --exclude '*.ogg' --exclude 'shaders/'
              --exclude 'ttf/' --exclude 'replay/')
    # (skins/ and gui/ stay: the engine reads their XML even without a
    # renderer; their images are covered by the *.png/*.jpg rules)
fi
rsync -a "${EXCLUDE[@]}" "$STK/data/" "$WORK/fs/data/"
for d in library models textures; do
    [ -d "$ASSETS/$d" ] && rsync -a "${EXCLUDE[@]}" "$ASSETS/$d/" "$WORK/fs/data/$d/"
done
copy_set() {   # copy_set <kind> <comma list or empty>
    local kind="$1" list="$2"
    mkdir -p "$WORK/fs/data/$kind"
    if [ -z "$list" ]; then
        rsync -a "${EXCLUDE[@]}" "$ASSETS/$kind/" "$WORK/fs/data/$kind/"
    else
        IFS=, read -ra items <<< "$list"
        for i in "${items[@]}"; do
            rsync -a "${EXCLUDE[@]}" "$ASSETS/$kind/$i" "$WORK/fs/data/$kind/"
        done
    fi
}
copy_set tracks "${STK_N64_TRACKS:-}"
copy_set karts "${STK_N64_KARTS:-}"
# music/ and sfx/: the XML descriptors always (StkConfig/SFXManager read
# them); the audio itself only for builds that play it
for d in music sfx; do
    [ -d "$ASSETS/$d" ] && rsync -a "${EXCLUDE[@]}" "$ASSETS/$d/" "$WORK/fs/data/$d/"
done
if [ "${STK_N64_SERVER:-0}" = 1 ]; then
    # FileManager insists every asset directory exists, even those a
    # server-only build never reads from: keep them, empty.
    for d in gui/icons gui/screens gui/dialogs replay shaders ttf po music sfx; do  # keep dirs
        mkdir -p "$WORK/fs/data/$d"
        : > "$WORK/fs/data/$d/.keep"
    done
    # GUIEngine::Skin only checks that the configured fonts exist
    # (getAssetChecked); empty stand-ins keep 16 MB of fonts off the cartridge.
    for f in "$STK/data/ttf/"*; do : > "$WORK/fs/data/ttf/$(basename "$f")"; done
fi
[ -n "$ARGS" ] && cp "$ARGS" "$WORK/fs/data/n64_args.txt"

"$BIN/mkdfs" "$WORK/data.dfs" "$WORK/fs" >/dev/null

# --- ELF -> ROM (n64.mk's %.z64 rule) ------------------------------------------
cp "$ELF" "$WORK/stk.elf"
"$BIN/n64sym" --all "$WORK/stk.elf" "$WORK/stk.sym"
cp "$WORK/stk.elf" "$WORK/stk.stripped"
"$BIN/mips64-elf-strip" -s "$WORK/stk.stripped"
"$BIN/n64elfcompress" -o "$WORK" -c "${STK_N64_ELFCOMPRESS:-1}" "$WORK/stk.stripped"
"$BIN/n64tool" --toc --title "SuperTuxKart" --output "$OUT" \
    --align 256 "$WORK/stk.stripped" "$WORK/stk.sym" "$WORK/data.dfs"

printf 'ROM %s: %s (DFS %s, %s files)\n' "$OUT" \
    "$(du -h "$OUT" | cut -f1)" "$(du -h "$WORK/data.dfs" | cut -f1)" \
    "$(find "$WORK/fs" -type f | wc -l)"
