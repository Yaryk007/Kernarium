// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Andarium

/// The Acknowledgements resource the app bundles reads, groups every project
/// and carries a license text for each. `tests/tooling/andarium` keeps it
/// complete against what the app embeds.
struct AcknowledgementsTests {
    private let acknowledgements = Acknowledgements.load()

    @Test func theBundledResourceDecodes() {
        #expect(acknowledgements.projects.count >= 15)
        #expect(!acknowledgements.groups.isEmpty)
    }

    @Test func everyProjectHasAGroupAndAText() throws {
        let groups = Set(acknowledgements.groups.map(\.id))
        for project in acknowledgements.projects {
            #expect(groups.contains(project.group), "\(project.name) has no group")
            #expect(!project.copyright.isEmpty && !project.license.isEmpty && !project.spdx.isEmpty)
            let blocks = acknowledgements.blocks(of: project)
            #expect(!blocks.isEmpty, "\(project.name) has no license text")
            #expect(blocks.allSatisfy { !$0.text.isEmpty })
        }
    }

    @Test func everyGroupListsItsProjectsOnce() throws {
        // The projects a page opens are not in the list, and an app that
        // embeds the translated code's framework lists all the others once.
        let embedding = try bundle(embedding: [Acknowledgements.aotFramework])
        let listed = acknowledgements.groups
            .flatMap { acknowledgements.projects(in: $0, in: embedding, rootCarried: true) }.map(\.id)
        #expect(listed.sorted() == acknowledgements.projects.filter { $0.parent == nil }.map(\.id).sorted())
    }

    @Test func translatedLibrariesAreOpenedFromAndroidAndShownOnlyWithTheirFramework() throws {
        let translated = try #require(acknowledgements.project("aot-libraries"))
        let android = try #require(acknowledgements.project("android"))
        #expect(translated.parent == android.id)
        #expect(translated.requiresFramework == Acknowledgements.aotFramework)
        #expect(android.texts.isEmpty == false || android.licenses.isEmpty == false)

        let with = try bundle(embedding: [Acknowledgements.aotFramework])
        let without = try bundle(embedding: ["SomethingElse"])
        #expect(acknowledgements.children(of: android, in: with).map(\.id) == ["aot-libraries"])
        #expect(acknowledgements.children(of: android, in: without).isEmpty)
        #expect(acknowledgements.isShown(translated, in: with))
        #expect(!acknowledgements.isShown(translated, in: without))
        // Reached from Android's page, never as a row of the list.
        for bundle in [with, without] {
            let listed = acknowledgements.groups.flatMap { acknowledgements.projects(in: $0, in: bundle) }
            #expect(!listed.contains { $0.id == "aot-libraries" })
        }
        // Its page is the notices: a heading and text for each.
        let blocks = acknowledgements.blocks(of: translated)
        #expect(blocks.count >= 10)
        #expect(blocks.allSatisfy { $0.title?.isEmpty == false && !$0.text.isEmpty })
    }

    @Test func theTranslatedLibrariesHaveTheirOwnParagraphAndTitle() throws {
        let translated = try #require(acknowledgements.project("aot-libraries"))
        #expect(translated.displayName == "Translated system libraries")
        let paragraph = try #require(translated.introduction)
        #expect(paragraph.hasPrefix("Andarium contains machine code translated from the libraries of the Android system it runs"))
        #expect(paragraph.hasSuffix("is listed under Translated system libraries."))
        #expect(acknowledgements.project("android")?.introduction == nil)
    }

    @Test func magiskAndBusyBoxAreListedOnlyWhereTheReleaseCarriesRoot() throws {
        let magisk = try #require(acknowledgements.project("magisk"))
        let busybox = try #require(acknowledgements.project("busybox"))
        let system = try #require(acknowledgements.groups.first { $0.id == "system" })
        let bundle = try bundle(embedding: [Acknowledgements.aotFramework])
        for project in [magisk, busybox] {
            #expect(project.requiresRoot == true && project.group == system.id)
            #expect(!acknowledgements.isShown(project, in: bundle))
            #expect(acknowledgements.isShown(project, in: bundle, rootCarried: true))
        }
        let without = acknowledgements.projects(in: system, in: bundle).map(\.id)
        let with = acknowledgements.projects(in: system, in: bundle, rootCarried: true).map(\.id)
        #expect(!without.contains("magisk") && !without.contains("busybox"))
        #expect(with.contains("magisk") && with.contains("busybox"))
        // The others are as they were.
        #expect(without == with.filter { $0 != "magisk" && $0 != "busybox" })
    }

    @Test func eachCarriesItsLicenceTextAndNamesWhereTheSourceIs() throws {
        let magisk = try #require(acknowledgements.project("magisk"))
        let busybox = try #require(acknowledgements.project("busybox"))
        // The notice lines name the contributors and no person.
        #expect(magisk.copyright == "The Magisk contributors" && magisk.license == "GNU GPL v3 or later")
        #expect(magisk.spdx == "GPL-3.0-or-later")
        #expect(busybox.copyright == "The BusyBox contributors" && busybox.license == "GNU GPL v2")
        let three = acknowledgements.blocks(of: magisk)
        let two = acknowledgements.blocks(of: busybox)
        #expect(three.contains { $0.title == "GNU General Public License, Version 3" && $0.text.contains("Version 3, 29 June 2007") })
        #expect(two.contains { $0.title == "GNU General Public License, Version 2" && $0.text.contains("Version 2, June 1991") })
        for blocks in [three, two] {
            let source = try #require(blocks.first { $0.title == "Source code" })
            #expect(source.text.contains("https://github.com/topjohnwu/Magisk") && source.text.contains("v31.1"))
        }
    }

    /// A bundle that embeds the named frameworks, as an app does.
    private func bundle(embedding frameworks: [String]) throws -> Bundle {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("embedding-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        for name in frameworks {
            try FileManager.default.createDirectory(
                at: root.appendingPathComponent("Frameworks/\(name).framework", isDirectory: true),
                withIntermediateDirectories: true)
        }
        return try #require(Bundle(url: root))
    }

    @Test func theSystemIsFirstAndUserModeLinuxIsNotListed() {
        #expect(acknowledgements.projects.first?.name == "Android Open Source Project")
        #expect(!acknowledgements.projects.contains { $0.name == "User-Mode Linux" })
        #expect(acknowledgements.project("linux")?.spdx == "GPL-2.0-only WITH Linux-syscall-note")
    }

    @Test func aLicensePageOpensOnTheListAboveIt() {
        #expect(InfoPage.license("mesa").trail == [.acknowledgements, .license("mesa")])
        #expect(InfoPage.acknowledgements.trail == [.acknowledgements])
    }
}
