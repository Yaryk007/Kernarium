// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation
import Testing

@testable import GuestHostCore

struct GuestPointerMappingTests {
    /// A 200 x 100 view drawing a square display: it is 100 wide, centred.
    private func letterboxed() throws -> GuestPointerMapping {
        try #require(GuestPointerMapping(
            boundsSize: CGSize(width: 200, height: 100),
            drawableSize: CGSize(width: 200, height: 100),
            contentSize: CGSize(width: 100, height: 100)))
    }

    @Test func theDisplayFillsTheRangeAndTheBarsAreOutside() throws {
        let mapping = try letterboxed()
        #expect(mapping.position(of: CGPoint(x: 50, y: 0)) == .init(x: 0, y: 0, inside: true))
        #expect(mapping.position(of: CGPoint(x: 100, y: 50)) == .init(x: 16384, y: 16384, inside: true))
        #expect(mapping.position(of: CGPoint(x: 150, y: 100)) == .init(x: 32767, y: 32767, inside: true))
        // A bar: the position is the nearest edge of the display.
        #expect(mapping.position(of: CGPoint(x: 10, y: 50)) == .init(x: 0, y: 16384, inside: false))
        #expect(mapping.position(of: CGPoint(x: 190, y: 120)) == .init(x: 32767, y: 32767, inside: false))
    }

    @Test func thePointsOfAViewScaleToItsDrawable() throws {
        // Retina: the drawable has twice the pixels, which changes no position.
        let mapping = try #require(GuestPointerMapping(
            boundsSize: CGSize(width: 200, height: 100),
            drawableSize: CGSize(width: 400, height: 200),
            contentSize: CGSize(width: 100, height: 100)))
        #expect(mapping.position(of: CGPoint(x: 100, y: 50)) == .init(x: 16384, y: 16384, inside: true))
        #expect(mapping.position(of: CGPoint(x: 49, y: 50))?.inside == false)
    }

    @Test func aDisplayThatIsTheViewHasNoBars() throws {
        let mapping = try #require(GuestPointerMapping(
            boundsSize: CGSize(width: 100, height: 200),
            drawableSize: CGSize(width: 100, height: 200),
            contentSize: CGSize(width: 1080, height: 2160)))
        #expect(mapping.position(of: CGPoint(x: 0, y: 0)) == .init(x: 0, y: 0, inside: true))
        #expect(mapping.position(of: CGPoint(x: 100, y: 200)) == .init(x: 32767, y: 32767, inside: true))
        #expect(mapping.position(of: CGPoint(x: 25, y: 150)) == .init(x: 8192, y: 24576, inside: true))
    }

    @Test func theDisplayFrameIsWhereThePointerIsOverTheGuest() throws {
        let mapping = try letterboxed()
        #expect(mapping.displayFrame == CGRect(x: 50, y: 0, width: 100, height: 100))
        // Retina: the drawable has twice the pixels, the frame is still in points.
        let retina = try #require(GuestPointerMapping(
            boundsSize: CGSize(width: 200, height: 100),
            drawableSize: CGSize(width: 400, height: 200),
            contentSize: CGSize(width: 100, height: 100)))
        #expect(retina.displayFrame == CGRect(x: 50, y: 0, width: 100, height: 100))
        // Every point of the frame is over the display and none beside it is.
        let frame = mapping.displayFrame
        #expect(mapping.position(of: CGPoint(x: frame.minX, y: frame.midY))?.inside == true)
        #expect(mapping.position(of: CGPoint(x: frame.maxX, y: frame.midY))?.inside == true)
        #expect(mapping.position(of: CGPoint(x: frame.minX - 1, y: frame.midY))?.inside == false)
        let full = try #require(GuestPointerMapping(
            boundsSize: CGSize(width: 100, height: 200),
            drawableSize: CGSize(width: 100, height: 200),
            contentSize: CGSize(width: 1080, height: 2160)))
        #expect(full.displayFrame == CGRect(x: 0, y: 0, width: 100, height: 200))
    }

    @Test func aGeometryOrPointThatIsNotUsableMapsNothing() {
        #expect(GuestPointerMapping(
            boundsSize: .zero, drawableSize: CGSize(width: 1, height: 1), contentSize: CGSize(width: 1, height: 1)) == nil)
        #expect(GuestPointerMapping(
            boundsSize: CGSize(width: 1, height: 1), drawableSize: CGSize(width: 1, height: 1),
            contentSize: CGSize(width: CGFloat.infinity, height: 1)) == nil)
        let mapping = GuestPointerMapping(
            boundsSize: CGSize(width: 1, height: 1), drawableSize: CGSize(width: 1, height: 1),
            contentSize: CGSize(width: 1, height: 1))
        #expect(mapping?.position(of: CGPoint(x: CGFloat.nan, y: 0)) == nil)
    }
}

struct GuestPointerTrackerTests {
    private func inside(_ x: UInt32, _ y: UInt32) -> GuestPointerMapping.Position {
        .init(x: x, y: y, inside: true)
    }

    private func outside(_ x: UInt32, _ y: UInt32) -> GuestPointerMapping.Position {
        .init(x: x, y: y, inside: false)
    }

    @Test func hoveringOverTheDisplayIsInRangeAndLeavingEndsTheContact() {
        var pointer = GuestPointerTracker()
        let enter = pointer.hover(inside(10, 20))
        #expect(enter == [PointerReport(x: 10, y: 20, buttons: 0, inRange: true)])
        #expect(pointer.inRange)
        // The same place again says nothing new.
        let same = pointer.hover(inside(10, 20))
        #expect(same.isEmpty)
        let moved = pointer.hover(inside(11, 20))
        #expect(moved.count == 1)
        // Beside the display, and then gone from the view: the contact ends
        // where the pointer was, once.
        let beside = pointer.hover(outside(32767, 20))
        #expect(beside == [PointerReport(x: 11, y: 20, buttons: 0, inRange: false)])
        let gone = pointer.hover(nil)
        #expect(gone.isEmpty)
        #expect(!pointer.isActive)
    }

    @Test func aClickIsABeginAndAnEndAtThePointer() {
        var pointer = GuestPointerTracker()
        _ = pointer.hover(inside(100, 200))
        let down = pointer.touch(.began, at: inside(100, 200), buttons: HardwareButtons.primary)
        #expect(down == [PointerReport(x: 100, y: 200, buttons: 1, inRange: true)])
        #expect(pointer.buttons == 1)
        let up = pointer.touch(.ended, at: inside(100, 200), buttons: 0)
        #expect(up == [PointerReport(x: 100, y: 200, buttons: 0, inRange: true)])
        #expect(pointer.buttons == 0)
        #expect(pointer.inRange)
    }

    @Test func aRightClickAndTheButtonsOfOneTouch() {
        var pointer = GuestPointerTracker()
        let secondary = pointer.touch(.began, at: inside(5, 5), buttons: HardwareButtons.secondary)
        #expect(secondary.first?.buttons == HardwareButtons.secondary)
        // A second button joins, and then the first lifts, in one touch.
        let both = pointer.touch(.moved, at: inside(5, 5), buttons: HardwareButtons.secondary | HardwareButtons.back)
        #expect(both.first?.buttons == 0b1010)
        let back = pointer.touch(.moved, at: inside(5, 5), buttons: HardwareButtons.back)
        #expect(back.first?.buttons == HardwareButtons.back)
    }

    @Test func aDragGoesOnPastTheEdgeAtTheEdge() {
        var pointer = GuestPointerTracker()
        _ = pointer.touch(.began, at: inside(32000, 100), buttons: HardwareButtons.primary)
        // The pointer is beside the display with the button down: it stays in
        // range at the edge, as a desktop pointer's drag does.
        let past = pointer.touch(.moved, at: outside(32767, 100), buttons: HardwareButtons.primary)
        #expect(past == [PointerReport(x: 32767, y: 100, buttons: 1, inRange: true)])
        // The button comes up outside: the contact ends there.
        let up = pointer.touch(.ended, at: outside(32767, 300), buttons: 0)
        #expect(up == [PointerReport(x: 32767, y: 300, buttons: 0, inRange: false)])
        #expect(!pointer.isActive)
    }

    @Test func aClickThatBeginsBesideTheDisplayIsNotTheGuests() {
        var pointer = GuestPointerTracker()
        let began = pointer.touch(.began, at: outside(0, 100), buttons: HardwareButtons.primary)
        let moved = pointer.touch(.moved, at: inside(50, 100), buttons: HardwareButtons.primary)
        let hover = pointer.hover(inside(60, 100))
        let ended = pointer.touch(.ended, at: inside(50, 100), buttons: 0)
        #expect(began.isEmpty)
        #expect(moved.isEmpty)
        #expect(hover.isEmpty)
        #expect(ended.isEmpty)
        // The next hover is the guest's again.
        let next = pointer.hover(inside(60, 100))
        #expect(next.count == 1)
    }

    @Test func aCancelledTouchLiftsTheButtonsAndEndsTheContact() {
        var pointer = GuestPointerTracker()
        _ = pointer.touch(.began, at: inside(7, 8), buttons: HardwareButtons.primary)
        let cancelled = pointer.touch(.cancelled, at: nil, buttons: 0)
        #expect(cancelled == [PointerReport(x: 7, y: 8, buttons: 0, inRange: false)])
        #expect(!pointer.isActive)
    }

    @Test func leavingTheFrontLiftsEverythingOnce() {
        var pointer = GuestPointerTracker()
        _ = pointer.touch(.began, at: inside(7, 8), buttons: HardwareButtons.primary | HardwareButtons.secondary)
        let lifted = pointer.leave()
        #expect(lifted == [PointerReport(x: 7, y: 8, buttons: 0, inRange: false)])
        let again = pointer.leave()
        #expect(again.isEmpty)
        // Nothing was ever reported: nothing to lift.
        var fresh = GuestPointerTracker()
        let nothing = fresh.leave()
        #expect(nothing.isEmpty)
    }

    @Test func buttonsOutsideTheFiveTheGuestHasAreDropped() {
        #expect(HardwareButtons.normalized(uiKitMask: 0b11) == 0b11)
        #expect(HardwareButtons.normalized(uiKitMask: 0b1_1111) == 0b1_1111)
        #expect(HardwareButtons.normalized(uiKitMask: 0b110_0001) == 0b1)
        #expect(HardwareButtons.normalized(uiKitMask: -1) == 0b1_1111)
        var pointer = GuestPointerTracker()
        let report = pointer.touch(.began, at: inside(1, 1), buttons: 0xFF)
        #expect(report.first?.buttons == 0b1_1111)
    }
}

struct ScrollAccumulatorTests {
    @Test func aDetentOfTranslationIsADetentOfWheel() {
        var scroll = ScrollAccumulator(pointsPerDetent: 64)
        let step = scroll.add(translationX: 0, translationY: 64)
        #expect(step.wheel == 120)
        #expect(step.horizontalWheel == 0)
    }

    @Test func downwardTranslationIsNegativeAndRightwardIsPositive() {
        var scroll = ScrollAccumulator(pointsPerDetent: 60)
        // Content moving up is a scroll down; content moving left, a scroll right.
        let step = scroll.add(translationX: -30, translationY: -30)
        #expect(step.wheel == -60)
        #expect(step.horizontalWheel == 60)
    }

    @Test func whatDoesNotMakeAWholeUnitWaitsForTheNextReport() {
        var scroll = ScrollAccumulator(pointsPerDetent: 120)
        var total: Int32 = 0
        for _ in 0..<10 {
            total += scroll.add(translationX: 0, translationY: 0.45).wheel
        }
        // 4.5 units in all: four are sent, half a unit waits.
        #expect(total == 4)
        let rest = scroll.add(translationX: 0, translationY: 0.6)
        #expect(rest.wheel == 1)
        let back = scroll.add(translationX: 0, translationY: -0.2)
        #expect(back.wheel == 0)
    }

    @Test func aWildTranslationIsBoundedAndANonNumberIsIgnored() {
        var scroll = ScrollAccumulator(pointsPerDetent: 1)
        let wild = scroll.add(translationX: 0, translationY: 1e9)
        #expect(wild.wheel == ScrollAccumulator.largestReport)
        let broken = scroll.add(translationX: .nan, translationY: 5)
        #expect(broken.wheel == 0)
        #expect(broken.horizontalWheel == 0)
        scroll.reset()
        let tiny = scroll.add(translationX: 0, translationY: 0.001)
        #expect(tiny.wheel == 0)
    }

    @Test func aGestureStartsFromNothing() {
        var scroll = ScrollAccumulator(pointsPerDetent: 120)
        _ = scroll.add(translationX: 0, translationY: 0.9)
        scroll.reset()
        let step = scroll.add(translationX: 0, translationY: 0.9)
        #expect(step.wheel == 0)
    }
}

struct RelativeMotionAccumulatorTests {
    @Test func gameControllersVerticalIsFlippedToTheGuests() {
        var motion = RelativeMotionAccumulator()
        // The pointer moved down and to the right: Game Controller reports a
        // negative vertical delta.
        let step = motion.add(gameControllerDeltaX: 12, deltaY: -7)
        #expect(step.dx == 12)
        #expect(step.dy == 7)
    }

    @Test func fractionsOfAPixelAreKept() {
        var motion = RelativeMotionAccumulator()
        var sum: Int32 = 0
        for _ in 0..<5 { sum += motion.add(gameControllerDeltaX: 0.4, deltaY: 0).dx }
        #expect(sum == 2)
        let rest = motion.add(gameControllerDeltaX: 0.4, deltaY: 0)
        #expect(rest.dx == 0)
        motion.reset()
        let fresh = motion.add(gameControllerDeltaX: 0.6, deltaY: 0)
        #expect(fresh.dx == 0)
        let broken = motion.add(gameControllerDeltaX: .infinity, deltaY: 0)
        #expect(broken.dx == 0)
        #expect(broken.dy == 0)
    }
}
