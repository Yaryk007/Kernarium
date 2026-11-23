// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// Which links of the file the application accepts, and the links it builds
/// itself for the sideloading apps.
struct UpdateLinksTests {
    private let policy = UpdateLinkPolicy(
        sourceHost: "example.com", hosts: [UpdateHostRule("downloads.example.org")!, UpdateHostRule("*.example.net")!])

    // MARK: Host rules

    @Test func aHostRuleIsAHostNameOrAStarDotDomain() {
        #expect(UpdateHostRule("example.com") == UpdateHostRule("EXAMPLE.com"))
        #expect(UpdateHostRule("*.example.com")?.includesSubdomains == true)
        #expect(UpdateHostRule("example.com")?.includesSubdomains == false)
        for text in ["", "*", "*.", "*example.com", "**.example.com", "example..com", "-example.com", "example-.com", ".example.com",
            "example.com.", "exa mple.com", "example.com/path", "example.com:443", "user@example.com", "https://example.com", "exämple.com",
            "ex_ample.com", String(repeating: "a", count: 64) + ".com", "*.*.example.com"] {
            #expect(UpdateHostRule(text) == nil, "\(text)")
        }
    }

    @Test func aStarRuleAcceptsSubdomainsAndNotTheDomainItself() {
        // Only reserved names: example.* and the .test and .invalid top-level domains.
        let rule = UpdateHostRule("*.example.test")!
        #expect(rule.accepts("cdn.example.test"))
        #expect(rule.accepts("a.b.example.test"))
        #expect(!rule.accepts("example.test"))
        #expect(!rule.accepts("evilexample.test"))
        #expect(!rule.accepts("example.test.evil.invalid"))
    }

    // MARK: Links

    @Test func aHttpsLinkOnAnAcceptedHostPasses() {
        #expect(policy.link("https://example.com/a.ipa")?.absoluteString == "https://example.com/a.ipa")
        #expect(policy.link("https://EXAMPLE.com/a.ipa") != nil)
        #expect(policy.link("https://downloads.example.org/a/b.ipa?x=1") != nil)
        #expect(policy.link("https://cdn.example.net/a.ipa") != nil)
        #expect(policy.link("https://example.com:443/a.ipa") != nil)
        #expect(policy.link("https://example.com") != nil)
    }

    @Test func aLinkThatBreaksARuleIsDropped() {
        let bad = [
            "http://example.com/a.ipa", "HTTPS://example.com/a.ipa", "ftp://example.com/a.ipa", "altstore://install?url=https://example.com/a.ipa",
            "javascript:alert(1)", "data:text/plain,hi", "file:///etc/passwd", "//example.com/a.ipa", "/a.ipa", "a.ipa", "",
            "https://other.example.com/a.ipa", "https://example.org/a.ipa", "https://example.net/a.ipa", "https://evil.invalid/a.ipa",
            "https://example.com.evil.invalid/a.ipa", "https://example.com@evil.invalid/a.ipa",
            "https://user@example.com/a.ipa", "https://user:pass@example.com/a.ipa", "https://example.com:8443/a.ipa",
            "https://example.com:80/a.ipa", "https://exam%70le.com/a.ipa", "https://exämple.com/a.ipa", "https://example.com/a b.ipa",
            "https://example.com/a\n.ipa", "https://example.com/\u{7F}", "https://example.com/é", "https://example.com./a.ipa",
            "https://[::1]/a.ipa", "https://127.0.0.1/a.ipa", " https://example.com/a.ipa", "https://example.com/a.ipa ",
            "https://example.com/" + String(repeating: "a", count: 2048),
        ]
        for text in bad { #expect(policy.link(text) == nil, "\(text.prefix(80))") }
    }

    @Test func aHostThatOnlyEndsWithTheFilesHostIsNotIt() {
        let reserved = UpdateLinkPolicy(sourceHost: "example.test")
        #expect(reserved.link("https://example.test/a.ipa") != nil)
        #expect(reserved.link("https://evilexample.test/a.ipa") == nil)
        #expect(reserved.link("https://example.test.evil.invalid/a.ipa") == nil)
        #expect(reserved.link("https://example.test@evil.invalid/a.ipa") == nil)
    }

    @Test func aLinkAtTheSizeLimitPassesAndOneByteMoreDoesNot() {
        let prefix = "https://example.com/"
        let atLimit = prefix + String(repeating: "a", count: UpdateLinkPolicy.longestLinkBytes - prefix.utf8.count)
        #expect(atLimit.utf8.count == 2048)
        #expect(policy.link(atLimit) != nil)
        #expect(policy.link(atLimit + "a") == nil)
    }

    @Test func theFilesHostIsTheOneItWasReadFromAndTheBuildCannotBeWidenedByTheLink() {
        let other = UpdateLinkPolicy(sourceHost: "downloads.example.org")
        #expect(other.link("https://downloads.example.org/a.ipa") != nil)
        #expect(other.link("https://example.com/a.ipa") == nil)
    }

    // MARK: Tools

    @Test func aToolLinkIsBuiltFromItsTemplateWithTheAddressEncoded() {
        let file = URL(string: "https://example.com/releases/andarium-1.1-16.ipa")!
        #expect(UpdateTool.altStore.installLink(appFile: file)?.absoluteString
            == "altstore://install?url=https%3A%2F%2Fexample.com%2Freleases%2Fandarium-1.1-16.ipa")
        #expect(UpdateTool.sideStore.installLink(appFile: file)?.absoluteString
            == "sidestore://install?url=https%3A%2F%2Fexample.com%2Freleases%2Fandarium-1.1-16.ipa")
    }

    @Test func theEncodedAddressCannotBreakOutOfItsQueryItem() {
        let file = URL(string: "https://example.com/a.ipa?x=1&url=https://evil.example.net/b.ipa#frag")!
        let link = UpdateTool.altStore.installLink(appFile: file)!
        let components = URLComponents(url: link, resolvingAgainstBaseURL: false)!
        #expect(components.scheme == "altstore")
        #expect(components.host == "install")
        #expect(components.queryItems?.count == 1)
        #expect(components.queryItems?.first?.name == "url")
        #expect(components.queryItems?.first?.value == file.absoluteString)
        #expect(!link.absoluteString.dropFirst("altstore://install?url=".count).contains { "&=?#/:+ ".contains($0) })
    }

    @Test func eachToolHasItsOwnSchemeAndAProbe() {
        #expect(UpdateTool.altStore.scheme == "altstore")
        #expect(UpdateTool.sideStore.scheme == "sidestore")
        #expect(UpdateTool.altStore.probe.absoluteString == "altstore://")
        #expect(UpdateTool.sideStore.probe.absoluteString == "sidestore://")
        #expect(UpdateTool.altStore.name == "AltStore")
        #expect(UpdateTool.sideStore.name == "SideStore")
        #expect(UpdateTool.allCases.count == 2)
    }

    // MARK: The device

    @Test func anIPhoneOrIPadOffersTheToolsItHas() {
        func has(_ schemes: Set<String>) -> (URL) -> Bool { { schemes.contains($0.scheme ?? "") } }
        #expect(UpdateHost.device.tools(canOpen: has([])) == [])
        #expect(UpdateHost.device.tools(canOpen: has(["altstore"])) == [.altStore])
        #expect(UpdateHost.device.tools(canOpen: has(["sidestore"])) == [.sideStore])
        #expect(UpdateHost.device.tools(canOpen: has(["altstore", "sidestore"])) == [.altStore, .sideStore])
        #expect(UpdateHost.device.tools(canOpen: has(["livecontainer"])) == [])
    }

    @Test func aMacAndAVisionProOfferNoTool() {
        #expect(UpdateHost.mac.tools(canOpen: { _ in true }) == [])
        #expect(UpdateHost.vision.tools(canOpen: { _ in true }) == [])
    }
}
