// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

struct MysticSkinTests {
    @Test func zipFindsNestedSystemMemberUsingCentralDirectorySizes() throws {
        let zip = try makeZIP(["boot.img", "images/system.img.xz"], zip64: true)
        defer { try? FileManager.default.removeItem(at: zip) }
        let member = try SystemImageZIP.read(zip)
        #expect(member.fileName == "system.img.xz")
        #expect(member.offset > 30)
        #expect(member.compressedBytes == 4)
        #expect(member.imageBytes == 6_308_233_216)
        #expect(member.compression == 8)
        #expect(member.crc == 0x12345678)
    }

    @Test func zipRejectsMissingAndAmbiguousSystemImages() throws {
        for names in [["payload.bin"], ["system.img", "other/system.img"]] {
            let zip = try makeZIP(names)
            defer { try? FileManager.default.removeItem(at: zip) }
            #expect(throws: SkinImageError.missingSystem) { try SystemImageZIP.read(zip) }
        }
    }

    // A synthetic ZIP with descriptor flags, so neither size comes from the
    // local header. The payload is never decoded by these directory tests.
    private func makeZIP(_ names: [String], zip64: Bool = false) throws -> URL {
        var zip = Data(), directory = Data()
        func field(_ value: UInt64, _ bytes: Int) -> Data {
            Data((0..<bytes).map { UInt8(truncatingIfNeeded: value >> ($0 * 8)) })
        }
        for path in names {
            let start = UInt64(zip.count)
            let name = Data(path.utf8)
            let size = UInt64(name.count)
            zip += field(0x04034b50, 4) + field(45, 2) + field(8, 2) + field(8, 2)
            zip += Data(repeating: 0, count: 16) + field(size, 2) + field(0, 2)
            zip += name + field(0, 4)
            directory += field(0x02014b50, 4) + field(45, 2) + field(45, 2)
            directory += field(8, 2) + field(8, 2) + field(0, 4)
            directory += field(0x12345678, 4) + field(4, 4) + field(zip64 ? 0xffffffff : 5, 4)
            directory += field(size, 2) + field(zip64 ? 12 : 0, 2)
            directory += Data(repeating: 0, count: 10) + field(start, 4) + name
            if zip64 {
                directory += field(1, 2) + field(8, 2) + field(6_308_233_216, 8)
            }
        }
        let offset = UInt64(zip.count), count = UInt64(names.count)
        zip += directory + field(0x06054b50, 4) + field(0, 4) + field(count, 2) + field(count, 2)
        zip += field(UInt64(directory.count), 4) + field(offset, 4) + field(0, 2)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try zip.write(to: url)
        return url
    }

    @Test func ext4MountUsesOnlyTheFirstStageFstab() throws {
        func entry(_ name: String, _ payload: String) -> Data {
            let sizes = [1, 0o100644, 0, 0, 1, 0, payload.utf8.count, 0, 0, 0, 0, name.utf8.count + 1, 0]
            var data = Data(("070701" + sizes.map { String(format: "%08x", $0) }.joined() + name).utf8)
            data.append(0)
            while !data.count.isMultiple(of: 4) { data.append(0) }
            data.append(Data(payload.utf8))
            while !data.count.isMultiple(of: 4) { data.append(0) }
            return data
        }
        let mount = "/dev/block/by-name/system /system erofs ro wait,first_stage_mount\n"
        let decoy = entry("other", mount)
        let archive = decoy + entry("first_stage_ramdisk/system/etc/fstab.uml", mount)
        let offset = try ext4MountOffset(for: "system", in: archive)
        #expect(offset > decoy.count)
        #expect(archive[offset..<offset+5] == Data("erofs".utf8))
        #expect(throws: SkinImageError.self) { try ext4MountOffset(for: "vendor", in: archive) }
        #expect(throws: SkinImageError.self) { try ext4MountOffset(for: "system", in: decoy) }
    }
}
