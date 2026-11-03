#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# tools/probe/heldpresent.c built into build/linux/release/heldpresent,
# where tools/probe/bare_run.py reads a program's tree and variant off its
# path, and then, given a machine's arguments, run there on weston:
#
#   tools/probe/heldpresent.sh                  # build only
#   FNL_DRIVE_BARE=schenker tools/probe/heldpresent.sh 120 \
#       --env HELDPRESENT_PCI=0000:01:00.0 NVK 3 8 20
#
# The arguments after the build are bare_run.py's, less the program: the
# seconds, its options, then heldpresent's own. Not a target of the build:
# it links the Vulkan loader and libwayland-client, which nothing the
# build makes may (README), and it is run by hand, the moment a weston
# round on NVK loses its window (TODO).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
P=/usr/share/wayland-protocols
G=$root/build/heldpresent-proto
out=$root/build/linux/release/heldpresent
mkdir -p "$G" "$(dirname "$out")"
for x in stable/xdg-shell/xdg-shell \
         stable/presentation-time/presentation-time; do
    n=$(basename "$x")
    wayland-scanner client-header "$P/$x.xml" "$G/$n-client-protocol.h"
    wayland-scanner private-code "$P/$x.xml" "$G/$n-protocol.c"
done
gcc -std=c11 -O2 -Wall -Wextra -I"$G" -o "$out" \
    "$root/tools/probe/heldpresent.c" "$G/xdg-shell-protocol.c" \
    "$G/presentation-time-protocol.c" -lvulkan -lwayland-client
echo "built $out"
if [ $# -gt 0 ]; then
    secs=$1
    shift
    exec python3 "$root/tools/probe/bare_run.py" "$out" "$secs" --weston "$@"
fi
