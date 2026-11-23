# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_memory_domain_lifetime_tests
    domain_lifetime_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>
    $<TARGET_OBJECTS:uml_native_mmu_test_variant>)
target_link_libraries(uml_memory_domain_lifetime_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_memory_domain_lifetime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_memory_domain_lifetime_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME memory-domain-lifetime
    COMMAND uml_memory_domain_lifetime_tests LABELS tsan)

# The heaps a unit of the engine maps for itself and gives back whole.
add_executable(uml_memory_domain_heap_tests domain_heap_tests.cpp)
target_link_libraries(uml_memory_domain_heap_tests PRIVATE UML::MemoryHeap)
target_include_directories(uml_memory_domain_heap_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory)
uml_add_unit_test(NAME memory-domain-heap
    COMMAND uml_memory_domain_heap_tests LABELS tsan)

add_executable(uml_memory_address_window_tests address_window_tests.cpp)
target_link_libraries(uml_memory_address_window_tests PRIVATE
    uml_address_layout_test_variant)
uml_add_unit_test(NAME memory-address-window-reads
    COMMAND uml_memory_address_window_tests)

add_executable(uml_native_window_reservation_tests
    native_window_reservation_tests.cpp)
target_link_libraries(uml_native_window_reservation_tests PRIVATE
    UML::BridgeAbi
    UML::MemoryTestComponent)
target_include_directories(uml_native_window_reservation_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
# The test asserts where the host places allocations, which a concurrent
# VM-heavy test can perturb.
uml_add_unit_test(NAME memory-native-window-reservation
    COMMAND uml_native_window_reservation_tests)
set_tests_properties(memory-native-window-reservation PROPERTIES
    RUN_SERIAL TRUE)

# The direct-to-shadow slot conversion exists because a guest page is smaller
# than the host page.
add_executable(uml_native_direct_conversion_tests
    native_direct_conversion_tests.cpp)
target_link_libraries(uml_native_direct_conversion_tests PRIVATE
    UML::BridgeAbi
    UML::MemoryTestComponent)
target_include_directories(uml_native_direct_conversion_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
# Like the other native-window tests, it maps at fixed window addresses
# that a concurrent VM-heavy test can occupy.
uml_add_unit_test(NAME memory-native-direct-conversion
    COMMAND uml_native_direct_conversion_tests)
set_tests_properties(memory-native-direct-conversion PROPERTIES
    RUN_SERIAL TRUE)

add_executable(uml_shared_backing_view_tests shared_backing_view_tests.cpp)
target_link_libraries(uml_shared_backing_view_tests PRIVATE
    UML::MemorySharedBacking)
uml_add_unit_test(NAME memory-shared-backing-view
    COMMAND uml_shared_backing_view_tests)

add_executable(uml_word_shared_mutex_tests word_shared_mutex_tests.cpp)
target_include_directories(uml_word_shared_mutex_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
uml_add_unit_test(NAME memory-word-shared-mutex
    COMMAND uml_word_shared_mutex_tests LABELS tsan)

add_executable(uml_guest_ram_commitment_tests guest_ram_commitment_tests.cpp)
target_link_libraries(uml_guest_ram_commitment_tests PRIVATE
    UML::MemorySharedBacking)
# The footprint assertions read the whole process, which a concurrent
# memory-heavy test would move.
uml_add_unit_test(NAME memory-guest-ram-commitment
    COMMAND uml_guest_ram_commitment_tests)
set_tests_properties(memory-guest-ram-commitment PROPERTIES RUN_SERIAL TRUE)

# The guest RAM image's footprint assertions read the whole process too.
add_executable(uml_guest_ram_image_tests guest_ram_image_tests.cpp)
target_link_libraries(uml_guest_ram_image_tests PRIVATE
    UML::MemorySharedBacking)
uml_add_unit_test(NAME memory-guest-ram-image
    COMMAND uml_guest_ram_image_tests)
set_tests_properties(memory-guest-ram-image PROPERTIES RUN_SERIAL TRUE)

# Raw MMU compositions carry the shared-backing archive MappedBacking
# references, as the production memory component does.
add_executable(uml_static_mmu_batch_tests
    static_mmu_batch_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_mmu_batch_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_mmu_batch_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_mmu_batch_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME static-mmu-batch COMMAND uml_static_mmu_batch_tests
    LABELS tsan)

# A retired domain gives the pages of its bookkeeping back to the process.
add_executable(uml_static_retire_tests
    static_retire_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_retire_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_retire_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_retire_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME memory-static-retire COMMAND uml_static_retire_tests
    LABELS tsan)

# The x86-64 guest's writer accounting at the four mapping primitives.
if(UML_BUILD_X86_64_GUEST)
    add_executable(uml_frame_accounting_tests
        frame_accounting_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
    target_link_libraries(uml_frame_accounting_tests PRIVATE
        UML::BridgeAbi
        UML::MemorySharedBacking)
    target_include_directories(uml_frame_accounting_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_definitions(uml_frame_accounting_tests PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=1)
    uml_add_unit_test(NAME memory-frame-accounting
        COMMAND uml_frame_accounting_tests LABELS tsan)

    # The holders of those frames: registration, the debt it owes, and the
    # publication of a store or a kernel write at every holder.
    add_executable(uml_frame_holder_tests
        frame_holder_tests.cpp
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
    target_link_libraries(uml_frame_holder_tests PRIVATE
        UML::BridgeAbi
        UML::MemorySharedBacking)
    target_include_directories(uml_frame_holder_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_definitions(uml_frame_holder_tests PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=1)
    uml_add_unit_test(NAME memory-frame-holders
        COMMAND uml_frame_holder_tests LABELS tsan)
endif()

# What a mapping change publishes as a code change.
add_executable(uml_static_mmu_code_change_tests
    static_mmu_code_change_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_mmu_code_change_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_mmu_code_change_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_mmu_code_change_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME static-mmu-code-change
    COMMAND uml_static_mmu_code_change_tests LABELS tsan)

# The executable-mapping count of a physical page's record and its epoch.
add_executable(uml_static_mmu_executable_count_tests
    static_mmu_executable_count_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_mmu_executable_count_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_mmu_executable_count_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_mmu_executable_count_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME static-mmu-executable-count
    COMMAND uml_static_mmu_executable_count_tests LABELS tsan)

# What StaticGuestMemory reports as a code page's stamp.
add_executable(uml_static_guest_memory_stamp_tests
    static_guest_memory_stamp_tests.cpp
    $<TARGET_OBJECTS:uml_static_guest_memory>
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_guest_memory_stamp_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_guest_memory_stamp_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_guest_memory_stamp_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME static-guest-memory-stamp
    COMMAND uml_static_guest_memory_stamp_tests LABELS tsan)

# Which generations hold a translation of a frame of guest code, and what a
# code synchronization or a kernel write tells them. The fetch observer holds a
# reader between its copy of a frame and its return.
add_executable(uml_static_mmu_code_holder_tests
    static_mmu_code_holder_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>
    $<TARGET_OBJECTS:uml_static_guest_memory_test_variant>)
target_link_libraries(uml_static_mmu_code_holder_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_mmu_code_holder_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_mmu_code_holder_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1
    UML_STATIC_GUEST_MEMORY_TESTING=1)
uml_add_unit_test(NAME static-mmu-code-holder
    COMMAND uml_static_mmu_code_holder_tests LABELS tsan)

# The counters reporter's start leaves the caller's signal mask as it found it
# when an allocation or the thread's creation fails, before and after the
# signals are blocked.
add_executable(uml_code_holder_reporter_tests
    code_holder_reporter_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_code_holder_reporter_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_test_allocation_failure)
target_include_directories(uml_code_holder_reporter_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_code_holder_reporter_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME code-holder-reporter-path-failure
    COMMAND uml_code_holder_reporter_tests 1)
uml_add_unit_test(NAME code-holder-reporter-thread-failure
    COMMAND uml_code_holder_reporter_tests 2)
uml_add_unit_test(NAME code-holder-reporter-create-failure
    COMMAND uml_code_holder_reporter_tests 3)

# The fill index's word layout and radix table, and the backing slots its
# words name. The slot test builds real backings.
add_executable(uml_fill_index_tests fill_index_tests.cpp)
target_link_libraries(uml_fill_index_tests PRIVATE
    UML::MemoryContract
    UML::MemoryHeap)
target_include_directories(uml_fill_index_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
uml_add_unit_test(NAME memory-fill-index COMMAND uml_fill_index_tests
    LABELS tsan)

add_executable(uml_backing_slots_tests
    backing_slots_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_backing_slots_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_backing_slots_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_backing_slots_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME memory-backing-slots COMMAND uml_backing_slots_tests)

# The page index of the static MMU, through its bridge, against a model.
add_executable(uml_static_mmu_index_tests
    static_mmu_index_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_static_mmu_index_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_static_mmu_index_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_static_mmu_index_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME static-mmu-index COMMAND uml_static_mmu_index_tests
    LABELS tsan)

add_executable(uml_native_mmu_tests
    native_mmu_tests.cpp
    $<TARGET_OBJECTS:uml_native_mmu_test_variant>
    $<TARGET_OBJECTS:uml_memory_vm_bridge>
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
target_link_libraries(uml_native_mmu_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_native_mmu_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_native_mmu_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)
# The test reserves the fixed Darwin native bridge window. Running after
# several concurrent VM-heavy tests can make that host reservation fail
# transiently even though an isolated retry succeeds.
set_tests_properties(native-4k-shadow-mmu PROPERTIES RUN_SERIAL TRUE)

# A run's end retires its native address space after the loader has released
# part of guest RAM.
add_executable(uml_native_retire_tests
    native_retire_tests.cpp
    $<TARGET_OBJECTS:uml_static_mmu_test_variant>
    $<TARGET_OBJECTS:uml_native_mmu_test_variant>)
target_link_libraries(uml_native_retire_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking)
target_include_directories(uml_native_retire_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/memory
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
target_compile_definitions(uml_native_retire_tests PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=1)
uml_add_unit_test(NAME memory-native-retire COMMAND uml_native_retire_tests
    LABELS tsan)
# Like the other native-window tests, it maps at fixed window addresses that a
# concurrent VM-heavy test can occupy.
set_tests_properties(memory-native-retire PROPERTIES RUN_SERIAL TRUE)
