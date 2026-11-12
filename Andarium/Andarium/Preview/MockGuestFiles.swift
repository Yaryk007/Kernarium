// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI

/// The Simulator preview's internal storage: a tree that changes as files are
/// imported, with the pace and the failures of a real transfer.
final class MockGuestFiles: GuestFilesBackend {
    /// A file or folder of the tree.
    fileprivate final class Node {
        var size: UInt64
        var modified: Date
        var children: [String: Node]?
        var refused = false

        init(size: UInt64 = 0, modified: Date, children: [String: Node]? = nil, refused: Bool = false) {
            self.size = size
            self.modified = modified
            self.children = children
            self.refused = refused
        }
    }

    private let root = MockTree.make()
    private var free = UInt64(10.6 * Double(Limits.bytesPerGiB))

    /// Folders whose listing never comes, for the loading state.
    var heldFolders: Set<String> = []
    /// Names that fail part way, with the reason.
    var failing: [String: String] = [:]
    /// Failing names that work on Try Again.
    var recovering: Set<String> = []

    private func node(at path: String) -> Node? {
        guard path.hasPrefix(GuestPath.root) else { return nil }
        var current = root
        for part in path.dropFirst(GuestPath.root.count).split(separator: "/") {
            guard let next = current.children?[String(part)] else { return nil }
            current = next
        }
        return current
    }

    private func file(at path: String) -> GuestFile? {
        node(at: path).map { GuestFile(path: path, isFolder: $0.children != nil, size: $0.size, modified: $0.modified) }
    }

    func list(_ folder: String) async throws -> [GuestFile] {
        // A big folder takes the guest a moment.
        try await Task.sleep(for: .milliseconds(folder.hasSuffix("/Camera") ? 500 : 90))
        while heldFolders.contains(folder) { try await Task.sleep(for: .seconds(60)) }
        guard let node = node(at: folder), let children = node.children else {
            throw GuestFolderProblem.unavailable("The folder is no longer there.")
        }
        if node.refused { throw GuestFolderProblem.refused }
        return children.map { name, child in
            GuestFile(
                path: GuestPath.child(name, of: folder), isFolder: child.children != nil,
                size: child.size, modified: child.modified
            )
        }
    }

    func freeBytes() async -> UInt64? { free }

    func prepareImport(bytes: UInt64, into folder: String) async throws {}

    func prepareExport(bytes: UInt64) async throws {}

    func measure(_ file: GuestFile) async throws -> UInt64 {
        try await Task.sleep(for: .milliseconds(600))
        return node(at: file.path).map(Self.bytes) ?? 0
    }

    private static func bytes(of node: Node) -> UInt64 {
        guard let children = node.children else { return node.size }
        return children.values.reduce(0) { $0 + bytes(of: $1) }
    }

    /// Long enough to watch, whatever the size.
    private func move(_ size: UInt64, failing reason: String?, progress: @Sendable (UInt64) -> Void) async throws {
        let seconds = min(6, 0.7 + Double(size) / Double(Limits.bytesPerGiB) * 1.6)
        let steps = max(Int(seconds * 20), 1)
        for step in 1...steps {
            try await Task.sleep(for: .milliseconds(50))
            if let reason, step * 5 >= steps * 3 {
                throw Failure(message: reason)
            }
            progress(UInt64(Double(size) * Double(step) / Double(steps)))
        }
    }

    struct Failure: LocalizedError {
        var message: String
        var errorDescription: String? { message }
    }

    func send(
        _ pick: FilePick, to path: String, replacing: Bool, progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) async throws {
        let reason = failing[pick.name]
        if recovering.contains(pick.name) { failing[pick.name] = nil }
        try await move(pick.size, failing: reason) { progress($0, pick.size) }
        let name = GuestPath.name(of: path)
        guard let parent = node(at: GuestPath.parent(of: path)) else { return }
        free -= min(free, pick.size)
        parent.children?[name] = Node(size: pick.size, modified: MockTree.now)
    }

    func receive(
        _ file: GuestFile, into directory: URL, progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) async throws -> URL {
        let total = try await measure(file)
        try await move(total, failing: failing[file.name]) { progress($0, total) }
        let url = directory.appendingPathComponent(file.name, isDirectory: file.isFolder)
        if file.isFolder {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            for name in ["Part 1.txt", "Part 2.txt"] {
                try Self.placeholder.write(to: url.appendingPathComponent(name))
            }
        } else {
            try Self.placeholder.write(to: url)
        }
        return url
    }

    private static let placeholder = Data("A mock file from the Andarium preview.\n".utf8)

    func importPickerStandIn(deliver: @escaping ([FilePick]) -> Void) -> AnyView? {
        AnyView(MockImportPicker(files: self, deliver: deliver))
    }

    /// Two names of the folder and one that is free.
    func clashingPicks(in folder: String) -> [FilePick] {
        let names = node(at: folder)?.children?.filter { $0.value.children == nil }.keys.sorted().prefix(2) ?? []
        guard !names.isEmpty else { return MockTree.photoPicks }
        return names.map { FilePick(name: $0, size: 6 * Limits.bytesPerMiB) } + [FilePick(name: "New notes.txt", size: 18_000)]
    }

    // MARK: Launch states

    /// Puts `browser` in the state of a `-screen files…` launch.
    static func present(_ screen: String, browser: FileBrowserModel, files: MockGuestFiles, model: AppModel) {
        let root = GuestPath.root
        func folder(_ parts: String...) -> [FolderRoute] {
            var routes = [FolderRoute(path: root)]
            var path = root
            for part in parts {
                path = GuestPath.child(part, of: path)
                routes.append(FolderRoute(path: path))
            }
            return routes
        }
        func item(_ name: String, _ size: UInt64, moved: UInt64 = 0, state: FileTransfer.ItemState = .waiting) -> FileTransfer.Item {
            FileTransfer.Item(name: name, size: size, moved: moved, state: state)
        }
        func held(_ direction: FileTransfer.Direction, _ path: String, _ items: [FileTransfer.Item]) -> FileTransfer {
            let transfer = FileTransfer(direction: direction, folder: path, items: items)
            transfer.stays = true
            return transfer
        }
        let gib = Limits.bytesPerGiB
        let mib = Limits.bytesPerMiB
        let download = GuestPath.child("Download", of: root)
        let movies = GuestPath.child("Movies", of: root)
        var route = folder()
        switch screen {
        case "files-deep":
            route = folder("Pictures", "Trips", "2026 Lisbon", "Edited")
        case "files-loading":
            route = folder("DCIM", "Camera")
            files.heldFolders = [route[2].path]
        case "files-empty":
            route = folder("Audiobooks")
        case "files-refused":
            route = folder("Android", "data")
        case "files-usb-off":
            // The guest's USB debugging is off and the browser lists all the same.
            model.session.usbDebugging = false
        case "files-starting":
            // Android's storage comes up a few seconds in, and the browser follows.
            model.session.storageReady = false
            Task {
                try? await Task.sleep(for: .seconds(6))
                model.session.storageReady = true
            }
        case "files-select", "files-review":
            route = folder("DCIM", "Camera")
            let camera = route[2].path
            let photos = ["IMG_20261006_132805.jpg", "IMG_20261012_195948.jpg", "IMG_20261021_173355.jpg"]
            browser.selection = (photos.map { GuestPath.child($0, of: camera) } + [GuestPath.child("Boarding pass.pdf", of: download)])
                .compactMap(files.file(at:))
            browser.reviewingSelection = false
        case "files-transfer", "files-transfer-open", "files-transfer-long":
            // The long one stays in the root, which scrolls on a phone.
            route = screen == "files-transfer-long" ? folder() : folder("Download")
            let transfer = held(.importing, download, [
                item("Lisbon walk, full.mov", 4 * gib + 716 * mib, moved: 2 * gib + 100 * mib, state: .moving),
                item("Tram 28.mov", 812 * mib),
                item("Harbour.jpg", 6 * mib),
            ])
            transfer.expanded = screen == "files-transfer-open"
            browser.transfer = transfer
        case "files-done":
            route = folder("Download")
            let transfer = held(.importing, download, [
                item("Harbour.jpg", 6 * mib, moved: 6 * mib, state: .done),
                item("Miradouro.jpg", 7 * mib, moved: 7 * mib, state: .done),
                item("Tiles.jpg", 5 * mib, moved: 5 * mib, state: .done),
            ])
            transfer.stage = .finished
            browser.transfer = transfer
            files.add(["Harbour.jpg", "Miradouro.jpg", "Tiles.jpg"], to: download)
            browser.fresh = Set(["Harbour.jpg", "Miradouro.jpg", "Tiles.jpg"].map { GuestPath.child($0, of: download) })
        case "files-done-export":
            route = folder("Movies")
            let transfer = held(.exporting, movies, [
                item("Lisbon walk.mp4", 4 * gib + 716 * mib, moved: 4 * gib + 716 * mib, state: .done),
                item("Birthday 2026.mov", 1 * gib + 300 * mib, moved: 1 * gib + 300 * mib, state: .done),
            ])
            transfer.stage = .ready
            browser.transfer = transfer
        case "files-share":
            route = folder("Movies")
            let transfer = held(.exporting, movies, [item("Lisbon walk.mp4", 4 * gib + 716 * mib, moved: 4 * gib + 716 * mib, state: .done)])
            transfer.stage = .ready
            let staged = FileManager.default.temporaryDirectory.appendingPathComponent("Lisbon walk.mp4")
            try? Self.placeholder.write(to: staged)
            transfer.staged = [staged]
            browser.transfer = transfer
        case "files-failed", "files-failed-open":
            route = folder("Download")
            let transfer = held(.importing, download, [
                item("Harbour.jpg", 6 * mib, moved: 6 * mib, state: .done),
                item("Lisbon walk, full.mov", 14 * gib, state: .failed("Not enough space in the guest")),
                item("Tram 28.mov", 12 * gib, state: .failed("Not enough space in the guest")),
            ])
            transfer.stage = .failed
            transfer.expanded = screen == "files-failed-open"
            browser.transfer = transfer
            files.add(["Harbour.jpg"], to: download)
            browser.fresh = [GuestPath.child("Harbour.jpg", of: download)]
        case "files-stopped":
            route = folder("Download")
            let reason = FileBrowserModel.leftForegroundReason
            let transfer = held(.importing, download, [
                item("Lisbon walk, full.mov", 4 * gib + 716 * mib, state: .failed(reason)),
                item("Tram 28.mov", 812 * mib, state: .failed(reason)),
                item("Harbour.jpg", 6 * mib, moved: 6 * mib, state: .done),
            ])
            transfer.stage = .failed
            browser.transfer = transfer
            files.add(["Harbour.jpg"], to: download)
        case "files-conflict", "files-conflict-long":
            route = screen == "files-conflict-long" ? folder() : folder("Download")
            browser.conflict = ImportConflict(
                picks: [
                    FilePick(name: "Boarding pass.pdf", size: 182_000),
                    FilePick(name: "Invoice 2026-10.pdf", size: 96_000),
                    FilePick(name: "New notes.txt", size: 18_000),
                ],
                clashing: [
                    FilePick(name: "Boarding pass.pdf", size: 182_000),
                    FilePick(name: "Invoice 2026-10.pdf", size: 96_000),
                ],
                folder: download
            )
        default:
            break
        }
        browser.route = route
        model.sessionPreset = SessionPreset(
            settings: true, settingsLarge: !["files-medium", "files-starting"].contains(screen)
        )
        model.phase = .running(.normal)
    }

    private func add(_ names: [String], to folder: String) {
        guard let parent = node(at: folder) else { return }
        for name in names {
            parent.children?[name] = Node(size: 6 * Limits.bytesPerMiB, modified: MockTree.now)
        }
    }
}

/// The preview's document picker: the Simulator has no documents to pick, so
/// it offers the results the flows need.
private struct MockImportPicker: View {
    @Environment(\.dismiss) private var dismiss
    let files: MockGuestFiles
    let deliver: ([FilePick]) -> Void
    @State private var folder = GuestPath.root

    var body: some View {
        NavigationStack {
            List {
                Section("Files") {
                    pick("3 photos", detail: "18 MiB", MockTree.photoPicks)
                    pick("A long video", detail: "4.7 GiB", [FilePick(name: "Lisbon walk, full.mov", size: 4 * Limits.bytesPerGiB + 716 * Limits.bytesPerMiB)])
                }
                Section("Cases") {
                    Button("Names already in this folder") { finish(MockFolderProbe.clashes(files)) }
                    Button("A file the guest has no room for") {
                        let name = "Disk image.iso"
                        files.failing[name] = "Not enough space in the guest"
                        finish([FilePick(name: name, size: 14 * Limits.bytesPerGiB)])
                    }
                    Button("One of three fails") {
                        let name = "Unreadable.bin"
                        files.failing[name] = "The file couldn\u{2019}t be read"
                        files.recovering.insert(name)
                        finish([
                            FilePick(name: "Notes.txt", size: 18_000),
                            FilePick(name: name, size: 40 * Limits.bytesPerMiB),
                            FilePick(name: "Scan.pdf", size: 2 * Limits.bytesPerMiB),
                        ])
                    }
                }
            }
            .navigationTitle("Mock Picker")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", systemImage: "xmark") { dismiss() }
                }
            }
        }
    }

    private func pick(_ title: String, detail: String, _ picks: [FilePick]) -> some View {
        Button {
            finish(picks)
        } label: {
            LabeledContent(title, value: detail)
        }
    }

    private func finish(_ picks: [FilePick]) {
        dismiss()
        deliver(picks)
    }
}

/// The folder on screen, for the picker's case of names that clash.
@MainActor
enum MockFolderProbe {
    static weak var browser: FileBrowserModel?

    static func clashes(_ files: MockGuestFiles) -> [FilePick] {
        files.clashingPicks(in: browser?.current ?? GuestPath.root)
    }
}

/// The mock internal storage. The dates are fixed, so a screenshot is the
/// same on any day.
private enum MockTree {
    static let now = Date(timeIntervalSince1970: 1_794_182_400)

    static func date(daysAgo: Int) -> Date { now.addingTimeInterval(Double(-daysAgo) * 86400) }

    private typealias Node = MockGuestFiles.Node

    private static func file(_ size: UInt64, _ daysAgo: Int) -> Node {
        Node(size: size, modified: date(daysAgo: daysAgo))
    }

    private static func folder(_ daysAgo: Int, _ children: [String: Node] = [:], refused: Bool = false) -> Node {
        Node(modified: date(daysAgo: daysAgo), children: children, refused: refused)
    }

    private static let mib = Limits.bytesPerMiB
    private static let gib = Limits.bytesPerGiB

    static let photoPicks = [
        FilePick(name: "Harbour.jpg", size: 6 * mib),
        FilePick(name: "Miradouro.jpg", size: 7 * mib),
        FilePick(name: "Tiles.jpg", size: 5 * mib),
    ]

    /// The camera's roll, named the way a phone names it.
    private static func cameraRoll() -> [String: Node] {
        var roll: [String: Node] = [:]
        let stamps = [
            ("20261102", "141207", 7), ("20261102", "141455", 7), ("20261102", "152033", 7),
            ("20261031", "093218", 9), ("20261031", "101544", 9), ("20261029", "184402", 11),
            ("20261029", "184917", 11), ("20261025", "120310", 15), ("20261021", "173355", 19),
            ("20261018", "081126", 22), ("20261012", "195948", 28), ("20261006", "132805", 34),
        ]
        for (index, stamp) in stamps.enumerated() {
            let size = UInt64(3_400_000 + (index * 217_000) % 1_900_000)
            roll["IMG_\(stamp.0)_\(stamp.1).jpg"] = file(index == 0 ? 4_180_000 : index == 1 ? 3_960_000 : size, stamp.2)
        }
        roll["VID_20261101_190122.mp4"] = file(312 * mib, 8)
        roll["VID_20261027_211840.mp4"] = file(1 * gib + 80 * mib, 13)
        return roll
    }

    static func make() -> MockGuestFiles.Node {
        folder(0, [
            "Alarms": folder(120),
            "Android": folder(3, [
                "data": folder(3, refused: true),
                "media": folder(14, [
                    "org.lineageos.aperture": folder(14, ["Backup": folder(40)]),
                    "org.lineageos.recorder": folder(21),
                ]),
                "obb": folder(46, refused: true),
            ]),
            "Audiobooks": folder(120),
            "DCIM": folder(7, [
                "Camera": folder(7, cameraRoll()),
                "Screenshots": folder(2, [
                    "Screenshot_20261107-101522.png": file(1_240_000, 2),
                    "Screenshot_20261104-223011.png": file(980_000, 5),
                    "Screenshot_20261030-081745.png": file(1_610_000, 10),
                ]),
            ]),
            "Documents": folder(4, [
                "Letters": folder(62, ["Landlord.odt": file(26_000, 62)]),
                "Taxes 2025": folder(201, ["Return.pdf": file(640_000, 201), "Receipts.zip": file(38 * mib, 203)]),
                "Reading list.md": file(4_200, 4),
                "Lease.pdf": file(1_900_000, 88),
            ]),
            "Download": folder(1, [
                "Annual report 2026 final (reviewed by legal) signed copy.pdf": file(8_300_000, 1),
                "Boarding pass.pdf": file(182_000, 3),
                "F-Droid.apk": file(11_400_000, 6),
                "Invoice 2026-10.pdf": file(96_000, 9),
                "Map tiles Portugal.zip": file(1 * gib + 300 * mib, 16),
                "Recipe.txt": file(3_100, 20),
                "Telegram": folder(5, ["Report.pdf": file(712_000, 5)]),
                "Podcast sample.mp3": file(44 * mib, 12),
            ]),
            "Movies": folder(8, [
                "Birthday 2026.mov": file(1 * gib + 300 * mib, 8),
                "Lisbon walk.mp4": file(4 * gib + 716 * mib, 8),
                "Trailer.mkv": file(812 * mib, 30),
            ]),
            "Music": folder(44, [
                "Night Drive": folder(44, [
                    "01 Overpass.flac": file(38 * mib, 44),
                    "02 Tunnels.flac": file(44 * mib, 44),
                ]),
                "Voice warmup.m4a": file(7 * mib, 90),
            ]),
            "Notifications": folder(120, ["Chime.ogg": file(42_000, 120)]),
            "Pictures": folder(6, [
                "Trips": folder(6, [
                    "2026 Lisbon": folder(6, [
                        "Edited": folder(6, [
                            "Prints": folder(6, ["Tiles A3.tiff": file(62 * mib, 6)]),
                            "Alfama at dusk.jpg": file(7 * mib, 6),
                            "Belem.jpg": file(6 * mib, 6),
                            "Harbour crop.jpg": file(5 * mib, 6),
                            "Miradouro wide.jpg": file(9 * mib, 7),
                            "Tram 28.jpg": file(6 * mib, 7),
                        ]),
                        "Originals": folder(7),
                    ]),
                ]),
                "Wallpapers": folder(70, ["Dune.png": file(4 * mib, 70)]),
            ]),
            "Podcasts": folder(120),
            "Recordings": folder(13, ["Meeting 3 Nov.m4a": file(21 * mib, 6)]),
            "Ringtones": folder(120),
        ])
    }
}
