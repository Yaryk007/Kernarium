// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private let root = GuestFilePath.root
private let v2Features = "shell_v2,cmd,stat_v2,ls_v2,abb_exec"
private let v1Features = "shell_v2,cmd,abb_exec"

private func connected(_ adbd: FakeAdbd) throws -> AdbConnection {
    let connection = AdbConnection(transport: adbd.client)
    try connection.handshake(signer: nil)
    return connection
}

private func storage(_ extra: [String: FakeGuestStorage.Node] = [:]) -> FakeGuestStorage {
    var nodes: [String: FakeGuestStorage.Node] = [root: .folder]
    nodes.merge(extra) { _, new in new }
    return FakeGuestStorage(nodes)
}

@Suite(.timeLimit(.minutes(1)))
struct AdbSyncListAndSendTests {
    @Test func aV2ListingCarriesSixtyFourBitSizesAndTimes() throws {
        let guest = storage([
            root + "/Movies": .folder,
            root + "/clip.mov": .large(5_000_000_000, mtime: 1_760_000_000),
        ])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        let entries = try sync.list(root)
        #expect(entries.map(\.name) == ["Movies", "clip.mov"])
        let clip = try #require(entries.last)
        #expect(clip.size == 5_000_000_000 && clip.modified == 1_760_000_000 && !clip.isDirectory)
        #expect(entries[0].isDirectory)
        #expect(guest.requests == [.list(root)])
        #expect(try sync.stat(root + "/clip.mov").size == 5_000_000_000)
    }

    @Test func aListingWithoutV2UsesTheOlderReply() throws {
        let guest = storage([root + "/a.txt": .file(Data(count: 3), mtime: 5), root + "/big": .large(5_000_000_000, mtime: 5)])
        let adbd = FakeAdbd(features: v1Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        let entries = try sync.list(root)
        #expect(entries.map(\.name) == ["a.txt", "big"])
        #expect(entries[0].size == 3 && entries[0].modified == 5)
        // Version 1 sizes are 32-bit, which is why the client asks for 2.
        #expect(entries[1].size == 5_000_000_000 % (1 << 32))
    }

    @Test func anEmptyFolderAndOneTheDeviceCannotOpenBothListNothing() throws {
        let guest = storage([
            root + "/Empty": .folder, root + "/Private": .refused, root + "/Android": .folder,
            root + "/Android/data": .dotted,
        ])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        #expect(try sync.list(root + "/Empty").isEmpty)
        #expect(try sync.list(root + "/Private").isEmpty)
        // The FUSE view of Android/data lists the two and nothing else.
        #expect(try sync.list(root + "/Android/data").map(\.name) == ["..", "."])
        // Each terminator was read whole, so the next request is understood.
        #expect(try sync.stat(root + "/Android").isDirectory)
    }

    @Test func aStatReportsTheModificationTime() throws {
        let guest = storage([root + "/a": .file(Data(count: 1), mtime: 1_234_567)])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let stat = try AdbSync(connection: connection).stat(root + "/a")
        #expect(stat.modified == 1_234_567 && stat.size == 1)
    }

    @Test func sendFramesTheBytesAndSetsTheTime() throws {
        let body = Data((0..<600_000).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ 5) })
        let guest = storage()
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        let source = MemorySource(body)
        var totals: [UInt64] = []
        try sync.send(
            root + "/out.bin", mode: 0o100664, modified: 1_700_000_123,
            next: { try source.read(limit: 100_000) }, sent: { totals.append($0) })
        let sent = try #require(guest.sent(root + "/out.bin"))
        #expect(sent.data == body && sent.mtime == 1_700_000_123)
        #expect(guest.requests == [.send(root + "/out.bin", mode: 0o100664)])
        #expect(totals == totals.sorted() && totals.last == UInt64(body.count))
        // Batches, not a report per frame.
        #expect(totals.count <= body.count / AdbSync.sendBatch + 2)
    }

    @Test func aFailAfterTheDoneIsTheDevicesOwnMessage() throws {
        let guest = storage()
        guest.sendFailure = "secure_open failed: No space left on device"
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        let source = MemorySource(Data(count: 1000))
        #expect(throws: AdbError.remote("secure_open failed: No space left on device")) {
            try sync.send(root + "/x", mode: 0o100664, modified: 0, next: { try source.read(limit: 4096) })
        }
    }

    @Test func aDeviceThatClosesTheStreamAtOnceStillSaysWhy() throws {
        let guest = storage()
        guest.refuseSend = "secure_open failed: Permission denied"
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        // Enough bytes that the write meets the close.
        let source = MemorySource(Data(count: 6_000_000))
        #expect(throws: AdbError.remote("secure_open failed: Permission denied")) {
            try sync.send(root + "/x", mode: 0o100664, modified: 0, next: { try source.read(limit: 100_000) })
        }
    }

    @Test func aSourceThatFailsEndsTheSend() throws {
        let guest = storage()
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        #expect(throws: AdbError.io(EIO)) {
            try sync.send(root + "/x", mode: 0o100664, modified: 0, next: { throw AdbError.io(EIO) })
        }
    }

    @Test func receiveStillJoinsChunksAfterAListing() throws {
        let body = Data((0..<250_000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        let guest = storage([root + "/f": .file(body, mtime: 1)])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        _ = try sync.list(root)
        var received = Data()
        try sync.receive(root + "/f") { received.append($0) }
        #expect(received == body)
    }
}

@Suite(.timeLimit(.minutes(1)))
struct GuestFilesListingTests {
    @Test func theFolderListsWhatTheFilesAppShows() throws {
        let guest = storage([
            root + "/DCIM": .folder,
            root + "/.hidden": .file(Data(count: 1), mtime: 1),
            root + "/.andarium-ab12.part": .file(Data(count: 1), mtime: 1),
            root + "/Notes.txt": .file(Data(count: 42), mtime: 1_700_000_000),
            root + "/Shortcut": .link(root + "/DCIM"),
            root + "/Alias.txt": .link(root + "/Notes.txt"),
            root + "/Dangling": .link(root + "/nowhere"),
        ])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let entries = try GuestFiles.list(root, on: connection)
        #expect(entries.map(\.name) == ["Alias.txt", "DCIM", "Notes.txt", "Shortcut"])
        let alias = try #require(entries.first { $0.name == "Alias.txt" })
        #expect(!alias.isFolder && alias.size == 42)
        let shortcut = try #require(entries.first { $0.name == "Shortcut" })
        #expect(shortcut.isFolder && shortcut.size == 0)
        #expect(entries.first { $0.name == "Notes.txt" }?.modified == Date(timeIntervalSince1970: 1_700_000_000))
        // A folder with entries needs no second look.
        #expect(guest.shellCommands.isEmpty)
    }

    @Test func anEmptyFolderIsNotARefusedOne() throws {
        let guest = storage([
            root + "/Audiobooks": .folder, root + "/Android": .folder, root + "/Android/data": .dotted,
            root + "/Private": .refused, root + "/Hidden": .folder, root + "/Hidden/.nomedia": .file(Data(), mtime: 1),
        ])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        // adbd lists an empty folder and a folder it cannot open alike.
        #expect(try GuestFiles.list(root + "/Audiobooks", on: connection).isEmpty)
        #expect(try GuestFiles.list(root + "/Android/data", on: connection).isEmpty)
        #expect(throws: GuestFilesError.refused) { try GuestFiles.list(root + "/Private", on: connection) }
        #expect(throws: GuestFilesError.missing) { try GuestFiles.list(root + "/Gone", on: connection) }
        // A folder of hidden files has entries, which the shell need not confirm.
        let before = guest.shellCommands.count
        #expect(try GuestFiles.list(root + "/Hidden", on: connection).isEmpty)
        #expect(guest.shellCommands.count == before)
        #expect(guest.shellCommands.contains("ls -A -- '\(root)/Private' >/dev/null"))
    }

    @Test func aPathOutsideInternalStorageIsNeverListed() throws {
        let guest = storage()
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        for path in ["/", "/data", root + "/..", root + "/../../data", "/storage/emulated/01", root + "//x"] {
            #expect(throws: GuestFilesError.badPath) { try GuestFiles.list(path, on: connection) }
            #expect(throws: GuestFilesError.badPath) { try GuestFiles.checkReadable(path, on: connection) }
        }
        #expect(guest.requests.isEmpty && guest.shellCommands.isEmpty)
    }

    @Test func theListingSessionIsClosedBeforeTheShellIsAsked() throws {
        let guest = storage([root + "/Empty": .folder])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        _ = try GuestFiles.list(root + "/Empty", on: connection)
        #expect(guest.requests == [.list(root + "/Empty"), .quit])
        #expect(adbd.servicesOpened.map { $0.hasPrefix("shell") ? "shell" : $0 } == ["sync:", "shell"])
    }

    @Test func aWalkSeesEveryFileBelowAFolder() throws {
        let guest = storage([
            root + "/Trip": .folder,
            root + "/Trip/a.jpg": .file(Data(count: 10), mtime: 1),
            root + "/Trip/Day 2": .folder,
            root + "/Trip/Day 2/b.jpg": .file(Data(count: 20), mtime: 1),
            root + "/Trip/Day 2/.nomedia": .file(Data(), mtime: 1),
            root + "/Trip/Private": .refused,
            root + "/Trip/Link": .link(root + "/Trip/Day 2"),
            root + "/Trip/.andarium-zz.part": .file(Data(count: 5), mtime: 1),
        ])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        var visits = 0
        let items = try GuestFiles.walk(root + "/Trip", using: sync, visit: { visits += 1 })
        // A folder the shell cannot open comes out as an empty one; links and
        // partial files are not carried.
        #expect(items.map(\.relativePath) == ["Day 2", "Private", "a.jpg", "Day 2/.nomedia", "Day 2/b.jpg"])
        #expect(items.filter { !$0.isFolder }.reduce(0) { $0 + $1.size } == 30)
        #expect(visits == 3)
        // A folder's parent comes before what it holds.
        #expect(items.firstIndex { $0.relativePath == "Day 2" }! < items.firstIndex { $0.relativePath == "Day 2/b.jpg" }!)
    }
}

@Suite(.timeLimit(.minutes(1)))
struct GuestFilesTransferTests {
    @Test func aDownloadCopiesTheFileAndCountsIt() throws {
        let body = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 3) })
        let guest = storage([root + "/v.mp4": .file(body, mtime: 1)])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        let sink = MemorySink()
        var steps: [UInt64] = []
        let length = try GuestFiles.download(root + "/v.mp4", using: sync, into: sink, progress: { steps.append($0) })
        #expect(length == UInt64(body.count) && sink.data == body)
        #expect(steps.first == 0 && steps.last == UInt64(body.count) && steps == steps.sorted())
    }

    @Test func aMissingFileIsMissing() throws {
        let guest = storage()
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let sync = try AdbSync(connection: connection)
        #expect(throws: GuestFilesError.missing) {
            try GuestFiles.download(root + "/nope", using: sync, into: MemorySink(), progress: { _ in })
        }
    }

    @Test func anUploadGoesToTheTemporaryNameAndOneMoveGivesItsName() throws {
        let body = Data((0..<500_000).map { UInt8(truncatingIfNeeded: $0 &* 11) })
        let guest = storage([root + "/Download": .folder])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let temporary = GuestFiles.temporaryPath(in: root + "/Download", token: "ab12cd34")
        #expect(temporary == root + "/Download/.andarium-ab12cd34.part")
        var steps: [UInt64] = []
        let length = try GuestFiles.upload(
            MemorySource(body), modified: Date(timeIntervalSince1970: 1_700_000_000),
            to: root + "/Download/Clip.mov", temporary: temporary, replacing: false,
            on: try connected(adbd), progress: { steps.append($0) })
        #expect(length == UInt64(body.count))
        #expect(guest.sent(temporary)?.data == body && guest.sent(temporary)?.mtime == 1_700_000_000)
        #expect(guest.sent(root + "/Download/Clip.mov") == nil)
        #expect(steps.last == UInt64(body.count))
        #expect(guest.shellCommands == [
            "if [ -e '\(root)/Download/Clip.mov' ] || [ -L '\(root)/Download/Clip.mov' ]; then exit 17; fi; "
                + "mv -- '\(temporary)' '\(root)/Download/Clip.mov' && { "
                + "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://\(root)/Download/Clip.mov'"
                + " >/dev/null 2>&1; true; }"
        ])
    }

    @Test func replacingMovesOverTheOldFileAndNeverOverAFolder() throws {
        let guest = storage([root + "/D": .folder])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        try GuestFiles.upload(
            MemorySource(Data(count: 10)), modified: .now, to: root + "/D/it's.txt", temporary: root + "/D/.andarium-1.part",
            replacing: true, on: connection, progress: { _ in })
        #expect(guest.shellCommands == [
            "if [ -d '\(root)/D/it'\\''s.txt' ]; then exit 21; fi; "
                + "mv -f -- '\(root)/D/.andarium-1.part' '\(root)/D/it'\\''s.txt' && { "
                + "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://\(root)/D/it%27s.txt'"
                + " >/dev/null 2>&1; true; }"
        ])
    }

    @Test func aNameThatIsTakenIsRefusedBeforeAnythingIsReplaced() throws {
        let guest = storage([root + "/D": .folder])
        guest.shell = { _ in (17, "") }
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        #expect(throws: GuestFilesError.exists) {
            try GuestFiles.upload(
                MemorySource(Data(count: 10)), modified: .now, to: root + "/D/a", temporary: root + "/D/.andarium-1.part",
                replacing: false, on: connection, progress: { _ in })
        }
    }

    @Test func aFolderOfThatNameIsInTheWayOfAReplace() throws {
        let guest = storage([root + "/D": .folder])
        guest.shell = { _ in (21, "") }
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        #expect(throws: GuestFilesError.folderInTheWay) {
            try GuestFiles.upload(
                MemorySource(Data(count: 10)), modified: .now, to: root + "/D/a", temporary: root + "/D/.andarium-1.part",
                replacing: true, on: connection, progress: { _ in })
        }
    }

    @Test func aMoveThatFailsReportsTheShellsWords() throws {
        let guest = storage([root + "/D": .folder])
        guest.shell = { _ in (1, "mv: rename failed: Permission denied\n") }
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        #expect(throws: AdbError.remote("mv: rename failed: Permission denied")) {
            try GuestFiles.upload(
                MemorySource(Data(count: 10)), modified: .now, to: root + "/D/a", temporary: root + "/D/.andarium-1.part",
                replacing: true, on: connection, progress: { _ in })
        }
    }

    @Test func aFileThatArrivedShortIsNotMoved() throws {
        let guest = storage([root + "/D": .folder])
        guest.truncatesSends = true
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        #expect(throws: GuestFilesError.changed) {
            try GuestFiles.upload(
                MemorySource(Data(count: 1000)), modified: .now, to: root + "/D/a", temporary: root + "/D/.andarium-1.part",
                replacing: true, on: connection, progress: { _ in })
        }
        #expect(guest.shellCommands.isEmpty)
    }

    @Test func aFailedSendNeverRunsTheMove() throws {
        let guest = storage([root + "/D": .folder])
        guest.sendFailure = "No space left on device"
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        #expect(throws: AdbError.remote("No space left on device")) {
            try GuestFiles.upload(
                MemorySource(Data(count: 1000)), modified: .now, to: root + "/D/a", temporary: root + "/D/.andarium-1.part",
                replacing: true, on: connection, progress: { _ in })
        }
        #expect(guest.shellCommands.isEmpty && guest.sent(root + "/D/a") == nil)
    }

    @Test func aTemporaryNameInAnotherFolderOrOutsideTheRootIsRefused() throws {
        let guest = storage([root + "/D": .folder])
        let adbd = FakeAdbd(features: v2Features, services: guest.services)
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        for (path, temporary) in [
            (root + "/D/a", root + "/E/.andarium-1.part"),
            ("/data/a", "/data/.andarium-1.part"),
            (root + "/D/..", root + "/D/.andarium-1.part"),
            (root + "/D/" + String(repeating: "x", count: 256), root + "/D/.andarium-1.part"),
        ] {
            #expect(throws: GuestFilesError.badPath) {
                try GuestFiles.upload(
                    MemorySource(Data(count: 1)), modified: .now, to: path, temporary: temporary,
                    replacing: true, on: connection, progress: { _ in })
            }
        }
        #expect(guest.requests.isEmpty)
    }
}

struct GuestFilesHelperTests {
    @Test func freeSpaceIsReadFromDfsLastLine() {
        let output = """
            Filesystem     1024-blocks    Used Available Capacity Mounted on
            /dev/fuse          7872332 2431120   5441212      31% /storage/emulated

            """
        #expect(GuestFiles.parseFreeSpace(output) == 5_441_212 * 1024)
        #expect(GuestFiles.parseFreeSpace("df: /nope: No such file or directory\n") == nil)
        #expect(GuestFiles.parseFreeSpace("") == nil)
        #expect(GuestFiles.parseFreeSpace("Filesystem 1024-blocks Used Available Capacity Mounted on\nx y z w") == nil)
        #expect(GuestFiles.freeSpaceCommand(for: root) == "df -Pk '\(root)'")
    }

    @Test func theGuestKeepsItsReserve() {
        let reserve = GuestFiles.guestReserveBytes
        #expect(GuestFiles.guestHasRoom(for: 1000, free: 1000 + reserve))
        #expect(!GuestFiles.guestHasRoom(for: 1000, free: 1000 + reserve - 1))
        #expect(!GuestFiles.guestHasRoom(for: 5000, free: 4000))
        #expect(GuestFiles.guestHasRoom(for: 0, free: reserve))
    }

    @Test func theDeviceKeepsRoomForTheRunsOwnWrites() {
        let reserve = TransferRoom.deviceReserveBytes
        #expect(reserve > RunSpace.pauseBelowBytes)
        #expect(TransferRoom.deviceHasRoom(for: 10, free: 10 + reserve))
        #expect(!TransferRoom.deviceHasRoom(for: 10, free: 10 + reserve - 1))
        #expect(!TransferRoom.deviceHasRoom(for: 100, free: 50))
    }

    @Test func pathsStayInsideInternalStorage() {
        #expect(GuestFilePath.isInsideRoot(root))
        #expect(GuestFilePath.isInsideRoot(root + "/DCIM/Camera"))
        for path in ["", "/", "/storage", root + "/", root + "//a", root + "/a/../b", root + "/./a", root + "x", "/storage/emulated/01/a"] {
            #expect(!GuestFilePath.isInsideRoot(path), "\(path)")
        }
        #expect(GuestFilePath.parent(of: root + "/a/b") == root + "/a")
        #expect(GuestFilePath.parent(of: "/a") == "/")
        #expect(GuestFilePath.name(of: root + "/a/b.txt") == "b.txt")
        #expect(GuestFilePath.child("x", of: root) == root + "/x")
    }

    @Test func namesAreOneComponentOfAtMost255Bytes() {
        #expect(GuestFilePath.isValidName("IMG 1 (copy).jpg"))
        #expect(GuestFilePath.isValidName(String(repeating: "a", count: 255)))
        #expect(!GuestFilePath.isValidName(String(repeating: "a", count: 256)))
        #expect(!GuestFilePath.isValidName(String(repeating: "\u{00E9}", count: 128)))
        for name in ["", ".", "..", "a/b", "a\0b"] { #expect(!GuestFilePath.isValidName(name)) }
    }

    @Test func aSecondCopyIsNumberedTheWayAndroidDoes() {
        #expect(GuestFileNames.freeName(for: "Clip.mov", taken: ["Clip.mov"]) == "Clip (1).mov")
        #expect(GuestFileNames.freeName(for: "Clip.mov", taken: ["Clip.mov", "Clip (1).mov", "Clip (2).mov"]) == "Clip (3).mov")
        #expect(GuestFileNames.freeName(for: "README", taken: ["README"]) == "README (1)")
        #expect(GuestFileNames.freeName(for: "a.tar.gz", taken: []) == "a.tar (1).gz")
    }

    @Test func pickedNamesThatRepeatAreMadeDistinct() {
        #expect(GuestFileNames.distinct(["a.jpg", "b.jpg", "a.jpg", "a.jpg"]) == ["a.jpg", "b.jpg", "a (1).jpg", "a (2).jpg"])
        #expect(GuestFileNames.distinct(["a.jpg", "a (1).jpg", "a.jpg"]) == ["a.jpg", "a (1).jpg", "a (2).jpg"])
        #expect(GuestFileNames.distinct(["x", "y"]) == ["x", "y"])
        #expect(GuestFileNames.distinct([]) == [])
    }

    @Test func partialFilesAreRecognisedAndSweptByPattern() {
        #expect(GuestFiles.isPartial(".andarium-0f1e2d3c.part"))
        #expect(!GuestFiles.isPartial("clip.part") && !GuestFiles.isPartial(".andarium-x.txt"))
        #expect(GuestFiles.removeCommand(for: root + "/a/.andarium-1.part") == "rm -f -- '\(root)/a/.andarium-1.part'")
        #expect(GuestFiles.removeStaleCommand(in: root + "/it's") == "rm -f -- '\(root)/it'\\''s'/.andarium-*.part")
    }
}
