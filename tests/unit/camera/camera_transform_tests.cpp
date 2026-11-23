// SPDX-License-Identifier: GPL-2.0-or-later
#include "nv12_transform.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

struct Picture {
    std::uint32_t width, height;
    std::vector<std::uint8_t> luma, chroma;
    Picture(std::uint32_t w, std::uint32_t h) : width(w), height(h), luma(std::size_t(w) * h), chroma(std::size_t(w) * h / 2) {}
    Nv12Source source() const { return {luma.data(), width, chroma.data(), width, width, height}; }
    Nv12Target target() { return {luma.data(), width, chroma.data(), width, width, height}; }
};

// Each luma sample is its own position, so any misplacement shows.
Picture ramp(std::uint32_t width, std::uint32_t height)
{
    Picture picture(width, height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) picture.luma[std::size_t(y) * width + x] = std::uint8_t(y * 16 + x);
    for (std::uint32_t y = 0; y < height / 2; ++y)
        for (std::uint32_t x = 0; x < width / 2; ++x) {
            picture.chroma[std::size_t(y) * width + x * 2] = std::uint8_t(100 + y * 8 + x);
            picture.chroma[std::size_t(y) * width + x * 2 + 1] = std::uint8_t(200 + y * 8 + x);
        }
    return picture;
}

void crops()
{
    // A 16:9 target of a 4:3 picture takes the full width and the middle band.
    auto crop = centredCrop(640, 480, 1920, 1080);
    require(crop.width == 640 && crop.height == 360 && crop.x == 0 && crop.y == 60);
    // The reverse takes the full height and the middle columns.
    crop = centredCrop(1920, 1080, 640, 480);
    require(crop.width == 1440 && crop.height == 1080 && crop.x == 240 && crop.y == 0);
    // Equal aspect is the whole picture.
    crop = centredCrop(1280, 720, 640, 360);
    require(crop == Nv12Rect({0, 0, 1280, 720}));
    // A quarter turn swaps the target's aspect: a portrait target of a landscape picture.
    crop = centredCrop(1920, 1080, 1080, 1920, true);
    require(crop.width == 1920 && crop.height == 1080);
    crop = centredCrop(1920, 1080, 1080, 1920, false);
    require(crop.width == 606 && crop.height == 1080);
    // Everything is even.
    crop = centredCrop(1001, 777, 5, 3);
    require(crop.x % 2 == 0 && crop.y % 2 == 0 && crop.width % 2 == 0 && crop.height % 2 == 0);
    require(centredCrop(0, 10, 4, 4) == Nv12Rect {});
}

void identityAndRotation()
{
    const auto source = ramp(8, 4);
    Picture same(8, 4);
    require(transformNv12(source.source(), {0, 0, 8, 4}, 0, same.target()));
    require(same.luma == source.luma && same.chroma == source.chroma);

    // Clockwise 90: the source's left column becomes the top row, read bottom to top.
    Picture turned(4, 8);
    require(transformNv12(source.source(), {0, 0, 8, 4}, 90, turned.target()));
    for (std::uint32_t v = 0; v < 8; ++v)
        for (std::uint32_t u = 0; u < 4; ++u)
            require(turned.luma[std::size_t(v) * 4 + u] == source.luma[std::size_t(4 - 1 - u) * 8 + v]);
    // Chroma pairs stay Cb then Cr.
    for (std::uint32_t v = 0; v < 4; ++v)
        for (std::uint32_t u = 0; u < 2; ++u) {
            require(turned.chroma[std::size_t(v) * 4 + u * 2] == source.chroma[std::size_t(2 - 1 - u) * 8 + v * 2]);
            require(turned.chroma[std::size_t(v) * 4 + u * 2 + 1] == source.chroma[std::size_t(2 - 1 - u) * 8 + v * 2 + 1]);
        }
    // 180 reverses both axes; 270 undoes 90.
    Picture flipped(8, 4);
    require(transformNv12(source.source(), {0, 0, 8, 4}, 180, flipped.target()));
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 8; ++x)
            require(flipped.luma[std::size_t(y) * 8 + x] == source.luma[std::size_t(3 - y) * 8 + (7 - x)]);
    Picture back(8, 4);
    require(transformNv12(turned.source(), {0, 0, 4, 8}, 270, back.target()));
    require(back.luma == source.luma && back.chroma == source.chroma);
}

// A half turn of a picture the rows' way, which the camera device writes into the guest's rows without a second pass: each
// row reversed, from the last row up, is the transform's rotation of 180 degrees, whatever the width (the vector loops and
// the tail of a row both).
void aRowReversedIsTheHalfTurn()
{
    for (const std::uint32_t width : {2u, 4u, 6u, 8u, 18u, 30u, 34u, 66u, 130u, 258u}) {
        const std::uint32_t height = 6;
        Picture source(width, height);
        for (std::size_t i = 0; i < source.luma.size(); ++i) source.luma[i] = std::uint8_t((i * 7 + i / 13) ^ 0x35);
        for (std::size_t i = 0; i < source.chroma.size(); ++i) source.chroma[i] = std::uint8_t((i * 11 + i / 5) ^ 0xa7);
        Picture reference(width, height);
        require(transformNv12(source.source(), {0, 0, width, height}, 180, reference.target()));
        Picture rows(width, height);
        for (std::uint32_t row = 0; row < height; ++row)
            reverseLumaRow(source.luma.data() + std::size_t(height - 1 - row) * width, rows.luma.data() + std::size_t(row) * width, width);
        for (std::uint32_t row = 0; row < height / 2; ++row)
            reverseChromaRow(source.chroma.data() + std::size_t(height / 2 - 1 - row) * width,
                             rows.chroma.data() + std::size_t(row) * width, width / 2);
        require(rows.luma == reference.luma, "the luma rows are the half turn's");
        require(rows.chroma == reference.chroma, "so are the chroma rows, each pair kept in order");
    }
    // A row of no samples has nothing to turn.
    std::uint8_t spare = 0x5a;
    reverseLumaRow(&spare, &spare, 0);
    reverseChromaRow(&spare, &spare, 0);
    require(spare == 0x5a);
}

void aPlaneTurnedHalfAroundIsTheTransformsHalfTurn()
{
    for (const std::uint32_t width : {2u, 8u, 18u, 34u, 130u}) {
        const std::uint32_t height = 8;
        Picture source(width, height);
        for (std::size_t i = 0; i < source.luma.size(); ++i) source.luma[i] = std::uint8_t((i * 5 + i / 7) ^ 0x6b);
        for (std::size_t i = 0; i < source.chroma.size(); ++i) source.chroma[i] = std::uint8_t((i * 3 + i / 9) ^ 0xc2);
        Picture reference(width, height);
        require(transformNv12(source.source(), {0, 0, width, height}, 180, reference.target()));
        // Planes with a stride wider than the row, as a photo's pixel buffer has.
        const std::size_t stride = width + 16;
        std::vector<std::uint8_t> lumaIn(stride * height, 0xee), chromaIn(stride * (height / 2), 0xee);
        for (std::uint32_t row = 0; row < height; ++row)
            std::copy_n(source.luma.data() + std::size_t(row) * width, width, lumaIn.data() + row * stride);
        for (std::uint32_t row = 0; row < height / 2; ++row)
            std::copy_n(source.chroma.data() + std::size_t(row) * width, width, chromaIn.data() + row * stride);
        std::vector<std::uint8_t> lumaOut(stride * height, 0x11), chromaOut(stride * (height / 2), 0x11);
        turnPlaneHalf(lumaIn.data(), stride, lumaOut.data(), stride, width, height, false);
        turnPlaneHalf(chromaIn.data(), stride, chromaOut.data(), stride, width / 2, height / 2, true);
        for (std::uint32_t row = 0; row < height; ++row) {
            require(std::equal(reference.luma.begin() + std::size_t(row) * width, reference.luma.begin() + std::size_t(row + 1) * width,
                               lumaOut.begin() + row * stride), "each luma row is the half turn's");
            require(lumaOut[row * stride + width] == 0x11, "and nothing past the row is written");
        }
        for (std::uint32_t row = 0; row < height / 2; ++row)
            require(std::equal(reference.chroma.begin() + std::size_t(row) * width, reference.chroma.begin() + std::size_t(row + 1) * width,
                               chromaOut.begin() + row * stride), "each chroma row is, its pairs kept in order");
    }
}

void cropAndScale()
{
    const auto source = ramp(8, 8);
    // A crop at an offset, copied unscaled.
    Picture window(4, 4);
    require(transformNv12(source.source(), {2, 2, 4, 4}, 0, window.target()));
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) require(window.luma[std::size_t(y) * 4 + x] == source.luma[std::size_t(y + 2) * 8 + x + 2]);
    require(window.chroma[0] == source.chroma[1 * 8 + 1 * 2] && window.chroma[1] == source.chroma[1 * 8 + 1 * 2 + 1]);

    // A downscale by two averages 2 x 2 blocks.
    Picture half(4, 4);
    require(transformNv12(source.source(), {0, 0, 8, 8}, 0, half.target()));
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            const unsigned sum = source.luma[std::size_t(2 * y) * 8 + 2 * x] + source.luma[std::size_t(2 * y) * 8 + 2 * x + 1] +
                source.luma[std::size_t(2 * y + 1) * 8 + 2 * x] + source.luma[std::size_t(2 * y + 1) * 8 + 2 * x + 1];
            require(half.luma[std::size_t(y) * 4 + x] == (sum + 2) / 4);
        }

    // An upscale of a flat picture stays flat, and a ramp stays between its ends.
    Picture flat(4, 4);
    for (auto& v : flat.luma) v = 77;
    for (auto& v : flat.chroma) v = 99;
    Picture big(16, 12);
    require(transformNv12(flat.source(), {0, 0, 4, 4}, 0, big.target()));
    for (const auto v : big.luma) require(v == 77);
    for (const auto v : big.chroma) require(v == 99);
    Picture wide(8, 4);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) wide.luma[std::size_t(y) * 8 + x] = std::uint8_t(x * 30);
    Picture stretched(16, 4);
    require(transformNv12(wide.source(), {0, 0, 8, 4}, 0, stretched.target()));
    for (std::uint32_t x = 1; x < 16; ++x) require(stretched.luma[x] >= stretched.luma[x - 1]);
    require(stretched.luma[0] == 0 && stretched.luma[15] == 210);
}

void refusals()
{
    const auto source = ramp(8, 8);
    Picture target(4, 4);
    require(!transformNv12(source.source(), {0, 0, 8, 8}, 45, target.target()));
    require(!transformNv12(source.source(), {2, 2, 8, 8}, 0, target.target()));  // outside
    require(!transformNv12(source.source(), {1, 0, 4, 4}, 0, target.target()));  // odd
    Picture odd(3, 4);
    require(!transformNv12(source.source(), {0, 0, 8, 8}, 0, odd.target()));
    auto narrow = target.target();
    narrow.lumaStride = 2;
    require(!transformNv12(source.source(), {0, 0, 8, 8}, 0, narrow));
    auto empty = source.source();
    empty.luma = nullptr;
    require(!transformNv12(empty, {0, 0, 8, 8}, 0, target.target()));
}

void black()
{
    Picture picture(6, 4);
    for (auto& v : picture.luma) v = 200;
    for (auto& v : picture.chroma) v = 9;
    fillNv12Black(picture.target());
    for (const auto v : picture.luma) require(v == 0);
    for (const auto v : picture.chroma) require(v == 128);
}

} // namespace

int main()
{
    crops();
    identityAndRotation();
    aRowReversedIsTheHalfTurn();
    aPlaneTurnedHalfAroundIsTheTransformsHalfTurn();
    cropAndScale();
    refusals();
    black();
    return 0;
}
