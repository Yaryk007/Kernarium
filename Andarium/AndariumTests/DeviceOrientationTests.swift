// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Darwin
import Foundation
import Testing
import UIKit
@testable import Andarium

/// The device's orientation as the guest follows it on an iPhone: UIKit's
/// numbers against `DeviceOrientationReading`, the mapping to Android's
/// rotation, what the boot request states, and the observer that begins and
/// ends the device's notifications with the session, and the application's
/// path from a turn to the record the guest is sent. The real turns of a
/// device are the user's check: the Simulator cannot turn an iPhone's
/// `UIDevice` under a portrait-only interface, so the device is injected.
@MainActor
struct DeviceOrientationTests {
    // MARK: UIKit's numbers

    @Test func theReadingsAreUIKitsOwnNumbersAndMapToAndroidsRotations() {
        let expected: [(UIDeviceOrientation, GuestOrientation?)] = [
            (.unknown, nil),
            (.portrait, .upright),
            (.portraitUpsideDown, .upsideDown),
            // iOS names a landscape orientation by where the home indicator is, so
            // landscapeLeft is the device turned counter-clockwise: Android's 1.
            (.landscapeLeft, .turnedCounterClockwise),
            (.landscapeRight, .turnedClockwise),
            (.faceUp, nil),
            (.faceDown, nil),
        ]
        #expect(expected.count == DeviceOrientationReading.allCases.count)
        for (uiKit, guest) in expected {
            let reading = DeviceOrientationReading(rawValue: uiKit.rawValue)
            #expect(reading != nil, "\(uiKit.rawValue)")
            #expect(reading.flatMap { GuestOrientation($0) } == guest, "\(uiKit.rawValue)")
        }
        // The values the guest is sent are Android's Surface.ROTATION_* numbers.
        #expect(GuestOrientation.upright.rawValue == 0)
        #expect(GuestOrientation.turnedCounterClockwise.rawValue == 1)
        #expect(GuestOrientation.upsideDown.rawValue == 2)
        #expect(GuestOrientation.turnedClockwise.rawValue == 3)
    }

    // MARK: The boot request

    @Test func theBootRequestStatesTheOrientationOnlyWhereTheGuestFollowsIt() {
        let haptics = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS)
        let orientation = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION)
        #expect(orientation == 2)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, followsDeviceOrientation: true) == orientation)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true, followsDeviceOrientation: false) == haptics)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true, followsDeviceOrientation: true) == haptics | orientation)
    }

    // MARK: The observer

    /// A device that is turned by hand, with its notifications in a centre of its own.
    private final class Device {
        let center = NotificationCenter()
        var reading = DeviceOrientationReading.portrait
        var begun = 0
        var ended = 0
        var generating: Bool { begun > ended }

        var source: DeviceOrientationObserver.Source {
            DeviceOrientationObserver.Source(
                begin: { [self] in begun += 1 },
                end: { [self] in ended += 1 },
                reading: { [self] in reading },
                center: center,
                notification: UIDevice.orientationDidChangeNotification)
        }

        /// The device turns: the notification follows the new reading, as UIKit's does.
        func turn(to new: DeviceOrientationReading) {
            reading = new
            center.post(name: UIDevice.orientationDidChangeNotification, object: nil)
        }
    }

    private final class Seen {
        var values: [GuestOrientation?] = []
    }

    private func observer(_ device: Device, _ seen: Seen) -> DeviceOrientationObserver {
        DeviceOrientationObserver(source: device.source) { seen.values.append($0) }
    }

    @Test func theNotificationsRunOnlyWhileTheOrientationIsWanted() {
        let device = Device()
        let seen = Seen()
        let observer = observer(device, seen)
        // Nothing is generated, observed or reported until it is wanted.
        #expect(!device.generating && !observer.isObserving)
        device.turn(to: .landscapeLeft)
        #expect(seen.values.isEmpty)
        observer.observe(true)
        #expect(device.generating && observer.isObserving)
        // Asking again changes nothing.
        observer.observe(true)
        #expect(device.begun == 1)
        // What the device shows at once is the first report.
        #expect(seen.values == [.turnedCounterClockwise])
        observer.observe(false)
        #expect(!device.generating && !observer.isObserving)
        #expect(device.ended == 1)
        // It reports nil, so that nothing seen before is given again as current, and then
        // nothing at all.
        #expect(seen.values == [.turnedCounterClockwise, nil])
        device.turn(to: .portrait)
        #expect(seen.values.count == 2)
        observer.observe(false)
        #expect(device.ended == 1)
    }

    @Test func eachTurnIsReportedAndFlatOrUnknownIsNot() {
        let device = Device()
        let seen = Seen()
        let observer = observer(device, seen)
        observer.observe(true)
        #expect(seen.values == [.upright])
        device.turn(to: .landscapeRight)
        device.turn(to: .portraitUpsideDown)
        // Face up and face down are no orientation, and the first reading may be unknown.
        device.turn(to: .faceUp)
        device.turn(to: .faceDown)
        device.turn(to: .unknown)
        device.turn(to: .landscapeLeft)
        device.turn(to: .portrait)
        #expect(seen.values == [.upright, .turnedClockwise, .upsideDown, .turnedCounterClockwise, .upright])
        observer.observe(false)
    }

    @Test func aDeviceThatDoesNotKnowYetReportsWhenItDoes() {
        let device = Device()
        device.reading = .unknown
        let seen = Seen()
        let observer = observer(device, seen)
        observer.observe(true)
        #expect(seen.values.isEmpty)
        device.turn(to: .landscapeLeft)
        #expect(seen.values == [.turnedCounterClockwise])
        observer.observe(false)
    }

    // MARK: From a turn to the guest

    /// The guest's end of the motion connection, a socket pair for each connection.
    private final class Guest: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var ends: [Int32] = []
        func openStream() throws -> Int32 {
            var sockets: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
            lock.withLock { ends.append(sockets[1]) }
            return sockets[0]
        }
        var connections: Int { lock.withLock { ends.count } }
        private var current: Int32? { lock.withLock { ends.last } }
        deinit { for end in ends { close(end) } }

        /// A control frame of the service: "ANC2", the sensor idle, nothing else in use.
        func sayItTakesOrientations() {
            let frame: [UInt8] = [0x41, 0x4e, 0x43, 0x32, 0, 0, 0, 0, 0x10, 0x27, 0, 0, 0x10, 0x27, 0, 0]
            if let end = current { _ = write(end, frame, frame.count) }
        }

        /// The values of the records the host wrote, each of 20 bytes, within the time.
        func records(timeout: Duration = .seconds(2)) -> [UInt8] {
            guard let end = current else { return [] }
            var bytes: [UInt8] = []
            let deadline = ContinuousClock.now + timeout
            while bytes.count < 20, ContinuousClock.now < deadline {
                var descriptor = pollfd(fd: end, events: Int16(POLLIN), revents: 0)
                guard poll(&descriptor, 1, 20) > 0 else { continue }
                var chunk = [UInt8](repeating: 0, count: 20 - bytes.count)
                let size = recv(end, &chunk, chunk.count, MSG_DONTWAIT)
                guard size > 0 else { break }
                bytes.append(contentsOf: chunk.prefix(size))
            }
            return bytes
        }
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition(), ContinuousClock.now < deadline { try? await Task.sleep(for: .milliseconds(20)) }
        return condition()
    }

    /// A device with no accelerometer or gyroscope, as a Mac and the Simulator are: the guest is
    /// given the orientation without it (it does not depend on the sensors), and a device's own
    /// readings do not mix with the records read here.
    private final class NoSensors: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = false
        let hasGyroscope = false
        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) {}
        func stop(_ sensor: MotionSample.Sensor) {}
    }

    /// The application's path, from the device's notification to the bytes the guest reads: the
    /// observer the runtime owns, the motion sensors it feeds and the session behind them, on a
    /// device without sensors (the orientation does not depend on them).
    @Test func aTurnReachesTheGuestAsTheRecordOnADeviceWithoutSensors() async throws {
        let guest = Guest()
        let sensors = MotionSensors(opener: guest, device: NoSensors())
        let device = Device()
        let observer = DeviceOrientationObserver(source: device.source) { sensors.setOrientation($0) }
        sensors.update(foreground: true, axes: .portrait, followsOrientation: true)
        observer.observe(true)
        #expect(await eventually { guest.connections == 1 })
        // Nothing before the guest says it takes orientations.
        #expect(guest.records(timeout: .milliseconds(150)).isEmpty)
        guest.sayItTakesOrientations()
        func record(_ value: UInt8) -> [UInt8] {
            Array(OrientationRecord.encoded(GuestOrientation(rawValue: value)!)) }
        #expect(guest.records() == record(0))
        // Each turn is the record of its Android rotation: landscape left 1, upside down 2, landscape right 3.
        for (turn, value) in [(DeviceOrientationReading.landscapeLeft, 1), (.portraitUpsideDown, 2),
                              (.landscapeRight, 3), (.portrait, 0)] as [(DeviceOrientationReading, UInt8)] {
            device.turn(to: turn)
            #expect(guest.records() == record(value), "\(turn)")
        }
        // Lying flat gives nothing, and the guest keeps the last value.
        device.turn(to: .faceUp)
        #expect(guest.records(timeout: .milliseconds(150)).isEmpty)
        // The session leaves the foreground: the observation ends and so does the connection.
        observer.observe(false)
        sensors.update(foreground: false, axes: .portrait, followsOrientation: true)
        sensors.stop()
        #expect(!device.generating)
    }

    /// The Simulator has no accelerometer or gyroscope, and a device that has them is offered them
    /// to the guest without being asked: the session connects to the guest's service for the sensors
    /// alone, and a device that has none holds no connection for them (a connection for the
    /// orientation is the other test's). An iPad app on a Mac has what Core Motion reports there,
    /// which is a sensor on some Macs, so only the connection is held to the device's answer.
    @Test func theSensorsAreOfferedWhereTheDeviceHasThemAndNowhereElse() async throws {
        let guest = Guest()
        let sensors = MotionSensors(opener: guest)
        #if targetEnvironment(simulator)
        #expect(!sensors.available, "The Simulator has no motion sensors")
        #endif
        sensors.update(foreground: true, axes: .portrait)
        if sensors.available {
            #expect(await eventually { guest.connections == 1 })
        } else {
            try await Task.sleep(for: .milliseconds(300))
            #expect(guest.connections == 0)
        }
        sensors.stop()
    }

    @Test func beginAndEndStayBalancedAcrossManySessions() {
        let device = Device()
        let seen = Seen()
        let observer = observer(device, seen)
        for _ in 0..<5 {
            observer.observe(true)
            observer.observe(true)
            observer.observe(false)
        }
        observer.observe(false)
        #expect(device.begun == 5 && device.ended == 5)
        #expect(!device.generating)
    }
}
