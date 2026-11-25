// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Publisher filenames, byte counts and checksums from the MysticGSI release feed.
public struct MysticSkin: Identifiable, Sendable {
    public let name: String
    public let path: String
    public let bytes: Int64
    public let md5: String
    public var id: String { path }
    public var url: URL {
        URL(string: "https://downloads.sourceforge.net/project/mystic-gsi-updates/")!.appendingPathComponent(path)
    }

    public static let catalog = [
        Self(name: "One UI", path: "OneUI/Oneui-gts9wifi-16-X710ZCS5DZA1-AB-20260211-MysticGSI.zip",
             bytes: 3_116_028_143, md5: "b35eb30fd519a57ad4d2d3cbfec30be8"),
        Self(name: "HyperOS", path: "HyperOS/Hyperos-pudding-16-OS3.0.50.2.W-AB-20260122-MysticGSI.zip",
             bytes: 3_994_747_809, md5: "9eb03ec3f211365aecbaa346be9516b5"),
        Self(name: "ColorOS", path: "ColorOS/Coloros-OP64DDL1-16-1769698117230-AB-20260315-MysticGSI.zip",
             bytes: 4_527_871_680, md5: "4b77c93824bc1200d3ad1c11d60522d6")
    ]
}

public enum SkinImageError: LocalizedError {
    case archive, missingSystem, ramdisk
    public var errorDescription: String? {
        switch self {
        case .archive: "The ZIP system image is damaged or uses an unsupported compression method."
        case .missingSystem: "Choose a ZIP containing a single system.img, system.img.xz, or system.img.gz."
        case .ramdisk: "This Andarium boot image cannot be configured to mount an ext4 system."
        }
    }
}

/// Locate only the system mount in the carried, uncompressed newc boot ramdisk.
/// Padding "ext4" to five bytes preserves the boot header and CPIO entry offsets.
public func ext4MountOffset(for partition: String, in ramdisk: Data) throws -> Int {
    guard ["system", "system_ext", "vendor", "odm", "product"].contains(partition), ramdisk.startIndex == 0 else { throw SkinImageError.ramdisk }
    func field(_ start: Int) throws -> Int {
        guard let number = Int(String(decoding: ramdisk[start..<start+8], as: UTF8.self), radix: 16),
              number >= 0 else { throw SkinImageError.ramdisk }
        return number
    }
    var pos = 0
    while pos + 110 <= ramdisk.count {
        guard ramdisk[pos..<pos+6] == Data("070701".utf8) else { throw SkinImageError.ramdisk }
        let size = try field(pos + 54), nameSize = try field(pos + 94)
        let name = pos + 110, end = name + nameSize, contents = (end + 3) & ~3
        guard nameSize > 0, end <= ramdisk.count, ramdisk[end-1] == 0,
              contents + size <= ramdisk.count else { throw SkinImageError.ramdisk }
        let path = String(decoding: ramdisk[name..<end-1], as: UTF8.self)
        if path == "first_stage_ramdisk/system/etc/fstab.uml" {
            let mount = Data("/dev/block/by-name/\(partition) /\(partition) ".utf8)
            let line = mount + Data("erofs ro wait,first_stage_mount\n".utf8)
            guard let match = ramdisk.range(of: line, in: contents..<contents+size),
                  match.lowerBound == contents || ramdisk[match.lowerBound-1] == 10 else { throw SkinImageError.ramdisk }
            return match.lowerBound + mount.count
        }
        if path == "TRAILER!!!" { break }
        pos = (contents + size + 3) & ~3
    }
    throw SkinImageError.ramdisk
}
