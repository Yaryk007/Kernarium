// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_description.hpp"

#include "photo_dimensions.hpp"
#include "vhost_user_device.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <string>

namespace uml::camera {
namespace {
using uml::platform::BayerOrder;
using uml::platform::CameraInfo;
using uml::platform::CameraLensKind;
using uml::platform::CameraPosition;
using uml::virtio::toBytes;

/* The pixel pitch the sensor size is derived from, as the sensor is not stated:
 * the ratio of the lenses stays exact, and the absolute figures are nominal
 * until a Bayer still replaces them. */
constexpr double nominalPitchMicrometres = 1.2;

constexpr std::uint32_t maximumCameras = VIRTIO_VIDEO_CAMERA_MAX_CAMERAS;

/* The sizes every camera offers beyond its native ones, so that a guest has
 * the usual preview and recording sizes; the device scales to them. */
constexpr std::pair<std::uint32_t, std::uint32_t> standardSizes[] = {
    {176, 144}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
};

std::uint32_t lensOf(CameraLensKind kind) noexcept
{
    switch (kind) {
    case CameraLensKind::zoom: return VIRTIO_VIDEO_CAMERA_LENS_ZOOM;
    case CameraLensKind::wide: return VIRTIO_VIDEO_CAMERA_LENS_WIDE;
    case CameraLensKind::ultraWide: return VIRTIO_VIDEO_CAMERA_LENS_ULTRA_WIDE;
    case CameraLensKind::telephoto: return VIRTIO_VIDEO_CAMERA_LENS_TELEPHOTO;
    case CameraLensKind::external: return VIRTIO_VIDEO_CAMERA_LENS_EXTERNAL;
    }
    return VIRTIO_VIDEO_CAMERA_LENS_EXTERNAL;
}

std::uint32_t facingOf(CameraPosition position) noexcept
{
    switch (position) {
    case CameraPosition::back: return VIRTIO_VIDEO_CAMERA_FACING_BACK;
    case CameraPosition::front: return VIRTIO_VIDEO_CAMERA_FACING_FRONT;
    case CameraPosition::external: return VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL;
    }
    return VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL;
}

std::uint32_t bayerOf(BayerOrder order) noexcept
{
    switch (order) {
    case BayerOrder::rggb: return VIRTIO_VIDEO_CAMERA_BAYER_RGGB;
    case BayerOrder::grbg: return VIRTIO_VIDEO_CAMERA_BAYER_GRBG;
    case BayerOrder::gbrg: return VIRTIO_VIDEO_CAMERA_BAYER_GBRG;
    case BayerOrder::bggr: return VIRTIO_VIDEO_CAMERA_BAYER_BGGR;
    case BayerOrder::none: break;
    }
    return VIRTIO_VIDEO_CAMERA_BAYER_NONE;
}

std::uint32_t q16(double value) noexcept
{
    return std::uint32_t(std::int32_t(std::lround(value * 65536.0)));
}

/* The frame-rate ranges a size of the camera runs at: the union of the ranges
 * of every platform format that covers the size. */
std::vector<std::pair<std::uint32_t, std::uint32_t>> rangesOf(const CameraInfo& info, std::uint32_t width,
                                                              std::uint32_t height)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (const auto& format : info.videoFormats)
        if (format.width >= width && format.height >= height) ranges.emplace_back(format.minimumFrameRate, format.maximumFrameRate);
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    return merged;
}

std::vector<VideoMode> videoModes(const CameraInfo& info, const CameraTierLimits& limits)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    std::uint32_t largestWidth = 0, largestHeight = 0;
    for (const auto& format : info.videoFormats) {
        if (format.width > limits.maximumVideoWidth || format.height > limits.maximumVideoHeight) continue;
        if (format.width % 2 || format.height % 2) continue;
        if (std::find(sizes.begin(), sizes.end(), std::make_pair(format.width, format.height)) == sizes.end())
            sizes.emplace_back(format.width, format.height);
        largestWidth = std::max(largestWidth, format.width);
        largestHeight = std::max(largestHeight, format.height);
    }
    for (const auto& [width, height] : standardSizes) {
        if (width > largestWidth || height > largestHeight) continue;
        if (std::find(sizes.begin(), sizes.end(), std::make_pair(width, height)) == sizes.end()) sizes.emplace_back(width, height);
    }
    std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
        return std::uint64_t(a.first) * a.second > std::uint64_t(b.first) * b.second ||
            (std::uint64_t(a.first) * a.second == std::uint64_t(b.first) * b.second && a.first > b.first);
    });
    std::vector<VideoMode> modes;
    for (const auto& [width, height] : sizes)
        for (const auto& range : rangesOf(info, width, height)) modes.push_back({width, height, range.first, range.second});
    return modes;
}

void fillWire(CameraDescription& camera)
{
    const auto& info = camera.info;
    auto& wire = camera.wire;
    wire = {};
    wire.index = camera.index;
    wire.facing = facingOf(info.position);
    wire.lens = lensOf(info.lens);
    std::uint32_t flags = 0;
    if (info.autofocus) flags |= VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS;
    if (info.flash) flags |= VIRTIO_VIDEO_CAMERA_F_FLASH;
    if (info.torch) flags |= VIRTIO_VIDEO_CAMERA_F_TORCH;
    if (info.zoomMaximum > info.zoomMinimum) flags |= VIRTIO_VIDEO_CAMERA_F_ZOOM;
    if (info.manualExposure) flags |= VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE;
    if (info.manualFocus) flags |= VIRTIO_VIDEO_CAMERA_F_MANUAL_FOCUS;
    if (info.manualWhiteBalance) flags |= VIRTIO_VIDEO_CAMERA_F_MANUAL_WHITE_BALANCE;
    if (camera.raw()) flags |= VIRTIO_VIDEO_CAMERA_F_RAW;
    wire.flags = flags;
    wire.sensor_orientation = camera.sensorOrientation;
    wire.pixel_array_width = info.pixelArrayWidth;
    wire.pixel_array_height = info.pixelArrayHeight;
    double sensorWidth = info.pixelArrayWidth * nominalPitchMicrometres;
    double sensorHeight = info.pixelArrayHeight * nominalPitchMicrometres;
    // The sensor size and the focal length are a pair that describes the field of view the host states: the guest takes
    // its lens labels (focal length over the sensor's diagonal) and 35 mm equivalents from it. The focal length is the
    // one that gives the field of view for the nominal size; a Bayer still's own figure (the lens's physical focal
    // length) replaces it, and the size then follows the field of view, in the pixel array's shape, so that the pair is
    // the same ratio before and after the lens has learned its colour data and the physical figure sits beside a
    // size that agrees with it (the Exif of a still and the DNG state the same focal length). A host that states no
    // field of view leaves the nominal size.
    const bool fieldOfViewStated = info.horizontalFieldOfView > 1.0f && info.horizontalFieldOfView < 179.0f;
    const double halfTangent = fieldOfViewStated ? std::tan(double(info.horizontalFieldOfView) * std::numbers::pi / 360.0) : 0.0;
    double focal = 0;
    if (camera.calibration && camera.calibration->focalLengthMillimetres > 0) {
        focal = camera.calibration->focalLengthMillimetres * 1000.0;
        if (fieldOfViewStated && info.pixelArrayWidth > 0 && info.pixelArrayHeight > 0) {
            sensorWidth = 2.0 * focal * halfTangent;
            sensorHeight = sensorWidth * info.pixelArrayHeight / info.pixelArrayWidth;
        }
    } else if (fieldOfViewStated) {
        focal = (sensorWidth / 2.0) / halfTangent;
    } else {
        focal = sensorWidth;
    }
    wire.sensor_width_um = std::uint32_t(std::lround(sensorWidth));
    wire.sensor_height_um = std::uint32_t(std::lround(sensorHeight));
    wire.focal_length_um = std::uint32_t(std::lround(focal));
    wire.aperture_x100 = std::uint32_t(std::lround(info.aperture * 100.0f));
    wire.min_focus_distance_mm = std::uint32_t(std::lround(info.minimumFocusDistanceMillimetres));
    wire.iso_min = info.isoMinimum;
    wire.iso_max = info.isoMaximum;
    wire.exposure_min_ns = info.exposureMinimumNanoseconds;
    wire.exposure_max_ns = info.exposureMaximumNanoseconds;
    wire.zoom_min_q16 = q16(info.zoomMinimum);
    wire.zoom_max_q16 = q16(info.zoomMaximum);
    wire.num_switch_overs = std::uint32_t(std::min<std::size_t>(info.switchOvers.size(), VIRTIO_VIDEO_CAMERA_MAX_SWITCH_OVERS));
    for (std::uint32_t index = 0; index < wire.num_switch_overs; ++index) wire.switch_over_q16[index] = q16(info.switchOvers[index]);
    wire.max_wb_gain_q16 = q16(info.maximumWhiteBalanceGain);
    if (camera.calibration) {
        const auto& color = *camera.calibration;
        for (std::size_t index = 0; index < 4; ++index) wire.intrinsics_q16[index] = q16(color.intrinsics[index]);
        if (camera.raw()) {
            wire.bayer_order = bayerOf(color.bayer);
            wire.black_level = color.blackLevel;
            wire.white_level = color.whiteLevel;
            wire.raw_width = camera.rawSize.width;
            wire.raw_height = camera.rawSize.height;
            wire.reference_illuminant1 = color.illuminant1;
            wire.reference_illuminant2 = color.illuminant2;
            for (std::size_t index = 0; index < 9; ++index) {
                wire.color_matrix1_q16[index] = q16(color.colorMatrix1[index]);
                wire.color_matrix2_q16[index] = q16(color.colorMatrix2[index]);
                wire.forward_matrix1_q16[index] = q16(color.forwardMatrix1[index]);
                wire.forward_matrix2_q16[index] = q16(color.forwardMatrix2[index]);
            }
        }
    }
}

} // namespace

std::uint32_t rawWireFormat(BayerOrder order) noexcept
{
    switch (order) {
    case BayerOrder::rggb: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB;
    case BayerOrder::grbg: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG;
    case BayerOrder::gbrg: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GBRG;
    case BayerOrder::bggr: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_BGGR;
    case BayerOrder::none: break;
    }
    return 0;
}

BayerOrder bayerOrderOf(std::uint32_t wireFormat) noexcept
{
    switch (wireFormat) {
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB: return BayerOrder::rggb;
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG: return BayerOrder::grbg;
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GBRG: return BayerOrder::gbrg;
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_BGGR: return BayerOrder::bggr;
    default: return BayerOrder::none;
    }
}

namespace {

// The most cameras of a group in which every two are told to run together, `size` cameras already chosen, `pool` the
// cameras that run with all of them and are above the last one chosen (bit i: camera i). `adjacent[i]` has bit j when
// cameras i and j are told to run together.
std::size_t largestGroup(std::span<const std::uint32_t> adjacent, std::uint32_t pool, std::size_t size)
{
    std::size_t best = size;
    for (std::uint32_t rest = pool; rest != 0; rest &= rest - 1) {
        const auto camera = std::uint32_t(std::countr_zero(rest));
        const std::uint32_t above = camera + 1 >= 32 ? 0u : ~0u << (camera + 1);
        best = std::max(best, largestGroup(adjacent, pool & adjacent[camera] & above, size + 1));
    }
    return best;
}

std::vector<VideoMode> modesOf(const uml::platform::CaptureOffer& offer)
{
    std::vector<VideoMode> modes;
    for (const auto& size : offer.videoSizes)
        for (const auto& range : offer.rates) modes.push_back({size.width, size.height, range.minimum, range.maximum});
    return modes;
}

} // namespace

std::vector<uml::platform::FrameSize> candidateVideoSizes(std::span<const uml::platform::CameraCaptureFormat> formats)
{
    std::vector<uml::platform::FrameSize> sizes;
    const auto add = [&](uml::platform::FrameSize size) {
        if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
    };
    for (const auto& [width, height] : standardSizes) add({width, height});
    for (const auto& format : formats) add({format.width, format.height});
    return sizes;
}

std::vector<CameraDescription> arrangeCameras(std::span<const CameraInfo> infos,
                                              std::span<const std::vector<std::size_t>> concurrentSets,
                                              std::string_view hostModel, const CalibrationStore& calibration,
                                              const ArrangementOptions& options)
{
    // Wire order. A zoom camera, which switches between lenses, is never published: each lens is a camera of its own,
    // so that a guest switches lenses by switching cameras and every lens keeps its own controls, formats and Bayer
    // stills.
    std::vector<std::size_t> order;
    const auto contains = [&](std::size_t index) { return std::find(order.begin(), order.end(), index) != order.end(); };
    const auto take = [&](auto&& wanted) {
        for (std::size_t index = 0; index < infos.size(); ++index)
            if (!contains(index) && infos[index].lens != CameraLensKind::zoom && wanted(infos[index])) {
                order.push_back(index);
                return true;
            }
        return false;
    };
    const auto lens = [](CameraPosition position, CameraLensKind kind) {
        return [position, kind](const CameraInfo& info) { return info.position == position && info.lens == kind; };
    };
    // The primary back camera is the wide lens, or else the ultra wide, or else the telephoto.
    for (const auto kind : {CameraLensKind::wide, CameraLensKind::ultraWide, CameraLensKind::telephoto})
        if (take(lens(CameraPosition::back, kind))) break;
    // The front camera.
    if (!take(lens(CameraPosition::front, CameraLensKind::wide)))
        take([](const CameraInfo& info) { return info.position == CameraPosition::front; });
    // The other back lenses.
    for (const auto kind : {CameraLensKind::wide, CameraLensKind::ultraWide, CameraLensKind::telephoto})
        take(lens(CameraPosition::back, kind));
    // External cameras.
    while (order.size() < maximumCameras && take([](const CameraInfo& info) { return info.position == CameraPosition::external; })) {
    }
    order.resize(std::min<std::size_t>(order.size(), maximumCameras));

    // Which cameras run together: those a platform set names.
    const auto together = [&](std::size_t a, std::size_t b) {
        return std::any_of(concurrentSets.begin(), concurrentSets.end(), [&](const auto& set) {
            return std::find(set.begin(), set.end(), a) != set.end() && std::find(set.begin(), set.end(), b) != set.end();
        });
    };
    const auto runsWith = [&](std::size_t a, std::size_t b) { return a != b && together(a, b); };

    // Each camera that lists its formats is arranged once, here: its anchor is
    // the format it runs in whatever it is asked, and everything the guest is
    // offered comes from it. A camera that could run with others keeps that only
    // when the formats a multi-camera session takes offer as much as its best
    // format; otherwise it keeps the whole offer and does not run with others.
    const auto limits = limitsOf(options.tier);
    struct Arranged {
        std::size_t platform {};
        std::optional<std::size_t> anchor;
        uml::platform::CaptureOffer offer;
        bool concurrent {true};
        bool concurrencyDropped {};
    };
    std::vector<Arranged> arranged;
    for (const auto platform : order) {
        Arranged entry;
        entry.platform = platform;
        const auto& formats = infos[platform].captureFormats;
        if (!formats.empty()) {
            auto rules = offerRulesOf(limits);
            rules.candidateSizes = candidateVideoSizes(formats);
            const bool couldRunWithOthers = std::any_of(order.begin(), order.end(), [&](std::size_t other) { return runsWith(platform, other); });
            // Whether the camera keeps its concurrency is decided on the formats as listed: a limit on the photo
            // sizes of a front camera (limitedStillFormats) is no reason for it to start running with others.
            entry.concurrent = couldRunWithOthers && uml::platform::keepsConcurrency(formats, rules);
            entry.concurrencyDropped = couldRunWithOthers && !entry.concurrent;
            // The anchor and the offer are made on the formats with the position's still limit, as the platform's
            // session makes the photo output (the same function, setAnchors).
            const auto limited = uml::platform::limitedStillFormats(infos[platform].position, formats);
            entry.anchor = uml::platform::chooseAnchor(limited, rules, entry.concurrent);
            if (!entry.anchor) {
                std::fprintf(stderr, "Camera: platform camera %zu is not offered: none of its %zu formats carries a stream\n", platform,
                             formats.size());
                continue;
            }
            entry.offer = uml::platform::offerOf(limited[*entry.anchor], rules);
        }
        arranged.push_back(std::move(entry));
    }

    std::vector<CameraDescription> cameras;
    for (std::size_t wire = 0; wire < arranged.size(); ++wire) {
        const auto& entry = arranged[wire];
        CameraDescription camera;
        camera.index = std::uint32_t(wire);
        camera.platformIndex = entry.platform;
        camera.info = infos[entry.platform];
        const auto& info = camera.info;
        camera.sensorOrientation = options.sensorOrientation ? options.sensorOrientation(info) : info.sensorOrientation;
        if (entry.anchor) {
            camera.anchor = entry.anchor;
            camera.concurrencyDropped = entry.concurrencyDropped;
            camera.video = modesOf(entry.offer);
            camera.jpegSizes = entry.offer.stills;
            camera.info.stillSizes = entry.offer.stills;
            // The camera zooms up to the system's recommendation for the format it runs in, where the system gives
            // one; where it does not, up to what the format itself delivers, and never beyond the device's largest
            // (a front camera's other formats zoom further than the one it runs in); its range always holds 1.0.
            const auto& anchorFormat = info.captureFormats[*entry.anchor];
            if (anchorFormat.recommendedZoomMaximum > 0)
                camera.info.zoomMaximum = std::max({anchorFormat.recommendedZoomMaximum, 1.0f, info.zoomMinimum});
            else if (anchorFormat.maximumZoom > 0)
                camera.info.zoomMaximum = std::min(info.zoomMaximum, std::max({anchorFormat.maximumZoom, 1.0f, info.zoomMinimum}));
        } else {
            camera.video = videoModes(info, limits);
            // Every size works with every format, within the tier's cap and the limit of the camera's position.
            for (const auto& size : info.stillSizes)
                if (std::uint64_t(size.width) * size.height <= limits.maximumStillPixels && uml::platform::stillWithinPositionLimit(info.position, size))
                    camera.jpegSizes.push_back(size);
        }
        // RAW is advertised only for a lens whose colour data is known, and, for
        // a camera that has an anchor, was learned in it: another format may
        // make another Bayer size.
        if (info.rawStills) {
            camera.calibration = calibration.load(hostModel, CalibrationStore::lensKey(info));
            if (camera.calibration && camera.anchor) {
                const auto& format = info.captureFormats[*camera.anchor];
                if (camera.calibration->anchorWidth != format.width || camera.calibration->anchorHeight != format.height)
                    camera.calibration.reset();
            }
            if (camera.calibration && camera.calibration->bayer != BayerOrder::none && camera.calibration->rawWidth != 0 &&
                std::uint64_t(camera.calibration->rawWidth) * camera.calibration->rawHeight <= limits.maximumStillPixels &&
                camera.calibration->whiteLevel > camera.calibration->blackLevel)
                camera.rawSize = {camera.calibration->rawWidth, camera.calibration->rawHeight};
        }
        cameras.push_back(std::move(camera));
    }

    for (auto& camera : cameras) fillWire(camera);

    // Which cameras the guest is told run together. The wire names pairs, and the guest takes every group of cameras
    // in which each two are paired as one it may run at once, so the pairs are chosen such that no such group holds
    // more cameras than the device runs at once (concurrentCameras: a third camera is refused, whatever the platform
    // lists). A pair is a platform set's two cameras that kept their concurrency. The primary back camera with the
    // front camera comes first (it is the concurrent feature), then the other pairs with a front camera, then the rest,
    // each in wire order; a pair that would make a larger group conflicts instead.
    const std::size_t count = cameras.size();
    const auto isFront = [&](std::size_t camera) { return cameras[camera].info.position == CameraPosition::front; };
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (std::size_t a = 0; a < count; ++a)
        for (std::size_t b = a + 1; b < count; ++b)
            if (runsWith(cameras[a].platformIndex, cameras[b].platformIndex) && arranged[a].concurrent && arranged[b].concurrent)
                pairs.emplace_back(a, b);
    const auto rankOf = [&](const std::pair<std::size_t, std::size_t>& pair) {
        const bool primaryWithFront = pair.first == 0 && cameras[0].info.position == CameraPosition::back && isFront(pair.second);
        if (primaryWithFront) return 0;
        return isFront(pair.first) || isFront(pair.second) ? 1 : 2;
    };
    std::stable_sort(pairs.begin(), pairs.end(), [&](const auto& x, const auto& y) { return rankOf(x) < rankOf(y); });
    std::vector<std::uint32_t> adjacent(count, 0);
    for (const auto& [a, b] : pairs) {
        adjacent[a] |= 1u << b;
        adjacent[b] |= 1u << a;
        if (largestGroup(adjacent, (1u << count) - 1, 0) > concurrentCameras) {
            adjacent[a] &= ~(1u << b);
            adjacent[b] &= ~(1u << a);
        }
    }
    for (auto& camera : cameras) {
        for (const auto& other : cameras) {
            if (other.index == camera.index) continue;
            if (adjacent[camera.index] >> other.index & 1u)
                camera.wire.concurrent_mask |= 1u << other.index;
            else
                camera.wire.conflict_mask |= 1u << other.index;
        }
    }
    return cameras;
}

std::vector<std::uint8_t> cameraQueryResponse(std::span<const CameraDescription> cameras, std::uint32_t start,
                                              std::size_t capacity, std::uint32_t sessionFlags)
{
    virtio_video_camera_query_resp response {};
    response.hdr = {VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY, 0};
    response.num_cameras = std::uint32_t(cameras.size());
    response.session_flags = sessionFlags;
    response.desc_size = sizeof(virtio_video_camera_desc);
    const std::size_t room = capacity > sizeof(response) ? (capacity - sizeof(response)) / sizeof(virtio_video_camera_desc) : 0;
    const std::size_t count = start >= cameras.size() ? 0 : std::min<std::size_t>(cameras.size() - start, room);
    // A buffer with no room for a record would answer "complete" and tell the
    // driver there is nothing more.
    if (count == 0 && start < cameras.size()) throw std::invalid_argument("camera response is too small");
    response.num_descs = std::uint32_t(count);
    response.next_index = start + count < cameras.size() ? std::uint32_t(start + count) : 0;
    auto bytes = toBytes(response);
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = toBytes(cameras[start + index].wire);
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    return bytes;
}

std::vector<std::uint8_t> cameraFormatsResponse(const CameraDescription& camera, std::uint32_t kind)
{
    std::vector<std::uint8_t> descs;
    std::uint32_t count = 0;
    const auto append = [&descs](const auto& value) {
        const auto bytes = toBytes(value);
        descs.insert(descs.end(), bytes.begin(), bytes.end());
    };
    const auto range = [](std::uint32_t low, std::uint32_t high, std::uint32_t step) {
        return virtio_video_format_range {low, high, step, {}};
    };
    const auto descriptor = [&](std::uint32_t format, std::uint32_t frames) {
        virtio_video_format_desc desc {};
        desc.format = format;
        desc.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
        desc.plane_align = 16;
        desc.num_frames = frames;
        return desc;
    };
    if (kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) {
        // One frame record per size, with its rate ranges.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
        for (const auto& mode : camera.video)
            if (std::find(sizes.begin(), sizes.end(), std::make_pair(mode.width, mode.height)) == sizes.end())
                sizes.emplace_back(mode.width, mode.height);
        if (!sizes.empty()) {
            append(descriptor(VIRTIO_VIDEO_FORMAT_NV12, std::uint32_t(sizes.size())));
            for (const auto& [width, height] : sizes) {
                std::uint32_t rates = 0;
                for (const auto& mode : camera.video) rates += mode.width == width && mode.height == height;
                append(virtio_video_format_frame {range(width, width, 1), range(height, height, 1), rates, {}});
                for (const auto& mode : camera.video)
                    if (mode.width == width && mode.height == height) append(range(mode.minimumFrameRate, mode.maximumFrameRate, 1));
            }
            ++count;
        }
    } else if (kind == VIRTIO_VIDEO_CAMERA_STREAM_STILL) {
        if (!camera.jpegSizes.empty()) {
            append(descriptor(VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, std::uint32_t(camera.jpegSizes.size())));
            for (const auto& size : camera.jpegSizes)
                append(virtio_video_format_frame {range(size.width, size.width, 1), range(size.height, size.height, 1), 0, {}});
            ++count;
        }
        if (camera.raw()) {
            append(descriptor(rawWireFormat(camera.calibration->bayer), 1));
            append(virtio_video_format_frame {range(camera.rawSize.width, camera.rawSize.width, 1),
                                              range(camera.rawSize.height, camera.rawSize.height, 1), 0, {}});
            ++count;
        }
    }
    virtio_video_query_capability_resp response {};
    response.hdr = {VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY, 0};
    response.num_descs = count;
    auto bytes = toBytes(response);
    bytes.insert(bytes.end(), descs.begin(), descs.end());
    return bytes;
}

std::vector<std::optional<std::size_t>> anchorsByPlatformCamera(std::span<const CameraDescription> cameras,
                                                                std::size_t platformCameraCount)
{
    std::vector<std::optional<std::size_t>> anchors(platformCameraCount);
    for (const auto& camera : cameras)
        if (camera.anchor && camera.platformIndex < anchors.size()) anchors[camera.platformIndex] = camera.anchor;
    return anchors;
}

std::string concurrencySummary(std::span<const CameraDescription> cameras, std::span<const std::vector<std::size_t>> platformSets)
{
    std::string line = "Camera: runs together:";
    bool any = false;
    for (const auto& camera : cameras)
        for (const auto& other : cameras)
            if (camera.index < other.index && (camera.wire.concurrent_mask >> other.index & 1u) != 0) {
                line += (any ? "," : "") + std::string(" ") + std::to_string(camera.index) + "+" + std::to_string(other.index);
                any = true;
            }
    if (!any) line += " none";
    line += " (the device runs " + std::to_string(concurrentCameras) + " cameras at once); the platform's multi-camera sets:";
    if (platformSets.empty()) line += " none";
    for (const auto& set : platformSets) {
        line += " {";
        for (std::size_t member = 0; member < set.size(); ++member) line += (member != 0 ? " " : "") + std::to_string(set[member]);
        line += "}";
    }
    return line;
}

std::string anchorSummary(const CameraDescription& camera)
{
    if (!camera.anchor || camera.video.empty()) return {};
    const auto& info = camera.info;
    const auto& format = info.captureFormats[*camera.anchor];
    const char* position = info.position == CameraPosition::back ? "back" : info.position == CameraPosition::front ? "front" : "external";
    const char* lens = "external";
    switch (info.lens) {
    case CameraLensKind::zoom: lens = "zoom"; break;
    case CameraLensKind::wide: lens = "wide"; break;
    case CameraLensKind::ultraWide: lens = "ultra wide"; break;
    case CameraLensKind::telephoto: lens = "telephoto"; break;
    case CameraLensKind::external: break;
    }
    const auto size = [](std::uint32_t width, std::uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); };
    std::string rates;
    for (const auto& mode : camera.video) {
        if (mode.width != camera.video.front().width || mode.height != camera.video.front().height) break;
        if (!rates.empty()) rates += ",";
        rates += std::to_string(mode.minimumFrameRate) + "-" + std::to_string(mode.maximumFrameRate);
    }
    std::string stills;
    for (const auto& still : camera.jpegSizes) stills += (stills.empty() ? "" : " ") + size(still.width, still.height);
    std::string line = "Camera: camera " + std::to_string(camera.index) + " (" + position + " " + lens + ") runs in " +
        size(format.width, format.height) + (format.multiCamera ? " (multi-camera)" : "") + " of " +
        std::to_string(info.captureFormats.size()) + " formats; offers stills " + (stills.empty() ? "none" : stills) +
        ", video up to " + size(camera.video.front().width, camera.video.front().height) + " at " + rates + " fps";
    if (camera.concurrencyDropped) line += "; not concurrent: the formats a multi-camera session takes offer less";
    return line;
}

std::string bufferCheckLine(std::uint32_t camera, std::uint32_t width, std::uint32_t height, std::uint64_t bufferBytes,
                            std::size_t rowBytes, std::uint64_t reservedBytes)
{
    std::string line = "Camera: camera " + std::to_string(camera) + " delivers " + std::to_string(width) + "x" + std::to_string(height) +
        " in buffers of " + std::to_string(bufferBytes) + " bytes (rows of " + std::to_string(rowBytes) +
        "); the reserve counts " + std::to_string(reservedBytes) + " a buffer";
    if (bufferBytes > reservedBytes) line += ": the buffer is larger than the reserve counts";
    return line;
}

std::uint32_t supportedFrameRate(const CameraDescription& camera, std::uint32_t width, std::uint32_t height,
                                 std::uint32_t rate) noexcept
{
    if (!videoSizeAllowed(camera, width, height)) return 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    if (camera.anchor) {
        // The offer's own ranges: the anchor's, which no clip has touched.
        for (const auto& mode : camera.video)
            if (mode.width >= width && mode.height >= height) ranges.emplace_back(mode.minimumFrameRate, mode.maximumFrameRate);
    } else {
        ranges = rangesOf(camera.info, width, height);
    }
    std::uint32_t best = 0;
    std::uint32_t bestDistance = ~0u;
    for (const auto& range : ranges) {
        const auto clamped = std::clamp(rate, range.first, range.second);
        const auto distance = clamped > rate ? clamped - rate : rate - clamped;
        if (distance < bestDistance) {
            best = clamped;
            bestDistance = distance;
        }
    }
    return best;
}

bool videoSizeAllowed(const CameraDescription& camera, std::uint32_t width, std::uint32_t height) noexcept
{
    if (width < 16 || height < 16 || (width & 1) || (height & 1)) return false;
    for (const auto& mode : camera.video)
        if (mode.width >= width && mode.height >= height) return true;
    return false;
}

CameraLayout summarizeLayout(std::span<const CameraDescription> cameras, CameraTier tier) noexcept
{
    CameraLayout layout;
    layout.tier = tier;
    for (const auto& camera : cameras) {
        const auto& info = camera.info;
        if (info.position == CameraPosition::back) {
            layout.back = true;
            layout.autofocus |= info.autofocus;
            layout.flash |= info.flash;
        }
        if (info.position == CameraPosition::front) {
            layout.front = true;
            for (const auto& mode : camera.video)
                layout.frontMaximumVideoHeight = std::max(layout.frontMaximumVideoHeight, mode.height);
        }
        layout.manualSensor |= info.manualExposure;
        layout.raw |= camera.raw();
    }
    for (const auto& a : cameras)
        for (const auto& b : cameras)
            if (a.info.position == CameraPosition::back && b.info.position == CameraPosition::front &&
                (a.wire.concurrent_mask >> b.index & 1u))
                layout.concurrent = true;
    return layout;
}

} // namespace uml::camera
