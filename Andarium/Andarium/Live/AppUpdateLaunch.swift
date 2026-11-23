// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import UIKit

extension AppEntry {
    /// The update notice of this launch: a newer version of Andarium, found
    /// by one read of the version file the build names. A build that names
    /// none (every Debug build, and a Release build made without the ignored
    /// `GuestHost/Config/Update.local.xcconfig`) has none, and neither has a
    /// launch that is not the person's own: a harness never reads, shows or
    /// keeps anything of it. `info` is the build's `Info.plist`.
    @MainActor
    func makeAppUpdate(on model: AppModel, info: [String: Any]? = Bundle.main.infoDictionary) -> UpdateNotice? {
        guard announcesWhatsNew, let build, let configuration = UpdateConfiguration(info: info) else { return nil }
        let host: UpdateHost =
            model.padAppHost.isiOSAppOnMac ? .mac : model.padAppHost.isiOSAppOnVision ? .vision : .device
        return UpdateNotice(
            configuration: configuration,
            app: UpdateNotice.App(name: "Andarium", installedBuild: build, installedLabel: model.system.appVersion),
            store: DefaultsUpdateStore(defaults: defaults), fetcher: UpdateReader(), host: host,
            canOpen: { UIApplication.shared.canOpenURL($0) },
            open: { UIApplication.shared.open($0, options: [:], completionHandler: nil) })
    }
}
