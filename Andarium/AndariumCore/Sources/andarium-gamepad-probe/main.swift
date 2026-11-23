// SPDX-License-Identifier: GPL-2.0-or-later

// A test-only tool that plays a gamepad script through the real encoders to a
// TCP port, where the macOS launcher's `--vsock-forward` reaches the guest's
// gamepad relay (vsock 5558). It prints what the guest's drivers ask of the
// controllers, one line each, so a test can check the output bytes' effects.
// It is never linked into the application.

import AndariumCore
import AndariumCoreTestSupport
import Darwin
import Foundation

private struct Options {
    var host = "127.0.0.1"
    var port: UInt16?
    var script = "identity"
    var holdSeconds = 5

    init?(_ arguments: [String]) {
        var iterator = arguments.makeIterator()
        while let argument = iterator.next() {
            switch argument {
            case "--host": guard let value = iterator.next() else { return nil }; host = value
            case "--port": guard let value = iterator.next(), let port = UInt16(value) else { return nil }; self.port = port
            case "--script": guard let value = iterator.next() else { return nil }; script = value
            case "--hold": guard let value = iterator.next(), let seconds = Int(value) else { return nil }; holdSeconds = seconds
            default: return nil
            }
        }
        guard port != nil, GamepadScript.named(script) != nil else { return nil }
    }
}

private func say(_ line: String) {
    FileHandle.standardOutput.write(Data((line + "\n").utf8))
}

/// A TCP connection to the forwarded port, with a bound on the wait.
private struct TCPOpener: GuestStreamOpener {
    var host: String
    var port: UInt16

    func openStream() throws -> Int32 {
        let descriptor = socket(AF_INET, SOCK_STREAM, 0)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        guard inet_pton(AF_INET, host, &address.sin_addr) == 1 else {
            close(descriptor)
            throw POSIXError(.EINVAL)
        }
        var timeout = timeval(tv_sec: 0, tv_usec: 250_000)
        setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard result == 0 else {
            let code = errno
            close(descriptor)
            throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        return descriptor
    }
}

/// Prints each rumble tone as the player would play it.
private final class PrintingPlayer: HapticPlayer, @unchecked Sendable {
    func play(_ score: HapticScore) -> HapticPlayResult {
        if let tone = score.events.first, tone.kind == .tone {
            say("rumble \(score.target.rawValue) \(tone.intensity) \(tone.sharpness)")
        }
        return .played
    }
    func setToneIntensity(_ intensity: Float, on target: HapticTarget) -> Bool {
        say("rumble \(target.rawValue) \(intensity)")
        return true
    }
    func stop(_ target: HapticTarget) { say("rumble \(target.rawValue) 0") }
    func stopAll() { say("rumble stop-all") }
    func prepare() {}
}

private final class PrintingIndicators: GamepadIndicators, @unchecked Sendable {
    func setLight(controller: Int, red: UInt8, green: UInt8, blue: UInt8) { say("light \(controller) \(red) \(green) \(blue)") }
    func setPlayerIndex(controller: Int, index: Int?) { say("player \(controller) \(index.map(String.init) ?? "none")") }
}

guard let options = Options(Array(CommandLine.arguments.dropFirst())), let port = options.port,
    let script = GamepadScript.named(options.script)
else {
    FileHandle.standardError.write(
        Data(
            "usage: andarium-gamepad-probe --port <port> [--host <ipv4>] [--script \(GamepadScript.names.joined(separator: "|"))] [--hold <seconds>]\n"
                .utf8))
    exit(2)
}

let session = GamepadSession(
    opener: TCPOpener(host: options.host, port: port), player: PrintingPlayer(), indicators: PrintingIndicators(),
    wake: { say("wake") }, log: { say("log \($0)") })
// Not booted: the attempts go on until the relay answers.
session.update(running: true, booted: false, foreground: true)
var waited = 0
while !session.isConnected && waited < 60_000 {
    usleep(50_000)
    waited += 50
}
guard session.isConnected else {
    FileHandle.standardError.write(Data("andarium-gamepad-probe: no relay answered\n".utf8))
    exit(1)
}
say("connected")
script.play(on: session)
say("played \(options.script)")
// The guest's output (rumble, lights, player numbers) arrives while this waits.
sleep(UInt32(max(0, options.holdSeconds)))
session.update(running: false, booted: false, foreground: false)
session.settle()
say("done")
