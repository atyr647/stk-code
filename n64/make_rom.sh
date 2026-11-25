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
    EXCLUDE+=(--exclude '*.ogg' --exclude 'shaders/'
              --exclude 'ttf/' --exclude 'replay/')
    # Images are kept, but emptied below: a server build never decodes
    # them, yet it resolves materials through the texture files' presence
    # (ServerDummyTexture). Without them every texture falls back to the
    # default material, and triangles STK ignores for physics, or routes
    # to the effect mesh, end up in the collision mesh.
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
        : > "$WORK/fs/data/$d/n64_keep.txt"
    done
    # GUIEngine::Skin only checks that the configured fonts exist
    # (getAssetChecked); empty stand-ins keep 16 MB of fonts off the cartridge.
    for f in "$STK/data/ttf/"*; do : > "$WORK/fs/data/ttf/$(basename "$f")"; done
fi
if [ "${STK_N64_SERVER:-0}" = 1 ]; then
    find "$WORK/fs/data" -type f \( -iname '*.png' -o -iname '*.jpg' \
         -o -iname '*.jpeg' \) -exec truncate -s 0 {} +
fi
if [ "${STK_N64_SERVER:-0}" != 1 ]; then
    # Client ROMs. Translations (16 MB of .po for 71 languages) only with
    # STK_N64_PO=1: the cartridge budget is not settled yet; STK needs the
    # directory and runs in English without it. (Placeholder files are not
    # dot-files: mkdfs leaves those out, and an empty directory with them.)
    mkdir -p "$WORK/fs/data/po"
    if [ "${STK_N64_PO:-0}" = 1 ]; then
        rsync -a "$STK/data/po/" "$WORK/fs/data/po/"
    else
        : > "$WORK/fs/data/po/n64_keep.txt"
    fi
    # Client ROMs. Replays are benchmark ghosts only (15 MB): left out.
    rm -rf "$WORK/fs/data/replay"; mkdir -p "$WORK/fs/data/replay"
    : > "$WORK/fs/data/replay/n64_keep.txt"
    # STK reads a font file whole into RAM: the emoji (10 MB) and CJK
    # (4.6 MB) fonts cannot be loaded on the N64 at all. The emoji font gets
    # an empty stand-in (for the skin's existence checks); the CJK font is
    # cut down to the symbols STK's own text needs (keyboard keys, stars,
    # ticks: n64_font_subset.py, needs Python's fontTools). Chinese,
    # Japanese and emoji text is not displayable.
    [ -f "$WORK/fs/data/ttf/NotoColorEmoji.ttf" ] && : > "$WORK/fs/data/ttf/NotoColorEmoji.ttf"
    if [ -s "$WORK/fs/data/ttf/wqy-microhei.ttf" ]; then
        python3 "$STK/n64/tools/n64_font_subset.py" "$STK/data/ttf/wqy-microhei.ttf" \
            "$WORK/fs/data/ttf/wqy-microhei.ttf"
    fi
    # Without translations (STK_N64_PO, above) all text is in Latin script:
    # the fallback fonts for other scripts (870 KB, also read whole) would
    # only take RAM. Cantarell (text) and SigmarOne (digits) stay.
    if [ "${STK_N64_PO:-0}" != 1 ]; then
        for f in "$WORK/fs/data/ttf/Noto"*; do : > "$f"; done
    fi
    # The N64 audio back-end is not written yet (n64/compat/src/sdl_n64.c:
    # no audio device), and STK only opens .ogg files to play them: empty
    # stand-ins keep 80 MB of music off the cartridge until then.
    # STK_N64_OGG=1 keeps them.
    if [ "${STK_N64_OGG:-0}" != 1 ]; then
        find "$WORK/fs/data" -type f -iname '*.ogg' -exec truncate -s 0 {} +
    fi
fi
if [ "${STK_N64_SERVER:-0}" != 1 ]; then
    # images at the size STK would resize them to on the N64 (<= 64x64),
    # skin borders scaled along: see the script
    python3 "$(dirname "$0")/tools/n64_gui_images.py" "$WORK/fs/data"
fi
[ -n "$ARGS" ] && cp "$ARGS" "$WORK/fs/data/n64_args.txt"

"$BIN/mkdfs" "$WORK/data.dfs" "$WORK/fs" >/dev/null

# --- ELF -> ROM (n64.mk's %.z64 rule) ------------------------------------------
cp "$ELF" "$WORK/stk.elf"
"$BIN/n64sym" --all "$WORK/stk.elf" "$WORK/stk.sym"
cp "$WORK/stk.elf" "$WORK/stk.stripped"
"$BIN/mips64-elf-strip" -s "$WORK/stk.stripped"
# Paged code (client builds, n64/compat/src/pager.c): the .paged section is
# its own file on the cartridge (stk.paged), read in 32 KB blocks at run
# time; the image IPL3 loads at boot leaves it out
PAGED=""
if "$BIN/mips64-elf-objdump" -h "$WORK/stk.stripped" | grep -q ' \.paged '; then
    "$BIN/mips64-elf-objcopy" -O binary -j .paged "$WORK/stk.stripped" "$WORK/stk.paged"
    "$BIN/mips64-elf-objcopy" -R .paged "$WORK/stk.stripped"
    PAGED="$WORK/stk.paged"
fi
"$BIN/n64elfcompress" -o "$WORK" -c "${STK_N64_ELFCOMPRESS:-1}" "$WORK/stk.stripped"
"$BIN/n64tool" --toc --title "SuperTuxKart" --output "$OUT" \
    --align 256 "$WORK/stk.stripped" "$WORK/stk.sym" "$WORK/data.dfs" $PAGED

printf 'ROM %s: %s (DFS %s, %s files)\n' "$OUT" \
    "$(du -h "$OUT" | cut -f1)" "$(du -h "$WORK/data.dfs" | cut -f1)" \
    "$(find "$WORK/fs" -type f | wc -l)"
