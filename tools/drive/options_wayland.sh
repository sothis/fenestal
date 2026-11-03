#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The Wayland variant, under the checkers, across the option matrix.
#
# No input and no portal: what is stressed here is what the command line
# reaches -- fractional and clamped surface scales, and both late-latch
# settings. stress.py is the round that drives the window. Each run lives
# long enough to settle, then takes a SIGTERM, which is the testbed's own
# shutdown path (it asks for the handlers).
#
# What is being watched for is a sanitizer report, a non-zero exit, or a line
# on stderr -- not a picture. The Linux tree has every backend, so the
# program is told which to open, as a round's is: never left to choose; and
# told its renderer, as every round's program is (FNL_RENDERER).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/build/drive"
mkdir -p "$OUT"
export TSAN_OPTIONS="suppressions=$ROOT/tsan.supp"
LIVE=${LIVE:-12}

fails=0
run() {
    local variant=$1; shift
    local tag=$1; shift
    local bin="$ROOT/build/linux/$variant/fenestal-testbed"
    local log="$OUT/wl-$variant-$tag.log"
    [ -x "$bin" ] || { echo "skip  $variant $tag  not built"; return; }

    FNL_BACKEND=wayland FNL_RENDERER=vulkan "$bin" "$@" > "$log" 2>&1 &
    local pid=$!
    sleep "$LIVE"
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"; local early=$?
        echo "FAIL  $variant $tag  died early (rc=$early)"
        sed -n '1,12p' "$log" | sed 's/^/        /'
        fails=$((fails+1))
        return
    fi
    # CPU time, as evidence it was drawing and not parked: a window that
    # opened and then did nothing passes every other check here.
    local cpu; cpu=$(ps -o time= -p "$pid" 2>/dev/null | tr -d ' ')
    kill -TERM "$pid" 2>/dev/null
    wait "$pid"; local rc=$?

    # Everything the testbed prints on a clean run is the backend it opened,
    # the renderer it draws with and the device inventory, one line per device
    # that can present (see pick_device), the one it took, how long the other
    # renderers took to list theirs, and the protocols this compositor lacks
    # (wayland.c). Anything else is a sanitizer or a degradation this wants
    # to hear about.
    local noise quiet='^backend: Wayland, named by FNL_BACKEND$'
    quiet="$quiet|^renderer: Vulkan, named by FNL_RENDERER\$"
    quiet="$quiet|^GPU in use: |^[^ ].* lists [0-9]+ GPUs? here, in [0-9]+ ms\$"
    quiet="$quiet|^not bound, so their fallbacks are in use: "
    noise=$(grep -vE "$quiet|^GPU [0-9]+: |^\$" "$log" | head -6)
    if [ "$rc" -ne 0 ] || [ -n "$noise" ]; then
        echo "FAIL  $variant $tag  rc=$rc cpu=$cpu"
        [ -n "$noise" ] && printf '%s\n' "$noise" | sed 's/^/        /'
        fails=$((fails+1))
    else
        echo "ok    $variant $tag  rc=$rc cpu=$cpu"
    fi
}

echo "=== release ==="
run release default
run release scale-fractional --scale=1.5
run release scale-clamped    --scale=8
run release no-late-latch    --no-late-latch
run release late-latch-fixed --late-latch=8

echo "=== asan + ubsan ==="
run asan default
run asan scale-fractional --scale=2.5
run asan scale-clamped    --scale=8

echo "=== tsan ==="
run tsan default
run tsan scale-fractional --scale=1.75
run tsan no-late-latch    --no-late-latch

echo
echo "$fails failed"
exit $((fails > 0))
