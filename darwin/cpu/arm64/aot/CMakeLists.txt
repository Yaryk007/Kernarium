# SPDX-License-Identifier: GPL-2.0-or-later

# Ahead-of-time native code for the Android image's own arm64 libraries
# (docs/architecture.md, "Ahead-of-time code"). The product wraps the blob of
# a release artifact (tools/aot/release_artifact.py), which names the release
# it was built from and the tools that built it, and generates nothing
# (UML_AOT_SOURCE is "artifact"). A build that qualifies a release, or
# measures a variant, generates the blob from the release named by
# UML_AOT_IMAGE instead (UML_AOT_SOURCE is "image"). Neither is committed.

# The size of a Linux task stack, from the kernel configuration the guests
# are built with: the diagnostics sampler and the stack-budget test measure
# against it.
set(_uml_aot_kernel_fragment
    ${PROJECT_SOURCE_DIR}/kernel/common/config/darwin-uml.fragment)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_uml_aot_kernel_fragment})
file(STRINGS ${_uml_aot_kernel_fragment} _uml_aot_stack_order
    REGEX "^CONFIG_KERNEL_STACK_ORDER=[0-9]+$")
if(NOT _uml_aot_stack_order MATCHES "^CONFIG_KERNEL_STACK_ORDER=([0-9]+)$")
    message(FATAL_ERROR
        "${_uml_aot_kernel_fragment} sets no CONFIG_KERNEL_STACK_ORDER")
endif()
math(EXPR _uml_aot_kernel_stack_bytes "4096 << ${CMAKE_MATCH_1}")
set_property(GLOBAL PROPERTY UML_AOT_KERNEL_STACK_BYTES
    ${_uml_aot_kernel_stack_bytes})

set(_uml_aot_dir ${CMAKE_CURRENT_BINARY_DIR})
set(_uml_aot_tools ${PROJECT_SOURCE_DIR}/tools/aot)
set(_uml_aot_image_dir ${_uml_aot_dir}/image)
set(_uml_aot_blob ${_uml_aot_dir}/blob.bin)
set(_uml_aot_manifest ${_uml_aot_dir}/manifest.json)
set(_uml_aot_fw_parent "${PROJECT_BINARY_DIR}/lib")
set(_uml_aot_fw_dir "${_uml_aot_fw_parent}/UmlArm64Aot.framework")

# An iOS framework states the lowest system that loads it, as the platform's
# validation requires of every embedded framework.
set(_uml_aot_plist_minimum)
if(NOT UML_TARGET_MACOS)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "UmlArm64Aot needs the Apple deployment target for its Info.plist")
    endif()
    set(_uml_aot_plist_minimum
"	<key>MinimumOSVersion</key>
	<string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>
")
endif()
file(MAKE_DIRECTORY "${_uml_aot_fw_dir}")
file(WRITE "${_uml_aot_fw_dir}/Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
	<key>CFBundleExecutable</key>
	<string>UmlArm64Aot</string>
	<key>CFBundleIdentifier</key>
	<string>org.uml-darwin.arm64-aot</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>UmlArm64Aot</string>
	<key>CFBundlePackageType</key>
	<string>FMWK</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
${_uml_aot_plist_minimum}</dict>
</plist>
")

set(_uml_aot_tool_sources
    ${_uml_aot_tools}/a64decode.py
    ${_uml_aot_tools}/asm.py
    ${_uml_aot_tools}/elf64.py
    ${_uml_aot_tools}/rewrite.py)

if(UML_AOT_SOURCE STREQUAL "image")
    if(NOT IS_DIRECTORY "${UML_AOT_IMAGE}" OR
       NOT EXISTS "${UML_AOT_IMAGE}/release-manifest.json")
        message(FATAL_ERROR
            "UML_AOT_IMAGE must name an accepted Android release directory")
    endif()
    if(NOT UML_AOT_FSCK_EROFS OR NOT EXISTS "${UML_AOT_FSCK_EROFS}")
        message(FATAL_ERROR
            "UML_AOT_FSCK_EROFS must name an fsck.erofs that decodes Zstandard")
    endif()
    execute_process(COMMAND "${UML_AOT_FSCK_EROFS}" --help
        OUTPUT_VARIABLE _uml_aot_fsck_help ERROR_VARIABLE _uml_aot_fsck_help_err
        RESULT_VARIABLE _uml_aot_fsck_status)
    if(NOT _uml_aot_fsck_help MATCHES "zstd" AND
       NOT _uml_aot_fsck_help_err MATCHES "zstd")
        message(FATAL_ERROR
            "UML_AOT_FSCK_EROFS (${UML_AOT_FSCK_EROFS}) cannot read the "
            "release's Zstandard-compressed EROFS images: its --help does not "
            "list zstd among the algorithms it checks. Use an fsck.erofs built "
            "with libzstd, such as the one the Android build produces "
            "(tools/aot/README.md, \"Tools\").")
    endif()
    if(NOT UML_AOT_LIBRARIES)
        message(FATAL_ERROR "UML_AOT_LIBRARIES is empty")
    endif()
    file(READ "${UML_AOT_IMAGE}/release-manifest.json" _uml_aot_release_json)
    string(JSON _uml_aot_release_id GET "${_uml_aot_release_json}" release_id)
    # The blob names the release it is made from, by its ID and the digests of
    # its manifest and of its system partition, and runs only with it.
    file(SHA256 "${UML_AOT_IMAGE}/release-manifest.json"
        _uml_aot_release_manifest_sha256)
    file(READ "${UML_AOT_IMAGE}/guest/partition-layout.json"
        _uml_aot_layout_json)
    string(JSON _uml_aot_partitions GET "${_uml_aot_layout_json}"
        system_disk partitions)
    string(JSON _uml_aot_partition_count LENGTH "${_uml_aot_partitions}")
    set(_uml_aot_system_sha256)
    math(EXPR _uml_aot_partition_last "${_uml_aot_partition_count} - 1")
    foreach(_index RANGE ${_uml_aot_partition_last})
        string(JSON _name GET "${_uml_aot_partitions}" ${_index} name)
        if(_name STREQUAL "system")
            string(JSON _uml_aot_system_sha256 GET "${_uml_aot_partitions}"
                ${_index} sha256)
        endif()
    endforeach()
    if(NOT _uml_aot_system_sha256)
        message(FATAL_ERROR
            "${UML_AOT_IMAGE}/guest/partition-layout.json has no system "
            "partition digest")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${UML_AOT_IMAGE}/release-manifest.json"
        "${UML_AOT_IMAGE}/guest/partition-layout.json")

    set(_uml_aot_library_args)
    foreach(_library IN LISTS UML_AOT_LIBRARIES UML_AOT_BOOT_CODE)
        list(APPEND _uml_aot_library_args --library "${_library}")
    endforeach()

    # Unpack the release's system partition (a Zstandard EROFS image) once.
    set(_uml_aot_extract ${Python3_EXECUTABLE} -B ${_uml_aot_tools}/extract_image.py
        --release "${UML_AOT_IMAGE}" --fsck-erofs "${UML_AOT_FSCK_EROFS}")
    set(_uml_aot_extract_inputs ${_uml_aot_tools}/extract_image.py
        "${UML_AOT_IMAGE}/release-manifest.json"
        "${UML_AOT_IMAGE}/guest/partition-layout.json")
    add_custom_command(
        OUTPUT ${_uml_aot_image_dir}/identity.json
        COMMAND ${_uml_aot_extract} --out ${_uml_aot_image_dir}
        DEPENDS ${_uml_aot_extract_inputs}
        COMMENT "Unpacking the accepted release's system partition for AOT"
        VERBATIM)
    set(_uml_aot_trees ${_uml_aot_image_dir}/identity.json)
    set(_uml_aot_mount_args)

    # A library outside the system partition comes from its own partition (a
    # path under /vendor, /product, /system_ext or /odm) or from an APEX (a path
    # under /apex/<name>); each is unpacked once.
    set(_uml_aot_seen)
    foreach(_library IN LISTS UML_AOT_LIBRARIES)
        if(_library MATCHES "^/apex/([^/]+)/")
            set(_name "${CMAKE_MATCH_1}")
            set(_key "apex-${_name}")
            if(NOT _key IN_LIST _uml_aot_seen)
                list(APPEND _uml_aot_seen "${_key}")
                set(_dir ${_uml_aot_image_dir}/${_key})
                add_custom_command(
                    OUTPUT ${_dir}/identity.json
                    COMMAND ${_uml_aot_extract} --apex ${_name}
                        --system-root ${_uml_aot_image_dir}/root --out ${_dir}
                    DEPENDS ${_uml_aot_extract_inputs}
                        ${_uml_aot_image_dir}/identity.json
                    COMMENT "Unpacking the ${_name} APEX for AOT"
                    VERBATIM)
                list(APPEND _uml_aot_trees ${_dir}/identity.json)
                list(APPEND _uml_aot_mount_args
                    --mount "/apex/${_name}=${_dir}/root")
            endif()
        elseif(_library MATCHES "^/(vendor|product|system_ext|odm)/")
            set(_name "${CMAKE_MATCH_1}")
            set(_key "partition-${_name}")
            if(NOT _key IN_LIST _uml_aot_seen)
                list(APPEND _uml_aot_seen "${_key}")
                set(_dir ${_uml_aot_image_dir}/${_key})
                add_custom_command(
                    OUTPUT ${_dir}/identity.json
                    COMMAND ${_uml_aot_extract} --partition ${_name} --out ${_dir}
                    DEPENDS ${_uml_aot_extract_inputs}
                        ${_uml_aot_image_dir}/identity.json
                    COMMENT "Unpacking the ${_name} partition for AOT"
                    VERBATIM)
                list(APPEND _uml_aot_trees ${_dir}/identity.json)
                list(APPEND _uml_aot_mount_args "--mount" "/${_name}=${_dir}/root")
            endif()
        endif()
    endforeach()

    # A diagnostics build also rewrites a small library of its own, whose control
    # flow the differential test needs and no release library is certain to have.
    set(_uml_aot_generated_args)
    set(_uml_aot_generated_files)
    if(UML_AOT_DIAGNOSTICS)
        # Count the instructions each entry starts, to measure the budget's bound.
        # The default layout is the product's.
        if(UML_AOT_COUNT_RETIRED)
            list(APPEND _uml_aot_generated_args --count-retired)
        endif()
        set(_uml_aot_fixture ${_uml_aot_dir}/fixture/libaotfixture.so)
        set(_uml_aot_oat_fixture ${_uml_aot_dir}/fixture/boot-fixture.oat)
        add_custom_command(
            OUTPUT ${_uml_aot_fixture} ${_uml_aot_oat_fixture}
            COMMAND ${Python3_EXECUTABLE} -B ${_uml_aot_tools}/fixture.py
                --out ${_uml_aot_fixture} --oat-out ${_uml_aot_oat_fixture}
            DEPENDS ${_uml_aot_tools}/fixture.py
            COMMENT "Generating the AOT test libraries"
            VERBATIM)
        list(APPEND _uml_aot_generated_args
            --generated "/aot-test/libaotfixture.so=${_uml_aot_fixture}"
            --generated "/aot-test/boot-fixture.oat=${_uml_aot_oat_fixture}")
        list(APPEND _uml_aot_generated_files ${_uml_aot_fixture}
            ${_uml_aot_oat_fixture})
    endif()


    # Rewrite the libraries into one position-independent blob.
    add_custom_command(
        OUTPUT ${_uml_aot_blob} ${_uml_aot_manifest}
            ${_uml_aot_dir}/functions.txt ${_uml_aot_dir}/libraries.txt
            ${_uml_aot_dir}/steps.txt ${_uml_aot_dir}/sites.txt
            ${_uml_aot_dir}/licences.json
        COMMAND ${Python3_EXECUTABLE} -B ${_uml_aot_tools}/rewrite.py
            --root ${_uml_aot_image_dir}/root
            ${_uml_aot_mount_args}
            ${_uml_aot_library_args}
            ${_uml_aot_generated_args}
            --release-id "${_uml_aot_release_id}"
            --release-manifest-sha256 "${_uml_aot_release_manifest_sha256}"
            --system-partition-sha256 "${_uml_aot_system_sha256}"
            --out ${_uml_aot_dir}
        DEPENDS ${_uml_aot_trees} ${_uml_aot_tool_sources}
            ${_uml_aot_generated_files}
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/a64_instruction_decode_partitions.tsv
            ${PROJECT_SOURCE_DIR}/darwin/cpu/arm64/layout/assembly_offsets.h
        COMMENT "Rewriting the AOT libraries"
        VERBATIM)
else()
    # The product: the blob an artifact carries, checked here and again
    # whenever it or the tools it names change.
    set(_uml_aot_artifact_json "${UML_AOT_ARTIFACT_DIR}/artifact.json")
    set(_uml_aot_blob "${UML_AOT_ARTIFACT_DIR}/blob.bin")
    set(_uml_aot_manifest "${UML_AOT_ARTIFACT_DIR}/manifest.json")
    file(READ "${_uml_aot_artifact_json}" _uml_aot_artifact_text)
    string(JSON _uml_aot_release_id GET "${_uml_aot_artifact_text}"
        release release_id)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_uml_aot_artifact_json}")
    set(_uml_aot_verify_args --artifact "${UML_AOT_ARTIFACT_DIR}")
    if(UML_AOT_ARTIFACT_ACCEPTED)
        list(APPEND _uml_aot_verify_args
            --accepted "${UML_AOT_ARTIFACT_ACCEPTED}")
    endif()
    set(_uml_aot_verify
        ${Python3_EXECUTABLE} -B ${_uml_aot_tools}/release_artifact.py verify
        ${_uml_aot_verify_args})
    set(_uml_aot_artifact_stamp ${_uml_aot_dir}/artifact-verified.stamp)
    add_custom_command(
        OUTPUT ${_uml_aot_artifact_stamp}
        COMMAND ${_uml_aot_verify}
        COMMAND ${CMAKE_COMMAND} -E touch ${_uml_aot_artifact_stamp}
        DEPENDS "${_uml_aot_blob}" "${_uml_aot_artifact_json}"
            "${_uml_aot_manifest}" "${UML_AOT_ARTIFACT_DIR}/licences.json"
            ${_uml_aot_tool_sources} ${_uml_aot_tools}/release_artifact.py
            ${_uml_aot_tools}/libraries.txt ${_uml_aot_tools}/extract_image.py
            ${_uml_aot_tools}/licences.py
            ${PROJECT_SOURCE_DIR}/tests/unit/arm64/a64_instruction_decode_partitions.tsv
            ${PROJECT_SOURCE_DIR}/darwin/cpu/arm64/layout/assembly_offsets.h
        COMMENT "Checking the AOT artifact"
        VERBATIM)
    add_custom_target(uml_aot_artifact_verified DEPENDS
        ${_uml_aot_artifact_stamp})
    uml_declare_architecture_target(uml_aot_artifact_verified cpu gadget)
endif()

add_library(uml_arm64_aot_framework SHARED aot_blob.S)
uml_declare_architecture_target(uml_arm64_aot_framework cpu gadget)
set_source_files_properties(aot_blob.S PROPERTIES
    COMPILE_DEFINITIONS "AOT_BLOB=\"${_uml_aot_blob}\""
    OBJECT_DEPENDS ${_uml_aot_blob})
if(UML_AOT_SOURCE STREQUAL "image")
    target_sources(uml_arm64_aot_framework PRIVATE
        ${_uml_aot_blob}
        ${_uml_aot_dir}/functions.txt
        ${_uml_aot_dir}/libraries.txt
        ${_uml_aot_dir}/steps.txt
        ${_uml_aot_dir}/sites.txt)
    set_source_files_properties(${_uml_aot_blob} ${_uml_aot_dir}/functions.txt
        ${_uml_aot_dir}/libraries.txt ${_uml_aot_dir}/steps.txt
        ${_uml_aot_dir}/sites.txt
        PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
else()
    add_dependencies(uml_arm64_aot_framework uml_aot_artifact_verified)
endif()
set_target_properties(uml_arm64_aot_framework PROPERTIES
    LINKER_LANGUAGE C
    PREFIX ""
    SUFFIX ""
    OUTPUT_NAME "UmlArm64Aot"
    LIBRARY_OUTPUT_DIRECTORY "${_uml_aot_fw_dir}"
    BUILD_WITH_INSTALL_NAME_DIR TRUE
    INSTALL_NAME_DIR "@rpath/UmlArm64Aot.framework")
add_custom_command(TARGET uml_arm64_aot_framework POST_BUILD
    COMMAND /usr/bin/codesign --force --sign -
        "${_uml_aot_fw_dir}"
    COMMENT "Code-signing UmlArm64Aot.framework"
    VERBATIM)

set_property(GLOBAL PROPERTY UML_AOT_FRAMEWORK_DIR "${_uml_aot_fw_dir}")
set_property(GLOBAL PROPERTY UML_AOT_FRAMEWORK_TARGET
    "uml_arm64_aot_framework")
set_property(GLOBAL PROPERTY UML_AOT_MANIFEST "${_uml_aot_manifest}")
set_property(GLOBAL PROPERTY UML_AOT_SOURCE "${UML_AOT_SOURCE}")
set_property(GLOBAL PROPERTY UML_AOT_RELEASE_ID "${_uml_aot_release_id}")
if(UML_AOT_SOURCE STREQUAL "image")
    # What the tests that read the generated tables and the unpacked image
    # need; an artifact carries none of it.
    set_property(GLOBAL PROPERTY UML_AOT_IMAGE_ROOT
        "${_uml_aot_image_dir}/root")
    set_property(GLOBAL PROPERTY UML_AOT_FUNCTIONS
        "${_uml_aot_dir}/functions.txt")
    set_property(GLOBAL PROPERTY UML_AOT_LIBRARY_FILES
        "${_uml_aot_dir}/libraries.txt")
    set_property(GLOBAL PROPERTY UML_AOT_SITES "${_uml_aot_dir}/sites.txt")
    set_property(GLOBAL PROPERTY UML_AOT_STEPS "${_uml_aot_dir}/steps.txt")
    set_property(GLOBAL PROPERTY UML_AOT_BOOT_CODE "${UML_AOT_BOOT_CODE}")
endif()

# The part the gadget executor links: the binder and the entry call. The
# product target is compiled with no counter, report or test switch in any
# configuration. A diagnostics build links a second compilation of the same
# source with them instead (UML_AOT_RUNTIME_TARGET names the one a composition
# links); tests that exercise the switches use only that one.
function(_uml_aot_add_runtime TARGET DIAGNOSTICS)
    add_library(${TARGET} OBJECT aot_runtime.cpp)
    uml_declare_architecture_target(${TARGET} cpu gadget)
    target_link_libraries(${TARGET}
        PUBLIC uml_arm64_aot_framework
        PRIVATE UML::BridgeAbi UML::CpuContract UML::MemoryContract)
    target_include_directories(${TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_definitions(${TARGET} PRIVATE
        UML_AOT_DIAGNOSTICS=${DIAGNOSTICS}
        UML_MEMORY_COUNT_AUDIT=${DIAGNOSTICS}
        UML_AOT_KERNEL_STACK_BYTES=${_uml_aot_kernel_stack_bytes}
        UML_AOT_INSTANCE_CAPACITY=${UML_AOT_INSTANCE_CAPACITY}
        UML_AOT_SHARED_VERDICTS=$<BOOL:${UML_AOT_SHARED_VERDICTS}>)
endfunction()

_uml_aot_add_runtime(uml_arm64_aot_runtime 0)
if(UML_AOT_DIAGNOSTICS)
    _uml_aot_add_runtime(uml_arm64_aot_runtime_diagnostics 1)
    set_property(GLOBAL PROPERTY UML_AOT_RUNTIME_TARGET
        uml_arm64_aot_runtime_diagnostics)
else()
    set_property(GLOBAL PROPERTY UML_AOT_RUNTIME_TARGET
        uml_arm64_aot_runtime)
endif()
