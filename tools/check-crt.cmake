# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# A Windows image imports the Universal CRT and not msvcrt.dll. Run by the
# build after the testbed and the DLL are linked, in the mingw tree alone
# (MSVC links the UCRT by construction):
#
#   cmake -DOBJDUMP=<objdump> -DFILE=<image> -P tools/check-crt.cmake
#
# A compiler configured for msvcrt -- Ubuntu's mingw-w64 is -- would build
# and run; the difference is what its printf does with %zu and which
# runtime a reader's machine must have. build.sh refuses such a compiler
# before configuring, and this is the check that the tree under it did not
# slip through.

if(NOT OBJDUMP OR NOT FILE)
    message(FATAL_ERROR "check-crt: OBJDUMP and FILE must both be named")
endif()

execute_process(COMMAND ${OBJDUMP} -p ${FILE}
                OUTPUT_VARIABLE _imports ERROR_VARIABLE _imports
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check-crt: ${OBJDUMP} -p ${FILE} failed: ${_imports}")
endif()

if(_imports MATCHES "DLL Name: msvcrt")
    message(FATAL_ERROR
        "${FILE} imports msvcrt.dll: this mingw-w64 compiler does not target "
        "the Universal CRT, and the tree links ucrtbase")
endif()
if(NOT _imports MATCHES "DLL Name: ucrtbase" AND
   NOT _imports MATCHES "DLL Name: api-ms-win-crt-")
    message(FATAL_ERROR
        "${FILE} imports no Universal CRT DLL: the imports are\n${_imports}")
endif()
