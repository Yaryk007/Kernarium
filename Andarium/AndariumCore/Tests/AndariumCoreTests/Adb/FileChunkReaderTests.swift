// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct FileChunkReaderTests {
    private func scratch(_ body: Data) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("chunks-\(UUID().uuidString)")
        try body.write(to: url)
        return url
    }

    @Test func aFileComesBackInPiecesOfNoMoreThanTheLimitAndTheCapacity() throws {
        let body = Data((0..<1000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        let url = try scratch(body)
        defer { try? FileManager.default.removeItem(at: url) }
        let reader = try FileChunkReader(contentsOf: url, capacity: 300)
        #expect(reader.size == 1000 && !reader.isDirectory)
        var all = Data()
        var sizes: [Int] = []
        while let piece = try reader.read(upTo: 1 << 20) {
            sizes.append(piece.count)
            all.append(piece)
        }
        #expect(all == body)
        // The capacity bounds a piece even when more is asked for.
        #expect(sizes == [300, 300, 300, 100])
        #expect(try reader.read(upTo: 10) == nil)
        let small = try FileChunkReader(contentsOf: url, capacity: 300)
        #expect(try small.read(upTo: 40)?.count == 40)
        #expect(try small.read(upTo: 0) == nil)
    }

    @Test func aPieceIsACopyThatTheNextReadDoesNotChange() throws {
        let url = try scratch(Data([1, 2, 3, 4, 5, 6]))
        defer { try? FileManager.default.removeItem(at: url) }
        let reader = try FileChunkReader(contentsOf: url, capacity: 3)
        let first = try #require(try reader.read(upTo: 3))
        let second = try #require(try reader.read(upTo: 3))
        #expect(first == Data([1, 2, 3]) && second == Data([4, 5, 6]))
    }

    @Test func theSizeIsThatOfTheFileAsOpenedNotOfOneReadBefore() throws {
        let url = try scratch(Data())
        defer { try? FileManager.default.removeItem(at: url) }
        // The size a picker reads before a file is down: cached on the URL.
        #expect(try url.resourceValues(forKeys: [.fileSizeKey]).fileSize == 0)
        try Data(count: 4096).write(to: url)
        let reader = try FileChunkReader(contentsOf: url, capacity: 1024)
        #expect(reader.size == 4096)
    }

    @Test func aFolderAndAMissingFileAreNotRead() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("chunks-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: folder) }
        let reader = try FileChunkReader(contentsOf: folder, capacity: 16)
        #expect(reader.isDirectory && reader.size == 0)
        #expect(throws: POSIXError.self) { try reader.read(upTo: 16) }
        #expect(throws: POSIXError.self) {
            try FileChunkReader(contentsOf: folder.appendingPathComponent("nothing"), capacity: 16)
        }
    }

    @Test func aSparseFileOfSeveralGibibytesIsReadByItsLengthAlone() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("chunks-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: url) }
        let descriptor = open(url.path, O_CREAT | O_WRONLY | O_EXCL, 0o600)
        try #require(descriptor >= 0)
        try #require(ftruncate(descriptor, off_t(5) << 30) == 0)
        close(descriptor)
        let reader = try FileChunkReader(contentsOf: url, capacity: 1 << 20)
        #expect(reader.size == UInt64(5) << 30)
        let piece = try #require(try reader.read(upTo: 1 << 20))
        #expect(piece.count == 1 << 20 && piece.allSatisfy { $0 == 0 })
    }
}
