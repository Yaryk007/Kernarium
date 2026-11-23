// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a build says about where its updates are published and which
/// application it is, from the keys its `Info.plist` carries. Only a build
/// made with an address has them (`GuestHost/Config/Update.xcconfig`); every
/// other build has no configuration, and so no read, no card and no Info
/// section.
public struct UpdateConfiguration: Equatable, Sendable {
    /// The `Info.plist` keys, shared by every application.
    public static let hostKey = "UpdateSourceHost"
    public static let pathKey = "UpdateSourcePath"
    public static let linkHostsKey = "UpdateLinkHosts"
    public static let appIdentifierKey = "UpdateAppIdentifier"

    /// The version file's address: `https://` and the host, then the path.
    public let address: URL
    /// The application's original bundle identifier. A sideloading tool signs
    /// the installed application under another one, so the file is matched by
    /// this value, which the build copies from its own setting into a key
    /// the tool does not touch.
    public let appIdentifier: String
    /// The hosts links of the file may name besides the file's own.
    public let linkHosts: [UpdateHostRule]

    public init(address: URL, appIdentifier: String, linkHosts: [UpdateHostRule] = []) {
        self.address = address
        self.appIdentifier = appIdentifier
        self.linkHosts = linkHosts
    }

    /// nil unless the keys name a host, a path and an identifier that are all
    /// well formed. `UpdateLinkHosts` is a list separated by spaces, of
    /// exact host names and `*.` patterns; one that is no host name makes the
    /// whole configuration void, as a half-read list would accept more or
    /// less than the maker wrote.
    public init?(info: [String: Any]?) {
        guard let info,
            let host = info[Self.hostKey] as? String,
            let path = info[Self.pathKey] as? String,
            let identifier = info[Self.appIdentifierKey] as? String,
            let address = Self.address(host: host, path: path),
            Self.isBundleIdentifier(identifier)
        else { return nil }
        var hosts: [UpdateHostRule] = []
        if let list = info[Self.linkHostsKey] as? String {
            for entry in list.split(separator: " ", omittingEmptySubsequences: true) {
                guard let rule = UpdateHostRule(String(entry)) else { return nil }
                hosts.append(rule)
            }
        }
        self.init(address: address, appIdentifier: identifier, linkHosts: hosts)
    }

    /// The address a host and a path name: no port, no user name, no query,
    /// no fragment. The code builds it, so no file holds a whole address.
    public static func address(host: String, path: String) -> URL? {
        guard UpdateHostRule.isHostName(host), path.hasPrefix("/"), path.utf8.count <= 512,
            path.utf8.allSatisfy({ $0 > 0x20 && $0 < 0x7F && $0 != 0x3F && $0 != 0x23 })
        else { return nil }
        var parts = URLComponents()
        parts.scheme = "https"
        parts.host = host
        parts.path = path
        guard let url = parts.url, parts.percentEncodedPath == path else { return nil }
        return url
    }

    /// A reverse-DNS identifier as the system writes one: letters, digits,
    /// hyphens and dots.
    public static func isBundleIdentifier(_ text: String) -> Bool {
        (1...155).contains(text.utf8.count)
            && text.utf8.allSatisfy { byte in
                (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5A)
                    || (byte >= 0x61 && byte <= 0x7A) || byte == 0x2E || byte == 0x2D
            }
    }

    /// The policy for links in a file read from `address`.
    public var links: UpdateLinkPolicy {
        UpdateLinkPolicy(sourceHost: address.host ?? "", hosts: linkHosts)
    }
}
