// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import GuestHostCore
import Testing
@testable import Andarium

/// What VoiceOver says as a screen appears is the text the screen shows.
@MainActor
struct SpokenTextTests {
    /// The words are `SpacePauseWords`', which a language translates; the
    /// runtime's English `title` and `detail` are not what the screen shows. In
    /// English the two read alike, so the other languages are where a speech
    /// that took the runtime's text would differ.
    @Test(arguments: ["en", "ja", "zh-Hans", "zh-Hant"])
    func theLowSpaceScreenSpeaksTheWordsItShows(language: String) {
        let locale = Locale(identifier: language)
        for stage in [SpacePause.Stage.pausing, .paused, .notPaused] {
            let pause = SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: stage)
            let shown = "\(SpacePauseWords.title(pause, locale: locale)). \(SpacePauseWords.detail(pause, locale: locale))"
            #expect(LowSpaceOverlay.spoken(pause, locale: locale) == shown, "\(language) \(stage)")
            if language != "en" {
                #expect(shown != "\(pause.title). \(pause.detail)", "\(language) \(stage) is still English")
            }
        }
    }

    @Test func aGuestThatOnlyStopsKeepsTheRuntimesDetail() {
        let pause = SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .paused, ending: .stop)
        #expect(LowSpaceOverlay.spoken(pause).hasSuffix(pause.detail))
    }

    @Test func theBootsElapsedTimeIsSpokenAsWords() {
        #expect(BootView.spokenElapsed(65) == Duration.seconds(65).formatted(.units(allowed: [.hours, .minutes, .seconds], width: .wide)))
        #expect(BootView.spokenElapsed(-3) == BootView.spokenElapsed(0))
    }
}
