// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// What the Android release Andarium carries holds for root: a copy of its own
/// ramdisk with Magisk patched in and the official Magisk app, in the release's
/// `root/` directory. A release made before root has none, and the switch is
/// then not offered. The runtime takes the ramdisk from the release itself and
/// from no other file (`darwin/device/block/README.md`, "Boot selection"); the
/// application reads here what it needs to install the app.
public struct RootRelease: Equatable, Sendable {
    /// Where the runtime finds the patched ramdisk, relative to the release.
    public static let bootImage = "root/init_boot-magisk.img"

    /// "v31.1".
    public var versionName: String
    /// Magisk's own version code, 31100 for v31.1: the daemon accepts a
    /// manager app whose code is at least its own.
    public var versionCode: Int
    /// The unmodified official app, relative to the release.
    public var apkPath: String

    public init(versionName: String, versionCode: Int, apkPath: String) {
        self.versionName = versionName
        self.versionCode = versionCode
        self.apkPath = apkPath
    }

    /// The `root` object of a release manifest (schema 8), or nil when the
    /// release carries none or it is not one this reader accepts: a version
    /// that is no positive integer, a name that is no single short line, or a path that leaves the release's
    /// `root/` directory.
    public init?(manifest: Data) {
        guard let object = try? JSONSerialization.jsonObject(with: manifest) as? [String: Any],
            let root = object["root"] as? [String: Any],
            let magisk = root["magisk"] as? [String: Any],
            let name = magisk["version_name"] as? String, Self.isVersionName(name),
            let number = magisk["version_code"] as? NSNumber, Self.isVersionCode(number),
            let apk = root["apk"] as? [String: Any], let apkPath = apk["path"] as? String,
            Self.isRootPath(apkPath),
            let image = root["boot_image"] as? [String: Any], let imagePath = image["path"] as? String,
            imagePath == Self.bootImage
        else { return nil }
        self.init(versionName: name, versionCode: number.intValue, apkPath: apkPath)
    }

    /// One line of at most 32 bytes: the name goes into a bug report's line.
    static func isVersionName(_ name: String) -> Bool {
        guard !name.isEmpty, name.utf8.count <= 32 else { return false }
        return !name.unicodeScalars.contains {
            CharacterSet.controlCharacters.contains($0) || CharacterSet.newlines.contains($0)
        }
    }

    /// An integer from 1 to what an Android version code holds (2^31 - 1),
    /// written as one: 3.5, 31100.0 and a boolean are not.
    static func isVersionCode(_ number: NSNumber) -> Bool {
        guard CFGetTypeID(number) != CFBooleanGetTypeID(), !CFNumberIsFloatType(number) else { return false }
        guard let code = Int(exactly: number) else { return false }
        return code >= 1 && code <= Int(Int32.max)
    }

    /// A release-relative path inside `root/`, with no dot segments.
    static func isRootPath(_ path: String) -> Bool {
        guard path.hasPrefix("root/"), path.count > "root/".count, path.count <= 128 else { return false }
        return !path.split(separator: "/", omittingEmptySubsequences: false)
            .contains { $0.isEmpty || $0 == "." || $0 == ".." }
    }

    /// The release in `folder`, when its manifest has a `root` object whose
    /// two files are both there as regular files, each reached without a link
    /// in its path under `folder`: a partial `root/` is no root.
    public static func carried(in folder: URL, manifest: Data) -> RootRelease? {
        guard let release = RootRelease(manifest: manifest) else { return nil }
        func regular(_ path: String) -> Bool {
            guard let descriptor = try? FileChunkReader.openRegularFile(path, under: folder) else { return false }
            Darwin.close(descriptor)
            return true
        }
        guard regular(bootImage), regular(release.apkPath) else { return nil }
        return release
    }

    /// Opens the Magisk app file of the release in `folder` for the upload:
    /// the file `carried` saw, reached without a link, checked as a regular
    /// file on the descriptor it was opened on and read from that descriptor.
    public func openApk(in folder: URL, capacity: Int) throws -> FileChunkReader {
        try FileChunkReader(regularFileAt: apkPath, under: folder, capacity: capacity)
    }

    /// The Magisk app file inside the release `folder`.
    public func apkURL(in folder: URL) -> URL { folder.appendingPathComponent(apkPath) }
}

/// The guest's root: the person's switch, and what the application keeps so
/// that a start that cannot boot with Magisk does not trap the person
/// (`RootFallbackRule`) and the Magisk app is put in once and not forced back
/// (`RootInstallRule`). It belongs to the guest: erasing the guest returns it
/// to off.
public struct RootState: Equatable, Codable, Sendable {
    /// What Home says of a root the application changed or paused itself.
    public enum Attention: String, Codable, Sendable {
        /// A start with root ended before Android booted: starts are without
        /// root until the person decides.
        case failedStart
        /// The same, and the switch was turned off with it.
        case turnedOffAfterFailure
        /// The person stopped a start with root before Android booted.
        case stoppedStart
    }

    /// The person's switch. Off for a new guest and for one stored before the
    /// switch existed.
    public var on = false
    /// The start that is being handed to the runtime asks for the Magisk
    /// ramdisk. Never saved: whether a boot used it is the runtime's own
    /// record of that boot (`SessionRecord.ramdisk`), which a restart in
    /// place or a process that ended before the runtime began cannot make
    /// stale.
    public var startRooted = false
    /// What Home owes the person, if anything.
    public var attention: Attention?
    /// The version code the application itself put the Magisk app in at, in
    /// this period of root being on; nil before. An app it only saw is not
    /// recorded, since the application cannot tell whose it is. Turning root
    /// off ends the period.
    public var appVersionCode: Int?
    /// The application has put the Magisk app in this guest at some time.
    /// Turning root off does not clear it.
    public var appEverHandled = false

    /// The last install of the Magisk app the application ran, and why, for
    /// the bug report.
    public struct AppInstall: Equatable, Codable, Sendable {
        public enum Reason: String, Codable, Sendable {
            /// The guest said there was no app, or could not be asked.
            case absent
            /// The app there was older than the release's.
            case older
            /// An app of that package name was there and the state did not
            /// say the application put it in for this release.
            case notRecorded
        }

        public var reason: Reason
        /// The install ended well; false for one that failed or ran out of time.
        public var installed: Bool
        /// Whether the app was on Android's screen after that install
        /// (`MagiskAppOpening`); nil when no open was tried, as for an install
        /// that failed or one an older application recorded.
        public var opened: Bool?

        public init(reason: Reason, installed: Bool, opened: Bool? = nil) {
            self.reason = reason
            self.installed = installed
            self.opened = opened
        }
    }

    public var appInstall: AppInstall?

    public init() {}

    /// Starts are without root while a failed start waits for the person.
    public var paused: Bool { attention == .failedStart }

    /// The switch changed by the person. Off ends the period (and what waited
    /// for a decision); on, with nothing pending, begins a new one.
    public mutating func setOn(_ on: Bool) {
        self.on = on
        attention = nil
        if !on {
            appVersionCode = nil
            appInstall = nil
        }
    }

    /// The data disk was replaced by another (a snapshot restored), which may
    /// not have the Magisk app the application put in: what it remembers of
    /// the app is forgotten, as in a new guest, and the next start with root
    /// asks the guest again. The switch stays.
    public mutating func dataDiskReplaced() {
        appVersionCode = nil
        appEverHandled = false
        appInstall = nil
    }

    /// What a Magisk app step that began earlier learned of the app, added to
    /// the state as it is now. Only the app's own bookkeeping changes: the
    /// switch, the pause and what Home owes are the person's and this
    /// start's to change, and a step that ran while root was turned off
    /// meanwhile starts no period and records nothing at all.
    public mutating func adoptApp(from learned: RootState) {
        guard on else { return }
        appEverHandled = appEverHandled || learned.appEverHandled
        if let code = learned.appVersionCode { appVersionCode = max(appVersionCode ?? 0, code) }
        if let install = learned.appInstall { appInstall = install }
    }

    /// "Try with root again": the pause ends and the start that follows boots
    /// Magisk (`AppModel.tryRootAgain` makes it at once).
    public mutating func tryAgain() {
        on = true
        attention = nil
    }

    private enum CodingKeys: String, CodingKey {
        case on, attention, appVersionCode, appEverHandled, appInstall
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        on = try values.decodeIfPresent(Bool.self, forKey: .on) ?? false
        attention = try? values.decodeIfPresent(Attention.self, forKey: .attention)
        appVersionCode = try values.decodeIfPresent(Int.self, forKey: .appVersionCode)
        appEverHandled = try values.decodeIfPresent(Bool.self, forKey: .appEverHandled) ?? false
        appInstall = try? values.decodeIfPresent(AppInstall.self, forKey: .appInstall)
    }
}

extension Guest {
    /// Whether a start in `mode` boots the Magisk ramdisk: a normal start, with
    /// the switch on, in a release that carries root, and no failed start
    /// waiting for the person. `withoutRootOnce` is the person's "Start without
    /// root" for this start alone.
    public func bootsRooted(mode: StartMode, releaseCarriesRoot: Bool, withoutRootOnce: Bool = false) -> Bool {
        mode == .normal && root.on && releaseCarriesRoot && !root.paused && !withoutRootOnce
    }
}

/// The Magisk app and how the guest is asked about it.
public enum MagiskApp {
    public static let package = "com.topjohnwu.magisk"

    /// One line per package whose name contains the filter:
    /// `package:com.topjohnwu.magisk versionCode:31100`.
    static let probeCommand = "pm list packages --show-versioncode \(package)"
    /// What the guest says of the app.
    public enum Presence: Equatable, Sendable {
        case absent
        case installed(versionCode: Int)
        /// The guest could not be asked, or its answer was not the line.
        case unknown
    }

    /// The presence an answer states. Only the exact package counts, and a
    /// line that is not `package:<name> versionCode:<digits>` is no answer.
    static func presence(of output: GuestShellOutput) -> Presence {
        guard output.exitCode == 0 else { return .unknown }
        var found: Presence = .absent
        for line in output.text.split(whereSeparator: \.isNewline) {
            let words = line.split(separator: " ")
            guard words.count >= 2, words[0].hasPrefix("package:") else { return .unknown }
            guard words[0].dropFirst("package:".count) == package else { continue }
            guard words[1].hasPrefix("versionCode:"),
                let code = Int(words[1].dropFirst("versionCode:".count)), code >= 0
            else { return .unknown }
            found = .installed(versionCode: code)
        }
        return found
    }

    public static func presence(shell: any GuestShell, limit: Duration) async -> Presence {
        guard let output = try? await shell.run(probeCommand, limit: limit) else { return .unknown }
        return presence(of: output)
    }

    static let uninstallCommand = "pm uninstall --user 0 \(package)"

    /// Removes the app; whether the guest says it did.
    public static func uninstall(shell: any GuestShell, limit: Duration) async -> Bool {
        guard let output = try? await shell.run(uninstallCommand, limit: limit) else { return false }
        return output.exitCode == 0
    }
}
