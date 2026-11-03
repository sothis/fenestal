#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The build, as one command.
#
#   ./build.sh                        every tree -- thirteen, and the ARM
#                                     Linux target's four where its
#                                     compiler is
#   ./build.sh linux                  that target, release: Wayland, X11
#                                     and KMS, drawn with Vulkan and OpenGL
#   ./build.sh win64                  that target, release: Win32 drawn
#                                     with Direct3D 12 and Vulkan
#   ./build.sh macos                  that target: Cocoa drawn with Metal,
#                                     Vulkan on MoltenVK and OpenGL 4.1
#   ./build.sh macos-arm64            the macos tree for Apple's own
#                                     processors, cross-compiled
#   ./build.sh linux-arm64            the linux tree for 64-bit ARM (the
#                                     Raspberry Pi), cross-compiled
#   ./build.sh linux asan             that target, that checker
#   ./build.sh linux clang            that target, built by clang
#   ./build.sh linux asan -DFNL_VERIFY_FONT=OFF
#   ./build.sh linux release -DFNL_BACKEND=x11 -DFNL_RENDERER=vulkan
#                                     one backend and one renderer alone,
#                                     into the same tree: a consumer's
#                                     smaller build
#   ./build.sh dist                   build/fenestal.zip and
#                                     fenestal-commented.zip alone, from
#                                     the working tree as it stands
#
# Anything after the target and the variant goes to the configure step, and
# goes *last*, so a flag on the command line overrides one this script chose.
#
# A Linux tree has every Linux backend, and the one a program opens is
# chosen when it starts -- or named, FNL_BACKEND=x11 in its environment;
# and every tree has every renderer its system has, chosen the same way or
# named by FNL_RENDERER=vulkan (DESIGN *One build per operating system*).
#
# Every tree lands under build/<target>/<variant>, and a successful build
# is followed by three checks of the source -- the layering
# (tools/check-layering.py), the tables (tools/check-tables.py) and the API
# reference (tools/mkapidoc.py), any of which fails it -- and leaves
# build/fenestal.zip and build/fenestal-commented.zip beside them
# (tools/mkdist.py). There is no run step and no clean mode: `rm -rf build`
# is something to type deliberately.
#
# Arguments are assembled with `set -- ... "$@"` and never by concatenation:
# a -D whose value contains a space (the sanitizer flags do) would be split by
# the shell and its second half would reach cmake as a stray source path.
set -eu
cd "$(dirname "$0")"

# After the cd, so the sweep can re-enter this script however `$0` was spelt.
SELF="$PWD/${0##*/}"

# mingw-w64 has no sanitizer runtimes: -fsanitize=address, =undefined and
# =thread all fail at the link with `cannot find -lasan` and the like. `gc`
# and `analyzer` need no runtime and cross as they are. `clang` does not
# cross: clang against mingw-w64 wants a sysroot of its own, and the rule it
# checks -- warning-clean under clang -- is about the code, which the
# native clang tree already compiles but for the Windows sources.
#
# The macOS trees are clang's from the start and cross-compiled, and take
# release and gc: LLVM's sanitizer runtimes for Darwin are not part of the
# Linux toolchain that builds them, and gcc's analyser does not target
# Darwin. gc is the Debug tree there, for FNL_ASSERT and the fault sites,
# its dead code stripped by ld64's -dead_strip where a GNU linker takes
# --gc-sections.
#
# The ARM Linux tree takes release and the two checkers that run, asan and
# tsan -- on the Pi's own sanitizer runtimes, never the cross compiler's
# (_runtimes() in tools/drive/bare_driver.py says why) -- and gc. Not
# clang, which would want an ARM sysroot of its own, nor analyzer, whose
# findings are the source's and the native tree's already.
#
# A function because the sweep skips these pairs and the single-tree path
# refuses them out loud, and the two must not disagree.
refused() {
    case "$1:$2" in
        win64:asan|win64:tsan|win64:clang) return 0 ;;
        macos:release|macos:gc|macos-arm64:release|macos-arm64:gc)
            return 1 ;;
        macos:*|macos-arm64:*) return 0 ;;
        linux-arm64:release|linux-arm64:asan|linux-arm64:tsan|\
        linux-arm64:gc) return 1 ;;
        linux-arm64:*) return 0 ;;
    esac
    return 1
}

# Whether Ubuntu's aarch64 cross gcc is here, for the ARM Linux tree: g++
# as well, for the public headers' C++ check. The sweep builds that tree
# where it is and says it skipped it where it is not -- the one tree whose
# compiler a machine that builds the rest may lack.
have_arm64() {
    command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 &&
        command -v aarch64-linux-gnu-g++ >/dev/null 2>&1
}

# The clang the clang trees use: FNL_CLANG, else clang-21 (the version that
# matches the installed clang-tidy and clang-format), else plain clang.
find_clang() {
    for c in "${FNL_CLANG:-}" clang-21 clang; do
        [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && {
            command -v "$c"; return 0; }
    done
    return 1
}

# ---- the baked font --------------------------------------------------------
# tools/mkfont's output is the same bytes in every native tree, so a tree may
# take a release tree's instead of baking its own. Whether that tree's bake
# can be lent: all four files are there, verifyfont passed on them since
# they were written, and none of what the bake reads is newer than they
# are. mkfont writes the files before verifyfont reads them, and a borrower
# checks nothing, so a bake whose check failed or was turned off
# (FNL_VERIFY_FONT) is not lent; nor is a stale one, an old atlas under new
# sources. The answer then is no: a native tree bakes its own, and the
# cross tree, which cannot, is refused with the command that makes one.
font_ready() {
    for f in font_data.h font_metrics.h font_atlas.bin font_cells.bin; do
        [ -f "$1/$f" ] || return 1
    done
    [ -n "$(find "$1/font_verified.stamp" -newer "$1/font_atlas.bin" \
            2>/dev/null)" ] || return 1
    for src in tools/mkfont.c tools/font_charset.h tools/font_codec.h \
               tools/font_raster.h \
               fonts/ubuntu/*.ttf; do
        [ -n "$(find "$1/font_atlas.bin" -newer "$src" 2>/dev/null)" ] \
            || return 1
    done
    return 0
}

# ---- the layering ----------------------------------------------------------
# Neither the compiler nor the linker objects when src/core/ names a window
# server, so the script that counts is run here, where every build passes.
# After the build and not before it: a tree that compiles is still wanted
# while the reference that failed this is being found.
#
# A property of the checkout as the zip is, so the sweep's children skip it
# on the same variable and the sweep asks once. The same two quiet ways out
# as dist(), for the same reasons; a violation fails the script.
layering() {
    if [ "${FNL_NO_DIST:-}" = 1 ]; then
        return 0
    fi
    if [ ! -f tools/check-layering.py ]; then
        return 0
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "build.sh: no python3, so the layering went unchecked" >&2
        return 0
    fi
    python3 tools/check-layering.py
    # The tables no compiler compares: keys, events, cursors, and the fields
    # of FnlDrawState against fnl_draw_state_eq(). Same terms as the layering.
    if [ -f tools/check-tables.py ]; then
        python3 tools/check-tables.py
    fi
    # The API reference, build/api/index.html, made from the headers; it
    # fails on a function with no comment, no thread tag or no caller.
    if [ -f tools/mkapidoc.py ]; then
        python3 tools/mkapidoc.py --quiet
    fi
}

# ---- the source zip --------------------------------------------------------
# A function here and not a CMake target, because what goes in is a property
# of the checkout and not of any one tree: the sweep sets FNL_NO_DIST=1 for
# its children and packs once, after the last.
#
# Three ways out. A tree unpacked from the zip is not the top of a git
# checkout -- it may sit inside one, under build/ -- and the packer's
# candidates are `git ls-files`, so for that tree silence is right. A
# machine without python3 gets one line and a build that still succeeded. A
# *failure* from mkdist fails the script: the manifest has gone stale against
# the tree.
dist() {
    if [ "${FNL_NO_DIST:-}" = 1 ]; then
        return 0
    fi
    if [ "$(git rev-parse --show-toplevel 2>/dev/null)" != "$(pwd -P)" ]; then
        return 0
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "build.sh: no python3, so no build/fenestal.zip" >&2
        return 0
    fi
    python3 tools/mkdist.py
    python3 tools/mkdist.py --keep-comments -o build/fenestal-commented.zip
}

# ---- dist: the source zips alone -------------------------------------------
# Both archives and no build, packed from the working tree as it stands:
# untracked files in, deleted ones out, each file's mode its own
# (mkdist.py --worktree), so a dirty checkout packs as it is on disk. Asked
# for, so where a build's pack is silent this one fails and says why.
if [ "${1:-}" = dist ]; then
    if [ $# -gt 1 ]; then
        echo "build.sh: dist takes no arguments" >&2
        exit 1
    fi
    if [ "$(git rev-parse --show-toplevel 2>/dev/null)" != "$(pwd -P)" ]
    then
        echo "build.sh: dist packs a git checkout's working tree, and" \
             "this is not the top of one" >&2
        exit 1
    fi
    python3 tools/mkdist.py --worktree
    python3 tools/mkdist.py --worktree --keep-comments \
        -o build/fenestal-commented.zip
    exit 0
fi

# ---- no arguments: every tree ----------------------------------------------
# Thirteen: six variants of the native target, three of the Windows one and
# two of each macOS one -- and the ARM Linux one's four, where have_arm64()
# says its compiler is.
#
# The order is load-bearing. Release comes first within a target because the
# checker trees and the clang tree take its baked font, and linux comes
# before the others because the cross trees take that one's and cannot
# bake at all.
#
# One re-entry per tree and not a loop around the body: each tree assembles
# its own configure line with `set --`, and POSIX sh has one argument list.
#
# It stops at the first failure. Nearly all of the code is shared, so going
# on would print the same error once for every tree after it.
if [ $# -eq 0 ]; then
    for target in linux linux-arm64 win64 macos macos-arm64; do
        if [ "$target" = linux-arm64 ] && ! have_arm64; then
            printf '\n==> linux-arm64 skipped: no aarch64-linux-gnu-gcc\n'
            continue
        fi
        for variant in release clang asan tsan analyzer gc; do
            refused "$target" "$variant" && continue
            printf '\n==> %s %s\n' "$target" "$variant"
            FNL_NO_DIST=1 "$SELF" "$target" "$variant"
        done
    done
    layering
    dist
    exit 0
fi

TARGET=$1; shift
case $TARGET in
    linux|win64|macos|macos-arm64|linux-arm64) ;;
    wayland-vulkan|wayland-gl|x11-vulkan|x11-gl|kms-vulkan|kms-gl|\
    linux-vulkan|linux-gl|win64-vulkan|win64-d3d12|\
    macos-metal|macos-vulkan)
        # The names the trees had when each was one platform or one
        # renderer; said rather than refused bare, since the hand still
        # types them.
        case $TARGET in
        win64-*) SYSTEM=win64 ;;
        macos-*) SYSTEM=macos ;;
        *)       SYSTEM=linux ;;
        esac
        echo "build.sh: one tree has every backend and renderer of its" \
             "system now: ./build.sh $SYSTEM" >&2
        echo "          FNL_BACKEND and FNL_RENDERER=${TARGET#*-} in a" \
             "program's environment name one of each" >&2
        exit 1 ;;
    *)
        echo "build.sh: unknown target '$TARGET'" >&2
        echo "          linux | win64 | macos | macos-arm64 |" \
             "linux-arm64 | dist" >&2
        exit 1 ;;
esac

VARIANT=release
case ${1:-} in
    release|clang|asan|tsan|analyzer|gc) VARIANT=$1; shift ;;
    -*|'') ;;
    # Neither a variant nor a flag: a misspelt variant, which would
    # otherwise build release and reach cmake as a source path.
    *)
        echo "build.sh: unknown variant '$1'" >&2
        echo "          release | clang | asan | tsan | analyzer | gc" >&2
        exit 1 ;;
esac

if refused "$TARGET" "$VARIANT"; then
    case $TARGET in
    macos|macos-arm64)
        echo "build.sh: the macOS tree is built by clang, cross, and" >&2
        echo "          takes release and gc: no Darwin sanitizer" >&2
        echo "          runtime or analyser here." >&2
        exit 1 ;;
    linux-arm64)
        echo "build.sh: the ARM Linux tree takes release, asan, tsan" >&2
        echo "          and gc: no ARM sysroot for clang, and the" >&2
        echo "          analyser's findings are the native tree's." >&2
        exit 1 ;;
    esac
    if [ "$VARIANT" = clang ]; then
        echo "build.sh: no clang tree for the cross target; the native" >&2
        echo "          target takes one: ./build.sh linux clang" >&2
    else
        echo "build.sh: $VARIANT needs a sanitizer runtime mingw-w64" >&2
        echo "          does not have (no libasan/libubsan/libtsan)." >&2
    fi
    echo "          win64 takes: release | analyzer | gc" >&2
    exit 1
fi

DIR="build/$TARGET/$VARIANT"

# ---- the checker flags, innermost so a command-line -D still wins ----------
# Both linker variables, because this project links an archive and a shared
# object: the executable one alone would leave libfenestal uninstrumented.
case $VARIANT in
release)
    # Said rather than left to the option's default, so a tree configured
    # when native was the default comes back to x86-64-v2; native is
    # `./build.sh <target> release -DFNL_NATIVE_ARCH=ON`, which goes last
    # and wins.
    set -- -DFNL_NATIVE_ARCH=OFF "$@" ;;
clang)
    # Optimised, as release is, so clang's optimiser and its warnings see the
    # code gcc's do; -Werror because warning-clean under clang is a rule and
    # this tree is what checks it. The compiler is passed every time, so the
    # cache never keeps another one -- CMake starts the cache over when it
    # differs from what the tree was configured with.
    if ! CLANG=$(find_clang); then
        echo "build.sh: the clang tree needs clang (clang-21 or FNL_CLANG)" >&2
        exit 1
    fi
    # And clang++ for the public headers' C++ check, which g++ passes on
    # what clang++ refuses: a type declared in an anonymous union.
    CLANGXX=$(printf '%s' "$CLANG" |
              sed 's/clang\(-[0-9]*\)\{0,1\}$/clang++\1/')
    set -- "-DCMAKE_C_COMPILER=$CLANG" "-DCMAKE_ASM_COMPILER=$CLANG" \
           "-DCMAKE_CXX_COMPILER=$CLANGXX" \
           -DFNL_NATIVE_ARCH=OFF -DFNL_WERROR=ON "$@" ;;
asan)
    SAN="-fsanitize=address,undefined"
    set -- -DCMAKE_BUILD_TYPE=Debug -DFNL_NATIVE_ARCH=OFF \
           "-DCMAKE_C_FLAGS=$SAN -fno-omit-frame-pointer" \
           "-DCMAKE_EXE_LINKER_FLAGS=$SAN" \
           "-DCMAKE_SHARED_LINKER_FLAGS=$SAN" "$@" ;;
tsan)
    set -- -DCMAKE_BUILD_TYPE=Debug -DFNL_NATIVE_ARCH=OFF \
           "-DCMAKE_C_FLAGS=-fsanitize=thread" \
           "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread" \
           "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread" "$@" ;;
analyzer)
    set -- -DFNL_NATIVE_ARCH=OFF -DFNL_HARDEN=OFF \
           "-DCMAKE_C_FLAGS=-fanalyzer" "$@" ;;
gc)
    # --gc-sections is the check: the tree has to link with every
    # unreferenced section discarded. --print-gc-sections is the report, and
    # opt-in because nearly all of it is the Vulkan headers' constants.
    #
    # An environment variable and not a pipe through grep: dash has no
    # `pipefail`, so a filter would hand `set -e` its own exit status and a
    # failing build would report success.
    if [ "${FNL_GC_PRINT:-}" = 1 ]; then
        GC_PRINT=" -Wl,--print-gc-sections"
    else
        GC_PRINT=
    fi
    # The cross tree's one runtime check, in trap mode, which needs no
    # runtime library: mingw-w64 has none, and this is otherwise the only
    # tree in which win32.c runs under any checker. A trap is an illegal
    # instruction, which a round reports as a crash.
    GC_C="-ffunction-sections -fdata-sections"
    GC_LINK="-Wl,--gc-sections$GC_PRINT"
    case $TARGET in
    win64) GC_C="$GC_C -fsanitize=undefined -fsanitize-trap=all" ;;
    # Mach-O keeps each function apart already; ld64 strips by -dead_strip.
    # The linker named again: these flags take the place of the toolchain
    # file's _INIT ones, which are what choose lld.
    macos|macos-arm64) GC_C=; GC_LINK="-fuse-ld=lld -Wl,-dead_strip" ;;
    esac
    set -- -DCMAKE_BUILD_TYPE=Debug -DFNL_NATIVE_ARCH=OFF \
           "-DCMAKE_C_FLAGS=$GC_C" \
           "-DCMAKE_EXE_LINKER_FLAGS=$GC_LINK" \
           "-DCMAKE_SHARED_LINKER_FLAGS=$GC_LINK" \
           "$@" ;;
esac

# ---- the target flags ------------------------------------------------------
# The backend and the renderer are always named, so no tree depends on
# what the cache default was on the day it was configured.
case $TARGET in
linux)
    set -- -DFNL_BACKEND=linux -DFNL_RENDERER=all "$@" ;;
win64)
    # mkfont links FreeType and runs *during* the build; compiled for Windows
    # it would not run. The cross tree takes the host release tree's bake,
    # on the terms a native checker tree does: there, and not stale.
    HOST_GEN="$PWD/build/linux/release/generated"
    if ! font_ready "$HOST_GEN"; then
        echo "build.sh: $TARGET needs the host build first, for a baked" >&2
        echo "          font no older than what it is baked from and" >&2
        echo "          checked by verifyfont (FNL_VERIFY_FONT on):" >&2
        echo "          ./build.sh linux release -DFNL_VERIFY_FONT=ON" >&2
        exit 1
    fi
    # Direct3D 12's shaders are DXIL, which Microsoft's dxc compiles and
    # signs; tools/dxc.py fetches the pinned release once.
    DXC=$(python3 tools/dxc.py --path)
    set -- -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
           -DFNL_RENDERER=all "-DFNL_GENERATED_DIR=$HOST_GEN" "-DFNL_DXC=$DXC" \
           "$@" ;;
macos|macos-arm64)
    # The same borrowed bake as the Windows trees, for the same reason, and
    # Apple's SDK, copied once from a Mac (tools/macos_sdk.py). One toolchain
    # file for each processor, the arm64 one the x86-64 one included.
    HOST_GEN="$PWD/build/linux/release/generated"
    if ! font_ready "$HOST_GEN"; then
        echo "build.sh: $TARGET needs the host build first, for a baked" >&2
        echo "          font no older than what it is baked from and" >&2
        echo "          checked by verifyfont (FNL_VERIFY_FONT on):" >&2
        echo "          ./build.sh linux release -DFNL_VERIFY_FONT=ON" >&2
        exit 1
    fi
    SDK=$(python3 tools/macos_sdk.py --path)
    TOOLCHAIN=cmake/macos-x86_64.cmake
    if [ "$TARGET" = macos-arm64 ]; then
        TOOLCHAIN=cmake/macos-arm64.cmake
    fi
    set -- "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN" \
           "-DFNL_MACOS_SDK=$SDK" -DFNL_RENDERER=all \
           "-DFNL_GENERATED_DIR=$HOST_GEN" "$@" ;;
linux-arm64)
    # The same borrowed bake again; the compiler is Ubuntu's aarch64 gcc
    # (cmake/linux-aarch64.cmake says which packages).
    HOST_GEN="$PWD/build/linux/release/generated"
    if ! font_ready "$HOST_GEN"; then
        echo "build.sh: $TARGET needs the host build first, for a baked" >&2
        echo "          font no older than what it is baked from and" >&2
        echo "          checked by verifyfont (FNL_VERIFY_FONT on):" >&2
        echo "          ./build.sh linux release -DFNL_VERIFY_FONT=ON" >&2
        exit 1
    fi
    if ! have_arm64; then
        echo "build.sh: $TARGET needs gcc-aarch64-linux-gnu and" >&2
        echo "          g++-aarch64-linux-gnu" >&2
        exit 1
    fi
    set -- -DCMAKE_TOOLCHAIN_FILE=cmake/linux-aarch64.cmake \
           -DFNL_BACKEND=linux -DFNL_RENDERER=all \
           "-DFNL_GENERATED_DIR=$HOST_GEN" "$@" ;;
esac

# ---- whose font ------------------------------------------------------------
# A native checker tree takes its own target's release bake when that can be
# lent, and bakes its own when it cannot -- so one tree asked for on a clean
# checkout still builds unaided. Release is the lender. The analyser tree
# always bakes: it is where -fanalyzer reads mkfont.c and verifyfont.c.
#
# Always passed, empty included, or a tree would go on borrowing from a
# lender its cache remembers and the disk no longer has.
#
# Empty in a cross tree, whose bake is FNL_GENERATED_DIR's: a remembered
# FNL_FONT_DIR would take its place.
case $TARGET:$VARIANT in
win64:*|macos:*|macos-arm64:*|linux-arm64:*|*:release|*:analyzer)
    set -- -DFNL_FONT_DIR:PATH= "$@" ;;
*)
    LENDER="$PWD/build/$TARGET/release/generated"
    if font_ready "$LENDER"; then
        set -- "-DFNL_FONT_DIR:PATH=$LENDER" "$@"
    else
        set -- -DFNL_FONT_DIR:PATH= "$@"
    fi ;;
esac

cmake -S . -B "$DIR" "$@"
cmake --build "$DIR" -j
# MoltenVK beside the macOS programs, which is what Vulkan is on a Mac
# (tools/moltenvk.py). A fetch that fails fails the build, as a pack that
# fails does.
if [ "$TARGET" = macos ] || [ "$TARGET" = macos-arm64 ]; then
    python3 tools/moltenvk.py --into "$DIR"
fi
layering
dist
