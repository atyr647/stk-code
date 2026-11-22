#!/usr/bin/env bash
# Boot a .z64 in ares headlessly and save what reaches the screen.
#
#   n64/tools/ares_run.sh ROM OUT.png [max_seconds] [settle_samples]
#
# Same approach as atyr647/Pak tcl/tools/ares_test.tcl (ares built by Pak's
# tools/build_ares.sh): Xvfb + Mesa's software rasterisers, ares started
# fullscreen with the flags that test uses, and the screen grabbed until it
# stops changing -- paraLLEl-RDP compiles its shaders on the first frame under
# lavapipe, so a fixed sleep is the wrong tool. Exits 0 once a non-black,
# stable frame has been captured; 1 if none arrived; 2 if ares refused to start.
#
# ARES (default /opt/pak-ares/bin/ares) can be overridden.
set -u
ROM="$1"; OUT="$2"; MAX="${3:-240}"; SETTLE="${4:-3}"
ARES="${ARES:-/opt/pak-ares/bin/ares}"
LOG="${OUT%.png}.log"
for t in Xvfb import convert; do
    command -v "$t" >/dev/null || { echo "ares_run: missing $t"; exit 2; }
done
[ -x "$ARES" ] || { echo "ares_run: no ares at $ARES (run Pak's tools/build_ares.sh)"; exit 2; }

DISP=""
for d in $(seq 90 119); do
    [ -e /tmp/.X$d-lock ] && continue
    Xvfb :$d -screen 0 1280x960x24 -ac -nolisten tcp >/dev/null 2>&1 &
    XPID=$!
    sleep 2
    if [ -e /tmp/.X$d-lock ]; then DISP=$d; break; fi
    kill -9 $XPID 2>/dev/null
done
[ -n "$DISP" ] || { echo "ares_run: no free X display"; exit 2; }

DISPLAY=:$DISP LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy "$ARES" \
    --system "Nintendo 64" --no-file-prompt --fullscreen \
    --setting Audio/Mute=true "$ROM" > "$LOG" 2>&1 &
APID=$!

cleanup() { kill -9 $APID 2>/dev/null; kill -9 $XPID 2>/dev/null; }
trap cleanup EXIT

deadline=$(( $(date +%s) + MAX ))
last=""; stable=0; lit=0
while [ "$(date +%s)" -lt "$deadline" ]; do
    sleep 3
    if grep -q "Invalid setting" "$LOG" 2>/dev/null; then
        echo "ares_run: ares rejected a setting:"; cat "$LOG"; exit 2
    fi
    kill -0 $APID 2>/dev/null || { echo "ares_run: ares exited"; cat "$LOG"; exit 2; }
    DISPLAY=:$DISP import -window root "$OUT.tmp.png" 2>/dev/null || continue
    mean=$(convert "$OUT.tmp.png" -format '%[fx:mean.r*255] %[fx:mean.g*255] %[fx:mean.b*255]' info: 2>/dev/null) || continue
    peak=$(echo "$mean" | awk '{m=$1; if($2>m)m=$2; if($3>m)m=$3; print m}')
    if awk -v p="$peak" 'BEGIN{exit !(p<=4)}'; then stable=0; last=""; continue; fi
    if [ -n "$last" ] && awk -v a="$mean" -v b="$last" 'BEGIN{split(a,x," ");split(b,y," ");for(i=1;i<=3;i++){d=x[i]-y[i]; if(d<0)d=-d; if(d>0.5) exit 1}; exit 0}'; then
        stable=$((stable+1))
    else
        stable=0
    fi
    last="$mean"
    cp "$OUT.tmp.png" "$OUT"
    if [ "$stable" -ge "$SETTLE" ]; then lit=1; break; fi
done
rm -f "$OUT.tmp.png"
if [ "$lit" = 1 ]; then echo "ares_run: frame captured -> $OUT"; exit 0; fi
[ -f "$OUT" ] && echo "ares_run: picture never settled; last frame -> $OUT" || echo "ares_run: no frame reached the screen"
grep -v ALSA "$LOG" | tail -5
exit 1
