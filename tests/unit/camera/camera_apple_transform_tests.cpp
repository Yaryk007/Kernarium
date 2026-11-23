// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Apple host's accelerated crop, rotate and scale held to the portable
// transform: a copy and a quarter turn are exact, and a scale agrees within the
// resampling filters' difference. The scaler of the system takes a picture that
// arrives as a pixel buffer (what the camera delivers); the same picture in
// memory takes vImage, and both are checked.
#include "apple/accelerated_nv12.hpp"
#include "require.hpp"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

struct Picture {
    std::uint32_t width, height;
    std::size_t lumaStride, chromaStride;
    std::vector<std::uint8_t> luma, chroma;
    Picture(std::uint32_t w, std::uint32_t h, std::size_t pad = 0)
        : width(w), height(h), lumaStride(w + pad), chromaStride(w + pad), luma(lumaStride * h), chroma(chromaStride * (h / 2))
    {
    }
    [[nodiscard]] Nv12Source source(const void* native = nullptr) const
    {
        return {luma.data(), lumaStride, chroma.data(), chromaStride, width, height, native};
    }
    [[nodiscard]] Nv12Target target()
    {
        return {luma.data(), lumaStride, chroma.data(), chromaStride, width, height};
    }
};

// Every sample is its own position, so a misplaced one shows.
Picture ramp(std::uint32_t width, std::uint32_t height, std::size_t pad = 0)
{
    Picture picture(width, height, pad);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) picture.luma[y * picture.lumaStride + x] = std::uint8_t(x * 7 + y * 13);
    for (std::uint32_t y = 0; y < height / 2; ++y)
        for (std::uint32_t x = 0; x < width / 2; ++x) {
            picture.chroma[y * picture.chromaStride + x * 2] = std::uint8_t(30 + x * 3 + y * 5);
            picture.chroma[y * picture.chromaStride + x * 2 + 1] = std::uint8_t(200 - x * 2 - y * 4);
        }
    return picture;
}

// A smooth picture, which two scaling filters agree on.
Picture smooth(std::uint32_t width, std::uint32_t height)
{
    Picture picture(width, height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            picture.luma[y * picture.lumaStride + x] = std::uint8_t(30 + 150 * x / width + 60 * y / height);
    for (std::uint32_t y = 0; y < height / 2; ++y)
        for (std::uint32_t x = 0; x < width / 2; ++x) {
            picture.chroma[y * picture.chromaStride + x * 2] = std::uint8_t(60 + 120 * x * 2 / width);
            picture.chroma[y * picture.chromaStride + x * 2 + 1] = std::uint8_t(190 - 100 * y * 2 / height);
        }
    return picture;
}

struct Output {
    std::vector<std::uint8_t> luma, chroma;
    std::uint32_t width, height;
    Output(std::uint32_t w, std::uint32_t h) : luma(std::size_t(w) * h), chroma(std::size_t(w) * h / 2), width(w), height(h) {}
    Nv12Target target() { return {luma.data(), width, chroma.data(), width, width, height}; }
};

struct Difference {
    double lumaMean {};
    int lumaMax {};
    double chromaMean {};
    int chromaMax {};
};

Difference compare(const Output& a, const Output& b)
{
    require(a.width == b.width && a.height == b.height);
    Difference difference;
    double lumaSum = 0, chromaSum = 0;
    for (std::size_t index = 0; index < a.luma.size(); ++index) {
        const int d = std::abs(int(a.luma[index]) - int(b.luma[index]));
        lumaSum += d;
        difference.lumaMax = std::max(difference.lumaMax, d);
    }
    for (std::size_t index = 0; index < a.chroma.size(); ++index) {
        const int d = std::abs(int(a.chroma[index]) - int(b.chroma[index]));
        chromaSum += d;
        difference.chromaMax = std::max(difference.chromaMax, d);
    }
    difference.lumaMean = lumaSum / double(a.luma.size());
    difference.chromaMean = chromaSum / double(a.chroma.size());
    return difference;
}

// The picture as a pixel buffer, locked, so it can be read through memory too.
class Surface {
public:
    explicit Surface(const Picture& picture)
    {
        CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                      &kCFTypeDictionaryValueCallBacks);
        CFDictionaryRef properties = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, properties);
        require(CVPixelBufferCreate(kCFAllocatorDefault, picture.width, picture.height,
                                    kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attributes, &buffer_) == kCVReturnSuccess);
        CFRelease(properties);
        CFRelease(attributes);
        require(CVPixelBufferLockBaseAddress(buffer_, 0) == kCVReturnSuccess);
        auto* luma = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer_, 0));
        auto* chroma = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer_, 1));
        lumaStride_ = CVPixelBufferGetBytesPerRowOfPlane(buffer_, 0);
        chromaStride_ = CVPixelBufferGetBytesPerRowOfPlane(buffer_, 1);
        for (std::uint32_t y = 0; y < picture.height; ++y)
            std::copy_n(&picture.luma[y * picture.lumaStride], picture.width, luma + y * lumaStride_);
        for (std::uint32_t y = 0; y < picture.height / 2; ++y)
            std::copy_n(&picture.chroma[y * picture.chromaStride], picture.width, chroma + y * chromaStride_);
        width_ = picture.width;
        height_ = picture.height;
        luma_ = luma;
        chroma_ = chroma;
    }
    ~Surface()
    {
        CVPixelBufferUnlockBaseAddress(buffer_, 0);
        CFRelease(buffer_);
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    [[nodiscard]] Nv12Source source() const
    {
        return {luma_, lumaStride_, chroma_, chromaStride_, width_, height_, buffer_};
    }

private:
    CVPixelBufferRef buffer_ {};
    std::uint8_t* luma_ {};
    std::uint8_t* chroma_ {};
    std::size_t lumaStride_ {}, chromaStride_ {};
    std::uint32_t width_ {}, height_ {};
};

// A copy and a quarter turn move samples and change none, so they are exact,
// in a picture with padding at the end of each row and a crop inside it.
void exactMoves()
{
    auto accelerated = makeAcceleratedNv12Transformer();
    auto portable = makePortableNv12Transformer();
    const auto picture = ramp(64, 48, 16);
    for (const std::uint32_t rotation : {0u, 90u, 180u, 270u}) {
        for (const Nv12Rect crop : {Nv12Rect {0, 0, 64, 48}, Nv12Rect {8, 4, 32, 24}}) {
            const bool quarter = rotation == 90 || rotation == 270;
            Output expected(quarter ? crop.height : crop.width, quarter ? crop.width : crop.height);
            Output actual(expected.width, expected.height);
            require(portable->transform(picture.source(), crop, rotation, expected.target()));
            require(accelerated->transform(picture.source(), crop, rotation, actual.target()));
            require(actual.luma == expected.luma);
            require(actual.chroma == expected.chroma);
        }
    }
}

// A scale in memory (vImage) agrees with the portable one on smooth content.
void scaledInMemory()
{
    auto accelerated = makeAcceleratedNv12Transformer();
    auto portable = makePortableNv12Transformer();
    const auto picture = smooth(1280, 720);
    struct Case {
        Nv12Rect crop;
        std::uint32_t rotation, width, height;
    };
    for (const Case c : {Case {{0, 0, 1280, 720}, 0, 640, 360}, Case {{160, 0, 960, 720}, 0, 320, 240},
                         Case {{0, 0, 1280, 720}, 90, 360, 640}, Case {{0, 0, 1280, 720}, 180, 640, 360},
                         Case {{0, 0, 1280, 720}, 270, 180, 320}, Case {{0, 0, 640, 360}, 0, 1280, 720}}) {
        Output expected(c.width, c.height);
        Output actual(c.width, c.height);
        require(portable->transform(picture.source(), c.crop, c.rotation, expected.target()));
        require(accelerated->transform(picture.source(), c.crop, c.rotation, actual.target()));
        const auto difference = compare(expected, actual);
        std::printf("memory %ux%u rot %u: luma %.2f/%d chroma %.2f/%d\n", c.width, c.height, c.rotation, difference.lumaMean,
                    difference.lumaMax, difference.chromaMean, difference.chromaMax);
        require(difference.lumaMean < 2.0 && difference.lumaMax < 24);
        require(difference.chromaMean < 2.0 && difference.chromaMax < 24);
    }
}

// The same through the system's scaler, with a crop and a quarter turn.
void scaledBySystem()
{
    auto accelerated = makeAcceleratedNv12Transformer();
    auto portable = makePortableNv12Transformer();
    const auto picture = smooth(1920, 1080);
    const Surface surface(picture);
    struct Case {
        Nv12Rect crop;
        std::uint32_t rotation, width, height;
    };
    for (const Case c : {Case {{0, 0, 1920, 1080}, 0, 1280, 720}, Case {{240, 0, 1440, 1080}, 0, 640, 480},
                         Case {{0, 0, 1920, 1080}, 90, 720, 1280}, Case {{0, 0, 1920, 1080}, 180, 1280, 720},
                         Case {{0, 0, 1920, 1080}, 270, 540, 960}, Case {{240, 0, 1440, 1080}, 90, 480, 640}}) {
        Output expected(c.width, c.height);
        Output actual(c.width, c.height);
        require(portable->transform(picture.source(), c.crop, c.rotation, expected.target()));
        require(accelerated->transform(surface.source(), c.crop, c.rotation, actual.target()));
        const auto difference = compare(expected, actual);
        std::printf("system %ux%u rot %u: luma %.2f/%d chroma %.2f/%d\n", c.width, c.height, c.rotation, difference.lumaMean,
                    difference.lumaMax, difference.chromaMean, difference.chromaMax);
        require(difference.lumaMean < 2.5 && difference.lumaMax < 32);
        require(difference.chromaMean < 2.5 && difference.chromaMax < 32);
    }
}

void refusals()
{
    auto accelerated = makeAcceleratedNv12Transformer();
    const auto picture = ramp(64, 48);
    Output out(32, 24);
    require(!accelerated->transform(picture.source(), {0, 0, 64, 48}, 45, out.target()));
    require(!accelerated->transform(picture.source(), {1, 0, 62, 48}, 0, out.target()));
    require(!accelerated->transform(picture.source(), {0, 0, 80, 48}, 0, out.target()));
    Output odd(31, 24);
    require(!accelerated->transform(picture.source(), {0, 0, 64, 48}, 0, odd.target()));
}

} // namespace

int main()
{
    exactMoves();
    scaledInMemory();
    scaledBySystem();
    refusals();
    std::puts("camera-apple-transform ok");
    return 0;
}
