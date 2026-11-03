# SPDX-License-Identifier: CC0-1.0
# Written in 2026 by Janos Sonntag and dedicated to the public domain.

cmake_minimum_required(VERSION 3.18)

# texture3: texture's textured cube on Metal or Vulkan, with a scene for
# each. Each scene is compiled where the library has its renderer -- both on
# macOS (Vulkan on MoltenVK), where the library chooses which draws; Vulkan
# alone on Linux and Windows.
#
# Builds on its own, bringing in the library from ../.. --
#
#     cmake -S demos/texture3 -B build/texture3 && cmake --build build/texture3
#
# -- or as part of the top-level build, where the library is already there.
project(fenestal_demo_texture3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# On a Mac the Metal scene is Objective-C, which Metal needs (scene_metal.m).
if(APPLE)
    enable_language(OBJC)
endif()

if(NOT TARGET fenestal_static)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. fenestal)
endif()

set(TEXTURE3_SOURCES main.c)
set(TEXTURE3_DEFS)
if(vulkan IN_LIST FNL_RENDERERS)
    list(APPEND TEXTURE3_SOURCES scene_vulkan.c)
    list(APPEND TEXTURE3_DEFS SCENE_HAS_VULKAN)
endif()
if(metal IN_LIST FNL_RENDERERS)
    # Objective-C under ARC, which owns the scene's Metal objects.
    list(APPEND TEXTURE3_SOURCES scene_metal.m)
    list(APPEND TEXTURE3_DEFS SCENE_HAS_METAL)
    set_source_files_properties(scene_metal.m PROPERTIES
                                COMPILE_OPTIONS -fobjc-arc)
endif()
if(NOT TEXTURE3_DEFS)
    message(STATUS "texture3: skipped, neither Metal nor Vulkan in "
                   "this build (${FNL_RENDERERS})")
    return()
endif()

# The shaders, once for each scene, by the library's tools:
#
#   Vulkan        SPIR-V by glslangValidator, embedded as `<file>_spv`
#                 (cube_vert_spv) in <file>.h
#   Metal         that SPIR-V translated to MSL by spirv-cross
#                 (FNL_SPIRV_CROSS) on the library's flags (FNL_MSL_FLAGS:
#                 MSL 2.1, Y flipped so the picture is Vulkan's, a binding
#                 its own index -- fnl_metal.h), embedded as text by the
#                 library's script (FNL_EMBED_TEXT) as `<file>_msl` in
#                 <file>.msl.h, which the device compiles at run time: only
#                 Xcode has Metal's offline compiler
set(TEXTURE3_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TEXTURE3_GEN_DIR})
set(TEXTURE3_SHADER_HDRS)
foreach(_s cube.vert cube.frag)
    string(REPLACE "." "_" _var ${_s})
    set(_in ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s})
    set(_out ${TEXTURE3_GEN_DIR}/${_s})
    if(vulkan IN_LIST FNL_RENDERERS)
        add_custom_command(
            OUTPUT ${_out}.h
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${_in}
                    --vn ${_var}_spv -o ${_out}.h
            DEPENDS ${_in}
            VERBATIM)
        list(APPEND TEXTURE3_SHADER_HDRS ${_out}.h)
    endif()
    if(metal IN_LIST FNL_RENDERERS)
        add_custom_command(
            OUTPUT ${_out}.msl.h
            BYPRODUCTS ${_out}.spv ${_out}.msl
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${_in}
                    -o ${_out}.spv
            COMMAND ${FNL_SPIRV_CROSS} ${_out}.spv ${FNL_MSL_FLAGS}
                    --output ${_out}.msl
            COMMAND ${CMAKE_COMMAND} -DIN=${_out}.msl -DOUT=${_out}.msl.h
                    -DVAR=${_var}_msl -P ${FNL_EMBED_TEXT}
            DEPENDS ${_in} ${FNL_EMBED_TEXT}
            VERBATIM)
        list(APPEND TEXTURE3_SHADER_HDRS ${_out}.msl.h)
    endif()
endforeach()
add_custom_target(fenestal_demo_texture3_shaders
                  DEPENDS ${TEXTURE3_SHADER_HDRS})

# libm on Linux. Nothing more for Metal: the framework, which is the
# runtime, is on both libraries' own link line, and Foundation with it
# through Cocoa's.
set(TEXTURE3_LIBS)
if(UNIX AND NOT APPLE)
    list(APPEND TEXTURE3_LIBS m)
endif()

# ---- the two programs -------------------------------------------------------
# texture3 on the static library and texture3-dyn on the shared one, both at the
# top of the build tree.
foreach(_kind static dyn)
    if(_kind STREQUAL static)
        set(_t fenestal_demo_texture3)
        set(_out texture3)
        set(_fnl fenestal::static)
    else()
        set(_t fenestal_demo_texture3_dyn)
        set(_out texture3-dyn)
        set(_fnl fenestal::fenestal)
    endif()
    add_executable(${_t} ${TEXTURE3_SOURCES})
    set_target_properties(${_t} PROPERTIES
                          OUTPUT_NAME ${_out}
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_include_directories(${_t} PRIVATE ${TEXTURE3_GEN_DIR})
    target_compile_definitions(${_t} PRIVATE ${TEXTURE3_DEFS})
    target_link_libraries(${_t} PRIVATE ${_fnl} ${TEXTURE3_LIBS})
    target_compile_options(${_t} PRIVATE -Wall -Wextra)
    # The shaders' own target: a custom command listed as a source of both
    # programs would run twice at once under make.
    add_dependencies(${_t} fenestal_demo_texture3_shaders)
endforeach()

# The shared library beside texture3-dyn, which looks for it in its own folder
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
add_dependencies(fenestal_demo_texture3_dyn fenestal_demo_lib)
if(APPLE)
    set_target_properties(fenestal_demo_texture3_dyn PROPERTIES
                          INSTALL_RPATH @loader_path)
elseif(NOT WIN32)
    set_target_properties(fenestal_demo_texture3_dyn PROPERTIES
                          INSTALL_RPATH $ORIGIN)
endif()
# The install rpath at build time too, so the build tree's own path to the
# library is never written in; SKIP_BUILD_RPATH off because a toolchain file
# may set CMAKE_SKIP_BUILD_RPATH (the ARM one does).
set_target_properties(fenestal_demo_texture3_dyn PROPERTIES
                      SKIP_BUILD_RPATH OFF BUILD_WITH_INSTALL_RPATH ON)
