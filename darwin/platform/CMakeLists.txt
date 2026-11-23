# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_platform_contract INTERFACE)
add_library(UML::PlatformContract ALIAS uml_platform_contract)
uml_declare_architecture_target(uml_platform_contract platform)

# Header-only: blocks the guest timer around host thread creation.
add_library(uml_guest_timer_mask INTERFACE)
add_library(UML::GuestTimerMask ALIAS uml_guest_timer_mask)
uml_declare_architecture_target(uml_guest_timer_mask platform)
target_include_directories(uml_guest_timer_mask INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin)

# Header-only: the host GPU's fixed budget, evaluated once at launch.
add_library(uml_gpu_budget INTERFACE)
add_library(UML::GpuBudget ALIAS uml_gpu_budget)
uml_declare_architecture_target(uml_gpu_budget platform)
target_include_directories(uml_gpu_budget INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})

# Header-only: host file-descriptor ownership.
add_library(uml_descriptor INTERFACE)
add_library(UML::Descriptor ALIAS uml_descriptor)
uml_declare_architecture_target(uml_descriptor platform)
target_include_directories(uml_descriptor INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin)

# Header-only: what the platform publishes for the runtime's session record to
# read (frame_figures.hpp, host_state_figures.hpp, gpu_memory_figures.hpp).
add_library(uml_published_figures INTERFACE)
add_library(UML::PublishedFigures ALIAS uml_published_figures)
uml_declare_architecture_target(uml_published_figures platform)
target_include_directories(uml_published_figures INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin)

set(UML_VIDEO_DECODER_SOURCES
    apple/video_decoder.mm
    apple/hevc_stream.cpp
    apple/h264_stream.cpp
    apple/vp9_stream.cpp)
add_library(uml_video_decoder STATIC ${UML_VIDEO_DECODER_SOURCES})
add_library(UML::VideoDecoder ALIAS uml_video_decoder)
uml_declare_architecture_target(uml_video_decoder platform)
target_include_directories(uml_video_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_video_decoder PRIVATE UML::PlatformContract
    "-framework Foundation" "-framework CoreMedia" "-framework CoreVideo"
    "-framework VideoToolbox")
if(UML_BUILD_UNIT_TESTS)
    # The same sources with the session-loss injection the recovery test
    # drives; products compile them without it.
    add_library(uml_video_decoder_test_variant STATIC
        ${UML_VIDEO_DECODER_SOURCES})
    uml_declare_architecture_target(uml_video_decoder_test_variant platform)
    target_include_directories(uml_video_decoder_test_variant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_video_decoder_test_variant PUBLIC
        UML_VIDEO_DECODER_TESTING=1)
    target_link_libraries(uml_video_decoder_test_variant PRIVATE
        UML::PlatformContract
        "-framework Foundation" "-framework CoreMedia" "-framework CoreVideo"
        "-framework VideoToolbox")
endif()

# The hardware video encoder: VideoToolbox discovery, sessions and Annex B
# output behind darwin/platform/video_encoder.hpp.
set(UML_VIDEO_ENCODER_SOURCES
    apple/video_encoder.mm
    apple/video_encoder_discovery.mm)
add_library(uml_video_encoder STATIC ${UML_VIDEO_ENCODER_SOURCES})
add_library(UML::VideoEncoder ALIAS uml_video_encoder)
uml_declare_architecture_target(uml_video_encoder platform)
target_include_directories(uml_video_encoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_video_encoder PRIVATE UML::PlatformContract
    "-framework Accelerate" "-framework Foundation" "-framework CoreMedia"
    "-framework CoreVideo" "-framework VideoToolbox")
if(UML_BUILD_UNIT_TESTS)
    # The same sources with the session-loss injection the recovery test
    # drives; products compile them without it.
    add_library(uml_video_encoder_test_variant STATIC
        ${UML_VIDEO_ENCODER_SOURCES})
    uml_declare_architecture_target(uml_video_encoder_test_variant platform)
    target_include_directories(uml_video_encoder_test_variant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_video_encoder_test_variant PUBLIC
        UML_VIDEO_ENCODER_TESTING=1)
    target_link_libraries(uml_video_encoder_test_variant PRIVATE
        UML::PlatformContract
        "-framework Accelerate" "-framework Foundation" "-framework CoreMedia"
        "-framework CoreVideo" "-framework VideoToolbox")
endif()

add_library(uml_host_battery STATIC apple/host_battery.mm)
add_library(UML::HostBattery ALIAS uml_host_battery)
uml_declare_architecture_target(uml_host_battery platform)
target_include_directories(uml_host_battery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_host_battery PRIVATE
    UML::PlatformContract
    "-framework Foundation")
if(UML_TARGET_MACOS)
    target_link_libraries(uml_host_battery PRIVATE "-framework IOKit")
else()
    target_link_libraries(uml_host_battery PRIVATE "-framework UIKit")
endif()

# The system's microphone permission. Reading it never prompts.
add_library(uml_microphone_authorization STATIC
    apple/microphone_authorization.mm)
add_library(UML::MicrophoneAuthorization ALIAS uml_microphone_authorization)
uml_declare_architecture_target(uml_microphone_authorization platform)
target_include_directories(uml_microphone_authorization PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_microphone_authorization PRIVATE
    UML::PlatformContract
    "-framework AVFAudio"
    "-framework Foundation")

# The camera: the picture and DNG helpers, which are portable C++, and the
# AVFoundation implementation of darwin/platform/camera.hpp with the system's
# camera permission. The synthetic camera has its own directory.
add_library(uml_nv12_transform STATIC nv12_transform.cpp)
add_library(UML::Nv12Transform ALIAS uml_nv12_transform)
uml_declare_architecture_target(uml_nv12_transform platform)
target_include_directories(uml_nv12_transform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(uml_dng_color STATIC dng_color.cpp)
add_library(UML::DngColor ALIAS uml_dng_color)
uml_declare_architecture_target(uml_dng_color platform)
target_include_directories(uml_dng_color PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# What a still's exposure was, from its photo's metadata (a pure function).
add_library(uml_still_exposure STATIC still_exposure.cpp)
add_library(UML::StillExposure ALIAS uml_still_exposure)
uml_declare_architecture_target(uml_still_exposure platform)
target_include_directories(uml_still_exposure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_still_exposure PUBLIC UML::DngColor)

# Which photo size a camera is asked for, and whether the photo is that size.
add_library(uml_photo_dimensions STATIC photo_dimensions.cpp)
add_library(UML::PhotoDimensions ALIAS uml_photo_dimensions)
uml_declare_architecture_target(uml_photo_dimensions platform)
target_include_directories(uml_photo_dimensions PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Which video stabilization a camera is asked for (a pure function).
add_library(uml_video_stabilization STATIC video_stabilization.cpp)
add_library(UML::VideoStabilization ALIAS uml_video_stabilization)
uml_declare_architecture_target(uml_video_stabilization platform)
target_include_directories(uml_video_stabilization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(uml_apple_camera STATIC
    apple/accelerated_nv12.cpp
    apple/camera_authorization.mm
    apple/camera_capture.mm
    apple/key_path_observer.mm
    apple/photo_metadata.cpp
    apple/turned_still.mm)
add_library(UML::AppleCamera ALIAS uml_apple_camera)
uml_declare_architecture_target(uml_apple_camera platform)
target_include_directories(uml_apple_camera PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_apple_camera PUBLIC UML::DngColor UML::Nv12Transform UML::PhotoDimensions UML::StillExposure
    UML::VideoEncoder UML::VideoStabilization)
target_link_libraries(uml_apple_camera PRIVATE
    UML::PlatformContract
    "-framework Accelerate"
    "-framework AVFoundation"
    "-framework CoreGraphics"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework Foundation"
    "-framework ImageIO"
    "-framework IOSurface"
    "-framework VideoToolbox")

# The camera without hardware, for tests and for the launchers that run a guest
# against a chart.
add_subdirectory(synthetic_camera)

add_library(uml_host_gpu_memory STATIC apple/host_gpu_memory.mm)
add_library(UML::HostGpuMemory ALIAS uml_host_gpu_memory)
uml_declare_architecture_target(uml_host_gpu_memory platform)
target_include_directories(uml_host_gpu_memory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_host_gpu_memory PRIVATE
    UML::PlatformContract
    objc
    "-framework Metal")

add_library(uml_run_routing OBJECT run_routing.cpp)
add_library(UML::RunRouting ALIAS uml_run_routing)
uml_declare_architecture_target(uml_run_routing platform)
target_include_directories(uml_run_routing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(uml_run_routing PRIVATE
    UML_RUN_ROUTING_TESTING=0)
if(UML_BUILD_UNIT_TESTS)
    # The same source with its failure-injection points, for the routing
    # transaction tests; products compile it without them.
    add_library(uml_run_routing_test_variant OBJECT run_routing.cpp)
    uml_declare_architecture_target(uml_run_routing_test_variant platform)
    target_include_directories(uml_run_routing_test_variant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_run_routing_test_variant PRIVATE
        UML_RUN_ROUTING_TESTING=1)
endif()

add_library(uml_host_topology OBJECT host_topology.cpp)
add_library(UML::HostTopology ALIAS uml_host_topology)
uml_declare_architecture_target(uml_host_topology platform)
target_link_libraries(uml_host_topology PRIVATE UML::PlatformContract)
target_include_directories(uml_host_topology PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(uml_host_environment OBJECT apple/host_environment.mm)
add_library(UML::HostEnvironment ALIAS uml_host_environment)
uml_declare_architecture_target(uml_host_environment platform)
target_link_libraries(uml_host_environment PRIVATE
    UML::PlatformContract)
target_include_directories(uml_host_environment PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# The system's thermal state, Low Power Mode and active processor count, for
# the stall record.
add_library(uml_host_thermal OBJECT apple/host_thermal.mm)
add_library(UML::HostThermal ALIAS uml_host_thermal)
uml_declare_architecture_target(uml_host_thermal platform)
target_link_libraries(uml_host_thermal PRIVATE
    UML::PlatformContract)
target_include_directories(uml_host_thermal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# The system's network path: whether the host has one, and the event-driven
# monitor that keeps it current (no polling; the system calls back on a change).
# The product monitor calls the system's creation of a monitor directly. A
# test variant publishes the private header that lets a test have the system
# give none, and the product archive has no trace of it.
function(_uml_add_host_network_path TARGET TESTING)
    add_library(${TARGET} STATIC apple/host_network_path.mm)
    uml_declare_architecture_target(${TARGET} platform)
    target_include_directories(${TARGET} PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin)
    if(TESTING)
        target_include_directories(${TARGET} PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/apple)
        target_compile_definitions(${TARGET} PRIVATE
            UML_HOST_NETWORK_PATH_TESTING=1)
    else()
        target_compile_definitions(${TARGET} PRIVATE
            UML_HOST_NETWORK_PATH_TESTING=0)
    endif()
    target_link_libraries(${TARGET} PRIVATE
        UML::PlatformContract
        "-framework Foundation"
        "-framework Network")
endfunction()

_uml_add_host_network_path(uml_host_network_path 0)
add_library(UML::HostNetworkPath ALIAS uml_host_network_path)
if(UML_BUILD_UNIT_TESTS OR UML_BUILD_TOOLING_TESTS OR
   UML_BUILD_INTEGRATION_TESTS)
    _uml_add_host_network_path(uml_host_network_path_test_variant 1)
    add_library(UML::HostNetworkPathTestVariant ALIAS
        uml_host_network_path_test_variant)
endif()

add_library(uml_guest_files OBJECT guest_files.cpp)
add_library(UML::GuestFiles ALIAS uml_guest_files)
uml_declare_architecture_target(uml_guest_files platform)
target_link_libraries(uml_guest_files PRIVATE UML::PlatformContract)
target_include_directories(uml_guest_files PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
if(UML_BUILD_UNIT_TESTS)
    # The same source under the iOS family's disk-lock policy, which takes no
    # host file lock, for its tests on a macOS host.
    add_library(uml_guest_files_process_locks OBJECT guest_files.cpp)
    uml_declare_architecture_target(uml_guest_files_process_locks platform)
    target_link_libraries(uml_guest_files_process_locks PRIVATE
        UML::PlatformContract)
    target_include_directories(uml_guest_files_process_locks PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_guest_files_process_locks PRIVATE
        UML_GUEST_FILES_PROCESS_LOCKS=1)
endif()

add_library(uml_platform_component INTERFACE)
add_library(UML::PlatformComponent ALIAS uml_platform_component)
uml_declare_architecture_target(uml_platform_component platform)
target_link_libraries(uml_platform_component INTERFACE
    UML::PlatformContract
    "-framework Foundation")
target_sources(uml_platform_component INTERFACE
    $<TARGET_OBJECTS:uml_run_routing>
    $<TARGET_OBJECTS:uml_host_topology>
    $<TARGET_OBJECTS:uml_host_environment>
    $<TARGET_OBJECTS:uml_host_thermal>
    $<TARGET_OBJECTS:uml_guest_files>)
