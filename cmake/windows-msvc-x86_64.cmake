# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Cross-compile the library and the examples for x86-64 Windows the way the
# MSVC tree is built -- Microsoft's runtime, its headers, its linker's
# flags -- on a Linux machine, with clang-cl rather than cl.
#
#   ./build.sh linux                 # the host build first, for the bake
#   ./build.sh win64-msvc            # which is these two lines, spelled out
#
#   cmake -S . -B build/win64-msvc/release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/windows-msvc-x86_64.cmake \
#         -DFNL_GENERATED_DIR=$PWD/build/linux/release/generated \
#         -DFNL_DXC=$(python3 tools/dxc.py --path)
#   cmake --build build/win64-msvc/release -j
#
# FNL_MSVC_ROOT is the MSVC CRT and the Windows SDK, laid out by xwin
# (tools/msvc_sdk.py): crt/ and sdk/ under it. The configure also takes it
# from the environment, or runs the script, as the FreeBSD sysroot's does.
# FNL_GENERATED_DIR is the font, as every cross tree takes it.
#
# What makes this the MSVC tree and not the mingw one: clang-cl's front end
# is Microsoft's, so the sources are built with /W3, /std:c11, /guard:cf and
# the rest of CMakeLists.txt's MSVC set, and the runtime linked is the
# Universal CRT's (this SDK's) rather than mingw-w64's. The archive is the
# objects themselves, as a native cl's is: there is no relocatable link and
# no objcopy, so fenestal::static keeps its internals global, and the DLL's
# exports are the .def list. That relaxation is DESIGN's *Building Windows
# with MSVC*, and this tree takes it for the same reason.
#
# The debug CRT is not redistributable and so is not in the splat: every
# configuration links the release runtime (CMAKE_MSVC_RUNTIME_LIBRARY
# below), which is also what a gc tree's /OPT:REF then reads.
#
# lld-link is the linker and llvm-lib the librarian, which is what clang-cl
# on a machine without Visual Studio drives; llvm-rc makes the manifest
# resource and llvm-mt merges it, where a Linux `mt` is GNU cpio's.

if(NOT FNL_MSVC_ROOT AND DEFINED ENV{FNL_MSVC_ROOT} AND
   NOT "$ENV{FNL_MSVC_ROOT}" STREQUAL "")
    set(FNL_MSVC_ROOT "$ENV{FNL_MSVC_ROOT}")
endif()
if(NOT FNL_MSVC_ROOT)
    execute_process(COMMAND python3
                    ${CMAKE_CURRENT_LIST_DIR}/../tools/msvc_sdk.py --path
                    OUTPUT_VARIABLE FNL_MSVC_ROOT
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _fnl_msvc_rc)
    if(NOT _fnl_msvc_rc EQUAL 0 OR NOT FNL_MSVC_ROOT)
        message(FATAL_ERROR
            "the win64-msvc tree cross-compiles against the MSVC CRT and "
            "the Windows SDK, and none was named: -DFNL_MSVC_ROOT=$(python3 "
            "tools/msvc_sdk.py --path)")
    endif()
endif()
if(NOT EXISTS ${FNL_MSVC_ROOT}/crt/include/vcruntime.h OR
   NOT EXISTS ${FNL_MSVC_ROOT}/sdk/include/um/windows.h)
    message(FATAL_ERROR "${FNL_MSVC_ROOT} has no crt/ and sdk/: "
                        "tools/msvc_sdk.py --fetch makes them")
endif()
# Named for CMakeLists.txt, which hands them to a consumer: the runtime's
# headers and libraries are the target's, as a sysroot's are.
set(FNL_MSVC_CRT ${FNL_MSVC_ROOT}/crt)
set(FNL_MSVC_SDK ${FNL_MSVC_ROOT}/sdk)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# FNL_CLANG, else clang-21, as build.sh's clang trees find it: the same
# compiler in its MSVC front end, so a machine has one clang.
if(DEFINED ENV{FNL_CLANG} AND NOT "$ENV{FNL_CLANG}" STREQUAL "")
    set(_fnl_clang $ENV{FNL_CLANG})
else()
    set(_fnl_clang clang-21)
endif()
string(REGEX REPLACE "clang(-[0-9]+)?$" "clang-cl\\1" _fnl_clangcl
       "${_fnl_clang}")
set(CMAKE_C_COMPILER   ${_fnl_clangcl})
set(CMAKE_CXX_COMPILER ${_fnl_clangcl})
set(CMAKE_ASM_COMPILER ${_fnl_clangcl})
set(_fnl_triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER_TARGET   ${_fnl_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_fnl_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_fnl_triple})

# LLVM's own tools, by the names this machine packages them: the bare ones
# are not all on PATH (llvm-rc's is not), and `ld`, `ar` and `mt` on a Linux
# host are binutils', GNU cpio's and the GNU linker's. find_program() caches
# its answer, so each is unset first or the second tool would be the first
# again.
foreach(_t LINKER:lld-link AR:llvm-lib RC_COMPILER:llvm-rc MT:llvm-mt
        DLLTOOL:llvm-dlltool)
    string(REPLACE ":" ";" _tv "${_t}")
    list(GET _tv 0 _var)
    list(GET _tv 1 _tool)
    unset(_p CACHE)
    find_program(_p NAMES ${_tool}-21 ${_tool} NO_CMAKE_FIND_ROOT_PATH)
    if(NOT _p)
        message(FATAL_ERROR "the win64-msvc tree needs LLVM's ${_tool}, and "
                            "none is on PATH: install LLVM 21's tools")
    endif()
    set(CMAKE_${_var} ${_p})
endforeach()

# The runtime's headers and libraries reach the compile and the link through
# INCLUDE and LIB, which clang-cl and lld-link read as cl and link do: the
# environment is what survives a -DCMAKE_C_FLAGS (build.sh's checker trees
# pass one) and its own compiler test, which reads this file again with
# none of the cache. CMakeLists.txt hands the same directories to the
# targets themselves, so a build after a configure by hand has them too.
set(_fnl_inc "")
set(_fnl_lib "")
foreach(_d crt/include sdk/include/ucrt sdk/include/shared sdk/include/um)
    string(APPEND _fnl_inc "${FNL_MSVC_ROOT}/${_d};")
endforeach()
foreach(_d crt/lib/x64 sdk/lib/ucrt/x64 sdk/lib/um/x64)
    string(APPEND _fnl_lib "${FNL_MSVC_ROOT}/${_d};")
endforeach()
set(ENV{INCLUDE} "${_fnl_inc}")
set(ENV{LIB} "${_fnl_lib}")

# The release runtime in every configuration: the debug CRT is not
# redistributable, and this SDK has none. Handed to CMake's own probe
# projects, which read this file again and see none of the cache.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_MSVC_RUNTIME_LIBRARY)

# Nothing is found in a sysroot here -- there is none, the SDK is the
# target's -- and no program may be the target's.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
