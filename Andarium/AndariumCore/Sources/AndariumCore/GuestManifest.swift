// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a release's manifest must say before Andarium installs it.
public enum GuestManifest {
    public enum Kind: Equatable, Sendable {
        /// The official LineageOS release of this ID.
        case official(release: String)
        /// LineageOS with GApps made from the official release of this ID.
        case gapps(base: String)
        /// A guest of one product family, or, with none, of any.
        case other(family: String?)
    }

    /// The host interface of the official release, and of the guests Andarium
    /// makes from other images.
    static let officialABI = 26
    static let otherABI = 29

    public static func fits(_ manifest: Data, as kind: Kind) -> Bool {
        guard let root = try? JSONSerialization.jsonObject(with: manifest) as? [String: Any],
              let family = root["product_family"] as? String, let release = root["release_id"] as? String,
              let android = root["android"] as? [String: Any], android["major"] as? Int == 17,
              (android["abi_list"] as? [String])?.contains("arm64-v8a") == true,
              let kernel = root["kernel"] as? [String: Any], kernel["page_size"] as? Int == 4096,
              (kernel["release"] as? String)?.hasPrefix("6.18.") == true
        else { return false }
        // A family may go on to name its kernel: lineage-24-mainline-6.18-4k.
        func isFamily(_ name: String) -> Bool { family == name || family.hasPrefix(name + "-") }
        let abi = kernel["abi_version"] as? Int
        switch kind {
        case .official(let expected):
            return isFamily("lineage-24") && !isFamily("lineage-24-gapps") && release == expected && abi == officialABI
        case .gapps(let base):
            return isFamily("lineage-24-gapps") && abi == officialABI
                && root["base_release_id"] as? String == base && release != base
        case .other(let name):
            return (name.map(isFamily) ?? true) && abi == otherABI
        }
    }
}
