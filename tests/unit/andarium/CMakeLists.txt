# SPDX-License-Identifier: GPL-2.0-or-later

# AndariumCore is the Andarium application's model as a Swift package with no
# UIKit, so its tests run on the host. SwiftPM keeps its products in the build
# tree, not beside the package.
uml_add_unit_test(NAME andarium-core
    COMMAND /usr/bin/xcrun swift test
        --package-path ${PROJECT_SOURCE_DIR}/Andarium/AndariumCore
        --scratch-path ${CMAKE_BINARY_DIR}/andarium-core-swiftpm
    LABELS andarium
    TIMEOUT 1200)

# The guest's motion sub-HAL (android/lineage-24/overlay/sensors): the wire
# format, its device-orientation part against the golden frames that
# AndariumCore checks too (the fixture's path is the test's argument, so the
# file has one copy), and what the orientation sensor reports and when.
add_executable(uml_andarium_motion_protocol_tests motion_protocol_tests.cpp)
target_include_directories(uml_andarium_motion_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/sensors)
uml_add_unit_test(NAME andarium-motion-protocol
    COMMAND uml_andarium_motion_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/motion-orientation-golden-frames.txt
    LABELS andarium)

add_executable(uml_andarium_device_orientation_tests device_orientation_tests.cpp)
target_include_directories(uml_andarium_device_orientation_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/sensors)
uml_add_unit_test(NAME andarium-device-orientation
    COMMAND uml_andarium_device_orientation_tests LABELS andarium)

# The guest's vibrator HAL (android/lineage-24/overlay/vibrator): the wire format
# against the golden frames that AndariumCore checks too (the fixture's path is
# the test's argument, so the file has one copy), the timelines the framework's
# calls become, and the HAL's state and timing against a fake link.
add_executable(uml_andarium_haptics_protocol_tests haptics_protocol_tests.cpp)
target_include_directories(uml_andarium_haptics_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/vibrator)
uml_add_unit_test(NAME andarium-haptics-protocol
    COMMAND uml_andarium_haptics_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/haptics-golden-frames.txt
    LABELS andarium)

add_executable(uml_andarium_haptics_player_tests haptics_player_tests.cpp)
target_include_directories(uml_andarium_haptics_player_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/vibrator)
uml_add_unit_test(NAME andarium-haptics-player
    COMMAND uml_andarium_haptics_player_tests LABELS andarium)

# The guest's thermal HAL (android/lineage-24/overlay/thermal): the wire format
# against the golden frames that AndariumCore checks too (the fixture's path is
# the test's argument, so the file has one copy), and the status, temperature
# and thresholds that each severity becomes.
add_executable(uml_andarium_thermal_protocol_tests thermal_protocol_tests.cpp)
target_include_directories(uml_andarium_thermal_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/thermal)
uml_add_unit_test(NAME andarium-thermal-protocol
    COMMAND uml_andarium_thermal_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/thermal-golden-frames.txt
    LABELS andarium)

add_executable(uml_andarium_thermal_levels_tests thermal_levels_tests.cpp)
target_include_directories(uml_andarium_thermal_levels_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/thermal)
uml_add_unit_test(NAME andarium-thermal-levels
    COMMAND uml_andarium_thermal_levels_tests LABELS andarium)

# The guest's gamepad relay (android/lineage-24/overlay/gamepad): the wire format
# against the golden frames that AndariumCore checks too (the fixture's path is
# the test's argument, so the file has one copy), the ranges of every field and
# the reader that turns a byte stream into frames.
add_executable(uml_andarium_gamepad_protocol_tests gamepad_protocol_tests.cpp)
target_include_directories(uml_andarium_gamepad_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/gamepad)
uml_add_unit_test(NAME andarium-gamepad-protocol
    COMMAND uml_andarium_gamepad_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gamepad-golden-frames.txt
    LABELS andarium)

# The guest's camera HAL (android/lineage-24/overlay/camera/hal): its copy of the
# camera device's records and controls against the wire header, the IDs and
# stream tables that a host's cameras become, how a request's settings become
# controls and an exposure's metadata its results, and the edits to a JPEG that
# the host encoded.
add_executable(uml_andarium_camera_abi_tests camera_abi_tests.cpp)
target_include_directories(uml_andarium_camera_abi_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal
    ${PROJECT_SOURCE_DIR}/darwin/device/camera
    ${PROJECT_SOURCE_DIR}/kernel/common/video)
uml_add_unit_test(NAME andarium-camera-abi
    COMMAND uml_andarium_camera_abi_tests LABELS andarium)

add_executable(uml_andarium_camera_plan_tests camera_plan_tests.cpp)
target_include_directories(uml_andarium_camera_plan_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME andarium-camera-plan
    COMMAND uml_andarium_camera_plan_tests LABELS andarium)

add_executable(uml_andarium_camera_controls_tests camera_controls_tests.cpp)
target_include_directories(uml_andarium_camera_controls_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME andarium-camera-controls
    COMMAND uml_andarium_camera_controls_tests LABELS andarium)

# How a request ends: the one deadline of its buffers, and a failed request
# (ERROR_REQUEST, so that the application hears of it) against a request that
# lost some of its outputs (ERROR_BUFFER).
add_executable(uml_andarium_camera_request_tests camera_request_tests.cpp)
target_include_directories(uml_andarium_camera_request_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME andarium-camera-request
    COMMAND uml_andarium_camera_request_tests LABELS andarium)

# When the HAL closes the host stream of an output: a preview that waits behind
# a still keeps it (and the host its capture session), a dropped output's stream
# still goes soon.
add_executable(uml_andarium_camera_streams_tests camera_streams_tests.cpp)
target_include_directories(uml_andarium_camera_streams_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME andarium-camera-streams
    COMMAND uml_andarium_camera_streams_tests LABELS andarium)

# The crop region that a result states, against the residual the camera service
# splits off an application's zoom.
add_executable(uml_andarium_camera_crop_tests camera_crop_tests.cpp)
target_include_directories(uml_andarium_camera_crop_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME andarium-camera-crop
    COMMAND uml_andarium_camera_crop_tests LABELS andarium)

# What the camera HAL does when the host ends a camera's video: the session kinds,
# the session-scoped state of a session that is over, and the reopening of a
# camera's streams.
add_executable(uml_andarium_camera_session_tests camera_session_tests.cpp)
target_include_directories(uml_andarium_camera_session_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME andarium-camera-session
    COMMAND uml_andarium_camera_session_tests LABELS andarium)

# The guest's Wi-Fi address as a function of eth0's
# (android/lineage-24/overlay/tools/kernarium_wifi_mac.h): the golden vectors
# (the guest runner reads the same file), the properties of every result and the
# rounds that refuse a candidate.
add_executable(uml_andarium_wifi_mac_tests wifi_mac_tests.cpp)
target_include_directories(uml_andarium_wifi_mac_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/tools)
uml_add_unit_test(NAME andarium-wifi-mac
    COMMAND uml_andarium_wifi_mac_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wifi-mac-golden-vectors.txt
    LABELS andarium)

# The old-application layer (android/lineage-24/overlay/oldapp), which lets a
# 32-bit application written for Android before 4.0 start on ART: the
# allocator that keeps its heap below 2 GiB, the scan of an application
# library's relocations for the allocator imports to redirect (synthetic
# ELF32 images), the page-protection window that writes into read-only memory
# pass through, and the JNI interposer against a fake function table that
# accepts live references only. The interposer's table indices are checked
# against the JDK's jni.h member order (the fixture's path is the test's
# argument).
set(UML_OLDAPP_DIR ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/oldapp)
add_executable(uml_andarium_oldapp_arena_tests oldapp_arena_tests.cpp)
target_include_directories(uml_andarium_oldapp_arena_tests PRIVATE ${UML_OLDAPP_DIR})
uml_add_unit_test(NAME andarium-oldapp-arena
    COMMAND uml_andarium_oldapp_arena_tests LABELS andarium)

add_executable(uml_andarium_oldapp_elf_scan_tests oldapp_elf_scan_tests.cpp)
target_include_directories(uml_andarium_oldapp_elf_scan_tests PRIVATE ${UML_OLDAPP_DIR})
uml_add_unit_test(NAME andarium-oldapp-elf-scan
    COMMAND uml_andarium_oldapp_elf_scan_tests LABELS andarium)

add_executable(uml_andarium_oldapp_protection_tests oldapp_protection_tests.cpp)
target_include_directories(uml_andarium_oldapp_protection_tests PRIVATE ${UML_OLDAPP_DIR})
uml_add_unit_test(NAME andarium-oldapp-protection
    COMMAND uml_andarium_oldapp_protection_tests LABELS andarium)

add_executable(uml_andarium_oldapp_jni_tests oldapp_jni_tests.cpp ${UML_OLDAPP_DIR}/jni_shim.cpp)
target_include_directories(uml_andarium_oldapp_jni_tests PRIVATE ${UML_OLDAPP_DIR})
uml_add_unit_test(NAME andarium-oldapp-jni
    COMMAND uml_andarium_oldapp_jni_tests ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jni-slots.tsv
    LABELS andarium)
