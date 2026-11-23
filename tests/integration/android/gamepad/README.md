# Android gamepad probe

`AndariumGamepadProbe` is a foreground application that records what Android
delivers from game controllers and answers with vibration and lights. It
describes each controller through the public `InputDevice` API (identity, keys,
motion ranges, vibrators, battery, lights), records the key and motion events
(each motion sample of a batch with its own time) and the
`InputDeviceListener` callbacks, and it consumes controller events so that the
system's BACK and HOME handling never sees them. The screen lists the latest
events, which makes it the gamepad tester for a hands-on check.

`tests/integration/android/run_android_gamepad.py` drives it. The harness sends
broadcasts to `org.example.andarium.gamepadprobe.COMMAND` (`reset`, `record`,
`dump`, `vibrate`, `cancel`, `light`, `release`; the extras are documented on
the activity) and reads `files/state.json` with
`adb exec-out run-as org.example.andarium.gamepadprobe cat files/state.json`
after a `dump` whose `nonce` it finds in the file. The controllers are the
runner's synthetic ones, played over the relay's vsock port; no real controller
is read.

Build it either as an explicit Soong module in the Android workspace
(`m AndariumGamepadProbe`, with a temporary source copy at
`test/andarium-gamepad`), or without Soong from the SDK tools in a tree's
prebuilts with `build-apk.sh ANDROID_ROOT OUTPUT_DIR`, which writes only below
the output directory and signs with the AOSP test key. It is not a product
package and is never installed in a release image. After exporting a Soong
build's APK, remove that copy and `system/app/AndariumGamepadProbe` below the
product output before a release build; the target-files verifier rejects a
release that still holds it.
