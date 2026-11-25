// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation

extension SystemCatalog {
    @concurrent static func download(_ skin: MysticSkin, progress: @escaping SystemImageImport.Progress) async throws -> Entry {
        let status = "Downloading \(skin.name)"
        progress(0, status)
        let delegate = SkinDownloadProgress(total: skin.bytes) { progress($0, status) }
        let (archive, response) = try await URLSession.shared.download(from: skin.url, delegate: delegate)
        defer { try? FileManager.default.removeItem(at: archive) }
        guard let http = response as? HTTPURLResponse, http.statusCode == 200,
              (try archive.resourceValues(forKeys: [.fileSizeKey])).fileSize == Int(skin.bytes) else {
            throw ImportError.invalid("MysticGSI did not return the complete \(skin.name) download.")
        }
        try verifyDownload(archive, skin: skin, progress: progress)
        return try importDownload(archive, name: skin.name, progress: progress)
    }

    private nonisolated static func verifyDownload(_ archive: URL, skin: MysticSkin, progress: SystemImageImport.Progress) throws {
        let handle = try FileHandle(forReadingFrom: archive)
        defer { try? handle.close() }
        var digest = Insecure.MD5()
        var checked: Int64 = 0
        while let chunk = try handle.read(upToCount: 4 << 20), !chunk.isEmpty {
            digest.update(data: chunk)
            checked += Int64(chunk.count)
            progress(Double(checked) / Double(skin.bytes), "Checking the download")
        }
        guard digest.finalize().map({ String(format: "%02x", $0) }).joined() == skin.md5 else {
            throw ImportError.invalid("The download did not match MysticGSI’s published checksum.")
        }
    }
}

private nonisolated final class SkinDownloadProgress: NSObject, URLSessionDownloadDelegate {
    let total: Int64
    let progress: @Sendable (Double) -> Void

    init(total: Int64, progress: @escaping @Sendable (Double) -> Void) {
        self.total = total
        self.progress = progress
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        progress(Double(totalBytesWritten) / Double(total))
    }

    // URLSession's async download method returns the temporary file.
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {}
}
