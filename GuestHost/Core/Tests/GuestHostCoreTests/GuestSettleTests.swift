// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import GuestHostCore

/// The result of a sync or quiesce: an application calls the guest's storage
/// settled only when the guest confirmed it, or when no guest ran.
struct GuestSettleTests {
    @Test func aConfirmedWritebackIsSettled() {
        let settle = GuestSettle(unflushed: 0, storage: .confirmed, suspension: .suspended)
        #expect(settle.isSettled && settle.guestStorageSettled)
        #expect(settle.logLine(reason: "background") == "guest storage settled on background")
    }

    @Test func noGuestIsSettled() {
        // Nothing to settle is not a failure to reach a guest.
        let settle = GuestSettle(unflushed: 0, storage: .noGuest, suspension: .notAsked)
        #expect(settle.isSettled)
        #expect(settle.logLine(reason: "background") == "no guest running on background: nothing to settle")
        // The loader says so with a negative value when no runtime is loaded.
        #expect(GuestSettle(unflushed: -1, storage: .noGuest, suspension: .notAsked).isSettled)
    }

    @Test(arguments: [GuestSettle.Storage.notReported, .unconfirmed, .refused, .unreachable])
    func anyOtherOutcomeIsNotSettled(storage: GuestSettle.Storage) {
        let settle = GuestSettle(unflushed: 0, storage: storage, suspension: .notAsked)
        #expect(!settle.isSettled && !settle.guestStorageSettled)
        let line = settle.logLine(reason: "background")
        #expect(line.hasPrefix("guest storage NOT confirmed on background: "))
        #expect(!line.contains("settled on"))
    }

    @Test func aConfirmedGuestWithUnflushedHostFilesIsNotSettled() {
        let settle = GuestSettle(unflushed: 2, storage: .confirmed, suspension: .notAsked)
        #expect(!settle.isSettled && settle.guestStorageSettled)
        #expect(settle.logLine(reason: "inactive") == "inactive sync left 2 guest file(s) unflushed")
        // An unconfirmed guest and unflushed files say both.
        let both = GuestSettle(unflushed: 1, storage: .unreachable, suspension: .notAsked)
        #expect(both.logLine(reason: "inactive").hasSuffix("; the guest's console could not be reached"))
    }

    @Test func aGuestThatWasNotPutToSleepSaysSo() {
        let settle = GuestSettle(unflushed: 0, storage: .confirmed, suspension: .notSuspended)
        #expect(settle.isSettled)
        #expect(settle.logLine(reason: "terminating") == "guest storage settled on terminating (guest not suspended)")
        let refused = GuestSettle(unflushed: 0, storage: .refused, suspension: .notSuspended)
        #expect(refused.logLine(reason: "terminating").hasSuffix("(guest not suspended)"))
    }

    @Test func theRuntimeKeepingItsRAMIsSaidOnlyOfASettledGuest() {
        let settled = GuestSettle(unflushed: 0, storage: .confirmed, suspension: .suspended)
        #expect(settled.logLine(reason: "low space", kept: "RAM kept") == "guest storage settled on low space, RAM kept")
        let unconfirmed = GuestSettle(unflushed: 0, storage: .unconfirmed, suspension: .notSuspended)
        #expect(!unconfirmed.logLine(reason: "low space", kept: "RAM kept").contains("RAM kept"))
    }

    @Test func aValueTheBuildDoesNotKnowClaimsNothing() {
        let known = GuestSettle(unflushed: 0, rawStorage: 1, rawSuspension: 1)
        #expect(known == GuestSettle(unflushed: 0, storage: .confirmed, suspension: .suspended))
        let unknown = GuestSettle(unflushed: 0, rawStorage: 9, rawSuspension: 9)
        // A report that was never written is zero, and is no confirmation.
        let blank = GuestSettle(unflushed: -1, rawStorage: 0, rawSuspension: 0)
        #expect(blank.storage == .notReported && !blank.isSettled)
        #expect(unknown.storage == .unconfirmed && unknown.suspension == .notSuspended)
        #expect(!unknown.isSettled)
    }

    @Test func theRawValuesAreTheRuntimesOwn() {
        // KERNARIUM_RUNTIME_GUEST_STORAGE_* and _SUSPENSION_*.
        #expect([
            GuestSettle.Storage.notReported, .confirmed, .noGuest, .unconfirmed, .refused, .unreachable,
        ].map(\.rawValue) == [0, 1, 2, 3, 4, 5])
        #expect([GuestSettle.Suspension.notAsked, .suspended, .notSuspended].map(\.rawValue) == [0, 1, 2])
    }
}
