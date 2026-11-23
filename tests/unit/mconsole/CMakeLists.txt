# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_console_tests guest_console_tests.cpp)
target_link_libraries(uml_guest_console_tests PRIVATE UML::DeviceGuestConsole)
# A suspend with no reply at the one ten-second deadline is aborted with a wake.
# The request's socket stays open for its final reply, which decides a refusal
# (the guest runs) from silence or a "suspended" that followed the wake
# (unconfirmed, woken at the resume); a busy retry shares the deadline.
uml_add_unit_test(NAME guest-console-suspend-timeout
    COMMAND uml_guest_console_tests suspend-timeout
    LABELS console)
uml_add_unit_test(NAME guest-console-suspend-silent
    COMMAND uml_guest_console_tests suspend-silent
    LABELS console)
uml_add_unit_test(NAME guest-console-suspend-suspended-after-wake
    COMMAND uml_guest_console_tests suspend-suspended-after-wake
    LABELS console)
# A reply refused in its first part is refused as a whole when its end arrives
# only after the deadline.
uml_add_unit_test(NAME guest-console-suspend-partial-refusal
    COMMAND uml_guest_console_tests suspend-partial-refusal
    LABELS console)
uml_add_unit_test(NAME guest-console-suspend-retry-deadline
    COMMAND uml_guest_console_tests suspend-retry-deadline
    LABELS console)
# A suspend's reply states whether the suspend synced: only the two whole
# texts do, and a kernel that says only "suspended" states nothing.
uml_add_unit_test(NAME guest-console-suspend-statement
    COMMAND uml_guest_console_tests suspend-statement
    LABELS console)
# A guest that reports itself busy is asked again for two seconds; any other
# refusal, or one that stays busy, leaves it running with no stop.
uml_add_unit_test(NAME guest-console-suspend-failure
    COMMAND uml_guest_console_tests suspend-failure
    LABELS console)
uml_add_unit_test(NAME guest-console-location
    COMMAND uml_guest_console_tests location
    LABELS console)
uml_add_unit_test(NAME guest-console-run
    COMMAND uml_guest_console_tests run
    LABELS console)
uml_add_unit_test(NAME guest-console-wake
    COMMAND uml_guest_console_tests wake
    LABELS console)
# A writeback is the console's sync, confirmed by its reply; a kernel without
# the command is asked for SysRq 's', which only starts one.
uml_add_unit_test(NAME guest-console-writeback
    COMMAND uml_guest_console_tests writeback
    LABELS console)
uml_add_unit_test(NAME guest-console-writeback-timeout
    COMMAND uml_guest_console_tests writeback-timeout
    LABELS console)
# Only the whole reply "synced in <n> ms" confirms, and one budget covers the
# console's lock, every part of a reply, the fallback and its settling period.
uml_add_unit_test(NAME guest-console-writeback-strict
    COMMAND uml_guest_console_tests writeback-strict
    LABELS console)
uml_add_unit_test(NAME guest-console-writeback-budget
    COMMAND uml_guest_console_tests writeback-budget
    LABELS console)
uml_add_unit_test(NAME guest-console-trim
    COMMAND uml_guest_console_tests trim
    LABELS console)
# The guest's memory figures: the strict reply grammar, a request sent under
# the caller's lock and waited for without it, and the guests that refuse,
# garble or never answer it.
uml_add_unit_test(NAME guest-console-memory
    COMMAND uml_guest_console_tests memory
    LABELS console)
