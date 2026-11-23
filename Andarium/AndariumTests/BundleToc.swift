// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Writes the table of contents of a bundletool `.apks` (`toc.pb`) for the hosted tests of Install APK: the
/// messages `BundleTable` reads, as protocol buffers, so that a test can build
/// the variants a real bundle has (one APK shared by several, a master split
/// for each) and the damage a hostile one has.
nonisolated struct BundleToc {
    struct Apk {
        var path: String
        /// Empty for a master split.
        var splitID = ""
        var master = false
        var module = "base"
        /// `AbiAlias` of bundletool: 1 armeabi, 2 armeabi-v7a, 3 arm64-v8a, 4 x86, 5 x86_64, 8 riscv64.
        var abi: Int?
        /// The processors the siblings of a split are for: nil for the other processors of the
        /// variant's splits, as bundletool writes them.
        var abiAlternatives: [Int]?
        var standalone = false
        /// `ApkTargeting.sdk_version_targeting`: the lowest API level, and the siblings'.
        var sdk: Int?
        var sdkAlternatives: [Int] = []
        /// A kind of targeting this reader keeps (a screen density) or does not weigh (a sanitizer).
        var density = false
        var other = false
        /// A texture compression format (`value`, `alternative`), which is kept on an app's split.
        var texture: (value: Int, alternative: Int)?

        static func master(_ path: String, module: String = "base", sdk: Int? = nil, sdkAlternatives: [Int] = []) -> Apk {
            Apk(path: path, splitID: module == "base" ? "" : module, master: true, module: module, sdk: sdk, sdkAlternatives: sdkAlternatives)
        }

        static func split(
            _ path: String, _ id: String, abi: Int? = nil, abiAlternatives: [Int]? = nil, module: String = "base",
            texture: (value: Int, alternative: Int)? = nil
        ) -> Apk {
            Apk(path: path, splitID: id, master: false, module: module, abi: abi, abiAlternatives: abiAlternatives, texture: texture)
        }

        static func standalone(_ path: String) -> Apk {
            Apk(path: path, standalone: true)
        }
    }

    struct Variant {
        var minSdk: Int? = 21
        /// The Android the siblings are for: nil for the other variants' levels, as bundletool writes them.
        var sdkAlternatives: [Int]?
        var abis: [Int] = []
        /// The processors the other variants are for: nil for theirs.
        var abiAlternatives: [Int]?
        /// A field of the variant's targeting this reader does not weigh (a screen density).
        var density = false
        /// Several processors together (`multi_abi_targeting`), which this reader does not weigh either.
        var multiAbi = false
        var number: Int?
        var apks: [Apk]
    }

    /// An asset module (an asset pack) and its slices: `AssetSliceSet` of bundletool's
    /// `BuildApksResult`.
    struct AssetModule {
        var name: String
        /// `DeliveryType`: 1 install-time, 2 on-demand, 3 fast-follow; nil for none stated.
        var delivery: Int?
        var slices: [AssetSlice]

        static func installTime(_ name: String, _ slices: [AssetSlice]) -> AssetModule {
            AssetModule(name: name, delivery: 1, slices: slices)
        }

        static func onDemand(_ name: String, _ slices: [AssetSlice]) -> AssetModule {
            AssetModule(name: name, delivery: 2, slices: slices)
        }

        static func fastFollow(_ name: String, _ slices: [AssetSlice]) -> AssetModule {
            AssetModule(name: name, delivery: 3, slices: slices)
        }
    }

    struct AssetSlice {
        var path: String
        var splitID: String
        var master = false
        var abi: Int?
        /// The lowest API level, as `ApkTargeting.sdk_version_targeting` states it, and the siblings'.
        var minSdk: Int?
        var sdkAlternatives: [Int] = []
        var abiAlternatives: [Int] = []
        /// A kind of targeting this reader does not weigh (a sanitizer).
        var other = false
        /// False for a description that carries no asset slice metadata.
        var metadata = true
        /// A texture compression format (`value`, `alternative`).
        var texture: (value: Int, alternative: Int)?

        static func slice(
            _ path: String, _ id: String, master: Bool = false, abi: Int? = nil, minSdk: Int? = nil,
            sdkAlternatives: [Int] = [], abiAlternatives: [Int] = [], other: Bool = false, texture: (value: Int, alternative: Int)? = nil
        ) -> AssetSlice {
            AssetSlice(
                path: path, splitID: id, master: master, abi: abi, minSdk: minSdk, sdkAlternatives: sdkAlternatives,
                abiAlternatives: abiAlternatives, other: other, texture: texture)
        }
    }

    var variants: [Variant]
    var assetModules: [AssetModule] = []
    var packageName: String? = "com.example.bundle"

    // MARK: Protocol buffers

    static func varint(_ value: UInt64) -> Data {
        var data = Data()
        var rest = value
        while rest >= 0x80 {
            data.append(UInt8(rest & 0x7f) | 0x80)
            rest >>= 7
        }
        data.append(UInt8(rest))
        return data
    }

    static func message(_ field: Int, _ body: Data) -> Data {
        varint(UInt64(field << 3 | 2)) + varint(UInt64(body.count)) + body
    }

    static func string(_ field: Int, _ text: String) -> Data { message(field, Data(text.utf8)) }

    static func int(_ field: Int, _ value: Int) -> Data { varint(UInt64(field << 3)) + varint(UInt64(value)) }

    private static func abiTargeting(_ aliases: [Int], alternatives: [Int] = []) -> Data {
        aliases.reduce(Data()) { $0 + message(1, int(1, $1)) } + alternatives.reduce(Data()) { $0 + message(2, int(1, $1)) }
    }

    private static func sdkTargeting(_ levels: [Int], alternatives: [Int] = []) -> Data {
        levels.reduce(Data()) { $0 + message(1, message(1, int(1, $1))) }
            + alternatives.reduce(Data()) { $0 + message(2, message(1, int(1, $1))) }
    }

    /// `ApkTargeting` of a description.
    private static func apkTargeting(
        abi: Int?, abiAlternatives: [Int], sdk: Int?, sdkAlternatives: [Int], density: Bool, other: Bool,
        texture: (value: Int, alternative: Int)? = nil
    ) -> Data {
        var targeting = Data()
        if let abi { targeting += message(1, abiTargeting([abi], alternatives: abiAlternatives)) }
        if sdk != nil || !sdkAlternatives.isEmpty {
            targeting += message(5, sdkTargeting(sdk.map { [$0] } ?? [], alternatives: sdkAlternatives))
        }
        if density { targeting += message(4, message(1, message(1, int(1, 6)))) }
        if let texture { targeting += message(6, message(1, int(1, texture.value)) + message(2, int(1, texture.alternative))) }
        if other { targeting += message(8, message(1, int(1, 1))) }
        return targeting
    }

    private static func description(_ apk: Apk, siblings: [Apk]) -> Data {
        var body = Data()
        // The other processors the variant's splits of the module are for.
        let others = Array(Set(siblings.compactMap(\.abi)).subtracting(apk.abi.map { [$0] } ?? [])).sorted()
        let targeting = apkTargeting(
            abi: apk.abi, abiAlternatives: apk.abiAlternatives ?? others, sdk: apk.sdk, sdkAlternatives: apk.sdkAlternatives,
            density: apk.density, other: apk.other, texture: apk.texture)
        if !targeting.isEmpty { body += message(1, targeting) }
        body += string(2, apk.path)
        if apk.standalone {
            body += message(4, Data())
        } else {
            var split = Data()
            if !apk.splitID.isEmpty { split += string(1, apk.splitID) }
            if apk.master { split += int(2, 1) }
            body += message(3, split)
        }
        return message(2, body)
    }

    private static func assetSet(_ module: AssetModule) -> Data {
        var metadata = string(1, module.name)
        if let delivery = module.delivery { metadata += int(4, delivery) }
        var body = message(1, metadata)
        for slice in module.slices {
            var description = Data()
            let targeting = apkTargeting(
                abi: slice.abi, abiAlternatives: slice.abiAlternatives, sdk: slice.minSdk, sdkAlternatives: slice.sdkAlternatives,
                density: false, other: slice.other, texture: slice.texture)
            if !targeting.isEmpty { description += message(1, targeting) }
            description += string(2, slice.path)
            if slice.metadata {
                var split = Data()
                if !slice.splitID.isEmpty { split += string(1, slice.splitID) }
                if slice.master { split += int(2, 1) }
                description += message(7, split)
            }
            body += message(2, description)
        }
        return message(3, body)
    }

    private func variantData(_ variant: Variant, index: Int) -> Data {
        // The other variants' levels and processors are this variant's alternatives, as bundletool writes them.
        let siblings = variants.enumerated().filter { $0.offset != index }.map(\.element)
        let levels = Array(Set(siblings.compactMap(\.minSdk)).subtracting(variant.minSdk.map { [$0] } ?? [])).sorted()
        let abis = Array(Set(siblings.flatMap(\.abis)).subtracting(variant.abis)).sorted()
        var targeting = Data()
        if let sdk = variant.minSdk {
            targeting += Self.message(1, Self.sdkTargeting([sdk], alternatives: variant.sdkAlternatives ?? levels))
        }
        if !variant.abis.isEmpty {
            targeting += Self.message(2, Self.abiTargeting(variant.abis, alternatives: variant.abiAlternatives ?? abis))
        }
        if variant.density { targeting += Self.message(3, Self.message(1, Self.message(1, Self.int(1, 6)))) }
        if variant.multiAbi { targeting += Self.message(4, Self.message(1, Self.message(1, Self.int(1, 3)))) }
        var body = Self.message(1, targeting)
        // One ApkSet for each module, in the order the modules first appear.
        var order: [String] = []
        for apk in variant.apks where !order.contains(apk.module) { order.append(apk.module) }
        for module in order {
            let members = variant.apks.filter { $0.module == module }
            var set = Self.message(1, Self.string(1, module))
            for apk in members { set += Self.description(apk, siblings: members) }
            body += Self.message(2, set)
        }
        if let number = variant.number { body += Self.int(3, number) }
        return Self.message(1, body)
    }

    func build() -> Data {
        var data = Data()
        for (index, entry) in variants.enumerated() { data += variantData(entry, index: index) }
        for module in assetModules { data += Self.assetSet(module) }
        data += Self.message(2, Self.string(2, "1.17.1"))
        if let packageName { data += Self.string(4, packageName) }
        return data
    }
}
