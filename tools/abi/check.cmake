# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Compares what this build's headers and library say the ABI is against the
# committed baseline, and says what to do about a difference. Run by the
# build (cmake -P), with MAJOR (the project's major version), ABI, SYMS,
# BASE_ABI and BASE_SYMS set. A cross tree cannot run the layout dump, so it
# gives SYMS and BASE_SYMS alone and the layout pair is skipped.
#
# Before 1.0 the two files are compared exactly: any change is allowed, made
# by copying the build's output over the baseline in the same commit. From
# 1.0 only an addition is allowed, and the baseline is a floor the build
# must still stand on:
#
#   - every `sizeof` is still there and no smaller, and every `offset` and
#     `enum` line is there with the same value, a *_COUNT enum excepted, which
#     counts and so grows;
#   - a struct that grew puts every field it gained at or after its old
#     size. A field placed in what was tail padding is an ABI change and not
#     an addition: a caller built against the old header passes an
#     indeterminate byte there, and the library would read it as a value.
#     Give the struct an explicit field to end on instead, or a pointer-sized
#     one to append;
#   - FnlEvent is the one struct whose size may not grow: it is the
#     caller's storage, and a new event's payload must fit its union, or be
#     fetched by length and getter as the composition and the clipboard are.
#     Its new union members start inside it by design;
#   - a new struct or constant is allowed, a removed or renamed line is not;
#   - every exported name is still exported.

set(_major "${MAJOR}")
if("${_major}" STREQUAL "")
    message(FATAL_ERROR "check.cmake: MAJOR is not set")
endif()

# Reads an .abi file into variables named <prefix>__<kind>__<name>, and the
# list <prefix>__keys of "<kind>__<name>".
function(fnl_abi_read file prefix)
    file(STRINGS ${file} _lines)
    set(_keys)
    set(_file_major "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^major ([0-9]+)$")
            set(_file_major ${CMAKE_MATCH_1})
        elseif(_l MATCHES "^(sizeof|offset|enum) ([^ ]+) (-?[0-9]+)$")
            set(${prefix}__${CMAKE_MATCH_1}__${CMAKE_MATCH_2}
                ${CMAKE_MATCH_3} PARENT_SCOPE)
            list(APPEND _keys "${CMAKE_MATCH_1}__${CMAKE_MATCH_2}")
        endif()
    endforeach()
    set(${prefix}__keys "${_keys}" PARENT_SCOPE)
    set(${prefix}__major "${_file_major}" PARENT_SCOPE)
endfunction()

function(fnl_abi_additions now_file base_file)
    fnl_abi_read(${now_file} now)
    fnl_abi_read(${base_file} base)
    if(NOT "${base__major}" STREQUAL "${_major}")
        message(FATAL_ERROR
            "${base_file} is the baseline of major '${base__major}', this "
            "build is major ${_major}: create tools/abi/fenestal-${_major}* "
            "from this build's output.")
    endif()
    set(_bad)
    foreach(_k IN LISTS base__keys)
        string(REGEX MATCH "^[a-z]+" _kind "${_k}")
        string(REGEX REPLACE "^[a-z]+__" "" _name "${_k}")
        if(NOT DEFINED now__${_k})
            list(APPEND _bad "removed or renamed: ${_kind} ${_name}")
        elseif(_kind STREQUAL "sizeof")
            if(_name STREQUAL "FnlEvent")
                if(NOT now__${_k} EQUAL base__${_k})
                    list(APPEND _bad "FnlEvent changed size (${base__${_k}} to ${now__${_k}}): it is the caller's storage and may not grow")
                endif()
            elseif(now__${_k} LESS base__${_k})
                list(APPEND _bad "${_name} shrank (${base__${_k}} to ${now__${_k}})")
            endif()
        elseif(_kind STREQUAL "enum" AND _name MATCHES "_COUNT$")
            if(now__${_k} LESS base__${_k})
                list(APPEND _bad "${_name} shrank (${base__${_k}} to ${now__${_k}})")
            endif()
        elseif(NOT now__${_k} EQUAL base__${_k})
            list(APPEND _bad "${_kind} ${_name} changed from ${base__${_k}} to ${now__${_k}}")
        endif()
    endforeach()
    # What the build has that the baseline has not: a new field must sit at
    # or after the old end of a struct that existed.
    foreach(_k IN LISTS now__keys)
        if(DEFINED base__${_k})
            continue()
        endif()
        string(REGEX MATCH "^[a-z]+" _kind "${_k}")
        if(NOT _kind STREQUAL "offset")
            continue()
        endif()
        string(REGEX REPLACE "^offset__" "" _name "${_k}")
        string(REGEX MATCH "^[^.]+" _struct "${_name}")
        if(_struct STREQUAL "FnlEvent")
            continue()
        endif()
        if(DEFINED base__sizeof__${_struct} AND
           now__${_k} LESS base__sizeof__${_struct})
            list(APPEND _bad
                 "${_name} is at ${now__${_k}}, inside ${_struct}'s old size ${base__sizeof__${_struct}}: appended fields start at or after it")
        endif()
    endforeach()
    if(_bad)
        list(JOIN _bad "\n  " _text)
        message(FATAL_ERROR
            "The public ABI is no longer a superset of ${base_file}:\n"
            "  ${_text}\n"
            "From 1.0 the ABI only grows; a change to what is in the baseline "
            "wants FNL_VERSION_MAJOR to go up with it.")
    endif()
endfunction()

function(fnl_syms_additions now_file base_file)
    file(STRINGS ${now_file} _now)
    file(STRINGS ${base_file} _base)
    set(_bad)
    foreach(_s IN LISTS _base)
        if(NOT _s IN_LIST _now)
            list(APPEND _bad "${_s}")
        endif()
    endforeach()
    if(_bad)
        list(JOIN _bad "\n  " _text)
        message(FATAL_ERROR
            "No longer exported, though ${base_file} has them:\n  ${_text}\n"
            "From 1.0 the exports only grow.")
    endif()
endfunction()

foreach(_pair "ABI;BASE_ABI;layout and enum values" "SYMS;BASE_SYMS;exports")
    list(GET _pair 0 _now)
    list(GET _pair 1 _base)
    list(GET _pair 2 _what)
    if("${${_now}}" STREQUAL "")
        continue()
    endif()
    if(_major EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol
                                ${${_now}} ${${_base}}
                        RESULT_VARIABLE _differ)
        if(_differ)
            message(FATAL_ERROR
                "The public ABI's ${_what} differ from the baseline:\n"
                "  diff ${${_base}} ${${_now}}\n"
                "An intended change is made by copying the second over the "
                "first in the same commit, with a line in CHANGELOG.md. "
                "Before 1.0 any change may be; from 1.0 an addition only. "
                "See fenestal.h *stability*.")
        endif()
    elseif(_now STREQUAL "ABI")
        fnl_abi_additions(${${_now}} ${${_base}})
    else()
        fnl_syms_additions(${${_now}} ${${_base}})
    endif()
endforeach()
