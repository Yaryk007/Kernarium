// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The application's runtime loader includes this header with no include path.
#include "../../platform/frame_bytes.hpp"
#include "jpeg_gap.hpp"

#include <cstdint>

namespace uml::camera {

/* The size caps of a session, fixed by the host's fixed GPU budget (the
 * runtime's launch budget, which a small device has little of). The tier is a
 * dimension of the camera layout, because the media profiles depend on it. */
enum class CameraTier : std::uint8_t {
    /* Video up to 1080p, stills up to 12 MP, four buffers per stream. */
    small,
    /* Video up to 4K, stills up to 48 MP, six buffers per stream. */
    large,
};

struct CameraTierLimits {
    std::uint32_t maximumVideoWidth {};
    std::uint32_t maximumVideoHeight {};
    std::uint64_t maximumStillPixels {};
    /* The buffers a stream is asked to queue (the pipeline depth). */
    std::uint32_t buffers {};
};

/* How many frames of one camera the host holds at once: the device's inbox
 * (`inboxFramesPerCamera`, the newest few of a camera), the frame the pump is
 * writing to the guest, the frame the platform's callback is delivering and the
 * one the capture is filling. */
inline constexpr std::uint32_t inboxFramesPerCamera = 3;
inline constexpr std::uint32_t heldFramesPerCamera = inboxFramesPerCamera + 3;
/* The cameras that stream at once: the front and the back camera. */
inline constexpr std::uint32_t concurrentCameras = 2;
/* The row alignment the platform's capture buffers were measured at: a 1920x1440
 * NV12 buffer takes 4,423,680 bytes where its pixels are 4,147,200. The device
 * checks the first buffer of each configuration against it. */
inline constexpr std::uint32_t captureRowAlignment = 256;

inline constexpr std::uint64_t largeTierBudgetBytes = 1ull << 30;

/* The largest tier any host is given for now: 1080p video and 12 MP stills,
 * whatever its GPU budget. The tier code and the layouts of the larger tier
 * stay, so lifting the cap is changing this constant. */
inline constexpr CameraTier largestOfferedTier = CameraTier::small;

[[nodiscard]] constexpr CameraTier cameraTierForBudget(std::uint64_t gpuBudgetBytes) noexcept
{
    const CameraTier byBudget = gpuBudgetBytes < largeTierBudgetBytes ? CameraTier::small : CameraTier::large;
    return byBudget > largestOfferedTier ? largestOfferedTier : byBudget;
}

[[nodiscard]] constexpr CameraTierLimits limitsOf(CameraTier tier) noexcept
{
    // 12 MP is 4032 x 3024 (12.19 million pixels); 48 MP is 8064 x 6048 (48.77 million).
    return tier == CameraTier::small ? CameraTierLimits {1920, 1080, 12'700'000, 4}
                                     : CameraTierLimits {3840, 2160, 50'000'000, 6};
}

/* The bytes of an NV12 frame of `width` by `height` in a capture buffer whose
 * rows are padded to captureRowAlignment: the platform model's byte count
 * (uml::platform::alignedFrameBytes), the one the offer's frame bound and this
 * reserve share. */
[[nodiscard]] constexpr std::uint64_t alignedFrameBytes(std::uint32_t width, std::uint32_t height) noexcept
{
    return uml::platform::alignedFrameBytes(width, height, captureRowAlignment);
}

/* The largest capture buffer the host holds for a tier, and so the largest frame
 * the offer lets a stream deliver: a frame of the tier's width at 4:3, the
 * tallest aspect ratio a camera format has, at the aligned row size. The
 * platform scales a format's frames to the stream's size at the format's own
 * aspect ratio, so a 4:3 format delivers a 1920x1440 frame to a 1080p stream; a
 * format whose frame for a size it offers would take more is not offered that
 * size (uml::platform::offerOf, with this bound and captureRowAlignment). */
[[nodiscard]] constexpr std::uint64_t maximumDeliveredFrameBytes(const CameraTierLimits& limits) noexcept
{
    return alignedFrameBytes(limits.maximumVideoWidth, (limits.maximumVideoWidth * 3 + 3) / 4);
}

/* What the device lets the host hold at once, which the reserve counts. The
 * device enforces the first and derives the second from what its cameras can
 * do, so the reserve bounds what the device admits and never what a layout
 * only advertises. The default is the fullest use there is, the ceiling of a
 * tier.
 *  - `streams`: the cameras the device lets run at once. A start beyond it is
 *    refused as a camera that conflicts with a running one is. It is two only
 *    for the layout of back and front cameras with the concurrent feature, and
 *    never less than one;
 *  - `rawStill`: a RAW capture, with its DNG, can be taken in this run: a
 *    lens the guest sees has RAW stills and either offers them (it has colour
 *    data) or has not taught the store its colour data yet, which its first
 *    still does. It holds from the first boot of a run, because colour data
 *    learned later only moves a lens from the second case to the first;
 *  - `stillWorkingBytesPerPixel`: the working storage, beyond the model's, the
 *    platform camera's source holds for each pixel of a JPEG still
 *    (`uml::platform::Camera::stillWorkingBytesPerPixel`); the default is the
 *    system camera's, so the ceiling holds it. */
struct CameraUse {
    std::uint32_t streams {concurrentCameras};
    bool rawStill {true};
    std::uint32_t stillWorkingBytesPerPixel {uml::platform::systemCameraStillWorkingBytesPerPixel};

    friend bool operator==(const CameraUse&, const CameraUse&) = default;
};

/* The guest's JPEG buffer for a still of `pixels` pixels, in bytes: a byte and
 * a half a pixel (a YUV 4:2:0 picture) and the Exif gap the device reserves in
 * the file. The device states it as the stream's plane size, and the guest
 * allocates its buffer from it. */
[[nodiscard]] constexpr std::uint64_t jpegBufferBytes(std::uint64_t pixels) noexcept
{
    return pixels * 3 / 2 + exifGapLength;
}

/* The largest file the host takes from the platform for a still of `pixels`
 * pixels, in bytes: the largest that always fits the guest's buffer
 * (`jpegBufferBytes`) once the Exif gap is put in it, which adds the gap's
 * payload and the four bytes of its segment header to a file that has no Exif
 * of its own to drop. A larger file fails its capture before it is copied. It
 * is the platform's file and the device's copy that the cost of a capture holds. */
[[nodiscard]] constexpr std::uint64_t maximumJpegBytes(std::uint64_t pixels) noexcept
{
    return pixels * 3 / 2 > 4 ? pixels * 3 / 2 - 4 : 0;
}

/* What one capture holds while it is with the host, in bytes. A JPEG still of
 * `pixels` pixels holds its file twice (the platform's and the device's copy;
 * the Exif gap shares the scan of the device's) and the working storage the
 * platform camera's source holds for each pixel
 * (`uml::platform::Camera::stillWorkingBytesPerPixel`); a RAW still its pixels
 * (2 bytes each) three times, the platform's buffer, the device's copy and the
 * DNG that comes with it. The device admits a capture only while the captures
 * it holds, these costs summed, stay within `stillAdmissionBytes`. The
 * platform's own working storage while it makes a picture is the camera's
 * `stillWorkingBytesPerPixel`. */
[[nodiscard]] constexpr std::uint64_t jpegCaptureBytes(std::uint64_t pixels, std::uint32_t workingBytesPerPixel = 0) noexcept
{
    return maximumJpegBytes(pixels) * 2 + pixels * workingBytesPerPixel;
}

[[nodiscard]] constexpr std::uint64_t rawCaptureBytes(std::uint64_t pixels) noexcept
{
    return pixels * 2 * 3;
}

/* The most the captures the device holds at once may cost: the fullest capture
 * the use allows, a JPEG with a RAW where RAW is possible. The first capture is
 * always admitted; a later one waits until it fits. */
[[nodiscard]] constexpr std::uint64_t stillAdmissionBytes(const CameraTierLimits& limits, const CameraUse& use) noexcept
{
    return jpegCaptureBytes(limits.maximumStillPixels, use.stillWorkingBytesPerPixel) +
        (use.rawStill ? rawCaptureBytes(limits.maximumStillPixels) : 0);
}

/* The host-side memory a live session of a tier can hold at its worst, in
 * bytes, outside the guest's RAM, for what the device lets it use
 * (`CameraUse`). A live session takes it out of the GPU budget B
 * (gpu_pool_bytes in platform/gpu_budget.hpp), so the application's memory
 * limit holds it. Each part is derived from the tier's limits:
 *  - the capture pools: the largest capture buffer the offer lets a stream
 *    take (maximumDeliveredFrameBytes), for each frame the host holds
 *    (heldFramesPerCamera) of each camera that streams at once
 *    (`CameraUse::streams`);
 *  - the device's staging: one NV12 frame of the largest video size, where a frame is
 *    cropped, rotated and scaled before it is copied to the guest;
 *  - the transformer's own scratch, which the platform's transformer keeps between
 *    frames and which the staging does not cover: a scaled pixel buffer and a rotation
 *    scratch of one such frame each, and vImage's temporaries, measured at 3,691,696
 *    bytes for the largest scale of the small tier (a 1080p frame turned a quarter),
 *    which is 1.19 frames and counted as 1.2;
 *  - the stills: what the device admits at once (`stillAdmissionBytes`).
 * The device's black pictures are written from a fixed chunk of the device and
 * hold nothing more, in a live or a blocked session; a blocked session opens no
 * capture session and holds none of the rest. Nothing the guest is offered
 * depends on it: the offer is bounded by the same buffer
 * (maximumDeliveredFrameBytes), whatever the use. */
struct CameraHostMemory {
    std::uint64_t capturePools {};
    std::uint64_t staging {};
    std::uint64_t transformerScratch {};
    std::uint64_t stills {};

    [[nodiscard]] constexpr std::uint64_t total() const noexcept
    {
        return capturePools + staging + transformerScratch + stills;
    }
};

[[nodiscard]] constexpr CameraHostMemory cameraHostMemory(CameraTier tier, const CameraUse& use = {}) noexcept
{
    const CameraTierLimits limits = limitsOf(tier);
    const std::uint64_t frame = std::uint64_t {limits.maximumVideoWidth} * limits.maximumVideoHeight * 3 / 2;
    return {
        .capturePools = maximumDeliveredFrameBytes(limits) * heldFramesPerCamera * use.streams,
        .staging = frame,
        .transformerScratch = frame * 2 + frame * 6 / 5,
        .stills = stillAdmissionBytes(limits, use),
    };
}

/* The reserve in whole mebibytes, rounded up: the figure the GPU budget gives
 * up while the cameras are live. With no `CameraUse` it is the tier's ceiling,
 * which a layout the device does not enforce, and the application's plan before
 * the cameras are composed, count. */
[[nodiscard]] constexpr std::uint64_t cameraHostMemoryReserveMiB(CameraTier tier, const CameraUse& use = {}) noexcept
{
    constexpr std::uint64_t mebibyte = 1ull << 20;
    return (cameraHostMemory(tier, use).total() + mebibyte - 1) / mebibyte;
}

} // namespace uml::camera
