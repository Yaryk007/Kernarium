// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Files for a `TextMessageNotice` that live in memory, record every call the
/// notice makes and fail the calls a test names. What they keep is what a
/// notice made afterwards over the same directory reads.
public final class RecordingNoticeFiles: NoticeFiles, @unchecked Sendable {
    private let lock = NSLock()
    private let base = MemoryNoticeFiles()
    private var callLog: [String] = []
    private var failing: Set<String> = []
    private var firedLog: [String] = []

    public init() {}

    /// Each call the notice made, in order: "replace", "flushFile", "flushDirectory".
    public var calls: [String] { lock.withLock { callLog } }
    /// The calls that failed because a test said so.
    public var fired: [String] { lock.withLock { firedLog } }
    public func clearCalls() { lock.withLock { callLog = [] } }
    /// From now on these calls fail, until `stop`.
    public func fail(_ operations: String...) { lock.withLock { failing = Set(operations) } }
    public func stop() { lock.withLock { failing = [] } }
    public func contents(_ url: URL) -> Data? { try? base.read(url) }
    public func put(_ data: Data, at url: URL) { try? base.replace(data, at: url) }

    private func step(_ name: String) throws {
        try lock.withLock {
            callLog.append(name)
            if failing.contains(name) {
                firedLog.append(name)
                throw POSIXError(.EIO)
            }
        }
    }

    public func read(_ url: URL) throws -> Data? { try base.read(url) }
    public func replace(_ data: Data, at url: URL) throws {
        try step("replace")
        try base.replace(data, at: url)
    }
    public func flushFile(at url: URL) throws { try step("flushFile") }
    public func flushDirectory(at url: URL) throws { try step("flushDirectory") }
}
