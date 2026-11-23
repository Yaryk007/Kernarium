// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Opening an Android app from a Home Screen icon, Shortcuts, Search or Siri.
/// A request names one app; what happens next follows the Start button's own
/// rules (`StartPlan`, `StartVerdict`) and Android's state, and is decided by
/// the pure functions here so that every outcome is a row of a test.
///
/// The request lives in memory only. Nothing is stored that would make the
/// next launch of Andarium open an app: iOS ending the process drops it.

/// One request to open an app. A newer request replaces an older one.
public struct AppOpenRequest: Equatable, Sendable, Identifiable {
    /// Counts requests, so an answer meant for an older one is told from the
    /// current one.
    public let serial: Int
    public let component: AndroidComponent
    /// The app's name as the request knew it, for the cards and banners.
    public let label: String

    public var id: Int { serial }
}

/// Why a request waits at Home for the person.
public enum AppOpenHold: Equatable, Sendable {
    /// An update that did not finish: Home asks whether to try again or restore.
    case update
    /// The device has too little free space to start Android.
    case space
    /// A snapshot operation holds the guest's files; the request goes on when it ends.
    case snapshot
}

/// Where a request is.
public enum AppOpenStage: Equatable, Sendable {
    /// Held until the person answers what Home asks, or what holds it ends.
    case waiting(AppOpenHold, since: Date)
    /// Android is starting for it.
    case starting
    /// Android is up: the app opens when Android is ready for it.
    case opening
    /// The launch command is sent and its reply is awaited. It is never sent
    /// again for this request: a pause or a hold that comes now waits for the
    /// reply, it does not queue another launch.
    case launching
}

public struct PendingAppOpen: Equatable, Sendable {
    public var request: AppOpenRequest
    public var stage: AppOpenStage
}

/// What went wrong, in the shape of a banner over Android.
public enum AppOpenFailure: Equatable, Sendable {
    /// The app is not in Android (removed, disabled, or Android was erased).
    case notInAndroid
    /// Android did not open it in time; trying again may help.
    case tookTooLong
    /// Android is in recovery mode and runs no apps.
    case recovery
    /// Android's first-run setup is not finished; the app opens once it is.
    case setupUnfinished
    /// The image has no tool that opens apps (an older image): trying again cannot help.
    case toolMissing

    /// Whether the banner offers Try Again.
    public var canRetry: Bool {
        switch self {
        case .tookTooLong, .setupUnfinished: true
        case .notInAndroid, .recovery, .toolMissing: false
        }
    }
}

public struct AppOpenBanner: Equatable, Sendable, Identifiable {
    public let serial: Int
    public let failure: AppOpenFailure
    public let label: String
    /// The app the request was for, whose icon the banner draws.
    public let app: AndroidComponent
    /// What Try Again asks again; nil where the banner offers no retry.
    public let component: AndroidComponent?
    public let shownAt: Date

    public var id: Int { serial }

    public init(serial: Int, failure: AppOpenFailure, label: String, app: AndroidComponent, component: AndroidComponent?, shownAt: Date) {
        self.serial = serial
        self.failure = failure
        self.label = label
        self.app = app
        self.component = component
        self.shownAt = shownAt
    }
}

/// Why a request that Andarium will not carry out says so, where it says so.
public enum AppOpenRefusal: Equatable, Sendable {
    /// Android is in recovery mode: a banner says so.
    case recovery
    /// There is no virtual machine at all (it was erased): an alert says the
    /// app is not in Android.
    case noAndroid
    /// This device cannot draw Android: the alert Start raises.
    case needsNewerChip
    /// The virtual machine cannot start with this Android: Home explains it.
    case cannotStart
    /// The run ended in failure: its own screen says so.
    case runEnded
    /// An erase or another task that changes the guest is under way at Home:
    /// nothing starts under it, and the request is dropped.
    case busy
}

/// What is true of Andarium at the moment a request arrives.
public struct AppOpenSituation: Equatable, Sendable {
    public var phase: Phase
    public var plan: StartPlan
    /// The verdict Start would get at Home (`Lifecycle.startVerdict`).
    public var verdict: StartVerdict
    /// A snapshot operation holds the guest's files.
    public var holdsGuestFiles: Bool
    /// The guest is paused for want of space.
    public var spacePaused: Bool
    /// A task that changes the guest (an erase, a create) is under way behind
    /// the busy overlay: the guest in hand is not the one the task leaves.
    public var guestBusy: Bool
    /// A snapshot restore is under way, or has unlocked the guest's files
    /// and not yet been adopted by the model: the guest and the plan in hand
    /// are the ones before it.
    public var restoring: Bool

    public init(
        phase: Phase, plan: StartPlan, verdict: StartVerdict, holdsGuestFiles: Bool, spacePaused: Bool, guestBusy: Bool = false,
        restoring: Bool = false
    ) {
        self.phase = phase
        self.plan = plan
        self.verdict = verdict
        self.holdsGuestFiles = holdsGuestFiles
        self.spacePaused = spacePaused
        self.guestBusy = guestBusy
        self.restoring = restoring
    }
}

public enum AppOpenDecision: Equatable, Sendable {
    /// Android runs: the app opens as soon as Android is ready for it.
    case openNow
    /// Android is off and the Start button would start it: start it.
    case startAndroid
    /// Android is starting already.
    case alreadyStarting
    case hold(AppOpenHold)
    case refuse(AppOpenRefusal)
}

public enum AppOpenPlanner {
    /// What a request does in a situation. The rows:
    ///
    /// - A normal run opens the app now, unless the guest is paused for space
    ///   (the request waits for the person to resume it).
    /// - Recovery runs no apps. A start under way is the one it is, and a boot
    ///   into Recovery refuses when it ends.
    /// - A failed run needs a new process; its screen says why.
    /// - At Home the Start button's own rules apply. An unfinished update, a
    ///   device without room and a snapshot operation wait for the person or
    ///   for the operation; a guest that cannot start, a device that cannot
    ///   draw Android, and no guest at all refuse; every other start starts.
    public static func decide(_ situation: AppOpenSituation) -> AppOpenDecision {
        switch situation.phase {
        case .running(.normal):
            return situation.spacePaused ? .hold(.space) : .openNow
        case .running(.recovery):
            return .refuse(.recovery)
        case let .booting(kind):
            return kind == .recovery ? .refuse(.recovery) : .alreadyStarting
        case .failed:
            return .refuse(.runEnded)
        case .home:
            // An erase or a create owns the guest: Start cannot be pressed under it either.
            if situation.guestBusy { return .refuse(.busy) }
            // The verdict below is the restore's predecessor's: wait for the
            // model to have adopted what the restore left, then decide again.
            if situation.restoring { return .hold(.snapshot) }
        }
        switch situation.verdict {
        case .noGuest:
            return .refuse(.noAndroid)
        case .needsNewerChip:
            return .refuse(.needsNewerChip)
        case .relaunchRequired:
            return .refuse(.runEnded)
        case .blocked:
            return .refuse(.cannotStart)
        case .lowSpace, .unfinishedUpdate, .allowed:
            break
        }
        // An unfinished update is the person's to settle, even where Start
        // could try it again: Home asks which way.
        if case .unfinishedUpdate = situation.plan { return .hold(.update) }
        switch situation.verdict {
        case .lowSpace: return .hold(.space)
        case .unfinishedUpdate: return .hold(.update)
        default: return situation.holdsGuestFiles ? .hold(.snapshot) : .startAndroid
        }
    }
}

/// What the model does next, after the state has moved.
public enum AppOpenEffect: Equatable, Sendable {
    case none
    /// Start Android as the Start button does, then open.
    case startAndroid
    /// Open the request's app (Android is up).
    case open(AppOpenRequest)
    /// Say it at Home, over the guest's absence: an alert.
    case alert(AppOpenRefusal, label: String)
}

/// The pending request and the banner of the last failure, as one state that
/// every event moves. The model feeds it the events and does what it answers.
public struct AppOpenState: Equatable, Sendable {
    public private(set) var pending: PendingAppOpen?
    public private(set) var banner: AppOpenBanner?
    private var serial = 0

    /// How long a request waits at Home for the person before it is dropped.
    public static let holdLimit: TimeInterval = 600
    /// How long a banner stays, and the longer time of one with Try Again.
    public static let bannerLimit: TimeInterval = 12
    public static let retryBannerLimit: TimeInterval = 20

    public init() {}

    /// A new request. It replaces a pending one and the banner of an earlier failure.
    public mutating func request(
        _ component: AndroidComponent, label: String, in situation: AppOpenSituation, now: Date
    ) -> AppOpenEffect {
        serial += 1
        banner = nil
        let request = AppOpenRequest(serial: serial, component: component, label: label)
        switch AppOpenPlanner.decide(situation) {
        case .openNow:
            pending = PendingAppOpen(request: request, stage: .opening)
            return .open(request)
        case .startAndroid:
            pending = PendingAppOpen(request: request, stage: .starting)
            return .startAndroid
        case .alreadyStarting:
            pending = PendingAppOpen(request: request, stage: .starting)
            return .none
        case let .hold(hold):
            pending = PendingAppOpen(request: request, stage: .waiting(hold, since: now))
            return .none
        case let .refuse(refusal):
            pending = nil
            switch refusal {
            case .recovery:
                show(.recovery, for: request, now: now)
                return .none
            case .noAndroid:
                return .alert(.noAndroid, label: label)
            case .needsNewerChip:
                return .alert(.needsNewerChip, label: label)
            case .cannotStart, .runEnded, .busy:
                return .none
            }
        }
    }

    /// The person answered what Home asked (Start, Update and start, Restore
    /// then Start): a request that waited is starting now. The wait has its
    /// limit whenever the expiry gets to it: a Start that comes first finds the
    /// request over, and starts Android without it.
    public mutating func personStarted(now: Date) {
        guard var pending, case let .waiting(_, since) = pending.stage else { return }
        if now.timeIntervalSince(since) >= Self.holdLimit {
            self.pending = nil
            return
        }
        pending.stage = .starting
        self.pending = pending
    }

    /// What held a request ended (a snapshot operation, or the pause for
    /// space). The request is decided again in the new situation.
    public mutating func releaseHold(
        _ hold: AppOpenHold, in situation: AppOpenSituation, now: Date
    ) -> AppOpenEffect {
        guard let pending, case let .waiting(held, since) = pending.stage, held == hold else { return .none }
        // The wait has its limit, whenever the expiry gets to it: a release that
        // comes first finds the request over, not opened.
        if now.timeIntervalSince(since) >= Self.holdLimit {
            self.pending = nil
            return .none
        }
        switch AppOpenPlanner.decide(situation) {
        case .openNow:
            self.pending?.stage = .opening
            return .open(pending.request)
        case .startAndroid:
            self.pending?.stage = .starting
            return .startAndroid
        case .alreadyStarting:
            self.pending?.stage = .starting
            return .none
        case let .hold(again):
            // A hold that changes kind (a snapshot operation that leaves too
            // little room) is the same wait: its limit counts from the first.
            self.pending?.stage = .waiting(again, since: since)
            return .none
        case let .refuse(refusal):
            self.pending = nil
            if refusal == .recovery { show(.recovery, for: pending.request, now: now) }
            return .none
        }
    }

    /// The boot reached Android. A request that was starting opens now, or,
    /// when Android came up in Recovery, ends with the banner that says so.
    public mutating func androidIsUp(recovery: Bool, now: Date) -> AppOpenEffect {
        guard let pending, pending.stage == .starting else { return .none }
        if recovery {
            self.pending = nil
            show(.recovery, for: pending.request, now: now)
            return .none
        }
        self.pending?.stage = .opening
        return .open(pending.request)
    }

    /// How opening the request ended. An answer for a request that was
    /// replaced or dropped meanwhile changes nothing.
    public mutating func opened(_ outcome: AndroidAppOpenOutcome, for request: AppOpenRequest, now: Date) {
        guard pending?.request == request else { return }
        // A request taken back to waiting (`mayReplaceLaunch`) has no answer: its replacement launch was never sent.
        if case .waiting? = pending?.stage { return }
        pending = nil
        switch outcome {
        case .opened, .openedRenamed: break
        case .notInAndroid: show(.notInAndroid, for: request, now: now)
        case .failed: show(.tookTooLong, for: request, now: now)
        case .toolMissing: show(.toolMissing, for: request, now: now)
        }
    }

    /// The guest was paused for want of space while Android was being made
    /// ready for the request: it waits for the person's answer like one that
    /// arrived during the pause, and the wait is bounded by `holdLimit`.
    /// Returns whether a request that was opening now waits. A request whose
    /// launch is sent (`.launching`) is left as it is: its reply is awaited,
    /// and the launch is not sent again.
    @discardableResult
    public mutating func spacePaused(now: Date) -> Bool {
        guard var pending, pending.stage == .opening else { return false }
        pending.stage = .waiting(.space, since: now)
        self.pending = pending
        return true
    }

    /// Right before the launch is sent: the request is decided again in the
    /// situation of that moment, because Android's readiness was waited for
    /// over seconds in which the run could have ended or paused. True where
    /// the launch may go.
    public mutating func readyToLaunch(_ request: AppOpenRequest, in situation: AppOpenSituation, now: Date) -> Bool {
        guard pending?.request == request, pending?.stage == .opening else { return false }
        return decideLaunch(of: request, in: situation, now: now)
    }

    /// Right before a replacement launch is sent: the first command of a
    /// launch in flight was answered (the activity was not found), the apps
    /// were listed again to learn why, and the launch of the app's renamed
    /// activity is a command that has not been sent. The request is decided
    /// again as `readyToLaunch` decides it, because the listing took seconds:
    /// a pause for space takes it back to waiting, a run that ended or a
    /// request that replaced it ends it. True where the command may go.
    public mutating func mayReplaceLaunch(_ request: AppOpenRequest, in situation: AppOpenSituation, now: Date) -> Bool {
        guard pending?.request == request, pending?.stage == .launching else { return false }
        return decideLaunch(of: request, in: situation, now: now)
    }

    private mutating func decideLaunch(of request: AppOpenRequest, in situation: AppOpenSituation, now: Date) -> Bool {
        switch AppOpenPlanner.decide(situation) {
        case .openNow:
            // The launch goes out now; from here its reply is waited for.
            pending?.stage = .launching
            return true
        case .hold(.space):
            pending?.stage = .waiting(.space, since: now)
            return false
        case .refuse(.recovery):
            pending = nil
            show(.recovery, for: request, now: now)
            return false
        case .hold, .startAndroid, .alreadyStarting, .refuse:
            pending = nil
            return false
        }
    }

    /// Android's first-run setup was seen to be unfinished: the request ends
    /// with a banner whose Try Again asks again.
    public mutating func setupUnfinished(for request: AppOpenRequest, now: Date) {
        guard pending?.request == request else { return }
        pending = nil
        show(.setupUnfinished, for: request, now: now)
    }

    /// "Don't open": the request is dropped; Android keeps starting.
    public mutating func drop() {
        pending = nil
    }

    /// The run ended or was stopped, or the guest was erased: nothing waits.
    public mutating func runEnded() {
        pending = nil
    }

    public mutating func dismissBanner() {
        banner = nil
    }

    /// Drops a request that waited at Home for longer than `holdLimit`, and a
    /// banner that has stayed its time. Returns whether anything went.
    @discardableResult
    public mutating func expire(now: Date) -> Bool {
        var changed = false
        if let pending, case let .waiting(_, since) = pending.stage, now.timeIntervalSince(since) >= Self.holdLimit {
            self.pending = nil
            changed = true
        }
        if let banner {
            let limit = banner.failure.canRetry ? Self.retryBannerLimit : Self.bannerLimit
            if now.timeIntervalSince(banner.shownAt) >= limit {
                self.banner = nil
                changed = true
            }
        }
        return changed
    }

    private mutating func show(_ failure: AppOpenFailure, for request: AppOpenRequest, now: Date) {
        banner = AppOpenBanner(
            serial: request.serial, failure: failure, label: request.label, app: request.component,
            component: failure.canRetry ? request.component : nil, shownAt: now)
    }
}
