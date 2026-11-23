// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
import UniformTypeIdentifiers

@testable import AndariumCore

struct InstallRefusalTests {
    @Test func theFourFormsOfAnAppAreTaken() {
        for name in ["Signal-7.21.4.apk", "A.APK", "my app.Apk", "app.apks", "Trip Planner.XAPK", "app.apkm", "App.ApkM", "a.b.apks"] {
            #expect(InstallRefusal(fileName: name) == nil, "\(name)")
        }
    }

    @Test func aFileThatIsNotAnAppIsTurnedAwayAndAZipNeverIsTaken() {
        // A zip is refused whatever it holds: people keep all kinds of archives.
        for name in ["Trip photos.zip", "app.zip", "base.apks.zip", "notes.txt", "apk", "apks", "app.apk.zip", "app.aab", "app.jar", ""] {
            #expect(InstallRefusal(fileName: name) == .notApk, "\(name)")
        }
        #expect(InstallRefusal.acceptedExtensions == ["apk", "apks", "xapk", "apkm"])
    }

    @Test func eachRefusalHasItsOwnWords() {
        #expect(InstallRefusal.notApk.message == "This isn\u{2019}t an app. Choose a file that ends in .apk, .apks, .xapk or .apkm.")
        #expect(InstallRefusal.encrypted.message == "This file is encrypted, so it can\u{2019}t be installed. Choose an app file that isn\u{2019}t encrypted.")
        #expect(InstallRefusal.appBundle.message
            == "This is an app bundle for publishing apps, which can\u{2019}t be installed. Choose an .apk, .apks, .xapk or .apkm file.")
        #expect(InstallRefusal.noApp.message
            == "There\u{2019}s no app in this file. Choose an .apk, .apks, .xapk or .apkm file that holds an app.")
        #expect(InstallRefusal.damaged.message
            == "This file is damaged, or the parts of the app don\u{2019}t fit together. Download it again, or choose another file.")
        #expect(InstallRefusal.noMatchingProcessor.message
            == "This app has no code for this device\u{2019}s processor. Choose a version of the app made for ARM processors.")
        #expect(InstallRefusal.damagedOrEncrypted.message
            == "This file is damaged or encrypted, so it can\u{2019}t be installed. Download it again, or choose another file.")
        #expect(InstallRefusal.partTooLarge.message == "Android can\u{2019}t install an app with a part of 4 GiB or more.")
        let all: [InstallRefusal] = [
            .notApk, .tooLarge, .encrypted, .appBundle, .noApp, .damaged, .noMatchingProcessor, .damagedOrEncrypted, .partTooLarge,
        ]
        #expect(Set(all.map(\.message)).count == all.count)
    }

    @Test func aPlainApkIsOneThatIsNamedLikeOne() {
        for name in ["a.apk", "A.APK", "x.y.Apk"] { #expect(InstallRefusal.isPlainApk(fileName: name), "\(name)") }
        for name in ["a.apks", "a.xapk", "a.apkm", "a.zip", "apk", "a.apk.zip", ""] { #expect(!InstallRefusal.isPlainApk(fileName: name), "\(name)") }
    }

    @Test func aSingleApkOfFourGibibytesOrMoreIsTurnedAwayWithItsOwnWords() {
        let limit = InstallRefusal.singleApkLimit
        #expect(limit == 4 * Bytes.gib)
        for size in [0, 1, 3 * Bytes.gib, limit - 1] {
            #expect(InstallRefusal(sizeBytes: size) == nil, "\(size)")
        }
        for size in [limit, limit + 1, 4_509_714_371, .max] {
            #expect(InstallRefusal(sizeBytes: size) == .tooLarge, "\(size)")
        }
        #expect(InstallRefusal.tooLarge.message == "Android can\u{2019}t install an app from a single .apk file of 4 GiB or more.")
    }

    /// The type the system gives a file name, as the picker's rows have it.
    private func type(of name: String) -> UTType? {
        UTType(filenameExtension: URL(fileURLWithPath: name).pathExtension)
    }

    private func picked(_ name: String) -> Bool {
        InstallRefusal.pickerTypes.contains { type(of: name)?.conforms(to: $0) == true }
    }

    @Test func thePickerOffersTheTypesOfTheFourForms() {
        let pickers = InstallRefusal.pickerTypes
        // One type for each extension: a type the system cannot make is not
        // replaced by `.data`, which would let every file through.
        #expect(pickers.count == InstallRefusal.acceptedExtensions.count)
        #expect(pickers.count == 4)
        // Not everything: an unfiltered picker is what these types replace.
        for picker in pickers { #expect(picker != .item && picker != .data && picker != .archive && picker != .zip) }
        for name in ["a.apk", "A.APK", "my app.Apk", "a.apks", "a.xapk", "a.XAPK", "a.apkm"] {
            #expect(picked(name), "\(name)")
        }
        // The system or an application that declares an extension changes
        // the identifier, never the agreement between the picker and the files.
        #expect(pickers == InstallRefusal.acceptedExtensions.map { UTType(filenameExtension: $0, conformingTo: .data) })
        for (type, ext) in zip(pickers, InstallRefusal.acceptedExtensions) {
            #expect(type.conforms(to: .data) && type != .data, "\(ext)")
            #expect(UTType(filenameExtension: ext, conformingTo: .data) == type, "\(ext)")
        }
    }

    @Test func thePickerDimsEveryOtherKindOfFile() {
        let pickers = InstallRefusal.pickerTypes
        for name in ["a.zip", "a.txt", "a.jar", "a.ipa", "a.json", "a.pdf", "a.png", "a.aab", "a.apk.zip"] {
            #expect(!picked(name), "\(name)")
        }
        // A file with no extension has no type to match, and the two plain
        // types conform to none of the four.
        #expect(type(of: "noextension") == nil)
        for picker in pickers { #expect(!UTType.data.conforms(to: picker) && !UTType.item.conforms(to: picker) && !UTType.zip.conforms(to: picker)) }
    }

    @Test func theFilterAndTheRefusalAgreeOnWhatTheInstallTakes() {
        for name in ["a.apk", "A.APK", "my app.Apk", "a.apks", "a.xapk", "a.apkm"] {
            #expect(InstallRefusal(fileName: name) == nil && picked(name), "\(name)")
        }
        for name in ["a.zip", "a.apk.zip", "a.txt"] {
            #expect(InstallRefusal(fileName: name) == .notApk && !picked(name), "\(name)")
        }
    }
}
