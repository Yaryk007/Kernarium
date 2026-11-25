// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

#if canImport(FoundationNetworking)
    import FoundationNetworking
#endif

/// Downloads a file of known size over several connections. A host that sends
/// each request to a mirror of its choice may choose a slow one, so every
/// connection asks for its own, and one that fails or falls far behind goes to
/// the fastest seen. A piece that fails is asked for again from where it ended. The caller
/// checks the whole file's digest: the mirrors are not compared here.
public enum SegmentedDownload {
    public enum Problem: Error, Equatable, Sendable {
        /// The server does not answer a range of a file of this size.
        case rangesUnsupported
        /// Every attempt of a run of them failed.
        case failed
    }

    static let pieceBytes: Int64 = 8 << 20
    /// A connection below this part of the fastest one's rate, or below this
    /// many bytes a second, is left for another.
    static let slowShare = 0.25
    static let slowRate = 64.0 * 1024
    /// How long a rate is measured over.
    static let window = 4.0

    /// `progress` hears the bytes on disk and the rate, about twice a second.
    public static func run(
        _ url: URL, to file: URL, bytes: Int64, connections: Int = 4,
        progress: @escaping @Sendable (_ received: Int64, _ bytesPerSecond: Int64) async -> Void
    ) async throws {
        guard bytes > 0, FileManager.default.createFile(atPath: file.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        let sizing = try FileHandle(forWritingTo: file)
        try sizing.truncate(atOffset: UInt64(bytes))
        try sizing.close()

        let pieces = Pieces(bytes: bytes)
        try await withThrowingTaskGroup(of: Void.self) { group in
            for _ in 0..<connections {
                group.addTask {
                    try await work(url, file: file, bytes: bytes, pieces: pieces, progress: progress)
                }
            }
            try await group.waitForAll()
        }
        guard await pieces.received == bytes else { throw Problem.failed }
    }

    private static func work(
        _ url: URL, file: URL, bytes: Int64, pieces: Pieces,
        progress: @escaping @Sendable (Int64, Int64) async -> Void
    ) async throws {
        let handle = try FileHandle(forWritingTo: file)
        defer { try? handle.close() }
        // HTTP/2 would carry every piece of one session over one connection.
        var transport = Transport()
        var session = transport.session()
        defer { session.finishTasksAndInvalidate() }
        // Where the last request was sent; nil asks the host for a mirror.
        var mirror: URL?
        let clock = ContinuousClock()
        while let piece = await pieces.next() {
            var position = piece.lowerBound
            do {
                try Task.checkCancellation()
                var request = URLRequest(url: mirror ?? url)
                request.setValue("bytes=\(piece.lowerBound)-\(piece.upperBound - 1)", forHTTPHeaderField: "Range")
                request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
                // A host may answer a client it takes for a browser with a page.
                request.setValue("Andarium", forHTTPHeaderField: "User-Agent")
                let expected = "bytes \(piece.lowerBound)-\(piece.upperBound - 1)/\(bytes)"
                let started = clock.now
                var windowStart = started
                var windowBytes: Int64 = 0
                for try await event in transport.start(request, expectedRange: expected, in: session) {
                    switch event {
                    case .answered(let address):
                        mirror = address
                    case .data(let data):
                        guard position + Int64(data.count) <= piece.upperBound else { throw Problem.rangesUnsupported }
                        try handle.seek(toOffset: UInt64(position))
                        try handle.write(contentsOf: data)
                        position += Int64(data.count)
                        if let update = await pieces.add(Int64(data.count)) {
                            await progress(update.received, update.bytesPerSecond)
                        }
                        windowBytes += Int64(data.count)
                        let elapsed = (clock.now - windowStart) / .seconds(1)
                        if elapsed >= window, position < piece.upperBound {
                            if await pieces.shouldLeave(rate: Double(windowBytes) / elapsed) { throw Slow() }
                            windowStart = clock.now
                            windowBytes = 0
                        }
                    }
                }
                guard position == piece.upperBound else { throw Problem.failed }
                await pieces.finished(
                    rate: Double(piece.count) / max((clock.now - started) / .seconds(1), 0.001), from: mirror)
            } catch {
                await pieces.giveBack(position..<piece.upperBound)
                try Task.checkCancellation()
                if let problem = error as? Problem, problem == .rangesUnsupported { throw problem }
                if error is CocoaError { throw error }
                mirror = await pieces.mirror(after: mirror)
                // The stream's end has cancelled its task.
                session.finishTasksAndInvalidate()
                transport = Transport()
                session = transport.session()
                if error is Slow { continue }
                guard await pieces.mayRetry() else { throw Problem.failed }
                try await Task.sleep(for: .seconds(1))
            }
        }
    }

    private struct Slow: Error {}

    /// What is left to download and how it goes.
    actor Pieces {
        private var left: [Range<Int64>] = []
        private(set) var received: Int64 = 0
        private var best = 0.0
        private var fastest: URL?
        private var changes = 0
        private var failures = 0
        private let clock = ContinuousClock()
        private var reportedAt: ContinuousClock.Instant
        private var reportedBytes: Int64 = 0

        init(bytes: Int64) {
            reportedAt = clock.now
            var start: Int64 = 0
            while start < bytes {
                left.append(start..<min(start + SegmentedDownload.pieceBytes, bytes))
                start += SegmentedDownload.pieceBytes
            }
            left.reverse()
        }

        func next() -> Range<Int64>? { left.popLast() }

        func giveBack(_ piece: Range<Int64>) {
            if !piece.isEmpty { left.append(piece) }
        }

        func add(_ count: Int64) -> (received: Int64, bytesPerSecond: Int64)? {
            received += count
            failures = 0
            let now = clock.now
            let elapsed = (now - reportedAt) / .seconds(1)
            guard elapsed >= 0.5 else { return nil }
            let rate = Int64(Double(received - reportedBytes) / elapsed)
            reportedAt = now
            reportedBytes = received
            return (received, rate)
        }

        func finished(rate: Double, from mirror: URL?) {
            guard rate > best else { return }
            best = rate
            fastest = mirror
        }

        /// Where a connection goes that leaves `mirror`: the fastest seen, or,
        /// from that one, wherever the host sends it.
        func mirror(after mirror: URL?) -> URL? {
            if mirror == fastest { fastest = nil }
            return fastest
        }

        /// Bounded, so that a file whose mirrors all differ still arrives.
        func shouldLeave(rate: Double) -> Bool {
            guard rate < max(best * SegmentedDownload.slowShare, SegmentedDownload.slowRate), changes < 60 else { return false }
            changes += 1
            return true
        }

        /// A run of failures with no byte between them ends the download.
        func mayRetry() -> Bool {
            failures += 1
            return failures <= 30
        }
    }

    private enum Event: Sendable {
        case answered(URL)
        case data(Data)
    }

    /// The session's delegate: each request's bytes as a stream.
    private final class Transport: NSObject, URLSessionDataDelegate, @unchecked Sendable {
        private struct Request {
            var continuation: AsyncThrowingStream<Event, any Error>.Continuation
            var expectedRange: String
        }

        func session() -> URLSession {
            let configuration = URLSessionConfiguration.ephemeral
            configuration.timeoutIntervalForRequest = 10
            configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
            return URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
        }

        private let lock = NSLock()
        private var requests: [Int: Request] = [:]

        func start(
            _ request: URLRequest, expectedRange: String, in session: URLSession
        ) -> AsyncThrowingStream<Event, any Error> {
            AsyncThrowingStream { continuation in
                let task = session.dataTask(with: request)
                let identifier = task.taskIdentifier
                lock.withLock { requests[identifier] = Request(continuation: continuation, expectedRange: expectedRange) }
                continuation.onTermination = { [weak self] _ in
                    task.cancel()
                    self?.lock.withLock { _ = self?.requests.removeValue(forKey: identifier) }
                }
                task.resume()
            }
        }

        private func request(_ task: URLSessionTask) -> Request? { lock.withLock { requests[task.taskIdentifier] } }

        func urlSession(
            _ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
            completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void
        ) {
            guard let request = request(dataTask), let http = response as? HTTPURLResponse, let address = http.url,
                  address.scheme?.lowercased() == "https"
            else { return completionHandler(.cancel) }
            switch http.statusCode {
            case 206 where http.value(forHTTPHeaderField: "Content-Range") == request.expectedRange:
                request.continuation.yield(.answered(address))
                completionHandler(.allow)
            case 200, 206:
                request.continuation.finish(throwing: Problem.rangesUnsupported)
                completionHandler(.cancel)
            default:
                request.continuation.finish(throwing: Problem.failed)
                completionHandler(.cancel)
            }
        }

        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
            request(dataTask)?.continuation.yield(.data(data))
        }

        func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?) {
            request(task)?.continuation.finish(throwing: error)
        }
    }
}
