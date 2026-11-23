// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a read of the version file came to.
public enum UpdateFetch: Equatable, Sendable {
    /// The body of a 200 answer within the limit.
    case data(Data)
    /// The read was made and gave no file: a status other than 200, a body
    /// over the limit, a refused redirect, a timeout, a failure of TLS.
    case failed
    /// No read happened: it was cancelled, the device is offline, or Low Data
    /// Mode or the cellular settings forbid it. It does not use up the day.
    case notTried
}

/// Reads the version file, behind a seam so the rules around it are tested
/// without the network.
public protocol UpdateFetching: Sendable {
    func fetch(_ address: URL) async -> UpdateFetch
}

/// The one request the update notice makes: a GET of the address and nothing
/// else. It sends the least that identifies the person's install:
///
/// - an ephemeral session with no cookies, no cache and no credentials, and
///   no conditional header (an ETag could act as an identifier);
/// - a fixed `User-Agent` without the application's name or build, and a
///   fixed `Accept-Language`, so the host cannot count versions or languages;
/// - no query, no identifier and nothing about the device in the request;
/// - `https` only, redirects included, at most five of them;
/// - no wait for connectivity, no retry, 15 seconds in all, 1 MiB at most,
///   and never under Low Data Mode.
public struct UpdateReader: UpdateFetching {
    public static let timeout: TimeInterval = 15
    public static let userAgent = "update-check/1"
    public static let acceptLanguage = "*"
    public static let longestRedirectChain = 5

    private let makeConfiguration: @Sendable () -> URLSessionConfiguration

    public init() {
        makeConfiguration = { Self.configuration() }
    }

    init(configuration: @escaping @Sendable () -> URLSessionConfiguration) {
        makeConfiguration = configuration
    }

    static func configuration() -> URLSessionConfiguration {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpCookieStorage = nil
        configuration.httpCookieAcceptPolicy = .never
        configuration.httpShouldSetCookies = false
        configuration.urlCache = nil
        configuration.urlCredentialStorage = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        configuration.waitsForConnectivity = false
        configuration.allowsConstrainedNetworkAccess = false
        configuration.timeoutIntervalForRequest = timeout
        configuration.timeoutIntervalForResource = timeout
        configuration.httpAdditionalHeaders = ["User-Agent": userAgent, "Accept-Language": acceptLanguage]
        configuration.tlsMinimumSupportedProtocolVersion = .TLSv12
        return configuration
    }

    static func request(for address: URL) -> URLRequest {
        var request = URLRequest(url: address, cachePolicy: .reloadIgnoringLocalAndRemoteCacheData, timeoutInterval: timeout)
        request.httpMethod = "GET"
        request.httpShouldHandleCookies = false
        return request
    }

    public func fetch(_ address: URL) async -> UpdateFetch {
        guard address.scheme == "https" else { return .failed }
        let collector = Collector()
        let session = URLSession(configuration: makeConfiguration(), delegate: collector, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        let task = session.dataTask(with: Self.request(for: address))
        return await withTaskCancellationHandler {
            await withCheckedContinuation { (continuation: CheckedContinuation<UpdateFetch, Never>) in
                collector.begin(continuation)
                if Task.isCancelled {
                    task.cancel()
                    collector.answer(.notTried)
                } else {
                    task.resume()
                }
            }
        } onCancel: {
            task.cancel()
        }
    }

    /// Collects the body under the rules and answers once.
    private final class Collector: NSObject, URLSessionDataDelegate, @unchecked Sendable {
        private let lock = NSLock()
        private var continuation: CheckedContinuation<UpdateFetch, Never>?
        private var body = Data()
        private var refused = false
        private var redirects = 0

        func begin(_ continuation: CheckedContinuation<UpdateFetch, Never>) {
            lock.withLock { self.continuation = continuation }
        }

        /// Ends a read that never started.
        func answer(_ answer: UpdateFetch) {
            let waiting = lock.withLock { () -> CheckedContinuation<UpdateFetch, Never>? in
                defer { continuation = nil }
                return continuation
            }
            waiting?.resume(returning: answer)
        }

        /// Marks the read as refused by a rule; the caller cancels the task.
        private func refuse() {
            lock.withLock { refused = true }
        }

        func urlSession(
            _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
            newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void
        ) {
            let follow = lock.withLock { () -> Bool in
                redirects += 1
                return redirects <= UpdateReader.longestRedirectChain && request.url?.scheme == "https"
            }
            if follow {
                completionHandler(UpdateReader.request(for: request.url!))
            } else {
                // A redirect that is refused ends the read: the 3xx answer is
                // no file, so nothing waits for it.
                refuse()
                task.cancel()
                completionHandler(nil)
            }
        }

        func urlSession(
            _ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge,
            completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
        ) {
            // The server's certificate is checked as the system does; no
            // other challenge is answered, and no credential is ever sent.
            if challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust {
                completionHandler(.performDefaultHandling, nil)
            } else {
                // A server that asks for a login has answered: the read is an
                // attempt that failed, and uses up the day.
                refuse()
                completionHandler(.cancelAuthenticationChallenge, nil)
            }
        }

        func urlSession(
            _ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
            completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void
        ) {
            guard let http = response as? HTTPURLResponse, http.statusCode == 200, http.url?.scheme == "https",
                http.expectedContentLength <= Int64(UpdateFile.largestBytes)
            else {
                refuse()
                completionHandler(.cancel)
                return
            }
            completionHandler(.allow)
        }

        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
            let over = lock.withLock { () -> Bool in
                body.append(data)
                return body.count > UpdateFile.largestBytes
            }
            if over {
                refuse()
                dataTask.cancel()
            }
        }

        func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?) {
            let (continuation, answer) = lock.withLock { () -> (CheckedContinuation<UpdateFetch, Never>?, UpdateFetch) in
                let waiting = self.continuation
                self.continuation = nil
                if refused { return (waiting, .failed) }
                guard let error else { return (waiting, .data(body)) }
                return (waiting, Self.answer(for: error))
            }
            continuation?.resume(returning: answer)
        }

        /// A read that never reached the network or was cancelled is not an
        /// attempt; every other failure is.
        private static func answer(for error: any Error) -> UpdateFetch {
            guard let error = error as? URLError else { return .failed }
            if error.networkUnavailableReason == .constrained { return .notTried }
            switch error.code {
            case .cancelled, .notConnectedToInternet, .dataNotAllowed, .internationalRoamingOff, .callIsActive:
                return .notTried
            default:
                return .failed
            }
        }
    }
}
