# SPDX-License-Identifier: CC0-1.0
# Written in 2026 by Janos Sonntag and dedicated to the public domain.

cmake_minimum_required(VERSION 3.18)

# texture2: texture's textured cube on Direct3D 12 or Vulkan, with a scene
# for each. Each scene is compiled where the library has its renderer --
# both on Windows, where the library chooses which draws; Vulkan alone on
# Linux and macOS.
#
# Builds on its own, bringing in the library from ../.. --
#
#     cmake -S demos/texture2 -B build/texture2 && cmake --build build/texture2
#
# -- or as part of the top-level build, where the library is already there.
project(fenestal_demo_texture2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET fenestal_static)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. fenestal)
endif()

set(TEXTURE2_SOURCES main.c)
set(TEXTURE2_DEFS)
if(vulkan IN_LIST FNL_RENDERERS)
    list(APPEND TEXTURE2_SOURCES scene_vulkan.c)
    list(APPEND TEXTURE2_DEFS SCENE_HAS_VULKAN)
endif()
if(d3d12 IN_LIST FNL_RENDERERS)
    list(APPEND TEXTURE2_SOURCES scene_d3d12.c)
    list(APPEND TEXTURE2_DEFS SCENE_HAS_D3D12)
endif()
if(NOT TEXTURE2_DEFS)
    message(STATUS "texture2: skipped, neither Direct3D 12 nor Vulkan in "
                   "this build (${FNL_RENDERERS})")
    return()
endif()

# The shaders, once for each scene, by the library's tools:
#
#   Vulkan        SPIR-V by glslangValidator, embedded as `<file>_spv`
#                 (cube_vert_spv) in <file>.h
#   Direct3D 12   that SPIR-V translated to HLSL by spirv-cross
#                 (FNL_SPIRV_CROSS) with Y flipped, so the picture is
#                 Vulkan's (fnl_d3d12.h), and compiled to DXIL by
#                 Microsoft's dxc (FNL_DXC, which build.sh names), embedded
#                 as `<file>_dxil` in <file>.dxil.h
set(TEXTURE2_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TEXTURE2_GEN_DIR})
set(TEXTURE2_SHADER_HDRS)
foreach(_s cube.vert cube.frag)
    string(REPLACE "." "_" _var ${_s})
    set(_in ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s})
    set(_out ${TEXTURE2_GEN_DIR}/${_s})
    if(vulkan IN_LIST FNL_RENDERERS)
        add_custom_command(
            OUTPUT ${_out}.h
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${_in}
                    --vn ${_var}_spv -o ${_out}.h
            DEPENDS ${_in}
            VERBATIM)
        list(APPEND TEXTURE2_SHADER_HDRS ${_out}.h)
    endif()
    if(d3d12 IN_LIST FNL_RENDERERS)
        if(_s MATCHES "vert$")
            set(_profile vs_6_0)
        else()
            set(_profile ps_6_0)
        endif()
        add_custom_command(
            OUTPUT ${_out}.dxil.h
            BYPRODUCTS ${_out}.spv ${_out}.hlsl
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${_in}
                    -o ${_out}.spv
            COMMAND ${FNL_SPIRV_CROSS} ${_out}.spv --hlsl --shader-model 60
                    --flip-vert-y --output ${_out}.hlsl
            COMMAND ${FNL_DXC} -nologo -T ${_profile} -E main
                    -Vn ${_var}_dxil -Fh ${_out}.dxil.h ${_out}.hlsl
            DEPENDS ${_in} ${FNL_DXC}
            VERBATIM)
        list(APPEND TEXTURE2_SHADER_HDRS ${_out}.dxil.h)
    endif()
endforeach()
add_custom_target(fenestal_demo_texture2_shaders
                  DEPENDS ${TEXTURE2_SHADER_HDRS})

# libm on Linux; and on Windows what the Direct3D 12 scene calls itself, the
# root signature's serializer and the interface ids, named here because the
# scene calls them: the library's targets hand them on for its own use, and
# a consumer's code does not lean on that.
set(TEXTURE2_LIBS)
if(UNIX AND NOT APPLE)
    list(APPEND TEXTURE2_LIBS m)
endif()
if(d3d12 IN_LIST FNL_RENDERERS)
    list(APPEND TEXTURE2_LIBS d3d12 dxguid)
endif()

# ---- the two programs -------------------------------------------------------
# texture2 on the static library and texture2-dyn on the shared one, both at the
# top of the build tree.
foreach(_kind static dyn)
    if(_kind STREQUAL static)
        set(_t fenestal_demo_texture2)
        set(_out texture2)
        set(_fnl fenestal::static)
    else()
        set(_t fenestal_demo_texture2_dyn)
        set(_out texture2-dyn)
        set(_fnl fenestal::fenestal)
    endif()
    add_executable(${_t} ${TEXTURE2_SOURCES})
    set_target_properties(${_t} PROPERTIES
                          OUTPUT_NAME ${_out}
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_include_directories(${_t} PRIVATE ${TEXTURE2_GEN_DIR})
    target_compile_definitions(${_t} PRIVATE ${TEXTURE2_DEFS})
    target_link_libraries(${_t} PRIVATE ${_fnl} ${TEXTURE2_LIBS})
    target_compile_options(${_t} PRIVATE -Wall -Wextra)
    # The shaders' own target: a custom command listed as a source of both
    # programs would run twice at once under make.
    add_dependencies(${_t} fenestal_demo_texture2_shaders)
endforeach()

# The shared library beside texture2-dyn, which looks for it in its own folder
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
add_dependencies(fenestal_demo_texture2_dyn fenestal_demo_lib)
if(APPLE)
    set_target_properties(fenestal_demo_texture2_dyn PROPERTIES
                          INSTALL_RPATH @loader_path)
elseif(NOT WIN32)
    set_target_properties(fenestal_demo_texture2_dyn PROPERTIES
                          INSTALL_RPATH $ORIGIN)
endif()
# The install rpath at build time too, so the build tree's own path to the
# library is never written in; SKIP_BUILD_RPATH off because a toolchain file
# may set CMAKE_SKIP_BUILD_RPATH (the ARM one does).
set_target_properties(fenestal_demo_texture2_dyn PROPERTIES
                      SKIP_BUILD_RPATH OFF BUILD_WITH_INSTALL_RPATH ON)
