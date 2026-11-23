# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_virtio_input STATIC
    hardware_input.cpp
    hardware_input_run.cpp
    hardware_reports.cpp
    held_input.cpp
    hid_keyboard.cpp
    in_process_service.cpp
    touchscreen.cpp
    virtio_input_device.cpp)
add_library(UML::VirtioInput ALIAS uml_virtio_input)
uml_declare_architecture_target(uml_virtio_input device)
target_include_directories(uml_virtio_input
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
        ${PROJECT_SOURCE_DIR}/kernel/common/input)
target_link_libraries(uml_virtio_input PUBLIC UML::Descriptor)
target_link_libraries(uml_virtio_input PRIVATE UML::VhostUserTransport)
