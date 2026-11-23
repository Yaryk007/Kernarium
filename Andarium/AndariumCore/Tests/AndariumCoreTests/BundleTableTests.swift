// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A bundletool `.apks` is read through its table of contents: the variants of
/// the app it holds, one of which is installed for the guest's Android, and
/// nothing is guessed from the names of the APKs.
struct BundleTableTests {
    private typealias Apk = BundleTocFixture.Apk
    private typealias Variant = BundleTocFixture.Variant

    private func entry(_ name: String, size: Int = 64) -> ZipFixture.Item {
        var data = Data("PK-apk:\(name):".utf8)
        data.append(Data(repeating: 0x41, count: max(0, size - data.count)))
        return .init(name, data, method: 0)
    }

    private func archive(_ toc: BundleTocFixture, extra: [String] = []) -> [ZipFixture.Item] {
        var paths: [String] = []
        for variant in toc.variants {
            for apk in variant.apks where !paths.contains(apk.path) { paths.append(apk.path) }
        }
        return [.init("toc.pb", toc.build(), method: 8)] + (paths + extra).map { entry($0) }
    }

    private func plan(_ items: [ZipFixture.Item]) throws -> ApkContainerPlan {
        guard case let .parts(plan) = try ApkContainer.inspect(ZipDataSource(ZipFixture(items).build()), fileName: "app.apks") else {
            Issue.record("not an archive of parts")
            throw InstallRefusal.damaged
        }
        return plan
    }

    private func refusal(_ items: [ZipFixture.Item]) -> InstallRefusal? {
        do {
            _ = try ApkContainer.inspect(ZipDataSource(ZipFixture(items).build()), fileName: "app.apks")
            return nil
        } catch let refusal as InstallRefusal {
            return refusal
        } catch {
            Issue.record("\(error)")
            return nil
        }
    }

    /// The bundle of a real build: the same splits for every variant but the
    /// master and the processor splits, which two of the variants share.
    private func release() -> BundleTocFixture {
        let shared: [Apk] = [
            .split("splits/base-en.apk", "config.en"), .split("splits/base-ja.apk", "config.ja"),
            .split("splits/base-xxhdpi.apk", "config.xxhdpi"), .split("splits/base-hdpi.apk", "config.hdpi"),
        ]
        func abis(_ suffix: String) -> [Apk] {
            [
                .split("splits/base-arm64_v8a\(suffix).apk", "config.arm64_v8a", abi: 3),
                .split("splits/base-armeabi_v7a\(suffix).apk", "config.armeabi_v7a", abi: 2),
                .split("splits/base-x86\(suffix).apk", "config.x86", abi: 4),
                .split("splits/base-x86_64\(suffix).apk", "config.x86_64", abi: 5),
            ]
        }
        return BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [.master("splits/base-master.apk")] + abis("") + shared),
            Variant(minSdk: 23, apks: [.master("splits/base-master_2.apk")] + abis("_2") + shared),
            Variant(minSdk: 31, apks: [.master("splits/base-master_3.apk")] + abis("_2") + shared),
        ])
    }

    private func entries(_ plan: ApkContainerPlan) -> Set<String> { Set(plan.candidates.map(\.entry)) }

    private typealias Module = BundleTocFixture.AssetModule
    private typealias Slice = BundleTocFixture.AssetSlice

    /// A bundle of one split variant with the asset modules `modules` and the APKs they name.
    private func bundle(_ modules: [Module], variants: [Variant]? = nil, missing: Set<String> = []) -> [ZipFixture.Item] {
        let toc = BundleTocFixture(
            variants: variants ?? [Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en")])],
            assetModules: modules)
        var extra: [String] = []
        for path in modules.flatMap({ $0.slices.map(\.path) }) where !missing.contains(path) && !extra.contains(path) { extra.append(path) }
        return archive(toc, extra: extra)
    }

    private let pack: [Slice] = [
        .slice("asset-slices/levels-master.apk", "levels", master: true),
        .slice("asset-slices/levels-config.tcf_astc.apk", "levels.config.tcf_astc"),
    ]

    // MARK: One variant

    @Test func theVariantForTheGuestsAndroidIsTheOneInstalled() throws {
        let found = try plan(archive(release()))
        #expect(found.kind == .bundletool && found.variants.count == 3)
        let arm = ["arm64-v8a", "armeabi-v7a", "armeabi"]
        // Android 17 (API 37): the newest variant, with its own master and the processor splits it shares with the middle one.
        let newest = try found.selecting(guestAbis: arm, guestSdk: 37)
        #expect(entries(newest) == [
            "splits/base-master_3.apk", "splits/base-arm64_v8a_2.apk", "splits/base-en.apk", "splits/base-ja.apk",
            "splits/base-xxhdpi.apk", "splits/base-hdpi.apk",
        ])
        #expect(newest.candidates.first?.entry == "splits/base-master_3.apk" && newest.candidates.first?.name == "base.apk")
        #expect(newest.candidates.first?.splitName == nil)
        #expect(newest.variants.isEmpty)
        let middle = try found.selecting(guestAbis: arm, guestSdk: 30)
        #expect(middle.candidates.first?.entry == "splits/base-master_2.apk" && entries(middle).contains("splits/base-arm64_v8a_2.apk"))
        let oldest = try found.selecting(guestAbis: arm, guestSdk: 22)
        #expect(oldest.candidates.first?.entry == "splits/base-master.apk" && entries(oldest).contains("splits/base-arm64_v8a.apk"))
        #expect(!entries(oldest).contains("splits/base-arm64_v8a_2.apk"))
        // The exact levels a variant starts at.
        #expect(try found.selecting(guestAbis: arm, guestSdk: 31).candidates.first?.entry == "splits/base-master_3.apk")
        #expect(try found.selecting(guestAbis: arm, guestSdk: 23).candidates.first?.entry == "splits/base-master_2.apk")
        #expect(try found.selecting(guestAbis: arm, guestSdk: 21).candidates.first?.entry == "splits/base-master.apk")
        // A guest that cannot say its level gets the newest variant.
        #expect(try found.selecting(guestAbis: arm).candidates.first?.entry == "splits/base-master_3.apk")
        // No variant is for an Android older than the oldest one.
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 20) }
    }

    @Test func noPartOfAnotherVariantIsEverSentBesideTheChosenOne() throws {
        let found = try plan(archive(release()))
        for level in [21, 22, 23, 29, 31, 37, 60] {
            let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: level)
            // One base, one split for each processor at most, no split twice.
            #expect(chosen.candidates.filter { $0.splitName == nil }.count == 1, "api \(level)")
            let ids = chosen.candidates.compactMap(\.splitName)
            #expect(Set(ids).count == ids.count, "api \(level)")
            #expect(chosen.candidates.compactMap(\.abi) == ["arm64-v8a"], "api \(level)")
            #expect(Set(chosen.candidates.map(\.name)).count == chosen.candidates.count, "api \(level)")
        }
    }

    @Test func aBundleIsReadFromItsTableAndNotFromTheNamesOfItsApks() throws {
        // `-master_2` is not a master here: the table says which APK is.
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [.master("splits/something.apk"), .split("splits/base-master_2.apk", "config.en")]),
        ])
        let chosen = try plan(archive(toc)).selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(chosen.candidates.first?.entry == "splits/something.apk")
        #expect(chosen.candidates.map(\.splitName) == [nil, "config.en"])
        // An APK the table does not name is not installed, whatever it is called.
        let more = try plan(archive(toc, extra: ["splits/base-xxhdpi.apk", "standalones/standalone.apk", "universal.apk"]))
        #expect(entries(try more.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)) == ["splits/something.apk", "splits/base-master_2.apk"])
    }

    @Test func aFeatureModuleIsInstalledWithTheBase() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [
                .master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en"),
                .master("splits/feature-master.apk", module: "feature"),
                .split("splits/feature-arm64_v8a.apk", "feature.config.arm64_v8a", abi: 3, module: "feature"),
                .split("splits/feature-x86.apk", "feature.config.x86", abi: 4, module: "feature"),
            ]),
        ])
        let chosen = try plan(archive(toc)).selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(entries(chosen) == ["splits/base-master.apk", "splits/base-en.apk", "splits/feature-master.apk", "splits/feature-arm64_v8a.apk"])
        #expect(chosen.candidates.first?.entry == "splits/base-master.apk")
        // Its master has the module's name for an id, and is not a second base.
        #expect(chosen.candidates.filter { $0.splitName == nil }.count == 1)
    }

    // MARK: Universal and standalone

    @Test func aUniversalBundleInstallsAsOneApk() throws {
        let toc = BundleTocFixture(variants: [Variant(minSdk: nil, apks: [.standalone("universal.apk")])])
        let found = try plan(archive(toc))
        let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(chosen.candidates.map(\.entry) == ["universal.apk"] && chosen.candidates.first?.name == "base.apk")
        #expect(chosen.candidates.first?.splitName == nil)
        #expect(chosen.data.isEmpty)
    }

    @Test func standaloneBuildsForOneProcessorEachAreChosenByTheGuestsProcessors() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 1, abis: [4], apks: [.standalone("standalones/standalone-x86.apk")]),
            Variant(minSdk: 1, abis: [2], apks: [.standalone("standalones/standalone-armeabi_v7a.apk")]),
            Variant(minSdk: 1, abis: [3], apks: [.standalone("standalones/standalone-arm64_v8a.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 37).candidates.map(\.entry) == ["standalones/standalone-arm64_v8a.apk"])
        #expect(try found.selecting(guestAbis: ["armeabi-v7a"], guestSdk: 37).candidates.map(\.entry) == ["standalones/standalone-armeabi_v7a.apk"])
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: ["riscv64"], guestSdk: 37) }
    }

    @Test func theSplitsOfARecentAndroidBeatTheStandalonesOfAnOldOne() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 1, apks: [.standalone("standalones/standalone.apk")]),
            Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en")]),
        ])
        let found = try plan(archive(toc))
        #expect(try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37).candidates.first?.entry == "splits/base-master.apk")
        #expect(try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 19).candidates.map(\.entry) == ["standalones/standalone.apk"])
    }

    @Test func aVariantForSomethingElseIsNotChosen() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, density: true, apks: [.standalone("standalones/standalone-xxhdpi.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37) }
    }

    // MARK: Targeting

    private let arm = ["arm64-v8a", "armeabi-v7a", "armeabi"]

    @Test func anAlternativeForANewerAndroidThatTheGuestHasExcludesTheVariantWithNoFallingBackAcrossIt() throws {
        // The variant from 21 has a sibling from 23; the one from 23 is for a processor the guest lacks.
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [.master("splits/old-master.apk")]),
            Variant(minSdk: 23, abis: [4], apks: [.master("splits/new-master.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 37) }
        // Below 23 the older variant is the one that fits.
        #expect(try found.selecting(guestAbis: arm, guestSdk: 22).candidates.map(\.entry) == ["splits/old-master.apk"])
        // A guest with the processor takes the newer one.
        #expect(try found.selecting(guestAbis: ["x86"], guestSdk: 37).candidates.map(\.entry) == ["splits/new-master.apk"])
    }

    @Test func aProcessorValueTheReaderCannotNameRefusesItsVariantOrApkAndIsNeverNoRestriction() throws {
        // 9 is no processor this reader has a name for; 0 is none stated; 8 is RISC-V.
        for alias in [9, 0, 8, 1000] {
            let variant = BundleTocFixture(variants: [Variant(minSdk: 21, abis: [alias], apks: [.master("splits/base-master.apk")])])
            let found = try plan(archive(variant))
            #expect(throws: InstallRefusal.damaged, "variant \(alias)") { _ = try found.selecting(guestAbis: arm, guestSdk: 37) }
            // Of two variants, the one with the processor it cannot name is the one left out.
            let two = BundleTocFixture(variants: [
                Variant(minSdk: 21, abis: [alias], abiAlternatives: [], apks: [.master("splits/odd-master.apk")]),
                Variant(minSdk: 21, abis: [3], abiAlternatives: [], apks: [.master("splits/arm-master.apk")]),
            ])
            #expect(try plan(archive(two)).selecting(guestAbis: arm, guestSdk: 37).candidates.map(\.entry) == ["splits/arm-master.apk"], "variant \(alias)")
            // A split for it is not installed either.
            let split = BundleTocFixture(variants: [Variant(apks: [
                .master("splits/base-master.apk"), .split("splits/odd.apk", "config.odd", abi: alias, abiAlternatives: []),
                .split("splits/arm.apk", "config.arm64_v8a", abi: 3, abiAlternatives: []),
            ])])
            #expect(entries(try plan(archive(split)).selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/arm.apk"], "split \(alias)")
        }
    }

    @Test func twoVariantsThatFitAreRefusedAndNotChosenByTheirNumber() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, sdkAlternatives: [], number: 0, apks: [.master("splits/a-master.apk")]),
            Variant(minSdk: 23, sdkAlternatives: [], number: 1, apks: [.master("splits/b-master.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 37) }
        // With the alternatives bundletool writes, the same two are told apart.
        var told = toc
        told.variants[0].sdkAlternatives = nil
        told.variants[1].sdkAlternatives = nil
        #expect(try plan(archive(told)).selecting(guestAbis: arm, guestSdk: 37).candidates.map(\.entry) == ["splits/b-master.apk"])
        // The same processor twice is no better.
        let abis = BundleTocFixture(variants: [
            Variant(minSdk: 1, abis: [3], abiAlternatives: [], apks: [.standalone("standalones/a.apk")]),
            Variant(minSdk: 1, abis: [3], abiAlternatives: [], apks: [.standalone("standalones/b.apk")]),
        ])
        #expect(throws: InstallRefusal.damaged) { _ = try plan(archive(abis)).selecting(guestAbis: arm, guestSdk: 37) }
    }

    @Test func theProcessorOfAVariantIsTheGuestsMostPreferredOneTheBundleHasCodeFor() throws {
        // Written as bundletool writes them: each variant lists the others' processors as its alternatives.
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 1, abis: [3], apks: [.standalone("standalones/arm64.apk")]),
            Variant(minSdk: 1, abis: [2], apks: [.standalone("standalones/v7a.apk")]),
            Variant(minSdk: 1, abis: [4], apks: [.standalone("standalones/x86.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 37).candidates.map(\.entry) == ["standalones/arm64.apk"])
        #expect(try found.selecting(guestAbis: ["armeabi-v7a", "arm64-v8a"], guestSdk: 37).candidates.map(\.entry) == ["standalones/v7a.apk"])
        #expect(try found.selecting(guestAbis: ["x86_64", "x86"], guestSdk: 37).candidates.map(\.entry) == ["standalones/x86.apk"])
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: ["riscv64"], guestSdk: 37) }
    }

    @Test func anApkIsForTheGuestsAndroidByItsOwnTargetingToo() throws {
        let toc = BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), Apk(path: "splits/new.apk", splitID: "config.new", sdk: 31),
            Apk(path: "splits/future.apk", splitID: "config.future", sdk: 99),
            // A sibling for a newer Android that the guest has makes this one the wrong fit.
            Apk(path: "splits/old.apk", splitID: "config.old", sdk: 1, sdkAlternatives: [28]),
        ])])
        let found = try plan(archive(toc))
        #expect(entries(try found.selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/new.apk"])
        #expect(entries(try found.selecting(guestAbis: arm, guestSdk: 27)) == ["splits/base-master.apk", "splits/old.apk"])
        // A base for a newer Android than the guest's leaves no base.
        let future = BundleTocFixture(variants: [Variant(apks: [.master("splits/base-master.apk", sdk: 99)])])
        #expect(throws: InstallRefusal.damaged) { _ = try plan(archive(future)).selecting(guestAbis: arm, guestSdk: 37) }
    }

    @Test func targetingThatIsNotWeighedRefusesTheFileOnlyWhereItsApkWouldBeInstalled() throws {
        let toc = BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"),
            // A density is kept; a sanitizer is not weighed.
            Apk(path: "splits/hdpi.apk", splitID: "config.hdpi", density: true),
            Apk(path: "splits/odd.apk", splitID: "config.odd", sdk: 99, other: true),
        ])])
        let found = try plan(archive(toc))
        // Out of reach of this guest, so never installed, so no matter.
        #expect(entries(try found.selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/hdpi.apk"])
        let reachable = BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), Apk(path: "splits/odd.apk", splitID: "config.odd", other: true),
        ])])
        #expect(throws: InstallRefusal.damaged) { _ = try plan(archive(reachable)).selecting(guestAbis: arm, guestSdk: 37) }
        // A variant for several processors together is not chosen; one for a density neither.
        let multi = BundleTocFixture(variants: [Variant(minSdk: 1, multiAbi: true, apks: [.standalone("standalones/multi.apk")])])
        #expect(throws: InstallRefusal.damaged) { _ = try plan(archive(multi)).selecting(guestAbis: arm, guestSdk: 37) }
    }

    @Test func aBundleWithSplitsAndAMultiAbiStandaloneInstallsThroughTheSplits() throws {
        // The standalone build for old Androids is for the processors together (not weighed here) and
        // from level 1; the split variant is from 21. Android 17 takes the splits.
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 1, multiAbi: true, apks: [.standalone("standalones/multi.apk")]),
            Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en")]),
        ])
        let found = try plan(archive(toc))
        #expect(entries(try found.selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/base-en.apk"])
        // Below 21 only the standalone is for the guest, and it is not weighed: refused.
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 19) }
    }

    @Test func aSplitOnlyForProcessorsTheGuestLacksIsNoMatchingProcessorNotLeftOutQuietly() throws {
        let toc = BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), .split("splits/x86.apk", "config.x86", abi: 4), .split("splits/x64.apk", "config.x86_64", abi: 5),
        ])])
        #expect(throws: InstallRefusal.noMatchingProcessor) { _ = try plan(archive(toc)).selecting(guestAbis: arm, guestSdk: 37) }
        // A split with the sibling the guest prefers listed as an alternative is the wrong fit too.
        let preferred = BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), .split("splits/v7a.apk", "config.armeabi_v7a", abi: 2, abiAlternatives: [3]),
            .split("splits/arm64.apk", "config.arm64_v8a", abi: 3, abiAlternatives: [2]),
        ])])
        #expect(entries(try plan(archive(preferred)).selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/arm64.apk"])
    }

    // MARK: Descriptions of a path

    @Test func aPathTwoVariantsShareTakesTheDescriptionOfTheVariantThatIsChosen() throws {
        // The first variant describes the shared APK as the x86 one; the chosen one, as the ARM64 one.
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [.master("splits/old-master.apk"), .split("splits/shared.apk", "config.x86", abi: 4, abiAlternatives: [])]),
            Variant(minSdk: 23, apks: [.master("splits/new-master.apk"), .split("splits/shared.apk", "config.arm64_v8a", abi: 3, abiAlternatives: [])]),
        ])
        let found = try plan(archive(toc))
        let chosen = try found.selecting(guestAbis: arm, guestSdk: 37)
        #expect(entries(chosen) == ["splits/new-master.apk", "splits/shared.apk"])
        #expect(chosen.candidates.last?.splitName == "config.arm64_v8a" && chosen.candidates.last?.abi == "arm64-v8a")
        // The older one, on its own guest, sees its own description of the path.
        #expect(throws: InstallRefusal.noMatchingProcessor) { _ = try found.selecting(guestAbis: arm, guestSdk: 22) }
        #expect(try found.selecting(guestAbis: ["x86"], guestSdk: 22).candidates.last?.splitName == "config.x86")
    }

    @Test func aPathDescribedTwiceInOneVariantIsDamaged() {
        let twice = BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), .split("splits/a.apk", "config.en"), .split("splits/a.apk", "config.fr"),
        ])])
        #expect(refusal(archive(twice)) == .damaged)
        // The base twice, as a split as well.
        let base = BundleTocFixture(variants: [Variant(apks: [.master("splits/base-master.apk"), .split("splits/base-master.apk", "config.en")])])
        #expect(refusal(archive(base)) == .damaged)
    }

    // MARK: Asset packs

    @Test func theSlicesOfAnInstallTimePackAreInstalledWithTheBase() throws {
        let found = try plan(bundle([.installTime("levels", pack)]))
        #expect(found.unplacedData == 0)
        let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(entries(chosen) == [
            "splits/base-master.apk", "splits/base-en.apk", "asset-slices/levels-master.apk", "asset-slices/levels-config.tcf_astc.apk",
        ])
        #expect(chosen.candidates.first?.entry == "splits/base-master.apk" && chosen.candidates.first?.name == "base.apk")
        #expect(chosen.candidates.filter { $0.splitName == nil }.count == 1)
        #expect(chosen.candidates.compactMap(\.splitName).contains("levels") && chosen.candidates.compactMap(\.splitName).contains("levels.config.tcf_astc"))
        // Every part has a name of its own in the session.
        #expect(Set(chosen.candidates.map(\.name)).count == chosen.candidates.count)
        #expect(chosen.unplacedData == 0 && chosen.assetSlices.isEmpty)
    }

    @Test func aPackThatIsNotInstallTimeIsNotInstalledAndIsCounted() throws {
        for module in [Module.onDemand("levels", pack), .fastFollow("levels", pack), Module(name: "levels", delivery: nil, slices: pack)] {
            let found = try plan(bundle([module]))
            #expect(found.unplacedData == 1)
            let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
            #expect(entries(chosen) == ["splits/base-master.apk", "splits/base-en.apk"])
            // The count is what the install's result is made of.
            #expect(chosen.unplacedData == 1)
        }
        // One pack of each kind: the install-time one is installed, the two others counted.
        let mixed = try plan(bundle([
            .installTime("levels", pack), .onDemand("extra", [.slice("asset-slices/extra-master.apk", "extra", master: true)]),
            .fastFollow("later", [.slice("asset-slices/later-master.apk", "later", master: true)]),
        ]))
        #expect(mixed.unplacedData == 2)
        let chosen = try mixed.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(entries(chosen).contains("asset-slices/levels-master.apk") && !entries(chosen).contains("asset-slices/extra-master.apk"))
        #expect(!entries(chosen).contains("asset-slices/later-master.apk"))
    }

    @Test func aSliceIsForTheGuestsAndroidAndProcessor() throws {
        let slices: [Slice] = [
            .slice("asset-slices/levels-master.apk", "levels", master: true),
            .slice("asset-slices/levels-new.apk", "levels.config.new", minSdk: 31),
            .slice("asset-slices/levels-arm64.apk", "levels.config.arm64_v8a", abi: 3),
            .slice("asset-slices/levels-x86.apk", "levels.config.x86", abi: 4),
        ]
        let found = try plan(bundle([.installTime("levels", slices)]))
        let old = try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 30)
        #expect(entries(old) == ["splits/base-master.apk", "splits/base-en.apk", "asset-slices/levels-master.apk", "asset-slices/levels-arm64.apk"])
        let recent = try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 37)
        #expect(entries(recent).contains("asset-slices/levels-new.apk") && !entries(recent).contains("asset-slices/levels-x86.apk"))
        // The processor is the guest's one choice for the app's splits and the pack's together.
        let x86 = try found.selecting(guestAbis: ["x86"], guestSdk: 37)
        #expect(entries(x86).contains("asset-slices/levels-x86.apk") && !entries(x86).contains("asset-slices/levels-arm64.apk"))
        // Dimensions the guest cannot be asked about (a language, a texture format) keep their slice.
        #expect(entries(try found.selecting(guestAbis: ["arm64-v8a"], guestSdk: 37)).contains("asset-slices/levels-master.apk"))
    }

    @Test func aSliceIsMatchedByTheSameRulesAsAnApkAndUnweighedTargetingRefusesIt() throws {
        // The reviewer's shape: a base variant and an install-time asset set.
        let base = [Variant(minSdk: 21, apks: [.master("splits/base-master.apk")])]
        let found = try plan(bundle([.installTime("levels", pack)], variants: base))
        #expect(entries(try found.selecting(guestAbis: arm, guestSdk: 37)) == [
            "splits/base-master.apk", "asset-slices/levels-master.apk", "asset-slices/levels-config.tcf_astc.apk",
        ])
        // An alternative above the slice and at or below the guest's level leaves it out.
        let layered: [Slice] = [
            .slice("asset-slices/levels-master.apk", "levels", master: true),
            .slice("asset-slices/levels-v1.apk", "levels.config.v1", minSdk: 1, sdkAlternatives: [28]),
            .slice("asset-slices/levels-v28.apk", "levels.config.v28", minSdk: 28, sdkAlternatives: [1]),
        ]
        let sets = try plan(bundle([.installTime("levels", layered)], variants: base))
        #expect(entries(try sets.selecting(guestAbis: arm, guestSdk: 37)).contains("asset-slices/levels-v28.apk"))
        #expect(!entries(try sets.selecting(guestAbis: arm, guestSdk: 37)).contains("asset-slices/levels-v1.apk"))
        #expect(entries(try sets.selecting(guestAbis: arm, guestSdk: 27)).contains("asset-slices/levels-v1.apk"))
        // A slice for a processor this reader cannot name is left out; one with targeting it does not weigh is refused when it would be sent.
        let odd = try plan(bundle([.installTime("levels", [
            .slice("asset-slices/levels-master.apk", "levels", master: true), .slice("asset-slices/levels-odd.apk", "levels.config.odd", abi: 9),
        ])], variants: base))
        #expect(!entries(try odd.selecting(guestAbis: arm, guestSdk: 37)).contains("asset-slices/levels-odd.apk"))
        let other = try plan(bundle([.installTime("levels", [
            .slice("asset-slices/levels-master.apk", "levels", master: true), .slice("asset-slices/levels-tier.apk", "levels.config.tier", other: true),
        ])], variants: base))
        #expect(throws: InstallRefusal.damaged) { _ = try other.selecting(guestAbis: arm, guestSdk: 37) }
    }

    @Test func aStandaloneApkHasItsInstallTimePacksFusedInAndKeepsOnlyTheCount() throws {
        let toc = BundleTocFixture(
            variants: [Variant(minSdk: 1, apks: [.standalone("standalones/standalone.apk")])],
            assetModules: [.installTime("levels", pack), .onDemand("extra", [.slice("asset-slices/extra-master.apk", "extra", master: true)])])
        let found = try plan(archive(toc, extra: ["asset-slices/levels-master.apk", "asset-slices/levels-config.tcf_astc.apk", "asset-slices/extra-master.apk"]))
        let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 19)
        #expect(chosen.candidates.map(\.entry) == ["standalones/standalone.apk"])
        #expect(chosen.unplacedData == 1)
    }

    @Test func aSliceThatIsNotAnApkSliceMakesItsPackIncomplete() throws {
        let slices: [Slice] = [
            .slice("asset-slices/levels-master.apk", "levels", master: true),
            Slice(path: "asset-slices/levels-other.apk", splitID: "levels.other", metadata: false),
            .slice("asset-slices/levels-data.zip", "levels.data"),
        ]
        let found = try plan(bundle([.installTime("levels", slices)]))
        #expect(found.unplacedData == 1)
        let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(entries(chosen).contains("asset-slices/levels-master.apk"))
        #expect(!entries(chosen).contains("asset-slices/levels-other.apk") && !entries(chosen).contains("asset-slices/levels-data.zip"))
    }

    @Test func aPackThatContradictsTheTableOrTheFileIsDamaged() throws {
        // A slice the file does not hold; a directory; a split installed twice, from the app and the pack.
        #expect(refusal(bundle([.installTime("levels", pack)], missing: ["asset-slices/levels-master.apk"])) == .damaged)
        let twice = try plan(bundle([.installTime("levels", [.slice("asset-slices/levels-en.apk", "config.en")])]))
        #expect(throws: InstallRefusal.damaged) { _ = try twice.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37) }
        let same = try plan(bundle([.installTime("a", [.slice("asset-slices/a-master.apk", "pack")]), .installTime("b", [.slice("asset-slices/b-master.apk", "pack")])]))
        #expect(throws: InstallRefusal.damaged) { _ = try same.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37) }
        // The same file named by two packs.
        #expect(refusal(bundle([
            .installTime("a", [.slice("asset-slices/shared.apk", "a")]), .installTime("b", [.slice("asset-slices/shared.apk", "b")]),
        ])) == nil)
        let shared = try plan(bundle([.installTime("a", [.slice("asset-slices/shared.apk", "a")]), .installTime("b", [.slice("asset-slices/shared.apk", "b")])]))
        #expect(throws: InstallRefusal.damaged) { _ = try shared.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37) }
    }

    @Test func thePacksAreBoundedLikeTheRestOfTheTable() {
        var slices: [Slice] = []
        for index in 0...ApkContainer.maxParts { slices.append(.slice("asset-slices/s\(index).apk", "s\(index)")) }
        #expect(refusal(bundle([.installTime("big", slices)])) == .damaged)
        let modules = (0..<(BundleTable.maxAssetModules + 1)).map { Module.onDemand("m\($0)", []) }
        #expect(refusal(bundle(modules)) == .damaged)
        // A table that fills the first part's room with slices leaves none for the app's own.
        let fifty = (0..<ApkContainer.maxParts).map { Slice.slice("asset-slices/f\($0).apk", "f\($0)") }
        let found = try? plan(bundle([.installTime("fits", fifty)]))
        #expect(found != nil)
        #expect(throws: InstallRefusal.damaged) { _ = try found?.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37) }
    }

    // MARK: A table that cannot be trusted

    @Test func aTableThatIsNotOneIsDamagedNotGuessedFrom() {
        let names = ["splits/base-master.apk", "splits/base-en.apk"]
        for bytes in [
            Data(), Data("toc".utf8), Data(repeating: 0xff, count: 40), Data([0x0a, 0x7f]), Data([0x0a]), Data([0x08]),
            Data(repeating: 0x80, count: 12), Data([0x0b, 0x01]), Data([0x0c]), Data([0x0e, 0x00]), Data([0x00, 0x01]),
        ] {
            let items: [ZipFixture.Item] = [.init("toc.pb", bytes, method: 8)] + names.map { entry($0) }
            #expect(refusal(items) == .damaged, "\(Array(bytes))")
        }
    }

    @Test func everyCutOfATableIsDamagedOrAWholeTable() {
        var table = release()
        table.assetModules = [.installTime("levels", pack), .onDemand("extra", [.slice("asset-slices/extra-master.apk", "extra", master: true)])]
        let toc = table.build()
        let extras = ["asset-slices/levels-master.apk", "asset-slices/levels-config.tcf_astc.apk", "asset-slices/extra-master.apk"]
        var cut = 0
        while cut < toc.count {
            let items: [ZipFixture.Item] = [.init("toc.pb", toc.prefix(cut), method: 8)] + archive(table, extra: extras).dropFirst()
            let result = refusal(items)
            #expect(result == nil || result == .damaged, "cut at \(cut)")
            cut += 7
        }
    }

    @Test func aTableThatNamesWhatTheFileDoesNotHoldOrContradictsItselfIsDamaged() {
        // An APK the file does not hold.
        let missing = BundleTocFixture(variants: [Variant(apks: [.master("splits/base-master.apk"), .split("splits/gone.apk", "config.en")])])
        #expect(refusal(archive(missing).filter { $0.name != "splits/gone.apk" }) == .damaged)
        // A directory is not an APK.
        let directory = BundleTocFixture(variants: [Variant(apks: [.master("splits/")])])
        #expect(refusal([.init("toc.pb", directory.build()), .init("splits/", Data(), method: 0), entry("other.apk")]) == .damaged)
        // No master, two masters, no variant, a split twice.
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.split("splits/base-en.apk", "config.en")])]))) == .damaged)
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.master("splits/a.apk"), .master("splits/b.apk")])]))) == .damaged)
        #expect(refusal(archive(BundleTocFixture(variants: []), extra: ["splits/base-master.apk"])) == .damaged)
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), .split("splits/a.apk", "config.en"), .split("splits/b.apk", "config.en"),
        ])]))) == .damaged)
        // A master of a feature module is not the app's base.
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.master("splits/f.apk", module: "feature")])]))) == .damaged)
        // A standalone APK with splits beside it.
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.standalone("universal.apk"), .split("splits/a.apk", "config.en")])]))) == .damaged)
    }

    @Test func aTableOverTheCapOrOfTooManyPartsIsDamaged() {
        let big = Data(repeating: 0, count: ApkContainer.maxManifestBytes + 1)
        #expect(refusal([.init("toc.pb", big, method: 8), entry("splits/base-master.apk")]) == .damaged)
        var apks: [Apk] = [.master("splits/base-master.apk")]
        for index in 0..<ApkContainer.maxParts { apks.append(.split("splits/l\(index).apk", "config.l\(index)")) }
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: apks)]))) == .damaged)
        apks.removeLast()
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: apks)]))) == nil)
        // More variants than a bundle has.
        let variants = (0..<(BundleTable.maxVariants + 1)).map { Variant(minSdk: $0 + 1, apks: [.master("splits/base-master.apk")]) }
        #expect(refusal(archive(BundleTocFixture(variants: variants))) == .damaged)
    }

    @Test func theWireFormatIsReadWithItsLengthsChecked() throws {
        // A varint of eleven bytes, a length past the end, a group, a field number of zero.
        for bytes in [
            Data(repeating: 0x80, count: 10) + Data([0x01]), Data([0x0a, 0x05, 0x01]), Data([0x0b]), Data([0x04]), Data([0x00, 0x00]),
            Data([0x0f]), Data([0x0e]),
        ] {
            #expect(throws: BundleTable.Failure.self, "\(Array(bytes))") { _ = try BundleTable(bytes) }
        }
        // Fields it does not read are skipped, fixed-width ones included.
        let fixed32 = Data([0x1d, 1, 2, 3, 4])
        let fixed64 = Data([0x21, 1, 2, 3, 4, 5, 6, 7, 8])
        let skipped = try BundleTable(fixed32 + fixed64 + BundleTocFixture.string(7, "x") + BundleTocFixture.int(9, 5))
        #expect(skipped.variants.isEmpty)
        // The asset modules, with the delivery and the targeting of each slice.
        var withPacks = release()
        withPacks.assetModules = [
            .installTime("levels", [.slice("asset-slices/levels-master.apk", "levels", master: true), .slice("asset-slices/l.apk", "levels.x", abi: 3, minSdk: 28)]),
            .fastFollow("later", []), Module(name: "none", delivery: nil, slices: []),
        ]
        let packs = try BundleTable(withPacks.build()).assetModules
        #expect(packs.map(\.name) == ["levels", "later", "none"] && packs.map(\.delivery) == [.installTime, .fastFollow, .unknown])
        #expect(packs[0].slices.map(\.path) == ["asset-slices/levels-master.apk", "asset-slices/l.apk"])
        #expect(packs[0].slices[1].targeting.abis == ["arm64-v8a"] && packs[0].slices[1].targeting.sdk == [28])
        #expect(packs[0].slices[0].targeting.sdk.isEmpty && packs[0].slices[0].targeting.abis.isEmpty)
        #expect(!packs[0].hasUnusableSlice)
        // A table with no asset sets has none.
        #expect(try BundleTable(release().build()).assetModules.isEmpty)
        // A whole table reads.
        let table = try BundleTable(release().build())
        #expect(table.variants.map(\.targeting.sdk) == [[21], [23], [31]])
        // Each variant lists the levels of the others as its alternatives.
        #expect(table.variants.map(\.targeting.sdkAlternatives) == [[23, 31], [21, 31], [21, 23]])
        #expect(table.variants[2].apks.first?.isMaster == true && table.variants[2].apks.first?.module == "base")
        #expect(table.variants[2].apks.contains { $0.path == "splits/base-arm64_v8a_2.apk" && $0.targeting.abis == ["arm64-v8a"] })
    }

    // MARK: Targeting read strictly, and the parts of a table kept once matched

    private typealias T = BundleTocFixture

    /// `SdkVersion { min { value } }`; no value for nil (the default, 0).
    private func version(_ level: Int?) -> Data { T.message(1, level.map { T.int(1, $0) } ?? Data()) }

    /// `SdkVersionTargeting` as the field `field` of a targeting message.
    private func sdkTargeting(_ values: [Int?], alternatives: [Int?] = [], field: Int = 1) -> Data {
        T.message(field, values.reduce(Data()) { $0 + T.message(1, version($1)) } + alternatives.reduce(Data()) { $0 + T.message(2, version($1)) })
    }

    /// `AbiTargeting` as the field `field` of a targeting message.
    private func abiTargeting(_ values: [Int], alternatives: [Int] = [], field: Int = 2) -> Data {
        T.message(field, values.reduce(Data()) { $0 + T.message(1, T.int(1, $1)) } + alternatives.reduce(Data()) { $0 + T.message(2, T.int(1, $1)) })
    }

    private func rawApk(_ path: String, id: String = "", master: Bool = false, targeting: Data? = Data(), metadata: Int = 3) -> Data {
        T.message(2, (targeting.map { T.message(1, $0) } ?? Data()) + T.string(2, path) + T.message(metadata, T.string(1, id) + T.int(2, master ? 1 : 0)))
    }

    private func rawVariant(_ apks: Data, targeting: Data = Data(), number: Int = 0) -> Data {
        T.message(1, T.message(1, targeting) + T.message(2, T.message(1, T.string(1, "base")) + apks) + T.int(3, number))
    }

    private func raw(_ toc: Data, _ names: [String]) -> [ZipFixture.Item] {
        [.init("toc.pb", toc, method: 8)] + names.map { entry($0) }
    }

    private func rawPlan(_ toc: Data, _ names: [String]) throws -> ApkContainerPlan { try plan(raw(toc, names)) }

    @Test func anInstallTimeSliceThatMatchedItsOwnTargetingIsNotDroppedAfterwardsByTheProcessorOfTheAppsSplits() throws {
        // The app has an arm64 split; the install-time slice declares armeabi-v7a, which the guest has too.
        let app = Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-arm64_v8a.apk", "config.arm64_v8a", abi: 3)])
        let found = try plan(bundle([.installTime("req", [.slice("asset-slices/req-master.apk", "req", master: true, abi: 2)])], variants: [app]))
        let chosen = try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 37)
        #expect(entries(chosen) == ["splits/base-master.apk", "splits/base-arm64_v8a.apk", "asset-slices/req-master.apk"])
        #expect(chosen.unplacedData == 0)
        // A slice for a processor the guest lacks is left out by its own targeting, as before.
        let x86 = try plan(bundle([.installTime("req", [.slice("asset-slices/req-master.apk", "req", master: true, abi: 4)])], variants: [app]))
        #expect(!entries(try x86.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 37)).contains("asset-slices/req-master.apk"))
    }

    @Test func aSliceOfAnInstallTimePackWithATextureFormatRefusesTheFileAndAnOrdinarySplitDoesNot() throws {
        let slices: [Slice] = [
            .slice("asset-slices/astc.apk", "textures.astc", texture: (9, 10)), .slice("asset-slices/etc2.apk", "textures.etc2", texture: (10, 9)),
        ]
        let found = try plan(bundle([.installTime("textures", slices)]))
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 37) }
        // One slice is enough, and an unmatched one (for a newer Android) is not in the way.
        let one = try plan(bundle([.installTime("textures", [.slice("asset-slices/master.apk", "textures", master: true), .slice("asset-slices/astc.apk", "textures.astc", texture: (9, 10))])]))
        #expect(throws: InstallRefusal.damaged) { _ = try one.selecting(guestAbis: arm, guestSdk: 37) }
        let future = try plan(bundle([.installTime("textures", [.slice("asset-slices/master.apk", "textures", master: true), .slice("asset-slices/astc.apk", "textures.astc", minSdk: 99, texture: (9, 10))])]))
        #expect(entries(try future.selecting(guestAbis: arm, guestSdk: 37)).contains("asset-slices/master.apk"))
        // A pack that is not install-time is not installed, so its textures do not matter.
        let later = try plan(bundle([.fastFollow("textures", slices)]))
        #expect(entries(try later.selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/base-en.apk"])
        // The app's own splits keep every texture format, as every language and density.
        let split = Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-tcf_astc.apk", "config.tcf_astc", texture: (9, 10))])
        #expect(entries(try plan(archive(BundleTocFixture(variants: [split]))).selecting(guestAbis: arm, guestSdk: 37)) == ["splits/base-master.apk", "splits/base-tcf_astc.apk"])
    }

    @Test func aSingularFieldThatComesTwiceIsATableThatCannotBeReadAndNoRestrictionIsLostToIt() {
        // The x86 restriction of a variant, then a second targeting message with an SDK: merged by a protocol buffer parser.
        let doubled = T.message(1, T.message(1, abiTargeting([4])) + T.message(1, sdkTargeting([21])) + T.message(2, T.message(1, T.string(1, "base")) + rawApk("base.apk", master: true)))
        #expect(refusal(raw(doubled, ["base.apk"])) == .damaged)
        // The same at the level of an APK: x86, then an empty targeting message.
        let apk = T.message(2, T.message(1, abiTargeting([4], field: 1)) + T.message(1, Data()) + T.string(2, "base.apk") + T.message(3, T.int(2, 1)))
        #expect(refusal(raw(rawVariant(apk), ["base.apk"])) == .damaged)
        // The minimum of an SDK, twice: 21 and then 99 is 99, not 21.
        let twice = T.message(1, T.message(1, T.message(1, T.int(1, 21) + T.int(1, 99))))
        #expect(refusal(raw(rawVariant(rawApk("base.apk", master: true), targeting: twice), ["base.apk"])) == .damaged)
        // A second path, split metadata, or targeting of an asset slice, and a second module metadata.
        let slice = T.message(1, Data()) + T.string(2, "asset-slices/a.apk") + T.message(7, T.string(1, "a"))
        let assets = T.message(3, T.message(1, T.string(1, "a") + T.int(4, 1)) + T.message(2, slice + T.message(1, abiTargeting([4], field: 1))))
        #expect(refusal(raw(rawVariant(rawApk("base.apk", master: true)) + assets, ["base.apk", "asset-slices/a.apk"])) == .damaged)
        let metadata = T.message(3, T.message(1, T.string(1, "a") + T.int(4, 1)) + T.message(1, T.string(1, "b")) + T.message(2, slice))
        #expect(refusal(raw(rawVariant(rawApk("base.apk", master: true)) + metadata, ["base.apk", "asset-slices/a.apk"])) == .damaged)
        // Each is whole, once: it reads.
        let once = T.message(3, T.message(1, T.string(1, "a") + T.int(4, 1)) + T.message(2, slice))
        #expect(refusal(raw(rawVariant(rawApk("base.apk", master: true)) + once, ["base.apk", "asset-slices/a.apk"])) == nil)
    }

    @Test func aStatedLevelIsTheLevelAndAnUnstatedOneIsZeroSoANewerAlternativeExcludesIt() throws {
        // No value, and an alternative from 1: bundletool's default 0 is excluded by it on any guest that has 1.
        let unstated = try rawPlan(rawVariant(rawApk("base.apk", master: true), targeting: sdkTargeting([], alternatives: [1], field: 1)), ["base.apk"])
        #expect(throws: InstallRefusal.damaged) { _ = try unstated.selecting(guestAbis: arm, guestSdk: 37) }
        // A level stated as 0 is the same level, not 1.
        let zero = try rawPlan(rawVariant(rawApk("base.apk", master: true), targeting: sdkTargeting([0], alternatives: [1], field: 1)), ["base.apk"])
        #expect(throws: InstallRefusal.damaged) { _ = try zero.selecting(guestAbis: arm, guestSdk: 37) }
        // A guest below the alternative still has the variant, and one with no alternative at all keeps it.
        #expect(try zero.selecting(guestAbis: arm, guestSdk: 0).candidates.count == 1)
        let alone = try rawPlan(rawVariant(rawApk("base.apk", master: true), targeting: sdkTargeting([nil], field: 1)), ["base.apk"])
        #expect(try alone.selecting(guestAbis: arm, guestSdk: 37).candidates.count == 1)
        // The regular pair from 21 and 23 is as it was.
        let pair = rawVariant(rawApk("old.apk", master: true), targeting: sdkTargeting([21], alternatives: [23]))
            + rawVariant(rawApk("new.apk", master: true), targeting: sdkTargeting([23], alternatives: [21]), number: 1)
        #expect(try rawPlan(pair, ["old.apk", "new.apk"]).selecting(guestAbis: arm, guestSdk: 37).candidates.map(\.entry) == ["new.apk"])
        #expect(try rawPlan(pair, ["old.apk", "new.apk"]).selecting(guestAbis: arm, guestSdk: 22).candidates.map(\.entry) == ["old.apk"])
    }

    @Test func anAbiThatIsNamedNowhereIsNeverNoRestrictionNotEvenAsAnAlternative() throws {
        // No value and an alternative the reader has no name for.
        for alias in [9, 99, 0] {
            let variant = rawVariant(rawApk("base.apk", master: true), targeting: abiTargeting([], alternatives: [alias]))
            #expect(throws: InstallRefusal.damaged) { _ = try rawPlan(variant, ["base.apk"]).selecting(guestAbis: arm, guestSdk: 37) }
            // As an alternative of an APK, which is left out by it; when it is the only code there is, there is no match.
            let only = rawVariant(rawApk("base.apk", master: true) + rawApk("odd.apk", id: "config.odd", targeting: abiTargeting([3], alternatives: [alias], field: 1)))
            #expect(throws: InstallRefusal.noMatchingProcessor) { _ = try rawPlan(only, ["base.apk", "odd.apk"]).selecting(guestAbis: arm, guestSdk: 37) }
            let beside = rawVariant(
                rawApk("base.apk", master: true) + rawApk("odd.apk", id: "config.odd", targeting: abiTargeting([3], alternatives: [alias], field: 1))
                    + rawApk("arm.apk", id: "config.arm64_v8a", targeting: abiTargeting([3], field: 1)))
            #expect(entries(try rawPlan(beside, ["base.apk", "odd.apk", "arm.apk"]).selecting(guestAbis: arm, guestSdk: 37)) == ["base.apk", "arm.apk"], "alias \(alias)")
        }
    }

    @Test func targetingThatContradictsItselfOrHoldsAFieldNobodyNamedIsNotAcceptedAndIsNotInstalled() throws {
        let base = rawApk("base.apk", master: true)
        // The same processor as a value and as an alternative; two SDK values.
        #expect(refusal(raw(rawVariant(base, targeting: abiTargeting([3], alternatives: [3])), ["base.apk"])) == .damaged)
        let levels = T.message(1, T.message(1, version(21)) + T.message(1, version(30)))
        #expect(refusal(raw(rawVariant(base, targeting: levels), ["base.apk"])) == .damaged)
        // A field of the targeting of a variant, of an APK or of an SDK that the schema has no number for: a scalar of any
        // wire type that holds something is a restriction that cannot be weighed; one that is zero holds nothing.
        for unknown in [T.int(99, 1), Data([0xcd, 0x06, 1, 2, 3, 4]), Data([0xc9, 0x06, 1, 0, 0, 0, 0, 0, 0, 0])] {
            let found = try rawPlan(rawVariant(base, targeting: unknown), ["base.apk"])
            #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 37) }
            let split = rawVariant(base + rawApk("odd.apk", id: "config.odd", targeting: unknown))
            #expect(throws: InstallRefusal.damaged) { _ = try rawPlan(split, ["base.apk", "odd.apk"]).selecting(guestAbis: arm, guestSdk: 37) }
            let inside = rawVariant(base, targeting: T.message(1, unknown))
            #expect(throws: InstallRefusal.damaged) { _ = try rawPlan(inside, ["base.apk"]).selecting(guestAbis: arm, guestSdk: 37) }
        }
        #expect(try rawPlan(rawVariant(base, targeting: T.int(99, 0)), ["base.apk"]).selecting(guestAbis: arm, guestSdk: 37).candidates.count == 1)
        // A field the schema has, of the wrong wire type, is a table that cannot be read.
        #expect(refusal(raw(rawVariant(base, targeting: T.int(2, 1)), ["base.apk"])) == .damaged)
        #expect(refusal(raw(rawVariant(base + rawApk("a.apk", id: "x", targeting: T.int(1, 1))), ["base.apk", "a.apk"])) == .damaged)
    }
}
