// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation

/// Downloads on request and turns a GSI into a guest with the r28 boot and
/// vendor partitions. An existing r28 installation can supply those files.
nonisolated enum GuestSystemInstaller {
    @concurrent static func install(
        _ choice: GuestSystemChoice, address: String, local: URL?, at destination: URL,
        report: @escaping @MainActor @Sendable (String) -> Void
    ) async throws {
        let manager = FileManager.default
        let work = manager.temporaryDirectory.appendingPathComponent("andarium-system-" + UUID().uuidString)
        try manager.createDirectory(at: work, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: work) }

        let image: URL
        if let local {
            image = local
        } else {
            let address = try sourceURL(for: choice, custom: address)
            await report("Downloading \(choice.title)…")
            image = try await download(address, to: work.appendingPathComponent("download"),
                                       expectedBytes: choice == .lineage ? SystemInstallation.officialBytes : choice.skin?.bytes)
        }

        if choice == .lineage {
            await report("Installing LineageOS r28…")
            try SystemInstallation.install(image, choice: choice, at: destination)
            return
        }
        if let skin = choice.skin {
            await report("Verifying the MysticGSI download…")
            try verify(image, skin: skin)
        }

        let kind = try SystemImageImport.kind(of: image)
        var input = image
        if kind == .zip {
            let zip = try ZipArchive(source: ZipFileSource(contentsOf: image), checkpoint: { try Task.checkCancellation() })
            if choice == .custom, zip.entries.contains(where: {
                $0.name == SystemFolder.manifest || $0.name.hasSuffix("/" + SystemFolder.manifest)
            }) {
                await report("Installing the Andarium guest…")
                try SystemInstallation.install(image, choice: .custom, at: destination)
                return
            }
            await report("Unpacking the ROM ZIP…")
            input = try SystemImageImport.extractZIP(zip, into: work) { _, _ in }
            if local == nil { try manager.removeItem(at: image) }
        }

        var layoutBase = SystemInstallation.directory(for: .lineage)
        if SystemInstallation.selection(in: layoutBase) != .lineage {
            await report("Downloading the LineageOS r28 boot and vendor base…")
            let archive = try await download(SystemInstallation.officialURL, to: work.appendingPathComponent("lineage.ipa"),
                                             expectedBytes: SystemInstallation.officialBytes)
            await report("Verifying the LineageOS r28 base…")
            layoutBase = work.appendingPathComponent("base", isDirectory: true)
            try SystemInstallation.install(archive, choice: .lineage, at: layoutBase)
            try manager.removeItem(at: archive)
        }

        await report("Preparing the Android system disk…")
        try assemble(input, choice: choice, base: layoutBase, destination: destination, work: work)
    }

    private static func sourceURL(for choice: GuestSystemChoice, custom: String) throws -> URL {
        if choice == .lineage { return SystemInstallation.officialURL }
        if let skin = choice.skin { return skin.url }
        return try SystemInstallation.customURL(custom)
    }

    private static func download(_ url: URL, to target: URL, expectedBytes: Int64?) async throws -> URL {
        if let expectedBytes { try SystemImageImport.requireSpace(UInt64(expectedBytes), at: target.deletingLastPathComponent()) }
        let (file, response) = try await URLSession.shared.download(from: url)
        defer { try? FileManager.default.removeItem(at: file) }
        guard let http = response as? HTTPURLResponse, http.statusCode == 200,
              let final = http.url, SystemInstallation.isHTTPS(final),
              let actual = try file.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              actual > 0, actual <= SystemInstallation.diskLimit,
              expectedBytes == nil || Int64(actual) == expectedBytes
        else { throw SystemInstallation.Problem.download }
        try FileManager.default.moveItem(at: file, to: target)
        try Task.checkCancellation()
        return target
    }

    private static func verify(_ image: URL, skin: MysticSkin) throws {
        let input = try FileHandle(forReadingFrom: image)
        defer { try? input.close() }
        var hash = Insecure.MD5()
        while let data = try input.read(upToCount: 4 << 20), !data.isEmpty {
            try Task.checkCancellation()
            hash.update(data: data)
        }
        guard hash.finalize().map({ String(format: "%02x", $0) }).joined() == skin.md5 else {
            throw SystemInstallation.Problem.checksum
        }
    }

    private static func assemble(
        _ source: URL, choice: GuestSystemChoice, base: URL, destination: URL, work: URL
    ) throws {
        let manager = FileManager.default
        guard !manager.fileExists(atPath: destination.path) else { throw SystemInstallation.Problem.alreadyInstalled }
        let carried = try SystemImageImport.carriedLayout(in: base)
        var input = source
        var kind = try SystemImageImport.kind(of: input)
        for round in 0..<3 where [.xz, .gzip, .sparse].contains(kind) {
            let unpacked = work.appendingPathComponent("unpacked-\(round).img")
            try SystemImageImport.unpack(input, kind: kind, into: unpacked) { _, _ in }
            if input.deletingLastPathComponent() == work { try manager.removeItem(at: input) }
            input = unpacked
            kind = try SystemImageImport.kind(of: input)
        }
        let size = UInt64(try input.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0)
        guard size > 0, size <= SystemInstallation.diskLimit else { throw SystemInstallation.Problem.incompatible }

        var stage = destination.deletingLastPathComponent()
            .appendingPathComponent(".android-install-" + UUID().uuidString, isDirectory: true)
        let disk = stage.appendingPathComponent(SystemFolder.systemDisk)
        try manager.createDirectory(at: disk.deletingLastPathComponent(), withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: stage) }
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        try stage.setResourceValues(excluded)
        var layout: SystemImageImport.DiskLayout
        switch kind {
        case .erofs, .ext4:
            let target = choice == .custom
                ? SystemImageImport.targetPartition(for: source.lastPathComponent, in: carried.disk) : "system"
            guard let partition = carried.disk.partitions.first(where: { $0.name == target })
            else { throw SystemInstallation.Problem.incompatible }
            try SystemImageImport.requireSpace(carried.disk.size - partition.size
                                               + SystemImageImport.alignUp(size, carried.disk.alignment), at: stage)
            layout = try SystemImageImport.assemble(
                replacing: target, with: input, carried: carried.disk,
                carriedDisk: base.appendingPathComponent(SystemFolder.systemDisk), into: disk
            ) { _, _ in }
            if kind == .ext4 { try SystemImageImport.mountExt4(target, disk: disk, layout: layout) }
        case .gptDisk:
            layout = try SystemImageImport.readGPT(input, size: size, alignment: carried.disk.alignment)
            if let problem = SystemImageImport.hostProblem(layout) { throw SystemInstallation.Problem.invalid(problem) }
            try SystemImageImport.requireSpace(size, at: stage)
            try manager.copyItem(at: input, to: disk)
        default:
            throw SystemInstallation.Problem.invalid("Import an ext4 or EROFS system image, a ROM ZIP, or an Andarium GPT system disk.")
        }
        let hash = try SystemImageImport.hash(disk, layout: &layout) { _, _ in }
        var document = carried.document
        document["system_disk"] = SystemImageImport.json(layout, wholeSHA256: hash)
        try JSONSerialization.data(withJSONObject: document, options: [.sortedKeys]).write(to: stage.appendingPathComponent(SystemFolder.layout))

        let command = stage.appendingPathComponent(SystemFolder.commandLine)
        try manager.createDirectory(at: command.deletingLastPathComponent(), withIntermediateDirectories: true)
        try manager.copyItem(at: base.appendingPathComponent(SystemFolder.commandLine), to: command)
        let metadata = try Data(contentsOf: base.appendingPathComponent(SystemFolder.manifest))
        try manifest(metadata, choice: choice, hash: hash).write(to: stage.appendingPathComponent(SystemFolder.manifest))
        _ = try SystemFolder.release(in: stage)
        try choice.rawValue.write(to: stage.appendingPathComponent(SystemInstallation.selectionFile), atomically: true, encoding: .utf8)
        try Task.checkCancellation()
        try manager.moveItem(at: stage, to: destination)
    }

    private static func manifest(_ base: Data, choice: GuestSystemChoice, hash: String) throws -> Data {
        guard var manifest = try JSONSerialization.jsonObject(with: base) as? [String: Any],
              var android = manifest["android"] as? [String: Any] else { throw SystemInstallation.Problem.incompatible }
        let release = "\(choice.storageName)-\(hash.prefix(12))"
        manifest["release_id"] = release
        manifest["product_family"] = choice.storageName
        manifest.removeValue(forKey: "root")
        manifest.removeValue(forKey: "inventory")
        manifest.removeValue(forKey: "acceptance")
        if choice.skin != nil { android["major"] = 16 }
        android["fingerprint"] = "andarium/\(choice.storageName)/\(release)"
        android["build_number"] = 1
        android["security_patch"] = ""
        android.removeValue(forKey: "build_id")
        manifest["android"] = android
        return try JSONSerialization.data(withJSONObject: manifest, options: [.sortedKeys])
    }
}
