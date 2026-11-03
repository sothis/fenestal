# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Run as a script: cmake -DIN=<file> -DOUT=<header> -DVAR=<name> -P this
#
# A text file as `static const char <VAR>[]` in a C header, one string literal
# per line so the header reads as the source did. For the GLSL that
# spirv-cross writes, which OpenGL compiles at run time from the text and so
# is embedded as text, where SPIR-V is embedded as words.
#
# A CMake script and not a tool of the tree's: it runs during the build, on
# the host, in the cross tree as well, and needs nothing compiled for it.

foreach(_v IN ITEMS IN OUT VAR)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "embed_text.cmake: ${_v} is not set")
    endif()
endforeach()

file(READ "${IN}" _text)
string(REPLACE "\\" "\\\\" _text "${_text}")
string(REPLACE "\"" "\\\"" _text "${_text}")
# -std=c11 reads trigraphs, and ??/ would end the literal early.
string(REPLACE "?" "\\?" _text "${_text}")
string(REPLACE "\n" "\\n\"\n    \"" _text "${_text}")
get_filename_component(_name "${IN}" NAME)
string(REGEX REPLACE "\\.new$" "" _name "${_name}")
file(WRITE "${OUT}"
     "/* ${_name}, as spirv-cross wrote it. Generated; do not edit. */\n"
     "#pragma once\n"
     "static const char ${VAR}[] =\n"
     "    \"${_text}\";\n")
