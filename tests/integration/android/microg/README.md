# Android microG signature spoofing probe

The probe guards the restricted signature spoofing of route `p0007`
([contract](../../../../android/lineage-24/README.md#android-product)): only
`com.google.android.gms` and `com.android.vending`, signed with microG's own key
and carrying Google's certificate as their `fake-signature` meta-data, present
that certificate. The image ships no Google or microG software, so a regression
test cannot install the real packages. It checks the negative side instead: a
package with one of those names and that meta-data, but signed with the AOSP
test key, must report its own signer through `signatures` and `signingInfo`, and
with nothing installed under the names neither is reported.

- `AndariumMicrogTests` is an instrumentation that reads both packages through
  both APIs and checks them against `-e expect none|own|google|notgoogle|report`.
- `AndariumMicrogDecoyServices` and `AndariumMicrogDecoyStore` are code-less
  packages with the two names and Google's certificate as `fake-signature`.

All three are signed with the AOSP test key, never a release key, and are never
part of an image. [`build-apks.sh`](build-apks.sh) builds them from the SDK
tools in an Android source tree's prebuilts, writing only to its output
directory. The runner
[`run_android_microg_spoofing.py`](../run_android_microg_spoofing.py) installs
and removes them over the shared adb server: `--mode regression` is the opt-in
`android-microg-spoofing` integration test (`UML_ANDROID_MICROG_RELEASE` and
`UML_ANDROID_MICROG_PROBE_DIR`), and `--mode report --expect google|notgoogle`
reads whatever microG packages a guest has, to qualify a build that spoofs.
