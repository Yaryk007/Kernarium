// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import CoreLocation
import Foundation
import Testing
@testable import Andarium

/// iOS's location permission as a test plays it: the access the system
/// reports, and the question it asks the person, answered when the test says.
@MainActor
final class FakeLocationAccess: LocationAccessSource {
    var state: LocationAuthorization
    var changed: (@MainActor () -> Void)?
    private(set) var asked = 0

    init(_ state: LocationAuthorization = .notDetermined) { self.state = state }

    func request() { asked += 1 }

    /// The person answers the question iOS is showing, or changes the access
    /// in Settings while Andarium is open.
    func answer(_ state: LocationAuthorization) {
        self.state = state
        changed?()
    }
}

/// The position source and the guest's receiver as a test plays them.
@MainActor
final class RecordingReceiver: LocationUpdates, LocationSink {
    private(set) var reading = false
    private(set) var reports: [LocationFix] = []
    private(set) var clears = 0
    func start() { reading = true }
    func stop() { reading = false }
    func report(_ fix: LocationFix) { reports.append(fix) }
    func clear() { clears += 1 }
}

/// Location asks iOS when it is turned on, shows itself off and unusable when
/// iOS refuses or the person takes the access away, tags what iOS gives as
/// approximate, and gives the guest a position only while all of it holds. The
/// switch applies at once in a run. Nothing here reads a real location.
@MainActor
struct LocationFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private struct Setup {
        let model: AppModel
        let driver: StubDriver
        let system: FakeLocationAccess
        let receiver: RecordingReceiver
        let session: LocationSession
        var applied: Int { appliedCount.value }
        let appliedCount: Counter
    }

    @MainActor final class Counter { var value = 0 }

    /// A model with the device's wiring: a driver that keeps the guest, the
    /// location controller on a fake permission in `access`, and a session
    /// set the way `LiveRuntime.updateLocation` sets it, for a normal run in
    /// front (`running`).
    private func setup(
        _ access: LocationAuthorization = .notDetermined, controller: Bool = true, running: Bool = true,
        chosen: Bool = false
    ) -> Setup {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        var guest = Guest.fresh(on: host)
        guest.location = chosen
        let model = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        // A test never opens the system's Settings.
        model.openSettings = {}
        let fake = FakeLocationAccess(access)
        let receiver = RecordingReceiver()
        let session = LocationSession(updates: receiver, sink: receiver)
        let counter = Counter()
        if controller {
            model.location = LocationController(model: model, source: fake) { [weak model] in
                counter.value += 1
                guard let model else { return }
                session.update(
                    enabled: model.locationShownOn, authorization: model.locationAuthorization,
                    foreground: true, running: running)
            }
        }
        return Setup(
            model: model, driver: driver, system: fake, receiver: receiver, session: session, appliedCount: counter)
    }

    private func fix(approximate: Bool = false) -> LocationFix {
        LocationFix(
            utcNanoseconds: 1_800_000_000_000_000_000, latitude: 37.3349, longitude: -122.009,
            approximate: approximate, horizontalAccuracy: approximate ? 5000 : 8)
    }

    // MARK: Not asked yet -> allowed or refused

    @Test func aNewGuestHasItOff() {
        let s = setup()
        #expect(s.model.guest?.location == false)
        #expect(!s.model.locationShownOn)
        #expect(s.system.asked == 0)
    }

    @Test func turningItOnAsksIOSAtOnceAndNothingIsSharedUntilTheAnswer() {
        let s = setup()
        s.model.setLocation(true)
        #expect(s.system.asked == 1)
        #expect(s.model.guest?.location == true)
        #expect(s.model.locationShownOn)
        s.session.receiverPower(true)
        #expect(!s.session.isLocating)

        s.system.answer(.precise)
        #expect(s.model.locationAuthorization == .precise)
        #expect(s.model.locationShownOn)
        #expect(s.session.isLocating)
        #expect(s.driver.savedGuest?.location == true)
        // Nothing asked again.
        #expect(s.system.asked == 1)
    }

    @Test func aRefusalTakesTheChoiceBackAndTheGuestGetsNothing() {
        let s = setup()
        s.model.setLocation(true)
        s.session.receiverPower(true)
        s.system.answer(.denied)
        #expect(s.model.locationAuthorization == .denied)
        #expect(s.model.guest?.location == false)
        #expect(!s.model.locationShownOn)
        #expect(s.driver.savedGuest?.location == false)
        #expect(!s.session.isLocating)
        #expect(s.receiver.reports.isEmpty)
    }

    // MARK: Refused: nothing to ask

    @Test(arguments: [LocationAuthorization.denied, .restricted, .servicesOff])
    func turningItOnWhereIOSRefusesExplainsAndAsksNothing(_ access: LocationAuthorization) {
        let s = setup(access)
        s.model.setLocation(true)
        #expect(s.model.permissionPrompt == .location)
        #expect(s.model.guest?.location == false)
        #expect(!s.model.locationShownOn)
        #expect(s.system.asked == 0)
        #expect(LocationWords.alert(for: access) != nil)
    }

    @Test func settingsHasTheWayOutOfARefusalButNotOutOfARestriction() {
        #expect(LocationWords.alert(for: .denied)?.offersSettings == true)
        #expect(LocationWords.alert(for: .servicesOff)?.offersSettings == true)
        #expect(LocationWords.alert(for: .approximate)?.offersSettings == true)
        #expect(LocationWords.alert(for: .restricted)?.offersSettings == false)
        #expect(LocationWords.alert(for: .precise) == nil)
        #expect(LocationWords.alert(for: .notDetermined) == nil)
    }

    @Test func allowingItInSettingsTurnsOnTheSwitchTheyTapped() {
        let s = setup(.denied)
        s.model.setLocation(true)
        s.model.openLocationSettings()
        #expect(s.model.locationAwaitsSettings)
        #expect(!s.model.locationShownOn)
        // The person allows it in Settings and returns.
        s.system.state = .precise
        s.model.location?.refresh()
        #expect(s.model.locationAuthorization == .precise)
        #expect(s.model.locationShownOn)
        #expect(s.model.guest?.location == true)
        #expect(!s.model.locationAwaitsSettings)
        #expect(s.driver.savedGuest?.location == true)
    }

    @Test func goingToSettingsAndComingBackWithoutAllowingLeavesItOff() {
        let s = setup(.denied)
        s.model.setLocation(true)
        s.model.openLocationSettings()
        s.model.location?.refresh()
        #expect(!s.model.locationShownOn)
        #expect(!s.model.locationAwaitsSettings || s.model.locationAuthorization == .denied)
        // A later allowance, with no tap, does not turn it on.
        s.model.locationAwaitsSettings = false
        s.system.state = .precise
        s.model.location?.refresh()
        #expect(!s.model.locationShownOn)
    }

    // MARK: Access taken away or changed while it is on

    @Test func accessTakenAwayInSettingsTurnsItOffAndStopsTheGuestsPosition() {
        let s = setup(.precise, chosen: true)
        s.model.location?.refresh()
        s.session.receiverPower(true)
        s.session.deliver(fix())
        #expect(s.receiver.reports.count == 1 && s.session.isLocating)
        let clears = s.receiver.clears

        s.system.answer(.denied)
        #expect(!s.model.locationShownOn)
        #expect(s.model.guest?.location == false)
        #expect(s.driver.savedGuest?.location == false)
        #expect(!s.session.isLocating)
        #expect(s.receiver.clears > clears)
    }

    @Test func locationServicesTurnedOffStopsIt() {
        let s = setup(.precise, chosen: true)
        s.model.location?.refresh()
        s.session.receiverPower(true)
        s.system.answer(.servicesOff)
        #expect(!s.model.locationShownOn)
        #expect(!s.session.isLocating)
        #expect(LocationWords.detail(for: .servicesOff, shownOn: false) == LocationWords.servicesOff)
    }

    @Test func approximateAccessStaysOnAndTagsWhatTheGuestGets() {
        let s = setup(.approximate, chosen: true)
        s.model.location?.refresh()
        #expect(s.model.locationShownOn)
        s.session.receiverPower(true)
        #expect(s.session.isLocating)
        s.session.deliver(fix(approximate: false))
        #expect(s.receiver.reports.isEmpty)
        s.session.deliver(fix(approximate: true))
        #expect(s.receiver.reports == [fix(approximate: true)])
        #expect(LocationWords.detail(for: .approximate, shownOn: true) == LocationWords.approximate)
        #expect(LocationWords.detail(for: .approximate, shownOn: false) == nil)
    }

    @Test func aChangeOfAccuracyInSettingsClearsTheHeldPositionAndKeepsTheSwitch() {
        let s = setup(.precise, chosen: true)
        s.model.location?.refresh()
        s.session.receiverPower(true)
        s.session.deliver(fix())
        let clears = s.receiver.clears
        s.system.answer(.approximate)
        #expect(s.model.locationShownOn)
        #expect(s.receiver.clears == clears + 1)
        // A position still on its way from before the change is the old class.
        s.session.deliver(fix(approximate: false))
        #expect(s.receiver.reports.count == 1)
        s.session.deliver(fix(approximate: true))
        #expect(s.receiver.reports.count == 2)
    }

    @Test func accessThatWasOnlyForOnceHasEndedByTheNextLaunch() {
        // The choice was stored while iOS allowed it once; iOS asks again, and
        // no question is under way.
        let s = setup(.notDetermined, chosen: true)
        s.model.location?.refresh()
        #expect(s.model.locationAuthorization == .notDetermined)
        #expect(s.model.guest?.location == false)
        #expect(!s.model.locationShownOn)
        #expect(s.driver.savedGuest?.location == false)
        #expect(s.system.asked == 0)
    }

    @Test func aQuestionOnScreenIsNotMistakenForOnceAccessEnding() {
        let s = setup()
        s.model.setLocation(true)
        s.model.location?.refresh()
        #expect(s.model.guest?.location == true)
        #expect(s.model.locationShownOn)
    }

    // MARK: Live

    @Test func theSwitchAppliesAtOnceInARunAndTheGuestsPositionGoesWithIt() {
        let s = setup(.precise)
        s.model.location?.refresh()
        s.model.setLocation(true)
        s.session.receiverPower(true)
        #expect(s.session.isLocating)
        s.session.deliver(fix())
        #expect(s.receiver.reading)

        let clears = s.receiver.clears
        s.model.setLocation(false)
        #expect(!s.session.isLocating && !s.receiver.reading)
        #expect(s.receiver.clears > clears)
        #expect(s.driver.savedGuest?.location == false)

        s.model.setLocation(true)
        #expect(s.session.isLocating && s.receiver.reading)
    }

    @Test func noRunMeansNoReadingWhateverTheSwitchSays() {
        let s = setup(.precise, running: false)
        s.model.location?.refresh()
        s.model.setLocation(true)
        s.session.receiverPower(true)
        #expect(!s.session.isLocating)
    }

    @Test func aPreviewWithoutAControllerTakesTheAccessAsGiven() {
        let s = setup(controller: false)
        s.model.setLocation(true)
        #expect(s.model.locationAuthorization == .precise)
        #expect(s.model.locationShownOn)
        s.model.setLocation(false)
        #expect(!s.model.locationShownOn)
    }

    @Test func theControllerReadsTheAccessWhenItIsMade() {
        #expect(setup(.denied).model.locationAuthorization == .denied)
        #expect(setup(.approximate).model.locationAuthorization == .approximate)
        #expect(setup(.notDetermined).model.locationAuthorization == .notDetermined)
    }

    @Test func turningItOffForgetsAPendingTripToSettings() {
        let s = setup(.denied)
        s.model.setLocation(true)
        s.model.openLocationSettings()
        s.model.setLocation(false)
        #expect(!s.model.locationAwaitsSettings)
    }
}

/// What the row says in each state, and what Core Location's answers make of
/// the access.
@MainActor
struct LocationAccessMappingTests {
    @Test func theRowSaysWhyItCannotBeUsed() {
        #expect(LocationWords.detail(for: .denied, shownOn: false) == "Not allowed in Settings")
        #expect(LocationWords.detail(for: .restricted, shownOn: false) != nil)
        #expect(LocationWords.detail(for: .servicesOff, shownOn: false) != nil)
        #expect(LocationWords.detail(for: .precise, shownOn: true) == nil)
        #expect(LocationWords.detail(for: .notDetermined, shownOn: true) == nil)
    }

    @Test(arguments: [
        (CLAuthorizationStatus.notDetermined, CLAccuracyAuthorization.fullAccuracy, true, LocationAuthorization.notDetermined),
        (.restricted, .fullAccuracy, true, .restricted),
        (.restricted, .fullAccuracy, false, .restricted),
        (.denied, .fullAccuracy, true, .denied),
        (.denied, .fullAccuracy, false, .servicesOff),
        (.authorizedWhenInUse, .fullAccuracy, true, .precise),
        (.authorizedWhenInUse, .reducedAccuracy, true, .approximate),
        (.authorizedAlways, .fullAccuracy, true, .precise),
        (.authorizedAlways, .reducedAccuracy, true, .approximate),
        (.authorizedWhenInUse, .fullAccuracy, false, .servicesOff),
        (.notDetermined, .fullAccuracy, false, .servicesOff),
    ])
    func coreLocationsAnswersMakeTheAccess(
        status: CLAuthorizationStatus, accuracy: CLAccuracyAuthorization, services: Bool, expected: LocationAuthorization
    ) {
        #expect(LocationAuthorization(status: status, accuracy: accuracy, servicesEnabled: services) == expected)
    }

    @Test func aPositionFromCoreLocationBecomesAFixTaggedWithTheAccuracyAtDelivery() throws {
        let when = Date(timeIntervalSince1970: 1_800_000_000.5)
        let location = CLLocation(
            coordinate: CLLocationCoordinate2D(latitude: 37.3349, longitude: -122.009), altitude: 20,
            horizontalAccuracy: 6, verticalAccuracy: 4, course: 90, courseAccuracy: 12, speed: 1.5,
            speedAccuracy: 0.2, timestamp: when)
        let precise = try #require(HostLocation.fix(from: location, approximate: false))
        #expect(!precise.approximate)
        #expect(precise.latitude == 37.3349 && precise.longitude == -122.009)
        #expect(precise.utcNanoseconds == 1_800_000_000_500_000_000)
        #expect(precise.altitude == 20 && precise.ellipsoidalAltitude != nil)
        #expect(precise.horizontalAccuracy == 6 && precise.verticalAccuracy == 4)
        #expect(precise.speed == 1.5 && precise.speedAccuracy == 0.2)
        #expect(precise.course == 90 && precise.courseAccuracy == 12)
        #expect(try #require(HostLocation.fix(from: location, approximate: true)).approximate)
    }

    @Test func anInvalidPositionIsDroppedAndInvalidFieldsAreLeftOut() throws {
        let invalid = CLLocation(
            coordinate: CLLocationCoordinate2D(latitude: 1, longitude: 2), altitude: 0, horizontalAccuracy: -1,
            verticalAccuracy: -1, timestamp: Date())
        #expect(HostLocation.fix(from: invalid, approximate: false) == nil)
        // A region's centre: the area is the accuracy, and nothing else is known.
        let area = CLLocation(
            coordinate: CLLocationCoordinate2D(latitude: 1, longitude: 2), altitude: 0, horizontalAccuracy: 5000,
            verticalAccuracy: -1, timestamp: Date())
        let fix = try #require(HostLocation.fix(from: area, approximate: true))
        #expect(fix.altitude == nil && fix.verticalAccuracy == nil)
        #expect(fix.speed == nil && fix.course == nil)
        #expect(fix.horizontalAccuracy == 5000)
    }

    @Test func theRuntimesFixCarriesAFlagForEachValidField() {
        let full = LocationFix(
            utcNanoseconds: 42, latitude: -33.8568, longitude: 151.2153, approximate: false, altitude: 3,
            ellipsoidalAltitude: 4, horizontalAccuracy: 5, verticalAccuracy: 6, speed: 7, speedAccuracy: 8,
            course: 9, courseAccuracy: 10)
        let runtime = RuntimeLocationSink.runtimeFix(full)
        #expect(runtime.struct_size == UInt32(MemoryLayout<kernarium_runtime_location_fix>.size))
        #expect(runtime.utc_ns == 42 && runtime.latitude == -33.8568 && runtime.longitude == 151.2153)
        let every = [
            KERNARIUM_RUNTIME_LOCATION_HAS_ALTITUDE, KERNARIUM_RUNTIME_LOCATION_HAS_ELLIPSOIDAL_ALTITUDE,
            KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY, KERNARIUM_RUNTIME_LOCATION_HAS_VERTICAL_ACCURACY,
            KERNARIUM_RUNTIME_LOCATION_HAS_SPEED, KERNARIUM_RUNTIME_LOCATION_HAS_SPEED_ACCURACY,
            KERNARIUM_RUNTIME_LOCATION_HAS_COURSE, KERNARIUM_RUNTIME_LOCATION_HAS_COURSE_ACCURACY,
        ].reduce(UInt32(0)) { $0 | UInt32($1) }
        #expect(runtime.flags == every)
        #expect(runtime.altitude == 3 && runtime.ellipsoidal_altitude == 4)
        #expect(runtime.horizontal_accuracy == 5 && runtime.vertical_accuracy == 6)
        #expect(runtime.speed == 7 && runtime.speed_accuracy == 8)
        #expect(runtime.course == 9 && runtime.course_accuracy == 10)

        // An approximate fix with nothing but a position and its area.
        let area = LocationFix(
            utcNanoseconds: 1, latitude: 1, longitude: 2, approximate: true, horizontalAccuracy: 5000)
        let bare = RuntimeLocationSink.runtimeFix(area)
        #expect(bare.flags == UInt32(KERNARIUM_RUNTIME_LOCATION_APPROXIMATE | KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY))
        #expect(bare.altitude == 0 && bare.speed == 0 && bare.course == 0)
    }

    @Test func withNoGuestTheRuntimeRefusesAndNothingFails() {
        // The loader answers -ENODEV when no runtime is loaded, for a fix and
        // for a clear alike.
        let sink = RuntimeLocationSink()
        sink.report(LocationFix(utcNanoseconds: 1, latitude: 1, longitude: 2, approximate: false))
        sink.clear()
        #expect(kernarium_runtime_loader_report_location(nil) <= 0)
    }
}
