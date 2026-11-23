// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
import UIKit
@testable import Andarium

/// How the guest's display presents itself to VoiceOver. With Android's screen
/// reader off it is one element, as it always was. With it on, a touch must
/// reach the guest with VoiceOver silent, which only a child element of the
/// display does when it carries the direct-touch trait and `silentOnTouch`:
/// the display is then a container of one element that covers it, and the
/// texts are the child's. The Simulator has no VoiceOver, so this reads the
/// structure and not the speech.
@MainActor
struct GuestDisplayAccessibilityTests {
    private let words = GuestDisplayAccessibility(
        label: "Android screen", value: "Android\u{2019}s screen reader on", hint: "Touches go to Android.")

    private func display(width: CGFloat = 400, height: CGFloat = 300) -> GuestMetalView {
        GuestMetalView(frame: CGRect(x: 0, y: 0, width: width, height: height), device: nil)
    }

    private func direct(_ takesFocus: Bool = false) -> GuestDisplayAccessibility {
        var accessibility = words
        accessibility.directTouch = true
        accessibility.takesFocus = takesFocus
        return accessibility
    }

    @Test func withTheScreenReaderOffTheDisplayIsOneElement() {
        let view = display()
        view.applyAccessibility(words)
        #expect(view.isAccessibilityElement)
        #expect(view.accessibilityElements == nil)
        #expect(view.accessibilityLabel == words.label)
        #expect(view.accessibilityValue == words.value)
        #expect(view.accessibilityHint == words.hint)
        #expect(view.accessibilityTraits == .none)
    }

    @Test func withoutDescriptionTheDisplayIsNoElement() {
        let view = display()
        view.applyAccessibility(direct())
        view.applyAccessibility(nil)
        #expect(!view.isAccessibilityElement)
        #expect(view.accessibilityElements == nil)
        #expect(view.accessibilityLabel == nil)
    }

    @Test func withTheScreenReaderOnTheDisplayIsAContainerOfOneDirectTouchElement() throws {
        let view = display()
        view.applyAccessibility(direct())
        #expect(!view.isAccessibilityElement)
        let elements = try #require(view.accessibilityElements)
        #expect(elements.count == 1)
        let element = try #require(elements.first as? UIAccessibilityElement)
        #expect(element.accessibilityContainer as? UIView === view)
        #expect(element.accessibilityTraits == .allowsDirectInteraction)
        #expect(element.accessibilityDirectTouchOptions == .silentOnTouch)
        #expect(element.accessibilityDirectTouchOptions.contains(.silentOnTouch))
        #expect(!element.accessibilityDirectTouchOptions.contains(.requiresActivation))
    }

    @Test func theTextsAreTheElementsAndTheContainerHasNone() throws {
        let view = display()
        view.applyAccessibility(direct())
        let element = try #require(view.accessibilityElements?.first as? UIAccessibilityElement)
        #expect(element.accessibilityLabel == words.label)
        #expect(element.accessibilityValue == words.value)
        #expect(element.accessibilityHint == words.hint)
        #expect(view.accessibilityLabel == nil)
        #expect(view.accessibilityValue == nil)
        #expect(view.accessibilityHint == nil)
        #expect(view.accessibilityTraits == .none)
    }

    @Test func theElementCoversTheDisplayAndFollowsItsLayout() throws {
        let view = display(width: 400, height: 300)
        view.applyAccessibility(direct())
        let element = try #require(view.accessibilityElements?.first as? UIAccessibilityElement)
        #expect(element.accessibilityFrameInContainerSpace == view.bounds)
        // A rotation, a window resize or a new resolution scale gives the view
        // other bounds; the element follows them with the layout.
        view.frame = CGRect(x: 0, y: 0, width: 300, height: 520)
        view.layoutIfNeeded()
        #expect(element.accessibilityFrameInContainerSpace == CGRect(x: 0, y: 0, width: 300, height: 520))
    }

    @Test func theSameElementServesEveryUpdateWhileTheScreenReaderStaysOn() throws {
        let view = display()
        view.applyAccessibility(direct())
        let first = try #require(view.accessibilityElements?.first as? UIAccessibilityElement)
        var changed = direct()
        changed.value = "Turning off Android\u{2019}s screen reader\u{2026}"
        view.applyAccessibility(changed)
        let second = try #require(view.accessibilityElements?.first as? UIAccessibilityElement)
        #expect(first === second)
        #expect(second.accessibilityValue == changed.value)
        #expect(view.accessibilityElements?.count == 1)
    }

    @Test func turningTheScreenReaderOffGivesTheDisplayBackAsOneElement() throws {
        let view = display()
        view.applyAccessibility(direct())
        view.applyAccessibility(words)
        #expect(view.isAccessibilityElement)
        #expect(view.accessibilityElements == nil)
        #expect(view.accessibilityLabel == words.label)
        #expect(view.accessibilityTraits == .none)
        // And on again: a new element, not a stale one.
        view.applyAccessibility(direct())
        #expect(!view.isAccessibilityElement)
        #expect(view.accessibilityElements?.count == 1)
    }

    @Test func activatingTheElementSendsTheGuestNoTouch() throws {
        let view = display()
        view.applyAccessibility(direct())
        let element = try #require(view.accessibilityElements?.first as? UIAccessibilityElement)
        // Handled: the default would tap the guest at the activation point.
        #expect(element.accessibilityActivate())
    }
}
