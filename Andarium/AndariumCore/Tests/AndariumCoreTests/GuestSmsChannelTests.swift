// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Darwin
import Foundation
import Testing

/// The frame and the exchange of the text message channel: the golden bytes
/// (`tests/unit/andarium/fixtures/sms-golden-frames.txt`, which the guest's
/// own tests read), the outcomes over real socket pairs, the deadline and the
/// cancellation over a scripted system.
struct GuestSmsChannelTests {
    private static let identifier = "00112233445566778899aabbccddeeff"

    // MARK: Frames (U1)

    @Test func theFramesAreTheGoldenOnes() throws {
        let requests = try SmsFixture.vectors(prefix: "frame-request-")
        let answers = try SmsFixture.vectors(prefix: "frame-answer-")
        #expect(requests.count == 2 && answers.count == 3)
        for vector in requests {
            let payload = SmsFixture.bytes(ofHex: vector.fields.first ?? "-")
            #expect(GuestSmsChannel.frame(request: payload) == vector.bytes, Comment(rawValue: vector.name))
        }
        for vector in answers {
            let payload = SmsFixture.bytes(ofHex: vector.fields.first ?? "-")
            #expect(GuestSmsChannel.frame(answer: payload) == vector.bytes, Comment(rawValue: vector.name))
            // The header alone announces the payload's length.
            #expect(GuestSmsChannel.answerLength(header: Array(vector.bytes.prefix(8))) == payload.count, Comment(rawValue: vector.name))
        }
        // The largest frames.
        #expect(try SmsFixture.vector("frame-request-longest").bytes.count == 2758)
        #expect(try SmsFixture.vector("frame-answer-longest").bytes.count == 66)
        // The zero-length answer carries a version 1 header.
        #expect(try SmsFixture.vector("frame-answer-zero").bytes == Data([0x4B, 0x53, 0x4D, 0x53, 1, 0, 0, 0]))
        #expect(GuestSmsChannel.answerLength(header: Array(try SmsFixture.vector("frame-answer-zero").bytes)) == 0)
    }

    @Test func aRequestHasOneToEightThousandOneHundredNinetyTwoBytes() throws {
        #expect(GuestSmsChannel.frame(request: Data()) == nil)
        #expect(GuestSmsChannel.frame(request: Data(count: 1))?.count == 9)
        #expect(GuestSmsChannel.frame(request: Data(count: 8192))?.count == 8200)
        #expect(GuestSmsChannel.frame(request: Data(count: 8193)) == nil)
        let header = Array(try SmsFixture.vector("header-request-8192").bytes)
        #expect(header == [0x4B, 0x53, 0x4D, 0x53, 1, 0, 0x00, 0x20])
        #expect(GuestSmsChannel.frame(answer: Data(count: 4096))?.count == 4104)
        #expect(GuestSmsChannel.frame(answer: Data(count: 4097)) == nil)
    }

    @Test func anAnswerHeaderIsOnlyWhatTheFrameAllows() throws {
        #expect(GuestSmsChannel.answerLength(header: Array(try SmsFixture.vector("header-answer-4096").bytes)) == 4096)
        for name in ["reject-magic", "reject-version", "reject-version-0", "reject-answer-length-4097"] {
            #expect(GuestSmsChannel.answerLength(header: Array(try SmsFixture.vector(name).bytes)) == nil, Comment(rawValue: name))
        }
        // A cut header, and one with a byte too many.
        #expect(GuestSmsChannel.answerLength(header: Array(try SmsFixture.vector("frame-answer-zero").bytes.prefix(7))) == nil)
        #expect(GuestSmsChannel.answerLength(header: Array(try SmsFixture.vector("frame-answer-zero").bytes) + [0]) == nil)
        // The request headers the service refuses are not answers either way: the length rule is the receiver's.
        #expect(GuestSmsChannel.port == 5560)
    }

    // MARK: Over socket pairs (U2)

    /// Connects each attempt to the next scripted step: a refusal, or one end of a socket pair whose other end
    /// a thread of the test plays.
    private final class PairConnector: GuestSmsConnector, @unchecked Sendable {
        enum Step {
            case refuse(POSIXErrorCode)
            case peer(@Sendable (Int32) -> Void)
        }

        private let lock = NSLock()
        private var steps: [Step]
        private(set) var opens = 0

        init(_ steps: [Step]) { self.steps = steps }

        func open(timeoutMilliseconds: UInt32) throws -> Int32 {
            let step: Step = lock.withLock {
                opens += 1
                return steps.isEmpty ? .refuse(.ECONNREFUSED) : steps.removeFirst()
            }
            switch step {
            case .refuse(let code): throw POSIXError(code)
            case .peer(let body):
                var fds: [Int32] = [0, 0]
                precondition(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0)
                let peer = fds[1]
                Thread.detachNewThread {
                    body(peer)
                    Darwin.close(peer)
                }
                return fds[0]
            }
        }
    }

    /// Reads `count` bytes from a blocking descriptor; nil at the end of the stream.
    private static func readAll(_ descriptor: Int32, _ count: Int) -> Data? {
        var data = Data(count: count)
        var done = 0
        while done < count {
            let got = data.withUnsafeMutableBytes { Darwin.read(descriptor, $0.baseAddress! + done, count - done) }
            if got <= 0 { return nil }
            done += got
        }
        return data
    }

    /// A peer that takes the whole frame (the header says how long it is) and then does `then`.
    private static func service(_ then: @escaping @Sendable (Int32, Data) -> Void) -> PairConnector.Step {
        .peer { descriptor in
            guard let header = readAll(descriptor, 8), let body = readAll(descriptor, Int(header[6]) | Int(header[7]) << 8) else { return }
            then(descriptor, header + body)
        }
    }

    private static func write(_ data: Data, to descriptor: Int32) {
        data.withUnsafeBytes { _ = Darwin.write(descriptor, $0.baseAddress, data.count) }
    }

    private func send(_ steps: [PairConnector.Step], payload: Data? = nil) async -> (GuestSmsChannel.Outcome, PairConnector) {
        let connector = PairConnector(steps)
        let channel = GuestSmsChannel(connector: connector)
        let request = GuestSmsRequest(sender: "Bank", text: "code", identifier: Self.identifier, attempt: .first)!
        return (await channel.send(payload ?? request.payload), connector)
    }

    @Test func aWholeAnswerIsAnswered() async throws {
        let answer = Data("2.handed-over.\(Self.identifier).1".utf8)
        let seen = Seen()
        let (outcome, connector) = await send([Self.service { descriptor, frame in
            seen.set(frame)
            Self.write(GuestSmsChannel.frame(answer: answer)!, to: descriptor)
        }])
        #expect(outcome == .answered(answer))
        #expect(connector.opens == 1)
        // The peer got the request's whole frame.
        let request = GuestSmsRequest(sender: "Bank", text: "code", identifier: Self.identifier, attempt: .first)!
        #expect(seen.value == GuestSmsChannel.frame(request: request.payload))
    }

    private final class Seen: @unchecked Sendable {
        private let lock = NSLock()
        private var data: Data?
        var value: Data? { lock.withLock { data } }
        func set(_ value: Data) { lock.withLock { data = value } }
    }

    @Test func aZeroLengthAnswerIsNotTaken() async {
        let (outcome, _) = await send([Self.service { descriptor, _ in
            Self.write(GuestSmsChannel.frame(answer: Data())!, to: descriptor)
        }])
        #expect(outcome == .notTaken)
    }

    /// The same bytes decide the outcome whether the request was a first attempt or a retry: the attempt is the
    /// payload's, and the app's mapping is another row.
    @Test func theOutcomeIsTheSameForAFirstAttemptAndARetry() async {
        for attempt in [GuestSms.Attempt.first, .retry] {
            let request = GuestSmsRequest(sender: "Bank", text: "code", identifier: Self.identifier, attempt: attempt)!
            let (zero, _) = await send([Self.service { descriptor, _ in
                Self.write(GuestSmsChannel.frame(answer: Data())!, to: descriptor)
            }], payload: request.payload)
            #expect(zero == .notTaken)
            let (ended, _) = await send([Self.service { _, _ in }], payload: request.payload)
            #expect(ended == .noAnswer)
        }
    }

    @Test func theEndOfTheStreamAfterTheFrameIsNoAnswer() async {
        // The service read the whole frame and ended: it was dispatched, or its handler threw.
        let (outcome, _) = await send([Self.service { _, _ in }])
        #expect(outcome == .noAnswer)
    }

    @Test func aPeerThatClosesPartWayThroughTheFrameIsNoAnswer() async {
        let (outcome, connector) = await send([.peer { descriptor in _ = Self.readAll(descriptor, 12) }])
        #expect(outcome == .noAnswer)
        #expect(connector.opens == 1)
    }

    @Test func anAnswerThatIsCutOrBadIsNoAnswer() async throws {
        let whole = GuestSmsChannel.frame(answer: Data("2.handed-over.\(Self.identifier).1".utf8))!
        let cut = Data(whole.prefix(20))
        let (cutOutcome, _) = await send([Self.service { descriptor, _ in Self.write(cut, to: descriptor) }])
        #expect(cutOutcome == .noAnswer)
        // A bad header: the magic, the frame version and an answer longer than 4096 bytes.
        for name in ["reject-magic", "reject-version", "reject-answer-length-4097"] {
            let header = try SmsFixture.vector(name).bytes
            let (outcome, _) = await send([Self.service { descriptor, _ in Self.write(header, to: descriptor) }])
            #expect(outcome == .noAnswer, Comment(rawValue: name))
        }
        let (halfHeader, _) = await send([Self.service { descriptor, _ in Self.write(Data(whole.prefix(5)), to: descriptor) }])
        #expect(halfHeader == .noAnswer)
    }

    @Test func aConnectionRefusedAtFirstIsAskedAgainAndThenAnswered() async {
        let answer = Data("2.uncertain.\(Self.identifier).unknown".utf8)
        let (outcome, connector) = await send([
            .refuse(.ECONNREFUSED), .refuse(.ECONNRESET), .refuse(.ETIMEDOUT),
            Self.service { descriptor, _ in Self.write(GuestSmsChannel.frame(answer: answer)!, to: descriptor) },
        ])
        #expect(outcome == .answered(answer))
        #expect(connector.opens == 4)
    }

    @Test func noGuestOrAnotherErrorEndsTheSendAtOnceAsNotSent() async {
        for code in [POSIXErrorCode.ENODEV, .EIO, .EPERM, .ENETUNREACH] {
            let (outcome, connector) = await send([.refuse(code)])
            #expect(outcome == .notSent, "\(code)")
            #expect(connector.opens == 1, "\(code)")
        }
    }

    @Test func aPayloadTheFrameCannotCarryIsNotSent() async {
        let (empty, connector) = await send([.refuse(.ECONNREFUSED)], payload: Data())
        #expect(empty == .notSent && connector.opens == 0)
        let (huge, _) = await send([.refuse(.ECONNREFUSED)], payload: Data(count: 8193))
        #expect(huge == .notSent)
    }

    /// A task that was cancelled before it began never reaches the guest.
    @Test func aTaskCancelledBeforeItBeginsSendsNothing() async {
        let connector = PairConnector([Self.service { _, _ in }])
        let channel = GuestSmsChannel(connector: connector)
        let payload = GuestSmsRequest(sender: "Bank", text: "code", identifier: Self.identifier, attempt: .first)!.payload
        let task = Task { () -> GuestSmsChannel.Outcome in
            withUnsafeCurrentTask { $0?.cancel() }
            return await channel.send(payload)
        }
        #expect(await task.value == .notSent)
        #expect(connector.opens == 0)
    }

    // MARK: The deadline over a scripted system (U7)

    /// A system with a clock of its own and descriptors that are scripts: a wait that finds nothing ready advances
    /// the clock by its timeout (`blocking` false), or waits for a shutdown (`blocking` true).
    final class ScriptedSystem: GuestSmsSystem, @unchecked Sendable {
        struct Peer {
            /// Bytes the peer takes in all; the rest of a write is refused until it is raised.
            var acceptLimit = Int.max
            var written = Data()
            /// Bytes the host can read, and whether the stream ends after them.
            var incoming = Data()
            var eof = false
            var shutdown = false
        }

        private let condition = NSCondition()
        private var time: UInt64 = 0
        private var peers: [Int32: Peer] = [:]
        private var sleeps: [UInt64] = []
        private var shutdownLog: [Int32] = []
        private var closeLog: [(descriptor: Int32, thread: Thread)] = []
        /// Wait for a shutdown, in real time, instead of advancing the clock.
        var blocking = false
        private var waiters = 0

        var now: UInt64 { condition.withLock { time } }
        var shutdowns: [Int32] { condition.withLock { shutdownLog } }
        var closes: [(descriptor: Int32, thread: Thread)] { condition.withLock { closeLog } }
        var sleepsSeen: [UInt64] { condition.withLock { sleeps } }
        func peer(_ descriptor: Int32) -> Peer { condition.withLock { peers[descriptor] ?? Peer() } }

        func setPeer(_ descriptor: Int32, _ peer: Peer) { condition.withLock { peers[descriptor] = peer } }
        func change(_ descriptor: Int32, _ body: (inout Peer) -> Void) {
            condition.withLock {
                var peer = peers[descriptor] ?? Peer()
                body(&peer)
                peers[descriptor] = peer
                condition.broadcast()
            }
        }
        func advance(_ nanoseconds: UInt64) { condition.withLock { time += nanoseconds } }

        /// Blocks until a worker waits in `wait` (blocking mode).
        func untilWaiting() -> Bool {
            condition.withLock {
                let deadline = Date(timeIntervalSinceNow: 5)
                while waiters == 0 { if !condition.wait(until: deadline) { return false } }
                return true
            }
        }

        func nowNanoseconds() -> UInt64 { now }

        func sleep(nanoseconds: UInt64) {
            condition.withLock {
                time += nanoseconds
                sleeps.append(nanoseconds)
            }
        }

        func wait(_ descriptor: Int32, forWriting: Bool, timeoutMilliseconds: Int32) -> Bool {
            condition.withLock {
                func ready() -> Bool {
                    guard let peer = peers[descriptor] else { return true }
                    if peer.shutdown { return true }
                    return forWriting ? peer.written.count < peer.acceptLimit : !peer.incoming.isEmpty || peer.eof
                }
                if ready() { return true }
                if !blocking {
                    time += UInt64(timeoutMilliseconds) * 1_000_000
                    return false
                }
                waiters += 1
                condition.broadcast()
                defer { waiters -= 1 }
                _ = condition.wait(until: Date(timeIntervalSinceNow: Double(timeoutMilliseconds) / 1000))
                return ready()
            }
        }

        func read(_ descriptor: Int32, into buffer: UnsafeMutableRawBufferPointer) -> Int {
            condition.withLock {
                var peer = peers[descriptor] ?? Peer()
                if !peer.incoming.isEmpty {
                    let count = min(buffer.count, peer.incoming.count)
                    peer.incoming.prefix(count).withUnsafeBytes { buffer.copyMemory(from: $0) }
                    peer.incoming.removeFirst(count)
                    peers[descriptor] = peer
                    return count
                }
                return peer.eof || peer.shutdown ? 0 : -Int(EAGAIN)
            }
        }

        func write(_ descriptor: Int32, from buffer: UnsafeRawBufferPointer) -> Int {
            condition.withLock {
                var peer = peers[descriptor] ?? Peer()
                if peer.shutdown { return -Int(EPIPE) }
                let room = peer.acceptLimit == .max ? buffer.count : min(buffer.count, peer.acceptLimit - peer.written.count)
                guard room > 0 else { return -Int(EAGAIN) }
                peer.written.append(contentsOf: buffer.prefix(room))
                peers[descriptor] = peer
                return room
            }
        }

        func prepare(_ descriptor: Int32) {}

        func shutdown(_ descriptor: Int32) {
            condition.withLock {
                peers[descriptor, default: Peer()].shutdown = true
                shutdownLog.append(descriptor)
                condition.broadcast()
            }
        }

        func close(_ descriptor: Int32) {
            condition.withLock { closeLog.append((descriptor, Thread.current)) }
        }
    }

    /// Opens what the test says: each call takes the next closure, which may advance the clock first.
    final class ScriptedConnector: GuestSmsConnector, @unchecked Sendable {
        typealias Step = @Sendable (UInt32) throws -> Int32
        private let lock = NSLock()
        private var steps: [Step]
        private var timeoutLog: [UInt32] = []
        init(_ steps: [Step]) { self.steps = steps }
        var timeouts: [UInt32] { lock.withLock { timeoutLog } }
        func open(timeoutMilliseconds: UInt32) throws -> Int32 {
            var next: Step?
            lock.lock()
            timeoutLog.append(timeoutMilliseconds)
            if !steps.isEmpty { next = steps.removeFirst() }
            lock.unlock()
            guard let step = next else { throw POSIXError(.ECONNREFUSED) }
            return try step(timeoutMilliseconds)
        }
    }

    private let payload = GuestSmsRequest(sender: "Bank", text: "code", identifier: identifier, attempt: .first)!.payload

    @Test func theConnectStageEndsAtFiveSeconds() {
        let system = ScriptedSystem()
        let connector = ScriptedConnector([])
        system.advance(1_000_000_000_000)
        let start = system.now
        let outcome = GuestSmsExchange(connector: connector, system: system).run(payload)
        #expect(outcome == .notSent)
        let elapsed = system.now - start
        #expect(elapsed >= 5_000_000_000 && elapsed <= 5_250_000_000, "\(elapsed)")
        // Each attempt is at most 250 ms, and none outlasts the stage.
        #expect(connector.timeouts.allSatisfy { $0 <= 250 && $0 >= 1 })
        #expect(connector.timeouts.count >= 10)
    }

    @Test func theWriteStageEndsTwoSecondsAfterItStarts() {
        let system = ScriptedSystem()
        // The connect takes 3 s; the peer takes nothing.
        let connector = ScriptedConnector([{ _ in system.advance(3_000_000_000); system.setPeer(7, .init(acceptLimit: 0)); return 7 }])
        let start = system.now
        let outcome = GuestSmsExchange(connector: connector, system: system).run(payload)
        #expect(outcome == .notSent)
        let elapsed = system.now - start - 3_000_000_000
        #expect(elapsed >= 2_000_000_000 && elapsed <= 3_100_000_000, "\(elapsed)")
        // A peer that took part of the frame and then stopped: the same end, and no answer is promised.
        let system2 = ScriptedSystem()
        let connector2 = ScriptedConnector([{ _ in system2.setPeer(7, .init(acceptLimit: 5)); return 7 }])
        #expect(GuestSmsExchange(connector: connector2, system: system2).run(payload) == .noAnswer)
        #expect(system2.peer(7).written.count == 5)
        #expect(system2.now >= 2_000_000_000 && system2.now <= 3_100_000_000)
    }

    @Test func theWholeSendEndsThirtySecondsAfterItsEntryWhateverCameBefore() {
        let system = ScriptedSystem()
        // The call is made long after the clock started, as a send is after its notice write.
        system.advance(77_000_000_000)
        let entry = system.now
        // A connect that takes 4.9 s, a write at once, and an answer that never comes.
        let connector = ScriptedConnector([{ _ in system.advance(4_900_000_000); system.setPeer(9, .init()); return 9 }])
        let outcome = GuestSmsExchange(connector: connector, system: system).run(payload)
        #expect(outcome == .noAnswer)
        let elapsed = system.now - entry
        #expect(elapsed >= 30_000_000_000 && elapsed <= 31_000_000_000, "\(elapsed)")
        // The frame went on that one connection, whole.
        #expect(system.peer(9).written == GuestSmsChannel.frame(request: payload))
        #expect(connector.timeouts.count == 1)
    }

    @Test func aFrameIsWrittenOnAtMostOneConnection() {
        let system = ScriptedSystem()
        // The first connection ends after the frame; nothing connects again.
        let connector = ScriptedConnector([{ _ in system.setPeer(4, .init(eof: true)); return 4 }, { _ in system.setPeer(5, .init(eof: true)); return 5 }])
        #expect(GuestSmsExchange(connector: connector, system: system).run(payload) == .noAnswer)
        #expect(connector.timeouts.count == 1)
        #expect(system.peer(5).written.isEmpty)
        #expect(system.closes.map(\.descriptor) == [4])
    }

    @Test func aWriteErrorBeforeTheFirstByteIsNotSent() {
        let system = ScriptedSystem()
        let connector = ScriptedConnector([{ _ in system.change(6) { $0.shutdown = true }; return 6 }])
        #expect(GuestSmsExchange(connector: connector, system: system).run(payload) == .notSent)
    }

    // MARK: Cancellation (U8)

    private func exchange(
        _ system: ScriptedSystem, _ connector: ScriptedConnector
    ) -> (GuestSmsExchange, () -> GuestSmsChannel.Outcome) {
        let exchange = GuestSmsExchange(connector: connector, system: system)
        let payload = payload
        let box = ResultBox()
        let finished = DispatchSemaphore(value: 0)
        Thread.detachNewThread {
            box.set(exchange.run(payload))
            finished.signal()
        }
        return (exchange, {
            _ = finished.wait(timeout: .now() + 10)
            return box.get() ?? .answered(Data("never finished".utf8))
        })
    }

    private final class ResultBox: @unchecked Sendable {
        private let lock = NSLock()
        private var value: GuestSmsChannel.Outcome?
        func set(_ outcome: GuestSmsChannel.Outcome) { lock.withLock { value = outcome } }
        func get() -> GuestSmsChannel.Outcome? { lock.withLock { value } }
    }

    @Test func aCancelDuringTheConnectStageSendsNothingAndClosesWhatItDoesNotOwn() {
        let system = ScriptedSystem()
        system.blocking = true
        let entered = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        // An attempt that cannot be interrupted: it returns a connection after the cancel.
        let connector = ScriptedConnector([{ _ in
            entered.signal()
            _ = release.wait(timeout: .now() + 10)
            system.setPeer(11, .init())
            return 11
        }])
        let (exchange, result) = exchange(system, connector)
        #expect(entered.wait(timeout: .now() + 5) == .success)
        exchange.cancel()
        release.signal()
        #expect(result() == .notSent)
        // The connection that completed after the cancel is closed unused, by the worker, and was never shut down.
        #expect(system.closes.map(\.descriptor) == [11])
        #expect(system.shutdowns.isEmpty)
        #expect(system.peer(11).written.isEmpty)
    }

    @Test func aCancelDuringTheWriteStageIsNotSentBeforeTheFirstByteAndNoAnswerAfter() {
        for (accepted, expected) in [(0, GuestSmsChannel.Outcome.notSent), (5, .noAnswer)] {
            let system = ScriptedSystem()
            system.blocking = true
            let connector = ScriptedConnector([{ _ in system.setPeer(12, .init(acceptLimit: accepted)); return 12 }])
            let (exchange, result) = exchange(system, connector)
            #expect(system.untilWaiting())
            exchange.cancel()
            #expect(result() == expected, "\(accepted)")
            #expect(system.shutdowns == [12])
            #expect(system.closes.map(\.descriptor) == [12])
        }
    }

    @Test func aCancelDuringTheAnswerWaitIsNoAnswerAtOnce() {
        let system = ScriptedSystem()
        system.blocking = true
        let connector = ScriptedConnector([{ _ in system.setPeer(13, .init()); return 13 }])
        let (exchange, result) = exchange(system, connector)
        #expect(system.untilWaiting())
        exchange.cancel()
        #expect(result() == .noAnswer)
        #expect(system.shutdowns == [13])
        // Only the worker closes.
        let closes = system.closes
        #expect(closes.map(\.descriptor) == [13])
        #expect(closes.allSatisfy { !$0.thread.isMainThread && $0.thread !== Thread.current })
    }

    @Test func aCancelAfterTheExchangeEndedShutsNothingDown() {
        let system = ScriptedSystem()
        let answer = GuestSmsChannel.frame(answer: Data("2.uncertain.\(Self.identifier).unknown".utf8))!
        let connector = ScriptedConnector([{ _ in system.setPeer(14, .init(incoming: answer)); return 14 }])
        let done = GuestSmsExchange(connector: connector, system: system)
        #expect(done.run(payload) == .answered(Data("2.uncertain.\(Self.identifier).unknown".utf8)))
        done.cancel()
        #expect(system.shutdowns.isEmpty)
    }

    /// A descriptor number the test reopens after the exchange closed it is not shut down by a late cancel.
    @Test func aLateCancelNeverShutsDownANumberThatWasReused() {
        let system = ScriptedSystem()
        let answer = GuestSmsChannel.frame(answer: Data("2.uncertain.\(Self.identifier).unknown".utf8))!
        let first = GuestSmsExchange(connector: ScriptedConnector([{ _ in system.setPeer(15, .init(incoming: answer)); return 15 }]), system: system)
        #expect(first.run(payload) != .noAnswer)
        // Another exchange now holds number 15.
        system.blocking = true
        let (second, result) = exchange(system, ScriptedConnector([{ _ in system.setPeer(15, .init()); return 15 }]))
        #expect(system.untilWaiting())
        first.cancel()
        #expect(system.shutdowns.isEmpty)
        second.cancel()
        #expect(result() == .noAnswer)
        #expect(system.shutdowns == [15])
    }
}
