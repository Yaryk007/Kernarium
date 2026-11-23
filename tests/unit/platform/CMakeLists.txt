# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_file_sync_tests guest_file_sync_tests.cpp)
target_link_libraries(uml_guest_file_sync_tests PRIVATE UML::GuestFiles)
uml_add_unit_test(NAME platform-guest-file-sync
    COMMAND uml_guest_file_sync_tests)

# The iOS family's disk locks: the kernel's locks stay in the registry and no
# host file is locked at any step of a guest's life.
add_executable(uml_guest_file_process_lock_tests
    guest_file_process_lock_tests.cpp)
target_link_libraries(uml_guest_file_process_lock_tests PRIVATE
    uml_guest_files_process_locks)
uml_add_unit_test(NAME platform-guest-file-process-locks
    COMMAND uml_guest_file_process_lock_tests)

add_executable(uml_video_decoder_tests video_decoder_tests.cpp)
target_link_libraries(uml_video_decoder_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-decoder
    COMMAND uml_video_decoder_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 120)

# Losing the hardware session: the decoder's test variant invalidates the
# session of the next picture, as iOS does to an app that was in the background.
add_executable(uml_video_decoder_recovery_tests video_decoder_recovery_tests.cpp)
target_link_libraries(uml_video_decoder_recovery_tests PRIVATE
    uml_video_decoder_test_variant)
uml_add_unit_test(NAME platform-video-decoder-recovery
    COMMAND uml_video_decoder_recovery_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 120)

# The decoder's HEVC header readers and output ordering, which need no hardware.
add_executable(uml_hevc_stream_tests hevc_stream_tests.cpp)
target_link_libraries(uml_hevc_stream_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-hevc-stream
    COMMAND uml_hevc_stream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The decoder's H.264 header readers and output ordering, which need no hardware.
add_executable(uml_h264_stream_tests h264_stream_tests.cpp)
target_link_libraries(uml_h264_stream_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-h264-stream
    COMMAND uml_h264_stream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The decoder's VP9 packet and header readers, which need no hardware and are
# what its bounds on a guest's packets stand on.
add_executable(uml_vp9_stream_tests vp9_stream_tests.cpp)
target_link_libraries(uml_vp9_stream_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-vp9-stream
    COMMAND uml_vp9_stream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The video encoder backend's Annex B and parameter-set readers, which need no
# hardware.
add_executable(uml_video_encoder_bitstream_tests video_encoder_bitstream_tests.cpp)
target_include_directories(uml_video_encoder_bitstream_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(NAME platform-video-encoder-bitstream
    COMMAND uml_video_encoder_bitstream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The hardware video encoder on this host: discovery, every advertised profile
# and mode, and the stream checked against the decoder.
add_executable(uml_video_encoder_tests video_encoder_tests.cpp)
target_link_libraries(uml_video_encoder_tests PRIVATE
    UML::VideoEncoder UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-encoder
    COMMAND uml_video_encoder_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 600)

# Losing the hardware session: the encoder's test variant fails sessions on
# demand, as the system does when it revokes the hardware.
add_executable(uml_video_encoder_recovery_tests video_encoder_recovery_tests.cpp)
target_link_libraries(uml_video_encoder_recovery_tests PRIVATE
    uml_video_encoder_test_variant UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-encoder-recovery
    COMMAND uml_video_encoder_recovery_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 300)

# The JPEG size bound, with the sample delivered in two Core Media blocks (the test variant splits it on demand).
add_executable(uml_video_encoder_jpeg_bound_tests video_encoder_jpeg_bound_tests.cpp)
target_link_libraries(uml_video_encoder_jpeg_bound_tests PRIVATE
    uml_video_encoder_test_variant)
uml_add_unit_test(NAME platform-video-encoder-jpeg-bound
    COMMAND uml_video_encoder_jpeg_bound_tests
    LABELS video TIMEOUT 300)

# Discovery against a scripted host, which needs no hardware: a refusal and a
# ceiling are answers and are stored; a probe the host did not answer is not an
# answer, leaves the discovery incomplete and out of the cache, and is asked
# again later, a bounded number of times.
add_executable(uml_video_encoder_discovery_tests video_encoder_discovery_tests.cpp)
target_link_libraries(uml_video_encoder_discovery_tests PRIVATE
    uml_video_encoder_test_variant)
uml_add_unit_test(NAME platform-video-encoder-discovery
    COMMAND uml_video_encoder_discovery_tests
    LABELS video)

# The capability cache, through child processes that find a valid file, no
# file, or a file they cannot vouch for.
add_executable(uml_video_encoder_cache_tests video_encoder_cache_tests.cpp)
target_link_libraries(uml_video_encoder_cache_tests PRIVATE UML::VideoEncoder)
uml_add_unit_test(NAME platform-video-encoder-cache
    COMMAND uml_video_encoder_cache_tests
    LABELS video TIMEOUT 900)

# The system limits hardware encoder sessions across every process, so the tests
# that hold them (one holds all it can get) run one at a time.
set_tests_properties(platform-video-encoder platform-video-encoder-recovery
    platform-video-encoder-cache PROPERTIES RESOURCE_LOCK video-encoder-hardware)
# A host that left a probe of the discovery unanswered in every discovery the
# test asked for cannot validate those codecs; the test says so and is skipped,
# not passed.
set_tests_properties(platform-video-encoder PROPERTIES SKIP_RETURN_CODE 77)

# The platform's run routing; the transaction tests use its test variant.
add_executable(uml_run_routing_tests
    run_routing_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing_test_variant>)
target_link_libraries(uml_run_routing_tests PRIVATE
    uml_test_allocation_failure)
target_include_directories(uml_run_routing_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_compile_definitions(uml_run_routing_tests PRIVATE
    UML_RUN_ROUTING_TESTING=1)
uml_add_unit_test(
    NAME runtime-route-transaction
    COMMAND uml_run_routing_tests
    LABELS tsan)

add_executable(uml_run_routing_stress_tests
    run_routing_stress_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing>)
target_include_directories(uml_run_routing_stress_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME runtime-route-unbound-reads
    COMMAND uml_run_routing_stress_tests)

# The process's memory limit: the host's own, else the one a launch states, so a
# Mac measurement sees the budgets that derive from the limit as the iPad does.
add_executable(uml_host_environment_tests
    host_environment_tests.cpp
    $<TARGET_OBJECTS:uml_host_environment>)
target_link_libraries(uml_host_environment_tests PRIVATE
    UML::PlatformContract
    "-framework Foundation")
target_include_directories(uml_host_environment_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME platform-host-environment
    COMMAND uml_host_environment_tests)

# The widest run of address space a process has free: what the settings of an
# application signed without Extended Virtual Addressing offer a guest by.
add_executable(uml_address_space_tests address_space_tests.cpp)
target_include_directories(uml_address_space_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME platform-address-space
    COMMAND uml_address_space_tests)

# The Apple thermal source's notices of a change of the thermal state or of Low
# Power Mode, until the caller withdraws and after the provider ends.
add_executable(uml_host_thermal_tests
    host_thermal_tests.mm
    $<TARGET_OBJECTS:uml_host_thermal>)
target_link_libraries(uml_host_thermal_tests PRIVATE
    UML::PlatformContract
    "-framework Foundation")
target_include_directories(uml_host_thermal_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME platform-host-thermal
    COMMAND uml_host_thermal_tests)

# The host's network path: the state every report replaces, which statuses are
# an outage, and the system monitor's report at each start, until it ends.
add_executable(uml_host_network_path_tests host_network_path_tests.cpp)
target_link_libraries(uml_host_network_path_tests PRIVATE
    UML::HostNetworkPathTestVariant)
uml_add_unit_test(
    NAME platform-host-network-path
    COMMAND uml_host_network_path_tests)

# The host GPU's fixed budget: what the limit leaves above the floor, the
# guest's RAM where there is no limit, and when the process is short of the
# floor.
add_executable(uml_gpu_budget_tests gpu_budget_tests.cpp)
target_link_libraries(uml_gpu_budget_tests PRIVATE UML::GpuBudget)
uml_add_unit_test(
    NAME platform-gpu-budget
    COMMAND uml_gpu_budget_tests)
