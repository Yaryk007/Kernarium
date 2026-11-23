# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME kernarium-device-framework-symbols
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_device_framework_symbols.py
    LABELS kernarium;package
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kernarium-kernel-dsym-symbols
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_kernel_dsym_symbols.py
    LABELS kernarium;package
    TIMEOUT 180)

uml_add_tooling_test(
    NAME kernarium-device-framework-embed
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_device_framework_embed.py
    LABELS kernarium;package
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-keys
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_keys.py
    LABELS kernarium
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-gestures
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_gestures.py
    LABELS kernarium
    TIMEOUT 30)

# A launch of Andarium keeps the phone silent unless the run tests haptics.
uml_add_tooling_test(
    NAME kernarium-ipad-automation-launch
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_launch.py
    LABELS kernarium
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernarium-ipad-automation-supervisor
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_supervisor.py
    LABELS kernarium
    TIMEOUT 120)

# The host's test run records no screen video and keeps no attachments: the
# scheme says so, `start` refuses a run file that does not, and the result
# bundle goes under the state directory.
uml_add_tooling_test(
    NAME kernarium-ipad-automation-recording
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_ipad_automation_recording.py
    LABELS kernarium
    TIMEOUT 30)

# `watch` keeps the host serving: it adopts a serving host and replaces one that
# is shown to be dead, and where it cannot tell (a heartbeat it cannot read, an
# approval the device may have asked for, what is left of the old host) it
# replaces nothing and ends with a status of its own. The tests run the real
# script and its supervisor against fake `xcrun`, `xcodebuild` and
# `caffeinate` executables in a scratch folder.
#
# Both are pinned by real waits, so each file runs as several shards that CTest
# runs side by side (`--shard INDEX/COUNT`, automation_shards.py): together
# the shards run every test once. A shard's bound is sized from its share at
# gate concurrency on a loaded host.
function(_uml_add_watch_shards PREFIX SCRIPT COUNT)
    math(EXPR _last "${COUNT} - 1")
    foreach(_index RANGE ${_last})
        math(EXPR _number "${_index} + 1")
        uml_add_tooling_test(
            NAME ${PREFIX}-shard${_number}
            COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/${SCRIPT}
                --shard ${_index}/${COUNT}
            LABELS kernarium
            TIMEOUT 300)
    endforeach()
endfunction()

_uml_add_watch_shards(kernarium-ipad-automation-watch
    test_ipad_automation_watch.py 6)
_uml_add_watch_shards(kernarium-ipad-automation-watch-process
    test_ipad_automation_watch_process.py 4)

uml_add_tooling_test(
    NAME kernarium-automation-shards
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_automation_shards.py
    LABELS kernarium
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kernarium-audio-interrupter
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_audio_interrupter.py
    LABELS kernarium
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kernarium-payload-removal
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_payload_removal.py
    LABELS kernarium
    TIMEOUT 60)

if(TARGET uml-initramfs)
    uml_add_tooling_test(
        NAME kernarium-automation-payload
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_automation_payload.py
            --tool $<TARGET_FILE:uml-initramfs>
        LABELS kernarium;boot
        TIMEOUT 60)
endif()

# Every host-side end of a running guest goes through the launcher's stop, which
# has the guest write back first.
uml_add_tooling_test(
    NAME kernarium-host-stop-paths
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernarium/test_host_stop_paths.py
    LABELS kernarium
    TIMEOUT 30)
