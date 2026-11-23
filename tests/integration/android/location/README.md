# Android location probe

`AndariumLocationProbe` is a foreground application that reads the gps provider
through Android's public `LocationManager` API, with its satellite status and
NMEA listeners, and judges what it receives against the expectation it was
launched with: `fine` (a precise host fix, exact and with its own accuracy),
`approximate` (an approximate host fix reaching an application that holds the
precise permission: at least 2000 m uncertain, no altitude, speed or bearing,
and sentences that say estimate), `coarse` (an application with only the coarse
permission: Android's own fudged location, and no satellite status or
sentences) and `none` (the host has no position: the receiver powers up and
reports nothing). It writes `files/result.json` in its data directory when the
run ends; read it with `adb exec-out run-as org.example.andarium.locationprobe
cat files/result.json`.

The harness sets the permission state before each launch (`pm grant` and
`pm revoke` of `android.permission.ACCESS_FINE_LOCATION`), boots the guest with
the matching `--location` source, and launches the probe with
`am start -n org.example.andarium.locationprobe/.LocationActivity --es expect
fine --ed lat 35.6895 --ed lon 139.6917 --ei seconds 15`. The extras are
documented on the activity. The default order registers the satellite status
and NMEA listeners after requesting fixes, while the HAL's first session starts;
`--es order status_first` registers them first and so also expects `onStarted`
and, for `fine`, `onFirstFix`: the framework replays neither to a listener that
registers later, and no session begins for the probe if another client already
holds the gps provider.
The tests never read a real location: the sources are the launcher's synthetic
ones.

Build this directory as an explicit Soong module in the Android workspace
(`m AndariumLocationProbe`), with a temporary source copy at
`test/andarium-location`. It is not a product package and is never installed in
a release image. After exporting the APK, remove that copy and
`system/app/AndariumLocationProbe` below the product output before a release
build; the target-files verifier rejects a release that still holds it.
