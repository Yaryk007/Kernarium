#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Gives back the memory of the apps the system starts around boot and nobody
# uses. Privacy sandbox services, the backup apps, key provisioning, domain
# verification and Health Connect each start from a boot receiver or a bind by
# the system and then sit cached, holding memory until the low memory killer
# wants it. So do Messaging, whose boot receiver queries the SMS provider and so
# starts the provider's process (named com.android.phone, not after its
# package), and the icon renderer's provider, which the first listing of the
# launcher applications leaves cached. Their receivers and bindings stay: this
# runs once, after boot has settled, and kills the processes of the listed
# packages that are cached, which is a death the system could have chosen
# itself. Alarms, jobs and bindings survive it, and the next use starts the app
# again: a text message from the host is a broadcast to the SMS application and
# starts Messaging, then the provider when the message is stored. A package with
# a process that is not cached (a started service, the previous app, the home
# app, a renderer a listing holds) is logged and left alone; so is one the
# system keeps higher than cached.
#
# Each line below is a package and the names of its processes. The list is
# reviewed: the target-files verifier requires exactly this one.

reclaim_list='
android.ext.services .adservices android.ext.services
com.android.federatedcompute.services com.android.federatedcompute.services
com.android.ondevicepersonalization.services com.android.ondevicepersonalization.services
com.stevesoltys.seedvault com.stevesoltys.seedvault
org.calyxos.backup.contacts org.calyxos.backup.contacts
com.android.localtransport com.android.localtransport
com.android.rkpdapp com.android.rkpdapp
com.android.statementservice com.android.statementservice
com.android.healthconnect.controller com.android.healthconnect.controller
com.android.messaging com.android.messaging
com.android.providers.telephony com.android.phone
com.kernarium.icons com.kernarium.icons
'

tag=kernarium_reclaim_boot_apps
settle=120

note() {
    log -t "$tag" -p i "$*"
}

# The shell's globbing and word splitting must not touch what the listing says.
set -f
nl='
'

# One row of the listing, for example
#   "#29: cch  + 5 CEM  --------- 1587:.adservices/u0a104" or
#   "# 9: cch  +15 CEM  --------- 1583:com.android.localtransport/1000".
# Sets label (the adjustment), row_name (the complete process name, which may itself hold
# a colon: "<package>:<name>") and row_user0 (set for a process of user 0: a system uid
# is printed bare, an application as u0a<n> and an isolated process as u0i<n>).
parse_row() {
    row_name=""
    row_user0=""
    set -- $1
    if [ "$1" = "#" ]; then
        label=$3
    else
        label=$2
    fi
    for field in "$@"; do
        case "$field" in
            [0-9]*:*/*)
                pid=${field%%:*}
                case "$pid" in *[!0-9]*) continue ;; esac
                rest=${field#*:}
                row_name=${rest%/*}
                uid=${rest##*/}
                case "$uid" in
                    u0a*|u0i*) row_user0=1 ;;
                    ""|*[!0-9]*) ;;
                    *) row_user0=1 ;;
                esac
                return
                ;;
        esac
    done
}

sleep "$settle"

failed=0
IFS=$nl
for entry in $reclaim_list; do
    unset IFS
    set -- $entry
    pkg=$1
    shift
    processes=$*
    # The decision is made from a listing taken for this package, not from one taken before the
    # earlier packages were handled.
    lru=$(dumpsys activity lru 2>/dev/null </dev/null)
    if [ -z "$lru" ]; then
        note "$pkg left alone: no process list"
        failed=1
        continue
    fi
    cached=""
    busy=""
    kept=""
    for proc in $processes; do
        is_cached=""
        not_cached=""
        IFS=$nl
        for line in $lru; do
            unset IFS
            parse_row "$line"
            # The whole process name, of user 0, and every row that has it.
            [ "$row_name" = "$proc" ] && [ -n "$row_user0" ] || continue
            case "$label" in
                cch*) is_cached=1 ;;
                svc|svcb|prev|home) busy="$busy $proc:$label"; not_cached=1 ;;
                *) kept="$kept $proc:$label"; not_cached=1 ;;
            esac
        done
        unset IFS
        # A name counts as cached only when every one of its rows is: a row of another uid that
        # took the name must not stand for the process that is not cached.
        if [ -n "$is_cached" ] && [ -z "$not_cached" ]; then
            cached="$cached $proc"
        fi
    done
    if [ -n "$busy" ]; then
        note "$pkg left alone, not cached:$busy"
    elif [ -z "$cached" ]; then
        note "$pkg has no cached process (kept:${kept:- none})"
    else
        out=$(cmd activity kill --user 0 "$pkg" 2>&1 </dev/null)
        rc=$?
        # The command says nothing of whether a process ended. The activity manager leaves what
        # it keeps above service (persistent, visible, perceptible), but a process that turned
        # previous or service since the listing can still be ended: that window is accepted, as
        # a low memory kill would end it too.
        note "$pkg asked to end cached$cached (kept:${kept:- none}) rc=$rc $out"
    fi
done
exit "$failed"
