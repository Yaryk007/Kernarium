// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The root switch's model: what a release carries, what the guest keeps of
/// the switch, the three rules, the Magisk app check and
/// the boot screen's install step. The release files here are made up: no
/// Magisk file is in the repository.
struct RootReleaseTests {
    private func manifest(
        version: Any = 31_100, name: String = "v31.1", apk: String = "root/Magisk-v31.1.apk",
        image: String = "root/init_boot-magisk.img", withRoot: Bool = true
    ) -> Data {
        var object: [String: Any] = ["schema": 8, "release_id": "lineage24-test"]
        if withRoot {
            object["root"] = [
                "magisk": ["version_name": name, "version_code": version],
                "apk": ["path": apk, "sha256": String(repeating: "a", count: 64), "size": 10],
                "boot_image": ["path": image, "sha256": String(repeating: "b", count: 64), "size": 12],
            ]
        }
        return try! JSONSerialization.data(withJSONObject: object)
    }

    @Test func readsTheRootObjectOfTheManifest() throws {
        let release = try #require(RootRelease(manifest: manifest()))
        #expect(release == RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: "root/Magisk-v31.1.apk"))
        #expect(RootRelease.bootImage == "root/init_boot-magisk.img")
    }

    @Test func aReleaseWithoutRootOrWithAnUnreadableOneIsNoRoot() {
        #expect(RootRelease(manifest: manifest(withRoot: false)) == nil)
        #expect(RootRelease(manifest: Data("not json".utf8)) == nil)
        for version: Any in [0, -1, "31100", true, 3.5, 1e30, 2_147_483_648, UInt64.max] {
            #expect(RootRelease(manifest: manifest(version: version)) == nil, "version \(version)")
        }
        #expect(RootRelease(manifest: manifest(version: 2_147_483_647))?.versionCode == 2_147_483_647)
        // A number written as a fraction or an exponent is no integer, whatever its value.
        for written in ["31100.0", "3.11e4", "31100.5"] {
            let text = String(decoding: manifest(), as: UTF8.self).replacingOccurrences(
                of: "\"version_code\":31100", with: "\"version_code\":\(written)")
            #expect(text.contains(written))
            #expect(RootRelease(manifest: Data(text.utf8)) == nil, "written \(written)")
        }
        #expect(RootRelease(manifest: manifest(name: "")) == nil)
        // The name is one short line: it becomes a line of a bug report.
        for name in ["v31.1\nroot: on", "v31.1\r", "v\u{2028}31", "v\u{85}31", "v\u{0}31", String(repeating: "\u{E9}", count: 17)] {
            #expect(RootRelease(manifest: manifest(name: name)) == nil, "name \(name.debugDescription)")
        }
        #expect(RootRelease(manifest: manifest(name: String(repeating: "v", count: 32))) != nil)
        #expect(RootRelease(manifest: manifest(name: String(repeating: "v", count: 33))) == nil)
        // The patched image is the one name the runtime reads.
        #expect(RootRelease(manifest: manifest(image: "root/other.img")) == nil)
        for path in ["Magisk.apk", "/root/Magisk.apk", "root/../Magisk.apk", "root//Magisk.apk", "root/./a.apk", "root/", "other/Magisk.apk"] {
            #expect(RootRelease(manifest: manifest(apk: path)) == nil, "path \(path)")
        }
    }

    @Test func aPartialRootDirectoryIsNoRoot() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("root-release-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: folder) }
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("root"), withIntermediateDirectories: true)
        let data = manifest()
        #expect(RootRelease.carried(in: folder, manifest: data) == nil)
        try Data("ramdisk".utf8).write(to: folder.appendingPathComponent(RootRelease.bootImage))
        #expect(RootRelease.carried(in: folder, manifest: data) == nil)
        try Data("app".utf8).write(to: folder.appendingPathComponent("root/Magisk-v31.1.apk"))
        let release = try #require(RootRelease.carried(in: folder, manifest: data))
        #expect(release.apkURL(in: folder).lastPathComponent == "Magisk-v31.1.apk")
        // A link in the place of the image is not the release's own file.
        let image = folder.appendingPathComponent(RootRelease.bootImage)
        let elsewhere = folder.appendingPathComponent("elsewhere.img")
        try Data("ramdisk".utf8).write(to: elsewhere)
        try FileManager.default.removeItem(at: image)
        try FileManager.default.createSymbolicLink(at: image, withDestinationURL: elsewhere)
        #expect(RootRelease.carried(in: folder, manifest: data) == nil)
    }

    /// A release folder with both files, the app under `root/`.
    private func releaseFolder() throws -> URL {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("root-release-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("root"), withIntermediateDirectories: true)
        try Data("ramdisk".utf8).write(to: folder.appendingPathComponent(RootRelease.bootImage))
        try Data("app".utf8).write(to: folder.appendingPathComponent("root/Magisk-v31.1.apk"))
        return folder
    }

    @Test func anAppReachedThroughALinkedDirectoryOrNotARegularFileIsNoRoot() throws {
        let folder = try releaseFolder()
        defer { try? FileManager.default.removeItem(at: folder) }
        // The directory the app is under is a link to a directory elsewhere.
        let elsewhere = folder.appendingPathComponent("elsewhere")
        try FileManager.default.createDirectory(at: elsewhere, withIntermediateDirectories: true)
        try Data("app".utf8).write(to: elsewhere.appendingPathComponent("Magisk-v31.1.apk"))
        try FileManager.default.createSymbolicLink(
            at: folder.appendingPathComponent("root/linked"), withDestinationURL: elsewhere)
        #expect(
            RootRelease.carried(in: folder, manifest: manifest(apk: "root/linked/Magisk-v31.1.apk")) == nil,
            "a linked directory")
        // A pipe is not a file, and opening it must not wait for a writer.
        let pipe = folder.appendingPathComponent("root/pipe.apk")
        try #require(mkfifo(pipe.path, 0o600) == 0)
        #expect(RootRelease.carried(in: folder, manifest: manifest(apk: "root/pipe.apk")) == nil, "a pipe")
        // The ordinary layout is root.
        #expect(RootRelease.carried(in: folder, manifest: manifest()) != nil)
        // A `root` that is itself a link leaves the release.
        let moved = folder.appendingPathComponent("moved")
        try FileManager.default.moveItem(at: folder.appendingPathComponent("root"), to: moved)
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("root"), withDestinationURL: moved)
        #expect(RootRelease.carried(in: folder, manifest: manifest()) == nil, "a linked root directory")
    }

    @Test func aComponentThatIsOnlyDotsToTheFilesystemNeverLeavesRoot() throws {
        let folder = try releaseFolder()
        defer { try? FileManager.default.removeItem(at: folder) }
        // A file outside root/, in the release folder itself.
        try Data("not root's app".utf8).write(to: folder.appendingPathComponent("outside.apk"))
        // A NUL ends the name the filesystem is given: this component is ".." to it and not to Swift.
        let sneaky = "root/..\u{0}x/outside.apk"
        #expect(RootRelease(manifest: manifest(apk: sneaky))?.apkPath == sneaky, "the manifest reader sees a name")
        #expect(RootRelease.carried(in: folder, manifest: manifest(apk: sneaky)) == nil)
        let release = RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: sneaky)
        #expect(throws: POSIXError.self) { try release.openApk(in: folder, capacity: 16) }
        // The same for a NUL anywhere, and for the plain forms.
        for path in [
            "root/Magisk-v31.1.apk\u{0}", "root/Mag\u{0}isk-v31.1.apk", "root/.\u{0}x/outside.apk", "root/./Magisk-v31.1.apk",
            "root/../outside.apk", "root//Magisk-v31.1.apk", "",
        ] {
            let odd = RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: path)
            #expect(throws: POSIXError.self) { try odd.openApk(in: folder, capacity: 16) }
        }
        // A name that only looks like the plain ones is a name, and the file is read.
        let ordinary = RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: "root/Magisk-v31.1.apk")
        #expect(try ordinary.openApk(in: folder, capacity: 16).size == 3)
    }

    @Test func theAppIsUploadedFromTheFileThatWasOpenedAndChecked() throws {
        let folder = try releaseFolder()
        defer { try? FileManager.default.removeItem(at: folder) }
        let release = try #require(RootRelease.carried(in: folder, manifest: manifest()))
        let reader = try release.openApk(in: folder, capacity: 16)
        #expect(reader.size == 3)
        // The path is replaced after the open by a link to another file.
        let apk = release.apkURL(in: folder)
        let other = folder.appendingPathComponent("other.apk")
        try Data("not the release's app".utf8).write(to: other)
        try FileManager.default.removeItem(at: apk)
        try FileManager.default.createSymbolicLink(at: apk, withDestinationURL: other)
        #expect(try reader.read(upTo: 16) == Data("app".utf8))
        // A new open refuses the link, and so does a path of another shape.
        #expect(throws: POSIXError.self) { try release.openApk(in: folder, capacity: 16) }
        let escaping = RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: "root/../other.apk")
        #expect(throws: POSIXError.self) { try escaping.openApk(in: folder, capacity: 16) }
    }
}

struct RootStateTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    @Test func aGuestStoredBeforeTheSwitchHasRootOff() throws {
        let old = """
            {"cores":6,"memoryMiB":4096,"capacityGiB":8,"microphone":false,"includeInBackups":false}
            """
        let guest = try JSONDecoder().decode(Guest.self, from: Data(old.utf8))
        #expect(guest.root == RootState())
        #expect(!guest.root.on && !guest.root.startRooted && guest.root.attention == nil)
    }

    @Test func theStateIsKeptWithTheGuest() throws {
        var guest = Guest.fresh(on: host)
        guest.root.setOn(true)
        guest.root.startRooted = true
        guest.root.appVersionCode = 31_100
        guest.root.appEverHandled = true
        guest.root.attention = .stoppedStart
        let back = try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest))
        // The request of a start is not the guest's: it is never saved, so a
        // process that ends between the save and the runtime's start leaves
        // nothing that says a boot used the Magisk ramdisk.
        var kept = guest.root
        kept.startRooted = false
        #expect(back.root == kept)
        #expect(!back.root.startRooted)
        #expect(!String(decoding: try JSONEncoder().encode(guest), as: UTF8.self).contains("ooted"))
        // A guest saved by an earlier build, which wrote the flag, reads it as no start.
        let earlier = """
            {"cores":6,"memoryMiB":4096,"capacityGiB":8,"microphone":false,"includeInBackups":false,
             "root":{"on":true,"rooted":true}}
            """
        let old = try JSONDecoder().decode(Guest.self, from: Data(earlier.utf8))
        #expect(old.root.on && !old.root.startRooted)
        // A stored value this reader does not know is no attention.
        let odd = """
            {"cores":6,"memoryMiB":4096,"capacityGiB":8,"microphone":false,"includeInBackups":false,
             "root":{"on":true,"attention":"something-new"}}
            """
        let read = try JSONDecoder().decode(Guest.self, from: Data(odd.utf8))
        #expect(read.root.on && read.root.attention == nil)
    }

    @Test func turningRootOffEndsThePeriodAndWhatWaitedForADecision() {
        var root = RootState()
        root.setOn(true)
        root.appVersionCode = 31_100
        root.appEverHandled = true
        root.attention = .failedStart
        root.setOn(false)
        #expect(!root.on && root.attention == nil && root.appVersionCode == nil)
        // The guest still has had the app.
        #expect(root.appEverHandled)
        root.attention = .failedStart
        root.tryAgain()
        #expect(root.on && root.attention == nil && !root.paused)
    }

    @Test func aStartBootsRootedOnlyWhenEverythingAllowsIt() {
        var guest = Guest.fresh(on: host)
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true))
        guest.root.setOn(true)
        #expect(guest.bootsRooted(mode: .normal, releaseCarriesRoot: true))
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: false), "a release with no root")
        #expect(!guest.bootsRooted(mode: .recovery, releaseCarriesRoot: true), "recovery has its own ramdisk")
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true, withoutRootOnce: true))
        guest.root.attention = .failedStart
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true), "paused after a failed start")
        guest.root.attention = .stoppedStart
        #expect(guest.bootsRooted(mode: .normal, releaseCarriesRoot: true), "a stop is no failure")
        guest.root.attention = .turnedOffAfterFailure
        guest.root.on = false
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true))
    }

    @Test func aStepThatRanEarlierAddsOnlyTheAppsBookkeeping() {
        var learned = RootState()
        learned.on = true
        learned.startRooted = true
        learned.appVersionCode = 31_100
        learned.appEverHandled = true
        learned.appInstall = .init(reason: .absent, installed: true)
        // The state as it is now: the person turned root off and on again, and a start paused it.
        var now = RootState()
        now.on = true
        now.attention = .failedStart
        now.adoptApp(from: learned)
        #expect(now.on && now.attention == .failedStart && !now.startRooted)
        #expect(now.appVersionCode == 31_100 && now.appEverHandled && now.appInstall == learned.appInstall)
        // Root was turned off while the step ran: no period begins, and the switch stays off.
        var off = RootState()
        off.adoptApp(from: learned)
        #expect(!off.on && off.appVersionCode == nil && off.appInstall == nil)
        // Not even that the application ever put the app in: after off and on, that would suppress the install.
        #expect(!off.appEverHandled)
        // A newer code already recorded is not lowered.
        var newer = RootState()
        newer.on = true
        newer.appVersionCode = 32_000
        newer.adoptApp(from: learned)
        #expect(newer.appVersionCode == 32_000)
    }

    @Test func aFactoryResetKeepsTheChoiceAndForgetsTheApp() {
        var guest = Guest.fresh(on: host)
        guest.root.setOn(true)
        guest.root.startRooted = true
        guest.root.appVersionCode = 31_100
        guest.root.appEverHandled = true
        guest.root.appInstall = .init(reason: .absent, installed: true)
        guest.root.attention = .failedStart
        let erased = guest.erased
        #expect(erased.root.on)
        #expect(erased.root.attention == nil && erased.root.appVersionCode == nil && !erased.root.appEverHandled)
        #expect(erased.root.appInstall == nil, "the last install's note outlived the data it was about")
    }
}

struct MagiskAppTests {
    private func output(_ text: String, exit: Int? = 0) -> GuestShellOutput { GuestShellOutput(text: text, exitCode: exit) }

    @Test func readsTheVersionCodeOfTheExactPackage() {
        #expect(MagiskApp.presence(of: output("")) == .absent)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk versionCode:31100\n")) == .installed(versionCode: 31_100))
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk.stub versionCode:1\n")) == .absent)
        #expect(
            MagiskApp.presence(
                of: output("package:com.topjohnwu.magisk.stub versionCode:1\npackage:com.topjohnwu.magisk versionCode:27000\r\n"))
                == .installed(versionCode: 27_000))
    }

    @Test func anAnswerThatIsNotTheLineIsNoAnswer() {
        #expect(MagiskApp.presence(of: output("", exit: 1)) == .unknown)
        #expect(MagiskApp.presence(of: output("", exit: nil)) == .unknown)
        #expect(MagiskApp.presence(of: output("Error: unknown option --show-versioncode")) == .unknown)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk\n")) == .unknown)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk versionCode:abc\n")) == .unknown)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk versionCode:-4\n")) == .unknown)
    }

    @Test func theCommandsNameOnlyThePackage() {
        #expect(MagiskApp.probeCommand == "pm list packages --show-versioncode com.topjohnwu.magisk")
        #expect(MagiskApp.resolveCommand.hasSuffix(" com.topjohnwu.magisk"))
        #expect(MagiskApp.uninstallCommand.hasSuffix(" com.topjohnwu.magisk"))
    }
}

struct RootRulesTests {
    private func state(period: Int? = nil, ever: Bool = false) -> RootState {
        var state = RootState()
        state.on = true
        state.appVersionCode = period
        state.appEverHandled = ever
        return state
    }

    @Test func anAbsentAppIsInstalledOncePerPeriodByDefault() {
        #expect(RootInstallRule.policy == .oncePerOnPeriod)
        #expect(RootInstallRule.opensAppAfterInstall)
        #expect(RootInstallRule.decide(presence: .absent, carriedVersionCode: 31_100, state: state()) == .install)
        #expect(RootInstallRule.decide(presence: .unknown, carriedVersionCode: 31_100, state: state()) == .install)
        // The person removed it after the application put it in: it stays removed.
        #expect(RootInstallRule.decide(presence: .absent, carriedVersionCode: 31_100, state: state(period: 31_100)) == .none)
        // An older app is updated whatever the period says.
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 27_000), carriedVersionCode: 31_100, state: state(period: 27_000)) == .install)
        // An app the state says the application put in for this release is left alone.
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 31_100), carriedVersionCode: 31_100, state: state(period: 31_100)) == .none)
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 32_000), carriedVersionCode: 31_100, state: state(period: 31_100)) == .none)
        // One of the carried version that the state does not say it put in is installed over.
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 31_100), carriedVersionCode: 31_100, state: state()) == .install)
        // A newer one is left alone, recorded or not: Android refuses the downgrade.
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 32_000), carriedVersionCode: 31_100, state: state()) == .none)
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 32_000), carriedVersionCode: 31_100, state: state(period: 27_000)) == .none)
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 31_100), carriedVersionCode: 31_200, state: state(period: 31_100)) == .install)
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 31_200), carriedVersionCode: 31_200, state: state(period: 31_100)) == .install)
    }

    @Test func theOtherChoicesOfTheDecisionAreOneConstantAway() {
        for (policy, period, ever, expected) in [
            (RootInstallRule.Policy.everyRootedBoot, 31_100 as Int?, true, RootInstallRule.Action.install),
            (.oncePerGuest, nil, false, .install),
            (.oncePerGuest, nil, true, .none),
            (.oncePerGuest, 31_100, true, .none),
            (.oncePerOnPeriod, nil, true, .install),
        ] {
            #expect(
                RootInstallRule.decide(
                    presence: .absent, carriedVersionCode: 31_100, state: state(period: period, ever: ever), policy: policy)
                    == expected, "\(policy) \(String(describing: period)) \(ever)")
        }
    }

    @Test func theRuleRemembersWhatItPutIn() {
        let first = RootInstallRule.recorded(state(), versionCode: 31_100)
        #expect(first.appVersionCode == 31_100 && first.appEverHandled)
        // A lower code seen later does not lower the memory.
        #expect(RootInstallRule.recorded(first, versionCode: 27_000).appVersionCode == 31_100)
    }

    @Test func rootOffKeepsTheAppByDefault() {
        #expect(RootOffRule.appPolicy == .keep)
        var off = state(period: nil, ever: true)
        off.on = false
        #expect(!RootOffRule.removesApp(state: off))
        #expect(RootOffRule.removesApp(state: off, policy: .uninstall))
        // Nothing to remove when the application never put the app in, and never while root is on.
        #expect(!RootOffRule.removesApp(state: RootState(), policy: .uninstall))
        #expect(!RootOffRule.removesApp(state: state(ever: true), policy: .uninstall))
    }

    @Test func aFailedStartPausesRootByDefault() {
        #expect(RootFallbackRule.behaviour == .bootStockOnce)
        var root = state()
        root.startRooted = true
        #expect(RootFallbackRule.failed(&root))
        #expect(root.on && !root.startRooted && root.paused && root.attention == .failedStart)
        var other = state()
        #expect(RootFallbackRule.failed(&other, behaviour: .turnOff))
        #expect(!other.on && other.attention == .turnedOffAfterFailure)
        var none = state()
        #expect(!RootFallbackRule.failed(&none, behaviour: .none))
        #expect(none.on && none.attention == nil)
    }

    @Test func aStopBeforeBootOffersAStartWithoutRoot() {
        var root = state()
        RootFallbackRule.stopped(&root)
        #expect(root.attention == .stoppedStart && root.on)
        // It never replaces a decision that waits.
        var paused = state()
        paused.attention = .failedStart
        RootFallbackRule.stopped(&paused)
        #expect(paused.attention == .failedStart)
    }

    @Test func aStartWithoutRootRemovesTheAppOnlyWhenTheRuleSaysSo() async {
        let present = RootScriptedShell { command in
            command == MagiskApp.probeCommand
                ? GuestShellOutput(text: "package:com.topjohnwu.magisk versionCode:31100\n", exitCode: 0)
                : GuestShellOutput(text: "Success\n", exitCode: 0)
        }
        let removedPresent = await RootOffRule.removeAppIfAny(shell: present, limit: .seconds(1))
        #expect(removedPresent)
        #expect(present.commands == [MagiskApp.probeCommand, MagiskApp.uninstallCommand])
        let absent = RootScriptedShell { _ in GuestShellOutput(text: "", exitCode: 0) }
        let removedAbsent = await RootOffRule.removeAppIfAny(shell: absent, limit: .seconds(1))
        #expect(removedAbsent)
        #expect(absent.commands == [MagiskApp.probeCommand])
        let unreadable = await RootOffRule.removeAppIfAny(shell: RootScriptedShell { _ in GuestShellOutput(text: "", exitCode: 1) }, limit: .seconds(1))
        #expect(!unreadable)
    }
}

struct RootFallbackRecordTests {
    private func record(
        end: String, status: Int = 0, boot: String? = "unfinished", running: Bool = false, ramdisk: String? = "root"
    ) throws -> SessionRecord {
        var text = "uml-session 8\nstate \(running ? "running" : "ended")\nend \(end)\nstatus \(status)\npid 1\n"
        if let boot { text += "boot \(boot)\nrestarts 0\n" }
        if let ramdisk { text += "ramdisk \(ramdisk)\n" }
        return try #require(SessionRecord(text: text))
    }

    @Test func aKernelThatEndedTheStartBeforeAndroidBootedIsAFailedStart() throws {
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic")))
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "kernel-abort")))
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "kernel-exit", status: 1)))
    }

    @Test func aRootedStartThatPanickedBeforeInitSaidAnythingIsAFailedStartToo() throws {
        // A ramdisk with no /init, or a magiskinit that dies before AOSP's
        // first stage: the runtime saw no output of init, so the boot is "unobserved".
        for end in ["guest-panic", "kernel-abort"] {
            let ended = try record(end: end, boot: "unobserved")
            #expect(ended.bootProgress == .unobserved && ended.bootFinished == nil)
            #expect(RootFallbackRule.endedBeforeBoot(ended), "\(end)")
        }
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "kernel-exit", status: 2, boot: "unobserved")))
    }

    @Test func onlyABootThatTheRecordSaysUsedTheMagiskRamdiskCounts() throws {
        // A restart in place into recovery, or a stock start: whatever the application asked for.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", ramdisk: "recovery")))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: "unobserved", ramdisk: "recovery")))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", ramdisk: "stock")))
        // A record that names no ramdisk (an earlier runtime, a boot never begun) is no rooted boot.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", ramdisk: nil)))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: "unobserved", ramdisk: nil)))
        // A record with no boot evidence at all, even one that names the ramdisk.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: nil)))
    }

    @Test func everyOtherEndTakesTheOrdinaryPaths() throws {
        // Android had booted, or the record does not say.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: "completed")))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: nil)))
        // A kernel's clean exit, a stop, a power-off.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "kernel-exit", status: 0)))
        for end in ["powered-off", "restarted", "stopped", "host-fault", "hibernation-restore-failed", "process-exit", "failed"] {
            let ended = try record(end: end, status: 1)
            #expect(!RootFallbackRule.endedBeforeBoot(ended), "\(end)")
            #expect(!RootFallbackRule.endedBeforeBoot(try record(end: end, status: 1, boot: "unobserved")), "\(end) unobserved")
        }
        // The system or the person ended the process: the record is still running.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "running", running: true)))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", running: true)))
    }

    @Test func aRestartLoopBeforeBootIsAFailedStartToo() {
        #expect(RootFallbackRule.endedBeforeBoot(RunEnd(result: -100, restartedInALoop: true), bootCompleted: false))
        #expect(!RootFallbackRule.endedBeforeBoot(RunEnd(result: -100, restartedInALoop: true), bootCompleted: true))
        #expect(!RootFallbackRule.endedBeforeBoot(RunEnd(result: -101, memoryRefusedMiB: 4096), bootCompleted: false))
        #expect(!RootFallbackRule.endedBeforeBoot(RunEnd(result: 0, stoppedByUser: true), bootCompleted: false))
    }
}

/// A guest shell that answers by command and remembers what it was asked.
final class RootScriptedShell: GuestShell, @unchecked Sendable {
    private let lock = NSLock()
    private var asked: [String] = []
    private let answer: @Sendable (String) async throws -> GuestShellOutput

    init(_ answer: @escaping @Sendable (String) async throws -> GuestShellOutput) { self.answer = answer }

    var commands: [String] { lock.withLock { asked } }

    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
        lock.withLock { asked.append(command) }
        return try await answer(command)
    }
}

actor RootCounter {
    private(set) var value = 0
    func bump() { value += 1 }
}

@Suite struct RootReportTests {
    private let release = RootRelease(versionName: "v31.1", versionCode: 31100, apkPath: "root/Magisk-v31.1.apk")

    @Test func aStockGuestSaysSoInFourLines() {
        let text = RootReport.text(state: RootState(), release: nil, lastBoot: .stock)
        #expect(text.split(separator: "\n").count == 4)
        #expect(text.contains("root switch: off"))
        #expect(text.contains("the release carries root: no"))
    }

    @Test func theLastBootsRamdiskIsWhatTheRecordSaysAndNotWhatWasAskedFor() {
        var state = RootState()
        state.on = true
        // A failed rooted start pauses root and asks for none: the report still says what booted.
        state.attention = .failedStart
        #expect(RootReport.text(state: state, release: release, lastBoot: .root).contains("booted the Magisk ramdisk: yes"))
        #expect(RootReport.text(state: state, release: release, lastBoot: .stock).contains("booted the Magisk ramdisk: no"))
        #expect(RootReport.text(state: state, release: release, lastBoot: .recovery).contains("booted the Magisk ramdisk: no"))
        #expect(RootReport.text(state: state, release: release, lastBoot: nil).contains("booted the Magisk ramdisk: unknown"))
    }

    @Test func aRootedGuestNamesTheMagiskVersionAndWhatTheApplicationRecorded() {
        var state = RootState()
        state.on = true
        state.appVersionCode = 31100
        let text = RootReport.text(state: state, release: release, lastBoot: .root)
        #expect(text.contains("root switch: on"))
        #expect(text.contains("booted the Magisk ramdisk: yes"))
        #expect(text.contains("Magisk v31.1 (31100)"))
        #expect(text.contains("version code the application recorded: 31100"))
    }

    @Test func theLastInstallAndItsReasonAreInTheReport() {
        var state = RootState()
        state.on = true
        for (reason, words) in [
            (RootState.AppInstall.Reason.absent, "not in the virtual machine"),
            (.older, "older than the release's"),
            (.notRecorded, "not recorded as put in by the application for this release"),
        ] {
            state.appInstall = .init(reason: reason, installed: reason != .older)
            let text = RootReport.text(state: state, release: release)
            #expect(text.contains("last Magisk app install: \(reason == .older ? "did not finish" : "done"), the app "))
            #expect(text.contains(words), "\(reason)")
        }
        // The note is kept with the guest, and the period's end forgets it.
        let decoded = try? JSONDecoder().decode(RootState.self, from: JSONEncoder().encode(state))
        #expect(decoded?.appInstall == state.appInstall)
        state.setOn(false)
        #expect(state.appInstall == nil)
    }

    @Test func theOpenAfterTheLastInstallIsInTheReportWhenOneWasTried() {
        var state = RootState()
        state.on = true
        state.appInstall = .init(reason: .absent, installed: true)
        #expect(!RootReport.text(state: state, release: release).contains("on the screen"), "no open was tried: no line")
        state.appInstall = .init(reason: .absent, installed: true, opened: true)
        #expect(RootReport.text(state: state, release: release).contains("Magisk app on the screen after that install: yes\n"))
        state.appInstall = .init(reason: .absent, installed: true, opened: false)
        #expect(RootReport.text(state: state, release: release).contains("Magisk app on the screen after that install: no\n"))
        // Kept with the guest, and a state an older application saved reads back with no open recorded.
        let decoded = try? JSONDecoder().decode(RootState.self, from: JSONEncoder().encode(state))
        #expect(decoded?.appInstall == .init(reason: .absent, installed: true, opened: false))
        let older = #"{"on":true,"appInstall":{"reason":"absent","installed":true}}"#
        let read = try? JSONDecoder().decode(RootState.self, from: Data(older.utf8))
        #expect(read?.appInstall == .init(reason: .absent, installed: true, opened: nil))
    }

    @Test func aPausedGuestSaysWhy() {
        var state = RootState()
        state.on = true
        state.attention = .failedStart
        #expect(RootReport.text(state: state, release: release).contains("paused: a start with root ended before Android booted"))
    }

    @Test func theEntryIsNamedForTheReportAndCarriesNoPathOrIdentifier() {
        let entry = RootReport.entry(state: RootState(), release: nil)
        #expect(entry.name == "andarium/root.txt")
        let text = String(decoding: entry.data, as: UTF8.self)
        #expect(!text.contains("/"))
    }
}
