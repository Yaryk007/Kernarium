// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

// The layout on the folding iPhone's displays (`FoldLayout.swift`), one test per
// pose and rotation of the Spread design. The readings are what the iPhone Duo
// Simulator reports in each pose (iOS 27.2): the cover 466 x 678 with the
// strip its camera keeps, the inner display 951 x 669 held upright (the fold
// down its middle) and 669 x 951 turned (the fold across it), the system's
// fold band 40 points wide with its margins, active while the device is bent
// and inactive flat.

/// The display and its pose, as the system reports them.
private enum Pose {
    case cover
    case coverLeadingStrip
    case upright(bent: Bool)
    case turned(bent: Bool)
}

private func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
    CGRect(x: x, y: y, width: w, height: h)
}

/// A reading in a pose, with the status bar shown (Home) or hidden (the
/// session). The cover keeps its strip either way.
private func reading(_ pose: Pose, statusBarHidden: Bool = true) -> FoldReading {
    switch pose {
    case .cover:
        // The camera, and the system's block at the strip's top once the status
        // bar is hidden; shown, the block is taller.
        return FoldReading(
            window: rect(0, 0, 466, 678), safeArea: rect(0, 0, 382, 644), hinge: .closed,
            occlusions: [rect(399.7, 29.3, 37, 37), statusBarHidden ? rect(382, 0, 84, 82) : rect(382, 0, 84, 174)])
    case .coverLeadingStrip:
        // The cover turned to landscape, where the camera is on the leading side.
        return FoldReading(
            window: rect(0, 0, 678, 466), safeArea: rect(84, 0, 594, 432), hinge: .closed,
            occlusions: [rect(29.3, 29.3, 37, 37), rect(0, 0, 84, 82)])
    case .upright(let bent):
        return FoldReading(
            window: rect(0, 0, 951, 669), safeArea: statusBarHidden ? rect(0, 0, 951, 635) : rect(0, 0, 867, 635),
            hinge: bent ? .partiallyOpen : .fullyOpen,
            division: .init(frame: rect(455.5, 0, 40, 669), isActive: bent),
            occlusions: statusBarHidden ? [] : [rect(867, 0, 84, 124)])
    case .turned(let bent):
        return FoldReading(
            window: rect(0, 0, 669, 951), safeArea: statusBarHidden ? rect(0, 0, 669, 917) : rect(0, 82, 669, 835),
            hinge: bent ? .partiallyOpen : .fullyOpen,
            division: .init(frame: rect(0, 455.5, 669, 40), isActive: bent),
            occlusions: statusBarHidden ? [] : [rect(525, 0, 144, 82)])
    }
}

private let plainPhone = FoldReading.plain(window: rect(0, 0, 402, 874), safeArea: rect(0, 62, 402, 778))
private let plainPad = FoldReading.plain(window: rect(0, 0, 834, 1194), safeArea: rect(0, 24, 834, 1146))

struct FoldPoseTests {
    @Test func aDisplayThatReportsNoHingeIsPlain() {
        #expect(plainPhone.pose == .plain)
        #expect(plainPad.pose == .plain)
        #expect(!plainPhone.pose.folds)
        #expect(SessionPlan(plainPhone) == nil)
        #expect(SessionPlan(plainPad) == nil)
        #expect(FoldHalves(plainPhone) == nil)
        #expect(HomePages(plainPhone, frame: plainPhone.safeArea) == nil)
        #expect(FoldSheet.detentHeight(plainPhone) == nil)
    }

    @Test func onlyTheInnerDisplayBentAtTheFoldIsPlacedAroundTheHinge() {
        // The one decision: flat, upright or turned, every screen is laid out
        // as on an iPad; the cover and a display with no hinge have no spread.
        #expect(reading(.upright(bent: true)).pose.spreads)
        #expect(reading(.turned(bent: true)).pose.spreads)
        #expect(!reading(.upright(bent: false)).pose.spreads)
        #expect(!reading(.turned(bent: false)).pose.spreads)
        #expect(!reading(.cover).pose.spreads)
        #expect(!reading(.coverLeadingStrip).pose.spreads)
        #expect(!plainPhone.pose.spreads)
        #expect(!plainPad.pose.spreads)
    }

    @Test func flatTheInnerDisplayHasNoHingeAwareLayoutAtAllAsOnAnIPad() throws {
        // Every layout that is decided from the hinge answers as it does on a
        // display with no hinge, in both holding directions.
        for pose in [Pose.upright(bent: false), .turned(bent: false)] {
            let read = reading(pose, statusBarHidden: false)
            #expect(HomePages(read, frame: read.safeArea) == nil, "\(pose)")
            #expect(FoldSheet.detentHeight(read) == nil, "\(pose)")
            let plan = try #require(SessionPlan(reading(pose)))
            #expect(plan.keys == .edge && plan.hud == plan.guest, "\(pose)")
            #expect(FoldHalves(read) == nil, "\(pose)")
        }
        // Bent, the same asks answer with the fold.
        for pose in [Pose.upright(bent: true), .turned(bent: true)] {
            let read = reading(pose, statusBarHidden: false)
            #expect(HomePages(read, frame: read.safeArea) != nil, "\(pose)")
            #expect(FoldHalves(read) != nil, "\(pose)")
        }
        #expect(FoldSheet.detentHeight(reading(.turned(bent: true))) != nil)
        // The cover and a display with no hinge have no halves either.
        #expect(FoldHalves(reading(.cover)) == nil)
        #expect(FoldHalves(plainPad) == nil)
    }

    @Test func theCoverIsAHingeWithNoFoldBand() {
        #expect(reading(.cover).pose == .cover)
        #expect(reading(.coverLeadingStrip).pose == .cover)
        #expect(!reading(.cover).pose.isInner)
    }

    @Test func theInnerDisplayIsToldByItsFoldBandWhateverTheHingeStatusSays() {
        #expect(reading(.upright(bent: true)).pose == .upright(bent: true))
        #expect(reading(.upright(bent: false)).pose == .upright(bent: false))
        #expect(reading(.turned(bent: true)).pose == .turned(bent: true))
        #expect(reading(.turned(bent: false)).pose == .turned(bent: false))
        var odd = reading(.upright(bent: true))
        odd.hinge = .closed
        #expect(odd.pose == .upright(bent: true))
        for pose in [FoldPose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            #expect(pose.isInner && pose.folds)
        }
    }

    @Test func aBandWiderThanTallRunsAcrossAndOneTallerThanWideDown() {
        #expect(reading(.upright(bent: true)).division!.frame.height > reading(.upright(bent: true)).division!.frame.width)
        #expect(reading(.turned(bent: true)).division!.frame.width > reading(.turned(bent: true)).division!.frame.height)
    }

    @Test func theSessionsSafeAreaKeepsTheHomeIndicatorAndLosesTheStatusBar() {
        // Home and the session read different safe areas in the same pose, and
        // the next start's display must be the session's.
        for pose in [Pose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            let home = reading(pose, statusBarHidden: false)
            let session = reading(pose, statusBarHidden: true)
            #expect(home.safeArea != session.safeArea)
            #expect(home.sessionSafeArea == session.safeArea)
            #expect(session.sessionSafeArea == session.safeArea)
        }
        #expect(reading(.upright(bent: true)).sessionSafeArea == rect(0, 0, 951, 635))
        #expect(reading(.turned(bent: true)).sessionSafeArea == rect(0, 0, 669, 917))
    }

    @Test func theCoversSessionSafeAreaKeepsTheCamerasStrip() {
        #expect(reading(.cover).sessionSafeArea == rect(0, 0, 382, 644))
        #expect(reading(.cover, statusBarHidden: false).sessionSafeArea == rect(0, 0, 382, 644))
    }
}

struct SessionPlanTests {
    private func plan(_ pose: Pose, statusBarHidden: Bool = true) throws -> SessionPlan {
        try #require(SessionPlan(reading(pose, statusBarHidden: statusBarHidden)))
    }

    // MARK: Cover

    @Test func theCoverKeepsTodaysPhoneAndPutsTheKeysInTheStrip() throws {
        let plan = try plan(.cover)
        #expect(plan.guest == rect(0, 0, 382, 644))
        // The strip below the system's block at its top, to the home
        // indicator's edge: 84 x 558 points, as the design draws it.
        #expect(plan.keys == .rail(rect(382, 86, 84, 558)))
        // Notices hang from the guest's top.
        #expect(plan.hud == plan.guest)
    }

    @Test func theRailStartsBelowWhateverTheSystemKeepsAtTheStripsTop() throws {
        // With the status bar shown the system's block is 174 points tall.
        let plan = try plan(.cover, statusBarHidden: false)
        #expect(plan.keys == .rail(rect(382, 178, 84, 466)))
        #expect(plan.guest == rect(0, 0, 382, 644))
    }

    @Test func aStripOnTheLeadingSideHoldsTheRailThere() throws {
        let plan = try plan(.coverLeadingStrip)
        #expect(plan.guest == rect(84, 0, 594, 432))
        #expect(plan.keys == .rail(rect(0, 86, 84, 346)))
    }

    @Test func aCoverWithNoStripHasNoRail() throws {
        var bare = reading(.cover)
        bare.safeArea = bare.window
        bare.occlusions = []
        let plan = try #require(SessionPlan(bare))
        #expect(plan.guest == bare.window)
        #expect(plan.keys == .edge)
    }

    // MARK: Inner, upright

    @Test(arguments: [true, false])
    func heldUprightTheGuestSpansBothHalvesBentOrFlat(bent: Bool) throws {
        let plan = try plan(.upright(bent: bent))
        #expect(plan.guest == rect(0, 0, 951, 635))
        #expect(plan.keys == .edge)
    }

    @Test func heldUprightTheStatisticsHangFromTheTrailingHalfWhileBentAndFromTheTopWhenFlat() throws {
        // Bent, beside the keys and clear of the fold's band and its margins;
        // flat, from the top of the display as on an iPad.
        #expect(try plan(.upright(bent: true)).hud == rect(495.5, 0, 455.5, 635))
        #expect(try plan(.upright(bent: false)).hud == rect(0, 0, 951, 635))
    }

    @Test func bendingTheHingeHeldUprightNeverResizesTheGuest() throws {
        #expect(try plan(.upright(bent: true)).guest == plan(.upright(bent: false)).guest)
        #expect(!SessionPlan.resizesGuest(from: try plan(.upright(bent: true)), to: try plan(.upright(bent: false))))
    }

    @Test func theUprightPlanIsTheSameWhenHomeReadItWithTheStatusBarShown() throws {
        #expect(try plan(.upright(bent: true), statusBarHidden: false) == plan(.upright(bent: true)))
    }

    // MARK: Inner, turned

    @Test func turnedAndFlatTheSessionIsLaidOutAsOnAnIPad() throws {
        let plan = try plan(.turned(bent: false))
        #expect(plan.guest == rect(0, 0, 669, 917))
        #expect(plan.keys == .edge)
        #expect(plan.hud == plan.guest)
    }

    @Test func standingTheGuestIsTheUpperHalfAndTheLowerHalfIsTheDeck() throws {
        let plan = try plan(.turned(bent: true))
        #expect(plan.guest == rect(0, 0, 669, 455.5))
        #expect(plan.keys == .deck(rect(0, 495.5, 669, 421.5)))
        #expect(plan.hud == rect(0, 495.5, 669, 421.5))
    }

    // MARK: What stays clear of the fold

    @Test func whileBentNothingOfAndariumsOnTheInnerDisplayTouchesTheFoldBand() throws {
        for pose in [Pose.upright(bent: true), .turned(bent: true)] {
            let reading = reading(pose)
            let band = try #require(reading.division).frame
            let plan = try #require(SessionPlan(reading))
            // The statistics and the notices.
            #expect(!plan.hud.intersects(band), "\(pose)")
            // The deck, and the guest above it when standing.
            if case .deck(let deck) = plan.keys {
                #expect(!deck.intersects(band), "\(pose)")
                #expect(!plan.guest.intersects(band), "\(pose)")
            }
        }
    }

    @Test func flatNothingOfTheSessionIsPlacedByTheFoldAsOnAnIPad() throws {
        for pose in [Pose.upright(bent: false), .turned(bent: false)] {
            let plan = try #require(SessionPlan(reading(pose)))
            #expect(plan.keys == .edge, "\(pose)")
            #expect(plan.hud == plan.guest, "\(pose)")
        }
    }

    // MARK: Which changes resize Android

    @Test func theGuestIsResizedWhereItsShapeChangesAndNowhereElse() throws {
        let cover = try plan(.cover)
        let upBook = try plan(.upright(bent: true))
        let upFlat = try plan(.upright(bent: false))
        let turnedFlat = try plan(.turned(bent: false))
        let standing = try plan(.turned(bent: true))
        // The fold and the unfold.
        #expect(SessionPlan.resizesGuest(from: cover, to: upBook))
        #expect(SessionPlan.resizesGuest(from: upFlat, to: cover))
        #expect(SessionPlan.resizesGuest(from: cover, to: turnedFlat))
        #expect(SessionPlan.resizesGuest(from: standing, to: cover))
        // Held upright the hinge moves nothing.
        #expect(!SessionPlan.resizesGuest(from: upBook, to: upFlat))
        #expect(!SessionPlan.resizesGuest(from: upFlat, to: upBook))
        // The turn.
        #expect(SessionPlan.resizesGuest(from: upBook, to: standing))
        #expect(SessionPlan.resizesGuest(from: upFlat, to: turnedFlat))
        // Standing and flat, turned: the one hinge move that resizes Android.
        #expect(SessionPlan.resizesGuest(from: turnedFlat, to: standing))
        #expect(SessionPlan.resizesGuest(from: standing, to: turnedFlat))
        // The same shape again.
        #expect(!SessionPlan.resizesGuest(from: cover, to: cover))
        #expect(!SessionPlan.resizesGuest(from: standing, to: standing))
        // A display that does not fold is not part of a transition.
        #expect(!SessionPlan.resizesGuest(from: nil, to: nil))
        #expect(SessionPlan.resizesGuest(from: nil, to: cover))
    }

    @Test func theGuestsSizeInEachPoseIsWhatAndroidIsToldAt160DotsAPoint() throws {
        // Android sees sw382, sw951 (635 tall) and sw669 (917 or 455.5 tall).
        #expect(try plan(.cover).guest.size == CGSize(width: 382, height: 644))
        #expect(try plan(.upright(bent: true)).guest.size == CGSize(width: 951, height: 635))
        #expect(try plan(.turned(bent: false)).guest.size == CGSize(width: 669, height: 917))
        #expect(try plan(.turned(bent: true)).guest.size == CGSize(width: 669, height: 455.5))
    }
}

struct FoldScreensTests {
    @Test func homeHasOnePageEachSideOfTheFoldUprightWhileBent() throws {
        // Home lays out in the window less the status bar's safe area.
        let reading = reading(.upright(bent: true), statusBarHidden: false)
        let pages = try #require(HomePages(reading, frame: reading.safeArea))
        #expect(pages.upright)
        #expect(pages.first == 455.5)
        #expect(pages.gap == 40)
        #expect(pages.second == 371.5)
        #expect(pages == HomePages(reading, frame: reading.safeArea))
    }

    @Test func homeHasAnUpperAndALowerPageTurnedWhileBent() throws {
        let reading = reading(.turned(bent: true), statusBarHidden: false)
        let pages = try #require(HomePages(reading, frame: reading.safeArea))
        #expect(!pages.upright)
        #expect(pages.first == 373.5)
        #expect(pages.gap == 40)
        #expect(pages.second == 421.5)
    }

    @Test func flatHomeIsLaidOutAsOnAnIPadInBothDirections() {
        for pose in [Pose.upright(bent: false), .turned(bent: false)] {
            let reading = reading(pose, statusBarHidden: false)
            #expect(HomePages(reading, frame: reading.safeArea) == nil, "\(pose)")
        }
    }

    @Test func homeKeepsItsColumnOnTheCoverAndOnADisplayThatDoesNotFold() {
        let cover = reading(.cover, statusBarHidden: false)
        #expect(HomePages(cover, frame: cover.safeArea) == nil)
        #expect(HomePages(plainPad, frame: plainPad.safeArea) == nil)
    }

    @Test func aPageTooSmallIsNoPage() {
        let reading = reading(.upright(bent: true), statusBarHidden: false)
        #expect(HomePages(reading, frame: rect(0, 0, 560, 635)) == nil)
    }

    @Test func overlaysSitOnTheHalfWhereTheSystemPutsItsAlertsWhileBent() throws {
        // Leading when upright, lower when turned.
        let upright = try #require(FoldHalves(reading(.upright(bent: true), statusBarHidden: false)))
        #expect(upright.card == rect(0, 0, 455.5, 635))
        #expect(upright.second == rect(495.5, 0, 371.5, 635))
        let turned = try #require(FoldHalves(reading(.turned(bent: true), statusBarHidden: false)))
        #expect(turned.card == rect(0, 495.5, 669, 421.5))
        #expect(turned.first == rect(0, 82, 669, 373.5))
    }

    @Test func bentWelcomeSitsOnAHalfAndACardIsNeverPaddedOutward() throws {
        let book = try #require(FoldHalves(reading(.upright(bent: true), statusBarHidden: false)))
        #expect(book.area(for: .welcome) == book.card)
        let standing = try #require(FoldHalves(reading(.turned(bent: true), statusBarHidden: false)))
        #expect(standing.area(for: .welcome) == standing.card)
        // A navigation bar takes the top 60 points of the space: a view keeps
        // to its area, and is never padded outward.
        let space = CGRect(x: 0, y: 60, width: 867, height: 575)
        #expect(book.insets(for: .card, in: space).trailing == space.maxX - book.card.maxX)
        let around = book.insets(for: .welcome, in: space)
        #expect(around.top >= 0 && around.leading >= 0 && around.bottom >= 0 && around.trailing >= 0)
        // A space of the half's own leaves no padding.
        #expect(book.insets(for: .card, in: book.card) == FoldInsets())
    }

    @Test func theBootSitsOnTheUpperHalfWhenStandingAndOnTheAlertsHalfWhileBentUpright() throws {
        let standing = try #require(FoldHalves(reading(.turned(bent: true), statusBarHidden: false)))
        #expect(standing.area(for: .boot) == rect(0, 82, 669, 373.5))
        #expect(standing.area(for: .boot) == standing.first)
        let book = try #require(FoldHalves(reading(.upright(bent: true), statusBarHidden: false)))
        #expect(book.area(for: .boot) == book.card)
    }

    @Test func aCardKeepsTheHalfWhereTheSystemPutsItsAlertsWhileBent() throws {
        for pose in [Pose.upright(bent: true), .turned(bent: true)] {
            let halves = try #require(FoldHalves(reading(pose, statusBarHidden: false)))
            #expect(halves.area(for: .card) == halves.card)
            #expect(halves.area(for: .welcome) == halves.card)
        }
    }

    @Test func noPlaceOfAScreenCrossesTheFoldBandWhileBent() throws {
        for pose in [Pose.upright(bent: true), .turned(bent: true)] {
            let read = reading(pose, statusBarHidden: false)
            let band = try #require(read.division?.frame)
            let halves = try #require(FoldHalves(read))
            for screen in [FoldScreen.card, .boot, .welcome] {
                #expect(!halves.area(for: screen).intersects(band), "\(screen) bent \(pose)")
            }
        }
    }

    @Test func theFileBrowsersImportIsInTheToolbarOnTheCoverAlone() {
        #expect(FoldPose.cover.importIsInToolbar)
        for pose in [FoldPose.plain, .upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            #expect(!pose.importIsInToolbar)
        }
    }

    @Test func aSheetTurnedAndBentRisesToTwelvePointsBelowTheFoldAndStops() {
        for hidden in [true, false] {
            let reading = reading(.turned(bent: true), statusBarHidden: hidden)
            // The safe area ends at 917 whether or not the status bar shows.
            #expect(FoldSheet.detentHeight(reading) == 917 - 495.5 - 12)
        }
    }

    @Test func aSheetKeepsTheSystemsPlacementOnTheCoverUprightAndWhileFlat() {
        #expect(FoldSheet.detentHeight(reading(.cover)) == nil)
        #expect(FoldSheet.detentHeight(reading(.upright(bent: true))) == nil)
        #expect(FoldSheet.detentHeight(reading(.upright(bent: false))) == nil)
        #expect(FoldSheet.detentHeight(reading(.turned(bent: false))) == nil)
    }

    @Test func aSheetNeverShrinksBelowTwoHundredPoints() {
        var small = reading(.turned(bent: true))
        small.safeArea = rect(0, 0, 669, 600)
        #expect(FoldSheet.detentHeight(small) == 200)
    }
}

struct FoldRotationTests {
    @Test func theInnerDisplayTurnsAsAnIPadDoesAndTheCoverAsAnIPhoneDoes() {
        // An iPad turns.
        #expect(FoldPose.plain.interfaceRotates(idiomIsPhone: false))
        // An iPhone does not, and neither does the cover.
        #expect(!FoldPose.plain.interfaceRotates(idiomIsPhone: true))
        #expect(!FoldPose.cover.interfaceRotates(idiomIsPhone: true))
        // The inner display turns in every pose, though the device is a phone.
        for pose in [FoldPose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            #expect(pose.interfaceRotates(idiomIsPhone: true), "\(pose)")
        }
    }

    @Test func theHostDrivesTheGuestInASessionOnTheInnerDisplayAndNotOnTheCover() {
        let sessionPhases: [Phase] = [.booting(.normal), .running(.normal), .running(.recovery)]
        for phase in sessionPhases {
            let inner = SessionOrientationPolicy(
                phase: phase, interfaceRotates: FoldPose.upright(bent: true).interfaceRotates(idiomIsPhone: true))
            #expect(inner.hostDrivesGuestRotation, "\(phase)")
            let cover = SessionOrientationPolicy(
                phase: phase, interfaceRotates: FoldPose.cover.interfaceRotates(idiomIsPhone: true))
            #expect(!cover.hostDrivesGuestRotation, "\(phase)")
        }
        for phase in [Phase.home, .failed] {
            let inner = SessionOrientationPolicy(
                phase: phase, interfaceRotates: FoldPose.turned(bent: false).interfaceRotates(idiomIsPhone: true))
            #expect(!inner.hostDrivesGuestRotation, "\(phase)")
        }
    }

    @Test func movingBetweenTheCoverAndTheInnerDisplayIsAChangeTheSceneRootActsOn() {
        final class Token {}
        let scene = Token(), driver = Token()
        func inputs(rotates: Bool) -> SceneUpdateInputs {
            SceneUpdateInputs(
                scene: ObjectIdentifier(scene), driver: ObjectIdentifier(driver),
                sceneSize: CGSize(width: 466, height: 678), screenSize: CGSize(width: 466, height: 678),
                interfaceOrientation: 1, phase: .running(.normal), interfaceRotates: rotates)
        }
        #expect(inputs(rotates: true) != inputs(rotates: false))
        #expect(inputs(rotates: true) == inputs(rotates: true))
    }
}
