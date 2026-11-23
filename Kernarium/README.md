# Kernarium

Kernarium is the iOS application that runs one Linux or Android guest on the
Darwin runtime. It targets the iOS device, whose slice also runs on an Apple
silicon Mac in Designed for iPad mode (macOS 27.0 and later:
`LSMinimumSystemVersion`), and the iOS Simulator, where the guest
renders on the CPU for UI tests
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)). Build,
launch, and test commands are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium).
[Andarium](../Andarium/README.md) is the Android-only application.

## Ownership

```text
Kernarium/            SwiftUI application: settings, guest selection, console,
                      microphone setting, background running and picture in
                      picture
Runtime/              runtime framework target, one per kernel flavour
KernariumTests/       hosted application and runtime tests
KernariumUITests/     UI tests
KernariumAutomation/  long-lived UI-automation host for a connected device
AudioInterrupter/     test-support application that takes the audio session
scripts/              launch, packaging, bundle assembly, and automation tools
```

The runtime loader, the guest display and its input, the zstd utility
(`ZstdUtil`), the microphone permission and the shader-cache bound are not in
this directory: [`GuestHost/`](../GuestHost/README.md) holds them, compiled
into this application and into Andarium.

## Runtimes and runs

Kernarium uses `6.18`-series runtime frameworks for Linux
(`KRuntime_6_18_4K`) and Android (`KRuntime_Android_6_18_4K`), both with 4 KiB
guest pages. Their names stay stable across patch releases and kernel commits,
and each carries its kernel ELF in signed Mach-O `__TEXT` (see the
[architecture](../docs/architecture.md#memory-and-executable-code)). The
application embeds every framework, links none at launch, and loads exactly
one for the process lifetime. Packaging includes each runtime's dSYM in its
XCFramework and then strips the framework binary with `strip -S`, which drops
the linker's debug map (it names the build host's directories) and keeps the
UUID the dSYM matches. The dSYM alone also names the kernel's functions, at
their place in `__TEXT,__uml_elf` (`scripts/kernel-dsym-symbols.py`).

Kernarium makes one run per launch. A guest that restarts itself is booted
again inside that run, in the same process, and the run returns only when the
guest is over; the application then says how it ended and asks to be
relaunched, and never starts a second run in the same process. The guest runs
on the named `uml-launcher` thread, which leaves the main thread to UIKit and
MetalKit. The restart loop and its guarantees are in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle); the
application learns of a restart from `set_restart_callback` and re-applies what
belongs to a kernel (the page-cache trimming and the vCPU throttle) to the new
one. The defaults for a new launch are in
`Kernarium/Kernarium/EmulatorConfiguration.swift`.

## Memory

The host GPU's budget is fixed at launch from the guest's memory, the process's
memory limit and three launch parameters, and the settings offer no more guest
RAM than the limit leaves for the chosen vCPU count
([architecture](../docs/architecture.md#memory-and-executable-code) owns the
formulas; [graphics](../docs/graphics.md#device-memory-budget) what the budget
bounds). The application states the parameters in the boot request, asks the
runtime for the plan (`plan_gpu_budget`) before the run and shows a dismissible
notice when the limit leaves the GPU less than its floor; nothing refuses a
launch for lack of memory. Only a guest RAM the runtime does not admit
(`kernarium_runtime_loader_guest_memory_admitted`, the runtime's own rule) ends
a start, before the run and with a notice that says why; the settings never
offer such a size, and where the loader's bound is zero (the process has no
room for even the smallest window) they offer no RAM stepper and the notice
gives the reason alone, and saving the settings keeps the stored size (a zero
an earlier version stored counts as none: the default applies). The
settings ask the loader
(`kernarium_runtime_loader_maximum_guest_memory_mib`) for the RAM bound and a
new configuration's guest RAM
(`kernarium_runtime_loader_default_guest_memory_mib` at the loaded vCPU count,
which stops at 4 GiB and also keeps the GPU 992 MiB on a device that offers 4
GiB), because the runtime framework is loaded once per process. The parameters
default to the conservative launch set the default is measured with; the RAM
bound counts none of them. The `KERNARIUM_GPU_BUDGET_*_MIB` and `KERNARIUM_GUEST_MEMORY_MIB`
environment variables state values for one measurement launch and are never
stored.

The runtime answers the system's memory pressure itself
([architecture](../docs/architecture.md#memory-and-executable-code)). It also
reports every level the system delivers to the application, with whether the
host responds (a critical level) or is already responding, through
`set_memory_pressure_callback`
(runtime API 41): once for each notification, before that notification's work,
and with no figures. The system merges the levels raised during a response into
the next notification, so a level raised during a response is reported after it.
The loader queues each report to the main queue as its own block and setting or
clearing its handler does not cancel the ones queued. Neither application sets
a callback; the runtime API keeps it for an application that wants the reports.

## Guest storage

A Linux guest boots an image file in the application's Documents directory:
the one the settings name, or, while they name none, the accepted KDE desktop
image `kde-20260929-r2.erofs`
([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)). A name the
settings already store is kept, and every image in Documents stays selectable.
An Android guest is a `.kern` bundle directory in Documents; the
[Android README](../android/lineage-24/README.md#apple-consumers-and-guest-bundles)
owns its layout. A bundle holds the release's read-only system disk and the
guest's persistent data disk (`guest/data.img`), which the runtime creates at
the first launch and every later launch reopens, so rebuilding the application
or a runtime framework must not erase it and a bundle is never recreated merely
to relaunch it. When copying a bundle to a device, target the complete
`Documents/<name>.kern` destination; never copy into or replace the Documents
directory itself, which can flatten the bundle or destroy other guests.

Launch does not compare kernel, runtime, or rootfs release identities; missing
or malformed inputs fail at their ordinary file or image boundary. The
application chooses the Android data disk's capacity and passes it with the
boot request; the runtime creates the disk or grows it to that capacity, keeps
a disk that is already larger, and attaches exactly the system disk and the
data disk (the
[static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)
owns the layout). Compressed delivery is an application concern: `ZstdUtil`
publishes its destination only after a complete decode.

## Guest boot

A Linux run boots the base archive the runtime embeds
([`tools/rootfs/boot-ramdisk`](../tools/rootfs/boot-ramdisk/README.md))
followed by a segment holding the generic per-run `/init` and its
`/boot.env`; the runtime writes that initrd to its temporary directory and
removes it when the run returns. The parameters mount the image read-only on
`ubd0` beneath an overlay whose writable layer is an ext4 filesystem formatted
afresh on `ubd1` (the sparse `kernarium-overlay.ext4` the application recreates
in its temporary directory at each launch). `/init` then switches root into the
image's own `/sbin/init`, which boots `graphical.target`.

Nothing is typed on the console and the runtime edits no image: accounts,
device groups, getty and display-manager policy, network and resolver belong
to the image. The KDE, Phrog and postmarketOS images Kernarium boots carry
that setup ([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)); the
unified `rootfs.erofs` carries none of it and boots to its own defaults.

## Automation runs

The release runtime accepts no host payload and carries no test campaign.
Test and benchmark drivers package one of two automation variants instead; the
packaging script (`scripts/package-ios-frameworks.sh`) checks that a release
runtime carries none of the settings below.

- **Linux automation**: the `kernarium-ios-device-automation` preset
  (`UML_KERNARIUM_AUTOMATION_HARNESS`), packaged with `--automation`. Only it
  reads `KERNARIUM_BOOT_PAYLOAD` (a plain file name in Documents: a padded newc
  segment of at most 64 MiB, written by `uml-initramfs`, appended after the
  runtime's own so its files replace the runtime's), `KERNARIUM_SYSTEMD_UNIT`
  (the target booted instead of `graphical.target`) and
  `KERNARIUM_PAYLOAD_REMOVE=1` (the runtime deletes the payload once it holds
  it, because `devicectl` cannot remove a file in an application container). A
  payload also attaches the artifact relay, whose frames the runtime prints on
  standard output. `tests/integration/support/kernarium.py` writes payloads for
  the lanes in [`testing.md`](../docs/testing.md#apple-builds-and-kernarium)
  and [`tests/perf/README.md`](../tests/perf/README.md).
- **Android automation**: the `kernarium-ios-device-android-automation`
  preset, packaged with `--android-automation`. The release runtime always
  starts the guest with `androidboot.kernarium.adb_auth=1`, so a fresh bundle's
  first ADB connection waits for the "Allow USB debugging" prompt. Only the
  automation runtime reads `KERNARIUM_ANDROID_ADB_AUTH`: `0` starts one launch
  with authorization off, as the macOS launcher does. It refuses that unless
  `KERNARIUM_HOST_FORWARD` is unset or listens on loopback, because
  authorization off gives whatever reaches the forward an unauthenticated shell
  (and `adb root` on a userdebug image). A loopback forward is reached by the
  processes of the device and by no guest process: the user network gives the
  guest no path to the host's loopback or to any address of the host
  ([`darwin/README.md`](../darwin/README.md#user-network)).
- Both automation runtimes also read `KERNARIUM_BACKGROUND_JOB_AT_START` and
  `KERNARIUM_BACKGROUND_HOLD_DELAY_MS`, which start a background job with the
  guest and move the GPU hold for a measurement; the packaging script refuses a
  release runtime that contains their names.

The launcher alone tells an Android guest how adbd is reached, on its kernel
command line; a release's command-line file cannot set these arguments. The
boot request carries the adb host key to authorize for that boot
(`android_adb_public_key`, runtime API 19) and the host's time zone
(`android_time_zone`, API 23: Kernarium sends none,
[Andarium](../Andarium/README.md#runtime) the system's zone), and a launch with
no `KERNARIUM_HOST_FORWARD` has adbd listen on vsock alone. The request also
states what the host has that the guest is to find (`android_host_features`,
API 31): the runtime turns each bit it knows into one kernel argument, from the
table in `KernariumHostFeatures.hpp` (the haptics bit becomes the hardware SKU
`haptics`, which declares the guest's vibrator; the orientation bit, API 37,
becomes `androidboot.kernarium.orientation=1`, which lists the guest's
device-orientation sensor; the compass bit, API 38, becomes
`androidboot.kernarium.compass=1`, which lists its magnetometer; the
high-refresh bit, API 44, becomes `androidboot.kernarium.high_refresh=1` and
also makes the display's one mode 120 Hz on an iOS device whose screen can show
it, see [Display refresh rate](#display-refresh-rate)), and ignores the others;
Kernarium sends none. The
runtime validates the values (`KernariumAdbChannel.hpp`,
`KernariumGuestTimeZone.hpp`). An Android guest has its own serial number and
eth0 address, which the runtime keeps in `identity.json` beside the data disk
([the identity](../darwin/device/block/README.md#installation-identity)); a
request states values only for a run that needs fixed ones
(`android_serial_number` and `android_ethernet_mac`, API 42). The launcher
states the release an Android guest boots to the emulator, which the
ahead-of-time component needs; a request keeps it out with
`android_aot_disabled` (API 45), as Andarium's Home switch does, and Kernarium
sends none. It also asks whether the guest boots the release's root ramdisk
(`android_root`, API 46; zero, which Kernarium sends, boots the release's own):
the ramdisk is the release's Magisk-patched file and no request names a path
([the boot selection](../darwin/device/block/README.md#boot-selection)).

## Devices and display

Kernarium attaches:

- an absolute touchscreen and an evdev keyboard (Linux key codes 1 through
  127, plus Back and Home for Android's navigation keys);
- Virtio-Sound playback and capture, which records silence unless the user
  turns on Microphone;
- an Android-only Virtio battery reflecting the Apple host's battery;
- SLiRP networking from the verified iOS prefix, whose counts of DNS, DHCP and
  TCP set-up go into the session record, beside the denial of the network that
  the application states (`set_network_notice`, runtime API 36);
- a Virtio vsock device ([the device](../darwin/device/vsock/README.md)),
  through which the application opens streams to guest ports (`connect_vsock`,
  runtime API 19; the guest cannot connect to the application);
- an Android-only Virtio GNSS receiver ([the device](../darwin/device/gnss/README.md)),
  which gives the guest the position the application reports
  (`report_location`) and tells it when the guest powers the receiver
  (`set_location_receiver_callback`; runtime API 32). The Linux runtime keeps
  both entries and answers `-ENODEV`;
- an Android-only Virtio camera ([the device](../darwin/device/camera/README.md)),
  made after the GNSS receiver when the host has a front camera. The runtime
  discovers the host's cameras, which needs no permission, and names the layout
  that describes them as `androidboot.product.vendor.sku`, a kernel argument a
  release's command-line file cannot set; it composes it again at each boot,
  from the colour data the run has learned since. The boot request states the
  mode (`android_camera_mode`, runtime API 37): live opens a capture session
  only while a guest app has a camera open, and blocked lists the same cameras
  with black frames, never opens a session and reports Android's camera lens
  cover on the side keys. The application asks the system for access and states
  live only when it has it, and nothing changes the mode before the guest powers
  off. The camera tier follows the launch's GPU budget within the cap (the small
  tier for every host), live cameras take their host-side memory out of the
  GPU's pool (the use the composed layout lets a session hold, which the
  application asks for before the boot with `plan_camera_reserve`, runtime API
  40: the runtime discovers the cameras without a permission or a session,
  keeps them and the answers composed from them until asked to discover again,
  and answers the figure the next boot would take while the source, the
  cameras and the calibration data are unchanged, or the tier's ceiling, saying
  so, where it cannot discover them; a host with no camera and a Linux guest
  answer 0), the colour data a Bayer
  still teaches is kept in
  `android_camera_calibration_path`, the state of the capture (idle, streaming,
  interrupted, interrupted by iPad multitasking, by system pressure, and, for a
  blocked guest, blocked in use while an app has a camera open, which opens no
  session; runtime API 39) comes back through `set_camera_state_callback` and
  `set_camera_rotation` states how the device body is turned from portrait where
  the host owns the guest's rotation: each boot derives every camera's sensor
  orientation from it, and a change after the boot turns the picture by the
  difference ([orientation](../darwin/device/camera/README.md#orientation)). A
  runtime serves the synthetic camera when
  `KERNARIUM_CAMERA_SOURCE` names it, serves the host's cameras with only the
  back ones openable for `host:back` (the front camera is listed, so the
  layout is an image's, every request to open it is refused before the host,
  and the runtime says at start that the filter is installed), gives the guest
  no cameras for any other value, a typo of that name included (never the
  host's), and prints the ROTATE_AND_CROP settings that reach the cameras and
  the process's footprint when a capture session opens, closes and after each
  still. The Linux runtime keeps both entries and never calls back;
- an Android-only hardware keyboard, mouse and pointer
  ([the devices](../darwin/device/input/README.md)), the last Virtio devices,
  unplugged until the application plugs them (`set_input_connected`) and then
  fed with `send_keyboard_usage`, `send_pointer` and `send_mouse`; the guest's
  lock lights and pointer capture come back through
  `set_input_status_callback` (runtime API 33), and so does whether the guest
  has registered the keyboard and the pointer (runtime API 35), off again at a
  guest restart and at the run's end. The Linux runtime keeps the entries and
  answers `-ENODEV`. Kernarium itself uses none of them;
- a Virtio-GPU Metal framebuffer; and
- the guest's `ttyS0` stream mirrored to a terminal view.

Point display mode maps safe-area points to guest pixels at 160 DPI; Retina
mode multiplies both geometry and density by `nativeScale`. The GPU's EDID and
Android's logical density on the kernel command line use that launch geometry.
The touchscreen's absolute range is fixed at 0 through 32767 on both axes, and
Kernarium maps the guest's visible rectangle in the view onto it.

A host short of the space a hibernation image takes quiesces with
`quiesce_resident` (runtime API 25), which keeps the guest's RAM in the process
([`docs/architecture.md`](../docs/architecture.md#hibernation)).

A contact the host takes away, rather than ends (UIKit's cancelled touches, a
display mode change, the view leaving its window), is cancelled with
`cancel_touch` (runtime API 24), not released: the touchscreen turns the slot
into a palm before the lift, so Android reports `ACTION_CANCEL` and libinput a
cancelled touch, and no tap or click follows (`guest-touch-cancel`,
[`docs/testing.md`](../docs/testing.md)). Known limits: X11 has no per-touch
cancel, so an X11 application under Xwayland can still read a cancelled touch
as a click, and Android's Recovery menu ignores the tool type, so a cancelled
still tap there still selects the item under it
([`android/lineage-24/README.md`](../android/lineage-24/README.md)).

The Metal and Mesa shader cache policies are in
[`graphics.md`](../docs/graphics.md#metal-shader-cache). The guest view follows
the scene's geometry (windows, tiling, Slide Over, Full Screen Apps and
rotation). Kernarium runs one guest per process and so has one window: its
scene manifest does not support multiple scenes.

### Display refresh rate

The guest's display has one mode, 60 Hz. When the boot request carries
`KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH` (runtime API 44), the runtime
decides once, at boot, whether this run has 120 Hz (`grantedRefreshRate` of
`NativeDisplay`): the boot view's own screen must show at least 100 frames
a second (a screen between 61 and 99 is refused), and the boot size's 120 Hz
detailed timing must fit the EDID (`darwin/device/gpu/edid_timing.hpp`; the
12.9" and 13" iPad Pro at native size do not). Granted, an Android guest gets
one 120 Hz mode (the 60 Hz bytes do not change) and
`androidboot.kernarium.high_refresh=1`. Refused, the run is a 60 Hz run in
every respect (no bit, no property, no pacer), with a line on stderr, and macOS
and visionOS always refuse. `display_refresh_hz` of the runtime's statistics
(120 or 60; 0 with no display) is what the run was granted, which the
application follows. After a resize, the mode's rate follows the mode the
runtime makes: 120 only where granted and the new size fits the EDID.
At 60 Hz, and on a Mac, a present follows each guest flush. At 120 Hz a display
link on the view's own window scene (it follows the view to another scene,
never makes one on a scene that is disconnecting, and a missing link or scene
leaves a retry) paces the presents (`darwin/device/gpu/present_pacer.hpp`): it
runs only while frames wait and asks iOS for the rate the guest renders at in
two steps, 60 and 120 frames a second. A guest above 62.5 frames a second,
judged over a second, has the 120 step, and one at 60 or less the 60 step,
which shows every frame the setting off does. The 120 step is left only after
a fall measure over two seconds is below 61 and the one over a second no
longer supports it, and a step changes no sooner than two seconds after the
last change since the link woke (a wake forgets it, and a ceiling that forbids
the step lowers it at once), so a guest that alternates across the threshold,
or whose frames arrive early and late, changes the step at most once in each
half of its cycle (halves of two seconds or less) and a steady speed-up
changes it once. Once the link has settled, a steady guest of 60 or less, or 63
or more, makes no change. One between them never changes the step twice within
the two seconds: with frames early or late by up to 4 ms it changes it once at
most, and with 4 ms of jitter in the interval between its frames it can change
it up to five times in 12 s, at rates from about 60.05 to 62.05 frames a
second. The rate is judged on the frames the display could show: a tick counts
no more of the frames that arrived since the tick before than its time holds at
120 Hz, and two at least, at the 60 step's ticks as at the 120 step's. A burst
within one on-time tick counts two at most (one that straddles two ticks, or
arrives at a late tick, counts more). A guest at 60 with 20 frames at once at
one point of each second, or at 63 with 3 to 12 of them bunched at one point of
each second, counts 62 or fewer: it does not take the 120 step from the 60 step
and keeps the 120 step it has (after a wake) while its count is 61 or more. A
guest whose bursts of two or more frames, each within one tick, come 31 or more
times a second takes the 120 step and keeps it. The time from a change of the
guest's rate to the link following it is the later of the dwell since the last
change (2 s) and the window after the change (1 s for a rise, 2 s for a fall)
for any guest whose counted frames pass a threshold in every window of a second,
however they bunch; just over the rise threshold the rise can come later (at 63
counted a second by up to half a second, at 64 by about a tenth). One whose
counted frames stay a frame under a threshold holds the old step as long as they
are. Every time here is the time plus one tick of the link. The ceiling
is the rate the EDID's timing states for the mode (the
largest clock the field holds, for a size beyond it), the screen's maximum and
Low Power Mode; from 100 it is the 120 step, and any lower rate, a mode of 47
Hz included, is the 60 step. A frame whose draw found no drawable is drawn
again, 30 times at most and until its draws have waited 0.9 s in all for a
drawable. A draw asks the layer for a drawable once, and only while the frame
it claims has budget left (a layer waits a second for one that the display
holds), so the thread that draws is held under 1.9 s for a frame, the 0.9 s
and one wait of the layer, not half a minute; a drawable that arrives late is
drawn into. The budget is the claimed frame's, so a frame that arrives while a
tick decides is not given a second one; the debt outlives a link made again,
and only a draw of another frame renews it. The link pauses after 100 ms
without a frame. When the
ceiling is below the mode's rate the guest is held, as under
Low Power Mode. `CADisableMinimumFrameDurationOnPhone` in the application's
`Info.plist` lets an iPhone exceed 60. What the guest does with the property
is in [`android/lineage-24/README.md`](../android/lineage-24/README.md).

### Microphone

The Microphone switch in Emulator settings is off by default; while it is off,
or iOS has not granted access, a guest that records gets silence and nothing
opens the microphone. Turning it on is the one place iOS is asked for access
(`MicrophoneSetting`); the application then tells the runtime
(`set_microphone`), which never asks. While the switch is on, the input also
opens when a camera opens in video mode, ahead of the recording, and closes
when video mode ends, unless a recording is running, which keeps it open. A
guest kept running in the background may keep recording under the `audio`
background mode.
[`../darwin/device/audio/README.md`](../darwin/device/audio/README.md#microphone)
owns the device side.

## Lifecycle

Kernarium asks the guest to write back when the application loses focus,
quiesces it before the application is backgrounded or terminated, and resumes
it on foregrounding. A key press, and a game controller's button through
`wake_for_input` (runtime API 34), also wake a guest that suspended itself, as a
phone wakes to its keyboard; a touch does not. The runtime side of sync,
quiesce, resume, and wake is in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle). The
calls report what the guest confirmed (`kernarium_runtime_settle`), and the
application logs the guest's storage as settled only when the guest confirmed
its writeback or none ran (`GuestSettle`).

Every host-side end of the running guest asks it to write back first: the
screen closing and Shut down below both come to `EmulatorLauncher.stop()`,
which states the intent at once and, off the main actor, has the guest write
its storage back (the runtime's `sync`, once, for four seconds at most) before
the host stops it, again until the run has ended (`RunStop`, `StopSettle` and
`GuestStopDelivery`: [GuestHost](../GuestHost/README.md)). A delivery that
finds no guest goes on to the host's stop and the next asks again. Termination
quiesces the guest as above, and nothing else ends a run from the host. A
write-back worker that passed the run's check and is delayed past the run's
end can ask the next run for one write-back, and the four seconds do not
bound the host-file flush.

iOS revokes the host GPU within seconds of `didEnterBackground`, and a guest
request that reached Metal afterwards would make the guest's Vulkan device
lost for good. The application therefore holds the guest's GPU requests
(`GuestGPUHold`) inside `didEnterBackground`, before it quiesces, and releases
them when it returns to the foreground. The hold and the quiesce share the
handler's four-second budget (the hold's own wait for a running request, and
for an Android guest a camera video that runs, which the hold ends and waits for
the guest to stop, two seconds at most, are taken out of it); the quiesce carries
on under the `guest-sync` background task. The hold is given the edge's deadline,
and its camera wait takes what that leaves after the drain's second, two seconds
at most (the drain always has its second): the main path of a kept-running job,
which waits up to 1.5 s for the screen to go off first, waits for the camera for
1.5 s at most, so the gate is drained at the deadline at the latest, less the
wake-up of its timers. A kept-running job's bound (`gpuHoldBound`, 1.5 s) takes the hold
without the camera's wait, so on that path the gate is closed 1.5 s after the edge
and drained 2.5 s after it at the latest, and a bound that fires while the main
path waits for the camera holds at once and ends that wait. A guest that restarts
while a hold is pending boots after the requests are given back. Guest page-cache
trimming stays off in the foreground, and quiescing trims once by itself.

On a device the quiesce ends by hibernating the guest's RAM, so that a paused
Kernarium no longer carries the guest's wired memory
([`../docs/architecture.md`](../docs/architecture.md#hibernation));
`KERNARIUM_GUEST_HIBERNATION=off|on` overrides it. iOS may still end a
suspended application, and the guest goes with it: a marker file records the
background edge, and the next launch says the guest restarted because
Kernarium was closed in the background.

### Keeping a guest running in the background

A guest pauses in the background unless its owner chose otherwise. Settings
holds one mode per guest, stored under the guest's root filesystem name, and
`off` is the default because running costs battery. `BackgroundController`
decides what the lifecycle edges do and acts only through
`BackgroundEffects`, which the unit tests replace.

- `pictureInPicture` keeps the guest running while iOS shows its display in a
  Picture in Picture window. The runtime mirrors the display to a sink the
  application sets (`kernarium_runtime_loader_set_frame_sink`,
  [`graphics.md`](../docs/graphics.md)), which feeds an
  `AVSampleBufferDisplayLayer`: through the receiver of a render synchronizer
  on the host clock from iOS 27, and by enqueueing on the layer's renderer,
  each frame marked to show at once, before it. It needs the `audio` background mode in
  `Info.plist` and a playback audio session that mixes with other audio.
- `keepRunning` keeps a guest working without a window for a time its owner
  chose, through one `BGContinuedProcessingTask` (`BackgroundJob`) that the
  user starts with a button tap (submitted through `submitTaskRequest` from
  iOS 27 and `submit(_:)` before). iOS grants that task no GPU, so the guest's GPU
  requests stay held for the whole stay.
- A guest kept running has its vCPU threads at background priority
  (`set_background_throttle`) and trims its page cache periodically
  (`set_page_cache_trimming`). An Android guest in `keepRunning` has its screen
  turned off with its power key before the GPU is held, so it can suspend
  itself; work that should make progress there has to hold a wakelock.
  `BackgroundPolicy.swift` and `GuestGPUHold.swift` document the order of the
  edges.

### Low space on the device

A guest's disk grows as the guest writes (the Linux overlay in the temporary
directory, an Android bundle's `data.img`), and a write the device cannot take
is held, with the guest's other disk I/O, until the device has room
([kernel contract](../kernel/common/README.md#block-devices-and-filesystems));
the pause is the guard. The thresholds and the pause itself are
[GuestHostCore](../GuestHost/README.md), which Andarium shares, as is the
figure: the space a write can use (`HostStorage`), which leaves out the
purgeable space iOS Settings counts.

- A start needs room for the guest's first writes (`StartSpace`: 1.5 GiB, and
  3 GiB for a bundle that has no data disk yet). On a device short of it a
  notice covers the guest views, nothing starts, and Check again tries once
  more.
- While a run is on, `GuestSpacePause` reads the free space now and then and
  pauses the guest below 1 GiB. The runtime suspends it with its RAM kept in
  the process (`quiesce_resident`), because a hibernation image is the write
  the device has no room for; the guest's processor time standing still
  confirms the suspension, and a screen over the guest offers Resume and Shut
  down. A guest that does not hold still is called so, with Shut down and
  Check again.
- Resume reads the space again and wakes the guest at 1.5 GiB or more. Shut
  down is the run's stop, the same as leaving the guest, with the guest's
  write-back first: a guest the suspension holds answers what the suspension
  confirmed, one that ran on is asked, the screen says Shutting down, and the
  run ends with the notice that the guest shut down. Kernarium has no clean
  power-off request.
- No lifecycle edge wakes a paused guest. While `BackgroundController
  .spaceHeld`, an edge holds the guest's GPU and does nothing else, in every
  background mode, so the guest stays suspended and resident in the
  background; the Picture in Picture window and the keep-running job end when
  the pause begins. The watch reads the space only while the policy leaves
  the guest running.
- `KERNARIUM_SPACE_HARNESS=1` makes a file `free-space-mib` in Documents,
  holding a number, stand for the device's free space while it exists, and a
  file `space-answer` there (`resume`, `shutdown` or `check`) answer the
  screen once it offers that answer, so a test drives the pause without
  filling the disk.

## Statistics

An application that shows how hard the guest works asks the runtime, which
measures nothing between calls (runtime API 20, `read_statistics` and
`read_guest_memory`). Two readings of one boot give the share of the guest's
processors that computed and the rate of presented frames.
`read_guest_memory` asks the guest kernel through the management console, so it
is never sent where it could wake a guest that sleeps (`-EAGAIN`) and is called
away from the main thread. `KernariumStatistics.hpp` holds the rules, with
`kernarium-statistics` as their test. The reading also counts the graphics
allocations the host has refused the guest (`gpu_memory_refusals`, runtime API
26), which Andarium turns into a notice, and the host GPU's memory budget with
the usage it refuses by (`gpu_memory_budget_bytes` and `gpu_memory_used_bytes`,
runtime API 27), which Andarium shows as VRAM; Kernarium reads none of them.
`read_graphics_memory` (runtime API 28) adds the budget's holders by guest
process, and whether the guest's own low-memory killer reclaims them
(`guest_reclaims`, API 29;
[`docs/graphics.md`](../docs/graphics.md#device-memory-budget)).

## Simulator

The Simulator build is for UI tests of the application and its Android guest.
Configure and build a `kernarium-ios-simulator*` runtime preset, package it with
`scripts/package-ios-frameworks.sh --sdk simulator`, and build the scheme for a
concrete Simulator, or run the tests with `scripts/run-ios-simulator-tests.sh`
([`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium)). The
Simulator renders on the CPU
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)).

## Scripts

Each script documents its options in `--help` or its header. Two facts the
headers do not make obvious: `package-ios-frameworks.sh` writes one shared
output directory, so after `--runtimes android` for Andarium package the full
set again before building Kernarium; and `audio-interrupter.py build` signs
with the team of
[`GuestHost/Config/Signing.local.xcconfig`](../GuestHost/README.md#signing) or
`UML_DEVELOPMENT_TEAM`.
