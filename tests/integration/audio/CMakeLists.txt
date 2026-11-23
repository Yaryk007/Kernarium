# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_apple_audio_playback_tests apple_playback_tests.cpp)
target_link_libraries(uml_apple_audio_playback_tests PRIVATE UML::AudioDevice)
uml_add_integration_test(NAME apple-audio-playback
    COMMAND uml_apple_audio_playback_tests LABELS audio TIMEOUT 20)

# The interruption test builds the sink for the iOS Simulator itself, so it
# needs no target of this tree. It runs in a simulator device it creates and
# deletes, and ends its own steps before the timeout.
# It runs twice: on iOS 27, where the system posts the session becoming inactive
# and the resumption recommendation, and on iOS 26, the iOS floor, where it
# posts the interruption notification and the sink and the microphone fall back
# to the earlier engine calls.
uml_add_integration_test(NAME apple-audio-interruption
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_ios_interruption.py
        --sink-directory ${PROJECT_SOURCE_DIR}/darwin/device/audio
        --include-directory ${PROJECT_SOURCE_DIR}/darwin
        --deployment-target 27.0
    LABELS audio TIMEOUT 840)
set_tests_properties(apple-audio-interruption PROPERTIES SKIP_RETURN_CODE 77)
uml_add_integration_test(NAME apple-audio-interruption-ios26
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_ios_interruption.py
        --sink-directory ${PROJECT_SOURCE_DIR}/darwin/device/audio
        --include-directory ${PROJECT_SOURCE_DIR}/darwin
        --deployment-target 26.0 --runtime-major 26
        --legacy-session-notifications
    LABELS audio TIMEOUT 840)
set_tests_properties(apple-audio-interruption-ios26 PROPERTIES SKIP_RETURN_CODE 77)
