// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// What Install APK makes of the archives that hold an app in parts: the
/// layouts of the .apks, .xapk and .apkm files, the choice of the parts this
/// virtual machine installs, and the refusals. The APKs in these archives are
/// opaque bytes, as they are to the install.
struct ApkContainerTests {
    // MARK: Fixtures

    private func apk(_ label: String, size: Int = 64) -> Data {
        var data = Data("PK-apk:\(label):".utf8)
        data.append(Data(repeating: 0x41, count: max(0, size - data.count)))
        return data
    }

    private func item(_ name: String, _ data: Data? = nil, method: UInt16 = 0) -> ZipFixture.Item {
        .init(name, data ?? apk(name), method: method)
    }

    private func inspect(_ items: [ZipFixture.Item], fileName: String = "app.apks") throws -> ApkContainer.Inspection {
        try ApkContainer.inspect(ZipDataSource(ZipFixture(items).build()), fileName: fileName)
    }

    private func plan(_ items: [ZipFixture.Item], fileName: String = "app.apks") throws -> ApkContainerPlan {
        guard case let .parts(plan) = try inspect(items, fileName: fileName) else {
            Issue.record("not an archive of parts")
            throw InstallRefusal.damaged
        }
        return plan
    }

    private func refusal(_ items: [ZipFixture.Item], fileName: String = "app.apks") -> InstallRefusal? {
        do {
            _ = try inspect(items, fileName: fileName)
            return nil
        } catch let refusal as InstallRefusal {
            return refusal
        } catch {
            Issue.record("\(error)")
            return nil
        }
    }

    private func json(_ object: Any, name: String = "manifest.json") throws -> ZipFixture.Item {
        .init(name, try JSONSerialization.data(withJSONObject: object), method: 8)
    }

    private func entries(_ plan: ApkContainerPlan) -> [String] { plan.candidates.map(\.entry) }

    // MARK: A plain APK

    @Test func anApkUnderAnotherNameInstallsAsAnApk() throws {
        let items = [item("AndroidManifest.xml"), item("classes.dex"), item("resources.arsc"), item("lib/arm64-v8a/liba.so")]
        for name in ["app.apks", "app.xapk", "app.apkm", "app.zip"] {
            #expect(try inspect(items, fileName: name) == .singleApk, "\(name)")
        }
    }

    @Test func anApkThatHoldsApksAsAssetsIsStillTheApkItLooksLike() throws {
        // The outer file is an app, and what it carries as assets is not an instruction to install it.
        let items = [
            item("AndroidManifest.xml"), item("classes.dex"), item("assets/helper.apk", apk("inner", size: 500)),
            item("assets/base.apk"), item("assets/split_config.en.apk"),
        ]
        for name in ["app.apks", "app.xapk", "app.apkm"] {
            #expect(try inspect(items, fileName: name) == .singleApk, "\(name)")
        }
        // Even with the files a container has beside it.
        #expect(try inspect(items + [item("info.json", Data("{}".utf8))], fileName: "app.apkm") == .singleApk)
        #expect(try inspect(items + [item("manifest.json", Data("{}".utf8))], fileName: "app.xapk") == .singleApk)
    }

    // MARK: bundletool

    @Test func aBundletoolFileWithNoTableIsReadAsTheArchiveItIs() throws {
        // `toc.pb` is what makes it bundletool's; without it the APKs are side by side.
        let plan = try plan([item("base.apk"), item("split_config.en.apk"), item("splits/")])
        #expect(plan.kind == .archive && plan.variants.isEmpty)
    }

    @Test func aBundletoolFileWithAnUnreadableTableIsDamagedNotAStandalone() {
        #expect(refusal([item("toc.pb"), item("splits/base-arm64_v8a.apk"), item("standalones/standalone.apk")]) == .damaged)
        #expect(refusal([item("toc.pb"), item("universal.apk")]) == .damaged)
    }

    // MARK: The processor

    private func flatSplits(_ abis: [String]) throws -> ApkContainerPlan {
        try plan(
            [item("base.apk"), item("split_config.xxhdpi.apk"), item("split_config.en.apk")]
                + abis.map { item("split_config.\($0).apk") }, fileName: "app.apkm")
    }

    @Test func the64BitSplitIsKeptAloneWhenTheAppHasIt() throws {
        let kept = try flatSplits(["arm64_v8a", "armeabi_v7a", "x86", "x86_64"]).selecting(guestAbis: ["arm64-v8a", "armeabi-v7a", "armeabi"])
        #expect(Set(entries(kept)) == ["base.apk", "split_config.xxhdpi.apk", "split_config.en.apk", "split_config.arm64_v8a.apk"])
    }

    @Test func the32BitSplitIsKeptWhenThereIsNo64BitOne() throws {
        let kept = try flatSplits(["armeabi_v7a", "x86"]).selecting(guestAbis: ["arm64-v8a", "armeabi-v7a", "armeabi"])
        #expect(Set(entries(kept)) == ["base.apk", "split_config.xxhdpi.apk", "split_config.en.apk", "split_config.armeabi_v7a.apk"])
        let old = try flatSplits(["armeabi", "x86_64"]).selecting(guestAbis: ["arm64-v8a", "armeabi-v7a", "armeabi"])
        #expect(entries(old).contains("split_config.armeabi.apk") && !entries(old).contains("split_config.x86_64.apk"))
    }

    @Test func aGuestThatListsOnly64BitsKeepsNo32BitSplit() throws {
        #expect(throws: InstallRefusal.noMatchingProcessor) {
            _ = try flatSplits(["armeabi_v7a"]).selecting(guestAbis: ["arm64-v8a"])
        }
    }

    @Test func anAppWithCodeOnlyForOtherProcessorsHasNoMatch() throws {
        let plan = try flatSplits(["x86", "x86_64"])
        #expect(throws: InstallRefusal.noMatchingProcessor) { _ = try plan.selecting(guestAbis: ApkContainer.defaultGuestAbis) }
    }

    @Test func theBaseIsKeptWhateverItsFileIsCalled() throws {
        // A base that looks like a processor split is still the base: it has no processor of its own.
        for name in ["armeabi-v7a.apk", "arm64_v8a.apk", "x86.apk", "x86_64.apk"] {
            let found = try plan([item(name), item("split_config.arm64_v8a.apk")], fileName: "app.apkm")
            let chosen = try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"])
            #expect(chosen.candidates.first?.entry == name, "\(name)")
            #expect(chosen.candidates.first?.splitName == nil && chosen.candidates.first?.abi == nil, "\(name)")
            #expect(chosen.candidates.count == 2, "\(name)")
        }
        // Nor can a plan built by hand lose it to the filter.
        var forged = try flatSplits(["arm64_v8a", "x86"])
        forged.candidates[0].abi = "x86"
        #expect(throws: InstallRefusal.damaged) { _ = try forged.selecting(guestAbis: ["arm64-v8a"]) }
        var headless = try flatSplits(["arm64_v8a"])
        headless.candidates.removeFirst()
        #expect(throws: InstallRefusal.damaged) { _ = try headless.selecting(guestAbis: ["arm64-v8a"]) }
    }

    @Test func anAppWithNoProcessorSplitsKeepsEverything() throws {
        let plan = try flatSplits([])
        #expect(try plan.selecting(guestAbis: ApkContainer.defaultGuestAbis) == plan)
        #expect(plan.candidates.count == 3)
    }

    @Test func processorNamesAreReadInEverySpelling() {
        #expect(ApkContainer.abi(ofConfig: "arm64_v8a") == "arm64-v8a")
        #expect(ApkContainer.abi(ofConfig: "config.arm64_v8a") == "arm64-v8a")
        #expect(ApkContainer.abi(ofConfig: "config.arm64-v8a") == "arm64-v8a")
        #expect(ApkContainer.abi(ofConfig: "feature.config.armeabi_v7a") == "armeabi-v7a")
        #expect(ApkContainer.abi(ofConfig: "config_x86_64") == "x86_64")
        #expect(ApkContainer.abi(ofConfig: "CONFIG.X86") == "x86")
        for name in ["config.xxhdpi", "config.en", "master", "feature", "config.tcf_astc", "armeabi_v7a_extra", "arm", ""] {
            #expect(ApkContainer.abi(ofConfig: name) == nil, "\(name)")
        }
    }

    // MARK: The other layouts

    @Test func anInstallerAppsExportIsRead() throws {
        let plan = try plan([
            item("meta.sai_v2.json", Data("{}".utf8)), item("icon.png"), item("base.apk", apk("base", size: 900)),
            item("split_config.arm64_v8a.apk"), item("split_config.xxhdpi.apk"), item("split_config.fr.apk"),
        ])
        #expect(plan.kind == .sai)
        #expect(plan.candidates.map(\.splitName) == [nil, "config.arm64_v8a", "config.xxhdpi", "config.fr"])
        #expect(plan.candidates.map(\.name).first == "base.apk")
        #expect(plan.candidates.first?.bytes == 900)
    }

    @Test func anApkmFileIsRead() throws {
        let plan = try plan([
            item("info.json", Data(#"{"apkm_version":6,"pname":"com.example.app"}"#.utf8)), item("icon.png"),
            item("split_config.arm64_v8a.apk"), item("base.apk"), item("split_config.xhdpi.apk"), item("split_feature_extra.apk"),
        ], fileName: "App.apkm")
        #expect(plan.kind == .apkm)
        #expect(plan.candidates.first?.entry == "base.apk")
        #expect(Set(plan.candidates.compactMap(\.splitName)) == ["config.arm64_v8a", "config.xhdpi", "feature_extra"])
    }

    @Test func anApkmFilesDataIsPlacedLikeAnXapks() throws {
        let obb = "main.184.com.example.app.obb"
        let found = try plan([
            item("info.json", Data(#"{"apkm_version":5,"pname":"com.example.app","obb_files":["main.184.com.example.app.obb"]}"#.utf8)),
            item("base.apk", apk("base", size: 400)), item("split_config.en.apk"), item(obb, apk("obb", size: 70_000)),
        ], fileName: "App.apkm")
        #expect(found.kind == .apkm && found.packageName == "com.example.app")
        #expect(found.data == [ApkContainerPlan.DataFile(entry: obb, bytes: 70_000, folder: "Android/obb/com.example.app", name: obb)])
        #expect(found.dataBytes == 70_000 && found.unplacedData == 0)
        let place = try #require(found.guestLocation(of: found.data[0]))
        #expect(place.path == "/storage/emulated/0/Android/obb/com.example.app/" + obb)
        // An OBB the info does not list is not data.
        let unlisted = try plan([
            item("info.json", Data(#"{"pname":"com.example.app"}"#.utf8)), item("base.apk"), item("x.obb", apk("obb")),
        ], fileName: "App.apkm")
        #expect(unlisted.data.isEmpty && unlisted.unplacedData == 0)
    }

    @Test func anApkmFilesDataThatCannotBePlacedIsCounted() throws {
        func found(_ info: String, _ extra: [ZipFixture.Item] = []) throws -> ApkContainerPlan {
            try plan([item("info.json", Data(info.utf8)), item("base.apk")] + extra, fileName: "App.apkm")
        }
        // No package, a name that is not an OBB file's, a folder: nothing is placed, and it is said.
        let noPackage = try found(#"{"obb_files":["a.obb"]}"#, [item("a.obb")])
        #expect(noPackage.data.isEmpty && noPackage.unplacedData == 1 && noPackage.packageName == nil)
        for name in ["a.txt", ".a.obb", "a b.obb", "sub/a.obb", "a'b.obb", ".obb"] {
            let odd = try found(#"{"pname":"com.example.app","obb_files":[\#(try jsonString(name))]}"#, [item(name)])
            #expect(odd.data.isEmpty && odd.unplacedData == 1, "\(name.debugDescription)")
        }
    }

    private func jsonString(_ text: String) throws -> String {
        String(decoding: try JSONSerialization.data(withJSONObject: [text]), as: UTF8.self).dropFirst().dropLast().description
    }

    @Test func anApkmInfoThatCannotBeTrustedIsDamaged() throws {
        func refused(_ info: Data) -> InstallRefusal? {
            refusal([item("info.json", info), item("base.apk"), item("a.obb")], fileName: "App.apkm")
        }
        #expect(refused(Data("not json".utf8)) == .damaged)
        #expect(refused(Data("[1]".utf8)) == .damaged)
        #expect(refused(Data(#"{"pname":"../x","obb_files":["a.obb"]}"#.utf8)) == .damaged)
        #expect(refused(Data(#"{"pname":"com.example.app","obb_files":"a.obb"}"#.utf8)) == .damaged)
        #expect(refused(Data(#"{"pname":"com.example.app","obb_files":[1]}"#.utf8)) == .damaged)
        #expect(refused(Data(#"{"pname":"com.example.app","obb_files":["gone.obb"]}"#.utf8)) == .damaged)
        #expect(refused(Data(#"{"pname":"com.example.app","obb_files":["a.obb","a.obb"]}"#.utf8)) == .damaged)
        #expect(refused(Data(#"{"pname":"com.example.app","obb_files":["a.obb"]}"#.utf8)) == nil)
        let manyNames = String(repeating: #""a.obb","#, count: ApkContainer.maxParts + 1) + #""a.obb""#
        #expect(refused(Data(#"{"pname":"com.example.app","obb_files":[\#(manyNames)]}"#.utf8)) == .damaged)
        // Over the cap, like an .xapk's manifest.
        #expect(refused(Data(String(repeating: " ", count: ApkContainer.maxManifestBytes + 1).utf8)) == .damaged)
    }

    @Test func aFolderOfApksIsReadWhenItIsTheOnlyOne() throws {
        let one = try plan([item("My App/base.apk"), item("My App/split_config.en.apk"), item("My App/notes.txt")])
        #expect(one.kind == .archive && entries(one).first == "My App/base.apk" && one.candidates.count == 2)
        #expect(refusal([item("a/base.apk"), item("b/base.apk")]) == .damaged)
        #expect(refusal([item("a/base.apk"), item("b/split_config.en.apk")]) == .damaged)
        // The root's files win over a folder's.
        let mixed = try plan([item("base.apk"), item("other/base.apk"), item("split_config.en.apk")])
        #expect(Set(entries(mixed)) == ["base.apk", "split_config.en.apk"])
    }

    @Test func theBaseIsTheOneApkThatIsNotNamedLikeASplit() throws {
        let plan = try plan([item("com.example.app.apk"), item("config.arm64_v8a.apk"), item("config.xxhdpi.apk")])
        #expect(plan.candidates.first?.entry == "com.example.app.apk")
        #expect(refusal([item("config.arm64_v8a.apk"), item("config.xxhdpi.apk")]) == .damaged)
        #expect(refusal([item("one.apk"), item("two.apk")]) == .damaged)
    }

    @Test func junkEntriesAreNotParts() throws {
        let plan = try plan([
            item("__MACOSX/._base.apk"), item("._base.apk"), item(".DS_Store"), item("base.apk"), item("split_config.en.apk"),
            item("lib/", Data(), method: 0),
        ])
        #expect(Set(entries(plan)) == ["base.apk", "split_config.en.apk"])
    }

    @Test func anArchiveInsideAnArchiveIsNotFollowed() {
        #expect(refusal([item("inner.zip", Data("PK-inner".utf8)), item("readme.txt")]) == .noApp)
        #expect(refusal([item("parts.apks"), item("parts.xapk")]) == .noApp)
    }

    // MARK: .xapk

    private func xapkManifest(
        package: String = "com.example.game", splits: [[String: String]]? = nil, expansions: [[String: Any]]? = nil
    ) throws -> ZipFixture.Item {
        var object: [String: Any] = ["xapk_version": 2, "package_name": package, "name": "Game", "version_code": "12"]
        if let splits { object["split_apks"] = splits }
        if let expansions { object["expansions"] = expansions }
        return try json(object)
    }

    @Test func aVersionTwoXapkWithItsDataIsRead() throws {
        let obb = "Android/obb/com.example.game/main.12.com.example.game.obb"
        let plan = try plan([
            xapkManifest(
                splits: [
                    ["file": "com.example.game.apk", "id": "base"], ["file": "config.arm64_v8a.apk", "id": "config.arm64_v8a"],
                    ["file": "config.xxhdpi.apk", "id": "config.xxhdpi"],
                ],
                expansions: [["file": obb, "install_location": "EXTERNAL_STORAGE", "install_path": obb]]),
            item("icon.png"), item("com.example.game.apk", apk("base", size: 400)), item("config.arm64_v8a.apk"), item("config.xxhdpi.apk"),
            item(obb, apk("obb", size: 100_000)),
        ], fileName: "Game.xapk")
        #expect(plan.kind == .xapk && plan.packageName == "com.example.game")
        #expect(plan.candidates.map(\.entry) == ["com.example.game.apk", "config.arm64_v8a.apk", "config.xxhdpi.apk"])
        #expect(plan.candidates.map(\.splitName) == [nil, "config.arm64_v8a", "config.xxhdpi"])
        #expect(plan.data == [ApkContainerPlan.DataFile(entry: obb, bytes: 100_000, folder: "Android/obb/com.example.game", name: "main.12.com.example.game.obb")])
        #expect(plan.dataBytes == 100_000 && plan.unplacedData == 0)
    }

    @Test func aVersionOneXapkIsOneApkAndItsObbFound() throws {
        let obb = "Android/obb/com.example.game/main.3.com.example.game.obb"
        let patch = "Android/obb/com.example.game/patch.3.com.example.game.obb"
        let plan = try plan([
            json(["xapk_version": 1, "package_name": "com.example.game"]), item("com.example.game.apk"),
            item(obb, apk("obb", size: 70_000)), item(patch, apk("patch", size: 30_000)),
            item("Android/data/com.example.game/files/cache.bin", apk("data", size: 10)),
        ], fileName: "Game.xapk")
        #expect(plan.candidates.count == 1 && plan.candidates[0].entry == "com.example.game.apk")
        #expect(Set(plan.data.map(\.name)) == ["main.3.com.example.game.obb", "patch.3.com.example.game.obb"])
        #expect(plan.dataBytes == 100_000)
        #expect(plan.unplacedData == 1)
    }

    @Test func dataThatIsNotAnObbOfThePackageIsNotPlaced() throws {
        let entry = "x.obb"
        for destination in [
            "Android/obb/other.package/main.1.obb", "Android/obb/com.example.game/sub/main.1.obb", "Android/obb/com.example.game/..",
            "Android/obb/com.example.game/.hidden.obb", "Android/obb/com.example.game/.andarium-00000000-x.part",
            "Android/obb/com.example.game/main.1.txt", "Android/obb/com.example.game/", "Android/data/com.example.game/main.1.obb",
            "/Android/obb/com.example.game/main.1.obb", "../Android/obb/com.example.game/main.1.obb", "Android/obb/com.example.game/a\u{1}b.obb",
            "Android/obb/com.example.game/.obb", "Android/obb/com.example.game/main\\1.obb", "Android/obb//main.1.obb",
        ] {
            let plan = try plan([
                xapkManifest(expansions: [["file": entry, "install_location": "EXTERNAL_STORAGE", "install_path": destination]]),
                item("com.example.game.apk"), item(entry, apk("obb")),
            ])
            #expect(plan.data.isEmpty && plan.unplacedData == 1, "\(destination.debugDescription)")
        }
        // A location other than external storage is not placed either.
        let other = try plan([
            xapkManifest(expansions: [["file": entry, "install_location": "INTERNAL", "install_path": "Android/obb/com.example.game/main.1.obb"]]),
            item("com.example.game.apk"), item(entry),
        ])
        #expect(other.data.isEmpty && other.unplacedData == 1)
    }

    @Test func aDataFileTheArchiveDoesNotHoldOrNamesTwiceIsDamaged() throws {
        let destination = "Android/obb/com.example.game/main.1.com.example.game.obb"
        #expect(refusal([
            try xapkManifest(expansions: [["file": "missing.obb", "install_path": destination]]), item("com.example.game.apk"),
        ]) == .damaged)
        #expect(refusal([
            try xapkManifest(expansions: [["file": "a.obb", "install_path": destination], ["file": "b.obb", "install_path": destination]]),
            item("com.example.game.apk"), item("a.obb"), item("b.obb"),
        ]) == .damaged)
        #expect(refusal([try xapkManifest(expansions: [["install_path": destination]]), item("com.example.game.apk")]) == .damaged)
    }

    @Test func anXapkManifestThatCannotBeTrustedIsDamaged() throws {
        // Not JSON, not an object, a package name that is a path, a split that is not there, two bases, none.
        #expect(refusal([item("manifest.json", Data("not json".utf8)), item("a.apk")]) == .damaged)
        #expect(refusal([item("manifest.json", Data("[1,2]".utf8)), item("a.apk")]) == .damaged)
        for package in ["../x", "com", "com.example/..", "com..x", "1com.example", "com.exa mple", ""] {
            #expect(refusal([try xapkManifest(package: package), item("a.apk")]) == .damaged, "\(package)")
        }
        #expect(refusal([
            try xapkManifest(splits: [["file": "gone.apk", "id": "base"]]), item("a.apk"),
        ]) == .damaged)
        #expect(refusal([
            try xapkManifest(splits: [["file": "a.apk", "id": "base"], ["file": "b.apk", "id": "base"]]), item("a.apk"), item("b.apk"),
        ]) == .damaged)
        #expect(refusal([
            try xapkManifest(splits: [["file": "a.apk", "id": "config.en"]]), item("a.apk"),
        ]) == .damaged)
        #expect(refusal([try xapkManifest(), item("a.apk"), item("b.apk")]) == .damaged)
    }

    @Test func aManifestOverTheCapIsNotRead() throws {
        var manifest = ZipFixture.Item("manifest.json", Data(String(repeating: " ", count: ApkContainer.maxManifestBytes + 1).utf8), method: 8)
        #expect(refusal([manifest, item("a.apk")]) == .damaged)
        manifest.data = Data(#"{"package_name":"com.example.game"}"#.utf8) + Data(String(repeating: " ", count: 1000).utf8)
        #expect(try plan([manifest, item("a.apk")]).packageName == "com.example.game")
    }

    // MARK: Refusals

    @Test func anAppBundleIsRefusedWithItsOwnWords() {
        #expect(refusal([item("BundleConfig.pb"), item("base/manifest/AndroidManifest.xml"), item("base/dex/classes.dex")], fileName: "app.aab") == .appBundle)
        #expect(refusal([item("base/manifest/AndroidManifest.xml")], fileName: "app.apks") == .appBundle)
    }

    @Test func anArchiveWithNoAppIsRefused() {
        #expect(refusal([item("photos/a.jpg"), item("notes.txt")], fileName: "Trip photos.apks") == .noApp)
        #expect(refusal([item("readme.txt")], fileName: "x.xapk") == .noApp)
    }

    @Test func anEncryptedArchiveIsRefused() throws {
        var secret = ZipFixture.Item("base.apk", apk("base"), method: 0)
        secret.flags = 0x0001
        #expect(refusal([secret, item("split_config.en.apk")]) == .encrypted)
        var aes = ZipFixture.Item("base.apk", apk("base"), method: 99)
        aes.flags = 0x0001
        #expect(refusal([aes]) == .encrypted)
    }

    @Test func aFileThatIsNotAZipIsWordedByWhatItCouldBe() throws {
        // Not a ZIP at all: the encrypted kind of an .apkm may be that; any other
        // name an app in parts has is damaged. A saved web page is no more an app than noise is.
        for noise in [incompressible(5000), Data("<html><body>Please sign in</body></html>".utf8) + Data(repeating: 0x20, count: 200)] {
            for (name, expected) in [
                ("a.apkm", InstallRefusal.damagedOrEncrypted), ("A.APKM", .damagedOrEncrypted), ("a.apks", .damaged),
                ("a.xapk", .damaged), ("a.XAPK", .damaged), ("a.zip", .notApk),
            ] {
                do {
                    _ = try ApkContainer.inspect(ZipDataSource(noise), fileName: name)
                    Issue.record("\(name): no error")
                } catch {
                    #expect(error as? InstallRefusal == expected, "\(name)")
                }
            }
        }
        // A file too short to be an archive at all.
        for name in ["a.apkm", "a.apks", "a.xapk"] {
            do {
                _ = try ApkContainer.inspect(ZipDataSource(Data()), fileName: name)
                Issue.record("\(name): no error")
            } catch {
                #expect(error as? InstallRefusal == (name == "a.apkm" ? .damagedOrEncrypted : .damaged), "\(name)")
            }
        }
    }

    @Test func aDownloadThatStoppedEarlyIsDamagedUnderEveryName() throws {
        // It starts like a ZIP and has no end: the cut-off file, not an encrypted one.
        let whole = ZipFixture([item("base.apk", apk("base", size: 20_000)), item("split_config.en.apk", apk("en", size: 20_000))]).build()
        for cut in [4, 30, 1000, whole.count / 2, whole.count - 30, whole.count - 1] {
            for name in ["a.apks", "a.xapk", "a.apkm", "a.APKM"] {
                do {
                    _ = try ApkContainer.inspect(ZipDataSource(whole.prefix(cut)), fileName: name)
                    Issue.record("\(name) at \(cut): no error")
                } catch {
                    #expect(error as? InstallRefusal == .damaged, "\(name) at \(cut)")
                }
            }
        }
        // The whole of it reads.
        #expect(refusal([item("base.apk"), item("split_config.en.apk")], fileName: "a.apkm") == nil)
    }

    @Test func aDamagedArchiveIsRefusedWithItsOwnWords() {
        var overlap = ZipFixture.Item("b.apk", apk("b"), method: 0)
        overlap.headerOffset = 0
        #expect(refusal([item("base.apk"), overlap]) == .damaged)
        #expect(refusal([item("base.apk"), item("base.apk")]) == .damaged)
        #expect(refusal([item("base.apk"), item("../escape.apk")]) == .damaged)
        #expect(refusal([item("base.apk", Data())]) == .damaged)
    }

    @Test func anApkOf4GibOrMoreInsideIsRefusedWithTheSizeWords() {
        var big = ZipFixture.Item("base.apk", apk("base"), method: 8)
        big.zip64 = true
        big.declaredSize = InstallRefusal.singleApkLimit
        #expect(refusal([big, item("split_config.en.apk")]) == .partTooLarge)
        big.declaredSize = InstallRefusal.singleApkLimit - 1
        #expect(refusal([big, item("split_config.en.apk")]) == nil)
        var splitBig = ZipFixture.Item("split_config.en.apk", apk("en"), method: 8)
        splitBig.zip64 = true
        splitBig.declaredSize = 5 * Bytes.gib
        #expect(refusal([item("base.apk"), splitBig]) == .partTooLarge)
    }

    @Test func aContainerOfManyApksIsLimitedInTheParts() throws {
        let many = [item("base.apk")] + (0..<ApkContainer.maxParts).map { item("split_config.l\($0).apk") }
        #expect(refusal(many) == .damaged)
        let allowed = [item("base.apk")] + (0..<(ApkContainer.maxParts - 1)).map { item("split_config.l\($0).apk") }
        #expect(try plan(allowed).candidates.count == ApkContainer.maxParts)
    }

    // MARK: Names in the session

    @Test func everyPartHasAUniquePlainNameForTheSession() throws {
        let plan = try plan([
            item("base.apk"), item("split_config.arm64_v8a.apk"), item("split config \u{E9}.apk"), item("A/B/.. weird %name%.apk"),
            item("split_config.ja.apk"),
        ].filter { !$0.name.contains("/") })
        let names = plan.candidates.map(\.name)
        #expect(Set(names).count == names.count)
        for name in names { #expect(PackageInstallCommand.isValidPartName(name), "\(name)") }
        #expect(names.first == "base.apk")
    }

    @Test func theExtensionOfTheFileDoesNotDecideWhatItIs() throws {
        let items = [item("base.apk"), item("split_config.en.apk")]
        for name in ["a.apks", "a.xapk", "a.apkm", "a.zip", "a", "a.APKS"] {
            #expect(try plan(items, fileName: name).kind == .archive, "\(name)")
        }
    }

    // MARK: Where data goes

    @Test func aDataFileIsPlacedInTheGuestsObbFolderOfThePackageAndNowhereElse() throws {
        let obb = "Android/obb/com.example.game/main.12.com.example.game.obb"
        let plan = try plan([
            xapkManifest(splits: [["file": "com.example.game.apk", "id": "base"]], expansions: [["file": obb, "install_path": obb]]),
            item("com.example.game.apk"), item(obb, apk("obb", size: 1000)),
        ], fileName: "Game.xapk")
        let file = try #require(plan.data.first)
        let place = try #require(plan.guestLocation(of: file))
        #expect(place.folder == "/storage/emulated/0/Android/obb/com.example.game")
        #expect(place.path == place.folder + "/main.12.com.example.game.obb")
        // A plan built by hand cannot name another place.
        for (folder, name) in [
            ("Android/obb/other.package", "main.1.obb"), ("Android/data/com.example.game", "main.1.obb"),
            ("Android/obb/com.example.game/sub", "main.1.obb"), ("Android/obb/com.example.game", "../main.1.obb"),
            ("Android/obb/com.example.game", ".main.1.obb"), ("Android/obb/com.example.game", "main.1.txt"),
            ("Android/obb/../com.example.game", "main.1.obb"), ("/Android/obb/com.example.game", "main.1.obb"),
            ("Android/obb/com.example.game", "a'b.obb"),
        ] {
            let forged = ApkContainerPlan.DataFile(entry: obb, bytes: 1, folder: folder, name: name)
            #expect(plan.guestLocation(of: forged) == nil, "\(folder)/\(name)")
        }
        var unnamed = plan
        unnamed.packageName = nil
        #expect(unnamed.guestLocation(of: file) == nil)
    }
}
