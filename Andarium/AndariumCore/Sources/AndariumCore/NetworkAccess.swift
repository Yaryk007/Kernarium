// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Network

/// Why iOS refuses Andarium the network, as its path says: the person turned
/// Wireless Data off for Andarium, or never answered iOS's question about it.
/// This is what the notice is for; having no network at all is not (Android
/// shows that itself).
public enum NetworkDenial: Equatable, Hashable, Sendable {
    /// Cellular data is denied and nothing else carries the path.
    case cellular
    /// Wi-Fi is denied for Andarium.
    case wifi
}

/// What the system's network path says about Andarium's access.
public enum NetworkAccess: Equatable, Sendable {
    /// A network is there for Andarium to use.
    case allowed
    /// iOS denies Andarium the network.
    case denied(NetworkDenial)
    /// No usable network, and no denial behind it: Airplane Mode, no signal,
    /// an inactive VPN, the Local Network permission. Android's own status
    /// shows that, and no notice is for it.
    case unavailable
}

/// A path as the system reported it, reduced to what Andarium decides on:
/// `NWPath.status` and `NWPath.unsatisfiedReason`. The mapping to
/// `NetworkAccess` is here, apart from the monitor, so a test gives it any
/// path.
public struct NetworkPathReport: Equatable, Sendable {
    public enum Status: Equatable, Sendable {
        case satisfied, unsatisfied, requiresConnection
    }

    /// `NWPath.UnsatisfiedReason`.
    public enum Reason: Equatable, Sendable {
        case notAvailable, cellularDenied, wifiDenied, localNetworkDenied, vpnInactive
    }

    public var status: Status
    /// Why an unsatisfied path is so; nil for any other status.
    public var reason: Reason?

    public init(status: Status, reason: Reason? = nil) {
        self.status = status
        self.reason = reason
    }

    /// A satisfied path is allowed. An unsatisfied one is a denial only when
    /// iOS names cellular or Wi-Fi access as the reason; no network, an
    /// inactive VPN and the Local Network permission are none of that. A path
    /// that requires a connection first is neither.
    public var access: NetworkAccess {
        switch status {
        case .satisfied:
            return .allowed
        case .requiresConnection:
            return .unavailable
        case .unsatisfied:
            switch reason {
            case .cellularDenied?: return .denied(.cellular)
            case .wifiDenied?: return .denied(.wifi)
            case .notAvailable?, .localNetworkDenied?, .vpnInactive?, nil: return .unavailable
            }
        }
    }

    public init(_ path: NWPath) {
        switch path.status {
        case .satisfied:
            self.init(status: .satisfied)
        case .requiresConnection:
            self.init(status: .requiresConnection)
        case .unsatisfied:
            let reason: Reason
            switch path.unsatisfiedReason {
            case .notAvailable: reason = .notAvailable
            case .cellularDenied: reason = .cellularDenied
            case .wifiDenied: reason = .wifiDenied
            case .localNetworkDenied: reason = .localNetworkDenied
            case .vpnInactive: reason = .vpnInactive
            @unknown default: reason = .notAvailable
            }
            self.init(status: .unsatisfied, reason: reason)
        @unknown default:
            self.init(status: .unsatisfied, reason: .notAvailable)
        }
    }
}

/// The notice that iOS denies Andarium the network: what Home and the session
/// show, from the reports of the path.
///
/// A denial shows one notice. The person dismisses it, and the same denial
/// does not show it again until the path has been allowed in between: leaving
/// the app and coming back, or the path changing from one unavailable state to
/// the same denial, repeats nothing. A denial by the other cause is another
/// notice. The notice goes when the path is allowed again, and when there is
/// no network at all.
public struct NetworkNotice: Equatable, Sendable {
    /// The denial in force, shown or dismissed; nil while the path is allowed
    /// or unavailable.
    public private(set) var denial: NetworkDenial?
    private var dismissed: Set<NetworkDenial> = []

    public init() {}

    /// The denial the notice is up for, which a screen shows; nil when there is
    /// none or the person has dismissed it.
    public var showing: NetworkDenial? {
        guard let denial, !dismissed.contains(denial) else { return nil }
        return denial
    }

    public var isShowing: Bool { showing != nil }

    /// The path reported `access`.
    public mutating func apply(_ access: NetworkAccess) {
        switch access {
        case .allowed:
            denial = nil
            dismissed.removeAll()
        case .denied(let cause):
            denial = cause
        case .unavailable:
            denial = nil
        }
    }

    /// The person dismisses the notice.
    public mutating func dismiss() {
        if let denial { dismissed.insert(denial) }
    }
}

/// The system's reports of the network path, behind a protocol so a test gives
/// its own. Starting is cheap, stopping ends every callback, and nothing here
/// makes a request of its own or wakes the process but a change of the path.
@MainActor
public protocol NetworkPathSource: AnyObject {
    /// Begins delivering reports, the current path first and then each
    /// change, on the main actor. Starting a started source starts it again.
    func start(_ deliver: @escaping @MainActor (NetworkPathReport) -> Void)
    func stop()
}

/// The system's path monitor. It is event driven: `NWPathMonitor` calls back
/// when the path changes and at no other time, it opens no connection, and a
/// stopped source holds no monitor. It makes a new monitor at each start,
/// since a cancelled one cannot start again.
@MainActor
public final class SystemNetworkPathSource: NetworkPathSource {
    private var monitor: NWPathMonitor?
    private let queue = DispatchQueue(label: "andarium.network-path", qos: .utility)

    public init() {}

    public func start(_ deliver: @escaping @MainActor (NetworkPathReport) -> Void) {
        stop()
        let monitor = NWPathMonitor()
        monitor.pathUpdateHandler = { path in
            let report = NetworkPathReport(path)
            Task { @MainActor in deliver(report) }
        }
        monitor.start(queue: queue)
        self.monitor = monitor
    }

    public func stop() {
        monitor?.cancel()
        monitor = nil
    }
}

/// Watches the path while it is wanted and keeps the notice. The model says
/// when a watch is wanted (the application is active and has a guest on Home
/// or in a session) and the watcher starts or stops its source to match; a
/// report that arrives after a stop is dropped, and so is one from an earlier
/// start of the source that is still on its way when the next start has
/// begun.
@MainActor
public final class NetworkAccessWatcher {
    private let source: any NetworkPathSource
    public private(set) var notice = NetworkNotice()
    /// The source is started.
    public private(set) var isWatching = false
    /// Which start of the source a report belongs to.
    private var generation = 0
    /// Runs after every change of the notice.
    public var changed: (@MainActor () -> Void)?

    public init(source: any NetworkPathSource) {
        self.source = source
    }

    /// Starts the source when a watch is wanted and it is not running, and
    /// stops it when it is not wanted and is. The first report follows a start
    /// at once.
    public func setWanted(_ wanted: Bool) {
        guard wanted != isWatching else { return }
        isWatching = wanted
        if wanted {
            generation &+= 1
            let started = generation
            source.start { [weak self] report in self?.receive(report, from: started) }
        } else {
            source.stop()
        }
    }

    func receive(_ report: NetworkPathReport, from started: Int) {
        guard isWatching, started == generation else { return }
        let before = notice
        notice.apply(report.access)
        if notice != before { changed?() }
    }

    /// The person dismisses the notice.
    public func dismiss() {
        let before = notice
        notice.dismiss()
        if notice != before { changed?() }
    }
}
