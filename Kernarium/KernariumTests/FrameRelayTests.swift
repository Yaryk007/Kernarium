// SPDX-License-Identifier: GPL-2.0-or-later
import CoreVideo
import Foundation
import Testing
@testable import Kernarium

/// The relay between the runtime's frame thread and the main thread keeps only
/// the newest frame, so a busy main thread never queues frames.
@MainActor
struct FrameRelayTests {
    private func buffer(_ width: Int) throws -> CVPixelBuffer {
        var created: CVPixelBuffer?
        let status = CVPixelBufferCreate(
            kCFAllocatorDefault, width, 2, kCVPixelFormatType_32BGRA, nil,
            &created
        )
        #expect(status == kCVReturnSuccess)
        return try #require(created)
    }

    @Test
    func framesArrivingBeforeTheMainThreadRunsCollapseToTheNewest() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        var shown: [Int] = []
        relay.handler = { shown.append(CVPixelBufferGetWidth($0)) }

        relay.receive(try buffer(2))
        relay.receive(try buffer(4))
        relay.receive(try buffer(6))
        #expect(hops.count == 1)
        #expect(shown.isEmpty)

        hops.removeFirst()()
        #expect(shown == [6])
    }

    @Test
    func aFrameAfterTheHopRunsSchedulesAnother() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        var shown: [Int] = []
        relay.handler = { shown.append(CVPixelBufferGetWidth($0)) }

        relay.receive(try buffer(2))
        hops.removeFirst()()
        relay.receive(try buffer(4))
        #expect(hops.count == 1)
        hops.removeFirst()()
        #expect(shown == [2, 4])
    }

    @Test
    func aFrameWithNoHandlerIsDropped() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        relay.receive(try buffer(2))
        hops.removeFirst()()
        // Nothing to assert beyond not crashing; a later frame still arrives.
        var shown = 0
        relay.handler = { _ in shown += 1 }
        relay.receive(try buffer(2))
        hops.removeFirst()()
        #expect(shown == 1)
    }

    /// A delivery that is still waiting for the main thread when the window lets go of the relay
    /// (`GuestPictureInPicture.disarm` closes it) holds the relay it came through. It must show
    /// nothing once the relay is closed.
    @Test
    func aDeliveryPendingWhenTheRelayIsClosedShowsNothingAfterIt() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        var shown = 0
        relay.handler = { _ in shown += 1 }
        relay.receive(try buffer(4))
        #expect(hops.count == 1)
        relay.close()
        hops.removeFirst()()
        #expect(shown == 0)
        // A frame after it goes nowhere either, and nothing is left to show.
        relay.receive(try buffer(6))
        hops.removeFirst()()
        #expect(shown == 0)
    }

    /// What the runtime holds of the mirror: the sink and its context, as the loader was given them
    /// (`FrameMirror.register`), and how often the sink was let go.
    private final class RuntimeStandIn {
        var sink: kernarium_runtime_frame_sink?
        var context: UnsafeMutableRawPointer?
        var lettingGo = 0

        func register(
            _ sink: kernarium_runtime_frame_sink?, _ context: UnsafeMutableRawPointer?, _: UInt32, _: UInt32
        ) -> Int32 {
            if sink == nil { lettingGo += 1 }
            self.sink = sink
            self.context = context
            return 0
        }

        /// A frame as the runtime's thread delivers it.
        func deliver(_ frame: CVPixelBuffer) {
            if let sink, let context { sink(context, Unmanaged.passUnretained(frame).toOpaque()) }
        }
    }

    /// A delivery that is still waiting for the main thread when the window disarms holds the relay
    /// it came through. It must show nothing once the window has let go (`FrameMirror.disarm` closes
    /// the relay), and a frame the runtime delivers while the mirror is armed is shown.
    @Test
    func aDeliveryPendingWhenTheMirrorIsDisarmedShowsNothingAfterIt() async throws {
        let runtime = RuntimeStandIn()
        var shown: [Int] = []
        let mirror = FrameMirror(
            deliver: { shown.append(CVPixelBufferGetWidth($0)) },
            register: { runtime.register($0, $1, $2, $3) })
        #expect(mirror.arm(width: 64, framesPerSecond: 2) == 0)
        #expect(mirror.isHolding)

        // The hop to the main thread cannot run before this test yields.
        runtime.deliver(try buffer(4))
        try await Task.sleep(for: .milliseconds(200))
        #expect(shown == [4], "an armed mirror shows what the runtime delivers")

        runtime.deliver(try buffer(6))
        mirror.disarm()
        #expect(runtime.lettingGo == 1)
        #expect(!mirror.isHolding)
        try await Task.sleep(for: .milliseconds(200))
        #expect(shown == [4], "nothing is shown after the mirror was disarmed")
    }
}
