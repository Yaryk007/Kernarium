#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Moves a guest that started with an older release to the product's default for
# the on-screen keyboard, once.
#
# Android hides its on-screen keyboard while a hardware keyboard is attached,
# unless "Show on-screen keyboard" (Settings.Secure.show_ime_with_hard_keyboard)
# is on. The product used to keep it on, because Android believed a keyboard was
# always attached and would never have shown its own. The host's keyboard is now
# a device that is attached only while it is there, so the product's default is
# off (SettingsProvider overlay). SettingsProvider writes a default when it first
# creates a guest's settings and never again, so a guest created earlier still
# holds 1, and would keep its on-screen keyboard over a real one.
#
# The script changes a stored 1 to 0, once: the marker records that it has
# looked, so a person who turns the setting on afterwards keeps it on. A stored 0
# or no value is already the default and is left alone. The setting has no
# history to tell the old default from a person's choice of 1, so a 1 that a
# person chose before this release is turned off once too; Android's own
# settings turn it on again. It acts on the current user (the first boot after an
# update, user 0). A boot that cannot read or change the setting leaves the marker
# unset and the next boot tries again. Nothing here can fail a boot.

tag=kernarium_keyboard_default
marker=persist.sys.kernarium.keyboard_default

note() {
    log -t "$tag" -p i "$*"
}

[ "$(getprop $marker)" = 1 ] && exit 0

# The services start before boot completes; a retry covers a slow one.
value=
tries=0
while [ "$tries" -lt 30 ]; do
    value=$(cmd settings get secure show_ime_with_hard_keyboard 2>/dev/null)
    case "$value" in
        0 | 1 | null) break ;;
    esac
    tries=$((tries + 1))
    sleep 2
done

case "$value" in
    1)
        cmd settings put secure show_ime_with_hard_keyboard 0 >/dev/null 2>&1
        if [ "$(cmd settings get secure show_ime_with_hard_keyboard 2>/dev/null)" != 0 ]; then
            note "could not turn off the on-screen keyboard with a hardware keyboard: keeping 1"
            exit 1
        fi
        note "the on-screen keyboard now hides while a hardware keyboard is attached (was 1)"
        ;;
    0 | null)
        note "the on-screen keyboard setting is already the product's default ($value)"
        ;;
    *)
        note "the on-screen keyboard setting is unreadable: keeping it"
        exit 1
        ;;
esac
setprop "$marker" 1
exit 0
