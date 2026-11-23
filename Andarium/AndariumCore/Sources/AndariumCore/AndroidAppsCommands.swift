// SPDX-License-Identifier: GPL-2.0-or-later

import CryptoKit
import Foundation

/// Runs one command in the guest's shell with the shell protocol v2 and
/// returns what it left: the icons come back as bytes on stdout, which a text
/// answer would break. A failure to reach the guest throws.
public protocol GuestBinaryShell: Sendable {
    func runBinary(_ command: String, limit: Duration) async throws -> ShellResult
}

/// The image's `kernarium_apps` tool, which lists the launchable apps with
/// their names and icons and starts one. It is the only code that knows the
/// tool's words and the format of its answers; the rest of the application
/// sees `AndroidApp`, `AndroidAppIconAnswer` and the answers of
/// `AndroidAppsTool`. The tool's contract (`android/lineage-24/README.md`):
/// four verbs, one version number on the first line of every answer, text in
/// lines ended by LF with TAB-separated fields, an `end` line that closes a
/// listing or an icon answer, and an exit status that says how it ended. New
/// fields only ever come at the end of a line and new header keys are ignored;
/// another version number is a tool this Andarium does not know. Everything
/// that comes back is untrusted: a stream whose framing does not hold is
/// discarded whole, and what is kept is checked again against the grammar.
public struct AndroidAppsCommands: AndroidAppsTool {
    public static let commandName = "kernarium_apps"
    /// The one version of the answers this reader knows.
    public static let supportedVersion = 1

    /// The shell's status for a command it cannot find (an image older than the tool).
    private static let commandNotFound = 127
    /// The tool's status for arguments it does not know.
    private static let usageStatus = 64
    /// The tool's status for a launch it refuses.
    private static let refusedStatus = 2
    /// The tool's status for a failure whose reason it does not give.
    private static let failureStatus = 1

    /// A listing is under 900 KiB, an icon answer under 800 KiB.
    static let longestListing = 900 * 1_024
    static let longestIconAnswer = 800 * 1_024
    /// The largest PNG the tool sends, and the sizes it renders.
    static let largestPNG = 98_304
    static let iconSizes: Set<Int> = [192, 128, 96]

    private let shell: any GuestBinaryShell

    public init(shell: any GuestBinaryShell) {
        self.shell = shell
    }

    // MARK: Questions

    public func digest(limit: Duration = AndroidAppLimits.digestLimit) async -> AndroidAppDigestAnswer {
        guard let result = try? await shell.runBinary(Self.digestCommand, limit: limit) else { return .failed }
        return Self.parseDigest(result)
    }

    public func list(limit: Duration = AndroidAppLimits.listLimit) async -> AndroidAppListing {
        guard let result = try? await shell.runBinary(Self.listCommand, limit: limit) else { return .failed }
        return Self.parseListing(result)
    }

    public func icons(of apps: [AndroidApp], limit: Duration = AndroidAppLimits.iconLimit) async -> [String: AndroidAppIconAnswer] {
        // The tool refuses a component named twice.
        var seen = Set<String>()
        let asked = Array(apps.filter { seen.insert($0.id).inserted }.prefix(AndroidAppLimits.iconsPerAsk))
        guard let command = Self.iconsCommand(for: asked.map(\.component)),
              let result = try? await shell.runBinary(command, limit: limit),
              let answers = Self.parseIcons(result, asked: asked.map(\.component))
        else { return [:] }
        return Dictionary(uniqueKeysWithValues: zip(asked.map(\.id), answers))
    }

    public func open(_ component: AndroidComponent, limit: Duration = AndroidAppLimits.openLimit) async -> AndroidAppOpenAnswer {
        guard let result = try? await shell.runBinary(Self.launchCommand(for: component), limit: limit) else { return .failed }
        return Self.parseLaunch(result, component: component)
    }

    // MARK: Commands

    public static let digestCommand = "\(commandName) digest"
    public static let listCommand = "\(commandName) list"

    /// One to eight components, each in single quotes (a `$` of an inner
    /// class is no shell's), or nil for none, for more, or for a repeat.
    public static func iconsCommand(for components: [AndroidComponent]) -> String? {
        guard (1...AndroidAppLimits.iconsPerAsk).contains(components.count), Set(components).count == components.count
        else { return nil }
        return "\(commandName) icons " + components.map { "'\($0.identifier)'" }.joined(separator: " ")
    }

    public static func launchCommand(for component: AndroidComponent) -> String {
        "\(commandName) launch '\(component.identifier)'"
    }

    // MARK: Answers

    public static func parseDigest(_ result: ShellResult) -> AndroidAppDigestAnswer {
        switch status(of: result) {
        case .tooOld: return .unavailable
        case .failed: return .failed
        case .ok: break
        }
        guard let lines = textLines(result.stdout, longest: 4_096) else { return .failed }
        switch header(lines) {
        case .unknownVersion: return .unavailable
        case .malformed: return .failed
        case let .known(fields, rest):
            // The header block is the whole answer, and all of it is required.
            guard rest.isEmpty, let block = requiredBlock(fields) else { return .failed }
            return .digest(block.digest)
        }
    }

    public static func parseListing(_ result: ShellResult) -> AndroidAppListing {
        switch status(of: result) {
        case .tooOld: return .unavailable
        case .failed: return .failed
        case .ok: break
        }
        guard result.stdout.count <= longestListing, let lines = textLines(result.stdout, longest: longestListing)
        else { return .failed }
        let block: [String: String]
        let rest: ArraySlice<String>
        switch header(lines) {
        case .unknownVersion: return .unavailable
        case .malformed: return .failed
        case let .known(fields, remaining): (block, rest) = (fields, remaining)
        }
        guard let header = requiredBlock(block) else { return .failed }
        let (digest, count) = (header.digest, header.count)
        var rows = rest
        // The marker is its first field: a later version may append more.
        guard let marker = rows.first, marker.split(separator: "\t", omittingEmptySubsequences: false).first == "rows"
        else { return .failed }
        rows = rows.dropFirst()
        guard let last = rows.last, counted("end", in: last) == count else { return .failed }
        rows = rows.dropLast()
        guard rows.count == count else { return .failed }
        var seen = Set<AndroidComponent>()
        var apps: [AndroidApp] = []
        for row in rows {
            guard let app = parseApp(row) else { return .failed }
            guard seen.insert(app.component).inserted else { continue }
            apps.append(app)
        }
        return .apps(AndroidAppList(apps: apps, digest: digest))
    }

    /// One row: at least the five fields; a field a later version appends is
    /// ignored. A row with fewer, a component or an icon key that is not what
    /// the grammar says, or a built-in flag that is neither 0 nor 1 makes the
    /// whole answer a failure.
    static func parseApp(_ row: String) -> AndroidApp? {
        let fields = row.split(separator: "\t", omittingEmptySubsequences: false)
        guard fields.count >= 5, let component = AndroidComponent(packageName: String(fields[0]), activityClass: String(fields[1])),
              fields[3] == "0" || fields[3] == "1", isHex(String(fields[4]), length: 16)
        else { return nil }
        return AndroidApp(
            component: component, label: AndroidAppText.label(String(fields[2]), fallback: component.packageName),
            builtIn: fields[3] == "1", iconTag: String(fields[4]))
    }

    /// The answers of an icons command, in the order the components were
    /// asked, or nil when the stream's framing does not hold (it is then
    /// discarded whole). A record whose bytes do not pass the checks (the
    /// digest, the PNG signature, the size) is an `error` for that app only.
    public static func parseIcons(_ result: ShellResult, asked: [AndroidComponent]) -> [AndroidAppIconAnswer]? {
        guard status(of: result) == .ok, result.stdout.count <= longestIconAnswer else { return nil }
        var reader = ByteReader(result.stdout)
        guard reader.line() == "\(commandName) \(supportedVersion)", let opening = reader.line(),
              counted("icons", in: opening) == asked.count
        else { return nil }
        var answers: [AndroidAppIconAnswer] = []
        for component in asked {
            guard let line = reader.line() else { return nil }
            let fields = line.split(separator: "\t", omittingEmptySubsequences: false)
            guard fields.count >= 9, fields[0] == "icon", fields[1] == Substring(component.identifier) else { return nil }
            switch fields[2] {
            case "ok":
                guard let width = Int(fields[3]), let height = Int(fields[4]), let length = Int(fields[7]),
                      width == height, iconSizes.contains(width), (1...largestPNG).contains(length),
                      fields[6] == "0" || fields[6] == "1", isHex(String(fields[8]), length: 64),
                      let data = reader.bytes(length), reader.byte() == 0x0A
                else { return nil }
                answers.append(checkedIcon(data, side: width, digest: String(fields[8]), hasAlpha: fields[6] == "1"))
            case "missing", "noicon", "error", "deferred":
                // Nothing follows such a record, not even a line feed.
                guard fields[3] == "0", fields[4] == "0", fields[7] == "0" else { return nil }
                answers.append(fields[2] == "missing" ? .missing : fields[2] == "noicon" ? .noIcon : fields[2] == "error" ? .error : .deferred)
            default:
                return nil
            }
        }
        guard let closing = reader.line(), counted("end", in: closing) == asked.count, reader.atEnd else { return nil }
        return answers
    }

    public static func parseLaunch(_ result: ShellResult, component: AndroidComponent) -> AndroidAppOpenAnswer {
        switch result.exitCode {
        case 0:
            // One line, `launched`, the component and whatever a later version
            // appends, ended by its line feed: an answer cut short is no answer.
            guard let lines = textLines(result.stdout, longest: 4_096), lines.count == 1 else { return .failed }
            let fields = lines[0].split(separator: "\t", omittingEmptySubsequences: false)
            return fields.count >= 2 && fields[0] == "launched" && fields[1] == Substring(component.identifier) ? .opened : .failed
        case refusedStatus:
            return .notFound
        case failureStatus:
            return .startFailed
        case commandNotFound, usageStatus:
            return .unavailable
        default:
            return .failed
        }
    }

    // MARK: Framing

    private enum Status { case ok, tooOld, failed }

    /// Exit 127 is an image without the tool and 64 a tool that would not
    /// take the command: neither offers the feature. Any other status but 0
    /// is a failure, and then stdout holds nothing to use.
    private static func status(of result: ShellResult) -> Status {
        switch result.exitCode {
        case 0: .ok
        case commandNotFound, usageStatus: .tooOld
        default: .failed
        }
    }

    private enum Header {
        case unknownVersion
        case malformed
        /// The header keys, and the lines after the block (up to and including `rows`, for a listing).
        case known([String: String], ArraySlice<String>)
    }

    /// The version line, then `key<TAB>value` lines up to the first line that
    /// is not one (a listing's `rows`) or the end. A key that repeats is a
    /// malformed header.
    private static func header(_ lines: [String]) -> Header {
        guard let first = lines.first else { return .malformed }
        let words = first.split(separator: " ", omittingEmptySubsequences: false)
        guard words.count == 2, words[0] == Substring(commandName), let version = Int(words[1]) else { return .malformed }
        guard version == supportedVersion else { return .unknownVersion }
        var fields: [String: String] = [:]
        var index = 1
        while index < lines.count {
            let parts = lines[index].split(separator: "\t", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2, parts[0] != "rows" else { break }
            // A value is its first field: a later version may append more.
            let value = parts[1].split(separator: "\t", maxSplits: 1, omittingEmptySubsequences: false).first ?? ""
            guard fields.updateValue(String(value), forKey: String(parts[0])) == nil else { return .malformed }
            index += 1
        }
        return .known(fields, lines[index...])
    }

    /// The text of an answer as lines, or nil when it is not UTF-8, holds a
    /// carriage return, a NUL, or is longer than `longest`.
    private static func textLines(_ data: Data, longest: Int) -> [String]? {
        guard data.count <= longest, !data.contains(0x0D), !data.contains(0), let text = String(data: data, encoding: .utf8) else { return nil }
        var lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        // The last line ends with its LF, which leaves an empty piece.
        guard lines.last == "" else { return nil }
        lines.removeLast()
        return lines
    }

    private static func isHex(_ text: String, length: Int) -> Bool {
        text.utf8.count == length && text.utf8.allSatisfy { ($0 >= 48 && $0 <= 57) || ($0 >= 97 && $0 <= 102) }
    }

    /// A decimal number of digits only, no sign and no space.
    private static func decimal(_ text: String) -> Int? {
        guard !text.isEmpty, text.utf8.count <= 9, text.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }) else { return nil }
        return Int(text)
    }

    /// The header block every answer of `digest` and `list` begins with, each
    /// of its lines required and well formed (the digest, the count and
    /// `skipped` and `user` as numbers, and the `locales` line, which may be
    /// empty). Keys it does not know are ignored.
    private static func requiredBlock(_ fields: [String: String]) -> (digest: String, count: Int)? {
        guard let digest = fields["digest"], isHex(digest, length: 64),
              let count = fields["count"].flatMap(decimal), (0...AndroidAppLimits.mostApps).contains(count),
              fields["skipped"].flatMap(decimal) != nil, fields["locales"] != nil, fields["user"].flatMap(decimal) != nil
        else { return nil }
        return (digest, count)
    }

    /// The number of a line `key<TAB>N`, with any fields a later version
    /// appends ignored.
    private static func counted(_ key: String, in line: String) -> Int? {
        let fields = line.split(separator: "\t", omittingEmptySubsequences: false)
        guard fields.count >= 2, fields[0] == Substring(key) else { return nil }
        return decimal(String(fields[1]))
    }

    private static let pngSignature = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])

    /// An icon's bytes against what its record declared: the digest, the PNG
    /// signature, and an image header (IHDR) of the declared square size.
    private static func checkedIcon(_ data: Data, side: Int, digest: String, hasAlpha: Bool) -> AndroidAppIconAnswer {
        let sum = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard sum == digest, data.count >= 24, data.prefix(8) == pngSignature else { return .error }
        let bytes = Array(data)
        let width = Int(bytes[16]) << 24 | Int(bytes[17]) << 16 | Int(bytes[18]) << 8 | Int(bytes[19])
        let height = Int(bytes[20]) << 24 | Int(bytes[21]) << 16 | Int(bytes[22]) << 8 | Int(bytes[23])
        guard width == side, height == side, Array(bytes[12..<16]) == Array("IHDR".utf8) else { return .error }
        return .png(data, hasAlpha: hasAlpha)
    }
}

/// Reads an answer that mixes text lines and counted bytes.
private struct ByteReader {
    private let data: Data
    private var offset: Data.Index

    init(_ data: Data) {
        self.data = data
        offset = data.startIndex
    }

    var atEnd: Bool { offset == data.endIndex }

    /// The text up to the next LF, which is consumed; nil at the end, or for
    /// a line that is not UTF-8 or that holds a carriage return.
    mutating func line() -> String? {
        guard let end = data[offset...].firstIndex(of: 0x0A) else { return nil }
        let bytes = data[offset..<end]
        guard !bytes.contains(0x0D), let text = String(data: bytes, encoding: .utf8) else { return nil }
        offset = data.index(after: end)
        return text
    }

    mutating func bytes(_ count: Int) -> Data? {
        guard count >= 0, data.endIndex - offset >= count else { return nil }
        let chunk = Data(data[offset..<(offset + count)])
        offset += count
        return chunk
    }

    mutating func byte() -> UInt8? {
        guard offset < data.endIndex else { return nil }
        defer { offset += 1 }
        return data[offset]
    }
}
