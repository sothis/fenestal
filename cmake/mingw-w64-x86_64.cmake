# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the example for x86-64 Windows with mingw-w64.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh win64                 # which is these two lines, spelled out:
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

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}-ranlib)
set(CMAKE_DLLTOOL      ${TOOLCHAIN_PREFIX}-dlltool)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Programs come from the host; everything else only from the target sysroot,
# so a stray host library cannot be linked into a Windows binary.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# _WIN32_WINNT, WINVER and __USE_MINGW_ANSI_STDIO are CMakeLists.txt's,
# after project(): CMAKE_C_FLAGS_INIT here would be replaced by any
# -DCMAKE_C_FLAGS, which build.sh's checker trees pass.
