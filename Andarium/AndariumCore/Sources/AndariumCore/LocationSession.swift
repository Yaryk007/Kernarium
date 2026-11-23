// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The device's position source as a session drives it: the application reads
/// iOS's Core Location while it runs, and hands each reading to the session's
/// `deliver`.
@MainActor
public protocol LocationUpdates: AnyObject {
    /// Starts reading positions; a reading already running stays.
    func start()
    /// Stops reading.
    func stop()
}

/// The guest's GNSS receiver, where the session's fixes go.
@MainActor
public protocol LocationSink: AnyObject {
    /// The newest position. The receiver repeats it for as long as it stays
    /// worth reporting.
    func report(_ fix: LocationFix)
    /// Drops the position the receiver holds: it reports no fix until the
    /// next one.
    func clear()
}

/// Shares the device's location with the guest's GNSS receiver while all of
/// these hold, and reads it from iOS only then:
///
/// - the person's Location switch is on;
/// - iOS allows Andarium to use the location while it is in use;
/// - Andarium is in front, and the guest is not held still;
/// - a normal run of the guest is going; and
/// - the guest has powered its receiver (an Android app wants a position).
///
/// The receiver's power events are a level: one repeats, and one can arrive
/// late at a guest's end, so a run's end or a restart is what forgets the
/// power, and an event outside a run is ignored. When any condition ends the
/// reading stops and the held position is dropped at once.
///
/// A position goes to the guest only if it was read under the accuracy iOS
/// grants now: a change of the authorization drops what the receiver holds
/// first, and a position still on its way from before the change (tagged the
/// other class) is dropped, so a position of the old class is never repeated
/// under the new one.
///
/// The session runs on the main actor, where Core Location delivers.
@MainActor
public final class LocationSession {
    private let updates: any LocationUpdates
    private let sink: any LocationSink

    private var switchOn = false
    private var authorization = LocationAuthorization.notDetermined
    private var foreground = false
    private var running = false
    private var receiverPowered = false
    private var locating = false

    public init(updates: any LocationUpdates, sink: any LocationSink) {
        self.updates = updates
        self.sink = sink
    }

    /// Whether positions are being read for the guest now.
    public var isLocating: Bool { locating }

    /// What is wanted now: `enabled` is the person's switch as the screens
    /// show it, `authorization` what iOS says, `foreground` that Andarium is
    /// active and not holding the guest still, and `running` that a normal
    /// run of the guest has begun and not ended. The end of a run forgets the
    /// receiver's power.
    public func update(enabled: Bool, authorization: LocationAuthorization, foreground: Bool, running: Bool) {
        let changed = authorization != self.authorization
        self.switchOn = enabled
        self.authorization = authorization
        self.foreground = foreground
        if !running { receiverPowered = false }
        self.running = running
        reconcile(authorizationChanged: changed)
    }

    /// The guest powered its receiver (true) or stopped it (false). Outside a
    /// run the event is a late one and means nothing.
    public func receiverPower(_ powered: Bool) {
        guard running else { return }
        receiverPowered = powered
        reconcile(authorizationChanged: false)
    }

    /// The guest restarted itself: the new boot powers its receiver again if
    /// an application wants it.
    public func guestRestarted() {
        receiverPowered = false
        reconcile(authorizationChanged: false)
    }

    /// A position iOS read, which is the receiver's only while all the
    /// conditions hold and its class is the one iOS grants now.
    public func deliver(_ fix: LocationFix) {
        guard locating, fix.approximate == authorization.isApproximate else { return }
        sink.report(fix)
    }

    private var wanted: Bool {
        switchOn && authorization.allowsLocating && foreground && running && receiverPowered
    }

    private func reconcile(authorizationChanged: Bool) {
        if authorizationChanged { sink.clear() }
        let want = wanted
        if want, !locating {
            locating = true
            updates.start()
        } else if !want, locating {
            locating = false
            updates.stop()
            if !authorizationChanged { sink.clear() }
        }
    }
}
