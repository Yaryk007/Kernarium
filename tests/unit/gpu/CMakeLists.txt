# SPDX-License-Identifier: GPL-2.0-or-later

cmake_path(GET CMAKE_CURRENT_BINARY_DIR PARENT_PATH
    _uml_unit_binary_directory)

find_package(Threads REQUIRED)
set(_uml_gpu_device_directory ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
set(_uml_host_gl_libraries
    uml_gpu_renderer
    UML::ThirdPartyVirglrenderer
    UML::ThirdPartyEpoxy)

# Every GPU unit test process places Metal's shader cache in its scratch
# directory instead of opening the host's shared one.
add_library(uml_gpu_test_metal_shader_cache OBJECT
    metal_shader_cache_isolation.cpp)
target_link_libraries(uml_gpu_test_metal_shader_cache PUBLIC
    "-framework CoreFoundation")

# uml_add_gpu_unit_test(<test> <target> <source>... [LIBRARIES <library>...]
#     [INCLUDES <directory>...] [VULKAN] [UNIT_DIRECTORY] [TIMEOUT <seconds>])
#
# Builds <target> from the sources and registers it as the unit test <test>,
# labelled gpu, and vulkan for VULKAN. UNIT_DIRECTORY runs it in the unit-test
# binary directory. The target takes its own Metal shader cache. The unit-gpu
# test preset selects every test named gpu-*, so <test> must be.
function(uml_add_gpu_unit_test TEST TARGET)
    cmake_parse_arguments(PARSE_ARGV 2 _gpu_test
        "VULKAN;UNIT_DIRECTORY" "TIMEOUT" "LIBRARIES;INCLUDES")
    if(NOT "${TEST}" MATCHES "^gpu-")
        message(FATAL_ERROR
            "${TEST}: a GPU unit test is named gpu-*, which is how the "
            "unit-gpu test preset selects it")
    endif()
    add_executable(${TARGET} ${_gpu_test_UNPARSED_ARGUMENTS})
    target_link_libraries(${TARGET} PRIVATE ${_gpu_test_LIBRARIES}
        uml_gpu_test_metal_shader_cache)
    if(_gpu_test_INCLUDES)
        target_include_directories(${TARGET} PRIVATE ${_gpu_test_INCLUDES})
    endif()
    set(_options LABELS gpu)
    if(_gpu_test_VULKAN)
        list(APPEND _options vulkan)
    endif()
    if(_gpu_test_UNIT_DIRECTORY)
        list(APPEND _options WORKING_DIRECTORY "${_uml_unit_binary_directory}")
    endif()
    if(_gpu_test_TIMEOUT)
        list(APPEND _options TIMEOUT ${_gpu_test_TIMEOUT})
    endif()
    uml_add_unit_test(NAME ${TEST} COMMAND ${TARGET} ${_options})
endfunction()

# The renderer trims a guest context once it has been quiet for a while; the
# tracker that decides when needs no GPU device, only the GPU component.
if(TARGET uml_gpu_context_idle_tracker)
    uml_add_gpu_unit_test(gpu-context-idle-tracker
        uml_gpu_context_idle_tracker_tests context_idle_tracker_tests.cpp
        LIBRARIES UML::GpuContextIdleTracker Threads::Threads)
endif()

# Whose bytes the budget's bytes are: the ledger needs no GPU device either.
if(TARGET uml_gpu_context_memory_ledger)
    uml_add_gpu_unit_test(gpu-context-memory-ledger
        uml_gpu_context_memory_ledger_tests context_memory_ledger_tests.cpp
        LIBRARIES UML::GpuContextMemoryLedger Threads::Threads)
endif()

# When the guest is told its graphics memory is under pressure: the evaluator
# needs no GPU device either.
if(TARGET uml_gpu_memory_pressure)
    uml_add_gpu_unit_test(gpu-memory-pressure
        uml_gpu_memory_pressure_tests memory_pressure_tests.cpp
        LIBRARIES UML::GpuMemoryPressure)
endif()

# Whether a refused Venus allocation waits for the guest's reclaimer: the
# policy needs no GPU device either.
if(TARGET uml_gpu_reclaim_wait)
    uml_add_gpu_unit_test(gpu-reclaim-wait
        uml_gpu_reclaim_wait_tests reclaim_wait_tests.cpp
        LIBRARIES UML::GpuReclaimWait Threads::Threads)
endif()

uml_add_gpu_unit_test(gpu-metal-submit-timing-collector
    uml_gpu_metal_submit_timing_tests metal_submit_timing_tests.cpp
    LIBRARIES Threads::Threads
    VULKAN)

# The emulator bounds its own Metal shader cache by its index; the test needs
# no Metal device.
if(TARGET uml_gpu_metal_shader_cache)
    add_executable(uml_gpu_metal_shader_cache_tests
        metal_shader_cache_tests.cpp)
    target_link_libraries(uml_gpu_metal_shader_cache_tests PRIVATE
        UML::GpuMetalShaderCache)
    uml_add_unit_test(NAME gpu-metal-shader-cache
        COMMAND uml_gpu_metal_shader_cache_tests
        LABELS gpu)
endif()

# The emulator places and bounds Mesa's disk shader cache through the process
# environment; the test needs no Mesa and no Metal device.
if(TARGET uml_gpu_mesa_shader_cache)
    add_executable(uml_gpu_mesa_shader_cache_tests
        mesa_shader_cache_tests.cpp)
    target_link_libraries(uml_gpu_mesa_shader_cache_tests PRIVATE
        UML::GpuMesaShaderCache)
    uml_add_unit_test(NAME gpu-mesa-shader-cache
        COMMAND uml_gpu_mesa_shader_cache_tests
        LABELS gpu)
endif()

# Guest allocations and the host GPU driver's own share one reserving pool.
if(TARGET uml_gpu_device_memory_pool)
    uml_add_gpu_unit_test(gpu-device-memory-pool
        uml_gpu_device_memory_pool_tests device_memory_pool_tests.cpp
        LIBRARIES UML::GpuDeviceMemoryPool Threads::Threads
        TIMEOUT 60)
endif()

# Whether the pool's reclaim hook has a free to wait for.
if(TARGET uml_gpu_freed_memory_watch)
    uml_add_gpu_unit_test(gpu-freed-memory-watch
        uml_gpu_freed_memory_watch_tests freed_memory_watch_tests.cpp
        LIBRARIES UML::GpuFreedMemoryWatch Threads::Threads)
endif()

# The graphics footprint counts host GPU memory that Metal's allocated size
# omits.
uml_add_gpu_unit_test(gpu-host-graphics-footprint
    uml_gpu_host_graphics_footprint_tests host_graphics_footprint_tests.mm
    LIBRARIES UML::HostGpuMemory "-framework Metal"
    TIMEOUT 60)
set_tests_properties(gpu-host-graphics-footprint PROPERTIES
    SKIP_RETURN_CODE 77)

if(TARGET uml_third_party_kosmickrisp)
    uml_add_gpu_unit_test(gpu-kosmickrisp-external-image-alias
        uml_gpu_kosmickrisp_external_image_alias_tests
        kosmickrisp_external_image_alias_tests.cpp
        LIBRARIES UML::ThirdPartyKosmicKrisp
        VULKAN TIMEOUT 120)
    uml_add_gpu_unit_test(gpu-kosmickrisp-timestamp-availability
        uml_gpu_kosmickrisp_timestamp_availability_tests
        kosmickrisp_timestamp_availability_tests.cpp
        LIBRARIES UML::ThirdPartyKosmicKrisp
        VULKAN TIMEOUT 120)

    # A transfer that names an image or buffer with no Metal object, because it
    # was never bound or its bind was refused, is refused by the command, not
    # passed to Metal as nil, which ends the process; a refused bind logs the
    # reason once.
    uml_add_gpu_unit_test(gpu-kosmickrisp-unbound-transfers
        uml_gpu_kosmickrisp_unbound_transfer_tests
        kosmickrisp_unbound_transfer_tests.cpp
        LIBRARIES UML::ThirdPartyKosmicKrisp
        VULKAN TIMEOUT 120)

    # What a caller of the pipeline binary commands chooses is checked before
    # it is read, copied or dereferenced: the size of an imported key, the
    # size of its data, and which of the three sources a create request
    # names, and what chain a pipeline key request begins with. Each row runs in
    # a process of its own.
    uml_add_gpu_unit_test(gpu-kosmickrisp-pipeline-binary-requests
        uml_gpu_kosmickrisp_pipeline_binary_requests_tests
        kosmickrisp_pipeline_binary_requests_tests.cpp
        LIBRARIES UML::ThirdPartyKosmicKrisp
        VULKAN TIMEOUT 300)

    add_library(uml_gpu_vulkan_draw_test_support STATIC
        vulkan_draw_test.cpp)
    target_link_libraries(uml_gpu_vulkan_draw_test_support PUBLIC
        UML::ThirdPartyKosmicKrisp)
    target_include_directories(uml_gpu_vulkan_draw_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    uml_add_gpu_unit_test(gpu-kosmickrisp-line-multiview
        uml_gpu_kosmickrisp_line_multiview_tests
        kosmickrisp_line_multiview_tests.cpp
        LIBRARIES uml_gpu_vulkan_draw_test_support
        VULKAN)

    # The probe writes a JSON report of what KosmicKrisp advertises and what
    # its verifiers measure. Each capability test runs it and checks one
    # capability in the report.
    add_library(uml_gpu_kosmickrisp_capability_probe STATIC
        kosmickrisp_capability_probe.mm)
    target_compile_definitions(uml_gpu_kosmickrisp_capability_probe PRIVATE
        VK_ENABLE_BETA_EXTENSIONS=1)
    target_link_libraries(uml_gpu_kosmickrisp_capability_probe PRIVATE
        UML::ThirdPartyKosmicKrisp "-framework Foundation")
    add_dependencies(uml_gpu_kosmickrisp_capability_probe
        uml_dependency_prefix_verify)
    foreach(capability
            dynamic_logic_op device_address_commands graphics_pipeline_library
            legacy_vertex_attribute rgb32_texel non_seamless_cube
            incremental_present unused_attachments)
        string(REPLACE "_" "-" test_name "${capability}")
        uml_add_gpu_unit_test(gpu-kosmickrisp-${test_name}-capabilities
            uml_gpu_kosmickrisp_${capability}_capability_tests
            kosmickrisp_${capability}_capability_tests.cpp
            LIBRARIES uml_gpu_kosmickrisp_capability_probe
            VULKAN)
    endforeach()
    target_link_libraries(uml_gpu_kosmickrisp_rgb32_texel_capability_tests
        PRIVATE UML::ThirdPartyKosmicKrisp)

    uml_add_gpu_unit_test(gpu-kosmickrisp-legacy-vertex-input-draws
        uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests
        kosmickrisp_legacy_vertex_input_draw_tests.cpp
        LIBRARIES uml_gpu_vulkan_draw_test_support
        VULKAN)
    # The same draws with every render pipeline of a dynamic vertex-input
    # variant built again from the parent's NIR, the way one for a render pass
    # with unbound colour attachments is, because the variant freed its MSL.
    uml_add_unit_test(NAME gpu-kosmickrisp-legacy-vertex-input-regenerated-msl
        COMMAND uml_gpu_kosmickrisp_legacy_vertex_input_draw_tests
        LABELS gpu vulkan)
    set_property(TEST gpu-kosmickrisp-legacy-vertex-input-regenerated-msl
        APPEND PROPERTY ENVIRONMENT MESA_KK_REGENERATE_MSL=1)
endif()

if(TARGET uml_gpu_vulkan_draw_test_support)
    # Each <stem> draws through the shared harness: uml_gpu_<stem>_tests,
    # built from <stem>_tests.cpp, registers as gpu-<stem with dashes>.
    foreach(stem
            vulkan_device_address_commands kosmickrisp_geometry_invocations
            kosmickrisp_partial_position kosmickrisp_pipeline_statistics
            kosmickrisp_point_clipping kosmickrisp_tessellation_feedback
            kosmickrisp_raster_polygon
            kosmickrisp_raster_line kosmickrisp_raster_state
            kosmickrisp_geometry_side_effects kosmickrisp_provoking_unroll)
        string(REPLACE "_" "-" test_name "gpu-${stem}")
        uml_add_gpu_unit_test(${test_name} uml_gpu_${stem}_tests
            ${stem}_tests.cpp
            LIBRARIES uml_gpu_vulkan_draw_test_support
            VULKAN)
    endforeach()
    # The command storage guard counts Metal command allocators, so it is
    # Objective-C++ and links Metal.
    uml_add_gpu_unit_test(gpu-kosmickrisp-command-storage
        uml_gpu_kosmickrisp_command_storage_tests
        kosmickrisp_command_storage_tests.mm
        LIBRARIES uml_gpu_vulkan_draw_test_support
            "-framework Foundation" "-framework Metal"
        VULKAN)
    # The transform feedback pass guard counts the Metal command buffers and
    # encoders a chain of render passes costs, which is Objective-C++ as well.
    uml_add_gpu_unit_test(gpu-kosmickrisp-transform-feedback-passes
        uml_gpu_kosmickrisp_transform_feedback_passes_tests
        kosmickrisp_transform_feedback_passes_tests.mm
        LIBRARIES uml_gpu_vulkan_draw_test_support
            "-framework Foundation" "-framework Metal"
        VULKAN)
    foreach(stem geometry_invocations raster_line geometry_side_effects)
        target_link_libraries(uml_gpu_kosmickrisp_${stem}_tests PRIVATE
            Threads::Threads)
    endforeach()
endif()

if(TARGET uml_gpu_service AND UML_TARGET_MACOS)
    # A measurement run takes the iOS memory accounting of the segments that
    # virglrenderer shares with the host GPU stack by asking for container
    # files in place of POSIX shared memory.
    uml_add_gpu_unit_test(gpu-virgl-anon-file-backing
        uml_gpu_virgl_anon_file_tests virgl_anon_file_tests.cpp
        LIBRARIES UML::ThirdPartyVirglrenderer
        TIMEOUT 60)
endif()

if(TARGET uml_gpu_service)
    # A guest that destroys a framebuffer's attachment image view, or its
    # image, and then begins a render pass on that framebuffer must lose its
    # context, not crash the host in the driver's render-pass setup.
    uml_add_gpu_unit_test(gpu-venus-render-pass-lifetime
        uml_gpu_venus_render_pass_lifetime_tests
        venus_render_pass_lifetime_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        VULKAN UNIT_DIRECTORY TIMEOUT 120)

    # A bind the host driver refused is invisible to the guest, whose binds are
    # asynchronous. The guest's context must end there, and a copy that names
    # an image the driver never bound must be refused, not crash the host in
    # Metal's copy encoder.
    uml_add_gpu_unit_test(gpu-venus-bind-failure
        uml_gpu_venus_bind_failure_tests
        venus_bind_failure_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        VULKAN UNIT_DIRECTORY TIMEOUT 120)

    # The renderer serves no VK_KHR_pipeline_binary, which imports a binary under
    # a key and data the guest names and builds a pipeline from it: it does not
    # offer the extension, and every command of it, and every pipeline creation
    # that chains binaries, ends the context of a guest that enabled it by name
    # anyway. The rows of the renderer's object table that the binaries once
    # served as the vehicle of stay. The faults lie past the end of a heap block
    # or in freed memory, so the test runs each row in a child under Guard
    # Malloc, and the rows that name an object the renderer must free again under
    # the leak checker.
    uml_add_gpu_unit_test(gpu-venus-pipeline-binary-requests
        uml_gpu_venus_pipeline_binary_requests_tests
        venus_pipeline_binary_requests_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        VULKAN UNIT_DIRECTORY TIMEOUT 600)
    set_tests_properties(gpu-venus-pipeline-binary-requests PROPERTIES
        SKIP_RETURN_CODE 77)

    # A guest chooses the data of a pipeline cache, which the host driver parses
    # as if its own serializer had written it, so the renderer makes every cache
    # empty and answers a read of one with the blob of an empty cache. The
    # faults lie past the end of a heap block or at address zero, so the test
    # runs each row in a child under Guard Malloc.
    uml_add_gpu_unit_test(gpu-venus-pipeline-cache-bytes
        uml_gpu_venus_pipeline_cache_bytes_tests
        venus_pipeline_cache_bytes_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        VULKAN UNIT_DIRECTORY TIMEOUT 600)
    set_tests_properties(gpu-venus-pipeline-cache-bytes PROPERTIES
        SKIP_RETURN_CODE 77)

    # A handle the specification does not allow to be VK_NULL_HANDLE reaches its
    # handler as a null pointer when the guest sends object id 0, because the
    # codec refuses id 0 only for the first argument of a command; a shader
    # stage that names no shader or entry point reaches the runtime's hash, and
    # a host image copy names a mip level, aspect, region and row length the
    # image does not have. Each is refused and the host lives on, and a request
    # the specification allows is served. The faults lie past the end of a
    # heap block, so the test runs each row in a child under Guard Malloc.
    uml_add_gpu_unit_test(gpu-venus-required-handles
        uml_gpu_venus_required_handles_tests
        venus_required_handles_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        VULKAN UNIT_DIRECTORY TIMEOUT 900)
    set_tests_properties(gpu-venus-required-handles PROPERTIES
        SKIP_RETURN_CODE 77)

    # A blob larger than the shared memory behind it is refused where it is
    # created, for Venus memory (POSIX shared memory and the unlinked files iOS
    # uses) and for a coherent GL buffer. The render server is a thread of the
    # host, so a resource it kept past the end of its file ends the host, and a
    # refused blob leaves no mapping behind.
    uml_add_gpu_unit_test(gpu-blob-size-bounds
        uml_gpu_blob_size_bounds_tests
        blob_size_bounds_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        VULKAN UNIT_DIRECTORY TIMEOUT 240)
    # The GL case is the same binary's gl-coherent-buffer mode, run by itself:
    # a renderer without pinned host memory skips it with status 77.
    uml_add_unit_test(NAME gpu-blob-size-bounds-gl
        COMMAND uml_gpu_blob_size_bounds_tests gl-coherent-buffer
        LABELS gpu
        WORKING_DIRECTORY "${_uml_unit_binary_directory}"
        TIMEOUT 120)
    set_tests_properties(gpu-blob-size-bounds PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(gpu-blob-size-bounds-gl PROPERTIES SKIP_RETURN_CODE 77)

    uml_add_gpu_unit_test(gpu-host-gl-provider-lifetime
        uml_gpu_host_gl_provider_tests host_gl_provider_lifetime_tests.cpp
        LIBRARIES uml_gpu_renderer UML::ThirdPartyVirglrenderer
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The host GL provider has to be able to alias memory the caller already
    # owns as a sampled texture; the guest scanout import depends on it.
    uml_add_gpu_unit_test(gpu-host-pointer-image
        uml_gpu_host_pointer_image_tests host_gl_host_pointer_image_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The renderer reports the host memory the drivers hold by import, which
    # Metal counts in full and the device memory pool must not charge.
    uml_add_gpu_unit_test(gpu-host-imported-memory
        uml_gpu_host_imported_memory_tests host_gl_imported_memory_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # An idle host GL context must give back what its last batches held when
    # the renderer tells the driver it has gone idle (EGL_MESA_context_trim).
    uml_add_gpu_unit_test(gpu-host-context-trim
        uml_gpu_host_context_trim_tests host_gl_context_trim_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY
        TIMEOUT 120)

    # The renderer's own context (context 0) is trimmed like a guest context:
    # textures a guest made and deleted without naming a context give their
    # memory back within a few idle intervals.
    uml_add_gpu_unit_test(gpu-host-context0-trim
        uml_gpu_host_context0_trim_tests host_gl_context0_trim_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY
        TIMEOUT 120)

    # Contexts that draw, flush and trim at once on one driver screen. It is
    # also the program for a ThreadSanitizer pass over the trim and the flush
    # queue's reset of the screen's batch-state lists: build the Zink and EGL
    # stack with -Db_sanitize=thread and compile this test and
    # host_gl_provider.cpp with -fsanitize=thread. Zink's own unsynchronized
    # scalars (the screen's last finished batch, a state's submit count, the
    # context's state count) are reported there with or without a trim; no
    # report involves a state list or a state's link.
    uml_add_gpu_unit_test(gpu-host-context-trim-concurrent
        uml_gpu_host_context_trim_concurrent_tests
        host_gl_context_trim_concurrent_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} Threads::Threads
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY
        TIMEOUT 180)

    # A context the renderer trims right after it flushed, and that another
    # context then replaces on its thread, in a share group that holds an EGL
    # image: the path on which the threaded context's fence check once read the
    # driver thread's stores out of order. It keeps that path exercised under
    # load; Mesa's util test u_queue_fence_order_test discriminates the fix.
    uml_add_gpu_unit_test(gpu-host-gl-trim-unbind
        uml_gpu_host_gl_trim_unbind_tests
        host_gl_trim_unbind_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} Threads::Threads
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY
        TIMEOUT 120)

    # A context that draws a framebuffer-fetch fragment shader another context
    # created, and that the screen's shader cache served, must still have the
    # descriptor binding the shader reads. The read is past the end of a heap
    # block, so the test runs its body in a child under Guard Malloc.
    uml_add_gpu_unit_test(gpu-host-gl-fbfetch-shader-cache
        uml_gpu_host_gl_fbfetch_shader_cache_tests
        host_gl_fbfetch_shader_cache_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY
        TIMEOUT 120)

    # A transfer a context refuses must not leave that context refusing every
    # later transfer: Android's gralloc mapper only ever transfers.
    uml_add_gpu_unit_test(gpu-transfer-context-error
        uml_gpu_transfer_context_error_tests
        host_gl_transfer_context_error_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A guest buffer with padded rows is locked and unlocked in one transfer
    # that names the pitch as its stride, rather than one transfer per row,
    # which costs one readback per row: the rows must land at the pitch.
    uml_add_gpu_unit_test(gpu-padded-row-transfer
        uml_gpu_padded_row_transfer_tests
        host_gl_padded_row_transfer_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A blob texture has no guest pages, so a guest reads and writes it through
    # a staging buffer: the copy transfer must check the staging buffer for
    # guest storage in both directions, not the texture.
    uml_add_gpu_unit_test(gpu-blob-copy-transfer
        uml_gpu_blob_copy_transfer_tests
        host_gl_blob_copy_transfer_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A new texture reads as zeros, not as the pixels of the resource that
    # released its memory: the guest allocated zeroed pages, and the leftover
    # pixels may belong to another process.
    uml_add_gpu_unit_test(gpu-texture-zero-fill
        uml_gpu_texture_zero_fill_tests
        host_gl_texture_zero_fill_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A new multisampled texture, which the renderer's clear reaches, and a
    # new compressed texture, which no clear reaches, show nothing a released
    # one held.
    uml_add_gpu_unit_test(gpu-texture-zero-fill-special
        uml_gpu_texture_zero_fill_special_tests
        host_gl_texture_zero_fill_special_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The zero fill of a new compressed texture has several steps that the
    # host driver can refuse room for. A refused step fails the creation with
    # ENOMEM, which the renderer retries after giving back what it can, and
    # no texture is left partly written.
    uml_add_gpu_unit_test(gpu-compressed-zero-refusal
        uml_gpu_compressed_zero_refusal_tests
        host_gl_compressed_zero_refusal_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A gralloc BLOB buffer no GPU unit uses, such as a camera's JPEG output,
    # is created at any size within the renderer's bound without host storage,
    # keeps the bytes the guest wrote through the transfers, and is no resource
    # a command can name.
    uml_add_gpu_unit_test(gpu-hostless-buffer
        uml_gpu_hostless_buffer_tests
        host_gl_hostless_buffer_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The OpenGL path stays within the host GPU's fixed budget: a texture past
    # it is refused at creation with ENOMEM, the host never holds more than the
    # budget, and the renderer stores new textures once the guest has released
    # the old ones.
    uml_add_gpu_unit_test(gpu-host-gl-device-memory-budget
        uml_gpu_host_gl_device_memory_budget_tests
        host_gl_device_memory_budget_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # The budget holds a burst of creations with no work between them while
    # the graphics ledger leads Metal's counter: a heap the driver made
    # counts in both figures before its reservation ends, so the burst is
    # refused at the budget and nothing passes it once work is submitted.
    uml_add_gpu_unit_test(gpu-host-gl-allocation-burst
        uml_gpu_host_gl_allocation_burst_tests
        host_gl_allocation_burst_tests.mm
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
            "-framework Foundation" "-framework Metal"
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # The budget counts an IOSurface, which only Metal's counter holds, and
    # the driver's command storage, which only the graphics ledger holds,
    # both, and admits no texture into the room they take.
    uml_add_gpu_unit_test(gpu-host-gl-surface-and-command-storage
        uml_gpu_host_gl_surface_and_command_storage_tests
        host_gl_surface_and_command_storage_tests.mm
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
            "-framework Foundation" "-framework Metal" "-framework IOSurface"
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # A creation the budget refuses only because a quiet guest context still
    # holds its deleted textures is not refused: the renderer finishes and
    # trims every context, then creates the resource again. A real overcommit
    # still fails with ENOMEM, in bounded time.
    uml_add_gpu_unit_test(gpu-host-gl-reclaim-retry
        uml_gpu_host_gl_reclaim_retry_tests
        host_gl_reclaim_retry_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 240)

    # A draw whose upload memory the budget refuses is skipped with
    # GL_OUT_OF_MEMORY; the host does not copy into a buffer it never got.
    uml_add_gpu_unit_test(gpu-host-gl-upload-refusal
        uml_gpu_host_gl_upload_refusal_tests
        host_gl_upload_refusal_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 240)

    # A draw's upload, a texture's staged upload or a buffer's update that the
    # budget refuses is made again once after the host gave back what it
    # could: they are not lost for memory that finished work holds, a refusal
    # leaves nothing behind, and an upload no memory can make room for is
    # dropped without harm.
    foreach(_shape draw sustained upload buffer persist cycle)
        uml_add_gpu_unit_test(gpu-host-gl-refusal-retry-${_shape}
            uml_gpu_host_gl_refusal_retry_${_shape}_tests
            host_gl_refusal_retry_tests.cpp
            LIBRARIES ${_uml_host_gl_libraries}
            INCLUDES ${_uml_gpu_device_directory}
            UNIT_DIRECTORY TIMEOUT 240)
    endforeach()
    foreach(_shape sustained upload buffer persist cycle)
        string(TOUPPER ${_shape} _define)
        target_compile_definitions(uml_gpu_host_gl_refusal_retry_${_shape}_tests
            PRIVATE UML_REFUSAL_RETRY_${_define}=1)
    endforeach()

    # A texture shared with another context is written into its segment by a
    # read-back; one the full budget refuses is tried again at the next fence.
    uml_add_gpu_unit_test(gpu-host-gl-shm-export-refusal
        uml_gpu_host_gl_shm_export_refusal_tests
        host_gl_shm_export_refusal_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 240)

    # glthread's shared upload buffer is refused with the rest of the budget;
    # a glBufferSubData that meets the refusal neither faults the host nor
    # blocks uploads once the memory is back.
    uml_add_gpu_unit_test(gpu-host-gl-glthread-upload-refusal
        uml_gpu_host_gl_glthread_upload_refusal_tests
        host_gl_glthread_upload_refusal_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 240)

    # The command storage a destroyed context's batches grew to is released
    # with it, not kept with the batch states the screen holds for later.
    uml_add_gpu_unit_test(gpu-host-gl-batch-storage
        uml_gpu_host_gl_batch_storage_tests
        host_gl_batch_storage_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # What the driver caches for a context that is idle or destroyed goes
    # back while other contexts go on drawing.
    uml_add_gpu_unit_test(gpu-host-gl-idle-release
        uml_gpu_host_gl_idle_release_tests
        host_gl_idle_release_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # A resource the guest makes inside a command buffer, which the host
    # refuses for want of memory, is told to the guest by the blob that names
    # it: out of memory, once, and not as an id the host never saw.
    uml_add_gpu_unit_test(gpu-host-gl-refused-blob
        uml_gpu_host_gl_refused_blob_tests
        host_gl_refused_blob_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # The device memory pool frees the host GL driver's cache before it
    # refuses a reservation: the driver releases it for any thread, with no
    # context current (EGL_MESA_context_trim).
    uml_add_gpu_unit_test(gpu-host-cached-memory-release
        uml_gpu_host_cached_memory_release_tests
        host_gl_cached_memory_release_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # A YUV picture a guest keeps in one R8 image samples as its planes: a
    # decoder that plays into a GL or SurfaceView view would show stripes
    # otherwise.
    uml_add_gpu_unit_test(gpu-packed-yuv-planes
        uml_gpu_packed_yuv_planes_tests
        host_gl_packed_yuv_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A draw that samples the texture it renders into reads what the guest drew
    # before the texture barrier, a clear included: Skia's blends of a surface it
    # has just cleared were wrong otherwise.
    uml_add_gpu_unit_test(gpu-texture-barrier-clear
        uml_gpu_texture_barrier_tests
        host_gl_texture_barrier_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The scanout mask of the capset names what the display presents: minigbm
    # drops the video uses of a buffer entry that claims a scanout it cannot
    # have.
    uml_add_gpu_unit_test(gpu-scanout-caps
        uml_gpu_scanout_caps_tests
        host_gl_scanout_caps_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A layout the guest names has to fit the host storage behind it.
    uml_add_gpu_unit_test(gpu-import-bounds
        uml_gpu_import_bounds_tests host_gl_import_bounds_tests.mm
        LIBRARIES ${_uml_host_gl_libraries}
            "-framework Foundation" "-framework Metal"
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # The native display samples the Metal texture that stores a Virgl
    # scanout's GL texture, so the renderer must name the right one: its size,
    # format, orientation, device, and fenced contents.
    uml_add_gpu_unit_test(gpu-resource-metal-texture
        uml_gpu_resource_metal_texture_tests
        host_gl_resource_metal_texture_tests.mm
        LIBRARIES ${_uml_host_gl_libraries}
            "-framework Foundation" "-framework Metal"
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # One thread drives every host GL context, so a read in one context of a
    # texture another context left unsubmitted has to finish without a second
    # thread. Blocking there parks the GPU thread and wedges the whole guest.
    uml_add_gpu_unit_test(gpu-cross-context-readback
        uml_gpu_cross_context_readback_tests
        host_gl_cross_context_readback_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # Virglrenderer advertises depth formats the guest turns into default
    # framebuffers. A host that substitutes different storage for a sized depth
    # format leaves an OpenGL ES guest unable to name its own depth buffer.
    uml_add_gpu_unit_test(gpu-depth-format-advertisement
        uml_gpu_depth_format_advertisement_tests
        host_gl_depth_format_advertisement_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A guest that is not offered a compressed format it exposes decodes the
    # texture and stores four or more times its bytes; each format the host
    # samples is offered, decodes right, copies by its class and is stored
    # compressed.
    uml_add_gpu_unit_test(gpu-compressed-formats
        uml_gpu_compressed_formats_tests
        host_gl_compressed_formats_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries} UML::HostGpuMemory
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY TIMEOUT 120)

    # A stencil buffer needs no depth plane beside it: Android's hardware
    # renderer asks for a stencil of eight bits and no depth for every window,
    # and the renderer used to give it a depth-stencil image.
    uml_add_gpu_unit_test(gpu-stencil-only-target
        uml_gpu_stencil_only_target_tests
        host_gl_stencil_only_target_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # GL_VERTICES_SUBMITTED_ARB counts submitted vertices, not assembled ones.
    # Zink does not advertise LINE_LOOP, so Mesa decomposes every line loop
    # into independent lines before the driver sees it, doubling the count
    # unless the driver subtracts the conversion back out. A clipping query
    # active beside it, which Zink begins as a second Vulkan statistics query,
    # must still count the loop's clipping input primitives.
    uml_add_gpu_unit_test(gpu-line-loop-statistics
        uml_gpu_line_loop_statistics_tests
        host_gl_line_loop_statistics_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # Zink keeps the first result it reads when a poll finds a statistics
    # query available, so the host driver must not publish availability
    # before the counts.
    uml_add_gpu_unit_test(gpu-statistics-availability
        uml_gpu_statistics_availability_tests
        host_gl_statistics_availability_tests.cpp
        LIBRARIES ${_uml_host_gl_libraries}
        INCLUDES ${_uml_gpu_device_directory}
        UNIT_DIRECTORY)

    # A host without a usable GL stack or renderer skips these with status 77.
    set_tests_properties(
        gpu-transfer-context-error gpu-blob-copy-transfer
        gpu-texture-zero-fill gpu-texture-zero-fill-special
        gpu-compressed-zero-refusal gpu-hostless-buffer gpu-scanout-caps
        gpu-import-bounds
        gpu-resource-metal-texture gpu-cross-context-readback
        gpu-depth-format-advertisement gpu-stencil-only-target
        gpu-line-loop-statistics gpu-statistics-availability
        gpu-packed-yuv-planes gpu-texture-barrier-clear gpu-padded-row-transfer
        gpu-host-context-trim gpu-host-context-trim-concurrent
        gpu-host-gl-trim-unbind gpu-host-gl-fbfetch-shader-cache
        gpu-host-gl-device-memory-budget gpu-host-gl-allocation-burst
        gpu-host-gl-surface-and-command-storage
        gpu-host-gl-reclaim-retry gpu-host-imported-memory
        gpu-host-gl-shm-export-refusal
        gpu-host-gl-refusal-retry-draw gpu-host-gl-refusal-retry-sustained
        gpu-host-gl-refusal-retry-upload gpu-host-gl-refusal-retry-buffer
        gpu-host-gl-refusal-retry-persist gpu-host-gl-refusal-retry-cycle
        PROPERTIES SKIP_RETURN_CODE 77)

    # The renderer and the same-process render server share one fence table.
    # Exercise that ownership without a device: it is a pure lifetime
    # contract, and losing it faults the GPU thread.
    uml_add_gpu_unit_test(gpu-virgl-fence-table-ownership
        uml_gpu_virgl_fence_table_tests virgl_fence_table_ownership_tests.cpp
        LIBRARIES UML::ThirdPartyVirglrenderer
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-transport-wire-ownership
        uml_gpu_transport_wire_tests
        vhost_user_message_tests.cpp
        LIBRARIES
            uml_gpu_transport_wire_stop_wait_test_variant
            uml_test_allocation_failure
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    # Its own executable: the descriptor ceiling is process state, and filling
    # the file table would decide the outcome of anything sharing the process.
    uml_add_gpu_unit_test(gpu-transport-descriptor-ceiling
        uml_gpu_transport_ceiling_tests
        vhost_user_descriptor_ceiling_tests.cpp
        LIBRARIES uml_vhost_user_wire
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-service-fd-ownership
        uml_gpu_service_fd_tests
        gpu_lifetime_tests.cpp
        in_process_service_testing.cpp
        in_process_service_lifetime_tests.cpp
        vhost_user_protocol_tests.cpp
        vhost_user_server_lifetime_tests.cpp
        LIBRARIES uml_gpu_service_test_variant
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    # The display outlives the guest's GPU connection: a VNC client stays
    # connected, and keeps its input, while the connection is replaced.
    uml_add_gpu_unit_test(gpu-display-outlives-connection
        uml_gpu_display_outlives_connection_tests
        display_outlives_connection_tests.cpp
        LIBRARIES uml_gpu_service
        INCLUDES
            ${_uml_gpu_device_directory}
            ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
            ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)
    # A host without a usable renderer skips it with status 77.
    set_tests_properties(gpu-display-outlives-connection
        PROPERTIES SKIP_RETURN_CODE 77)

    # The frame counters of the shipped service's display, read through the
    # service. A host without a usable renderer skips it with status 77.
    uml_add_gpu_unit_test(gpu-display-statistics
        uml_gpu_display_statistics_tests
        in_process_service_statistics_tests.cpp
        LIBRARIES uml_gpu_service
        INCLUDES
            ${_uml_gpu_device_directory}
            ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
            ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)
    set_tests_properties(gpu-display-statistics
        PROPERTIES SKIP_RETURN_CODE 77)

    uml_add_gpu_unit_test(gpu-service-stop-wake
        uml_gpu_service_stop_wake_tests
        in_process_service_stop_wake_tests.cpp
        in_process_service_testing.cpp
        LIBRARIES uml_gpu_service_slow_stop_test_variant
        INCLUDES
            ${_uml_gpu_device_directory}
            ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
            ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    # The private memory report: the device with a renderer that has a budget.
    uml_add_gpu_unit_test(gpu-device-memory-report
        uml_gpu_device_memory_report_tests
        virtio_gpu_device_memory_report_tests.cpp
        LIBRARIES uml_gpu_protocol_device_test_variant
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-device-command-atomicity
        uml_gpu_device_atomicity_tests
        virtio_gpu_device_lifetime_tests.cpp
        LIBRARIES
            uml_gpu_protocol_device_test_variant
            uml_test_allocation_failure
            UML::PublishedFigures
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)

    # A guest's suspend with a fenced command still on the host: the device
    # keeps its context, and a driver that ends resets it.
    uml_add_gpu_unit_test(gpu-device-suspend-held-fence
        uml_gpu_device_suspend_tests
        virtio_gpu_suspend_tests.cpp
        LIBRARIES uml_gpu_server_device_test_variant
        INCLUDES ${_uml_gpu_device_directory} ${CMAKE_CURRENT_SOURCE_DIR}
        UNIT_DIRECTORY)
endif()

if(TARGET uml_gpu_display)
    # The decisions of the 120 Hz display link: a state machine, no clock.
    uml_add_gpu_unit_test(gpu-present-pacer
        uml_gpu_present_pacer_tests present_pacer_tests.cpp
        LIBRARIES uml_gpu_display
        UNIT_DIRECTORY)

    # The sizes whose 120 Hz mode the EDID can state, against the table that
    # the application's offer reads too (the fixture's path is the test's
    # argument, so the file has one copy).
    add_executable(uml_gpu_edid_timing_tests edid_timing_tests.cpp)
    target_link_libraries(uml_gpu_edid_timing_tests PRIVATE uml_gpu_display)
    uml_add_unit_test(NAME gpu-edid-timing
        COMMAND uml_gpu_edid_timing_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/edid-120hz-sizes.txt
        LABELS gpu)

    # The resize path's objects come from the shipped display library.
    uml_add_gpu_unit_test(gpu-display-resize
        uml_gpu_display_resize_tests display_resize_tests.cpp
        LIBRARIES uml_gpu_display uml_test_allocation_failure
        UNIT_DIRECTORY)

    uml_add_gpu_unit_test(gpu-vnc-lifetime
        uml_gpu_vnc_lifetime_tests
        vnc_display_lifetime_tests.cpp
        vnc_display_metal_testing.mm
        vnc_display_testing.cpp
        LIBRARIES uml_gpu_display_test_variant
            "-framework Foundation" "-framework Metal"
        UNIT_DIRECTORY)

    # This one links the shipped display library rather than a test variant,
    # so it also answers whether the product was built with the native Metal
    # readback that a Venus scanout has no alternative to.
    if(UML_TARGET_MACOS)
        uml_add_gpu_unit_test(gpu-vnc-product-readback
            uml_gpu_vnc_product_tests vnc_display_product_tests.mm
            LIBRARIES uml_gpu_display
                "-framework Foundation" "-framework Metal"
            UNIT_DIRECTORY)

        # The shipped display must return when the guest finishes while a
        # menu is open. It needs a window server session and skips without
        # one.
        uml_add_gpu_unit_test(gpu-native-display-menu-teardown
            uml_gpu_native_display_menu_teardown_tests
            native_display_menu_teardown_tests.mm
            LIBRARIES uml_gpu_display
                "-framework AppKit" "-framework CoreGraphics"
            INCLUDES ${_uml_gpu_device_directory}
            UNIT_DIRECTORY TIMEOUT 90)
        set_tests_properties(gpu-native-display-menu-teardown PROPERTIES
            SKIP_RETURN_CODE 77)

        # The native presenter must scale a scanout whose size or rectangle
        # differs from its framebuffer. It draws into an offscreen view and
        # skips on a host without a Metal device or offscreen drawables.
        uml_add_gpu_unit_test(gpu-native-metal-presentation
            uml_gpu_metal_presentation_tests metal_presentation_tests.mm
            LIBRARIES uml_gpu_display
                "-framework AppKit" "-framework CoreVideo"
                "-framework Metal" "-framework MetalKit"
                "-framework QuartzCore"
            INCLUDES ${_uml_gpu_device_directory}
            UNIT_DIRECTORY TIMEOUT 90)
        set_tests_properties(gpu-native-metal-presentation PROPERTIES
            SKIP_RETURN_CODE 77)

        # The windowless display (DisplayMode::offscreen) that measurement
        # runs use for the memory a view and Picture in Picture take on an
        # iPad. It skips on a host without a Metal device or without
        # drawables for a layer outside any window.
        uml_add_gpu_unit_test(gpu-offscreen-display
            uml_gpu_offscreen_display_tests offscreen_display_tests.mm
            LIBRARIES uml_gpu_display "-framework Metal"
            INCLUDES ${_uml_gpu_device_directory}
            UNIT_DIRECTORY TIMEOUT 120)
        set_tests_properties(gpu-offscreen-display PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
endif()

unset(_uml_unit_binary_directory)
unset(_uml_gpu_device_directory)
unset(_uml_host_gl_libraries)
