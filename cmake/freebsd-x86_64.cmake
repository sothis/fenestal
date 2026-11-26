# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the examples for 64-bit FreeBSD, on Linux,
# with clang and lld against a FreeBSD sysroot.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh freebsd               # which is these two lines, spelled out
#
#   cmake -S . -B build/freebsd/release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/freebsd-x86_64.cmake \
#         -DCMAKE_OBJCOPY=$(command -v llvm-objcopy) \
#         -DFNL_GENERATED_DIR=$PWD/build/linux/release/generated
#   cmake --build build/freebsd/release -j
#
# FNL_FREEBSD_SYSROOT is the base system's headers and libraries and the
# ports' -- Wayland, xcb, libdrm, GL, Vulkan's headers -- fetched once by
# tools/freebsd_sysroot.py, which build.sh and this file both question. The
# configure also takes it from the environment, or runs the script, so a
# hand-run cmake fetches it as `--path` does.
#
# FNL_GENERATED_DIR is the font, as for Windows and the Mac: tools/mkfont
# runs during the build, and this build would compile it for FreeBSD. The
# shaders need no host build.
#
# clang is the host's, told the target by the triple; the linker is lld,
# which pairs with it and needs no FreeBSD binutils. Nothing built here runs
# here: this is a tree to copy to a FreeBSD machine (tools/drive's bare
# driver does), not to launch.
#
# The target system's version is the sysroot's own: a binary names the
# FreeBSD it was linked against, as it does on Linux.

if(NOT FNL_FREEBSD_SYSROOT AND DEFINED ENV{FNL_FREEBSD_SYSROOT} AND
   NOT "$ENV{FNL_FREEBSD_SYSROOT}" STREQUAL "")
    set(FNL_FREEBSD_SYSROOT "$ENV{FNL_FREEBSD_SYSROOT}")
endif()
if(NOT FNL_FREEBSD_SYSROOT)
    execute_process(COMMAND python3
                    ${CMAKE_CURRENT_LIST_DIR}/../tools/freebsd_sysroot.py
                    --path
                    OUTPUT_VARIABLE FNL_FREEBSD_SYSROOT
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _fnl_fb_rc)
    if(NOT _fnl_fb_rc EQUAL 0 OR NOT FNL_FREEBSD_SYSROOT)
        message(FATAL_ERROR
            "the FreeBSD tree links against a FreeBSD sysroot, and none "
            "was named: -DFNL_FREEBSD_SYSROOT=$(python3 "
            "tools/freebsd_sysroot.py --path)")
    endif()
endif()
if(NOT EXISTS ${FNL_FREEBSD_SYSROOT}/usr/include/sys/event.h)
    message(FATAL_ERROR "${FNL_FREEBSD_SYSROOT} is not a FreeBSD sysroot: "
                        "tools/freebsd_sysroot.py --fetch makes one")
endif()

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT ${FNL_FREEBSD_SYSROOT})

# FNL_CLANG, else clang-21, as build.sh's clang trees find it. C++ for the
# public headers' check, which compiles them as C++ too.
if(DEFINED ENV{FNL_CLANG} AND NOT "$ENV{FNL_CLANG}" STREQUAL "")
    set(_fnl_clang $ENV{FNL_CLANG})
else()
    set(_fnl_clang clang-21)
endif()
string(REGEX REPLACE "clang(-[0-9]+)?$" "clang++\\1" _fnl_clangxx
       "${_fnl_clang}")
set(CMAKE_C_COMPILER    ${_fnl_clang})
set(CMAKE_CXX_COMPILER  ${_fnl_clangxx})
set(CMAKE_ASM_COMPILER  ${_fnl_clang})
# The sysroot names the release the headers are, and the binary records it.
set(_fnl_triple x86_64-unknown-freebsd15.1)
set(CMAKE_C_COMPILER_TARGET   ${_fnl_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_fnl_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_fnl_triple})
# lld: the host's clang would otherwise drive its GNU ld, which reads none of
# a FreeBSD image's notes. The archive's objcopy is llvm's, which build.sh
# passes: base's elftoolchain one takes no --keep-global-symbols.
foreach(_k EXE SHARED MODULE)
    string(APPEND CMAKE_${_k}_LINKER_FLAGS_INIT " -fuse-ld=lld")
endforeach()

# The ports' pkg-config files are the target's, and the prefix they name is
# the sysroot's: pkg-config prepends the sysroot to what it prints, and its
# search is these directories and no other -- the host's would offer this
# machine's Wayland and glibc's headers.
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${FNL_FREEBSD_SYSROOT})
set(_fnl_fb_pcdir "${FNL_FREEBSD_SYSROOT}/usr/local/libdata/pkgconfig")
foreach(_d usr/libdata/pkgconfig usr/local/share/pkgconfig usr/share/pkgconfig)
    string(APPEND _fnl_fb_pcdir ":${FNL_FREEBSD_SYSROOT}/${_d}")
endforeach()
set(ENV{PKG_CONFIG_LIBDIR} "${_fnl_fb_pcdir}")
set(ENV{PKG_CONFIG_PATH} "")

set(CMAKE_FIND_ROOT_PATH ${FNL_FREEBSD_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Nothing built here runs here, and a build RPATH would put this tree's
# directories -- the Vulkan stub's among them -- in every program the
# machine is sent.
set(CMAKE_SKIP_BUILD_RPATH ON)
