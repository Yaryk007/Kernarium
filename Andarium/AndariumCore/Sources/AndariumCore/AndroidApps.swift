// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// One launcher activity of one Android app: the thing a Home Screen icon
/// opens. It is the only form in which a name from Android reaches a command,
/// so the grammar below is the rule that keeps a package or class name from
/// being anything but a name: a value that does not follow it is never built.
public struct AndroidComponent: Hashable, Sendable, Codable {
    public let packageName: String
    public let activityClass: String

    /// `nil` for a pair that is not a package name and a class name.
    public init?(packageName: String, activityClass: String) {
        guard Self.isPackageName(packageName), Self.isActivityClass(activityClass) else { return nil }
        self.packageName = packageName
        self.activityClass = activityClass
    }

    private enum CodingKeys: String, CodingKey {
        case packageName, activityClass
    }

    /// A component read from a file follows the same grammar as one built
    /// from the tool's answer: a file that says otherwise is not read.
    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let package = try container.decode(String.self, forKey: .packageName)
        let activity = try container.decode(String.self, forKey: .activityClass)
        guard let component = Self(packageName: package, activityClass: activity) else {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(codingPath: decoder.codingPath, debugDescription: "not a component of an app"))
        }
        self = component
    }

    /// The component an identifier names (`package/class`), or nil.
    public init?(identifier: String) {
        let parts = identifier.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2, identifier.utf8.count <= Self.longestIdentifier else { return nil }
        self.init(packageName: String(parts[0]), activityClass: String(parts[1]))
    }

    /// `package/class`: the identity of an app's icon, which is the same on
    /// every launch, every device and after a reinstall, and needs no table
    /// to be read back.
    public var identifier: String { "\(packageName)/\(activityClass)" }

    /// The longest package name and class name the tool lists, and the
    /// longest `package/class` token.
    public static let longestPackageName = 255
    public static let longestActivityClass = 255
    public static let longestIdentifier = 511

    /// At least two dot-separated segments, each of letters, digits and
    /// underscores and starting with a letter.
    public static func isPackageName(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        guard !bytes.isEmpty, bytes.count <= longestPackageName else { return false }
        let segments = bytes.split(separator: UInt8(ascii: "."), omittingEmptySubsequences: false)
        guard segments.count >= 2 else { return false }
        return segments.allSatisfy { segment in
            guard let first = segment.first, isLetter(first) else { return false }
            return segment.allSatisfy { isLetter($0) || isDigit($0) || $0 == UInt8(ascii: "_") }
        }
    }

    /// The class name in full, as Android reports it (never the `.Name`
    /// shorthand): dot-separated Java identifiers, a `$` being part of an
    /// inner class's name. A quote, a space, a slash, a semicolon, a leading or
    /// trailing dot, an empty segment or a leading `-` is none.
    public static func isActivityClass(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        guard !bytes.isEmpty, bytes.count <= longestActivityClass else { return false }
        let segments = bytes.split(separator: UInt8(ascii: "."), omittingEmptySubsequences: false)
        return segments.allSatisfy { segment in
            guard let first = segment.first, isLetter(first) || first == UInt8(ascii: "_") || first == UInt8(ascii: "$")
            else { return false }
            return segment.allSatisfy {
                isLetter($0) || isDigit($0) || $0 == UInt8(ascii: "_") || $0 == UInt8(ascii: "$")
            }
        }
    }

    private static func isLetter(_ byte: UInt8) -> Bool { (byte >= 65 && byte <= 90) || (byte >= 97 && byte <= 122) }
    private static func isDigit(_ byte: UInt8) -> Bool { byte >= 48 && byte <= 57 }
}

/// An app Android lists with a launcher icon: what the chooser, the entity
/// and the cache know of it.
public struct AndroidApp: Hashable, Sendable, Codable, Identifiable {
    public var component: AndroidComponent
    /// Its name in the language Android is in.
    public var label: String
    /// Part of Android's own image (Settings, Camera, Files), not installed
    /// by the person.
    public var builtIn: Bool
    /// Changes when the app's icon does; the cached picture is kept while it
    /// stays the same.
    public var iconTag: String

    public var id: String { component.identifier }

    public init(component: AndroidComponent, label: String, builtIn: Bool, iconTag: String) {
        self.component = component
        self.label = label
        self.builtIn = builtIn
        self.iconTag = iconTag
    }
}

/// The apps of one answer of Android's listing, with the tag that says whether
/// anything changed since the last one.
public struct AndroidAppList: Equatable, Sendable, Codable {
    public var apps: [AndroidApp]
    /// Changes when the set of apps, a name or an icon does; nil where the
    /// image gives none.
    public var digest: String?

    public init(apps: [AndroidApp], digest: String?) {
        self.apps = apps
        self.digest = digest
    }
}

/// What Android's listing answered.
public enum AndroidAppListing: Equatable, Sendable {
    case apps(AndroidAppList)
    /// The image has no listing tool (an older one), or speaks a version of
    /// it this Andarium does not know: the feature offers no apps.
    case unavailable
    /// The virtual machine could not be asked, or what it printed was not a
    /// listing.
    case failed
}

/// What Android answered to the cheap question whether the apps changed.
public enum AndroidAppDigestAnswer: Equatable, Sendable {
    case digest(String)
    case unavailable
    case failed
}

/// What Android answered about one app's icon.
public enum AndroidAppIconAnswer: Equatable, Sendable {
    /// The icon as PNG data, square, never masked; `hasAlpha` says that some
    /// pixel is not opaque, which iOS would draw black.
    case png(Data, hasAlpha: Bool)
    /// The app is not a launcher activity now (removed, disabled, renamed).
    case missing
    /// The app declares no icon: the screens draw a placeholder.
    case noIcon
    /// The icon could not be loaded or rendered.
    case error
    /// The tool ran out of time before it: ask again.
    case deferred
}

/// What Android answered to the request to open an app.
public enum AndroidAppOpenAnswer: Equatable, Sendable {
    /// The app's launcher activity was started.
    case opened
    /// The tool refused: the activity is not in Android (the app was removed,
    /// disabled or replaced).
    case notFound
    /// The tool could not start it and does not say why (the activity manager
    /// reported an error, the activity class is gone): the listing tells.
    case startFailed
    /// The image has no such tool, or one that does not take the command: an
    /// image older than the feature. Asking again cannot help.
    case unavailable
    /// The virtual machine could not be asked, or did not answer in time.
    case failed
}

/// How a request to open an app ended, after the tool's refusal was followed
/// up (an app that changed its launcher activity).
public enum AndroidAppOpenOutcome: Equatable, Sendable {
    case opened
    /// Opened through the app's one other launcher activity, which replaces
    /// the one an icon was made from.
    case openedRenamed(AndroidComponent)
    case notInAndroid
    case failed
    /// The image cannot open apps this way (an older one).
    case toolMissing
}

/// Where the guest's list of apps and the way to open one come from: the
/// runtime asks the tool in Android's image (`AndroidAppsCommands`), and the
/// Simulator preview plays a list. The screens see only `AndroidApp`, its icon
/// and the answers above.
public protocol AndroidAppsTool: Sendable {
    /// Whether the apps changed since the answer with this digest.
    func digest(limit: Duration) async -> AndroidAppDigestAnswer
    /// The launchable apps, within `limit`.
    func list(limit: Duration) async -> AndroidAppListing
    /// The icons of up to `AndroidAppLimits.iconsPerAsk` apps, within `limit`,
    /// by the app's identifier. An app that is not in the answer was not
    /// answered for.
    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer]
    /// Starts one app's launcher activity.
    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer
}

extension AndroidAppsTool {
    /// Opens an app. An answer that the activity is not there, or a start that
    /// failed for a reason the tool does not give, is followed up once by
    /// listing the apps again, to learn why:
    ///
    /// - the app's package has one launcher activity now and had exactly one
    ///   in the last listing that held the saved activity (`launchersWhenSeen`),
    ///   so the app renamed its launcher: that activity is opened in place of
    ///   the old one;
    /// - any other case where the saved activity is not listed is an app that
    ///   is not in Android (removed, disabled, or one of several launchers
    ///   that went), and nothing else is opened in its place;
    /// - a start that failed while the activity is still listed is a failure.
    ///
    /// The apps are listed over seconds, and the replacement activity's launch
    /// is a command that has not been sent: `mayReplace` is asked right before
    /// it goes out, and where it answers false nothing is sent and the outcome
    /// is `.failed`, which the asker, having withheld the launch, does not take
    /// as an answer. A command already sent is never withheld.
    public func open(
        _ component: AndroidComponent, launchersWhenSeen: Int? = nil,
        listLimit: Duration = AndroidAppLimits.listLimit, openLimit: Duration = AndroidAppLimits.openLimit,
        mayReplace: (@Sendable () async -> Bool)? = nil
    ) async -> AndroidAppOpenOutcome {
        let answer = await open(component, limit: openLimit)
        switch answer {
        case .opened:
            return .opened
        case .failed:
            return .failed
        case .unavailable:
            return .toolMissing
        case .notFound, .startFailed:
            switch await self.list(limit: listLimit) {
            case .unavailable, .failed:
                return .failed
            case let .apps(list):
                if answer == .startFailed, list.apps.contains(where: { $0.component == component }) { return .failed }
                let sameApp = list.apps.filter { $0.component.packageName == component.packageName }
                guard launchersWhenSeen == 1, sameApp.count == 1, let only = sameApp.first, only.component != component
                else { return .notInAndroid }
                if let mayReplace, !(await mayReplace()) { return .failed }
                switch await open(only.component, limit: openLimit) {
                case .opened: return .openedRenamed(only.component)
                case .notFound: return .notInAndroid
                case .unavailable: return .toolMissing
                case .startFailed, .failed: return .failed
                }
            }
        }
    }
}

/// The bounds of what the tool is asked and what Andarium keeps of the answer.
public enum AndroidAppLimits {
    /// The tool's own deadlines sit inside these.
    public static let digestLimit = Duration.seconds(15)
    public static let listLimit = Duration.seconds(30)
    /// One `icons` ask is at most about 28 s of the tool's work.
    public static let iconLimit = Duration.seconds(40)
    public static let openLimit = Duration.seconds(30)
    /// The most apps one listing may hold, and the most icons one ask names.
    public static let mostApps = 1_024
    public static let iconsPerAsk = 8
    /// How often an icon the tool answered `deferred` is asked again in one
    /// refresh, and how many asks in a row may get no answer before the
    /// refresh stops asking for icons (the next one asks again).
    public static let deferredRetries = 2
    public static let silentAsksBeforeGivingUp = 3
    /// The longest name the screens are given, as the tool bounds one: 80
    /// Unicode scalars and 320 UTF-8 bytes (a longer one ends in an ellipsis).
    public static let longestLabelScalars = 80
    public static let longestLabelBytes = 320
    /// How much of a label's raw text is looked at, before any other work.
    public static let longestRawLabel = 1_024
    /// The largest icon picture kept, in bytes (the tool's own is 96 KiB).
    public static let largestIcon = 128 * 1_024
    /// The most records of icons kept across an erase: one for each app a
    /// listing may hold.
    public static let mostRecords = mostApps
}

/// What an app's own text is made into before a screen, a title or an icon's
/// name shows it, by the rule the tool applies to a label (`kernarium_apps`,
/// section 5), here applied again because the answer is untrusted: control
/// characters, line and paragraph separators and every space character are
/// spaces, a run of spaces is one and the ends are trimmed; every invisible
/// format character is dropped (the bidirectional marks and controls, zero
/// width characters, the byte order mark, tags) but the two joiners that emoji
/// and some scripts need; noncharacters are U+FFFD; and a long label is cut at
/// a Unicode scalar, to 80 scalars and 320 bytes, with an ellipsis. The cut is
/// made before any grapheme work, so a run of combining marks is bounded too.
public enum AndroidAppText {
    public static func label(_ text: String, fallback: String) -> String {
        let cleaned = clean(text)
        return cleaned.isEmpty ? clean(fallback) : cleaned
    }

    private static let ellipsis: Unicode.Scalar = "\u{2026}"

    private static func clean(_ text: String) -> String {
        var kept: [Unicode.Scalar] = []
        var bytes = 0
        var spacePending = false
        var overlong = false
        var looked = 0
        for scalar in text.unicodeScalars {
            looked += 1
            if looked > AndroidAppLimits.longestRawLabel {
                overlong = true
                break
            }
            guard let mapped = mapped(scalar) else { continue }
            if mapped == " " {
                spacePending = !kept.isEmpty
                continue
            }
            if spacePending {
                kept.append(" ")
                bytes += 1
                spacePending = false
            }
            kept.append(mapped)
            bytes += String(mapped).utf8.count
        }
        guard !kept.isEmpty else { return "" }
        let cut = overlong || kept.count > AndroidAppLimits.longestLabelScalars || bytes > AndroidAppLimits.longestLabelBytes
        guard cut else { return String(String.UnicodeScalarView(kept)) }
        // Room for the ellipsis: it is one scalar and three bytes.
        var head: [Unicode.Scalar] = []
        var used = 0
        for scalar in kept {
            let size = String(scalar).utf8.count
            guard head.count < AndroidAppLimits.longestLabelScalars - 1,
                  used + size <= AndroidAppLimits.longestLabelBytes - 3
            else { break }
            head.append(scalar)
            used += size
        }
        while head.last == " " { head.removeLast() }
        head.append(ellipsis)
        return String(String.UnicodeScalarView(head))
    }

    /// The scalar as it is kept, a space for what is one, or nil for what goes.
    private static func mapped(_ scalar: Unicode.Scalar) -> Unicode.Scalar? {
        switch scalar.properties.generalCategory {
        case .control, .lineSeparator, .paragraphSeparator, .spaceSeparator:
            return " "
        case .format:
            return scalar.value == 0x200C || scalar.value == 0x200D ? scalar : nil
        default:
            break
        }
        if isBlankLookalike(scalar) { return " " }
        if scalar.properties.isNoncharacterCodePoint { return "\u{FFFD}" }
        return scalar
    }

    /// The characters that draw nothing but are no format character, so a
    /// label made of them looks empty: the Hangul fillers (U+115F, U+1160,
    /// U+3164, U+FFA0) and the blank braille pattern (U+2800). They count as
    /// space, so a label of nothing else is the package name.
    private static func isBlankLookalike(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x115F, 0x1160, 0x3164, 0xFFA0, 0x2800: true
        default: false
        }
    }

    /// Alphabetical by label as the person reads it, the identifier settling a tie.
    public static func inOrder(_ lhs: AndroidApp, _ rhs: AndroidApp) -> Bool {
        switch lhs.label.localizedStandardCompare(rhs.label) {
        case .orderedAscending: true
        case .orderedDescending: false
        case .orderedSame: lhs.id < rhs.id
        }
    }
}
