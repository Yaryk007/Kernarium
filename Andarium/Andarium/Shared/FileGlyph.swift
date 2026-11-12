// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// What a file is, from its extension: nothing is read from the file to tell.
nonisolated enum GuestFileKind: Hashable, Sendable {
    case image, video, audio, pdf, text, sheet, archive, app, other

    init(name: String) {
        switch (name as NSString).pathExtension.lowercased() {
        case "jpg", "jpeg", "png", "webp", "heic", "heif", "gif", "bmp", "dng", "avif": self = .image
        case "mp4", "mkv", "mov", "webm", "3gp", "avi", "m4v": self = .video
        case "mp3", "m4a", "m4b", "flac", "ogg", "wav", "opus", "aac", "amr": self = .audio
        case "pdf", "epub": self = .pdf
        case "txt", "md", "log", "json", "xml", "csv", "doc", "docx", "odt", "rtf", "ppt", "pptx", "odp": self = .text
        case "xls", "xlsx", "ods": self = .sheet
        case "zip", "7z", "rar", "tar", "gz", "xz", "bz2", "zst", "iso", "obb": self = .archive
        case "apk", "apks", "xapk": self = .app
        default: self = .other
        }
    }

    var symbol: String {
        switch self {
        case .image: "photo.fill"
        case .video: "film.fill"
        case .audio: "music.note"
        case .pdf: "doc.richtext.fill"
        case .text: "doc.text.fill"
        case .sheet: "tablecells.fill"
        case .archive: "doc.zipper"
        case .app: "shippingbox.fill"
        case .other: "doc.fill"
        }
    }

    var color: Color {
        switch self {
        case .image: .indigo
        case .video: .pink
        case .audio: .orange
        case .pdf: .red
        case .text: .blue
        case .sheet: .green
        case .archive: .brown
        case .app: .mint
        case .other: .gray
        }
    }
}

/// A folder or a file as a coloured icon: a folder in the accent colour with
/// its standard purpose marked on it at the top of internal storage, a file as
/// a tile in the colour of its kind. Selected, it is a check on the accent.
struct FileGlyph: View {
    let name: String
    let isFolder: Bool
    /// A folder at the top of internal storage, where Android's standard
    /// folders are.
    var topLevel = false
    var size: CGFloat = 32
    var selected = false

    init(file: GuestFile, size: CGFloat = 32, selected: Bool = false) {
        self.init(
            name: file.name, isFolder: file.isFolder, topLevel: GuestPath.parent(of: file.path) == GuestPath.root,
            size: size, selected: selected)
    }

    init(name: String, isFolder: Bool, topLevel: Bool = false, size: CGFloat = 32, selected: Bool = false) {
        self.name = name
        self.isFolder = isFolder
        self.topLevel = topLevel
        self.size = size
        self.selected = selected
    }

    /// Android's standard folders, marked like a Mac's.
    private static let standardFolders: [String: String] = [
        "Alarms": "alarm.fill", "Android": "gearshape.fill", "Audiobooks": "book.fill", "DCIM": "camera.fill",
        "Documents": "doc.fill", "Download": "arrow.down", "Movies": "film.fill", "Music": "music.note",
        "Notifications": "bell.fill", "Pictures": "photo.fill", "Podcasts": "mic.fill",
        "Recordings": "waveform", "Ringtones": "music.note.list",
    ]

    var body: some View {
        Group {
            if selected {
                check
            } else if isFolder {
                folder
            } else {
                tile
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }

    private var check: some View {
        Image(systemName: "checkmark")
            .font(.system(size: size * 0.44, weight: .bold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(Color.accentColor.gradient, in: .rect(cornerRadius: size * 0.26, style: .continuous))
    }

    private var folder: some View {
        let inner = topLevel ? Self.standardFolders[name] : nil
        return ZStack {
            Image(systemName: "folder.fill")
                .resizable()
                .scaledToFit()
                .foregroundStyle(Color.accentColor.gradient)
            if let inner, size >= 26 {
                Image(systemName: inner)
                    .font(.system(size: size * 0.3, weight: .semibold))
                    .foregroundStyle(.white)
                    .offset(y: size * 0.07)
            }
        }
    }

    private var tile: some View {
        let kind = GuestFileKind(name: name)
        return Image(systemName: kind.symbol)
            .font(.system(size: size * 0.5, weight: .semibold))
            .foregroundStyle(kind.color)
            .frame(width: size, height: size)
            .background(kind.color.opacity(0.14), in: .rect(cornerRadius: size * 0.24, style: .continuous))
    }
}
