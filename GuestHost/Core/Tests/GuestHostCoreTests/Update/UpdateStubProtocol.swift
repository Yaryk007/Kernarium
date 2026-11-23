// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

@testable import GuestHostCore

/// A network of the tests' own: an in-process `URLProtocol` that answers by
/// host name, so each test names its own host and no request leaves the
/// process. It records what it was asked.
final class StubProtocol: URLProtocol, @unchecked Sendable {
    enum Behavior: Sendable {
        /// Answers with a status, headers and body chunks, one after another.
        case respond(status: Int, headers: [String: String] = [:], chunks: [Data])
        /// Redirects with a 302 to the address.
        case redirect(to: String)
        case fail(URLError)
        /// Asks for a login (or another challenge) with the authentication method.
        case challenge(method: String)
        /// Never answers until it is stopped.
        case hang
    }

    private static let lock = NSLock()
    nonisolated(unsafe) private static var behaviors: [String: Behavior] = [:]
    nonisolated(unsafe) private static var seen: [String: [URLRequest]] = [:]

    static func set(_ behavior: Behavior, for host: String) {
        lock.withLock { behaviors[host] = behavior }
    }

    static func requests(to host: String) -> [URLRequest] {
        lock.withLock { seen[host] ?? [] }
    }

    /// A configuration that routes every request here and is otherwise the reader's.
    static func configuration() -> URLSessionConfiguration {
        let configuration = UpdateReader.configuration()
        configuration.protocolClasses = [StubProtocol.self]
        return configuration
    }

    static func reader() -> UpdateReader {
        UpdateReader(configuration: { StubProtocol.configuration() })
    }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        guard let url = request.url, let host = url.host else { return }
        let behavior = Self.lock.withLock { () -> Behavior? in
            Self.seen[host, default: []].append(request)
            return Self.behaviors[host]
        }
        switch behavior {
        case .respond(let status, let headers, let chunks)?:
            let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            for chunk in chunks { client?.urlProtocol(self, didLoad: chunk) }
            client?.urlProtocolDidFinishLoading(self)
        case .redirect(let target)?:
            let response = HTTPURLResponse(
                url: url, statusCode: 302, httpVersion: "HTTP/1.1", headerFields: ["Location": target])!
            client?.urlProtocol(self, wasRedirectedTo: URLRequest(url: URL(string: target)!), redirectResponse: response)
        case .fail(let error)?:
            client?.urlProtocol(self, didFailWithError: error)
        case .challenge(let method)?:
            let space = URLProtectionSpace(
                host: host, port: 443, protocol: "https", realm: "Placeholder", authenticationMethod: method)
            let challenge = URLAuthenticationChallenge(
                protectionSpace: space, proposedCredential: nil, previousFailureCount: 0, failureResponse: nil,
                error: nil, sender: ChallengeSender())
            client?.urlProtocol(self, didReceive: challenge)
        case .hang?:
            break
        case nil:
            client?.urlProtocol(self, didFailWithError: URLError(.cannotFindHost))
        }
    }

    override func stopLoading() {}
}

/// The sender of a challenge the stub raises. The session answers through its
/// own, so this one is never used.
private final class ChallengeSender: NSObject, URLAuthenticationChallengeSender, @unchecked Sendable {
    func use(_ credential: URLCredential, for challenge: URLAuthenticationChallenge) {}
    func continueWithoutCredential(for challenge: URLAuthenticationChallenge) {}
    func cancel(_ challenge: URLAuthenticationChallenge) {}
    func performDefaultHandling(for challenge: URLAuthenticationChallenge) {}
    func rejectProtectionSpaceAndContinue(with challenge: URLAuthenticationChallenge) {}
}
