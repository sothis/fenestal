# Compares what this build's headers and library say the ABI is against the
# committed baseline, and says what to do about a difference. Run by the
# build (cmake -P), with ABI, SYMS, BASE_ABI and BASE_SYMS set.
foreach(_pair "ABI;BASE_ABI;layout and enum values" "SYMS;BASE_SYMS;exports")
    list(GET _pair 0 _now)
    list(GET _pair 1 _base)
    list(GET _pair 2 _what)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol
                            ${${_now}} ${${_base}}
                    RESULT_VARIABLE _differ)
    if(_differ)
        message(FATAL_ERROR
            "The public ABI's ${_what} differ from the baseline:\n"
            "  diff ${${_base}} ${${_now}}\n"
            "An intended change is made by copying the second over the first "
            "in the same commit, with a line in CHANGELOG.md. Before 1.0 any "
            "change may be; see fenestal.h *stability*.")
    endif()
endforeach()
