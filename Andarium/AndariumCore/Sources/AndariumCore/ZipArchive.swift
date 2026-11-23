// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import zlib

/// Why an archive, or one of its entries, cannot be read. A caller words
/// these; none of them is an adb failure.
public enum ZipError: Error, Equatable, Sendable {
    /// The bytes are not a ZIP archive (no end record, or a damaged one).
    case notAnArchive
    /// An entry is encrypted (the password-protected zip, or the AES one).
    case encrypted
    /// A form of ZIP this reader does not read: a split archive, or an entry
    /// compressed with a method other than stored and deflate.
    case unsupported(String)
    /// The archive contradicts itself or is hostile: an offset outside the
    /// file, entries that overlap, a repeated name, a name with `..`.
    case malformed(String)
    /// A bound of `ZipLimits` is exceeded.
    case tooLarge(String)
    /// An entry's data is not what its directory entry says (length or
    /// checksum), or the compressed stream is damaged or ends early.
    case corrupt(String)
}

/// The bounds a hostile archive is read under. Each is far above what a real
/// app container needs.
public struct ZipLimits: Equatable, Sendable {
    public var maxEntries = 4096
    public var maxNameBytes = 512
    /// The central directory as a whole; it is read into memory.
    public var maxDirectoryBytes: UInt64 = 8 * 1024 * 1024

    public init() {}
}

/// Asked at bounded intervals while an archive is parsed and its entries are
/// read and inflated: it throws to stop the work (a cancel, a deadline). The
/// reads and the inflating run on a thread of their own, where cancelling a
/// task is not seen, and a hostile stream can make a long computation out of
/// little input and less output.
public typealias ZipCheckpoint = @Sendable () throws -> Void

/// Random access to the bytes of an archive.
public protocol ZipSource: AnyObject, Sendable {
    var size: UInt64 { get }
    /// Fills `buffer` from `offset` as far as the source reaches and returns
    /// how many bytes it gave. Zero means the end.
    func read(at offset: UInt64, into buffer: UnsafeMutableRawBufferPointer) throws -> Int
}

/// An archive in a file, read with `pread`: nothing is unpacked, and nothing
/// is kept but the bytes of the read in hand.
public final class ZipFileSource: ZipSource, @unchecked Sendable {
    public let size: UInt64
    private let descriptor: Int32

    public init(contentsOf url: URL) throws {
        let opened = url.withUnsafeFileSystemRepresentation { path -> Int32 in
            guard let path else {
                errno = EINVAL
                return -1
            }
            return Darwin.open(path, O_RDONLY | O_CLOEXEC)
        }
        guard opened >= 0 else { throw Self.failure(errno) }
        var status = stat()
        guard fstat(opened, &status) == 0 else {
            let code = errno
            Darwin.close(opened)
            throw Self.failure(code)
        }
        guard status.st_mode & S_IFMT == S_IFREG else {
            Darwin.close(opened)
            throw Self.failure(EISDIR)
        }
        descriptor = opened
        size = UInt64(max(status.st_size, 0))
    }

    deinit { Darwin.close(descriptor) }

    public func read(at offset: UInt64, into buffer: UnsafeMutableRawBufferPointer) throws -> Int {
        guard offset < size, let base = buffer.baseAddress, !buffer.isEmpty else { return 0 }
        let count = Int(min(UInt64(buffer.count), size - offset))
        while true {
            let got = pread(descriptor, base, count, off_t(offset))
            if got >= 0 { return got }
            if errno != EINTR { throw Self.failure(errno) }
        }
    }

    private static func failure(_ code: Int32) -> Error {
        POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
    }
}

/// An archive in memory, for tests and for the small parts of a container.
public final class ZipDataSource: ZipSource, @unchecked Sendable {
    private let data: Data
    public var size: UInt64 { UInt64(data.count) }

    public init(_ data: Data) { self.data = data }

    public func read(at offset: UInt64, into buffer: UnsafeMutableRawBufferPointer) throws -> Int {
        guard offset < size, let base = buffer.baseAddress else { return 0 }
        let count = min(buffer.count, data.count - Int(offset))
        data.withUnsafeBytes { base.copyMemory(from: $0.baseAddress! + Int(offset), byteCount: count) }
        return count
    }
}

/// One entry of the central directory, with the sizes the directory states.
/// Those are the only sizes a reader trusts.
public struct ZipEntry: Equatable, Sendable {
    public let name: String
    /// 0 stored, 8 deflate; `ZipArchive.reader(for:)` refuses the others.
    public let method: UInt16
    public let compressedSize: UInt64
    public let size: UInt64
    public let crc32: UInt32
    public let isEncrypted: Bool
    let headerOffset: UInt64
    let nameByteCount: Int
    /// Where the next thing in the file starts: the data cannot reach it.
    let limit: UInt64

    public var isDirectory: Bool { name.hasSuffix("/") }
}

/// The central directory of a ZIP or ZIP64 archive, read through a
/// `ZipSource`. Reading it costs one tail read and one directory read; an
/// entry's data is read only when `reader(for:)` is asked for it.
public struct ZipArchive: Sendable {
    public let entries: [ZipEntry]
    public let source: any ZipSource
    private let byName: [String: Int]
    private let checkpoint: ZipCheckpoint?

    private static let endSignature: UInt32 = 0x0605_4b50
    private static let zip64EndSignature: UInt32 = 0x0606_4b50
    private static let zip64LocatorSignature: UInt32 = 0x0706_4b50
    private static let centralSignature: UInt32 = 0x0201_4b50
    private static let localSignature: UInt32 = 0x0403_4b50
    private static let localHeaderBytes = 30
    private static let centralHeaderBytes = 46

    public init(source: any ZipSource, limits: ZipLimits = ZipLimits(), checkpoint: ZipCheckpoint? = nil) throws {
        self.source = source
        self.checkpoint = checkpoint
        try checkpoint?()
        let size = source.size
        guard size >= 22 else { throw ZipError.notAnArchive }
        // The end record is the last signature whose comment reaches the end
        // of the file; the ZIP64 locator, if any, is the 20 bytes before it.
        let tailSize = Int(min(size, 20 + 22 + 0xFFFF))
        let tail = try Self.read(source, at: size - UInt64(tailSize), count: tailSize)
        var found: Int?
        var index = tail.count - 22
        while index >= 0 {
            if tail.uint32(at: index) == Self.endSignature, index + 22 + Int(tail.uint16(at: index + 20)) == tail.count {
                found = index
                break
            }
            index -= 1
        }
        guard let end = found else { throw ZipError.notAnArchive }
        let endOffset = size - UInt64(tailSize) + UInt64(end)
        var disk = UInt32(tail.uint16(at: end + 4))
        var directoryDisk = UInt32(tail.uint16(at: end + 6))
        var count = UInt64(tail.uint16(at: end + 10))
        var directorySize = UInt64(tail.uint32(at: end + 12))
        var directoryOffset = UInt64(tail.uint32(at: end + 16))
        var directoryEnd = endOffset
        let wantsZip64 = tail.uint16(at: end + 10) == 0xFFFF || tail.uint32(at: end + 12) == 0xFFFF_FFFF
            || tail.uint32(at: end + 16) == 0xFFFF_FFFF
        if end >= 20, tail.uint32(at: end - 20) == Self.zip64LocatorSignature {
            // The locator names the ZIP64 end record. A plain archive that
            // happens to hold the signature in its data does not claim any
            // field, and keeps its own values.
            let recordDisk = tail.uint32(at: end - 16)
            let recordOffset = tail.uint64(at: end - 12)
            if wantsZip64 {
                guard recordDisk == 0, recordOffset <= endOffset, endOffset - recordOffset >= 56 + 20 else {
                    throw ZipError.malformed("a ZIP64 end record outside the file")
                }
                let record = try Self.read(source, at: recordOffset, count: 56)
                guard record.uint32(at: 0) == Self.zip64EndSignature else {
                    throw ZipError.malformed("a ZIP64 end record without its signature")
                }
                disk = record.uint32(at: 16)
                directoryDisk = record.uint32(at: 20)
                let onThisDisk = record.uint64(at: 24)
                count = record.uint64(at: 32)
                directorySize = record.uint64(at: 40)
                directoryOffset = record.uint64(at: 48)
                guard onThisDisk == count else { throw ZipError.unsupported("a multi-disk archive") }
                directoryEnd = recordOffset
            }
        } else if wantsZip64 {
            throw ZipError.malformed("a ZIP64 field without a ZIP64 end record")
        }
        guard disk == 0, directoryDisk == 0 else { throw ZipError.unsupported("a multi-disk archive") }
        guard count <= UInt64(limits.maxEntries) else { throw ZipError.tooLarge("more than \(limits.maxEntries) entries") }
        guard directorySize <= limits.maxDirectoryBytes else { throw ZipError.tooLarge("a central directory of \(directorySize) bytes") }
        guard directoryOffset <= directoryEnd, directoryEnd - directoryOffset >= directorySize else {
            throw ZipError.malformed("a central directory outside the file")
        }
        guard directorySize >= count * UInt64(Self.centralHeaderBytes) else {
            throw ZipError.malformed("a central directory shorter than its entries")
        }
        let directory = try Self.read(source, at: directoryOffset, count: Int(directorySize))
        var parsed: [ZipEntry] = []
        parsed.reserveCapacity(Int(count))
        var position = 0
        for _ in 0..<count {
            try checkpoint?()
            parsed.append(try Self.parseEntry(in: directory, at: &position, fileSize: size, limits: limits))
        }
        entries = try Self.checked(parsed, directoryOffset: directoryOffset)
        var names: [String: Int] = [:]
        for (index, entry) in entries.enumerated() { names[entry.name] = index }
        byName = names
    }

    public func entry(named name: String) -> ZipEntry? {
        byName[name].map { entries[$0] }
    }

    /// The entry's whole data, `limit` bytes at most, for the small parts of a
    /// container (a manifest). An entry that declares more is refused before a
    /// byte of it is read.
    public func data(of entry: ZipEntry, limit: Int) throws -> Data {
        guard entry.size <= UInt64(limit) else { throw ZipError.tooLarge("\(entry.name) is \(entry.size) bytes") }
        let reader = try reader(for: entry, capacity: max(1, min(Int(entry.size), 64 * 1024)))
        var all = Data()
        while let piece = try reader.read(upTo: 64 * 1024) {
            try checkpoint?()
            all.append(piece)
        }
        return all
    }

    /// A reader of the entry's decoded bytes, in pieces of `capacity` bytes at
    /// most, that checks the length and the checksum at the end.
    public func reader(for entry: ZipEntry, capacity: Int = 256 * 1024) throws -> ZipEntryReader {
        try ZipEntryReader(source: source, entry: entry, capacity: capacity, checkpoint: checkpoint)
    }

    // MARK: Parsing

    private static func read(_ source: any ZipSource, at offset: UInt64, count: Int) throws -> Data {
        var data = Data(count: count)
        var filled = 0
        try data.withUnsafeMutableBytes { raw in
            while filled < count {
                let got = try source.read(
                    at: offset + UInt64(filled), into: UnsafeMutableRawBufferPointer(rebasing: raw[filled...]))
                if got <= 0 { throw ZipError.notAnArchive }
                filled += got
            }
        }
        return data
    }

    private static func parseEntry(in directory: Data, at position: inout Int, fileSize: UInt64, limits: ZipLimits) throws -> ZipEntry {
        guard position + centralHeaderBytes <= directory.count, directory.uint32(at: position) == centralSignature else {
            throw ZipError.malformed("a central directory entry without its signature")
        }
        let flags = directory.uint16(at: position + 8)
        let method = directory.uint16(at: position + 10)
        let crc = directory.uint32(at: position + 16)
        var compressed = UInt64(directory.uint32(at: position + 20))
        var size = UInt64(directory.uint32(at: position + 24))
        let nameLength = Int(directory.uint16(at: position + 28))
        let extraLength = Int(directory.uint16(at: position + 30))
        let commentLength = Int(directory.uint16(at: position + 32))
        var diskStart = UInt64(directory.uint16(at: position + 34))
        var offset = UInt64(directory.uint32(at: position + 42))
        let next = position + centralHeaderBytes + nameLength + extraLength + commentLength
        guard nameLength > 0, nameLength <= limits.maxNameBytes else {
            throw ZipError.tooLarge("a name of \(nameLength) bytes")
        }
        guard next <= directory.count else { throw ZipError.malformed("a central directory entry past the directory") }
        let nameStart = position + centralHeaderBytes
        guard let name = String(data: directory.subdata(in: (directory.startIndex + nameStart)..<(directory.startIndex + nameStart + nameLength)), encoding: .utf8)
        else { throw ZipError.malformed("a name that is not UTF-8") }
        // The ZIP64 extra field stands in for the fields that read all ones,
        // in this order.
        var cursor = nameStart + nameLength
        let extraEnd = cursor + extraLength
        while cursor + 4 <= extraEnd {
            let id = directory.uint16(at: cursor)
            let length = Int(directory.uint16(at: cursor + 2))
            guard cursor + 4 + length <= extraEnd else { throw ZipError.malformed("an extra field past its entry") }
            if id == 0x0001 {
                var field = cursor + 4
                let end = field + length
                func take64() throws -> UInt64 {
                    guard field + 8 <= end else { throw ZipError.malformed("a short ZIP64 extra field") }
                    defer { field += 8 }
                    return directory.uint64(at: field)
                }
                if size == 0xFFFF_FFFF { size = try take64() }
                if compressed == 0xFFFF_FFFF { compressed = try take64() }
                if offset == 0xFFFF_FFFF { offset = try take64() }
                if diskStart == 0xFFFF {
                    guard field + 4 <= end else { throw ZipError.malformed("a short ZIP64 extra field") }
                    diskStart = UInt64(directory.uint32(at: field))
                }
            }
            cursor += 4 + length
        }
        guard diskStart == 0 else { throw ZipError.unsupported("a multi-disk archive") }
        guard compressed < fileSize, offset < fileSize else { throw ZipError.malformed("an entry outside the file") }
        try validate(name: name)
        position = next
        return ZipEntry(
            name: name, method: method, compressedSize: compressed, size: size, crc32: crc,
            isEncrypted: flags & 0x0001 != 0 || flags & 0x0040 != 0 || method == 99, headerOffset: offset,
            nameByteCount: nameLength, limit: 0)
    }

    /// An entry name no archive of an app has: a path out of its folder.
    private static func validate(name: String) throws {
        guard !name.contains("\0"), !name.contains("\\"), !name.hasPrefix("/"),
              !name.split(separator: "/").contains("..")
        else { throw ZipError.malformed("the name \(name.debugDescription)") }
    }

    /// The directory's entries as a whole: no name twice, and each one's
    /// local header and data inside the file, before the directory and clear
    /// of every other.
    private static func checked(_ parsed: [ZipEntry], directoryOffset: UInt64) throws -> [ZipEntry] {
        var seen = Set<String>()
        for entry in parsed where !seen.insert(entry.name).inserted {
            throw ZipError.malformed("the name \(entry.name.debugDescription) twice")
        }
        let order = parsed.indices.sorted { parsed[$0].headerOffset < parsed[$1].headerOffset }
        var limits = [UInt64](repeating: directoryOffset, count: parsed.count)
        for (rank, index) in order.enumerated() {
            let entry = parsed[index]
            let next = rank + 1 < order.count ? parsed[order[rank + 1]].headerOffset : directoryOffset
            guard entry.headerOffset < next else { throw ZipError.malformed("entries that share a position") }
            let minimum = entry.headerOffset + UInt64(localHeaderBytes + entry.nameByteCount)
            let (end, overflow) = minimum.addingReportingOverflow(entry.compressedSize)
            guard !overflow, end <= next else { throw ZipError.malformed("entries that overlap, or data past the directory") }
            limits[index] = next
        }
        return parsed.indices.map { index in
            let entry = parsed[index]
            return ZipEntry(
                name: entry.name, method: entry.method, compressedSize: entry.compressedSize, size: entry.size,
                crc32: entry.crc32, isEncrypted: entry.isEncrypted, headerOffset: entry.headerOffset,
                nameByteCount: entry.nameByteCount, limit: limits[index])
        }
    }
}

/// The decoded bytes of one entry, in pieces. It reads the compressed bytes
/// with `pread` into one buffer and inflates into another, and hands each
/// piece out as a `Data` of its own (the reason `FileChunkReader` gives for
/// not using `FileHandle`: this runs on the thread of an adb operation, which
/// empties no autorelease pool until it ends).
///
/// It never gives more than the directory's length: an entry that inflates to
/// more fails before the excess leaves, and one that ends short, fails the
/// checksum, or ends the compressed stream early fails at the end.
public final class ZipEntryReader {
    public let entry: ZipEntry
    private let source: any ZipSource
    private let capacity: Int
    private let output: UnsafeMutableRawPointer
    private let input: UnsafeMutableRawPointer
    private static let inputCapacity = 64 * 1024
    private var stream: UnsafeMutablePointer<z_stream>?
    private var position: UInt64
    private let end: UInt64
    private var delivered: UInt64 = 0
    private var checksum: UInt = 0
    private var streamEnded = false
    private var finished = false
    private let checkpoint: ZipCheckpoint?

    /// The whole entry has been given and its end was reached: the compressed
    /// stream ended where the directory says, and the length and the checksum
    /// match. A caller that must not act on a damaged entry (a commit) asks
    /// this after the last piece, and does not rely on how the pieces ended.
    public var isComplete: Bool { finished }

    init(source: any ZipSource, entry: ZipEntry, capacity: Int, checkpoint: ZipCheckpoint? = nil) throws {
        precondition(capacity > 0)
        guard !entry.isEncrypted else { throw ZipError.encrypted }
        guard entry.method == 0 || entry.method == 8 else {
            throw ZipError.unsupported("compression method \(entry.method)")
        }
        guard entry.method == 8 || entry.compressedSize == entry.size else {
            throw ZipError.malformed("a stored entry whose two lengths differ")
        }
        self.source = source
        self.entry = entry
        self.capacity = capacity
        self.checkpoint = checkpoint
        output = UnsafeMutableRawPointer.allocate(byteCount: capacity, alignment: 16)
        input = UnsafeMutableRawPointer.allocate(byteCount: Self.inputCapacity, alignment: 16)
        var header = [UInt8](repeating: 0, count: 30)
        let got = try header.withUnsafeMutableBytes { try source.read(at: entry.headerOffset, into: $0) }
        guard got == 30, header.withUnsafeBytes({ $0.loadUnaligned(as: UInt32.self) }) == 0x0403_4b50 else {
            output.deallocate()
            input.deallocate()
            throw ZipError.malformed("a local header without its signature")
        }
        let nameLength = Int(UInt16(header[26]) | UInt16(header[27]) << 8)
        let extraLength = Int(UInt16(header[28]) | UInt16(header[29]) << 8)
        let start = entry.headerOffset + 30 + UInt64(nameLength) + UInt64(extraLength)
        let (stop, overflow) = start.addingReportingOverflow(entry.compressedSize)
        guard nameLength == entry.nameByteCount, !overflow, stop <= entry.limit else {
            output.deallocate()
            input.deallocate()
            throw ZipError.malformed("a local header that disagrees with the directory")
        }
        // The local name is the directory's: an entry is the one its directory
        // entry names, not another that happens to be as long.
        var localName = [UInt8](repeating: 0, count: nameLength)
        let gotName = try localName.withUnsafeMutableBytes { try source.read(at: entry.headerOffset + 30, into: $0) }
        guard gotName == nameLength, localName.elementsEqual(entry.name.utf8) else {
            output.deallocate()
            input.deallocate()
            throw ZipError.malformed("a local name that is not the directory's")
        }
        position = start
        end = stop
        if entry.method == 8 {
            let allocated = UnsafeMutablePointer<z_stream>.allocate(capacity: 1)
            allocated.initialize(to: z_stream())
            guard inflateInit2_(allocated, -15, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
                allocated.deallocate()
                output.deallocate()
                input.deallocate()
                throw ZipError.unsupported("inflate")
            }
            stream = allocated
        }
    }

    deinit {
        if let stream {
            inflateEnd(stream)
            stream.deallocate()
        }
        output.deallocate()
        input.deallocate()
    }

    /// The next piece of at most `limit` bytes (never more than the
    /// capacity), or nil once the whole entry has been given and checked.
    public func read(upTo limit: Int) throws -> Data? {
        let want = min(limit, capacity)
        guard want > 0 else { return nil }
        while !finished {
            try checkpoint?()
            let produced = try produce(up: want)
            if produced > 0 { return Data(bytes: output, count: produced) }
        }
        return nil
    }

    /// Fills `output` with up to `want` bytes; zero only when the entry is
    /// over (and `finished` is set after its checks).
    private func produce(up want: Int) throws -> Int {
        let remaining = entry.size - delivered
        if remaining == 0 {
            try close()
            return 0
        }
        let count: Int
        if let stream {
            count = try inflate(stream, want: want, remaining: remaining)
        } else {
            count = try copy(want: want, remaining: remaining)
        }
        checksum = zlib.crc32(checksum, output.assumingMemoryBound(to: Bytef.self), UInt32(count))
        delivered += UInt64(count)
        return count
    }

    private func copy(want: Int, remaining: UInt64) throws -> Int {
        let count = Int(min(UInt64(want), remaining))
        let got = try source.read(at: position, into: UnsafeMutableRawBufferPointer(start: output, count: count))
        guard got > 0 else { throw ZipError.corrupt("\(entry.name) ends early") }
        position += UInt64(got)
        return got
    }

    private func inflate(_ stream: UnsafeMutablePointer<z_stream>, want: Int, remaining: UInt64) throws -> Int {
        stream.pointee.next_out = output.assumingMemoryBound(to: Bytef.self)
        stream.pointee.avail_out = UInt32(want)
        while true {
            try checkpoint?()
            if stream.pointee.avail_in == 0, position < end {
                let count = Int(min(UInt64(Self.inputCapacity), end - position))
                let got = try source.read(at: position, into: UnsafeMutableRawBufferPointer(start: input, count: count))
                guard got > 0 else { throw ZipError.corrupt("\(entry.name) ends early") }
                position += UInt64(got)
                stream.pointee.next_in = input.assumingMemoryBound(to: Bytef.self)
                stream.pointee.avail_in = UInt32(got)
            }
            let status = zlib.inflate(stream, Z_NO_FLUSH)
            let produced = want - Int(stream.pointee.avail_out)
            guard UInt64(produced) <= remaining else { throw ZipError.corrupt("\(entry.name) is longer than it says") }
            switch status {
            case Z_STREAM_END:
                streamEnded = true
                guard UInt64(produced) == remaining else { throw ZipError.corrupt("\(entry.name) is shorter than it says") }
                return produced
            case Z_OK, Z_BUF_ERROR:
                if produced > 0 { return produced }
                if stream.pointee.avail_in == 0, position >= end { throw ZipError.corrupt("\(entry.name) ends early") }
            default:
                throw ZipError.corrupt("\(entry.name) does not inflate")
            }
        }
    }

    /// The entry has given its length: the compressed stream must end here
    /// and the checksum must match.
    private func close() throws {
        if let stream, !streamEnded {
            var spare: UInt8 = 0
            while true {
                try checkpoint?()
                if stream.pointee.avail_in == 0, position < end {
                    let count = Int(min(UInt64(Self.inputCapacity), end - position))
                    let got = try source.read(at: position, into: UnsafeMutableRawBufferPointer(start: input, count: count))
                    guard got > 0 else { throw ZipError.corrupt("\(entry.name) ends early") }
                    position += UInt64(got)
                    stream.pointee.next_in = input.assumingMemoryBound(to: Bytef.self)
                    stream.pointee.avail_in = UInt32(got)
                }
                let status = withUnsafeMutablePointer(to: &spare) { pointer -> Int32 in
                    stream.pointee.next_out = pointer
                    stream.pointee.avail_out = 1
                    return zlib.inflate(stream, Z_NO_FLUSH)
                }
                if stream.pointee.avail_out == 0 { throw ZipError.corrupt("\(entry.name) is longer than it says") }
                if status == Z_STREAM_END { break }
                if status != Z_OK, status != Z_BUF_ERROR { throw ZipError.corrupt("\(entry.name) does not inflate") }
                if stream.pointee.avail_in == 0, position >= end { throw ZipError.corrupt("\(entry.name) ends early") }
            }
            streamEnded = true
        }
        guard UInt32(truncatingIfNeeded: checksum) == entry.crc32 else {
            throw ZipError.corrupt("\(entry.name) fails its checksum")
        }
        finished = true
    }
}

extension Data {
    fileprivate func uint16(at index: Int) -> UInt16 {
        UInt16(self[startIndex + index]) | UInt16(self[startIndex + index + 1]) << 8
    }

    fileprivate func uint32(at index: Int) -> UInt32 {
        UInt32(uint16(at: index)) | UInt32(uint16(at: index + 2)) << 16
    }

    fileprivate func uint64(at index: Int) -> UInt64 {
        UInt64(uint32(at: index)) | UInt64(uint32(at: index + 4)) << 32
    }
}
