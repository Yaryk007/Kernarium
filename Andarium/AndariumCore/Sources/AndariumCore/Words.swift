// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Counts as the screens say them, in the person's language. A plural is a
/// variation of the one key in the catalog, so each language has its own
/// forms.
public enum CountWords {
    /// "1 vCPU", "6 vCPUs".
    public static func vcpus(_ count: Int) -> String {
        String(localized: "\(count) vCPUs", bundle: .module)
    }
}

/// Whole sentences one after another. A text that says one more thing only
/// when it applies is its sentences joined, not one string with a hole in
/// it, so each sentence is translated whole. English separates sentences with
/// a space; Chinese and Japanese write them with none.
public enum Sentences {
    public static func join(_ sentences: String...) -> String { join(sentences) }

    public static func join(_ sentences: [String]) -> String {
        sentences.reduce("") { text, sentence in
            text.isEmpty
                ? sentence
                : String(
                    localized: "sentence.join", defaultValue: "\(text) \(sentence)", bundle: .module,
                    comment: "Joins two whole sentences: a space in English, none in Chinese and Japanese.")
        }
    }
}

/// Parts of what VoiceOver reads, one after another, as the language lists them.
public enum ListWords {
    public static func join(_ parts: [String]) -> String {
        parts.reduce("") { text, part in
            text.isEmpty
                ? part
                : String(
                    localized: "list.join", defaultValue: "\(text), \(part)", bundle: .module,
                    comment: "Joins two parts of a spoken list: a comma and a space in English, the language's own comma in Chinese and Japanese.")
        }
    }
}
