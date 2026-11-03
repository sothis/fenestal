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
#   .github/stage.sh <target>    stage/fenestal-<target>-<commit>.tar.xz
#                                for linux and linux-arm64, .zip for win64
#                                and macos: common, the library, static and
#                                shared, in lib/, and the programs linked
#                                with the static one in bin/
#
# The programs are the testbed, fenestal-dev and each demo, never a demo's
# -dyn twin, which wants the shared library beside it. A program the tree
# should have and has not fails the script: an archive missing one is a
# build that went wrong.
#
# tar for Linux, because the shared library is a file and two symlinks to
# it and the programs want their execute bit, neither of which python's
# zipfile keeps; zip for Windows, whose programs want neither, and for
# macOS, made by ditto, which keeps both, as a Mac's downloads are. macos
# is the two macOS trees joined, x86-64 and arm64, packed on a Mac.
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

# Each demo's folder names its program; none has a space in it.
DEMOS=
for d in demos/*/; do
    d=${d%/}
    DEMOS="$DEMOS ${d##*/}"
done

# ---- macOS: universal, and each program an app bundle ----------------------
# The two macOS trees, x86-64 and arm64, joined by lipo into one file of
# each, as a Mac's own downloads are. Each program is an app bundle: the
# program in Contents/MacOS, MoltenVK in Contents/Frameworks, where the
# library looks for it (src/renderer/vulkan/vk_load.c), and the whole signed
# ad hoc, which Apple's processors require of all code. That is no Developer
# ID's signature, so a bundle downloaded by a browser is still quarantined,
# and Gatekeeper asks before it opens. Zipped by ditto, which keeps the
# dylib's symlinks, where python's zipfile would not.
if [ "$TARGET" = macos ]; then
    X=build/macos/release
    A=build/macos-arm64/release
    for t in "$X" "$A"; do
        if [ ! -d "$t" ]; then
            echo "stage.sh: macos joins two trees, and $t is missing" >&2
            exit 1
        fi
    done
    for tool in lipo codesign ditto; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "stage.sh: macos is packed on a Mac: no $tool here" >&2
            exit 1
        fi
    done
    VERSION=$(sed -n 's/^project(fenestal VERSION \([0-9.]*\).*/\1/p' \
              CMakeLists.txt)

    for f in "$X"/libfenestal.a "$X"/libfenestal*.dylib; do
        if [ -L "$f" ]; then
            cp -P "$f" "$DIR/lib/"
        else
            lipo -create "$f" "$A/${f##*/}" -output "$DIR/lib/${f##*/}"
        fi
    done

    for p in fenestal-testbed fenestal-dev $DEMOS; do
        app=$DIR/bin/$p.app
        mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" \
                 "$app/Contents/Resources"
        lipo -create "$X/$p" "$A/$p" -output "$app/Contents/MacOS/$p"
        cp "$X/libMoltenVK.dylib" "$app/Contents/Frameworks/"
        cp "$X/MoltenVK-LICENSE.txt" "$app/Contents/Resources/"
        cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>$p</string>
    <key>CFBundleIdentifier</key>
    <string>io.github.sothis.fenestal.$p</string>
    <key>CFBundleName</key>
    <string>$p</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
        # Inside out: the bundle's signature seals what it holds, which
        # has to be signed first.
        codesign --force --sign - "$app/Contents/Frameworks/libMoltenVK.dylib"
        codesign --force --sign - "$app"
    done

    cd "$OUT"
    rm -f "$NAME-$COMMIT.zip"
    ditto -c -k --keepParent "$NAME" "$NAME-$COMMIT.zip"
    said "$OUT/$NAME-$COMMIT.zip"
    exit 0
fi

# ---- Linux and Windows ------------------------------------------------------
# -P keeps the shared library's two names symlinks to the third.
for f in "$TREE"/libfenestal.a "$TREE"/libfenestal.so* \
         "$TREE"/libfenestal.dll "$TREE"/libfenestal.dll.a; do
    if [ -e "$f" ]; then
        cp -P "$f" "$DIR/lib/"
    fi
done

for p in fenestal-testbed fenestal-dev $DEMOS; do
    cp "$TREE/$p$EXE" "$DIR/bin/"
done

cd "$OUT"
case $TARGET in
win64)
    rm -f "$NAME-$COMMIT.zip"
    python3 -m zipfile -c "$NAME-$COMMIT.zip" "$NAME" ;;
*)
    tar -cJf "$NAME-$COMMIT.tar.xz" "$NAME" ;;
esac
said "$OUT/$(ls "$NAME-$COMMIT".*)"
