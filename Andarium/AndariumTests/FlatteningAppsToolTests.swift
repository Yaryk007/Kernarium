// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import ImageIO
import Testing
import UIKit
@testable import Andarium

/// An icon the tool gives is decoded whole and kept as a picture the host drew
/// itself, on an opaque white square, whatever the tool said of its alpha; one
/// that does not decode is no icon and is asked for again.
struct FlatteningAppsToolTests {
    private struct Base: AndroidAppsTool {
        var answers: [String: AndroidAppIconAnswer]

        func digest(limit: Duration) async -> AndroidAppDigestAnswer { .unavailable }
        func list(limit: Duration) async -> AndroidAppListing { .unavailable }
        func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] { answers }
        func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer { .unavailable }
    }

    private func app(_ name: String) -> AndroidApp {
        AndroidApp(
            component: AndroidComponent(packageName: "com.example.\(name)", activityClass: "com.example.\(name).Main")!,
            label: name, builtIn: false, iconTag: "0000000000000001")
    }

    /// A red square in the middle of a transparent picture, `side` pixels wide and `height` high.
    private func transparent(side: Int, height: Int? = nil) -> Data {
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = false
        let size = CGSize(width: side, height: height ?? side)
        return UIGraphicsImageRenderer(size: size, format: format).image { context in
            UIColor.red.setFill()
            context.fill(CGRect(x: size.width / 4, y: size.height / 4, width: size.width / 2, height: size.height / 2))
        }.pngData()!
    }

    private func opaque(side: Int) -> Data {
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        return UIGraphicsImageRenderer(size: CGSize(width: side, height: side), format: format).image { context in
            UIColor.blue.setFill()
            context.fill(CGRect(x: 0, y: 0, width: side, height: side))
        }.pngData()!
    }

    /// The pixel at (x, y) as 0...255 red, green, blue and alpha, whatever the file's own layout.
    private func pixel(_ data: Data, _ x: Int, _ y: Int) -> [UInt8]? {
        guard let image = UIImage(data: data)?.cgImage else { return nil }
        var bytes = [UInt8](repeating: 0, count: 4)
        guard let context = CGContext(
            data: &bytes, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        context.draw(image, in: CGRect(x: -x, y: -(image.height - 1 - y), width: image.width, height: image.height))
        return bytes
    }

    /// Whether no pixel of the picture is less than fully opaque (iOS paints a
    /// transparent pixel of a Home Screen icon black).
    private func isFullyOpaque(_ data: Data) -> Bool {
        guard let image = UIImage(data: data)?.cgImage else { return false }
        let width = image.width, height = image.height
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        let drawn = bytes.withUnsafeMutableBytes { buffer -> Bool in
            guard let context = CGContext(
                data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        return drawn && stride(from: 3, to: bytes.count, by: 4).allSatisfy { bytes[$0] == 255 }
    }

    private func answers(_ given: [String: AndroidAppIconAnswer]) async -> [String: AndroidAppIconAnswer] {
        await FlatteningAppsTool(base: Base(answers: given)).icons(of: [], limit: .seconds(1))
    }

    @Test func aTransparentPictureIsDrawnOnWhiteAndHasNoAlpha() async throws {
        let comet = app("comet")
        let result = await answers([comet.id: .png(transparent(side: 192), hasAlpha: true)])
        guard case let .png(data, hasAlpha)? = result[comet.id] else { Issue.record("no picture: \(result)"); return }
        #expect(!hasAlpha)
        #expect(isFullyOpaque(data))
        #expect(pixel(data, 2, 2) == [255, 255, 255, 255], "the transparent corner is white")
        #expect(pixel(data, 96, 96) == [255, 0, 0, 255], "the red square is kept")
        #expect(UIImage(data: data)?.cgImage.map { $0.width == 192 && $0.height == 192 } == true)
    }

    @Test func aPictureTheToolCalledOpaqueButIsNotIsFlattenedAllTheSame() async throws {
        let comet = app("comet")
        let result = await answers([comet.id: .png(transparent(side: 128), hasAlpha: false)])
        guard case let .png(data, hasAlpha)? = result[comet.id] else { Issue.record("no picture: \(result)"); return }
        #expect(!hasAlpha && isFullyOpaque(data))
        #expect(pixel(data, 1, 1) == [255, 255, 255, 255])
    }

    @Test func anOpaquePictureComesBackAsOneTheHostDrew() async throws {
        let comet = app("comet")
        let original = opaque(side: 96)
        let result = await answers([comet.id: .png(original, hasAlpha: true)])
        guard case let .png(data, hasAlpha)? = result[comet.id] else { Issue.record("no picture: \(result)"); return }
        #expect(!hasAlpha && isFullyOpaque(data))
        #expect(pixel(data, 10, 10) == [0, 0, 255, 255])
    }

    @Test func aFlattenedPictureIsEightBitsAChannelAndWithinTheStoresBound() async throws {
        // A picture as full of detail as an icon gets: its flattened PNG is 8 bits a
        // channel (not the display's extended range) and one the store keeps.
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = false
        let noisy = UIGraphicsImageRenderer(size: CGSize(width: 192, height: 192), format: format).image { context in
            for y in stride(from: 0, to: 192, by: 4) {
                for x in stride(from: 0, to: 192, by: 4) {
                    UIColor(
                        red: CGFloat((x * 7 + y * 3) % 256) / 255, green: CGFloat((x * 11 + y * 5) % 256) / 255,
                        blue: CGFloat((x * 13 + y * 17) % 256) / 255, alpha: 1).setFill()
                    context.fill(CGRect(x: x, y: y, width: 4, height: 4))
                }
            }
        }.pngData()!
        let comet = app("comet")
        let result = await answers([comet.id: .png(noisy, hasAlpha: true)])
        guard case let .png(data, _)? = result[comet.id] else { Issue.record("no picture: \(result)"); return }
        let source = try #require(CGImageSourceCreateWithData(data as CFData, nil))
        let properties = try #require(CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any])
        #expect(properties[kCGImagePropertyDepth] as? Int == 8)
        #expect(data.count <= AndroidAppLimits.largestIcon, "\(data.count) bytes")
        #expect(AndroidAppStore.isWholePNG(data))
    }

    @Test func aPictureThatDoesNotDecodeWholeIsAnErrorAndAskedForAgain() async throws {
        let cut = app("cut"), junk = app("junk"), empty = app("empty")
        let whole = transparent(side: 192)
        let result = await answers([
            cut.id: .png(whole.prefix(whole.count - 40), hasAlpha: true),
            junk.id: .png(Data(repeating: 0x42, count: 600), hasAlpha: false),
            empty.id: .png(Data(), hasAlpha: false),
        ])
        #expect(result[cut.id] == .error, "a truncated picture is no icon")
        #expect(result[junk.id] == .error)
        #expect(result[empty.id] == .error)
    }

    @Test func aPictureThatIsNotSquareOrNotOfTheToolsSizeIsAnError() async throws {
        let wide = app("wide"), big = app("big")
        let result = await answers([
            wide.id: .png(transparent(side: 192, height: 96), hasAlpha: true),
            big.id: .png(transparent(side: 256), hasAlpha: true),
        ])
        #expect(result[wide.id] == .error)
        #expect(result[big.id] == .error)
    }

    @Test func theOtherAnswersPassThroughAsTheyAre() async throws {
        let one = app("one"), two = app("two"), three = app("three"), four = app("four")
        let result = await answers([one.id: .noIcon, two.id: .missing, three.id: .deferred, four.id: .error])
        #expect(result == [one.id: .noIcon, two.id: .missing, three.id: .deferred, four.id: .error])
    }
}
