# SPDX-License-Identifier: CC0-1.0
# Written in 2026 by Janos Sonntag and dedicated to the public domain.

cmake_minimum_required(VERSION 3.18)

# texture: a textured cube on Vulkan, compiled where the library has that
# renderer: Linux, macOS (on MoltenVK) and Windows.
#
# Builds on its own, bringing in the library from ../.. --
#
#     cmake -S demos/texture -B build/texture && cmake --build build/texture
#
# -- or as part of the top-level build, where the library is already there.
project(fenestal_demo_texture C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET fenestal_static)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. fenestal)
endif()

set(TEXTURE_SOURCES main.c)
set(TEXTURE_DEFS)
if(vulkan IN_LIST FNL_RENDERERS)
    list(APPEND TEXTURE_SOURCES scene_vulkan.c)
    list(APPEND TEXTURE_DEFS SCENE_HAS_VULKAN)
endif()
if(NOT TEXTURE_DEFS)
    message(STATUS "texture: skipped, no Vulkan in this build "
                   "(${FNL_RENDERERS})")
    return()
endif()

# The shaders, by the library's tools: SPIR-V by glslangValidator, embedded as
# `<file>_spv` (cube_vert_spv) in <file>.h.
set(TEXTURE_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TEXTURE_GEN_DIR})
set(TEXTURE_SHADER_HDRS)
foreach(_s cube.vert cube.frag)
    string(REPLACE "." "_" _var ${_s})
    set(_in ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s})
    set(_out ${TEXTURE_GEN_DIR}/${_s})
    if(vulkan IN_LIST FNL_RENDERERS)
        add_custom_command(
            OUTPUT ${_out}.h
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${_in}
                    --vn ${_var}_spv -o ${_out}.h
            DEPENDS ${_in}
            VERBATIM)
        list(APPEND TEXTURE_SHADER_HDRS ${_out}.h)
    endif()
endforeach()
add_custom_target(fenestal_demo_texture_shaders
                  DEPENDS ${TEXTURE_SHADER_HDRS})

# sinf and friends are libm's on Linux; Windows and macOS have them in libc.
# The scene links no API: Vulkan's entries come from the library's loader.
set(TEXTURE_LIBS)
if(UNIX AND NOT APPLE)
    list(APPEND TEXTURE_LIBS m)
endif()

# ---- the two programs -------------------------------------------------------
# texture on the static library and texture-dyn on the shared one, both at the
# top of the build tree.
foreach(_kind static dyn)
    if(_kind STREQUAL static)
        set(_t fenestal_demo_texture)
        set(_out texture)
        set(_fnl fenestal::static)
    else()
        set(_t fenestal_demo_texture_dyn)
        set(_out texture-dyn)
        set(_fnl fenestal::fenestal)
    endif()
    add_executable(${_t} ${TEXTURE_SOURCES})
    set_target_properties(${_t} PROPERTIES
                          OUTPUT_NAME ${_out}
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_include_directories(${_t} PRIVATE ${TEXTURE_GEN_DIR})
    target_compile_definitions(${_t} PRIVATE ${TEXTURE_DEFS})
    target_link_libraries(${_t} PRIVATE ${_fnl} ${TEXTURE_LIBS})
    if(NOT MSVC)
        target_compile_options(${_t} PRIVATE -Wall -Wextra)
    endif()
    # The shaders' own target: a custom command listed as a source of both
    # programs would run twice at once under make.
    add_dependencies(${_t} fenestal_demo_texture_shaders)
endforeach()

# The shared library beside texture-dyn, which looks for it in its own folder
# alone: the rpath is $ORIGIN or @loader_path, and Windows looks there
# anyway. The file copied is the one the loader asks for, the DLL or the
# soname's (libfenestal.so.1, libfenestal.1.dylib). One copy for every
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
add_dependencies(fenestal_demo_texture_dyn fenestal_demo_lib)
if(APPLE)
    set_target_properties(fenestal_demo_texture_dyn PROPERTIES
                          INSTALL_RPATH @loader_path)
elseif(NOT WIN32)
    set_target_properties(fenestal_demo_texture_dyn PROPERTIES
                          INSTALL_RPATH $ORIGIN)
endif()
# The install rpath at build time too, so the build tree's own path to the
# library is never written in; SKIP_BUILD_RPATH off because a toolchain file
# may set CMAKE_SKIP_BUILD_RPATH (the ARM one does).
set_target_properties(fenestal_demo_texture_dyn PROPERTIES
                      SKIP_BUILD_RPATH OFF BUILD_WITH_INSTALL_RPATH ON)
