# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_runtime_context_tests context_tests.cpp)
target_link_libraries(uml_runtime_context_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator
    uml_test_allocation_failure)
target_include_directories(uml_runtime_context_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-context-lifecycle
    COMMAND uml_runtime_context_tests
    LABELS tsan)

# The import through which the guest kernel says how its run ends.
add_executable(uml_runtime_guest_shutdown_tests guest_shutdown_tests.cpp)
target_link_libraries(uml_runtime_guest_shutdown_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceGuestEnd
    UML::Emulator)
target_include_directories(uml_runtime_guest_shutdown_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-guest-shutdown
    COMMAND uml_runtime_guest_shutdown_tests
    LABELS tsan)

add_executable(uml_runtime_kernel_signal_scope_tests
    kernel_signal_scope_tests.cpp)
target_link_libraries(uml_runtime_kernel_signal_scope_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-signal-scope
    COMMAND uml_runtime_kernel_signal_scope_tests)

# The panic path's rewrite of the kernel's frames to the carrier bytes.
add_executable(uml_runtime_kernel_crash_frames_tests
    kernel_crash_frames_tests.cpp)
target_link_libraries(uml_runtime_kernel_crash_frames_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-crash-frames
    COMMAND uml_runtime_kernel_crash_frames_tests)

# The record of how a guest session ended and the kernel output it quotes,
# and the network, storage and camera slots' text against the golden files
# AndariumCore parses too.
add_executable(uml_runtime_session_record_tests
    session_record_tests.cpp)
target_link_libraries(uml_runtime_session_record_tests PRIVATE
    UML::RuntimeSessionRecord
    UML::RuntimeGuestStatistics)
uml_add_unit_test(
    NAME runtime-session-record
    COMMAND uml_runtime_session_record_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/andarium/fixtures/network-slot-golden.txt
        ${PROJECT_SOURCE_DIR}/tests/unit/andarium/fixtures/storage-slot-golden.txt
        ${PROJECT_SOURCE_DIR}/tests/unit/andarium/fixtures/camera-slot-golden.txt)

# The same record against a state checkpoint that is delayed past an end: the
# binary defines pwrite to hold one write, so it is a binary of its own.
add_executable(uml_runtime_session_record_delayed_checkpoint_tests
    session_record_delayed_checkpoint_tests.cpp)
target_link_libraries(uml_runtime_session_record_delayed_checkpoint_tests PRIVATE
    UML::RuntimeSessionRecord
    UML::RuntimeGuestStatistics)
uml_add_unit_test(
    NAME runtime-session-record-delayed-checkpoint
    COMMAND uml_runtime_session_record_delayed_checkpoint_tests)

# The console's word that an Android guest finished booting, which the
# emulator reports at a run's end and an application's restart loop reads.
add_executable(uml_runtime_console_boot_signal_tests
    console_boot_signal_tests.cpp)
target_include_directories(uml_runtime_console_boot_signal_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(
    NAME runtime-console-boot-signal
    COMMAND uml_runtime_console_boot_signal_tests
    LABELS tsan)

# The refusal to resume on a disk that changed: it ends the process, and the
# session record says why.
add_executable(uml_runtime_guest_resume_refusal_tests
    guest_resume_refusal_tests.cpp)
target_link_libraries(uml_runtime_guest_resume_refusal_tests PRIVATE
    UML::GuestFiles
    UML::RuntimeGuestStatistics
    UML::RuntimeGuestSync
    UML::RuntimeSessionRecord)
uml_add_unit_test(
    NAME runtime-guest-resume-refusal
    COMMAND uml_runtime_guest_resume_refusal_tests)

# The kernel a shared library links into itself is found through the library
# that holds the lookup, not through the executable that loads it.
add_library(uml_runtime_embedded_kernel_probe SHARED
    embedded_kernel_probe.cpp
    $<TARGET_OBJECTS:uml_runtime_kernel_signals>)
# The directory's link_libraries() would give the library the process-wide
# test host-capability installer, which a second copy in the process aborts.
set_property(TARGET uml_runtime_embedded_kernel_probe PROPERTY
    LINK_LIBRARIES UML::MemorySharedBacking)
target_include_directories(uml_runtime_embedded_kernel_probe PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
target_compile_options(uml_runtime_embedded_kernel_probe PRIVATE
    -Wall -Wextra -Werror)
target_link_options(uml_runtime_embedded_kernel_probe PRIVATE
    "LINKER:-sectcreate,__TEXT,__uml_elf,${CMAKE_CURRENT_SOURCE_DIR}/embedded_kernel_probe.cpp"
    "LINKER:-sectalign,__TEXT,__uml_elf,0x4000")
set_target_properties(uml_runtime_embedded_kernel_probe PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_property(TARGET uml_runtime_embedded_kernel_probe APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embedded_kernel_probe.cpp")
add_executable(uml_runtime_embedded_kernel_image_tests
    embedded_kernel_image_tests.cpp)
target_link_libraries(uml_runtime_embedded_kernel_image_tests PRIVATE
    UML::RuntimeKernelSignals)
add_dependencies(uml_runtime_embedded_kernel_image_tests
    uml_runtime_embedded_kernel_probe)
uml_add_unit_test(
    NAME runtime-embedded-kernel-image
    COMMAND uml_runtime_embedded_kernel_image_tests
        $<TARGET_FILE:uml_runtime_embedded_kernel_probe>)

# The ledger of what an embedded kernel creates through its imports.
add_executable(uml_runtime_kernel_resources_tests kernel_resources_tests.cpp)
target_link_libraries(uml_runtime_kernel_resources_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant)
target_include_directories(uml_runtime_kernel_resources_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(
    NAME runtime-kernel-resources
    COMMAND uml_runtime_kernel_resources_tests)

# The loader's release of the kernel image against a host-service view that
# still borrows its guest-RAM tail.
add_executable(uml_runtime_kernel_image_release_tests
    kernel_image_release_tests.cpp)
target_link_libraries(uml_runtime_kernel_image_release_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-image-release
    COMMAND uml_runtime_kernel_image_release_tests
    LABELS tsan)

add_executable(uml_runtime_sequential_run_tests sequential_run_tests.cpp)
target_link_libraries(uml_runtime_sequential_run_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceSocketPaths
    UML::MemorySharedBacking
    uml_emulator_test_variant
    uml_test_mach_vm_region)
# The JIT's exclusive-monitor probe the restart modes read between runs.
target_sources(uml_runtime_sequential_run_tests PRIVATE
    $<TARGET_OBJECTS:uml_arm64_jit_test_support>)
target_include_directories(uml_runtime_sequential_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/platform
    ${PROJECT_SOURCE_DIR}/darwin/runtime
    ${PROJECT_SOURCE_DIR}/tests/unit/emulator)
uml_add_unit_test(
    NAME runtime-sequential-run-same-handle
    COMMAND uml_runtime_sequential_run_tests same-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-fresh-handle
    COMMAND uml_runtime_sequential_run_tests fresh-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-unclaimed-first-run
    COMMAND uml_runtime_sequential_run_tests unclaimed-first-run)
# A guest that restarts itself boots again in the same process; every other end
# leaves the guest spent, the hand-back refuses what still holds the guest, and
# it waits for a stop or a resume still reaching the hibernation it drops.
# One process each, because the claim on guest RAM outlives every handle.
foreach(_uml_restart_mode IN ITEMS restart power-off halt stopped failed
        another-handle borrowed-view running-thread abort-stop abort-resume
        release-same-handle release-fresh-handle release-halt
        release-borrowed-view release-running-thread)
    uml_add_unit_test(
        NAME runtime-restart-${_uml_restart_mode}
        COMMAND uml_runtime_sequential_run_tests ${_uml_restart_mode})
endforeach()
unset(_uml_restart_mode)
# A start the process refuses, or that waits for the process, publishes no
# setting the whole process keeps (the JIT cache budget, the memory limit).
uml_add_unit_test(
    NAME runtime-refused-start-no-cache-budget
    COMMAND uml_runtime_sequential_run_tests refused-start-no-cache-budget)
uml_add_unit_test(
    NAME runtime-waiting-start-memory-limit
    COMMAND uml_runtime_sequential_run_tests waiting-start-memory-limit)
# Where the build has the ahead-of-time component: the release of a cleanly
# powered-off guest ends its session, and the next one chooses the component
# afresh, whatever the last one did.
if(TARGET uml_arm64_aot_framework)
    target_compile_definitions(uml_runtime_sequential_run_tests PRIVATE
        UML_TEST_AOT=1
        UML_TEST_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>)
    # Where the build has the production loader too, a kernel image it refuses
    # leaves the AOT choice as it was.
    if(UML_BUILD_KERNEL_RUNNER)
        uml_add_unit_test(
            NAME runtime-loader-refusal-keeps-aot-choice
            COMMAND uml_runtime_sequential_run_tests
                loader-refusal-keeps-aot-choice)
    endif()
    uml_add_unit_test(
        NAME runtime-release-reselects-aot
        COMMAND uml_runtime_sequential_run_tests release-reselects-aot)
    # A start decides only once it owns the process, so one that arrives while
    # the session before it is ending waits for the end, and neither undoes the
    # other's choice; a restart is no session boundary.
    foreach(_uml_aot_mode IN ITEMS release-race-off-then-on
            release-race-on-then-off release-race-mismatched
            restart-keeps-aot-on restart-keeps-aot-off
            failed-start-keeps-aot-choice failed-restart-keeps-aot-choice)
        uml_add_unit_test(
            NAME runtime-${_uml_aot_mode}
            COMMAND uml_runtime_sequential_run_tests ${_uml_aot_mode})
    endforeach()
    unset(_uml_aot_mode)
else()
    target_compile_definitions(uml_runtime_sequential_run_tests PRIVATE
        UML_TEST_AOT=0 UML_TEST_KERNEL_RUNNER=0)
endif()

add_executable(uml_runtime_x86_tier_tests x86_tier_tests.cpp)
target_include_directories(uml_runtime_x86_tier_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-x86-tier
    COMMAND uml_runtime_x86_tier_tests
    LABELS tsan)

# The registry of an embedded kernel's threads across a storm of restarts.
add_executable(uml_runtime_kernel_threads_tests kernel_threads_tests.cpp)
target_link_libraries(uml_runtime_kernel_threads_tests PRIVATE
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-kernel-threads
    COMMAND uml_runtime_kernel_threads_tests)

# The fence against real threads. Each test stops and starts threads of the
# process, so the binaries run alone.
add_executable(uml_runtime_thread_fence_tests thread_fence_tests.cpp)
target_link_libraries(uml_runtime_thread_fence_tests PRIVATE
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-thread-fence
    COMMAND uml_runtime_thread_fence_tests
    TIMEOUT 240)
uml_add_unit_test(
    NAME runtime-thread-fence-registry-overflow
    COMMAND uml_runtime_thread_fence_tests overflow
    TIMEOUT 120)
set_tests_properties(runtime-thread-fence
    runtime-thread-fence-registry-overflow PROPERTIES RUN_SERIAL TRUE)

# The session record's checkpoint thread is held by the fence, and keeps its
# cadence.
add_executable(uml_runtime_session_record_fence_tests
    session_record_fence_tests.cpp)
target_link_libraries(uml_runtime_session_record_fence_tests PRIVATE
    UML::RuntimeGuestStatistics
    UML::RuntimeSessionRecord
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-session-record-fence
    COMMAND uml_runtime_session_record_fence_tests
    TIMEOUT 120)
set_tests_properties(runtime-session-record-fence PROPERTIES RUN_SERIAL TRUE)

# The order of a hibernation against recording steps, and a real one against
# real kernel threads and guest RAM.
add_executable(uml_runtime_guest_hibernation_tests guest_hibernation_tests.cpp)
target_link_libraries(uml_runtime_guest_hibernation_tests PRIVATE
    UML::RuntimeHibernation
    UML::RuntimeSessionRecord
    UML::RuntimeThreadFence
    UML::MemorySharedBacking)
uml_add_unit_test(
    NAME runtime-guest-hibernation
    COMMAND uml_runtime_guest_hibernation_tests
    TIMEOUT 240)
set_tests_properties(runtime-guest-hibernation PROPERTIES RUN_SERIAL TRUE)

# The virtual Jetsam: the decision, the line, and the sampler of a run whose
# launch stated the process's memory limit.
add_executable(uml_runtime_virtual_jetsam_tests virtual_jetsam_tests.cpp)
target_link_libraries(uml_runtime_virtual_jetsam_tests PRIVATE
    UML::RuntimeVirtualJetsam
    UML::PlatformContract
    "-framework Foundation")
target_sources(uml_runtime_virtual_jetsam_tests PRIVATE
    $<TARGET_OBJECTS:uml_host_environment>)
target_include_directories(uml_runtime_virtual_jetsam_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME runtime-virtual-jetsam
    COMMAND uml_runtime_virtual_jetsam_tests
    LABELS tsan)

# The memory the guest side of the process holds beside the guest's RAM as the
# vCPU count grows, and the most RAM a process limit leaves a guest.
add_executable(uml_runtime_guest_memory_budget_tests
    guest_memory_budget_tests.cpp)
target_include_directories(uml_runtime_guest_memory_budget_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-guest-memory-budget
    COMMAND uml_runtime_guest_memory_budget_tests)

# What the host reads of a guest without asking it: the CPU time of the vCPU
# carriers, found by name among the process's threads, and the RAM the host
# holds for the guest.
add_executable(uml_runtime_guest_statistics_tests
    guest_statistics_tests.cpp)
target_link_libraries(uml_runtime_guest_statistics_tests PRIVATE
    UML::RuntimeGuestStatistics)
target_include_directories(uml_runtime_guest_statistics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(
    NAME runtime-guest-statistics
    COMMAND uml_runtime_guest_statistics_tests
    LABELS tsan)

# The stall record: which vCPUs are stalled and why from synthetic samples,
# the rings and the slot that never exceed their size, and the slot's text
# against the golden file AndariumCore parses too.
add_executable(uml_runtime_stall_record_tests stall_record_tests.cpp)
target_link_libraries(uml_runtime_stall_record_tests PRIVATE
    UML::RuntimeStallRecord)
uml_add_unit_test(
    NAME runtime-stall-record
    COMMAND uml_runtime_stall_record_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/andarium/fixtures/stall-slot-golden.txt
    LABELS tsan)

# The activity record the stall watch sleeps by: carriers that name themselves
# and enter idle waits, and the queue timer they arm.
add_executable(uml_runtime_carrier_activity_tests carrier_activity_tests.cpp)
target_link_libraries(uml_runtime_carrier_activity_tests PRIVATE
    UML::RuntimeCarrierActivity)
uml_add_unit_test(
    NAME runtime-carrier-activity
    COMMAND uml_runtime_carrier_activity_tests
    TIMEOUT 120)
set_tests_properties(runtime-carrier-activity PROPERTIES RUN_SERIAL TRUE)

# The kernel's blocking imports, marked while they block.
add_executable(uml_runtime_kernel_waits_tests kernel_waits_tests.cpp)
target_link_libraries(uml_runtime_kernel_waits_tests PRIVATE
    UML::RuntimeCarrierActivity
    UML::RuntimeKernelWaits
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-kernel-waits
    COMMAND uml_runtime_kernel_waits_tests)

# The watch thread against carriers that wait in marked calls and the fence
# that holds it. The binary stops and starts threads of the process, so it
# runs alone.
add_executable(uml_runtime_stall_watch_tests stall_watch_tests.cpp)
target_link_libraries(uml_runtime_stall_watch_tests PRIVATE
    UML::RuntimeCarrierActivity
    UML::RuntimeKernelWaits
    UML::RuntimeSessionRecord
    UML::RuntimeStallRecord
    UML::RuntimeStallWatch
    UML::RuntimeThreadFence
    UML::PlatformContract
    "-framework Foundation")
target_sources(uml_runtime_stall_watch_tests PRIVATE
    $<TARGET_OBJECTS:uml_host_thermal>)
uml_add_unit_test(
    NAME runtime-stall-watch
    COMMAND uml_runtime_stall_watch_tests
    TIMEOUT 120)
set_tests_properties(runtime-stall-watch PROPERTIES RUN_SERIAL TRUE)

# What the host gives back when the system warns of memory pressure: the
# policy against a clock and actions of the test's own, and the system's side
# against a source the test raises.
add_executable(uml_runtime_memory_pressure_response_tests
    memory_pressure_response_tests.cpp)
target_link_libraries(uml_runtime_memory_pressure_response_tests PRIVATE
    UML::RuntimeMemoryPressure
    UML::RuntimeGuestStatistics
    UML::PlatformContract)
uml_add_unit_test(
    NAME runtime-memory-pressure-response
    COMMAND uml_runtime_memory_pressure_response_tests
    TIMEOUT 120)

# A level raised after exit() has run the destructors of ordinary statics, with a
# guest still owning its watch. The object that registers the raising handler is
# linked ahead of the response's, which is what puts the handler after their
# destructors.
add_library(uml_runtime_memory_pressure_exit_first OBJECT
    memory_pressure_exit_first.cpp)
add_executable(uml_runtime_memory_pressure_exit_tests
    memory_pressure_exit_tests.cpp)
target_link_libraries(uml_runtime_memory_pressure_exit_tests PRIVATE
    uml_runtime_memory_pressure_exit_first
    UML::RuntimeMemoryPressure
    UML::RuntimeGuestStatistics
    UML::PlatformContract)
uml_add_unit_test(
    NAME runtime-memory-pressure-exit
    COMMAND uml_runtime_memory_pressure_exit_tests
    TIMEOUT 60)
