# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the examples for x86-64 macOS, on Linux, with
# clang and ld64.lld against Apple's SDK.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh macos                 # which is these two lines, spelled out:
#
#   cmake -S . -B build/macos/release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/macos-x86_64.cmake \
#         -DFNL_MACOS_SDK=$(python3 tools/macos_sdk.py --path) \
#         -DFNL_GENERATED_DIR=$PWD/build/linux/release/generated
#   cmake --build build/macos/release -j
#
# The SDK is the headers and the .tbd stubs of the system's libraries and
# frameworks, copied once from a Mac (tools/macos_sdk.py). FNL_GENERATED_DIR
# is the font, as for Windows: tools/mkfont runs during the build, and this
# build would compile it for macOS. The shaders need no host build.
#
# Apple's own toolchain is not used, and nothing here runs on the Mac: the
# compiler is clang-21, the version the clang trees use, and the linker is
# LLVM's Mach-O one. DESIGN *Cross-compiling for macOS*.

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_OSX_ARCHITECTURES x86_64)
# Ventura, the last macOS whose own drivers drive schenker's HD 630, and so
# the oldest that has run here; newer than the API this tree calls needs
# (CAMetalLayer and performWindowDragWithEvent: are 10.11 to 10.15), and
# what Apple still ships an SDK for.
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)

# CMake's probe projects read this file again with none of the cache: the
# small ones are handed the SDK (CMAKE_TRY_COMPILE_PLATFORM_VARIABLES,
# below), and a whole one -- the LTO check's -- takes it from the
# environment, which every cmake this one starts inherits.
if(NOT FNL_MACOS_SDK AND DEFINED ENV{FNL_MACOS_SDK})
    set(FNL_MACOS_SDK $ENV{FNL_MACOS_SDK})
endif()
if(FNL_MACOS_SDK)
    set(ENV{FNL_MACOS_SDK} ${FNL_MACOS_SDK})
else()
    message(FATAL_ERROR "the macOS trees are built against Apple's SDK, and "
                        "FNL_MACOS_SDK names none: "
                        "-DFNL_MACOS_SDK=$(python3 tools/macos_sdk.py --path)")
endif()
set(CMAKE_OSX_SYSROOT ${FNL_MACOS_SDK})
# Handed on to CMake's own probe projects, which read this file again and
# see none of the cache otherwise.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES FNL_MACOS_SDK)

# FNL_CLANG, else clang-21, as build.sh's clang trees find it.
if(DEFINED ENV{FNL_CLANG} AND NOT "$ENV{FNL_CLANG}" STREQUAL "")
    set(_fnl_clang $ENV{FNL_CLANG})
else()
    set(_fnl_clang clang-21)
endif()
set(CMAKE_C_COMPILER    ${_fnl_clang})
set(CMAKE_OBJC_COMPILER ${_fnl_clang})
set(CMAKE_ASM_COMPILER  ${_fnl_clang})
# C++ only for the public headers' check, which compiles them as C++ too:
# a C++ caller on a Mac meets clang++ first.
string(REGEX REPLACE "clang(-[0-9]+)?$" "clang++\\1" _fnl_clangxx
       "${_fnl_clang}")
set(CMAKE_CXX_COMPILER ${_fnl_clangxx})
set(_fnl_triple x86_64-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET})
set(CMAKE_C_COMPILER_TARGET    ${_fnl_triple})
set(CMAKE_OBJC_COMPILER_TARGET ${_fnl_triple})
set(CMAKE_ASM_COMPILER_TARGET  ${_fnl_triple})
set(CMAKE_CXX_COMPILER_TARGET  ${_fnl_triple})

# The LLVM tools beside the compiler. llvm-link and opt are the archive's
# (CMakeLists.txt *The archive*): ld64.lld has no partial link, so the
# library's objects are bitcode and are merged and made local before they
# are compiled to the one object. LLVM's names only, never a bare one: a
# missing llvm-link would otherwise be coreutils' link, and ar, ranlib and nm
# binutils', none of which reads Mach-O or bitcode. opt is the one LLVM tool
# packaged without the prefix. -DFNL_LLVM_<TOOL>=<path> names one by hand.
foreach(_t ar ranlib install-name-tool link opt nm)
    string(TOUPPER ${_t} _v)
    string(REPLACE "-" "_" _v ${_v})
    set(_names llvm-${_t}-21 llvm-${_t})
    if(_t STREQUAL "opt")
        list(APPEND _names opt-21)
    endif()
    find_program(FNL_LLVM_${_v} NAMES ${_names} NO_CMAKE_FIND_ROOT_PATH)
    if(NOT FNL_LLVM_${_v})
        string(REPLACE ";" " or " _names "${_names}")
        message(FATAL_ERROR "the macOS trees need LLVM's ${_t} (${_names}), "
                            "and none is on PATH: install LLVM 21's tools, "
                            "or name one with -DFNL_LLVM_${_v}=<path>")
    endif()
endforeach()
set(CMAKE_AR                ${FNL_LLVM_AR})
set(CMAKE_RANLIB            ${FNL_LLVM_RANLIB})
set(CMAKE_INSTALL_NAME_TOOL ${FNL_LLVM_INSTALL_NAME_TOOL})

foreach(_k EXE SHARED MODULE)
    string(APPEND CMAKE_${_k}_LINKER_FLAGS_INIT " -fuse-ld=lld")
endforeach()

set(CMAKE_FIND_ROOT_PATH ${CMAKE_OSX_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
