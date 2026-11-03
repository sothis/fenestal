#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Gathers a release tree into stage/, which CI uploads as the run's
# artifact (.github/workflows/ci.yml).
#
#   .github/stage.sh common      the public headers, the API reference and
#                                the commented source, unpacked
#   .github/stage.sh <target>    that target's release tree: the library,
#                                static and shared, in <target>/lib, and
#                                the programs linked with the static one in
#                                <target>/bin
#
# The programs are the testbed, fenestal-dev and each demo, never a demo's
# -dyn twin, which wants the shared library beside it. A program the tree
# should have and has not fails the script: an artifact missing one is a
# build that went wrong. The shared library goes in under each of its
# names, as copies: an artifact keeps no symlink.
#
# An artifact keeps no file mode either, so a program comes back from
# GitHub without its execute bit.
set -eu
cd "$(dirname "$0")/.."

OUT=stage

if [ "${1:-}" = common ]; then
    mkdir -p "$OUT/include" "$OUT/source"
    cp -R include/fenestal "$OUT/include/"
    cp -R build/api "$OUT/api"
    python3 -m zipfile -e build/fenestal-commented.zip "$OUT/source/"
    exit 0
fi

TARGET=${1:?usage: .github/stage.sh common | <target>}
TREE=build/$TARGET/release
EXE=
if [ "$TARGET" = win64 ]; then
    EXE=.exe
fi

mkdir -p "$OUT/$TARGET/lib" "$OUT/$TARGET/bin"
for f in "$TREE"/libfenestal.a "$TREE"/libfenestal.so* \
         "$TREE"/libfenestal.dll "$TREE"/libfenestal.dll.a \
         "$TREE"/libfenestal*.dylib; do
    if [ -e "$f" ]; then
        cp -L "$f" "$OUT/$TARGET/lib/"
    fi
done

# Each demo's folder names its program; none has a space in it.
DEMOS=
for d in demos/*/; do
    d=${d%/}
    DEMOS="$DEMOS ${d##*/}"
done
for p in fenestal-testbed fenestal-dev $DEMOS; do
    cp "$TREE/$p$EXE" "$OUT/$TARGET/bin/"
done

# Vulkan on a Mac is MoltenVK, which the library opens from beside the
# program (tools/moltenvk.py), with the licence it ships under.
if [ -e "$TREE/libMoltenVK.dylib" ]; then
    cp "$TREE/libMoltenVK.dylib" "$TREE/MoltenVK-LICENSE.txt" \
       "$OUT/$TARGET/bin/"
fi
