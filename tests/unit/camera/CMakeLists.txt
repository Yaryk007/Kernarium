# SPDX-License-Identifier: GPL-2.0-or-later

# The camera wire is header-only here: its layout is checked at compile time and
# its values at run time, with no device behind it.
add_executable(uml_camera_protocol_tests camera_protocol_tests.cpp)
target_include_directories(uml_camera_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/kernel/common/video)
uml_add_unit_test(NAME camera-protocol COMMAND uml_camera_protocol_tests
    LABELS "device;camera")

# What the application does with a running camera video when it leaves the
# foreground: the call that ends it and waits for the guest, and the order
# before the GPU hold.
add_executable(uml_camera_background_video_tests camera_background_video_tests.cpp)
target_link_libraries(uml_camera_background_video_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-background-video COMMAND uml_camera_background_video_tests
    LABELS "device;camera")

# The picture helpers of darwin/platform (camera.hpp's neighbours).
add_executable(uml_camera_transform_tests camera_transform_tests.cpp)
target_link_libraries(uml_camera_transform_tests PRIVATE UML::Nv12Transform)
uml_add_unit_test(NAME camera-frame-transform COMMAND uml_camera_transform_tests
    LABELS "device;camera")

add_executable(uml_camera_dng_tests camera_dng_tests.cpp)
target_link_libraries(uml_camera_dng_tests PRIVATE UML::DngColor)
uml_add_unit_test(NAME camera-dng-color COMMAND uml_camera_dng_tests
    LABELS "device;camera")

# Discovery of the host's own cameras: no session, no permission request, no
# capture.
add_executable(uml_camera_apple_discovery_tests camera_apple_discovery_tests.cpp)
target_link_libraries(uml_camera_apple_discovery_tests PRIVATE UML::AppleCamera UML::CameraDevice)
uml_add_unit_test(NAME camera-apple-discovery COMMAND uml_camera_apple_discovery_tests
    LABELS "device;camera")

# A platform call a host lacks, in camera discovery (an iPad app on a Mac or on a Vision Pro meets one): an
# object that does not take it is not sent it, and a discovery that raises anyway presents no cameras with one
# log line and a live process. The raise is stood in by a replaced method of the discovery session; no camera
# is opened.
add_executable(uml_camera_discovery_unavailable_call_tests camera_discovery_unavailable_call_tests.mm)
target_link_libraries(uml_camera_discovery_unavailable_call_tests PRIVATE UML::AppleCamera
    "-framework Foundation" "-framework AVFoundation")
target_include_directories(uml_camera_discovery_unavailable_call_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform/apple)
uml_add_unit_test(NAME camera-discovery-unavailable-call COMMAND uml_camera_discovery_unavailable_call_tests
    LABELS "device;camera")

# What a still reports of its exposure, from a literal photo-metadata dictionary
# (no camera is opened).
add_executable(uml_camera_still_exposure_tests camera_still_exposure_tests.cpp)
target_link_libraries(uml_camera_still_exposure_tests PRIVATE UML::AppleCamera
    "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO")
target_include_directories(uml_camera_still_exposure_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform/apple)
uml_add_unit_test(NAME camera-still-exposure COMMAND uml_camera_still_exposure_tests
    LABELS "device;camera")

# The key-value observer of the pressure watch, on a stand-in object (no camera is
# opened): the handler outlives a stop that comes while it runs.
add_executable(uml_camera_key_path_observer_tests camera_key_path_observer_tests.mm)
target_link_libraries(uml_camera_key_path_observer_tests PRIVATE UML::AppleCamera "-framework Foundation")
target_include_directories(uml_camera_key_path_observer_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform/apple)
uml_add_unit_test(NAME camera-key-path-observer COMMAND uml_camera_key_path_observer_tests
    LABELS "device;camera")

# Which photo size a camera is asked for, on tables of the shape the platform
# lists per format (no camera is opened).
add_executable(uml_camera_photo_dimensions_tests camera_photo_dimensions_tests.cpp)
target_link_libraries(uml_camera_photo_dimensions_tests PRIVATE UML::PhotoDimensions)
uml_add_unit_test(NAME camera-photo-dimensions COMMAND uml_camera_photo_dimensions_tests
    LABELS "device;camera")

# Which video stabilization a camera is asked for, over what its format supports
# (no camera is opened).
add_executable(uml_camera_video_stabilization_tests camera_video_stabilization_tests.cpp)
target_link_libraries(uml_camera_video_stabilization_tests PRIVATE UML::VideoStabilization)
uml_add_unit_test(NAME camera-video-stabilization COMMAND uml_camera_video_stabilization_tests
    LABELS "device;camera")

# The anchor model: the one format a camera runs in, what it offers from it, the
# session's admit and resize transactions on a fake session, and the invariants
# over seeded generated format tables.
add_executable(uml_camera_capture_model_tests camera_capture_model_tests.cpp)
target_link_libraries(uml_camera_capture_model_tests PRIVATE UML::PhotoDimensions UML::VideoStabilization)
uml_add_unit_test(NAME camera-capture-model COMMAND uml_camera_capture_model_tests
    LABELS "device;camera")

# The Apple host's accelerated transform against the portable one.
add_executable(uml_camera_apple_transform_tests camera_apple_transform_tests.cpp)
target_link_libraries(uml_camera_apple_transform_tests PRIVATE UML::AppleCamera)
uml_add_unit_test(NAME camera-apple-transform COMMAND uml_camera_apple_transform_tests
    LABELS "device;camera")

# A photo of a camera presented half around is turned and encoded in the hardware JPEG encoder (a pixel buffer stands
# in for the photo; no camera is opened).
add_executable(uml_camera_apple_turned_still_tests camera_apple_turned_still_tests.cpp)
target_link_libraries(uml_camera_apple_turned_still_tests PRIVATE UML::AppleCamera UML::VideoEncoder
    "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreVideo" "-framework ImageIO")
uml_add_unit_test(NAME camera-apple-turned-still COMMAND uml_camera_apple_turned_still_tests
    LABELS "device;camera")

# The synthetic camera is the platform camera that the other camera tests use.
add_executable(uml_camera_synthetic_tests camera_synthetic_tests.cpp)
target_link_libraries(uml_camera_synthetic_tests PRIVATE UML::SyntheticCamera)
uml_add_unit_test(NAME camera-synthetic COMMAND uml_camera_synthetic_tests
    LABELS "device;camera")

# The launcher's way to the platform cameras (UML::CameraSource), without a
# session or a permission request.
add_executable(uml_camera_source_tests camera_source_tests.cpp)
target_link_libraries(uml_camera_source_tests PRIVATE UML::CameraSource)
uml_add_unit_test(NAME camera-source COMMAND uml_camera_source_tests
    LABELS "device;camera")

# The camera device's pure parts: each is a function of its inputs, checked
# without a device.
add_executable(uml_camera_jpeg_gap_tests camera_jpeg_gap_tests.cpp)
target_link_libraries(uml_camera_jpeg_gap_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-jpeg-gap COMMAND uml_camera_jpeg_gap_tests
    LABELS "device;camera")

# How the guest's camera orientation follows the device where the host owns the
# rotation: pure arithmetic on the platform's camera description.
add_executable(uml_camera_orientation_tests camera_orientation_tests.cpp)
target_link_libraries(uml_camera_orientation_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-orientation COMMAND uml_camera_orientation_tests
    LABELS "device;camera")

add_executable(uml_camera_calibration_tests camera_calibration_tests.cpp)
target_link_libraries(uml_camera_calibration_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-calibration COMMAND uml_camera_calibration_tests
    LABELS "device;camera")

# The device's tests use the synthetic camera as the platform camera.
add_executable(uml_camera_controls_tests camera_controls_tests.cpp)
target_link_libraries(uml_camera_controls_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
uml_add_unit_test(NAME camera-controls COMMAND uml_camera_controls_tests
    LABELS "device;camera")

add_executable(uml_camera_description_tests camera_description_tests.cpp)
target_link_libraries(uml_camera_description_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
target_include_directories(uml_camera_description_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal)
uml_add_unit_test(NAME camera-description COMMAND uml_camera_description_tests
    LABELS "device;camera")

# The device behind the real transport across a guest's suspend. The front
# end is the GPU tests', so their support header and protocol header are on
# the path.
add_executable(uml_camera_held_requests_tests camera_held_requests_tests.cpp)
target_include_directories(uml_camera_held_requests_tests BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unit/gpu
    ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_link_libraries(uml_camera_held_requests_tests PRIVATE
    UML::CameraDevice UML::SyntheticCamera UML::VhostUserTransport)
uml_add_unit_test(NAME camera-held-requests COMMAND uml_camera_held_requests_tests
    LABELS "device;camera;vhost-user" TIMEOUT 120)

add_executable(uml_camera_device_tests camera_device_tests.cpp)
target_link_libraries(uml_camera_device_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
uml_add_unit_test(NAME camera-device COMMAND uml_camera_device_tests
    LABELS "device;camera" TIMEOUT 120)

# The layouts a host composes for the Android image: the closed set and its fallback order against the image's own
# table (the generator's output, which the test takes as its argument), and the concurrent feature against the HAL's
# own rule (its headers compile without Android).
add_executable(uml_camera_layout_tests camera_layout_tests.cpp)
target_include_directories(uml_camera_layout_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal
    ${PROJECT_SOURCE_DIR}/kernel/common/video)
target_link_libraries(uml_camera_layout_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
uml_add_unit_test(NAME camera-layout
    COMMAND uml_camera_layout_tests
        ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/generated/layouts.tsv
    LABELS "device;camera")
