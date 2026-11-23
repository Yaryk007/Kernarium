// SPDX-License-Identifier: GPL-2.0-or-later
#include "accelerated_nv12.hpp"

#include <Accelerate/Accelerate.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace uml::platform {
namespace {

// One thread does a frame's work. A frame is already the best work unit of the
// transport worker, and vImage's own tiling would put a second set of busy
// threads beside the guest's.
constexpr vImage_Flags scaleFlags = kvImageDoNotTile;
constexpr vImage_Flags rotateFlags = kvImageDoNotTile;
// The scaler of the system takes the frames that need real work. A smaller
// picture costs vImage less than a trip to the scaler costs the worker.
constexpr std::uint64_t scalerMinimumSourceArea = 1'000'000;
// The scaler is given up for good after this many failures in a row.
constexpr unsigned scalerFailureLimit = 3;

template<class T> class Held {
public:
    Held() = default;
    explicit Held(T value) noexcept : value_(value) {}
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Held& operator=(Held&& other) noexcept
    {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    ~Held() { reset(); }
    void reset() noexcept
    {
        if (value_) CFRelease(value_);
        value_ = nullptr;
    }
    [[nodiscard]] T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    T value_ {};
};

// A plane of 1-byte samples (luma) or of 2-byte pairs (interleaved Cb, Cr). The
// sizes are in samples, a pair counting as one.
struct Plane {
    const std::uint8_t* data;
    std::size_t stride;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bytes;
};

struct OutPlane {
    std::uint8_t* data;
    std::size_t stride;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bytes;
};

vImage_Buffer bufferOf(const Plane& plane) noexcept
{
    return {const_cast<std::uint8_t*>(plane.data), plane.height, plane.width, plane.stride};
}

vImage_Buffer bufferOf(const OutPlane& plane) noexcept
{
    return {plane.data, plane.height, plane.width, plane.stride};
}

// ----------------------------------------------------------------------------
// The system's scaler (VideoToolbox), for frames that arrive as pixel buffers
// ----------------------------------------------------------------------------

// The output is full-range BT.709 NV12, which is what the camera delivers and
// what the guest is told; tagging both sides the same keeps the scaler from
// converting between matrices.
void tagBt709(CVBufferRef buffer) noexcept
{
    CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
}

Held<CVPixelBufferRef> makeSurfaceBuffer(std::uint32_t width, std::uint32_t height)
{
    Held<CFDictionaryRef> properties(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    Held<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!properties || !attributes) return {};
    CFDictionarySetValue(attributes.get(), kCVPixelBufferIOSurfacePropertiesKey, properties.get());
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                            attributes.get(), &buffer) != kCVReturnSuccess)
        return {};
    Held<CVPixelBufferRef> result(buffer);
    tagBt709(buffer);
    return result;
}

Held<CFNumberRef> number(double value) noexcept
{
    return Held<CFNumberRef>(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value));
}

class Scaler {
public:
    Scaler() = default;

    [[nodiscard]] bool usable() const noexcept { return failures_ < scalerFailureLimit; }
    void failed() noexcept { ++failures_; }
    void succeeded() noexcept { failures_ = 0; }

    // Crops `source` (a pixel buffer) to `crop` and scales it to `width` x
    // `height`; the picture ends in `result()`, which the next call reuses.
    [[nodiscard]] bool run(CVPixelBufferRef source, std::uint32_t sourceWidth, std::uint32_t sourceHeight, Nv12Rect crop,
                           std::uint32_t width, std::uint32_t height)
    {
        if (!ensureSession()) return false;
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(source);
        if (surface == nullptr) return false;
        Held<CVPixelBufferRef> view;
        {
            CVPixelBufferRef created = nullptr;
            if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr, &created) != kCVReturnSuccess)
                return false;
            view = Held<CVPixelBufferRef>(created);
        }
        tagBt709(view.get());
        // The clean aperture is the crop, set on every frame even when the crop
        // is the whole picture: CoreVideo mirrors an attachment into the pixel
        // buffer's surface, and the camera reuses its surfaces, so one that was
        // cropped before would crop again.
        Held<CFMutableDictionaryRef> aperture(CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        if (!aperture) return false;
        const auto w = number(crop.width), h = number(crop.height);
        const auto x = number(double(crop.x) + crop.width / 2.0 - sourceWidth / 2.0);
        const auto y = number(double(crop.y) + crop.height / 2.0 - sourceHeight / 2.0);
        if (!w || !h || !x || !y) return false;
        CFDictionarySetValue(aperture.get(), kCVImageBufferCleanApertureWidthKey, w.get());
        CFDictionarySetValue(aperture.get(), kCVImageBufferCleanApertureHeightKey, h.get());
        CFDictionarySetValue(aperture.get(), kCVImageBufferCleanApertureHorizontalOffsetKey, x.get());
        CFDictionarySetValue(aperture.get(), kCVImageBufferCleanApertureVerticalOffsetKey, y.get());
        CVBufferSetAttachment(view.get(), kCVImageBufferCleanApertureKey, aperture.get(), kCVAttachmentMode_ShouldPropagate);
        if (!scaled_ || scaledWidth_ != width || scaledHeight_ != height) {
            scaled_ = makeSurfaceBuffer(width, height);
            scaledWidth_ = width;
            scaledHeight_ = height;
            if (!scaled_) return false;
        }
        return VTPixelTransferSessionTransferImage(transfer_.get(), view.get(), scaled_.get()) == noErr;
    }

    [[nodiscard]] CVPixelBufferRef result() const noexcept { return scaled_.get(); }

private:
    bool ensureSession()
    {
        if (transfer_) return true;
        VTPixelTransferSessionRef transfer = nullptr;
        if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &transfer) != noErr) return false;
        transfer_ = Held<VTPixelTransferSessionRef>(transfer);
        if (VTSessionSetProperty(transfer_.get(), kVTPixelTransferPropertyKey_ScalingMode,
                                 kVTScalingMode_CropSourceToCleanAperture) != noErr) {
            transfer_.reset();
            return false;
        }
        return true;
    }

    Held<VTPixelTransferSessionRef> transfer_;
    Held<CVPixelBufferRef> scaled_;
    std::uint32_t scaledWidth_ {};
    std::uint32_t scaledHeight_ {};
    unsigned failures_ {};
};

// ----------------------------------------------------------------------------
// The transformer
// ----------------------------------------------------------------------------

class AcceleratedTransformer final : public Nv12Transformer {
public:
    bool transform(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation, const Nv12Target& target) override
    {
        if (!validNv12Transform(source, crop, rotation, target)) return false;
        const bool quarter = rotation == 90 || rotation == 270;
        const bool scaling = crop.width != (quarter ? target.height : target.width) ||
            crop.height != (quarter ? target.width : target.height);
        // A copy and a quarter turn are vImage's alone: they cost less than a
        // trip to the scaler. A scale of a picture that has a pixel buffer goes
        // to the scaler, which does it for a tenth of the worker's time.
        if (scaling && source.native != nullptr && scaler_.usable() &&
            std::uint64_t(crop.width) * crop.height >= scalerMinimumSourceArea) {
            if (viaScaler(source, crop, rotation, target)) {
                scaler_.succeeded();
                return true;
            }
            scaler_.failed();
        }
        return viaVimage(source, crop, rotation, target);
    }

private:
    bool viaScaler(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation, const Nv12Target& target)
    {
        auto* pixels = static_cast<CVPixelBufferRef>(const_cast<void*>(source.native));
        const bool quarter = rotation == 90 || rotation == 270;
        const std::uint32_t width = quarter ? target.height : target.width;
        const std::uint32_t height = quarter ? target.width : target.height;
        if (!scaler_.run(pixels, source.width, source.height, crop, width, height)) return false;
        CVPixelBufferRef result = scaler_.result();
        if (CVPixelBufferGetWidth(result) != width || CVPixelBufferGetHeight(result) != height ||
            CVPixelBufferGetPlaneCount(result) != 2)
            return false;
        if (CVPixelBufferLockBaseAddress(result, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return false;
        const Plane luma {static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(result, 0)),
                          CVPixelBufferGetBytesPerRowOfPlane(result, 0), width, height, 1};
        const Plane chroma {static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(result, 1)),
                            CVPixelBufferGetBytesPerRowOfPlane(result, 1), width / 2, height / 2, 2};
        const OutPlane outLuma {target.luma, target.lumaStride, target.width, target.height, 1};
        const OutPlane outChroma {target.chroma, target.chromaStride, target.width / 2, target.height / 2, 2};
        // The scaled picture is read where it lies: copied, or turned into the target.
        const bool ok = (rotation == 0 ? scale(luma, outLuma) : turn(luma, outLuma, rotation)) &&
            (rotation == 0 ? scale(chroma, outChroma) : turn(chroma, outChroma, rotation));
        CVPixelBufferUnlockBaseAddress(result, kCVPixelBufferLock_ReadOnly);
        return ok;
    }

    bool viaVimage(const Nv12Source& source, Nv12Rect crop, std::uint32_t rotation, const Nv12Target& target)
    {
        const Plane luma {source.luma + std::size_t(crop.y) * source.lumaStride + crop.x, source.lumaStride, crop.width,
                          crop.height, 1};
        const Plane chroma {source.chroma + std::size_t(crop.y / 2) * source.chromaStride + std::size_t(crop.x / 2) * 2,
                            source.chromaStride, crop.width / 2, crop.height / 2, 2};
        const OutPlane outLuma {target.luma, target.lumaStride, target.width, target.height, 1};
        const OutPlane outChroma {target.chroma, target.chromaStride, target.width / 2, target.height / 2, 2};
        return plane(luma, outLuma, rotation, turnedLuma_) && plane(chroma, outChroma, rotation, turnedChroma_);
    }

    // Scales a plane to the size the rotation will turn into the output's, then
    // turns it; a rotation of zero scales straight into the output, and a
    // plane that is already the size that the rotation needs is only turned.
    bool plane(const Plane& in, const OutPlane& out, std::uint32_t rotation, std::vector<std::uint8_t>& scratch)
    {
        const bool quarter = rotation == 90 || rotation == 270;
        const std::uint32_t scaledWidth = quarter ? out.height : out.width;
        const std::uint32_t scaledHeight = quarter ? out.width : out.height;
        if (rotation == 0) return scale(in, out);
        if (in.width == scaledWidth && in.height == scaledHeight) return turn(in, out, rotation);
        scratch.resize(std::size_t(scaledWidth) * scaledHeight * in.bytes);
        const OutPlane scaled {scratch.data(), std::size_t(scaledWidth) * in.bytes, scaledWidth, scaledHeight, in.bytes};
        if (!scale(in, scaled)) return false;
        return turn({scaled.data, scaled.stride, scaled.width, scaled.height, scaled.bytes}, out, rotation);
    }

    bool scale(const Plane& in, const OutPlane& out)
    {
        if (in.width == out.width && in.height == out.height) {
            for (std::uint32_t row = 0; row < out.height; ++row)
                std::memcpy(out.data + std::size_t(row) * out.stride, in.data + std::size_t(row) * in.stride,
                            std::size_t(out.width) * out.bytes);
            return true;
        }
        const auto source = bufferOf(in);
        const auto destination = bufferOf(out);
        const auto run = [&](void* temporary, vImage_Flags flags) {
            return in.bytes == 1 ? vImageScale_Planar8(&source, &destination, temporary, flags)
                                 : vImageScale_CbCr8(&source, &destination, temporary, flags);
        };
        const auto needed = run(nullptr, scaleFlags | kvImageGetTempBufferSize);
        if (needed < 0) return false;
        if (temporary_.size() < std::size_t(needed)) temporary_.resize(std::size_t(needed));
        return run(needed > 0 ? temporary_.data() : nullptr, scaleFlags) == kvImageNoError;
    }

    static bool turn(const Plane& in, const OutPlane& out, std::uint32_t rotation)
    {
        const auto source = bufferOf(in);
        const auto destination = bufferOf(out);
        const std::uint8_t constant = rotation == 90 ? kRotate90DegreesClockwise
            : rotation == 180 ? kRotate180DegreesClockwise : kRotate270DegreesClockwise;
        // An interleaved pair turns as one 16-bit sample.
        const auto error = in.bytes == 1 ? vImageRotate90_Planar8(&source, &destination, constant, 0, rotateFlags)
                                         : vImageRotate90_Planar16U(&source, &destination, constant, 0, rotateFlags);
        return error == kvImageNoError;
    }

    Scaler scaler_;
    std::vector<std::uint8_t> turnedLuma_;
    std::vector<std::uint8_t> turnedChroma_;
    std::vector<std::uint8_t> temporary_;
};

} // namespace

std::unique_ptr<Nv12Transformer> makeAcceleratedNv12Transformer()
{
    return std::make_unique<AcceleratedTransformer>();
}

} // namespace uml::platform
