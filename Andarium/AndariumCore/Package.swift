// swift-tools-version: 6.0
// SPDX-License-Identifier: GPL-2.0-or-later

import PackageDescription

// The Andarium application's model and policy, with no UIKit, SwiftUI or
// runtime dependency: `swift test` runs it on macOS, and the application and
// its Simulator preview import it. It re-exports GuestHostCore, what both
// applications decide the same way.
let package = Package(
    name: "AndariumCore",
    defaultLocalization: "en",
    platforms: [.macOS("27.0"), .iOS("26.0")],
    products: [
        .library(name: "AndariumCore", targets: ["AndariumCore"]),
        // For the application's hosted tests only: neither application links it.
        // Dynamic like the application's own product: a static copy would
        // give the test bundle a second AndariumCore, whose types the
        // application's code does not recognise (`error as? AdbError`).
        .library(name: "AndariumCoreTestSupport", type: .dynamic, targets: ["AndariumCoreTestSupport"]),
    ],
    dependencies: [
        .package(path: "../../GuestHost/Core"),
    ],
    targets: [
        .target(
            name: "AndariumCore",
            dependencies: [.product(name: "GuestHostCore", package: "Core")],
            resources: [.process("Resources/Localizable.xcstrings")]
        ),
        // Simulated controllers and the guest end of the gamepad wire, for tests
        // and the probe tool. The application never links it; the hosted
        // tests do.
        .target(
            name: "AndariumCoreTestSupport",
            dependencies: ["AndariumCore"]
        ),
        // Follow developer on X's logic, for the package's tests only. Its sources
        // are the application's own, which the application compiles only where the
        // row is compiled in (a symlink to Andarium/Shared/FollowX/Logic), so no
        // product of the package carries any of it; here the condition is always
        // set. Neither application links it.
        .target(
            name: "AndariumFollowXLogic",
            swiftSettings: [.define("FOLLOW_X")]
        ),
        // A test-only tool that plays a gamepad script to the guest's relay.
        .executableTarget(
            name: "andarium-gamepad-probe",
            dependencies: ["AndariumCore", "AndariumCoreTestSupport"]
        ),
        .testTarget(
            name: "AndariumCoreTests",
            dependencies: ["AndariumCore", "AndariumCoreTestSupport", "AndariumFollowXLogic"]
        ),
    ]
)
