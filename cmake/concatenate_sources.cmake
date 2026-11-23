# SPDX-License-Identifier: GPL-2.0-or-later
#
# Joins source files into one translation unit, for a test that must reach a
# file-local function of the source under test:
#
#   cmake -DOUTPUT=<file> "-DINPUTS=<a;b;c>" -P concatenate_sources.cmake
#
# Each part follows a #line directive that names it, so diagnostics point at
# the part and not at the joined file.
if(NOT OUTPUT OR NOT INPUTS)
    message(FATAL_ERROR "OUTPUT and INPUTS are required")
endif()
set(_joined "")
foreach(_input IN LISTS INPUTS)
    file(READ "${_input}" _text)
    string(APPEND _joined "#line 1 \"${_input}\"\n${_text}\n")
endforeach()
file(WRITE "${OUTPUT}" "${_joined}")
