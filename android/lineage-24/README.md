# LineageOS 24 UML guest producer

This directory owns the standalone LineageOS 24 guest producer for Darwin UML.
LineageOS 24.0 is Android 17: its tree is the AOSP `android-17.0.0_r1` release
with LineageOS's forks of the projects it changes. The exact source identity and
product name live in [`metadata/lineage.env`](metadata/lineage.env). The guest
kernel is not owned here: it is the mainline kernel line built with its Android
overlay, described in
[`../../kernel/mainline-6.18`](../../kernel/mainline-6.18/README.md#android-product).

The producer runs locally on an x86_64 Linux builder. Repository scripts accept
builder-local paths only; transport, machines, accounts, and credentials are
outside this repository.

## Producer boundary

`build/prepare-sources.sh` is the only producer command allowed to use the
network; it pins the Android source, the guest Mesa source and the sources of
TalkBack and eSpeak NG. Patching, builds,
verification, image assembly, and publication are offline (a `build/make` topic,
route `p0002`, keeps `lunch` from calling LineageOS's network-reading
`roomservice.py`).

The producer has three independent outputs: the `lineage_andarium` target files
of the configured build variant (with the kernel-less `init_boot` and LineageOS
recovery images); one static GPT guest disk containing both boot ramdisks; and a
compact rootfs release installed by Apple consumers. Use the scripts under
`build/` in that order; each script's `--help` is the canonical interface.

The kernel is an input, not an output: the `android-6.18` artifact the Mac
builds with `kernel/mainline-6.18/build/build-kernel.sh --product android` and
publishes with `publish-artifact.py --product android`. The build, the disk
assembly and the release publisher take it with `--kernel-artifact` and verify
it against the tracked mainline source. A release ships its kernel files
unchanged, and updating the artifact never republishes the GPT or invalidates a
guest bundle.

## Source and patch ownership

- `metadata/` pins the Android source: `base-manifest.xml` pins every Repo
  project and `lineage.env` records the snapshot and the product identity.
- `patches/projects.tsv` maps each changed project from its exact base to its
  final tree. The generated series under `patches/projects/` are the only
  maintained modifications to upstream projects; a route (`pNNNN`) is one
  project's series.
- `overlay/` owns the complete `device/kernarium/andarium` product tree and is
  synchronized as one verified unit.
- `third_party/mesa-venus/` and `third_party/meson2hermetic/` pin the guest
  Mesa source and the Soong-port converter, each with its own series.
  `third_party/accessibility/` pins the upstream commits of TalkBack, eSpeak NG
  and libsonic, which `build/prepare-sources.sh` materializes at
  `external/talkback`, `external/espeak-ng` and `external/espeak-ng/sonic`
  with the series of `dependencies/patches/` (see
  [Accessibility](#accessibility)).
- `tools/` holds the regenerators, verifiers and release tools; `support/`
  holds the tracked disk, release and instance policies.

Every series uses the shared [`scripts/patch_series.py`](../../scripts/patch_series.py)
format and is identified by the tree it produces: replay applies it to the
pinned base and requires the final tree recorded in `projects.tsv` or the owning
`metadata.env`. Never edit generated patch files; amend clean Git topic branches
and run the matching regenerator under `tools/`.

## Android product

The product is `lineage_andarium` (brand Andarium, manufacturer Kernarium,
device `andarium`; lunch target `lineage_andarium-cp2a-user`): the Cuttlefish
base with LineageOS's `common_full_tablet_wifionly` chain on top.
`overlay/lineage_andarium.mk` sets the LineageOS switches and names what the
product leaves out, chiefly the Cuttlefish virtual hardware that has no Darwin
UML implementation. `Build.SOC_MANUFACTURER` and `SOC_MODEL` are `Kernarium` and
`Virtual CPU`, which joined are the host's name for its virtual CPU.

- **ABIs.** A 64-bit system with a 32-bit application ABI: `arm64-v8a`,
  `armeabi-v7a` and `armeabi`, in that order (`zygote64_32`). Daemons, HALs,
  recovery and the ramdisks stay 64-bit, and WebView renderers too (route
  `p0007`). The kernel runs 4 KiB pages only, because Android's 32-bit libraries
  are 4 KiB aligned. ABI support does not lower the minimum installable target
  SDK; test an older APK with `adb install --bypass-low-target-sdk-block`.
- **Applications.** Settings, Trebuchet, Files, Package Installer, the LineageOS
  WebView, LatinIME, ThemePicker, Messaging for the host's
  [text messages](#text-messages), TalkBack and eSpeak NG
  ([Accessibility](#accessibility)) and a curated set (camera, gallery, music, calendar,
  calculator, PDF viewer, backup). There is no browser, app store or install
  restriction, and no Google apps, services or closed-source Google code;
  open-source components built from source are allowed and are the ones listed
  here. The two prebuilt files under `root/` ([Root files](#root-files)) are
  admitted too: the official Magisk release the pin names, unmodified, and the
  `init_boot` Magisk's own patcher makes from it. Applications that need absent
  hardware or LineageOS cloud services are excluded by name in the product
  makefile, and the build and the verifier assert that list.
- **Signature spoofing.** The image ships no microG, and
  no guest interface names Google's services; Andarium names them only to say
  that they are left out on purpose ([No Google
  apps](../../Andarium/README.md#runtime)). LineageOS's restricted
  spoofing is on in the `user` build as well as in debuggable ones (route
  `p0007`): only `com.google.android.gms` and `com.android.vending`, signed with
  microG's own key and carrying Google's certificate as their `fake-signature`
  meta-data, present that certificate, and nothing else does. It has no
  setting, permission or API.
- **Settings.** Settings is the stock application apart from route `p0009`,
  which removes the pages for hardware the product lacks (the Connected devices
  entry, the Bluetooth developer option, the media output switcher, and the
  hearing, Bluetooth pairing, telephony, biometric and USB pages) and hides the
  Wi-Fi hotspot, wireless display and System update pages.
- **Hardware not advertised.** Telephony (apart from receiving text
  messages), NFC, UWB, Thread,
  biometric, secure element, USB, Bluetooth and Trusty; the vibrator only on a
  device with haptics ([Haptics](#haptics)) and the cameras only as the host
  has them ([Camera](#camera)). The host's position is declared on
  every device ([Location](#location)). SecureElement is kept but not
  persistent (route `p0024`): nothing starts it at boot, and the first
  `SEService` client that binds it starts it.
- **Diagnostics.** A `user` image needs no guest root for diagnostics: the
  host-captured console, UML's mconsole and tracefs at `/sys/kernel/tracing`
  cover them. Only what these cannot reach needs a `userdebug` build with `su`.

### Build variants

`LINEAGE24_BUILD_VARIANT` in `metadata/lineage.env` is `user` or `userdebug`;
lunch, the target-files verifier, the release manifest and the accepted-release
descriptor all follow it. `user` is the default and the only variant that is
signed, published or accepted. It keeps the stock shell privilege boundary: no
`su`, no `adb root`, `ro.debuggable=0`, no permissive SELinux domain, and the
verifier rejects debug-only payload, debuggable applications and the in-tree
test applications. `userdebug` carries `su`, is debuggable, keeps the AOSP test
keys and is never re-signed. Each variant builds in its own output directory and
`build-aosp.sh` refuses the other one ([Release signing](#release-signing)).

### First boot and provisioning

LineageOS Setup Wizard handles the first boot of a fresh guest; completing it
provisions the guest (both settings 1, the wizard's HOME activity disabled), so
later boots go straight to the launcher. The image has no trade-in mode: its
only boot-time requester, DeviceDiagnostics, is excluded, so adbd stays one
process through a first boot.

A harness that erases a guest provisions it over adb with
[`tools/android_provision.py`](../../tools/android_provision.py): it sets both
settings, waits until the wizard is bound, then disables the wizard package for
user 0 and stops it, and changes nothing in the image. It returns only after the
wizard has no process and no status bar record and adbd has been stable with no
trade-in mode pending. A measurement of the first-boot experience itself keeps
the wizard.

The wizard shows Welcome, Language, Date and time and Theme (a secondary user
sees Welcome and Theme). Finish on the Theme page ends the wizard and goes
straight to the home screen: LineageOS's closing page, with its logo and home
button, is not shown (route `p0026`); the verifier requires the two flow scripts
in the wizard's resource overlay byte for byte. The other
LineageOS pages are left out because a default answers them or they cannot apply
to a virtual device: Network (the virtual Wi-Fi is already connected), Location
(stays on), Screen lock (a fresh guest has none), Navigation (three buttons, the
platform default; route `p0026` removes the wizard's forcing), Restore and
Andarium features (not useful here), and Bluetooth, SIM, device-specific setup,
biometrics and recovery update (no such hardware or script).

The home screen is one empty page with a dock of Files and Settings: no
widget, no second page and no other item. Calendar, Gallery, Music and
Calculator stay installed and are in the app drawer. The Launcher3 resource
overlay owns the layouts and the verifier requires them. The launcher reads its
layout once, so a guest created earlier keeps its home screen. An item the
launcher adds by itself, such as a newly installed application, goes on the
first page while it has room, then on the later pages in order (route `p0010`).

### Accessibility

The image carries TalkBack, Android's screen reader, and eSpeak NG, a speech
engine, as the applications `TalkBack` and `eSpeakNG` in `/product/app`; the
image turns nothing on by itself, and Andarium turns TalkBack on only at the
person's request. Both are open source and built from source by the image
build, signed with the image's other applications. `third_party/accessibility/`
pins the upstream commits of TalkBack, eSpeak NG and libsonic;
`build/prepare-sources.sh` materializes them at `external/talkback`,
`external/espeak-ng` and `external/espeak-ng/sonic`, TalkBack and eSpeak NG
with the series of `dependencies/patches/talkback` and `espeak-ng`; and
`tools/verify-accessibility-source.py` checks the pins against
`dependencies/manifest.tsv`, the series and the checkouts, as the preflight,
the patch step and the build run it. A series holds the changes to the project
and its Soong build, `Build with Soong`: the Soong modules, the manifests, and
the licence modules the image's NOTICE reports. The eSpeak NG series then
installs the application's JNI library on the application's own partition, as
the product's generic system rule requires. The TalkBack series also patches
out the three libraries the tree lacks and leaves every Google Play services
library out of the phone build, which recognises no text in images (OCR) as a
result. It leaves every network and Google service permission out as well:
TalkBack requests none, and its settings offer no web page, store page or image
description. eSpeak NG requests none either. The target-files verifier requires
both applications and refuses a request by either for a network or Google
service permission.

### Motion sensors

The product installs Android's AIDL multi-HAL service and a motion sub-HAL
(`overlay/sensors/`) advertising an accelerometer and a gyroscope with
continuous delivery and 10 ms minimum periods. Andarium supplies samples only
while an application uses the sensor, on a device that has it. The sub-HAL
accepts a single stream on vsock port 5556 from host CID 2 (no guest-originated
source) and tells the host which sensors are active.
`overlay/sensors/motion_protocol.hpp` and
`Andarium/AndariumCore/Sources/AndariumCore/MotionStream.swift` own the wire
format. Axes match the mounted virtual display, Core Motion acceleration is
multiplied by -9.80665, and events carry the guest's `CLOCK_BOOTTIME` at receipt:
a live input path, not a synchronized recording. Andarium's
[scene policy](../../Andarium/README.md#devices) describes windowed operation.

When the launch passes `androidboot.kernarium.orientation=1`, the sub-HAL also
lists an on-change, non-wake-up `TYPE_DEVICE_ORIENTATION` sensor, and the window
manager follows the host's orientation as a rotation proposal that Android's own
policy (auto-rotate, the foreground application's requested orientation) still
decides on; the sub-HAL gives the framework the host's last orientation when it
enables the sensor, and Android keeps its rotation until the host has said one.
Without the argument the sub-HAL does not list the sensor and Android judges the
accelerometer, as before. The host sends its current
orientation at the start of each connection whose control frames say the guest
has the sensor (`overlay/sensors/motion_protocol.hpp` has the frames).

When the launch passes `androidboot.kernarium.compass=1` (a host with a
magnetometer), the sub-HAL also lists a continuous `TYPE_MAGNETIC_FIELD` sensor
(20 ms to 1 s) fed with the host's field in the device's frame, with the host's
accuracy as the event's status, and says so in a 20-byte "ANC3" control frame;
the framework declares `android.hardware.sensor.compass` from the same property
(the `SystemConfig` topic of route p0007 reads the vendor's `kernarium_compass`
permission directory only then). The property is supplied only by an application
that speaks the 20-byte request, and forcing it for an older application is
unsupported. Without the argument the image lists no magnetometer, declares no
compass and ends the connection on a magnetic field record, as before.

### Haptics

The vibrator exists only on a device with haptics. The host passes
`androidboot.product.hardware.sku=haptics` for one (Android's ODM SKU, which
`Build.ODM_SKU` reports). The ODM manifest of that SKU
(`overlay/vibrator/manifest_haptics.xml`, named by `ODM_MANIFEST_HAPTICS_FILES`
in `overlay/BoardConfig.mk`) is the only place that declares
`android.hardware.vibrator` `IVibrator/default`, and the service's init script
starts it for that SKU alone. Without the SKU nothing is declared or started,
and Android sees no vibrator.

`overlay/vibrator/` is the vendor AIDL HAL (version 4). It offers amplitude
control, the predefined effects CLICK, DOUBLE_CLICK, TICK, THUD, POP,
HEAVY_CLICK and TEXTURE_TICK, the nine composition primitives (up to 128 per
composition, delays up to 1 s) and completion callbacks; the framework's
intensity settings are applied before the HAL. Envelope (PWLE) effects,
frequency control, audio-coupled haptics, always-on effects and vendor effects
are unsupported and not advertised. The HAL fixes the durations it reports
(`haptics_effects.hpp`) and completes each callback at the declared time whether
or not a host is connected; the host's Core Haptics player decides what fills
them. The HAL listens on vsock port 5557 and accepts only host CID 2; a later
connection replaces an earlier one. It sends each frame without waiting and shuts
a connection down when a frame does not go whole. `overlay/vibrator/haptics_protocol.hpp`
and `Andarium/AndariumCore/Sources/AndariumCore/HapticsProtocol.swift` own the
wire format, which the golden frames in `tests/unit/andarium/fixtures` pin.

### Location

The host's position is the guest's GNSS receiver: `/dev/gnss0`, a Kernarium
Virtio device that the guest kernel registers in Linux's GNSS class
([stream contract](../../darwin/device/gnss/README.md)). Every device declares
`android.hardware.location` and `android.hardware.location.gps` and the vendor
manifest declares `android.hardware.gnss` `IGnss/default` (version 7), with no
SKU: the node exists on every device, and the host reports no fix while it has
no position to give. ueventd gives the node to `gps`.

`overlay/gnss/` is the vendor AIDL HAL. It opens the node at the framework's
`start()`, which powers the receiver on the host, and closes it at `stop()`; it
reads the NMEA epochs and reports a location once for each newer fix, at the
interval `setPositionMode` gave. A location carries the fix's own UTC time and
its age, the ellipsoidal altitude (sea level altitude plus the geoid separation,
or none when either is missing), and the accuracies, speed and bearing only
when the host knew them. The HAL declares no capability, so the framework
schedules fixes slower than every 10 s itself, and it answers every extension a
real receiver offers that this one cannot (satellites, measurements, navigation
messages, batching, geofencing, assistance data, antenna information,
corrections) with `EX_UNSUPPORTED_OPERATION`. The satellite list is always
empty, and nothing the framework injects (time, locations, assistance data) is
used. A fix never sets the clock or the time zone.

A fix the host marked approximate (iOS's reduced accuracy) is reported as a GNSS
fix at least 2000 m uncertain with no altitude, speed or bearing, whatever the
stream carries, and the NMEA sentences an application can listen to are
rewritten to the same. An application that holds only the coarse permission
gets Android's own coarse location of the fix.

A session holds a system-suspend wakelock from `start()` to `stop()`. A guest
that suspends itself with its display off is woken once an epoch while the
receiver is open, and without the lock it would suspend and wake every second
for as long as a client listens. The lock belongs to the HAL's process, so it
goes with the HAL, and it holds back only Android's own autosuspend.

The HAL never calls the framework from an entry point or under one of its own
locks: every announcement, status and report is posted to the HAL's one
reporter thread, which makes the calls in the order posted, so a framework
thread that holds a lock of its own while it calls the HAL cannot wait on a HAL
thread that waits for it, and no entry point waits for a delivery. A callback's
announcement and every status are never dropped: they reach the callback they
were posted for, in one order across callbacks, even if the HAL is closed or
the callback replaced first, and a `start()` is refused while 64 sessions'
statuses are still undelivered. A session's `engine_on` and `session_begin`
come before its first location and its `session_end` and `engine_off` after the
last. Locations, sentences and satellite reports may be dropped: a session's
queued ones go when it is restarted, or the HAL is closed or its callback
replaced, and only the newest 64 are kept when the framework is slow.

`overlay/gnss/nmea.hpp` and `location_policy.hpp` hold the parsing and the
rules, `gnss_engine.hpp` the session, `gnss_reporter.hpp` the calls to the
framework and `gnss_hal.hpp` the state behind the AIDL entry points, with no
Android dependency; the `gnss-hal-*` unit tests read the host writer's own
epochs through them.
`tests/integration/android/location/` is a probe application for the candidate
image, never installed in a release.

### Thermal

The thermal HAL tells Android how hot the host is. `overlay/thermal/` is the
vendor AIDL HAL, which replaces the AOSP example HAL (its APEX is not
installed); the vendor manifest declares `android.hardware.thermal`
`IThermal/default` on every device. Its one SKIN sensor follows the severity
the host application last sent (NONE to CRITICAL), and stays at NONE until a
host connects. The HAL never reports EMERGENCY or SHUTDOWN, whatever the host
sends, and keeps its severity when the host link drops.
`overlay/thermal/thermal_protocol.hpp` and
`Andarium/AndariumCore/Sources/AndariumCore/ThermalProtocol.swift` own the wire,
which the golden frames in `tests/unit/andarium/fixtures` pin.

From SEVERE on, Android holds the display's render rate to 30 Hz, and at
MODERATE to 60 Hz; the display mode does not change. The host application sends
MODERATE only while a 120 Hz run is held to 60 Hz (Low Power Mode, or a screen
that shows under 100 frames a second), so a 60 Hz run never meets it. The
target-files verifier requires each point's range to be exactly one minimum
and one maximum, and the thermal runner checks both ends of the vote. The map is the vendor default display
configuration (`/vendor/etc/displayconfig/default.xml`, from `overlay/config/`),
which the display topic of route `p0007` makes a display with no map of its own
use.
`tests/integration/android/run_android_thermal.py` drives the stream on a
candidate image.

### High-refresh display

When the display the host shows is a 120 Hz mode, the launch passes
`androidboot.kernarium.high_refresh=1` and the virtual display's one mode is
120 Hz (its EDID). Android needs no other input: with one mode it renders at the
mode's rate, picks a lower render rate for content that asks for one, and holds
it to 60 Hz or 30 Hz at the thermal points above. Nothing in the image reads the
property; the mode carries the rate. A 60 Hz host passes neither and behaves as
before.

### Game controllers

The host application's game controllers are HID devices of the guest.
`kernarium_gamepad` (`overlay/gamepad/`) is a vendor daemon that init starts at
boot on every device, in a domain of its own. It listens on vsock port 5558,
accepts only host CID 2, and creates a device through `/dev/uhid` for each
controller the host describes: the node Android's Bluetooth stack uses for a
paired controller, so the kernel's own driver for that device binds it and
Android applies its stock key layout. The relay knows no controller family; the
identity, the descriptor and the meaning of every report are the host's, so a
fix to one ships in an application update. It moves the host's input reports
into the device and the driver's output and feature requests (rumble, lights,
calibration) back to the host.

There is one device per controller, at most eight at a time. A connection that
ends, or a host that connects again, removes every device, and the host creates
its controllers again after the relay's `HELLO`. A driver request that the host
cannot get is answered with `EIO` at once, so a driver never waits out uhid's
five seconds. Writes to the host never block: a frame that does not go whole
shuts the connection down. With no controller the relay only waits, and
Recovery does not run it.
`overlay/gamepad/gamepad_protocol.hpp` and
`Andarium/AndariumCore/Sources/AndariumCore/GamepadProtocol.swift` own the wire
format, which the golden frames in `tests/unit/andarium/fixtures` pin.
`tests/integration/android/run_android_gamepad.py` (`android-gamepad`) plays the
host against a booted release; its probe app is
[`tests/integration/android/gamepad`](../../tests/integration/android/gamepad/README.md).

### Camera

The host's cameras are the guest's. The camera device (Virtio ID 65533, the
video device's camera function) gives each camera a V4L2 capture node and a
metadata node, and the emulated camera HAL of `hardware/google/camera` (route
`p0030`) takes the cameras, their frames, stills, RAW and per-frame metadata
from them and writes the controls back. Its provider is the lazy service
`android.hardware.camera.provider@2.7-service-google-lazy`
(`ICameraProvider/internal/0`, declared once in the vendor manifest). A
system_ext init file restarts it, for a layout with cameras, whenever
cameraserver starts: cameraserver lists only a provider that has registered,
nothing else starts a lazy AIDL provider, and init drops a property trigger on
cameraserver's status in a vendor file. The provider exits when cameraserver
holds no camera, and init starts it again through its interface line when an
application opens a camera or the torch. A provider crash while cameraserver
holds it leaves the guest without cameras until cameraserver starts again.
The image expects the driver to register the camera
nodes as `/dev/video10` and above; the codec nodes stay in `video0` to
`video9`, which the HAL never opens. The HAL's rules (the wire, camera IDs,
controls, Exif, concurrency) are the headers of `overlay/camera/hal/`, which
the HAL includes and the `andarium-camera-*` unit tests compile without
Android. The camera application is LineageOS's Aperture, without its Google
Lens launcher and button, with lens buttons labelled by focal length and with
a switch for free zoom (route `p0031`). Its lens selector lists each lens of
the host as a camera of its own and the camera has no free zoom: a button for
each lens, ordered ultra wide, main, telephoto, and labelled by the ratio of
its 35 mm-equivalent focal length to that of the main camera of its side (the
lowest camera ID; the external cameras are one side), snapped to 0.5, 1, 2,
2.5, 3, 4 or 5 when within 20 %. Pinch to zoom (the front camera's too),
the zoom slider and the Zoom action of the hardware keys are off, the volume
keys keep their other actions (the shutter by default), and a tap on the
active button does nothing. When CameraX closes the camera with a recoverable
error and opens it again (the HAL's report that the host ended the camera's
video, ERROR_DEVICE), the application ends the recording in progress and saves
its clip, and stays open; a start still waiting for its sound is cancelled.
While a recording that is ending is not finalized, the pause and resume
button and the mute of that recording are skipped (the microphone setting
applies to the next recording), and the button pressed with no recording
does nothing. A CameraX recording in another camera application goes on across
the camera's loss and reopening, and its clip holds the gap. A product overlay in
`overlay/overlay/packages/apps/Aperture` sets the switch and ignores camera
ID 2, which only a host that publishes the wide lens twice has. The build
installs it as a static runtime overlay of the application; the application
declares no overlayable section, so Android applies the preinstalled overlay,
and the target-files verifier reads the overlay, not the application.
The host publishes each back lens as a camera of its own and no zoom camera,
and a phone's cameras are the wide lens "0", the front camera "1", the ultra
wide "3" and the telephoto "4".

The image states no camera feature of its own. The host composes a camera
layout from the cameras it has and passes its name as
`androidboot.product.vendor.sku`; the vendor permission directory
`sku_<layout>/` declares the features that layout earns, and an init trigger on
the name selects the media profiles file that matches its size caps. The set is
closed, and `tools/camera_layouts.py` is its only source
(`overlay/camera/generated/layouts.tsv` lists every layout with its dimensions
and fallbacks):

- `none` (no camera), `front` (a front camera) and `front-manual` (a front
  camera with manual sensor control);
- `bf-<small|large>[-flash][-manual[-raw]][-concurrent][-front2160]`, a back
  and a front camera: `small` caps video at 1080p, `large` at 4K; `flash` is
  the back flash, `manual` manual sensor control, `raw` RAW capture (only with
  `manual`), `concurrent` streaming both cameras at once (cameras "0" and "1"
  in one set the host streams together, both meeting the guaranteed concurrent
  stream sizes), and `front2160` the front camera's video at 2160p (only with
  `large`).

That is 39 layouts. A host whose combination the image lacks takes the nearest
by dropping dimensions in this order: concurrent, RAW, front 2160p, flash,
large to small, manual; then `front`, then `none`. The layouts differ in
features and media profiles and never in HALs: there is no manifest per layout,
and none declares `android.hardware.camera.level.full`.

A session started with Camera off lists the same cameras and delivers black
frames. The image expects the always-registered side-key device (1af4:0002) to
report `SW_CAMERA_LENS_COVER` as 1 for the whole boot of such a session and 0
otherwise; Android then holds the camera's hardware toggle, and the sensor
privacy service shows no reminder (dialog or notification) for the blocked
camera, because the Unblock it would offer cannot lift a hardware toggle. A
combined camera and microphone reminder becomes the microphone's. The service's
`SensorPrivacyService` topic of route `p0007` does this, the framework overlay
leaves the `config_sensorUseStartedActivity*` strings at the platform's
defaults, and `android-camera` ([testing](../../docs/testing.md#android))
checks it. The host also sends black frames while it interrupts a camera, and
the HAL passes them on as frames of the camera. When the host ends the video of a
camera (the application left the foreground while the guest had a video stream
open on it), the camera's video buffers fail under a black, interrupted record
for as long as the guest keeps the stream, and the HAL decides by the session's
streams (`camera_session.hpp`). A session with a stream whose consumer is a
video encoder is over: the request that met the loss fails with every output
buffer returned as an error and an `ERROR_REQUEST` (the camera service returns a
request's buffers only for a request it was told failed), the HAL reports
`ERROR_DEVICE` once besides, the requests it still holds drain as
`ERROR_REQUEST` without the frame source, and the opened session answers every
later request, flush and configuration with the internal error (a flush first
ends what the session holds as failed requests); a close succeeds. Any other
session is told nothing: the HAL closes the camera's video streams between
requests, which ends the host's loss, and opens them again with the next
request. The rules have limits: the camera service
refuses further work on a device in its error state, so a client that reaches the
HAL after `ERROR_DEVICE` talks to the provider directly, and there a configuration
is refused by the HAL, which reaches the client as an illegal-argument answer and
leaves no capture session behind, so a flush after it returns OK; and a
session closed while it still holds requests returns their buffers as errors
without a request error for each, and a flush notifies the errors of the requests
it holds in the order it reaches them, which can precede the request in the
sensor. Output sizes the host lacks that
CTS and the camera API require (standard JPEG sizes, 3840x2160 where that is the
largest video mode, which is the recording size of the large tier, the YUV sizes
of every smaller JPEG, 1920x1440 and 1280x720 for a concurrent camera) are cut
from the smallest native video mode that covers them, scaled in the HAL and, for
a JPEG, encoded there; the 3840x2160 one is cut from the mode of that size, so
it asks the host for nothing a stream of that mode does not, and it is listed
only when the camera's stills reach that size. A request whose
outputs the host refuses or does not deliver within the HAL's deadline (3 s,
15 s for a request with a still) is a failed request (`ERROR_REQUEST`, which
Camera2 and CameraX report as a failed capture); a request that loses only some
of its outputs reports `ERROR_BUFFER` for those. `BURST_CAPTURE` is never
declared. A camera lists the keys it honours: the frame duration and the black
level lock only with `MANUAL_SENSOR`, the AF regions only with autofocus, and no
camera lists the tone map mode or curves or states the available tone map modes
(the templates carry no tone map key). Every result carries the scene mode
(`DISABLED`) and, with `MANUAL_SENSOR`, the black level lock. A result states
the crop region its centred camera applies (`camera_crop.hpp`: the requested
size centred in the active array, clamped to the array and to the smallest size
the maximum digital zoom allows, which is the array over that zoom; the whole
array when the request has none), because the camera service joins the zoom
ratio and that residual region again in the result. The HAL's session layer has
divided the region by the ratio before the camera sees it and multiplies the
result's back, so the camera's rule has no ratio in it. A video output
that stays configured but leaves the requests has its host stream closed after
half a second and two requests without a still; a request that takes a still
counts for nothing and starts the count again for every open video output, so
a still, or a queue of stills and other captures, never closes the stream of a
preview waiting behind it (the host would close and reopen its capture session
when no other stream, still or torch holds it open; `camera_streams.hpp`, which
states the two limits of the rule). An output that leaves the configuration closes with the
reconfiguration. The Exif focal length in 35 mm film follows from the camera's
stated focal length and physical sensor size and is left out where the camera
states no size (`camera_exif.hpp`). A camera that states RAW
(a lens whose colour data the host has learned) also states the lens shading
map modes OFF and ON, which the stock device info requires of a RAW camera: it
refuses the camera at boot otherwise, and the camera is never listed again. It
states the hot pixel map modes OFF and ON and returns an empty hot pixel map for
ON, since the host hands over no list of defective pixels, and it states that
its lens shading is not applied to the RAW data (the host copies the platform's
Bayer pixels as they are and the shading map it returns is all ones; whether
the platform's own data is corrected is known only from a device). The
candidate's smoke test is
`tests/integration/android/run_android_camera.py` (`android-camera`; its
restart mode, `android-camera-restart`, boots again over a lens's colour data
and expects the same cameras).

### Text messages

The guest receives text messages from the host and sends none. Port 5560 on
vsock is held from boot by `kernarium_sms_listener` (`overlay/sms/listener/`), a
small native daemon in an SELinux domain of its own that creates, binds and
listens on the socket and never accepts, reads, writes or connects it, so that
the host's connections queue and no other guest domain can take the port. When a
connection waits and no session runs, it sets the control property that starts
`kernarium_sms_wake`, a script in a second domain that asks the activity manager
to start the service as the system user (again after 20 s if no session attached,
and after a session that ended early with a connection still waiting, after a wait
that doubles from half a second to 2 s for the first seventeen such sessions, which
outlast the host's 30 s window, and to 20 s after them); the service then takes the listening
socket from the holder over its unix socket (`/dev/socket/kernarium_sms`), which
only the service's domain may connect to; init makes and binds that socket and
does not listen on it, so the holder listens on it itself. `KernariumSms` (`overlay/sms/`) is a
system application with one non-exported, non-persistent service in a third
domain. It serves the connections from the host's CID 2 alone, ends 30 s after
the last message, and with it its process: nothing of it is resident in between.
The waker starts it, and the system uid can, as the service is not exported; it
serves the connections from CID 2 alone, so neither an adb shell, adb over the
network included, nor an application can deliver a message. The end of the service loses no
connection that it had not accepted, as the holder keeps the listening socket; a request it had
accepted ends with it, and the record answers the host's retry as uncertain or as handed over
earlier. If the holder ends while a
connection waits that nothing has accepted (or while a session runs, and that session ends with
one waiting), the kernel closes the connection: a request the host had already written gets no
answer, a connection that arrives while no holder listens is refused and asked for again within
the host's deadline, and the host's retry for the lost request is answered as uncertain and never
handed over. That message is lost and never delivered twice. While the service keeps ending
early with a request waiting (the holder runs, and the request stays queued), the request may get
no answer within the host's 30 s: when the fault clears in about the last 2.5 s of it, or when the
request arrives after the 2 s phase (the next try is up to 20 s later). The app then reports it as
not confirmed, and the next send asks again with the same identifier, so that request is neither
lost nor handed over twice. `HostFrame.java` and
`Andarium/AndariumCore/Sources/AndariumCore/GuestSmsChannel.swift` own the
frame. A message is a sender (a number or a short name), a text of limited
length and an identifier the host chooses for it. `Message.java` and
`Andarium/AndariumCore/Sources/AndariumCore/Adb/GuestSms.swift` own what is
accepted and refused, each limit stated once.

The target-files verifier holds that boundary. Only the reviewed types and the
holder may create, bind or listen on a vsock socket; no domain but the holder
and the service holds a permission on the holder's vsock sockets, the service
only accept, read, write and shutdown, and the holder only the creation of its
socket, none on the service's own. The waker holds no vsock socket, and neither it
nor the holder holds a right in the network, capability or hardware service
classes, however an attribute grants it. The holder has no rule of its own for
binder, a service lookup or a file; the platform's grants to every domain still
reach it (the binder device nodes, a binder call to `system_server` and the lookup
of the binder statistics service), the verifier pins that set, and it can obtain
no binder handle with it, as no rule gives it a call permission to the service
manager. The waker's binder rights are call and transfer to the service manager and
to `system_server` and no others, and neither daemon holds more than the platform's
rights on the binder and hardware binder device nodes or any on the vendor's (a
debuggable build adds the platform's call and transfer on the root shell). Init
alone enters their domains and controls them: of the keys of the control properties
that init checks for either service (`ctl.<service>`, which it checks first for
start, stop and restart, and `ctl.<action>$<service>`), init alone may set every
one but the key that starts the waker, which the holder sets too.
The holder's start of the waker is denied on the legacy key, which init checks first
and allows on the qualified one; the policy silences that denial, and the verifier
requires it.
The platform labels the directories of the user (`/data/data`, and `/data/user_de/<id>` from the
second boot of a data image) with the user's categories, which the service's level lacks, so
the service alone is an `mlstrustedsubject`, as every system application is: otherwise it could
not search the path of its record, and every message after the first restart would be refused.
Neither daemon is, and the verifier requires both. The compiled manifest of the
service names SDK levels in a `uses-sdk` element, which the build adds, and no
activity, receiver, provider, filter or permission request. The
platform's own `getattr` for `dumpstate` stats a socket and carries no data. Of
the data, only the platform's managers of application data reach it. For the
services it may find, `system_app_data_file` and the vsock sockets of other types,
the service holds only what every ordinary application domain holds through one
platform rule, plus the activity, package and role services.

The service hands Android the message as SMS-DELIVER PDUs, concatenated when
the text needs more than one part: an `SMS_DELIVER` broadcast to the SMS role
holder, then `SMS_RECEIVED` to the applications that may receive SMS, so
Messaging stores and notifies and applications holding `RECEIVE_SMS` get it.
With no SMS role holder and no Messaging installed, the message is stored in
the inbox, and with no SMS provider either it goes only to the `SMS_RECEIVED`
receivers. The broadcasts are sent, not awaited, so a handed-over answer says
only that Android took the message. Each message carries an identifier the host
chooses, and the host marks a repeated send of a message whose answer it did not
get as a retry. The service records the message in its device-protected storage
before handing it over, answers a retry from that record without handing
anything over, and closes an identifier that a retry finds unrecorded, so no
message is handed over twice. When the record cannot say whether a message was
handed over, the answer is uncertain, and that identifier is never handed over
afterwards.

The product carries Messaging and TelephonyProvider and declares
`android.hardware.telephony.messaging` and `config_sms_capable` (the SMS role
needs both), but not `android.hardware.telephony`: there is no radio, SIM or
SMS sending. Without TeleService, TelephonyProvider requests
`MANAGE_APP_OPS_MODES` itself (route `p0034`), which its one-time-code check
needs to reassert the SMS application's app-ops.

### Networking

The guest's one network device, `eth0`, is the host's user-mode network (SLiRP,
10.0.2.0/24) and the management NIC: adb is reached through the host's forward
to 10.0.2.15, so the framework never manages `eth0`; its hardware address and
the guest's serial number are the installation's own, which the host chooses at
random once for each data disk. `kernarium_net_setup`
(`overlay/tools`) prepares it at `post-fs-data` and stacks the `virt_wifi`
device `wlan0` on it, with a hardware address derived from `eth0`'s
(`overlay/tools/kernarium_wifi_mac.h`: no kernel argument carries it, because an
`androidboot.*` argument becomes a property every application can read). The
lower devices, `eth0` and the macvlan `buried_eth0`, carry no IPv6, so no
link-local address built from their hardware addresses shows the installation's
values to an application. The guest's IPv6 is `wlan0`'s, which the framework
enables once Wi-Fi is up, with an address that is not built from the MAC; a
later change that gives the guest more IPv6 puts it on `wlan0`. The tool exits with an error when it cannot turn IPv6 off, and `wlan0`
is then not created. The kernel needs `CONFIG_MACVLAN`, `CONFIG_CFG80211`
and `CONFIG_VIRT_WIFI` built in (the verifier requires them). Apps see Wi-Fi
only, the open SSID `VirtWifi`, because the product declares
`android.hardware.wifi` and not `android.hardware.ethernet`; turning Wi-Fi or
airplane mode off takes apps offline while adb keeps working. The platform
validates a Wi-Fi network before it checks it (route `p0029`): NetworkMonitor
reports it validated once private DNS passes and then probes it, again after a
back-off while the checks fail, and without evaluating private DNS again. A
failed check, including one that a sign-in or block page answers, shows a warning
in the status bar and the Wi-Fi list (route `p0033`) and changes nothing else:
a failed check never leaves the network unvalidated and no sign-in notification
is posted. A forced re-evaluation checks a validated network; a network whose
private DNS has failed is evaluated as the stock rules say. A VPN, a test
network, a network under CTS's test URLs, a network the stock rules do not
validate, DNS and the private DNS settings keep the stock rules. The
product's one network, VirtWifi, is saved once and is never disabled for lack of
internet (`overlay/rro/KernariumWifiOverlay`), because the platform never joins a
disabled network again. Until a boot has found VirtWifi stored and joined, a
boot saves it when the saved list lacks it and saves it again when Wi-Fi is on
and nothing joined, which enables it, unless the Wi-Fi service's dump shows
that the user turned Auto-connect off for it
(`overlay/tools/kernarium_wifi_network.sh`); a guest that stops before the
Wi-Fi service has stored the network gets it back at the next boot. Wi-Fi that
is off stays off and a network forgotten after that boot stays forgotten. There is
no SoftAP, Wi-Fi Direct, Aware or RTT, and `wificond` is not shipped. A VPN
application that allows bypass carries the traffic of the applications it covers
because netd puts the UID range rule of such a VPN ahead of the implicit network
rules that the firewall installs for the default network (route `p0032`).

### Audio, settings and lock screen defaults

The vendor audio policy exposes one built-in microphone, the Virtio sound
capture stream. The product declares `android.hardware.microphone` permanently,
and the host decides whether the stream carries silence (the default) or its
microphone. It also declares `android.software.midi`, which starts MidiService
for applications' virtual MIDI devices; there is no USB or Bluetooth MIDI
transport. The primary output has a 5 ms HAL buffer, so AudioFlinger gives it a
fast mixer, and its writer runs at `SCHED_FIFO` priority 3 and writes straight
into an ALSA ring of four such periods (route `p0005` and the
`ro.hardware.audio.tinyalsa.*` vendor properties); the boot command line lowers
the Virtio sound driver's minimum period to 5 ms for it. The output reports its
position from its ALSA device, less the frames in the ring and the latency the
host reports for its output (route `p0005`). The input's ALSA read alone
paces its I/O thread, and the frames its client takes keep to real time: a read
waits for its frames until a short slack after the last of them is due and makes
the missing ones up then, so that a stall of the device leaves the audio of a
recording as long as its timestamps span. The count may lead the clock by that
slack at most and the frames beyond are dropped; with the host's capture credit
capped a stall causes none, they are stale backlog or come from a device faster
than the clock. A long interruption of the whole process is not caught up: the
clock starts again and the interruption stays a gap in the timeline of the
audio, as it does in the video (the read that it stopped holds at most one
buffer of silence), and a reader that is late during a stall of the device
leaves a gap of its own. The input's position keeps its client's count and
reports the time its last frame was due, or the time of the reply when that is
too old (route `p0005`). A low-latency AAudio stream with a data callback that
leaves its capacity unspecified starts with a two-burst buffer and grows it by
one burst per underrun until the app sets a size (route `p0028`). A Virtio
battery device reports the Apple host's percentage and charging state; on iOS
the percentage is `UIDevice.batteryLevel`, which moves in 5% steps, so the
guest's does too, and the Mac's is exact.
The swipe lock screen can be turned on: `ro.lockscreen.disable.default` turns it
off for user 0 of a fresh `/data` only.

### Keyboard and mouse

The host's keyboard and mouse are three virtio-input devices of the guest. They
exist from boot and are plugged into Android only while the host reports the
hardware: "Kernarium Hardware Keyboard" (1af4:0003), "Kernarium Mouse" (1af4:0004:
the wheels, and relative motion while an app captures the pointer) and
"Kernarium Pointer" (1af4:0005: the absolute position, which places Android's
cursor). The ids and names are permanent, because Android keys a keyboard layout
and its settings to a device's identity. The host sends key positions and never
characters, so Android's own layout, key repeat and Caps Lock apply, and every
key goes to Android.

`overlay/input/` holds the configuration Android reads for them from
`/vendor/usr`. The keyboard and the mouse are external, as USB devices are, so a
key or a click wakes the screen; the pointer follows the display's orientation.
None of the three has a key layout of its own: Android picks one for the
keyboard from the language of the input method, asks when it cannot, and keeps
the user's choice. The side-key keyboard (1af4:0002: Power, Volume Up and Down,
Back and Home) has a layout of those five keys alone, so Android no longer counts
it as a full keyboard that is always attached. The verifier requires each file's
content.

Android hides its on-screen keyboard while a hardware keyboard is attached
unless "Show on-screen keyboard" is on, and the product's default is off (the
SettingsProvider overlay): the keyboard shows until the host's keyboard is
attached. SettingsProvider writes a default only when it first creates a guest's
settings, so a guest that an older release created holds 1. Once boot completes,
`kernarium_keyboard_default` (`overlay/tools`, a script in a core domain of its
own) turns a stored 1 off, once; its marker property records that it has looked,
so a person who turns the setting on afterwards keeps it on. The setting has no
history that tells the old default from a person's choice of 1, so a 1 chosen
earlier is turned off once as well.

While an app holds pointer capture, Android lights the LED_MISC of a cursor
device that advertises it, and puts it out when the capture ends; the mouse is
that device, and the host reads the LED to take hold of its own pointer for the
guest. A device without the LED sees no change. It is a topic of route `p0022`
(`frameworks/native`), and the verifier requires its dump line in
`libinputflinger.so`.

init starts a service on a keychord whenever its keys are down together on any
input device, so typing would fire one: the product's `bugreport` service has
none (Cuttlefish's was A and B), and the verifier refuses a keychord in the
product's init script. Bug reports are taken over adb.

### Debugging and adb

Fresh settings databases keep Developer options and USB debugging off, and
`ro.adb.secure` keeps the build type's value (route `p0001`). adbd listens on
TCP port 5555 and vsock port 5555 and retains the stock authorization prompt.
Three immutable launch properties, which adbd reads itself (route `p0011`),
carry host-owned choices that bundle command lines may not override:

- `ro.boot.kernarium.adb_auth` can disable the authorization check for host-side
  diagnostics: iOS fixes it to one (only the automation runtime lets a test
  launch turn it off, on a loopback forward,
  [`../../Kernarium/README.md`](../../Kernarium/README.md#automation-runs)), the
  standalone macOS product fixes it to zero.
- `ro.boot.kernarium.adb_tcp` is 0 when the launch has no host forward, as in
  the iOS release: adbd then listens on `vsock:5555` alone.
- `ro.boot.kernarium.adb_key` is the adb public key the host application
  authorizes for the boot; adbd accepts it whatever the user's setting. Its type
  is `kernarium_adb_key_prop` (`overlay/sepolicy/system_ext/private`), which no
  application may read: the other two are `bootloader_prop`, readable by every
  application, and a key that is unique to an installation would identify it.
  adbd reads it and init sets it; dumpstate can read it too, so a bug report
  that the user shares can carry the key.

The user's debugging setting stays meaningful while adbd remains the host's
channel: route `p0007` records the setting in `persist.sys.usb.config` and
restarts adbd instead of stopping it, and route `p0011` refuses every key but the
host's, with no prompt, while USB and wireless debugging are both off. `adb root`
is refused at once on a `user` build whatever the authorization policy, with
AOSP's answer for a production build (route `p0011`). Route `p0011` also makes adbd
refuse an `adb reverse` whose device side is a vsock socket, the socket family of the
host-to-guest channels, or a descriptor adbd already holds. SELinux stays enforcing in
both variants. `tests/integration/android/run_android_adb_debugging.py`
qualifies this contract.

### Old applications

A 32-bit application that targets an API level below 14 is a game written for
Dalvik, which gave native code direct object pointers and a heap below 2 GiB.
The layer that gives ART's process those two things is a choice made per
application, off by default: the native-return case below cannot be covered, so
the person switches it on for the games that need it. The store is one
persistent property per package, `persist.sys.kernarium.oldapp.<package>`; its
value is the turn-on time and an empty or unset property means off.
`ActivityThread.handleBindApplication` (a topic of route p0007) tests the
target level, then the bitness, then that the application is not a system
application (neither `FLAG_SYSTEM` nor `FLAG_UPDATED_SYSTEM_APP`), then reads
that property, and only an application that is below API 14, 32-bit, not a
system application and switched on gets `/system/lib/libkernarium_oldapp.so`
(`overlay/oldapp`) in its process; an application that is not eligible never
reads the property. Packages that share a user ID can share a process, so the
hook also leaves the layer out when the user ID has more than one package. A
failed load is logged and the application starts as before.
Applications that target below API 23 get Android's permission review before
their first component runs. The library, which includes no Android header,
patches ART's one JNI function table in place and gives the application:

- Dalvik's array layout for a Java `byte[]` that a call from the application's
  own native code returns (a method registered with the return type `[B`, or a
  field of that type): the value is a block with the length at offset 8 and the
  bytes at offset 16 that stands for the real array and holds a global reference
  to it. The JNI functions that take an object accept a reference to a block, from
  any caller, as a reference to that array: the wrapper makes a local reference of
  its own to the array, under the layer's lock, whichever thread's block it is,
  and gives the runtime only that, never the block's own reference (Java code that
  runs during a call can end the block), so identity
  (`IsSameObject`, `NewGlobalRef`, monitors) and writes (`SetByteArrayRegion`,
  `Release*Elements`, `ReleasePrimitiveArrayCritical`) keep their meaning. From the
  first block on, the runtime is never handed an address of the region: every call
  form (`Call*`, `CallStatic*`, `CallNonvirtual*`, `NewObject*`, the variadic, `V` and
  `A` entries) gives it the array for a reference that is a block, and null for any
  other address of the region (a block that was reclaimed, or one that is being built
  or revived), whatever blocks exist at that moment. For that the layer knows the
  parameters of every method a call names: from the signature at the lookup
  (`GetMethodID`, `GetStaticMethodID`), else by reflection at the first call, from the
  method ID alone (an ID from `FromReflectedMethod`; an exception pending then is taken
  out for the time and put back). That needs neither a receiver nor a class, so a static
  call with a null class, which the runtime runs, runs here too. The registry has no
  limit, and a failure to learn is not remembered. Where the
  parameters cannot be had at a call (no reflection, no method ID, no memory for the
  method's record), the application's call does not reach the runtime: the process
  stops after one line in the log. A platform caller's call goes on with its receiver
  converted and its arguments as they are. The layer reads the
  length from its own record, never from the length word, which the application
  can overwrite. The block and its array behave as one memory at the points
  where the application meets Java: before and after a call that is passed the
  block (as an argument or as the receiver, so `clone()` and a method without
  parameters see it), where the application asks for the bytes
  (`Get...Elements`, `Get...Critical`, `GetByteArrayRegion`), around
  `SetByteArrayRegion` and at a release. The block keeps a copy of its bytes as
  of the last such point. At each point, in chunks of 4 KiB, a chunk whose block
  differs from that copy was written through the raw pointer and is merged with
  the array byte by byte: a byte the application wrote goes to the array, every
  other byte takes the array's if Java changed it, and a byte both sides wrote
  takes the application's. Any other chunk takes the bytes Java changed. The
  block never takes a byte Java did not change, and the array never takes one
  the application did not write (the merge stores those bytes, one byte store
  each, into the array's own memory through the real array's critical pair, and
  a block the application did not write is read through region calls and makes no
  critical get), so a write by either side to such a byte that races a
  synchronization survives it. A byte of the array that a point takes is read
  once, and that one value goes to the block and to its copy, so a Java write
  that lands meanwhile is taken at the next point, never reverted. A committing
  release (mode 0 or `JNI_COMMIT`) is such a point and keeps Java's bytes
  wherever the application did not write; `JNI_ABORT` discards the working copy,
  and the block takes the array's bytes. `Get...Elements` and `Get...Critical`
  of a block report a copy (`isCopy` is `JNI_TRUE`), so an application that
  commits when told so makes its writes visible through any other reference to
  the array; a real array keeps the runtime's own flag. Bytes Java writes
  outside those points (another thread, a callback after the native method
  returned) show at the next one, also when they land while a point runs. A
  byte both sides write in the same interval keeps one of the two values, the
  application's unless its write lands while a point stores Java's: that is
  the one case in which a racing write is lost. A reference to a block ends where a Dalvik local
  reference did: at `DeleteLocalRef` of a reference the innermost frame holds (a
  reference of an outer frame is left alone, as both runtimes leave it), at
  `PopLocalFrame`, at the return of a native
  method of the application's own libraries (found by name or registered; those
  installed with the application or in its private data directories; another
  package's library keeps the runtime's binding) and when its thread detaches. A
  thread holds at most 512 live references; a result beyond the limit stays ART's
  own reference. The same array returned again to the thread is the same block
  (among its eight most recent). A block whose references have all ended stays at
  its address with its bytes as of that end (what the application wrote through
  the pointer reaches the array then) until the runtime collects the array; the
  same array obtained again as a result, on any thread, is that block again, so
  Java's later writes and the application's later raw writes meet in it, and until
  then they reach neither side. A reference to a block that ended, or that another
  thread holds, still means the array wherever the application kept it (a native
  variable, from any thread): a function that is handed it works on the array for
  the length of the call, and the use makes no reference of the calling thread and
  moves no block, so a thread outside any native method, or one at the limit, can
  use it. Only an array that arrives as a result makes a reference again, and
  `DeleteLocalRef` and the releases do not. The layer keeps its own record of
  where each block starts, and an address is a block only when that record says a
  block starts exactly there; no byte of the region decides it. A block whose
  array the collector has cleared is still an ended block of its length until the
  layer reclaims it, and a wrapper that finds no array behind it acts as for a null
  reference (no elements, no copy). The reference of a reclaimed block is no
  block, however the memory it points into has been used since: its length is 0,
  and it is a block again only if a new block starts at exactly that address. A
  block is complete before the record names it, so a thread that looks without the
  lock at the reference of a live or ended block (its kind, its length) finds it
  whole. The record is a mapping of its own, one 32-bit word for each 64 bytes of the
  region (2 MiB of address space for the product's 32 MiB region), resident
  by page only where block starts have been recorded; a page stays resident after
  its blocks are gone.
  A pointer from `Get...Elements` or `Get...Critical` pins its block as Dalvik
  pinned the array: the block keeps its place, and a strong reference to the
  array, until the pointer is released (mode 0 or `JNI_ABORT`; `JNI_COMMIT`
  keeps it). `GetPrimitiveArrayCritical` of a kept reference works like
  `Get...Elements` unless the thread already has a runtime critical region open
  (`GetPrimitiveArrayCritical`, `GetStringCritical`), where no call that makes a
  reference may be made: it then returns the block as it is when a pin or a live
  reference keeps its array, and NULL otherwise. With an exception pending a
  conversion uses the block's strong reference under a pin of the wrapper's own,
  and synchronizes nothing; `MonitorExit` of a block's reference, which JNI
  allows then, takes the exception out, releases the monitor with the array and
  puts the exception back (one `MonitorExit` raised itself stays). A monitor
  call on a block whose array is gone is refused with `JNI_ERR`. Two threads
  that use one block at once share its memory as on Dalvik. Every
  synchronization of a block with its array, its own thread's included, runs
  under one process-wide lock that no call into Java holds, so the
  synchronizations of all threads run one at a time. Nothing under it waits for
  the runtime's collector (a critical get, nested in a region the thread holds
  or not, since the runtime raises a thread's count only for a movable array or
  string; an allocation in the Java heap): those are made before it is taken, on
  a reference that is valid outside it, so a thread that waits for it inside a
  critical region never holds up a collection. Between a critical get and its
  release the layer makes no runtime call except that release. A release goes to the block whose data
  starts at its pointer, whichever reference comes with it, and never reaches
  the runtime for a pointer in the region; a pointer inside the region that is
  not at a block's start is not read as a header, copies nothing, and counts as
  the release of the block of the reference that comes with it. A pointer the
  runtime itself handed out for a real reference to the array (the layer records
  those from `Get...Elements` and `Get...Critical`, from the first one on, under a
  lock of their own, until their release with mode 0 or `JNI_ABORT`) goes to the runtime's
  own Release when it comes with a block's reference, with the block's array,
  and closes its critical region; any other pointer that comes with a block's
  reference is the application's own buffer, and is copied into the block. At
  most 16384 blocks exist at once, live, ended or pinned, since each ends with a
  weak global reference and an end never frees a block whose pointer the
  application may hold. Ended and pinned blocks do not count toward the 512 of a
  thread. When the region or the 16384 are used up, a result first makes room:
  the blocks whose arrays are gone are freed (a full pass is made whenever the
  runtime has collected since a full pass last ran, and otherwise once a second)
  and, once in ten seconds, the runtime is asked to collect; a result that still
  finds none stays ART's own reference. Where no weak global reference can be
  had for an ended block it stays strong and is tried again; where the array's
  identity cannot be had (`System.identityHashCode` and `Runtime.gc()` are
  looked up when the layer installs, before any block exists, and again once a
  second after a failure) the result stays ART's own reference. The process
  holds 32 MiB of blocks, and each block's copy takes as many bytes again in the
  ordinary heap.
  An array the application creates itself (`NewByteArray`) stays ART's own;
- an allocator below 2 GiB for the libraries the application ships: their
  `malloc`, `calloc`, `strdup`, `strndup`, `operator new` and `operator
  delete` imports are redirected as the library is first seen.
  Every loaded library's `free`, `realloc`, `reallocarray` and
  `malloc_usable_size` imports accept such a block, libc's own among them, so a
  buffer the application allocated can be grown by `getline` or freed by the
  framework. A library's `realloc` and `reallocarray` hand out arena blocks only
  once every one of its consumer imports is rewritten.

Only the application's own code gets blocks: a caller is told apart by the
mapping its return address is in, so the platform's libraries in the same
process see the real arrays. Page protection is restored after each write, and
a library whose pages cannot all be opened is left untouched. The library logs
under the tag `kernarium_oldapp` the first `byte[]` result that stayed ART's own
reference (with the blocks and bytes its thread held, and the ended blocks that
waited), the first time it asked the runtime to collect, a package it could
not learn (the runtime's `ActivityThread.currentPackageName()`, else the process
name; every library but the platform's then counts as the application's), the first allocation
that went to libc because the low arena had no room, and, at most every 30 s
while it grows, the most blocks any one thread has held.

The layer cannot cover: a native method that returns a block to Java (ART
decodes the returned reference itself), which is why it is a per-application
choice; a `byte[]` passed into a native method as an argument or the elements
of a `byte[][]`; raw reads of other objects; bytes written through the pointer
that Java reads without one of the points above (a block stored in a field and
read after the native method returned); a release or a synchronization with an
exception pending, which a debuggable application (CheckJNI) aborts on; a
call that bypasses the platform library's PLT; packed relocations; `memalign`;
constructors that ran before the library's first scan; a library loaded
without a call into JNI (found by the next periodic scan), which until then
keeps libc's `free` and can abort on a block another library allocated; a
library with an allocator import in executable code (a text relocation) or
whose relocation pages cannot be reopened, which keeps libc's allocator,
`realloc` included, for its own blocks; a block's address reused after its array was
collected, while the application still holds the old pointer or reference (a stale
reference reaches the array of the block now at its address, as a dangling pointer
could); and more than 512 loaded libraries.
`tests/integration/android/run_android_oldapp.py` qualifies it
([testing](../../docs/testing.md#android)).

### Framework and CTS behaviour

Framework, policy and CTS fixes are patch topics under `patches/projects/`
(each topic's commit message states its reason) or overlay files that the
verifier requires; the vendor SELinux additions live in `overlay/sepolicy/`. The
product uses software KeyMint and gatekeeper, enforcing SELinux policy, no
`system_other` image and no loadable modules or DLKM partitions. These CTS
outcomes are known and not defects of the image:

- LineageOS's restricted networking default is kept; a CTS run sets
  `restricted_networking_mode` to 0 when the guest starts.
- `HostsideVpnTests#testAutomaticOnOffKeepaliveModeClose`, `...NoClose` and
  `#testDownloadWithDownloadManagerDisallowed` fail on LineageOS's netd, which
  does not let traffic explicitly bound to a VPN network fall through to the
  underlying network as AOSP does: a socket bound to a split-tunnel VPN cannot
  reach the internet, and the DownloadManager's address lookup returns no data.
- Tests that reboot the device cannot pass where reboots are disabled, as they
  must be when the emulator exits on reboot.
- `JobSchedulingTest#testAnr_*` and `SurfaceSyncGroupTests#testProcessCrash`
  assume a smaller `hw_timeout_multiplier` than a multiplier of 5.
- dEQP's `deqp:testercore` can abort at exit on a destroyed mutex (an upstream
  exit race with no effect on any result), and the EGL `get_frame_timestamps`
  cases need a quiet host.
- HEVC 4K decoding at 60 frames per second and 32-bit encoding at the largest
  sizes miss the published point
  ([hardware video](#hardware-video-encoding-and-decoding)).
- The measurement, satellite and navigation message cases of
  `CtsLocationGnssTestCases` and `VtsHalGnssTargetTest` fail: the host's
  position has no satellites, raw measurements or ephemeris, and the product
  declares `android.hardware.location.gps` regardless.
- DocumentsUI (Files) on a smallest width of 720 dp or more can throw an
  `IllegalStateException` from AppCompat's `CascadingMenuPopup` while its
  overflow menu changes during the popup's exit transition; real tablets share
  this upstream defect, and there is no product fix.

### Boot, memory and time

- A first boot compiles nothing: `tools/precompile-first-boot-apps.py` compiles
  the packages Soong leaves without artifacts, and `build-aosp.sh` verifies the
  packaged tree.
- The boot animation is off, and boot runs with expedited RCU grace periods that
  the vendor init script turns off at `sys.boot_completed`. The Mac launcher and
  Kernarium read the release's one command line file.
- The launcher sets `androidboot.hw_timeout_multiplier`, which scales the
  framework's ANR, watchdog and input timeouts, from the guest's vCPU count: 3 at
  six or more, 5 below, where the gadget engines are slower than a phone
  (`darwin/device/block/android_boot.hpp`). The release's command line does not
  set it, and the vendor init script falls back to 5 when a launch passes none.
  The launchers refuse any value of it in a release's command line except the
  `=5` that earlier releases carry, which they drop.
- The release's command line carries `androidboot.serialno=umlandroid17`, the
  fallback for a launch that states no serial number, so that the guest always
  has exactly one. A launcher that states the installation's own
  ([`../../darwin/device/block/README.md`](../../darwin/device/block/README.md))
  drops the fallback and appends it; the builder and the verifier require the
  one token.
- The guest has a compressed swap of half its RAM (one zram entry in
  `fstab.uml`), enabled at `sys.boot_completed`. PinnerService does not lock the
  home app's files, and `kernarium_reclaim_boot_apps.sh` kills only the cached
  processes of a reviewed list of apps that boot, or the first listing of the
  launcher applications, starts (Messaging, the SMS provider's process and the
  icon renderer among them) after boot completes. Each package is decided from a
  listing taken for it, by its complete process name, for user 0, and every row of
  the process must be cached; a process that turns previous or service in the instant
  between its own listing and the kill can still be ended once, as a low-memory
  kill would end it. The next use starts them again: a text message from the host
  starts Messaging and then the provider.
- lmkd ends the cached app that holds the most host graphics memory while the
  kernel reports host GPU pressure in `/sys/class/drm/card0/gpu_memory_report`
  (route `p0027`; `ro.lmk.gpu_reclaim`, set by the product, turns it on). It does
  nothing on a kernel without the file.
- The virtio-gpu display is paced by a kernel vblank timer, so the product drops
  the inherited `present_fence_not_reliable` property and the guest needs a
  kernel with the pacing. Incremental installs need a kernel with Incremental FS.
- The guest's wall clock is the host's, so the platform takes no network time;
  the verifier fails any NTP server or network time origin in the framework
  overlay.

### Time zone

A new guest starts on GMT and the platform has no automatic time zone, so the
host application passes the zone it is in at every start
(`androidboot.kernarium.timezone`, an IANA name the runtime has checked; the
standalone macOS launcher passes none). `kernarium_time_zone.sh` applies it with
`cmd alarm set-timezone` at boot completion, but only while nobody in the guest
has chosen a zone: it replaces a zone only if that is the one it applied last
(marker `persist.sys.kernarium.timezone_applied`), so a zone picked in Android
stays.

### Old application switch

Old app compatibility is a choice the person makes for one application at a time
(Andarium's Settings); the image keeps the choice and offers the command that
makes and reads it. The framework reads the choice in the application's own
process (route `p0007`), so an application that is not switched on never loads
the compatibility layer.

- **Store.** One persistent property per package,
  `persist.sys.kernarium.oldapp.<package>`: the value holds the turn-on time and
  the installation it was made for, and an empty or unset property means off.
  It lives in the data partition, so an Android update keeps it, Erase resets it
  and a snapshot restore brings it back. Its type, `kernarium_oldapp_prop`
  (`overlay/sepolicy/system_ext/private`), is a system-owned restricted
  property: the shell domain sets it, every application domain reads it (the
  shell domain is itself an application domain in this policy) and a neverallow
  keeps every other application domain from setting it.
- **Eligibility.** Not a system application, a 32-bit primary ABI, a target API
  level below 14, no shared user ID, and a package name that can end a property
  name as it is (the whole key at most 255 bytes). `OldAppEligibility` is the
  one place that says so.
- **Persistence.** A choice belongs to the installation it was made for (an
  update keeps the installation, an uninstall and a new install make another):
  the listing clears the property of a package that is absent, is not eligible
  any more or is another installation, so a choice lasts at most until the next
  listing, which Andarium makes whenever it starts, after each install and when
  its page opens. The command's changes and that cleanup are serialised by a lock
  on a file that only the shell user can open; turning off is accepted for any
  name that can be a property name.
- **Command.** `kernarium_oldapp` (`overlay/oldapp_switch`, a Java tool that
  `app_process` starts, installed on system_ext) runs as the adb shell user, with
  no root on a `user` build. It lists the eligible applications of the owner
  user with their labels and state, and turns the choice on or off for one
  package, which is device wide; an older image has no command, and the host
  hides the feature. The command's usage message and `OldAppCommand` carry the
  arguments, the listing's fields and the statuses.
- **Kept by a later release.** The property's name prefix, type and meaning, the
  eligibility and persistence rules, and the command's name, arguments, listing
  and statuses. `tests/integration/android/run_android_oldapp_switch.py`
  qualifies it ([testing](../../docs/testing.md#android)).

### Launcher applications

Andarium's Add to Home Screen puts an Android application's icon on the iOS Home
Screen, so the host asks the guest which applications have a launcher entry, with
their labels and icons, and has one started. The image carries one command for
both, `kernarium_apps` (`overlay/app_shortcuts`, a Java tool that `app_process`
starts, installed on system_ext like `kernarium_oldapp`), and one application that
draws, `KernariumIcons` (`overlay/app_icons`). The command runs as the adb shell
user, holds no permission of its own and writes nothing: no file, property or
setting. It reads the labels (string resources) and nothing else of an application's
resources: it opens no image, drawable or icon file, and `icons` only asks the
renderer for each picture and relays a PNG it has checked. It builds the framework's
system assets for the labels itself, without the static
overlays that target the framework (their idmaps come from `idmap2`, which the
shell domain may not execute), and falls back to the framework's own
initialisation when that is not possible. Reading any resources initialises the
framework's display metrics, which read `qemu.sf.lcd_density` and fall back
when the shell domain may not: the policy's one line for the tool
(`overlay/sepolicy/system_ext/private/kernarium_apps.te`) is a `dontaudit` of
that read, so no call draws a denial and the shell domain gains no permission.

- **Renderer.** `KernariumIcons` (`com.kernarium.icons`) is an ordinary application of
  system_ext: signed with the default key, in `app/` and not `priv-app/`, with no
  launcher entry, no permission, no network, no backup, and no SELinux rule of its
  own (its processes run as `untrusted_app` and `isolated_app`). Its one provider
  (`content://com.kernarium.icons`) only brokers: the platform lets the shell user
  and the system in (`ACCESS_CONTENT_PROVIDERS_EXTERNALLY`) and the provider
  refuses any other uid again. It binds its one service, which runs in an isolated
  process, and that process opens the target application's package files and
  runtime overlays and draws the icon (a fixed density, day mode and locale, so
  that a key is one picture); the PNG comes back to the tool through the provider. The shell
  user cannot bind a service, so the tool reaches the provider through the activity
  manager, for the user it listed the applications for, and takes only the image's own
  provider (package, exact class, system application), checked at every acquisition.
  A worker that dies or hangs costs the one icon (it is `error`, never
  retried at a smaller size) and the next icon is drawn by a fresh one; a provider
  that does not answer a call is no renderer at all. An image
  without the renderer, or with another package's provider for its authority, fails
  `icons` with status 1 and one line, and the tool's
  other commands are unaffected. Each start of the isolated process leaves one
  audit record, AOSP's own and tagged with its bug note: a denied `getattr` of the
  application's data directory, which has no effect. `check_icons_app` in
  `tools/verify-target-files.py` holds the package to this description.

- **Entries.** One entry is a launcher activity of the foreground user: enabled for
  that user (the package manager's own answer, so an alias the manifest disables
  and `pm enable` enabled counts), exported, handling `MAIN` with `LAUNCHER`,
  asking for no permission the shell user lacks. Its identity is `package/activity`, which an update or a
  reinstall keeps. The grammar of both names (ASCII, at most 255 bytes each) is
  the one place that says what the tool lists, takes and passes to a process.
- **Commands.** `digest` prints a header block whose digest changes with the set
  of entries, their versions and update times (a built-in application that was never
  updated also carries the image's build time, an input of its own: its files carry one
  fixed time), the foreground user and Android's language (it opens no application's
  resources); `list` prints the
  block and one row per entry with the label in Android's language list (read for one
  fixed configuration, so the list is its only input), a built-in flag and an icon key; `icons` answers one PNG record (192 by 192, adaptive
  icons composed from their layers with no mask, drawn for a fixed density, day mode
  and locale so that a key is one picture) for each of up to eight components, with
  its length and SHA-256; labels and icons come from the application's effective
  resource files for the user (splits, shared libraries and overlays); the tool
  passes on only a PNG whose signature, header (with its checksum), dimensions
  (the size asked), 8-bit RGB or RGBA, non-interlaced layout and 96 KiB bound it has
  checked, and an icon whose picture fails the check is `error`; `launch` starts exactly one entry as a
  launcher's tap does, by running `cmd activity start-activity` with separate
  arguments, and refuses (status 2) anything that is not an entry now.
  A change of the set is noticed by comparing digests, and an icon is fetched only when
  its key changed.
- **Bounds.** A label is made safe (no control, separator, bidirectional or
  invisible format character) and cut to 80 code points, and cleaned only as far as
  the cut needs; one that cannot be read or does not fit in memory is listed under its
  package name and costs no other entry; at most 8 entries per
  package, 1024 rows, 8 icons per call, 96 KiB per PNG, 20 s before no further icon is
  started and 7 s for one icon's request in the renderer (its queue, bind and drawing together: a drawing that
  hangs or exhausts memory, which its isolated process contains, is `error` for that icon
  alone) within 8 s of the tool's own, after which the renderer is taken as not there; every answer is under 1 MiB, and a command
  that fails prints nothing on standard output and one line of the tool's own words
  on standard error. A question without an answer (the foreground user, a
  permission, the configuration) fails the command: there is no substitute.
  The exit statuses are 0 done, 1 failure, 2 refused, 64 usage.
- **Kept by a later release.** The command's name, verbs, answer framing, grammar,
  bounds and statuses; an older image has no command (the shell's status 127) and
  the host does not offer the feature. The renderer's authority, request and answer
  are between the tool and the application of one image, which ship together. `tests/integration/android/run_android_app_shortcuts.py`
  qualifies it ([testing](../../docs/testing.md#android)).

### Branding and diagnostics

The Setup Wizard names the system Andarium through its runtime resource
overlay; Settings, About, the boot animation, the fingerprint and the build
properties keep LineageOS's names. LineageOS's statistics reporting is removed
rather than disabled (route `p0025` removes LineageParts' statistics feature,
route `p0026` the wizard's diagnostic data row), and the verifier fails an image
that still carries the uploader.

## Graphics

Both graphics APIs come from one Mesa build of this project's own guest Mesa
source rather than from the AOSP Mesa snapshot, so the Android guest and the
Linux guest run the same drivers against the same renderer. GLES is Mesa's Virgl
gallium driver behind `libEGL_kernarium`, `libGLESv1_CM_kernarium` and
`libGLESv2_kernarium` (`ro.hardware.egl=kernarium`); the Vulkan HAL is Mesa's
Venus driver over Virtio-GPU (`ro.hardware.vulkan=virtio`); gralloc is the
allocator's own `gralloc.minigbm` (`ro.hardware.gralloc=minigbm`); and
presentation is `VK_ANDROID_native_buffer`. The host side of this stack is in
[`../../docs/graphics.md`](../../docs/graphics.md#android-guest-requirements).

The Android guest allocates Venus memory synchronously: vendor init sets
`debug.mesa.vn.perf=no_async_mem_alloc` at early-init, before any process that
loads the driver starts, so a host GPU budget refusal reaches the application
as `VK_ERROR_OUT_OF_DEVICE_MEMORY` instead of aborting it. The verifier requires
the setting and that applications, SurfaceFlinger and the shell may read it.

The boot command line is the only writer of the three graphics HAL selections,
the OpenGL ES version and `androidboot.lcd_density`; every launcher passes the
density and a release's command line may not set it. The verifier refuses a
vendor `ro.hardware.egl`, `ro.hardware.vulkan` or `ro.hardware.gralloc`. The
display follows the host view
([`../../docs/graphics.md`](../../docs/graphics.md#dynamic-native-geometry)),
density stays at its launch value, and the touchscreen's absolute range is fixed
at 0 through 32767 so touch stays exact at every size. The iOS host's
touchscreen supports ten contacts; VNC remains a single pointer.

`third_party/mesa-venus/` pins the source as three independent inputs: the
upstream Mesa revision, the guest Vulkan series under
[`../../tools/rootfs/vulkan/`](../../tools/rootfs/vulkan/README.md), and the
Android port series (the generated Soong port and the few source changes only
Android needs). A guest replay assigns new commit identities each time, so the
guest half is pinned by tree. The HAL and the host renderer must speak the same
Venus protocol revision: a change to the protocol series or the Vulkan-Headers
pin needs a regenerated codec (`tools/generate-mesa-venus-port.py protocol`), a
port replay and a new release, and `tools/verify-mesa-venus-source.py` refuses a
pin that differs from its owner. The Android hardware buffer handle type that a
swapchain needs arrives with the guest Vulkan epoch pinned in
`third_party/mesa-venus/metadata.env`.

The product declares OpenGL ES 3.2 (`androidboot.opengles.version=196610`),
`android.hardware.opengles.aep` and `android.software.opengles.deqp.level`
132776705, the level at which every EGL, GLES2, GLES3 and GLES31 dEQP list of
the Android 17 CTS passed whole on both ABIs. A declared level makes the CTS run
every list dated up to it, so the verifier pins the level and requires the
extension pack, and a platform update that moves the `latest` level file fails
verification until its new lists pass.

The product declares Vulkan 1.4 (`level` 1, `compute` 0) and deliberately no
`android.software.vulkan.deqp.level`: the driver has not been qualified against
Android's Vulkan dEQP must-pass list, and the verifier refuses a declaration
until those lists pass. One gap is known: `vkMapMemory` of host-visible memory
exported as a hardware buffer fails with `VK_ERROR_MEMORY_MAP_FAILED`, because
gralloc backs such a buffer with a classic 3D resource that Venus refuses to map
by design. Memory an application shares as a file descriptor is not that case:
the driver offers `VK_KHR_external_memory_fd` and `VK_EXT_external_memory_dma_buf`
(the opaque-fd and dma-buf handle types) next to the hardware buffer extensions,
and host-visible memory exported as either is a blob that maps, imports and is
read by the GPU. `overlay/tools` ships pass-or-fail checks of the presentation
and buffer paths (`kernarium_vk_present`, `kernarium_vk_ahb_import`, which
covers both ways of sharing memory, and the `kernarium_ahb_*` and
`kernarium_egl_configs` programs).

## Hardware video encoding and decoding

The host's VideoToolbox encoders and decoders reach the guest as V4L2 stateful
devices (Virtio video, [`darwin/device/video`](../../darwin/device/video/README.md))
and the product exposes them through Codec2. The vendor service
`android.hardware.media.c2-service-v4l2` of `external/v4l2_codec2` (route
`p0018`, which also adds the HEVC component) serves AVC and HEVC encoders and
AVC, HEVC and VP9 decoders beside the software `c2.android.*` components;
MediaCodec chooses the hardware decoders first. The VP9 decoder lists Profile 0
and, where the host's hardware decodes it, Profile 2 (10-bit), which is shown
as 8-bit as HEVC Main 10 is. A VP9 stream of 12 bits or of 4:4:4 or 4:2:2 is
refused by the hardware decoder, so a player that picks the first VP9 decoder
without stating the profile (Media3, for a WebM or MP4 file) reports a playback
error; AV1, which the host does not decode, stays with the software decoder.
Encoder parameters come from the device, and a capability the device does not
list is not advertised. HDR content is shown as SDR because the host has no
P010 sampler.

`overlay/config/media_codecs_c2.xml` is the product's one vendor codec list. Its
performance points, the encoder floors, the instance limits and pixel rates, and
the service's SELinux and seccomp additions are required by the target-files
verifier, which also refuses a hardware decoder beyond the three. Qualification is
opt-in
([`../../docs/testing.md`](../../docs/testing.md#android)).

## Payload reduction

Payload reduction is done through product configuration and regenerated AOSP
patches, never by deleting files from a completed image.
`KERNARIUM_REMOVED_PACKAGES` in `overlay/lineage_andarium.mk` lists what the
product omits: HAL APEXes with no Darwin UML hardware, AVF guest payloads, OTA
and DSU executables, and Cuttlefish host-monitoring services. The build and the
verifier reject any excluded module that reappears. The CAS HAL APEX (CTS needs
it) stays, as do DeviceLock, Virtualization and the modular
framework containers, which the platform SDK classes link. EasterEgg stays
too: Settings' Android version row opens the platform logo screen, which starts
the egg, and the verifier requires its package.

Android suspends itself when the screen is off and nothing holds a wakelock, and
this machine's adb host arrives over the network, which only briefly wakes a
suspend ([kernel](../../kernel/common/README.md#suspend-wake-and-the-management-console)).
adbd's `p0011` topic therefore sets `sys.kernarium.adb.host_connected` while it
has an adb host, and product init holds Cuttlefish's `suspend_blocker` wakelock
for it. The wakelock holds back only Android's own autosuspend: a suspend the
host requests through the management console still suspends the guest.

## Static disk and boot contract

The guest sees exactly two disks; a third would stall boot, because first-stage
init mounts a partition by name from any Android GPT disk it finds.

- The **system disk**, `guest/system.img`, is read-only and ships with the
  release. One GPT holds `init_boot`, `recovery`, and the five EROFS partitions
  (`system`, `system_ext`, `product`, `vendor`, `odm`), each exactly its image
  rounded up to 4 KiB and packed back to back. The disk is raw and attached as
  `ubd0r`.
- The **data disk** is the guest instance's writable disk, a sparse GPT the host
  creates (see [`darwin/device/block`](../../darwin/device/block/README.md)):
  `misc`, `metadata`, a reserved gap, and `userdata` to the last MiB. The host
  writes no filesystem: first-stage init formats metadata and userdata on first
  boot. It is attached as `ubd1`, with discard.

`support/image/disk-policy.json` is the tracked policy: partition names, GUIDs,
packing rules, and the data disk template with its capacity range (4 through
128 GiB, 8 by default). `tools/ubd-disk.py assemble` turns it and the built
images into `system.img` and the release's `partition-layout.json` (schema 2).
The host reads only that file, never the policy, so a layout is a release
property: changing a size or an offset needs no host change. The host does not
hash at boot; the installer verifies a release once.

The data disk only grows: the host raises its size and rewrites its GPT, and
Android grows the mounted `userdata` filesystem online at every boot. Recovery
does not grow userdata, and neither guest rewrites a live partition table. There
is no `super`, dynamic partition, Virtual A/B, AVB, OTA slot, or DLKM partition.

The five read-only partitions are compressed EROFS (Zstandard level 15, 16 KiB
physical clusters on 4 KiB blocks), and so is every APEX payload except the
presigned CTS shim APEX. The parameters are written once as
`UML_ANDROID_EROFS_COMPRESSOR` and `UML_ANDROID_EROFS_PCLUSTER_SIZE` in
`overlay/lineage_andarium.mk`, and routes `p0023` and `p0013` give AOSP's EROFS
tools Zstandard support. `userdata` is an ordinary unencrypted ext4 filesystem:
the emulator exposes no hardware-backed KeyMint and the host application sandbox
protects the bundle.

`init_boot` and `recovery` are kernel-less boot-image-v4 ramdisks from the signed
AOSP target files; `recovery` is the stock LineageOS recovery adapted to the
virtual hardware (`overlay/recovery/`, route `p0016`). The kernel stays outside
the disk in the selected runtime artifact. Boot mode `auto` follows the standard
command in the writable `misc` partition and is the launcher default.

A guest reboot restarts the guest in the same host process, so `adb reboot`,
`adb reboot recovery` and `adb reboot bootloader` all return to a running guest.
Every restart reads `misc` again: `boot-recovery` boots recovery, which clears
it, and no command boots Android. There is no bootloader, so the launcher
behaves as a bootloader that continues and clears a `bootonce-bootloader`
command. A guest that restarts five times in a row within 15 s of booting, or
eight times in a row each within 15 s or before its boot had finished
(`sys.boot_completed`, recovery's start or apexd's `markBootCompleted()` on the
console, once init's output was seen there), ends the launch with exit status 3;
restarts after a finished boot that lasted 15 s or more are not limited. A
recovery that restarts after that window while `misc` still asks for recovery
is not ended by the guard, because recovery's start counts as a finished boot.

`build/assemble-image.sh` owns ramdisk composition, system disk assembly and
verification, and `tools/verify-lineage-recovery.py` checks the recovery image.

## Release installation

`support/release/release-manifest.schema.json` defines the compact rootfs
release. It binds the system disk and its layout, boot ranges, command line,
target-file support, source provenance and, when the release has them, the
[Magisk root files](#root-files). The Android section records the
security patch level and the build number (`ro.build.version.incremental`),
which a release sets to its release number (`build-aosp.sh --build-number N`:
`r26` is 26 and `r10000` is 10000, up to ten digits), and so does every proof
build or rebuild of that release. The host orders releases by it and refuses a
downgrade ([Guest instance storage](#guest-instance-storage)). The checked
`metadata/accepted-release.env` selects one reviewed release without recording
a builder path.

The manifest also records, as `outer.input_trees`, the Git object ID of every
path that [`support/release/input-paths.tsv`](support/release/input-paths.tsv)
marks `input`: the overlay, product configuration, patch series, route table,
support files, base manifest and the producing tools, each with its reason in
the list. An object ID follows content, so it survives a history rewrite and a
builder that works from an exported snapshot; the publisher takes it from the
clean checkout it runs in. The accepted descriptor repeats the IDs
(`LINEAGE24_ACCEPTED_INPUT_TREES`), and `install-release.py --print-acceptance`
prints the whole descriptor for a candidate. Andarium's Release configuration
compares them with the commit it archives
([`scripts/check-embedded-artifacts.py`](../../scripts/check-embedded-artifacts.py)).
Verifier-only tools, documentation and tests are excluded, and a tracked file
under this directory that the list does not classify fails
`lineage24-release-inputs`. A release of schema 6 predates the IDs and records
none, so the check refuses it as unverifiable.

After copying a reviewed candidate to the workstation, install it with:

```sh
android/lineage-24/tools/install-release.py \
  --source /absolute/local/path/to/release
```

The installer validates the candidate, including every system disk partition
hash, and installs it atomically below `.build/android/releases`. It never
replaces an existing release. `--aot-artifact` also installs the release's AOT
artifact (`tools/aot/release_artifact.py`) below `.build/android/aot`, once it
verifies against the release; `metadata/accepted-aot.env` accepts it, and its
`LINEAGE24_ACCEPTED_AOT_ARCHIVES` field is whether an archive of Andarium's
Release configuration may carry it. The artifact is not part of the release's file
inventory. Apple CMake does not import the rootfs release
and does not invoke Repo, Soong, Kbuild, image assembly, publication, or
transport.

The release manifest keeps the kernel build that qualified its target files as
historical provenance only. Apple runtime selection uses an independent
compatible Android kernel artifact and compares no rootfs, kernel commit or
emulator release at launch, so a kernel-only update advances only
[`accepted-android-4k.env`](../../kernel/mainline-6.18/artifacts/accepted-android-4k.env)
and keeps the accepted release.

### Release signing

Every `user` build is signed with one private key set that never enters the
repository, a log or a release; there is no unsigned publication and no key
generated per build. `tools/publish-release.py --signing-keys DIR` takes the
unsigned target files and `DIR`, a directory owner-only (no group or other
access) that the caller deletes; the tool copies no key and refuses to sign any
other variant. It signs in a private work directory with the AOSP
`sign_target_files_apks`, assembles the system disk from the signed archive, and
deletes the work directory whether or not signing succeeded. `--disk-dir`,
`--mkbootfs`, `--mkbootimg` and `--first-boot-artifacts` are required.

The signed archives name the release certificate `keys/releasekey.x509.pem`
(`verify-target-files.py --signed` checks it), so no build-host path or temporary
name enters a release. Signing fails when any file stays signed with a key
outside `DIR`, and the tool requires the system's and the recovery's
`otacerts.zip` to trust the release certificate and no public AOSP or LineageOS
certificate. A presigned file keeps its upstream signature and is named in the
build receipt. The key directory, its backup and the transfer to the builder are
the maintainer's responsibility.

### Root files

A release can carry the Magisk root files of its own `init_boot` under `root/`:
`init_boot-magisk.img`, that image as Magisk's patcher makes it, and the
unmodified official `Magisk-<version>.apk`. The [`metadata/magisk.env`](metadata/magisk.env)
pin names the Magisk release by name, version and the SHA-256 of its APK; it is a
release input, the APK is given to the producer as a local file, and no Magisk
file is in the repository.

`build/assemble-image.sh --magisk-apk APK` makes the files on the builder after
the disk is assembled (`tools/magisk-boot.py`), and `tools/publish-release.py`
takes `--magisk-apk APK` or `--without-root`, never neither. A schema 8 manifest
records them as its optional `root` object, which its
[schema](support/release/release-manifest.schema.json) defines, and lists both
files in its inventory. A release without root files is valid, as is every
release of schema 6 and 7, which cannot carry them.

`tools/verify-magisk-boot.py` is the one verifier. The producer, the installer
and `Andarium/scripts/stage-system-image.py` run it, and it refuses by name a
`root/` that is partial, holds anything beside the two files or is reached through
a link, a file that is not the recorded one, and a patched image that is not its
own disk's `init_boot` plus exactly what Magisk adds.

A schema 8 accepted descriptor binds the root record
(`LINEAGE24_ACCEPTED_ROOT_RECORD_SHA256`: a digest of it, or `none` for a release
without root files), so a release with root files and one without never share a
descriptor. `install-release.py --print-acceptance` also checks the candidate's
Magisk release against the tracked pin; an accepted release installed later is
history and is checked against its descriptor alone.
`scripts/check-embedded-artifacts.py` ties the staged and the packaged root files
to the installed accepted release's, and that release to the descriptor: a
schema 6 or 7 descriptor admits no root files, and a schema 8 one must name the
installed record's digest, or `none`.

### Superseded releases

After a replacement release is validated, remove the superseded Android build,
release, transfer, and guest-bundle artifacts from the local `.build` tree,
local temporary directories, and the Android build machine. Resolve releases
and bundles from their manifests and build outputs from their recorded
invocation and canonical output root, and delete only explicit Android paths.
Preserve the current accepted release and its active writable bundle, the
Android source and patch-producer checkouts, accepted kernel inputs, dependency
prefixes, non-Android guests, and unrelated files beside a bundle.

## Storage accounting

The framework reports exact filesystem bytes for the static virtual disks:
`StorageManager` sums `/data` and the five mounted OS filesystems, and Settings
derives its used space and System category from those counts. Retail capacity
rounding does not apply to these virtual disks. The `frameworks/base` storage
topic in route `p0007` owns this policy.

## Guest instance storage

A guest instance is the writable half of a guest: a directory holding the data
disk, `data.img` (the creation and growth rules are in
[`darwin/device/block`](../../darwin/device/block/README.md)), `identity.json`,
the installation's own serial number and eth0 address (made by the host and kept
with the disk,
[`darwin/device/block`](../../darwin/device/block/README.md#installation-identity)),
and `instance.json`, which records the release the disk was last started with. The
system disk is not part of an instance. Every launcher that keeps instances,
the macOS script and the Andarium app, applies the same rules before each
start; [`tools/android_instance.py`](../../tools/android_instance.py) is the
reference implementation and
[`support/instance/update-decisions.json`](support/instance/update-decisions.json)
holds decision vectors and the record's transitions that any other
implementation must reproduce. The release's identity comes from its manifest
and layout, never from hashing the disk at launch.

The build number orders releases; the rules compare nothing else. Numbers
compare as integers, except that `2026110800` to `2026110899`, the counter
the first public releases were built with, are older than every number outside
that range (`LEGACY_BUILD_FIRST` and `LEGACY_BUILD_LAST` in the reference
implementation, `BuildNumber.legacyRange` in AndariumCore, and
`legacy_build_numbers` in the vectors). Before a start:

| Case | Action |
| --- | --- |
| no instance | create it; the host creates the data disk, which the guest formats on first boot |
| same release | start |
| newer build, same `data_layout` | snapshot, record the pending update, start; Android upgrades its own data |
| newer build, newer `data_layout` | snapshot, run the registered migration (none exists yet), start |
| older build | refuse: Android's key store, package manager and apexd do not go back |
| same build, another release or system disk, or a `data.img` without `instance.json` | refuse |
| an unfinished update, same build | retry with its snapshot kept, restore, or reset |
| an unfinished update, newer build | restore the pre-update snapshot, then update directly to the newer build |

The Andarium app adds one rule that the reference implementation and the
vectors do not carry: an unsigned proof image (a manifest whose `acceptance` is
`unsigned-proof`) never updates an instance that has data, and an instance a
proof created never updates to another system. Both are refused as a mismatch,
also when a proof and a signed release share a release ID, and Erase is the way
out; Set up does not replace such files either. The record
of an instance a proof created is schema 2 with `unsigned_proof` true, which a
reader of schema 1 refuses, and a signed release's record stays schema 1.

An update is finished when the host first sees the guest reach
`sys.boot_completed`: the pending update is cleared and the snapshot deleted. A
newer release after that updates from the data as it is, with a snapshot of its
own. Any older build can update directly to any newer build.

The snapshot, `data.img.pre-<release_id>`, is an APFS clone (a volume without
clones refuses the update) that lives only while the update is unfinished; a
restore puts back the data the new release started from and does not boot the
previous release. An update whose first start fails offers three ways out: start
it again keeping the snapshot, restore the snapshot and update afresh, or reset
the instance (delete it and create it anew), which is also the way out of a
refused downgrade. A future data layout changes the partition table only inside
the reserved gap, never moving `userdata`. The app decides whether to exclude the
instance directory from backup.

## Apple consumers and guest bundles

The macOS Android product is configured with the `macos-android17` preset. Its
build requires a compatible Android kernel artifact; launch takes any rootfs
directory with the expected payload layout and one directly writable GPT image.
It does not alter the normal Linux executable or search for `rootfs.erofs`.

[`../../tools/boot_android.py`](../../tools/boot_android.py) is the ordinary
macOS launcher. It selects the accepted installed release and a guest instance
(see [Guest instance storage](#guest-instance-storage)), applies the update
rules, and starts the emulator on the release's system disk and the instance's
data disk. A disk never shrinks. The launcher exposes guest ADB only on host
loopback; `tools/boot_android.py --help` lists the session options, and the
guest records silence unless `--audio-input host` records from the Mac's
microphone (see the [audio device README](../../darwin/device/audio/README.md#microphone)).

Kernarium represents one Android guest as a `.kern` directory containing:

- `release-manifest.json`;
- `boot/kernel-command-line.txt`;
- `guest/system.img`, the release's read-only system disk, and
  `guest/partition-layout.json`, which describes it; and
- `guest/data.img`, the guest's writable data disk, which the runtime creates at
  the first launch and only grows afterwards, and `guest/identity.json`, the
  installation's identity, which the runtime makes beside it.

The kernel is supplied by the runtime framework and is not bundle payload. The
manifest is provenance rather than a runtime compatibility token. Android
appends the host-owned display density, hardware timeout multiplier, ADB
authorization policy, ADB host key, ADB TCP policy, and tagged-pointer option at
launch, and launches write
persistent guest changes only to `guest/data.img` and `guest/identity.json`. How Kernarium keeps,
relaunches, and copies a bundle is in
[`../../Kernarium/README.md`](../../Kernarium/README.md#guest-storage).

Andarium carries the accepted release instead:
`Andarium/scripts/stage-system-image.py` checks it against its own manifest and
stages the system disk, the partition layout, the kernel command line and the
manifest, at their release paths, for the application to bundle. See
[`../../Andarium/README.md`](../../Andarium/README.md#build).

## Validation boundary

Producer verification covers pinned source/patch identity, target files, VINTF,
SELinux policy, executable alignment, module exclusion, partition layout, boot
ramdisks, and release inventory. Runtime boot, persistence, recovery, graphics,
input, networking, connected-device memory limits, and application compatibility
are separate Apple-product gates.

Historical boot, ART, memory, and topology experiments are summarized in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
