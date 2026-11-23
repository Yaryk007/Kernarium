// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// The one request the notice makes, against a network of the tests' own: what
/// it sends, what it refuses and what it answers.
struct UpdateReaderTests {
    private let body = Data("{ \"apps\": [] }".utf8)

    private func address(_ host: String, _ path: String = "/source.json") -> URL {
        URL(string: "https://\(host)\(path)")!
    }

    // MARK: What it sends

    @Test func theConfigurationKeepsNothingAndWaitsForNothing() {
        let configuration = UpdateReader.configuration()
        #expect(configuration.httpCookieStorage == nil)
        #expect(configuration.httpCookieAcceptPolicy == .never)
        #expect(configuration.httpShouldSetCookies == false)
        #expect(configuration.urlCache == nil)
        #expect(configuration.urlCredentialStorage == nil)
        #expect(configuration.requestCachePolicy == .reloadIgnoringLocalAndRemoteCacheData)
        #expect(configuration.waitsForConnectivity == false)
        #expect(configuration.allowsConstrainedNetworkAccess == false)
        #expect(configuration.timeoutIntervalForRequest == 15)
        #expect(configuration.timeoutIntervalForResource == 15)
        #expect(configuration.tlsMinimumSupportedProtocolVersion == .TLSv12)
        // An ephemeral configuration writes no cache, cookie or credential to disk.
        #expect(configuration.httpAdditionalHeaders?["User-Agent"] as? String == "update-check/1")
        #expect(configuration.httpAdditionalHeaders?["Accept-Language"] as? String == "*")
    }

    @Test func itSendsOneGetWithAFixedUserAgentAndNothingThatIdentifiesTheInstall() async {
        let host = "sends.example.com"
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: host)
        let result = await StubProtocol.reader().fetch(address(host))
        #expect(result == .data(body))
        let requests = StubProtocol.requests(to: host)
        #expect(requests.count == 1)
        let request = requests[0]
        #expect(request.httpMethod == "GET")
        #expect(request.url?.absoluteString == "https://sends.example.com/source.json")
        #expect(request.url?.query == nil)
        #expect(request.httpBody == nil)
        #expect(request.httpShouldHandleCookies == false)
        #expect(request.cachePolicy == .reloadIgnoringLocalAndRemoteCacheData)
        #expect(request.value(forHTTPHeaderField: "User-Agent") == "update-check/1")
        #expect(request.value(forHTTPHeaderField: "Accept-Language") == "*")
        for header in ["Cookie", "Authorization", "If-None-Match", "If-Modified-Since", "Referer", "X-Request-ID", "Cache-Control"] {
            #expect(request.value(forHTTPHeaderField: header) == nil, "\(header)")
        }
        let names = Set((request.allHTTPHeaderFields ?? [:]).keys.map { $0.lowercased() })
        #expect(names.isSubset(of: ["user-agent", "accept-language", "accept", "accept-encoding", "connection", "host"]), "\(names)")
    }

    // MARK: What it answers

    @Test func aBodyWithinTheLimitIsTheAnswer() async {
        let host = "answers.example.com"
        StubProtocol.set(.respond(status: 200, chunks: [body.prefix(5), body.dropFirst(5)]), for: host)
        #expect(await StubProtocol.reader().fetch(address(host)) == .data(body))
    }

    @Test func aBodyExactlyAtTheLimitIsTheAnswerAndOneByteMoreIsNot() async {
        let limit = UpdateFile.largestBytes
        let atLimit = Data(repeating: 0x20, count: limit)
        StubProtocol.set(.respond(status: 200, chunks: [atLimit.prefix(limit / 2), atLimit.suffix(limit / 2)]), for: "at.example.com")
        #expect(await StubProtocol.reader().fetch(address("at.example.com")) == .data(atLimit))
        StubProtocol.set(.respond(status: 200, chunks: [atLimit, Data([0x20])]), for: "over.example.com")
        #expect(await StubProtocol.reader().fetch(address("over.example.com")) == .failed)
    }

    @Test func aBodyThatDeclaresMoreThanTheLimitIsRefusedBeforeItIsRead() async {
        let host = "declares.example.com"
        StubProtocol.set(.respond(status: 200, headers: ["Content-Length": "\(UpdateFile.largestBytes + 1)"], chunks: [body]), for: host)
        #expect(await StubProtocol.reader().fetch(address(host)) == .failed)
    }

    @Test(arguments: [201, 204, 206, 301, 304, 400, 403, 404, 410, 429, 500, 503])
    func aStatusOtherThan200IsNoAnswer(status: Int) async {
        let host = "status\(status).example.com"
        StubProtocol.set(.respond(status: status, chunks: [body]), for: host)
        #expect(await StubProtocol.reader().fetch(address(host)) == .failed, "\(status)")
    }

    // MARK: Redirects

    @Test func aRedirectToHttpsIsFollowedWithTheSameFixedRequest() async {
        StubProtocol.set(.redirect(to: "https://target1.example.com/moved.json"), for: "from1.example.com")
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: "target1.example.com")
        #expect(await StubProtocol.reader().fetch(address("from1.example.com")) == .data(body))
        let followed = StubProtocol.requests(to: "target1.example.com")
        #expect(followed.count == 1)
        #expect(followed.first?.value(forHTTPHeaderField: "User-Agent") == "update-check/1")
        #expect(followed.first?.value(forHTTPHeaderField: "Accept-Language") == "*")
        #expect(followed.first?.value(forHTTPHeaderField: "Cookie") == nil)
    }

    @Test func aRedirectToHttpIsRefusedAndNeverRequested() async {
        StubProtocol.set(.redirect(to: "http://target2.example.com/moved.json"), for: "from2.example.com")
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: "target2.example.com")
        #expect(await StubProtocol.reader().fetch(address("from2.example.com")) == .failed)
        #expect(StubProtocol.requests(to: "target2.example.com").isEmpty)
    }

    @Test func aRedirectToAnotherSchemeIsRefused() async {
        StubProtocol.set(.redirect(to: "ftp://target3.example.com/x"), for: "from3.example.com")
        #expect(await StubProtocol.reader().fetch(address("from3.example.com")) == .failed)
        StubProtocol.set(.redirect(to: "altstore://install?url=https://example.com/a.ipa"), for: "from3b.example.com")
        #expect(await StubProtocol.reader().fetch(address("from3b.example.com")) == .failed)
    }

    @Test func aChainOfMoreThanFiveRedirectsIsRefused() async {
        for index in 0..<8 {
            StubProtocol.set(.redirect(to: "https://hop\(index + 1).example.com/x"), for: "hop\(index).example.com")
        }
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: "hop8.example.com")
        #expect(await StubProtocol.reader().fetch(address("hop0.example.com")) == .failed)
        // Five redirects are followed.
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: "hop5.example.com")
        #expect(await StubProtocol.reader().fetch(address("hop0.example.com")) == .data(body))
    }

    // MARK: Failures

    @Test func anAddressThatIsNotHttpsIsNeverRequested() async {
        let host = "plain.example.com"
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: host)
        #expect(await StubProtocol.reader().fetch(URL(string: "http://\(host)/source.json")!) == .failed)
        #expect(await StubProtocol.reader().fetch(URL(string: "ftp://\(host)/source.json")!) == .failed)
        #expect(StubProtocol.requests(to: host).isEmpty)
    }

    @Test func aTimeoutAndAFailedHandshakeAreAReadThatFailed() async {
        StubProtocol.set(.fail(URLError(.timedOut)), for: "slow.example.com")
        #expect(await StubProtocol.reader().fetch(address("slow.example.com")) == .failed)
        StubProtocol.set(.fail(URLError(.secureConnectionFailed)), for: "tls.example.com")
        #expect(await StubProtocol.reader().fetch(address("tls.example.com")) == .failed)
        StubProtocol.set(.fail(URLError(.serverCertificateUntrusted)), for: "cert.example.com")
        #expect(await StubProtocol.reader().fetch(address("cert.example.com")) == .failed)
        StubProtocol.set(.fail(URLError(.cannotFindHost)), for: "dns.example.com")
        #expect(await StubProtocol.reader().fetch(address("dns.example.com")) == .failed)
    }

    /// A server that asks for a login, a client certificate or any other
    /// credential has answered: the read is an attempt that failed, which uses
    /// up the day, and no credential is ever sent.
    @Test(arguments: [
        NSURLAuthenticationMethodHTTPBasic, NSURLAuthenticationMethodHTTPDigest, NSURLAuthenticationMethodNTLM,
        NSURLAuthenticationMethodNegotiate, NSURLAuthenticationMethodClientCertificate,
    ])
    func anAuthenticationChallengeIsAReadThatFailed(method: String) async {
        let host = "login-\(method.lowercased().filter(\.isLetter)).example.com"
        StubProtocol.set(.challenge(method: method), for: host)
        #expect(await StubProtocol.reader().fetch(address(host)) == .failed, "\(method)")
        #expect(StubProtocol.requests(to: host).count == 1)
    }

    @Test func noNetworkAndLowDataModeAreNoReadAtAll() async {
        StubProtocol.set(.fail(URLError(.notConnectedToInternet)), for: "offline.example.com")
        #expect(await StubProtocol.reader().fetch(address("offline.example.com")) == .notTried)
        StubProtocol.set(.fail(URLError(.dataNotAllowed)), for: "cellular.example.com")
        #expect(await StubProtocol.reader().fetch(address("cellular.example.com")) == .notTried)
        let constrained = URLError(
            .notConnectedToInternet,
            userInfo: [NSURLErrorNetworkUnavailableReasonKey: URLError.NetworkUnavailableReason.constrained.rawValue])
        #expect(constrained.networkUnavailableReason == .constrained)
        StubProtocol.set(.fail(constrained), for: "lowdata.example.com")
        #expect(await StubProtocol.reader().fetch(address("lowdata.example.com")) == .notTried)
        let constrainedOther = URLError(
            .cannotConnectToHost,
            userInfo: [NSURLErrorNetworkUnavailableReasonKey: URLError.NetworkUnavailableReason.constrained.rawValue])
        StubProtocol.set(.fail(constrainedOther), for: "lowdata2.example.com")
        #expect(await StubProtocol.reader().fetch(address("lowdata2.example.com")) == .notTried)
    }

    @Test func aCancelledReadIsNoReadAtAll() async {
        let host = "cancel.example.com"
        StubProtocol.set(.hang, for: host)
        let reader = StubProtocol.reader()
        let url = address(host)
        let task = Task { await reader.fetch(url) }
        // The request is under way once the stub has seen it.
        let deadline = ContinuousClock.now + .seconds(10)
        while StubProtocol.requests(to: host).isEmpty, ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(10))
        }
        #expect(!StubProtocol.requests(to: host).isEmpty)
        task.cancel()
        #expect(await task.value == .notTried)
    }

    @Test func aReadCancelledBeforeItStartsMakesNoRequest() async {
        let host = "early.example.com"
        StubProtocol.set(.respond(status: 200, chunks: [body]), for: host)
        let reader = StubProtocol.reader()
        let url = address(host)
        let task = Task {
            try? await Task.sleep(for: .seconds(30))
            return await reader.fetch(url)
        }
        task.cancel()
        #expect(await task.value == .notTried)
        #expect(StubProtocol.requests(to: host).isEmpty)
    }
}
