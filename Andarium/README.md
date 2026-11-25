# Andarium

Andarium runs a locally installed Android guest on the Darwin runtime. On
first launch, choose the Android system and start its download or import.
The install package carries the runtime and kernel. The official default is
LineageOS 24 r28. MysticGSI skins and custom ext4, EROFS, and ROM ZIP images
use the r28 boot and vendor base. Home’s appearance menu switches between
the build 30 layout and the newer layout, and can reopen the Android chooser
on the next launch. [Kernarium](../Kernarium/README.md)
stays the developer application for Linux guests and for Android bundles.
Andarium also runs on an Apple silicon Mac in Designed for iPad mode, whose
minimum is macOS 27.0 (`LSMinimumSystemVersion`; the iOS minimum alone would
map to macOS 26.0), and it
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
AndariumUITests/      UI tests of the device app in the Simulator
AndariumPreviewUITests/ UI tests of the Simulator preview
Config/               entitlements, Info.plist, privacy manifest, bridging header,
                      build settings (Andarium.xcconfig)
scripts/              system staging, acknowledgements, screen capture
Andarium.xcodeproj/   JSON project (project.xcproj) and its shared schemes
../GuestHost/Sources  host code shared with Kernarium: the runtime loader, the
                      display and touch view, zstd, the shader caches
```

The project file is the JSON format Xcode 27 reads, kept in the canonical form
`xcrun xcprojformatter --update Andarium/Andarium.xcodeproj` produces: format
after every hand edit. Every configuration takes its base settings from
`Config/Andarium.xcconfig`, which includes
`../GuestHost/Config/Signing.xcconfig` and sets whether
[Follow developer on X](#follow-developer-on-x) is compiled in; the
signer's team is only in the ignored `Signing.local.xcconfig` beside the
signing file ([`GuestHost/README.md`](../GuestHost/README.md#signing)), so the
project names no team.

## Targets

`Andarium` is the device application (iPhone and iPad, and an Apple silicon
Mac in Designed for iPad mode). It embeds the Android runtime frameworks and
loads the system image from local storage. It has no background mode: the guest runs while
Andarium is in the foreground and is paused when it leaves. It runs one scene
and one guest per process, and needs the increased memory limit and extended
virtual addressing entitlements (`Config/Andarium.entitlements`). The model
and the runtime belong to the process (`AndariumProcess`), so a scene that
connects again shows the same ones.

`AndariumUITests` is the UI-test target of `Andarium`, in the scheme's test
action. It is never part of the application.

`AndariumPreview` is a Simulator-only target built from `Shared` and `Preview`
with mock data. It embeds no runtime and boots nothing. Its launch arguments
(`-screen <name>` and the others in `Preview/AppEntry.swift`) open any screen
or state directly, and `scripts/capture-screens.py` (`--help`) captures them:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' build
```

`AndariumPreviewUITests` drives the preview through those arguments; the
scheme's `test` action runs it.

## Devices

The guest is drawn with Metal 4, which the A14 and later chips have. No
capability key names the chip, so `UIRequiredDeviceCapabilities` names `metal`
and the application asks the GPU for the Metal 4 family at launch
(`HostDevice.runsGuestGraphics`), refusing to start a guest without it; the
Simulator draws on the CPU and always passes. iPhone and iPad are supported,
and so is an Apple silicon Mac in Designed for iPad mode: every configuration
sets `SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD` to YES, which gives Xcode its Mac
destination and adds nothing to the built application.
`ITSAppUsesNonExemptEncryption` is NO in `Config/Info.plist`.

The iPhone interface is portrait-only and the guest owns its rotation:
Android turns its own display and the sensor axes stay fixed. A guest whose
image has a device-orientation sensor (the boot request's `orientation` host
feature, [Kernarium](../Kernarium/README.md#automation-runs)) follows the
device's orientation as iOS reports it, which already includes the person's
Portrait Orientation Lock: while a session is in front the application
observes `UIDevice` (`DeviceOrientationObserver`) and sends the orientation
over the motion connection (`MotionOrientation.swift`), which Android treats as
a rotation proposal that its own policy still decides on. That is the display's
own behaviour and uses no accelerometer or gyroscope reading, which are the
guest's separately ([Runtime](#runtime), Motion). A guest whose image has no
such sensor judges the accelerometer as before. On iPad
the interface turns with the device in a session as it does at Home, full screen
or in a window, and the host drives the guest: it is held at display rotation
zero, its display follows the scene, and the sensor axes follow the scene's
interface orientation (`MotionMount`, `SessionOrientationPolicy`). An
application that asks for one fixed orientation is letterboxed in the other
one, as in a window. The guest's own rotation preferences are restored when
the host stops driving it. The display keeps the last frame's proportions
while it turns, until Android draws the new layout.

A folding iPhone (iPhone Duo) has two displays. Its cover is an iPhone's, and
its inner display turns with the device as an iPad's does, the host driving the
guest. The layout follows the display the system lights, which the scene's root
reads from the hinge and the view's reserved regions (`FoldSceneReader`) and
`AndariumCore/FoldLayout.swift` decides as pure functions; a device that
reports no hinge keeps every layout it has. Only the inner display bent at the
fold is placed around the hinge (`FoldPose.spreads`): flat, upright or turned,
every screen is laid out as on an iPad of the same size and shape.

- **Cover.** The guest keeps an iPhone's shape beside the strip the camera
  keeps, which holds the side keys as an always-open column and the statistics.
- **Inner display, upright.** The guest is one display across both halves, so
  moving the hinge never resizes Android, and the side keys stay on its edge.
  Bent, the statistics and notices hang from the trailing half and Andarium's
  own pieces never sit on the fold; flat, they hang from the top of the display.
- **Inner display, turned.** Flat, the guest fills the display, with the keys
  on its edge and the statistics and notices at its top. Bent, the device
  stands like a laptop: the guest is the upper half and the lower half is a
  deck with the keys in one row and the statistics and notices above them.
- **Other screens.** Bent, Home is one page each side of the fold; flat it is
  Home as on an iPad, one scrolling list with Start at the foot of the screen.
  Flat, Welcome, the boot, the busy, low-space and end cards, and the cards
  inside a sheet (the system's) are laid out as on a display with no hinge,
  centred in the safe area as the Start bar and the system's alerts are. While
  the device is bent they keep to the half where the system puts its alerts,
  and the boot of a standing device to the upper half. While bent, Welcome
  and the end screen scroll when taller than their half. System alerts stay
  where iOS puts them, inside the device's own safe area. A sheet turned and
  bent rises to a little below the fold; a sheet keeps the placement it opened
  with, and what has been typed in it, until it closes, whatever the hinge or
  the turn does meanwhile.
  On the cover the file browser's Import is a round button in the toolbar's
  column beside the camera, between More and Done, and the bar keeps Export.

The guest is resized only where its shape changes (the cover and the inner
display, standing and the other turned poses), through the display's ordinary
resize. Telling Android where the crease is is not done.

While the guest's display is shown (the boot and the run) everything behind it
is black, in the light appearance too: the scene's root view and its window
carry it, so the area outside the safe area stays black through a rotation or
a window resize. Home and sheets follow the system appearance
(`AndariumHostingController`).

## Build

Packaging the Android runtime needs no Linux runtime build:

```sh
cmake --preset kernarium-ios-device-android
cmake --build --preset kernarium-ios-device-android
Kernarium/scripts/package-ios-frameworks.sh --runtimes android
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'generic/platform=iOS' build
```

The frameworks go to `.build/kernarium-xcframeworks`, which Kernarium shares,
so package the runtime a build needs just before it. A runtime that links the
ahead-of-time framework is packaged with `UmlArm64Aot.xcframework`: the
Simulator runtime takes the accepted AOT artifact, and the device runtime
takes it only when `accepted-aot.env` accepts it for archives (a device build
that measures it names an artifact, `-DUML_AOT_ARTIFACT=DIR`, when it
configures the runtime).

## System setup

The app ships without a guest disk. Setup downloads the official LineageOS 24
guest or imports a compatible guest ZIP from a direct HTTPS link. The importer
checks the selected family, kernel ABI, layout and disk digest before making
the image available in local storage. Custom XOS, HyperOS, ColorOS and One UI
ports need an arm64 Android 17 release built for Andarium's 4 KiB runtime with
its release manifest, kernel command line, partition layout and GPT system disk.
The runtime still checks the release and its disks at boot. Custom releases
keep their guest data separate from the official guest.

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

The project has Debug and Release. Release is the distributed build, with its
JIT option; it reads the automation environment and prints the console lines
the iPad harness reads, as Debug does. A launch that sets no `ANDARIUM_` or
`KERNARIUM_` name uses none of that.

- The launch environment (`Live/LaunchEnvironment.swift`), the harness's
  console lines (`Shared/ConsoleReport.swift`), the launch's deferred work
  (create, erase, start), the isolated guest instance and the UI tests' probes
  are in both configurations.
- The JIT's policy and words (`Shared/Jit.swift`), debugger side
  (`Live/JitDebugger.swift`) and Home row are in the application and not in
  AndariumCore.
- The document picker of Install APK and of the file browser's Import is the
  one in copy mode (`Shared/CopyDocumentPicker.swift`): it moves each copy at
  once into a folder of its own under `tmp/PickedFiles/` and removes it when
  the install or import that uses it is over (Install anyway and Try Again
  still find it) and at the next launch. A picker that fails shows an alert.
  The build phase "Check document pickers" runs
  [`scripts/check-document-pickers.py`](scripts/check-document-pickers.py) on
  every build: the sources hold no SwiftUI `fileImporter` and no document
  picker made without `asCopy: true` outside that file, and both call sites
  present the copy-mode picker.
- A launch removes what an ended process staged in the temporary directory
  (`BugReports/`, `GuestFiles/`, `PickedFiles/`) and `Feedback/`, where an older
  build staged the files of an email feedback draft
  (`Shared/LegacyFeedbackFolder.swift`; the hosted `LaunchSweepTests`).
- The scheme's Archive action uses Release, the build offered to Macs as an
  iPad app too. The app icon is `DuskJade` (`Shared/DuskJade.icon`).
- An archive refuses an embedded artifact that is behind the archived commit: a
  build phase of Release runs
  [`scripts/check-embedded-artifacts.py`](../scripts/check-embedded-artifacts.py)
  when the action is install, which an archive and an explicit install of
  Release both run, and an ordinary Release build is not checked.
  The check requires the archive to ship without an Android system and checks
  the accepted runtime kernel and AOT artifacts against their descriptors and
  input trees. Pending inputs require a release ID in
  `PENDING_INPUTS_DECISION`.
- [Follow developer on X](#follow-developer-on-x) is carried by Release when
  the handle is in the ignored `Config/Developer.local.xcconfig` and the logo
  in the ignored `Config/Local/`. The same build phase runs
  [`scripts/check-release-archive.py`](scripts/check-release-archive.py), which
  refuses a handle that is not a handle of X and the placeholder of
  `Developer.local.xcconfig.example` in any letter case (no handle is no
  link); a home, volume or temporary-directory path in the host code of the
  executable or of an embedded framework (the guest's sections are not host
  code); the linker's debug map in an embedded framework; a test bundle or
  the frameworks that run tests (XCTest, XCUIAutomation, Swift Testing and the
  SDK's other test frameworks), embedded or only named by a load command; and
  a linked test-support library (`AndariumCoreTestSupport`). It refuses an
  update address the notice cannot read and the example host of
  `Update.local.xcconfig.example` (see [Update notice](#update-notice)).

An archive is made after the steps of [Build](#build), with
`xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium archive`.

The privacy manifests of the application (`Config/PrivacyInfo.xcprivacy`) and
of the frameworks (`Kernarium/PrivacyManifests`) declare the required-reason
APIs the binaries use, which `scripts/check-privacy-manifests.py` checks for
the frameworks as they are packaged. The application also reads the system
uptime for its thermal report, which its manifest does not declare.

### Follow developer on X

Info has one passive outward link more: Follow developer on X, a row in the
section of Project on GitHub, which opens the developer's profile through the
system (`openURL`: the X app when it is installed, the browser otherwise;
`Shared/FollowX/`). It has no badge, prompt or counter, and nothing is
remembered about a tap. The name X in its text is the service's logo, drawn
inline at the text's size and colour as a template image, and the same logo is
the row's leading icon; VoiceOver reads "Follow developer on X" with the link
trait. The profile and the logo belong to the developer and are tracked nowhere:
the row is compiled in, under the `FOLLOW_X` condition, only in a Release build
whose ignored `Config/Developer.local.xcconfig` (from
`Developer.local.xcconfig.example`) names the handle (`DEVELOPER_X_HANDLE`: a
handle, never an address, as `//` starts an xcconfig comment; the code builds
the address) and whose ignored `Config/Local/` directory holds `x-logo.svg`, a
single-colour vector that `XLogoLocal.xcassets` links to. A build with the
handle and without the logo fails its first build phase, and an archive
refuses a handle that is no handle of X and the example one
(`scripts/check-release-archive.py`). A fork or a Debug
build holds none of it (no row, handle, logo or `FollowX` strings table).
Tests and captures turn the row on in a Debug build with
`FOLLOW_X_HANDLE=example_handle` on the `xcodebuild` line, with a glyph drawn
for them (`XLogoGlyph.xcassets`) in place of the logo; adding
`FOLLOW_X_LOGO_SOURCE=local` takes the logo from `Config/Local/`.
`AndariumPreview` takes the arrangement only, never the local handle. The
handle's shape and the address are in `Shared/FollowX/Logic`, which
AndariumCore's tests build through a symlinked target no product links, and the
words are in the `FollowX` string table.

### Update notice

A sideloaded build tells the person when a newer version exists: Home's first
notice (`Shared/AppUpdateCard.swift`: a title, one line, one button, and a close
button that puts away that build only), a detail the button opens, and Info's
Updates section with the Check for Updates switch (on by default). Which
version exists comes from one AltStore-format file the maker hosts; what is
read, checked and sent is
[GuestHost's Update notice](../GuestHost/README.md#update-notice), whose
`GuestHostCore` model and `GuestHost/Sources/UpdateNoticeViews.swift` the
Kernarium application can use as well. Andarium supplies its name, the moment
and the launch (`Live/AppUpdateLaunch.swift`).

- `sideload/source.json` is that file for Andarium: the AltStore source that
  [the README's sideloading section](../README.md#sideloading) tells people to
  add, served from `main`, whose newest version's `size` and `date` are those of
  its release's IPA (`guest-host-core` reads it with the application's own reader).
- The address is compiled in, under the `UPDATE_NOTICE` definition, only in a
  Release build whose ignored `GuestHost/Config/Update.local.xcconfig` (from
  `Update.local.xcconfig.example`: a host, a path and the hosts links may name
  besides the file's own, never a whole address) names a host. The build writes
  the `UpdateSourceHost`, `UpdateSourcePath`, `UpdateLinkHosts` and
  `UpdateAppIdentifier` keys of `Info.plist`, with the original bundle
  identifier, and the two `LSApplicationQueriesSchemes` (`altstore`,
  `sidestore`) that let the detail offer an installed tool. A fork or a Debug
  build has no address, no read and no notice. An archive refuses the
  example host and a malformed address
  (`scripts/check-release-archive.py`). Tests and captures name
  `UPDATE_SOURCE_HOST=example.com UPDATE_SOURCE_PATH=/source.json` on the
  `xcodebuild` line to build the keys into a Debug build; `AndariumPreview`
  sets the host empty and shows a made-up file through the real reader.
- The read is at most one a day, a few seconds after Home shows, while no run
  is active, starting or pending, and never in a test or harness launch
  (`announcesWhatsNew` is false there) or under Low Data Mode. Nothing waits
  for it: a start cancels it.
- The detail lists the version, its day, its size, the iOS it needs and the
  installed version; "Update with AltStore" and "Update with SideStore" appear
  only for an installed tool, then the app file and the release page, and Skip
  This Version. On a Mac or Vision Pro the card shows and the detail offers the
  app file and the release page, with the steps in words instead of the tool
  rows.
- The sentences are `UpdateWords` in the package's catalog, in the four
  languages; the application's name is the only value inserted besides the
  version, its build and its day.

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
  lower offers the bound alone, and a bound of zero (the loader's answer where
  the process has no room for even the smallest window) offers none: the
  System sheet leaves the memory row out, and the guest shows zero for that
  run alone: a save keeps the size the person had, and a guest set up there
  takes the default once there is room. The runtime starts every size the picker
  offers: a start whose memory the loader refuses
  (`kernarium_runtime_loader_guest_memory_admitted`) ends before the run with
  that reason. `MemoryPolicy` holds those bounds, defaults
  and the estimates and no formula of its own. A guest with more than the
  runtime's conservative size for its vCPU count (`safeMemoryMiB`) is likely
  to be closed by iOS: Home shows a notice and the memory ruler a line and red
  ticks, both following the guest's memory and vCPU count (`MemoryWarning`).
  A guest above the graphics line (`MemoryPolicy.graphicsLineMiB`) may run out
  of graphics memory (VRAM, the host GPU's budget): the System sheet turns the
  ticks past it orange and warns that games may (`GraphicsMemoryWarning`), and
  Home says nothing. The line is the default where the runtime lowered the
  default to keep the GPU 992 MiB
  (`kernarium_runtime_loader_default_guest_memory_holds_gpu_budget`), and
  elsewhere the largest size whose estimate of VRAM is still 992 MiB or more
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
  at the first start, sized by the guest's capacity, and only grows it after.
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
  `data.img` first (the update's copy, until the updated Android has booted),
  and Erase virtual machine deletes everything the guest has stored, its snapshots,
  the adb key, its installation identity, the sender of the last text message
  sent to it and the notice of a text message whose outcome was not yet shown
  included.
- **Backup.** The guest is left out of iCloud and computer backups unless the
  user turns on Include in backups. Its snapshots are never backed up.
- **Snapshots.** A snapshot is an APFS clone of `data.img` with a copy of
  `instance.json`, in `Library/Guest/Snapshots/` (`SnapshotStore`): it costs
  nothing when made and then the blocks the guest rewrites. It keeps the data
  and the record of its Android, not the guest's choices, the system disk or
  the adb key; a restore keeps the installation's identity. Create, Restore and
  Delete run only while the guest is stopped
  (Home, with no run active, pending or launching, since iOS takes no file lock
  on the disk), and Create only after Android's own shut down and with no
  update unfinished and on a volume that clones. Restore replaces the data and
  keeps nothing of the state it replaces; a snapshot of an older Android then
  updates at the next start, and one of a newer Android or an older layout is
  refused. Rename runs at any time.
  Each operation has one commit point (the snapshot's rename into place, the
  set-aside rename of a delete, the rename of a restore's clone over
  `data.img`): an error before it changes nothing, and one after it does not
  fail the operation. A restore that took effect and did not finish completes
  at the next start or launch (`TwoDiskStorage.recoverSnapshots` runs before
  the guest is read); until then Create and Restore are refused and every
  snapshot but that one can be deleted, and the start plan is the one that
  start finds, from the snapshot's own record. The update's copy is not a
  snapshot.
  The store's file calls are injected (`SnapshotFileOperations`).
  `SnapshotDriver` is what the screens call, and a refusal is a
  `SnapshotRefusal`. The driver reports what it does as `SnapshotActivity`,
  which the model keeps (`AppModel.snapshotGate`, observable); no start begins
  and the update's Restore waits while an operation runs; a rename waits for
  nothing and is waited for. A listing is never older than the last finished
  operation (`revision`), can come without its figures, and names the release
  the data is on; a cancelled one stops measuring.
  Home's Virtual machine section has a Snapshots row (how many, the space they
  use, Manage) that opens the Snapshots sheet: the list newest first, a page
  per snapshot (Restore, its name, Delete) and a page on the space they use,
  with Take snapshot at the foot as Start is on Home. `SnapshotsModel` holds
  what Home and the sheet show (names first, then the figures; they give way
  to the measuring state when an operation other than a rename finishes or a
  run begins or ends, and are measured when nothing was read, after an
  operation and when a run ends, never because a view appeared or while the
  guest starts) and runs
  each operation;
  `SnapshotWords` says every refusal, plan and figure in one place. A restore
  closes the sheet and Home says what happened, Start being next. The preview
  plays the same screens on mock snapshots (`-screen snapshots-…`).
- **Release notes.** The notes are one file of `Shared`, `WhatsNew.json`, that
  lists them per build, for every build (from build 9: build 5, the first
  public release, and builds 6 to 8 and 10 have none). One string table,
  `Localization/WhatsNew.xcstrings`, serves them.
  The first launch of an update that carries notes shows a ribbon above Start,
  on at most three launches, that opens them; a build shows the notes up to its
  own number and no later ones. An entry can name the devices it is for (more
  guest memory than a limit, an iPad, a device with haptics, a compass or a
  camera): the ribbon and its sheet announce only the entries the device meets.
  An entry can also name the feature it is about (Add to Home Screen); the
  ribbon and its sheet show it only where the feature's own rule offers it,
  asked each time they are drawn. Info keeps every entry as written
  (`WhatsNewRules`, `WhatsNewStore`). A launch with a launch setting, or one a
  test runs, shows no ribbon and keeps no record, and neither does a launch
  whose notes cannot be read, which also has no What's New row in Info.

## Runtime

The device application drives the Android runtime through the same loader as
Kernarium. A run starts when the guest's display has its size and the disk is
ready, on a thread of its own. Android's clean power-off releases the kernel's
resources and guest RAM and returns to Home, which states the storage the data
disk then holds and where Start boots again in the same process; an unclean
shutdown, a host stop or a cleanup that fails keeps the failure screen and
requires reopening Andarium. A run the host cannot give the guest's memory
ends before the kernel starts with a result of its own
(`KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED`), which the library's bare
out-of-memory status is not, and the failure screen names the
memory and says to lower it; the settings offer only sizes the process's
address space holds, so this is a fallback.

- **No Google apps, on purpose.** The boot screen of a new installation's
  first Android boot (a first start, and the retry of one that ended before
  Android booted) carries a panel saying that Android has no Google apps. Its
  strings are the only ones that name Google, which `scripts/localization.py`
  allows by key.
- **Slow start.** A start still on the boot screen after three minutes
  (`SlowStart`; eight for a guest's first start and the first start after an
  update, which also get a title that does not say the start is slower than
  usual; a first start that ended before Android booted is still the first,
  by the guest's `first-boot-pending` record, kept until `sys.boot_completed`
  is seen) puts a notice above the steps: the step under way and the time, and
  Stop. The boot goes on
  behind it and the notice goes with the boot; it has no dismissal. It sits
  above the steps and pushes them down; where the screen is short, the screen
  scrolls. The time is the start's, on the runtime's pause-aware run clock
  (`GuestRunClock`, which stops while the guest is held and while the
  application is away before the runtime runs) and kept through the restarts
  of a start that had not finished. Stop asks first (an alert with Stop and Cancel, in the
  screen's own appearance: the boot's dark screen asks the window for dark, so the
  alert over it is dark whatever the system's is, and Recovery's
  light screen leaves it to the system), then the
  application states the intent at once and the runtime's stop is requested
  off the main actor, on the queue of the runtime's other waiting calls, again
  every second until the run has ended (`RunStop`: the runtime drops a stop
  that arrives before its run begins). The first delivery that reaches a guest
  asks it to write its storage back before the host stops it (the runtime's
  `sync`, `StopSettle`: four seconds at most, as a lifecycle edge's budget); a
  delivery that finds none goes on to the host's stop and the next asks again,
  and the ask of a run that has ended is dropped before it calls the runtime.
  Stopping covers that wait. A stopped Android guest cannot be
  released, so the end is the end screen with its own wording
  (`RunEnd.stoppedByUser`), not Home, and a stop before the runtime runs ends
  at Home. A start the person stopped leaves its preparation to end: the
  guest's files stay its own until then, and a start asked for since, Erase,
  Create, the update's Restore and a snapshot rename wait for it. A process
  that ended in the boot leaves the runtime's session record with the console's
  last output of that boot, which the next bug report carries as the previous
  session's record.
- **Recovery the guest asked for.** `init: starting service 'recovery'` on the
  console (`ConsoleScanner.recoveryStartedCount`) makes the session a recovery
  session, as the launch's recovery mode does, whatever the start was: `adb
  reboot recovery`, the recovery image's own menu, Android's rescue after
  failed boots. The recovery's reboot is a restart into a normal session.
- **Time zone.** Every start gives the guest the host's zone
  (`GuestTimeZone`, the boot request's `android_time_zone`); the system image
  applies it unless the guest's user chose a zone
  ([Android README](../android/lineage-24/README.md#time-zone)).
- **Haptics.** On a device with a Taptic Engine the boot request states the
  haptics bit (`android_host_features`), which gives the guest a vibrator, and
  the application plays the vibrator's scores on Core Haptics. It connects to
  the guest's HAL over vsock 5557 (`HapticsSession`), plays only while it is in
  front and holds the guest running, and lets everything go the moment it is
  not. The HAL sends the effect or primitive and its duration, and
  `HapticShapes` decides what fills it, so the feel is tuned in an app update.
  Android's own vibration settings decide what the guest asks for. Haptics
  under Capabilities, a per-guest switch that is on until the person turns it
  off, decides whether this device plays it: off, the application holds no
  connection and no engine for the Taptic Engine and the guest keeps its
  vibrator. A harness launch can keep it off for its run
  ([Automation](#automation)). A game controller's rumble is not governed by
  it. An iPad, a Mac or the Simulator, which have no Taptic Engine, give the
  guest no vibrator and
  Capabilities no Haptics row. The wire is `HapticsProtocol`, whose golden frames
  the HAL's test shares (`tests/unit/andarium/fixtures`); the
  [Android README](../android/lineage-24/README.md) owns the HAL.
- **Heat.** The application tells the guest's thermal HAL how hot the device is
  over vsock 5559 (`ThermalSession`; the wire is `ThermalProtocol`, whose
  golden frames the HAL's test shares in `tests/unit/andarium/fixtures`).
  Android hears of heat only from iOS's critical state: nominal, fair and
  serious are none, critical is Android's critical, and the host never sends
  any other level. The severity is the hottest reported state of the last
  120 s: it rises at once and falls 120 s after critical ended, so a state
  that flickers around critical is one steady cap. A state Android is told
  nothing of, serious included, has no hold. While critical is told Android
  lowers the guest's frame rate to 30, and the first time in a run that this
  starts the session screen says so once. The session and stall records, the
  statistics keep iOS's real state, serious included. A system without the HAL hears nothing and shows no notice; the
  [Android README](../android/lineage-24/README.md) owns the HAL.
  Settings' Performance section, after Stats in a normal session, has "When
  hot": Limit frame rate, the default, or Full frame rate
  (`HeatLimitPreference`, stored until it is turned back on). Full frame rate tells
  Android none at every state of iOS's, critical included, at once when it is
  chosen, and shows no notice at any heat; Limit frame rate again tells it the
  policy's severity at once. The policy runs either way (`deviceIsHot` says
  the device is held at critical, for the section's footnote). The notice's
  words are a button that opens an alert whose Turn Off chooses Full frame
  rate in one tap; Dismiss only dismisses. A harness launch runs with the
  default and keeps nothing; `ANDARIUM_HEAT_LIMIT` (`1` or `0`) states it for
  the run ([Automation](#automation)).
  On an iPhone `PhoneHardware` knows, LineageOS presents itself as that iPhone:
  the model, maker and chip are properties of the boot request
  (`android_properties`) and the device name is set through the shell unless
  the person gave one. Home's "Notch space" (`NotchSpacePreference`, off by
  default), on a phone with a sensor housing, gives the display the housing's
  row and tells Android of a cutout at `sys.boot_completed`
  (`DisplayCutoutEmulation`): the image's tall cutout overlay, or, once a rooted
  start was given root for the shell in Magisk, overlays of the phone's own size
  that also place the clock and the icons beside its housing. Only root may make
  those; they stay in the guest and serve starts without root too.
  Home's Performance section has "120 Hz" (`HighRefreshPreference`, off by
  default) above JIT, drawn only where the screen shows at least 100 frames a
  second (an iPhone Pro, an iPad Pro; a screen between 61 and 99 does not
  qualify), where the display size the next start would have can carry the 120
  Hz mode, and never on a Mac or Vision Pro; Settings has no row of it,
  because Home, which shows only while no guest runs, is where the guest's
  configuration is. The section is one heading over a group for each setting
  that has an explanation (120 Hz, AOT, JIT), each a panel of the switch with
  its note beneath it, closer together than two sections are. The heading is
  drawn when one of the groups is, and a group that is not offered leaves out
  its note too.
  Turned on, the next start of Android asks the runtime for the 120 Hz display
  ([Display refresh rate](../Kernarium/README.md#display-refresh-rate)); a
  change applies at that start. The runtime
  decides once, at boot, whether the run has the mode, and the application
  uses what it granted (`display_refresh_hz` of the runtime's statistics) for
  the run. While iOS is in Low Power Mode, or after the window moves to a
  screen that shows under 100 frames a second, a granted run tells Android
  MODERATE (the thermal wire's severity between light and severe), which holds
  Android's render rate to 60 Hz; "When hot" still decides the heat levels,
  and the heavier severity wins. The screen's maximum is read again at each
  geometry update. A harness launch starts with the setting off, and
  `ANDARIUM_HIGH_REFRESH` (`1` or `0`) states it for the run, which keeps
  nothing.
- **Game controllers.** Each game controller iOS has connected is a HID
  device in the guest, made through the guest's relay over vsock 5558
  (`GamepadSession`; the wire is `GamepadProtocol`, whose golden frames the
  relay's test shares in `tests/unit/andarium/fixtures`). A DualSense,
  DualSense Edge, DualShock 4 or Xbox Wireless Controller presents as that
  device, bound by the guest's own driver, and any other as a plain HID
  gamepad that lists only what the controller has. Controllers are looked for
  during a normal run, and nothing is scanned for or paired. Their input
  reaches the guest only while Andarium is in front, and leaving the front
  lets every button go; what a controller holds on the return reaches the
  guest with its next report. Home, Menu, Options and Share are taken from iOS's
  gestures while a controller is held and reach Android as buttons (iOS's own
  Home setting decides whether Andarium may have Home). A button going down
  wakes a guest that suspended itself (`wake_for_input`, runtime API 34).
  Rumble, the light and the player number follow the guest's drivers onto
  the controller, and `Config/Info.plist` declares the extended gamepad
  profile. There is no switch. An image without the relay is given up on
  quietly after boot; the [Android README](../android/lineage-24/README.md)
  owns the relay.
- **Location.** Location is a per-guest switch under Capabilities on Home,
  off for a new guest and for one stored before. iOS is asked, for
  when-in-use access, only when the person turns it on; Andarium has no
  background or Always access. The position is read only while the switch is
  on, iOS allows it, Andarium is in front, a normal run goes on and the
  guest's GNSS receiver is powered, and goes to the
  [GNSS device](../darwin/device/gnss/README.md) through the runtime. A change
  of access or of Precise Location drops the held position first. An
  approximate position is passed as approximate; the guest owns what apps see
  of it. The row is dimmed and off when iOS refuses, the device restricts
  location or Location Services are off, and says Approximate location, with an
  explanation and Settings, when Precise Location is off. No test reads a real
  location.
- **Camera.** Camera is a per-guest switch under Capabilities on Home, after
  Microphone; it is off for a new guest and for one stored before. iOS is asked
  for camera access only when the person turns it on. The guest always lists
  the host's cameras (the layout is composed from them, with no permission
  asked), and the switch decides one thing: whether the host captures. It is
  read once, when the guest starts, as the boot request's
  `android_camera_mode` (runtime API 37): live only while it is on and iOS
  allows it, otherwise blocked, which gives the guest black frames, opens no
  capture session and reports Android's camera lens cover. The row is off with
  its reason when iOS refuses or the device restricts the camera, and turning
  it on then explains why and, for a refusal, offers Settings. A device with no
  camera shows the row off and disabled with a note and keeps the person's
  choice; a device has one when it has a built-in front camera, the rule the
  guest's layout follows, and the iPad app running on a Mac or on an Apple
  Vision Pro has none and looks for none (`CameraAvailability`; the system
  answers for a Vision Pro from iOS 26.1, and below it the hardware's name
  does: `IPadAppHost`). On an iPad, a
  session that other apps share the screen with pauses the camera (black
  frames) and shows a notice that goes with the interruption. When a guest app
  opens a camera while the session is blocked, the runtime reports it
  (`KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE`, runtime API 39) and the
  session shows a notice that Camera is off and can be turned on only from
  Home, for the next start. A dismissal holds for the run; an undismissed
  notice goes shortly after the guest closes the camera and returns at a
  later open; it is not shown on a device with no camera to share. Camera apps
  in the guest save a photo taken
  sideways the right way up from the guest's accelerometer, which the guest has
  wherever the device has sensors (see Motion below); an iPad app on a Mac and
  the Simulator have none. Where the
  host owns the rotation (an iPad window) it tells the runtime how the device
  body is turned from portrait (`CameraRotation`): each start derives every
  camera's sensor orientation from it, and a later turn turns the picture
  ([orientation](../darwin/device/camera/README.md#orientation)). The colour
  data a Bayer still teaches is kept in a folder of Application Support that
  backups leave out. The memory a live session's cameras hold comes out of the
  graphics budget ([camera device](../darwin/device/camera/README.md)): with
  Camera on, the VRAM the System sheet and the statistics show is the pool,
  the budget less that reserve, and the ruler's graphics warning follows the
  pool too; the memory offered and the default are unchanged. The reserve is the
  figure the runtime answers for this device's cameras and the colour data they
  have learned (`plan_camera_reserve`, which asks iOS for no permission and
  opens no session), read again while Camera is on whenever either may have
  changed; until the first answer, and where the runtime cannot discover the
  cameras, the sheet uses the tier's ceiling. The Simulator has
  no camera: the preview states the access (`-systemCamera` reads the
  Simulator's own permission) and the runtime serves a synthetic camera
  (`KERNARIUM_CAMERA_SOURCE`) to a run that has none, and no camera at all for a
  value it does not know. No test
  captures a real camera. The default suites boot no guest; the opt-in
  `CameraGuestTests` boots one through this runtime on the synthetic camera and
  reads the vendor SKU, the camera IDs and the lens cover of a live and a
  blocked session.
- **Root.** Root is a per-guest switch in a section of its own on Home, after
  Capabilities (`RootSection`; the guest's `RootState`, off for a new guest and
  for one stored before). Home shows only while Android is stopped, so a change
  applies at the next start. Turning it on asks first (what an app allowed in
  Magisk can do); turning it off applies at once. The row exists only where the
  release this copy carries holds root (`RootRelease`: the manifest's `root`
  object and the files in the release's `root/`, which the
  [Android release](../android/lineage-24/README.md) makes and
  `stage-system-image.py` stages); without it a start boots the release's own
  ramdisk. An installed system that came without root files has a button in the
  row's place: it downloads the pinned official Magisk app, makes the patched
  ramdisk from the system's own `init_boot` on the device (`MagiskBoot`,
  `MagiskInstaller`), records both in the installed manifest and turns root on. A start with root asks the runtime for
  the release's Magisk-patched ramdisk (`android_root`, runtime API 46;
  [boot selection](../darwin/device/block/README.md#boot-selection)).
  Before the boot screen ends, a start with root asks the guest whether the
  official Magisk app is there (`RootAppStep`, over the application's adb
  channel) and puts the release's own copy in when it is not (a regular file
  reached under `root/` with no link in its path, opened once without following
  one and sent from that descriptor, `RootRelease.openApk`): the screen adds
  a fifth step, Magisk app, after Startup only for a start that installs, and
  the check, the install and the opening of the app are bound together to
  45 s. The app is opened by the activity the package manager names as its
  launcher (`MagiskAppOpening`: `am start` resolves default activities only, so
  the launcher intent never resolves): after a bounded wait for user 0's unlock,
  up to five tries, each checked against the activity manager's one current
  focus record. The tries run even when the unlock never shows: a start while
  locked either fails, and is recorded as not opened, or succeeds. An app that
  does not open is no failure and no alert; `andarium/root.txt` says so. The step belongs to its boot and to the period of the switch it began in:
  after a Stop, a restart, a later start or a change of the switch its result
  adds nothing to the state and raises no alert or step (an app that is in
  the guest and not recorded is installed over at the next start). A package of that name at the
  carried version that the application did not put in is installed over like
  an older one, since only its signature says whose it is and the package
  manager checks it and refuses another signer; a newer one is left alone,
  since Android refuses a downgrade, and none is ever uninstalled. A failure is an alert and
  the boot goes on with root on. A start with root that ends before Android
  boots (a guest panic or exit the previous session's record shows, a restart
  loop, or a Stop by the person) is the fallback's: starts boot without root
  until the person chooses, and a Home card offers Try with root again, which
  also starts Android with root through the Start button's path, and Turn
  off root (or, after a Stop, Start without root); a panic ends the process, so
  the next launch reads the previous session's record, which names the ramdisk
  of the boot that ended (`SessionRecord.ramdisk`) and how far it got: a boot
  with the Magisk ramdisk that ended in a guest panic or an error exit before
  Android's boot completed, whether or not init had printed anything, is a
  failed start, and one that restarted into recovery or never began is not.
  What a start asks for (`RootState.startRooted`) is never saved. The same
  record decides the rest: the Magisk app step and a stopped start's card
  follow the boot the record names (a boot that restarted in place into
  recovery, or never began, installs nothing), the end of a run reads it too
  (a kernel's error exit pauses root without a relaunch), and the bug report
  says from it which ramdisk the last boot used. The previous session's record
  stays where the runtime wrote it until the pause decided from it is saved,
  no run begins while that save is owed, and a start with root whose settings
  cannot be saved begins without root. The record of a run is moved aside as
  the next run is asked for (`RunRecord`), so the one at its place is always
  the current run's and no time has to tell them apart; one that cannot be
  moved is emptied, and one that can be neither moved nor emptied is not read.
  The record a failed start's pause was decided from is moved aside as soon as
  the pause is saved, so that a relaunch after Try with root again does not
  read it again. Restoring a snapshot forgets what the application
  remembers of the Magisk app (`RootState.dataDiskReplaced`), since the
  snapshot's disk may not have it, and so does a restore that replaced the
  disk and could not finish, at once and not at the recovery that ends it; the
  switch stays. The bug report carries `andarium/root.txt` (the
  switch, the ramdisk of the last start, the Magisk version the release
  carries, why the last install of the Magisk app was made and whether the app was then on the screen). The wording is in `RootWords`. Three rules are one constant each in AndariumCore:
  when an absent Magisk app is installed again (`RootInstallRule`), what becomes
  of the app when root is off (`RootOffRule`), and what a failed start with root
  does to the switch (`RootFallbackRule`).
- **Input.** The shared [GuestHost input](../GuestHost/README.md) maps up to
  ten fingers onto the guest's display; the hardware keyboard and mouse are
  the next item. A touch the system takes over reaches
  Android as a cancelled touch, not a lift, so no tap follows. Only the side
  keys take touches: while they are open, a touch that begins anywhere else
  reaches Android from its first event and closes the keys
  (`SessionState.closeSideKeys`, called by the display's `touchDidBegin`).
  Beside the closed keys their tip, while it shows, takes touches on its card
  alone (on a Mac, where an iPad app's popover keeps its dimming view, every
  touch), and it goes with the closed keys when they open or the session ends.
- **VoiceOver.** A new screen, the start of a run and how an operation ended
  are announced (`VoiceOver`), the busy and low-space screens are modal, and the
  closed side keys carry actions that move them. Android's display is one
  element (`GuestDisplayAccessibility`) whose value is Android's screen reader's
  state and whose activation sends Android no touch; while the screen reader is
  on the display is a container of one direct-touch element, so touches reach
  Android and it speaks instead of VoiceOver, with no rotor setting needed
  ([Android's screen reader](#android-operations)). The sentence of a run's
  start is announced at high priority, which VoiceOver's own speech of the
  element its cursor lands on after the screen change does not cut off; the
  display's element takes VoiceOver's focus once it has been spoken, with
  nothing over the display, and the card that asks waits for it too (at most 15
  seconds, where VoiceOver reports no end); the card then takes the focus on its
  own title. Each option row on Home
  reads as one element, its control, which carries the row's title as its label
  (the title is no second stop), followed by the line under it.
- **Keyboard and mouse.** The guest has a hardware keyboard and a mouse
  (virtio-input devices, plugged and unplugged at run time) while iOS has the
  hardware. Every key goes to Android as its position (the usage on the HID
  Keyboard/Keypad page), Android owns the layout and the repeat, and Andarium
  keeps no key command or reserved combination: what iOS keeps for itself
  (Command-Tab, Command-H, the Globe key) never reaches the app. The display
  holds the keyboard focus, which shows no software keyboard, and gives it up to
  a sheet, an alert or the bug report's share sheet while one is open, so
  Andarium's own text fields take the keys then and a sheet without a field
  takes them from the guest all the same. The keyboard is plugged while iOS
  reports one and by the first key that goes down, and unplugged 3 s after the
  last report or key and once no key is held (`HardwareInputPolicy`). The system
  pointer is the guest's mouse on an iPad: Android draws its cursor and the
  system pointer is hidden over the display. The mouse is plugged once iOS
  reports one and the person has moved it over the display, clicked or scrolled,
  so a mouse the Simulator reports without one being there plugs nothing; the
  pointer's position uses the touchscreen's range and its buttons and scroll are
  the mouse's, once the guest has registered the pointer (runtime API 35 reports
  the registration of the keyboard and of the pointer and publishes both off
  when a boot ends); until then, and for good on a kernel that never registers
  it, the system pointer's clicks are touches and its use only plugs the mouse.
  The devices need a kernel that follows configuration changes (the hot-plug
  topic of the virtio-input devices), which the accepted kernels have; on a
  kernel without it they plug and nothing sees them, and input sent is
  dropped. A touch of the pointer is the
  mouse's or a finger's from its beginning to its end. An iPhone has no pointer
  to give the guest, and Settings' "Use the pointer as touch" (off for a fresh
  install, kept as `PointerPreference`, absent on an iPhone) leaves the guest
  without a mouse so that the pointer's clicks are touches. An Android
  application that captures the pointer lights an LED on its mouse; the
  application then asks the scene for the pointer lock and sends the mouse's raw
  motion, buttons and scroll from Game Controller, and where iOS refuses the
  lock (a window that is not full screen) the raw motion goes on while the
  pointer is over the display (`PointerCapture`); the capture waits while a
  sheet or an alert is up and ends with the guest's boot. Leaving the front, the
  end of a run, a sheet, a keyboard that goes and the window losing key release
  every key and button and end the pointer's contact; the devices exist only
  while a run goes on, and plug once the guest's console has answered
  (`KeyboardMouse`). Caps Lock is Android's: the application taps it once before
  a key when iOS's state differs from the light Android reports
  (`CapsLockReconciler`). Android hides its on-screen keyboard while it has a
  hardware keyboard.
- **Resolution.** Home's Guest system section has a Resolution row: the pixels
  the guest's display will have, and the end's name where the choice is an
  end (Native, Fastest), with Change opening the list of every choice with its
  pixels and density. The choices (`ResolutionChoices`) are 1.0, every 0.5 step
  that is at least a quarter below the screen's native scale, then native. The
  guest's pixels are the display's points times the choice, rounded down, and
  its density is 160 dpi a point of choice (`GuestDisplayLayout`), so
  Android's interface keeps its size at every choice. The row states what the
  next start gives: the display is the window less the session's safe area
  (`SessionDisplayArea`, read by `DisplayAreaProbe`), and follows a window as it
  resizes. A choice applies at the next start. Native is stored as the symbol
  and follows the screen, and a stored number that is no choice of this screen
  goes to the nearest one. A person who never chose gets the choice nearest 1.5
  (240 dpi) where the system holds the application to 4 GiB of memory or less,
  else the choice nearest 2.0 (320 dpi), which is native only where native is
  that near (`DisplayOptions`). The Retina mode an earlier version stored is no
  choice, so everyone coming from it gets this default; a choice made on Home
  is stored as a scale and stays. Native's host allocations count against the
  same iOS process limit as guest RAM; neither the graphics-memory estimate
  nor the memory default counts the display's pixels.
- **JIT.** A build on a device has a JIT row in Home's Performance section,
  off until the person turns it on and kept for the install
  (`JitPreference`). An iOS
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
  under the row. The note's ready sentence shows only while the row is on. A
  guest changed after the memory was prepared needs Andarium
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
  slower than on gadgets, and a long single-process computation runs faster.
- **AOT.** Android's system libraries and boot image run from machine code
  translated ahead of time
  ([`docs/architecture.md`](../docs/architecture.md#ahead-of-time-code)). A
  build that embeds `UmlArm64Aot.framework` (what Acknowledgements reads) has an
  AOT group in Home's Performance section, between the 120 Hz group and the
  JIT group: a switch, on by default and kept for the install
  (`TranslatedLibrariesPreference`), with the note beneath its panel; a fresh
  install and an update start on, and a harness launch starts on and keeps
  nothing. The choice is read at each start. Off, the boot request keeps the
  release out of the launch (`android_aot_disabled`,
  [Kernarium](../Kernarium/README.md#automation-runs)) and Android runs on
  the engine alone, as in a build without the framework; a running Android is
  not changed. While the next start runs on the JIT, which does not use the
  translated code, the row is dimmed, its switch cannot be changed and the line
  beneath the group says so. Each start writes what it chose beside the session
  record (`start.txt`, `StartRecord`), kept with it at the next launch, and a
  bug report carries both as `andarium/start-current.txt` and
  `andarium/start-previous.txt`: `translated-libraries` is `on`, `off`,
  `not-used-with-jit` or `unavailable` (a build without the framework). The
  record names the process and the second of its start, and is a session's
  only when that session's record names the same process and began no earlier;
  a choice whose start left no session record is copied as
  `andarium/start-current-unmatched.txt` or
  `andarium/start-previous-unmatched.txt`, and the session records are always
  copied.
- **Motion.** The guest has the device's accelerometer and gyroscope wherever
  the device has them, with no setting and no permission. An iPad app on a Mac
  and the Simulator have none, so nothing is read there, and Android still
  lists both sensors. The host reads a sensor only while the guest is in front
  and an application in it uses the sensor, and sends samples over private
  vsock port 5556 from the process's one Core Motion manager, which is never
  released (a released manager crashes inside Core Motion). A failure of the
  device's sensors stops them and keeps the connection, which also carries the
  orientation; they are tried again when the guest's demand changes or the
  application next updates the session. The system image must include the
  [motion sub-HAL](../android/lineage-24/README.md#motion-sensors).
  The magnetometer, for an image that lists it (the `compass` host feature),
  is Core Motion's calibrated device-motion field with Core Motion's accuracy
  as Android's, read while an application uses it, with no setting and no
  permission. Core Motion's request to move the device for calibration is no
  failure; a start that delivers nothing in 5 s without that request, or any
  other Core Motion error, turns it off for the rest of the run, where the
  accelerometer and gyroscope are tried again as above. A run ends with the
  foreground, a changed mount or a restart, not with a reconnect. From iOS 27.2
  a motion status of denied or restricted hides it from the guest; nothing is
  requested. A launch with `ANDARIUM_COMPASS_READOUT=1`
  prints, at most about twice a second, the Core Motion sample behind the
  guest's field with iOS's own attitude and heading.
- **Text messages.** The Developer section of Settings has Send text
  message… under ADB over network, in every build configuration, while Android
  runs in a normal start (`Capabilities.device`). Pressed, the row closes
  Settings and a bar rises over the running Android, above the keyboard, which
  does not move Android. The bar has a
  title (the row's symbol, "Text message" as the heading VoiceOver starts on,
  and a line saying Android receives it as an incoming text) with Close, a
  sender (the last one used, else Andarium; it is cleaned before it is judged),
  a message box, which alone asks iOS for a one-time code, and Send. While a
  message is being sent the fields refuse edits and keep the keyboard. The
  session owns the bar (`TextMessageState`): a send that is under way ends
  with the run, and its late answer is dropped. Send waits while Android is
  paused for lack of space, and the keyboard goes while it is, so the pause
  card does not cover the bar's title. The words say only what is known: that
  Android took the message, already had it, did not receive it, did not confirm
  it, or cannot tell, and VoiceOver hears them. Sending again, within the run, a
  message that Android did not confirm asks about the first send instead of
  handing it over again (`GuestSms.Retries`); "not sent" is said only of a first
  send. When a message may have arrived, the bar says so, also the next time it
  opens after a session that ended before a send's result was shown, and sending
  it again is then a new message (`TextMessageNotice`). Sixteen messages waiting
  for Android to confirm them refuse a new draft until Android restarts. Each
  message goes to the guest over vsock port 5560 (`GuestSmsChannel`), not over
  adb. The sender and text rules and the limits are `GuestSms`'s; the system
  image must include the
  [SMS service](../android/lineage-24/README.md#text-messages). The row does
  not check for it: a running image without the service leaves the connection
  unanswered, and the bar says the message was not sent after about 5 s.
- **Free space.** Free space is the space a write can use (`HostStorage`,
  `statfs(2)`), the one figure the watch, the start and transfer checks and
  every screen use; it leaves out the purgeable space iOS Settings counts, so
  Settings may show more. Once Android has booted a normal start, the
  application reads it (`RunSpace.nextCheck`: every 2 s near the limit, every
  30 s on a roomy device) and pauses the guest below 1 GiB
  (`RunSpace.pauseBelowBytes`), before the data disk can fail to grow. A full
  volume holds the guest's disk I/O until it has room instead of
  failing it ([kernel contract](../kernel/common/README.md#block-devices-and-filesystems)),
  and the pause stays the guard. The pause is the runtime's
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
  without one; each has a bar of its share, orange from 90%. The readings go
  on every second, but one reaches the views only when it draws something
  other than they show (`StatsPublishGate`, `Readout.Drawn`: the figures as
  written, the bars to a thousandth and the sparkline's slots), so a steady
  guest redraws nothing; the harness's console report prints every reading.
  Settings' Show on screen is kept for the install and restored at launch,
  off on a fresh one
  (`StatsPreference`, its own stored value beside `DisplayOptions`). A launch
  with any launch setting is a harness's and starts with them off, so a
  measured run never inherits the overlay; `ANDARIUM_STATS` (`1` or `0`) states
  the value. Such a launch keeps nothing, and neither does one that states
  the resolution, so what the person stored stays (`PreferenceKeeper`).
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
- **Network denied.** While the application is active and has a guest, the
  model watches the system's network path (`NetworkAccessWatcher`, an
  `NWPathMonitor` that calls back for a change of the path alone and makes no
  request). A path unsatisfied because iOS denies Andarium cellular data or
  Wi-Fi shows one notice per cause, a card on Home and a banner in a session
  (`NetworkWords`), with Open Settings and a dismissal that holds until the
  path has been allowed; no network at all, an inactive VPN and the Local
  Network permission are not this notice. The runtime is told which denial is
  in force (runtime API 36), and the session record notes it beside the counts
  of the guest's network (`NetworkRecord`), which the bug report carries in
  words.
- **Memory low.** The runtime answers iOS's memory pressure itself
  ([`docs/architecture.md`](../docs/architecture.md#memory-and-executable-code))
  and the session record keeps its level. The application shows no notice for
  it at any level and registers no handler for it: the level is the system's,
  not a measure of how near Andarium is to its own limit. The notices of a
  session share one column, which scrolls when they are together taller than
  its room.
- **Session ends.** The runtime records how each session ended
  ([`docs/architecture.md`](../docs/architecture.md)). The next launch keeps
  that record as the previous session's (`PreviousSession`) and, after an end
  that was neither Android's own nor a stop, Home shows a notice with its cause
  (`SessionRecord.notice`). A bug report carries the previous process's record
  for the whole life of the process, and beside it the last earlier run of
  this process (`RunRecord`), kept apart from it. At launch, an earlier run
  the process before left is not this process's: it becomes the previous
  session's when that process left no other record (a failed start whose
  pause was saved), and is dropped otherwise (`PreviousSession`). The runtime writes the record without the serial
  number, the adb key and the time zone the launch stated and without any
  hardware address in its colon spelling, so neither the bug report nor the
  previous session's copy has them.
  `ExitReports` keeps the exit and diagnostic reports
  MetricKit delivers beside the records. The record's stall slot (`StallRecord`)
  says when the guest's vCPUs stopped running on the host and what the host was
  doing; the bug report carries it in words, with the guest's `/proc` counters
  (`GuestProcFiles`), which adds at most 10 s in all to a report and says per
  file why one is missing. Everything Andarium keeps or copies of a session is
  bounded: a record longer than the runtime writes (16 KiB at most) is not read
  or copied, and a line says so; a report of the stalls is under 80 KiB per
  session whatever a record claims; and MetricKit reports are 30 at most, of
  1 MiB at most each.

## Android operations

Andarium is the adb host for its one guest. The runtime opens a connected
stream socket to the guest's vsock port 5555
(`kernarium_runtime_loader_connect_vsock`), so adbd is reached by no other
process and through no listening socket of the device's unless ADB over network
is on. `AndariumCore/Adb` speaks the protocol.

- **Key.** `AdbKeyStore` keeps one RSA-2048 key per guest in the Keychain, this
  device only and not synchronized. Its public key goes in each boot request
  (`android_adb_public_key`), which adbd authorizes for that boot.
- **Android's screen reader.** In a normal session, the Accessibility section,
  last in Settings, turns Android's screen reader (TalkBack) on and off by
  editing `enabled_accessibility_services` and `accessibility_enabled` in one
  adb shell command that reads the list it writes, so every other service the
  virtual machine has on stays on (`ScreenReader`, `Capabilities.screenReader`).
  Each command acts on the foreground user (`--user`), checks that lookup and
  the live list (split without pathname expansion; a list that is not a list of
  services is refused before anything is written), and fails when any write or
  the delete fails; another user's own setting stays as it is. Its reply, like a
  read's, is exactly the report `ScreenReader.parse` defines, ending with its
  terminator, and the screen reader is on only where the list names it and
  `accessibility_enabled` is on: any other reply is "couldn't check", never a
  state, and a change that left the two apart is not a change. Opening Android's
  Accessibility page needs a shell result with status 0. A change, a read it
  asks for and the opening of that page are the session's operations: the
  session's end (a power off, Android's own restart) cancels them, they ask
  nothing of Android once it has ended, and nothing they report reaches the next
  session. The switch follows what Android reports while Settings is open
  and, while VoiceOver runs, every few seconds. While VoiceOver runs and the
  screen reader is off, a card over Android says that VoiceOver cannot read
  Android's screen, that the screen reader can and how to come back (touch the
  side keys, on the edge they are on: the keys report it as they move), and
  asks; nothing turns the screen reader on but the person's own action, and
  turning it off, in Settings or in Android, ends the asking for the run.
  Turning it on says so with the way back, and so does the first read of a run
  that finds it on already while something covers the display; where the
  display takes VoiceOver's focus its own words carry it, and its hint ends with
  the way back. The card that says a system has no screen reader shows once per
  run, not once per virtual machine. The system image carries TalkBack and
  eSpeak NG ([Android README](../android/lineage-24/README.md#accessibility)); a
  virtual machine whose system has none shows the switch as unavailable and the
  card says so.
- **Create bug report and Install APK** are in every build, in a normal
  session. Settings lists Install APK and the file browser first, above the
  statistics, and the side keys' tip adds a line saying where Install APK is
  exactly where the row is offered (`Capabilities.offersApkInstall`). A bug
  report is Android's zip with Andarium's session records and
  MetricKit reports added under `andarium/`, the application's container paths
  taken out (`BugReportAttachments`); a zip that cannot be amended safely is
  shared as Android made it. An install streams the file to the package
  manager in pieces that never outlive their read (`FileChunkReader`). It
  reads the file's size once the coordinated read has the file, and refuses
  before the first byte when the device (`TransferRoom`) or the virtual
  machine's data partition lacks room for the app and its reserve
  (`InstallRoom`), or when the file is a single .apk of 4 GiB or more, which
  Android cannot install (`InstallRefusal.tooLarge`). The first attempt is
  always the plain install. When Android's own answer is the refusal of an app
  built for a much older version (`INSTALL_FAILED_DEPRECATED_SDK_VERSION`),
  and for no other answer, an alert asks whether to install it anyway, and
  Install anyway sends the same file again with the package manager's
  `--bypass-low-target-sdk-block`, once (`LegacyInstallFlow`). Nothing else in
  Andarium sends that flag, and the choice is kept nowhere.
- **An app in parts** installs from one file: .apks, .xapk or .apkm, never
  .zip, and an .aab is refused. The file is recognised by its content, not its
  name: an APK under one of those names installs as an APK, and a file that is
  not a ZIP, or is cut short, is damaged. It is read in place, never unpacked
  or copied, under bounds on what a hostile file can ask for (the entry count,
  the sizes, and the time, which a cancel ends; `ZipArchive`). The APKs inside
  are never opened. A bundletool .apks is read through its table of contents,
  and the Android and the processor of each variant and APK are matched as
  bundletool's matchers do, from the values and the alternatives each lists:
  exactly one variant must fit the guest, and a sibling made for a newer
  Android that the guest has excludes a variant (there is no falling back
  across it; a level a variant does not state is 0). A processor or any other
  restriction the reader has no name for, in a list of values or of
  alternatives and whatever its wire type, never means no restriction: it
  leaves its variant or APK out, and targeting it does not weigh on an APK
  that would be installed (a graphics API, a device tier) refuses the file. A
  variant for several processors together (the standalone builds of old
  Androids) or for a density is not weighed and is not chosen. A table that
  states a singular field twice, more than one SDK value, or a processor as
  its own alternative is damaged. Each variant keeps its own description of an
  APK; a path described twice in one variant is a damaged table. The parts of
  the chosen variant were each matched to the guest by their own targeting and
  none is dropped after that. The one variant installed
  has the splits of the processor the guest prefers, and a universal .apks
  installs as one APK; a table that cannot be read, or names no variant for the
  guest, is a damaged file and nothing is guessed from the names. The slices of
  the table's install-time asset packs go in the same session, as `bundletool
  install-apks` does, when their own targeting fits the guest (a language or a
  density is not weighed: every such slice is sent; a slice with a texture
  format refuses the file, since the guest's formats are not asked, while an
  app's own texture splits are sent). A
  fast-follow or on-demand pack, or one the table does not say is install-time,
  is not installed, and the result says the app's data was not all copied.
  The other layouts are
  classified by entry names and the container's own metadata. The parts go
  into one install session over the same connection, each read from the
  archive as it is written and checked when it ends; a session that was not
  committed is abandoned on a fresh connection when the install fails or is
  cancelled (not when the guest is gone or the session's creation was not
  answered, which leaves it to Android), and the older-app retry opens a new
  session with the flag. Only the package manager's whole answer being
  `Success` installs. Each APK inside is held to the 4 GiB limit; the file
  itself may be larger, and its footprint does not grow with it. A file with
  encrypted entries, no app, or parts that do not fit together is refused, and
  nothing is installed. The OBB files of an .xapk, or an .apkm that lists
  them, are copied once the app is in, to `Android/obb/<package>/` of
  internal storage and nowhere else, and only for a package the package
  manager has checked against the base APK: the session is created for the
  package the file names. Each goes to a hidden partial name as an import
  does, covered by the same ledger, and is moved into place only after it has
  been verified. Other data in the file (`Android/data`, say) is not placed.
  The copy has its own Cancel, and a stop or a failure in it leaves the app
  installed, with a result that says its data was not all copied. That
  result is a warning, not a success: the caution symbol in the caution
  colour, and a title of its own (`ApkInstallOutcome.words`).
- **Old app compatibility** (Settings > Apps) makes Android run one app built
  for an Android before 4.0 more like the older versions did, for an app that
  crashes. The image's `kernarium_oldapp` command lists the
  installed apps it can apply to and writes the switch
  ([`android/lineage-24`](../android/lineage-24/README.md)); `OldAppCompatibility`
  in `AndariumCore` is the only code that knows its words and its output, and
  everything else sees `OldApp`, `OldAppListing` and `OldAppWrite`. The
  Settings row exists only in a normal session, once a listing has found an
  app. Andarium lists once at the start of every run, with Settings closed,
  again after an install and each time the page opens; a listing that is
  older than a change the page made is dropped. The page's switches are off
  while the virtual machine cannot be asked (Recovery, paused for space). An image without the command, one
  that answers `list` as a usage error, or one that speaks a version of its
  listing this Andarium does not know, offers nothing. Turning the switch on is
  always asked; turning it off only when the app is open (`pidof`; the command
  does not say), and the image closes the app either way. A change the image
  does not take is said, and the list is read again. After Install anyway of
  an app that Android refused for a level below 14, the install card says where
  the switch is when the listing taken as the file went again and the one after
  the install differ by an app: the card's detail gains two sentences. The
  switch is the person's choice per app; Andarium keeps nothing of it.
- **Add to Home Screen** (Settings > Apps) puts an Android app's own icon and
  name on the iOS Home Screen, through Apple's Shortcuts. An App Intent
  (`OpenAndroidAppIntent`, `Shared/AndroidAppIntents.swift`) has one entity per
  launchable app, `AndroidAppEntity`, identified by the `package/class` of its
  launcher activity, so an icon made before Android was erased finds its app
  again once the app is reinstalled. A held tile in Shortcuts adds the icon; the
  row opens a chooser and a guide with Apple's Shortcuts button. There is no URL
  scheme, extension or shared container: the intent runs in Andarium's process.
  The list comes from the image's `kernarium_apps` tool
  ([`android/lineage-24`](../android/lineage-24/README.md)) through
  `AndroidAppsTool` and `AndroidAppRefresh` in `AndariumCore`, and is cached by
  `AndroidAppStore` (catalog, icons, and a small record per icon that survives
  an erase), so the tiles and the chooser answer while Android is off. The
  automatic listing of a run is its first: it is made 60 s after Android
  reports its boot complete, so that nothing starts in the guest while it
  settles, and again when a pause for space that kept it from being made is
  over. Earlier asks come from opening the chooser, from an installed app, from
  a launch that found its app missing, and, at once and with its app still
  opening, from the request that began the boot for an app the cached list does
  not name. The row is offered in a normal session until a listing finds that
  the image has no tool. A Mac and a Vision Pro have no Home Screen: there the
  intent runs, but the apps are no entities, nothing is written to Spotlight,
  the tiles are not told of them, and Android is asked only when an app is
  opened by name: every open takes one listing, with no icons, before it
  resolves the app, and a launch that found its app missing lists again to
  learn why. The guest is asked for one listing at a time. An icon
  is stored only as a picture the host drew itself (`FlatteningAppsTool`), and a
  label as the tool's own sanitising makes it, whatever the tool or the cache
  file says.
  A request from an icon follows the Start button's rules (`AppOpenPlanner`): a
  running Android opens the app once `AndroidAppReadiness` says it is ready
  (setup finished for the foreground user; for a request that began the boot or
  arrived during it, the home screen's first resume observed, and once the boot
  is over another app in front is no obstacle; a launch that was sent is never
  sent again), a stopped one is started, and a
  start that needs the person's answer (an unfinished update, too little space,
  a restore under way) waits for it in a card above Home's notice; a request
  during an erase is dropped. The request is in memory only and Don't open
  drops it. A failure is a banner over Android. The switch on the chooser governs Spotlight (Search) only: every
  write to the index takes one ordered path (`AndroidAppsIndexWriter`), judged
  against the switch and the catalog as they are when it runs, and a launch
  finishes a write that failed. The tiles, the entities and the Siri phrase do
  not follow the switch. Shortcuts shows a tile for each app from iOS 27 only,
  so the host offers the feature from there
  (`IPadAppHost.offersAddToHomeScreen`): an iOS 26 host is treated as a Mac or a
  Vision Pro is, and the entity, its query, the action and its automatic
  shortcut are all declared `@available(iOS 27.0, *)`, so Shortcuts lists no
  Android action before it, the Find action the system makes for an entity
  included (`andarium-open-intent-availability` reads the declarations and, for
  a built application, its extracted App Intents metadata). The feature is in every build
  configuration. Tests:
  `AndariumCore`'s `AppOpen*`, `AndroidApp*` suites, the hosted
  `AndroidApps*Tests` and the preview's `AddToHomeUITests`
  (`-screen add-home-*`, `Preview/AddToHomePreview.swift`).
- **File browser** in Settings browses the guest's internal storage
  (`/storage/emulated/0/`, the guest Files app's root) and imports and exports
  files over the same channel, in a normal session and whatever the guest's USB
  debugging setting is, since adbd serves the application's key either way
  (`Live/LiveGuestFiles.swift`, `AdbSync`, `GuestFiles`). On a page 520 pt or
  wider the list has Name, Modified and Size columns; Modified is as wide as the
  widest date the language writes, English's width at least, so a date stays on
  one line and Name gives up the room (`FileDateProbe`). One transfer runs
  at a time; Install APK and Create bug report wait for it. An import is
  written to a hidden `.andarium-*.part` name beside its destination and moved
  into place, so a stopped or failed one leaves nothing and a Replace keeps the
  original until the new file is whole. A partial file that the guest's abrupt
  end or iOS ending the application left behind is removed by the next boot
  that may find one, whatever its folder and with no import or open Settings
  needed: the boot looks when an import did not end cleanly, when an older data
  disk was put in place (a snapshot or update restore), and on the first boot
  of a guest an earlier version made, and an ordinary boot does not
  (`PartialFileSweep`, `PartialSweepLedger`). An import in progress is never
  touched. The move's own shell command then has
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
  authenticates the computer. A connection whose peer address is one of the
  device's own, loopback included, is closed at once, so no process of the
  device (another application, or adb on the same Mac when Andarium runs on
  that Mac) reaches the guest's adbd through the bridge; a computer on the
  network, another Mac included, is served. iOS asks for Local Network access
  once, through `NSLocalNetworkUsageDescription`. Nothing listens before the
  toggle.
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
means adding its entry to the script's table and running it. Magisk and BusyBox,
which the Android release carries only with root, are listed only where the
release the application carries has root (`requires_root`, answered from
`AppModel.rootAvailable`): Magisk's page holds the GPL-3.0 text, which the
generator takes from two upstream projects' own copies and refuses unless they
agree, and each page says where the source of both is available (the Magisk
project, at the tag of the version the release carries). The machine code
translated from the Android libraries (`UmlArm64Aot.framework`) carries its own
notices: the generator reads the AOT artifact's `licences.json`
(`--aot-artifact`, else the accepted artifact installed below
`.build/android/aot`) and lists each distinct notice once as "Translated system
libraries", a page opened from the page of Android Open Source Project, with the
paragraph the application's code carries above its row, in an application that
embeds the framework only.

## Languages

Andarium is in English, Simplified Chinese, Traditional Chinese (Taiwan usage)
and Japanese, and follows the language of iOS and the per-app language setting.
The strings are String Catalogs in `Localization/` (screens, permission texts,
What's New notes) and in `AndariumCore`'s resources. The JIT texts are their own
catalog, table `JIT`. The text of Follow developer on X is a
catalog of its own, table `FollowX`, built only where the feature is compiled
in (`EXCLUDED_SOURCE_FILE_NAMES` in `Config/Andarium.xcconfig`). English is the key (a readout or a
joined list has a named key whose comment says which readout it serves), and
the person's virtual machine is "virtual machine" in English, 虚拟机, 虛擬機器
and 仮想マシン, and its storage is 存储空间 in Simplified Chinese; the name of
iOS's own Settings page there keeps Apple's 储存空间. Chinese says 你 and writes no space next to Latin text; a
number keeps its space before its unit. Bug report and session record text, log
lines and technical names stay English. Text shown on a card, an alert or an
overlay passes `String.keepingTogether`, so a number stays with its unit and a
short quoted Chinese or Japanese name is not cut across two lines.

`scripts/localization.py check` is the catalogs' gate. After a build with
`SWIFT_EMIT_LOC_STRINGS=YES`, `scripts/localization.py sync --check
--stringsdata <DerivedData>` fails on a key the code uses and a catalog lacks,
or the reverse. It runs on a development build made with
`FOLLOW_X_HANDLE=example_handle` (the output of
`Andarium` and `AndariumPreview` together; one without the feature has none of
its strings and fails). A
string no catalog reaches (a `String` built in code) shows as English under
Xcode's Accented Pseudolanguage, so a run with it on is part of adding a
screen.

## Automation

A harness launch (`xcrun devicectl device process launch`, environment given
as `DEVICECTL_CHILD_` variables) reaches a state without touching the screen.
The names are the application's, in `Live/LaunchEnvironment.swift`;
`AndariumCore` knows the settings
(`LaunchOverrides.Setting`) and not the names. The `KERNARIUM_` names mean what
they mean in Kernarium; the automation Android runtime
(`package-ios-frameworks.sh --runtimes android --android-automation`)
additionally reads `KERNARIUM_ANDROID_ADB_AUTH`
([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
`KERNARIUM_DISPLAY_SCALE` (`native` or a number of guest pixels to a point)
states the resolution; the earlier `KERNARIUM_RETINA_MODE` (`1` native, `0`
one pixel to a point) is read where it is not stated.
`ANDARIUM_THERMAL_STATE` (`nominal`, `fair`, `serious` or `critical`) stands
for iOS's thermal state in the Simulator and in automation; only `critical`
reaches the guest as heat. `ANDARIUM_HEAT_LIMIT` (`1` or `0`) states
Settings' "When hot" for the run: `0` tells the guest nothing of the heat.
`ANDARIUM_HIGH_REFRESH` (`1` or `0`) states Home's "120 Hz" for the run. A
UI test's probe session (`ANDARIUM_INPUT_PROBE`) that states the thermal state
runs the thermal session against a stand-in for the guest's HAL, and its log
says what the guest would be told (`heat told <severity>`).
`ANDARIUM_FOLLOW_ORIENTATION` (`1` or `0`) states whether the guest follows the
device's orientation, where the device's idiom (an iPhone follows) would
otherwise decide.
`ANDARIUM_HAPTICS=0` plays none of the guest's haptics on the device for that
launch, as with the Haptics switch off, and leaves the stored switch alone; any
other value states nothing. A launch that states only it is still the person's
own. The device runners state it unless a run tests haptics (`--haptics`), and
a hosted test on an iPhone gives it as `TEST_RUNNER_ANDARIUM_HAPTICS=0`. It
silences the side keys' and rulers' own touch feedback too, which the Haptics
switch does not govern; a game controller's rumble is governed by neither.
`ANDARIUM_INPUT_PROBE=1` is the UI tests' launch: the session opens at once
with no guest behind it, and what the guest's keyboard and mouse would receive,
and the touches that reach its touchscreen, go to one accessibility element
(`inputProbeLog`) instead of the runtime; Settings carries a text field
(`inputProbeField`) that takes the keys a sheet keeps. The probe's guest
registers each device it is plugged; `unregistered` makes it one that never
does. The launch states "Use the pointer as touch" off, hides the tips (but for
`ANDARIUM_INPUT_PROBE_TIP=1`, which shows the side keys' tip anew) and keeps
nothing.
`ANDARIUM_OLD_APPS_PROBE` (`apps`, `refuse` or `none`), with the probe, gives its
session a scripted list of two made-up old apps in place of the image's command:
`apps` lists them (one on and open), `refuse` also refuses every change, `none` is
an image without the command.
`ANDARIUM_ANDROID_APPS_PROBE=1`, with the probe, gives its session the played list of
made-up Android apps that Add to Home Screen's preview screens use, in the cache and
the App Shortcuts, so that Apple's Shortcuts shows their tiles under Andarium.
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
reads are `Shared/ConsoleReport.swift`'s.

The container's `Documents` is the harness's; Andarium never deletes a file it
did not create. The guest is under `Library/Guest/` and is not the harness's to
write. A launch boots the system the build carries, so another release means
another staged build. iOS asks to allow local network access only when the
guest itself reaches the local network, and only a person answers; a harness
run that provokes it waits for the focus
(`run_ios_device_android.py --wait-active`).
