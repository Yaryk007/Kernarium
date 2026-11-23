Kernarium
===

Run a real Linux kernel natively on iOS, through the magic of [User-mode Linux](https://en.wikipedia.org/wiki/User-mode_Linux).

## Apps

### Andarium

The app was released on App Store but removed by Apple later due to violation of App Review Guidelines:
```
Guideline 2.5.2 & 1.1 - Performance - Software Requirements


We noticed that the app includes functionality that isn’t focused on the experience for supported Apple platforms.


Specifically, your app allows users to replicate Android operating systems on iOS platforms.
```

An Android emulator app built on the Kernarium runtime is **no longer** available for download in App Store. Source code is available under [`Andarium/`](/Andarium/).

#### Sideloading

You can sideload the app with [AltStore](https://altstore.io) or [SideStore](https://sidestore.io).

The easy way is to add the source, so updates show up in AltStore or SideStore:

```
https://raw.githubusercontent.com/imxieyi/Kernarium/main/sideload/source.json
```

Alternatively, install the IPA from [releases](https://github.com/imxieyi/Kernarium/releases) by hand:

1. Download `Andarium-<version>.ipa` on the device. It's about 1 GB.
2. Open AltStore or SideStore, tap `+` in **My Apps** and pick the IPA.

A few things to know:

- iOS 26 or later is required.
- A free Apple ID works, but the app expires after 7 days and must be refreshed in AltStore or SideStore.
- With a free Apple ID, Android gets less memory. AltStore 2.2 or later keeps the increased memory limit, SideStore doesn't by default. Extended virtual addressing needs a paid developer account.
- JIT is optional. To use it, assign the universal script to Andarium in StikDebug once, then open Andarium from StikDebug.
- To update by hand, install the new IPA with the same Apple ID and the same tool. It replaces the old version and keeps your virtual machine.

### Kernarium

A generic Linux emulator app capable of running various Linux distributions. It's still at an early stage of development and isn't ready for release yet. Source code is available under [`Kernarium/`](/Kernarium/).

## Overview

The User-mode Linux kernel is compiled to native arm64 code, then linked to an iOS runtime framework. The kernel runs natively without emulation. Userspace programs are emulated using an [iSH](https://github.com/ish-app/ish)-style threaded interpreter, with SMP support. Guest GPU acceleration is provided by [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html), a translation layer from Vulkan to Metal.

This emulator architecture sits between a full system emulator (like [UTM](https://github.com/utmapp/UTM)) and a full userspace emulator (like [iSH](https://github.com/ish-app/ish)). It combines the benefits of a real Linux kernel and a userspace emulator that only needs to emulate userspace code. It's capable of running advanced workloads including Docker containers, Linux desktops, Wine/Proton and a full Android system at decent speed on modern iOS devices.

## Features

- Kernel
    - Mainline 6.18 LTS kernel for both Linux and Android guests
    - Supports most kernel features, including namespaces, SELinux and more
- CPU emulation
    - Supports `arm64`, `arm32`, `x86_64` guest instructions
    - Both interpreter and JIT execution modes
- GPU acceleration
    - Vulkan 1.4 via [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html)
    - OpenGL 4.6 & GLES 3.2 via [Zink](https://docs.mesa3d.org/drivers/zink.html)
    - Hardware video encoding and decoding (H.264, HEVC; VP9 decoding) via [VideoToolbox](https://developer.apple.com/documentation/videotoolbox)
- Storage
    - Supports virtual disks via UBD driver, with discard support
    - Supports any Linux filesystem including ext2/3/4, erofs, xfs, ...
    - Supports loop devices and FUSE
- Network
    - Full Linux networking stack
    - Guest network access via [libslirp](https://gitlab.freedesktop.org/slirp/libslirp)

## Build or Embed

Point your coding agent at this repo if you want to build the project or embed its runtime in other apps. The agent guide is [`AGENTS.md`](/AGENTS.md), and the documentation is under [`docs/`](/docs/).

Building Android system image requires a powerful remote x86_64 Linux building machine ([requirements](https://source.android.com/docs/setup/start/requirements)). Alternatively, you can download prebuilt images from [releases](https://github.com/imxieyi/Kernarium/releases) and point your coding agent at it.

## License

This project is licensed under GPL-2.0-or-later, except where a file's SPDX header says otherwise; see [`LICENSES/`](/LICENSES/).
