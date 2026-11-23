// SPDX-License-Identifier: GPL-2.0-or-later
#include "turned_still.hpp"

#include "nv12_transform.hpp"
#include "video_encoder.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace uml::platform {
namespace {

// The encoder takes a few milliseconds for a still this size; a bound that is never reached on a healthy host.
constexpr std::chrono::milliseconds kEncodeBound {5000};

struct CfRelease {
    void operator()(CFTypeRef value) const noexcept { if (value) CFRelease(value); }
};
template <typename T>
using Cf = std::unique_ptr<std::remove_pointer_t<T>, CfRelease>;

class PixelLock {
public:
    explicit PixelLock(CVPixelBufferRef pixels) : pixels_(pixels) { ok_ = CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess; }
    ~PixelLock() { if (ok_) CVPixelBufferUnlockBaseAddress(pixels_, kCVPixelBufferLock_ReadOnly); }
    PixelLock(const PixelLock&) = delete;
    PixelLock& operator=(const PixelLock&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    CVPixelBufferRef pixels_;
    bool ok_ {};
};

// The colours a photo states, by its attachments, the colour space object it carries and its ICC profile. A photo is read
// as sRGB when it has no profile, so what matters is whether what it states is sRGB: BT.709 primaries (sRGB's too) with an
// sRGB or BT.709 transfer. Those are the explicit attachments, the colour spaces kCGColorSpaceSRGB and kCGColorSpaceITUR_709,
// and the spaces Core Video itself makes of those attachments, which are equal to neither named space. A colour space made
// of an ICC profile is equal to the one it is the profile of.
struct StatedColours {
    bool other {};       // something it states is not that
    bool primaries {};   // its primaries are stated, and are
    bool transfer {};    // its transfer is stated, and is
};

// The colour space Core Video makes of BT.709 primaries over this transfer function and, when there is one, this matrix: the
// space it makes over the sRGB transfer differs with the matrix a photo carries (a photo here carries none or BT.601's).
Cf<CGColorSpaceRef> coreVideoSpaceOf(CFStringRef transfer, CFStringRef matrix)
{
    const void* keys[3] = {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey, kCVImageBufferYCbCrMatrixKey};
    const void* values[3] = {kCVImageBufferColorPrimaries_ITU_R_709_2, transfer, matrix};
    Cf<CFDictionaryRef> attachments(CFDictionaryCreate(nullptr, keys, values, matrix ? 3 : 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    return Cf<CGColorSpaceRef>(attachments ? CVImageBufferCreateColorSpaceFromAttachments(attachments.get()) : nullptr);
}

// Whether the space is one of those that state sRGB's colours: equal to it, or made of the same ICC profile.
bool isSrgbOrBt709Space(CGColorSpaceRef space)
{
    const Cf<CGColorSpaceRef> statesSrgb[] = {
        Cf<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceSRGB)),
        Cf<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceITUR_709)),
        coreVideoSpaceOf(kCVImageBufferTransferFunction_ITU_R_709_2, nullptr),
        coreVideoSpaceOf(kCVImageBufferTransferFunction_ITU_R_709_2, kCVImageBufferYCbCrMatrix_ITU_R_601_4),
        coreVideoSpaceOf(kCVImageBufferTransferFunction_sRGB, nullptr),
        coreVideoSpaceOf(kCVImageBufferTransferFunction_sRGB, kCVImageBufferYCbCrMatrix_ITU_R_601_4),
    };
    const Cf<CFDataRef> profile(CGColorSpaceCopyICCData(space));
    for (const auto& known : statesSrgb) {
        if (!known) continue;
        if (CFEqual(space, known.get())) return true;
        const Cf<CFDataRef> knownProfile(CGColorSpaceCopyICCData(known.get()));
        if (profile && knownProfile && CFEqual(profile.get(), knownProfile.get())) return true;
    }
    return false;
}

StatedColours statedColours(CVPixelBufferRef photo)
{
    StatedColours colours;
    Cf<CFTypeRef> primaries(CVBufferCopyAttachment(photo, kCVImageBufferColorPrimariesKey, nullptr));
    if (primaries) {
        if (CFEqual(primaries.get(), kCVImageBufferColorPrimaries_ITU_R_709_2)) colours.primaries = true;
        else colours.other = true;
    }
    Cf<CFTypeRef> transfer(CVBufferCopyAttachment(photo, kCVImageBufferTransferFunctionKey, nullptr));
    if (transfer) {
        if (CFEqual(transfer.get(), kCVImageBufferTransferFunction_ITU_R_709_2) || CFEqual(transfer.get(), kCVImageBufferTransferFunction_sRGB)) colours.transfer = true;
        else colours.other = true;
    } else if (Cf<CFTypeRef> gamma(CVBufferCopyAttachment(photo, kCVImageBufferGammaLevelKey, nullptr)); gamma) {
        colours.other = true;   // a gamma stated without a transfer function is no transfer this accepts
    }
    Cf<CFTypeRef> object(CVBufferCopyAttachment(photo, kCVImageBufferCGColorSpaceKey, nullptr));
    if (object) {
        if (CFGetTypeID(object.get()) == CGColorSpaceGetTypeID() && isSrgbOrBt709Space(static_cast<CGColorSpaceRef>(const_cast<void*>(object.get()))))
            colours.primaries = colours.transfer = true;
        else colours.other = true;
    }
    Cf<CFTypeRef> profile(CVBufferCopyAttachment(photo, kCVImageBufferICCProfileKey, nullptr));
    if (profile) {
        Cf<CGColorSpaceRef> space(CFGetTypeID(profile.get()) == CFDataGetTypeID() ? CGColorSpaceCreateWithICCData(static_cast<CFDataRef>(profile.get())) : nullptr);
        if (space && isSrgbOrBt709Space(space.get())) colours.primaries = colours.transfer = true;
        else colours.other = true;
    }
    return colours;
}

bool statesAnotherMatrix(CVPixelBufferRef photo)
{
    Cf<CFTypeRef> matrix(CVBufferCopyAttachment(photo, kCVImageBufferYCbCrMatrixKey, nullptr));
    return matrix && !CFEqual(matrix.get(), kCVImageBufferYCbCrMatrix_ITU_R_601_4);
}

} // namespace

TurnedStill turnedStillOf(CVPixelBufferRef photo, std::uint32_t jpegQuality, std::uint64_t maximumBytes, bool captureIsSrgb)
{
    TurnedStill result;
    const auto fail = [&](TurnedStillFailure failure) {
        result.jpeg.clear();
        result.failure = failure;
        return std::move(result);
    };
    if (photo == nullptr) return fail(TurnedStillFailure::noPixels);
    const std::size_t width = CVPixelBufferGetWidth(photo);
    const std::size_t height = CVPixelBufferGetHeight(photo);
    if (CVPixelBufferGetPixelFormatType(photo) != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        CVPixelBufferGetPlaneCount(photo) != 2 || width == 0 || height == 0 || ((width | height) & 1) != 0 || width > 0xffff || height > 0xffff)
        return fail(TurnedStillFailure::unsupportedFormat);
    if (statesAnotherMatrix(photo)) return fail(TurnedStillFailure::notBt601);
    const StatedColours colours = statedColours(photo);
    if (colours.other) return fail(TurnedStillFailure::notSrgb);
    // What the photo does not state is the capture's colour space, which is sRGB only if the selection of it took effect.
    if (!(colours.primaries && colours.transfer) && !captureIsSrgb) return fail(TurnedStillFailure::unknownColours);

    auto encoder = makeVideoEncoder();
    if (!encoder) return fail(TurnedStillFailure::noEncoder);
    VideoEncodeSettings settings;
    settings.codec = VideoCodec::jpeg;
    settings.profile = VideoEncodeProfile::jpegBaseline;
    settings.input = RawVideoLayout::nv12;
    settings.width = std::uint32_t(width);
    settings.height = std::uint32_t(height);
    // The samples go to the encoder as they are: a JFIF image states BT.601 over the full range, and the encoder
    // converts nothing for JPEG.
    settings.color = {VideoColorPrimaries::unspecified, VideoTransferFunction::unspecified, VideoMatrixCoefficients::bt601, true};
    settings.jpegQuality = std::clamp<std::uint32_t>(jpegQuality, 1, 100);
    settings.maximumJpegBytes = maximumBytes;
    if (encoder->configure(settings) != VideoEncodeStatus::ok) return fail(TurnedStillFailure::noEncoder);
    auto frame = encoder->acquireFrame();
    if (!frame || frame->planeCount() < 2) return fail(TurnedStillFailure::noEncoder);
    {
        const PixelLock lock(photo);
        if (!lock.ok()) return fail(TurnedStillFailure::noPixels);
        auto luma = frame->plane(0);
        auto chroma = frame->plane(1);
        if (frame->stride(0) < width || frame->rows(0) < height || frame->stride(1) < width || frame->rows(1) < height / 2 ||
            luma.size() < std::size_t(frame->stride(0)) * height || chroma.size() < std::size_t(frame->stride(1)) * (height / 2))
            return fail(TurnedStillFailure::noEncoder);
        turnPlaneHalf(static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(photo, 0)), CVPixelBufferGetBytesPerRowOfPlane(photo, 0),
                      luma.data(), frame->stride(0), std::uint32_t(width), std::uint32_t(height), false);
        turnPlaneHalf(static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(photo, 1)), CVPixelBufferGetBytesPerRowOfPlane(photo, 1),
                      chroma.data(), frame->stride(1), std::uint32_t(width / 2), std::uint32_t(height / 2), true);
    }
    encoder->submit(std::move(frame), 0, true);
    encoder->drain();

    std::vector<std::uint8_t> image;
    bool drained = false, errored = false;
    const auto deadline = std::chrono::steady_clock::now() + kEncodeBound;
    while (!drained && std::chrono::steady_clock::now() < deadline) {
        pollfd readable {encoder->readinessDescriptor(), POLLIN, 0};
        (void)::poll(&readable, 1, 20);
        for (auto& item : encoder->takeFrames()) {
            switch (item.kind) {
            case EncodedVideoFrameKind::frame: image = std::move(item.bytes); break;
            case EncodedVideoFrameKind::drained: drained = true; break;
            case EncodedVideoFrameKind::error: errored = true; break;
            case EncodedVideoFrameKind::parameterSets: break;
            }
        }
    }
    if (!drained) return fail(TurnedStillFailure::timedOut);
    if (errored || image.empty()) return fail(TurnedStillFailure::failed);
    result.jpeg = std::move(image);
    return result;
}

} // namespace uml::platform
