// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the package manager answered an install with, in plain words. The
/// install stream's output ends in `Success` or `Failure [CODE: text]`.
nonisolated enum PackageInstallMessages {
    /// The message for the codes pm prints (`INSTALL_` dropped) that mean the
    /// same to the person.
    private static let table: [(codes: [String], message: String)] = [
        (
            ["PARSE_FAILED_NOT_APK", "FAILED_INVALID_APK", "PARSE_FAILED_BAD_MANIFEST",
             "PARSE_FAILED_MANIFEST_MALFORMED", "PARSE_FAILED_UNEXPECTED_EXCEPTION"],
            String(localized: "The file isn\u{2019}t a valid app.")
        ),
        (
            ["PARSE_FAILED_NO_CERTIFICATES", "PARSE_FAILED_INCONSISTENT_CERTIFICATES",
             "PARSE_FAILED_CERTIFICATE_ENCODING"],
            String(localized: "The app isn\u{2019}t signed correctly.")
        ),
        (
            ["FAILED_UPDATE_INCOMPATIBLE", "FAILED_SHARED_USER_INCOMPATIBLE"],
            String(localized: "A version of this app signed by someone else is installed. Uninstall it in the virtual machine first.")
        ),
        (["FAILED_VERSION_DOWNGRADE"], String(localized: "A newer version of this app is already installed.")),
        (["FAILED_OLDER_SDK"], String(localized: "This app needs a newer system than this virtual machine runs.")),
        (
            ["FAILED_DEPRECATED_SDK_VERSION"],
            String(localized: "This app was made for a system version that this virtual machine no longer installs.")
        ),
        (
            ["FAILED_NO_MATCHING_ABIS"],
            String(localized: "This app has no code for this device\u{2019}s processor. Choose a version of the app made for ARM processors.")
        ),
        (
            ["FAILED_INSUFFICIENT_STORAGE"],
            String(localized: "The virtual machine doesn\u{2019}t have enough free space. Free some up in the virtual machine, or give it more storage with Resize.")
        ),
        (
            ["FAILED_MISSING_SPLIT"],
            String(localized: "Some parts of this app are missing. Choose an .apks, .xapk or .apkm file that holds all of them.")
        ),
        (["FAILED_MISSING_SHARED_LIBRARY"], String(localized: "The app needs a system library this virtual machine doesn\u{2019}t have.")),
        (
            ["FAILED_DUPLICATE_PERMISSION", "FAILED_CONFLICTING_PROVIDER", "FAILED_DUPLICATE_PACKAGE"],
            String(localized: "The app conflicts with an app that\u{2019}s already installed.")
        ),
        (
            ["FAILED_USER_RESTRICTED", "FAILED_VERIFICATION_FAILURE", "FAILED_ABORTED"],
            String(localized: "The virtual machine refused to install the app.")
        ),
    ]

    /// The result of an install from its output: nil for success, otherwise
    /// the message for the person. Output with neither word is a failure that
    /// names nothing.
    static func failure(inOutput output: String) -> String? {
        let generic = String(localized: "The virtual machine couldn\u{2019}t install the app.")
        switch PackageInstallResponse(output: output) {
        case .success:
            return nil
        case .unrecognized:
            // Session creation can fail on the exception alone, with no line
            // of its own: `IOException: Requested internal only, but not
            // enough space`.
            if isStorageException(output) { return message(forCode: "FAILED_INSUFFICIENT_STORAGE") }
            return generic
        case let .failure(line):
            guard let code = code(inFailure: line) else { return generic }
            return message(forCode: code)
        }
    }

    /// Whether the output is an exception that says the partition is full.
    private static func isStorageException(_ output: String) -> Bool {
        let text = output.lowercased()
        return text.contains("not enough space") || text.contains("no space left on device")
    }

    /// `Failure [INSTALL_FAILED_INVALID_APK: ...]` is `FAILED_INVALID_APK`.
    static func code(inFailure line: String) -> String? {
        guard let open = line.firstIndex(of: "[") else { return nil }
        let inside = line[line.index(after: open)...]
        let head = inside.prefix { $0 != ":" && $0 != "]" }
        let name = head.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return nil }
        return name.hasPrefix("INSTALL_") ? String(name.dropFirst("INSTALL_".count)) : name
    }

    static func message(forCode code: String) -> String {
        table.first { $0.codes.contains(code) }?.message ?? String(localized: "The virtual machine couldn\u{2019}t install the app (\(code)).")
    }

    /// Every code the table maps, for the test that checks the table.
    static var mappedCodes: [String] { table.flatMap(\.codes) }
}

/// How an install that reached the package manager ended.
nonisolated enum ApkInstallOutcome: Equatable, Sendable {
    case installed
    /// The app is installed, but the data files its archive carried (an OBB
    /// file of an .xapk or .apkm) were not all copied: the copy was stopped
    /// or failed, and installing the same file again copies them.
    case installedWithoutData
    /// The app is installed, but some of the archive's data is of a kind that
    /// is not copied (it names no OBB file of the app, or no package): installing
    /// the file again would not help.
    case installedDataNotPlaced
}

/// How the install of Settings' Install APK ended, with what the card adds to
/// say it: after Install anyway of an app built for an Android before 4.0 that
/// Old app compatibility lists, where its switch is (`OldAppsWords.installHint`).
nonisolated struct ApkInstallResult: Sendable {
    var outcome: ApkInstallOutcome
    var oldAppHint = false
}

/// How the install flow ended, for the model: the outcome, and the listing of
/// old apps taken before an Install anyway of an app built for an Android
/// before 4.0 (nil for any other install).
nonisolated struct LegacyInstallEnd: Sendable {
    var outcome: ApkInstallOutcome
    var oldAppsBefore: [OldApp]?
}

extension ApkInstallOutcome {
    /// What the install's card says, and how it presents it. An app that is in
    /// with some of its data not copied is a warning, with a title of its own, so
    /// that it is not taken for a plain success. `oldAppHint` adds the two
    /// sentences that say where Old app compatibility is.
    func words(fileName name: String, oldAppHint: Bool = false) -> BusyWords {
        let words = baseWords(fileName: name)
        guard oldAppHint else { return words }
        return BusyWords(title: words.title, detail: Sentences.join(words.detail, OldAppsWords.installHint), style: words.style)
    }

    private func baseWords(fileName name: String) -> BusyWords {
        switch self {
        case .installed:
            BusyWords(title: String(localized: "App installed"), detail: String(localized: "\(name) is installed in the virtual machine."))
        case .installedWithoutData:
            BusyWords(
                title: Self.warningTitle,
                detail: String(localized: "\(name) is installed in the virtual machine, but not all of its data files were copied. The app may not work without them. Install the file again to copy them."),
                style: .warning)
        case .installedDataNotPlaced:
            BusyWords(
                title: Self.warningTitle,
                detail: String(localized: "\(name) is installed in the virtual machine, but not all of its data files were copied. The app may not work without them."),
                style: .warning)
        }
    }

    /// The title of the card of an install whose app is in but whose data is not all there.
    static var warningTitle: String { String(localized: "App installed, some data missing") }
}

/// The package manager refused an app for the version of Android it was built
/// for, and the first attempt is over: Settings' Install APK asks the person
/// before it sends the file again. Carries the ordinary failure words, so a
/// caller that does not ask shows what it always showed.
nonisolated struct LegacyApkRefused: LocalizedError, Equatable {
    var target: LegacyTarget

    var errorDescription: String? { PackageInstallMessages.message(forCode: PackageInstallAnswer.tooOldCode) }
}

/// The question Settings' Install APK asks before it installs an app built for
/// a much older version of Android. It says why Android blocks such an app and
/// that the app may not work; it promises nothing and shows no code.
nonisolated enum LegacyInstallWords {
    static let title = String(localized: "Install this app anyway?")

    static let install = String(localized: "Install anyway")

    /// The message, with the versions when the package manager named both.
    static func message(for target: LegacyTarget) -> String {
        let body = String(
            localized: "This app was built for a much older version of Android. Android blocks apps like this because they predate its newer privacy and security protections. If you install it anyway, it may not work properly.")
        guard let app = target.appLevel, let required = target.requiredLevel else { return body }
        let detail = String(
            localized: "This app was built for API level \(app). Android in this virtual machine normally installs only apps built for API level \(required) or later.",
            comment: "API level is Android's number for a version; both are plain numbers (7, 24).")
        return body + "\n\n" + detail
    }
}
