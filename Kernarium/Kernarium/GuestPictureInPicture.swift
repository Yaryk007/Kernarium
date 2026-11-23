// SPDX-License-Identifier: GPL-2.0-or-later

import AVFoundation
import AVKit
import CoreMedia
import CoreVideo
import SwiftUI
import UIKit

/// Hands the newest frame from the runtime's thread to the main thread and
/// drops the ones it replaced, so a busy main thread never queues frames or
/// holds the runtime's buffer pool.
final class FrameRelay: @unchecked Sendable {
    private let lock = NSLock()
    private var latest: CVPixelBuffer?
    private var scheduled = false
    private let hop: (@escaping @Sendable () -> Void) -> Void
    /// Set on the main thread before the relay is given to the runtime.
    nonisolated(unsafe) var handler: (@MainActor (CVPixelBuffer) -> Void)?

    /// The window let go of the relay. A delivery the runtime made before it
    /// did may still be waiting on the main queue, and that block holds the
    /// relay whatever the window does with it, so the relay itself forgets
    /// where frames go and what it kept: the block then finds nothing to show.
    /// Called on the main thread, which is where the block runs.
    func close() {
        lock.lock()
        latest = nil
        lock.unlock()
        handler = nil
    }

    init(
        hop: @escaping (@escaping @Sendable () -> Void) -> Void = {
            DispatchQueue.main.async(execute: $0)
        }
    ) {
        self.hop = hop
    }

    /// Called on the runtime's thread; the buffer is borrowed, so this keeps
    /// its own reference.
    func receive(_ buffer: CVPixelBuffer) {
        lock.lock()
        latest = buffer
        let needsHop = !scheduled
        scheduled = true
        lock.unlock()
        guard needsHop else { return }
        hop { [self] in
            lock.lock()
            let frame = latest
            latest = nil
            scheduled = false
            lock.unlock()
            guard let frame else { return }
            MainActor.assumeIsolated { handler?(frame) }
        }
    }
}

/// How frames reach the Picture in Picture layer.
@MainActor
private protocol PictureInPictureFeed: AnyObject {
    /// Called whenever the window is prepared, before it is created.
    func prepare()
    func enqueue(_ sample: CMSampleBuffer)
    /// Removes the displayed image as the window is disarmed.
    func flushRemovingDisplayedImage()
}

/// iOS 26 and earlier: the frames go to the layer's renderer, each marked to
/// show as it arrives, and a renderer that iOS failed is flushed before it
/// takes the next frame.
@MainActor
private final class RendererFeed: PictureInPictureFeed {
    private let layer: AVSampleBufferDisplayLayer

    init(layer: AVSampleBufferDisplayLayer) {
        self.layer = layer
    }

    func prepare() {}

    // The renderer's enqueue, flush and status are deprecated in iOS 27,
    // which feeds the layer through ReceiverFeed and never reaches this.
    func enqueue(_ sample: CMSampleBuffer) {
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(
            sample, createIfNecessary: true
        ), CFArrayGetCount(attachments) > 0 {
            let dictionary = unsafeBitCast(
                CFArrayGetValueAtIndex(attachments, 0),
                to: CFMutableDictionary.self
            )
            CFDictionarySetValue(
                dictionary,
                Unmanaged.passUnretained(
                    kCMSampleAttachmentKey_DisplayImmediately
                ).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque()
            )
        }
        let renderer = layer.sampleBufferRenderer
        // iOS interrupts the layer as the app changes state (-11847); it
        // takes a flush to accept frames again.
        if renderer.status == .failed {
            renderer.flush()
        }
        renderer.enqueue(sample)
    }

    func flushRemovingDisplayedImage() {
        layer.sampleBufferRenderer.flush(removingDisplayedImage: true)
    }
}

/// iOS 27: the frames go through the receiver of a render synchronizer that
/// runs on the host clock, so a frame stamped with the time it is made is due
/// as it arrives, and each enqueue reports its outcome.
@available(iOS 27.0, *)
@MainActor
private final class ReceiverFeed: PictureInPictureFeed {
    private let synchronizer = AVSampleBufferRenderSynchronizer()
    private let receiver: AVSampleBufferVideoRenderer.Receiver

    init(layer: AVSampleBufferDisplayLayer) {
        receiver = synchronizer.sampleBufferReceiver(
            adding: layer.sampleBufferRenderer
        )
    }

    func prepare() {
        // The timeline is the host clock: no wait for media before it runs.
        let now = CMClockGetTime(CMClockGetHostTimeClock())
        synchronizer.delaysRateChangeUntilHasSufficientMediaData = false
        synchronizer.setRate(1, time: now, atHostTime: now)
    }

    func enqueue(_ sample: CMSampleBuffer) {
        let ready = CMReadySampleBuffer<CMSampleBuffer.DynamicContent>(
            unsafeBuffer: sample
        )
        switch receiver.enqueueImmediately(ready) {
        case let .cancelledDueToFlushRequiredToResume(error),
             let .cancelledDueToError(error):
            // iOS interrupts the layer as the app changes state (-11847); it
            // takes a flush to accept frames again.
            print("Kernarium: picture in picture layer flushed: \(error)")
            receiver.flush()
            _ = receiver.enqueueImmediately(ready)
        default:
            break
        }
    }

    func flushRemovingDisplayedImage() {
        let receiver = receiver
        Task { await receiver.flush(removingDisplayedImage: true) }
    }
}

private let runtimeFrameSink: kernarium_runtime_frame_sink = { context, buffer in
    guard let context, let buffer else { return }
    let relay = Unmanaged<FrameRelay>.fromOpaque(context).takeUnretainedValue()
    relay.receive(Unmanaged<CVPixelBuffer>.fromOpaque(buffer).takeUnretainedValue())
}

/// The runtime's frame sink while the window is armed, and the relay it
/// delivers through. `register` is how the sink reaches the runtime; a test
/// replaces it to deliver frames as the runtime does.
@MainActor
final class FrameMirror {
    typealias Register = (
        kernarium_runtime_frame_sink?, UnsafeMutableRawPointer?, UInt32, UInt32
    ) -> Int32

    private let register: Register
    private let deliver: @MainActor (CVPixelBuffer) -> Void
    private var relay: FrameRelay?
    private(set) var armedRate: UInt32?

    init(
        deliver: @escaping @MainActor (CVPixelBuffer) -> Void,
        register: @escaping Register = { kernarium_runtime_loader_set_frame_sink($0, $1, $2, $3) }
    ) {
        self.deliver = deliver
        self.register = register
    }

    /// Whether the runtime is delivering to the mirror, or a relay is left to let go.
    var isHolding: Bool { armedRate != nil || relay != nil }

    /// Asks the runtime for frames of at most `width` pixels at `framesPerSecond`;
    /// the runtime's answer, 0 when it agreed.
    func arm(width: UInt32, framesPerSecond: UInt32) -> Int32 {
        if armedRate == framesPerSecond { return 0 }
        let owner: FrameRelay
        if let relay {
            owner = relay
        } else {
            owner = FrameRelay()
            owner.handler = deliver
            relay = owner
        }
        let result = register(
            runtimeFrameSink, Unmanaged.passUnretained(owner).toOpaque(), width, framesPerSecond
        )
        if result == 0 { armedRate = framesPerSecond }
        return result
    }

    func disarm() {
        if armedRate != nil {
            // Returns only once no delivery is running, so the relay can go.
            _ = register(nil, nil, 0, 0)
            armedRate = nil
        }
        // A delivery still waiting on the main queue holds the relay: it shows
        // nothing once the relay is closed.
        relay?.close()
        relay = nil
    }
}

/// The window that keeps the guest running in the background: iOS shows the
/// guest's display in a floating Picture in Picture window and keeps the
/// process scheduled for as long as the user leaves it open.
///
/// It owns a sample-buffer layer fed from the runtime's copy of the guest's
/// display, through the receiver of a render synchronizer that runs on the host
/// clock where iOS offers one (27), so a frame stamped with the time it is made
/// is due as it arrives. While the guest's background mode asks for the window,
/// a slow trickle of frames keeps the layer holding a current picture, which iOS
/// needs before it starts the window on its own as the app leaves; the rate
/// rises once the window is up.
@MainActor
final class GuestPictureInPicture: NSObject, PictureInPictureWindow {
    static let shared = GuestPictureInPicture()

    /// The mirror's size and rate while the app is in front, and in the window.
    /// The window is small, and the picture is for glancing at.
    private static let standby = (width: UInt32(480), framesPerSecond: UInt32(2))
    private static let showing = (width: UInt32(800), framesPerSecond: UInt32(15))

    weak var controller: BackgroundController?
    /// Whether the running guest chose the window; the policy that reads it
    /// is installed with the observers.
    var wanted: () -> Bool = { false }

    let layer = AVSampleBufferDisplayLayer()
    private var feed: (any PictureInPictureFeed)?
    private var pictureInPicture: AVPictureInPictureController?
    private lazy var mirror = FrameMirror { [weak self] buffer in self?.show(buffer) }
    private var guestRunning = false
    private var starting = false
    private var format: CMVideoFormatDescription?
    private var formatSize = CMVideoDimensions(width: 0, height: 0)
    private var resignObserver: NSObjectProtocol?
    private var framesShown = 0

    private override init() {
        super.init()
        layer.videoGravity = .resizeAspect
        layer.backgroundColor = UIColor.black.cgColor
    }

    static var isSupported: Bool {
        AVPictureInPictureController.isPictureInPictureSupported()
    }

    // MARK: PictureInPictureWindow

    var canStart: Bool {
        guard let pictureInPicture else { return false }
        return starting || pictureInPicture.isPictureInPicturePossible
    }

    var isActive: Bool { pictureInPicture?.isPictureInPictureActive ?? false }

    func stop() {
        pictureInPicture?.stopPictureInPicture()
    }

    // MARK: arming

    /// The guest started or ended. The mirror exists only for a run.
    func guestRunningChanged(_ running: Bool) {
        guestRunning = running
        refresh()
    }

    /// Re-evaluates whether the window is wanted: the guest's choice can change
    /// while it runs, and so can what the runtime is able to do.
    func refresh() {
        if guestRunning, wanted(), Self.isSupported {
            prepareController()
            arm(rate: isActive ? Self.showing : Self.standby)
        } else {
            disarm()
        }
    }

    private func prepareController() {
        guard pictureInPicture == nil else { return }
        configureAudioSession()
        if feed == nil {
            if #available(iOS 27.0, *) {
                feed = ReceiverFeed(layer: layer)
            } else {
                feed = RendererFeed(layer: layer)
            }
        }
        feed?.prepare()
        let source = AVPictureInPictureController.ContentSource(
            sampleBufferDisplayLayer: layer,
            playbackDelegate: self
        )
        let created = AVPictureInPictureController(contentSource: source)
        created.canStartPictureInPictureAutomaticallyFromInline = true
        created.requiresLinearPlayback = true
        created.delegate = self
        pictureInPicture = created
        resignObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.willResignActiveNotification,
            object: nil,
            queue: .main
        ) { _ in
            // Guest audio claims and drops the shared session on its own
            // terms; the window needs it to be ours as the app leaves.
            MainActor.assumeIsolated {
                GuestPictureInPicture.shared.configureAudioSession()
            }
        }
    }

    private func configureAudioSession() {
        let session = AVAudioSession.sharedInstance()
        do {
            if session.category == .playAndRecord {
                // The guest's microphone is being captured, which needs the
                // record category, and the movie mode is valid only with
                // playback. The window plays in either.
                try session.setCategory(
                    .playAndRecord,
                    mode: .default,
                    options: [
                        .mixWithOthers, .defaultToSpeaker, .allowBluetoothA2DP,
                    ]
                )
            } else {
                try session.setCategory(
                    .playback,
                    mode: .moviePlayback,
                    options: [.mixWithOthers]
                )
            }
            try session.setActive(true)
        } catch {
            print("Kernarium: picture in picture audio session failed: \(error)")
        }
    }

    private func arm(rate: (width: UInt32, framesPerSecond: UInt32)) {
        if mirror.armedRate == rate.framesPerSecond { return }
        let result = mirror.arm(width: rate.width, framesPerSecond: rate.framesPerSecond)
        if result == 0 {
            print(
                "Kernarium: picture in picture mirror \(rate.width) px, " +
                    "\(rate.framesPerSecond) fps"
            )
        } else {
            print("Kernarium: picture in picture frame sink refused (\(result))")
        }
    }

    private func disarm() {
        guard mirror.isHolding || pictureInPicture != nil else {
            return
        }
        mirror.disarm()
        feed?.flushRemovingDisplayedImage()
        format = nil
        if let resignObserver {
            NotificationCenter.default.removeObserver(resignObserver)
            self.resignObserver = nil
        }
        if isActive { stop() }
        pictureInPicture = nil
        starting = false
    }

    // MARK: frames

    private func show(_ pixelBuffer: CVPixelBuffer) {
        let size = CMVideoDimensions(
            width: Int32(CVPixelBufferGetWidth(pixelBuffer)),
            height: Int32(CVPixelBufferGetHeight(pixelBuffer))
        )
        if format == nil || size.width != formatSize.width ||
            size.height != formatSize.height {
            var created: CMVideoFormatDescription?
            guard CMVideoFormatDescriptionCreateForImageBuffer(
                allocator: kCFAllocatorDefault,
                imageBuffer: pixelBuffer,
                formatDescriptionOut: &created
            ) == noErr, let created else { return }
            format = created
            formatSize = size
        }
        guard let format else { return }
        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
            decodeTimeStamp: .invalid
        )
        var sample: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: pixelBuffer,
            formatDescription: format,
            sampleTiming: &timing,
            sampleBufferOut: &sample
        ) == noErr, let sample else { return }
        guard let feed else { return }
        feed.enqueue(sample)
        framesShown += 1
        if framesShown % 150 == 1 {
            print(
                "Kernarium: picture in picture frames \(framesShown) " +
                    "(\(size.width)x\(size.height), active \(isActive))"
            )
        }
    }
}

extension GuestPictureInPicture:
    AVPictureInPictureSampleBufferPlaybackDelegate {
    nonisolated func pictureInPictureController(
        _ pictureInPictureController: AVPictureInPictureController,
        setPlaying playing: Bool
    ) {
        // The window shows a live display: there is nothing to pause.
    }

    nonisolated func pictureInPictureControllerTimeRangeForPlayback(
        _ pictureInPictureController: AVPictureInPictureController
    ) -> CMTimeRange {
        CMTimeRange(start: .negativeInfinity, duration: .positiveInfinity)
    }

    nonisolated func pictureInPictureControllerIsPlaybackPaused(
        _ pictureInPictureController: AVPictureInPictureController
    ) -> Bool {
        false
    }

    nonisolated func pictureInPictureController(
        _ pictureInPictureController: AVPictureInPictureController,
        didTransitionToRenderSize newRenderSize: CMVideoDimensions
    ) {}

    nonisolated func pictureInPictureController(
        _ pictureInPictureController: AVPictureInPictureController,
        skipByInterval skipInterval: CMTime,
        completion completionHandler: @escaping @Sendable () -> Void
    ) {
        completionHandler()
    }
}

extension GuestPictureInPicture: AVPictureInPictureControllerDelegate {
    nonisolated func pictureInPictureControllerWillStartPictureInPicture(
        _ pictureInPictureController: AVPictureInPictureController
    ) {
        print("Kernarium: picture in picture will start")
        Task { @MainActor in
            GuestPictureInPicture.shared.starting = true
        }
    }

    nonisolated func pictureInPictureControllerDidStartPictureInPicture(
        _ pictureInPictureController: AVPictureInPictureController
    ) {
        print("Kernarium: picture in picture started")
        Task { @MainActor in
            let window = GuestPictureInPicture.shared
            window.starting = false
            window.arm(rate: Self.showing)
            window.controller?.pictureInPictureDidStart()
        }
    }

    nonisolated func pictureInPictureController(
        _ pictureInPictureController: AVPictureInPictureController,
        failedToStartPictureInPictureWithError error: any Error
    ) {
        print("Kernarium: picture in picture failed to start: \(error)")
        Task { @MainActor in
            let window = GuestPictureInPicture.shared
            window.starting = false
            window.controller?.pictureInPictureDidFailToStart()
        }
    }

    nonisolated func pictureInPictureControllerDidStopPictureInPicture(
        _ pictureInPictureController: AVPictureInPictureController
    ) {
        print("Kernarium: picture in picture stopped")
        Task { @MainActor in
            let window = GuestPictureInPicture.shared
            window.starting = false
            window.arm(rate: Self.standby)
            window.controller?.pictureInPictureDidStop()
        }
    }

    nonisolated func pictureInPictureController(
        _ pictureInPictureController: AVPictureInPictureController,
        restoreUserInterfaceForPictureInPictureStopWithCompletionHandler
            completionHandler: @escaping @Sendable (Bool) -> Void
    ) {
        // Tapping the window brings the app forward, which is all there is
        // to restore.
        completionHandler(true)
    }
}

/// Holds the window's layer in the view hierarchy, behind the guest's display:
/// iOS starts the window on its own only for a layer that is on screen.
struct GuestPictureInPictureAnchor: UIViewRepresentable {
    func makeUIView(context: Context) -> GuestPictureInPictureAnchorView {
        GuestPictureInPictureAnchorView(layer: GuestPictureInPicture.shared.layer)
    }

    func updateUIView(_ view: GuestPictureInPictureAnchorView, context: Context) {}
}

final class GuestPictureInPictureAnchorView: UIView {
    private let hosted: AVSampleBufferDisplayLayer

    init(layer: AVSampleBufferDisplayLayer) {
        hosted = layer
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        self.layer.addSublayer(layer)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        hosted.frame = bounds
    }
}
