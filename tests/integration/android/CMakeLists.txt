# SPDX-License-Identifier: GPL-2.0-or-later

# The host's side of an Android guest's disks (android_disk_tool.cpp): the
# runner creates and checks the data disk with the library both launchers use.
add_executable(uml_android_disk_tool android_disk_tool.cpp)
target_link_libraries(uml_android_disk_tool PRIVATE UML::AndroidBoot)
target_compile_options(uml_android_disk_tool PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_android_disk_tool PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_android_disk_tool)

# A guest boots on the release's read-only system disk and a blank data disk
# the host created, then on the same data disk again.
uml_add_guest_integration_test(
    NAME guest-android-disks
    SCRIPT run_guest_android_disks.py
    ARGS --disk-tool $<TARGET_FILE:uml_android_disk_tool>
        --android-root ${PROJECT_SOURCE_DIR}/android/lineage-24
    LABELS guest;boot;android
    TIMEOUT 600
    CPUS 1)

# The release's adb debugging setting and adbd's key rule (the p0007 and p0011
# adb topics), on a fresh data disk and across reboots. It boots Android with
# the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_ADB_DEBUGGING_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-adb-debugging test boots")
if(UML_ANDROID_ADB_DEBUGGING_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-adb-debugging
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_adb_debugging.py
            --release ${UML_ANDROID_ADB_DEBUGGING_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# Every installation's serial number and eth0 address (identity.json beside the
# data disk): the guest reports the file's values, a reboot and a new launch
# keep them, and a second instance gets its own. It boots Android three times
# with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_INSTALLATION_IDENTITY_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-installation-identity test boots")
if(UML_ANDROID_INSTALLATION_IDENTITY_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-installation-identity
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_installation_identity.py
            --release ${UML_ANDROID_INSTALLATION_IDENTITY_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The shell command of Old app compatibility (android/lineage-24/overlay/oldapp_switch) and
# the type of its property, as the adb shell user and as an ordinary application: the
# runner installs probe applications (oldswitch_probe_apk.py), lists, refuses, switches
# one on and off, and has an application read and fail to set the property. It boots
# Android with the macOS launcher, so it is opt-in: name a release built with the
# command.
set(UML_ANDROID_OLDAPP_SWITCH_RELEASE "" CACHE PATH
    "Installed Android release directory, built with kernarium_oldapp, that the android-oldapp-switch test boots")
if(UML_ANDROID_OLDAPP_SWITCH_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-oldapp-switch
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_oldapp_switch.py
            --release ${UML_ANDROID_OLDAPP_SWITCH_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 1800
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The launcher-application shell command of Add to Home Screen
# (android/lineage-24/overlay/app_shortcuts), as the adb shell user: the runner installs probe
# applications (appsprobe_apk.py), checks the digest, the list, the icons and the launch with its
# refusals, and that the digest follows the set of applications. It boots Android with the macOS
# launcher, so it is opt-in: name a release built with the command.
set(UML_ANDROID_APP_SHORTCUTS_RELEASE "" CACHE PATH
    "Installed Android release directory, built with kernarium_apps, that the android-app-shortcuts test boots")
if(UML_ANDROID_APP_SHORTCUTS_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-app-shortcuts
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_app_shortcuts.py
            --release ${UML_ANDROID_APP_SHORTCUTS_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 1800
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The same runner with --image-inputs, for a release built from this
# repository's overlay: wlan0's address derived from eth0's, the Wi-Fi joined,
# no IPv6 link-local address on eth0 and the macvlan, and the adb key that no
# application may read. The accepted release predates
# these image inputs and fails it, so it names its own release.
set(UML_ANDROID_INSTALLATION_IDENTITY_IMAGE_RELEASE "" CACHE PATH
    "Installed Android release directory, built with the installation identity image inputs, that android-installation-identity-image boots")
if(UML_ANDROID_INSTALLATION_IDENTITY_IMAGE_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-installation-identity-image
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_installation_identity.py
            --release ${UML_ANDROID_INSTALLATION_IDENTITY_IMAGE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --image-inputs
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# A fresh guest starts in Setup Wizard; the runner completes the wizard through
# its pages and checks that the harness helper (boot_android.py --provision)
# reaches the same end state. It boots Android twice with the macOS launcher, so
# it is opt-in: name the release under test.
set(UML_ANDROID_OOBE_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-oobe test boots")
if(UML_ANDROID_OOBE_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-oobe
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_oobe.py
            --release ${UML_ANDROID_OOBE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# microG's restricted signature spoofing (route p0007): with nothing installed
# under the two microG names PackageManager reports neither, and test-key
# decoys with those names report their own signer, never Google's certificate.
# It boots Android with the macOS launcher, so it is opt-in: name the release
# under test and the directory that tests/integration/android/microg/build-apks.sh
# filled.
set(UML_ANDROID_MICROG_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-microg-spoofing test boots")
set(UML_ANDROID_MICROG_PROBE_DIR "" CACHE PATH
    "Directory with the three APKs of the microG probe (microg/build-apks.sh)")
if(UML_ANDROID_MICROG_RELEASE AND UML_ANDROID_MICROG_PROBE_DIR AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-microg-spoofing
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_microg_spoofing.py
            --release ${UML_ANDROID_MICROG_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-dir ${UML_ANDROID_MICROG_PROBE_DIR}
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The command that Andarium's bug report sends to the guest to read its /proc
# counters (AndariumCore's GuestProcFiles.swift, the one source of the string),
# run as the application's adb channel runs it, in an Android guest: every file
# arrives with content or says why it did not. It boots Android with the macOS
# launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_GUEST_PROC_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-guest-proc test boots")
if(UML_ANDROID_GUEST_PROC_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-guest-proc
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_guest_proc.py
            --release ${UML_ANDROID_GUEST_PROC_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 1800
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The image's Vulkan check (kernarium_vk_ahb_import): CPU-written hardware buffers
# imported and read back, and the driver's guest memory file descriptors beside
# them (VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf offered and
# enabled, host-visible memory exported, mapped by the Vulkan mapping, by the fd
# and by another process, read by the GPU and imported again, by another
# instance started with exec as well, including after the exporter freed it and
# with the fd outliving the devices). It boots Android
# with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_VULKAN_SMOKE_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-vulkan-smoke test boots")
if(UML_ANDROID_VULKAN_SMOKE_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-vulkan-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_vulkan_smoke.py
            --release ${UML_ANDROID_VULKAN_SMOKE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The host's hardware keyboard and mouse (the image's device configuration,
# the on-screen keyboard default and the pointer capture LED of the input
# topic): the runner plays the display's user over the launcher's VNC display
# with --hardware-input and reads Android's input dump and the probe app's log.
# It boots Android with the macOS launcher, so it is opt-in: name the release
# under test and the probe's APK (tests/integration/android/keymouse).
set(UML_ANDROID_KEYMOUSE_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-keymouse test boots")
set(UML_ANDROID_KEYMOUSE_PROBE_APK "" CACHE FILEPATH
    "AndariumKeyMouseProbe APK that the android-keymouse test installs")
if(UML_ANDROID_KEYMOUSE_RELEASE AND UML_ANDROID_KEYMOUSE_PROBE_APK AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-keymouse
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_keymouse.py
            --release ${UML_ANDROID_KEYMOUSE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-apk ${UML_ANDROID_KEYMOUSE_PROBE_APK}
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The Virtio video devices across a guest suspend (the transport keeps a codec
# stream's queued buffers, docs/architecture.md): the gallery plays a clip and
# screenrecord films the screen while the guest suspends by the host's route and
# by its own with its screen off; the picture moves again, screenrecord ends
# cleanly, Back closes the player, and the host closes every stream by a destroy
# and none by a reset. It boots Android with the macOS launcher, so it is
# opt-in: name the release under test.
set(UML_ANDROID_VIDEO_SUSPEND_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-video-suspend test boots")
if(UML_ANDROID_VIDEO_SUSPEND_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-video-suspend
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_video_suspend.py
            --release ${UML_ANDROID_VIDEO_SUSPEND_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The guest's one Wi-Fi network (the Wi-Fi module's overlay and the boot script
# of android/lineage-24, and the validation of routes p0029 and p0033): with the
# internet checks failing, the network stays enabled and joined, validated, and
# shows only Android's Wi-Fi mark, also when a sign-in or block page answers the
# probe; a suspend keeps the station and a restart joins it again; a joined network
# keeps its settings, a forgotten one stays forgotten and Wi-Fi that is off
# stays off. With previous releases named, the data disk of each is booted by the
# release under test: one that a release left with the network disabled is
# repaired once, and one that never disables it is left alone, and a failed check
# shows only the warning on both. It boots Android with the macOS launcher, so
# it is opt-in: name the release under test, and the previous releases (a
# ;-separated list) for the upgrade sections.
set(UML_ANDROID_WIFI_NETWORK_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-wifi-network test boots")
set(UML_ANDROID_WIFI_NETWORK_PREVIOUS_RELEASE "" CACHE STRING
    "Installed Android release directories (a ;-separated list) whose data disks the android-wifi-network test upgrades (empty: no upgrade section)")
if(UML_ANDROID_WIFI_NETWORK_RELEASE AND TARGET uml-darwin-android17)
    set(wifi_network_previous)
    foreach(previous_release IN LISTS UML_ANDROID_WIFI_NETWORK_PREVIOUS_RELEASE)
        list(APPEND wifi_network_previous --previous-release ${previous_release})
    endforeach()
    uml_add_integration_test(
        NAME android-wifi-network
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_wifi_network.py
            --release ${UML_ANDROID_WIFI_NETWORK_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            ${wifi_network_previous}
        LABELS guest;boot;android
        TIMEOUT 9000
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The launcher's restart-loop guard on an Android guest: a guest restarted after
# the launcher's 15 s window but before its boot finishes ends the launch with
# status 3 after eight boots, and restarts after finished boots (a reboot
# suite's) are never limited. It boots Android with the macOS launcher, so it
# is opt-in: name the release under test.
set(UML_ANDROID_RESTART_LOOP_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-restart-loop test boots")
if(UML_ANDROID_RESTART_LOOP_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-restart-loop
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_restart_loop.py
            --release ${UML_ANDROID_RESTART_LOOP_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# Test support only: a library the camera runner inserts into the launcher
# (DYLD_INSERT_LIBRARIES), which suspends the thread that serves the Virtio sound
# device for a while, as a busy host does to the worker its capture timeline
# runs on while the guest keeps running.
add_library(uml_audio_hold SHARED audio_hold.cpp)
set_target_properties(uml_audio_hold PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(uml_audio_hold PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_audio_hold PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_audio_hold)

# The host's cameras (routes p0030 and p0031 and the camera layouts): the runner
# boots the release with the launcher's synthetic camera and a camera layout on the
# vendor SKU, and checks the layout's features and media profiles, the cameras
# Android lists, one preview frame and one still of the camera application, and,
# in a blocked session, the same cameras with black frames and no still; a live
# session also records, and holds the sound of its recordings to real time through
# a stalled sound device and a stood-still guest. Its restart mode boots the release
# twice over the colour data that a lens's still taught and expects the same cameras
# to be listed again. Its video-lost mode tells the synthetic camera, through its
# test-only control pipe, that the application left the foreground while the video
# mode records, and expects the recording to end by itself with the clip saved, the
# application still in front and the camera to open again, and the video mode's
# viewfinder with no recording to be live again after the return (the release
# carries the last topics of route p0030). Its video-leave mode plays the application's
# leaving the foreground through the edge pipe of the test-only launcher
# (uml-darwin-android17-edge-test, which no product has) while the video mode records: the
# launcher ends the camera's running video, waits for the guest to stop its video streams
# and then holds the guest's GPU requests for a few seconds, and the recording must have
# ended before the hold landed. It boots Android with the macOS launcher, so it is opt-in:
# name the release under test.
set(UML_ANDROID_CAMERA_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-camera tests boot")
if(UML_ANDROID_CAMERA_RELEASE AND TARGET uml-darwin-android17)
    foreach(mode live blocked restart video-lost video-leave)
        set(camera_launcher uml-darwin-android17)
        if(mode STREQUAL "video-leave")
            set(camera_launcher uml-darwin-android17-edge-test)
        endif()
        if(mode STREQUAL "live")
            set(camera_test_name android-camera)
        else()
            set(camera_test_name android-camera-${mode})
        endif()
        uml_add_integration_test(
            NAME ${camera_test_name}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_android_camera.py
                --release ${UML_ANDROID_CAMERA_RELEASE}
                --emulator $<TARGET_FILE:${camera_launcher}>
                --audio-hold-library $<TARGET_FILE:uml_audio_hold>
                --mode ${mode}
            LABELS guest;boot;android
            TIMEOUT 3600
            PROCESSORS 4
            RESOURCE_GROUPS cpus:4)
    endforeach()
endif()

# The front camera's stated orientation of 270 and the picture an application gets through
# Camera API 1: the runner boots Android with the launcher's synthetic phone whose front camera
# is presented half around, opens each camera with a probe application, and checks the
# orientation and facing the framework states and the frame the application receives. It boots
# Android with the macOS launcher, so it is opt-in: it takes the release the camera tests name.
if(UML_ANDROID_CAMERA_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-camera-front-orientation
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_camera_front.py
            --release ${UML_ANDROID_CAMERA_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 1800
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The thermal HAL and the 30 fps cap (route p0007's display topic): the runner
# plays the host application over a vsock forward to the HAL's port and checks
# the thermal manager, the display manager's vote, SurfaceFlinger's render rate
# and the frame rates of the guest and the host at each severity. It boots
# Android with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_THERMAL_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-thermal test boots")
if(UML_ANDROID_THERMAL_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-thermal
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_thermal.py
            --release ${UML_ANDROID_THERMAL_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The device-orientation sensor (android/lineage-24/overlay/sensors) and the
# display rotation it drives: the runner plays the host application over a vsock
# forward to the motion port, sends orientation records and reads Android's
# rotation and sensor service, with auto-rotate on and off, applications that
# ask for an orientation, the lock screen and the setup wizard. It boots Android
# with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_ORIENTATION_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-orientation test boots")
if(UML_ANDROID_ORIENTATION_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-orientation
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_orientation.py
            --release ${UML_ANDROID_ORIENTATION_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 5400
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The magnetometer (android/lineage-24/overlay/sensors) and the compass feature:
# the runner plays the host application over a vsock forward to the motion port
# with the androidboot.kernarium.compass=1 argument, and a foreground client (the
# motion probe, tests/integration/android/motion/build-apk.sh) registers a
# listener for the field the runner sends for each mount. A second boot without
# the argument checks that the image lists no sensor and declares no feature. It
# boots Android with the macOS launcher, so it is opt-in: name the release under
# test and the probe application's APK.
set(UML_ANDROID_COMPASS_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-compass test boots")
set(UML_ANDROID_COMPASS_PROBE_APK "" CACHE FILEPATH
    "AndariumMotionProbe.apk built by tests/integration/android/motion/build-apk.sh")
if(UML_ANDROID_COMPASS_RELEASE AND UML_ANDROID_COMPASS_PROBE_APK AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-compass
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_compass.py
            --release ${UML_ANDROID_COMPASS_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-apk ${UML_ANDROID_COMPASS_PROBE_APK}
        LABELS guest;boot;android
        TIMEOUT 2400
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
    uml_add_integration_test(
        NAME android-compass-absent
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_compass.py
            --release ${UML_ANDROID_COMPASS_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --expect none
        LABELS guest;boot;android
        TIMEOUT 1800
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The gamepad relay, a vendor daemon of the image's overlay
# (android/lineage-24/overlay/gamepad): the runner plays the host application
# over a vsock forward to the relay's port, creates a controller of each driver
# family and checks what Android sees (devices, keys, axes), what comes back
# (rumble, lights, the drivers' feature requests), hot plug, the relay's
# recovery, and the guest's cost per report. It boots Android with the macOS
# launcher, so it is opt-in: name the release under test and the probe app's
# APK (tests/integration/android/gamepad/build-apk.sh).
set(UML_ANDROID_GAMEPAD_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-gamepad test boots")
set(UML_ANDROID_GAMEPAD_PROBE_APK "" CACHE FILEPATH
    "AndariumGamepadProbe.apk that the android-gamepad test installs")
if(UML_ANDROID_GAMEPAD_RELEASE AND UML_ANDROID_GAMEPAD_PROBE_APK AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-gamepad
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_gamepad.py
            --release ${UML_ANDROID_GAMEPAD_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-apk ${UML_ANDROID_GAMEPAD_PROBE_APK}
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The text-message service (android/lineage-24/overlay/sms): the runner plays the
# host application over a vsock forward to the service's port and sends the
# contract's frames. Its channel mode checks the channel (what the host may and
# what adb and the guest may not do, the service's domain, life and denials,
# every vsock listener of the guest); its record mode checks the delivery record
# across a crash, a reboot and fifty kills. Both boot Android with the macOS
# launcher, so they are opt-in: name the release under test and the probe's APK
# and DEX (tests/integration/android/sms/build-apk.sh). The runner's other modes
# and options (a proof build's rows, the variants with no Messaging or no SMS
# provider, the adbd topic, the kernel configuration) are run by hand.
set(UML_ANDROID_SMS_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-sms tests boot")
set(UML_ANDROID_SMS_PROBE_APK "" CACHE FILEPATH
    "AndariumSmsProbe.apk that the android-sms channel test installs")
set(UML_ANDROID_SMS_VSOCKPROBE_DEX "" CACHE FILEPATH
    "vsockprobe.dex that the android-sms channel test runs as the shell")
if(UML_ANDROID_SMS_RELEASE AND UML_ANDROID_SMS_PROBE_APK AND UML_ANDROID_SMS_VSOCKPROBE_DEX
        AND TARGET uml-darwin-android17)
    foreach(mode channel record)
        uml_add_integration_test(
            NAME android-sms-${mode}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_android_sms.py
                --release ${UML_ANDROID_SMS_RELEASE}
                --emulator $<TARGET_FILE:uml-darwin-android17>
                --probe-apk ${UML_ANDROID_SMS_PROBE_APK}
                --vsockprobe-dex ${UML_ANDROID_SMS_VSOCKPROBE_DEX}
                --mode ${mode}
            LABELS guest;boot;android
            TIMEOUT 3600
            PROCESSORS 4
            RESOURCE_GROUPS cpus:4)
    endforeach()
endif()

# The management console's sync on an Android guest, whose init leaves SysRq
# off: a file the guest holds dirty reaches the host's data image once sync
# replies, and survives a hard kill of the emulator. It boots Android with the
# macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_SYNC_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-sync test boots")
if(UML_ANDROID_SYNC_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-sync
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_sync.py
            --release ${UML_ANDROID_SYNC_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The old-application layer (android/lineage-24/overlay/oldapp): a probe
# application of its own, installed as an old (targetSdkVersion 10) and a
# modern (29) package, reads a Java byte[] the way a Dalvik-era game does,
# leaks local references and allocates below 2 GiB; the old package passes, the
# modern one is left alone. The release must carry the layer and its
# ActivityThread hook, so it is a release built from this repository's overlay
# (the accepted release predates them and fails it), and it boots Android with
# the macOS launcher, so the test is opt-in. The probe's native code is built
# with the host's clang++ and ld.lld (PATH).
set(UML_ANDROID_OLDAPP_RELEASE "" CACHE PATH
    "Installed Android release directory, built with the old-application layer, that the android-oldapp test boots")
if(UML_ANDROID_OLDAPP_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-oldapp
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_oldapp.py
            --release ${UML_ANDROID_OLDAPP_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The guest kernel's vDSO (kernel/common/README.md, arm64 task interface) on
# Android: the probes of guest-vdso-* run as the shell user, and bionic's own
# clock_gettime, gettimeofday and time are checked to call the vDSO. The probes
# are built with the host's clang++ and ld.lld (PATH), and the test boots Android
# with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_VDSO_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-vdso test boots")
if(UML_ANDROID_VDSO_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-vdso
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_vdso.py
            --release ${UML_ANDROID_VDSO_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()
