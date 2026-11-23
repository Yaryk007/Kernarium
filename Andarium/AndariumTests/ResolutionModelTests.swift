// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// The model's resolution: what the guest draws at, what is kept, and what
/// Home states of it.
@MainActor
struct ResolutionModelTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let phone = CGSize(width: 402, height: 778)

    private func model(native: Double = 3, limit: UInt64 = 0) -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        model.screenNativeScale = native
        model.memoryLimitBytes = limit
        return model
    }

    @Test func nothingIsKeptUntilThePersonChooses() {
        let model = model()
        #expect(model.chosenScale == nil)
        #expect(model.displayOptions == DisplayOptions())
        // A launch that states the resolution (a harness) is not a choice.
        model.launchScale = .factor(1.5)
        #expect(model.displayOptions == DisplayOptions())
    }

    @Test func aChoiceOnHomeIsKeptWhateverItIs() {
        let model = model()
        model.chooseResolution(.points)
        #expect(model.resolution == .factor(1))
        #expect(model.displayOptions == DisplayOptions(chosenScale: .points))
        model.chooseResolution(.native)
        #expect(model.resolution == .native)
        #expect(model.displayOptions == DisplayOptions(chosenScale: .native))
        model.chooseResolution(.factor(2.5))
        #expect(model.displayOptions == DisplayOptions(chosenScale: .factor(2.5)))
    }

    @Test func aKeptChoiceBeatsTheDefaultOnTheNextLaunch() {
        // A Mac (no limit) has 2.0 by default, and a kept 1.0 still wins.
        let kept = model(limit: 0)
        kept.chosenScale = .points
        #expect(kept.resolution == .factor(1))
        let small = model(limit: 2867 << 20)
        small.chosenScale = .native
        #expect(small.resolution == .native)
    }

    @Test func aPersonWhoNeverChoseGetsTheDeviceDefault() {
        // 320 dpi where the limit is above 4 GiB or there is none.
        #expect(model(native: 3, limit: 8 << 30).resolution == .factor(2))
        #expect(model(native: 3, limit: 0).resolution == .factor(2))
        #expect(model(native: 3, limit: (4 << 30) + 1).resolution == .factor(2))
        // 240 dpi at 4 GiB and under: an iPad mini 6, 2.0 native, held to 2,867 MiB.
        #expect(model(native: 2, limit: 2867 << 20).resolution == .factor(1.5))
        #expect(model(native: 3, limit: 4 << 30).resolution == .factor(1.5))
        // Native is the 2.0 of a 2.0 screen.
        #expect(model(native: 2, limit: 0).resolution == .native)
    }

    @Test func theDefaultStatesTheDensityItIsNamedFor() throws {
        let high = model(native: 3, limit: 8 << 30)
        high.displayPointsDidChange(phone)
        let wide = try #require(high.resolutionLayout)
        #expect((wide.width, wide.height, wide.dotsPerInch) == (804, 1556, 320))
        let low = model(native: 3, limit: 2867 << 20)
        low.displayPointsDidChange(phone)
        let narrow = try #require(low.resolutionLayout)
        #expect((narrow.width, narrow.height, narrow.dotsPerInch) == (603, 1167, 240))
    }

    @Test func theLaunchStatesTheResolutionWithoutKeepingIt() {
        let model = model(native: 3, limit: 8 << 30)
        model.chosenScale = .factor(2)
        model.launchScale = .points
        #expect(model.resolution == .factor(1))
        #expect(model.displayOptionsToKeep == nil)
        // What the person chose stays what is stored.
        #expect(model.displayOptions == DisplayOptions(chosenScale: .factor(2)))
        model.launchScale = nil
        #expect(model.resolution == .factor(2))
        #expect(model.displayOptionsToKeep == DisplayOptions(chosenScale: .factor(2)))
    }

    @Test func aNumberThatIsNoChoiceOfThisScreenGoesToTheNearestChoice() {
        let model = model(native: 2)
        model.chosenScale = .factor(2.5)
        #expect(model.resolution == .native)
        model.launchScale = .factor(1.4)
        #expect(model.resolution == .factor(1.5))
        // The stored number stays as it was chosen.
        #expect(model.displayOptions == DisplayOptions(chosenScale: .factor(2.5)))
    }

    @Test func homeStatesTheResolutionTheGuestWillGet() throws {
        let model = model(native: 3)
        #expect(model.resolutionLayout == nil)
        model.displayPointsDidChange(phone)
        model.chosenScale = .native
        let native = try #require(model.resolutionLayout)
        #expect((native.width, native.height, native.dotsPerInch) == (1206, 2334, 480))
        model.chooseResolution(.factor(1.5))
        let middle = try #require(model.resolutionLayout)
        #expect((middle.width, middle.height, middle.dotsPerInch) == (603, 1167, 240))
        // Each choice's own layout.
        let all = (0..<model.resolutionChoices.count).compactMap { model.resolutionLayout(at: $0)?.width }
        #expect(all == [402, 603, 804, 1005, 1206])
    }

    @Test func theStatedResolutionFollowsTheWindowAsItResizes() throws {
        let model = model(native: 2)
        model.chosenScale = .native
        model.displayPointsDidChange(CGSize(width: 834, height: 1190))
        #expect(try #require(model.resolutionLayout).width == 1668)
        model.displayPointsDidChange(CGSize(width: 600, height: 800))
        let window = try #require(model.resolutionLayout)
        #expect((window.width, window.height) == (1200, 1600))
    }

    @Test func aScreenWithOneChoiceStatesItAlone() throws {
        let model = model(native: 1)
        model.displayPointsDidChange(phone)
        #expect(!model.resolutionChoices.hasChoice)
        #expect(model.resolution == .native)
        let only = try #require(model.resolutionLayout)
        #expect((only.width, only.dotsPerInch) == (402, 160))
    }

    @Test func thePreviewStatesAScreenAndADisplayTheWindowDoesNotOverride() {
        let model = model(native: 3)
        model.statedNativeScale = 2.3
        model.statedDisplayPoints = phone
        model.screenDidChange(nativeScale: 3)
        model.displayPointsDidChange(CGSize(width: 834, height: 1190))
        #expect(model.screenNativeScale == 2.3)
        #expect(model.displayPoints == phone)
    }
}
