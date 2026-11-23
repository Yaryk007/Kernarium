// SPDX-License-Identifier: GPL-2.0-or-later
#include "nv12_transform.hpp"

#include <algorithm>
#include <cstring>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace uml::platform {
namespace {

// 16.16 fixed point is enough for 16384-sample pictures and keeps the
// arithmetic exact for the usual integer ratios.
constexpr std::uint32_t fractionBits = 8;

struct Plane {
    const std::uint8_t* data;
    std::size_t stride;
    std::uint32_t width;   // in samples
    std::uint32_t height;
    std::uint32_t pixel;   // bytes per sample: 1 luma, 2 interleaved chroma
};

struct OutPlane {
    std::uint8_t* data;
    std::size_t stride;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pixel;
};

// Scales one plane. `pixel` interleaved components are scaled independently.
void scalePlane(const Plane& in, const OutPlane& out)
{
    const std::uint32_t channels = in.pixel;
    if (in.width == out.width && in.height == out.height) {
        for (std::uint32_t row = 0; row < out.height; ++row)
            std::memcpy(out.data + row * out.stride, in.data + row * in.stride,
                        std::size_t(out.width) * channels);
        return;
    }
    const bool shrinkX = in.width > out.width;
    const bool shrinkY = in.height > out.height;
    if (shrinkX || shrinkY) {
        // Area average: every output sample is the mean of the input samples
        // whose centres fall in its footprint.
        for (std::uint32_t oy = 0; oy < out.height; ++oy) {
            const auto y0 = std::uint64_t(oy) * in.height / out.height;
            const auto y1 = std::max<std::uint64_t>(y0 + 1, std::uint64_t(oy + 1) * in.height / out.height);
            for (std::uint32_t ox = 0; ox < out.width; ++ox) {
                const auto x0 = std::uint64_t(ox) * in.width / out.width;
                const auto x1 = std::max<std::uint64_t>(x0 + 1, std::uint64_t(ox + 1) * in.width / out.width);
                const std::uint64_t count = (y1 - y0) * (x1 - x0);
                for (std::uint32_t channel = 0; channel < channels; ++channel) {
                    std::uint64_t sum = 0;
                    for (auto y = y0; y < y1; ++y) {
                        const auto* row = in.data + y * in.stride + channel;
                        for (auto x = x0; x < x1; ++x) sum += row[x * channels];
                    }
                    out.data[oy * out.stride + std::size_t(ox) * channels + channel] =
                        std::uint8_t((sum + count / 2) / count);
                }
            }
        }
        return;
    }
    // Upscale: bilinear between sample centres.
    std::vector<std::uint32_t> xs(out.width), xw(out.width);
    for (std::uint32_t ox = 0; ox < out.width; ++ox) {
        const auto position = ((std::uint64_t(ox) * 2 + 1) * in.width << fractionBits) / (std::uint64_t(out.width) * 2);
        const auto shifted = position >= (1u << (fractionBits - 1)) ? position - (1u << (fractionBits - 1)) : 0;
        xs[ox] = std::min<std::uint32_t>(std::uint32_t(shifted >> fractionBits), in.width - 1);
        xw[ox] = xs[ox] + 1 < in.width ? std::uint32_t(shifted & ((1u << fractionBits) - 1)) : 0;
    }
    for (std::uint32_t oy = 0; oy < out.height; ++oy) {
        const auto position = ((std::uint64_t(oy) * 2 + 1) * in.height << fractionBits) / (std::uint64_t(out.height) * 2);
        const auto shifted = position >= (1u << (fractionBits - 1)) ? position - (1u << (fractionBits - 1)) : 0;
        const auto y0 = std::min<std::uint32_t>(std::uint32_t(shifted >> fractionBits), in.height - 1);
        const auto wy = y0 + 1 < in.height ? std::uint32_t(shifted & ((1u << fractionBits) - 1)) : 0;
        const auto* upper = in.data + std::size_t(y0) * in.stride;
        const auto* lower = y0 + 1 < in.height ? upper + in.stride : upper;
        auto* target = out.data + std::size_t(oy) * out.stride;
        for (std::uint32_t ox = 0; ox < out.width; ++ox) {
            const auto x0 = xs[ox];
            const auto x1 = x0 + 1 < in.width ? x0 + 1 : x0;
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const std::uint32_t a = upper[std::size_t(x0) * channels + channel];
                const std::uint32_t b = upper[std::size_t(x1) * channels + channel];
                const std::uint32_t c = lower[std::size_t(x0) * channels + channel];
                const std::uint32_t d = lower[std::size_t(x1) * channels + channel];
                const std::uint32_t top = a * ((1u << fractionBits) - xw[ox]) + b * xw[ox];
                const std::uint32_t bottom = c * ((1u << fractionBits) - xw[ox]) + d * xw[ox];
                const auto value = (top * ((1u << fractionBits) - wy) + bottom * wy + (1u << (2 * fractionBits - 1)))
                    >> (2 * fractionBits);
                target[std::size_t(ox) * channels + channel] = std::uint8_t(value);
            }
        }
    }
}

// Copies `rect` of a plane, turned clockwise by `rotation`, into a tight
// buffer. The rect is in samples of the plane.
std::vector<std::uint8_t> turnPlane(const Plane& in, std::uint32_t rx, std::uint32_t ry, std::uint32_t rw,
                                    std::uint32_t rh, std::uint32_t rotation, std::uint32_t& outWidth,
                                    std::uint32_t& outHeight)
{
    const std::uint32_t channels = in.pixel;
    const bool quarter = rotation == 90 || rotation == 270;
    outWidth = quarter ? rh : rw;
    outHeight = quarter ? rw : rh;
    std::vector<std::uint8_t> out(std::size_t(outWidth) * outHeight * channels);
    for (std::uint32_t v = 0; v < outHeight; ++v) {
        for (std::uint32_t u = 0; u < outWidth; ++u) {
            std::uint32_t sx, sy;
            switch (rotation) {
            case 90: sx = v; sy = rh - 1 - u; break;
            case 180: sx = rw - 1 - u; sy = rh - 1 - v; break;
            case 270: sx = rw - 1 - v; sy = u; break;
            default: sx = u; sy = v; break;
            }
            const auto* from = in.data + std::size_t(ry + sy) * in.stride + std::size_t(rx + sx) * channels;
            std::memcpy(out.data() + (std::size_t(v) * outWidth + u) * channels, from, channels);
        }
    }
    return out;
}

} // namespace

Nv12Rect centredCrop(std::uint32_t width, std::uint32_t height, std::uint32_t targetWidth,
                     std::uint32_t targetHeight, bool quarterTurn) noexcept
{
    if (quarterTurn) std::swap(targetWidth, targetHeight);
    if (width == 0 || height == 0 || targetWidth == 0 || targetHeight == 0) return {};
    std::uint64_t cropWidth = width;
    std::uint64_t cropHeight = height;
    // Compare width / height with targetWidth / targetHeight.
    if (std::uint64_t(width) * targetHeight > std::uint64_t(height) * targetWidth)
        cropWidth = std::uint64_t(height) * targetWidth / targetHeight;
    else
        cropHeight = std::uint64_t(width) * targetHeight / targetWidth;
    cropWidth = std::max<std::uint64_t>(cropWidth & ~std::uint64_t(1), 2);
    cropHeight = std::max<std::uint64_t>(cropHeight & ~std::uint64_t(1), 2);
    cropWidth = std::min<std::uint64_t>(cropWidth, width & ~1u);
    cropHeight = std::min<std::uint64_t>(cropHeight, height & ~1u);
    Nv12Rect rect;
    rect.width = std::uint32_t(cropWidth);
    rect.height = std::uint32_t(cropHeight);
    rect.x = ((width - rect.width) / 2) & ~1u;
    rect.y = ((height - rect.height) / 2) & ~1u;
    return rect;
}

bool validNv12Transform(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation, const Nv12Target& target) noexcept
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) return false;
    if (!source.luma || !source.chroma || !target.luma || !target.chroma) return false;
    if (target.width == 0 || target.height == 0 || (target.width & 1) || (target.height & 1)) return false;
    if (crop.width == 0 || crop.height == 0 || (crop.x & 1) || (crop.y & 1) || (crop.width & 1) || (crop.height & 1))
        return false;
    if (std::uint64_t(crop.x) + crop.width > source.width || std::uint64_t(crop.y) + crop.height > source.height)
        return false;
    if (source.lumaStride < source.width || source.chromaStride < source.width) return false;
    if (target.lumaStride < target.width || target.chromaStride < target.width) return false;
    return true;
}

bool transformNv12(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation, const Nv12Target& target)
{
    if (!validNv12Transform(source, crop, rotation, target)) return false;

    const Plane luma {source.luma, source.lumaStride, source.width, source.height, 1};
    const Plane chroma {source.chroma, source.chromaStride, source.width / 2, source.height / 2, 2};
    if (rotation == 0) {
        const Plane lumaCrop {source.luma + std::size_t(crop.y) * source.lumaStride + crop.x, source.lumaStride,
                              crop.width, crop.height, 1};
        const Plane chromaCrop {source.chroma + std::size_t(crop.y / 2) * source.chromaStride + std::size_t(crop.x / 2) * 2,
                                source.chromaStride, crop.width / 2, crop.height / 2, 2};
        scalePlane(lumaCrop, {target.luma, target.lumaStride, target.width, target.height, 1});
        scalePlane(chromaCrop, {target.chroma, target.chromaStride, target.width / 2, target.height / 2, 2});
        return true;
    }
    std::uint32_t turnedWidth = 0, turnedHeight = 0;
    const auto turnedLuma = turnPlane(luma, crop.x, crop.y, crop.width, crop.height, rotation, turnedWidth, turnedHeight);
    std::uint32_t turnedChromaWidth = 0, turnedChromaHeight = 0;
    const auto turnedChroma = turnPlane(chroma, crop.x / 2, crop.y / 2, crop.width / 2, crop.height / 2, rotation,
                                        turnedChromaWidth, turnedChromaHeight);
    scalePlane({turnedLuma.data(), turnedWidth, turnedWidth, turnedHeight, 1},
               {target.luma, target.lumaStride, target.width, target.height, 1});
    scalePlane({turnedChroma.data(), std::size_t(turnedChromaWidth) * 2, turnedChromaWidth, turnedChromaHeight, 2},
               {target.chroma, target.chromaStride, target.width / 2, target.height / 2, 2});
    return true;
}

namespace {

class PortableTransformer final : public Nv12Transformer {
public:
    bool transform(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation, const Nv12Target& target) override
    {
        return transformNv12(source, crop, rotation, target);
    }
};

} // namespace

std::unique_ptr<Nv12Transformer> makePortableNv12Transformer() { return std::make_unique<PortableTransformer>(); }

void reverseLumaRow(const std::uint8_t* row, std::uint8_t* out, std::size_t samples) noexcept
{
    std::size_t index = 0;
#if defined(__ARM_NEON)
    for (; index + 16 <= samples; index += 16) {
        uint8x16_t block = vld1q_u8(row + samples - 16 - index);
        block = vrev64q_u8(block);
        vst1q_u8(out + index, vextq_u8(block, block, 8));
    }
#endif
    for (; index < samples; ++index) out[index] = row[samples - 1 - index];
}

void reverseChromaRow(const std::uint8_t* row, std::uint8_t* out, std::size_t pairs) noexcept
{
    std::size_t index = 0;
#if defined(__ARM_NEON)
    // A pair is one 16-bit lane: the lanes are reversed, each pair keeps its bytes' order.
    for (; index + 8 <= pairs; index += 8) {
        uint16x8_t block = vreinterpretq_u16_u8(vld1q_u8(row + (pairs - 8 - index) * 2));
        block = vrev64q_u16(block);
        vst1q_u8(out + index * 2, vreinterpretq_u8_u16(vextq_u16(block, block, 4)));
    }
#endif
    for (; index < pairs; ++index) {
        out[index * 2] = row[(pairs - 1 - index) * 2];
        out[index * 2 + 1] = row[(pairs - 1 - index) * 2 + 1];
    }
}

void turnPlaneHalf(const std::uint8_t* source, std::size_t sourceStride, std::uint8_t* target, std::size_t targetStride,
                   std::uint32_t width, std::uint32_t height, bool pairs) noexcept
{
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* from = source + std::size_t(height - 1 - row) * sourceStride;
        std::uint8_t* to = target + std::size_t(row) * targetStride;
        if (pairs) reverseChromaRow(from, to, width);
        else reverseLumaRow(from, to, width);
    }
}

void fillNv12Black(const Nv12Target& target) noexcept
{
    for (std::uint32_t row = 0; row < target.height; ++row)
        std::memset(target.luma + std::size_t(row) * target.lumaStride, 0, target.width);
    for (std::uint32_t row = 0; row < target.height / 2; ++row)
        std::memset(target.chroma + std::size_t(row) * target.chromaStride, 128, target.width);
}

} // namespace uml::platform
