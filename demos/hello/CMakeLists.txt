# SPDX-License-Identifier: CC0-1.0
# Written in 2026 by Janos Sonntag and dedicated to the public domain.

cmake_minimum_required(VERSION 3.18)

# hello: the smallest Fenestal program, a window whose scene is black.
#
# Builds on its own, bringing in the library from ../.. --
#
#     cmake -S demos/hello -B build/hello && cmake --build build/hello
#
# -- or as part of the top-level build, where the library is already there.
project(fenestal_demo_hello C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET fenestal_static)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. fenestal)
endif()

set(HELLO_SOURCES main.c)

# ---- the two programs -------------------------------------------------------
# hello on the static library and hello-dyn on the shared one, both at the
# top of the build tree.
foreach(_kind static dyn)
    if(_kind STREQUAL static)
        set(_t fenestal_demo_hello)
        set(_out hello)
        set(_fnl fenestal::static)
    else()
        set(_t fenestal_demo_hello_dyn)
        set(_out hello-dyn)
        set(_fnl fenestal::fenestal)
    endif()
    add_executable(${_t} ${HELLO_SOURCES})
    set_target_properties(${_t} PROPERTIES
                          OUTPUT_NAME ${_out}
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_libraries(${_t} PRIVATE ${_fnl})
    target_compile_options(${_t} PRIVATE -Wall -Wextra)
endforeach()

# The shared library beside hello-dyn, which looks for it in its own folder
# alone: the rpath is $ORIGIN or @loader_path, and Windows looks there
# anyway. The file copied is the one the loader asks for, the DLL or the
# soname's (libfenestal.so.0.9, libfenestal.0.9.dylib). One copy for every
# demo in the tree, made by the first to get here, so no two write it at
# once in a parallel build.
if(NOT TARGET fenestal_demo_lib)
    if(WIN32)
        set(_lib $<TARGET_FILE:fenestal_shared>)
    else()
        set(_lib $<TARGET_SONAME_FILE:fenestal_shared>)
    endif()
    add_custom_target(fenestal_demo_lib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_lib}
                ${CMAKE_BINARY_DIR}
        VERBATIM)
    add_dependencies(fenestal_demo_lib fenestal_shared)
endif()
add_dependencies(fenestal_demo_hello_dyn fenestal_demo_lib)
if(APPLE)
    set_target_properties(fenestal_demo_hello_dyn PROPERTIES
                          INSTALL_RPATH @loader_path)
elseif(NOT WIN32)
    set_target_properties(fenestal_demo_hello_dyn PROPERTIES
                          INSTALL_RPATH $ORIGIN)
endif()
# The install rpath at build time too, so the build tree's own path to the
# library is never written in; SKIP_BUILD_RPATH off because a toolchain file
# may set CMAKE_SKIP_BUILD_RPATH (the ARM one does).
set_target_properties(fenestal_demo_hello_dyn PROPERTIES
                      SKIP_BUILD_RPATH OFF BUILD_WITH_INSTALL_RPATH ON)
