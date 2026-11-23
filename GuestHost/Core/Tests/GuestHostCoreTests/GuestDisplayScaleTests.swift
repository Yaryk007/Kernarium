// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation
import Testing

@testable import GuestHostCore

struct GuestDisplayScaleTests {
    private let phone = CGSize(width: 402, height: 778)

    @Test func theLayoutIsThePointsTimesTheFactorRoundedDown() throws {
        let native = try #require(GuestDisplayLayout(scale: .native, points: phone, nativeScale: 3))
        #expect((native.width, native.height, native.dotsPerInch) == (1206, 2334, 480))
        let middle = try #require(GuestDisplayLayout(scale: .factor(1.5), points: phone, nativeScale: 3))
        #expect((middle.width, middle.height, middle.dotsPerInch) == (603, 1167, 240))
        let points = try #require(GuestDisplayLayout(scale: .points, points: phone, nativeScale: 3))
        #expect((points.width, points.height, points.dotsPerInch) == (402, 778, 160))
        // A fraction of a pixel is dropped on each axis, not carried over.
        let odd = try #require(GuestDisplayLayout(
            scale: .native, points: CGSize(width: 402, height: 778), nativeScale: 2.61))
        #expect((odd.width, odd.height) == (1049, 2030))
    }

    @Test func theDensityIs160DotsToEachPointOfScaleRounded() throws {
        for (scale, dpi) in [(1.0, 160), (1.5, 240), (2.0, 320), (2.5, 400), (2.3, 368), (2.61, 418)] {
            let layout = try #require(GuestDisplayLayout(scale: .factor(scale), points: phone, nativeScale: 3))
            #expect(layout.dotsPerInch == dpi, "\(scale)")
            #expect(GuestDisplayLayout.dotsPerInch(factor: scale) == dpi, "\(scale)")
        }
        // Native is the screen's own scale, whatever number that is.
        let layout = try #require(GuestDisplayLayout(scale: .native, points: phone, nativeScale: 2.61))
        #expect(layout.dotsPerInch == 418)
    }

    @Test func theInterfaceKeepsItsSizeInDensityIndependentPixels() throws {
        // The width in dp is what Android lays its interface out in; every
        // choice of one screen stays within one dp of the others.
        let widths = try [1.0, 1.5, 2.0, 2.5, 2.61].map { factor in
            try #require(GuestDisplayLayout(scale: .factor(factor), points: phone, nativeScale: 2.61)).dpWidth
        }
        for width in widths { #expect(abs(width - 402) <= 1, "\(width)") }
        let heights = try [1.0, 1.5, 2.0, 2.5, 2.61].map { factor in
            try #require(GuestDisplayLayout(scale: .factor(factor), points: phone, nativeScale: 2.61)).dpHeight
        }
        for height in heights { #expect(abs(height - 778) <= 1, "\(height)") }
    }

    @Test func nativeFollowsTheScreenAndAFactorDoesNot() throws {
        let twice = try #require(GuestDisplayLayout(scale: .native, points: phone, nativeScale: 2))
        #expect((twice.width, twice.dotsPerInch) == (804, 320))
        let thrice = try #require(GuestDisplayLayout(scale: .native, points: phone, nativeScale: 3))
        #expect((thrice.width, thrice.dotsPerInch) == (1206, 480))
        let fixed = try #require(GuestDisplayLayout(scale: .factor(1.5), points: phone, nativeScale: 2))
        #expect((fixed.width, fixed.dotsPerInch) == (603, 240))
    }

    @Test func aDisplayWithoutAPixelOrANumberHasNoLayout() {
        #expect(GuestDisplayLayout(scale: .native, points: .zero, nativeScale: 3) == nil)
        #expect(GuestDisplayLayout(scale: .native, points: CGSize(width: 0.9, height: 100), nativeScale: 1) == nil)
        #expect(GuestDisplayLayout(scale: .native, points: phone, nativeScale: 0) == nil)
        #expect(GuestDisplayLayout(scale: .native, points: phone, nativeScale: .nan) == nil)
        #expect(GuestDisplayLayout(scale: .factor(-1), points: phone, nativeScale: 3) == nil)
        #expect(GuestDisplayLayout(scale: .points, points: CGSize(width: Double.infinity, height: 10), nativeScale: 3) == nil)
    }

    @Test func aTextStatesNativeOrANumber() {
        #expect(GuestDisplayScale(text: "native") == .native)
        #expect(GuestDisplayScale(text: " Native\n") == .native)
        #expect(GuestDisplayScale(text: "1") == .factor(1))
        #expect(GuestDisplayScale(text: "1.5") == .factor(1.5))
        #expect(GuestDisplayScale(text: "2.61") == .factor(2.61))
        for bad in ["", "0", "-1", "nan", "inf", "9", "1,5", "1.5x", "sharpest", "retina"] {
            #expect(GuestDisplayScale(text: bad) == nil, "\(bad)")
        }
    }

    @Test func theTextReadsBackAsTheSameScale() {
        for scale in [GuestDisplayScale.native, .points, .factor(1.5), .factor(2.61), .factor(2.3), .factor(0.5)] {
            #expect(GuestDisplayScale(text: scale.text) == scale, "\(scale.text)")
        }
        #expect(GuestDisplayScale.native.text == "native")
        #expect(GuestDisplayScale.factor(1).text == "1")
        #expect(GuestDisplayScale.factor(1.5).text == "1.5")
        #expect(GuestDisplayScale.factor(2.61).text == "2.61")
    }

    @Test func theStoredFormIsTheSymbolOrTheNumber() throws {
        #expect(String(decoding: try JSONEncoder().encode([GuestDisplayScale.native]), as: UTF8.self) == #"["native"]"#)
        #expect(String(decoding: try JSONEncoder().encode([GuestDisplayScale.factor(1.5)]), as: UTF8.self) == "[1.5]")
        let decoder = JSONDecoder()
        #expect(try decoder.decode([GuestDisplayScale].self, from: Data(#"["native",1,1.5,"2"]"#.utf8))
            == [.native, .factor(1), .factor(1.5), .factor(2)])
        #expect(throws: DecodingError.self) { try decoder.decode([GuestDisplayScale].self, from: Data(#"["sharpest"]"#.utf8)) }
        #expect(throws: DecodingError.self) { try decoder.decode([GuestDisplayScale].self, from: Data("[0]".utf8)) }
        #expect(throws: DecodingError.self) { try decoder.decode([GuestDisplayScale].self, from: Data("[true]".utf8)) }
    }
}
