// SPDX-License-Identifier: GPL-2.0-or-later

#if FOLLOW_X

import Foundation

// Follow developer on X's logic: plain values and rules, no UIKit. The
// application compiles this file where the row is compiled in; the
// AndariumCore package compiles the same file for its tests
// (Andarium/README.md#follow-developer-on-x).

/// The developer's profile on X: a handle and the address built from it. The
/// address is made here and nowhere else, so no file holds a profile address;
/// the handle comes from the ignored local configuration.
nonisolated enum FollowXProfile {
    /// The host the address names, the one X's own links use.
    static let host = "x.com"

    /// Whether `handle` can be one of X's: 1 to 15 letters, digits and
    /// underscores.
    static func isWellFormed(handle: String) -> Bool {
        let bytes = handle.utf8
        return (1...15).contains(bytes.count) && bytes.allSatisfy { byte in
            (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5A)
                || (byte >= 0x61 && byte <= 0x7A) || byte == 0x5F
        }
    }

    /// The profile's address: `https://` and the host, then the handle as the
    /// path, with no query. nil for a handle that is not well formed.
    static func url(handle: String) -> URL? {
        guard isWellFormed(handle: handle) else { return nil }
        var parts = URLComponents()
        parts.scheme = "https"
        parts.host = host
        parts.path = "/" + handle
        return parts.url
    }

    #if FOLLOW_X
    /// The Info.plist key the build writes the row's handle to, only where the
    /// row is compiled in (`Config/Andarium.xcconfig`).
    static let key = "AndariumFollowXHandle"

    /// The handle in a property list (the application's Info.plist); nil when
    /// the key is absent or its value is not a well formed handle, which hides
    /// the row.
    static func handle(in info: [String: Any]?) -> String? {
        guard let value = info?[key] as? String, isWellFormed(handle: value) else { return nil }
        return value
    }

    /// The handle this build names.
    static var handle: String? { handle(in: Bundle.main.infoDictionary) }
    #endif
}

#endif
