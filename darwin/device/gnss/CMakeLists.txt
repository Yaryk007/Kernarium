# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_gnss_device STATIC
    endpoint.cpp
    in_process_service.cpp
    nmea_writer.cpp
    virtio_gnss_device.cpp)
add_library(UML::GnssDevice ALIAS uml_gnss_device)
uml_declare_architecture_target(uml_gnss_device device)
target_include_directories(uml_gnss_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
        ${PROJECT_SOURCE_DIR}/darwin/platform)
target_link_libraries(uml_gnss_device PRIVATE UML::VhostUserTransport)

# The `--location` test sources. Only the launcher and their unit test link
# them.
add_library(uml_gnss_synthetic_source STATIC synthetic_source.cpp)
add_library(UML::GnssSyntheticSource ALIAS uml_gnss_synthetic_source)
uml_declare_architecture_target(uml_gnss_synthetic_source device)
target_include_directories(uml_gnss_synthetic_source
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
