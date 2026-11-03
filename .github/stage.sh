#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Packs what CI uploads (.github/workflows/ci.yml), under stage/: each
# release tree as an archive for its operating system, and the source.
#
#   .github/stage.sh source      stage/fenestal-source-<commit>.zip: the
#                                build's commented source zip, renamed
#   .github/stage.sh common      stage/common: what every target's archive
#                                holds -- the public headers, the API
#                                reference, and the licence and notices the
#                                programs travel under; the source is an
#                                archive of its own
#   .github/stage.sh <target>    stage/fenestal-<target>-<commit>.tar.xz,
#                                or .zip for win64: common, the library,
#                                static and shared, in lib/, and the
#                                programs linked with the static one in bin/
#
# The programs are the testbed, fenestal-dev and each demo, never a demo's
# -dyn twin, which wants the shared library beside it. A program the tree
# should have and has not fails the script: an archive missing one is a
# build that went wrong.
#
# tar for Linux and macOS, because the shared library is a file and two
# symlinks to it and the programs want their execute bit, neither of which
# a zip keeps; zip for Windows, whose programs want neither and whose users
# open a zip.
set -eu
cd "$(dirname "$0")/.."

OUT=stage

COMMIT=$(printf '%.12s' "${GITHUB_SHA:-$(git rev-parse HEAD)}")

# The step's output, for the upload that follows it: a pattern would match
# linux-arm64's archive as well as linux's.
said() {
    echo "stage.sh: $1"
    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        echo "archive=$1" >> "$GITHUB_OUTPUT"
    fi
}

case ${1:-} in
common)
    rm -rf "$OUT/common"
    mkdir -p "$OUT/common/include"
    cp -R include/fenestal "$OUT/common/include/"
    cp -R build/api "$OUT/common/api"
    cp LICENSE THIRD_PARTY_NOTICES.md "$OUT/common/"
    exit 0 ;;
source)
    mkdir -p "$OUT"
    cp build/fenestal-commented.zip "$OUT/fenestal-source-$COMMIT.zip"
    said "$OUT/fenestal-source-$COMMIT.zip"
    exit 0 ;;
esac

TARGET=${1:?usage: .github/stage.sh source | common | <target>}
TREE=build/$TARGET/release
NAME=fenestal-$TARGET
DIR=$OUT/$NAME
EXE=
if [ "$TARGET" = win64 ]; then
    EXE=.exe
fi
if [ ! -d "$OUT/common" ]; then
    echo "stage.sh: no $OUT/common; .github/stage.sh common first" >&2
    exit 1
fi

rm -rf "$DIR"
mkdir -p "$DIR/lib" "$DIR/bin"
cp -R "$OUT/common/." "$DIR/"

# -P keeps the shared library's two names symlinks to the third.
for f in "$TREE"/libfenestal.a "$TREE"/libfenestal.so* \
         "$TREE"/libfenestal.dll "$TREE"/libfenestal.dll.a \
         "$TREE"/libfenestal*.dylib; do
    if [ -e "$f" ]; then
        cp -P "$f" "$DIR/lib/"
    fi
done

# Each demo's folder names its program; none has a space in it.
DEMOS=
for d in demos/*/; do
    d=${d%/}
    DEMOS="$DEMOS ${d##*/}"
done
for p in fenestal-testbed fenestal-dev $DEMOS; do
    cp "$TREE/$p$EXE" "$DIR/bin/"
done

# Vulkan on a Mac is MoltenVK, which the library opens from beside the
# program (tools/moltenvk.py), with the licence it ships under.
if [ -e "$TREE/libMoltenVK.dylib" ]; then
    cp "$TREE/libMoltenVK.dylib" "$TREE/MoltenVK-LICENSE.txt" "$DIR/bin/"
fi

cd "$OUT"
case $TARGET in
win64)
    rm -f "$NAME-$COMMIT.zip"
    python3 -m zipfile -c "$NAME-$COMMIT.zip" "$NAME" ;;
*)
    tar -cJf "$NAME-$COMMIT.tar.xz" "$NAME" ;;
esac
said "$OUT/$(ls "$NAME-$COMMIT".*)"
