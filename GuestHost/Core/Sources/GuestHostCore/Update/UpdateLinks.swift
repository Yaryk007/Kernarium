// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A host name the build accepts links on: an exact name, or `*.` and a
/// domain for every subdomain of it (never the domain itself).
public struct UpdateHostRule: Hashable, Sendable {
    public let name: String
    public let includesSubdomains: Bool

    /// nil for text that is no host name or pattern of one: ASCII letters,
    /// digits, hyphens and dots, at most 253 characters, no empty or
    /// hyphen-edged label.
    public init?(_ text: String) {
        var name = text.lowercased()
        var subdomains = false
        if name.hasPrefix("*.") {
            subdomains = true
            name.removeFirst(2)
        }
        guard Self.isHostName(name) else { return nil }
        self.name = name
        self.includesSubdomains = subdomains
    }

    func accepts(_ host: String) -> Bool {
        includesSubdomains ? host.hasSuffix("." + name) : host == name
    }

    static func isHostName(_ name: String) -> Bool {
        guard (1...253).contains(name.utf8.count) else { return false }
        return name.split(separator: ".", omittingEmptySubsequences: false).allSatisfy { label in
            (1...63).contains(label.utf8.count) && !label.hasPrefix("-") && !label.hasSuffix("-")
                && label.utf8.allSatisfy { byte in
                    (byte >= 0x30 && byte <= 0x39) || (byte >= 0x61 && byte <= 0x7A) || byte == 0x2D
                }
        }
    }
}

/// What a link of the version file must be before the application opens it:
/// `https` on the host the file was read from or on a host the build names,
/// with no user name, no port other than the default and no more than 2 KiB.
/// The file cannot widen the list.
public struct UpdateLinkPolicy: Equatable, Sendable {
    public static let longestLinkBytes = 2048

    /// The host the version file was read from.
    public let sourceHost: String
    public let hosts: [UpdateHostRule]

    public init(sourceHost: String, hosts: [UpdateHostRule] = []) {
        self.sourceHost = sourceHost.lowercased()
        self.hosts = hosts
    }

    /// The link `text` names, or nil when it fails a rule.
    public func link(_ text: String) -> URL? {
        let bytes = text.utf8
        guard bytes.count <= Self.longestLinkBytes,
            bytes.allSatisfy({ $0 > 0x20 && $0 < 0x7F }),
            let parts = URLComponents(string: text),
            parts.scheme == "https",
            parts.user == nil, parts.password == nil,
            parts.port == nil || parts.port == 443,
            parts.percentEncodedHost?.contains("%") == false,
            let host = parts.host?.lowercased(), UpdateHostRule.isHostName(host),
            host == sourceHost || hosts.contains(where: { $0.accepts(host) }),
            let url = parts.url
        else { return nil }
        return url
    }
}

/// A sideloading app that the update can be handed to. The application builds
/// the link from the template below and the checked address of the app file;
/// no scheme or link of the file's is ever opened.
public enum UpdateTool: CaseIterable, Hashable, Sendable {
    case altStore
    case sideStore

    /// The tool's name, which only its own row shows.
    public var name: String {
        switch self {
        case .altStore: "AltStore"
        case .sideStore: "SideStore"
        }
    }

    /// The URL scheme the tool answers to; the application's `Info.plist`
    /// lists it under `LSApplicationQueriesSchemes` so the application can
    /// ask whether the tool is installed.
    public var scheme: String {
        switch self {
        case .altStore: "altstore"
        case .sideStore: "sidestore"
        }
    }

    /// The address that asks the tool whether it is installed.
    public var probe: URL { URL(string: "\(scheme)://")! }

    /// The link that hands `appFile` to the tool to download, sign and
    /// install: `<scheme>://install?url=<the address, percent-encoded>`.
    public func installLink(appFile: URL) -> URL? {
        let unreserved = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
        guard let encoded = appFile.absoluteString.addingPercentEncoding(withAllowedCharacters: unreserved) else {
            return nil
        }
        return URL(string: "\(scheme)://install?url=\(encoded)")
    }
}

/// What kind of device the application is on, for the ways it offers to
/// update.
public enum UpdateHost: Sendable {
    case device
    /// An iPad application on an Apple silicon Mac.
    case mac
    /// An iPad application on an Apple Vision Pro.
    case vision

    /// The sideloading apps to offer: the ones installed on an iPhone or an
    /// iPad, none on a Mac or a Vision Pro, where the app file and the
    /// release page are the ways.
    public func tools(canOpen: (URL) -> Bool) -> [UpdateTool] {
        guard self == .device else { return [] }
        return UpdateTool.allCases.filter { canOpen($0.probe) }
    }
}
