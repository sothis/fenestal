# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the examples for macOS on Apple's own
# processors, on Linux: cmake/macos-x86_64.cmake with the architecture
# changed, and nothing else.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh macos-arm64           # which is these two lines, spelled out:
#
#   cmake -S . -B build/macos-arm64/release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/macos-arm64.cmake \
#         -DFNL_MACOS_SDK=$(python3 tools/macos_sdk.py --path) \
#         -DFNL_GENERATED_DIR=$PWD/build/linux/release/generated
#   cmake --build build/macos-arm64/release -j
#
# CMake's probe projects read this file again, so the architecture is set
# here and not passed: a -D would reach the main project alone.

set(FNL_MACOS_ARCH arm64)
include(${CMAKE_CURRENT_LIST_DIR}/macos-x86_64.cmake)
