// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// An installed app that Old app compatibility can apply to: built for an
/// Android before 4.0, 32-bit, not a system app, with no shared user. The
/// image decides which apps those are; Andarium shows what its listing names.
public struct OldApp: Equatable, Hashable, Sendable, Identifiable {
    public var package: String
    /// The app's name in the language Android is in; the package name when
    /// the listing gave none.
    public var label: String
    public var isOn: Bool
    /// When the switch was last turned on, in epoch seconds; 0 when it never was.
    public var turnedOn: Int
    /// When the app was last installed or updated, in epoch seconds.
    public var lastUpdate: Int

    public var id: String { package }

    public init(package: String, label: String, isOn: Bool, turnedOn: Int = 0, lastUpdate: Int = 0) {
        self.package = package
        self.label = label
        self.isOn = isOn
        self.turnedOn = turnedOn
        self.lastUpdate = lastUpdate
    }
}

/// What the image's listing answered.
public enum OldAppListing: Equatable, Sendable {
    /// The apps the switch can apply to, by name. None is an answer too.
    case apps([OldApp])
    /// The image has no such command (an older one), or speaks a version of
    /// it that this Andarium does not know (a version it numbers, or a `list`
    /// it answers as a usage error): the feature is not offered.
    case unavailable
    /// The virtual machine could not be asked, or what it printed was not a
    /// listing.
    case failed
}

/// How the image answered a change of one app's switch.
public enum OldAppWrite: Equatable, Sendable {
    /// Exit 0 and the line that names the app and the new state.
    case applied
    /// The image refused the app: it is not one the switch can apply to
    /// (exit 2), say because it was updated meanwhile.
    case notEligible
    /// Anything else: no answer, another status, an answer that is not the
    /// line.
    case failed
}

/// The image's `kernarium_oldapp` command, which lists the installed apps that
/// Old app compatibility can apply to and switches it for one of them, and the
/// one other question Andarium asks about an app (whether it is open). It is
/// the only code that knows the command's words and the format of its output;
/// the screens see `OldApp`, `OldAppListing` and `OldAppWrite`.
///
/// The command's contract: `list` prints `kernarium_oldapp 1` and then one
/// line per eligible app, `package<TAB>label<TAB>on|off<TAB>turned-on<TAB>last-update`,
/// and `set <package> on|off` prints `<package><TAB>on|off` and exits 0, or
/// exits 2 for an app that is not eligible and 64 for arguments it does not
/// know. The format only grows by trailing fields, and its version number
/// changes when a field's meaning does.
public struct OldAppCompatibility: Sendable {
    public static let commandName = "kernarium_oldapp"
    /// The one version of the listing this reader knows.
    public static let supportedVersion = 1

    /// The shell's status for a command it cannot find.
    private static let commandNotFound = 127
    /// The image's status for arguments it does not know: a `list` it would
    /// not take is a command that does not speak this version.
    private static let usageStatus = 64
    /// The image's status for an app the switch does not apply to.
    private static let notEligibleStatus = 2

    /// A listing is a few lines per app: it is bounded in the time it may take.
    public static let listLimit = Duration.seconds(20)
    /// The listing taken around an install, which the install's result waits
    /// for: a longer one is not worth the wait for a hint.
    public static let installListLimit = Duration.seconds(5)
    static let setLimit = Duration.seconds(20)
    static let runningLimit = Duration.seconds(10)

    /// The longest name the screens are given: a label is the app's own text.
    static let longestLabel = 200

    private let shell: any GuestShell

    public init(shell: any GuestShell) {
        self.shell = shell
    }

    // MARK: Questions

    /// Lists the eligible apps, within `limit`.
    public func list(limit: Duration = OldAppCompatibility.listLimit) async -> OldAppListing {
        guard let answer = try? await shell.run(Self.listCommand, limit: limit) else { return .failed }
        return Self.parseListing(answer)
    }

    /// Switches Old app compatibility on or off for `package`. The image
    /// force-stops the app. A name that is not a package name is never sent.
    public func set(package: String, on: Bool) async -> OldAppWrite {
        guard let command = Self.setCommand(package: package, on: on),
              let answer = try? await shell.run(command, limit: Self.setLimit)
        else { return .failed }
        return Self.parseWrite(answer, package: package, on: on)
    }

    /// Whether the app has a process now; nil when that could not be told.
    public func isOpen(package: String) async -> Bool? {
        guard let command = Self.openCommand(package: package),
              let answer = try? await shell.run(command, limit: Self.runningLimit)
        else { return nil }
        return Self.parseOpen(answer)
    }

    // MARK: Commands

    public static let listCommand = "\(commandName) list"

    /// The command that switches the layer for `package`; nil for a name that
    /// is not a package name, so that no word of it comes from the listing.
    public static func setCommand(package: String, on: Bool) -> String? {
        guard ApkContainer.isPackageName(package) else { return nil }
        return "\(commandName) set \(quotedForShell(package)) \(on ? "on" : "off")"
    }

    /// Android's `pidof` for the app's main process, which is named by its
    /// package. The image's command does not say whether an app is open, and
    /// the screens ask only to word an alert, so a process of another name
    /// goes unseen and the alert is the plain one.
    public static func openCommand(package: String) -> String? {
        guard ApkContainer.isPackageName(package) else { return nil }
        return "pidof \(quotedForShell(package))"
    }

    // MARK: Answers

    public static func parseListing(_ answer: GuestShellOutput) -> OldAppListing {
        if answer.exitCode == commandNotFound || answer.exitCode == usageStatus { return .unavailable }
        guard answer.exitCode == 0 else { return .failed }
        let lines = answer.text.split(omittingEmptySubsequences: true, whereSeparator: \.isNewline).map(String.init)
        guard let head = lines.first else { return .failed }
        let words = head.split(whereSeparator: { $0 == " " || $0 == "\t" })
        guard words.count >= 2, words[0] == commandName, let version = Int(words[1]) else { return .failed }
        guard version == supportedVersion else { return .unavailable }
        var seen = Set<String>()
        var apps: [OldApp] = []
        for line in lines.dropFirst() {
            guard let app = parseApp(line), seen.insert(app.package).inserted else { continue }
            apps.append(app)
        }
        return .apps(apps.sorted(by: appsInOrder))
    }

    /// One app's line: at least the five fields; a trailing field a later
    /// version adds is ignored. A line that is short, names no package or has
    /// a state other than on or off is not an app.
    static func parseApp(_ line: String) -> OldApp? {
        let fields = line.split(separator: "\t", omittingEmptySubsequences: false)
        guard fields.count >= 5 else { return nil }
        let package = String(fields[0])
        guard ApkContainer.isPackageName(package) else { return nil }
        let isOn: Bool
        switch fields[2] {
        case "on": isOn = true
        case "off": isOn = false
        default: return nil
        }
        return OldApp(
            package: package, label: label(String(fields[1]), package: package), isOn: isOn,
            turnedOn: Int(fields[3]) ?? 0, lastUpdate: Int(fields[4]) ?? 0)
    }

    /// The app's own text as a name: control characters are spaces, the
    /// bidirectional controls (which would reorder the title the name is
    /// quoted in) are dropped, the ends are trimmed and an enormous one is
    /// cut; the package name stands for a label that is empty.
    static func label(_ text: String, package: String) -> String {
        let plain = String(String.UnicodeScalarView(text.unicodeScalars.compactMap {
            if isBidirectionalControl($0) { return nil }
            return $0.properties.generalCategory == .control ? " " : $0
        }))
        let trimmed = plain.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return package }
        return String(trimmed.prefix(longestLabel))
    }

    /// The embedding, override and isolate controls (U+202A to U+202E and
    /// U+2066 to U+2069).
    private static func isBidirectionalControl(_ scalar: Unicode.Scalar) -> Bool {
        (0x202A...0x202E).contains(scalar.value) || (0x2066...0x2069).contains(scalar.value)
    }

    /// Alphabetical by label as the person reads it, the package name settling a tie.
    static func appsInOrder(_ lhs: OldApp, _ rhs: OldApp) -> Bool {
        switch lhs.label.localizedStandardCompare(rhs.label) {
        case .orderedAscending: true
        case .orderedDescending: false
        case .orderedSame: lhs.package < rhs.package
        }
    }

    public static func parseWrite(_ answer: GuestShellOutput, package: String, on: Bool) -> OldAppWrite {
        if answer.exitCode == notEligibleStatus { return .notEligible }
        guard answer.exitCode == 0 else { return .failed }
        let expected = "\(package)\t\(on ? "on" : "off")"
        let lines = answer.text.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
        return lines == [expected] ? .applied : .failed
    }

    /// `pidof` prints the process ids and exits 0, or exits 1 for none.
    public static func parseOpen(_ answer: GuestShellOutput) -> Bool? {
        switch answer.exitCode {
        case 0:
            let words = answer.text.split(whereSeparator: { $0 == " " || $0.isNewline })
            return !words.isEmpty && words.allSatisfy { $0.utf8.allSatisfy { $0 >= 48 && $0 <= 57 } }
        case 1:
            return false
        default:
            return nil
        }
    }
}

/// The hint after Install anyway: the app the person just installed is built
/// for an Android before 4.0, so the page that can help it is named.
public enum OldAppInstallHint {
    /// The first API level that is not "before 4.0" (Ice Cream Sandwich).
    public static let firstLevelOfAndroid4 = 14

    /// Whether the refusal that Install anyway answered named an app of that age.
    public static func isOldTarget(_ target: LegacyTarget) -> Bool {
        guard let level = target.appLevel else { return false }
        return level < firstLevelOfAndroid4
    }

    /// The app the listing after an install names that the listing before did
    /// not have, or has updated since: the one with the newest update time
    /// among those the install changed. Nil when no app was changed, which
    /// is when the installed app is one the switch cannot apply to.
    public static func installedApp(listedBefore before: [OldApp], listedAfter after: [OldApp]) -> OldApp? {
        let newest = before.map(\.lastUpdate).max() ?? 0
        let changed = after.filter { $0.lastUpdate > newest }
        return changed.max { $0.lastUpdate < $1.lastUpdate }
    }
}
