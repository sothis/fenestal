#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The release windows at once -- one per platform this desktop runs: the
# Linux tree twice, told to open Wayland and X11 (FNL_BACKEND), and the
# Windows tree twice under wine, told to draw with Direct3D 12 and Vulkan
# (FNL_RENDERER). Each is told its renderer, the Linux tree's too, as a
# round's program is: never left to the library's choice.
#
#   ./run_all.sh                      all four, no options
#   ./run_all.sh --frame-times        all four, with that option each
#   ./run_all.sh --scale=2 --no-late-latch
#
# Every argument is passed to all four, which is the point of the script: the
# comparisons worth making are the same option on several platforms at the same
# moment on the same desktop and the same GPU. There is no way to give one
# window an option and another a different one, and there should not be -- that
# is several terminals, and they are one command each.
#
# Which window is which: press F1 in each. The panel's Platform line names the
# pair, "Vulkan on Wayland" against "Vulkan on X11" against "Vulkan on Win32"
# and "Direct3D 12 on Win32". Nothing else tells them apart on screen -- the
# caption and the WM_CLASS are the same four times over, and three of the four
# are X11 windows on XWayland.
#
# Deliberately not a mode of build.sh, which says why in its own header:
# starting the binary is a decision rather than a build stage. So this builds
# nothing either. A missing tree is an error naming the command that would fix
# it, because a run script that quietly built first is a run script that can
# take a minute to show a window.
#
# The Windows build runs under wine, where it opens a real X11 window on
# XWayland. Vulkan goes through winevulkan onto the same driver the native
# builds use, and Direct3D 12 through vkd3d-proton onto Vulkan, since wine's
# own vkd3d runs no DXIL (tools/vkd3d_proton.py). WINEDEBUG=-all, because
# wine's own chatter is not this project's log and a reader grepping one of
# these files for a failure should not find somebody else's.
set -eu
cd "$(dirname "$0")"

# The harness's names for the four, which the logs are named for. Each ends
# in the renderer it is told to draw with, as FNL_RENDERER spells it.
TARGETS="wayland-vulkan x11-vulkan win64-d3d12 win64-vulkan"
OUT=build/run

# The tree each runs from: the Linux one has every backend, and this desktop
# offers two; the Windows one has every renderer, and wine runs both.
tree() {
    case $1 in
        win64-*) echo win64 ;;
        *)       echo linux ;;
    esac
}

# The cross tree's binary is a PE and is named for it; the native one is its
# own file. Tested with -f rather than -x throughout, since the executable
# bit is not what decides whether the cross binary runs.
binary() {
    case $1 in
        win64-*) echo "build/$(tree "$1")/release/fenestal-testbed.exe" ;;
        *)       echo "build/$(tree "$1")/release/fenestal-testbed" ;;
    esac
}

missing=
for t in $TARGETS; do
    [ -f "$(binary "$t")" ] || case " $missing " in
        *" $(tree "$t") "*) ;;
        *) missing="$missing $(tree "$t")" ;;
    esac
done
if [ -n "$missing" ]; then
    echo "run_all.sh: no release binary for:$missing" >&2
    for t in $missing; do
        echo "  ./build.sh $t" >&2
    done
    exit 1
fi

if ! command -v wine >/dev/null 2>&1; then
    echo "run_all.sh: wine is not on PATH, and the win64 tree needs it" >&2
    exit 1
fi

# The Direct3D 12 window runs from a copy of the tree's programs of its own,
# with vkd3d-proton and DXVK's DXGI beside it, fetched once and pinned by
# tools/vkd3d_proton.py, as the harness stages it (environment.py,
# _Vkd3dProton). Never into the tree: the Vulkan window runs from there,
# and DXVK's dxgi.dll beside it would be the one it loads. The
# overrides are that script's, so that wine loads the copies and not its own.
D3D12="$OUT/win64-d3d12"
dlls=$(python3 tools/vkd3d_proton.py --dir) || {
    echo "run_all.sh: no vkd3d-proton for win64-d3d12" \
         "-- tools/vkd3d_proton.py --fetch" >&2
    exit 1
}
overrides=$(python3 -c 'import sys; sys.path.insert(0, "tools")
import vkd3d_proton; print(vkd3d_proton.OVERRIDES)')
mkdir -p "$D3D12"
rm -f "$D3D12"/*.dll
cp -p "build/$(tree win64-d3d12)/release/"*.exe \
      "build/$(tree win64-d3d12)/release/"*.dll "$dlls"/*.dll "$D3D12/"

# Nothing here checks DISPLAY or WAYLAND_DISPLAY, and that is deliberate: each
# program already fails with the better message -- it knows which backend it
# was told to open, and which renderer -- and that message lands in its log,
# which is what the report at the bottom prints for a window that did not
# survive.

mkdir -p "$OUT"

# One log per window rather than four streams into this terminal. Not
# tidiness: --frame-times writes a line a second per window, and four of them
# interleaved with no way to tell which is which describe nothing. Both streams
# go to the file, since the library logs to stderr and the startup inventory
# with it.
#
# The target and its pid are kept together as one "target:pid" word, because a
# POSIX shell has no arrays and two lists that must stay in step are two lists
# that can fall out of it.
running=
for t in $TARGETS; do
    log="$OUT/$t.log"
    # Errors only from vkd3d-proton, and nothing from DXVK, nor a shader
    # cache left in the copy's directory: environment.py's _Vkd3dProton.ENV.
    case $t in
        win64-d3d12) env -u FNL_BACKEND FNL_RENDERER=d3d12 WINEDEBUG=-all \
                         WINEDLLOVERRIDES="$overrides" VKD3D_DEBUG=err \
                         VKD3D_SHADER_DEBUG=err DXVK_LOG_LEVEL=none \
                         VKD3D_SHADER_CACHE_PATH=0 \
                         wine "$D3D12/fenestal-testbed.exe" "$@" \
                         >"$log" 2>&1 & ;;
        win64-*) env -u FNL_BACKEND FNL_RENDERER=${t#*-} WINEDEBUG=-all \
                     wine "$(binary "$t")" "$@" >"$log" 2>&1 & ;;
        *)       FNL_BACKEND=${t%%-*} FNL_RENDERER=${t#*-} \
                     "$(binary "$t")" "$@" >"$log" 2>&1 & ;;
    esac
    running="$running $t:$!"
    echo "run_all.sh: $t is pid $! -- $log"
done

# Ctrl-C at this terminal already reaches all four: they are in this script's
# process group and the tty signals the group, not the leader. The kill is for
# the other way in -- a TERM sent to the script alone -- and every pid is one
# the loop below has not yet reaped, so a failure here is a window that has
# already gone and is not worth a word.
stopping=0
trap 'stopping=1
      for j in $running; do kill "${j#*:}" 2>/dev/null || true; done' INT TERM

# Wait for all four rather than for the first, so closing one window leaves
# the others alone -- which is the whole reason to start them together.
rc=0
for j in $running; do
    t=${j%%:*}
    status=0
    wait "${j#*:}" || status=$?
    if [ "$stopping" -eq 0 ] && [ "$status" -ne 0 ]; then
        rc=1
        echo "run_all.sh: $t exited $status, and its log ends:" >&2
        tail -n 10 "$OUT/$t.log" >&2 || true
    fi
done
exit "$rc"
