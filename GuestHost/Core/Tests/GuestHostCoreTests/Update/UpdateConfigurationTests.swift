// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// What a build says about its updates: the address, the application's
/// original identifier and the hosts its links may name. A build that does not
/// say all of it has no notice.
struct UpdateConfigurationTests {
    private func info(
        host: Any? = "example.com", path: Any? = "/source.json", identifier: Any? = "com.example.andarium",
        links: Any? = nil
    ) -> [String: Any] {
        var info: [String: Any] = [:]
        if let host { info[UpdateConfiguration.hostKey] = host }
        if let path { info[UpdateConfiguration.pathKey] = path }
        if let identifier { info[UpdateConfiguration.appIdentifierKey] = identifier }
        if let links { info[UpdateConfiguration.linkHostsKey] = links }
        return info
    }

    @Test func theKeysNameAnAddressAnIdentifierAndHosts() throws {
        let configuration = try #require(UpdateConfiguration(info: info(links: "downloads.example.org *.example.net")))
        #expect(configuration.address.absoluteString == "https://example.com/source.json")
        #expect(configuration.appIdentifier == "com.example.andarium")
        #expect(configuration.linkHosts == [UpdateHostRule("downloads.example.org")!, UpdateHostRule("*.example.net")!])
        #expect(configuration.links == UpdateLinkPolicy(sourceHost: "example.com", hosts: configuration.linkHosts))
    }

    @Test func aBuildWithNoKeysHasNoConfiguration() {
        #expect(UpdateConfiguration(info: nil) == nil)
        #expect(UpdateConfiguration(info: [:]) == nil)
        #expect(UpdateConfiguration(info: ["CFBundleName": "Andarium"]) == nil)
    }

    @Test(arguments: ["host", "path", "identifier"])
    func aBuildThatLacksOneOfTheThreeHasNoConfiguration(missing: String) {
        let partial = info(
            host: missing == "host" ? nil : "example.com", path: missing == "path" ? nil : "/source.json",
            identifier: missing == "identifier" ? nil : "com.example.andarium")
        #expect(UpdateConfiguration(info: partial) == nil)
    }

    @Test func emptyValuesMeanNoConfiguration() {
        #expect(UpdateConfiguration(info: info(host: "")) == nil)
        #expect(UpdateConfiguration(info: info(path: "")) == nil)
        #expect(UpdateConfiguration(info: info(identifier: "")) == nil)
    }

    @Test func valuesOfTheWrongTypeMeanNoConfiguration() {
        #expect(UpdateConfiguration(info: info(host: 5)) == nil)
        #expect(UpdateConfiguration(info: info(path: ["/a"])) == nil)
        #expect(UpdateConfiguration(info: info(identifier: true)) == nil)
    }

    @Test func aHostThatIsNoHostNameMeansNoConfiguration() {
        for host in ["example.com/x", "https://example.com", "example.com:8443", "user@example.com", "exa mple.com", "ex%61mple.com", "exämple.com", "-a.com", "a..com", "example.com?x=1"] {
            #expect(UpdateConfiguration(info: info(host: host)) == nil, "\(host)")
        }
    }

    @Test func aPathMustBeAbsoluteAndCarryNoQueryOrFragment() {
        #expect(UpdateConfiguration(info: info(path: "/a/b/source.json"))?.address.absoluteString == "https://example.com/a/b/source.json")
        #expect(UpdateConfiguration(info: info(path: "/"))?.address.absoluteString == "https://example.com/")
        for path in ["source.json", "https://evil.example.net/x", "/a?x=1", "/a#b", "/a b", "/a\n", "/é", "/a?", "/#"] {
            #expect(UpdateConfiguration(info: info(path: path)) == nil, "\(path)")
        }
    }

    @Test func aPathThatLooksLikeAnotherHostStaysOnTheConfiguredOne() {
        let configuration = UpdateConfiguration(info: info(path: "//evil.example.net/x"))
        #expect(configuration?.address.host == "example.com")
    }

    @Test func aBadEntryOfTheHostListVoidsTheWholeConfiguration() {
        #expect(UpdateConfiguration(info: info(links: "good.example.org bad_host!"))  == nil)
        #expect(UpdateConfiguration(info: info(links: "good.example.org https://x.example.org")) == nil)
        #expect(UpdateConfiguration(info: info(links: 5)) != nil, "a list of the wrong type names no host")
        #expect(UpdateConfiguration(info: info(links: ""))?.linkHosts == [])
        #expect(UpdateConfiguration(info: info(links: "   "))?.linkHosts == [])
        #expect(UpdateConfiguration(info: info(links: "a.example.org   b.example.org"))?.linkHosts.count == 2)
    }

    @Test func theIdentifierIsAReverseDNSName() {
        for identifier in ["com.example.andarium", "com.example.Andarium", "com.example.and-arium", "x"] {
            #expect(UpdateConfiguration.isBundleIdentifier(identifier), "\(identifier)")
        }
        for identifier in ["", "com.example.and arium", "com.example.andarium\n", "com.example.é", "com/example", String(repeating: "a", count: 156)] {
            #expect(!UpdateConfiguration.isBundleIdentifier(identifier), "\(identifier)")
        }
    }

    @Test func theAddressIsBuiltAndNeverTakenWhole() {
        #expect(UpdateConfiguration.address(host: "example.com", path: "/source.json")?.absoluteString == "https://example.com/source.json")
        #expect(UpdateConfiguration.address(host: "example.com", path: "/source.json")?.query == nil)
    }
}
