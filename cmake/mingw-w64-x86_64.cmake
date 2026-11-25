# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the examples for x86-64 Windows with a
# mingw-w64 compiler that targets the Universal CRT.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh win64                 # which is these two lines, spelled out
#
#   cmake -S . -B build/win64/release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#         -DFNL_GENERATED_DIR=$PWD/build/linux/release/generated \
#         -DFNL_DXC=$(python3 tools/dxc.py --path)
#   cmake --build build/win64/release -j
#
# FNL_GENERATED_DIR is not optional and the configure step says so: the font
# is baked by a tool that runs *during* the build, which a cross build would
# compile for Windows. The shaders need no host build -- SPIR-V is
# architecture-independent and glslangValidator on PATH is the host's.
#
# **The compiler must target the UCRT.** Ubuntu's mingw-w64 is configured
# for msvcrt and will not do, though its sysroot has the UCRT libraries; the
# one that does is MSYS2's UCRT64 or llvm-mingw's UCRT release, named by
# FNL_MINGW_PREFIX as a path and prefix:
#
#   -DFNL_MINGW_PREFIX=/opt/llvm-mingw/bin/x86_64-w64-mingw32-
#
# Unset, the two usual names are probed and the first whose mingw-w64
# headers say _UCRT (or name the UCRT's __MSVCRT_VERSION__) is taken. That is
# a header's answer and not a builtin macro's, so the probe includes
# <_mingw.h>. build.sh refuses before configuring and CMakeLists.txt probes
# again, so a configure that did not come through build.sh cannot fall back
# to msvcrt silently.

if(NOT FNL_MINGW_PREFIX AND DEFINED ENV{FNL_MINGW_PREFIX} AND
   NOT "$ENV{FNL_MINGW_PREFIX}" STREQUAL "")
    set(FNL_MINGW_PREFIX "$ENV{FNL_MINGW_PREFIX}")
endif()

if(FNL_MINGW_PREFIX)
    set(TOOLCHAIN_PREFIX "${FNL_MINGW_PREFIX}")
else()
    foreach(_cand x86_64-w64-mingw32ucrt-gcc x86_64-w64-mingw32-gcc)
        # find_program() caches its answer and skips the search when one is
        # there, so without this the second name would re-probe the first
        # compiler for ever -- and the cache survives the failed configure.
        unset(_cand_path CACHE)
        find_program(_cand_path ${_cand})
        if(NOT _cand_path)
            continue()
        endif()
        execute_process(COMMAND ${_cand_path} -E -dM -include _mingw.h
                        -x c /dev/null
                        OUTPUT_VARIABLE _macros ERROR_QUIET)
        if(_macros MATCHES "#define _UCRT" OR
           _macros MATCHES "#define __MSVCRT_VERSION__ 0x[Ee]00")
            get_filename_component(_cand_dir ${_cand_path} DIRECTORY)
            get_filename_component(_cand_name ${_cand_path} NAME)
            string(REGEX REPLACE "gcc(-[0-9.]+)?$" "" _cand_prefix
                   "${_cand_name}")
            if(_cand_dir STREQUAL "/usr/bin")
                # A distribution's, whose sysroot is under /usr: name it by
                # the prefix alone so PATH finds it and the root below is
                # its.
                set(TOOLCHAIN_PREFIX "${_cand_prefix}")
                string(REGEX REPLACE "-$" "" _root_prefix
                       "${_cand_prefix}")
                set(_fnl_mingw_root "/usr/${_root_prefix}")
            else()
                # Somewhere of its own, as llvm-mingw keeps its sysroot.
                set(TOOLCHAIN_PREFIX "${_cand_dir}/${_cand_prefix}")
            endif()
            break()
        endif()
    endforeach()
    if(NOT TOOLCHAIN_PREFIX)
        message(FATAL_ERROR
            "no UCRT-targeted mingw-w64 compiler: this tree links ucrtbase "
            "and not msvcrt.dll. Use MSYS2's UCRT64, llvm-mingw's UCRT "
            "release, or -DFNL_MINGW_PREFIX=<path><prefix>")
    endif()
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}windres)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}ar)
set(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}ranlib)
set(CMAKE_DLLTOOL      ${TOOLCHAIN_PREFIX}dlltool)

# A self-contained toolchain (llvm-mingw's) knows its own sysroot; the
# system-prefix one needs the root named. Naming Ubuntu's under a
# self-contained compiler would mix msvcrt's libraries into the search.
if(NOT FNL_MINGW_PREFIX AND _fnl_mingw_root)
    set(CMAKE_FIND_ROOT_PATH ${_fnl_mingw_root})
endif()

# Programs come from the host; everything else only from the target sysroot,
# so a stray host library cannot be linked into a Windows binary.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# _WIN32_WINNT, WINVER and __USE_MINGW_ANSI_STDIO are CMakeLists.txt's,
# after project(): CMAKE_C_FLAGS_INIT here would be replaced by any
# -DCMAKE_C_FLAGS, which build.sh's checker trees pass.
