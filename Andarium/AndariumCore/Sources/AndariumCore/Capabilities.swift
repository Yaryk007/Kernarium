// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the runtime and storage behind the screens can do. A screen shows a
/// control only when the capability behind it exists, so the application
/// never offers what it cannot do. The Simulator preview has all of them; the
/// device application has the ones its runtime provides.
public struct Capabilities: Equatable, Sendable {
    /// A computer can reach the guest with ADB over the network.
    public var adbOverNetwork: Bool
    /// An app picked in Files (an .apk) can be installed into the guest.
    public var apkInstall: Bool
    /// The guest's bug report can be made and shared.
    public var bugReport: Bool
    /// The session can show the guest's own statistics.
    public var statistics: Bool
    /// The guest's internal storage can be browsed, and files imported into it
    /// and exported from it.
    public var guestFiles: Bool
    /// A text message can be sent to the guest as if from the network
    /// (`GuestSms`).
    public var textMessages: Bool
    /// The session's Settings can turn TalkBack, Android's screen reader, on
    /// and off.
    public var screenReader: Bool

    public init(
        adbOverNetwork: Bool, apkInstall: Bool, bugReport: Bool, statistics: Bool, guestFiles: Bool,
        textMessages: Bool, screenReader: Bool
    ) {
        self.adbOverNetwork = adbOverNetwork
        self.apkInstall = apkInstall
        self.bugReport = bugReport
        self.statistics = statistics
        self.guestFiles = guestFiles
        self.textMessages = textMessages
        self.screenReader = screenReader
    }

    /// Whether a session started in `mode` can offer Install APK: Settings has
    /// the row, and the side keys' tip says where it is, in a normal session
    /// of a runtime that installs apps. Recovery runs no adb.
    public func offersApkInstall(in mode: StartMode) -> Bool {
        mode == .normal && apkInstall
    }

    /// Everything the mock runtime pretends to do.
    public static let preview = Capabilities(
        adbOverNetwork: true, apkInstall: true, bugReport: true, statistics: true, guestFiles: true,
        textMessages: true, screenReader: true
    )

    /// The device application: the session offers an app install, ADB over
    /// the network, a bug report, the guest's files and TalkBack, all through
    /// the runtime's adb channel, the statistics from the runtime's own
    /// readings, and text messages, which go to the guest's service over
    /// vsock (`GuestSmsChannel`) and not through adb.
    public static let device = Capabilities(
        adbOverNetwork: true,
        apkInstall: true,
        bugReport: true,
        statistics: true,
        guestFiles: true,
        textMessages: true,
        screenReader: true
    )
}
