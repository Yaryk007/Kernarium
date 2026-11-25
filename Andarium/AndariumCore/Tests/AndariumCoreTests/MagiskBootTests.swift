// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct MagiskBootTests {
    private let binaries = MagiskBoot.Binaries(
        initBinary: Data("magiskinit".utf8), magisk: Data("magisk".utf8), loader: Data("loader".utf8))

    private func entry(_ name: String, inode: Int, mode: Int, _ payload: String = "") -> [UInt8] {
        func padded(_ bytes: [UInt8]) -> [UInt8] { bytes + [UInt8](repeating: 0, count: (4 - bytes.count % 4) % 4) }
        let fields = [inode, mode, 0, 0, 1, 0, payload.utf8.count, 0, 0, 0, 0, name.utf8.count + 1, 0]
        let header = "070701" + fields.map { String(format: "%08X", $0) }.joined()
        return padded(Array((header + name).utf8) + [0]) + padded(Array(payload.utf8))
    }

    private func image(_ ramdisk: [UInt8]) -> Data {
        var header = [UInt8](repeating: 0, count: 4096)
        header.replaceSubrange(0..<8, with: Array("ANDROID!".utf8))
        for byte in 0..<4 { header[12 + byte] = UInt8(truncatingIfNeeded: ramdisk.count >> (8 * byte)) }
        header[40] = 4
        return Data(header + ramdisk)
    }

    private func stock(extra: [UInt8] = []) -> Data {
        image(entry("init", inode: 1, mode: 0o100755, "stock-init") + entry("dev", inode: 2, mode: 0o040755) + extra
            + entry("TRAILER!!!", inode: 0, mode: 0))
    }

    @Test func thePatchReplacesInitAndAddsMagisksEntries() throws {
        let patched = try MagiskBoot.patch(stock: stock(), binaries: binaries, preinit: "ubdb3", stockSHA1: "abc")
        #expect(patched.count % 4096 == 0)
        #expect(patched.prefix(12) == stock().prefix(12))
        let text = String(decoding: patched, as: UTF8.self)
        for name in [".backup/.magisk", ".backup/.rmlist", ".backup/init.xz", "overlay.d/sbin/magisk.xz", "overlay.d/sbin/init-ld.xz"] {
            #expect(text.contains(name))
        }
        #expect(text.contains("PREINITDEVICE=ubdb3\nSHA1=abc\n"))
        #expect(text.contains("magiskinit"))
        #expect(throws: MagiskBoot.Problem.alreadyPatched) {
            try MagiskBoot.patch(stock: patched, binaries: binaries, preinit: "ubdb3", stockSHA1: "abc")
        }
    }

    @Test func anImageItCannotReadIsRefused() {
        let twoArchives = stock(extra: entry("TRAILER!!!", inode: 0, mode: 0))
        let compressed = image(Array("\u{2}!L compressed".utf8))
        for refused in [Data(), Data("ANDROID!".utf8), compressed, twoArchives] {
            #expect(throws: MagiskBoot.Problem.unsupportedImage) {
                try MagiskBoot.patch(stock: refused, binaries: binaries, preinit: "ubdb3", stockSHA1: "abc")
            }
        }
    }

    @Test func theStoredStreamIsAnXZStream() {
        let stream = MagiskBoot.storedXZ(Array("abc".utf8))
        #expect(Array(stream.prefix(6)) == [0xfd, 0x37, 0x7a, 0x58, 0x5a, 0])
        #expect(Array(stream.suffix(2)) == [0x59, 0x5a])
        #expect(stream.count % 4 == 0)
    }
}
