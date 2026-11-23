// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore
import AndariumCoreTestSupport

/// The position source and the guest's receiver as a test plays them: every
/// call is kept, in order.
@MainActor
private final class Recorder: LocationUpdates, LocationSink {
    enum Event: Equatable {
        case start, stop, clear
        case report(LocationFix)
    }

    private(set) var events: [Event] = []

    func start() { events.append(.start) }
    func stop() { events.append(.stop) }
    func report(_ fix: LocationFix) { events.append(.report(fix)) }
    func clear() { events.append(.clear) }

    var starts: Int { events.filter { $0 == .start }.count }
    var stops: Int { events.filter { $0 == .stop }.count }
    var clears: Int { events.filter { $0 == .clear }.count }
    var reports: [LocationFix] {
        events.compactMap { if case .report(let fix) = $0 { fix } else { nil } }
    }

    func forget() { events.removeAll() }
}

private func fix(approximate: Bool = false, at seconds: Double = 1_800_000_000) -> LocationFix {
    LocationFix(
        utcNanoseconds: Int64(seconds * 1e9), latitude: 37.3349, longitude: -122.009, approximate: approximate,
        horizontalAccuracy: approximate ? 5000 : 8)
}

/// The conditions that put the guest's receiver on a real position, and the
/// order in which the held position goes.
@MainActor
struct LocationSessionTests {
    private func rig() -> (LocationSession, Recorder) {
        let recorder = Recorder()
        return (LocationSession(updates: recorder, sink: recorder), recorder)
    }

    /// A session with everything but the receiver's power true.
    private func ready(_ authorization: LocationAuthorization = .precise) -> (LocationSession, Recorder) {
        let (session, recorder) = rig()
        session.update(enabled: true, authorization: authorization, foreground: true, running: true)
        return (session, recorder)
    }

    // MARK: When positions are read

    @Test func nothingIsReadUntilTheGuestPowersItsReceiver() {
        let (session, recorder) = ready()
        #expect(!session.isLocating)
        #expect(recorder.starts == 0)
        session.receiverPower(true)
        #expect(session.isLocating)
        #expect(recorder.starts == 1)
    }

    @Test func positionsAreReadOnlyWhenEveryConditionHolds() {
        let flags = [true, false]
        for enabled in flags {
            for allowed in flags {
                for foreground in flags {
                    for running in flags {
                        for powered in flags {
                            let (session, recorder) = rig()
                            session.update(
                                enabled: enabled, authorization: allowed ? .precise : .denied,
                                foreground: foreground, running: running)
                            session.receiverPower(powered)
                            let expected = enabled && allowed && foreground && running && powered
                            let state = "enabled \(enabled) allowed \(allowed) foreground \(foreground) running \(running) powered \(powered)"
                            #expect(session.isLocating == expected, Comment(rawValue: state))
                            #expect(recorder.starts == (expected ? 1 : 0), Comment(rawValue: state))
                        }
                    }
                }
            }
        }
    }

    @Test(arguments: [
        LocationAuthorization.notDetermined, .servicesOff, .restricted, .denied,
    ])
    func noAuthorizationMeansNoReading(_ authorization: LocationAuthorization) {
        let (session, recorder) = ready(authorization)
        session.receiverPower(true)
        #expect(!session.isLocating)
        #expect(recorder.events.isEmpty || recorder.starts == 0)
        session.deliver(fix())
        #expect(recorder.reports.isEmpty)
    }

    @Test func approximateAccessReadsToo() {
        let (session, recorder) = ready(.approximate)
        session.receiverPower(true)
        #expect(session.isLocating)
        session.deliver(fix(approximate: true))
        #expect(recorder.reports == [fix(approximate: true)])
    }

    /// The ways a reading ends.
    enum Ending: CaseIterable, Sendable {
        case switchOff, accessRefused, background, runEnded, receiverOff, guestRestart

        @MainActor func apply(to session: LocationSession) {
            switch self {
            case .switchOff:
                session.update(enabled: false, authorization: .precise, foreground: true, running: true)
            case .accessRefused:
                session.update(enabled: true, authorization: .denied, foreground: true, running: true)
            case .background:
                session.update(enabled: true, authorization: .precise, foreground: false, running: true)
            case .runEnded:
                session.update(enabled: true, authorization: .precise, foreground: true, running: false)
            case .receiverOff:
                session.receiverPower(false)
            case .guestRestart:
                session.guestRestarted()
            }
        }
    }

    @Test(arguments: Ending.allCases)
    func eachConditionEndingStopsTheReadingAndDropsTheHeldPosition(_ ending: Ending) {
        let (session, recorder) = ready()
        session.receiverPower(true)
        session.deliver(fix())
        recorder.forget()
        ending.apply(to: session)
        #expect(!session.isLocating)
        #expect(recorder.events.contains(.stop))
        #expect(recorder.events.contains(.clear))
        // What arrives after the stop is not the guest's.
        recorder.forget()
        session.deliver(fix(at: 1_800_000_001))
        #expect(recorder.events.isEmpty)
    }

    @Test func theSwitchAppliesAtOnceInBothDirections() {
        let (session, recorder) = ready()
        session.receiverPower(true)
        session.deliver(fix())
        recorder.forget()
        session.update(enabled: false, authorization: .precise, foreground: true, running: true)
        #expect(recorder.events == [.stop, .clear])
        recorder.forget()
        session.update(enabled: true, authorization: .precise, foreground: true, running: true)
        // The guest's receiver is still powered, so the reading begins again.
        #expect(session.isLocating)
        #expect(recorder.events == [.start])
    }

    // MARK: The receiver's power is a level

    @Test func repeatedPowerEventsChangeNothing() {
        let (session, recorder) = ready()
        session.receiverPower(true)
        session.receiverPower(true)
        #expect(recorder.starts == 1)
        session.receiverPower(false)
        session.receiverPower(false)
        #expect(recorder.stops == 1)
    }

    @Test func aLateEventAfterTheRunEndsStartsNothing() {
        let (session, recorder) = ready()
        session.receiverPower(true)
        session.update(enabled: true, authorization: .precise, foreground: true, running: false)
        recorder.forget()
        // The runtime's event for the guest that just ended arrives late.
        session.receiverPower(true)
        #expect(!session.isLocating)
        #expect(recorder.events.isEmpty)
        // The next run does not inherit it.
        session.update(enabled: true, authorization: .precise, foreground: true, running: true)
        #expect(!session.isLocating)
        #expect(recorder.starts == 0)
    }

    @Test func aRestartForgetsThePowerUntilTheNewBootPowersItAgain() {
        let (session, recorder) = ready()
        session.receiverPower(true)
        session.guestRestarted()
        #expect(!session.isLocating)
        // The restart's own "off" event is a repeat of what is forgotten.
        session.receiverPower(false)
        #expect(recorder.stops == 1)
        session.receiverPower(true)
        #expect(session.isLocating)
        #expect(recorder.starts == 2)
    }

    @Test func theForegroundComingBackReadsAgainWhileThePowerHolds() {
        let (session, recorder) = ready()
        session.receiverPower(true)
        session.update(enabled: true, authorization: .precise, foreground: false, running: true)
        #expect(!session.isLocating)
        session.update(enabled: true, authorization: .precise, foreground: true, running: true)
        #expect(session.isLocating)
        #expect(recorder.starts == 2 && recorder.stops == 1)
    }

    // MARK: The class of a position

    @Test func aPositionOfTheOtherClassIsNeverTheGuests() {
        let (session, recorder) = ready(.precise)
        session.receiverPower(true)
        session.deliver(fix(approximate: true))
        #expect(recorder.reports.isEmpty)

        session.update(enabled: true, authorization: .approximate, foreground: true, running: true)
        session.deliver(fix(approximate: false))
        #expect(recorder.reports.isEmpty)
        session.deliver(fix(approximate: true))
        #expect(recorder.reports == [fix(approximate: true)])
    }

    @Test func aChangeOfAccuracyDropsTheHeldPositionBeforeTheNextOne() {
        let (session, recorder) = ready(.precise)
        session.receiverPower(true)
        session.deliver(fix())
        recorder.forget()
        session.update(enabled: true, authorization: .approximate, foreground: true, running: true)
        session.deliver(fix(approximate: true, at: 1_800_000_002))
        #expect(recorder.events == [.clear, .report(fix(approximate: true, at: 1_800_000_002))])
        // Reading goes on across the change.
        #expect(session.isLocating && recorder.starts == 0 && recorder.stops == 0)
    }

    @Test func aChangeOfAuthorizationThatEndsTheReadingDropsThePositionOnce() {
        let (session, recorder) = ready(.precise)
        session.receiverPower(true)
        session.deliver(fix())
        recorder.forget()
        session.update(enabled: true, authorization: .denied, foreground: true, running: true)
        #expect(recorder.events == [.stop, .clear] || recorder.events == [.clear, .stop])
        #expect(recorder.clears == 1)
    }

    @Test func aPositionIsPassedOnWholeAndInOrder() {
        let (session, recorder) = ready()
        session.receiverPower(true)
        let first = LocationFix(
            utcNanoseconds: 1, latitude: 1, longitude: 2, approximate: false, altitude: 3, ellipsoidalAltitude: 4,
            horizontalAccuracy: 5, verticalAccuracy: 6, speed: 7, speedAccuracy: 8, course: 9, courseAccuracy: 10)
        let second = fix(at: 1_800_000_003)
        session.deliver(first)
        session.deliver(second)
        #expect(recorder.reports == [first, second])
    }
}

/// What a reading from iOS becomes: a field iOS marks invalid is left out,
/// never sent as zero.
struct LocationFixTests {
    private func reading(
        horizontal: Double = 6, vertical: Double = 4, speed: Double = 1.5, speedAccuracy: Double = 0.2,
        course: Double = 90, courseAccuracy: Double = 12, latitude: Double = 37.3349, longitude: Double = -122.009
    ) -> LocationReading {
        LocationReading(
            timestamp: Date(timeIntervalSince1970: 1_800_000_000.25), latitude: latitude, longitude: longitude,
            altitude: 20, ellipsoidalAltitude: 52, horizontalAccuracy: horizontal, verticalAccuracy: vertical,
            speed: speed, speedAccuracy: speedAccuracy, course: course, courseAccuracy: courseAccuracy)
    }

    @Test func aFullReadingKeepsEveryField() throws {
        let fix = try #require(LocationFix(reading(), approximate: false))
        #expect(fix.utcNanoseconds == 1_800_000_000_250_000_000)
        #expect(fix.latitude == 37.3349 && fix.longitude == -122.009)
        #expect(!fix.approximate)
        #expect(fix.altitude == 20 && fix.ellipsoidalAltitude == 52 && fix.verticalAccuracy == 4)
        #expect(fix.horizontalAccuracy == 6)
        #expect(fix.speed == 1.5 && fix.speedAccuracy == 0.2)
        #expect(fix.course == 90 && fix.courseAccuracy == 12)
    }

    @Test func theClassIsWhateverTheCallerSaysIOSGrantedAtDelivery() throws {
        #expect(try #require(LocationFix(reading(), approximate: true)).approximate)
    }

    @Test func aNegativeVerticalAccuracyTakesTheAltitudesWithIt() throws {
        let fix = try #require(LocationFix(reading(vertical: -1), approximate: false))
        #expect(fix.altitude == nil && fix.ellipsoidalAltitude == nil && fix.verticalAccuracy == nil)
        #expect(fix.horizontalAccuracy == 6)
    }

    @Test func aNegativeSpeedOrCourseIsLeftOutWithItsAccuracy() throws {
        let fix = try #require(LocationFix(reading(speed: -1, speedAccuracy: 0.2, course: -1, courseAccuracy: 3), approximate: false))
        #expect(fix.speed == nil && fix.speedAccuracy == nil)
        #expect(fix.course == nil && fix.courseAccuracy == nil)
    }

    @Test func aValidValueWithAnInvalidAccuracyKeepsTheValueOnly() throws {
        let fix = try #require(LocationFix(reading(speedAccuracy: -1, courseAccuracy: -1), approximate: false))
        #expect(fix.speed == 1.5 && fix.speedAccuracy == nil)
        #expect(fix.course == 90 && fix.courseAccuracy == nil)
    }

    @Test func aZeroIsAValueAndStays() throws {
        let fix = try #require(LocationFix(reading(speed: 0, course: 0), approximate: false))
        #expect(fix.speed == 0 && fix.course == 0)
    }

    @Test func aReadingWithoutAValidPositionIsNoFix() {
        #expect(LocationFix(reading(horizontal: -1), approximate: false) == nil)
        #expect(LocationFix(reading(horizontal: .nan), approximate: false) == nil)
        #expect(LocationFix(reading(latitude: 91), approximate: false) == nil)
        #expect(LocationFix(reading(latitude: .nan), approximate: false) == nil)
        #expect(LocationFix(reading(longitude: 181), approximate: false) == nil)
        #expect(LocationFix(reading(longitude: -180.5), approximate: false) == nil)
    }

    @Test func theHemispheresAndTheEdgesAreKept() throws {
        let south = try #require(LocationFix(reading(latitude: -33.8568, longitude: 151.2153), approximate: false))
        #expect(south.latitude == -33.8568 && south.longitude == 151.2153)
        #expect(LocationFix(reading(latitude: 90, longitude: -180), approximate: false) != nil)
    }

    @Test func nonFiniteOptionalValuesAreLeftOut() throws {
        var odd = reading()
        odd.altitude = .nan
        odd.speed = .infinity
        let fix = try #require(LocationFix(odd, approximate: false))
        #expect(fix.altitude == nil)
        #expect(fix.ellipsoidalAltitude == 52)
        #expect(fix.speed == nil && fix.speedAccuracy == nil)
    }
}

struct LocationAuthorizationTests {
    @Test func onlyAllowedAccessReadsAndOnlyReducedAccessIsApproximate() {
        let all: [LocationAuthorization] = [.notDetermined, .servicesOff, .restricted, .denied, .precise, .approximate]
        #expect(all.filter(\.allowsLocating) == [.precise, .approximate])
        #expect(all.filter(\.isApproximate) == [.approximate])
        #expect(all.filter(\.refuses) == [.servicesOff, .restricted, .denied])
    }

    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    @Test func theChoiceIsOffForANewGuestAndForOneStoredBefore() throws {
        #expect(!Guest.fresh(on: host).location)
        let before = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":true,"includeInBackups":false}"#.utf8)
        #expect(try JSONDecoder().decode(Guest.self, from: before).location == false)
    }

    @Test func theChoiceIsKeptAsChosenThroughSaveAndErase() throws {
        var guest = Guest.fresh(on: host)
        guest.location = true
        let saved = try JSONEncoder().encode(guest)
        #expect(try JSONDecoder().decode(Guest.self, from: saved).location)
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest.erased)).location)
        guest.location = false
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest)).location == false)
    }
}
