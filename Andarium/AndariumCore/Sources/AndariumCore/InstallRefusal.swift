// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import UniformTypeIdentifiers

/// A file Install APK turns away before anything is sent to the guest. The
/// words are the ones the screen shows.
public enum InstallRefusal: Error, Equatable, Sendable {
    /// The file is not an app package.
    case notApk
    /// The file is a single .apk of `singleApkLimit` bytes or more.
    case tooLarge
    /// The file, or an entry of it, is encrypted or password-protected.
    case encrypted
    /// The file is an app bundle for publishing, which is not installable.
    case appBundle
    /// The archive holds no app.
    case noApp
    /// The archive is damaged, or its parts do not fit together.
    case damaged
    /// The app has code for no processor this virtual machine has.
    case noMatchingProcessor
    /// The file is not a ZIP and is named as one kind of app file that can be
    /// encrypted (.apkm): it is damaged, or it is the encrypted kind.
    case damagedOrEncrypted
    /// One APK of an app in parts is `singleApkLimit` bytes or more.
    case partTooLarge

    /// From this size a single .apk is a ZIP64 archive, which Android's
    /// package parser cannot read.
    public static let singleApkLimit: UInt64 = 4 * Bytes.gib

    /// The extensions the install takes. An .apk is installed as it always
    /// was; the others are archives of an app's parts, taken by what they
    /// hold, never by the extension.
    public static let acceptedExtensions = ["apk", "apks", "xapk", "apkm"]

    /// nil for a file the install takes: one that ends in .apk, .apks, .xapk
    /// or .apkm. A .zip is not taken, whatever it holds: people keep all kinds
    /// of archives that are not apps.
    public init?(fileName: String) {
        let name = fileName.lowercased()
        if Self.acceptedExtensions.contains(where: { name.hasSuffix("." + $0) }) { return nil }
        self = .notApk
    }

    /// Whether the file is named as a plain APK, which the install streams
    /// whole as it always did; the other names the picker offers are archives
    /// of an app's parts, read before anything is sent.
    public static func isPlainApk(fileName: String) -> Bool {
        fileName.lowercased().hasSuffix(".apk")
    }

    /// nil for a file whose size Android takes.
    public init?(sizeBytes: UInt64) {
        guard sizeBytes >= Self.singleApkLimit else { return nil }
        self = .tooLarge
    }

    public var message: String {
        switch self {
        case .notApk:
            String(localized: "This isn\u{2019}t an app. Choose a file that ends in .apk, .apks, .xapk or .apkm.", bundle: .module)
        case .tooLarge:
            String(
                localized: "Android can\u{2019}t install an app from a single .apk file of \(Formats.size(bytes: Self.singleApkLimit)) or more.",
                bundle: .module)
        case .encrypted:
            String(localized: "This file is encrypted, so it can\u{2019}t be installed. Choose an app file that isn\u{2019}t encrypted.", bundle: .module)
        case .appBundle:
            String(
                localized: "This is an app bundle for publishing apps, which can\u{2019}t be installed. Choose an .apk, .apks, .xapk or .apkm file.",
                bundle: .module)
        case .noApp:
            String(localized: "There\u{2019}s no app in this file. Choose an .apk, .apks, .xapk or .apkm file that holds an app.", bundle: .module)
        case .damaged:
            String(
                localized: "This file is damaged, or the parts of the app don\u{2019}t fit together. Download it again, or choose another file.",
                bundle: .module)
        case .noMatchingProcessor:
            String(
                localized: "This app has no code for this device\u{2019}s processor. Choose a version of the app made for ARM processors.",
                bundle: .module)
        case .damagedOrEncrypted:
            String(
                localized: "This file is damaged or encrypted, so it can\u{2019}t be installed. Download it again, or choose another file.",
                bundle: .module)
        case .partTooLarge:
            String(
                localized: "Android can\u{2019}t install an app with a part of \(Formats.size(bytes: Self.singleApkLimit)) or more.",
                bundle: .module)
        }
    }
}

extension InstallRefusal {
    /// The types the file picker offers: what the system calls a file that
    /// ends in .apk, .apks, .xapk or .apkm. No Info.plist declaration is
    /// needed. With none, each type is the dynamic one every file of that
    /// extension also gets; if the system or another application declares an
    /// extension, the call returns that type and the files carry it too, so
    /// the picker and the files always agree. Other extensions (.zip, none)
    /// are dimmed, and `init(fileName:)` still refuses a mislabelled file.
    public static var pickerTypes: [UTType] {
        // A type the system cannot make is left out: `.data` in its place would
        // let every file through the picker.
        acceptedExtensions.compactMap { UTType(filenameExtension: $0, conformingTo: .data) }
    }
}
