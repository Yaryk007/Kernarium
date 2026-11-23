# Android text-message probe

`AndariumSmsProbe` is an application that `tests/integration/android/run_android_sms.py` installs on a guest
under test. It holds `RECEIVE_SMS` and records the `SMS_RECEIVED` broadcasts it is handed, and it makes the
calls that an application's domain must not be able to make (a vsock socket, a virtual machine) when the
runner asks. `tool/VsockProbe.java` is the shell's counterpart, run with `app_process` from a DEX file: it
tries a vsock socket, or with `unix PATH` a connect of a `LocalSocket` to the file-system socket at PATH (the
handoff socket). It uses only the platform's public API, which is all the build's `android.jar` has;
`tests/tooling/android/sms/probe-stubs` declares what it uses, so the runner's tests refuse anything else.

Build both without Soong from the SDK tools in an Android tree's prebuilts with `build-apk.sh ANDROID_ROOT
OUTPUT_DIR`, which writes `AndariumSmsProbe.apk` (signed with the AOSP test key) and `vsockprobe.dex` below the
output directory only. The APK is also an explicit Soong module (`Android.bp`). It is not a product package and
is never installed in a release image: the target-files verifier rejects a release that holds it.

`run_android_sms.py --mode delivery --release DIR [--proof-image]` needs neither file: it is the check of a
built image's text-message path (a plain message and one with a one-time code are delivered and stored as one new
inbox row each, and `com.android.phone` keeps its pid; it needs the SMS provider), and `--proof-image` takes an
unsigned proof image.
