# GuestHost

GuestHost is the iOS source that [Kernarium](../Kernarium/README.md) and
[Andarium](../Andarium/README.md) share: what it takes to host one guest on a
Kernarium runtime framework and show it. Each application compiles
`GuestHost/Sources/` into its own module as a synchronized folder, so the types
are `internal` and `@testable import Kernarium` reaches them. It is not a
Swift package: the packages it would need are prebuilt XCFrameworks under
`.build/` and the runtime header outside any package root. What needs neither
(logic both applications decide the same way) is the exception: `Core/` is a
package with no UIKit, SwiftUI or runtime dependency, tested with `swift test`
(`guest-host-core`), and each application's own package or module imports it.

## Contents

```text
Core/                                  GuestHostCore, a Swift package both
                                       applications import: sizes and their text,
                                       the space a write can use on the device
                                       (HostStorage), the free-space thresholds
                                       (RunSpace), the start check's text
                                       (StartSpace), and the pause for space
                                       (SpacePauser, SuspensionCheck,
                                       SpacePause), and the hardware keyboard and
                                       pointer (the keys held down, Caps Lock, the
                                       pointer's position, buttons and scroll), and
                                       the display's scale: guest pixels to a point,
                                       or native, with the pixels and density it
                                       gives a view, the host's stop of a run
                                       (RunStop: kept until the run ends;
                                       StopSettle: the guest's write-back before
                                       it, bounded), and a monotonic clock that is
                                       not a required-reason API (MonotonicClock), and
                                       the update notice of a sideloaded application
                                       (Update/: the version file's reading and rules,
                                       the daily read, the record and its model, the
                                       words, and a String Catalog for them: see
                                       Update notice below)
Sources/KernariumRuntimeLoader.h/.mm   loads the embedded runtime framework and
                                       wraps its C API (Runtime/KernariumRuntime.h)
Sources/GuestDisplay.swift             the Metal view that shows the guest's display,
                                       its view controller, and the SwiftUI wrapper;
                                       the application gives it a scale
                                       (GuestDisplayScale)
Sources/GuestInput.swift               touch and pointer mapping onto the guest's
                                       touchscreen range, display viewport geometry
Sources/GuestHardwareInput.swift       what the display does with hardware keys and
                                       the system pointer, between UIKit's events and
                                       the application's sink
Sources/GuestResizeHold.swift          when the guest keeps its display mode while the
                                       host view changes size
Sources/SceneCoverage.swift            whether a window scene fills its screen
Sources/GuestRestartEvent.swift        a restart the guest asked for, as the runtime
                                       reports it
Sources/GuestStopDelivery.swift        how an application delivers a run's host stop
                                       (RunStop): off the main actor, on the queue of
                                       its runtime calls, again until the run ends, the
                                       guest's write-back first
Sources/LinuxKeyboard.swift            characters to Linux key codes
Sources/ZstdUtil.swift                 streamed, sparse zstd decode
Sources/MicrophonePermission.swift     the system microphone permission, read and requested
Sources/MetalShaderCache.swift         the bound on Metal's per-application shader cache
Sources/MesaShaderCache.swift          the place and size limit of Mesa's disk shader cache
Sources/GuestProcessorLimit.swift      the vCPU count the applications default to and offer
                                       at most, as the loader answers it
Config/Signing.xcconfig                the signing settings Kernarium's Xcode project
                                       uses as its base configuration and Andarium's
                                       `Config/Andarium.xcconfig` includes
Config/Update.xcconfig                 where a sideloaded application looks for its
                                       updates (the ignored `Update.local.xcconfig`,
                                       from its `.example`), which the same
                                       `Config/Andarium.xcconfig` includes
```

## Update notice

A sideloaded application learns of a newer version from one version file its
maker hosts: an AltStore-format source (what AltStore and SideStore read) with
one block of our own, `kernariumUpdates`, that adds a format number and a
release page per build. `Core/Sources/GuestHostCore/Update/` holds what both
applications decide the same way: the file's reading (`UpdateFile`), the link
rules (`UpdateLinkPolicy`), the one request (`UpdateReader`), the record and the
daily schedule (`UpdateStore`), the build's configuration
(`UpdateConfiguration`), the words (`UpdateWords`, in the package's own catalog)
and the model the screens read (`UpdateNotice`). An application supplies its
original bundle identifier (the `UpdateAppIdentifier` key of its `Info.plist`,
copied from `PRODUCT_BUNDLE_IDENTIFIER`: a sideloading tool re-signs the
installed application under another one), its name, the moment it reads and
the views; a build with no address has no configuration and no notice.

- The file is untrusted. Only a version, a build, a day, a size and links that
  pass the rules are shown or opened; every sentence is the application's own.
  A file that is not plain JSON in UTF-8 (the owner publishes plain JSON: no
  comments, no JSON5), over 1 MiB, or of another `format` is no answer; an
  entry that breaks a rule skips only itself; unknown fields and other
  applications' entries are ignored. The offer is the highest build above
  the installed one whose iOS range fits the device, never a lower build.
- A link is `https` on the file's own host or a host the build names
  (`UpdateLinkHosts`: exact names and `*.` patterns), with no user name, no port
  but the default and at most 2 KiB. A sideloading app's link is built from a
  fixed template, never taken from the file.
- The read is one GET, at most once in 24 hours, a few seconds after Home shows
  and only at the application's moment; it uses an ephemeral session with no
  cookies, cache or conditional header, a fixed `User-Agent` and
  `Accept-Language`, `https` only (redirects included), a 1 MiB and 15 s limit,
  and never runs under Low Data Mode or after a cancelled task. Any answer,
  a refused login included, uses up the day, and it does so before the body is
  decoded, off the main actor. A kept record is held to the rules again when
  it is loaded.
- The address is the maker's, in the ignored `Config/Update.local.xcconfig`
  (from `Update.local.xcconfig.example`: a host and a path, never a whole
  address, and the hosts the file's links may name besides its own). Only a
  Release build carries it; any other build, a fork's included, has no address,
  no read and no notice. `Config/Update.xcconfig` is the shared mapping an
  application's own xcconfig includes.
- `Sources/UpdateNoticeViews.swift` is the detail and Info's Updates section,
  drawn with a mark and the accent the application supplies. The application
  adds the card, which sits among its own notices.

## Signing

Both projects take their signing from `Config/Signing.xcconfig` (Kernarium as its
base configuration, Andarium through its own `Config/Andarium.xcconfig`, which
includes it), and it names no team. The signer's own Apple Developer team goes in
`Config/Signing.local.xcconfig`, which that file includes when it exists and Git
ignores:

```sh
cp GuestHost/Config/Signing.local.xcconfig.example GuestHost/Config/Signing.local.xcconfig
# then set DEVELOPMENT_TEAM in it
```

Without that file a build for the iOS Simulator works unsigned and a build for
a device stops with Xcode's "requires a development team" error. Name the team
in that file, not in Xcode's Signing & Capabilities editor, which would write it
into the tracked project. A worktree is a separate checkout and needs its own
copy to build for a device.
`Kernarium/scripts/audio-interrupter.py` reads the same file (or
`UML_DEVELOPMENT_TEAM`). The team is never tracked: the
`repository-environment-portability` test fails on a literal one.

## Rules

The display forwards up to ten simultaneous UIKit contacts through the runtime
API, each with an independent path and slot. Clipping a path to the visible
guest rectangle releases only that contact. A contact the host takes away
(`touchesCancelled`, a display mode change, the view leaving its window) is
cancelled in the guest instead of released (`GuestTouchPath.cancel()` and
`GuestTouchContacts.cancelAll()`, which call the runtime's `cancel_touch`), so
no tap or click follows; the guest-side contract is in
[`Kernarium/README.md`](../Kernarium/README.md#devices-and-display). An
application that draws controls over the display passes `touchDidBegin`, which
the display calls after it has sent the contacts of each touch that begins on
it; Kernarium passes none. An application that passes `accessibility`
(`GuestDisplayAccessibility`, in its own words) makes the display one VoiceOver
element with the application's `value`; activating it sends the guest no touch.
With `directTouch` the display is instead a container of one child element that
covers it and carries the words, the trait `allowsDirectInteraction` and
`silentOnTouch`, so touches reach the guest as they do with VoiceOver off and
VoiceOver says nothing of them; `takesFocus` moves VoiceOver's focus onto that
child each time it turns true (as the child appears, and when nothing covers
the display any more), and only the child does it (the trait and option on the
view itself do not pass touches). Without `accessibility` the display is no
element.
Kernarium passes none.

- The display takes hardware keys and the system pointer only for an
  application that gives it a `hardwareInput` (Andarium); Kernarium gives none
  and keeps UIKit's behaviour and its on-screen text path (`keyboardVisible`).
  A key is its position, the usage on the HID Keyboard/Keypad page, and the
  pointer's position is on the touchscreen's range, so Android's cursor is
  where a finger would be. The application decides whether the pointer is the
  guest's mouse (`routesPointer`, set once the guest has registered it) or only
  watched (`watchesPointer`: its use is reported, its touches stay the
  touchscreen's). A touch of the pointer is the mouse's or a finger's from its
  beginning to its end, and the system pointer is hidden over the display
  alone. Leaving the front, a sheet taking the keys, the window losing key and
  the pointer leaving lift every key and button the guest was told is down.
- A file here depends only on the system frameworks, the runtime header, and
  the prebuilt `LibZstd` XCFramework. It names neither application's views,
  settings or product text; an application supplies those.
- The applications build with the same Swift settings (Swift 5 language mode,
  main-actor default isolation, approachable concurrency), because these
  sources rely on them.
- Background running, picture in picture and the console are Kernarium's, and
  stay in `Kernarium/Kernarium/`.
- A change here is a change to both applications: build and test both.
