// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The connection to the guest's thermal HAL: vsock 5559, with the same
/// bound on one attempt as motion's and haptics'.
nonisolated struct ThermalConnector: GuestStreamOpener {
    func openStream() throws -> Int32 {
        let descriptor = kernarium_runtime_loader_connect_vsock(ThermalProtocol.port, 250)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
        return descriptor
    }
}

/// Where the application reads iOS's thermal state from.
nonisolated enum ThermalSource {
    /// The state now: the launch's own in a development build that states one
    /// (`ThermalOverride`), else the system's.
    static func current() -> HostThermalState {
        if let stated = ThermalOverride.stated(in: ProcessInfo.processInfo.environment) { return stated }
        return HostThermalState(ProcessInfo.processInfo.thermalState)
    }
}

/// Gives iOS's Low Power Mode to its reader the way `ThermalMonitor` gives the
/// thermal state: now, on every change the system announces, and on `refresh`.
@MainActor
final class LowPowerMonitor {
    private let read: @MainActor () -> Bool
    private let deliver: @MainActor (Bool) -> Void
    private let center: NotificationCenter
    private var observer: NSObjectProtocol?

    init(
        center: NotificationCenter = .default,
        read: @escaping @MainActor () -> Bool = { ProcessInfo.processInfo.isLowPowerModeEnabled },
        deliver: @escaping @MainActor (Bool) -> Void
    ) {
        self.center = center
        self.read = read
        self.deliver = deliver
    }

    deinit {
        if let observer { center.removeObserver(observer) }
    }

    func start() {
        if observer == nil {
            observer = center.addObserver(
                forName: .NSProcessInfoPowerStateDidChange, object: nil, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            }
        }
        refresh()
    }

    func refresh() {
        deliver(read())
    }

    func stop() {
        if let observer { center.removeObserver(observer) }
        observer = nil
        // A run that has ended holds nothing.
        deliver(false)
    }
}

/// Gives the thermal state to its reader: now, on every change the system
/// announces, and whenever `refresh` is called (the application returns to the
/// foreground, where a change may have gone unseen). The guest runs only in
/// the foreground, so there is nothing to read in the background.
@MainActor
final class ThermalMonitor {
    private let read: @MainActor () -> HostThermalState
    private let deliver: @MainActor (HostThermalState) -> Void
    private let center: NotificationCenter
    private var observer: NSObjectProtocol?

    init(
        center: NotificationCenter = .default,
        read: @escaping @MainActor () -> HostThermalState = ThermalSource.current,
        deliver: @escaping @MainActor (HostThermalState) -> Void
    ) {
        self.center = center
        self.read = read
        self.deliver = deliver
    }

    deinit {
        if let observer { center.removeObserver(observer) }
    }

    func start() {
        if observer == nil {
            observer = center.addObserver(
                forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            }
        }
        refresh()
    }

    func refresh() {
        deliver(read())
    }

    func stop() {
        if let observer { center.removeObserver(observer) }
        observer = nil
    }
}
