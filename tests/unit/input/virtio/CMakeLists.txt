# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_input_tests virtio_input_tests.cpp)
target_link_libraries(uml_virtio_input_tests PRIVATE UML::VirtioInput)
uml_add_unit_test(NAME virtio-input-protocol COMMAND uml_virtio_input_tests
    LABELS input)

# What a full event queue owes the guest: the release of a key or a contact
# that a dropped report would have ended.
add_executable(uml_virtio_input_queue_release_tests
    input_queue_release_tests.cpp)
target_link_libraries(uml_virtio_input_queue_release_tests
    PRIVATE UML::VirtioInput)
uml_add_unit_test(NAME virtio-input-queue-release
    COMMAND uml_virtio_input_queue_release_tests LABELS input)
