# SPDX-License-Identifier: GPL-2.0-or-later

# The only owner of executable memory: JIT mappings, the per-thread write
# window, the region a debugger prepares on an iOS device, and
# instruction-cache publication. Its engine kind lets the JIT engines link it
# and keeps the gadget engines and shared CPU code, which stay data-only, from
# doing so. The App Store runtime defines no such target: it has no JIT and
# links none of this.
if(UML_APP_STORE_RUNTIME)
    return()
endif()

add_library(uml_cpu_executable_memory OBJECT executable_memory.cpp)
uml_declare_architecture_target(uml_cpu_executable_memory cpu native)
# An iOS device reaches JIT memory through the region a debugger prepares
# (UML_IOS_DEBUGGER_JIT); no other product does.
if(UML_IOS_DEBUGGER_JIT)
    target_compile_definitions(uml_cpu_executable_memory PRIVATE
        UML_JIT_MEMORY_REGION_ROUTE=1)
endif()

if(UML_BUILD_UNIT_TESTS)
    # The same source with the region route and without a debugger, so a host
    # test can exercise the region's mapping, its alias and its allocation. It
    # never reaches a product.
    add_library(uml_cpu_executable_memory_region_test OBJECT
        executable_memory.cpp)
    uml_declare_architecture_target(uml_cpu_executable_memory_region_test
        cpu native)
    target_compile_definitions(uml_cpu_executable_memory_region_test PRIVATE
        UML_JIT_MEMORY_REGION_ROUTE=1
        UML_JIT_MEMORY_REGION_TEST=1)
endif()
