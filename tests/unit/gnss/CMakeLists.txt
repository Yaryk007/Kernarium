# SPDX-License-Identifier: GPL-2.0-or-later

# The device is driven through its queues and its event descriptor with
# synthetic fixes; no guest is involved.
add_executable(uml_gnss_nmea_writer_tests nmea_writer_tests.cpp)
target_link_libraries(uml_gnss_nmea_writer_tests PRIVATE UML::GnssDevice)
uml_add_unit_test(NAME gnss-nmea-writer
    COMMAND uml_gnss_nmea_writer_tests
    LABELS "device;gnss")

add_executable(uml_gnss_synthetic_source_tests synthetic_source_tests.cpp)
target_link_libraries(uml_gnss_synthetic_source_tests
    PRIVATE UML::GnssSyntheticSource)
uml_add_unit_test(NAME gnss-synthetic-source
    COMMAND uml_gnss_synthetic_source_tests
    LABELS "device;gnss")

add_executable(uml_gnss_device_tests virtio_gnss_device_tests.cpp)
target_link_libraries(uml_gnss_device_tests PRIVATE UML::GnssDevice)
uml_add_unit_test(NAME virtio-gnss-device
    COMMAND uml_gnss_device_tests
    LABELS "device;gnss")

# The guest GNSS HAL (android/lineage-24/overlay/gnss): its headers hold the
# parsing, the reporting rules and the session, with the operating system and
# the framework behind seams, so they build and run here. The host writer's own
# epochs are what they read, so the two ends of the wire cannot drift.
set(UML_GNSS_HAL_DIR ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/gnss)

add_executable(uml_gnss_hal_nmea_tests gnss_hal_nmea_tests.cpp)
target_link_libraries(uml_gnss_hal_nmea_tests PRIVATE UML::GnssDevice)
target_include_directories(uml_gnss_hal_nmea_tests PRIVATE ${UML_GNSS_HAL_DIR})
uml_add_unit_test(NAME gnss-hal-nmea
    COMMAND uml_gnss_hal_nmea_tests
    LABELS "android;gnss")

add_executable(uml_gnss_hal_policy_tests gnss_hal_policy_tests.cpp)
target_link_libraries(uml_gnss_hal_policy_tests PRIVATE UML::GnssDevice)
target_include_directories(uml_gnss_hal_policy_tests PRIVATE ${UML_GNSS_HAL_DIR})
uml_add_unit_test(NAME gnss-hal-policy
    COMMAND uml_gnss_hal_policy_tests
    LABELS "android;gnss")

add_executable(uml_gnss_hal_engine_tests gnss_hal_engine_tests.cpp)
target_link_libraries(uml_gnss_hal_engine_tests PRIVATE UML::GnssDevice)
target_include_directories(uml_gnss_hal_engine_tests PRIVATE ${UML_GNSS_HAL_DIR})
uml_add_unit_test(NAME gnss-hal-engine
    COMMAND uml_gnss_hal_engine_tests
    LABELS "android;gnss")

add_executable(uml_gnss_hal_entry_tests gnss_hal_entry_tests.cpp)
target_link_libraries(uml_gnss_hal_entry_tests PRIVATE UML::GnssDevice)
target_include_directories(uml_gnss_hal_entry_tests PRIVATE ${UML_GNSS_HAL_DIR})
uml_add_unit_test(NAME gnss-hal-entry
    COMMAND uml_gnss_hal_entry_tests
    LABELS "android;gnss")

add_executable(uml_gnss_hal_callbacks_tests gnss_hal_callbacks_tests.cpp)
target_link_libraries(uml_gnss_hal_callbacks_tests PRIVATE UML::GnssDevice)
target_include_directories(uml_gnss_hal_callbacks_tests PRIVATE ${UML_GNSS_HAL_DIR})
uml_add_unit_test(NAME gnss-hal-callbacks
    COMMAND uml_gnss_hal_callbacks_tests
    LABELS "android;gnss")
