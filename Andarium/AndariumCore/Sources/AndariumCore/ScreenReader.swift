// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// TalkBack, Android's screen reader, in the guest. Andarium turns it on and
/// off over adb the way Android's own Accessibility page does: the service
/// joins or leaves the secure setting `enabled_accessibility_services`, and
/// `accessibility_enabled` follows whether any service is left. Every command
/// addresses the foreground user, whom it names (`--user`), and a change reads
/// the service list, edits it and reads it back in one shell command, so a
/// service Android turned on since the host last looked stays on. The system
/// image carries TalkBack and its voice; a guest whose system has none offers
/// no switch.
public enum ScreenReader {
    /// The TalkBack builds Andarium recognises, by their service: the
    /// open-source build the system image carries, then Google's own.
    public static let services = [
        "com.android.talkback/com.google.android.marvin.talkback.TalkBackService",
        "com.google.android.marvin.talkback/com.google.android.marvin.talkback.TalkBackService",
    ]

    /// The most a report holds, in bytes: a complete one is a few hundred. A
    /// longer reply is not a report.
    static let maximumReportBytes = 32 * 1024
    /// The most services a list holds, and the longest list and component, as
    /// the report accepts them and the change command accepts a live list.
    static let maximumServices = 64
    static let maximumListLength = 8192
    static let maximumComponentLength = 256

    /// The first words of every command: the foreground user's number, which
    /// the rest names. The command ends here when Android names none, or its
    /// lookup failed (whatever it printed): the user is only digits, at most
    /// nine.
    private static let user = #"user=$(am get-current-user) || exit 1; case "$user" in ''|*[!0-9]*|??????????*) exit 1;; esac"#

    /// What a read and a change end with: the output `parse` reads, which is
    /// these records and no others, in this order: the status of each of the
    /// three commands, the packages `pm list packages` printed (it filters by
    /// substring, one `package:<name>` line each), the setting's value and the
    /// terminator. A failing command is a status, not an empty answer; the
    /// terminator says the report was printed to its end.
    private static let report = [
        "packages=$(pm list packages --user $user talkback); pm=$?",
        "services=$(settings --user $user get secure enabled_accessibility_services); sg=$?",
        "enabled=$(settings --user $user get secure accessibility_enabled); ag=$?",
        #"echo "pm=$pm"; echo "sg=$sg"; echo "ag=$ag""#,
        #"if [ -n "$packages" ]; then printf '%s\n' "$packages"; fi"#,
        #"echo "services=$services"; echo "accessibility=$enabled"; echo "end=1""#,
    ].joined(separator: "; ")

    /// What to run in the guest's shell to read the state; `parse` reads its
    /// output.
    public static let stateCommand = "\(user); \(report)"

    /// The state from the output of `stateCommand` or `changeCommand`; nil when
    /// the output is not exactly the report they print: a status that is not
    /// zero, a record missing, repeated, out of its place or unknown, a name
    /// that is not a package or a component, a character that is not printable
    /// ASCII, too much, or no terminator. Such a reply is not an answer, and
    /// is never read as on, off or no screen reader. A report whose setting is
    /// empty or `null` is an answer: no service is on.
    public static func parse(_ output: String) -> ScreenReaderState? {
        guard output.utf8.count <= maximumReportBytes,
              output.utf8.allSatisfy({ $0 == 0x0A || (0x20...0x7E).contains($0) }),
              output.hasSuffix("\n") else { return nil }
        var lines = output.split(separator: "\n", omittingEmptySubsequences: false).dropLast()[...]
        func next() -> Substring? { lines.popFirst() }
        func status(_ name: String) -> Bool {
            guard let line = next(), line.hasPrefix(name + "="), line.dropFirst(name.count + 1) == "0" else { return false }
            return true
        }
        guard status("pm"), status("sg"), status("ag") else { return nil }
        var packages = Set<String>()
        var count = 0
        while let line = lines.first, line.hasPrefix("package:") {
            let name = line.dropFirst("package:".count)
            count += 1
            guard count <= maximumServices, isPackage(name) else { return nil }
            packages.insert(String(name))
            lines.removeFirst()
        }
        guard let servicesLine = next(), servicesLine.hasPrefix("services="),
              let enabled = serviceList(String(servicesLine.dropFirst("services=".count))) else { return nil }
        guard let settingLine = next(), settingLine.hasPrefix("accessibility=") else { return nil }
        let setting: Bool
        switch settingLine.dropFirst("accessibility=".count) {
        case "1": setting = true
        case "0", "null": setting = false
        default: return nil
        }
        guard next() == "end=1", lines.isEmpty else { return nil }
        let installed = services.first { packages.contains(package(of: $0)) }
        return ScreenReaderState(installedService: installed, enabledServices: enabled, accessibilitySetting: setting)
    }

    /// The state from a finished `stateCommand` or `changeCommand`: nil when
    /// the shell, or a command in it, failed, or its output is not valid UTF-8.
    public static func parse(_ result: ShellResult) -> ScreenReaderState? {
        guard result.exitCode == 0, let text = String(data: result.stdout, encoding: .utf8) else { return nil }
        return parse(text)
    }

    /// The services in the setting's value: `null` or nothing when it was never
    /// written, else colon-separated components, none empty, each a package and
    /// a class (`isComponent`), at most `maximumServices` within
    /// `maximumListLength`. nil for any other value.
    public static func serviceList(_ value: String) -> [String]? {
        guard value.utf8.count <= maximumListLength else { return nil }
        if value == "null" || value.isEmpty { return [] }
        let components = value.split(separator: ":", omittingEmptySubsequences: false)
        guard components.count <= maximumServices, components.allSatisfy(isComponent) else { return nil }
        return components.map(String.init)
    }

    /// `package/class`: a package name's characters, one slash, then a class
    /// name's (a nested class has `$`).
    static func isComponent(_ value: Substring) -> Bool {
        guard value.utf8.count <= maximumComponentLength else { return false }
        let parts = value.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2, isPackage(parts[0]), !parts[1].isEmpty else { return false }
        return parts[1].utf8.allSatisfy { $0 == 0x24 || isNameByte($0) }
    }

    static func isPackage(_ value: Substring) -> Bool {
        !value.isEmpty && value.utf8.count <= maximumComponentLength && value.utf8.allSatisfy(isNameByte)
    }

    private static func isNameByte(_ byte: UInt8) -> Bool {
        (0x30...0x39).contains(byte) || (0x41...0x5A).contains(byte) || (0x61...0x7A).contains(byte)
            || byte == 0x5F || byte == 0x2E
    }

    /// The command that turns TalkBack's `service` on or off. It reads the
    /// setting itself and refuses, before it writes anything, a list that is
    /// not one (`serviceList`'s rules, checked in the shell, which splits it
    /// without pathname expansion), takes every known TalkBack out of it, adds
    /// `service` at the end when turning on, writes the list back and
    /// `accessibility_enabled` 1 (or deletes the setting and writes 0 when
    /// none is left), and ends with the report of `stateCommand`. It fails
    /// when the lookup of the user, the read, any write or the delete does.
    public static func changeCommand(turningOn on: Bool, service: String) -> String {
        let known = services.map(shellQuoted).joined(separator: "|")
        let adding = on ? ##"list="${list:+$list:}""## + shellQuoted(service) + "; " : ""
        let validate = #"case "$s" in */*/*|/*|*/|*[!A-Za-z0-9_./\$]*) exit 1;; */*) ;; *) exit 1;; esac"#
        return [
            user,
            "old=$(settings --user $user get secure enabled_accessibility_services) || exit 1",
            #"case "$old" in null) old=;; esac"#,
            "[ ${#old} -le \(maximumListLength) ] || exit 1",
            #"case "$old" in :*|*:|*::*) exit 1;; esac"#,
            "list=; n=0; ifs=$IFS; set -f; IFS=:",
            "for s in $old; do n=$((n+1)); [ $n -le \(maximumServices) ] || exit 1; \(validate); "
                + #"case "$s" in \#(known)) ;; *) list="${list:+$list:}$s";; esac; done"#,
            "IFS=$ifs; set +f",
            adding
                + #"if [ -n "$list" ]; then "#
                + "settings --user $user put secure enabled_accessibility_services \"$list\" && "
                + "settings --user $user put secure accessibility_enabled 1 || exit 1; else "
                + "settings --user $user delete secure enabled_accessibility_services >/dev/null && "
                + "settings --user $user put secure accessibility_enabled 0 || exit 1; fi",
            report,
        ].joined(separator: "; ")
    }

    /// Opens Android's Accessibility page, where TalkBack's own settings and
    /// the other services are.
    public static let settingsCommand =
        "\(user); am start --user $user -a android.settings.ACCESSIBILITY_SETTINGS >/dev/null"

    /// The state a change leaves in a mock guest, which has no shell.
    public static func state(afterTurningOn on: Bool, _ state: ScreenReaderState) -> ScreenReaderState {
        var enabled = state.enabledServices.filter { !services.contains($0) }
        if on, let service = state.installedService { enabled.append(service) }
        return ScreenReaderState(installedService: state.installedService, enabledServices: enabled)
    }

    /// What a guest's error text becomes where a person reads or hears it: no
    /// control or format character (a line break is a space), runs of spaces
    /// made one, and at most `maximumFailureText` characters; nil when nothing
    /// printable is left.
    public static func failureText(_ stderr: Data) -> String? {
        let bytes = stderr.prefix(4096)
        var text = ""
        var lastWasSpace = true
        for scalar in String(decoding: bytes, as: UTF8.self).unicodeScalars {
            let category = scalar.properties.generalCategory
            if scalar.properties.isWhitespace || category == .lineSeparator || category == .paragraphSeparator {
                if !lastWasSpace { text.append(" ") }
                lastWasSpace = true
            } else if category == .control || category == .format || category == .surrogate
                || category == .privateUse || category == .unassigned || scalar == "\u{FFFD}" {
                continue
            } else {
                text.unicodeScalars.append(scalar)
                lastWasSpace = false
            }
        }
        text = text.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return nil }
        return text.count > maximumFailureText ? String(text.prefix(maximumFailureText)) + "\u{2026}" : text
    }

    static let maximumFailureText = 160

    static func package(of service: String) -> String {
        String(service.prefix { $0 != "/" })
    }

    static func shellQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}

/// TalkBack in the guest as Android last reported it.
public struct ScreenReaderState: Equatable, Sendable {
    /// The service of the TalkBack the guest has, from `ScreenReader.services`;
    /// nil when it has none.
    public var installedService: String?
    /// Every accessibility service the guest has turned on, in its order.
    public var enabledServices: [String]
    /// The guest's `accessibility_enabled` as read; nil where it was not read
    /// (a mock guest), which follows the list as a change leaves it.
    public var accessibilitySetting: Bool?

    public init(installedService: String?, enabledServices: [String], accessibilitySetting: Bool? = nil) {
        self.installedService = installedService
        self.enabledServices = enabledServices
        self.accessibilitySetting = accessibilitySetting
    }

    /// `accessibility_enabled` is on: the guest binds its services.
    var accessibilityIsEnabled: Bool { accessibilitySetting ?? !enabledServices.isEmpty }

    /// `accessibility_enabled` agrees with the list: on exactly while a service
    /// is in it, as the change command leaves both. A write that failed half
    /// way leaves them apart.
    public var isConsistent: Bool { accessibilityIsEnabled == !enabledServices.isEmpty }

    /// TalkBack is installed, in the list and accessibility is enabled: what
    /// the person's Android runs. A list that names it while accessibility is
    /// off is not "on".
    public var isOn: Bool {
        guard let installedService else { return false }
        return accessibilityIsEnabled && enabledServices.contains(installedService)
    }

    public var isInstalled: Bool { installedService != nil }
}

/// What the TalkBack switch of the session's Settings offers.
public enum ScreenReaderOffer: Equatable, Sendable {
    /// Not read yet.
    case unknown
    /// The last read got no answer: Android did not say.
    case failed
    case on
    case off
    /// The guest's system has no TalkBack.
    case unavailable

    public init(state: ScreenReaderState?, readFailed: Bool = false) {
        if readFailed {
            self = .failed
            return
        }
        guard let state else {
            self = .unknown
            return
        }
        if state.isInstalled {
            self = state.isOn ? .on : .off
        } else {
            self = .unavailable
        }
    }

    /// Whether the switch shows TalkBack on.
    public var isOn: Bool { self == .on }

    /// Whether the switch can be used.
    public var canChange: Bool {
        switch self {
        case .on, .off: true
        case .unknown, .failed, .unavailable: false
        }
    }
}
