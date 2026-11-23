# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME lineage24-builder-lane
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR} --part verifiers
    LABELS android;audit
    TIMEOUT 180)

uml_add_tooling_test(
    NAME lineage24-target-files-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR} --part target-files
    LABELS android;audit
    TIMEOUT 300)

uml_add_tooling_test(
    NAME lineage24-aosp-patch-regenerator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_aosp_patch_regenerator.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-routed-workspaces
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_routed_workspaces.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 900)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-source-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_mesa_venus_source_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 300)

# TalkBack, eSpeak NG and libsonic are pinned in the metadata and in the
# dependency manifest alike, and a checkout is accepted only at its pin and
# its series' tree.
uml_add_tooling_test(
    NAME lineage24-accessibility-source-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_accessibility_source_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-port-generator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_generate_mesa_venus_port.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-first-boot-precompile
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_precompile_first_boot_apps.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-sign-release
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_sign_release.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-release-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

# The camera application's overlay, its patch series and the verifier name the same
# settings and values.
uml_add_tooling_test(
    NAME lineage24-camera-app-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_app_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

# The TalkBack the image builds is the package Andarium's screen-reader switch
# names, installed where the target-files verifier requires it.
uml_add_tooling_test(
    NAME lineage24-accessibility-package
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_accessibility_package.py
    LABELS android;audit
    TIMEOUT 60)

# The licence modules of the series that build TalkBack and eSpeak NG name what
# each holds, for the image's NOTICE, with the text of every copyleft kind.
uml_add_tooling_test(
    NAME lineage24-accessibility-licenses
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_accessibility_licenses.py
    LABELS android;audit
    TIMEOUT 60)

# The list of paths that decide a release names every producer, and the object
# IDs it yields do not depend on history.
uml_add_tooling_test(
    NAME lineage24-release-inputs
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_inputs.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

# The root files a release can carry beside its system disk: the pin of the
# Magisk release they are made from, the patcher's orchestration, and the
# verifier that checks the patched init_boot against its own stock image.
uml_add_tooling_test(
    NAME lineage24-magisk-root
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_magisk_root.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 120)

# The release manifest, installer and publisher carry the root files, and refuse
# a release whose root/ is partial, whose patched image does not verify against
# its own stock init_boot, or whose APK is not the pin.
uml_add_tooling_test(
    NAME lineage24-release-root
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_root.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME android17-macos-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_boot_android.py
    LABELS android
    TIMEOUT 30)

# The macOS Android launcher's own argument checks, on the built executable
# (registered where the Android CLI is part of the build).
if(TARGET uml-darwin-android17)
    uml_add_tooling_test(
        NAME android17-launcher-arguments
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android17_launcher_arguments.py
            $<TARGET_FILE:uml-darwin-android17>
        LABELS android
        TIMEOUT 120)
endif()

uml_add_tooling_test(
    NAME android17-instance-rules
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android_instance.py
    LABELS android
    TIMEOUT 30)

# Each Android runner's default adb forward port is its guest slot's first
# port, and its own fixed port outside a slot.
uml_add_tooling_test(
    NAME android-runner-slot-ports
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_slot_port_defaults.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-adb-debugging-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_adb_debugging_runner.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-guest-proc-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_guest_proc_runner.py
    LABELS android
    TIMEOUT 60)

# The Vulkan smoke runner's reading of what the image's check prints, rendered from
# the check's own formats.
uml_add_tooling_test(
    NAME android17-vulkan-smoke-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_vulkan_smoke_runner.py
    LABELS android
    TIMEOUT 60)

uml_add_tooling_test(
    NAME android17-installation-identity-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_installation_identity_runner.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-provision-helper
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android_provision.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-oobe-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_oobe_runner.py
    LABELS android
    TIMEOUT 30)

# The thermal runner's reading of the guest's dumps, and the records it sends.
uml_add_tooling_test(
    NAME android17-thermal-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_thermal_runner.py
    LABELS android
    TIMEOUT 30)

# The orientation runner without a guest: the wire against the golden frames,
# the parsers of the dumps it reads, the rotation each request earns, its host
# against a fake guest on loopback, and the probe application's manifest.
uml_add_tooling_test(
    NAME android17-orientation-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_orientation_runner.py
    LABELS android
    TIMEOUT 60)

# The Old app compatibility runner without a guest: the probe packages (manifests,
# the stand-in library, the probe's script), the parsers, and every step against
# a scripted image that follows the contract and differs one way at a time.
uml_add_tooling_test(
    NAME android17-oldapp-switch-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_oldapp_switch_runner.py
    LABELS android
    TIMEOUT 120)

# The Old app compatibility shell command's logic on a plain JVM: the eligibility
# rule, the listing, the writes and the exit statuses against a fake platform. It
# skips (77) where the host has no JDK.
uml_add_tooling_test(
    NAME android17-oldapp-switch-command
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_oldapp_switch_command.py
    LABELS android
    TIMEOUT 300)
set_tests_properties(android17-oldapp-switch-command PROPERTIES
    SKIP_RETURN_CODE 77)

# The icon renderer (android/lineage-24/overlay/app_icons, the app KernariumIcons): what it shares with
# kernarium_apps (the shell user's uid, the shape of a request, a path's rules, the sizes and bounds) on a plain
# JVM and again with one rule removed at a time, where the named rows must fail; and, as text with a mutant for
# each rule, what its provider, worker, drawing and service must do and must not touch, with its manifest
# (unprivileged, direct-boot aware, a guarded provider and an isolated service, nothing else) and build file.
# It skips (77) where the host has no JDK.
uml_add_tooling_test(
    NAME android17-icon-renderer
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_icon_renderer.py
    LABELS android
    TIMEOUT 600)
set_tests_properties(android17-icon-renderer PROPERTIES
    SKIP_RETURN_CODE 77)

# The launcher-application shell command (android/lineage-24/overlay/app_shortcuts) on a plain JVM:
# the component grammar, the labels' escaping and bounds, the listing's rule, digest and icon keys,
# the framing of every answer read back by a parser of the test's own, the icon records, the launch's
# refusals and argument vector, how a start's output is read, the child process (real processes), the
# icon source's bound, the resource files' order and the exit statuses against a fake platform, and
# again with one rule removed at a time, where the named rows must fail. It also checks the image's
# wiring of the command (product, build check, verifier list, no policy), that the sources write and
# open nothing, and what the Android glue must not do (as text, each rule with a mutant). It skips
# (77) where the host has no JDK.
uml_add_tooling_test(
    NAME android17-app-shortcuts-command
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_app_shortcuts_command.py
    LABELS android
    TIMEOUT 900)
set_tests_properties(android17-app-shortcuts-command PROPERTIES
    SKIP_RETURN_CODE 77)

# The launcher-application runner without a guest: the probe packages (manifests read back, the bad icon and
# the caller), the strict parsers of the command's answers (each deviation of a header, a row, a label and an
# icon record, and of the sampler's answer), the judgement of what the tool's process holds and of the
# renderer's two processes, and every step against a scripted guest that follows the contract and differs one
# way at a time. It skips (77) where the host has no keytool and jarsigner.
uml_add_tooling_test(
    NAME android17-app-shortcuts-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_app_shortcuts_runner.py
    LABELS android
    TIMEOUT 300)
set_tests_properties(android17-app-shortcuts-runner PROPERTIES
    SKIP_RETURN_CODE 77)

# The text message service's logic on a plain JVM: the PDUs it builds, read back
# with the platform's own UTF-16 decoder, the request's parsing and the answers,
# the delivery record (never twice, at every point a request can end at), and the
# host channel's server, frame and admission over stand-ins for the platform's
# socket classes. It skips (77) where the host has no JDK.
uml_add_tooling_test(
    NAME android17-sms-service
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_sms_service.py
    LABELS android
    TIMEOUT 300)
set_tests_properties(android17-sms-service PROPERTIES
    SKIP_RETURN_CODE 77)

# The old-application runner without a guest: the probe application's two
# packages (manifest, DEX file, libraries built for armv7 when the host has the
# tools), the verdicts, the permission review and the commands it sends.
uml_add_tooling_test(
    NAME android17-oldapp-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_oldapp_runner.py
    LABELS android
    TIMEOUT 120)

# The compass runner without a guest: the wire against the golden frames (the
# magnetic field records and the 20-byte "ANC3" frame), the oracle of the four
# mounts and the compass points, the parsers of what the guest prints, its host
# against a fake guest on loopback, and the launch-state check against scripted
# answers.
uml_add_tooling_test(
    NAME android17-compass-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_compass_runner.py
    LABELS android
    TIMEOUT 60)

# The Wi-Fi network runner's reading of the guest's status, listing, dumps and
# overlay list, and of the boot script's settle time; its upgrade checks against
# scripted answers.
uml_add_tooling_test(
    NAME android17-wifi-network-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_wifi_network_runner.py
    LABELS android
    TIMEOUT 60)

# The guest's Wi-Fi network script, the real text under sh, against a fake Wi-Fi
# framework with a config store, a memory list and a persistent marker: a stop
# before the store wrote the network, a save before the store is read, and every
# marker-2 state; mutants of the script must fail a case.
uml_add_tooling_test(
    NAME android17-wifi-network-script
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_wifi_network_script.py
    LABELS android
    TIMEOUT 360)

# The video-suspend runner's suspend step against a fake guest: a host GPU reset
# across the wake fails it by that name, and the shared reader of the launcher's
# log that the Wi-Fi network runner uses as well; and its start_guest, which the
# sync and vDSO runners share, against a stand-in emulator: a start that fails
# or is interrupted stops the emulator it launched.
uml_add_tooling_test(
    NAME android17-video-suspend-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_video_suspend_runner.py
    LABELS android
    TIMEOUT 30)

# The start_guest of the thermal, keyboard and mouse, gamepad, camera, Wi-Fi
# network and orientation runners (the compass and old-application runners
# start through the orientation runner's) against a stand-in emulator and an
# adb that does nothing: a start that fails or is interrupted stops the
# emulator it launched, and a guest that booted is returned running.
uml_add_tooling_test(
    NAME android17-runner-start-failure
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_runner_start_failure.py
    LABELS android
    TIMEOUT 180)

# The gamepad runner without a guest: the host's end of the wire against the
# golden frames and the C++ header, the controller families against the
# drivers' own report structures, the parsers of what the guest prints, the
# checks, and the reader thread against a fake relay on loopback.
uml_add_tooling_test(
    NAME android17-gamepad-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_gamepad_runner.py
    LABELS android
    TIMEOUT 60)

# The text-message runner without a guest: its frames and requests against the
# golden file, the record file it writes, its readers of the guest's logs and
# dumps, its judgements of a round of rows, its host and adb-server client
# against fakes on loopback, the catalogue of rows, and a parse of the probe's
# sources and build script.
uml_add_tooling_test(
    NAME android17-sms-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_sms_runner.py
    LABELS android
    TIMEOUT 120)

# The text message holder on the host: its source compiled as it is and run
# against the handoff socket as init makes it (bound, not listening), a vsock
# port stood in for by a unix socket, the waker's property, the descriptor
# passed to the service alone, and the next wake after a session. It skips (77)
# where the host has no C++ compiler.
uml_add_tooling_test(
    NAME android17-sms-holder
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_sms_holder.py
    LABELS android
    TIMEOUT 300)
set_tests_properties(android17-sms-holder PROPERTIES
    SKIP_RETURN_CODE 77)

# The keyboard and mouse runner's reading of the guest's input dump and of the
# probe's log, its VNC client's wire, and its key table against the launcher's.
uml_add_tooling_test(
    NAME android17-keymouse-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_keymouse_runner.py
    LABELS android
    TIMEOUT 30)

# The camera runner's plan for every layout, its reading of the guest's dumps,
# screenshot and still, and its arguments against the launcher's option parser.
uml_add_tooling_test(
    NAME android17-camera-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_runner.py
    LABELS android
    TIMEOUT 60)

# The front camera runner without a guest: the chart read at each of a frame's
# turns and mirrors, the probe's statement and the camera service's dump, the
# verdict for each camera, and the probe application's package (skipped where the
# host has no keytool and jarsigner).
uml_add_tooling_test(
    NAME android17-camera-front-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_front_runner.py
    LABELS android
    TIMEOUT 120)

# What the camera HAL does when the host ends the video of a camera (the
# application left the foreground): the callback path of the sensor is run, the
# series' own source compiled against stub headers, and the wiring of the rest
# of the series is read as text; nothing boots.
uml_add_tooling_test(
    NAME android17-camera-device-lost
    COMMAND ${CMAKE_COMMAND} -E env UML_CXX=${CMAKE_CXX_COMPILER}
        ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_device_lost.py
    LABELS android
    TIMEOUT 180)

# A result of the camera HAL states the crop region its centred camera applies,
# so that the zoom the camera service splits into a ratio and a residual region
# joins again in the result; a structural check of what the series does to the
# request state, the rule itself is a unit row.
uml_add_tooling_test(
    NAME android17-camera-crop-region
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_crop_region.py
    LABELS android
    TIMEOUT 120)

# The microG spoofing probe's sources agree with each other and with the
# certificate LineageOS compares with, and its runner reads `am instrument`.
uml_add_tooling_test(
    NAME android17-microg-probe
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_microg_probe.py
    LABELS android
    TIMEOUT 60)

# The camera layouts: the closed set of names the host composes, their fallback
# order, and the permission, media profiles and init files the generator renders
# from them (overlay/camera/generated holds exactly its output).
uml_add_tooling_test(
    NAME android17-camera-layouts
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_layouts.py
    LABELS android
    TIMEOUT 30)
