// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
import zlib

@testable import AndariumCore

/// The ZIP reader behind Install APK's containers: what it reads, and what a
/// hostile archive does not get past.
struct ZipArchiveTests {
    private func archive(_ fixture: ZipFixture, limits: ZipLimits = ZipLimits()) throws -> ZipArchive {
        try ZipArchive(source: ZipDataSource(fixture.build()), limits: limits)
    }

    private func drain(_ reader: ZipEntryReader, piece: Int = 256 * 1024) throws -> Data {
        var all = Data()
        while let part = try reader.read(upTo: piece) { all.append(part) }
        return all
    }

    private func read(_ archive: ZipArchive, _ name: String, piece: Int = 256 * 1024) throws -> Data {
        try drain(try archive.reader(for: #require(archive.entry(named: name)), capacity: piece), piece: piece)
    }

    private func expectError(_ expected: ZipError, _ body: () throws -> Void, sourceLocation: SourceLocation = #_sourceLocation) {
        do {
            try body()
            Issue.record("no error; wanted \(expected)", sourceLocation: sourceLocation)
        } catch let error as ZipError {
            #expect(error == expected, sourceLocation: sourceLocation)
        } catch {
            Issue.record("\(error)", sourceLocation: sourceLocation)
        }
    }

    /// Only the case of the error, not its words.
    private func expectKind(_ matches: (ZipError) -> Bool, _ body: () throws -> Void, sourceLocation: SourceLocation = #_sourceLocation) {
        do {
            try body()
            Issue.record("no error", sourceLocation: sourceLocation)
        } catch let error as ZipError {
            #expect(matches(error), "\(error)", sourceLocation: sourceLocation)
        } catch {
            Issue.record("\(error)", sourceLocation: sourceLocation)
        }
    }

    private func isMalformed(_ error: ZipError) -> Bool { if case .malformed = error { true } else { false } }
    private func isCorrupt(_ error: ZipError) -> Bool { if case .corrupt = error { true } else { false } }
    private func isTooLarge(_ error: ZipError) -> Bool { if case .tooLarge = error { true } else { false } }
    private func isUnsupported(_ error: ZipError) -> Bool { if case .unsupported = error { true } else { false } }

    // MARK: Reading

    @Test func storedAndDeflatedEntriesComeBackWhole() throws {
        let text = Data(String(repeating: "split apk \u{2713} ", count: 40_000).utf8)
        let random = incompressible(300_000)
        let zip = try archive(ZipFixture([
            .init("stored.bin", random, method: 0), .init("text.txt", text), .init("random.bin", random),
            .init("empty.bin", Data(), method: 0), .init("emptyDeflated.bin", Data()),
        ]))
        #expect(zip.entries.map(\.name) == ["stored.bin", "text.txt", "random.bin", "empty.bin", "emptyDeflated.bin"])
        #expect(try read(zip, "stored.bin") == random)
        #expect(try read(zip, "text.txt") == text)
        #expect(try read(zip, "random.bin") == random)
        #expect(try read(zip, "empty.bin").isEmpty)
        #expect(try read(zip, "emptyDeflated.bin").isEmpty)
        #expect(zip.entry(named: "text.txt")?.size == UInt64(text.count))
        #expect(zip.entry(named: "missing") == nil)
    }

    @Test func aPieceIsNeverLongerThanAsked() throws {
        let data = incompressible(70_000)
        let zip = try archive(ZipFixture([.init("a", data, method: 0), .init("b", data)]))
        for name in ["a", "b"] {
            let reader = try zip.reader(for: #require(zip.entry(named: name)), capacity: 4096)
            var total = 0
            while let piece = try reader.read(upTo: 1000) {
                #expect(piece.count <= 1000)
                total += piece.count
            }
            #expect(total == data.count)
            // Past the end it stays at the end.
            #expect(try reader.read(upTo: 1000) == nil)
        }
    }

    @Test func zip64RecordsAreRead() throws {
        let data = incompressible(5000)
        var fixture = ZipFixture([
            .init("one.bin", data, method: 0), .init("two.bin", data), .init("three.bin", data, method: 0),
        ])
        fixture.zip64End = true
        for index in fixture.items.indices { fixture.items[index].zip64 = true }
        let zip = try archive(fixture)
        #expect(zip.entries.count == 3)
        #expect(try read(zip, "one.bin") == data && read(zip, "two.bin") == data && read(zip, "three.bin") == data)
    }

    @Test func aZip64EndRecordAloneIsRead() throws {
        var fixture = ZipFixture([.init("a", Data("hello".utf8), method: 0)])
        fixture.zip64End = true
        let zip = try archive(fixture)
        #expect(try read(zip, "a") == Data("hello".utf8))
    }

    @Test func theDataDescriptorFormIsRead() throws {
        let data = incompressible(20_000)
        var one = ZipFixture.Item("a", data)
        one.descriptor = true
        var two = ZipFixture.Item("b", data, method: 0)
        two.descriptor = true
        var three = ZipFixture.Item("c", data)
        three.descriptor = true
        three.zip64 = true
        let zip = try archive(ZipFixture([one, two, three]))
        #expect(try read(zip, "a") == data && read(zip, "b") == data && read(zip, "c") == data)
    }

    @Test func anEndRecordSignatureInTheCommentIsNotTheEndRecord() throws {
        var fixture = ZipFixture([.init("a", Data("x".utf8), method: 0)])
        var comment = Data("noise ".utf8)
        comment.le(UInt32(0x0605_4b50))
        comment.append(Data(count: 30))
        fixture.comment = comment
        #expect(try read(archive(fixture), "a") == Data("x".utf8))
    }

    @Test func aDirectoryEntryHasNothing() throws {
        let zip = try archive(ZipFixture([.init("lib/", Data(), method: 0), .init("lib/a.so", Data("so".utf8), method: 0)]))
        #expect(zip.entry(named: "lib/")?.isDirectory == true)
        #expect(try read(zip, "lib/").isEmpty)
    }

    @Test func aSmallEntryCanBeReadWholeUpToItsCap() throws {
        let zip = try archive(ZipFixture([.init("manifest.json", Data(String(repeating: "x", count: 1000).utf8))]))
        let entry = try #require(zip.entry(named: "manifest.json"))
        #expect(try zip.data(of: entry, limit: 1000).count == 1000)
        expectKind(isTooLarge, { _ = try zip.data(of: entry, limit: 999) })
    }

    @Test func aFileReadsWithPreadAndLeavesNothingBehind() throws {
        let data = incompressible(100_000)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("zip-\(UUID().uuidString).apks")
        defer { try? FileManager.default.removeItem(at: url) }
        try ZipFixture([.init("a.apk", data, method: 0), .init("b.apk", data)]).build().write(to: url)
        let zip = try ZipArchive(source: ZipFileSource(contentsOf: url))
        #expect(try read(zip, "a.apk") == data && read(zip, "b.apk") == data)
    }

    /// Archives as the system's own zip(1) writes them, which the fixture's
    /// writer cannot vouch for: stored, deflated, forced ZIP64, and the form a
    /// pipe makes (sizes after the data).
    @Test func archivesFromTheSystemZipAreRead() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("zip-system-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let text = Data(String(repeating: "base.apk and its splits\n", count: 20_000).utf8)
        let random = incompressible(250_000)
        try text.write(to: directory.appendingPathComponent("text.txt"))
        try random.write(to: directory.appendingPathComponent("random.bin"))
        func zip(_ arguments: [String], stdin: Data? = nil) throws {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/zip")
            process.arguments = arguments
            process.currentDirectoryURL = directory
            let input = Pipe()
            process.standardInput = input
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            if let stdin { input.fileHandleForWriting.write(stdin) }
            try? input.fileHandleForWriting.close()
            process.waitUntilExit()
            #expect(process.terminationStatus == 0)
        }
        try zip(["-q", "-0", "stored.zip", "text.txt", "random.bin"])
        try zip(["-q", "-9", "deflated.zip", "text.txt", "random.bin"])
        try zip(["-q", "-fz", "forced64.zip", "text.txt", "random.bin"])
        try zip(["-q", "piped.zip", "-"], stdin: text)
        for name in ["stored.zip", "deflated.zip", "forced64.zip"] {
            let archive = try ZipArchive(source: ZipFileSource(contentsOf: directory.appendingPathComponent(name)))
            #expect(try read(archive, "text.txt") == text, "\(name)")
            #expect(try read(archive, "random.bin") == random, "\(name)")
        }
        let piped = try ZipArchive(source: ZipFileSource(contentsOf: directory.appendingPathComponent("piped.zip")))
        #expect(try read(piped, "-") == text)
    }

    // MARK: Whole sizes

    @Test func anEntryAbove4GibIsStreamedFromASparseFile() throws {
        let size: UInt64 = 4 * 1024 * 1024 * 1024 + 123_456
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("zip64-\(UUID().uuidString).xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        // One stored entry of zeros, as a sparse file: a local header, the
        // data (never written), and a ZIP64 directory.
        let name = Data("big.apk".utf8)
        var head = Data()
        head.le(UInt32(0x0403_4b50)); head.le(UInt16(45)); head.le(UInt16(0)); head.le(UInt16(0)); head.le(UInt16(0)); head.le(UInt16(0x21))
        var crc: UInt = 0
        var zeros = [UInt8](repeating: 0, count: 1 << 20)
        var left = size
        while left > 0 {
            let count = Int(min(UInt64(zeros.count), left))
            crc = zlib.crc32(crc, &zeros, UInt32(count))
            left -= UInt64(count)
        }
        head.le(UInt32(truncatingIfNeeded: crc)); head.le(UInt32(0xFFFF_FFFF)); head.le(UInt32(0xFFFF_FFFF))
        head.le(UInt16(name.count)); head.le(UInt16(20))
        head.append(name)
        head.le(UInt16(1)); head.le(UInt16(16)); head.le(size); head.le(size)
        var tail = Data()
        let directoryOffset = UInt64(head.count) + size
        var central = Data()
        central.le(UInt32(0x0201_4b50)); central.le(UInt16(0x031E)); central.le(UInt16(45)); central.le(UInt16(0)); central.le(UInt16(0))
        central.le(UInt16(0)); central.le(UInt16(0x21)); central.le(UInt32(truncatingIfNeeded: crc))
        central.le(UInt32(0xFFFF_FFFF)); central.le(UInt32(0xFFFF_FFFF))
        central.le(UInt16(name.count)); central.le(UInt16(28)); central.le(UInt16(0)); central.le(UInt16(0)); central.le(UInt16(0))
        central.le(UInt32(0)); central.le(UInt32(0))
        central.append(name)
        central.le(UInt16(1)); central.le(UInt16(24)); central.le(size); central.le(size); central.le(UInt64(0))
        tail.append(central)
        let recordOffset = directoryOffset + UInt64(central.count)
        tail.le(UInt32(0x0606_4b50)); tail.le(UInt64(44)); tail.le(UInt16(45)); tail.le(UInt16(45)); tail.le(UInt32(0)); tail.le(UInt32(0))
        tail.le(UInt64(1)); tail.le(UInt64(1)); tail.le(UInt64(central.count)); tail.le(directoryOffset)
        tail.le(UInt32(0x0706_4b50)); tail.le(UInt32(0)); tail.le(recordOffset); tail.le(UInt32(1))
        tail.le(UInt32(0x0605_4b50)); tail.le(UInt16(0)); tail.le(UInt16(0)); tail.le(UInt16(0xFFFF)); tail.le(UInt16(0xFFFF))
        tail.le(UInt32(0xFFFF_FFFF)); tail.le(UInt32(0xFFFF_FFFF)); tail.le(UInt16(0))
        try head.write(to: url)
        let handle = try FileHandle(forWritingTo: url)
        try handle.seek(toOffset: directoryOffset)
        try handle.write(contentsOf: tail)
        try handle.close()

        let zip = try ZipArchive(source: ZipFileSource(contentsOf: url))
        let entry = try #require(zip.entry(named: "big.apk"))
        #expect(entry.size == size && entry.compressedSize == size)
        let reader = try zip.reader(for: entry, capacity: 1 << 20)
        var total: UInt64 = 0
        while let piece = try reader.read(upTo: 1 << 20) { total += UInt64(piece.count) }
        #expect(total == size)
    }

    // MARK: Hostile archives

    @Test func bytesThatAreNotAnArchiveAreRefused() {
        expectError(.notAnArchive) { _ = try ZipArchive(source: ZipDataSource(Data())) }
        expectError(.notAnArchive) { _ = try ZipArchive(source: ZipDataSource(Data("not a zip file at all, just text".utf8))) }
        expectError(.notAnArchive) { _ = try ZipArchive(source: ZipDataSource(incompressible(5000))) }
        // An end record whose comment would run past the file.
        var broken = Data()
        broken.le(UInt32(0x0605_4b50)); broken.append(Data(count: 16)); broken.le(UInt16(40))
        expectError(.notAnArchive) { _ = try ZipArchive(source: ZipDataSource(broken)) }
    }

    @Test func anEncryptedEntryIsNamedAsSuch() throws {
        var item = ZipFixture.Item("secret.apk", Data("data".utf8), method: 0)
        item.flags = 0x0001
        let zip = try archive(ZipFixture([item, .init("open.apk", Data("x".utf8), method: 0)]))
        #expect(zip.entry(named: "secret.apk")?.isEncrypted == true)
        #expect(zip.entry(named: "open.apk")?.isEncrypted == false)
        expectError(.encrypted) { _ = try zip.reader(for: #require(zip.entry(named: "secret.apk"))) }
        var aes = ZipFixture.Item("aes.apk", Data("data".utf8), method: 99)
        aes.flags = 0x0001
        let second = try archive(ZipFixture([aes]))
        #expect(second.entries[0].isEncrypted)
    }

    @Test func aMethodOtherThanStoredAndDeflateIsNotRead() throws {
        let zip = try archive(ZipFixture([.init("lzma.bin", Data("data".utf8), method: 14)]))
        expectKind(isUnsupported, { _ = try zip.reader(for: #require(zip.entry(named: "lzma.bin"))) })
    }

    @Test func tooManyEntriesAreRefusedBeforeTheyAreRead() {
        let fixture = ZipFixture((0..<50).map { .init("f\($0)", Data("x".utf8), method: 0) })
        var limits = ZipLimits()
        limits.maxEntries = 49
        expectKind(isTooLarge, { _ = try archive(fixture, limits: limits) })
        limits.maxEntries = 50
        #expect((try? archive(fixture, limits: limits))?.entries.count == 50)
    }

    @Test func aNameLongerThanTheBoundIsRefused() {
        let fixture = ZipFixture([.init(String(repeating: "n", count: 513), Data("x".utf8), method: 0)])
        expectKind(isTooLarge, { _ = try archive(fixture) })
        #expect((try? archive(ZipFixture([.init(String(repeating: "n", count: 512), Data("x".utf8), method: 0)]))) != nil)
    }

    @Test func aDirectoryLargerThanTheBoundIsRefused() {
        let fixture = ZipFixture((0..<300).map { .init(String(repeating: "d", count: 200) + "\($0)", Data("x".utf8), method: 0) })
        var limits = ZipLimits()
        limits.maxDirectoryBytes = 40_000
        expectKind(isTooLarge, { _ = try archive(fixture, limits: limits) })
    }

    @Test func aRepeatedNameIsRefused() {
        expectKind(isMalformed, { _ = try archive(ZipFixture([.init("a.apk", Data("1".utf8), method: 0), .init("a.apk", Data("2".utf8), method: 0)])) })
    }

    @Test func aPathOutOfItsFolderIsRefused() {
        for name in ["../escape.apk", "a/../../b", "/etc/passwd", "a\\b.apk", "dir/.."] {
            expectKind(isMalformed, { _ = try archive(ZipFixture([.init(name, Data("x".utf8), method: 0)])) })
        }
        // A dot inside a name is a name.
        #expect((try? archive(ZipFixture([.init("a..b/c.apk", Data("x".utf8), method: 0), .init("./x", Data(), method: 0)]))) != nil)
    }

    @Test func aNameThatIsNotUtf8IsRefused() {
        var item = ZipFixture.Item("x", Data("x".utf8), method: 0)
        item.rawName = Data([0x61, 0xFF, 0xFE])
        expectKind(isMalformed, { _ = try archive(ZipFixture([item])) })
    }

    @Test func anOffsetOutsideTheFileIsRefused() {
        var item = ZipFixture.Item("a", Data("hello".utf8), method: 0)
        item.headerOffset = 1_000_000
        expectKind(isMalformed, { _ = try archive(ZipFixture([item])) })
        var late = ZipFixture.Item("b", Data("hello".utf8), method: 0)
        late.declaredCompressedSize = 1_000_000
        late.declaredSize = 1_000_000
        expectKind(isMalformed, { _ = try archive(ZipFixture([late])) })
    }

    @Test func entriesThatOverlapAreRefused() {
        let data = incompressible(1000)
        // The second entry claims the first one's position.
        var second = ZipFixture.Item("b", data, method: 0)
        second.headerOffset = 0
        expectKind(isMalformed, { _ = try archive(ZipFixture([.init("a", data, method: 0), second])) })
        // The first entry's data reaches into the second.
        var long = ZipFixture.Item("a", data, method: 0)
        long.declaredCompressedSize = 1500
        long.declaredSize = 1500
        expectKind(isMalformed, { _ = try archive(ZipFixture([long, .init("b", data, method: 0)])) })
        // The last entry's data reaches into the directory.
        var last = ZipFixture.Item("a", data, method: 0)
        last.declaredCompressedSize = 1200
        last.declaredSize = 1200
        expectKind(isMalformed, { _ = try archive(ZipFixture([last])) })
    }

    @Test func aDirectoryOutsideTheFileOrTheWrongCountIsRefused() {
        var shifted = ZipFixture([.init("a", Data("x".utf8), method: 0)])
        shifted.directoryOffsetShift = 5000
        expectKind(isMalformed, { _ = try archive(shifted) })
        var count = ZipFixture([.init("a", Data("x".utf8), method: 0)])
        count.declaredEntryCount = 3
        expectKind(isMalformed, { _ = try archive(count) })
    }

    @Test func aSplitArchiveIsNotRead() {
        var fixture = ZipFixture([.init("a", Data("x".utf8), method: 0)])
        fixture.diskNumber = 1
        expectKind(isUnsupported, { _ = try archive(fixture) })
    }

    // MARK: Damaged data

    @Test func aWrongChecksumFailsAtTheEnd() throws {
        var item = ZipFixture.Item("a", Data("payload".utf8), method: 0)
        item.crc = 0x1234_5678
        let zip = try archive(ZipFixture([item, .init("b", incompressible(5000))]))
        expectKind(isCorrupt, { _ = try read(zip, "a") })
        var deflated = ZipFixture.Item("c", incompressible(5000))
        deflated.crc = 1
        let second = try archive(ZipFixture([deflated]))
        expectKind(isCorrupt, { _ = try read(second, "c") })
    }

    @Test func anEntryThatInflatesToMoreThanItsLengthNeverGivesTheExcess() throws {
        let data = incompressible(10_000)
        var item = ZipFixture.Item("a", data)
        item.declaredSize = 5000
        let zip = try archive(ZipFixture([item]))
        let reader = try zip.reader(for: #require(zip.entry(named: "a")), capacity: 4096)
        var given = 0
        do {
            while let piece = try reader.read(upTo: 4096) { given += piece.count }
            Issue.record("no error")
        } catch let error as ZipError {
            #expect(isCorrupt(error))
        }
        #expect(given <= 5000)
    }

    @Test func anEntryThatInflatesToLessThanItsLengthFails() throws {
        var item = ZipFixture.Item("a", incompressible(5000))
        item.declaredSize = 9000
        let zip = try archive(ZipFixture([item]))
        expectKind(isCorrupt, { _ = try read(zip, "a") })
    }

    @Test func aStoredEntryWhoseTwoLengthsDifferIsRefused() throws {
        var item = ZipFixture.Item("a", Data("hello world".utf8), method: 0)
        item.declaredSize = 5
        let zip = try archive(ZipFixture([item]))
        expectKind(isMalformed, { _ = try zip.reader(for: #require(zip.entry(named: "a"))) })
    }

    @Test func aTruncatedDeflateStreamFails() throws {
        let data = incompressible(20_000)
        let packed = ZipFixture.deflate(data)
        var item = ZipFixture.Item("a", data)
        item.storedBytes = packed.prefix(packed.count / 2)
        let zip = try archive(ZipFixture([item]))
        expectKind(isCorrupt, { _ = try read(zip, "a") })
    }

    @Test func aDeflateStreamThatEndsBeforeItsLengthFails() throws {
        let data = incompressible(8000)
        var item = ZipFixture.Item("a", data)
        item.declaredSize = 12_000
        item.crc = ZipFixture.checksum(data)
        let zip = try archive(ZipFixture([item]))
        expectKind(isCorrupt, { _ = try read(zip, "a") })
    }

    @Test func aLocalHeaderThatDisagreesWithTheDirectoryIsRefused() throws {
        // A local name of the directory's length that is another name is as
        // wrong as one of another length: the entry is the one the directory names.
        var item = ZipFixture.Item("a", Data("hello".utf8), method: 0)
        item.localName = "b"
        let same = try archive(ZipFixture([item]))
        expectKind(isMalformed, { _ = try same.reader(for: #require(same.entry(named: "a"))) })
        var longer = ZipFixture.Item("a", Data("hello".utf8), method: 0)
        longer.localName = "abc"
        let second = try archive(ZipFixture([longer]))
        expectKind(isMalformed, { _ = try second.reader(for: #require(second.entry(named: "a"))) })
        var multibyte = ZipFixture.Item("caf\u{E9}.apk", Data("hello".utf8), method: 0)
        multibyte.localName = "cafe.apk\u{0}"
        let third = try archive(ZipFixture([multibyte]))
        expectKind(isMalformed, { _ = try third.reader(for: #require(third.entry(named: "caf\u{E9}.apk"))) })
        // The same name reads.
        let honest = try archive(ZipFixture([.init("caf\u{E9}.apk", Data("hello".utf8), method: 0)]))
        #expect(try honest.data(of: #require(honest.entry(named: "caf\u{E9}.apk")), limit: 100) == Data("hello".utf8))
    }

    @Test func anEntryIsCompleteOnlyOnceItsEndAndChecksumWereChecked() throws {
        let data = Data((0..<5000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        for method: UInt16 in [0, 8] {
            let zip = try archive(ZipFixture([.init("a", data, method: method)]))
            let reader = try zip.reader(for: #require(zip.entry(named: "a")), capacity: 1000)
            #expect(!reader.isComplete)
            var got = Data()
            while got.count < data.count, let piece = try reader.read(upTo: 1000) { got.append(piece) }
            // Every byte is out, but the end has not been asked for.
            #expect(got == data)
            #expect(!reader.isComplete, "method \(method)")
            #expect(try reader.read(upTo: 1) == nil)
            #expect(reader.isComplete, "method \(method)")
        }
        var bad = ZipFixture.Item("a", data, method: 0)
        bad.crc = 0x1234_5678
        let zip = try archive(ZipFixture([bad]))
        let reader = try zip.reader(for: #require(zip.entry(named: "a")), capacity: 8000)
        _ = try reader.read(upTo: 8000)
        expectKind(isCorrupt, { _ = try reader.read(upTo: 1) })
        #expect(!reader.isComplete)
    }

    @Test func aCheckpointStopsTheReadsAndTheInflatingOfAHostileStream() throws {
        // 262,144 empty deflate blocks (stored, length 0) before two bytes of
        // output: little output, a lot of input to walk.
        var packed = Data()
        for _ in 0..<262_144 { packed.append(contentsOf: [0, 0, 0, 255, 255]) }
        packed += ZipFixture.deflate(Data("{}".utf8))
        var item = ZipFixture.Item("manifest.json", Data("{}".utf8))
        item.storedBytes = packed
        let bytes = ZipFixture([item]).build()
        let source = CountingSource(bytes)
        let stopped = Flag()
        let zip = try ZipArchive(source: source) { if stopped.value { throw CancellationError() } }
        // Without a stop the manifest reads (the probe the check is for).
        #expect(try zip.data(of: zip.entries[0], limit: 1 << 20) == Data("{}".utf8))
        let before = source.reads
        #expect(before > 10)
        source.onRead = { if source.reads == before + 3 { stopped.value = true } }
        #expect(throws: CancellationError.self) { _ = try zip.data(of: zip.entries[0], limit: 1 << 20) }
        // It stopped within a read or two of the stop, not after the other ~15.
        #expect(source.reads - before <= 5, "\(source.reads - before) reads")
    }

    @Test func aCheckpointStopsArchiveParsingBeforeTheDirectoryIsRead() throws {
        let bytes = ZipFixture([.init("a", Data("x".utf8))]).build()
        #expect(throws: CancellationError.self) {
            _ = try ZipArchive(source: ZipDataSource(bytes), checkpoint: { throw CancellationError() })
        }
    }
}

private final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false
    var value: Bool {
        get { lock.withLock { flag } }
        set { lock.withLock { flag = newValue } }
    }
}

/// A source that counts its reads and tells a hook of each.
private final class CountingSource: ZipSource, @unchecked Sendable {
    private let data: Data
    private let lock = NSLock()
    private var count = 0
    var onRead: (@Sendable () -> Void)?
    var size: UInt64 { UInt64(data.count) }
    var reads: Int { lock.withLock { count } }

    init(_ data: Data) { self.data = data }

    func read(at offset: UInt64, into buffer: UnsafeMutableRawBufferPointer) throws -> Int {
        lock.withLock { count += 1 }
        onRead?()
        guard offset < size, let base = buffer.baseAddress else { return 0 }
        let n = min(buffer.count, data.count - Int(offset))
        data.withUnsafeBytes { base.copyMemory(from: $0.baseAddress! + Int(offset), byteCount: n) }
        return n
    }
}
