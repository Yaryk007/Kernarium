// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The model the entry builds owns the live runtime, which owns the device's
/// motion sensors, and letting go of the model must end all of them without
/// a crash. Core Motion crashed when its manager was released over again,
/// which a hosted run reaches because every test that builds a model releases
/// one; the application's own model lives as long as the process.
@MainActor
struct ModelLifetimeTests {
    @Test(.timeLimit(.minutes(5)))
    func aModelFromTheEntryCanBeBuiltAndReleasedOverAgain() async throws {
        let entry = AppEntry.fromArguments()
        for round in 0..<40 {
            weak var model: AppModel?
            weak var runtime: AnyObject?
            try autoreleasepool {
                let built = entry.makeModel()
                guard let driver = built.driver else { throw NoRuntime() }
                model = built
                runtime = driver as AnyObject
            }
            #expect(model == nil, "round \(round): the model outlived its owner")
            #expect(runtime == nil, "round \(round): the runtime outlived its model")
            if round % 10 == 0 { try await Task.sleep(for: .milliseconds(50)) }
        }
    }

    @Test(.timeLimit(.minutes(5)))
    func motionSensorsCanBeBuiltAndReleasedOverAgain() async throws {
        for round in 0..<300 {
            weak var sensors: MotionSensors?
            autoreleasepool {
                let built = MotionSensors()
                sensors = built
                _ = built.available
            }
            #expect(sensors == nil, "round \(round): the sensors outlived their owner")
            if round % 50 == 0 { try await Task.sleep(for: .milliseconds(50)) }
        }
    }

    @Test(.timeLimit(.minutes(5)))
    func motionSensorsReleasedWhileTheirSessionHasWorkDoNotCrash() async throws {
        for round in 0..<100 {
            let sensors = MotionSensors()
            sensors.update(enabled: true, foreground: true, axes: .portrait)
            sensors.restart()
            if round % 3 == 0 { try await Task.sleep(for: .milliseconds(20)) }
            sensors.stop()
        }
    }

    private struct NoRuntime: Error, CustomStringConvertible {
        var description: String { "this copy of the test application carries no system, so the model has no runtime" }
    }
}
