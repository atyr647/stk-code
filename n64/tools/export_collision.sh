#!/bin/sh
# Writes tracks/<name>/n64_collision.bin for every track of a data set, using
# a host STK built with STK_LOW_MEMORY + BULLET_SMALL_MEMORY. Each file is
# verified against the collision built in RAM before it is kept (see
# src/physics/collision_file.hpp). The host must have the target's byte
# order and word size: for the N64, the hosted big-endian MIPS build run
# under qemu-mips.
#
# Usage: export_collision.sh <stk binary> <data root> [track ...]
#   e.g. export_collision.sh "qemu-mips /opt/stkbuild-server-small/bin/supertuxkart" /opt/stkexp/root-v3
set -u
STK=$1
ROOT=$2
shift 2
TRACKS=${*:-$(ls "$ROOT/data/tracks")}
HOME_DIR=$(mktemp -d)
fail=0
for t in $TRACKS; do
    [ -f "$ROOT/data/tracks/$t/track.xml" ] || continue
    rm -f "$ROOT/data/tracks/$t/n64_collision.bin"
    out=$(STK_COLLISION_EXPORT=exit SUPERTUXKART_DATADIR="$ROOT" HOME="$HOME_DIR" \
          $STK --no-graphics --track="$t" --numkarts=1 --kart=tux \
          --profile-laps=1 --unlock-all 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    v=$(echo "$out" | grep -o 'Verified.*' | sed 's/.*\(: [0-9]* triangles\)/\1/')
    if [ -f "$ROOT/data/tracks/$t/n64_collision.bin" ]; then
        printf '%-28s ok   %s\n' "$t" "$v"
    else
        printf '%-28s FAIL %s\n' "$t" "$(echo "$out" | grep -E 'fatal|error' | tail -1)"
        fail=1
    fi
done
rm -rf "$HOME_DIR"
exit $fail
