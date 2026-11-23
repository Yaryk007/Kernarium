// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The device's motion sensors as a session drives them. Each sensor is
/// started at an interval or stopped; `handler` is called on a queue of the
/// device's own with the reading in the device's own units, and `failure`
/// when the device can no longer be read.
public protocol MotionDevice: AnyObject, Sendable {
    var hasAccelerometer: Bool { get }
    var hasGyroscope: Bool { get }
    /// The device has a calibrated magnetic field to share (its own gate decides:
    /// the sensor, device motion in the magnetic-north frame, the person's
    /// permission where the system has one).
    var hasMagnetometer: Bool { get }
    /// Starts the accelerometer or the gyroscope, or restarts it at the new
    /// interval if it runs.
    func start(
        _ sensor: MotionSample.Sensor, interval: TimeInterval,
        handler: @escaping @Sendable (_ x: Double, _ y: Double, _ z: Double) -> Void,
        failure: @escaping @Sendable () -> Void
    )
    func stop(_ sensor: MotionSample.Sensor)
    /// Starts the magnetometer, or retimes it if it runs. A reading is the
    /// calibrated field in the device's own axes, in microtesla, with Android's
    /// status for how far it can be trusted. `failure` says the field can no
    /// longer be read; the session stops the magnetometer alone then.
    func startMagnetometer(
        interval: TimeInterval,
        handler: @escaping @Sendable (_ x: Double, _ y: Double, _ z: Double, _ status: UInt8) -> Void,
        failure: @escaping @Sendable () -> Void
    )
    func stopMagnetometer()
}

extension MotionDevice {
    public var hasMagnetometer: Bool { false }
    public func startMagnetometer(
        interval: TimeInterval,
        handler: @escaping @Sendable (Double, Double, Double, UInt8) -> Void,
        failure: @escaping @Sendable () -> Void
    ) {}
    public func stopMagnetometer() {}
}

/// Shares the device's sensors with the guest while the guest is in front,
/// wherever the device has them, and reads each sensor only while the guest
/// says an application uses it (`MotionSampling`). The axes are set when a run
/// begins and never change per sample; a new mounting is a new run
/// (`MotionMount` decides when). No samples are retained across a run.
///
/// Where the guest follows the device's orientation (an iPhone), the same
/// connection also gives a guest that has the device-orientation sensor the
/// orientation the device reports (`setOrientation`), on a device without
/// sensors too. It is sent when it changes and, once, on each connection and
/// each time the guest enables the sensor; nothing runs for it between changes.
///
/// A failure of the device's accelerometer or gyroscope stops them and leaves
/// the connection, which also carries the orientation, as it is. They are tried
/// again when the guest's demand changes or the next `update` comes, and not
/// before. The magnetometer differs: a failure of the field stops the
/// magnetometer alone, and it is not tried again before the next run, which
/// begins on a new foreground period, a changed mount or an explicit restart (a
/// reconnect continues the run), so a field that fails every time cannot make
/// a retry loop. The guest is told once, by a reading of status 0 (the last
/// field, or zeros), and the accelerometer, the gyroscope and the orientation
/// go on untouched.
///
/// The field is delivered under a subscription (`MagneticDelivery`): what the
/// guest's demand asked for, in one run on one connection. A stop, a demand
/// that no longer asks for the field, a connection change and the run's end
/// each end it, and a reading or a failure that was already on its way when it
/// ended is dropped where it would be cached or sent, not only where it began.
///
/// Every call returns at once; the work is in order on one queue, and the
/// device and the stream are only touched there.
public final class MotionSession: @unchecked Sendable {
    private let opener: any GuestStreamOpener
    private let device: any MotionDevice
    private let retryDelay: DispatchTimeInterval
    private let queue = DispatchQueue(label: "andarium-motion-session")

    // Touched only on `queue`.
    private var sampling = MotionSampling()
    private var axes = MotionSample.Axes.portrait
    private var stream: MotionStream?
    private var streamEpoch: UInt64 = 0
    private var run: UInt64 = 0
    private var applied = MotionPlan.off
    /// The device's accelerometer or gyroscope failed in this run: neither is
    /// read until the guest's demand changes or the next `update`.
    private var deviceFailed = false
    /// The magnetometer failed in this run: it is not read again in this run.
    private var magnetometerFailed = false
    /// The device has a magnetometer to share, read when a run is about to
    /// begin and kept for the run.
    private var offersMagnetometer = false
    /// This run's hand-off of the magnetic field, and the subscription now
    /// running on it.
    private var delivery = MagneticDelivery()
    private var subscription: UInt64 = 0

    public init(
        opener: any GuestStreamOpener, device: any MotionDevice,
        retryDelay: DispatchTimeInterval = .seconds(1)
    ) {
        self.opener = opener
        self.device = device
        self.retryDelay = retryDelay
    }

    deinit { stream?.stop() }

    /// What is wanted now. A run begins when the device has sensors, or the
    /// guest follows the device's orientation, and the guest is in front. It
    /// ends when either stops being true or the axes change.
    public func update(foreground: Bool, axes: MotionSample.Axes, followsOrientation: Bool = false) {
        queue.async { [self] in
            deviceFailed = false
            if stream == nil, foreground { offersMagnetometer = device.hasMagnetometer }
            sampling.hasSensors = device.hasAccelerometer || device.hasGyroscope || offersMagnetometer
            sampling.followsOrientation = followsOrientation
            if !followsOrientation { sampling.orientation = nil }
            sampling.foreground = foreground
            if axes != self.axes, stream != nil { endRun() }
            self.axes = axes
            if sampling.wantsConnection {
                if stream == nil { beginRun() }
                reconcile()
            } else if stream != nil {
                endRun()
            }
        }
    }

    /// The device's orientation now, or nil once it is not being reported (the
    /// next report is then the first of a new period). The guest is given it
    /// at once if it takes orientations and has not had this one.
    public func setOrientation(_ orientation: GuestOrientation?) {
        queue.async { [self] in
            sampling.orientation = orientation
            if stream != nil { reconcile() }
        }
    }

    /// Ends the run now: the pending samples belong to a period that is over.
    /// The next `update` begins a new one.
    public func restart() {
        queue.async { [self] in
            if stream != nil { endRun() }
        }
    }

    /// Returns when everything asked of the session so far has been done.
    public func settle() {
        queue.sync {}
    }

    /// What the guest's service last said on the connection; nil until it
    /// says anything (an older image never does).
    public var demand: MotionDemand? { queue.sync { sampling.demand } }

    /// How the device's sensors are being read now.
    public var plan: MotionPlan { queue.sync { applied } }

    // MARK: A run

    private func beginRun() {
        run &+= 1
        let current = run
        sampling.connectionChanged(false)
        applied = .off
        deviceFailed = false
        magnetometerFailed = false
        delivery = MagneticDelivery()
        let stream = MotionStream(
            opener: opener, retryDelay: retryDelay,
            connectionChanged: { [weak self] connected in
                self?.queue.async { [weak self] in
                    guard let self, run == current else { return }
                    delivery.invalidate()
                    sampling.connectionChanged(connected)
                    reconcile()
                }
            },
            demandChanged: { [weak self] demand in
                self?.queue.async { [weak self] in
                    guard let self, run == current else { return }
                    deviceFailed = false
                    sampling.received(demand)
                    reconcile()
                }
            }
        )
        self.stream = stream
        streamEpoch = stream.start()
    }

    private func endRun() {
        run &+= 1
        for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] where applied.interval(of: sensor) != nil {
            device.stop(sensor)
        }
        if applied.magnetometer != nil { device.stopMagnetometer() }
        delivery.invalidate()
        applied = .off
        stream?.stop()
        stream = nil
        sampling.connectionChanged(false)
    }

    /// Starts, stops and retimes the device's sensors to match the plan.
    private func reconcile() {
        guard let stream else { return }
        let plan = sampling.plan
        let current = run
        let epoch = streamEpoch
        let axes = axes
        for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] {
            let wanted = hasSensor(sensor) && !deviceFailed ? plan.interval(of: sensor) : nil
            guard wanted != applied.interval(of: sensor) else { continue }
            if let wanted {
                device.start(sensor, interval: wanted, handler: { x, y, z in
                    stream.submit(MotionSample(sensor: sensor, x: x, y: y, z: z, axes: axes), epoch: epoch)
                }, failure: { [weak self] in
                    self?.queue.async { [weak self] in
                        guard let self, run == current else { return }
                        deviceFailed = true
                        reconcile()
                    }
                })
            } else {
                device.stop(sensor)
            }
            switch sensor {
            case .accelerometer: applied.accelerometer = wanted
            case .gyroscope: applied.gyroscope = wanted
            case .magnetometer: break  // its own path: `reconcileMagnetometer`
            }
        }
        reconcileMagnetometer(plan: plan, stream: stream, epoch: epoch, current: current, axes: axes)
        if let orientation = sampling.orientationToSend() {
            stream.submit(orientation, epoch: epoch)
        }
    }

    /// The magnetometer follows the plan like the other two, except that a
    /// failure of it latches it off for the run (`magnetometerFailed`).
    private func reconcileMagnetometer(
        plan: MotionPlan, stream: MotionStream, epoch: UInt64, current: UInt64, axes: MotionSample.Axes
    ) {
        let wanted = offersMagnetometer && !magnetometerFailed ? plan.magnetometer : nil
        guard wanted != applied.magnetometer else { return }
        if let wanted {
            // A retime goes on under the subscription it began; a start is a new one.
            if applied.magnetometer == nil { subscription = delivery.renew() }
            let delivery = delivery
            let subscription = subscription
            device.startMagnetometer(interval: wanted, handler: { x, y, z, status in
                delivery.deliver(
                    MotionSample(sensor: .magnetometer, x: x, y: y, z: z, axes: axes, status: status),
                    subscription: subscription, to: stream, epoch: epoch)
            }, failure: { [weak self] in
                self?.queue.async { [weak self] in
                    guard let self, run == current, !magnetometerFailed else { return }
                    // One reading of status 0 says the field is gone: the last one
                    // the guest had, or zeros when it had none.
                    guard delivery.finish(subscription: subscription, to: stream, epoch: epoch) else { return }
                    magnetometerFailed = true
                    reconcile()
                }
            })
        } else {
            device.stopMagnetometer()
            delivery.invalidate()
        }
        applied.magnetometer = wanted
    }

    private func hasSensor(_ sensor: MotionSample.Sensor) -> Bool {
        switch sensor {
        case .accelerometer: device.hasAccelerometer
        case .gyroscope: device.hasGyroscope
        case .magnetometer: offersMagnetometer
        }
    }
}

/// One run's hand-off of the magnetic field to the guest: the one place a
/// reading is cached or sent, and the one place the notice that the field is
/// gone is sent. Everything that reaches it carries the subscription it
/// belongs to and is checked against the current one under `lock`, which
/// every end of a subscription (`renew`, `invalidate`, `finish`) also takes,
/// so a reading cannot be cached or sent after the subscription that asked
/// for it has ended, and a run's last field cannot reach the next run's (each
/// has its own delivery). The device calls it from its own queue and the
/// session from its own; neither waits on the other, and the lock is held
/// across the stream's non-blocking send alone.
private final class MagneticDelivery: @unchecked Sendable {
    private let lock = NSLock()
    private var current: UInt64 = 0
    private var last: MotionSample?

    /// Ends the subscription that runs and begins another.
    func renew() -> UInt64 {
        lock.withLock {
            current &+= 1
            return current
        }
    }

    /// Ends the subscription that runs.
    func invalidate() { _ = renew() }

    /// Sends a reading of this subscription, if the guest still wants it, and
    /// keeps it as the last one the guest had. A field that cannot be sent
    /// changes nothing.
    func deliver(_ sample: MotionSample, subscription: UInt64, to stream: MotionStream, epoch: UInt64) {
        guard sample.isUsable else { return }
        lock.withLock {
            guard current == subscription else { return }
            if stream.submit(sample, epoch: epoch) { last = sample }
        }
    }

    /// Ends this subscription with the one reading that says the field is
    /// gone: the last one the guest had, with status 0, or zeros. Answers
    /// whether the subscription was still the current one.
    func finish(subscription: UInt64, to stream: MotionStream, epoch: UInt64) -> Bool {
        lock.withLock {
            guard current == subscription else { return false }
            current &+= 1
            let gone = last ?? MotionSample(sensor: .magnetometer, x: 0, y: 0, z: 0)
            stream.submit(gone.withStatus(0), epoch: epoch)
            return true
        }
    }
}
