# SPDX-License-Identifier: GPL-2.0-or-later

# The staging script stages the accepted release, and with --unsigned-proof a
# developer build's proof image, whose staged manifest (a golden file that
# AndariumCore's SystemInfo reads too) says it is a proof.
uml_add_tooling_test(
    NAME andarium-stage-system-image
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_stage_system_image.py
    LABELS andarium
    TIMEOUT 120)

uml_add_tooling_test(
    NAME andarium-device-support
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_andarium_support.py
    LABELS andarium;android
    TIMEOUT 30)

# Every device run starts with free diagnostic-report slots: the counting, the
# move off the device, the refusals and, by source structure, that each device
# runner makes the check before it installs or launches anything.
uml_add_tooling_test(
    NAME ios-device-report-slots
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_device_report_slots.py
    LABELS andarium;kernarium
    TIMEOUT 60)

uml_add_tooling_test(
    NAME andarium-android-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_android_runner.py
    LABELS andarium;android
    TIMEOUT 90)

# The stand-in debugger that serves the JIT's request for executable memory on
# an iOS device: its decisions on a fake memory, and the host side's parts that
# need no device.
uml_add_tooling_test(
    NAME andarium-ios-jit-debugger
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_ios_jit_debugger.py
    LABELS andarium;android
    TIMEOUT 30)

# The application's own touch feedback (the side keys, the rulers) plays through
# one modifier that a launch silencing haptics reaches: no view calls SwiftUI's
# or UIKit's feedback directly.
uml_add_tooling_test(
    NAME andarium-interface-feedback
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_interface_feedback.py
        --root ${PROJECT_SOURCE_DIR}
    LABELS andarium
    TIMEOUT 60)

# The developer-owned local settings of Andarium stay ignored whatever their
# name, and the tracked examples stay visible to Git.
uml_add_tooling_test(
    NAME andarium-local-config-ignore
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_local_config_ignore.py
        --root ${PROJECT_SOURCE_DIR}
    LABELS andarium
    TIMEOUT 60)

# The release build refuses an embedded artifact that is behind the commit it
# archives; the fixtures build a repository, a staged system and an application
# with a kernel carrying a banner and a configuration, and change one thing each.
# An unsigned proof image, in the staged or the packaged system, stops the build
# phase of an archive of Release and no other build's.
uml_add_tooling_test(
    NAME andarium-embedded-artifacts
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_check_embedded_artifacts.py
    LABELS andarium;android;package
    TIMEOUT 180)

# The document pickers open the files a person picks as copies: no fileImporter
# in the application, no document picker without `asCopy: true` outside
# Shared/CopyDocumentPicker.swift, and both call sites present the copy-mode
# picker. The fixtures change one source each; the build phase "Check document
# pickers" runs the check on every build.
uml_add_tooling_test(
    NAME andarium-document-pickers
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_document_pickers.py
        --root ${PROJECT_SOURCE_DIR}
    LABELS andarium
    TIMEOUT 60)

# What any archive of Release must satisfy beyond its embedded artifacts
# (scripts/check-release-archive.py, run by the same build phase): a profile
# link to X only with the developer's own handle, none of the build host's
# paths or a linker debug map in the host code, no test bundle or test
# framework, and no linked test-support library. The fixtures are application
# bundles, some around small linked libraries, that change one thing each.
uml_add_tooling_test(
    NAME andarium-release-archive
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_check_release_archive.py
    LABELS andarium;package
    TIMEOUT 120)

# The Acknowledgements resource names a project and its license text for each
# framework the application embeds and each library of its iOS device prefixes.
uml_add_tooling_test(
    NAME andarium-acknowledgements
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_acknowledgements.py
    LABELS andarium
    TIMEOUT 60)

# The screenshot script takes one run at a time into a directory, refuses a
# second run at once and releases the directory however a run ends.
uml_add_tooling_test(
    NAME andarium-capture-screens
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_capture_screens.py
    LABELS andarium
    TIMEOUT 60)

# The String Catalogs of the application: every string translated into each
# language in the state the editor writes, specifiers and plural categories
# that fit, no Google name but the notice's own, the permission texts and
# What's New notes present, and the catalog sync that follows the
# compiler's extraction.
uml_add_tooling_test(
    NAME andarium-localization
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_localization.py
    LABELS andarium
    TIMEOUT 180)

# Shortcuts offers "Open Android App", and its automatic shortcut, from iOS 27
# only: before it the apps are no entities, so the action has nothing to ask
# for. The declarations are @available(iOS 27.0, *) in the source, and the
# fixtures hold the extracted metadata's shape: an iOS 27.0 introduction beside
# the wildcard passes, the wildcard alone (offered on iOS 26) does not.
uml_add_tooling_test(
    NAME andarium-open-intent-availability
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/andarium/test_open_intent_availability.py
        --root ${PROJECT_SOURCE_DIR}
    LABELS andarium
    TIMEOUT 60)
