// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// A page of Info that a navigation path or a launch can open directly.
enum InfoPage: Hashable {
    case acknowledgements
    case license(String)
    case whatsNew

    /// The pages to push, in order, to land on this one.
    var trail: [InfoPage] {
        switch self {
        case .acknowledgements: [.acknowledgements]
        case .whatsNew: [.whatsNew]
        case .license: [.acknowledgements, self]
        }
    }
}

/// The projects Andarium carries and their licenses: the bundled resource
/// `Acknowledgements.json`, which `Andarium/scripts/acknowledgements.py`
/// generates from the verified dependency sources and the kernel's license
/// files. A test keeps it complete against what the app embeds.
nonisolated struct Acknowledgements: Decodable, Sendable {
    struct Group: Decodable, Identifiable, Sendable {
        let id: String
        let title: String
    }

    /// A license text shared by projects.
    struct License: Decodable, Sendable {
        let title: String
        let text: String
    }

    /// A text a project carries itself, with a heading when it has several.
    struct Notice: Decodable, Sendable {
        let title: String?
        let text: String
    }

    struct Project: Decodable, Identifiable, Hashable, Sendable {
        let id: String
        let group: String
        let name: String
        /// The holders, without the symbol or "Copyright".
        let copyright: String
        /// How the license reads in a list ("MIT License").
        let license: String
        /// Its SPDX expression.
        let spdx: String
        /// Identifiers into `Acknowledgements.licenses`.
        let licenses: [String]
        let texts: [Notice]
        /// The project whose page opens this one, which the list of projects
        /// then leaves out: its notices belong to what that project covers.
        let parent: String?
        /// The framework the app must embed for this project to be shown.
        let requiresFramework: String?
        /// Whether the project is shown only where the Android release the app
        /// carries has root (Magisk's files, and BusyBox with them).
        let requiresRoot: Bool?

        private enum CodingKeys: String, CodingKey {
            case id, group, name, copyright, license, spdx, licenses, texts, parent
            case requiresFramework = "requires_framework"
            case requiresRoot = "requires_root"
        }

        static func == (lhs: Project, rhs: Project) -> Bool { lhs.id == rhs.id }
        func hash(into hasher: inout Hasher) { hasher.combine(id) }
    }

    /// One heading and text on a project's license page.
    struct Block: Identifiable, Sendable {
        let id: Int
        let title: String?
        let text: String
    }

    let groups: [Group]
    let licenses: [String: License]
    let projects: [Project]

    /// The framework that holds the machine code translated from the Android
    /// system's libraries.
    static let aotFramework = "UmlArm64Aot"

    /// Whether the app in `bundle` embeds `framework`.
    static func carries(_ framework: String, in bundle: Bundle = .main) -> Bool {
        guard let frameworks = bundle.privateFrameworksURL else { return false }
        return FileManager.default.fileExists(
            atPath: frameworks.appendingPathComponent("\(framework).framework").path)
    }

    /// Whether this build shows a project: one that needs a framework is shown
    /// only by an app that embeds it, and one that needs root only where the
    /// release the app carries has it (`rootCarried`, `AppModel.rootAvailable`).
    func isShown(_ project: Project, in bundle: Bundle = .main, rootCarried: Bool = false) -> Bool {
        if project.requiresRoot == true, !rootCarried { return false }
        guard let framework = project.requiresFramework else { return true }
        return Self.carries(framework, in: bundle)
    }

    /// The projects the list shows under a group: those a page does not open.
    func projects(in group: Group, in bundle: Bundle = .main, rootCarried: Bool = false) -> [Project] {
        projects.filter {
            $0.group == group.id && $0.parent == nil && isShown($0, in: bundle, rootCarried: rootCarried)
        }
    }

    /// The projects a project's own page opens, in the resource's order.
    func children(of project: Project, in bundle: Bundle = .main, rootCarried: Bool = false) -> [Project] {
        projects.filter { $0.parent == project.id && isShown($0, in: bundle, rootCarried: rootCarried) }
    }

    func project(_ id: String) -> Project? {
        projects.first { $0.id == id }
    }

    /// The texts of a project's page, its own first, then the shared ones.
    func blocks(of project: Project) -> [Block] {
        var blocks = project.texts.map { (title: $0.title, text: $0.text) }
        for id in project.licenses {
            if let license = licenses[id] { blocks.append((title: license.title, text: license.text)) }
        }
        return blocks.enumerated().map { Block(id: $0.offset, title: $0.element.title, text: $0.element.text) }
    }

    static let empty = Acknowledgements(groups: [], licenses: [:], projects: [])

    /// The app's own, read once.
    static let bundled = load()

    static func decode(_ data: Data) throws -> Acknowledgements {
        try JSONDecoder().decode(Acknowledgements.self, from: data)
    }

    /// The acknowledgements in a bundle; empty if the resource is missing.
    static func load(from bundle: Bundle = .main) -> Acknowledgements {
        guard let url = bundle.url(forResource: "Acknowledgements", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let decoded = try? decode(data)
        else {
            assertionFailure("Acknowledgements.json is missing or unreadable")
            return .empty
        }
        return decoded
    }
}

extension Acknowledgements.Group {
    /// The group's heading in the person's language. The JSON's own title is
    /// English and stays for a group this build has no translation for.
    var heading: String {
        switch id {
        case "system":
            String(localized: "Operating system", comment: "Heading of the group of Acknowledgements with Android, LineageOS and Linux.")
        case "graphics":
            String(localized: "Graphics", comment: "Heading of the group of Acknowledgements with the graphics software.")
        case "libraries":
            String(localized: "Libraries", comment: "Heading of the group of Acknowledgements with the software libraries Andarium uses.")
        default: title
        }
    }
}

extension Acknowledgements.Project {
    /// The name in the person's language. The JSON's own is English and stays
    /// for a project this build has no translation for.
    var displayName: String {
        switch id {
        case "aot-libraries":
            String(localized: "Translated system libraries", comment: "Title of the page of notices, and of the row that opens it, for the Android system libraries that Andarium translates to machine code ahead of time.")
        default: name
        }
    }

    /// The paragraph a parent's page puts before the row that opens this
    /// project.
    var introduction: String? {
        switch id {
        case "aot-libraries":
            String(localized: "Andarium contains machine code translated from the libraries of the Android system it runs, so that those libraries start and run faster. The translated code is derived from the original libraries and stays under their licences: Apache License 2.0 for the Android Open Source Project and LineageOS, and the BSD, MIT, zlib and other licences of the libraries they include. Every notice that applies to a translated library is listed under Translated system libraries.", comment: "Paragraph on the Android Open Source Project page of Acknowledgements about the translated system libraries. The last words are the title of the row below it, Translated system libraries.")
        default: nil
        }
    }
}

/// The list of projects, grouped, ending with the line about names and marks.
struct AcknowledgementsView: View {
    @Environment(AppModel.self) private var model
    private let acknowledgements = Acknowledgements.bundled

    var body: some View {
        List {
            ForEach(acknowledgements.groups) { group in
                let projects = acknowledgements.projects(in: group, rootCarried: model.rootAvailable)
                Section {
                    ForEach(projects) { project in
                        NavigationLink(value: InfoPage.license(project.id)) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(project.name)
                                Text(project.license)
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                            }
                            .padding(.vertical, 2)
                        }
                    }
                } header: {
                    Text(group.heading)
                } footer: {
                    if group.id == acknowledgements.groups.last?.id {
                        Text("Android, LineageOS, Linux and the other names and marks here belong to their owners. They identify the software built into Andarium and imply no endorsement.")
                            .padding(.top, 8)
                    }
                }
            }
        }
        .navigationTitle("Acknowledgements")
        .navigationBarTitleDisplayMode(.inline)
    }
}

/// One project: who holds the copyright, the license, and its full text.
struct LicenseView: View {
    @Environment(AppModel.self) private var model
    let projectID: String
    private let acknowledgements = Acknowledgements.bundled

    var body: some View {
        ScrollView {
            if let project = acknowledgements.project(projectID),
               acknowledgements.isShown(project, rootCarried: model.rootAvailable)
            {
                VStack(alignment: .leading, spacing: 22) {
                    summary(project)
                    ForEach(acknowledgements.children(of: project, rootCarried: model.rootAvailable)) { child in
                        translated(child)
                    }
                    ForEach(acknowledgements.blocks(of: project)) { block in
                        VStack(alignment: .leading, spacing: 10) {
                            if let title = block.title {
                                Text(title).font(.headline)
                                    .accessibilityAddTraits(.isHeader)
                            }
                            ForEach(Array(paragraphs(block.text).enumerated()), id: \.offset) { _, paragraph in
                                Text(verbatim: paragraph)
                                    .font(.footnote)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                }
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 20)
                .padding(.vertical, 16)
            } else {
                ContentUnavailableView("No license", systemImage: "doc.text")
            }
        }
        .background(Color(.systemGroupedBackground))
        .navigationTitle(acknowledgements.project(projectID)?.displayName ?? String(localized: "License"))
        .navigationBarTitleDisplayMode(.inline)
    }

    private func summary(_ project: Acknowledgements.Project) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            field(String(localized: "Copyright"), "© \(project.copyright)")
            field(String(localized: "License"), project.license)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 20))
    }

    /// What a project covers that has notices of its own: a paragraph and the
    /// row that opens them.
    private func translated(_ child: Acknowledgements.Project) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            if let introduction = child.introduction {
                Text(introduction)
                    .font(.footnote)
                    .fixedSize(horizontal: false, vertical: true)
            }
            NavigationLink(value: InfoPage.license(child.id)) {
                HStack {
                    Text(child.displayName)
                    Spacer()
                    Image(systemName: "chevron.right")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.tertiary)
                        .accessibilityHidden(true)
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 20))
            }
            .buttonStyle(.plain)
        }
    }

    private func field(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.footnote)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.subheadline)
        }
        .accessibilityElement(children: .combine)
    }

    private func paragraphs(_ text: String) -> [String] {
        text.components(separatedBy: "\n\n")
    }
}
