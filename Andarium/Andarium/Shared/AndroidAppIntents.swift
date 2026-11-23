// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AppIntents
import CoreSpotlight
import Foundation

/// Add to Home Screen is an App Intent: one entity for each Android app that
/// has a launcher icon, and one intent that brings Andarium to the front and
/// opens it. Apple's Shortcuts shows the entities as tiles under Andarium, and
/// a held tile adds an icon to the Home Screen with the entity's picture and
/// name. Search and Siri find the same entities. There is no URL scheme, no
/// extension and no shared container: the intent runs in Andarium's process.
///
/// The entity, its query, the intent and the shortcut are all from iOS 27: the
/// system lists an action for an entity and its query (Find) by what the
/// build's metadata says of them, so an entity that exists before iOS 27 would
/// leave Shortcuts a Find action for apps that no query gives
/// (`IPadAppHost.offersAddToHomeScreen`).
///
/// An entity's identity is `package/class` of the app's launcher activity,
/// which is the same on every launch and after a reinstall and needs no table
/// to be read back: a Home Screen icon made before Android was erased finds its
/// app again when the app is installed again.
@available(iOS 27.0, *)
nonisolated struct AndroidAppEntity: AppEntity, IndexedEntity {
    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Android App"
    static let defaultQuery = AndroidAppQuery()

    let id: String
    /// The app's name in the language Android is in.
    let name: String

    init(id: String, name: String) {
        self.id = id
        self.name = name
    }

    init(_ app: AndroidApp) {
        self.init(id: app.id, name: app.label)
    }

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(
            title: "\(name)", subtitle: "Android",
            image: AndroidAppsHub.shared.store.icon(forID: id).map { DisplayRepresentation.Image(data: $0) }
                ?? DisplayRepresentation.Image(systemName: "app.dashed"))
    }

    var attributeSet: CSSearchableItemAttributeSet {
        let attributes = defaultAttributeSet
        attributes.title = name
        attributes.keywords = ["Android", id.split(separator: "/").first.map(String.init) ?? id]
        attributes.thumbnailData = AndroidAppsHub.shared.store.icon(forID: id)
        return attributes
    }
}

/// The entities of the cache. An identifier the cache no longer lists but a
/// record remembers (an app removed from Android, or Android erased) is still
/// an entity, so that a Home Screen icon made for it can say the app is gone,
/// and works again after a reinstall.
@available(iOS 27.0, *)
nonisolated struct AndroidAppQuery: EnumerableEntityQuery {
    func allEntities() async throws -> [AndroidAppEntity] {
        AndroidAppsHub.shared.entities()
    }

    func entities(for identifiers: [AndroidAppEntity.ID]) async throws -> [AndroidAppEntity] {
        let hub = AndroidAppsHub.shared
        guard hub.hostOffers else { return [] }
        let store = hub.store
        let catalog = store.catalog()
        return identifiers.compactMap { id in
            if let app = catalog?.app(withID: id) { return AndroidAppEntity(app) }
            guard let component = AndroidComponent(identifier: id) else { return nil }
            let known = store.record(forID: id)?.label ?? component.packageName
            return AndroidAppEntity(id: id, name: known)
        }
    }

    func suggestedEntities() async throws -> [AndroidAppEntity] {
        AndroidAppsHub.shared.entities()
    }
}

/// The system's own request to index the apps again.
@available(iOS 27.0, *)
extension AndroidAppQuery: IndexedEntityQuery {
    /// The system asks for apps to be indexed again: through the one ordered
    /// path to the index, which judges the switch and the catalog as they are
    /// then and offers only the apps that are listed. A write the index did not
    /// take is thrown, so that the system asks again.
    func reindexEntities(for identifiers: [AndroidAppEntity.ID], indexDescription: CSSearchableIndexDescription) async throws {
        try await AndroidAppsHub.shared.writer.reindex(identifiers)
    }

    func reindexAllEntities(indexDescription: CSSearchableIndexDescription) async throws {
        try await AndroidAppsHub.shared.writer.reindex(nil)
    }
}

/// The error a request that names nothing Android could open ends with.
nonisolated struct AndroidAppRequestError: Error, CustomLocalizedStringResourceConvertible {
    var localizedStringResource: LocalizedStringResource { "This isn\u{2019}t an Android app that Andarium can open." }
}

/// Opens an app in Android. Andarium comes to the front and starts Android first
/// when it is not running; what happens then is the application's
/// (`AppModel.requestAndroidApp`): the intent only hands the request over.
///
/// From iOS 27, where Shortcuts has a tile for each app; before it the apps are
/// no entities, so Shortcuts offers no action to choose one for. The
/// declaration says so, and a Mac and a Vision Pro of iOS 27 keep the action,
/// which runs by name.
@available(iOS 27.0, *)
struct OpenAndroidAppIntent: OpenIntent {
    static let title: LocalizedStringResource = "Open Android App"
    static let description = IntentDescription("Opens an app in Android.")
    static var supportedModes: IntentModes { .foreground(.immediate) }

    @Parameter(title: "App")
    var target: AndroidAppEntity

    static var parameterSummary: some ParameterSummary {
        Summary("Open \(\.$target)")
    }

    init() {}

    init(target: AndroidAppEntity) {
        self.target = target
    }

    func perform() async throws -> some IntentResult {
        let id = target.id
        let name = target.name
        let accepted = await MainActor.run {
            AndroidAppsHub.shared.model.requestAndroidApp(identifier: id, label: name)
        }
        guard accepted else { throw AndroidAppRequestError() }
        return .result()
    }
}

/// The tiles under Andarium in Shortcuts, and what Siri understands: from iOS
/// 27, as the intent is.
@available(iOS 27.0, *)
nonisolated struct AndroidAppShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: OpenAndroidAppIntent(),
            phrases: [
                "Open \(\.$target) in \(.applicationName)",
                "Open an Android app in \(.applicationName)",
            ],
            shortTitle: "Open Android App",
            systemImageName: "square.grid.2x2"
        )
    }
}
