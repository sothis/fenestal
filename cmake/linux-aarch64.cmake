# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the examples for 64-bit ARM Linux (the
# Raspberry Pi 4), with Ubuntu's aarch64 cross gcc and its own glibc.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh linux-arm64           # which is these two lines, spelled out:
#
#   cmake -S . -B build/linux-arm64/release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/linux-aarch64.cmake \
#         -DFNL_GENERATED_DIR=$PWD/build/linux/release/generated
#   cmake --build build/linux-arm64/release -j
#
# gcc, as the native tree is: gcc-aarch64-linux-gnu and g++-aarch64-linux-gnu
# (the public headers' C++ check), which bring libc6-dev-arm64-cross. A
# binary needs no newer glibc than that one, which is the Pi's (2.43 both).
# FNL_GENERATED_DIR is the font, as for Windows: tools/mkfont runs during
# the build, and this build would compile it for ARM.
#
# The runtimes the library opens at run time are wanted for their headers
# alone, and those are the host's: Wayland's, EGL's, xcb's, GL's and
# Vulkan's do not depend on the processor, and this gcc searches
# /usr/include after its own glibc's, never before. Hence INCLUDE BOTH
# below, and no ARM copies of any of them. Nothing in the tree links the
# Vulkan loader; the one a caller may link is a stub made at configure
# (CMakeLists.txt), answered on the Pi by its own.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_PREFIX aarch64-linux-gnu)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}-gcc)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Programs from the host (glslangValidator, wayland-scanner, spirv-cross);
# libraries only from the target's, so no x86-64 library is linked into an
# ARM binary; headers from either, the target's first.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Nothing built here runs here, and a build RPATH would put this tree's
# directories -- the Vulkan stub's among them -- in every program the Pi is
# sent.
set(CMAKE_SKIP_BUILD_RPATH ON)
