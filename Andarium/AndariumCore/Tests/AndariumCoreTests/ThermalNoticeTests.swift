// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

/// The notice's state: what the screen shows once the policy has decided.
struct ThermalNoticeTests {
    @Test func aNoticeShowsAndIsDismissed() {
        var notice = ThermalNotice()
        #expect(!notice.isShowing)
        notice.show()
        #expect(notice.isShowing)
        notice.dismiss()
        #expect(!notice.isShowing)
    }

    @Test func theNoticeIsOneShortSentenceWithNoProductName() {
        let text = ThermalNotice.text
        #expect(text.count <= 60)
        #expect(!text.contains("."))
        for name in ["Google", "Pixel", "Gemini", "Chrome"] { #expect(!text.contains(name)) }
    }

    @Test func theNoticeStaysThirtySecondsLikeTheGraphicsNotice() {
        #expect(ThermalNotice.visibleFor == GraphicsNotice.visibleFor)
    }
}
