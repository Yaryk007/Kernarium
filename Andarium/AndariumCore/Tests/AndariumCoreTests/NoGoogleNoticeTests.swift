// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
@testable import AndariumCore

private let everyKind: [BootKind] = [.normal, .firstStart, .upgrade, .recovery, .restart]

struct NoGoogleNoticeTests {
    // MARK: The panel on the boot screen

    @Test func onlyAFirstStartShowsThePanel() {
        #expect(everyKind.filter { NoGoogleNotice.showsOnBoot($0, firstBootPending: false) } == [.firstStart])
    }

    @Test func theRetryOfAFirstBootThatDidNotCompleteShowsItToo() {
        // A normal start of a guest whose first Android boot is unfinished is that boot still; no other kind changes.
        #expect(everyKind.filter { NoGoogleNotice.showsOnBoot($0, firstBootPending: true) } == [.normal, .firstStart])
    }
}
