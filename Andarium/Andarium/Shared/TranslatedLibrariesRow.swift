// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words of Home's AOT row.
enum TranslatedLibrariesWords {
    /// The row's title. AOT is a name and stays as it is, as JIT does; the note
    /// says what it is.
    static let title = "AOT"

    /// The line beneath the group while the switch works: that the libraries
    /// run faster, and when to turn it off.
    static let note = String(
        localized: "Translated system libraries run faster. Turn off only to troubleshoot.",
        comment: "The note beneath the AOT group on Home, in the footer style. AOT is a name and stays as it is. Says that the translated system libraries (Android's own libraries, run from machine code translated ahead of time) make Android faster, and that the switch is only for trouble-shooting.")

    /// The line beneath the AOT group: the note, or while the next start runs
    /// on the JIT engine, which does not use them, why the switch is dimmed.
    static func line(unused: Bool) -> String { unused ? notUsedWithJit : note }

    /// Beneath the group while the next start runs on the JIT engine, which
    /// does not use them: the switch is dimmed and changes nothing.
    static let notUsedWithJit = String(
        localized: "Not used while JIT is on.", table: "JIT",
        comment: "Under the AOT switch on Home while JIT is on and ready for the next start. AOT and JIT are names and stay as they are.")
}

/// Whether Android's system libraries run from the code translated from them
/// ahead of time (`AppModel.translatedLibrariesOn`), the second group of Home's
/// Performance section, between "120 Hz" and JIT. It is drawn only in a build
/// that carries the translated-code framework, and applies when Android next
/// starts: Home shows only while no guest runs. The cell is the icon, the title
/// and the switch; the note is beneath the group (`TranslatedLibrariesNote`).
/// While the next start runs on the JIT engine, which does not use the
/// translated code, the row is dimmed, its switch cannot be changed, and the
/// line beneath the group says so.
struct TranslatedLibrariesRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let unused = model.jitInUseForNextStart
        let title = TranslatedLibrariesWords.title
        let spoken: String = "\(title), \(TranslatedLibrariesWords.line(unused: unused))"
        OptionRow(title: title, symbol: "books.vertical.fill", dimmed: unused) {
            Toggle(isOn: Binding(
                get: { model.translatedLibrariesOn },
                set: { model.setTranslatedLibraries($0) }
            )) { Text(verbatim: title) }
            .labelsHidden()
            .disabled(unused)
            .accessibilityIdentifier("home-aot-switch")
        }
        // VoiceOver says the name first and then the line beneath the group (the
        // note, or why the switch is dimmed), as a label, so that it is heard
        // with hints off; the switch's own state follows. The label is a
        // String, so that no catalog key is extracted from it.
        .accessibilityLabel(spoken)
        // Voice Control matches the name alone, as it is on the screen.
        .accessibilityInputLabels([title])
        .id("aot")
    }
}

/// The line beneath the AOT group. The row says it for VoiceOver, so this is
/// not read again.
struct TranslatedLibrariesNote: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        SpokenNote(TranslatedLibrariesWords.line(unused: model.jitInUseForNextStart))
    }
}
