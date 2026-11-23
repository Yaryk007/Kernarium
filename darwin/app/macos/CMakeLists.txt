# SPDX-License-Identifier: GPL-2.0-or-later

# Every macOS executable shares one bundle identifier. Metal keeps its shader
# cache below the process's bundle identifier, and a process without one shares
# the cache of every other command-line process of the user. The Linux and the
# Android executables compile the same Mesa and KosmicKrisp shaders, so they
# share a cache as well.
set(UML_MACOS_APP_BUNDLE_IDENTIFIER "com.imxieyi.uml-darwin")

# Every launcher loads the Apple GPU frameworks from the one Frameworks
# directory beside it, so one target stages that directory and each launcher
# depends on it. A staging step in each launcher's own post-build replaced the
# directory while the other's was copying into or checking it, and the build
# of both launchers in one invocation failed. The stamp is outside the
# directory, which the runtime closure check requires to hold the frameworks
# and nothing else.
function(uml_stage_macos_gpu_frameworks)
    if(TARGET uml_macos_gpu_frameworks)
        return()
    endif()
    set(_uml_frameworks_dir "${PROJECT_BINARY_DIR}/bin/Frameworks")
    set(_uml_frameworks_stamp "${PROJECT_BINARY_DIR}/bin/Frameworks.staged")
    set(_uml_stage_inputs)
    set(_uml_stage_commands)
    foreach(_uml_gl_framework IN LISTS UML_APPLE_GPU_GL_FRAMEWORK_NAMES)
        list(APPEND _uml_stage_inputs
            "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework/${_uml_gl_framework}"
            "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework/Info.plist")
        list(APPEND _uml_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework
                ${_uml_frameworks_dir}/${_uml_gl_framework}.framework)
    endforeach()
    if(TARGET uml_third_party_kosmickrisp)
        list(APPEND _uml_stage_inputs
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/KosmicKrisp"
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/Info.plist")
        list(APPEND _uml_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}
                ${_uml_frameworks_dir}/KosmicKrisp.framework)
    endif()
    get_property(_uml_aot_fw_dir GLOBAL PROPERTY
        UML_AOT_FRAMEWORK_DIR)
    get_property(_uml_aot_fw_target GLOBAL PROPERTY
        UML_AOT_FRAMEWORK_TARGET)
    if(_uml_aot_fw_dir)
        list(APPEND _uml_stage_inputs
            "$<TARGET_FILE:${_uml_aot_fw_target}>"
            "${_uml_aot_fw_dir}/Info.plist")
        list(APPEND _uml_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_uml_aot_fw_dir}"
                "${_uml_frameworks_dir}/UmlArm64Aot.framework")
    endif()
    add_custom_command(OUTPUT "${_uml_frameworks_stamp}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_uml_frameworks_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_uml_frameworks_dir}
        ${_uml_stage_commands}
        COMMAND ${CMAKE_COMMAND} -E touch "${_uml_frameworks_stamp}"
        DEPENDS ${_uml_stage_inputs}
        COMMENT "Staging the Apple GPU frameworks"
        VERBATIM)
    add_custom_target(uml_macos_gpu_frameworks DEPENDS "${_uml_frameworks_stamp}")
    uml_declare_architecture_target(uml_macos_gpu_frameworks app)
    add_dependencies(uml_macos_gpu_frameworks uml_dependency_prefix_verify)
    if(_uml_aot_fw_target)
        add_dependencies(uml_macos_gpu_frameworks "${_uml_aot_fw_target}")
    endif()
endfunction()

# uml_add_macos_embedded_kernel_app(<target> <main source> <kernel artifact>
#     [NETWORK_PROVIDER <target>] [TEST_ONLY] [EDGE_TEST])
#
# NETWORK_PROVIDER names the user network the launcher links instead of the
# product's. TEST_ONLY makes a launcher for the tests that is no product: it is
# no macOS app target of the identity checks and has no dSYM. EDGE_TEST (a
# TEST_ONLY launcher only) compiles in the pipe that plays the application's
# leaving the foreground to the synthetic camera's run
# (edge_test/background_edge_control.hpp); no product launcher compiles it.
function(uml_add_macos_embedded_kernel_app
        TARGET_NAME MAIN_SOURCE ARTIFACT_TARGET)
    cmake_parse_arguments(_uml_app "TEST_ONLY;EDGE_TEST" "NETWORK_PROVIDER" "" ${ARGN})
    if(_uml_app_EDGE_TEST AND NOT _uml_app_TEST_ONLY)
        message(FATAL_ERROR "${TARGET_NAME}: EDGE_TEST needs TEST_ONLY")
    endif()
    if(NOT TARGET "${ARTIFACT_TARGET}")
        message(FATAL_ERROR
            "${TARGET_NAME} requires the artifact target ${ARTIFACT_TARGET}")
    endif()
    get_target_property(_uml_kernel_image "${ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
    if(NOT _uml_kernel_image)
        message(FATAL_ERROR "${ARTIFACT_TARGET} has no kernel ELF image")
    endif()

    # The loader aliases the ELF's executable pages from the section this
    # links in, as Kernarium's runtime does, so the image has to be safe for
    # that.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py"
            --image "${_uml_kernel_image}"
        RESULT_VARIABLE _uml_carrier_result
        OUTPUT_VARIABLE _uml_carrier_output
        ERROR_VARIABLE _uml_carrier_error)
    if(NOT _uml_carrier_result EQUAL 0)
        message(FATAL_ERROR
            "${TARGET_NAME}: the kernel is unsafe for the signed carrier:\n"
            "${_uml_carrier_output}${_uml_carrier_error}")
    endif()

    add_executable("${TARGET_NAME}"
        launcher.hpp
        main.cpp
        "${MAIN_SOURCE}")
    if(_uml_app_EDGE_TEST)
        target_sources("${TARGET_NAME}" PRIVATE
            edge_test/background_edge_control.hpp)
    endif()
    uml_declare_architecture_target("${TARGET_NAME}" app)
    target_include_directories("${TARGET_NAME}" PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/emulator/src
        ${PROJECT_SOURCE_DIR}/darwin/device)
    add_dependencies("${TARGET_NAME}"
        "${ARTIFACT_TARGET}"
        uml_dependency_prefix_verify)
    set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
        "${_uml_kernel_image}"
        "${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh"
        "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv")
    uml_stage_macos_gpu_frameworks()
    add_dependencies("${TARGET_NAME}" uml_macos_gpu_frameworks)
    foreach(_uml_gl_framework IN LISTS UML_APPLE_GPU_GL_FRAMEWORK_NAMES)
        set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
            "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework/${_uml_gl_framework}"
            "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework/Info.plist")
    endforeach()
    if(TARGET uml_third_party_kosmickrisp)
        set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/KosmicKrisp"
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/Info.plist")
    endif()
    get_property(_uml_aot_fw_dir GLOBAL PROPERTY
        UML_AOT_FRAMEWORK_DIR)
    get_property(_uml_aot_fw_target GLOBAL PROPERTY
        UML_AOT_FRAMEWORK_TARGET)
    set(_uml_verify_local_fw_args)
    if(_uml_aot_fw_dir)
        set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
            "$<TARGET_FILE:${_uml_aot_fw_target}>"
            "${_uml_aot_fw_dir}/Info.plist")
        set(_uml_verify_local_fw_args
            --local-framework "${_uml_aot_fw_dir}")
    endif()
    target_link_libraries("${TARGET_NAME}" PRIVATE
        UML::Emulator
        UML::AudioDevice
        UML::BatteryDevice
        UML::VideoDevice
        UML::CameraDevice
        UML::CameraSource
        UML::ConsoleDevice
        UML::VsockDevice
        UML::GnssDevice
        UML::GnssSyntheticSource
        UML::GpuDevice
        UML::GpuMetalShaderCache
        UML::GpuMesaShaderCache
        UML::VirtioInput)
    if(_uml_app_NETWORK_PROVIDER)
        target_link_libraries("${TARGET_NAME}" PRIVATE
            "${_uml_app_NETWORK_PROVIDER}")
    elseif(TARGET uml_network_provider)
        target_link_libraries("${TARGET_NAME}" PRIVATE UML::NetworkProvider)
    endif()
    target_compile_definitions("${TARGET_NAME}" PRIVATE
        UML_APP_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>
        UML_APP_EDGE_TEST=$<BOOL:${_uml_app_EDGE_TEST}>)
    # The executable carries its property list in __TEXT,__info_plist, where
    # CoreFoundation reads the identifier of a process that is not an
    # application bundle.
    set(UML_MACOS_APP_NAME "${TARGET_NAME}")
    set(_uml_info_plist "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}-Info.plist")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in" "${_uml_info_plist}"
        @ONLY)
    set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
        "${_uml_info_plist}")
    if(NOT _uml_app_TEST_ONLY)
        set_property(GLOBAL APPEND PROPERTY UML_MACOS_APP_TARGETS
            "${TARGET_NAME}")
    endif()
    target_link_options("${TARGET_NAME}" PRIVATE
        "LINKER:-export_dynamic"
        # The embedded ELF resolves this fixed-arity ABI with dlsym(). These
        # symbols have no native Mach-O callers, so force-load the deliberately
        # aggregated emulator product at the application boundary.
        "LINKER:-force_load,$<TARGET_FILE:uml_emulator>"
        # The kernel ELF is a read-only, host-page aligned __TEXT section:
        # the emulator runs it in place instead of copying it into the heap,
        # and the loader aliases its executable pages from it.
        "LINKER:-sectcreate,__TEXT,__uml_elf,${_uml_kernel_image}"
        "LINKER:-sectalign,__TEXT,__uml_elf,0x4000"
        "LINKER:-sectcreate,__TEXT,__info_plist,${_uml_info_plist}")

    # `<name>-dsym` writes the executable's dSYM beside it, with the kernel's
    # function symbols at their place in __TEXT,__uml_elf, where the runtime
    # moves a panic's kernel frames (runtime/kernel_crash_frames). It is not
    # part of the default build.
    if(NOT _uml_app_TEST_ONLY)
        add_custom_target("${TARGET_NAME}-dsym"
            COMMAND xcrun dsymutil "$<TARGET_FILE:${TARGET_NAME}>"
                -o "$<TARGET_FILE:${TARGET_NAME}>.dSYM"
            COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/scripts/kernel-dsym-symbols.py"
                --dsym "$<TARGET_FILE:${TARGET_NAME}>.dSYM/Contents/Resources/DWARF/$<TARGET_FILE_NAME:${TARGET_NAME}>"
                --elf "${_uml_kernel_image}"
            VERBATIM)
        uml_declare_architecture_target("${TARGET_NAME}-dsym" app)
        add_dependencies("${TARGET_NAME}-dsym" "${TARGET_NAME}")
    endif()

    set_target_properties("${TARGET_NAME}" PROPERTIES
        UML_BUNDLE_IDENTIFIER "${UML_MACOS_APP_BUNDLE_IDENTIFIER}"
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    add_custom_command(TARGET "${TARGET_NAME}" POST_BUILD
        COMMAND ${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh
            --binary $<TARGET_FILE:${TARGET_NAME}>
            --apple-gpu-prefix ${UML_APPLE_GPU_DEPENDENCY_PREFIX}
            --framework-directory $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
            ${_uml_verify_local_fw_args}
        COMMENT "Checking the ${TARGET_NAME} Apple GPU runtime closure"
        VERBATIM)
endfunction()

# Linux guests boot the accepted base archive for their userspace architecture
# plus a per-run archive the launcher writes; the Android launcher boots
# Android's own ramdisk and embeds neither.
function(uml_link_linux_boot_files TARGET_NAME)
    include("${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk/BootBase.cmake")
    if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
        uml_select_boot_base(x86_64 _uml_boot_base)
    else()
        uml_select_boot_base(aarch64 _uml_boot_base)
    endif()
    set(_uml_boot_init "${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk/init")
    target_link_libraries("${TARGET_NAME}" PRIVATE UML::BootInitramfs)
    target_link_options("${TARGET_NAME}" PRIVATE
        "LINKER:-sectcreate,__TEXT,__uml_boot_base,${_uml_boot_base}"
        "LINKER:-sectcreate,__TEXT,__uml_boot_init,${_uml_boot_init}")
    set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
        "${_uml_boot_base}" "${_uml_boot_init}")
endfunction()

if(UML_BUILD_CLI)
    uml_add_macos_embedded_kernel_app(
        uml-darwin linux_main.cpp "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    uml_link_linux_boot_files(uml-darwin)
    # The Linux launcher over the provider's test build, for the one
    # integration test whose guest connects to a host listener. The guest can
    # reach none through the gateway, so the test names a guest forward to a
    # Unix socket in UML_TEST_SLIRP_GUEST_FORWARD, which only the test build
    # of the provider reads. No product links that build, and nothing installs
    # or packages this launcher.
    if(UML_BUILD_INTEGRATION_TESTS AND
       TARGET uml_network_provider_test_variant)
        uml_add_macos_embedded_kernel_app(
            uml-darwin-network-test linux_main.cpp
            "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}"
            NETWORK_PROVIDER uml_network_provider_test_variant TEST_ONLY)
        uml_link_linux_boot_files(uml-darwin-network-test)
    endif()
endif()

if(UML_BUILD_ANDROID17_CLI)
    uml_add_macos_embedded_kernel_app(
        uml-darwin-android17 android17_main.cpp
        "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    target_link_libraries(uml-darwin-android17 PRIVATE UML::AndroidBoot)
    # The Android launcher with the edge pipe of android-camera-video-leave:
    # the test plays the application's leaving the foreground through it. The
    # product launcher above has neither the pipe nor the code that reads it,
    # and nothing installs or packages this one. It is built for the
    # integration tests and wherever a release is named for the camera rows.
    if(UML_BUILD_INTEGRATION_TESTS OR UML_ANDROID_CAMERA_RELEASE)
        uml_add_macos_embedded_kernel_app(
            uml-darwin-android17-edge-test android17_main.cpp
            "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" TEST_ONLY EDGE_TEST)
        target_link_libraries(uml-darwin-android17-edge-test PRIVATE
            UML::AndroidBoot)
    endif()
endif()
