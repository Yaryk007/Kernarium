// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// The version file the maker publishes (`sideload/source.json`), read the way
/// Andarium reads it: through `UpdateFile`, on the address and the link host a
/// release build is given, for the version and the build the project builds.
/// The size and the day are the release's own, so they are checked for their
/// shape and never pinned.
struct SideloadSourceTests {
    /// Where the repository's files are, from this file's own place in it.
    private static func repositoryFile(_ path: String) -> URL {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { url.deleteLastPathComponent() }
        return url.appending(path: path)
    }

    private struct Project {
        let identifier: String
        let version: String
        let build: String
        let deploymentTarget: String

        /// The application target's settings in `Andarium.xcodeproj`, a JSON
        /// file with trailing commas (the format Xcode 27 reads).
        static func read() throws -> Project {
            let data = try Data(contentsOf: SideloadSourceTests.repositoryFile("Andarium/Andarium.xcodeproj/project.xcproj"))
            let root = try #require(try JSONSerialization.jsonObject(with: data, options: [.json5Allowed]) as? [String: Any])
            let targets = try #require(root["targets"] as? [[String: Any]])
            let app = try #require(targets.first { $0["name"] as? String == "Andarium" })
            let settings = try #require(app["build-settings"] as? [String: Any])
            let project = try #require(root["build-settings"] as? [String: Any])
            let identifier = try #require(settings["PRODUCT_BUNDLE_IDENTIFIER"] as? String)
            let version = try #require(settings["MARKETING_VERSION"] as? String)
            let build = try #require(settings["CURRENT_PROJECT_VERSION"] as? String)
            let target = try #require(project["IPHONEOS_DEPLOYMENT_TARGET"] as? String)
            return Project(identifier: identifier, version: version, build: build, deploymentTarget: target)
        }
    }

    private static func fileData() throws -> Data {
        try Data(contentsOf: repositoryFile("sideload/source.json"))
    }

    private static func fileObject() throws -> [String: Any] {
        try #require(try JSONSerialization.jsonObject(with: fileData()) as? [String: Any])
    }

    private static func downloadAddress(_ data: Data) throws -> URL {
        let root = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
        let apps = try #require(root["apps"] as? [[String: Any]])
        let versions = try #require(apps.first?["versions"] as? [[String: Any]])
        let text = try #require(versions.first?["downloadURL"] as? String)
        return try #require(URL(string: text))
    }

    /// The reading a build with the file's own address, and the host its
    /// download links name, would make.
    private static func read(installedBuild: Int, project: Project) throws -> UpdateFile.Reading {
        let data = try fileData()
        let address = try #require(try fileObject()["sourceURL"] as? String)
        let source = try #require(URL(string: address))
        let host = try #require(source.host)
        let download = try Self.downloadAddress(data)
        let appHost = try #require(download.host)
        let rule = try #require(UpdateHostRule(appHost))
        let links = UpdateLinkPolicy(sourceHost: host, hosts: [rule])
        return UpdateFile.read(
            data, appIdentifier: project.identifier, installedBuild: installedBuild, os: UpdateOSVersion(major: 26), links: links)
    }

    @Test func theApplicationsOwnReaderOffersTheBuiltVersion() throws {
        let project = try Project.read()
        let build = try #require(Int(project.build))
        let reading = try Self.read(installedBuild: build - 1, project: project)
        guard case .read(let version?) = reading else { Issue.record("the reader offers no version: \(reading)"); return }
        #expect(version.version == project.version)
        #expect(version.build == build)
        #expect(version.sizeBytes > 0)
        #expect(version.minimumOS == UpdateOSVersion(project.deploymentTarget))
        #expect(version.maximumOS == nil)
        let name = "Andarium-\(project.version)-\(project.build).ipa"
        #expect(version.appFile.lastPathComponent == name)
        #expect(version.appFile.path.hasSuffix("/releases/download/andarium-\(project.version)/\(name)"))
        let notes = try #require(version.notes, "the release page is not offered")
        #expect(notes.path.hasSuffix("/releases/tag/andarium-\(project.version)"))
        #expect(notes.host == version.appFile.host)
    }

    @Test func theBuiltBuildItselfIsNoUpdate() throws {
        let project = try Project.read()
        let build = try #require(Int(project.build))
        #expect(try Self.read(installedBuild: build, project: project) == .read(nil))
        #expect(try Self.read(installedBuild: build + 1, project: project) == .read(nil))
    }

    /// Foundation reads a trailing comma and a comment without complaint, so the
    /// bytes are checked for what AltStore and SideStore refuse: the file is
    /// plain JSON, outside its strings no comment, no single quote and no comma
    /// before a closing bracket.
    @Test func theFileIsPlainJSON() throws {
        let text = try #require(String(data: Self.fileData(), encoding: .utf8))
        var inString = false
        var escaped = false
        var previous: Character = " "
        for character in text {
            if inString {
                if escaped { escaped = false } else if character == "\\" { escaped = true } else if character == "\"" { inString = false }
                continue
            }
            switch character {
            case "\"": inString = true
            case "/", "'": Issue.record("a comment or a single quote outside a string")
            case "}", "]": #expect(previous != ",", "a comma before a closing bracket")
            default: break
            }
            if !character.isWhitespace { previous = character }
        }
        #expect(!inString)
    }

    @Test func theFileIsOfTheRequiredShape() throws {
        let project = try Project.read()
        let data = try Self.fileData()
        #expect(data.count < UpdateFile.largestBytes)
        let root = try Self.fileObject()
        for key in ["name", "identifier", "sourceURL", "apps"] { #expect(root[key] != nil, "the source has no \(key)") }
        let apps = try #require(root["apps"] as? [[String: Any]])
        #expect(apps.count == 1)
        let app = try #require(apps.first)
        #expect(app["bundleIdentifier"] as? String == project.identifier)
        for key in ["name", "developerName", "localizedDescription", "iconURL"] {
            #expect(app[key] as? String != nil, "the app has no \(key)")
        }
        let versions = try #require(app["versions"] as? [[String: Any]])
        let entry = try #require(versions.first)
        #expect(entry["version"] as? String == project.version)
        #expect(entry["buildVersion"] as? String == project.build, "buildVersion must be a string")
        #expect(entry["minOSVersion"] as? String == project.deploymentTarget)
        #expect(entry["size"] as? Int != nil, "size must be a whole number of bytes")
        let date = try #require(entry["date"] as? String)
        #expect(UpdateDay(date) != nil, "date is not ISO 8601")
        #expect(entry["downloadURL"] as? String != nil)
    }

    /// The icon the app's page names is a tracked file: an address on the
    /// branch the source is served from is the same path in this repository.
    @Test func theIconTheAppNamesIsInTheRepository() throws {
        let root = try Self.fileObject()
        let apps = try #require(root["apps"] as? [[String: Any]])
        let text = try #require(apps.first?["iconURL"] as? String)
        let url = try #require(URL(string: text))
        // /<account>/<repository>/<branch>/<path in the repository>
        let path = url.pathComponents.dropFirst(4).joined(separator: "/")
        #expect(path.hasPrefix("sideload/") && path.hasSuffix(".png"), "the icon is not a png of the sideload directory: \(path)")
        #expect(FileManager.default.fileExists(atPath: Self.repositoryFile(path).path), "\(path) is not tracked")
    }

    @Test func everyAddressInTheFileIsHTTPS() throws {
        func addresses(_ value: Any, key: String) -> [(String, String)] {
            switch value {
            case let text as String where key.hasSuffix("URL") || key == "website": return [(key, text)]
            case let object as [String: Any]: return object.flatMap { addresses($0.value, key: $0.key) }
            case let list as [Any]: return list.flatMap { addresses($0, key: key) }
            default: return []
            }
        }
        let found = addresses(try Self.fileObject(), key: "")
        #expect(found.count >= 5)
        for (key, text) in found {
            let url = try #require(URL(string: text), "\(key) is no address")
            #expect(url.scheme == "https", "\(key) is not https")
            #expect(url.user == nil && url.port == nil, "\(key) names a user or a port")
        }
    }

    @Test func theFileNamesTheAddressTheReadmeTellsPeopleToAdd() throws {
        let readme = try String(contentsOf: Self.repositoryFile("README.md"), encoding: .utf8)
        let root = try Self.fileObject()
        let address = try #require(root["sourceURL"] as? String)
        #expect(readme.contains("\n\(address)\n"), "the README does not give the source's own address")
        // The address is what a build is given as a host and a path.
        let url = try #require(URL(string: address))
        #expect(UpdateConfiguration.address(host: url.host ?? "", path: url.path)?.absoluteString == address)
    }
}
