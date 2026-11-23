# SPDX-License-Identifier: GPL-2.0-or-later

# The host's response to the system's memory pressure, with a guest running
# (run_guest_memory_pressure.py). A real event needs a privileged tool, so the
# runner inserts an injector into the launcher: it makes every memory pressure
# source the process creates one the runner can raise.
add_library(uml_memory_pressure_injector SHARED pressure_injector.cpp)
set_target_properties(uml_memory_pressure_injector PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(uml_memory_pressure_injector PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_memory_pressure_injector PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_memory_pressure_injector)

# A warning and the normal level touch nothing while the trimmer is off, a
# critical level asks for the guest's gradual page-cache trimmer and never trims
# all the cache, a warning or the normal level after it turns the trimmer off at
# once, and the lines say so.
uml_add_guest_integration_test(
    NAME guest-memory-pressure
    SCRIPT run_guest_memory_pressure.py
    ARGS --injector-library $<TARGET_FILE:uml_memory_pressure_injector>
    LABELS guest;memory
    TIMEOUT 600
    CPUS 2)
