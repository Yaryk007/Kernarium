#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# The virtual Wi-Fi station sees one open network, VirtWifi. Saving it once
# lets the platform join it whenever the user's Wi-Fi switch is on. This neither
# turns Wi-Fi on nor connects: the switch, and forgetting the network, stay the
# user's.
#
# The marker is 1 once this script has saved the network and 2 once a boot found
# the network stored and the station joined it. Only 2 ends the script's work: at
# 2 the script asks the Wi-Fi service nothing, so a forgotten network stays
# forgotten and a network the user changed keeps its settings. Neither a save nor
# a join proves that the Wi-Fi service's config store holds the network, because
# the service writes the store some time after both, and a guest that stops in
# between keeps the marker and loses the network. So 2 is set only by a boot in
# which the network was listed at the script's first look, before the script added
# anything (it was loaded from the store), and the station joined it within the
# settle. A boot that saved the network leaves 1, and the next boot finds it
# stored and joined and sets 2. Until then the saved list decides on every boot:
# a listed network is left alone and a missing one is saved. Two consequences are
# accepted. A network forgotten before the script reached 2 (in the session that
# first saved it, or before any boot found it stored and joined) is saved again
# once at the next boot. And data of an earlier release that holds marker 1 and
# no network gets the network back on its first boot of this release, which the
# script logs.
#
# A listing the Wi-Fi service did not answer prints "No networks" with status 0,
# like an empty one (the shell command waits four seconds for the Wi-Fi thread
# and then gives an empty list). A look therefore runs the listing under
# `timeout` with a limit of $limit seconds, below the service's four: it finds
# the network absent only when the command exited 0 within the limit, and a
# failed or killed listing is unknown. No time is ever subtracted, so a step of
# the wall clock cannot turn a timeout into an answer. Unknown never saves at
# marker 1, because saving a stored network again resets its settings (metered,
# auto-connect). With no marker there is nothing to lose, and an unknown look
# still leads to the save. What can still pass for absent is an answer within
# the limit before the service has loaded the stored networks, which it does
# before boot_completed; at marker 1 three looks, ten seconds apart in all, must
# find it absent, and the cost of a miss is that network's metered and
# auto-connect settings, reset before the marker reached 2.
#
# Two limits remain. Another writer, the person in Wi-Fi settings, saving or
# re-saving the network within seconds of the script's first look, with the
# guest then stopping before the Wi-Fi service's buffered write, can leave
# marker 2 and no stored network; and a removal between the final listing and the
# add is undone. Tapping the network in Wi-Fi settings restores it in both.
#
# The platform no longer disables the network for lack of internet
# (rro/KernariumWifiOverlay), but a data disk written by an earlier release can
# still hold it disabled, and a disabled network is never joined again by
# itself. So until the marker is 2, a boot gives the station $settle seconds to
# join. If Wi-Fi is on and nothing has joined by then, saving the network again
# enables it (or puts it back, if this boot saved it and the service dropped it)
# and the platform's own scan joins it. A network that was listed at the first
# look and is gone at the settle was removed by the user during this boot: it
# stays removed, and the marker becomes 2. Wi-Fi that is off leaves the marker
# alone, so the next boot with Wi-Fi on still repairs.
#
# A network the user turned Auto-connect off for is no disabled network: it is
# never joined by itself either, so the settle ends the same way, and saving it
# again would turn Auto-connect back on. Before that save the script reads the
# network's Auto-connect from the Wi-Fi service's dump (`dumpsys wifi`; no
# `cmd wifi` command shows it). Off leaves the network alone and the marker at
# 1, and the script logs it: a boot with Auto-connect on joins and sets 2. The
# dump is read for the network ID of the open VirtWifi row of the final listing,
# and only the Auto-connect values printed under a configuration header of that
# ID count (another configuration of the same name, or a name that contains the
# text of a header, has another ID). Only a dump that shows Auto-connect off for
# that configuration, and never on, counts; a dump that fails, is denied or does
# not show it repairs as before, and so does a listing whose row is not one line.
# A dump cut after that configuration's own off value still reads as off, the
# value being the network's. dumpsys's own timeout prints a banner and exits 0,
# so it is given a longer one than the script's `timeout` ($dumplimit seconds),
# which ends a slow dump. A network that is disabled and has Auto-connect off
# too is left disabled until the user turns Auto-connect on, and the next boot
# repairs it. The script sleeps at most 113 s in all, plus the time its commands
# take (a listing at most $limit s, a dump at most $dumplimit s).

marker=persist.sys.kernarium.virtwifi_saved
settle=45
looks=3
limit=3
dumplimit=10
tag=kernarium_wifi_network

note() {
    log -t "$tag" "$1" 2>/dev/null
}

# Sets $state to saved when the listing holds VirtWifi (one line: id, name,
# security type, kept in $row), absent when it does not and the command exited 0 within $limit
# seconds (the Wi-Fi service answered), unknown otherwise (the command failed, or
# timeout killed it).
look() {
    listing=$(timeout "$limit" cmd wifi list-networks 2>/dev/null)
    code=$?
    row=$(echo "$listing" | grep -E '^[0-9]+ +VirtWifi +open')
    if [ -n "$row" ]; then
        state=saved
    elif [ "$code" = 0 ]; then
        state=absent
    else
        state=unknown
    fi
}

# Sets $autojoin to off when the Wi-Fi service's dump shows Auto-connect off for
# the configuration of the open VirtWifi row in $row (the final listing's) and
# never on, and to on otherwise. A header line of the dump (`ID: n SSID: ...`,
# with a star before the ID for the connected configuration only) starts the line
# or follows the `=` of a map entry, and names a configuration's ID; the
# Auto-connect lines that follow belong to it until the next header. Other lines
# that hold such text (a log line, or a scan entry, with a name that imitates it)
# are no header, and the ID is not taken from a header's SSID text.
autojoin_state() {
    autojoin=on
    case "$row" in
        '' | *'
'*) return ;;
    esac
    netid=${row%% *}
    mine=0
    off=0
    on=0
    lines=$(timeout "$dumplimit" dumpsys -t $((dumplimit + 10)) wifi 2>/dev/null |
        grep -E '(^|=)(\* )?ID: [0-9]+ SSID: |allowAutojoin=')
    saved_ifs=$IFS
    IFS='
'
    set -f
    for line in $lines; do
        case "$line" in
            *'ID: '[0-9]*' SSID: '*)
                rest=${line#*'ID: '}
                if [ "${rest%%[!0-9]*}" = "$netid" ]; then mine=1; else mine=0; fi ;;
            *allowAutojoin=true*) [ "$mine" = 1 ] && on=1 ;;
            *allowAutojoin=false*) [ "$mine" = 1 ] && off=1 ;;
        esac
    done
    set +f
    IFS=$saved_ifs
    [ "$off" = 1 ] && [ "$on" = 0 ] && autojoin=off
}

# Succeeds when, after the absent look in $state, $looks looks in all, five
# seconds apart, find the network absent.
missing() {
    seen=1
    while [ "$seen" -lt "$looks" ]; do
        sleep 5
        look
        [ "$state" = absent ] || return 1
        seen=$((seen + 1))
    done
    return 0
}

# Saves the network until the listing holds it; fails after 30 tries.
save() {
    tries=0
    while [ "$tries" -lt 30 ]; do
        # Saving the same network again updates it, so a retry adds no duplicate.
        cmd wifi add-network VirtWifi open >/dev/null 2>&1
        look
        [ "$state" = saved ] && return 0
        tries=$((tries + 1))
        sleep 2
    done
    return 1
}

# Waits up to $1 seconds for the station to join; $wifi keeps the last answer.
wait_joined() {
    waited=0
    while :; do
        wifi=$(cmd wifi status 2>/dev/null)
        case "$wifi" in
            *"Wifi is connected to"*) return 0 ;;
        esac
        [ "$waited" -ge "$1" ] && return 1
        sleep 5
        waited=$((waited + 5))
    done
}

# Records that this script saved the network.
saved_it() {
    [ "$current" = 1 ] || setprop "$marker" 1
}

current=$(getprop $marker)
[ "$current" = 2 ] && exit 0

look
first=$state
listed=0
added=0
if [ "$first" = saved ]; then
    listed=1
elif [ -z "$current" ] || { [ "$first" = absent ] && missing; }; then
    save || exit 1
    added=1
    saved_it
    [ "$current" = 1 ] && note "VirtWifi was not stored: saved again"
fi

if wait_joined "$settle"; then
    if [ "$listed" = 1 ]; then
        setprop "$marker" 2 && note "VirtWifi was stored and joined: marker 2"
    else
        note "VirtWifi joined, and is not known to be stored: marker stays 1"
    fi
    exit 0
fi
# Wi-Fi off, or its service not up: nothing to repair on this boot.
case "$wifi" in
    *"Wifi is enabled"*) ;;
    *) exit 0 ;;
esac
look
case "$state" in
    unknown) exit 0 ;;
    saved)
        autojoin_state
        if [ "$autojoin" = off ]; then
            note "VirtWifi has Auto-connect off: left alone"
            exit 0
        fi
        ;;
    absent)
        if [ "$listed" = 1 ]; then
            setprop "$marker" 2 && note "VirtWifi was removed during this boot: marker 2"
            exit 0
        fi
        [ "$added" = 1 ] || missing || exit 0
        ;;
esac
cmd wifi add-network VirtWifi open >/dev/null 2>&1
saved_it
note "VirtWifi saved again: Wi-Fi is on and nothing joined"
exit 0
