# SPDX-License-Identifier: GPL-2.0-or-later

# The Mach-O reader that the gadget-layout, gadget-label and bundle-identifier
# tests share, on small synthetic files: those tests need built images, which
# a tooling tree lacks.
uml_add_tooling_test(
    NAME macho-reader
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/support/test_macho.py
    LABELS cpu;emulator
    TIMEOUT 60)

# The interruption runner reads a booted Simulator device's processes from
# `ps`, whose rows of a process with multi-line arguments include lines that
# are no process; the runner ignores them rather than failing before it runs
# either test binary.
uml_add_tooling_test(
    NAME apple-audio-interruption-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/support/test_run_ios_interruption.py
        --runner ${PROJECT_SOURCE_DIR}/tests/integration/audio/run_ios_interruption.py
    LABELS device;audio
    TIMEOUT 60)
