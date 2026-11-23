# Android keyboard and mouse probe

`AndariumKeyMouseProbe` is a foreground application that logs the input
devices Android has, every key it receives, every mouse event (hover, buttons,
scroll, touches of tool type mouse), pointer capture and the relative events
captured while it holds it. `tests/integration/android/run_android_keymouse.py`
drives it: it reads `files/events.log` with `adb exec-out run-as
org.example.andarium.keymouseprobe cat files/events.log`, one line a record,
`<sequence> <kind> key=value ...`. The kinds and the extras that request or
release capture (`am start -n org.example.andarium.keymouseprobe/.KeyMouseActivity
--ez capture true`) are documented on the activity.

Build this directory as an explicit Soong module in the Android workspace
(`m AndariumKeyMouseProbe`), with a temporary source copy at
`test/andarium-keymouse`. It is not a product package and is never installed in
a release image. After exporting the APK, remove that copy and
`system/app/AndariumKeyMouseProbe` below the product output before a release
build; the target-files verifier rejects a release that still holds it.
