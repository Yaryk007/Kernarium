# SPDX-License-Identifier: GPL-2.0-or-later
"""Lines of `dumpsys wifi` that the Wi-Fi network script reads, as the Wi-Fi service of the Android 17 image printed
them: the lines the script's grep keeps (a configuration's header, and its `allowAutojoin=` line), in their order, each
cut at 200 characters. They show both states of the saved network VirtWifi and the header in each form the dump prints
it: the connected configuration has a star before its ID and the others have none; a header starts the line, or follows
`mPerId={n=`, `mPerIDForCurrentUser={n=`, `, n=` or `=` of a map entry. The `two-more` dumps also hold a network named
Cafe and one whose name holds the text `ID: 7 SSID: "VirtWifi"`; the lines that this name puts into the event log and
the scan entries are kept as they were printed. The network IDs are the Wi-Fi service's: the service numbers the
configurations again when it loads its store, so VirtWifi's ID is not always the same.

STATES says, for each dump, the ID of VirtWifi's configuration and whether its Auto-connect is on. The test files that
read this module read the dumps through DUMPS."""

STATES = {
    "connected-on": (0, True),
    "connected-off": (0, False),
    "connected-off-two-more": (0, False),
    "unconnected-off": (0, False),
    "unconnected-on": (0, True),
    "unconnected-off-two-more": (2, False),
    "unconnected-on-two-more": (2, True),
}

DUMPS = {}

DUMPS["connected-on"] = """\
* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Con
 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
mPerId={0=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Cur
 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
mPerIDForCurrentUser={0=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionG
 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Co
 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
"""

DUMPS["connected-off"] = """\
* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Con
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
mPerId={0=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Cur
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
mPerIDForCurrentUser={0=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionG
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Co
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
"""

DUMPS["connected-off-two-more"] = """\
 rec[39]: time=11-14 05:51:48.702 processed=ConnectableState org=L3ConnectedState dest=<null> what=CMD_SAVE_NETWORK screen=on uid=2000 2 "Q ID: 7 SSID: "VirtWifi""NONE nid=2 cuid=2000 suid=2000
2026-11-14T05:51:48.688964 - addOrUpdateNetworkInternal: added/updated config. netId=2 configKey="Q ID: 7 SSID: "VirtWifi""NONE uid=2000 name=android.uid.shell:2000
* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Con
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
ID: 1 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Connected
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
ID: 2 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
mPerId={0=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Cur
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
, 1=ID: 1 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conne
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 2=ID: 2 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
mPerIDForCurrentUser={0=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionG
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
, 1=ID: 1 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conne
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 2=ID: 2 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
=ID: 1 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Connecte
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, ScanResultMatchInfo: SSID: "Q ID: 7 SSID: "VirtWifi"", from scan result: false, SecurityParams List:Security Parameters:
=ID: 2 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: nul
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
=* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Co
 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=true
2026-11-14T05:51:48.693454 - setNetworkSelectionEnabled: configKey="Q ID: 7 SSID: "VirtWifi""NONE old networkStatus=NETWORK_SELECTION_PERMANENTLY_DISABLED disableReason=NETWORK_SELECTION_DISABLED_BY_W
2026-11-14T05:51:48.693524 - setNetworkSelectionStatus: configKey="Q ID: 7 SSID: "VirtWifi""NONE networkStatus=NETWORK_SELECTION_ENABLED disableReason=NETWORK_SELECTION_ENABLE
"""

DUMPS["unconnected-off"] = """\
ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conne
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
mPerId={0=ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Curre
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
mPerIDForCurrentUser={0=ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGro
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
=ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conn
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
"""

DUMPS["unconnected-on"] = """\
ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conne
 cuid=1000 cname=android.uid.system:1000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=false
mPerId={0=ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Curre
 cuid=1000 cname=android.uid.system:1000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=false
mPerIDForCurrentUser={0=ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGro
 cuid=1000 cname=android.uid.system:1000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=false
=ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conn
 cuid=1000 cname=android.uid.system:1000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected=false
"""

DUMPS["unconnected-off-two-more"] = """\
ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Connected
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conne
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
mPerId={0=ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 1=ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 2=ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently C
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
mPerIDForCurrentUser={0=ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 1=ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 2=ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently C
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
=ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Connecte
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, ScanResultMatchInfo: SSID: "Q ID: 7 SSID: "VirtWifi"", from scan result: false, SecurityParams List:Security Parameters:
=ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: nul
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
=ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conn
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=false mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnect
"""

DUMPS["unconnected-on-two-more"] = """\
ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Connected
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conne
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnecte
mPerId={0=ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 1=ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 2=ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently C
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnecte
mPerIDForCurrentUser={0=ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 1=ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup:
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, 2=ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently C
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnecte
=ID: 0 SSID: "Cafe" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Connecte
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
, ScanResultMatchInfo: SSID: "Q ID: 7 SSID: "VirtWifi"", from scan result: false, SecurityParams List:Security Parameters:
=ID: 1 SSID: "Q ID: 7 SSID: "VirtWifi"" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: nul
 cuid=2000 cname=android.uid.shell:2000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnected
=ID: 2 SSID: "VirtWifi" PROVIDER-NAME: null BSSID: null FQDN: null HOME-PROVIDER-NETWORK: false PRIO: 0 HIDDEN: false PMF: false CarrierId: -1 SubscriptionId: -1 SubscriptionGroup: null Currently Conn
 cuid=1000 cname=android.uid.system:1000 luid=2000 lname=android.uid.shell:2000 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false mostRecentlyConnecte
"""

# A dump that no Wi-Fi service printed: the unconnected dump with a log line of the kind the service keeps, which holds
# the text of a header (a name that imitates one), inside each block of VirtWifi's configuration, between its header
# and its Auto-connect line. In a real dump such lines never come there; the dump shows that the script does not take
# them for a header.
STATES["synthetic-decoy-in-block"] = STATES["unconnected-off"]
_decoy = '2026-11-14T05:47:20.000000 - setNetworkSelectionStatus: configKey="Q ID: 7 SSID: "VirtWifi""NONE'
DUMPS["synthetic-decoy-in-block"] = "\n".join(
    line for original in DUMPS["unconnected-off"].split("\n")
    for line in ([original, _decoy] if "ID: " in original and " SSID: " in original else [original]))
