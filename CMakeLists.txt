# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag

cmake_minimum_required(VERSION 3.18)

# gcc unless told otherwise, and before project() because that is when the
# compiler is detected. CC is checked by hand: it is only consulted during
# project(), so an unguarded default would shadow `CC=clang cmake ...`.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER gcc)
endif()

# ASM is for the generated .incbin stubs that carry the font blob and the
# licence text into .rodata.
project(fenestal VERSION 0.9.0 LANGUAGES C ASM)
# And on macOS Objective-C, for the two directories that speak to AppKit and
# Metal (src/backend/cocoa/, src/renderer/metal/), which have no C interface.
# Nothing else in the tree is Objective-C.
if(APPLE)
    enable_language(OBJC)
    set(CMAKE_OBJC_STANDARD 11)
    set(CMAKE_OBJC_STANDARD_REQUIRED ON)
endif()
# Every Windows target of this directory and below -- the demos too, when
# this tree adds them; a demo configured on its own compiles its own
# sources without them, and none needs them. Here and not as the
# toolchain file's CMAKE_C_FLAGS_INIT, which a -DCMAKE_C_FLAGS replaces
# (build.sh's checker trees pass one). A Windows tree builds no host
# tool.
#
#   _WIN32_WINNT / WINVER  Windows 10. SetProcessDpiAwarenessContext and
#                          GetDpiForWindow arrive in 1607 and mingw's headers
#                          hide them behind this.
#   __USE_MINGW_ANSI_STDIO mingw's own printf and not msvcrt.dll's, which
#                          predates C99 and prints %zu as the text "zu". It
#                          must precede <stdio.h>, which a -D does.
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00
                            __USE_MINGW_ANSI_STDIO=1)
endif()

# The version is said twice, here for the shared object's name and in
# fenestal.h for a caller's FNL_VERSION, and the two must not disagree.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fenestal.h _ver
     REGEX "^#define FNL_VERSION_(MAJOR|MINOR|PATCH) ")
string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*"
       "\\1.\\2.\\3" _hver "${_ver}")
if(NOT _hver STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "fenestal.h says ${_hver} and CMakeLists.txt says "
                        "${PROJECT_VERSION}; change both together")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The default build type, whether the fixtures are built and whether the
# demos are added are a project's to decide and not a dependency's.
# Compared by hand because PROJECT_IS_TOP_LEVEL is CMake 3.21.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FNL_IS_TOP_LEVEL TRUE)
else()
    set(FNL_IS_TOP_LEVEL FALSE)
endif()

# ---- Backends and renderers ----------------------------------------------
# A backend is the window system (Wayland, X11, KMS, Win32, Cocoa), a
# renderer the graphics API (Vulkan, OpenGL, Direct3D 12, Metal), and a
# platform one of each, such as Vulkan on X11. A build has every backend
# and every renderer its system has, the platform chosen at fnl_init():
# DESIGN *One build per operating system*.
#
# The names these options had are refused rather than read: a tree
# configured with one of them, left at its old default, has it dropped
# from the cache, and any other value is an error naming the new option.
foreach(_old_new FNL_PLATFORM:FNL_BACKEND:linux FNL_GPU:FNL_RENDERER:all)
    string(REPLACE ":" ";" _on "${_old_new}")
    list(GET _on 0 _old)
    list(GET _on 1 _new)
    list(GET _on 2 _default)
    if(DEFINED CACHE{${_old}})
        if(NOT "$CACHE{${_old}}" STREQUAL _default)
            message(FATAL_ERROR "${_old} is ${_new} now: -D${_new}="
                                "$CACHE{${_old}}")
        endif()
        unset(${_old} CACHE)
    endif()
endforeach()

# Linux has three backends -- two window servers and none, the bare
# display (kms) -- and a build has all three by default (linux), the one to
# open chosen from the session's environment. A single name builds that one
# alone, for a consumer that wants the smaller library and the shorter link
# line. Windows and macOS have one each and force it, so that a
# -DFNL_BACKEND=x11 there is refused here and not at the first #include.
#
# FNL_BACKENDS is the list the rest of this file reads. Its order is not
# the order they are tried in, which is backend.h's (FNL_BACKENDS(X)).
set(FNL_BACKEND "linux" CACHE STRING
    "Backend: linux (all three), wayland, x11, kms, win32 or cocoa")
set_property(CACHE FNL_BACKEND PROPERTY STRINGS linux wayland x11 kms win32
             cocoa)
if(WIN32)
    set(FNL_BACKEND win32)
elseif(APPLE)
    set(FNL_BACKEND cocoa)
elseif(NOT FNL_BACKEND MATCHES "^(linux|wayland|x11|kms)$")
    message(FATAL_ERROR
        "FNL_BACKEND is '${FNL_BACKEND}'; this target takes linux, "
        "wayland, x11 or kms")
endif()
if(FNL_BACKEND STREQUAL linux)
    set(FNL_BACKENDS wayland x11 kms)
else()
    set(FNL_BACKENDS ${FNL_BACKEND})
endif()
message(STATUS "Fenestal backends: ${FNL_BACKENDS}")

# The renderers a system has, in the order fnl_init() tries them (the
# code's order is renderer.h's, FNL_RENDERERS(X)): Windows Direct3D 12 and
# Vulkan; macOS Metal, Vulkan on MoltenVK, and OpenGL; Linux Vulkan and
# OpenGL.
# FNL_RENDERER names one of them, or `all` for every one, the default. A
# cache variable all the same: it is what a consumer names to ask for a
# renderer, and asking for one this system lacks is refused here and not
# at the link.
#
# OpenGL reaches the window through EGL on Wayland, X11 and KMS
# (src/renderer/gl/egl_<backend>.c; KMS's surfaceless, its frames handed to
# the backend), and through CGL on macOS (src/renderer/gl/cgl.m), at 4.1
# core, the renderer's 4.1 profile, its frames copied into the window's
# Metal layer. Windows draws with Direct3D 12, on WARP where there is no
# driver, and with Vulkan.
if(FNL_BACKEND STREQUAL win32)
    set(_fnl_system_renderers d3d12 vulkan)
elseif(FNL_BACKEND STREQUAL cocoa)
    set(_fnl_system_renderers metal vulkan gl)
else()
    set(_fnl_system_renderers vulkan gl)
endif()
set(FNL_RENDERER all CACHE STRING
    "Renderer: all (every one this system has), vulkan, gl, d3d12 or metal")
set_property(CACHE FNL_RENDERER PROPERTY STRINGS all vulkan gl d3d12 metal)
# FNL_RENDERERS is the list the rest of this file reads, and in the cache so
# that a consumer's directory, which added this one, can read it too: its
# scenes are one per renderer.
if(FNL_RENDERER STREQUAL all)
    set(FNL_RENDERERS "${_fnl_system_renderers}" CACHE INTERNAL
        "The build's renderers")
elseif(FNL_RENDERER IN_LIST _fnl_system_renderers)
    set(FNL_RENDERERS "${FNL_RENDERER}" CACHE INTERNAL
        "The build's renderers")
else()
    string(REPLACE ";" ", " _have "${_fnl_system_renderers}")
    message(FATAL_ERROR
        "FNL_RENDERER is '${FNL_RENDERER}'; FNL_BACKEND=${FNL_BACKEND} "
        "takes all or one of ${_have}")
endif()
list(LENGTH FNL_RENDERERS FNL_RENDERER_COUNT)
message(STATUS "Fenestal renderers: ${FNL_RENDERERS}")

# tools/mkfont links FreeType and runs during the build. A cross build cannot
# run it, and a checker tree beside a release tree of its own target need not:
# the bake is the same bytes. Either takes the four baked files from another
# tree's generated directory instead.
#
# Two variables because they say two things. FNL_GENERATED_DIR is the cross
# build's, and also means no host tool runs here at all. FNL_FONT_DIR lends
# the font and nothing else, so verifykeymap still builds and runs.
set(FNL_GENERATED_DIR "" CACHE PATH
    "A host build's generated directory, for a cross build")
set(FNL_FONT_DIR "" CACHE PATH
    "Another tree's generated directory, to take the baked font from")
if(CMAKE_CROSSCOMPILING AND NOT FNL_GENERATED_DIR)
    message(FATAL_ERROR
        "Cross-compiling needs -DFNL_GENERATED_DIR=<host build>/generated, "
        "because tools/mkfont must run on the build machine. Configure and "
        "build for the host first, then point at its generated directory -- "
        "build.sh does this for you, and names the tree it expects.")
endif()
if(FNL_FONT_DIR)
    set(FNL_FONT_FROM "${FNL_FONT_DIR}")
else()
    set(FNL_FONT_FROM "${FNL_GENERATED_DIR}")
endif()
if(FNL_FONT_FROM)
    # The .incbin stub below names these files, so the path has to be whole.
    get_filename_component(FNL_FONT_FROM "${FNL_FONT_FROM}" ABSOLUTE)
endif()

# ---- Build configuration / optimization -----------------------------------
# An empty CMAKE_BUILD_TYPE means no -O flag at all, so Release is the default
# for a single-config generator. FORCEd only when nothing was chosen.
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(FNL_IS_TOP_LEVEL AND NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS
        "No CMAKE_BUILD_TYPE set; defaulting to Release (-O3 -DNDEBUG)")
endif()

# The instruction set the library is compiled for. x86-64-v2 by default:
# SSE4.2 and POPCNT, every x86-64 CPU since about 2009, this project's
# Sandy Bridge desktop included, so a binary built anywhere runs on every
# machine the project is driven on. FNL_NATIVE_ARCH=ON is the opt-in for the
# machine doing the build (-march=native), whose binary may not run on an
# older CPU and need not on a newer one's peers. DESIGN *Building* has what
# the choice measured. Empty FNL_ARCH leaves the compiler's own default.
option(FNL_NATIVE_ARCH "Optimize for the host CPU (-march=native)" OFF)
set(FNL_ARCH "x86-64-v2" CACHE STRING
    "-march for the library when FNL_NATIVE_ARCH is off; empty for none")

# -DFNL_VERIFY_FONT=OFF skips the check on the bake; see verifyfont below.
option(FNL_VERIFY_FONT "Check the baked font atlases against FreeType" ON)

# Link-time optimisation, where supported. The check is an LTO try-compile and
# build.sh configures on every invocation, so the answer is cached -- keyed on
# the compiler, so CC=clang or a new gcc asks again. A change to CMAKE_C_FLAGS
# alone does not; delete FNL_IPO_KEY from the cache for that.
set(_ipo_key "${CMAKE_C_COMPILER}|${CMAKE_C_COMPILER_VERSION}")
if(NOT DEFINED FNL_IPO_OK OR NOT "${FNL_IPO_KEY}" STREQUAL "${_ipo_key}")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    set(FNL_IPO_OK "${_ipo_ok}" CACHE INTERNAL "Whether LTO is available")
    set(FNL_IPO_MSG "${_ipo_msg}" CACHE INTERNAL "Why LTO is unavailable")
    set(FNL_IPO_KEY "${_ipo_key}" CACHE INTERNAL "Compiler FNL_IPO_OK is for")
endif()

include(CheckCCompilerFlag)

# __FILE__ reaches the binary through the VKR macro, and with it the directory
# this tree was built in. The macro only: debug info keeps its real paths, so
# a debugger still finds the sources without being told where they are.
check_c_compiler_flag("-fmacro-prefix-map=/a=/b" FNL_HAS_MACRO_PREFIX_MAP)
set(FNL_PREFIX_MAP)
if(FNL_HAS_MACRO_PREFIX_MAP)
    set(FNL_PREFIX_MAP "-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=")
endif()

# An imported target is visible in the directory that made it and below, not
# above -- so as a subdirectory, the consumer's target would inherit
# Vulkan::Vulkan through fenestal_static and be unable to see it. Each
# dependency handed on is therefore made GLOBAL, but only when this file is
# what created it: CMake refuses to promote a target from another directory,
# and a consumer that already found the package has it in scope anyway.
macro(fnl_find_global package target)
    if(TARGET ${target})
        find_package(${package} REQUIRED)
    else()
        find_package(${package} REQUIRED)
        set_target_properties(${target} PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
endmacro()

# The library opens the Vulkan loader at run time (src/renderer/vulkan/
# vk_load.c) and links none, so that a machine without one still runs the
# program with another renderer; Vulkan::Vulkan is its headers. A caller's
# Vulkan scene needs no loader either: FnlVulkanGpu hands out the one the
# library opened, and the examples' scenes ask it for every entry. A caller
# may still link the loader, and in a cross tree what it links is made below.
#
# Cross-compiling there is no Vulkan to find: on Windows the loader ships as
# vulkan-1.dll with the driver, on a Mac Vulkan is MoltenVK, shipped beside
# the program (tools/moltenvk.py), on ARM Linux the loader is the Pi's own,
# and no sysroot has a library to link it by. Both halves are made here.
#
#   the headers  are the host's, symlinked into a staging directory. A plain
#                -I/usr/include would put glibc's headers ahead of the
#                target's.
#   the library  is synthesised from a list of every vk* identifier in the
#                sources, so it cannot go stale: a .def that dlltool makes an
#                import library of on Windows, on macOS a text stub (.tbd)
#                of libMoltenVK.dylib, which ld64.lld links against as it
#                links the SDK's own, and on Linux a shared object of empty
#                functions named libvulkan.so.1. A name the loader does not
#                export costs nothing unless referenced.
#
# FNL_HOST_INCLUDE is where the host's headers are looked for, these and the
# GL ones the macOS tree copies below: /usr/include on a Linux build machine.
# A Mac has none there, so a Mac building the macOS tree names a copy of a
# Linux machine's (.github/workflows/ci.yml does).
set(FNL_HOST_INCLUDE /usr/include CACHE PATH
    "The build machine's headers, for a cross tree's Vulkan and GL ones")
if(NOT vulkan IN_LIST FNL_RENDERERS)
    # Neither the loader nor its headers: the OpenGL renderer opens EGL at run
    # time and calls GL through what EGL hands out; Direct3D 12's runtime
    # is d3d12 and dxgi, which mingw-w64 has, and Metal's is the SDK's
    # framework.
elseif(CMAKE_CROSSCOMPILING)
    find_path(FNL_VULKAN_INCLUDE_DIR vulkan/vulkan.h
              PATHS ${FNL_HOST_INCLUDE} NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(_vkstage ${CMAKE_CURRENT_BINARY_DIR}/vulkan-include)
    file(MAKE_DIRECTORY ${_vkstage})
    foreach(_d vulkan vk_video)
        if(EXISTS ${FNL_VULKAN_INCLUDE_DIR}/${_d})
            file(CREATE_LINK ${FNL_VULKAN_INCLUDE_DIR}/${_d} ${_vkstage}/${_d}
                 SYMBOLIC)
        endif()
    endforeach()

    # The library's sources, the testbed's, the snippets' and the demos',
    # which a consumer copies: one that links this rather than asking
    # FnlVulkanGpu for its entries finds theirs in it. GLOB_RECURSE because
    # the sources are nested; the count check below is what says so if this
    # ever matches nothing.
    #
    # FNL_VULKAN_SCAN is the same for a consumer, whose program calls entry
    # points this tree never names: globs, as a CMake list, set before the
    # add_subdirectory() that brings this file in.
    set(FNL_VULKAN_SCAN "" CACHE STRING
        "Globs of consumer sources to scan for Vulkan entry points")
    file(GLOB_RECURSE _vk_srcs
         ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/testbed/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/demos/*.c
         ${FNL_VULKAN_SCAN})
    set(_vk_syms "")
    foreach(_f ${_vk_srcs})
        file(STRINGS ${_f} _lines REGEX "vk[A-Z][A-Za-z0-9]*")
        foreach(_l ${_lines})
            string(REGEX MATCHALL "vk[A-Z][A-Za-z0-9]*" _m "${_l}")
            list(APPEND _vk_syms ${_m})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _vk_syms)
    list(SORT _vk_syms)
    list(LENGTH _vk_syms _n)
    if(_n LESS 20)
        message(FATAL_ERROR
            "only ${_n} Vulkan entry points found in the sources -- the glob "
            "above is not matching them. A library built from that links, "
            "and fails at the first call.")
    endif()

    if(WIN32)
        set(_def "LIBRARY vulkan-1.dll\nEXPORTS\n")
        foreach(_sym ${_vk_syms})
            string(APPEND _def "    ${_sym}\n")
        endforeach()
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.def "${_def}")
        execute_process(
            COMMAND ${CMAKE_DLLTOOL}
                    -d ${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.def
                    -l ${CMAKE_CURRENT_BINARY_DIR}/libvulkan-1.a
            RESULT_VARIABLE _dlltool_rc)
        if(NOT _dlltool_rc EQUAL 0)
            message(FATAL_ERROR
                "dlltool could not build the Vulkan import library")
        endif()
        message(STATUS "Synthesised libvulkan-1.a from ${_n} entry points")
        add_library(Vulkan::Vulkan STATIC IMPORTED GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/libvulkan-1.a
            INTERFACE_INCLUDE_DIRECTORIES ${_vkstage})
    elseif(NOT APPLE)
        # The loader's soname, so a program records libvulkan.so.1 and the
        # machine's loader answers it. The toolchain file turns off the
        # build RPATH, or the stub's directory would be named in every
        # program.
        set(_stub_c "")
        foreach(_sym ${_vk_syms})
            string(APPEND _stub_c "void ${_sym}(void) {}\n")
        endforeach()
        set(_stubdir ${CMAKE_CURRENT_BINARY_DIR}/vulkan-stub)
        file(MAKE_DIRECTORY ${_stubdir})
        file(WRITE ${_stubdir}/vulkan.c.in "${_stub_c}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_stubdir}/vulkan.c.in ${_stubdir}/vulkan.c)
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib
                    -Wl,-soname,libvulkan.so.1
                    -o ${_stubdir}/libvulkan.so ${_stubdir}/vulkan.c
            RESULT_VARIABLE _stub_rc)
        if(NOT _stub_rc EQUAL 0)
            message(FATAL_ERROR "could not build the Vulkan loader's stub")
        endif()
        message(STATUS "Synthesised libvulkan.so from ${_n} entry points")
        add_library(Vulkan::Vulkan SHARED IMPORTED GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION ${_stubdir}/libvulkan.so
            IMPORTED_SONAME libvulkan.so.1
            INTERFACE_INCLUDE_DIRECTORIES ${_vkstage})
    else()
        # Mach-O's names carry a leading underscore. MoltenVK is linked
        # directly, with no loader between (tools/moltenvk.py says why), and
        # found beside the program (@rpath, set to the program's own
        # directory), where build.sh puts it.
        set(_syms "")
        foreach(_sym ${_vk_syms})
            if(_syms)
                string(APPEND _syms ", ")
            endif()
            string(APPEND _syms "_${_sym}")
        endforeach()
        set(_tbd ${CMAKE_CURRENT_BINARY_DIR}/libMoltenVK.tbd)
        file(WRITE ${_tbd}.in "--- !tapi-tbd
tbd-version:     4
targets:         [ x86_64-macos ]
install-name:    '@rpath/libMoltenVK.dylib'
current-version: 1
compatibility-version: 1
exports:
  - targets:     [ x86_64-macos ]
    symbols:     [ ${_syms} ]
...
")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_tbd}.in ${_tbd})
        message(STATUS "Synthesised libMoltenVK.tbd from ${_n} entry points")
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION ${_tbd}
            INTERFACE_INCLUDE_DIRECTORIES ${_vkstage}
            INTERFACE_LINK_OPTIONS "-Wl,-rpath,@executable_path")
    endif()
else()
    fnl_find_global(Vulkan Vulkan::Vulkan)
endif()
if(NOT WIN32)
    # The render thread. Windows has CreateThread; see src/core/thread.h.
    fnl_find_global(Threads Threads::Threads)
endif()
find_package(PkgConfig REQUIRED)
if(gl IN_LIST FNL_RENDERERS AND FNL_BACKEND STREQUAL cocoa)
    # Khronos's own header, declaring every GL type and enum and no
    # function: entry points come from the bridge (cgl.m). Copied from this
    # machine into the tree's generated headers, since a cross find_path
    # looks in Apple's SDK, which has none, and the host's /usr/include
    # must not reach a Darwin build whole.
    foreach(_h GL/glcorearb.h KHR/khrplatform.h)
        find_file(_fnl_host_${_h} ${_h} PATHS ${FNL_HOST_INCLUDE}
                  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        configure_file(${_fnl_host_${_h}}
                       ${CMAKE_CURRENT_BINARY_DIR}/generated/glhdr/${_h}
                       COPYONLY)
    endforeach()
    set(FNL_GLCOREARB_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/glhdr)
elseif(gl IN_LIST FNL_RENDERERS)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET GLOBAL egl)
    # Khronos's own header, declaring every GL type and enum and no
    # function: entry points come from the loader (src/renderer/gl/loader.c).
    find_path(FNL_GLCOREARB_DIR GL/glcorearb.h REQUIRED)
endif()

# For their headers: each runtime is opened at run time (src/core/dynlib.h)
# and none is linked, so that a machine without one still loads the
# program and steps past what needed it.
if(wayland IN_LIST FNL_BACKENDS)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET GLOBAL
                      wayland-client)
    if(gl IN_LIST FNL_RENDERERS)
        # The native window EGL draws into (src/renderer/gl/egl_wayland.c).
        pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET GLOBAL
                          wayland-egl)
    endif()
endif()
if(x11 IN_LIST FNL_BACKENDS)
    # Core xcb and nothing beside it: the backend hand-encodes the extension
    # requests it wants, because each xcb-* library would be another entry on
    # a consumer's link line. src/backend/x11/x11_ext.c has the encoding.
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET GLOBAL xcb)
endif()
# FreeType is a *build* dependency: linked into tools/mkfont, never into the
# library, and not wanted at all by a tree that borrows its bake.
if(NOT FNL_FONT_FROM)
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
endif()

# ---- Generated files ------------------------------------------------------
set(FNL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${FNL_GEN_DIR})

# Everything generated that the library's objects wait for: the shader
# headers and the verify stamps. Listed as sources of the object library.
set(FNL_GEN_FILES)

# ---- Wayland protocol code generation -------------------------------------
# From the vendored XML in protocols/, so the build needs no system-installed
# wayland-protocols.
if(wayland IN_LIST FNL_BACKENDS)
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(NOT WAYLAND_SCANNER)
        message(FATAL_ERROR
            "wayland-scanner not found (install the 'wayland' package)")
    endif()
endif()

function(wayland_protocol name)
    set(xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${name}.xml)
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/${name}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${xml}
                ${FNL_GEN_DIR}/${name}-client-protocol.h
        DEPENDS ${xml} VERBATIM)
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/${name}-protocol.c
        COMMAND ${WAYLAND_SCANNER} private-code ${xml}
                ${FNL_GEN_DIR}/${name}-protocol.c
        DEPENDS ${xml} VERBATIM)
    list(APPEND FNL_PROTO_SRCS
         ${FNL_GEN_DIR}/${name}-protocol.c
         ${FNL_GEN_DIR}/${name}-client-protocol.h)
    set(FNL_PROTO_SRCS ${FNL_PROTO_SRCS} PARENT_SCOPE)
endfunction()

if(wayland IN_LIST FNL_BACKENDS)
    wayland_protocol(xdg-shell)
    wayland_protocol(xdg-decoration-unstable-v1)
    wayland_protocol(cursor-shape-v1)
    wayland_protocol(viewporter)
    wayland_protocol(fractional-scale-v1)
    wayland_protocol(pointer-constraints-unstable-v1)
    wayland_protocol(relative-pointer-unstable-v1)
    wayland_protocol(text-input-unstable-v3)
    wayland_protocol(presentation-time)

    # And the core protocol's interfaces -- wl_surface_interface and the
    # rest -- as the library's own, hidden, copies: libwayland-client is
    # opened at run time (src/backend/wayland/wl_dyn.c), and the tables
    # above point at these by name, which a library not yet loaded cannot
    # answer. From the XML the installed headers were made from, not a
    # vendored one: the headers' inline requests index these tables by
    # opcode, so the two must be the same version. libwayland compares
    # interfaces by name, so its own copies and these agree.
    pkg_get_variable(_wl_datadir wayland-client pkgdatadir)
    if(NOT _wl_datadir OR NOT EXISTS ${_wl_datadir}/wayland.xml)
        message(FATAL_ERROR "wayland.xml not found under '${_wl_datadir}' "
                            "(wayland-client's pkgdatadir)")
    endif()
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/wayland-core-protocol.c
        COMMAND ${WAYLAND_SCANNER} private-code ${_wl_datadir}/wayland.xml
                ${FNL_GEN_DIR}/wayland-core-protocol.c
        DEPENDS ${_wl_datadir}/wayland.xml VERBATIM)
    list(APPEND FNL_PROTO_SRCS ${FNL_GEN_DIR}/wayland-core-protocol.c)
endif()

# ---- Shader codegen -------------------------------------------------------
# GLSL becomes SPIR-V at build time, embedded as a C array: no .spv to ship.
# FindVulkan sets the variable only on CMake >= 3.21, hence the PATH search.
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    find_program(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE glslangValidator)
endif()
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR
        "glslangValidator not found (install glslang / the Vulkan SDK)")
endif()
# Every module the library and the testbed embed goes through SPIRV-Tools'
# validator as it is written: glslang checks the GLSL, and this checks what
# it made of it, which is what the driver is handed. Only where this glslang
# was built with SPIRV-Tools, which its --help says.
execute_process(COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} --help
                OUTPUT_VARIABLE _glslang_help ERROR_VARIABLE _glslang_help)
set(FNL_SPIRV_VAL)
if(_glslang_help MATCHES "--spirv-val")
    set(FNL_SPIRV_VAL --spirv-val)
else()
    message(STATUS "glslang has no --spirv-val; SPIR-V goes unvalidated")
endif()

# ---- Shader translation ----------------------------------------------------
# One source per shader: the GLSL above, for Vulkan. OpenGL, Direct3D 12 and
# Metal get spirv-cross's translation of its SPIR-V, made here and checked as
# it is made -- GLSL 4.50 by glslang's OpenGL front end, each program's
# stages linked so their `Push` blocks are seen to agree; HLSL by glslang's
# HLSL front end. MSL has no compiler off a Mac, and is checked by being
# made: spirv-cross refuses what Metal cannot express. A Vulkan tree has no
# use for any of them and translates where spirv-cross is installed, as a
# check that a shader edit has not left the other three APIs behind. DESIGN
# *One shader source*.
#
# The conventions the renderers rely on: the push-constant block is named
# `Push` and becomes a std140 uniform block of that name (GL) or `cbuffer
# Push` (HLSL) or `constant Push& [[buffer(0)]]` (MSL); a vertex input at
# location N is TEXCOORDN in HLSL and [[attribute(N)]] in MSL; a binding is
# its own number in MSL; and HLSL and MSL flip Y at the end of each vertex
# stage, since Direct3D's and Metal's clip space points up where Vulkan's
# points down. GL needs no flip: that renderer draws into images of its
# own, row 0 at the top as Vulkan's are, and flips once onto the window.
find_program(FNL_SPIRV_CROSS spirv-cross)
if(FNL_SPIRV_CROSS)
    set(FNL_SHADER_XLATE ON)
    file(MAKE_DIRECTORY ${FNL_GEN_DIR}/spv ${FNL_GEN_DIR}/gl
                        ${FNL_GEN_DIR}/gl41 ${FNL_GEN_DIR}/hlsl
                        ${FNL_GEN_DIR}/msl)
elseif(NOT FNL_RENDERERS STREQUAL "vulkan")
    message(FATAL_ERROR "every renderer but Vulkan draws with spirv-cross's "
                        "translation of the shaders, and spirv-cross is not "
                        "installed")
else()
    set(FNL_SHADER_XLATE OFF)
    message(STATUS "spirv-cross not found; shaders are not translated for "
                   "OpenGL, Direct3D and Metal")
endif()
set(FNL_HLSL_MODEL 60)
# MSL 2.1, which every Mac that has macOS 13 takes; compiled by the device at
# run time, since only Xcode has Metal's offline compiler. The flags are the
# conventions above, and a consumer translating its own shaders for Metal
# passes the same: in the cache, for the reason FNL_EMBED_TEXT is below.
set(FNL_MSL_FLAGS --msl --msl-version 20100 --flip-vert-y
    --msl-decoration-binding CACHE INTERNAL
    "spirv-cross's flags for a shader the Metal renderer's conventions take")
# GLSL 4.10 for the GL renderer's 4.1 profile, the depth range mapped in
# the vertex stage (--fixup-clipspace: z = 2z - w) since 4.1 has no
# glClipControl, and no binding qualifiers (4.20's): a `Push` block is bound
# by name, as at 4.50, and each program has one sampler, on unit 0, which is
# where an unbound sampler uniform reads. In the cache for a consumer's own
# shaders, as the Metal flags are. DESIGN *OpenGL 4.1*.
set(FNL_GL41_FLAGS --version 410 --no-es --glsl-emit-push-constant-as-ubo
    --no-420pack-extension --fixup-clipspace CACHE INTERNAL
    "spirv-cross's flags for a shader the GL renderer's 4.1 profile takes")
# Direct3D 12 loads DXIL, compiled from the translation by Microsoft's dxc
# for Linux, whose validator signs it (tools/dxc.py fetches it, pinned, and
# build.sh names it here). A tool the build runs, like glslangValidator, and
# never linked. DESIGN *One shader source*.
if(d3d12 IN_LIST FNL_RENDERERS)
    set(FNL_DXC "" CACHE FILEPATH "Microsoft's dxc: tools/dxc.py --path")
    if(NOT FNL_DXC OR NOT EXISTS ${FNL_DXC})
        message(FATAL_ERROR "the Direct3D 12 renderer draws with DXIL that "
                            "dxc compiles, and FNL_DXC names no dxc: "
                            "-DFNL_DXC=$(python3 tools/dxc.py --path)")
    endif()
    file(MAKE_DIRECTORY ${FNL_GEN_DIR}/dxil)
endif()
# The script that embeds a translation as a C string, for a consumer that
# draws with OpenGL and translates its own shaders the same way. In the
# cache because a variable set here is not seen by the directory that added
# this one.
set(FNL_EMBED_TEXT ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
    CACHE INTERNAL "Embeds a text file as a C string: cmake -P")

# embed_shader(<name> <var> [<out> <define>])
# shaders/<name> as `const uint32_t <var>[]` in generated/<name>.h. The two
# optional arguments compile it again under -D<define>, into <out>.h.
#
# Translated as well where spirv-cross is: generated/gl/<stem>.<stage> and
# its `static const char <stem>_<stage>_glsl[]` in a header beside it;
# generated/gl41/<stem>.<stage> and its `<stem>_<stage>_glsl41[]`, GLSL 4.10
# for the GL renderer's 4.1 profile, with no binding qualifiers and Vulkan's
# depth range mapped to GL's in the vertex stage (DESIGN *OpenGL 4.1*);
# generated/hlsl/<stem>.<stage>, and generated/msl/<stem>.<stage> with its
# `<stem>_<stage>_msl[]` likewise. The stem is <out> without the stage, its
# dots made underscores -- text.frag.lcd is text_lcd.frag -- so that glslang
# reads the stage off the name when it links a program.
function(embed_shader name var)
    set(out ${name})
    set(define)
    if(ARGC GREATER 3)
        set(out ${ARGV2})
        set(define -D${ARGV3})
    endif()
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name})
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/${out}.h
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${FNL_SPIRV_VAL}
                ${define} ${src}
                --vn ${var} -o ${FNL_GEN_DIR}/${out}.h
        DEPENDS ${src} VERBATIM)
    list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/${out}.h)

    if(FNL_SHADER_XLATE)
        string(REGEX REPLACE "^.*\\." "" stage ${name})
        string(REPLACE ".${stage}" "" stem ${out})
        string(REPLACE "." "_" stem ${stem})
        set(spv  ${FNL_GEN_DIR}/spv/${stem}.${stage}.spv)
        set(gl   ${FNL_GEN_DIR}/gl/${stem}.${stage})
        set(gl41 ${FNL_GEN_DIR}/gl41/${stem}.${stage})
        set(hlsl ${FNL_GEN_DIR}/hlsl/${stem}.${stage})
        set(msl  ${FNL_GEN_DIR}/msl/${stem}.${stage})
        # Made under .new names and renamed into place only once every
        # check has passed: a make that stopped at a check would otherwise
        # leave the outputs newer than the shader, and the next one would
        # not run the check again.
        add_custom_command(
            OUTPUT ${gl}.h ${gl} ${gl41}.h ${gl41} ${hlsl} ${msl}.h
            BYPRODUCTS ${spv} ${hlsl}.spv ${gl}.new ${gl}.h.new ${hlsl}.new
                       ${gl41}.new ${gl41}.h.new
                       ${msl} ${msl}.new ${msl}.h.new
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${define}
                    ${src} -o ${spv}
            COMMAND ${FNL_SPIRV_CROSS} ${spv} --version 450 --no-es
                    --glsl-emit-push-constant-as-ubo --output ${gl}.new
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -S ${stage}
                    ${gl}.new
            COMMAND ${CMAKE_COMMAND} -DIN=${gl}.new -DOUT=${gl}.h.new
                    -DVAR=${stem}_${stage}_glsl
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            COMMAND ${FNL_SPIRV_CROSS} ${spv} ${FNL_GL41_FLAGS}
                    --output ${gl41}.new
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -S ${stage}
                    ${gl41}.new
            COMMAND ${CMAKE_COMMAND} -DIN=${gl41}.new -DOUT=${gl41}.h.new
                    -DVAR=${stem}_${stage}_glsl41
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            COMMAND ${FNL_SPIRV_CROSS} ${spv} --hlsl
                    --shader-model ${FNL_HLSL_MODEL} --flip-vert-y
                    --output ${hlsl}.new
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V -D -e main
                    -S ${stage} ${hlsl}.new -o ${hlsl}.spv
            COMMAND ${FNL_SPIRV_CROSS} ${spv} ${FNL_MSL_FLAGS}
                    --output ${msl}.new
            COMMAND ${CMAKE_COMMAND} -DIN=${msl}.new -DOUT=${msl}.h.new
                    -DVAR=${stem}_${stage}_msl
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            COMMAND ${CMAKE_COMMAND} -E rename ${gl}.new ${gl}
            COMMAND ${CMAKE_COMMAND} -E rename ${gl}.h.new ${gl}.h
            COMMAND ${CMAKE_COMMAND} -E rename ${gl41}.new ${gl41}
            COMMAND ${CMAKE_COMMAND} -E rename ${gl41}.h.new ${gl41}.h
            COMMAND ${CMAKE_COMMAND} -E rename ${hlsl}.new ${hlsl}
            COMMAND ${CMAKE_COMMAND} -E rename ${msl}.new ${msl}
            COMMAND ${CMAKE_COMMAND} -E rename ${msl}.h.new ${msl}.h
            DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            COMMENT "Translating ${out} for OpenGL, Direct3D and Metal"
            VERBATIM)
        list(APPEND FNL_GEN_FILES ${gl}.h ${gl41}.h ${hlsl} ${msl}.h)
        # And signed DXIL for Direct3D 12, as a byte array named
        # <stem>_<stage>_dxil, the entry spirv-cross names `main`.
        if(d3d12 IN_LIST FNL_RENDERERS)
            if(stage STREQUAL vert)
                set(profile vs_6_0)
            else()
                set(profile ps_6_0)
            endif()
            set(dxil ${FNL_GEN_DIR}/dxil/${stem}.${stage}.h)
            add_custom_command(
                OUTPUT ${dxil}
                COMMAND ${FNL_DXC} -nologo -T ${profile} -E main
                        -Vn ${stem}_${stage}_dxil -Fh ${dxil} ${hlsl}
                DEPENDS ${hlsl} ${FNL_DXC}
                COMMENT "Compiling ${out} to DXIL"
                VERBATIM)
            list(APPEND FNL_GEN_FILES ${dxil})
        endif()
    endif()
    set(FNL_GEN_FILES ${FNL_GEN_FILES} PARENT_SCOPE)
endfunction()

# gl_program(<name> <stage file>...)
# The translated stages of one GL program, linked by glslang as the driver
# will link them. A stage whose `Push` block differs from another's fails
# here, at build time, and not in a driver's link log.
function(gl_program name)
    if(NOT FNL_SHADER_XLATE)
        return()
    endif()
    foreach(_dir gl gl41)
        set(files)
        foreach(_s ${ARGN})
            list(APPEND files ${FNL_GEN_DIR}/${_dir}/${_s})
        endforeach()
        set(stamp ${FNL_GEN_DIR}/${_dir}/${name}.linked)
        add_custom_command(
            OUTPUT ${stamp}
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -l ${files}
            COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
            DEPENDS ${files}
            COMMENT "Linking the GL program ${name} (${_dir})"
            VERBATIM)
        list(APPEND FNL_GEN_FILES ${stamp})
    endforeach()
    set(FNL_GEN_FILES ${FNL_GEN_FILES} PARENT_SCOPE)
endfunction()

# ---- Warnings -------------------------------------------------------------
# One set, for the library, the examples and the host tools alike. Absent on
# purpose: -Wcast-qual (fires only inside the generated Wayland headers) and
# -Wswitch-enum (the VkResult and present-mode switches have a default:).
set(FNL_WARNINGS -Wall -Wextra
                -Wshadow -Wpointer-arith -Wstrict-prototypes
                -Wmissing-prototypes -Wold-style-definition
                -Wredundant-decls -Wwrite-strings -Wundef -Wvla
                -Wdouble-promotion -Wformat=2
                -Wmissing-field-initializers -Wsign-compare)
# Warning-clean is a rule under clang as under gcc. -Wthread-safety reads the
# FNL_GUARDED_BY/FNL_REQUIRES annotations (core/compiler.h), which are no-ops
# elsewhere; gcc has no such analysis, so there it is simply not asked for.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND FNL_WARNINGS -Wthread-safety -Wconditional-uninitialized
                             -Wcomma -Wunreachable-code -Wloop-analysis
                             -Wimplicit-fallthrough -Wassign-enum)
endif()
# gcc's, beyond -Wall -Wextra, each clean on this tree when it was added.
# Tried and left out, for a reason that is the tree's and not the warning's:
# -Wjump-misses-init (204 hits, every one a `goto fail` past a declaration,
# which is the unwind style here), -Wformat-truncation=2 (it also warns on
# calls whose length is checked, which level 1 in -Wall does not),
# -Wlogical-op (EAGAIN || EWOULDBLOCK, which POSIX lets differ) and
# -Wcast-align=strict (casts into the mapped overlay arena, aligned by
# construction and pinned by an assert in app.h).
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND FNL_WARNINGS -Wnull-dereference -Wduplicated-cond
                             -Wduplicated-branches -Wformat-overflow=2
                             -Wstringop-overflow=4 -Walloca)
endif()
# The clang trees build with this ON (build.sh), which is what makes the rule
# a check: a warning there fails the build. Off by default, because a
# consumer's newer compiler must not fail on a warning nobody here has seen.
option(FNL_WERROR "Treat warnings as errors" OFF)
if(FNL_WERROR)
    list(APPEND FNL_WARNINGS -Werror)
endif()

# ---- Host tools -----------------------------------------------------------
# Programs that run *during* the build. -fno-sanitize=all keeps them out of
# whatever CMAKE_C_FLAGS instruments the tree with: an instrumented generator
# is slower for nothing and fails the build on its own exit status. They get
# no -march=native and no LTO either, which are applied per target below.
#
# SANITIZED after the name leaves a tool in: for one whose whole purpose is to
# run the library's code on hostile input, where the checker is the detector.
#
# Not host tools in CMake's sense: a cross build would compile them for the
# target, which is why a cross tree runs none of them.
check_c_compiler_flag("-fno-sanitize=all" FNL_HAS_NO_SANITIZE)
function(fnl_host_tool name)
    set(_srcs ${ARGN})
    list(FIND _srcs SANITIZED _sanitized)
    list(REMOVE_ITEM _srcs SANITIZED)
    add_executable(${name} ${_srcs})
    # C only: fuzz_text assembles the font stub, where a C warning draws a
    # "not valid for assembler" note (fenestal_objects' reason).
    target_compile_options(${name} PRIVATE -O2
                           $<$<COMPILE_LANGUAGE:C>:${FNL_WARNINGS}>)
    if(FNL_HAS_NO_SANITIZE AND _sanitized EQUAL -1)
        target_compile_options(${name} PRIVATE -fno-sanitize=all)
        target_link_options(${name} PRIVATE -fno-sanitize=all)
    endif()
endfunction()

# ---- Baked font -----------------------------------------------------------
# The overlay's glyphs are rasterised by FreeType during the build, once per
# surface scale, and the coverage goes into the library as a blob. See
# tools/mkfont.c and src/renderer/vulkan/text.c.
#
# The blob is .incbin'd rather than compiled as a C array, which would be
# megabytes of initialiser for the compiler to chew through.
set(FNL_FONT_HDRS ${FNL_GEN_DIR}/font_data.h ${FNL_GEN_DIR}/font_metrics.h)

# Marking the stack non-executable is an ELF note; a PE assembler rejects the
# directive, and PE keeps that bit in the image header anyway.
if(WIN32)
    set(FNL_GNU_STACK "")
else()
    set(FNL_GNU_STACK "    .section .note.GNU-stack,\"\",%progbits\n")
endif()

# On macOS the blobs are C, not assembler: the archive is made from the
# library's objects as LLVM bitcode (*The archive*, below), which an
# .incbin stub is not, and a symbol it defines would be one global more.
# #embed is C23's, which clang takes in C11 as an extension and is quick
# about; the stub says so to itself, so that the warning stays on
# elsewhere. Aligned to four, as the assembler's .balign is. A blob given a
# terminator is text, and typed as the C that reads it declares it.
set(FNL_EMBED_PROLOGUE "/* Generated by CMakeLists.txt -- do not edit. */
#pragma clang diagnostic ignored \"-Wc23-extensions\"
")
function(fnl_embed_c out)
    set(_c "${FNL_EMBED_PROLOGUE}")
    while(ARGN)
        list(POP_FRONT ARGN _sym _file _nul)
        if(_nul)
            set(_type "char")
        else()
            set(_type "unsigned char")
        endif()
        string(APPEND _c "_Alignas(4) const ${_type} ${_sym}[] = {
#embed \"${_file}\"
")
        if(_nul)
            string(APPEND _c ", 0")
        endif()
        string(APPEND _c "\n};\n")
    endwhile()
    file(WRITE ${out}.in "${_c}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${out}.in ${out})
endfunction()

if(FNL_FONT_FROM)
    # Borrowed. The headers are copied by a build rule and the stub names the
    # lender's blobs where they lie, so a re-bake over there rebuilds here.
    #
    # The stub has a name of its own: mkfont's font_atlas.S is an output of
    # the bake, and a tree that goes back to baking must not find this one in
    # its place, newer than everything and naming files that may be gone.
    foreach(_f font_data.h font_metrics.h font_atlas.bin font_cells.bin)
        if(NOT EXISTS ${FNL_FONT_FROM}/${_f})
            message(FATAL_ERROR
                "no ${_f} in ${FNL_FONT_FROM}; FNL_FONT_DIR and "
                "FNL_GENERATED_DIR name a completed native build's generated "
                "directory")
        endif()
    endforeach()
    set(FNL_FONT_BINS ${FNL_FONT_FROM}/font_atlas.bin
                      ${FNL_FONT_FROM}/font_cells.bin)
    set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_borrowed.S)
    if(APPLE)
        set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_borrowed.c)
        fnl_embed_c(${FNL_FONT_STUB}
                    font_atlas_blob ${FNL_FONT_FROM}/font_atlas.bin ""
                    font_cells_blob ${FNL_FONT_FROM}/font_cells.bin "")
    endif()
    # Staged and installed with copy_if_different, as every file written at
    # configure is: file(WRITE) alone bumps the mtime and a re-configure would
    # then reassemble the stub and relink everything.
    file(WRITE ${FNL_GEN_DIR}/font_borrowed.S.in
"/* Generated by CMakeLists.txt -- do not edit. */
    .section .rodata
    .globl font_atlas_blob
    .balign 4
font_atlas_blob:
    .incbin \"${FNL_FONT_FROM}/font_atlas.bin\"
    .globl font_cells_blob
    .balign 4
font_cells_blob:
    .incbin \"${FNL_FONT_FROM}/font_cells.bin\"
${FNL_GNU_STACK}")
    if(NOT APPLE)
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${FNL_FONT_STUB}.in ${FNL_FONT_STUB})
    endif()
    foreach(_h font_data.h font_metrics.h)
        add_custom_command(
            OUTPUT ${FNL_GEN_DIR}/${_h}
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${FNL_FONT_FROM}/${_h} ${FNL_GEN_DIR}/${_h}
            DEPENDS ${FNL_FONT_FROM}/${_h} VERBATIM)
    endforeach()
else()
    fnl_host_tool(mkfont
                  tools/mkfont.c tools/font_charset.h tools/font_codec.h
                  tools/font_raster.h)
    target_link_libraries(mkfont PRIVATE PkgConfig::FREETYPE m)

    # Order matters: mkfont's FACE[] indexes these, and face 0 -- the
    # monospaced one -- is what the code point index and the metrics come off.
    set(FONT_TTF
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/ubuntu/UbuntuSansMono-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/ubuntu/UbuntuSans-Regular.ttf)
    set(FNL_FONT_BINS ${FNL_GEN_DIR}/font_atlas.bin
                      ${FNL_GEN_DIR}/font_cells.bin)
    set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_atlas.S)
    # mkfont writes each under a .new name and renames them all at the end,
    # as the shader rule does and for its reason.
    set(_font_new)
    foreach(_f ${FNL_FONT_HDRS} ${FNL_FONT_STUB} ${FNL_FONT_BINS})
        list(APPEND _font_new ${_f}.new)
    endforeach()
    add_custom_command(
        OUTPUT ${FNL_FONT_HDRS} ${FNL_FONT_STUB} ${FNL_FONT_BINS}
        BYPRODUCTS ${_font_new}
        COMMAND mkfont ${FONT_TTF} ${FNL_GEN_DIR}
        DEPENDS mkfont ${FONT_TTF} VERBATIM)
endif()

# font_metrics.h is included by app.h and so by every translation unit, here
# and in the verify tools. One target owns the rule that makes it and the
# others depend on that target: an output listed as a source of several
# targets is a rule each may run, and in a parallel build they collide.
add_custom_target(fenestal_font DEPENDS ${FNL_FONT_HDRS})

# ---- the licence, in the binary --------------------------------------------
# The baked atlas is a Modified Version of the Ubuntu font software under the
# UFL, renamed Fenestal Glyphs in COPYRIGHT.txt as its condition 2(b) asks,
# and its condition 1 has every copy carry the copyright notice and the
# licence text. A binary handed on without the repo would not, so both are
# .incbin'd, with a NUL so each reads as a string; --licence prints them.
set(FONT_LICENCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fonts/ubuntu)
set(FNL_LICENCE_STUB ${FNL_GEN_DIR}/font_licence.S)
if(APPLE)
    set(FNL_LICENCE_STUB ${FNL_GEN_DIR}/font_licence.c)
    fnl_embed_c(${FNL_LICENCE_STUB}
                ubuntu_copyright_txt ${FONT_LICENCE_DIR}/COPYRIGHT.txt 1
                ubuntu_licence_txt ${FONT_LICENCE_DIR}/LICENCE.txt 1)
endif()
file(WRITE ${FNL_GEN_DIR}/font_licence.S.in
"/* Generated by CMakeLists.txt -- do not edit. */
    .section .rodata
    .globl ubuntu_copyright_txt
ubuntu_copyright_txt:
    .incbin \"${FONT_LICENCE_DIR}/COPYRIGHT.txt\"
    .byte 0
    .globl ubuntu_licence_txt
ubuntu_licence_txt:
    .incbin \"${FONT_LICENCE_DIR}/LICENCE.txt\"
    .byte 0
${FNL_GNU_STACK}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FNL_GEN_DIR}/font_licence.S.in ${FNL_GEN_DIR}/font_licence.S)

# verifyfont re-runs the whole rasterisation through FreeType and compares
# the blob texel by texel, on every build that re-bakes. Only where the bake
# ran: a borrowing tree's bake was checked by the tree that lent it, and
# build.sh lends none without font_verified.stamp newer than its atlas
# (font_ready()).
if(NOT FNL_FONT_FROM)
    fnl_host_tool(verifyfont
                  tools/verifyfont.c tools/font_charset.h tools/font_codec.h
                  tools/font_raster.h)
    target_include_directories(verifyfont PRIVATE ${FNL_GEN_DIR})
    target_link_libraries(verifyfont PRIVATE PkgConfig::FREETYPE m)
    add_dependencies(verifyfont fenestal_font)

    if(FNL_VERIFY_FONT)
        add_custom_command(
            OUTPUT ${FNL_GEN_DIR}/font_verified.stamp
            COMMAND verifyfont ${FONT_TTF} ${FNL_GEN_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch
                    ${FNL_GEN_DIR}/font_verified.stamp
            DEPENDS verifyfont ${FNL_GEN_DIR}/font_data.h ${FNL_FONT_BINS}
            COMMENT "Verifying baked font atlases against FreeType"
            VERBATIM)
        list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/font_verified.stamp)
    endif()
endif()

# verifykeymap and the fuzz harnesses below run the library's own code on
# hostile input. A native Linux tree builds and runs them; a Linux cross
# tree -- linux-arm64 -- builds them for its processor and runs none, and
# its machine does, from the commands every tree that builds them writes to
# generated/checks.txt (tools/probe/bare_checks.py): the same programs,
# arguments and counts as the build's, in one place. Not Windows or macOS,
# whose backends apply no keymap of the library's.
set(FNL_CHECKS_BUILT FALSE)
set(FNL_CHECKS_RUN FALSE)
if(NOT FNL_BACKEND MATCHES "^(win32|cocoa)$")
    set(FNL_CHECKS_BUILT TRUE)
    if(NOT FNL_GENERATED_DIR)
        set(FNL_CHECKS_RUN TRUE)
    endif()
endif()
set(FNL_CHECK_LINES "")

# verifykeymap asks keymap.c about a keymap with every case in it, then feeds
# its parser that keymap damaged: the part of text entry no round can cover
# from outside, since a round types on whatever desk it is given. It also
# resolves names through a miniature xkeyboard-config tree it writes, and
# through the build machine's own where that is there, the way the KMS
# backend does (keymap_rules.c). On every build of the three backends that
# apply a keymap, and run where the build can run it. Instrumented where the
# tree is: a read past a damaged keymap's end is only seen by a checker, and
# the tool is quick and frees what it takes.
#
# No graphics API's headers: app.h names none, so the tool builds on a
# machine with only one renderer's.
if(FNL_CHECKS_BUILT)
    fnl_host_tool(verifykeymap SANITIZED tools/verifykeymap.c
                  src/backend/keymap.c src/backend/keymap_xkb.c
                  src/backend/keymap_rules.c)
    target_include_directories(verifykeymap PRIVATE ${FNL_GEN_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src
                               ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_dependencies(verifykeymap fenestal_font)
    string(APPEND FNL_CHECK_LINES "verifykeymap\n")
endif()
if(FNL_CHECKS_RUN)
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/keymap_verified.stamp
        COMMAND verifykeymap
        COMMAND ${CMAKE_COMMAND} -E touch ${FNL_GEN_DIR}/keymap_verified.stamp
        DEPENDS verifykeymap
        COMMENT "Verifying the layout code against a keymap and the files"
        VERBATIM)
    list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/keymap_verified.stamp)
endif()

# ---- The headers, as C11 and as C++ ---------------------------------------
# Compiled alone and never linked, once as C11 and once per C++ standard, all
# pedantic and warning-free. A C++ compiler is looked for and not required:
# without one the C++ half says so and is skipped.
include(CheckLanguage)
check_language(CXX)
set(_hc_inc -I${CMAKE_CURRENT_SOURCE_DIR}/include)
if(vulkan IN_LIST FNL_RENDERERS)
    get_target_property(_vk_inc Vulkan::Vulkan INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_d ${_vk_inc})
        list(APPEND _hc_inc -I${_d})
    endforeach()
    list(APPEND _hc_inc -DFNL_CHECK_VULKAN)
endif()
# <d3d12.h> is the Windows tree's alone, where the renderer is.
if(d3d12 IN_LIST FNL_RENDERERS)
    list(APPEND _hc_inc -DFNL_CHECK_D3D12)
endif()
set(_hc_src ${CMAKE_CURRENT_SOURCE_DIR}/tools/headercheck.c)
set(_hc_flags -fsyntax-only -pedantic-errors -Wall -Wextra -Werror)
# A custom command gets none of CMake's own target and sysroot flags, and the
# macOS tree's clang is the host's: without these it checks the headers for
# Linux, against the host's libc.
set(_hc_c_on)
set(_hc_cxx_on)
if(APPLE)
    set(_hc_c_on --target=${CMAKE_C_COMPILER_TARGET}
                 -isysroot ${CMAKE_OSX_SYSROOT})
    set(_hc_cxx_on --target=${CMAKE_CXX_COMPILER_TARGET}
                   -isysroot ${CMAKE_OSX_SYSROOT})
endif()
set(_hc_cmds COMMAND ${CMAKE_C_COMPILER} ${_hc_c_on} -std=c11 ${_hc_flags}
              ${_hc_inc} -x c ${_hc_src})
if(CMAKE_CXX_COMPILER)
    foreach(_std 11 14 17 20)
        list(APPEND _hc_cmds COMMAND ${CMAKE_CXX_COMPILER} ${_hc_cxx_on}
             -std=c++${_std} ${_hc_flags} ${_hc_inc} -x c++ ${_hc_src})
    endforeach()
else()
    message(STATUS "No C++ compiler: the headers are checked as C11 only")
endif()
# And as Objective-C under ARC against the SDK, where fnl_metal.h's types
# are Metal's own and its fields __unsafe_unretained.
if(APPLE)
    list(APPEND _hc_cmds COMMAND ${CMAKE_OBJC_COMPILER}
         --target=${CMAKE_OBJC_COMPILER_TARGET}
         -isysroot ${CMAKE_OSX_SYSROOT} -std=c11 -fobjc-arc ${_hc_flags}
         ${_hc_inc} -x objective-c ${_hc_src})
endif()
add_custom_command(
    OUTPUT ${FNL_GEN_DIR}/headers_checked.stamp
    ${_hc_cmds}
    COMMAND ${CMAKE_COMMAND} -E touch ${FNL_GEN_DIR}/headers_checked.stamp
    DEPENDS ${_hc_src} ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fenestal.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_vulkan.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_gl.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_metal.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_d3d12.h
    COMMENT "Compiling the public headers as C11 and C++11 to C++20"
    VERBATIM)
list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/headers_checked.stamp)

# ---- The ABI, against its baseline ----------------------------------------
# tools/abidump prints every public struct's size and offsets and every enum
# constant; with the export list (fenestal.syms) it is compared against
# tools/abi/ on every native build, so a header edit that moves what a
# compiled caller depends on fails here. tools/abi/check.cmake says what to
# do about a difference. A cross build cannot run the dump; the layout is the
# same on both 64-bit ABIs the project targets, and the native trees hold it.
# A baseline per renderer set: the interop headers and the exports differ. The
# layout holds every interop header's structs, OpenGL's whatever the build,
# so it is one of two (fenestal-0 with Vulkan's, fenestal-0-gl without);
# the exports are one list per set -- fenestal-0 for Vulkan alone, -gl for
# OpenGL alone, -all for Linux's two. The portable part is in each, and a
# change to it updates each.
if(NOT FNL_GENERATED_DIR)
    fnl_host_tool(abidump tools/abidump.c)
    target_include_directories(abidump PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(vulkan IN_LIST FNL_RENDERERS)
        target_include_directories(abidump SYSTEM PRIVATE
            $<TARGET_PROPERTY:Vulkan::Vulkan,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(abidump PRIVATE FNL_ABI_VULKAN)
        set(_abi_layout fenestal-0)
    else()
        set(_abi_layout fenestal-0-gl)
    endif()
    if(FNL_RENDERER_COUNT GREATER 1)
        set(_abi_syms fenestal-0-all)
    elseif(FNL_RENDERER STREQUAL vulkan)
        set(_abi_syms fenestal-0)
    else()
        set(_abi_syms fenestal-0-${FNL_RENDERER})
    endif()
    set(_abi_dir ${CMAKE_CURRENT_SOURCE_DIR}/tools/abi)
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/abi_checked.stamp
        COMMAND abidump > ${FNL_GEN_DIR}/fenestal.abi
        COMMAND ${CMAKE_COMMAND} -DABI=${FNL_GEN_DIR}/fenestal.abi
                -DSYMS=${FNL_GEN_DIR}/fenestal.syms
                -DBASE_ABI=${_abi_dir}/${_abi_layout}.abi
                -DBASE_SYMS=${_abi_dir}/${_abi_syms}.syms
                -P ${_abi_dir}/check.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${FNL_GEN_DIR}/abi_checked.stamp
        DEPENDS abidump ${FNL_GEN_DIR}/fenestal.syms
                ${_abi_dir}/${_abi_layout}.abi ${_abi_dir}/${_abi_syms}.syms
                ${_abi_dir}/check.cmake
        COMMENT "Checking the public ABI against tools/abi"
        VERBATIM)
    list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/abi_checked.stamp)
endif()

# ---- Fuzz harnesses ---------------------------------------------------------
# The parsers that take another program's bytes, each behind libFuzzer's entry
# point (tools/fuzz/). Every native tree builds each against replay.c and runs
# it over the committed corpus and a fixed-seed run of damaged copies, so the
# DESIGN's campaigns are a regression net and not a paragraph. Instrumented
# where the tree is, as verifykeymap is and for its reason.
#
# The clang trees also build each against libFuzzer, with ASan and UBSan, for
# a campaign: built so that they keep compiling, never run by the build.
#
#   build/linux/clang/fuzz_keymap_lf -max_total_time=600 \
#       tools/fuzz/corpus/keymap
#
# FNL_FUZZ_RUNS scales the damaged copies; the counts below are what keeps
# the asan tree's run to a few seconds a harness.
set(FNL_FUZZ_RUNS 1 CACHE STRING "Multiplier on the in-build fuzz runs")
set(FNL_FUZZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/fuzz)
if(FNL_CHECKS_BUILT)
    set(_fuzz_keymap_src tools/fuzz/fuzz_keymap.c src/backend/keymap.c
                         src/backend/keymap_xkb.c)
    set(_fuzz_rules_src  tools/fuzz/fuzz_rules.c src/backend/keymap.c
                         src/backend/keymap_xkb.c src/backend/keymap_rules.c)
    set(_fuzz_font_src   tools/fuzz/fuzz_font.c)
    set(_fuzz_text_src   tools/fuzz/fuzz_text.c src/core/text_measure.c
                         src/core/error.c ${FNL_FONT_STUB})
    set(_fuzz_keymap_runs 3000)
    set(_fuzz_rules_runs  3000)
    set(_fuzz_font_runs   10000)
    set(_fuzz_text_runs   20000)

    set(_fuzz_lf FALSE)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # A probe of its own: libFuzzer brings main(), so the stock flag
        # check's program would define it twice.
        set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
        check_c_source_compiles("
            #include <stddef.h>
            #include <stdint.h>
            int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);
            int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
            { return d && n ? 0 : 0; }" FNL_HAS_LIBFUZZER_MAIN)
        unset(CMAKE_REQUIRED_FLAGS)
        set(_fuzz_lf ${FNL_HAS_LIBFUZZER_MAIN})
    endif()

    foreach(_h keymap rules font text)
        fnl_host_tool(fuzz_${_h} SANITIZED ${_fuzz_${_h}_src}
                      tools/fuzz/replay.c)
        set(_targets fuzz_${_h})
        if(_fuzz_lf)
            add_executable(fuzz_${_h}_lf ${_fuzz_${_h}_src})
            set(_san -fsanitize=fuzzer,address,undefined)
            target_compile_options(fuzz_${_h}_lf PRIVATE -O1 -g ${_san}
                                   $<$<COMPILE_LANGUAGE:C>:${FNL_WARNINGS}>)
            target_link_options(fuzz_${_h}_lf PRIVATE ${_san})
            list(APPEND _targets fuzz_${_h}_lf)
        endif()
        foreach(_t ${_targets})
            target_include_directories(${_t} PRIVATE ${FNL_GEN_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/tools
                ${CMAKE_CURRENT_SOURCE_DIR}/include)
            add_dependencies(${_t} fenestal_font)
        endforeach()

        math(EXPR _runs "${_fuzz_${_h}_runs} * ${FNL_FUZZ_RUNS}")
        string(APPEND FNL_CHECK_LINES
               "fuzz_${_h} corpus/${_h} -runs=${_runs}\n")
        if(NOT FNL_CHECKS_RUN)
            continue()
        endif()
        file(GLOB _corpus CONFIGURE_DEPENDS ${FNL_FUZZ_DIR}/corpus/${_h}/*)
        add_custom_command(
            OUTPUT ${FNL_GEN_DIR}/fuzz_${_h}.stamp
            COMMAND fuzz_${_h} ${FNL_FUZZ_DIR}/corpus/${_h} -runs=${_runs}
            COMMAND ${CMAKE_COMMAND} -E touch ${FNL_GEN_DIR}/fuzz_${_h}.stamp
            DEPENDS fuzz_${_h} ${_corpus}
            COMMENT "Fuzzing ${_h}: the corpus and ${_runs} damaged copies"
            VERBATIM)
        list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/fuzz_${_h}.stamp)
    endforeach()
endif()
if(FNL_CHECKS_BUILT)
    # A program and its arguments a line, the corpus as a path under the
    # directory the programs are run from; staged, as every file written
    # at configure is, so that a re-configure touches nothing.
    file(WRITE ${FNL_GEN_DIR}/checks.txt.in "${FNL_CHECK_LINES}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FNL_GEN_DIR}/checks.txt.in ${FNL_GEN_DIR}/checks.txt)
endif()

embed_shader(text.vert text_vert_spv)
embed_shader(text.frag text_frag_spv)
# Twice from one source: the subpixel variant needs dual-source blending, an
# optional device feature, so text.c picks between them at pipeline creation.
embed_shader(text.frag text_frag_lcd_spv text.frag.lcd SUBPIXEL)
embed_shader(shape.vert shape_vert_spv)
embed_shader(shape.frag shape_frag_spv)
embed_shader(rect.vert rect_vert_spv)
embed_shader(rect.frag rect_frag_spv)
# One covering triangle, and the fetch that puts the scene image back on the
# window. See src/renderer/vulkan/frame.c.
embed_shader(fullscreen.vert fullscreen_vert_spv)
embed_shader(blit.frag blit_frag_spv)
# A post-process over the finished scene. See DESIGN.md's *Antialiasing*.
embed_shader(mlaa.frag mlaa_frag_spv)

gl_program(text       text.vert       text.frag)
gl_program(text_lcd   text.vert       text_lcd.frag)
gl_program(rect       rect.vert       rect.frag)
gl_program(shape      shape.vert      shape.frag)
gl_program(blit       fullscreen.vert blit.frag)
gl_program(mlaa       fullscreen.vert mlaa.frag)

# ---- The library ----------------------------------------------------------
# Layered: src/core names no window server and no graphics API, and
# tools/check-layering.py is the check that it does not.
#
# keymap.c is what the Linux backends share for text entry: both window
# servers hand over keysyms and leave the layout to the client, and the bare
# display has nobody to apply one. Windows and macOS deliver characters and
# have no use for it. keymap_xkb.c reads a keymap as text, which is how
# Wayland sends one and how xkeyboard-config keeps them; keymap_rules.c
# turns the console's names into those files, which only KMS has to.
if(FNL_BACKEND STREQUAL win32)
    set(FNL_BACKEND_SOURCES src/backend/win32/win32.c
                            src/backend/win32/win32_rawin.c)
elseif(FNL_BACKEND STREQUAL cocoa)
    set(FNL_BACKEND_SOURCES src/backend/cocoa/cocoa.m)
else()
    set(FNL_BACKEND_SOURCES src/backend/keymap.c)
    if(wayland IN_LIST FNL_BACKENDS OR kms IN_LIST FNL_BACKENDS)
        list(APPEND FNL_BACKEND_SOURCES src/backend/keymap_xkb.c)
    endif()
    if(wayland IN_LIST FNL_BACKENDS)
        list(APPEND FNL_BACKEND_SOURCES src/backend/wayland/wayland.c
                                        src/backend/wayland/wl_dyn.c)
    endif()
    if(x11 IN_LIST FNL_BACKENDS)
        list(APPEND FNL_BACKEND_SOURCES src/backend/x11/xcb_dyn.c
                                        src/backend/x11/x11.c
                                        src/backend/x11/x11_ext.c
                                        src/backend/x11/x11_keys.c
                                        src/backend/x11/x11_clipboard.c)
    endif()
    if(kms IN_LIST FNL_BACKENDS)
        list(APPEND FNL_BACKEND_SOURCES src/backend/keymap_rules.c
                                        src/backend/kms/kms.c
                                        src/backend/kms/kms_driver.c)
    endif()
endif()

# The bridge is a (backend, renderer) pair and a translation unit of its
# own, so that choosing it is a file name here and not an #ifdef in a file
# that would then need both backends' headers: one for each backend the
# build has, each a table the renderer looks up by the backend that opened
# (VkBridge, EglBridge).
set(_fnl_vk_bridges)
set(_fnl_egl_bridges)
foreach(_p IN LISTS FNL_BACKENDS)
    list(APPEND _fnl_vk_bridges src/renderer/vulkan/surface_${_p}.c)
    list(APPEND _fnl_egl_bridges src/renderer/gl/egl_${_p}.c)
endforeach()
# Win32's Vulkan frames can go through DXGI instead (present_dxgi.c).
if(win32 IN_LIST FNL_BACKENDS)
    list(APPEND _fnl_vk_bridges src/renderer/vulkan/present_dxgi.c)
endif()
# Every renderer the build has, each its own directory; a build of several
# links them side by side, each behind its table (renderer.h,
# FNL_RENDERERS(X)). The *_dyn.c and vk_load.c files open each runtime at
# run time.
set(FNL_RENDERER_SOURCES)
if(gl IN_LIST FNL_RENDERERS)
    list(APPEND FNL_RENDERER_SOURCES src/renderer/gl/device.c
                                     src/renderer/gl/loader.c
                                     src/renderer/gl/gl_compat.c
                                     src/renderer/gl/target.c
                                     src/renderer/gl/overlay.c
                                     src/renderer/gl/frame.c
                                     src/renderer/gl/memory.c)
    if(FNL_BACKEND STREQUAL cocoa)
        list(APPEND FNL_RENDERER_SOURCES src/renderer/gl/cgl.m)
    else()
        list(APPEND FNL_RENDERER_SOURCES src/renderer/gl/egl.c
                                         src/renderer/gl/egl_dyn.c
                                         ${_fnl_egl_bridges})
    endif()
endif()
if(d3d12 IN_LIST FNL_RENDERERS)
    list(APPEND FNL_RENDERER_SOURCES src/renderer/d3d12/device.c
                                     src/renderer/d3d12/frame.c
                                     src/renderer/d3d12/target.c
                                     src/renderer/d3d12/overlay.c
                                     src/renderer/d3d12/surface_win32.c
                                     src/renderer/d3d12/present_d3d11.c)
endif()
if(metal IN_LIST FNL_RENDERERS)
    list(APPEND FNL_RENDERER_SOURCES src/renderer/metal/device.m
                                     src/renderer/metal/frame.m
                                     src/renderer/metal/target.m
                                     src/renderer/metal/overlay.m
                                     src/renderer/metal/surface_cocoa.m)
endif()
if(vulkan IN_LIST FNL_RENDERERS)
    list(APPEND FNL_RENDERER_SOURCES src/renderer/vulkan/device.c
                                     src/renderer/vulkan/vk_load.c
                                     src/renderer/vulkan/swapchain.c
                                     src/renderer/vulkan/scene_target.c
                                     src/renderer/vulkan/pipeline.c
                                     src/renderer/vulkan/frame.c
                                     src/renderer/vulkan/text.c
                                     src/renderer/vulkan/overlay.c
                                     ${_fnl_vk_bridges})
endif()

set(FNL_CORE_SOURCES
    src/core/app.c src/core/error.c src/core/frame.c
    src/core/decor.c src/core/text_measure.c src/core/draw.c
    src/core/overlay.c src/core/api_overlay.c src/core/api_state.c
    src/core/events.c src/core/controls.c src/core/echo.c
    src/core/fault.c src/core/drivers.c src/core/rank.c
    src/core/parts.c src/core/metrics.c)

set(FNL_SOURCES
    ${FNL_CORE_SOURCES} ${FNL_BACKEND_SOURCES} ${FNL_RENDERER_SOURCES}
    ${FNL_FONT_STUB} ${FNL_LICENCE_STUB}
    ${FNL_PROTO_SRCS} ${FNL_GEN_FILES})

# Built once into an OBJECT library and linked into both an archive and a
# shared object, so the two cannot differ in sources or flags. Position
# independent because the shared build needs it; that costs the static build
# a register on x86-64, which is cheaper than compiling everything twice.
add_library(fenestal_objects OBJECT ${FNL_SOURCES})
add_dependencies(fenestal_objects fenestal_font)
set_target_properties(fenestal_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
# FNL_BUILD turns FNL_API into an export, and the visibility preset is what
# makes that mean something: without it every symbol is exported anyway.
#
# Not on Windows, where an export is a directive compiled into the object and
# these objects are the archive's as well: a program linking fenestal::static
# would export the whole API from its own image. There the objects are built
# plain and the DLL's exports are a list -- see fenestal.def below.
if(WIN32)
    target_compile_definitions(fenestal_objects PRIVATE FNL_STATIC)
else()
    target_compile_definitions(fenestal_objects PRIVATE FNL_BUILD)
endif()
set_target_properties(fenestal_objects PROPERTIES C_VISIBILITY_PRESET hidden
                      OBJC_VISIBILITY_PRESET hidden)
# The Objective-C is ARC's: AppKit's and Metal's objects are owned by the
# compiler's rules, and the C state holds them as owned `void *` where it
# must (src/backend/cocoa/cocoa_state.h).
if(APPLE)
    target_compile_options(fenestal_objects PRIVATE
                           $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
endif()
# mingw's default code model reaches extern data through a .refptr.<name>
# pointer slot in a COMDAT section, and a program holds one slot per name:
# the linker keeps one copy and discards the rest. The archive makes our
# symbols local (below), so the library's slot for, say, __stack_chk_guard
# is then discarded in favour of the program's while our code still points
# into it -- every stack canary reads garbage, and __stack_chk_fail recurses
# until the stack is gone. The small model addresses data directly, which a
# DLL or an executable under 2 GB, which is every one, can.
if(WIN32)
    target_compile_options(fenestal_objects PRIVATE
                           $<$<COMPILE_LANGUAGE:C>:-mcmodel=small>)
endif()
target_compile_options(fenestal_objects PRIVATE
                       $<$<COMPILE_LANGUAGE:C,OBJC>:${FNL_PREFIX_MAP}>)
# What a consumer of either library inherits -- the public headers and the
# system's libraries -- on a target of its own. Not by linking the object
# library: an object library linked directly hands over its objects as well,
# and the archive is built from one partially linked object instead (below).
add_library(fenestal_usage INTERFACE)
target_include_directories(fenestal_usage INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(fenestal_objects PUBLIC fenestal_usage)
# tools/ for font_codec.h, the blob's container format: written by mkfont and
# decoded here, and one definition is how the two cannot drift.
target_include_directories(fenestal_objects PRIVATE ${FNL_GEN_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR}/tools)
# The runtimes are each one's headers here, and none is on the link line:
# what is not part of the system is opened at run time (src/core/dynlib.h),
# so a machine missing one still loads the program and steps past the
# backend or the renderer that wanted it. What a consumer links is the system's
# own: libc's dlopen (CMAKE_DL_LIBS: nothing from glibc 2.34, libdl before
# it), m and pthread on Linux; user32, gdi32 and Direct3D 12's DLLs, part
# of every Windows 10, on Windows; the frameworks on macOS.
function(fnl_headers_of target)
    target_include_directories(fenestal_objects SYSTEM PRIVATE
        $<TARGET_PROPERTY:${target},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_options(fenestal_objects PRIVATE
        $<TARGET_PROPERTY:${target},INTERFACE_COMPILE_OPTIONS>)
endfunction()
foreach(_g IN LISTS FNL_RENDERERS)
    string(TOUPPER ${_g} _G)
    target_compile_definitions(fenestal_objects PRIVATE FNL_HAVE_RENDERER_${_G})
endforeach()
target_link_libraries(fenestal_usage INTERFACE m)
if(NOT WIN32)
    target_link_libraries(fenestal_usage INTERFACE ${CMAKE_DL_LIBS})
endif()
if(gl IN_LIST FNL_RENDERERS AND FNL_BACKEND STREQUAL cocoa)
    # The frameworks, which are the runtime, as Metal is: OpenGL for CGL
    # and the entry points, IOSurface for the frame's way into Metal. And
    # the copied Khronos header for the library and for a consumer's scene,
    # which this SDK has no copy of.
    target_link_libraries(fenestal_usage INTERFACE "-framework OpenGL"
                          "-framework IOSurface")
    target_include_directories(fenestal_objects SYSTEM PRIVATE
                               ${FNL_GLCOREARB_DIR})
    target_include_directories(fenestal_usage SYSTEM INTERFACE
                               ${FNL_GLCOREARB_DIR})
elseif(gl IN_LIST FNL_RENDERERS)
    # GL itself is reached through what eglGetProcAddress hands out, so
    # nothing links libGL or libOpenGL.
    fnl_headers_of(PkgConfig::EGL)
    target_include_directories(fenestal_objects SYSTEM PRIVATE
                               ${FNL_GLCOREARB_DIR})
endif()
if(d3d12 IN_LIST FNL_RENDERERS)
    # The runtime and DXGI; dxguid for the interface ids, a static library.
    target_link_libraries(fenestal_usage INTERFACE d3d12 dxgi dxguid)
endif()
if(metal IN_LIST FNL_RENDERERS)
    # The framework, which is the runtime: a Mac has Metal as it has AppKit.
    target_link_libraries(fenestal_usage INTERFACE "-framework Metal")
endif()
if(vulkan IN_LIST FNL_RENDERERS)
    fnl_headers_of(Vulkan::Vulkan)
    # And a consumer's, since fnl_vulkan.h includes <vulkan/vulkan.h>: the
    # headers come with the library and the loader with nothing, as a scene
    # reaches Vulkan through FnlVulkanGpu::get_instance_proc_addr. Read now
    # rather than named by a generator expression, which a consumer's
    # directory would evaluate where the imported target may not be seen.
    get_target_property(_vk_usage_inc Vulkan::Vulkan
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(_vk_usage_inc)
        target_include_directories(fenestal_usage SYSTEM INTERFACE
                                   ${_vk_usage_inc})
    endif()
endif()
if(NOT FNL_BACKEND MATCHES "^(win32|cocoa)$")
    # Each define puts the backend in backend.h's list (FNL_BACKENDS(X)).
    # PRIVATE because nothing a consumer compiles sees that header. KMS adds
    # nothing beside the render thread: the DRM node and evdev are ioctls
    # against the kernel's own headers, and the layout is a subprocess.
    target_link_libraries(fenestal_usage INTERFACE Threads::Threads)
    foreach(_p IN LISTS FNL_BACKENDS)
        string(TOUPPER ${_p} _P)
        target_compile_definitions(fenestal_objects PRIVATE FNL_HAVE_${_P})
    endforeach()
    if(wayland IN_LIST FNL_BACKENDS)
        fnl_headers_of(PkgConfig::WAYLAND_CLIENT)
        if(gl IN_LIST FNL_RENDERERS)
            fnl_headers_of(PkgConfig::WAYLAND_EGL)
        endif()
    endif()
    if(x11 IN_LIST FNL_BACKENDS)
        fnl_headers_of(PkgConfig::XCB)
    endif()
elseif(FNL_BACKEND STREQUAL cocoa)
    # AppKit for the window and the events, QuartzCore for the CAMetalLayer
    # the frame goes into -- under Vulkan too, which MoltenVK draws through
    # it. The backend is picked out of backend.h by __APPLE__.
    target_link_libraries(fenestal_usage INTERFACE Threads::Threads
                          "-framework Cocoa" "-framework QuartzCore")
else()
    # user32 for the window, the message loop and the DPI calls; gdi32
    # because a window class wants a cursor. No pthread: see core/thread.h.
    target_link_libraries(fenestal_usage INTERFACE user32 gdi32)
endif()

# The exported surface, read out of the public headers at configure time:
# every prototype marked FNL_API. One list for the archive's globals below
# and the DLL's exports further down, so the two cannot differ. A header
# edit re-runs the configure, so a new entry point cannot be missed.
set(FNL_API_SYMBOLS)
set(_fnl_interop)
foreach(_g IN LISTS FNL_RENDERERS)
    list(APPEND _fnl_interop fnl_${_g}.h)
endforeach()
foreach(_h fenestal.h ${_fnl_interop})
    set(_hp ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/${_h})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_hp})
    file(READ ${_hp} _text)
    # From the start of a line, where every prototype puts it: unanchored,
    # `#  define FNL_API` matches too and runs on into the next prototype.
    # A newline rather than file(STRINGS), so a prototype may still wrap.
    string(REGEX MATCHALL "\nFNL_API[^;(]*\\(" _protos "${_text}")
    foreach(_proto IN LISTS _protos)
        if(_proto MATCHES "(fnl_[a-z0-9_]+)[ \t\r\n]*\\($")
            list(APPEND FNL_API_SYMBOLS ${CMAKE_MATCH_1})
        endif()
    endforeach()
endforeach()
string(REPLACE ";" "\n" _syms "${FNL_API_SYMBOLS}")
file(GENERATE OUTPUT ${FNL_GEN_DIR}/fenestal.syms CONTENT "${_syms}\n")

# ---- The archive: one object, with the API its only globals ---------------
# Linked into one relocatable object first and then every symbol that is not
# the API made local, so a program linking fenestal::static meets fnl_* and
# nothing else: no app_event, create_swapchain or text_width of ours to
# collide with one of its own, or worse, to be silently interposed by it.
# The shared object needs none of this; its exports are the version script's
# and the visibility preset's.
#
# Where gcc's LTO built the objects, the partial link is the LTO link
# (-flinker-output=nolto-rel): the archive carries code optimised across the
# library's files, as the shared object does, and no IR -- which is also why
# it no longer needs fat objects to be linkable by another compiler. Other
# compilers link the machine code and drop whatever IR rode along.
set(FNL_PARTIAL
    ${CMAKE_CURRENT_BINARY_DIR}/fenestal_partial${CMAKE_C_OUTPUT_EXTENSION})
set(_fnl_r_lto -fno-lto)
if(FNL_IPO_OK AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(_cfg "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,")
    string(APPEND _cfg "$<CONFIG:MinSizeRel>>")
    set(_on "-flto=auto$<SEMICOLON>-flinker-output=nolto-rel")
    set(_fnl_r_lto "$<IF:${_cfg},${_on},-fno-lto>")
endif()
#
# Mach-O has no such pair: ld64.lld makes no relocatable object and there is
# no objcopy for the symbols. So on macOS the library's objects are LLVM
# bitcode (-flto=full, below), and the archive's one object is made from
# that: merged into one module by llvm-link, every symbol but the API made
# internal by opt -- and the code no longer reached dropped -- and compiled
# to machine code, optimised as one unit as gcc's partial link is. The
# Objective-C classes go internal with the rest: the runtime finds them
# through their sections, never by symbol.
if(APPLE)
    set(_mt_opt "$<IF:$<CONFIG:Debug>,-O0,-O3>")
    add_custom_command(
        OUTPUT ${FNL_PARTIAL}
        COMMAND ${FNL_LLVM_LINK} $<TARGET_OBJECTS:fenestal_objects>
                -o ${FNL_PARTIAL}.bc
        COMMAND ${FNL_LLVM_OPT} -passes=internalize,globaldce
                -internalize-public-api-file=${FNL_GEN_DIR}/fenestal.syms
                ${FNL_PARTIAL}.bc -o ${FNL_PARTIAL}.int.bc
        COMMAND ${CMAKE_C_COMPILER} --target=${CMAKE_C_COMPILER_TARGET}
                -isysroot ${CMAKE_OSX_SYSROOT} ${_mt_opt}
                -c ${FNL_PARTIAL}.int.bc -o ${FNL_PARTIAL}
        COMMAND ${CMAKE_COMMAND} -E remove ${FNL_PARTIAL}.bc
                ${FNL_PARTIAL}.int.bc
        DEPENDS fenestal_objects $<TARGET_OBJECTS:fenestal_objects>
                ${FNL_GEN_DIR}/fenestal.syms
        COMMENT "Linking the archive's one object, the API its only globals"
        COMMAND_EXPAND_LISTS VERBATIM)
    target_compile_options(fenestal_objects PRIVATE
                           $<$<COMPILE_LANGUAGE:C,OBJC>:-flto=full>)
else()
add_custom_command(
    OUTPUT ${FNL_PARTIAL}
    COMMAND ${CMAKE_C_COMPILER} -r -nostdlib ${_fnl_r_lto}
            $<TARGET_OBJECTS:fenestal_objects> -o ${FNL_PARTIAL}.r
    COMMAND ${CMAKE_OBJCOPY} -w -R .gnu.lto_* -R .gnu.debuglto_*
            -R .llvm.lto --keep-global-symbols=${FNL_GEN_DIR}/fenestal.syms
            ${FNL_PARTIAL}.r ${FNL_PARTIAL}
    COMMAND ${CMAKE_COMMAND} -E remove ${FNL_PARTIAL}.r
    DEPENDS fenestal_objects $<TARGET_OBJECTS:fenestal_objects>
            ${FNL_GEN_DIR}/fenestal.syms
    COMMENT "Linking the archive's one object, the API its only globals"
    COMMAND_EXPAND_LISTS VERBATIM)
endif()
set_source_files_properties(${FNL_PARTIAL} PROPERTIES
                            EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_library(fenestal_static STATIC ${FNL_PARTIAL})
# On Windows the DLL is linked from the archive's one object, not from the
# library's objects: its exports are a .def list (below), which gcc's LTO
# plugin does not count as a reference, so an LTO link of the slim objects
# drops every API function and leaves each export at address 0, with no
# error. The partial link is machine code with the API global, which the
# .def names like any object's. The archive makes that object first: a
# custom command's output in two independent targets is run twice at once
# by a parallel Makefile build.
if(WIN32)
    add_library(fenestal_shared SHARED ${FNL_PARTIAL})
    add_dependencies(fenestal_shared fenestal_static)
    set_target_properties(fenestal_shared PROPERTIES LINKER_LANGUAGE C)
else()
    add_library(fenestal_shared SHARED $<TARGET_OBJECTS:fenestal_objects>)
endif()
foreach(_t fenestal_static fenestal_shared)
    target_link_libraries(${_t} PUBLIC fenestal_usage)
    set_target_properties(${_t} PROPERTIES OUTPUT_NAME fenestal)
endforeach()
set_target_properties(fenestal_static PROPERTIES LINKER_LANGUAGE C)
# Without FNL_STATIC a consumer of the archive gets dllimport on Windows,
# against symbols in its own image. Carried by the target so nobody has to
# remember it.
target_compile_definitions(fenestal_static INTERFACE FNL_STATIC)
# Before 1.0 a minor release may break the one before it, so the soname
# carries the minor too (libfenestal.so.0.9); from 1.0 the major alone.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(_fnl_soversion 0.${PROJECT_VERSION_MINOR})
else()
    set(_fnl_soversion ${PROJECT_VERSION_MAJOR})
endif()
set_target_properties(fenestal_shared PROPERTIES
                      VERSION ${PROJECT_VERSION} SOVERSION ${_fnl_soversion})
# Every symbol the shared object takes from outside resolved at its own
# link, against the system's libraries alone: a runtime called directly
# rather than through its loader (src/core/dynlib.h) fails here, where the
# archive would leave it to a program that may link that runtime itself and
# never notice. ELF only: a DLL is resolved whole by its nature, and ld64
# has the same rule by default. Not under the sanitizers, whose runtime a
# shared object leaves to the program.
if(NOT WIN32 AND NOT APPLE
   AND NOT CMAKE_C_FLAGS MATCHES "-fsanitize=(address|thread)")
    target_link_options(fenestal_shared PRIVATE "LINKER:--no-undefined")
endif()
# Whitelist the exported surface; fenestal.map says why the visibility preset
# is not enough. GNU-style linkers only. LINK_DEPENDS so editing the map
# relinks.
include(CheckLinkerFlag)
set(_vs "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/fenestal.map")
check_linker_flag(C "${_vs}" _has_version_script)
if(_has_version_script)
    target_link_options(fenestal_shared PRIVATE "${_vs}")
    set_target_properties(fenestal_shared PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fenestal.map)
endif()
# The DLL's exports: the same list as the archive's globals.
if(WIN32)
    set(_def "EXPORTS\n")
    foreach(_sym IN LISTS FNL_API_SYMBOLS)
        string(APPEND _def "    ${_sym}\n")
    endforeach()
    file(GENERATE OUTPUT ${FNL_GEN_DIR}/fenestal.def CONTENT "${_def}")
    target_link_options(fenestal_shared PRIVATE ${FNL_GEN_DIR}/fenestal.def)
    set_property(TARGET fenestal_shared APPEND PROPERTY
                 LINK_DEPENDS ${FNL_GEN_DIR}/fenestal.def)
    # mingw's gcc links a -shared image against libgcc_s_seh-1.dll, which
    # is not Windows' own, for __emutls_get_address: _Thread_local is
    # emulated there. Static, as every program's already is.
    target_link_options(fenestal_shared PRIVATE -static-libgcc)
endif()
# The names a consumer writes: fenestal::fenestal is the shared object and
# fenestal::static the archive. Namespaced because CMake treats a name with
# `::` as a target and nothing else, so a typo fails at configure where a bare
# `fenestal` that matched no target would reach the linker as -l.
add_library(fenestal ALIAS fenestal_shared)
add_library(fenestal::fenestal ALIAS fenestal_shared)
add_library(fenestal::static ALIAS fenestal_static)

# ---- The example ----------------------------------------------------------
# A client of the library: it includes only the public headers, and linking
# it is what proves they stand alone. It compiles its own shaders with its
# own loop and not embed_shader(), because a caller is not expected to reuse
# this file's functions and this is what one would have to write.
set(TESTBED_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/testbed)
file(MAKE_DIRECTORY ${TESTBED_GEN_DIR})
set(TESTBED_SHADER_HDRS)
# One header per shader for every renderer the build has, each a name of its
# own so that all of them can sit in one directory: <shader>.h for Vulkan's
# SPIR-V (`<var>_spv`), .gl.h for OpenGL's GLSL (`<var>_glsl`), .dxil.h for
# Direct3D 12's DXIL (`<var>_dxil`) and .msl.h for Metal's MSL (`<var>_msl`).
foreach(_s scene.vert scene.frag star.vert star.frag)
    # glslang takes --vn verbatim, and `scene.vert_spv` is not an identifier.
    string(REPLACE "." "_" _var ${_s})
    string(REGEX REPLACE "^.*\\." "" _stage ${_s})
    set(_in ${CMAKE_CURRENT_SOURCE_DIR}/testbed/shaders/${_s})
    set(_spv ${TESTBED_GEN_DIR}/${_s}.spv)
    if(vulkan IN_LIST FNL_RENDERERS)
        add_custom_command(
            OUTPUT ${TESTBED_GEN_DIR}/${_s}.h
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V
                    ${FNL_SPIRV_VAL} ${_in}
                    --vn ${_var}_spv -o ${TESTBED_GEN_DIR}/${_s}.h
            DEPENDS ${_in}
            VERBATIM)
        list(APPEND TESTBED_SHADER_HDRS ${TESTBED_GEN_DIR}/${_s}.h)
    endif()
    if(NOT FNL_RENDERERS STREQUAL "vulkan")
        # The SPIR-V every translation is made from.
        add_custom_command(
            OUTPUT ${_spv}
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${_in}
                    -o ${_spv}
            DEPENDS ${_in}
            VERBATIM)
    endif()
    if(gl IN_LIST FNL_RENDERERS)
        # Translated back to GLSL for GL, as the library's own are (DESIGN
        # *One shader source*), and embedded as text, which GL compiles at
        # run time. The library's embed script, since the testbed lives in
        # this tree; a consumer's would be its own few lines.
        set(_gl ${TESTBED_GEN_DIR}/${_s}.gl)
        add_custom_command(
            OUTPUT ${TESTBED_GEN_DIR}/${_s}.gl.h
            BYPRODUCTS ${_gl}
            COMMAND ${FNL_SPIRV_CROSS} ${_spv} --version 450 --no-es
                    --glsl-emit-push-constant-as-ubo --output ${_gl}
            COMMAND ${CMAKE_COMMAND} -DIN=${_gl}
                    -DOUT=${TESTBED_GEN_DIR}/${_s}.gl.h -DVAR=${_var}_glsl
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            DEPENDS ${_spv} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            VERBATIM)
        # And GLSL 4.10 for a 4.1 context (fnl_gl_version()), the depth
        # mapped in the vertex stage, as the library's are: <s>.gl41.h,
        # `<var>_glsl41`.
        add_custom_command(
            OUTPUT ${TESTBED_GEN_DIR}/${_s}.gl41.h
            BYPRODUCTS ${_gl}41
            COMMAND ${FNL_SPIRV_CROSS} ${_spv} ${FNL_GL41_FLAGS}
                    --output ${_gl}41
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -S ${_stage}
                    ${_gl}41
            COMMAND ${CMAKE_COMMAND} -DIN=${_gl}41
                    -DOUT=${TESTBED_GEN_DIR}/${_s}.gl41.h -DVAR=${_var}_glsl41
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            DEPENDS ${_spv} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            VERBATIM)
        list(APPEND TESTBED_SHADER_HDRS ${TESTBED_GEN_DIR}/${_s}.gl.h
                                        ${TESTBED_GEN_DIR}/${_s}.gl41.h)
    endif()
    if(d3d12 IN_LIST FNL_RENDERERS)
        # To HLSL by spirv-cross, flipped in Y as the library's are, and to
        # signed DXIL by dxc.
        set(_hlsl ${TESTBED_GEN_DIR}/${_s}.hlsl)
        if(_s MATCHES "vert$")
            set(_profile vs_6_0)
        else()
            set(_profile ps_6_0)
        endif()
        add_custom_command(
            OUTPUT ${TESTBED_GEN_DIR}/${_s}.dxil.h
            BYPRODUCTS ${_hlsl}
            COMMAND ${FNL_SPIRV_CROSS} ${_spv} --hlsl
                    --shader-model ${FNL_HLSL_MODEL} --flip-vert-y
                    --output ${_hlsl}
            COMMAND ${FNL_DXC} -nologo -T ${_profile} -E main
                    -Vn ${_var}_dxil -Fh ${TESTBED_GEN_DIR}/${_s}.dxil.h
                    ${_hlsl}
            DEPENDS ${_spv} ${FNL_DXC}
            VERBATIM)
        list(APPEND TESTBED_SHADER_HDRS ${TESTBED_GEN_DIR}/${_s}.dxil.h)
    endif()
    if(metal IN_LIST FNL_RENDERERS)
        # To MSL by spirv-cross, on the library's conventions
        # (FNL_MSL_FLAGS) -- Y flipped, so the picture is Vulkan's -- and
        # embedded as text, which the device compiles at run time.
        set(_msl ${TESTBED_GEN_DIR}/${_s}.msl)
        add_custom_command(
            OUTPUT ${TESTBED_GEN_DIR}/${_s}.msl.h
            BYPRODUCTS ${_msl}
            COMMAND ${FNL_SPIRV_CROSS} ${_spv} ${FNL_MSL_FLAGS}
                    --output ${_msl}
            COMMAND ${CMAKE_COMMAND} -DIN=${_msl}
                    -DOUT=${TESTBED_GEN_DIR}/${_s}.msl.h -DVAR=${_var}_msl
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            DEPENDS ${_spv} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            VERBATIM)
        list(APPEND TESTBED_SHADER_HDRS ${TESTBED_GEN_DIR}/${_s}.msl.h)
    endif()
endforeach()

# A scene per renderer the build has, each the one file that names its graphics
# API -- in Objective-C for Metal, which has no C interface -- and
# SCENE_HAS_<API> for each, which is how main.c knows which it may be
# handed (FnlInitDesc::renderers) and dispatches on fnl_renderer().
set(TESTBED_SOURCES testbed/main.c testbed/overlay.c testbed/proc.c
                    testbed/gpumem.c testbed/titlebar_custom.c
                    testbed/bench.c testbed/bench_options.c
                    testbed/bench_panel.c testbed/bench_stats.c
                    testbed/bench_json.c testbed/bench_record.c
                    testbed/bench_load.c)
set(TESTBED_DEFINES)
foreach(_g IN LISTS FNL_RENDERERS)
    string(TOUPPER ${_g} _G)
    if(_g STREQUAL metal)
        list(APPEND TESTBED_SOURCES testbed/scene_metal.m)
    else()
        list(APPEND TESTBED_SOURCES testbed/scene_${_g}.c)
    endif()
    list(APPEND TESTBED_DEFINES SCENE_HAS_${_G})
endforeach()

add_executable(fenestal_testbed ${TESTBED_SOURCES} ${TESTBED_SHADER_HDRS})
target_include_directories(fenestal_testbed PRIVATE ${TESTBED_GEN_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/testbed)
set_target_properties(fenestal_testbed PROPERTIES OUTPUT_NAME fenestal-testbed)
# Windows subsystem, so Windows opens no console beside the window. On the
# example and not the library: which subsystem a consumer's executable is
# built for is the consumer's choice. It takes the streams away -- see
# use_parent_console() in testbed/main.c, and DESIGN *Cross-compiling for
# Windows*.
if(WIN32)
    set_target_properties(fenestal_testbed PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_compile_definitions(fenestal_testbed PRIVATE ${TESTBED_DEFINES})
target_link_libraries(fenestal_testbed PRIVATE fenestal_static)
if(APPLE)
    target_compile_options(fenestal_testbed PRIVATE
                           $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
endif()

# ---- The development instruments -------------------------------------------
# fenestal-dev, one program with a mode each (tools/drive/dev.c):
#
#   pace       tools/drive/pace.c, run by tools/drive/pace.py: a window
#              resized from a thread of its own, its frames counted
#   lifecycle  tools/drive/lifecycle.c: the public API called wrongly on
#              purpose, then opened and closed many times in one process
#              with what each lifetime holds counted
#
# Built with every tree because the measurements that matter are taken
# under the sanitizers and under wine, the trees a hand-written compile line
# gets wrong; nothing in the build runs it. A consumer that takes no interop
# header, so linking it proves the plain-window half of the link line, which
# the testbed cannot, and lifecycle.c's functions named as the library's
# internals prove the archive exports only the API.
add_executable(fenestal_dev tools/drive/dev.c tools/drive/pace.c
               tools/drive/lifecycle.c)
set_target_properties(fenestal_dev PROPERTIES OUTPUT_NAME fenestal-dev)
target_link_libraries(fenestal_dev PRIVATE fenestal_static)
# A console program on Windows, unlike the testbed: its whole output is the
# lines on stdout and stderr, and the GUI subsystem would take them from the
# runner.
target_compile_options(fenestal_dev PRIVATE ${FNL_WARNINGS} ${FNL_PREFIX_MAP})

# ---- The API reference's examples -----------------------------------------
# examples/*.c, each a complete consumer of the installed headers,
# built and linked in every tree and never run by the build: an example in
# the reference (tools/mkapidoc.py) that stops compiling fails here first.
file(GLOB FNL_SNIPPETS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.m)
# scene_<api>.c draws with that API, and is built where the build has that
# renderer; Metal's is scene_metal.m, Objective-C, and a tree without the
# language skips it.
foreach(_api vulkan gl d3d12 metal)
    if(NOT _api IN_LIST FNL_RENDERERS)
        list(REMOVE_ITEM FNL_SNIPPETS
             ${CMAKE_CURRENT_SOURCE_DIR}/examples/scene_${_api}.c
             ${CMAKE_CURRENT_SOURCE_DIR}/examples/scene_${_api}.m)
    endif()
endforeach()
set(FNL_SNIPPET_TARGETS)
foreach(_src ${FNL_SNIPPETS})
    get_filename_component(_name ${_src} NAME_WE)
    add_executable(fnl_snippet_${_name} ${_src})
    set_target_properties(fnl_snippet_${_name} PROPERTIES
                          OUTPUT_NAME snippet-${_name}
                          RUNTIME_OUTPUT_DIRECTORY
                          ${CMAKE_CURRENT_BINARY_DIR}/snippets)
    target_link_libraries(fnl_snippet_${_name} PRIVATE fenestal_static)
    target_compile_options(fnl_snippet_${_name} PRIVATE ${FNL_WARNINGS}
                           ${FNL_PREFIX_MAP})
    if(APPLE)
        target_compile_options(fnl_snippet_${_name} PRIVATE
                               $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
    endif()
    list(APPEND FNL_SNIPPET_TARGETS fnl_snippet_${_name})
endforeach()

# A stub only names what it .incbin's, so the assembler has to be told to
# look again when those files change.
set_source_files_properties(${FNL_FONT_STUB} PROPERTIES
                            OBJECT_DEPENDS "${FNL_FONT_BINS}")
set_source_files_properties(${FNL_LICENCE_STUB} PROPERTIES
    OBJECT_DEPENDS
    "${FONT_LICENCE_DIR}/COPYRIGHT.txt;${FONT_LICENCE_DIR}/LICENCE.txt")

# ---- Optimization flags (applied on top of the build-type -O level) --------
if(FNL_NATIVE_ARCH)
    check_c_compiler_flag("-march=native" _has_march_native)
    if(_has_march_native)
        target_compile_options(fenestal_objects PRIVATE -march=native)
    endif()
elseif(FNL_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    check_c_compiler_flag("-march=${FNL_ARCH}" _has_march_level)
    if(_has_march_level)
        target_compile_options(fenestal_objects PRIVATE -march=${FNL_ARCH})
    else()
        message(STATUS "-march=${FNL_ARCH} unsupported; compiler default")
    endif()
endif()

# LTO for optimised configs only. Per-config properties and not a guard on
# CMAKE_BUILD_TYPE, which is empty under a multi-config generator -- exactly
# where "not Debug" cannot be decided at configure time. Not on macOS, whose
# objects are bitcode in every configuration (*The archive*): CMake's thin
# LTO would contradict the full LTO the merge is made from.
if(FNL_IPO_OK AND NOT APPLE)
    foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(fenestal_objects PROPERTIES
                              INTERPROCEDURAL_OPTIMIZATION_${_cfg} TRUE)
    endforeach()
    # gcc's archive is the output of an LTO link (the partial link above),
    # machine code and no IR, so its objects need no machine code of their
    # own. Another compiler's partial link takes the machine code and drops
    # the IR, so there the objects carry both: fat. A compile option so that
    # it lands after CMake's own -fno-fat-lto-objects and wins.
    check_c_compiler_flag("-ffat-lto-objects" _has_fat_lto)
    if(_has_fat_lto AND NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(fenestal_objects PRIVATE
                               $<$<COMPILE_LANGUAGE:C>:-ffat-lto-objects>)
    endif()
else()
    message(STATUS "Link-time optimisation unavailable: ${FNL_IPO_MSG}")
endif()

# ---- Hardening, for optimised configurations ------------------------------
# -fhardened where the compiler has it (gcc 14 and later, glibc targets): one
# switch for _FORTIFY_SOURCE=3, the stack protector, stack-clash and CET
# protection, zeroed automatic variables, PIE and a read-only GOT. Where it
# does not -- mingw's gcc 13, and clang -- the parts that compiler takes, each
# checked, and each check links, so a part that would need a library on the
# link line (mingw's libssp) is left out rather than added: the link line is
# an invariant.
#
# Optimised configurations only: fortification needs -O, and the Debug
# checker trees fill automatic variables with a pattern instead, so an
# uninitialised read is loud where it is looked for and not a quiet zero.
# The analyser tree turns it off (build.sh) to keep reading the code it has
# always read. Not on the host tools: they run during the build and ship
# nowhere. Not on the demos: they are a consumer's own lines.
option(FNL_HARDEN "Harden optimised builds (-fhardened, or its parts)" ON)
include(CheckCSourceCompiles)
set(FNL_HARDEN_C)
set(FNL_HARDEN_EXE_LINK)
set(FNL_HARDEN_SO_LINK)
if(FNL_HARDEN)
    set(CMAKE_REQUIRED_FLAGS "-O2")
    check_c_compiler_flag("-fhardened" FNL_HAS_FHARDENED)
    if(FNL_HAS_FHARDENED)
        set(FNL_HARDEN_C -fhardened)
        set(FNL_HARDEN_EXE_LINK -fhardened)
        # Ubuntu's gcc passes -D_FORTIFY_SOURCE=3 itself at -O, and
        # -fhardened then says on every file that it left fortification
        # alone. Silenced only when what the toolchain set is already level
        # 3; anything less and the note is left to be read. Nothing else in
        # this tree sets one of -fhardened's parts by hand, which is the
        # only other thing -Whardened reports.
        check_c_source_compiles("
            #if !defined(_FORTIFY_SOURCE) || _FORTIFY_SOURCE < 3
            #error not fortified
            #endif
            int main(void) { return 0; }" FNL_FORTIFY_PREDEFINED)
        if(FNL_FORTIFY_PREDEFINED)
            list(APPEND FNL_HARDEN_C -Wno-hardened)
        endif()
    else()
        # An argument clang takes and then ignores for the target -- the
        # stack-clash probe on Darwin -- is a warning on every file, so the
        # probe fails on it there.
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            set(CMAKE_REQUIRED_FLAGS
                "-O2 -Werror=unused-command-line-argument")
        endif()
        foreach(_f -fstack-protector-strong -fstack-clash-protection
                   -fcf-protection=full -ftrivial-auto-var-init=zero)
            string(MAKE_C_IDENTIFIER "FNL_HAS${_f}" _var)
            check_c_compiler_flag("${_f}" ${_var})
            if(${_var})
                list(APPEND FNL_HARDEN_C ${_f})
            endif()
        endforeach()
        # Undefined first: a toolchain that predefines it would otherwise
        # draw a redefinition warning on every file.
        foreach(_level 3 2)
            string(CONCAT _src "#include <string.h>\n"
                   "int main(int c, char **v) { char b[8]; "
                   "memcpy(b, v[0], (unsigned)c); return b[0]; }")
            set(CMAKE_REQUIRED_FLAGS "-O2 -U_FORTIFY_SOURCE")
            set(CMAKE_REQUIRED_DEFINITIONS -D_FORTIFY_SOURCE=${_level})
            check_c_source_compiles("${_src}" FNL_HAS_FORTIFY${_level})
            unset(CMAKE_REQUIRED_DEFINITIONS)
            if(FNL_HAS_FORTIFY${_level})
                list(APPEND FNL_HARDEN_C -U_FORTIFY_SOURCE
                                         -D_FORTIFY_SOURCE=${_level})
                break()
            endif()
        endforeach()
    endif()
    unset(CMAKE_REQUIRED_FLAGS)
    if(WIN32)
        set(_ld_parts -Wl,--dynamicbase -Wl,--nxcompat
                      -Wl,--high-entropy-va)
    else()
        set(_ld_parts -Wl,-z,relro -Wl,-z,now)
    endif()
    foreach(_f ${_ld_parts})
        string(MAKE_C_IDENTIFIER "FNL_HAS${_f}" _var)
        check_linker_flag(C "${_f}" ${_var})
        if(${_var})
            list(APPEND FNL_HARDEN_SO_LINK ${_f})
            if(NOT FNL_HAS_FHARDENED)
                list(APPEND FNL_HARDEN_EXE_LINK ${_f})
            endif()
        endif()
    endforeach()
    message(STATUS "Hardening (optimised builds): ${FNL_HARDEN_C}")
endif()
# The other side of the zeroing above: in an unoptimised tree -- every
# checker tree but the analyser's -- an automatic variable read before it is
# written holds a repeated 0xFE byte pattern, which is a wild pointer, a
# negative count or a NaN, and so is loud. Not in a consumer's Debug build,
# where it would change behaviour nobody asked about: the top-level build
# only.
check_c_compiler_flag("-ftrivial-auto-var-init=pattern" FNL_HAS_INIT_PATTERN)
set(FNL_INIT_PATTERN)
if(FNL_HAS_INIT_PATTERN AND FNL_IS_TOP_LEVEL)
    set(FNL_INIT_PATTERN -ftrivial-auto-var-init=pattern)
endif()
set(_fnl_debug "$<CONFIG:Debug>")
target_compile_options(fenestal_objects PRIVATE
    "$<${_fnl_debug}:$<$<COMPILE_LANGUAGE:C,OBJC>:${FNL_INIT_PATTERN}>>")
foreach(_t fenestal_testbed fenestal_dev)
    target_compile_options(${_t} PRIVATE
        "$<${_fnl_debug}:${FNL_INIT_PATTERN}>")
endforeach()

set(_fnl_opt "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,")
string(APPEND _fnl_opt "$<CONFIG:MinSizeRel>>")
target_compile_options(fenestal_objects PRIVATE
    "$<${_fnl_opt}:$<$<COMPILE_LANGUAGE:C,OBJC>:${FNL_HARDEN_C}>>")
target_link_options(fenestal_shared PRIVATE
    "$<${_fnl_opt}:${FNL_HARDEN_SO_LINK}>")
foreach(_t fenestal_testbed fenestal_dev)
    target_compile_options(${_t} PRIVATE
        "$<${_fnl_opt}:${FNL_HARDEN_C}>")
    target_link_options(${_t} PRIVATE
        "$<${_fnl_opt}:${FNL_HARDEN_EXE_LINK}>")
endforeach()

# ---- As a dependency, and the example that is one ---------------------------
# No install() and no package config: the supported way in is
# add_subdirectory() and fenestal::static or fenestal::fenestal. It needs
# nothing installed, and it is the only path that works for the cross target,
# whose import library is synthesised at configure.
#
# Inside somebody else's project the two fixtures are built only when asked
# for by name: a consumer's `make` should not spend an LTO link on them.
if(NOT FNL_IS_TOP_LEVEL)
    set_target_properties(fenestal_testbed fenestal_dev
                          ${FNL_SNIPPET_TARGETS}
                          PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Scoped to C and Objective-C on the object library: it also assembles the
# .incbin stubs, and each C-only warning would draw a "not valid for
# assembler" note there.
target_compile_options(fenestal_objects PRIVATE
                       $<$<COMPILE_LANGUAGE:C,OBJC>:${FNL_WARNINGS}>)
target_compile_options(fenestal_testbed PRIVATE
                       ${FNL_WARNINGS} ${FNL_PREFIX_MAP})

# Each demo under demos/ configures on its own and pulls this tree in, as a
# consumer's project does. Added here as well, every folder with a
# CMakeLists.txt, so that every tree builds them through a consumer's own
# lines; last, so that they see the finished targets; and only at the top
# level, because reached from a demo this file must not reach back. Found
# rather than listed: a new demo is a new folder and nothing else.
if(FNL_IS_TOP_LEVEL)
    file(GLOB _fnl_demos CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/demos/*/CMakeLists.txt)
    foreach(_demo ${_fnl_demos})
        get_filename_component(_dir ${_demo} DIRECTORY)
        add_subdirectory(${_dir})
    endforeach()
endif()
