// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// Where a line must not end: after a number, before its unit, and inside a
/// short quoted name in Chinese or Japanese.
@Suite struct LineBreaksTests {
    @Test func aFigureKeepsItsUnit() {
        #expect("Android needs 1.5 GiB free.".keepingTogether == "Android needs 1.5\u{00A0}GiB free.")
        #expect("820 MiB, 64 KiB and 2 TB".keepingTogether == "820\u{00A0}MiB, 64\u{00A0}KiB and 2\u{00A0}TB")
        #expect("6 vCPUs".keepingTogether == "6\u{00A0}vCPUs")
        #expect("1 vCPU".keepingTogether == "1\u{00A0}vCPU")
    }

    @Test func aSpaceThatIsNotBetweenANumberAndItsUnitStays() {
        #expect("About GiB of space".keepingTogether == "About GiB of space")
        #expect("2 gibberish".keepingTogether == "2 gibberish")
        #expect("2 GiBs".keepingTogether == "2 GiBs")
        #expect("room 4 and more".keepingTogether == "room 4 and more")
    }

    @Test func aShortQuotedNameKeepsItsCharactersTogether() {
        #expect("请在“设置”中打开".keepingTogether == "请在“设\u{2060}置”中打开")
        #expect("「啟動」會重試".keepingTogether == "「啟\u{2060}動」會重試")
        #expect("“アップデートして起動”".keepingTogether == "“アップデートして起動”")
        #expect("“位置情報”をオン".keepingTogether == "“位\u{2060}置\u{2060}情\u{2060}報”をオン")
        #expect("“マイク”".keepingTogether == "“マ\u{2060}イ\u{2060}ク”")
    }

    @Test func aPathArrowStaysWithTheNameBeforeIt() {
        #expect("在“设置”>“App”中".keepingTogether == "在“设\u{2060}置”\u{2060}>“App”中")
        #expect("「設定」>「郵件」".keepingTogether == "「設\u{2060}定」\u{2060}>「郵\u{2060}件」")
        #expect("“設定”＞“アプリ”".keepingTogether == "“設\u{2060}定”\u{2060}＞“ア\u{2060}プ\u{2060}リ”")
        #expect("Settings > Apps and “Files” > x".keepingTogether == "Settings > Apps and “Files” > x")
        #expect("a > b".keepingTogether == "a > b")
    }

    @Test func otherQuotedTextIsLeftAlone() {
        #expect("“Files”".keepingTogether == "“Files”")
        #expect("“iPhone储存空间”".keepingTogether == "“iPhone储存空间”")
        #expect("“一”".keepingTogether == "“一”")
        #expect("“设置页面的名称”".keepingTogether == "“设置页面的名称”")
        #expect("plain text".keepingTogether == "plain text")
    }

    @Test func markingTwiceChangesNothingMore() {
        let text = "请在“设置”>“App”中释放1.5 GiB，或在「設定」中。"
        #expect(text.keepingTogether.keepingTogether == text.keepingTogether)
    }

    @Test func nothingVisibleChanges() {
        let text = "此装置仅剩820 MiB可用空间。请在“设置”中释放空间。"
        let marked = text.keepingTogether
        #expect(marked != text)
        let plain = marked.replacingOccurrences(of: "\u{00A0}", with: " ").replacingOccurrences(of: "\u{2060}", with: "")
        #expect(plain == text)
    }
}
