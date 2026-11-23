// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

/// The magnetic field as the host gives it to a guest that lists a
/// magnetometer: the guest's "ANC3" frame, the type-2 record with its status,
/// when the field is read, what a failure of it does, and that a guest that
/// never sends "ANC3" is never given a field. The two older frames are in
/// `MotionDemandTests` and `MotionOrientationTests`.
struct MotionCompassTests {
    private typealias Guest = MotionDemandTests.FakeGuest
    private typealias Device = MotionDemandTests.FakeDevice

    /// An independent "ANC3" fixture: little endian, magic, mask word, then the
    /// accelerometer's, the gyroscope's and the magnetometer's periods.
    private func anc3(
        mask: UInt32, accelerometer: UInt32 = 10_000, gyroscope: UInt32 = 10_000, magnetometer: UInt32 = 20_000
    ) -> Data {
        var bytes: [UInt8] = [0x41, 0x4e, 0x43, 0x33]
        for word in [mask, accelerometer, gyroscope, magnetometer] {
            bytes += (0..<4).map { UInt8((word >> UInt32(8 * $0)) & 0xff) }
        }
        return Data(bytes)
    }

    private func older(magic: UInt8, mask: UInt32) -> Data {
        var bytes: [UInt8] = [0x41, 0x4e, 0x43, magic]
        for word in [mask, 10_000, 10_000] as [UInt32] {
            bytes += (0..<4).map { UInt8((word >> UInt32(8 * $0)) & 0xff) }
        }
        return Data(bytes)
    }

    // MARK: The frame

    @Test func anAnc3FrameSaysWhichSensorsAreUsedAndHowOften() throws {
        #expect(MotionDemand(frame: anc3(mask: 0b00001, accelerometer: 20_000)) == MotionDemand(accelerometer: 20_000))
        #expect(MotionDemand(frame: anc3(mask: 0b00010, gyroscope: 66_667)) == MotionDemand(gyroscope: 66_667))
        #expect(MotionDemand(frame: anc3(mask: 0b01000, magnetometer: 50_000)) == MotionDemand(magnetometer: 50_000))
        // Bit 4 says the guest lists the orientation sensor, bit 2 that it is enabled.
        #expect(MotionDemand(frame: anc3(mask: 0b10000)) == MotionDemand(orientationSensor: .idle))
        #expect(MotionDemand(frame: anc3(mask: 0b10100)) == MotionDemand(orientationSensor: .active))
        let all = try #require(
            MotionDemand(frame: anc3(mask: 0b11111, accelerometer: 20_000, gyroscope: 1_000_000, magnetometer: 40_000)))
        #expect(
            all == MotionDemand(
                accelerometer: 20_000, gyroscope: 1_000_000, magnetometer: 40_000, orientationSensor: .active))
        let idle = try #require(MotionDemand(frame: anc3(mask: 0)))
        #expect(idle.isIdle && idle.orientationSensor == .absent && !idle.acceptsOrientation)
        // The period of a sensor that is not active is not read.
        #expect(
            MotionDemand(frame: anc3(mask: 0b01000, accelerometer: 1, gyroscope: 2, magnetometer: 25_000))
                == MotionDemand(magnetometer: 25_000))
    }

    @Test func eachSensorHasItsOwnLimitsOnThePeriod() throws {
        // The magnetometer is listed at 20 ms to 1 s; the others keep 10 ms to 1 s.
        let fast = try #require(MotionDemand(frame: anc3(mask: 0b01011, accelerometer: 0, gyroscope: 0, magnetometer: 0)))
        #expect(fast.accelerometer == 10_000 && fast.gyroscope == 10_000 && fast.magnetometer == 20_000)
        let between = try #require(MotionDemand(frame: anc3(mask: 0b01000, magnetometer: 19_999)))
        #expect(between.magnetometer == 20_000)
        let slow = try #require(
            MotionDemand(frame: anc3(mask: 0b01011, accelerometer: 5_000_000, gyroscope: 5_000_000, magnetometer: 5_000_000)))
        #expect(slow.accelerometer == 1_000_000 && slow.gyroscope == 1_000_000 && slow.magnetometer == 1_000_000)
        #expect(MotionDemand(magnetometer: 10_000).magnetometer == 20_000)
    }

    @Test func anAnc3FrameThatIsNotOneIsRefused() {
        let good = anc3(mask: 0b01001)
        #expect(MotionDemand(frame: good) != nil)
        // Bit 2 (the sensor enabled) without bit 4 (the sensor listed).
        #expect(MotionDemand(frame: anc3(mask: 0b00100)) == nil)
        #expect(MotionDemand(frame: anc3(mask: 0b01100)) == nil)
        // Every bit above 4.
        for bit in 5..<32 { #expect(MotionDemand(frame: anc3(mask: 1 << UInt32(bit))) == nil, "bit \(bit)") }
        // A frame of the wrong length: the magic says 20 bytes.
        #expect(MotionDemand(frame: good.prefix(16)) == nil)
        #expect(MotionDemand(frame: good.dropLast()) == nil)
        #expect(MotionDemand(frame: good + [0]) == nil)
        // A magic that is no frame's.
        var other = good
        other[3] = 0x34
        #expect(MotionDemand(frame: other) == nil)
        other[3] = 0x30
        #expect(MotionDemand(frame: other) == nil)
    }

    @Test func theOlderFramesStayAsTheyWereAndNeverCarryTheNewBits() {
        // "ANC1": 16 bytes, two sensors. "ANC2": three.
        #expect(MotionDemand(frame: older(magic: 0x31, mask: 0b11)) == MotionDemand(accelerometer: 10_000, gyroscope: 10_000))
        #expect(MotionDemand(frame: older(magic: 0x32, mask: 0b111))
            == MotionDemand(accelerometer: 10_000, gyroscope: 10_000, orientationSensor: .active))
        // Bit 3 is the magnetometer's, and bit 4 the listing of the orientation
        // sensor: neither belongs to an older frame.
        #expect(MotionDemand(frame: older(magic: 0x32, mask: 0b01000)) == nil)
        #expect(MotionDemand(frame: older(magic: 0x32, mask: 0b10000)) == nil)
        #expect(MotionDemand(frame: older(magic: 0x31, mask: 0b01000)) == nil)
        #expect(MotionDemand(frame: older(magic: 0x31, mask: 0b00100)) == nil)
        // A 20-byte frame with an older magic is no frame.
        #expect(MotionDemand(frame: anc3(mask: 0b11).withMagic(0x31)) == nil)
        #expect(MotionDemand(frame: anc3(mask: 0b11).withMagic(0x32)) == nil)
        // And an older service never says the magnetometer is in use.
        for mask: UInt32 in 0..<8 {
            #expect(MotionDemand(frame: older(magic: 0x32, mask: mask))?.magnetometer == nil)
        }
    }

    @Test func theMagicSaysHowLongAFrameIs() {
        #expect(MotionDemand.framing(of: Data()) == .incomplete)
        #expect(MotionDemand.framing(of: Data([0x41, 0x4e, 0x43])) == .incomplete)
        #expect(MotionDemand.framing(of: Data([0x41, 0x4e, 0x43, 0x31])) == .frame(length: 16))
        #expect(MotionDemand.framing(of: Data([0x41, 0x4e, 0x43, 0x32, 0, 0])) == .frame(length: 16))
        #expect(MotionDemand.framing(of: Data([0x41, 0x4e, 0x43, 0x33])) == .frame(length: 20))
        // The first 16 bytes of an "ANC3" frame are the start of one, not a bad frame.
        #expect(MotionDemand.framing(of: anc3(mask: 0).prefix(16)) == .frame(length: 20))
        // Magics that no frame has, as soon as four bytes show it.
        #expect(MotionDemand.framing(of: Data([0x41, 0x4e, 0x43, 0x34])) == .invalid)
        #expect(MotionDemand.framing(of: Data([0x41, 0x4e, 0x53, 0x31])) == .invalid)
        #expect(MotionDemand.framing(of: Data([0x55, 0x55, 0x55, 0x55])) == .invalid)
        // A slice of a larger buffer is read from its own start.
        let buffer = Data([0, 0, 0x41, 0x4e, 0x43, 0x33])
        #expect(MotionDemand.framing(of: buffer.dropFirst(2)) == .frame(length: 20))
    }

    // MARK: The record

    @Test func aMagneticFieldRecordCarriesItsStatusInTheByteAfterTheType() throws {
        for status in UInt8(0)...3 {
            let record = try #require(MotionSample(sensor: .magnetometer, x: 1, y: 2, z: 3, status: status).encoded)
            #expect(
                [UInt8](record) == [
                    0x41, 0x4e, 0x53, 0x31, 0x02, 0x00, status, 0x00,
                    0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40, 0x40,
                ], "status \(status)")
        }
        // Beyond high is high.
        for status: UInt8 in [4, 7, 255] {
            let record = try #require(MotionSample(sensor: .magnetometer, x: 1, y: 2, z: 3, status: status).encoded)
            #expect(record[6] == 3 && record[7] == 0)
        }
    }

    @Test func theOtherRecordsAreByteForByteWhatTheyWere() throws {
        // A status means nothing for them: byte 6 and 7 stay zero.
        let acceleration = try #require(MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1, status: 3).encoded)
        #expect([UInt8](acceleration.prefix(8)) == [0x41, 0x4e, 0x53, 0x31, 0x01, 0, 0, 0])
        #expect(acceleration.count == 20)
        let rotation = try #require(MotionSample(sensor: .gyroscope, x: 1, y: 0, z: 0, status: 3).encoded)
        #expect([UInt8](rotation.prefix(8)) == [0x41, 0x4e, 0x53, 0x31, 0x04, 0, 0, 0])
        #expect([UInt8](OrientationRecord.encoded(.upright).prefix(8)) == [0x41, 0x4e, 0x53, 0x31, 0x1b, 0, 0, 0])
        #expect(MotionSample(sensor: .accelerometer, x: 0, y: 0, z: 0, status: 3).status == 0)
        #expect(MotionSample(sensor: .gyroscope, x: 0, y: 0, z: 0, status: 3).status == 0)
        #expect(MotionSample(sensor: .magnetometer, x: 0, y: 0, z: 0, status: 3).status == 3)
    }

    @Test func theRecordsAreTheGoldenFramesTheGuestReads() throws {
        // The same bytes the guest's sub-HAL decodes (tests/unit/andarium reads the file too).
        for status in UInt8(0)...3 {
            let record = try #require(MotionSample(sensor: .magnetometer, x: 0, y: 25, z: -40, status: status).encoded)
            #expect([UInt8](record) == (try MotionOrientationFixture.bytes("magnetic-status-\(status)")), "status \(status)")
        }
        let rotation = try #require(MotionSample(sensor: .gyroscope, x: 1, y: -2, z: 0.5).encoded)
        #expect([UInt8](rotation) == (try MotionOrientationFixture.bytes("gyroscope-turn")))
        // Gravity along z reads 9.80665 m/s^2; the zeros of x and y come out negated, which is still zero.
        let gravity = try MotionOrientationFixture.bytes("accelerometer-gravity")
        let acceleration = try #require(MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1).encoded)
        #expect([UInt8](acceleration.prefix(8)) == Array(gravity.prefix(8)))
        #expect([UInt8](acceleration.suffix(4)) == Array(gravity.suffix(4)))
    }

    @Test func theFieldKeepsItsSignAndTurnsWithTheMountLikeTheRotationRate() {
        // A microtesla is a microtesla: no -9.80665, and the vector is not negated.
        let upright = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40)
        #expect(upright.x == 20 && upright.y == -5 && upright.z == -40)
        for axes in [MotionSample.Axes.portrait, .portraitUpsideDown, .landscapeLeft, .landscapeRight] {
            let field = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, axes: axes)
            let rate = MotionSample(sensor: .gyroscope, x: 20, y: -5, z: -40, axes: axes)
            #expect(field.x == rate.x && field.y == rate.y && field.z == rate.z, "\(axes)")
            // The same rotation about z as the acceleration, whose own sign differs.
            let gravity = MotionSample(sensor: .accelerometer, x: 20, y: -5, z: -40, axes: axes)
            #expect(Float(-9.80665) * field.x == gravity.x && Float(-9.80665) * field.y == gravity.y)
        }
        let left = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, axes: .landscapeLeft)
        #expect(left.x == -5 && left.y == -20 && left.z == -40)
        let right = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, axes: .landscapeRight)
        #expect(right.x == 5 && right.y == 20)
        let down = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, axes: .portraitUpsideDown)
        #expect(down.x == -20 && down.y == 5)
    }

    @Test func aReadingKeepsItsFieldWhenItsStatusChanges() {
        let reading = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, axes: .landscapeLeft, status: 3)
        let gone = reading.withStatus(0)
        #expect(gone.status == 0 && gone.x == reading.x && gone.y == reading.y && gone.z == reading.z)
        #expect(gone.sensor == .magnetometer && reading.withStatus(9).status == 3)
        #expect(MotionSample(sensor: .accelerometer, x: 1, y: 2, z: 3).withStatus(2).status == 0)
    }

    // MARK: When the field is read

    private func compassSampling() -> MotionSampling {
        var sampling = MotionSampling()
        sampling.hasSensors = true
        sampling.foreground = true
        sampling.connectionChanged(true)
        return sampling
    }

    @Test func theFieldIsReadOnlyForAServiceThatListsOneWhileAnApplicationUsesIt() {
        var sampling = compassSampling()
        // A service that never spoke is read as before: no field.
        #expect(sampling.plan == MotionPlan(accelerometer: 0.01, gyroscope: 0.01))
        #expect(sampling.plan.magnetometer == nil)
        // Older services cannot say the field is used.
        sampling.received(MotionDemand(accelerometer: 20_000, gyroscope: 20_000, orientationSensor: .active))
        #expect(sampling.plan.magnetometer == nil)
        // A service with a magnetometer: its period, read twice a period.
        sampling.received(MotionDemand(magnetometer: 20_000))
        #expect(sampling.plan == MotionPlan(magnetometer: 0.01))
        sampling.received(MotionDemand(magnetometer: 100_000))
        #expect(sampling.plan == MotionPlan(magnetometer: 0.05))
        sampling.received(MotionDemand(magnetometer: 1_000_000))
        #expect(sampling.plan == MotionPlan(magnetometer: 0.5))
        // Nobody uses anything: nothing runs, the connection stays.
        sampling.received(MotionDemand())
        #expect(sampling.plan == .off && sampling.wantsConnection)
        // The device and the foreground gate it like the others.
        sampling.received(MotionDemand(magnetometer: 20_000))
        sampling.foreground = false
        #expect(sampling.plan == .off)
        sampling.foreground = true
        sampling.hasSensors = false
        #expect(sampling.plan == .off)
        // A lost connection forgets that the last service had a magnetometer.
        sampling.hasSensors = true
        sampling.connectionChanged(false)
        sampling.connectionChanged(true)
        #expect(sampling.plan.magnetometer == nil)
    }

    private func eventually(_ timeout: Double = 3, _ condition: () -> Bool) -> Bool {
        let deadline = ContinuousClock.now + .milliseconds(Int64(timeout * 1000))
        while ContinuousClock.now < deadline {
            if condition() { return true }
            usleep(5_000)
        }
        return condition()
    }

    private func session(_ guest: Guest, _ device: Device) -> MotionSession {
        MotionSession(opener: guest, device: device, retryDelay: .milliseconds(40))
    }

    @Test func aServiceThatListsTheMagnetometerIsGivenTheFieldWhileAnApplicationUsesIt() throws {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        // Before the service says anything it is read as an old one: no field.
        #expect(eventually { device.intervals.count == 2 })
        #expect(device.magnetometerInterval == nil && device.magnetometerStarts == 0)
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 && device.intervals.isEmpty })
        device.emitField(20, -5, -40, status: 3)
        let record = try #require(MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 3).encoded)
        #expect(guest.receive(count: 20) == record)
        // Retimed as the application's period changes, and stopped when nobody uses it.
        guest.sayCompass(MotionDemand(magnetometer: 100_000))
        #expect(eventually { device.magnetometerInterval == 0.05 })
        guest.sayCompass(MotionDemand())
        #expect(eventually { device.magnetometerInterval == nil })
        session.settle()
        #expect(guest.isQuiet() && guest.connections == 1)
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 })
        // The run's end stops it.
        session.update(foreground: false, axes: .portrait)
        #expect(eventually { device.magnetometerInterval == nil })
    }

    @Test func theFieldCarriesTheMountOfTheRunItBelongsTo() throws {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .landscapeLeft)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 })
        device.emitField(20, -5, -40, status: 1)
        let record = try #require(
            MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, axes: .landscapeLeft, status: 1).encoded)
        #expect(guest.receive(count: 20) == record)
        session.update(foreground: false, axes: .landscapeLeft)
    }

    @Test func anOlderServiceIsNeverGivenTheFieldEvenOnADeviceThatHasOne() {
        // The images before the magnetometer send "ANC1" or "ANC2", or nothing.
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        guest.say(MotionDemand(accelerometer: 20_000, gyroscope: 20_000))
        #expect(eventually { device.intervals.count == 2 && session.demand != nil })
        guest.send([UInt8](older(magic: 0x32, mask: 0b111)))
        #expect(eventually { session.demand?.orientationSensor == .active })
        session.settle()
        #expect(device.magnetometerStarts == 0 && device.magnetometerInterval == nil)
        // Whatever the device might deliver is not sent.
        device.emitField(20, -5, -40, status: 3)
        #expect(guest.receive(count: 20, timeout: 60).isEmpty)
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aDeviceWithoutAMagnetometerNeverStartsOneForAServiceThatListsIt() {
        let guest = Guest()
        let device = Device(magnetometer: false)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(accelerometer: 20_000, magnetometer: 20_000))
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        session.settle()
        #expect(device.magnetometerStarts == 0)
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aDeviceWithOnlyAMagnetometerStillHoldsTheConnection() {
        let guest = Guest()
        let device = Device(accelerometer: false, gyroscope: false, magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(accelerometer: 20_000, magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 })
        #expect(device.intervals.isEmpty)
        session.update(foreground: false, axes: .portrait)
    }

    @Test func anAnc3FrameThatArrivesInPiecesIsWaitedFor() {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        let bytes = [UInt8](anc3(mask: 0b01000, magnetometer: 20_000))
        // The first 16 bytes are the size of an older frame: not a bad one.
        guest.send(Array(bytes[0..<16]))
        usleep(60_000)
        session.settle()
        #expect(guest.connections == 1 && device.intervals.count == 2 && device.magnetometerStarts == 0)
        guest.send(Array(bytes[16...]))
        #expect(eventually { device.magnetometerInterval == 0.01 && device.intervals.isEmpty })
        #expect(guest.connections == 1)
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aMagicNoFrameHasDropsTheConnectionAtOnce() {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        // Four bytes are enough to know: nothing waits for a length.
        guest.send([0x41, 0x4e, 0x43, 0x34])
        #expect(eventually { guest.connections >= 2 })
        session.update(foreground: false, axes: .portrait)
    }

    @Test func framesOfDifferentSizesInOneReadAreCutWhereTheMagicSays() {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        // One service sends one kind of frame, but the reader cuts by the magic
        // alone: 20 bytes, then 16, and the last frame is the one in force.
        guest.send([UInt8](anc3(mask: 0b01000, magnetometer: 20_000)) + [UInt8](older(magic: 0x31, mask: 0b01)))
        #expect(eventually { session.demand == MotionDemand(accelerometer: 10_000) })
        #expect(guest.connections == 1)
        // The older frame is the one in force: the field the first one began is
        // stopped, and nothing of it reaches the guest.
        session.settle()
        #expect(device.magnetometerInterval == nil)
        if device.magnetometerStarts == 1 { device.emitField(fromStart: 0, 20, -5, -40, status: 3) }
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        session.update(foreground: false, axes: .portrait)
    }

    // MARK: A failure of the field

    @Test func aFailingMagnetometerStopsItAloneAndTheRunGoesOn() throws {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(
            MotionDemand(accelerometer: 20_000, gyroscope: 20_000, magnetometer: 20_000, orientationSensor: .active))
        #expect(eventually { device.magnetometerInterval == 0.01 && device.intervals.count == 2 })
        device.emitField(20, -5, -40, status: 2)
        let last = try #require(MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 2).encoded)
        #expect(guest.receive(count: 20) == last)

        device.failMagnetometer()
        // The guest is told once: the last field, with status 0.
        let gone = try #require(MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 0).encoded)
        #expect(guest.receive(count: 20) == gone)
        #expect(eventually { device.magnetometerInterval == nil })
        // The run, the other two sensors and the orientation are untouched.
        session.settle()
        #expect(device.intervals.count == 2 && guest.connections == 1 && guest.isQuiet())
        device.emit(.accelerometer, 0, 0, -1)
        let acceleration = try #require(MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1).encoded)
        #expect(guest.receive(count: 20) == acceleration)
        session.setOrientation(.turnedClockwise)
        #expect(guest.receive(count: 20) == OrientationRecord.encoded(.turnedClockwise))

        // Said once: a failure the device reports again is not told again.
        device.failMagnetometer()
        #expect(guest.receive(count: 20, timeout: 60).isEmpty)
        // Not tried again in this run: not on a change of what the guest wants,
        // not on an update.
        guest.sayCompass(
            MotionDemand(accelerometer: 20_000, gyroscope: 20_000, magnetometer: 40_000, orientationSensor: .active))
        session.update(foreground: true, axes: .portrait, followsOrientation: true)
        session.settle()
        usleep(60_000)
        session.settle()
        #expect(device.magnetometerInterval == nil && device.magnetometerStarts == 1)
        #expect(device.intervals.count == 2)

        // The next run starts it again.
        session.update(foreground: false, axes: .portrait)
        #expect(eventually { device.intervals.isEmpty })
        session.update(foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 2 })
        guest.sayCompass(MotionDemand(magnetometer: 20_000, orientationSensor: .idle))
        #expect(eventually { device.magnetometerInterval == 0.01 && device.magnetometerStarts == 2 })
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aMagnetometerThatFailsBeforeItDeliveredAnythingTellsTheGuestWithZeros() throws {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .landscapeRight)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 })
        device.failMagnetometer()
        // Zeros are zeros whatever the mount (all three positive, not a turned -0).
        let gone = try #require(MotionSample(sensor: .magnetometer, x: 0, y: 0, z: 0, status: 0).encoded)
        #expect(guest.receive(count: 20) == gone)
        #expect(eventually { device.magnetometerInterval == nil })
        session.update(foreground: false, axes: .landscapeRight)
    }

    @Test func aFailingAccelerometerOrGyroscopeLeavesTheMagnetometerRunning() {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(accelerometer: 20_000, gyroscope: 20_000, magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 && device.intervals.count == 2 })
        device.fail()
        #expect(eventually { device.intervals.isEmpty })
        session.settle()
        #expect(device.magnetometerInterval == 0.01 && guest.connections == 1)
        // They have their own rule: the guest's demand changes and they start again.
        guest.sayCompass(MotionDemand(accelerometer: 20_000, magnetometer: 20_000))
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        session.update(foreground: false, axes: .portrait)
    }

    // MARK: A reading or a failure already on its way

    /// A guest that has been given the field, and the session running on it.
    private func subscribed(
        axes: MotionSample.Axes = .portrait, period: UInt32 = 20_000
    ) -> (guest: Guest, device: Device, session: MotionSession) {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: axes)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(magnetometer: period))
        #expect(eventually { device.magnetometerInterval != nil })
        return (guest, device, session)
    }

    @Test(arguments: [(UInt8(0x31), UInt32(0b001)), (UInt8(0x32), UInt32(0b111))])
    func aReadingOrAFailureOnItsWayAfterTheGuestFallsBackToAnOlderFrameIsDropped(
        magic: UInt8, mask: UInt32
    ) throws {
        let (guest, device, session) = subscribed()
        device.emitField(20, -5, -40, status: 3)
        #expect(guest.receive(count: 20) == (try #require(MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 3).encoded)))
        // The guest's service lost its magnetometer, or this is another one: an
        // older frame, after which no application has the field.
        guest.send([UInt8](older(magic: magic, mask: mask)))
        #expect(eventually { device.magnetometerInterval == nil && session.demand?.magnetometer == nil })
        session.settle()
        // Core Motion had already handed these to the first start's callbacks.
        device.emitField(fromStart: 0, 21, -4, -39, status: 3)
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        device.failMagnetometer(fromStart: 0)
        session.settle()
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        // The late failure ended nothing: the guest asks again and gets the field.
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 && device.magnetometerStarts == 2 })
        device.emitField(1, 2, 3, status: 2)
        #expect(guest.receive(count: 20) == (try #require(MotionSample(sensor: .magnetometer, x: 1, y: 2, z: 3, status: 2).encoded)))
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aReadingOrAFailureOnItsWayAfterAReconnectToAnOlderServiceIsDropped() {
        let (guest, device, session) = subscribed()
        guest.hangUp()
        #expect(eventually { guest.connections == 2 })
        // On the new connection the service has said nothing yet: no field is wanted.
        device.emitField(fromStart: 0, 20, -5, -40, status: 3)
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        // It says what an older image says.
        guest.say(MotionDemand(accelerometer: 20_000))
        #expect(eventually { session.demand == MotionDemand(accelerometer: 20_000) })
        session.settle()
        #expect(device.magnetometerInterval == nil)
        device.emitField(fromStart: 0, 20, -5, -40, status: 3)
        device.failMagnetometer(fromStart: 0)
        session.settle()
        #expect(guest.connections == 2)
        // Neither a type-2 record nor a closed connection (an older guest closes on one).
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        #expect(guest.isQuiet())
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aReadingOfAnEarlierSubscriptionOnTheSameConnectionIsDropped() {
        let (guest, device, session) = subscribed()
        guest.sayCompass(MotionDemand())
        #expect(eventually { device.magnetometerInterval == nil })
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerStarts == 2 && device.magnetometerInterval == 0.01 })
        // The first subscription ended; its readings do not count for the second.
        device.emitField(fromStart: 0, 20, -5, -40, status: 3)
        device.failMagnetometer(fromStart: 0)
        session.settle()
        #expect(guest.receive(count: 20, timeout: 80).isEmpty && device.magnetometerInterval == 0.01)
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aRetimeStaysInTheSubscriptionItBegan() throws {
        let (guest, device, session) = subscribed()
        guest.sayCompass(MotionDemand(magnetometer: 100_000))
        #expect(eventually { device.magnetometerInterval == 0.05 })
        // Core Motion may still be delivering to the handler of the first call.
        device.emitField(fromStart: 0, 20, -5, -40, status: 3)
        #expect(guest.receive(count: 20) == (try #require(MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 3).encoded)))
        session.update(foreground: false, axes: .portrait)
    }

    @Test func aReadingOfThePreviousRunCannotBecomeTheNextRunsLastField() throws {
        let (guest, device, session) = subscribed()
        device.emitField(20, -5, -40, status: 3)
        _ = guest.receive(count: 20)
        // The run ends (the guest went to the background) and another begins, on another mount.
        session.update(foreground: false, axes: .portrait)
        #expect(eventually { device.magnetometerInterval == nil })
        session.update(foreground: true, axes: .landscapeLeft)
        #expect(eventually { guest.connections == 2 })
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerStarts == 2 && device.magnetometerInterval == 0.01 })
        // The first run's reading arrives late: it is dropped and cached nowhere.
        device.emitField(fromStart: 0, 20, -5, -40, status: 3)
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        // The second run's field fails before it delivered anything: zeros, not the old field.
        device.failMagnetometer(fromStart: 1)
        let zeros = try #require(MotionSample(sensor: .magnetometer, x: 0, y: 0, z: 0, status: 0).encoded)
        #expect(guest.receive(count: 20) == zeros)
        session.update(foreground: false, axes: .landscapeLeft)
    }

    @Test func aFieldThatCannotBeSentDoesNotReplaceTheLastOne() throws {
        let (guest, device, session) = subscribed()
        device.emitField(20, -5, -40, status: 3)
        _ = guest.receive(count: 20)
        device.emitField(.nan, -5, -40, status: 3)
        device.emitField(1, 2, 1e39, status: 3)
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        device.failMagnetometer()
        // The last field that could be sent, with status 0.
        let gone = try #require(MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 0).encoded)
        #expect(guest.receive(count: 20) == gone)
        session.update(foreground: false, axes: .portrait)
    }

    @Test func theFieldIsNeverSentToAConnectionWhoseLatestFrameDoesNotAskForIt() throws {
        // The stream's own rule, with no session in between: where a frame is
        // read and a record is sent, under one lock.
        final class Seen: @unchecked Sendable {
            private let lock = NSLock()
            private var connected = false
            private var frames = 0
            func connection(_ value: Bool) { lock.withLock { connected = value } }
            func frame() { lock.withLock { frames += 1 } }
            var isConnected: Bool { lock.withLock { connected } }
            var frameCount: Int { lock.withLock { frames } }
        }
        let guest = Guest()
        let seen = Seen()
        let stream = MotionStream(
            opener: guest, retryDelay: .milliseconds(40),
            connectionChanged: { seen.connection($0) }, demandChanged: { _ in seen.frame() })
        let epoch = stream.start()
        #expect(eventually { seen.isConnected })
        let field = MotionSample(sensor: .magnetometer, x: 20, y: -5, z: -40, status: 3)
        let acceleration = MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1)
        // A service that has said nothing wants no field.
        #expect(!stream.submit(field, epoch: epoch))
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { seen.frameCount == 1 })
        #expect(stream.submit(field, epoch: epoch))
        #expect(guest.receive(count: 20) == field.encoded)
        // The older frames say it is no longer wanted; the other sensors are not affected.
        guest.send([UInt8](older(magic: 0x32, mask: 0b101)))
        #expect(eventually { seen.frameCount == 2 })
        #expect(!stream.submit(field, epoch: epoch))
        #expect(stream.submit(acceleration, epoch: epoch))
        #expect(guest.receive(count: 20) == acceleration.encoded)
        // A frame asking for it again, then another connection, which starts from nothing.
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { seen.frameCount == 3 })
        #expect(stream.submit(field, epoch: epoch))
        _ = guest.receive(count: 20)
        guest.hangUp()
        #expect(eventually { guest.connections == 2 && seen.isConnected })
        #expect(!stream.submit(field, epoch: epoch))
        // A frame that does not list the field, in the same read as one that does, says no.
        guest.send([UInt8](anc3(mask: 0b01000, magnetometer: 20_000)) + [UInt8](anc3(mask: 0b00001)))
        #expect(eventually { seen.frameCount == 5 })
        #expect(!stream.submit(field, epoch: epoch))
        stream.stop()
        #expect(!stream.submit(field, epoch: epoch))
    }

    // MARK: The device is asked once for a run

    @Test func theDevicesMagnetometerIsAskedAboutOncePerRunAndNotOnEveryUpdate() {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 })
        for _ in 0..<5 { session.update(foreground: true, axes: .portrait) }
        guest.sayCompass(MotionDemand(magnetometer: 40_000))
        #expect(eventually { device.magnetometerInterval == 0.02 })
        session.settle()
        #expect(device.magnetometerReads == 1)
        // A guest in the background is not asked about either.
        session.update(foreground: false, axes: .portrait)
        session.update(foreground: false, axes: .portrait)
        session.settle()
        #expect(device.magnetometerReads == 1)
        // The next run asks again.
        session.update(foreground: true, axes: .portrait)
        session.settle()
        #expect(device.magnetometerReads == 2)
        session.update(foreground: false, axes: .portrait)
    }

    // MARK: The orientation sensor in the same frame

    @Test func anAnc3FrameThatListsTheOrientationSensorLetsTheDeviceOrientationFlow() {
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait, followsOrientation: true)
        session.setOrientation(.turnedCounterClockwise)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(orientationSensor: .idle))
        #expect(guest.receive(count: 20) == OrientationRecord.encoded(.turnedCounterClockwise))
        session.update(foreground: false, axes: .portrait)
    }

    @Test func anAnc3FrameThatDoesNotListItGetsNoOrientationRecord() {
        // An iPad's guest: a magnetometer, and no device-orientation sensor.
        let guest = Guest()
        let device = Device(magnetometer: true)
        let session = session(guest, device)
        session.update(foreground: true, axes: .portrait, followsOrientation: true)
        session.setOrientation(.upright)
        #expect(eventually { guest.connections == 1 })
        guest.sayCompass(MotionDemand(magnetometer: 20_000))
        #expect(eventually { device.magnetometerInterval == 0.01 })
        #expect(session.demand?.acceptsOrientation == false)
        #expect(guest.receive(count: 20, timeout: 80).isEmpty)
        session.update(foreground: false, axes: .portrait)
    }
}

extension Data {
    fileprivate func withMagic(_ last: UInt8) -> Data {
        var copy = self
        copy[copy.startIndex + 3] = last
        return copy
    }
}
