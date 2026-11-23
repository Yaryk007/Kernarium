// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumFollowXLogic

/// Follow developer on X's logic: which handles are handles of X, the address
/// built from one, and the reading of the build's Info.plist. The package
/// compiles the application's own source for this, with the row's condition
/// set. The handles here are made up; the build's own is never in a test.
struct FollowXTests {
    @Test func theAddressIsTheHostAndTheHandleWithNothingElse() {
        let url = FollowXProfile.url(handle: "example_handle")
        #expect(url?.absoluteString == "https://x.com/example_handle")
        #expect(url?.scheme == "https" && url?.host == "x.com")
        #expect(url?.path == "/example_handle")
        #expect(url?.query == nil && url?.fragment == nil && url?.user == nil && url?.port == nil)
    }

    @Test func aHandleIsOneToFifteenLettersDigitsAndUnderscores() {
        for handle in ["a", "A", "0", "_", "a_b_C_9", "Some_One_99", String(repeating: "z", count: 15)] {
            #expect(FollowXProfile.isWellFormed(handle: handle), "\(handle)")
            #expect(FollowXProfile.url(handle: handle) != nil, "\(handle)")
        }
    }

    @Test func nothingElseIsAHandle() {
        let refused = [
            "", String(repeating: "z", count: 16), " ", "has space", "tab\t", "new\nline", "dash-x", "dot.x",
            "@someone", "someone/", "/someone", "https://x.com/someone", "x.com/someone", "some?one", "some#one",
            "some%20one", "some:one", "some@one", "\u{00E9}cole", "\u{FF21}\u{FF22}", "emoji\u{1F600}",
            "someone ", " someone", "\u{200B}someone",
        ]
        for handle in refused {
            #expect(!FollowXProfile.isWellFormed(handle: handle), "\(handle.debugDescription)")
            #expect(FollowXProfile.url(handle: handle) == nil, "\(handle.debugDescription)")
        }
    }

    @Test func theHandleIsTheInfoPlistKeyWhenItIsWellFormed() {
        #expect(FollowXProfile.handle(in: [FollowXProfile.key: "example_handle"]) == "example_handle")
        #expect(FollowXProfile.handle(in: nil) == nil)
        #expect(FollowXProfile.handle(in: [:]) == nil)
        #expect(FollowXProfile.handle(in: [FollowXProfile.key: 7]) == nil)
        #expect(FollowXProfile.handle(in: [FollowXProfile.key: ""]) == nil)
        #expect(FollowXProfile.handle(in: [FollowXProfile.key: "https://x.com/example_handle"]) == nil)
        #expect(FollowXProfile.handle(in: ["Other": "example_handle"]) == nil)
    }

    @Test func theKeyIsTheOneTheBuildAndTheCheckUse() {
        #expect(FollowXProfile.key == "AndariumFollowXHandle")
    }
}
