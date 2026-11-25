#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The build, as one command.
#
#   ./build.sh                        every tree this host can build
#   ./build.sh linux                  that target, release: Wayland, X11
#                                     and KMS, drawn with Vulkan and OpenGL
#   ./build.sh win64                  that target, release: Win32 with
#                                     mingw-w64 on the Universal CRT, drawn
#                                     with Direct3D 12 and Vulkan
#   ./build.sh win64-msvc             the same, built natively by MSVC
#   ./build.sh macos                  that target: Cocoa drawn with Metal,
#                                     Vulkan on MoltenVK and OpenGL 4.1
#   ./build.sh macos-arm64            the macos tree for Apple's own
#                                     processors, cross-compiled
#   ./build.sh macos-universal        the two macOS trees joined into one
#                                     set of universal binaries
#   ./build.sh freebsd                FreeBSD, native: Wayland, X11 and KMS,
#                                     drawn with Vulkan and OpenGL
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
        win64-msvc:release|win64-msvc:gc) return 1 ;;
        win64-msvc:*) return 0 ;;
        macos:release|macos:gc|macos-arm64:release|macos-arm64:gc)
            return 1 ;;
        macos:*|macos-arm64:*) return 0 ;;
        macos-universal:release|macos-universal:gc) return 1 ;;
        macos-universal:*) return 0 ;;
        freebsd:release|freebsd:asan|freebsd:tsan|freebsd:gc) return 1 ;;
        freebsd:*) return 0 ;;
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

# ---- the host --------------------------------------------------------------
# A sweep builds what this machine can: the cross trees on Linux, the native
# trees on FreeBSD, Windows and macOS. One tree asked for by name is still
# checked by its own target case, below.
host_windows() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT) return 0 ;;
    esac
    return 1
}

have_freebsd() {
    [ "$(uname -s)" = FreeBSD ]
}

have_msvc() {
    command -v cl >/dev/null 2>&1 || command -v cl.exe >/dev/null 2>&1
}

# The mingw tree links the Universal CRT, not msvcrt.dll: a compiler built
# for it ships mingw-w64's headers configured for the UCRT, whose _mingw.h
# defines _UCRT and names the runtime's __MSVCRT_VERSION__ 0xE00. That is a
# header's to say, not a builtin macro's, so the probe includes it.
# Ubuntu's mingw is built for msvcrt and answers neither; llvm-mingw's UCRT
# release and MSYS2's UCRT64 do. FNL_MINGW_PREFIX names one by path and
# prefix, as the toolchain file takes it:
# /opt/llvm-mingw/bin/x86_64-w64-mingw32-.
find_ucrt_gcc() {
    if [ -n "${FNL_MINGW_PREFIX:-}" ]; then
        p="${FNL_MINGW_PREFIX}gcc"
        [ -x "$p" ] || return 1
        "$p" -E -dM -include _mingw.h -x c /dev/null 2>/dev/null |
            grep -qE '^#define (_UCRT|__MSVCRT_VERSION__ 0x[Ee]00)' &&
            { printf '%s\n' "$p"; return 0; }
        return 1
    fi
    for c in x86_64-w64-mingw32ucrt-gcc x86_64-w64-mingw32-gcc; do
        p=$(command -v "$c" 2>/dev/null) || continue
        if "$p" -E -dM -include _mingw.h -x c /dev/null 2>/dev/null |
           grep -qE '^#define (_UCRT|__MSVCRT_VERSION__ 0x[Ee]00)'; then
            printf '%s\n' "$p"
            return 0
        fi
    done
    return 1
}

have_ucrt() {
    find_ucrt_gcc >/dev/null 2>&1
}

# Whether the configure will have Direct3D 12 in its renderers: Windows' set
# is all of them, or the last -DFNL_RENDERER= among the flags after the
# target and variant. Only a set with the renderer compiles DXIL and wants
# dxc, and fetching one for a -DFNL_RENDERER=vulkan tree would fail a build
# that never uses it (CMakeLists.txt demands FNL_DXC where d3d12 is).
want_d3d12() {
    renderers=all
    for a in "$@"; do
        case $a in
        -DFNL_RENDERER=*) renderers=${a#*=} ;;
        esac
    done
    case $renderers in
    all|d3d12) return 0 ;;
    *) return 1 ;;
    esac
}

# llvm-mingw's UCRT release is clang, which has no gcc -fanalyzer: the win64
# analyser tree needs a gcc-based UCRT compiler (MSYS2's UCRT64, or Debian's
# UCRT variant where a distribution has one).
ucrt_is_clang() {
    g=$(find_ucrt_gcc) || return 1
    "$g" --version 2>/dev/null | grep -qi clang
}

# clang's MinGW link is lld's, and lld's MinGW driver has no -r; llvm-objcopy
# takes no --keep-global-symbols on COFF either. So the clang UCRT tree's
# archive, one object with the API its only globals, needs binutils' PE ld
# and objcopy. They know nothing of the CRT: the UCRT64 toolchain's or a
# host's msvcrt-flavoured ones do. FNL_MINGW_LD and FNL_MINGW_OBJCOPY name
# them; a tool that answers LLD is not one.
find_mingw_ld() {
    for t in "${FNL_MINGW_LD:-}" x86_64-w64-mingw32-ld; do
        [ -n "$t" ] || continue
        p=$(command -v "$t" 2>/dev/null) || continue
        "$p" --version 2>/dev/null | grep -qi 'LLD\|LLVM' && continue
        printf '%s\n' "$p"
        return 0
    done
    return 1
}

find_mingw_objcopy() {
    for t in "${FNL_MINGW_OBJCOPY:-}" x86_64-w64-mingw32-objcopy; do
        [ -n "$t" ] || continue
        p=$(command -v "$t" 2>/dev/null) || continue
        "$p" --version 2>/dev/null | grep -qi 'LLD\|LLVM' && continue
        printf '%s\n' "$p"
        return 0
    done
    return 1
}

need_mingw_binutils() {
    echo "build.sh: the UCRT compiler found is clang (llvm-mingw), whose" >&2
    echo "          lld has no relocatable link; the archive needs" >&2
    echo "          binutils' PE ld and objcopy (binutils-mingw-w64, or" >&2
    echo "          MSYS2's binutils package)." >&2
    exit 1
}

# lipo, for joining the two macOS trees. LLVM's name carries the version
# here, as the toolchain file's other tools do.
find_lipo() {
    for l in "${FNL_LIPO:-}" llvm-lipo-21 llvm-lipo lipo; do
        [ -n "$l" ] && command -v "$l" >/dev/null 2>&1 && {
            command -v "$l"; return 0; }
    done
    return 1
}

# Whether any of these tools is on PATH. The macOS toolchain file probes
# each by these names (its `_names`), so the answer here is the same one the
# configure would give.
have_tool() {
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 && return 0
    done
    return 1
}

# The LLVM tool the macOS toolchain file would take: FNL_LLVM_<NAME> from the
# environment first, then the names its foreach probes -- llvm-<tool>-21 and
# llvm-<tool>, with opt-21 for opt. That foreach in
# cmake/macos-x86_64.cmake is the list; the two are kept together.
have_llvm_tool() {
    _var=$1
    _tool=$2
    eval "_path=\${FNL_LLVM_${_var}:-}"
    if [ -n "$_path" ]; then
        [ -x "$_path" ]
        return
    fi
    if [ "$_tool" = opt ]; then
        have_tool llvm-opt-21 llvm-opt opt-21
    else
        have_tool "llvm-${_tool}-21" "llvm-${_tool}"
    fi
}

# Whether the macOS trees can be built here: Apple's SDK, copied once from a
# Mac by tools/macos_sdk.py, and the tools their toolchain file names -- the
# compiler and the C++ driver it derives, the six LLVM tools it probes, and
# ld64.lld, which its -fuse-ld=lld has clang drive for a Mach-O target. The
# configure refuses with a line naming what is missing; the sweep asks the
# same things first, so a host without them skips the trees with a line
# instead of failing after the ones before it built.
have_macos() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 tools/macos_sdk.py --path >/dev/null 2>&1 || return 1
    _c="${FNL_CLANG:-clang-21}"
    command -v "$_c" >/dev/null 2>&1 || return 1
    # The C++ driver, as the toolchain's regex derives it: clang-N ->
    # clang++-N, clang -> clang++.
    case $_c in
    *clang-[0-9]*) _cxx="${_c%clang-*}clang++-${_c##*clang-}" ;;
    *clang) _cxx="${_c%clang}clang++" ;;
    *) _cxx=clang++ ;;
    esac
    command -v "$_cxx" >/dev/null 2>&1 || return 1
    for _t in ar ranlib install-name-tool link opt nm; do
        _v=$(printf '%s' "$_t" | tr 'a-z-' 'A-Z_')
        have_llvm_tool "$_v" "$_t" || return 1
    done
    have_tool ld64.lld-21 ld64.lld || return 1
    return 0
}

# llvm-objcopy, which the FreeBSD archive's --keep-global-symbols needs:
# base's elftoolchain objcopy does not take it.
find_llvm_objcopy() {
    for o in "${FNL_OBJCOPY:-}" llvm-objcopy llvm-objcopy-21 \
             llvm-objcopy21 llvm-objcopy20 llvm-objcopy19 llvm-objcopy18; do
        [ -n "$o" ] && command -v "$o" >/dev/null 2>&1 && {
            command -v "$o"; return 0; }
    done
    # FreeBSD's devel/llvm installs its tools under /usr/local/llvm<ver>/bin,
    # which PATH need not name. bin/ alone: a man page or a directory's
    # name would otherwise be taken for the tool.
    p=$(find /usr/local -type f -path '*/bin/llvm-objcopy*' 2>/dev/null |
        head -n 1)
    [ -n "$p" ] && { printf '%s\n' "$p"; return 0; }
    return 1
}

# ---- the baked font --------------------------------------------------------
# tools/mkfont's output is the same bytes in every native tree, so a tree may
# take a release tree's instead of baking its own. Whether that tree's bake
# can be lent: all four files are there, verifyfont as it stands now passed
# on them since they were written, and none of what the bake reads is newer
# than they are. mkfont writes the files before verifyfont reads them, and a
# borrower checks nothing, so a bake whose check failed or was turned off
# (FNL_VERIFY_FONT) is not lent; nor is a stale one, an old atlas under new
# sources. The answer then is no: a native tree bakes its own, and the
# cross tree, which cannot, is refused with the command that makes one.
font_ready() {
    for f in font_data.h font_metrics.h font_atlas.bin font_cells.bin; do
        [ -f "$1/$f" ] || return 1
    done
    [ -n "$(find "$1/font_verified.stamp" -newer "$1/font_atlas.bin" \
            -newer tools/verifyfont.c 2>/dev/null)" ] || return 1
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
    # Not by comparing paths: MSYS2 spells the same directory C:/... from git
    # and /c/... from pwd. The prefix is empty exactly at the top of a
    # checkout.
    if [ "$(git rev-parse --is-inside-work-tree 2>/dev/null)" != true ] ||
       [ -n "$(git rev-parse --show-prefix 2>/dev/null)" ]; then
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
    # The test dist() makes, and not a comparison of paths: MSYS2 spells the
    # same directory C:/... from git and /c/... from pwd.
    if [ "$(git rev-parse --is-inside-work-tree 2>/dev/null)" != true ] ||
       [ -n "$(git rev-parse --show-prefix 2>/dev/null)" ]; then
        echo "build.sh: dist packs a git checkout's working tree, and" \
             "this is not the top of one" >&2
        exit 1
    fi
    python3 tools/mkdist.py --worktree
    python3 tools/mkdist.py --worktree --keep-comments \
        -o build/fenestal-commented.zip
    exit 0
fi

# ---- no arguments: every tree this host can build --------------------------
# A Linux host builds the cross trees, the two macOS ones and their
# universal join; a FreeBSD host builds its own tree; a Windows host builds
# the mingw UCRT64 tree and the MSVC one; a macOS host builds the two
# macOS trees and the universal join. A target whose compiler is not here
# is skipped with one line, never a failure -- as linux-arm64 has been.
#
# The order is load-bearing. Release comes first within a target because the
# checker trees and the clang tree take its baked font, and linux comes
# before the others because the cross trees take that one's and cannot
# bake at all. macos-universal comes after its two slices, which it joins.
#
# One re-entry per tree and not a loop around the body: each tree assembles
# its own configure line with `set --`, and POSIX sh has one argument list.
#
# It stops at the first failure. Nearly all of the code is shared, so going
# on would print the same error once for every tree after it.
if [ $# -eq 0 ]; then
    case "$(uname -s)" in
    FreeBSD)
        targets="freebsd" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        targets="win64 win64-msvc" ;;
    Darwin)
        targets="macos macos-arm64 macos-universal" ;;
    *)
        targets="linux linux-arm64 win64 macos macos-arm64 macos-universal" ;;
    esac
    for target in $targets; do
        case $target in
        linux-arm64)
            have_arm64 || {
                printf '\n==> linux-arm64 skipped: no aarch64-linux-gnu-gcc\n'
                continue; } ;;
        win64)
            have_ucrt || {
                printf '\n==> win64 skipped: no UCRT mingw-w64'
                printf ' compiler (FNL_MINGW_PREFIX)\n'
                continue; } ;;
        win64-msvc)
            have_msvc || {
                printf '\n==> win64-msvc skipped: no cl on PATH\n'
                continue; } ;;
        freebsd)
            have_freebsd || {
                printf '\n==> freebsd skipped: not a FreeBSD host\n'
                continue; } ;;
        macos|macos-arm64|macos-universal)
            have_macos || {
                printf '\n==> %s skipped: no macOS SDK or LLVM tools' "$target"
                printf ' (tools/macos_sdk.py --fetch)\n'
                continue; }
            # The trees take a Linux release tree's bake rather than making
            # one (tools/mkfont runs during the build), and a Mac can make
            # none: a sweep there skips them with the reason.
            font_ready "$PWD/build/linux/release/generated" || {
                printf '\n==> %s skipped: no Linux release bake to lend' \
                       "$target"
                printf ' (build/linux/release/generated)\n'
                continue; }
            [ "$target" = macos-universal ] &&
                ! find_lipo >/dev/null 2>&1 && {
                printf '\n==> macos-universal skipped: no lipo\n'
                continue; } ;;
        esac
        for variant in release clang asan tsan analyzer gc; do
            refused "$target" "$variant" && continue
            if [ "$target" = win64 ] && [ "$variant" = analyzer ] &&
               ucrt_is_clang; then
                printf '\n==> win64 analyzer skipped: the UCRT compiler'
                printf ' is clang\n'
                continue
            fi
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
    linux|win64|win64-msvc|macos|macos-arm64|macos-universal|freebsd|\
    linux-arm64) ;;
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
        echo "          linux | win64 | win64-msvc | macos |" \
             "macos-arm64 | macos-universal |" >&2
        echo "          freebsd | linux-arm64 | dist" >&2
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
    macos-universal)
        echo "build.sh: the universal tree joins the two macOS trees" >&2
        echo "          and takes release and gc." >&2
        exit 1 ;;
    win64-msvc)
        echo "build.sh: the MSVC tree takes release and gc: no" >&2
        echo "          sanitizer or analyser tree is wired for cl." >&2
        exit 1 ;;
    freebsd)
        echo "build.sh: the FreeBSD tree takes release, asan, tsan and" >&2
        echo "          gc: its release already uses clang, and the" >&2
        echo "          analyser's findings are the Linux tree's." >&2
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

# ---- the two macOS trees, joined -------------------------------------------
# Not a tree of its own: the two one-architecture trees are built (or found
# already built) and tools/macos_universal.py joins their libraries and
# programs with lipo. The flags after the variant go to both, as they would
# to either tree alone. A sweep's child skips the source checks, which the
# sweep asks once.
if [ "$TARGET" = macos-universal ]; then
    if ! LIPO=$(find_lipo); then
        echo "build.sh: the universal tree needs lipo (llvm-lipo-21," \
             "llvm-lipo or FNL_LIPO)" >&2
        exit 1
    fi
    FNL_NO_DIST=1 "$SELF" macos "$VARIANT" "$@"
    FNL_NO_DIST=1 "$SELF" macos-arm64 "$VARIANT" "$@"
    FNL_LIPO=$LIPO python3 tools/macos_universal.py \
        --variant "$VARIANT" --out "$DIR"
    if [ "${FNL_NO_DIST:-}" != 1 ]; then
        layering
        dist
    fi
    exit 0
fi

# ---- the checker flags, innermost so a command-line -D still wins ----------
# Both linker variables, because this project links an archive and a shared
# object: the executable one alone would leave libfenestal uninstrumented.
case $VARIANT in
release)
    # Said rather than left to the option's default, so a tree configured
    # when native was the default comes back to x86-64-v2; native is
    # `./build.sh <target> release -DFNL_NATIVE_ARCH=ON`, which goes last
    # and wins. The build type too: CMake's MSVC platform defaults a
    # single-config tree to Debug, and this variant is the release one.
    set -- -DFNL_NATIVE_ARCH=OFF -DCMAKE_BUILD_TYPE=Release "$@" ;;
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
    if [ "$TARGET" = win64 ] && ucrt_is_clang; then
        echo "build.sh: the win64 analyser tree needs a gcc-based UCRT" >&2
        echo "          compiler; the one found is clang's (llvm-mingw)." >&2
        exit 1
    fi
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
    win64-msvc) GC_C=; GC_LINK="/OPT:REF /OPT:ICF" ;;
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

# A -DFNL_MINGW_PREFIX= given as an argument is what the configure will take,
# so the UCRT pre-checks read it as they read the environment's.
for _a in "$@"; do
    case $_a in
    -DFNL_MINGW_PREFIX=*)
        FNL_MINGW_PREFIX=${_a#*=}
        export FNL_MINGW_PREFIX
        ;;
    esac
done

# ---- the target flags ------------------------------------------------------
# The backend and the renderer are always named, so no tree depends on
# what the cache default was on the day it was configured.
case $TARGET in
linux)
    set -- -DFNL_BACKEND=linux -DFNL_RENDERER=all "$@" ;;
win64)
    # On Windows itself this is MSYS2's UCRT64 compiler, native; on Linux it
    # is a cross compiler and must be UCRT too. Both link ucrtbase and not
    # msvcrt.dll, which is what tools/check-crt.cmake proves.
    if host_windows; then
        UCRT=$(find_ucrt_gcc) || {
            echo "build.sh: win64 needs a UCRT-targeted mingw-w64" >&2
            echo "          compiler: MSYS2's UCRT64, llvm-mingw's UCRT" >&2
            echo "          release, or FNL_MINGW_PREFIX" >&2
            exit 1; }
        # CMake is a native program here, and an MSYS path is not one it
        # reads.
        if command -v cygpath >/dev/null 2>&1; then
            UCRT=$(cygpath -w "$UCRT")
        fi
        if want_d3d12 "$@"; then
            DXC=${FNL_DXC:-$(python3 tools/dxc.py --path)}
            set -- "-DFNL_DXC=$DXC" "$@"
        fi
        if ucrt_is_clang; then
            GNU_LD=$(find_mingw_ld) || need_mingw_binutils
            GNU_OBJCOPY=$(find_mingw_objcopy) || need_mingw_binutils
            if command -v cygpath >/dev/null 2>&1; then
                GNU_LD=$(cygpath -w "$GNU_LD")
                GNU_OBJCOPY=$(cygpath -w "$GNU_OBJCOPY")
            fi
            set -- "-DFNL_MINGW_LD=$GNU_LD" \
                   "-DCMAKE_OBJCOPY=$GNU_OBJCOPY" "$@"
        fi
        set -- "-DCMAKE_C_COMPILER=$UCRT" \
               "-DCMAKE_CXX_COMPILER=${UCRT%gcc}g++" \
               "-DCMAKE_ASM_COMPILER=$UCRT" \
               -DFNL_RENDERER=all "$@"
    else
        have_ucrt || {
            echo "build.sh: win64 needs a UCRT-targeted mingw-w64" >&2
            echo "          compiler: MSYS2's UCRT64, llvm-mingw's UCRT" >&2
            echo "          release, or FNL_MINGW_PREFIX" >&2
            exit 1; }
        # mkfont links FreeType and runs *during* the build; compiled for
        # Windows it would not run. The cross tree takes the host release
        # tree's bake, on the terms a native checker tree does: there, and
        # not stale.
        WIN64_CROSS=1
        HOST_GEN="$PWD/build/linux/release/generated"
        if ! font_ready "$HOST_GEN"; then
            echo "build.sh: $TARGET needs the host build first, for a" >&2
            echo "          baked font no older than what it is baked from" >&2
            echo "          and checked by verifyfont (FNL_VERIFY_FONT on):" >&2
            echo "          ./build.sh linux release -DFNL_VERIFY_FONT=ON" >&2
            exit 1
        fi
        # Direct3D 12's shaders are DXIL, which Microsoft's dxc compiles
        # and signs; tools/dxc.py fetches the pinned release once, for a
        # set that has the renderer.
        if want_d3d12 "$@"; then
            DXC=${FNL_DXC:-$(python3 tools/dxc.py --path)}
            set -- "-DFNL_DXC=$DXC" "$@"
        fi
        if ucrt_is_clang; then
            GNU_LD=$(find_mingw_ld) || need_mingw_binutils
            GNU_OBJCOPY=$(find_mingw_objcopy) || need_mingw_binutils
            set -- "-DFNL_MINGW_LD=$GNU_LD" \
                   "-DCMAKE_OBJCOPY=$GNU_OBJCOPY" "$@"
        fi
        set -- -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
               -DFNL_RENDERER=all "-DFNL_GENERATED_DIR=$HOST_GEN" "$@"
    fi ;;
win64-msvc)
    # A native Windows build with cl: no ASM stubs (tools/bin2c.c makes C
    # arrays), no partial link (fenestal::static keeps its globals), and
    # Ninja, so build/<target>/<variant> stays one flat directory.
    if ! host_windows; then
        echo "build.sh: win64-msvc builds on Windows, where cl runs;" >&2
        echo "          on Linux the cross tree is ./build.sh win64" >&2
        exit 1
    fi
    if ! have_msvc; then
        echo "build.sh: win64-msvc needs cl on PATH (vcvars64 or the" >&2
        echo "          Visual Studio developer prompt)" >&2
        exit 1
    fi
    if want_d3d12 "$@"; then
        DXC=${FNL_DXC:-$(python3 tools/dxc.py --path)}
        set -- "-DFNL_DXC=$DXC" "$@"
    fi
    set -- -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl \
           -DFNL_RENDERER=all "$@" ;;
freebsd)
    # Native, with the base system's clang and LLVM's objcopy: the archive's
    # --keep-global-symbols is not in elftoolchain's.
    if ! have_freebsd; then
        echo "build.sh: freebsd builds on FreeBSD; the other systems'" \
             "trees are theirs" >&2
        exit 1
    fi
    OBJCOPY=$(find_llvm_objcopy) || {
        echo "build.sh: the FreeBSD archive needs llvm-objcopy" >&2
        echo "          (devel/llvm, or FNL_OBJCOPY)" >&2
        exit 1; }
    set -- -DCMAKE_C_COMPILER=clang -DCMAKE_ASM_COMPILER=clang \
           -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_OBJCOPY=$OBJCOPY" \
           -DFNL_BACKEND=freebsd -DFNL_RENDERER=all "$@" ;;
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
borrow=false
case $TARGET:$VARIANT in
macos:*|macos-arm64:*|linux-arm64:*|*:release|*:analyzer)
    borrow=false ;;
win64:*)
    # The cross tree borrows FNL_GENERATED_DIR's bake; the native UCRT64
    # tree on Windows is a native checker's, and takes its own release.
    [ "${WIN64_CROSS:-}" = 1 ] && borrow=false || borrow=true ;;
*)
    borrow=true ;;
esac
if [ -n "${FNL_FONT_DIR:-}" ]; then
    # Named by the caller, as a CI job that brought the bake from the Linux
    # tree does. Goes first, so the caller's -D on the command line wins.
    set -- "-DFNL_FONT_DIR:PATH=$FNL_FONT_DIR" "$@"
elif [ "$borrow" = true ]; then
    LENDER="$PWD/build/$TARGET/release/generated"
    if font_ready "$LENDER"; then
        set -- "-DFNL_FONT_DIR:PATH=$LENDER" "$@"
    else
        set -- -DFNL_FONT_DIR:PATH= "$@"
    fi
else
    set -- -DFNL_FONT_DIR:PATH= "$@"
fi

cmake -S . -B "$DIR" "$@"
# A count beside -j: the FreeBSD tree builds with the base system's make,
# whose -j takes an argument.
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null) || JOBS=
[ -n "$JOBS" ] || JOBS=2
cmake --build "$DIR" -j "$JOBS"
# MoltenVK beside the macOS programs, which is what Vulkan is on a Mac
# (tools/moltenvk.py). A fetch that fails fails the build, as a pack that
# fails does.
if [ "$TARGET" = macos ] || [ "$TARGET" = macos-arm64 ]; then
    python3 tools/moltenvk.py --into "$DIR"
fi
layering
dist
