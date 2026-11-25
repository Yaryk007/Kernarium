// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation

/// Gives an installed Android system the root files its release came without:
/// the official Magisk app, downloaded from its own release and checked
/// against the pinned digest (android/lineage-24/metadata/magisk.env), and the
/// system's own `init_boot` with Magisk patched in.
nonisolated enum MagiskInstaller {
    static let address = URL(string: "https://github.com/topjohnwu/Magisk/releases/download/v31.1/Magisk-v31.1.apk")!
    static let versionName = "v31.1"
    static let versionCode = 31100
    static let bytes: Int64 = 10_679_283
    static let sha256 = "89cefac0a318afa39fb3880c45c87b036c05bc1ebf02a7a715b7f8d7c26ac6e7"
    static let apkPath = "root/Magisk-v31.1.apk"
    private static let memberLimit = 64 * 1024 * 1024
    private static let imageLimit: UInt64 = 64 * 1024 * 1024

    /// The release directory becomes a rooted one only with its manifest, which
    /// is written last: an attempt that ends early leaves no root.
    @concurrent static func install(
        in release: URL, report: @escaping @MainActor @Sendable (GuestSystemInstallProgress) -> Void
    ) async throws -> RootRelease {
        let manager = FileManager.default
        let stage = release.appendingPathComponent(".root-install-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: stage, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: stage) }

        let apk = try await GuestSystemInstaller.download(
            address, to: stage.appendingPathComponent((apkPath as NSString).lastPathComponent),
            label: "Downloading Magisk \(versionName)…", expectedBytes: bytes, report: report)
        await report(.working("Patching Android’s boot image…"))
        guard try SystemInstallation.archiveDigest(apk) == sha256 else { throw SystemInstallation.Problem.checksum }
        let archive = try ZipArchive(source: ZipFileSource(contentsOf: apk), checkpoint: { try Task.checkCancellation() })
        func member(_ name: String) throws -> Data {
            guard let entry = archive.entry(named: name) else { throw SystemInstallation.Problem.archive }
            return try archive.data(of: entry, limit: memberLimit)
        }
        let binaries = MagiskBoot.Binaries(
            initBinary: try member(MagiskBoot.Binaries.initMember),
            magisk: try member(MagiskBoot.Binaries.magiskMember),
            loader: try member(MagiskBoot.Binaries.loaderMember))

        let carried = try SystemImageImport.carriedLayout(in: release)
        let stock = try initBoot(of: carried.disk, in: release)
        let patched: Data
        do {
            patched = try MagiskBoot.patch(
                stock: stock, binaries: binaries, preinit: try preinitDevice(carried.document),
                stockSHA1: hex(Insecure.SHA1.hash(data: stock)))
        } catch is MagiskBoot.Problem {
            throw SystemInstallation.Problem.invalid("Magisk can’t be added to this system’s boot image.")
        }
        try patched.write(to: stage.appendingPathComponent((RootRelease.bootImage as NSString).lastPathComponent))
        try Task.checkCancellation()

        let file = release.appendingPathComponent(SystemFolder.manifest)
        guard var manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: file)) as? [String: Any] else {
            throw SystemInstallation.Problem.incompatible
        }
        manifest["root"] = [
            "magisk": ["name": "Magisk", "version_name": versionName, "version_code": versionCode] as [String: Any],
            "apk": ["path": apkPath, "sha256": sha256, "size": bytes] as [String: Any],
            "boot_image": [
                "path": RootRelease.bootImage, "sha256": hex(SHA256.hash(data: patched)), "size": patched.count,
                "stock_init_boot_sha256": hex(SHA256.hash(data: stock)),
                "stock_init_boot_sha1": hex(Insecure.SHA1.hash(data: stock)),
            ] as [String: Any],
        ] as [String: Any]
        let data = try JSONSerialization.data(withJSONObject: manifest, options: [.sortedKeys])

        // What is there without a manifest that names it is an earlier attempt's.
        let root = release.appendingPathComponent("root", isDirectory: true)
        if manager.fileExists(atPath: root.path) { try manager.removeItem(at: root) }
        try manager.moveItem(at: stage, to: root)
        try data.write(to: file, options: .atomic)
        guard let result = RootRelease.carried(in: release, manifest: data) else {
            throw SystemInstallation.Problem.incompatible
        }
        return result
    }

    private static func initBoot(of disk: SystemImageImport.DiskLayout, in release: URL) throws -> Data {
        guard let partition = disk.partitions.first(where: { $0.name == "init_boot" }),
              partition.imageBytes > 0, partition.imageBytes <= imageLimit
        else { throw SystemInstallation.Problem.incompatible }
        let file = try FileHandle(forReadingFrom: release.appendingPathComponent(SystemFolder.systemDisk))
        defer { try? file.close() }
        try file.seek(toOffset: partition.start)
        guard let data = try file.read(upToCount: Int(partition.imageBytes)), data.count == Int(partition.imageBytes) else {
            throw SystemInstallation.Problem.incompatible
        }
        return data
    }

    /// The data disk is the guest's second block device, and Magisk keeps its
    /// modules on userdata.
    private static func preinitDevice(_ layout: [String: Any]) throws -> String {
        let data = layout["data_disk"] as? [String: Any]
        let names = (data?["partitions"] as? [[String: Any]] ?? []).compactMap { $0["name"] as? String }
        guard let index = names.firstIndex(of: "userdata") else { throw SystemInstallation.Problem.incompatible }
        return "ubdb\(index + 1)"
    }

    private static func hex(_ digest: some Digest) -> String {
        digest.map { String(format: "%02x", $0) }.joined()
    }
}
