# SPDX-License-Identifier: Apache-2.0
#
# Input device configuration of "Kernarium Pointer" (1af4:0005), the host's
# absolute mouse: it places Android's cursor where the host's pointer is. It has
# no input property, so Android runs it as a pointer device with a cursor, and
# its raw range then follows the display's natural orientation like the
# touchscreen's. The device stays internal, as the touchscreen is, so a first
# contact or a button press wakes the screen only because the file says so.

touch.orientationAware = 1
touch.wake = 1
