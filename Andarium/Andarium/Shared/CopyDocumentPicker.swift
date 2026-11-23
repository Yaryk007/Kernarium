// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// The files of one pick that the copy-mode document picker made, in the
/// folder of Andarium's own that holds them (`PickedFolder`). Whoever keeps a
/// file of the pick keeps the folder, which is removed when the last one lets
/// go.
struct PickedCopies {
    var urls: [URL]
    var folder: PickedFolder
}

extension View {
    /// The system's document picker in copy mode, the one every build
    /// presents (`CopyDocumentPicker`).
    /// `onCompletion` hears the files of a pick, or why they did not arrive;
    /// a picker the person cancelled is silent.
    func documentCopyPicker(
        isPresented: Binding<Bool>, types: [UTType], allowsMultipleSelection: Bool = false,
        onCompletion: @escaping (Result<PickedCopies, Error>) -> Void
    ) -> some View {
        background {
            CopyDocumentPicker(
                isPresented: isPresented, types: types, allowsMultipleSelection: allowsMultipleSelection,
                onCompletion: onCompletion)
                .frame(width: 0, height: 0)
                .accessibilityHidden(true)
        }
    }
}

/// The document picker with `asCopy` set: Files hands over copies of the
/// files in the application's own inbox in place of access to the originals,
/// which a build whose signature the system does not tie to its bundle
/// identifier is refused. Each copy is moved at once into a folder of ours
/// (`PickedFolder`) and the pick's files are the ones there.
///
/// SwiftUI's `fileImporter` has no copy mode, so the picker is presented
/// from a view controller of the sheet it is shown over, as `fileImporter`
/// presents it, while `isPresented` is true.
struct CopyDocumentPicker: UIViewControllerRepresentable {
    @Binding var isPresented: Bool
    let types: [UTType]
    let allowsMultipleSelection: Bool
    let onCompletion: (Result<PickedCopies, Error>) -> Void

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIViewController(context: Context) -> UIViewController { UIViewController() }

    func updateUIViewController(_ anchor: UIViewController, context: Context) {
        context.coordinator.update(self, anchor: anchor)
    }

    @MainActor
    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        private var owner: CopyDocumentPicker?
        /// The picker on screen, if one is.
        private var picker: UIDocumentPickerViewController?

        func update(_ owner: CopyDocumentPicker, anchor: UIViewController) {
            self.owner = owner
            if owner.isPresented {
                guard picker == nil else { return }
                present(from: anchor)
            } else if let picker {
                // The model took the pick away (Settings closed, the run ended):
                // the picker goes with it and says nothing.
                self.picker = nil
                picker.delegate = nil
                picker.presentingViewController?.dismiss(animated: true)
            }
        }

        private func present(from anchor: UIViewController) {
            let picker = UIDocumentPickerViewController(forOpeningContentTypes: owner?.types ?? [.item], asCopy: true)
            picker.allowsMultipleSelection = owner?.allowsMultipleSelection ?? false
            picker.delegate = self
            self.picker = picker
            // Not inside the view update that asked for it. A sheet that is
            // still taking the last picker away (a second tap, at once) is
            // waited for a moment; one that never frees ends the pick.
            Task { @MainActor [weak self, weak anchor] in
                for attempt in 0..<20 {
                    guard let self, self.picker === picker else { return }
                    var presenter = anchor
                    while let parent = presenter?.parent { presenter = parent }
                    if let presenter, presenter.viewIfLoaded?.window != nil, presenter.presentedViewController == nil {
                        presenter.present(picker, animated: true)
                        return
                    }
                    if attempt < 19 { try? await Task.sleep(for: .milliseconds(100)) }
                }
                self?.finish(.failure(PickedFileFailure.notTaken))
            }
        }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            guard controller === picker else { return }
            finish(Self.take(urls))
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            guard controller === picker else { return }
            finish(nil)
        }

        /// The pick is over: the picker is let go and the answer, if there is
        /// one, is given.
        private func finish(_ result: Result<PickedCopies, Error>?) {
            picker = nil
            owner?.isPresented = false
            if let result { owner?.onCompletion(result) }
        }

        /// Moves the copies into a folder of ours (made in `root`, a test's own
        /// when it gives one). A folder that cannot be made, or a copy that
        /// cannot be moved, ends the pick with nothing of it left: the system's
        /// copies that were not moved are ours to remove too.
        static func take(_ urls: [URL], in root: URL = PickedFolder.directory) -> Result<PickedCopies, Error>? {
            guard !urls.isEmpty else { return nil }
            do {
                let folder = try PickedFolder(in: root)
                do {
                    return .success(PickedCopies(urls: try urls.map(folder.adopt), folder: folder))
                } catch {
                    folder.discard()
                    throw error
                }
            } catch {
                for url in urls { PickedFolder.removeUntaken(url) }
                return .failure(PickedFileFailure.notTaken)
            }
        }
    }
}
