# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag

cmake_minimum_required(VERSION 3.18)

# gcc unless told otherwise, and before project() because that is when the
# compiler is detected. CC is checked by hand: it is only consulted during
# project(), so an unguarded default would shadow `CC=clang cmake ...`.
# Windows is left to CMake's own default, which is MSVC there; FreeBSD has
# clang in its base system and gcc only as a port.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "FreeBSD")
        set(CMAKE_C_COMPILER clang)
    elseif(NOT CMAKE_HOST_WIN32)
        set(CMAKE_C_COMPILER gcc)
    endif()
endif()

# ASM is for the generated .incbin stubs that carry the font blob and the
# licence text into .rodata. MSVC has no assembler that takes them and
# embeds the same blobs as generated C arrays instead (tools/bin2c.c), so
# the language is enabled only where an assembler is what makes them.
project(fenestal VERSION 1.0.0 LANGUAGES C)
if(NOT MSVC)
    enable_language(ASM)
endif()
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
# (build.sh's checker trees pass one). A cross Windows tree builds no host
# tool; a native one builds them for Windows and runs them there.
#
#   _WIN32_WINNT / WINVER  Windows 10. SetProcessDpiAwarenessContext and
#                          GetDpiForWindow arrive in 1607 and mingw's headers
#                          hide them behind this.
#   __USE_MINGW_ANSI_STDIO mingw's own printf and not the C runtime's, which
#                          on msvcrt predates C99 and prints %zu as "zu".
#                          mingw's alone: MSVC's UCRT is C99's already, and
#                          the macro means nothing to it. It must precede
#                          <stdio.h>, which a -D does.
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
    if(MINGW)
        add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
    endif()
endif()
# The mingw tree links the Universal CRT, whose printf is C99's, and not
# msvcrt.dll: a compiler configured for it ships mingw-w64's headers for the
# UCRT, whose _mingw.h says so (or names the runtime's version). That is the
# header's to say and not a builtin macro's, so the probe includes it.
# build.sh refuses one that does not; this catches a cmake run that did not
# come through build.sh.
if(MINGW AND NOT MSVC)
    include(CheckCSourceCompiles)
    check_c_source_compiles("
        #include <_mingw.h>
        #if !defined(_UCRT) && \
            !(defined(__MSVCRT_VERSION__) && __MSVCRT_VERSION__ >= 0xE00)
        #error not the Universal CRT
        #endif
        int main(void) { return 0; }" FNL_MINGW_UCRT)
    if(NOT FNL_MINGW_UCRT)
        message(FATAL_ERROR
            "this mingw-w64 compiler targets msvcrt, and the tree links "
            "the Universal CRT: use MSYS2's UCRT64, llvm-mingw's UCRT "
            "release, or FNL_MINGW_PREFIX")
    endif()
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
# MSVC takes /std:c11 from VS 16.7; CMake passes it itself only from 3.21,
# and the tree configures from 3.18. Said here so a 3.18 configure is C11
# too, which every designated initialiser and _Static_assert in it needs.
if(MSVC)
    add_compile_options(/std:c11)
    # The testbed's benchmark and the harness's pace mode keep their state in
    # C11 atomics, which MSVC takes only behind this switch. The library
    # itself uses compiler.h's _Interlocked* shim and needs nothing. clang's
    # MSVC front end has them already and does not know the switch.
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_compile_options(/experimental:c11atomics)
    endif()
endif()

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
# renderer the graphics API (Vulkan, Direct3D 12, Metal), and a
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

# Linux and FreeBSD have three backends -- two window servers and none, the
# bare display (kms) -- and a build has all three by default (linux, or
# freebsd on that system), the one to open chosen from the session's
# environment. A single name builds that one alone, for a consumer that
# wants the smaller library and the shorter link line. Windows and macOS
# have one each: the cache is forced to that one, so a -DFNL_BACKEND=x11
# there builds the system's own and every reader of the cache agrees with
# what was built.
#
# FNL_BACKENDS is the list the rest of this file reads. Its order is not
# the order they are tried in, which is backend.h's (FNL_BACKENDS(X)).
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_fnl_backend_default freebsd)
    set(_fnl_backend_says "freebsd, wayland, x11 or kms")
else()
    set(_fnl_backend_default linux)
    set(_fnl_backend_says "linux, wayland, x11 or kms")
endif()
set(FNL_BACKEND "${_fnl_backend_default}" CACHE STRING
    "Backend: ${_fnl_backend_says}, win32 or cocoa")
set_property(CACHE FNL_BACKEND PROPERTY STRINGS linux freebsd wayland x11 kms
             win32 cocoa)
if(WIN32)
    # Forced in the cache as well as here: a normal variable alone would
    # leave the cache saying 'linux' (the default every target starts with)
    # while the build made win32, and a consumer reading the cache would see
    # a backend this tree does not have.
    set(FNL_BACKEND win32 CACHE STRING "Backend: win32" FORCE)
elseif(APPLE)
    set(FNL_BACKEND cocoa CACHE STRING "Backend: cocoa" FORCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    if(NOT FNL_BACKEND MATCHES "^(freebsd|wayland|x11|kms)$")
        message(FATAL_ERROR
            "FNL_BACKEND is '${FNL_BACKEND}'; this target takes "
            "freebsd, wayland, x11 or kms")
    endif()
elseif(NOT FNL_BACKEND MATCHES "^(linux|wayland|x11|kms)$")
    message(FATAL_ERROR
        "FNL_BACKEND is '${FNL_BACKEND}'; this target takes linux, "
        "wayland, x11 or kms")
endif()
if(FNL_BACKEND STREQUAL linux OR FNL_BACKEND STREQUAL freebsd)
    set(FNL_BACKENDS wayland x11 kms)
else()
    set(FNL_BACKENDS ${FNL_BACKEND})
endif()
message(STATUS "Fenestal backends: ${FNL_BACKENDS}")

# The renderers a system has, in the order fnl_init() tries them (the
# code's order is renderer.h's, FNL_RENDERERS(X)): Windows Direct3D 12 and
# Vulkan; macOS Metal and Vulkan on MoltenVK; Linux Vulkan.
# FNL_RENDERER names one of them, or `all` for every one, the default. A
# cache variable all the same: it is what a consumer names to ask for a
# renderer, and asking for one this system lacks is refused here and not
# at the link.
#
# Windows draws with Direct3D 12, on WARP where there is no driver, and
# with Vulkan.
if(FNL_BACKEND STREQUAL win32)
    set(_fnl_system_renderers d3d12 vulkan)
elseif(FNL_BACKEND STREQUAL cocoa)
    set(_fnl_system_renderers metal vulkan)
else()
    set(_fnl_system_renderers vulkan)
endif()
set(FNL_RENDERER all CACHE STRING
    "Renderer: all (every one this system has), vulkan, d3d12 or metal")
set_property(CACHE FNL_RENDERER PROPERTY STRINGS all vulkan d3d12 metal)
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
    if(MINGW AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        # lld's MinGW driver has no -r, so the archive's partial link is
        # binutils' ld, which cannot run LLVM's LTO; and clang's MinGW
        # target takes no -ffat-lto-objects, so a slim LTO object would
        # leave that link no machine code. This target has no LTO.
        set(_ipo_ok FALSE)
        set(_ipo_msg "clang's MinGW target has no LTO (binutils' ld)")
    else()
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    endif()
    set(FNL_IPO_OK "${_ipo_ok}" CACHE INTERNAL "Whether LTO is available")
    set(FNL_IPO_MSG "${_ipo_msg}" CACHE INTERNAL "Why LTO is unavailable")
    set(FNL_IPO_KEY "${_ipo_key}" CACHE INTERNAL "Compiler FNL_IPO_OK is for")
endif()

include(CheckCCompilerFlag)

# __FILE__ reaches the binary through the VKR macro, and with it the directory
# this tree was built in. The macro only: debug info keeps its real paths, so
# a debugger still finds the sources without being told where they are. MSVC
# ignores an unknown option with a warning, which a flag probe counts as
# success, so its probes are not asked.
# Probed only where a probe means something: a normal variable set before a
# check_* call shadows what the probe caches, so the feature would read FALSE
# whatever the compiler took.
if(NOT MSVC)
    check_c_compiler_flag("-fmacro-prefix-map=/a=/b" FNL_HAS_MACRO_PREFIX_MAP)
else()
    set(FNL_HAS_MACRO_PREFIX_MAP FALSE)
endif()
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
# FNL_HOST_INCLUDE is where the host's headers are looked for: /usr/include
# on a Linux build machine.
# A Mac has none there, so a Mac building the macOS tree names a copy of a
# Linux machine's (.github/workflows/ci.yml does).
set(FNL_HOST_INCLUDE /usr/include CACHE PATH
    "The build machine's headers, for a cross tree's Vulkan ones")
if(NOT vulkan IN_LIST FNL_RENDERERS)
    # Neither the loader nor its headers: Direct3D 12's runtime is d3d12 and
    # dxgi, which mingw-w64 has, and Metal's is the SDK's framework.
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
        # execute_process gets none of what CMake adds to a link: the target,
        # the sysroot and the linker a cross toolchain file chose are said
        # here, or this machine's linker would make a stub for this machine.
        set(_stub_x)
        if(CMAKE_C_COMPILER_TARGET)
            list(APPEND _stub_x --target=${CMAKE_C_COMPILER_TARGET})
        endif()
        if(CMAKE_SYSROOT)
            list(APPEND _stub_x --sysroot=${CMAKE_SYSROOT})
        endif()
        separate_arguments(_stub_ld UNIX_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} ${_stub_x} ${_stub_ld}
                    -shared -fPIC -nostdlib
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
        # directory), where build.sh puts it, or in an app bundle's
        # Frameworks, where an application's dylibs go.
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
targets:         [ x86_64-macos, arm64-macos ]
install-name:    '@rpath/libMoltenVK.dylib'
current-version: 1
compatibility-version: 1
exports:
  - targets:     [ x86_64-macos, arm64-macos ]
    symbols:     [ ${_syms} ]
...
")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_tbd}.in ${_tbd})
        message(STATUS "Synthesised libMoltenVK.tbd from ${_n} entry points")
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
        set(_rpaths "-Wl,-rpath,@executable_path"
                    "-Wl,-rpath,@executable_path/../Frameworks")
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION ${_tbd}
            INTERFACE_INCLUDE_DIRECTORIES ${_vkstage}
            INTERFACE_LINK_OPTIONS "${_rpaths}")
    endif()
else()
    # The loader is opened at run time and never linked (src/core/vk_load.c),
    # so the headers are all a tree needs. A native find that has them and no
    # library -- MSYS2's vulkan-headers with no .lib -- still builds; the
    # header-only target is otherwise the same seam.
    find_package(Vulkan QUIET)
    if(NOT TARGET Vulkan::Vulkan)
        find_path(FNL_VULKAN_INCLUDE_DIR vulkan/vulkan.h
                  HINTS ${Vulkan_INCLUDE_DIR} REQUIRED)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${FNL_VULKAN_INCLUDE_DIR})
        message(STATUS "Vulkan headers only: ${FNL_VULKAN_INCLUDE_DIR}")
    endif()
endif()
if(NOT WIN32)
    # The render thread. Windows has CreateThread; see src/core/thread.h.
    fnl_find_global(Threads Threads::Threads)
endif()
# pkg-config for the Linux and macOS trees' headers, and for a native
# mingw tree's FreeType. MSVC finds FreeType with CMake's own module and
# needs none of the others.
if(NOT MSVC)
    find_package(PkgConfig REQUIRED)
endif()
# For their headers: each runtime is opened at run time (src/core/dynlib.h)
# and none is linked, so that a machine without one still loads the
# program and steps past what needed it.
if(wayland IN_LIST FNL_BACKENDS)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET GLOBAL
                      wayland-client)
endif()
if(x11 IN_LIST FNL_BACKENDS)
    # Core xcb and nothing beside it: the backend hand-encodes the extension
    # requests it wants, because each xcb-* library would be another entry on
    # a consumer's link line. src/backend/x11/x11_ext.c has the encoding.
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET GLOBAL xcb)
endif()
# FreeBSD's libdrm installs its headers under <prefix>/include/libdrm,
# named drm.h and not drm/drm.h as the sources write them. A forwarding
# directory gives them the Linux spelling; Linux itself needs none of it,
# its /usr/include/drm being the kernel's own UAPI.
if(kms IN_LIST FNL_BACKENDS AND CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET GLOBAL libdrm)
    set(_drmff ${CMAKE_CURRENT_BINARY_DIR}/generated/drm)
    file(MAKE_DIRECTORY ${_drmff})
    foreach(_h drm.h drm_mode.h drm_fourcc.h)
        file(WRITE ${_drmff}/${_h}.in "#include <${_h}>\n")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_drmff}/${_h}.in ${_drmff}/${_h})
    endforeach()
    set(FNL_DRM_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
endif()
# FreeType is a *build* dependency: linked into tools/mkfont, never into the
# library, and not wanted at all by a tree that borrows its bake.
if(NOT FNL_FONT_FROM)
    if(MSVC)
        find_package(Freetype REQUIRED)
    else()
        pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    endif()
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
    wayland_protocol(pointer-warp-v1)
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
    # A cross tree's pkg-config reports the target's prefix, which is the
    # sysroot's here and nobody else's.
    if(_wl_datadir AND CMAKE_SYSROOT AND
       NOT EXISTS ${_wl_datadir}/wayland.xml AND
       EXISTS ${CMAKE_SYSROOT}${_wl_datadir}/wayland.xml)
        set(_wl_datadir ${CMAKE_SYSROOT}${_wl_datadir})
    endif()
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
# One source per shader: the GLSL above, for Vulkan. Direct3D 12 and Metal
# get spirv-cross's translation of its SPIR-V, made here and checked as it
# is made -- HLSL by glslang's HLSL front end. MSL has no compiler off a Mac,
# and is checked by being made: spirv-cross refuses what Metal cannot
# express. A Vulkan tree has no use for either and translates where
# spirv-cross is installed, as a check that a shader edit has not left the
# other two APIs behind. DESIGN *One shader source*.
#
# The conventions the renderers rely on: the push-constant block is named
# `Push` and becomes a `cbuffer Push` (HLSL) or `constant Push&
# [[buffer(0)]]` (MSL); a vertex input at
# location N is TEXCOORDN in HLSL and [[attribute(N)]] in MSL; a binding is
# its own number in MSL; and HLSL and MSL flip Y at the end of each vertex
# stage, since Direct3D's and Metal's clip space points up where Vulkan's
# points down.
find_program(FNL_SPIRV_CROSS spirv-cross)
if(FNL_SPIRV_CROSS)
    set(FNL_SHADER_XLATE ON)
    file(MAKE_DIRECTORY ${FNL_GEN_DIR}/spv ${FNL_GEN_DIR}/hlsl
                        ${FNL_GEN_DIR}/msl)
elseif(NOT FNL_RENDERERS STREQUAL "vulkan")
    message(FATAL_ERROR "every renderer but Vulkan draws with spirv-cross's "
                        "translation of the shaders, and spirv-cross is not "
                        "installed")
else()
    set(FNL_SHADER_XLATE OFF)
    message(STATUS "spirv-cross not found; shaders are not translated for "
                   "Direct3D and Metal")
endif()
set(FNL_HLSL_MODEL 60)
# MSL 2.1, which every Mac that has macOS 13 takes; compiled by the device at
# run time, since only Xcode has Metal's offline compiler. The flags are the
# conventions above, and a consumer translating its own shaders for Metal
# passes the same: in the cache, for the reason FNL_EMBED_TEXT is below.
set(FNL_MSL_FLAGS --msl --msl-version 20100 --flip-vert-y
    --msl-decoration-binding CACHE INTERNAL
    "spirv-cross's flags for a shader the Metal renderer's conventions take")
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
# translates its own shaders the same way. In the cache because a variable
# set here is not seen by the directory that added this one.
set(FNL_EMBED_TEXT ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
    CACHE INTERNAL "Embeds a text file as a C string: cmake -P")

# embed_shader(<name> <var> [<out> <define>])
# shaders/<name> as `const uint32_t <var>[]` in generated/<name>.h. The two
# optional arguments compile it again under -D<define>, into <out>.h.
#
# Translated as well where spirv-cross is: generated/hlsl/<stem>.<stage>, and
# generated/msl/<stem>.<stage> with its `static const char
# <stem>_<stage>_msl[]` in a header beside it. The stem is <out> without the
# stage, its dots made underscores -- text.frag.lcd is text_lcd.frag.
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
        set(hlsl ${FNL_GEN_DIR}/hlsl/${stem}.${stage})
        set(msl  ${FNL_GEN_DIR}/msl/${stem}.${stage})
        # Made under .new names and renamed into place only once every
        # check has passed: a make that stopped at a check would otherwise
        # leave the outputs newer than the shader, and the next one would
        # not run the check again. The .new names are not BYPRODUCTS: they
        # do not survive the rule, and Ninja takes a declared output that
        # is missing for a dirty one -- every build would rerun every
        # translation. What does survive is listed.
        add_custom_command(
            OUTPUT ${hlsl} ${msl}.h
            BYPRODUCTS ${spv} ${hlsl}.spv ${msl}
            COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V ${define}
                    ${src} -o ${spv}
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
            COMMAND ${CMAKE_COMMAND} -E rename ${hlsl}.new ${hlsl}
            COMMAND ${CMAKE_COMMAND} -E rename ${msl}.new ${msl}
            COMMAND ${CMAKE_COMMAND} -E rename ${msl}.h.new ${msl}.h
            DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
            COMMENT "Translating ${out} for Direct3D and Metal"
            VERBATIM)
        list(APPEND FNL_GEN_FILES ${hlsl} ${msl}.h)
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

# ---- Warnings -------------------------------------------------------------
# One set, for the library, the examples and the host tools alike. Absent on
# purpose: -Wcast-qual (fires only inside the generated Wayland headers) and
# -Wswitch-enum (the VkResult and present-mode switches have a default:).
# MSVC's set is its own: its C compiler takes none of the GNU spellings, and
# /W3 with /utf-8 is what the testbed and the examples are read with. The
# tree is warning-clean under gcc and clang; MSVC is not held to /WX by
# default, its warning set not having been gone over yet.
if(MSVC)
    set(FNL_WARNINGS /W3 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(FNL_WARNINGS -Wall -Wextra
                    -Wshadow -Wpointer-arith -Wstrict-prototypes
                    -Wmissing-prototypes -Wold-style-definition
                    -Wredundant-decls -Wwrite-strings -Wundef -Wvla
                    -Wdouble-promotion -Wformat=2
                    -Wmissing-field-initializers -Wsign-compare)
    # Warning-clean is a rule under clang as under gcc. -Wthread-safety reads
    # the FNL_GUARDED_BY/FNL_REQUIRES annotations (core/compiler.h), which
    # are no-ops elsewhere; gcc has no such analysis, so there it is simply
    # not asked for.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        list(APPEND FNL_WARNINGS -Wthread-safety -Wconditional-uninitialized
                                 -Wcomma -Wunreachable-code -Wloop-analysis
                                 -Wimplicit-fallthrough -Wassign-enum)
    endif()
    # gcc's, beyond -Wall -Wextra, each clean on this tree when it was added.
    # Tried and left out, for a reason that is the tree's and not the
    # warning's: -Wjump-misses-init (204 hits, every one a `goto fail` past a
    # declaration, which is the unwind style here), -Wformat-truncation=2 (it
    # also warns on calls whose length is checked, which level 1 in -Wall
    # does not), -Wlogical-op (EAGAIN || EWOULDBLOCK, which POSIX lets
    # differ) and -Wcast-align=strict (casts into the mapped overlay arena,
    # aligned by construction and pinned by an assert in app.h).
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        list(APPEND FNL_WARNINGS -Wnull-dereference -Wduplicated-cond
                                 -Wduplicated-branches -Wformat-overflow=2
                                 -Wstringop-overflow=4 -Walloca)
    endif()
endif()
# The clang trees build with this ON (build.sh), which is what makes the rule
# a check: a warning there fails the build. Off by default, because a
# consumer's newer compiler must not fail on a warning nobody here has seen.
option(FNL_WERROR "Treat warnings as errors" OFF)
if(FNL_WERROR)
    if(MSVC)
        list(APPEND FNL_WARNINGS /WX)
    else()
        list(APPEND FNL_WARNINGS -Werror)
    endif()
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
if(NOT MSVC)
    check_c_compiler_flag("-fno-sanitize=all" FNL_HAS_NO_SANITIZE)
else()
    set(FNL_HAS_NO_SANITIZE FALSE)
endif()
function(fnl_host_tool name)
    set(_srcs ${ARGN})
    list(FIND _srcs SANITIZED _sanitized)
    list(REMOVE_ITEM _srcs SANITIZED)
    add_executable(${name} ${_srcs})
    # C only: fuzz_text assembles the font stub, where a C warning draws a
    # "not valid for assembler" note (fenestal_objects' reason).
    if(MSVC)
        # No /O2 in Debug: cl refuses it beside the /RTC1 CMake's MSVC
        # Debug flags carry, and the gc tree's host tools are meant to be
        # unoptimised anyway.
        target_compile_options(${name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/O2>
            $<$<COMPILE_LANGUAGE:C>:${FNL_WARNINGS}>)
    else()
        target_compile_options(${name} PRIVATE -O2
                               $<$<COMPILE_LANGUAGE:C>:${FNL_WARNINGS}>)
    endif()
    if(FNL_HAS_NO_SANITIZE AND _sanitized EQUAL -1)
        target_compile_options(${name} PRIVATE -fno-sanitize=all)
        target_link_options(${name} PRIVATE -fno-sanitize=all)
    endif()
endfunction()

# cl has no assembler that takes the .incbin stubs, and a macOS archive is
# made from the library's objects as bitcode, which an assembler object is
# not: both take C arrays by tools/bin2c.c, made at build time so that a
# borrowed bake's re-bake over there is still what rebuilds here. Built on
# the host, which is what runs it. The grammar is
#   bin2c <out.c> <symbol> <file> <nul|-> ...
# cl has no assembler that takes the .incbin stubs, and a macOS archive is
# made from the library's objects as bitcode, which an assembler object is
# not: both take C arrays instead, by one of two routes. tools/bin2c.c
# writes them, and it runs on the machine that compiles it -- which a cross
# Windows tree cannot do, its bin2c being a PE. That tree takes clang's
# #embed, as the Mac's does.
set(FNL_EMBED_C FALSE)
if(APPLE OR (MSVC AND CMAKE_CROSSCOMPILING))
    set(FNL_EMBED_C TRUE)
endif()
if((MSVC AND NOT CMAKE_CROSSCOMPILING) OR APPLE)
    fnl_host_tool(bin2c tools/bin2c.c)
    function(fnl_bin2c out)
        set(_args)
        set(_deps)
        while(ARGN)
            list(POP_FRONT ARGN _sym _file _nul)
            list(APPEND _args ${_sym} ${_file} ${_nul})
            list(APPEND _deps ${_file})
        endwhile()
        add_custom_command(
            OUTPUT ${out}
            COMMAND bin2c ${out} ${_args}
            DEPENDS bin2c ${_deps} VERBATIM)
    endfunction()
endif()

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
    if(FNL_EMBED_C)
        set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_borrowed.c)
        fnl_embed_c(${FNL_FONT_STUB}
                    font_atlas_blob ${FNL_FONT_FROM}/font_atlas.bin ""
                    font_cells_blob ${FNL_FONT_FROM}/font_cells.bin "")
    elseif(MSVC)
        set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_borrowed.c)
        fnl_bin2c(${FNL_FONT_STUB}
                  font_atlas_blob ${FNL_FONT_FROM}/font_atlas.bin -
                  font_cells_blob ${FNL_FONT_FROM}/font_cells.bin -)
    endif()
    # Staged and installed with copy_if_different, as every file written at
    # configure is: file(WRITE) alone bumps the mtime and a re-configure would
    # then reassemble the stub and relink everything. Not where the stub is
    # C: that one is a build rule of its own.
    if(NOT FNL_EMBED_C AND NOT MSVC)
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
    # The stack note mkfont writes into its stub, which a Windows assembler
    # refuses: the same host test that empties FNL_GNU_STACK above.
    if(WIN32)
        target_compile_definitions(mkfont PRIVATE FNL_NO_GNU_STACK)
    endif()
    if(MSVC)
        target_link_libraries(mkfont PRIVATE Freetype::Freetype)
    else()
        target_link_libraries(mkfont PRIVATE PkgConfig::FREETYPE m)
    endif()

    # Order matters: mkfont's FACE[] indexes these, and face 0 -- the
    # monospaced one -- is what the code point index and the metrics come off.
    set(FONT_TTF
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/ubuntu/UbuntuSansMono-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/ubuntu/UbuntuSans-Regular.ttf)
    set(FNL_FONT_BINS ${FNL_GEN_DIR}/font_atlas.bin
                      ${FNL_GEN_DIR}/font_cells.bin)
    # cl has no assembler for the .incbin stub, and a macOS archive is made
    # from the library's objects as bitcode, which an assembler object is
    # not: both take the arrays bin2c makes from the blobs.
    if(MSVC OR APPLE)
        set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_atlas.c)
    else()
        set(FNL_FONT_STUB ${FNL_GEN_DIR}/font_atlas.S)
    endif()
    # mkfont writes each under a .new name and renames them all at the end,
    # as the shader rule does and for its reason. Those names are not
    # BYPRODUCTS: missing is normal for them, and Ninja would rerun the bake
    # on every build (the shader rule says it at length).
    if(MSVC OR APPLE)
        # mkfont still writes its assembler stub, which neither an MSVC
        # build nor a macOS archive can take: the arrays are made from the
        # blobs in the same rule. The stub itself survives and is listed.
        add_custom_command(
            OUTPUT ${FNL_FONT_HDRS} ${FNL_FONT_STUB} ${FNL_FONT_BINS}
            BYPRODUCTS ${FNL_GEN_DIR}/font_atlas.S
            COMMAND mkfont ${FONT_TTF} ${FNL_GEN_DIR}
            COMMAND bin2c ${FNL_FONT_STUB}
                    font_atlas_blob ${FNL_GEN_DIR}/font_atlas.bin -
                    font_cells_blob ${FNL_GEN_DIR}/font_cells.bin -
            DEPENDS mkfont bin2c ${FONT_TTF} VERBATIM)
    else()
        add_custom_command(
            OUTPUT ${FNL_FONT_HDRS} ${FNL_FONT_STUB} ${FNL_FONT_BINS}
            COMMAND mkfont ${FONT_TTF} ${FNL_GEN_DIR}
            DEPENDS mkfont ${FONT_TTF} VERBATIM)
    endif()
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
if(FNL_EMBED_C)
    set(FNL_LICENCE_STUB ${FNL_GEN_DIR}/font_licence.c)
    fnl_embed_c(${FNL_LICENCE_STUB}
                ubuntu_copyright_txt ${FONT_LICENCE_DIR}/COPYRIGHT.txt 1
                ubuntu_licence_txt ${FONT_LICENCE_DIR}/LICENCE.txt 1)
elseif(MSVC)
    set(FNL_LICENCE_STUB ${FNL_GEN_DIR}/font_licence.c)
    fnl_bin2c(${FNL_LICENCE_STUB}
              ubuntu_copyright_txt ${FONT_LICENCE_DIR}/COPYRIGHT.txt nul
              ubuntu_licence_txt ${FONT_LICENCE_DIR}/LICENCE.txt nul)
endif()
if(NOT FNL_EMBED_C AND NOT MSVC)
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
                    ${FNL_GEN_DIR}/font_licence.S.in
                    ${FNL_GEN_DIR}/font_licence.S)
endif()

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
    if(MSVC)
        target_link_libraries(verifyfont PRIVATE Freetype::Freetype)
    else()
        target_link_libraries(verifyfont PRIVATE PkgConfig::FREETYPE m)
    endif()
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
if(MSVC)
    set(_hc_inc /I${CMAKE_CURRENT_SOURCE_DIR}/include)
else()
    set(_hc_inc -I${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
if(vulkan IN_LIST FNL_RENDERERS)
    get_target_property(_vk_inc Vulkan::Vulkan INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_d ${_vk_inc})
        if(MSVC)
            list(APPEND _hc_inc /I${_d})
        else()
            list(APPEND _hc_inc -I${_d})
        endif()
    endforeach()
    if(MSVC)
        list(APPEND _hc_inc /DFNL_CHECK_VULKAN)
    else()
        list(APPEND _hc_inc -DFNL_CHECK_VULKAN)
    endif()
endif()
# <d3d12.h> is the Windows tree's alone, where the renderer is.
if(d3d12 IN_LIST FNL_RENDERERS)
    if(MSVC)
        list(APPEND _hc_inc /DFNL_CHECK_D3D12)
    else()
        list(APPEND _hc_inc -DFNL_CHECK_D3D12)
    endif()
endif()
set(_hc_src ${CMAKE_CURRENT_SOURCE_DIR}/tools/headercheck.c)
# The types of every field and the prototype of every function, as C11
# assertions (tools/abi/types.c): C's _Generic, so the C compile alone.
set(_hc_types ${CMAKE_CURRENT_SOURCE_DIR}/tools/abi/types.c)
if(MSVC)
    # /Zs is syntax-only. MSVC has no C++11 mode; its C++14 is the oldest.
    set(_hc_flags /nologo /Zs /W3)
    # The target and the SDK, for the same reason the clang branch names
    # them: a custom command gets none of them, and a cross clang-cl would
    # check the headers against this machine's.
    set(_hc_on)
    if(CMAKE_C_COMPILER_TARGET)
        list(APPEND _hc_on --target=${CMAKE_C_COMPILER_TARGET})
    endif()
    if(FNL_MSVC_CRT AND FNL_MSVC_SDK)
        # -imsvc and not /I: the SDK's own headers warn about their own
        # casing and pragmas, which is not this tree's to answer for.
        foreach(_d ${FNL_MSVC_CRT}/include ${FNL_MSVC_SDK}/include/ucrt
                ${FNL_MSVC_SDK}/include/shared ${FNL_MSVC_SDK}/include/um)
            list(APPEND _hc_inc -imsvc ${_d})
        endforeach()
    endif()
    set(_hc_cmds COMMAND ${CMAKE_C_COMPILER} ${_hc_on} /std:c11 ${_hc_flags}
                  ${_hc_inc} ${_hc_src}
                 COMMAND ${CMAKE_C_COMPILER} ${_hc_on} /std:c11 ${_hc_flags}
                  ${_hc_inc} ${_hc_types})
    if(CMAKE_CXX_COMPILER)
        foreach(_std 14 17 20)
            list(APPEND _hc_cmds COMMAND ${CMAKE_CXX_COMPILER} ${_hc_on} /TP
                 /std:c++${_std} ${_hc_flags} ${_hc_inc} ${_hc_src})
        endforeach()
    else()
        message(STATUS "No C++ compiler: the headers are checked as C11 only")
    endif()
else()
    set(_hc_flags -fsyntax-only -pedantic-errors -Wall -Wextra -Werror)
    # A custom command gets none of CMake's own target and sysroot flags, and
    # a cross tree's clang is the host's: without these it checks the headers
    # for Linux, against the host's libc. One set for the three languages,
    # which name the same target and the same root.
    set(_hc_c_on)
    set(_hc_cxx_on)
    if(CMAKE_C_COMPILER_TARGET)
        set(_hc_c_on --target=${CMAKE_C_COMPILER_TARGET})
        set(_hc_cxx_on --target=${CMAKE_CXX_COMPILER_TARGET})
    endif()
    if(CMAKE_OSX_SYSROOT)
        list(APPEND _hc_c_on -isysroot ${CMAKE_OSX_SYSROOT})
        list(APPEND _hc_cxx_on -isysroot ${CMAKE_OSX_SYSROOT})
    elseif(CMAKE_SYSROOT)
        list(APPEND _hc_c_on --sysroot=${CMAKE_SYSROOT})
        list(APPEND _hc_cxx_on --sysroot=${CMAKE_SYSROOT})
    endif()
    set(_hc_cmds COMMAND ${CMAKE_C_COMPILER} ${_hc_c_on} -std=c11
                  ${_hc_flags} ${_hc_inc} -x c ${_hc_src}
                 COMMAND ${CMAKE_C_COMPILER} ${_hc_c_on} -std=c11
                  ${_hc_flags} ${_hc_inc} -x c ${_hc_types})
    if(CMAKE_CXX_COMPILER)
        foreach(_std 11 14 17 20)
            list(APPEND _hc_cmds COMMAND ${CMAKE_CXX_COMPILER} ${_hc_cxx_on}
                 -std=c++${_std} ${_hc_flags} ${_hc_inc} -x c++ ${_hc_src})
        endforeach()
    else()
        message(STATUS "No C++ compiler: the headers are checked as C11 only")
    endif()
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
    DEPENDS ${_hc_src} ${_hc_types}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fenestal.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_vulkan.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_metal.h
            ${CMAKE_CURRENT_SOURCE_DIR}/include/fenestal/fnl_d3d12.h
    COMMENT "Compiling the public headers as C11 and C++11 to C++20"
    VERBATIM)
list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/headers_checked.stamp)

# ---- The ABI, against its baseline ----------------------------------------
# tools/abidump prints the public structs' sizes and offsets and every enum
# constant; with the export list (fenestal.syms) it is compared against
# tools/abi/ on every native build, so a header edit that moves what a
# compiled caller depends on fails here. tools/abi/check.cmake says what to
# do about a difference. A cross build cannot run the dump; the layout is the
# same on both 64-bit ABIs the project targets, and the native trees hold it.
# A baseline per renderer set: the interop headers and the exports differ. The
# layout holds fenestal.h's and Metal's whatever the build, Vulkan's where
# the set has it and Direct3D 12's where it has that -- one of four
# (fenestal-<major> with Vulkan's, -metal with Metal's alone, -d3d12 with
# Direct3D 12's and not Vulkan's, -d3d12-vulkan with both);
# the exports are one list per set -- fenestal-<major> for Vulkan alone,
# -d3d12 and -metal for one each, and -<set> for a system's several
# (d3d12-vulkan, metal-vulkan). The portable part is in each, and a change to
# it updates each. The major is the project's: the baselines are renamed, and
# a new set made, when it goes up, and a configure that finds none says so.
# The export list is compared in every tree, cross trees included: it is
# written at configure from the headers, and needs no host tool. The layout
# dump is the native trees' alone, as above.
set(_abi_dir ${CMAKE_CURRENT_SOURCE_DIR}/tools/abi)
# The layout baseline is keyed by what the dump holds: Metal's structs
# are in every one, Vulkan's where the set has it, Direct3D 12's where
# it has that. The exports are one list per set, as below.
set(_abi_base fenestal-${PROJECT_VERSION_MAJOR})
if(d3d12 IN_LIST FNL_RENDERERS)
    if(vulkan IN_LIST FNL_RENDERERS)
        set(_abi_layout ${_abi_base}-d3d12-vulkan)
    else()
        set(_abi_layout ${_abi_base}-d3d12)
    endif()
elseif(vulkan IN_LIST FNL_RENDERERS)
    set(_abi_layout ${_abi_base})
else()
    set(_abi_layout ${_abi_base}-metal)
endif()
if(FNL_RENDERER_COUNT GREATER 1)
    # Named for the set, in the order fnl_init() tries them: Windows'
    # d3d12-vulkan, macOS' metal-vulkan.
    string(JOIN "-" _fnl_abi_set ${FNL_RENDERERS})
    set(_abi_syms ${_abi_base}-${_fnl_abi_set})
elseif(vulkan IN_LIST FNL_RENDERERS)
    set(_abi_syms ${_abi_base})
else()
    set(_abi_syms ${_abi_base}-${FNL_RENDERERS})
endif()
# A missing baseline is a configure error and not a custom command's failure
# on the first build: the file's name says what to make.
foreach(_f ${_abi_syms}.syms ${_abi_layout}.abi)
    if(NOT FNL_GENERATED_DIR OR _f MATCHES "\\.syms$")
        if(NOT EXISTS ${_abi_dir}/${_f})
            message(FATAL_ERROR "No ABI baseline tools/abi/${_f}: this is "
                    "major ${PROJECT_VERSION_MAJOR}, so create it from a "
                    "build's generated/fenestal.abi or fenestal.syms "
                    "(tools/abi/check.cmake)")
        endif()
    endif()
endforeach()
if(NOT FNL_GENERATED_DIR)
    fnl_host_tool(abidump tools/abidump.c)
    target_include_directories(abidump PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(vulkan IN_LIST FNL_RENDERERS)
        target_include_directories(abidump SYSTEM PRIVATE
            $<TARGET_PROPERTY:Vulkan::Vulkan,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(abidump PRIVATE FNL_ABI_VULKAN)
    endif()
    if(d3d12 IN_LIST FNL_RENDERERS)
        target_compile_definitions(abidump PRIVATE FNL_ABI_D3D12)
    endif()
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/abi_checked.stamp
        BYPRODUCTS ${FNL_GEN_DIR}/fenestal.abi
        COMMAND abidump ${FNL_GEN_DIR}/fenestal.abi
        COMMAND ${CMAKE_COMMAND} -DMAJOR=${PROJECT_VERSION_MAJOR}
                -DABI=${FNL_GEN_DIR}/fenestal.abi
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
else()
    add_custom_command(
        OUTPUT ${FNL_GEN_DIR}/syms_checked.stamp
        COMMAND ${CMAKE_COMMAND} -DMAJOR=${PROJECT_VERSION_MAJOR}
                -DSYMS=${FNL_GEN_DIR}/fenestal.syms
                -DBASE_SYMS=${_abi_dir}/${_abi_syms}.syms
                -P ${_abi_dir}/check.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${FNL_GEN_DIR}/syms_checked.stamp
        DEPENDS ${FNL_GEN_DIR}/fenestal.syms ${_abi_dir}/${_abi_syms}.syms
                ${_abi_dir}/check.cmake
        COMMENT "Checking the export list against tools/abi"
        VERBATIM)
    list(APPEND FNL_GEN_FILES ${FNL_GEN_DIR}/syms_checked.stamp)
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
    set(_fuzz_json_src   tools/fuzz/fuzz_json.c src/core/json.c)
    set(_fuzz_keymap_runs 3000)
    set(_fuzz_rules_runs  3000)
    set(_fuzz_font_runs   10000)
    set(_fuzz_text_runs   20000)
    set(_fuzz_json_runs   20000)

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

    foreach(_h keymap rules font text json)
        fnl_host_tool(fuzz_${_h} SANITIZED ${_fuzz_${_h}_src}
                      tools/fuzz/replay.c)
        if(_h STREQUAL json AND NOT MSVC)
            # json_num() asks isfinite(), which FreeBSD's libc has in libm.
            target_link_libraries(fuzz_json PRIVATE m)
        endif()
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
# A caller's picture placed at a rectangle, on every renderer. See
# shaders/image.vert and fnl_overlay_image().
embed_shader(image.vert image_vert_spv)
embed_shader(image.frag image_frag_spv)
# One covering triangle, and the fetch that puts the scene image back on the
# window. See src/renderer/vulkan/frame.c.
embed_shader(fullscreen.vert fullscreen_vert_spv)
embed_shader(blit.frag blit_frag_spv)
# The window's linear image into a PQ swapchain: HDR's last pass on Vulkan,
# and the one place ST 2084 exists. See shaders/encode.frag.
embed_shader(encode.frag encode_frag_spv)
# A post-process over the finished scene. See DESIGN.md's *Antialiasing*.
embed_shader(mlaa.frag mlaa_frag_spv)
# The scene onto the window, resolved against the frames before it, at the
# offset each was rasterised at. Vulkan's alone: no other renderer draws
# FNL_AA_TAAU (FnlRendererVtbl::temporal_upscale says which).
embed_shader(taau.frag taau_frag_spv)

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
                                        src/backend/kms/kms_atomic.c)
        # The NVIDIA/nouveau move is Linux's: sysfs, systemd's socket and
        # the module loader's tree. FreeBSD's KMS vtable names no-op entries.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND FNL_BACKEND_SOURCES src/backend/kms/kms_driver.c)
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        # poll()/pipe for the pump, the Linux header's epoll/eventfd being
        # that system's alone.
        list(APPEND FNL_BACKEND_SOURCES src/backend/loop_bsd.c)
    endif()
endif()

# The bridge is a (backend, renderer) pair and a translation unit of its
# own, so that choosing it is a file name here and not an #ifdef in a file
# that would then need both backends' headers: one for each backend the
# build has, each a table the renderer looks up by the backend that opened
# (VkBridge).
set(_fnl_vk_bridges)
foreach(_p IN LISTS FNL_BACKENDS)
    list(APPEND _fnl_vk_bridges src/renderer/vulkan/surface_${_p}.c)
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
    src/core/decor.c src/core/text_measure.c src/core/text_atlas.c
    src/core/draw.c
    src/core/overlay.c src/core/api_overlay.c src/core/api_state.c
    src/core/events.c src/core/controls.c src/core/echo.c
    src/core/fault.c src/core/drivers.c src/core/rank.c
    src/core/parts.c src/core/metrics.c src/core/json.c
    src/core/mcp.c)

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
# DLL or an executable under 2 GB, which is every one, can. mingw's alone:
# MSVC makes no .refptr slots, and its archive is not localised (below).
if(MINGW)
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
if(FNL_DRM_INCLUDE_DIR)
    # FreeBSD's <drm/drm.h> forwarding headers (the libdrm include itself is
    # added with the backend's below).
    target_include_directories(fenestal_objects PRIVATE
                               ${FNL_DRM_INCLUDE_DIR})
endif()
# The runtimes are each one's headers here, and none is on the link line:
# what is not part of the system is opened at run time (src/core/dynlib.h),
# so a machine missing one still loads the program and steps past the
# backend or the renderer that wanted it. What a consumer links is the system's
# own: libc's dlopen (CMAKE_DL_LIBS: nothing from glibc 2.34, libdl before
# it), m and pthread on Linux; user32, gdi32, ws2_32 and Direct3D 12's DLLs,
# part of every Windows 10, on Windows; the frameworks on macOS.
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
if(NOT WIN32)
    # MSVC has no separate math library; its CRT carries it.
    target_link_libraries(fenestal_usage INTERFACE m)
    target_link_libraries(fenestal_usage INTERFACE ${CMAKE_DL_LIBS})
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
    # against the kernel's UAPI headers on Linux and libdrm's on FreeBSD,
    # and the layout is a subprocess.
    target_link_libraries(fenestal_usage INTERFACE Threads::Threads)
    foreach(_p IN LISTS FNL_BACKENDS)
        string(TOUPPER ${_p} _P)
        target_compile_definitions(fenestal_objects PRIVATE FNL_HAVE_${_P})
    endforeach()
    if(wayland IN_LIST FNL_BACKENDS)
        fnl_headers_of(PkgConfig::WAYLAND_CLIENT)
    endif()
    if(x11 IN_LIST FNL_BACKENDS)
        fnl_headers_of(PkgConfig::XCB)
    endif()
    if(kms IN_LIST FNL_BACKENDS AND FNL_DRM_INCLUDE_DIR)
        fnl_headers_of(PkgConfig::LIBDRM)
    endif()
elseif(FNL_BACKEND STREQUAL cocoa)
    # AppKit for the window and the events, QuartzCore for the CAMetalLayer
    # the frame goes into -- under Vulkan too, which MoltenVK draws through
    # it. The backend is picked out of backend.h by __APPLE__.
    target_link_libraries(fenestal_usage INTERFACE Threads::Threads
                          "-framework Cocoa" "-framework QuartzCore")
else()
    # user32 for the window, the message loop and the DPI calls; gdi32
    # because a window class wants a cursor; ws2_32 for the MCP port's
    # AF_UNIX socket (src/core/mcp.c). No pthread: see core/thread.h.
    target_link_libraries(fenestal_usage INTERFACE user32 gdi32 ws2_32)
    if(FNL_MSVC_CRT AND FNL_MSVC_SDK)
        # The CRT's and the SDK's own, when the tree is cross-compiled by
        # clang-cl (cmake/windows-msvc-x86_64.cmake). A consumer gets them
        # as it gets the SDK's frameworks on macOS: the target's runtime is
        # part of the sysroot, and a -DCMAKE_C_FLAGS would take a flag's
        # place but not this one.
        foreach(_d ${FNL_MSVC_CRT}/include ${FNL_MSVC_SDK}/include/ucrt
                ${FNL_MSVC_SDK}/include/shared ${FNL_MSVC_SDK}/include/um)
            target_include_directories(fenestal_usage SYSTEM INTERFACE ${_d})
        endforeach()
        foreach(_d ${FNL_MSVC_CRT}/lib/x64 ${FNL_MSVC_SDK}/lib/ucrt/x64
                ${FNL_MSVC_SDK}/lib/um/x64)
            target_link_options(fenestal_usage INTERFACE /libpath:${_d})
        endforeach()
    endif()
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
#
# binutils' PE ld, for clang's MinGW tree alone: lld's MinGW driver, which
# llvm-mingw links with, has no -r. build.sh finds it, and the objcopy to
# match, and passes both.
set(FNL_MINGW_LD "" CACHE FILEPATH "binutils' PE ld, for clang's MinGW")
if(MSVC)
    # No relocatable link and no objcopy: the archive is the objects
    # themselves, and the internals a consumer could collide with are
    # global in it. The DLL's surface is still the API alone, through the
    # .def below. The one relaxation the MSVC tree takes; DESIGN says so.
    add_library(fenestal_static STATIC $<TARGET_OBJECTS:fenestal_objects>)
    add_library(fenestal_shared SHARED $<TARGET_OBJECTS:fenestal_objects>)
    set_target_properties(fenestal_static fenestal_shared PROPERTIES
                          LINKER_LANGUAGE C)
else()
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
elseif(MINGW AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # llvm-mingw links with lld's MinGW driver, which has no -r, and
    # llvm-objcopy takes no --keep-global-symbols on COFF: the archive's
    # one object is binutils' ld and objcopy's. Neither knows the CRT, so
    # the msvcrt-flavoured ones a Linux host has will do.
    if(NOT FNL_MINGW_LD)
        message(FATAL_ERROR
            "clang's MinGW link is lld's, which has no relocatable link: "
            "the archive's one object needs binutils' PE ld and objcopy "
            "(-DFNL_MINGW_LD=... -DCMAKE_OBJCOPY=...; build.sh finds them)")
    endif()
    add_custom_command(
        OUTPUT ${FNL_PARTIAL}
        COMMAND ${FNL_MINGW_LD} -r $<TARGET_OBJECTS:fenestal_objects>
                -o ${FNL_PARTIAL}.r
        COMMAND ${CMAKE_OBJCOPY} --wildcard -R .llvm.lto
                --keep-global-symbols=${FNL_GEN_DIR}/fenestal.syms
                ${FNL_PARTIAL}.r ${FNL_PARTIAL}
        COMMAND ${CMAKE_COMMAND} -E remove ${FNL_PARTIAL}.r
        DEPENDS fenestal_objects $<TARGET_OBJECTS:fenestal_objects>
                ${FNL_GEN_DIR}/fenestal.syms
        COMMENT "Linking the archive's one object, the API its only globals"
        COMMAND_EXPAND_LISTS VERBATIM)
else()
add_custom_command(
    OUTPUT ${FNL_PARTIAL}
    COMMAND ${CMAKE_C_COMPILER} -r -nostdlib ${_fnl_r_lto}
            $<TARGET_OBJECTS:fenestal_objects> -o ${FNL_PARTIAL}.r
    COMMAND ${CMAKE_OBJCOPY} --wildcard -R .gnu.lto_*
            -R .gnu.debuglto_* -R .llvm.lto
            --keep-global-symbols=${FNL_GEN_DIR}/fenestal.syms
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
endif()
foreach(_t fenestal_static fenestal_shared)
    target_link_libraries(${_t} PUBLIC fenestal_usage)
    if(MSVC)
        # The static archive and the DLL's import library would both be
        # fenestal.lib, the archive's name from its OUTPUT_NAME and the
        # import library's from the default import prefix; the archive takes
        # a name of its own, libfenestal-static.lib. The DLL is
        # libfenestal.dll, as the mingw tree's is, and the import library
        # fenestal.lib, which is what an MSVC consumer writes.
        if("${_t}" STREQUAL "fenestal_static")
            set_target_properties(${_t} PROPERTIES PREFIX "lib"
                                  OUTPUT_NAME fenestal-static)
        else()
            set_target_properties(${_t} PROPERTIES PREFIX "lib"
                                  OUTPUT_NAME fenestal)
        endif()
    else()
        set_target_properties(${_t} PROPERTIES OUTPUT_NAME fenestal)
    endif()
endforeach()
set_target_properties(fenestal_static PROPERTIES LINKER_LANGUAGE C)
# Without FNL_STATIC a consumer of the archive gets dllimport on Windows,
# against symbols in its own image. Carried by the target so nobody has to
# remember it.
target_compile_definitions(fenestal_static INTERFACE FNL_STATIC)
# Before 1.0 a minor release may break the one before it, so the soname
# carries the minor too (libfenestal.so.0.9); from 1.0 the major alone
# (libfenestal.so.1).
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
if(NOT MSVC)
    # MSVC's linker ignores the flag with a warning CMake's probe does not
    # catch, so it is asked of the GNU-style linkers alone.
    # The committed map names every function of every interop header, which
    # a tree of one renderer does not define, and lld refuses a name it does
    # not find (GNU ld does not). So the tree links against the map cut to
    # its own FNL_API_SYMBOLS: the same nodes and order, a line kept only
    # where this tree has the function. The committed map is still the one
    # tools/check-version-map.py proves against the headers.
    set(_map_src ${CMAKE_CURRENT_SOURCE_DIR}/fenestal.map)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_map_src})
    file(STRINGS ${_map_src} _map_lines)
    set(_map_out)
    foreach(_l IN LISTS _map_lines)
        if(_l MATCHES "^[ \t]*(fnl_[a-z0-9_]+);[ \t]*$")
            if(NOT "${CMAKE_MATCH_1}" IN_LIST FNL_API_SYMBOLS)
                continue()
            endif()
        endif()
        string(APPEND _map_out "${_l}\n")
    endforeach()
    file(WRITE ${FNL_GEN_DIR}/fenestal.map "${_map_out}")
    set(_vs "-Wl,--version-script=${FNL_GEN_DIR}/fenestal.map")
    check_linker_flag(C "${_vs}" _has_version_script)
    if(_has_version_script)
        target_link_options(fenestal_shared PRIVATE "${_vs}")
        set_target_properties(fenestal_shared PROPERTIES
            LINK_DEPENDS ${FNL_GEN_DIR}/fenestal.map)
        # The linker applied it: every fnl_* the object exports carries the
        # node the map puts it in (tools/check-version-map.py). ELF only, and
        # where python3 and readelf are; a tree without them says so once.
        find_program(_fnl_python NAMES python3 python)
        find_program(_fnl_readelf NAMES readelf llvm-readelf)
        if(NOT WIN32 AND NOT APPLE AND _fnl_python AND _fnl_readelf)
            add_custom_command(TARGET fenestal_shared POST_BUILD
                COMMAND ${_fnl_python}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-version-map.py
                        --so $<TARGET_FILE:fenestal_shared>
                COMMENT "Checking the shared object's symbol versions"
                VERBATIM)
        elseif(NOT WIN32 AND NOT APPLE)
            message(STATUS "No python3 or readelf: the symbol versions of the "
                           "shared object go unchecked")
        endif()
    endif()
endif()
# The DLL's exports: the same list as the archive's globals.
if(WIN32)
    set(_def "EXPORTS\n")
    foreach(_sym IN LISTS FNL_API_SYMBOLS)
        string(APPEND _def "    ${_sym}\n")
    endforeach()
    file(GENERATE OUTPUT ${FNL_GEN_DIR}/fenestal.def CONTENT "${_def}")
    # link.exe wants a module-definition file named by /DEF: and refuses it
    # as a bare input (LNK1107); mingw's ld takes it as the input it is.
    if(MSVC)
        target_link_options(fenestal_shared PRIVATE
                            "/DEF:${FNL_GEN_DIR}/fenestal.def")
    else()
        target_link_options(fenestal_shared PRIVATE ${FNL_GEN_DIR}/fenestal.def)
    endif()
    set_property(TARGET fenestal_shared APPEND PROPERTY
                 LINK_DEPENDS ${FNL_GEN_DIR}/fenestal.def)
    # mingw's gcc links a -shared image against libgcc_s_seh-1.dll, which
    # is not Windows' own, for __emutls_get_address: _Thread_local is
    # emulated there. Static, as every program's already is. gcc's flag;
    # llvm-mingw and MSVC link no such runtime.
    if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_options(fenestal_shared PRIVATE -static-libgcc)
    endif()
endif()
# The names a consumer writes: fenestal::fenestal is the shared object and
# fenestal::static the archive. Namespaced because CMake treats a name with
# `::` as a target and nothing else, so a typo fails at configure where a bare
# `fenestal` that matched no target would reach the linker as -l.
add_library(fenestal ALIAS fenestal_shared)
add_library(fenestal::fenestal ALIAS fenestal_shared)
add_library(fenestal::static ALIAS fenestal_static)

# ---- A pkg-config file for a build that is not CMake's ---------------------
# The supported way in is add_subdirectory() above, and nothing is installed.
# A project that builds with its own Makefile still needs the include path and
# the archive's private dependencies written down somewhere, and that is what
# pkg-config is for. This one is written into the build tree beside the library
# and installs nothing, so the rule above holds:
#
#     PKG_CONFIG_PATH=<this dir> pkg-config --cflags --libs fenestal
#     PKG_CONFIG_PATH=<this dir> pkg-config --static --libs fenestal
#
# The two libraries have the same name, so --static is what brings the system
# libraries an archive does not carry. The Unix trees only: a Windows
# consumer links what CMake hands it, and a Mac one wants the frameworks by
# name. CMAKE_DL_LIBS is empty on glibc 2.34 and on FreeBSD, which is why
# the flag is written from it and not spelled -ldl.
if(NOT WIN32 AND NOT APPLE)
    set(_fnl_pc_vk_inc "")
    if(vulkan IN_LIST FNL_RENDERERS AND _vk_usage_inc)
        list(JOIN _vk_usage_inc " -I" _fnl_pc_vk_inc)
        set(_fnl_pc_vk_inc " -I${_fnl_pc_vk_inc}")
    endif()
    set(FNL_PC_LIBS "-lm -lpthread")
    if(CMAKE_DL_LIBS)
        list(JOIN CMAKE_DL_LIBS " -l" _fnl_dl)
        string(APPEND FNL_PC_LIBS " -l${_fnl_dl}")
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fenestal.pc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/fenestal.pc @ONLY)
endif()

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
# SPIR-V (`<var>_spv`), .dxil.h for
# Direct3D 12's DXIL (`<var>_dxil`) and .msl.h for Metal's MSL (`<var>_msl`).
foreach(_s scene.vert scene.frag star.vert star.frag)
    # glslang takes --vn verbatim, and `scene.vert_spv` is not an identifier.
    string(REPLACE "." "_" _var ${_s})
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
                    testbed/titlebar_custom.c
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
    # mingw's crt2.o defines mainCRTStartup beside WinMainCRTStartup; MSVC's
    # GUI subsystem asks for WinMain, and this program has main().
    if(MSVC)
        target_link_options(fenestal_testbed PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()
target_compile_definitions(fenestal_testbed PRIVATE ${TESTBED_DEFINES})
target_link_libraries(fenestal_testbed PRIVATE fenestal_static)
if(APPLE)
    target_compile_options(fenestal_testbed PRIVATE
                           $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
endif()
# The UCRT the tree was configured for is the one the images import:
# msvcrt.dll in an import table is a compiler that slipped through, and
# tools/check-crt.cmake fails the build. MSVC is UCRT by construction.
if(MINGW AND CMAKE_OBJDUMP)
    foreach(_t fenestal_testbed fenestal_shared)
        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP}
                    -DFILE=$<TARGET_FILE:${_t}>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-crt.cmake
            COMMENT "Checking ${_t} imports the Universal CRT, not msvcrt")
    endforeach()
elseif(MINGW)
    # A hand-configured tree without objdump would otherwise carry the proof's
    # absence as silence.
    message(WARNING "no objdump for this mingw tree: the Universal CRT import "
                    "check is not made (set CMAKE_OBJDUMP)")
endif()

# ---- The development instruments -------------------------------------------
# fenestal-dev, one program with a mode each (tools/drive/dev.c):
#
#   pace       tools/drive/pace.c, run by tools/drive/pace.py: a window
#              resized from a thread of its own, its frames counted
#   lifecycle  tools/drive/lifecycle.c: the public API called wrongly on
#              purpose, then opened and closed many times in one process
#              with what each lifetime holds counted
#   open       tools/drive/open.c: what a window costs to its first frame,
#              the cold open and the warm ones
#
# Built with every tree because the measurements that matter are taken
# under the sanitizers and under wine, the trees a hand-written compile line
# gets wrong; nothing in the build runs it. A consumer that takes no interop
# header, so linking it proves the plain-window half of the link line, which
# the testbed cannot, and lifecycle.c's functions named as the library's
# internals prove the archive exports only the API.
add_executable(fenestal_dev tools/drive/dev.c tools/drive/pace.c
               tools/drive/lifecycle.c tools/drive/open.c)
set_target_properties(fenestal_dev PROPERTIES OUTPUT_NAME fenestal-dev)
target_link_libraries(fenestal_dev PRIVATE fenestal_static)
# A console program on Windows, unlike the testbed: its whole output is the
# lines on stdout and stderr, and the GUI subsystem would take them from the
# runner.
target_compile_options(fenestal_dev PRIVATE ${FNL_WARNINGS} ${FNL_PREFIX_MAP})

# ---- The round through the port ------------------------------------------------
# round: a testbed started here, driven through the MCP port (tools/drive/
# round.c). It links nothing of the library and speaks the port's socket, so
# it is the harness that needs no window server, no portal and no consent. Not
# on Windows yet: the socket and the fork are POSIX's, and the Windows round is
# still tools/drive/mcp_round.py's.
if(NOT WIN32)
    set(_drive_tools round pace bench)
    add_executable(round tools/drive/round.c tools/drive/mcp_client.c)
    # pace: one cell of the pacing measurement, read through the port
    # (tools/drive/pace.c) -- where pace.py sweeps the whole matrix.
    add_executable(pace tools/drive/pacers.c tools/drive/mcp_client.c)
    # bench: one environment's benchmark run, its records brought home
    # (tools/drive/benchc.c) -- where bench.py drives every environment.
    add_executable(bench tools/drive/benchc.c tools/drive/mcp_client.c)
    target_compile_options(round PRIVATE ${FNL_WARNINGS} ${FNL_PREFIX_MAP})
    if(FNL_HAS_NO_SANITIZE)
        # They run during a round, not inside the program under test.
        foreach(_t ${_drive_tools})
            target_compile_options(${_t} PRIVATE -fno-sanitize=all)
            target_link_options(${_t} PRIVATE -fno-sanitize=all)
        endforeach()
    endif()
endif()

# ---- The MCP bridge -----------------------------------------------------------
# fenestal-mcp, stdin and stdout to the port's socket (tools/mcpbridge.c): what
# `socat - UNIX-CONNECT:<path>` is, for an MCP client's `command` on a machine
# that has no socat. Links nothing of the library, and Winsock on Windows.
add_executable(fenestal_mcp tools/mcpbridge.c)
set_target_properties(fenestal_mcp PROPERTIES OUTPUT_NAME fenestal-mcp)
if(WIN32)
    # fenestal_usage, which has ws2_32 and, cross-compiled by clang-cl, the
    # CRT's and the SDK's directories.
    target_link_libraries(fenestal_mcp PRIVATE fenestal_usage)
endif()
target_compile_options(fenestal_mcp PRIVATE ${FNL_WARNINGS} ${FNL_PREFIX_MAP})

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
foreach(_api vulkan d3d12 metal)
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
if(MSVC)
    # cl's /arch: names instruction set extensions (AVX2), not the
    # microarchitecture levels -march names, and it has no native form; an
    # unknown option is a warning, which a flag probe counts as success.
    # MSVC's default is the answer.
elseif(FNL_NATIVE_ARCH)
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
# Not under MSVC. Its /GL objects hold intermediate language, not machine code,
# and the static archive is these objects as they are: a consumer's link with
# another cl, clang-cl or lld-link cannot finish them. The DLL would keep LTCG
# only from these same objects, so it goes too -- the cost is the DLL's speed.
if(MSVC)
    message(STATUS "Link-time optimisation off: the static archive is made of "
                   "the objects, and /GL objects link only with the same cl")
elseif(FNL_IPO_OK AND NOT APPLE)
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
if(MSVC)
    # Its own set: control-flow guard at compile and link, and the image
    # flags cl.exe's linker takes. /GS, the stack protector, is on by
    # default.
    set(FNL_HARDEN_C /guard:cf)
    set(FNL_HARDEN_EXE_LINK /guard:cf /DYNAMICBASE /NXCOMPAT
                            /HIGHENTROPYVA)
    set(FNL_HARDEN_SO_LINK ${FNL_HARDEN_EXE_LINK})
    message(STATUS "Hardening (optimised builds): ${FNL_HARDEN_C}")
else()
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
endif()
# The other side of the zeroing above: in an unoptimised tree -- every
# checker tree but the analyser's -- an automatic variable read before it is
# written holds a repeated 0xFE byte pattern, which is a wild pointer, a
# negative count or a NaN, and so is loud. Not in a consumer's Debug build,
# where it would change behaviour nobody asked about: the top-level build
# only.
if(NOT MSVC)
    check_c_compiler_flag("-ftrivial-auto-var-init=pattern"
                          FNL_HAS_INIT_PATTERN)
else()
    set(FNL_HAS_INIT_PATTERN FALSE)
endif()
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
    set_target_properties(fenestal_testbed fenestal_dev fenestal_mcp
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
