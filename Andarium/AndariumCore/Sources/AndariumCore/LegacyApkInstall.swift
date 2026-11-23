// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the package manager is sent to install one app, and how its answer is
/// read. Settings' Install APK is the one caller that can ask for the check
/// on the app's target version to be skipped (`LegacyInstallFlow`).
public enum PackageInstallCommand {
    /// Android's `INSTALL_REASON_USER`: an install the person started.
    public static let userInstallReason = 4

    /// The most the package manager's answer to one install call may be. It is
    /// held when it arrives while the host still sends the file, so the stream
    /// is opened with it as its output limit.
    public static let replyBudgetBytes = 64 * 1024

    /// What lifts Android's refusal of an app built for a version older than
    /// the one it installs.
    public static let bypassLowTargetSdkBlock = "--bypass-low-target-sdk-block"

    /// The `abb_exec` service of an install of `size` bytes. `-r` replaces an
    /// installed app, as the person expects of an APK for one already there;
    /// `-t` admits test-only builds; `--install-reason` marks the install as
    /// the person's, because the launcher adds a home-screen icon only for
    /// such an install; `-S` gives the size and stays the tail, which the
    /// package manager reads the stream by. `bypassLowTargetSdkBlock` adds
    /// the one flag a second attempt of a refused legacy app carries and
    /// changes nothing else; it is false for every other install.
    public static func service(size: Int, bypassLowTargetSdkBlock: Bool = false) -> String {
        var words = ["abb_exec:package", "install", "-r", "-t", "--install-reason", String(userInstallReason)]
        if bypassLowTargetSdkBlock { words.append(Self.bypassLowTargetSdkBlock) }
        words += ["-S", String(size)]
        return words.joined(separator: "\0")
    }

    // MARK: Sessions

    // An app in several files (splits) is installed through a session, which
    // is how `adb install-multiple` does it: create it, write each file into
    // it, commit. The words are those of `service`, the flags of the create
    // are the single install's, and a session is a thing on the guest until
    // it is committed (which ends it, whatever the answer) or abandoned.

    /// `install-create` for a session whose files add up to `totalBytes`.
    /// The flag that lifts the refusal of an app built for a much older
    /// version belongs here and nowhere else: the session reads it when it is
    /// created, not when it is committed.
    public static func createSession(totalBytes: UInt64, bypassLowTargetSdkBlock: Bool = false) -> String {
        var words = ["abb_exec:package", "install-create", "-r", "-t", "--install-reason", String(userInstallReason)]
        if bypassLowTargetSdkBlock { words.append(Self.bypassLowTargetSdkBlock) }
        words += ["-S", String(totalBytes)]
        return words.joined(separator: "\0")
    }

    /// The same, for a session whose base APK must be the package `packageName`
    /// (`--pkg`): the package manager refuses the commit of any other. It is
    /// how an archive's unsigned claim about its app becomes a fact before
    /// anything is placed on the strength of it. A name that is not a package
    /// name is refused here, so that no word of the command is the archive's.
    public static func createSession(
        totalBytes: UInt64, bypassLowTargetSdkBlock: Bool = false, boundTo packageName: String
    ) throws -> String {
        guard ApkContainer.isPackageName(packageName) else { throw InstallRefusal.damaged }
        var words = createSession(totalBytes: totalBytes, bypassLowTargetSdkBlock: bypassLowTargetSdkBlock).split(
            separator: "\0", omittingEmptySubsequences: false).map(String.init)
        // `-S` and its size stay the tail.
        words.insert(contentsOf: ["--pkg", packageName], at: words.count - 2)
        return words.joined(separator: "\0")
    }

    /// `install-write` of one file of `bytes` bytes, which then follow on the
    /// stream (`-` is the standard input). `name` is the file's name in the
    /// session; it must be unique there and `isValidPartName`.
    public static func writeSession(_ session: String, name: String, bytes: UInt64) -> String {
        ["abb_exec:package", "install-write", "-S", String(bytes), session, name, "-"].joined(separator: "\0")
    }

    /// `install-commit`: the package manager checks the files, installs the
    /// app and answers `Success` or `Failure [CODE: text]`. A commit that
    /// fails ends the session as well.
    public static func commitSession(_ session: String) -> String {
        ["abb_exec:package", "install-commit", session].joined(separator: "\0")
    }

    /// `install-abandon`: deletes a session that was not committed. After a
    /// commit the session is gone and the package manager answers with an
    /// exception, so a caller abandons only before it commits.
    public static func abandonSession(_ session: String) -> String {
        ["abb_exec:package", "install-abandon", session].joined(separator: "\0")
    }

    /// The id in `Success: created install session [1858877423]`; nil for
    /// any other output (an exception trace, a full partition).
    public static func createdSession(inOutput output: String) -> String? {
        for line in output.split(whereSeparator: \.isNewline) {
            let text = line.trimmingCharacters(in: .whitespaces)
            guard text.hasPrefix("Success"), text.contains("created install session"),
                  let open = text.lastIndex(of: "["), let close = text.lastIndex(of: "]"), open < close
            else { continue }
            let id = text[text.index(after: open)..<close]
            if isSessionId(id) { return String(id) }
        }
        return nil
    }

    /// Whether the output of an `install-write` says the file arrived whole:
    /// `Success: streamed 2546 bytes`, and no failure beside it.
    public static func streamed(inOutput output: String, bytes: UInt64) -> Bool {
        let lines = output.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
        if case .failure = PackageInstallResponse(output: output) { return false }
        return lines.contains("Success: streamed \(bytes) bytes")
    }

    /// A session id is the digits of a positive 32-bit number.
    public static func isSessionId(_ text: some StringProtocol) -> Bool {
        !text.isEmpty && text.utf8.count <= 10 && text.utf8.allSatisfy { $0 >= 48 && $0 <= 57 } && (Int32(text) ?? 0) > 0
    }

    /// A name a file can have in a session: the package manager takes it as a
    /// file name, so it is one short component of plain characters, and it
    /// ends in `.apk`.
    public static func isValidPartName(_ name: String) -> Bool {
        name.hasSuffix(".apk") && name.utf8.count <= 128
            && name.utf8.allSatisfy { ($0 >= 48 && $0 <= 57) || ($0 >= 65 && $0 <= 90) || ($0 >= 97 && $0 <= 122) || $0 == 46 || $0 == 45 || $0 == 95 }
            && !name.hasPrefix(".")
    }
}

/// The versions in the package manager's refusal of an app built for a much
/// older Android, each nil when the answer does not say it.
public struct LegacyTarget: Equatable, Sendable {
    /// The API level the app targets (`but found 7`).
    public var appLevel: Int?
    /// The lowest API level the virtual machine's Android installs an app
    /// for (`at least SDK version 24`).
    public var requiredLevel: Int?

    public init(appLevel: Int? = nil, requiredLevel: Int? = nil) {
        self.appLevel = appLevel
        self.requiredLevel = requiredLevel
    }
}

/// What the package manager answered a command that ends in `Success` or in
/// `Failure [CODE: text]`, read once for every such answer. The text after a
/// `Failure` is a diagnostic the package manager builds from the APK, so it can
/// hold any line, `Success` included: success is the whole answer being the one
/// success line (after the lines an adb client prints before it), and a
/// `Failure` line anywhere, the first of them being the package manager's own,
/// is the answer however many lines follow it.
public enum PackageInstallResponse: Equatable, Sendable {
    case success
    /// The first line that starts with `Failure`.
    case failure(line: String)
    /// Neither: an exception trace, nothing, or text that is not an answer.
    case unrecognized

    /// The lines an adb client prints before the answer of a streamed install.
    private static let prelude: Set<String> = ["Performing Streamed Install", "Performing Push Install"]

    public init(output: String) {
        let lines = output.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        if let failure = lines.first(where: { $0.lowercased().hasPrefix("failure") }) {
            self = .failure(line: failure)
        } else if lines.filter({ $0 == "Success" }).count == 1, lines.allSatisfy({ $0 == "Success" || Self.prelude.contains($0) }) {
            self = .success
        } else {
            self = .unrecognized
        }
    }
}

/// What the package manager answered an install with, as far as the flow
/// needs it: the stream's output ends in `Success` or `Failure [CODE: text]`.
public enum PackageInstallAnswer: Equatable, Sendable {
    case installed
    /// The refusal of an app whose target version is below the floor:
    /// `INSTALL_FAILED_DEPRECATED_SDK_VERSION`. The only answer that gets
    /// the question.
    case tooOldTarget(LegacyTarget)
    /// Any other answer, with the code it names (`INSTALL_` dropped), if any.
    case refused(code: String?)

    /// The code of the too-old refusal, `INSTALL_` dropped.
    public static let tooOldCode = "FAILED_DEPRECATED_SDK_VERSION"

    /// Reads the output as a `PackageInstallResponse` does: only the whole
    /// answer being `Success` is a success, and the first line that starts
    /// with `Failure` is the answer otherwise. Case and the space around the
    /// words do not matter for a failure, and the text after the code may be
    /// missing.
    public init(output: String) {
        let line: String
        switch PackageInstallResponse(output: output) {
        case .success:
            self = .installed
            return
        case .unrecognized:
            self = .refused(code: nil)
            return
        case let .failure(first):
            line = first
        }
        guard let open = line.firstIndex(of: "[") else {
            self = .refused(code: nil)
            return
        }
        let inside = line[line.index(after: open)...]
        let name = inside.prefix { $0 != ":" && $0 != "]" }.trimmingCharacters(in: .whitespaces).uppercased()
        guard !name.isEmpty else {
            self = .refused(code: nil)
            return
        }
        let code = name.hasPrefix("INSTALL_") ? String(name.dropFirst("INSTALL_".count)) : name
        // Defensive: a line that names an instant app is not this refusal,
        // and is left to the ordinary words.
        guard code == Self.tooOldCode, !line.lowercased().contains("instant app") else {
            self = .refused(code: code)
            return
        }
        self = .tooOldTarget(Self.levels(in: String(inside)))
    }

    /// `App package must target at least SDK version 24, but found 7`: the
    /// number after "at least SDK version" and the one after "found".
    private static func levels(in text: String) -> LegacyTarget {
        let lower = text.lowercased()
        return LegacyTarget(
            appLevel: number(after: "found", in: lower), requiredLevel: number(after: "at least sdk version", in: lower))
    }

    /// The digits right after `marker` (a space, colon or equals sign may
    /// come between), nil when something else follows it.
    private static func number(after marker: String, in text: String) -> Int? {
        guard let range = text.range(of: marker) else { return nil }
        let rest = text[range.upperBound...].drop { $0 == " " || $0 == ":" || $0 == "=" }
        let digits = rest.prefix { $0.isASCII && $0.isNumber }
        return digits.isEmpty ? nil : Int(digits)
    }
}

/// Settings' Install APK as a state machine: the first attempt is the
/// ordinary install; only the too-old refusal of it asks the person, and
/// only "Install anyway" sends the same file again, with the flag, once.
/// Everything else the first attempt answers ends the install as it always
/// did, and nothing is asked twice for one file. The model drives it with the
/// real attempts and the person's answer.
public struct LegacyInstallFlow: Equatable, Sendable {
    public enum Step: Equatable, Sendable {
        /// Send the file now, with the flag when `bypassLowTargetSdkBlock`.
        case attempt(bypassLowTargetSdkBlock: Bool)
        /// The first attempt was refused for its target version: ask.
        case ask(LegacyTarget)
        case finished(Ending)
    }

    public enum Ending: Equatable, Sendable {
        case installed
        /// The person said Cancel, or the install was stopped. No words.
        case cancelled
        /// The last attempt's ordinary failure words show.
        case failed
    }

    /// What an attempt ended with.
    public enum Result: Equatable, Sendable {
        case installed
        case tooOldTarget(LegacyTarget)
        /// Any other failure, which Android or adb worded.
        case failed
    }

    public private(set) var step = Step.attempt(bypassLowTargetSdkBlock: false)
    /// The flag has been sent, or is being: the one second attempt is used.
    public private(set) var askedBefore = false

    public init() {}

    /// An attempt ended. Ignored unless one was under way.
    public mutating func attemptEnded(_ result: Result) {
        guard case let .attempt(bypass) = step else { return }
        switch result {
        case .installed:
            step = .finished(.installed)
        case let .tooOldTarget(target):
            // The flag attempt that is refused again, or a second question
            // for one file, is a failure with its ordinary words.
            if bypass || askedBefore {
                step = .finished(.failed)
            } else {
                askedBefore = true
                step = .ask(target)
            }
        case .failed:
            step = .finished(.failed)
        }
    }

    /// The person answered the question. Ignored unless one was asked.
    public mutating func answered(installAnyway: Bool) {
        guard case .ask = step else { return }
        step = installAnyway ? .attempt(bypassLowTargetSdkBlock: true) : .finished(.cancelled)
    }

    /// The install was stopped (the overlay's Cancel, or the guest ended)
    /// while an attempt or the question was open.
    public mutating func stopped() {
        if case .finished = step { return }
        step = .finished(.cancelled)
    }
}
