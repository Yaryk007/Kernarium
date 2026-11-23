// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Crop, rotate and scale of an 8-bit 4:2:0 picture whose chroma is interleaved
// (NV12). The camera device uses it to turn the capture's frame into the size
// and orientation a guest stream asked for.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace uml::platform {

struct Nv12Source {
    const std::uint8_t* luma {};
    std::size_t lumaStride {};
    const std::uint8_t* chroma {};
    std::size_t chromaStride {};
    std::uint32_t width {};
    std::uint32_t height {};
    // The platform's own handle to the same picture (the Apple camera's
    // CVPixelBufferRef), which the transformer of that platform may use to let
    // the system's scaler do the work. Null for a picture that exists only in
    // memory, and never read by another platform's transformer.
    const void* native {};
};

struct Nv12Target {
    std::uint8_t* luma {};
    std::size_t lumaStride {};
    std::uint8_t* chroma {};
    std::size_t chromaStride {};
    std::uint32_t width {};
    std::uint32_t height {};
};

// A rectangle of a picture, in luma samples. A crop of NV12 starts and ends on
// even samples.
struct Nv12Rect {
    std::uint32_t x {};
    std::uint32_t y {};
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const Nv12Rect&, const Nv12Rect&) = default;
};

// The largest centred rectangle of a `width` x `height` picture that has the
// aspect ratio of `targetWidth` x `targetHeight`, on even samples. A target
// that is turned a quarter (`quarterTurn`) takes the aspect of the picture the
// rotation will make of the crop.
[[nodiscard]] Nv12Rect centredCrop(std::uint32_t width, std::uint32_t height, std::uint32_t targetWidth,
                                   std::uint32_t targetHeight, bool quarterTurn = false) noexcept;

// Writes `crop` of `source`, turned clockwise by `rotation` degrees (0, 90, 180
// or 270) and scaled to the target's size, into `target`. A downscale averages
// the samples under each target sample; an upscale interpolates. Returns false
// for a rotation that is not a quarter turn, a crop outside the picture, a
// target that is empty or odd, or a stride shorter than a row.
[[nodiscard]] bool transformNv12(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation,
                                 const Nv12Target& target);

// Whether transformNv12() accepts the arguments: the checks every transformer
// makes before it touches a sample.
[[nodiscard]] bool validNv12Transform(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation,
                                      const Nv12Target& target) noexcept;

// A row of an NV12 plane turned half around: the `samples` bytes of a luma row, or the `pairs` interleaved Cb, Cr pairs
// of a chroma row (a pair keeps its order), last first, into `out`. The two rows do not overlap. A half turn of a
// picture is each row so turned, taken from the last row up, which is what a transform with rotation 180 does without
// the scaling.
void reverseLumaRow(const std::uint8_t* row, std::uint8_t* out, std::size_t samples) noexcept;
void reverseChromaRow(const std::uint8_t* row, std::uint8_t* out, std::size_t pairs) noexcept;

// A plane of 8-bit samples turned half around into another plane of the same size: `width` samples a row (luma), or
// that many interleaved Cb, Cr pairs (`pairs`, chroma), for `height` rows. Each row of the target is a row of the source
// reversed, from the last row up. The planes do not overlap and each stride holds a row.
void turnPlaneHalf(const std::uint8_t* source, std::size_t sourceStride, std::uint8_t* target, std::size_t targetStride,
                   std::uint32_t width, std::uint32_t height, bool pairs) noexcept;

// A picture of black: luma 0 and neutral chroma, which full-range YCbCr takes
// for black.
void fillNv12Black(const Nv12Target& target) noexcept;

// Something that does transformNv12()'s work. A platform's camera supplies the
// fastest one it has (the Apple backend uses the system's accelerated scaling
// and rotation); the portable one is the reference the others are held to. An
// object keeps its scratch memory between calls and is used by one thread at a
// time.
class Nv12Transformer {
public:
    virtual ~Nv12Transformer() = default;
    [[nodiscard]] virtual bool transform(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation,
                                         const Nv12Target& target) = 0;
};

[[nodiscard]] std::unique_ptr<Nv12Transformer> makePortableNv12Transformer();

} // namespace uml::platform
