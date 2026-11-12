# Andarium

Andarium is the iOS application that runs one Android (LineageOS 24, Android
17) guest on the Darwin runtime. Android ships inside the app; what the user
creates, sizes and erases is the guest. [Kernarium](../Kernarium/README.md)
stays the developer application for Linux guests and for Android bundles.
Andarium also runs on an Apple silicon Mac in Designed for iPad mode, and it
builds for the iOS Simulator, where the guest renders on the CPU for UI tests
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)).

## Ownership

```text
Andarium/Shared/      screens and model both targets build
Andarium/Preview/     mock screens, data and launch arguments (Simulator)
Andarium/Live/        the runtime, storage and session of the device app
AndariumCore/         Swift package: the model and the adb client, no UIKit; it
                      re-exports GuestHostCore (../GuestHost/Core)
AndariumTests/        hosted tests of the device app
Config/               entitlements, Info.plist, privacy manifest, bridging header
scripts/              system staging, the App Store bundle check,
                      acknowledgements, screen capture
Andarium.xcodeproj/   JSON project (project.xcproj) and its shared schemes
../GuestHost/Sources  host code shared with Kernarium: the runtime loader, the
                      display and touch view, zstd, the shader caches
```

The project file is the JSON format Xcode 27 reads, kept in the canonical form
`xcrun xcprojformatter --update Andarium/Andarium.xcodeproj` produces: format
after every hand edit. Every configuration takes its base settings from
`../GuestHost/Config/Signing.xcconfig`; the signer's team is only in the
ignored `Signing.local.xcconfig` beside it
([`GuestHost/README.md`](../GuestHost/README.md#signing)), so the project names
no team.

## Targets

`Andarium` is the device application (iPhone and iPad, and an Apple silicon
Mac in Designed for iPad mode). It embeds the Android runtime frameworks and
carries the system image. It has no background mode: the guest runs while
Andarium is in the foreground and is paused when it leaves. It runs one scene
and one guest per process, and needs the increased memory limit and extended
virtual addressing entitlements (`Config/Andarium.entitlements`). The model
and the runtime belong to the process (`AndariumProcess`), so a scene that
connects again shows the same ones.

`AndariumPreview` is a Simulator-only target built from `Shared` and `Preview`
with mock data. It embeds no runtime and boots nothing. Its launch arguments
(`-screen <name>` and the others in `Preview/AppEntry.swift`) open any screen
or state directly, and `scripts/capture-screens.py` (`--help`) captures them:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' build
```

## Devices

The guest is drawn with Metal 4, which the A14 and later chips have. No
capability key names the chip, so `UIRequiredDeviceCapabilities` names `metal`
and the application asks the GPU for the Metal 4 family at launch
(`HostDevice.runsGuestGraphics`), refusing to start a guest without it; the
Simulator draws on the CPU and always passes. iPhone and iPad are supported,
and so is an Apple silicon Mac in Designed for iPad mode: TestFlight and the
App Store offer the application to Macs, and every configuration sets
`SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD` to YES, which gives Xcode its Mac
destination and adds nothing to the built application.
`ITSAppUsesNonExemptEncryption` is NO in `Config/Info.plist`.

The iPhone interface is portrait-only. On iPad, a guest session whose scene
fills its screen locks the interface orientation, so Android alone rotates its
display and the sensor axes stay fixed. A window, or a full-screen scene whose
lock iPadOS releases, uses host-driven rotation: the guest is held at display
rotation zero and its display follows the window, and the sensor axes follow
the window's interface orientation (`MotionMount`). Returning to a locked
scene restores the guest's previous rotation preferences
(`SessionOrientationPolicy`).

## Build

Packaging the Android runtime alone, and staging the system from the accepted
release, need no Linux runtime build:

```sh
cmake --preset kernarium-ios-device-android
cmake --build --preset kernarium-ios-device-android
Kernarium/scripts/package-ios-frameworks.sh --runtimes android
Andarium/scripts/stage-system-image.py
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'generic/platform=iOS' build
```

The fork's `build-andarium-stikdebug-jit.yml` workflow packages the optimized
Release development runtime from this preset. It verifies the generated CMake
compile commands with `scripts/check-ios-jit-build.py` and checks the accepted
Android and kernel artifacts before and after application packaging. Its
unsigned IPA needs a development signature with the accompanying
`Andarium.StikDebug.entitlements`; JIT remains an explicit Home setting and
requires launching through StikDebug. The AppStore product remains separate.

The frameworks go to `.build/kernarium-xcframeworks`, which Kernarium shares,
so package the runtime a build needs just before it. The staging script checks
the accepted release against its manifest and layout and stages the system
disk, partition layout, kernel command line and manifest into
`.build/andarium-system`, which the application bundles as it stands.

The `AndariumTests` bundle runs hosted in the application on an iPad, in the
iOS Simulator or, as an iPad app, on an Apple silicon Mac (whose derived data
must be outside the temporary directories,
[`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium)). For the Simulator,
package its slices first (below), then:

```sh
xcodebuild test -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' \
    -only-testing:AndariumTests
```

The `Andarium` scheme also builds for the Simulator with the software
renderer: build the `kernarium-ios-simulator-android` runtime preset, package
it with `package-ios-frameworks.sh --sdk simulator --runtimes android` and
build for a concrete Simulator
([`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium) has the
destination rules).

### Configurations

The project has Debug, Release and AppStore. Release is the development and
harness build: it reads the automation environment and prints the console
lines the iPad harness reads. AppStore builds as Release does and leaves out
everything that exists for automation or only pretends:

- `APP_STORE` is a Swift compilation condition that compiles out the launch
  environment (`Live/LaunchEnvironment.swift`, so no `ANDARIUM_` or
  `KERNARIUM_` name is in the application or in `AndariumCore`), the harness's
  console lines (`Shared/ConsoleReport.swift`), the launch's deferred work
  (create, erase, start) and the isolated guest instance.
- It defines `UML_APP_STORE_APPLICATION` for the loader it compiles, which
  leaves out the display mirror, console input, the automation lookups and the
  JIT's preparation entries ([`GuestHost/README.md`](../GuestHost/README.md)).
  The JIT's policy and words (`Shared/Jit.swift`), debugger side
  (`Live/JitDebugger.swift`) and Home row are under `!APP_STORE`, in the
  application and not in AndariumCore, which the package builds without that
  condition.
- It embeds the App Store runtime, which reads no environment knob and carries
  no debugging or automation support (the `kernarium-ios-device-android-app-store`
  preset, packaged with `package-ios-frameworks.sh --app-store --runtimes
  android`; [`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
- Its Archive action uses it, and the archive is the build TestFlight and the
  App Store offer to Macs.
- It refuses an embedded artifact that is behind the archived commit: a build
  phase runs [`scripts/check-embedded-artifacts.py`](../scripts/check-embedded-artifacts.py),
  which compares the accepted Android release and kernel descriptors with the
  commit's inputs, and the staged and packaged system and runtime with the
  descriptors, and fails naming the changed paths and commits. It also refuses
  a packaged kernel whose Linux banner names a build user or host other than
  the neutral `kernarium@kernarium`. A release that records no input trees, a
  dirty input, or a staged or packaged artifact that is not the accepted one
  stops the build; only the user decides to ship pending changes, by naming the
  release ID in `PENDING_INPUTS_DECISION`.
- Its last build phase runs `scripts/check-app-store-bundle.py`, which fails the
  build on a missing Info.plist key, an environment knob, a symbol or log text
  of the debugging and automation support, the JIT (its symbols and text and
  the `brk #0xf00d` instruction), a test bundle or XCTest framework, a
  trace of the build host, a harness console line, or a privacy manifest that
  does not cover a binary
  ([`scripts/check-privacy-manifests.py`](../scripts/check-privacy-manifests.py));
  the script's header is the complete list.

```sh
cmake --preset kernarium-ios-device-android-app-store
cmake --build --preset kernarium-ios-device-android-app-store
Kernarium/scripts/package-ios-frameworks.sh --app-store --runtimes android
Andarium/scripts/stage-system-image.py
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -configuration AppStore -destination 'generic/platform=iOS' build
```

The privacy manifests of the application (`Config/PrivacyInfo.xcprivacy`) and
of the frameworks (`Kernarium/PrivacyManifests`) declare exactly what the App
Store build and the runtimes use. A release or harness build also reads the
system uptime for its thermal report, which the application's manifest does
not declare because only an App Store build is uploaded.

## Model and storage

`AndariumCore` holds everything the screens decide with, so it is tested on
the host (`andarium-core`, [`testing.md`](../docs/testing.md#andarium)): limits,
the memory policy, the guest and its life (`Lifecycle`), storage, boot
milestones, the adb client, motion and statistics. The application adds only
what a screen holds. A screen shows a control only when the capability behind
it exists (`Capabilities`); a build whose runtime cannot do a thing leaves its
control out.

- **Memory.** A guest has at most the shared
  [vCPU limit](../docs/architecture.md#memory-and-executable-code) and the RAM
  the runtime loader answers for that count
  (`kernarium_runtime_loader_maximum_guest_memory_mib`); a new guest gets the
  loader's default for it, which stops at 4 GiB however much is offered
  (`kernarium_runtime_loader_default_guest_memory_mib`). Android is usable
  from 1.5 GiB, so a guest has at least that (`Limits.minimumMemoryMiB`,
  Andarium's own: the runtime still accepts less for Kernarium's guests); a
  stored smaller size is raised to it on load, and a device whose bound is
  lower offers the bound alone. `MemoryPolicy` holds those bounds, defaults
  and the estimates and no formula of its own. A guest with more than the
  runtime's conservative size for its vCPU count (`safeMemoryMiB`) is likely
  to be closed by iOS: Home shows a notice and the memory ruler a line and red
  ticks, both following the guest's memory and vCPU count (`MemoryWarning`).
  A guest above the graphics line (`MemoryPolicy.graphicsLineMiB`) may run out
  of graphics memory (VRAM, the host GPU's budget): the System sheet turns the
  ticks past it orange and warns that games may (`GraphicsMemoryWarning`), and
  Home says nothing. The line is the default where the runtime lowered the
  default to keep the GPU 1 GiB
  (`kernarium_runtime_loader_default_guest_memory_holds_gpu_budget`), and
  elsewhere the largest size whose estimate of VRAM is still 1 GiB or more
  (`Limits.minimumDefaultGraphicsMemoryMiB`), so a range above 4 GiB warns
  only where the estimate falls. The sheet also shows the loader's estimate of
  VRAM for the chosen memory and vCPUs and says it is separate from the
  guest's RAM.
  The memory ruler follows the range, which on a Mac with much RAM has about
  a thousand positions: it labels only the round numbers of gibibytes that do
  not touch and draws the ticks that have room (`RulerThinning`), and
  VoiceOver's increment and decrement then move to the next labelled one.
- **Storage.** A guest's files live in `Library/Guest/` of the application's
  container behind `GuestStorage`. The system disk is Android's, read-only, and
  stays in the release directory; the runtime creates the data disk `data.img`
  at the first start, sized by the guest's capacity, and only grows it after;
  Home disables Start in recovery until then (`StartPlan.offersRecovery`).
  A larger size chosen in Resize waits on Home and applies as the next start,
  of any kind, is handed to the runtime (`Lifecycle.start`), which grows the
  disk then; the guest records the new capacity at that point, so a start that
  ends early leaves no growth pending beside a disk that already has it.
  The runtime passes both paths to Linux in a kernel argument, so they stay at
  paths free of spaces and the other characters `DiskPath` refuses (which is
  why the guest is not in `Application Support`).
- **Updates.** `InstanceRules.decide` applies the rules of the
  [Android README](../android/lineage-24/README.md#guest-instance-storage) and
  `StartPlan` is what Home shows of them before Start. An update clones
  `data.img` first, and Erase guest deletes everything the guest has stored,
  the snapshot and the adb key included.
- **Backup.** The guest is left out of iCloud and computer backups unless the
  user turns on Back up guest data.
- **Release notes.** `Shared/WhatsNew.json` bundles the notes per build,
  from build 9 (build 5, the first public release, and builds 6 to 8 have
  none).
  The first launch of an update that carries notes shows a ribbon above Start,
  on at most three launches, that opens them; a build shows the notes up to its
  own number and no later ones. Info keeps every release's notes
  (`WhatsNewRules`, `WhatsNewStore`). A launch with a launch setting, or one a
  test runs, shows no ribbon and keeps no record.

## Runtime

The device application drives the Android runtime through the same loader as
Kernarium. A run starts when the guest's display has its size and the disk is
ready, on a thread of its own. Android's clean power-off releases the kernel's
resources and guest RAM and returns to Home, which states the storage the data
disk then holds and where Start boots again in the same process; an unclean
shutdown, a host stop or a cleanup that fails keeps the failure screen and
requires reopening Andarium.

- **Time zone.** Every start gives the guest the host's zone
  (`GuestTimeZone`, the boot request's `android_time_zone`); the system image
  applies it unless the guest's user chose a zone
  ([Android README](../android/lineage-24/README.md#time-zone)).
- **Input.** The shared [GuestHost input](../GuestHost/README.md) maps up to
  ten fingers onto the guest's display. A touch the system takes over reaches
  Android as a cancelled touch, not a lift, so no tap follows. Only the side
  keys take touches: while they are open, a touch that begins anywhere else
  reaches Android from its first event and closes the keys
  (`SessionState.closeSideKeys`, called by the display's `touchDidBegin`).
  Beside the closed keys their tip, while it shows, takes touches on its card
  alone (on a Mac, where an iPad app's popover keeps its dimming view, every
  touch), and it goes with the closed keys when they open or the session ends.
- **Retina.** Retina mode applies at the next start and uses the screen's
  native pixels, whose host allocations count against the same iOS process
  limit as guest RAM (`DisplayOptions`).
- **Native resolution.** Native resolution lays the guest's display over the
  whole screen, under the Dynamic Island and the home indicator, and implies
  Retina mode; the display follows the larger view at once, the pixel density
  at the next start. The experimental notch switches Android's tall display
  cutout emulation through the shell after each `sys.boot_completed` and on
  every change, with a shell overlay that keeps the cutout unpainted
  (`DisplayCutoutEmulation`), so apps keep clear of the island.
- **JIT.** A development build on a device has a JIT row on Home, off until
  the person turns it on and kept for the install (`JitPreference`). Below it,
  the experimental Fast JIT row tiers the run (`FastJitPreference`, read by the
  runtime from `KERNARIUM_ARM64_JIT_TIER`). An iOS
  device gives the JIT its executable memory only through a debugger attached to
  the process ([`docs/architecture.md`](../docs/architecture.md#executable-code)):
  the person assigns StikDebug's universal script to Andarium once in StikDebug
  and opens Andarium from it. StikDebug spins until its request is served and
  the system ends it within a minute, so Andarium asks as it opens
  (`AppModel.prepareJitAtLaunch`), on a worker, and Home does not wait for it.
  With the row on it prepares the memory for the guest, or for the guest Create
  offers when there is none yet, and the debugger leaves within seconds; with
  the row off it tells the debugger to leave at once, and turning the row on
  later in that process says to open Andarium from StikDebug. A start uses the
  memory prepared at launch (or waits for the attempt, with "Preparing JIT" on
  the boot screen) and boots on the JIT engine; without a debugger, or if the
  preparation fails, it boots on gadgets and says why on the boot screen and
  under the row. A guest changed after the memory was prepared needs Andarium
  opened from StikDebug again, and the memory stays wired for the process
  whether the start uses it or not (the runtime's GPU plan counts it). No
  debugger request is made while none is attached, and one that goes unanswered
  because the debugger vanished is a start on gadgets, never a crash. Starting a
  Debug build from Xcode attaches lldb, which would stop at the request, so
  Debug serves the request only when the launch says a servicing debugger is
  attached (`ANDARIUM_JIT_DEBUGGER=1`). `ANDARIUM_JIT` (`1` or `0`) states the
  row for a harness launch, which keeps nothing; a launch with another setting
  starts it off. The JIT compiles every process's code again for itself
  ([cold code](../darwin/cpu/arm64/jit/README.md#cold-code)), so an Android
  guest's boot, application launches and background work run several times
  slower than on gadgets, and a long single-process computation runs faster. An
  App Store build carries none of this: its runtime has no JIT and the
  application has no row, preference, entries or sentences.
- **Motion.** Gyroscope needs no permission. The host reads a sensor only while
  a guest application uses it and sends samples over private vsock port 5556
  from the process's one Core Motion manager, which is never released (a
  released manager crashes inside Core Motion); the system image must include
  the [motion sub-HAL](../android/lineage-24/README.md#motion-sensors).
- **Free space.** Once Android has booted a normal start, the application
  reads the device's free space (`RunSpace.nextCheck`: every 2 s near the
  limit, every 30 s on a roomy device) and pauses the guest below 1 GiB
  (`RunSpace.pauseBelowBytes`), before the data disk, which Android mounts so
  that a failed write panics, can fail to grow. The pause is the runtime's
  suspension with the storage written back first and the RAM kept in the
  process: no image of it goes to the device that is short of space
  ([lifecycle](../docs/architecture.md#guest-lifecycle)). The suspension is
  confirmed by the guest's processor time standing still (`SuspensionCheck`);
  a guest that does not hold still is woken and suspended once more, and one
  that still runs is left running behind a screen that says so and offers
  Shut down and Check again, never Resume. A paused guest is asked nothing and
  no lifecycle edge wakes it, the background included: the application stays
  resident rather than hibernate. Settings closes and an adb operation and ADB
  over network end. A screen over the session offers Resume, which reads the
  space again and wakes the guest at 1.5 GiB (a start's reserve,
  `RunSpace.resumeAtBytes`) or more, and Shut down, which wakes the guest and
  has Android power off. The thresholds and the pause itself are
  [GuestHostCore](../GuestHost/README.md), shared with Kernarium; the reading
  comes from `AppEntry.makeModel(freeSpace:)`, which a test replaces.
- **Statistics.** The session's capsule and Settings' Stats rows show the
  runtime's readings ([`Kernarium/README.md`](../Kernarium/README.md#statistics)),
  read only while a view shows them, the application is active and the guest
  runs (`StatsSampling.isWanted`). Memory is the guest's in use against its
  size, and VRAM the host GPU's usage against the guest's budget
  ([`docs/graphics.md`](../docs/graphics.md#device-memory-budget)), a dash
  without one; each has a bar of its share, orange from 90%. Settings' Show on
  screen is kept for the install and restored at launch, off on a fresh one
  (`StatsPreference`, its own stored value beside `DisplayOptions`). A launch
  with any launch setting is a harness's and starts with them off, so a
  measured run never inherits the overlay; `ANDARIUM_STATS` (`1` or `0`) states
  the value. Such a launch keeps nothing, and neither does one that states
  Retina mode, so what the person stored stays (`PreferenceKeeper`).
- **Graphics memory.** The runtime counts the graphics allocations the host
  refused the guest ([`docs/graphics.md`](../docs/graphics.md#device-memory-budget)).
  While a normal session runs in the foreground the model reads the count every
  2 s, with or without a statistics view (`AppModel.refreshGraphicsNotice`), and
  a rise shows `GraphicsNoticeBanner`: a glass banner that takes touches only
  where it is drawn, dismissible and gone after 30 s. It tells the person to
  swipe a broken app away in Recents, which restarts the process that lost its
  graphics. `GraphicsNotice` decides: the first reading is a baseline, and
  refusals less than a minute apart are one burst, which shows one notice
  whether or not it was dismissed. Recovery runs no app and is not read. In the
  same loop `GraphicsReclaim` frees the budget silently: when it stays nearly
  full or the guest is refused, it ends the cached background app that holds
  the most, over the guest's shell. It acts only on a holder the guest kernel
  names by process, never on the foreground, visible, perceptible or home app.
  It stands down while the guest's own low-memory killer reclaims the budget
  (`guest_reclaims`, runtime API 29), which is then the only one that ends an
  app.
- **Session ends.** The runtime records how each session ended
  ([`docs/architecture.md`](../docs/architecture.md)). The next launch keeps
  that record as the previous session's (`PreviousSession`) and, after an end
  that was neither Android's own nor a stop, Home shows a notice with its cause
  (`SessionRecord.notice`). `ExitReports` keeps the exit and diagnostic reports
  MetricKit delivers beside the records.

## Android operations

Andarium is the adb host for its one guest. The runtime opens a connected
stream socket to the guest's vsock port 5555
(`kernarium_runtime_loader_connect_vsock`), so adbd is reached by no other
process and through no listening socket of the device's unless ADB over network
is on. `AndariumCore/Adb` speaks the protocol.

- **Key.** `AdbKeyStore` keeps one RSA-2048 key per guest in the Keychain, this
  device only and not synchronized. Its public key goes in each boot request
  (`android_adb_public_key`), which adbd authorizes for that boot.
- **Create bug report and Install APK** are in every build, in a normal
  session. A bug report is Android's zip with Andarium's session records and
  MetricKit reports added under `andarium/`, the application's container paths
  taken out (`BugReportAttachments`); a zip that cannot be amended safely is
  shared as Android made it.
- **File browser** in Settings browses the guest's internal storage
  (`/storage/emulated/0/`, the guest Files app's root) and imports and exports
  files over the same channel, in a normal session and whatever the guest's USB
  debugging setting is, since adbd serves the application's key either way
  (`Live/LiveGuestFiles.swift`, `AdbSync`, `GuestFiles`). One transfer runs
  at a time; Install APK and Create bug report wait for it. An import is
  written to a hidden `.andarium-*.part` name beside its destination and moved
  into place, so a stopped or failed one leaves nothing and a Replace keeps the
  original until the new file is whole. The move's own shell command then has
  the guest's media index complete the file's row (a file the shell user
  writes has only a pending one, which MediaStore keeps from other apps).
  A transfer stops when the application leaves the foreground, and its files
  say so. Turning USB debugging off restarts adbd, which ends a transfer in
  flight: it fails, and Try Again runs it whole. Android reports its boot done
  before internal storage is mounted, so the browser is unavailable until a
  check in the guest finds the storage mounted, writable and holding a
  standard folder (`InternalStorage`); nothing is imported or exported before
  then.
- **ADB over network** is off at every run and never persisted, and is
  available only while the guest's USB debugging is on. `AdbNetworkBridge`
  listens on TCP 5555 and relays each computer to a fresh vsock stream, serving
  at most 8 at once; it understands no byte of adb, so the guest's adbd
  authenticates the computer. iOS asks for Local Network access once, through
  `NSLocalNetworkUsageDescription`. Nothing listens before the toggle.
- **Privacy.** The adb features add no required-reason API beyond the
  disk-space API with reason E174.1 (the start's own space check already uses
  it) and, for the file browser, the file-timestamp API: an import reads the
  date of a file the person picked (3B52.1) and an export sets the date of its
  staged copy inside the application's container (C617.1), so both keep the
  file's own date.

## Acknowledgements

The application's Info screen lists every project it carries and shows its
copyright holders, license and full license text. The list is the bundled resource
`Andarium/Shared/Acknowledgements.json`, generated by
`scripts/acknowledgements.py` from the verified dependency sources and the
kernel's license files (never fetched; `--verify` compares the resource with
them). A tooling test fails when the Andarium project embeds a framework, or an
iOS device prefix contract declares a component, that no entry covers
([`docs/testing.md`](../docs/testing.md#andarium)). Adding a library therefore
means adding its entry to the script's table and running it.

## Automation

A harness launch (`xcrun devicectl device process launch`, environment given
as `DEVICECTL_CHILD_` variables) reaches a state without touching the screen.
The names are the application's, in `Live/LaunchEnvironment.swift`, which the
App Store configuration leaves out; `AndariumCore` knows the settings
(`LaunchOverrides.Setting`) and not the names. The `KERNARIUM_` names mean what
they mean in Kernarium; the automation Android runtime
(`package-ios-frameworks.sh --runtimes android --android-automation`)
additionally reads `KERNARIUM_ANDROID_ADB_AUTH`
([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
`ANDARIUM_GUEST_INSTANCE=<name>` runs an isolated guest under
`Library/TestGuests/<name>`, for tests that need a fresh guest while the
ordinary guest or its pending update must be preserved. A file `free-space-mib`
in that directory, holding a number, stands for the device's free space in the
run's low-space watch while it exists, so a harness drives the pause without
filling the disk; a file `space-answer` there (`resume`, `shutdown` or
`check`) answers the screen over a paused guest once it offers that answer.
`run_ios_device_android.py --engine jit` runs the guest on the JIT, with the
stand-in debugger of `tests/integration/support/ios_jit_debugger.py` serving
the request that StikDebug serves for a person (`--jit-debugger none` shows the
fall back to gadgets).

On an Apple silicon Mac, `xcodebuild test -scheme Andarium -destination
'platform=macOS,variant=Designed for iPad'
-only-testing:AndariumTests/AutomationHoldTests` is the launcher, with each
setting given as a `TEST_RUNNER_`-prefixed variable. The test run ends the
application, so `TEST_RUNNER_ANDARIUM_HOLD_SECONDS` keeps the host up that
long. The opt-in test switches are in
[`docs/testing.md`](../docs/testing.md#andarium). The console lines the harness
reads are `Shared/ConsoleReport.swift`'s, and an App Store build prints none.

The container's `Documents` is the harness's; Andarium never deletes a file it
did not create. The guest is under `Library/Guest/` and is not the harness's to
write. A launch boots the system the build carries, so another release means
another staged build. iOS asks to allow local network access only when the
guest itself reaches the local network, and only a person answers; a harness
run that provokes it waits for the focus
(`run_ios_device_android.py --wait-active`).
