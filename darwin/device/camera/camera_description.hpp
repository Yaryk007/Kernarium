// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "calibration_store.hpp"
#include "camera.hpp"
#include "camera_tiers.hpp"
#include "photo_dimensions.hpp"
#include "virtio_video.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::camera {

/* How the camera device names what the platform's cameras can do on the Virtio
 * wire. Every function here is a pure function of the platform's description,
 * the tier and the calibration, so a test checks it without a device. */

/* The fastest frame rate a guest is offered (photos win over fast streams). */
using uml::platform::videoRateCap;

/* What a guest of a tier may ask of a camera, which its anchor must serve: the
 * still cap, the frame the host may deliver for a stream, the tier's video size
 * and the rate cap. The candidate video sizes are the camera's own and are
 * filled by the caller (`candidateVideoSizes`). */
[[nodiscard]] inline uml::platform::OfferRules offerRulesOf(const CameraTierLimits& limits)
{
    uml::platform::OfferRules rules;
    rules.limits = {limits.maximumStillPixels, maximumDeliveredFrameBytes(limits), captureRowAlignment};
    rules.maximumVideoWidth = limits.maximumVideoWidth;
    rules.maximumVideoHeight = limits.maximumVideoHeight;
    rules.rateCap = videoRateCap;
    return rules;
}

/* The video sizes a camera's formats are offered at: the standard sizes and the
 * native sizes of the formats. */
[[nodiscard]] std::vector<uml::platform::FrameSize> candidateVideoSizes(
    std::span<const uml::platform::CameraCaptureFormat> formats);

/* A video size and the frame rates it runs at. */
struct VideoMode {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t minimumFrameRate {};
    std::uint32_t maximumFrameRate {};
};

/* One camera as the guest sees it. */
struct CameraDescription {
    /* What every wire command calls the camera. */
    std::uint32_t index {};
    /* The platform's index (`Camera::cameras()`). */
    std::size_t platformIndex {};
    uml::platform::CameraInfo info;
    /* The colour data learned for the lens, when it has some. */
    std::optional<uml::platform::CameraColorData> calibration;
    std::vector<VideoMode> video;
    std::vector<uml::platform::CameraStillSize> jpegSizes;
    /* The format the camera runs in, whatever it is asked (an index of
     * `info.captureFormats`), when the host lists formats: `video` and
     * `jpegSizes` are what it offers (uml::platform::offerOf). Nothing for a
     * host with no formats, whose cameras have one set of facts. */
    std::optional<std::size_t> anchor;
    /* The camera could run with others, but the formats a multi-camera session
     * takes offer less than its best format: it keeps the whole offer and does
     * not run with others. */
    bool concurrencyDropped {};
    /* The Bayer still's size; zero when the camera has none (or none is calibrated). */
    uml::platform::CameraStillSize rawSize;
    std::uint32_t sensorOrientation {};
    virtio_video_camera_desc wire {};

    [[nodiscard]] bool raw() const noexcept { return rawSize.width != 0; }
};

struct ArrangementOptions {
    CameraTier tier {CameraTier::small};
    /* The application's statement of a camera's SENSOR_ORIENTATION; the
     * platform's own is used where it states none. */
    std::function<std::uint32_t(const uml::platform::CameraInfo&)> sensorOrientation;
};

/* The wire's cameras for a host, in wire order: the primary back camera (the
 * wide lens, or else the ultra wide, or else the telephoto), the front camera,
 * the other back lenses (wide, ultra wide, telephoto) and then any external
 * camera. At most VIRTIO_VIDEO_CAMERA_MAX_CAMERAS. A host's zoom camera is not
 * published: each lens is a camera of its own. Any two cameras conflict unless
 * the host runs them together, and never more than the device runs at once
 * (`concurrentCameras`) are told to run together: of the pairs a platform set
 * names, the primary back camera with the front camera comes first, then the
 * other pairs with the front camera, and a pair that would make a third camera
 * run with them conflicts. A camera that runs in an anchor is stated up to
 * the zoom the system recommends for the anchor, where it gives one. */
[[nodiscard]] std::vector<CameraDescription> arrangeCameras(
    std::span<const uml::platform::CameraInfo> infos, std::span<const std::vector<std::size_t>> concurrentSets,
    std::string_view hostModel, const CalibrationStore& calibration, const ArrangementOptions& options);

/* The format each platform camera runs in, indexed by the platform's own camera
 * index (`CameraSession::setAnchors`): the wire's order is not the platform's. A
 * camera that is not offered, or lists no formats, has none. */
[[nodiscard]] std::vector<std::optional<std::size_t>> anchorsByPlatformCamera(std::span<const CameraDescription> cameras,
                                                                              std::size_t platformCameraCount);

/* What the log says of a camera at start: its anchor, what it is offered and
 * whether it runs with others. Device facts only. */
[[nodiscard]] std::string anchorSummary(const CameraDescription& camera);

/* What the log says of the cameras that run together, at start: the pairs of
 * cameras (wire numbers) the guest is told run together, and the multi-camera
 * sets the platform lists (platform camera numbers) they come from. */
[[nodiscard]] std::string concurrencySummary(std::span<const CameraDescription> cameras,
                                             std::span<const std::vector<std::size_t>> platformSets);

/* What the log says of the first buffer of a camera's capture: its size and the
 * pitch of its rows, against the frame the reserve counts for a buffer of the
 * tier (`reservedBytes`, maximumDeliveredFrameBytes), and a warning when the
 * buffer is larger. */
[[nodiscard]] std::string bufferCheckLine(std::uint32_t camera, std::uint32_t width, std::uint32_t height,
                                          std::uint64_t bufferBytes, std::size_t rowBytes, std::uint64_t reservedBytes);

/* The answer to VIRTIO_VIDEO_CMD_CAMERA_QUERY from `start`, as many records as
 * fit in `capacity` bytes. Throws std::invalid_argument when not one fits. */
[[nodiscard]] std::vector<std::uint8_t> cameraQueryResponse(std::span<const CameraDescription> cameras,
                                                            std::uint32_t start, std::size_t capacity,
                                                            std::uint32_t sessionFlags);

/* The answer to VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS. */
[[nodiscard]] std::vector<std::uint8_t> cameraFormatsResponse(const CameraDescription& camera, std::uint32_t kind);

/* The wire format of a Bayer order, and the order of a wire format. */
[[nodiscard]] std::uint32_t rawWireFormat(uml::platform::BayerOrder order) noexcept;
[[nodiscard]] uml::platform::BayerOrder bayerOrderOf(std::uint32_t wireFormat) noexcept;

/* The nearest frame rate the camera runs a stream of this size at: inside a
 * range of the camera's video modes that covers the size (every mode of an
 * anchored camera has the anchor's rates). Zero when the size is not a size of
 * the camera at all. */
[[nodiscard]] std::uint32_t supportedFrameRate(const CameraDescription& camera, std::uint32_t width,
                                               std::uint32_t height, std::uint32_t rate) noexcept;

/* The largest picture size a stream of the camera may have. */
[[nodiscard]] bool videoSizeAllowed(const CameraDescription& camera, std::uint32_t width, std::uint32_t height) noexcept;

/* What the layout of the guest's image depends on. */
struct CameraLayout {
    CameraTier tier {CameraTier::small};
    bool back {};
    bool front {};
    bool autofocus {};   // a back camera focuses
    bool flash {};       // a back camera has a flash
    bool manualSensor {};  // some camera takes manual exposure
    bool raw {};         // some camera has a calibrated Bayer still
    bool concurrent {};  // front and back cameras run together
    std::uint32_t frontMaximumVideoHeight {};
};

[[nodiscard]] CameraLayout summarizeLayout(std::span<const CameraDescription> cameras, CameraTier tier) noexcept;

} // namespace uml::camera
