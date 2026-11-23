// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Opening the Magisk app on the guest's screen after the application put it
/// in (`RootInstallRule.opensAppAfterInstall`), so that its one-time setup
/// dialog is the first thing the person meets.
///
/// What the guest does, as measured on the image:
///
/// - `am start` resolves an implicit intent for default activities only, and
///   the app's launcher filter carries no default category, so the launcher's
///   own intent (`-a MAIN -c LAUNCHER -p <package>`) never resolves, however
///   late it is sent. The launcher activity is asked of the package manager
///   and started by its component.
/// - Before Android has finished starting, a start by component answers "too
///   early" or "does not exist"; user 0 is unlocked a few seconds after
///   `sys.boot_completed`, and a start by component already works in that gap.
/// - The app asks for the notification permission when it first opens, and
///   Android's dialog for it is an activity of the app's own task, so the
///   resumed activity is the dialog's. The task in focus is the app's.
///
/// The open waits a bounded time for the unlock, then tries a bounded number
/// of times, each try checked by the activity manager's own answer. The tries
/// run even when the unlock was never seen: a start while user 0 is locked
/// either fails, and is recorded as not opened, or succeeds, so it gives no
/// wrong result, and it keeps the open working if the answer to the unlock
/// question ever changes its shape. It never fails the start: the caller
/// records the result and goes on.
public enum MagiskAppOpening {
    public struct Policy: Equatable, Sendable {
        /// How many times user 0 is asked whether it is unlocked, `pollInterval`
        /// apart, before the tries begin; they begin whatever the answers were.
        public var unlockPolls: Int
        public var pollInterval: Duration
        /// How many times the app is started and checked, `retryInterval` apart.
        public var attempts: Int
        public var retryInterval: Duration
        /// The bound on each command.
        public var commandLimit: Duration

        public init(
            unlockPolls: Int, pollInterval: Duration, attempts: Int, retryInterval: Duration, commandLimit: Duration
        ) {
            self.unlockPolls = unlockPolls
            self.pollInterval = pollInterval
            self.attempts = attempts
            self.retryInterval = retryInterval
            self.commandLimit = commandLimit
        }

        /// At most 14 s of waiting for the unlock and 8 s between five tries.
        public static let standard = Policy(
            unlockPolls: 15, pollInterval: .seconds(1), attempts: 5, retryInterval: .seconds(2),
            commandLimit: .seconds(10))
    }

    /// Waits for `duration`; a cancelled caller is released at once.
    public static func pause(_ duration: Duration) async {
        try? await Task.sleep(for: duration)
    }

    /// Whether the app was on the guest's screen when the last try ended.
    /// `sleep` is the wait between asks, which a test replaces.
    public static func run(
        shell: any GuestShell, policy: Policy = .standard,
        sleep: @Sendable (Duration) async -> Void = MagiskAppOpening.pause
    ) async -> Bool {
        var polls = 0
        while polls < policy.unlockPolls {
            if Task.isCancelled { return false }
            if await isUnlocked(shell: shell, limit: policy.commandLimit) { break }
            polls += 1
            if polls < policy.unlockPolls { await sleep(policy.pollInterval) }
        }
        var attempt = 0
        while attempt < policy.attempts {
            if Task.isCancelled { return false }
            if await tryOnce(shell: shell, limit: policy.commandLimit) { return true }
            attempt += 1
            if attempt < policy.attempts { await sleep(policy.retryInterval) }
        }
        return false
    }

    static func isUnlocked(shell: any GuestShell, limit: Duration) async -> Bool {
        guard let answer = try? await shell.run(MagiskApp.userStateCommand, limit: limit) else { return false }
        return MagiskApp.isUnlocked(answer)
    }

    /// One try: the launcher activity is resolved, started, and found in focus.
    static func tryOnce(shell: any GuestShell, limit: Duration) async -> Bool {
        guard let resolved = try? await shell.run(MagiskApp.resolveCommand, limit: limit),
            let component = MagiskApp.launcherComponent(in: resolved)
        else { return false }
        guard let started = try? await shell.run(MagiskApp.startCommand(component: component), limit: limit),
            MagiskApp.startTaken(started)
        else { return false }
        guard let focus = try? await shell.run(MagiskApp.focusCommand, limit: limit) else { return false }
        return MagiskApp.hasFocus(focus)
    }
}

extension MagiskApp {
    /// `RUNNING_UNLOCKED` once user 0's credential-encrypted storage is open.
    static let userStateCommand = "am get-started-user-state 0"
    /// The activity the launcher would start, as `package/class` on the
    /// answer's last line.
    static let resolveCommand =
        "cmd package resolve-activity --brief --user 0 -a android.intent.action.MAIN "
        + "-c android.intent.category.LAUNCHER \(package)"
    /// The task in focus on the display.
    static let focusCommand = "dumpsys activity activities 2>/dev/null | grep topDisplayFocusedRootTask"

    /// The launcher's start by component, which carries no `-p` and so does not
    /// depend on the default category. The component is quoted: a class name
    /// may hold `$`.
    static func startCommand(component: String) -> String {
        "am start --user 0 -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n '\(component)'"
    }

    static func isUnlocked(_ output: GuestShellOutput) -> Bool {
        output.exitCode == 0 && output.text.trimmingCharacters(in: .whitespacesAndNewlines) == "RUNNING_UNLOCKED"
    }

    /// The `package/class` the resolver names on its last line, only when it
    /// is an activity of this package and a class name `am start` takes
    /// whole (the short `.ui.Main` form included).
    static func launcherComponent(in output: GuestShellOutput) -> String? {
        guard output.exitCode == 0,
            let line = output.text.split(whereSeparator: \.isNewline).last?.trimmingCharacters(in: .whitespaces)
        else { return nil }
        let parts = line.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2, parts[0] == package else { return nil }
        let name = Array(parts[1].utf8)
        guard let first = name.first, name.count <= AndroidComponent.longestActivityClass,
            first == UInt8(ascii: ".") || isIdentifierStart(first), name.last != UInt8(ascii: "."),
            !zip(name, name.dropFirst()).contains(where: { $0 == UInt8(ascii: ".") && $1 == UInt8(ascii: ".") }),
            name.allSatisfy({ isIdentifierStart($0) || ($0 >= UInt8(ascii: "0") && $0 <= UInt8(ascii: "9")) || $0 == UInt8(ascii: ".") })
        else { return nil }
        return String(line)
    }

    private static func isIdentifierStart(_ byte: UInt8) -> Bool {
        (byte >= UInt8(ascii: "a") && byte <= UInt8(ascii: "z")) || (byte >= UInt8(ascii: "A") && byte <= UInt8(ascii: "Z"))
            || byte == UInt8(ascii: "_") || byte == UInt8(ascii: "$")
    }

    /// The activity manager took the start: the command ended well and printed
    /// no error. An error is a line that begins with `Error`, or an exception
    /// the shell command reports ("Too early to start activity").
    static func startTaken(_ output: GuestShellOutput) -> Bool {
        guard output.exitCode == 0 else { return false }
        return !output.text.split(whereSeparator: \.isNewline).contains {
            $0.hasPrefix("Error") || $0.contains("Exception")
        }
    }

    /// The task in focus is this package's. The query's output is the lines of
    /// the activity manager's dump that name the focused task, so it counts
    /// only when the query succeeded and exactly one line is the current
    /// record, `topDisplayFocusedRootTask=Task{... A=10126:com.topjohnwu.magisk}`
    /// (the key begins the line, never the tail of another key such as
    /// `previous_topDisplayFocusedRootTask`), and the record's own value, up to
    /// its closing brace, holds exactly one `A=<uid>:<package>` field, at the
    /// value's own depth, naming the package: what follows the closing brace
    /// belongs to other fields, and what is inside a nested value belongs to
    /// another task. Two fields of that kind are ambiguous whatever they
    /// name. A value that never closes counts only when no other current key
    /// follows in it. Anything else, a second current record included, is not
    /// focus.
    static func hasFocus(_ output: GuestShellOutput) -> Bool {
        guard output.exitCode == 0 else { return false }
        let key = "topDisplayFocusedRootTask="
        let records = output.text.split(whereSeparator: \.isNewline).compactMap { line -> Substring? in
            let trimmed = line.drop(while: { $0 == " " || $0 == "\t" })
            return trimmed.hasPrefix(key) ? trimmed.dropFirst(key.count) : nil
        }
        guard records.count == 1, let record = records.first, record.hasPrefix("Task{") else { return false }
        let body = record.dropFirst("Task{".count)
        // The characters of the value's own depth. A nested value is left out and
        // leaves `{}` behind, so a token that touches one is no field of this task.
        var own = ""
        var depth = 1
        for character in body {
            if character == "{" {
                if depth == 1 { own += "{}" }
                depth += 1
            } else if character == "}" {
                depth -= 1
                if depth == 0 { break }
            } else if depth == 1 {
                own.append(character)
            }
        }
        if depth > 0 {
            // The value never closed.
            guard !body.contains(key) else { return false }
        }
        let fields = own.split(whereSeparator: { $0 == " " || $0 == "\t" }).filter { $0.hasPrefix("A=") }
        guard fields.count == 1, let field = fields.first else { return false }
        let rest = field.dropFirst(2)
        guard let colon = rest.firstIndex(of: ":") else { return false }
        let uid = rest[..<colon]
        return !uid.isEmpty && uid.allSatisfy(\.isASCII) && uid.allSatisfy(\.isNumber)
            && rest[rest.index(after: colon)...] == package
    }
}
