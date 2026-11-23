// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

extension String {
    /// The text with three kinds of place marked where a line must not break.
    ///
    /// A figure keeps its unit: the space between a number and a byte or
    /// vCPU unit becomes a no-break space, so `1.5` never ends one line with
    /// `GiB` starting the next. A short quoted name in Chinese or Japanese
    /// keeps its characters together: a word joiner sits between them,
    /// because those scripts break between any two characters and a name of
    /// two or three would otherwise be cut in the middle. The characters on
    /// the screen are the same; only the line breaking changes. An arrow
    /// between two quoted names (“Settings”>“Apps”) stays with the name
    /// before it, so no line starts with it. Apply it where text is shown,
    /// never to a string a test, a log or a report reads.
    public var keepingTogether: String {
        var text = self
        text = Self.unitSpace.stringByReplacingMatches(
            in: text, range: NSRange(text.startIndex..., in: text), withTemplate: "$1\u{00A0}")
        text = Self.pathArrow.stringByReplacingMatches(
            in: text, range: NSRange(text.startIndex..., in: text), withTemplate: "$1\u{2060}$2")
        for pattern in Self.quotedNames {
            let matches = pattern.matches(in: text, range: NSRange(text.startIndex..., in: text))
            for match in matches.reversed() {
                guard let range = Range(match.range(at: 1), in: text) else { continue }
                let joined = text[range].map(String.init).joined(separator: "\u{2060}")
                text.replaceSubrange(range, with: joined)
            }
        }
        return text
    }

    /// A digit, a space and a unit that is a word of its own.
    private static let unitSpace = try! NSRegularExpression(
        pattern: "([0-9]) (?=(?:[KMGT]i?B|vCPUs?)(?![A-Za-z]))")

    /// An arrow between two quoted names, right after the closing quote of
    /// the first: the greater-than sign of Chinese and the wide one of Japanese.
    private static let pathArrow = try! NSRegularExpression(
        pattern: "([\u{201D}\u{300D}])([>\u{FF1E}])")

    /// Two to four Han, Hiragana or Katakana characters between the quotes
    /// of the language: the curly pair of Simplified Chinese and Japanese,
    /// the corner brackets of Traditional Chinese.
    private static let quotedNames: [NSRegularExpression] = [
        try! NSRegularExpression(pattern: "\u{201C}([\\p{Han}\\p{Hiragana}\\p{Katakana}\u{30FC}]{2,4})\u{201D}"),
        try! NSRegularExpression(pattern: "\u{300C}([\\p{Han}\\p{Hiragana}\\p{Katakana}\u{30FC}]{2,4})\u{300D}"),
    ]
}
