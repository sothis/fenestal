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
#                                for linux, linux-arm64 and freebsd, .zip
#                                for win64, win64-msvc and macos: common,
#                                the library, static and shared, in lib/,
#                                and the programs linked with the static
#                                one in bin/
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
# is the universal tree, both processors' slices joined by build.sh, packed
# on a Mac.
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
case $TARGET in
win64|win64-msvc) EXE=.exe ;;
esac
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
# `./build.sh macos release` has already built both processors' slices and
# joined them into build/macos/release; this packs that as a Mac's own
# downloads are: each program an app bundle, the program in Contents/MacOS,
# MoltenVK in Contents/Frameworks, where the library looks for it
# (src/renderer/vulkan/vk_load.c), and the whole signed ad hoc, which
# Apple's processors require of all code. That is no Developer ID's
# signature, so a bundle downloaded by a browser is still quarantined, and
# Gatekeeper asks before it opens. Zipped by ditto, which keeps the dylib's
# symlinks, where python's zipfile would not.
if [ "$TARGET" = macos ]; then
    X=build/macos/release
    for tool in codesign ditto; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "stage.sh: macos is packed on a Mac: no $tool here" >&2
            exit 1
        fi
    done
    VERSION=$(sed -n 's/^project(fenestal VERSION \([0-9.]*\).*/\1/p' \
              CMakeLists.txt)

    for f in "$X"/libfenestal.a "$X"/libfenestal*.dylib; do
        cp -P "$f" "$DIR/lib/"
    done

    # MoltenVK only beside a program that has a Vulkan scene. The tree has
    # every renderer, so a program that never draws with Vulkan still lists
    # the Vulkan GPUs -- and steps past one that is not there, as any
    # machine without the runtime does (src/renderer/vulkan/vk_load.c);
    # hello draws nothing and fenestal-dev's modes draw nothing, and the
    # other demos and the testbed each carry scene_vulkan.c.
    has_vulkan() {
        case $1 in
        fenestal-dev) return 1 ;;
        fenestal-testbed) [ -f testbed/scene_vulkan.c ] ;;
        *) [ -f "demos/$1/scene_vulkan.c" ] ;;
        esac
    }

    for p in fenestal-testbed fenestal-dev $DEMOS; do
        app=$DIR/bin/$p.app
        mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" \
                 "$app/Contents/Resources"
        cp "$X/$p" "$app/Contents/MacOS/$p"
        if has_vulkan "$p"; then
            cp "$X/libMoltenVK.dylib" "$app/Contents/Frameworks/"
            cp "$X/MoltenVK-LICENSE.txt" "$app/Contents/Resources/"
        fi
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
        if [ -f "$app/Contents/Frameworks/libMoltenVK.dylib" ]; then
            codesign --force --sign - \
                "$app/Contents/Frameworks/libMoltenVK.dylib"
        fi
        codesign --force --sign - "$app"
    done

    cd "$OUT"
    rm -f "$NAME-$COMMIT.zip"
    ditto -c -k --keepParent "$NAME" "$NAME-$COMMIT.zip"
    said "$OUT/$NAME-$COMMIT.zip"
    exit 0
fi

# ---- Linux, Windows and FreeBSD ---------------------------------------------
# -P keeps the shared library's two names symlinks to the third. MSVC's
# archive and the DLL's import library are .lib, and named apart from each
# other (CMakeLists.txt).
if [ "$TARGET" = win64-msvc ]; then
    for f in "$TREE"/libfenestal.dll "$TREE"/fenestal.lib \
             "$TREE"/libfenestal-static.lib; do
        if [ -e "$f" ]; then
            cp "$f" "$DIR/lib/"
        fi
    done
else
    for f in "$TREE"/libfenestal.a "$TREE"/libfenestal.so* \
             "$TREE"/libfenestal.dll "$TREE"/libfenestal.dll.a; do
        if [ -e "$f" ]; then
            cp -P "$f" "$DIR/lib/"
        fi
    done
fi

for p in fenestal-testbed fenestal-dev $DEMOS; do
    cp "$TREE/$p$EXE" "$DIR/bin/"
done

cd "$OUT"
case $TARGET in
win64|win64-msvc)
    rm -f "$NAME-$COMMIT.zip"
    python3 -m zipfile -c "$NAME-$COMMIT.zip" "$NAME" ;;
*)
    tar -cJf "$NAME-$COMMIT.tar.xz" "$NAME" ;;
esac
said "$OUT/$(ls "$NAME-$COMMIT".*)"
