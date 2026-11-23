// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A reader of protocol buffer messages: the wire format only (varints,
/// fixed-width values and length-delimited fields), with every length
/// checked against the bytes that remain. The input is hostile, so a field it
/// cannot make sense of ends the read with `BundleTable.Failure` and nothing in
/// it allocates by what a field says.
struct ProtoReader {
    enum Value {
        case varint(UInt64)
        /// A fixed-width value (32 or 64 bits), little-endian.
        case fixed(UInt64)
        case bytes(Data)
    }

    private let data: Data
    private var position: Int

    init(_ data: Data) {
        self.data = data
        position = data.startIndex
    }

    /// The next field of the message: its number and value; nil at the end.
    mutating func next() throws -> (number: Int, value: Value)? {
        guard position < data.endIndex else { return nil }
        let key = try varint()
        let number = Int(truncatingIfNeeded: key >> 3)
        guard number > 0, number < (1 << 29) else { throw BundleTable.Failure() }
        switch key & 7 {
        case 0:
            return (number, .varint(try varint()))
        case 1:
            return (number, .fixed(try fixedWidth(8)))
        case 2:
            let length = try varint()
            guard length <= UInt64(data.endIndex - position) else { throw BundleTable.Failure() }
            let end = position + Int(length)
            defer { position = end }
            return (number, .bytes(data.subdata(in: position..<end)))
        case 5:
            return (number, .fixed(try fixedWidth(4)))
        default:
            // Groups are gone from the format; the rest are not wire types.
            throw BundleTable.Failure()
        }
    }

    private mutating func fixedWidth(_ count: Int) throws -> UInt64 {
        guard count <= data.endIndex - position else { throw BundleTable.Failure() }
        var value: UInt64 = 0
        for index in 0..<count { value |= UInt64(data[position + index]) << UInt64(8 * index) }
        position += count
        return value
    }

    private mutating func varint() throws -> UInt64 {
        var result: UInt64 = 0
        for shift in stride(from: 0, to: 70, by: 7) {
            guard position < data.endIndex else { throw BundleTable.Failure() }
            let byte = data[position]
            position += 1
            if shift == 63, byte > 1 { throw BundleTable.Failure() }
            result |= UInt64(byte & 0x7f) << UInt64(shift)
            if byte & 0x80 == 0 { return result }
        }
        throw BundleTable.Failure()
    }
}

/// The fields of one message that the schema says occur once, seen so far. A
/// second occurrence is merged or replaced by a protocol buffer parser, so a
/// restriction the first one carried would be lost to this reader: it ends the
/// read instead.
private struct Once {
    private var seen = Set<Int>()

    mutating func callAsFunction(_ number: Int) throws {
        guard seen.insert(number).inserted else { throw BundleTable.Failure() }
    }
}

/// The table of contents of a bundletool `.apks` (`toc.pb`, a `BuildApksResult`
/// message): the forms of the app the bundle holds (variants), for the version
/// of Android they are for, and the APKs of each; and the asset modules (asset
/// packs) with their delivery and their slices. Only what the choice of one
/// variant and of the packs installed with it needs is read; the rest of the
/// message is skipped.
struct BundleTable: Equatable, Sendable {
    /// The table cannot be read, or holds more than a bundle does.
    struct Failure: Error, Equatable {}

    /// The limits of a table: the message itself is bounded by the caller, and
    /// the parser descends no further than the messages it knows (it skips an
    /// unknown field as bytes), so it has no depth to limit.
    static let maxVariants = 64
    /// The most values and alternatives one dimension of targeting may list.
    static let maxTargetingValues = 64

    typealias Targeting = ApkContainerPlan.Targeting

    struct Apk: Equatable, Sendable {
        var path: String
        /// The split's id (`config.arm64_v8a`); empty for a master split.
        var splitID: String
        var isMaster: Bool
        /// The module the APK is of (`base` for the app itself).
        var module: String
        /// A standalone APK holds the whole app (`universal.apk`).
        var isStandalone: Bool
        /// The Android and the processor the APK is for, with the siblings'.
        var targeting: Targeting
    }

    struct Variant: Equatable, Sendable {
        var number: Int
        /// What the variant is for, as a whole. A kind of targeting this reader
        /// does not weigh (a screen density, a texture format, several
        /// processors together, a runtime) is `other`: the variant is not chosen.
        var targeting = Targeting()
        var apks: [Apk]
    }

    /// How an asset module (asset pack) reaches a device (`DeliveryType`).
    enum Delivery: Equatable, Sendable {
        /// With the app: `bundletool install-apks` installs it in the same session.
        case installTime
        case onDemand
        case fastFollow
        /// Not stated: not known to be install-time.
        case unknown
    }

    /// One slice of an asset module: an APK of the module that carries its files.
    struct AssetSlice: Equatable, Sendable {
        var path: String
        /// The split's id; empty for none.
        var splitID: String
        /// The Android and the processor the slice is for, with the siblings'.
        var targeting: Targeting
    }

    struct AssetModule: Equatable, Sendable {
        var name: String
        var delivery: Delivery
        var slices: [AssetSlice]
        /// Some description of the module is not an APK slice this install can send
        /// (it names no split metadata, or a path that is not an APK).
        var hasUnusableSlice: Bool
    }

    var variants: [Variant]
    var assetModules: [AssetModule] = []

    /// The most asset modules and asset slices a table may name.
    static let maxAssetModules = 256

    /// `AbiAlias` of bundletool's targeting messages, in Android's spelling.
    private static let abiNames: [Int: String] = [
        1: "armeabi", 2: "armeabi-v7a", 3: "arm64-v8a", 4: "x86", 5: "x86_64", 6: "mips", 7: "mips64", 8: "riscv64",
    ]

    init(_ data: Data) throws {
        var variants: [Variant] = []
        var modules: [AssetModule] = []
        var slices = 0
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            if number == 1 {
                guard variants.count < Self.maxVariants else { throw Failure() }
                variants.append(try Self.variant(bytes, index: variants.count))
            } else if number == 3 {
                // `asset_slice_set`: one asset module and its slices.
                guard modules.count < Self.maxAssetModules else { throw Failure() }
                let module = try Self.assetModule(bytes)
                slices += module.slices.count
                guard slices <= ApkContainer.maxParts else { throw Failure() }
                modules.append(module)
            }
        }
        self.variants = variants
        assetModules = modules
    }

    // MARK: Messages

    private static func variant(_ data: Data, index: Int) throws -> Variant {
        var result = Variant(number: index, apks: [])
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            switch (number, value) {
            case let (1, .bytes(bytes)):
                try once(1)
                result.targeting = try variantTargeting(bytes)
            case let (2, .bytes(bytes)):
                try apkSet(bytes, into: &result)
            case let (3, .varint(raw)):
                try once(3)
                result.number = Int(truncatingIfNeeded: min(raw, UInt64(Int32.max)))
            default:
                continue
            }
        }
        return result
    }

    /// Whether a field this reader has no name for holds anything: a restriction
    /// it cannot weigh.
    private static func holds(_ value: ProtoReader.Value) -> Bool {
        switch value {
        case let .bytes(bytes): !bytes.isEmpty
        case let .varint(raw), let .fixed(raw): raw != 0
        }
    }

    /// `VariantTargeting`: the Android (field 1) and the processor (field 2) are
    /// weighed; any other field that holds something makes the variant one for
    /// something else, whatever its wire type. A field of the schema that is not
    /// a message, or one that comes twice, is a table that cannot be read.
    private static func variantTargeting(_ data: Data) throws -> Targeting {
        var targeting = Targeting()
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard (1...6).contains(number) else {
                if holds(value) { targeting.other = true }
                continue
            }
            try once(number)
            guard case let .bytes(bytes) = value else { throw Failure() }
            switch number {
            case 1: try sdkTargeting(bytes, into: &targeting)
            case 2: try abiTargeting(bytes, into: &targeting)
            default: if !bytes.isEmpty { targeting.other = true }
            }
        }
        return targeting
    }

    /// `ApkTargeting`: the processor (field 1) and the Android (field 5) are
    /// weighed. The language (3) and the screen density (4) are not: every such
    /// split is installed, which the package manager takes and picks from by
    /// itself. The texture format (6) is noted, since an asset slice with one is
    /// not installed (`ApkContainerPlan.selecting`). Any other kind of targeting
    /// that holds something is `other`.
    private static func apkTargeting(_ data: Data) throws -> Targeting {
        var targeting = Targeting()
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard number == 1 || (3...11).contains(number) else {
                if holds(value) { targeting.other = true }
                continue
            }
            try once(number)
            guard case let .bytes(bytes) = value else { throw Failure() }
            switch number {
            case 1: try abiTargeting(bytes, into: &targeting)
            case 5: try sdkTargeting(bytes, into: &targeting)
            case 3, 4: continue
            case 6: if !bytes.isEmpty { targeting.texture = true }
            default: if !bytes.isEmpty { targeting.other = true }
            }
        }
        // A split holds the code of one processor.
        if targeting.abis.count > 1 { targeting.other = true }
        return targeting
    }

    /// `SdkVersionTargeting`: the value (field 1; at most one) and the
    /// alternatives (field 2), each an `SdkVersion` whose `min` (field 1) is an
    /// `Int32Value`. A level that is not stated is 0, as bundletool reads it.
    private static func sdkTargeting(_ data: Data, into targeting: inout Targeting) throws {
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard number == 1 || number == 2 else {
                if holds(value) { targeting.other = true }
                continue
            }
            guard case let .bytes(version) = value else { throw Failure() }
            let level = try sdkMinimum(version)
            if number == 1 {
                guard targeting.sdk.isEmpty else { throw Failure() }
                targeting.sdk.append(level)
            } else {
                guard targeting.sdkAlternatives.count < maxTargetingValues else { throw Failure() }
                targeting.sdkAlternatives.append(level)
            }
        }
    }

    /// `SdkVersion.min.value`; 0 when the version names none.
    private static func sdkMinimum(_ data: Data) throws -> Int {
        var minimum = 0
        var once = Once()
        var reader = ProtoReader(data)
        while let (field, boxed) = try reader.next() {
            // The one field of `SdkVersion` is `min`; anything else that holds something is a restriction unread.
            guard field == 1 else {
                if holds(boxed) { throw Failure() }
                continue
            }
            try once(1)
            guard case let .bytes(wrapper) = boxed else { throw Failure() }
            var wrapped = ProtoReader(wrapper)
            var inner = Once()
            while let (leaf, number) = try wrapped.next() {
                guard leaf == 1 else {
                    if holds(number) { throw Failure() }
                    continue
                }
                try inner(1)
                guard case let .varint(raw) = number else { throw Failure() }
                minimum = Int(truncatingIfNeeded: min(raw, UInt64(Int32.max)))
            }
        }
        return minimum
    }

    /// `AbiTargeting`: the values (field 1) and the alternatives (field 2), each
    /// an `Abi` whose `alias` (field 1) names a processor. An alias this reader
    /// has no name for, in either list, makes the targeting `unknownAbi`: what
    /// has it is never installed, and is never taken for no restriction. A
    /// processor that is both a value and an alternative contradicts itself.
    private static func abiTargeting(_ data: Data, into targeting: inout Targeting) throws {
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard number == 1 || number == 2 else {
                if holds(value) { targeting.other = true }
                continue
            }
            guard case let .bytes(abi) = value else { throw Failure() }
            var alias: UInt64 = 0
            var once = Once()
            var inner = ProtoReader(abi)
            while let (field, item) = try inner.next() {
                guard field == 1 else {
                    if holds(item) { targeting.other = true }
                    continue
                }
                try once(1)
                guard case let .varint(raw) = item else { throw Failure() }
                alias = raw
            }
            let name = abiNames[Int(truncatingIfNeeded: min(alias, 1000))]
            guard targeting.abis.count + targeting.abiAlternatives.count < maxTargetingValues else { throw Failure() }
            if name == nil { targeting.unknownAbi = true }
            if number == 1 {
                if let name { targeting.abis.append(name) }
            } else if let name {
                targeting.abiAlternatives.append(name)
            }
        }
        guard Set(targeting.abis).isDisjoint(with: targeting.abiAlternatives) else { throw Failure() }
    }

    /// `AssetSliceSet`: the module's metadata (field 1: its name is field 1 and
    /// its delivery type field 4) and its slices (field 2, `ApkDescription`s).
    private static func assetModule(_ data: Data) throws -> AssetModule {
        var module = AssetModule(name: "", delivery: .unknown, slices: [], hasUnusableSlice: false)
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            if number == 1 {
                try once(1)
                var single = Once()
                var inner = ProtoReader(bytes)
                while let (field, item) = try inner.next() {
                    if field == 1, case let .bytes(text) = item {
                        try single(1)
                        module.name = String(decoding: text, as: UTF8.self)
                    }
                    if field == 4, case let .varint(raw) = item {
                        try single(4)
                        switch raw {
                        case 1: module.delivery = .installTime
                        case 2: module.delivery = .onDemand
                        case 3: module.delivery = .fastFollow
                        default: module.delivery = .unknown
                        }
                    }
                }
            } else if number == 2 {
                guard module.slices.count < ApkContainer.maxParts else { throw Failure() }
                if let slice = try assetSlice(bytes) { module.slices.append(slice) } else { module.hasUnusableSlice = true }
            }
        }
        return module
    }

    /// An `ApkDescription` of an asset slice: its path (2), its targeting (1) and
    /// the split metadata that marks it an asset slice (7: the split's id). A
    /// description of another kind is nil.
    private static func assetSlice(_ data: Data) throws -> AssetSlice? {
        var path: String?
        var targeting = Targeting()
        var split: String?
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            switch number {
            case 1:
                try once(1)
                targeting = try apkTargeting(bytes)
            case 2:
                try once(2)
                guard let text = String(data: bytes, encoding: .utf8) else { throw Failure() }
                path = text
            case 7:
                try once(7)
                var id = ""
                var single = Once()
                var inner = ProtoReader(bytes)
                while let (field, item) = try inner.next() {
                    if field == 1, case let .bytes(text) = item {
                        try single(1)
                        id = String(decoding: text, as: UTF8.self)
                    }
                }
                split = id
            default:
                continue
            }
        }
        guard let path else { throw Failure() }
        guard let split, path.lowercased().hasSuffix(".apk") else { return nil }
        return AssetSlice(path: path, splitID: split, targeting: targeting)
    }

    /// `ApkSet`: a module's metadata (field 1, the name its field 1) and its APKs (field 2).
    private static func apkSet(_ data: Data, into variant: inout Variant) throws {
        var module = ""
        var descriptions: [Data] = []
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            if number == 1 {
                try once(1)
                var single = Once()
                var inner = ProtoReader(bytes)
                while let (field, name) = try inner.next() {
                    if field == 1, case let .bytes(text) = name {
                        try single(1)
                        module = String(decoding: text, as: UTF8.self)
                    }
                }
            } else if number == 2 {
                guard variant.apks.count + descriptions.count < ApkContainer.maxParts else { throw Failure() }
                descriptions.append(bytes)
            }
        }
        for description in descriptions {
            if let apk = try apkDescription(description, module: module) { variant.apks.append(apk) }
        }
    }

    /// `ApkDescription`: the targeting (field 1), the path (2), and either split metadata
    /// (3: the split's id, and whether it is the module's master) or standalone metadata (4).
    /// An APK of any other kind (instant, system) is not installed here.
    private static func apkDescription(_ data: Data, module: String) throws -> Apk? {
        var path: String?
        var targeting = Targeting()
        var split: (id: String, master: Bool)?
        var standalone = false
        var once = Once()
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            switch number {
            case 1:
                try once(1)
                targeting = try apkTargeting(bytes)
            case 2:
                try once(2)
                guard let text = String(data: bytes, encoding: .utf8) else { throw Failure() }
                path = text
            case 3:
                try once(3)
                var id = ""
                var master = false
                var single = Once()
                var inner = ProtoReader(bytes)
                while let (field, item) = try inner.next() {
                    if field == 1, case let .bytes(text) = item {
                        try single(1)
                        id = String(decoding: text, as: UTF8.self)
                    }
                    if field == 2, case let .varint(flag) = item {
                        try single(2)
                        master = flag != 0
                    }
                }
                split = (id, master)
            case 4:
                try once(4)
                standalone = true
            default:
                continue
            }
        }
        guard let path else { throw Failure() }
        guard split != nil || standalone else { return nil }
        return Apk(
            path: path, splitID: split?.id ?? "", isMaster: split?.master ?? false, module: module, isStandalone: standalone,
            targeting: targeting)
    }
}
