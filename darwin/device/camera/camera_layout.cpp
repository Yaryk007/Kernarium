// SPDX-License-Identifier: GPL-2.0-or-later

#include "camera_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace uml::camera {

using uml::platform::CameraLensKind;
using uml::platform::CameraPosition;

namespace {

struct Size {
    std::uint32_t width {};
    std::uint32_t height {};
    [[nodiscard]] std::uint64_t pixels() const noexcept { return std::uint64_t(width) * height; }
    friend bool operator==(const Size&, const Size&) = default;
};

// The sizes the camera API requires of every camera and guarantees one that
// streams beside another ("1440p" is the 4:3 size).
constexpr Size size1080p {1920, 1080};
constexpr Size size720p {1280, 720};
constexpr Size size480p {640, 480};
constexpr Size size240p {320, 240};
constexpr Size size1440p {1920, 1440};
constexpr Size standardJpegSizes[] = {size1080p, size720p, size480p, size240p};

[[nodiscard]] bool contains(const std::vector<Size>& list, const Size& size)
{
    return std::find(list.begin(), list.end(), size) != list.end();
}

[[nodiscard]] std::uint64_t most(const std::vector<Size>& list) noexcept
{
    std::uint64_t pixels = 0;
    for (const auto& size : list) pixels = std::max(pixels, size.pixels());
    return pixels;
}

// A size the HAL can cut: even, and covered by a native video mode in both
// dimensions (it never enlarges a picture).
[[nodiscard]] bool cuttable(const std::vector<Size>& natives, const Size& target) noexcept
{
    if (target.width == 0 || target.height == 0 || ((target.width | target.height) & 1u) != 0) return false;
    return std::any_of(natives.begin(), natives.end(),
                       [&](const Size& native) { return native.width >= target.width && native.height >= target.height; });
}

// The list as the camera API guarantees it of a camera that streams beside
// another: 1280x720 when its largest size reaches that, with 1920x1440 when it
// reaches that.
[[nodiscard]] bool meetsGuaranteedSizes(const std::vector<Size>& list) noexcept
{
    if (list.empty()) return false;
    const auto largest = most(list);
    const bool has1440p = contains(list, size1440p);
    const bool has720p = contains(list, size720p);
    if (largest >= size1440p.pixels()) return has1440p && has720p;
    if (largest >= size720p.pixels()) return has720p;
    return true;
}

[[nodiscard]] std::string tierName(CameraTier tier)
{
    return tier == CameraTier::small ? "small" : "large";
}

} // namespace

LayoutDimensions normalizedLayout(LayoutDimensions dimensions) noexcept
{
    if (dimensions.cameras != LayoutCameras::backAndFront) {
        dimensions.tier = CameraTier::small;
        dimensions.flash = false;
        dimensions.raw = false;
        dimensions.concurrent = false;
        dimensions.front2160 = false;
    }
    if (dimensions.cameras == LayoutCameras::none) dimensions.manual = false;
    if (!dimensions.manual) dimensions.raw = false;
    if (dimensions.tier != CameraTier::large) dimensions.front2160 = false;
    return dimensions;
}

std::string layoutName(const LayoutDimensions& wanted)
{
    const auto dimensions = normalizedLayout(wanted);
    switch (dimensions.cameras) {
    case LayoutCameras::none: return "none";
    case LayoutCameras::front: return dimensions.manual ? "front-manual" : "front";
    case LayoutCameras::backAndFront: break;
    }
    std::string name = "bf-" + tierName(dimensions.tier);
    if (dimensions.flash) name += "-flash";
    if (dimensions.manual) name += "-manual";
    if (dimensions.raw) name += "-raw";
    if (dimensions.concurrent) name += "-concurrent";
    if (dimensions.front2160) name += "-front2160";
    return name;
}

std::vector<LayoutDimensions> allLayouts()
{
    std::vector<LayoutDimensions> layouts;
    layouts.push_back({LayoutCameras::none});
    layouts.push_back({LayoutCameras::front});
    layouts.push_back({LayoutCameras::front, CameraTier::small, false, true});
    for (const auto tier : {CameraTier::small, CameraTier::large})
        for (const bool front2160 : tier == CameraTier::large ? std::vector<bool> {false, true} : std::vector<bool> {false})
            for (const bool flash : {false, true})
                for (const auto [manual, raw] : {std::pair {false, false}, std::pair {true, false}, std::pair {true, true}})
                    for (const bool concurrent : {false, true})
                        layouts.push_back({LayoutCameras::backAndFront, tier, flash, manual, raw, concurrent, front2160});
    return layouts;
}

std::optional<LayoutDimensions> layoutDimensionsNamed(std::string_view name)
{
    static const std::vector<std::pair<std::string, LayoutDimensions>> table = [] {
        std::vector<std::pair<std::string, LayoutDimensions>> layouts;
        for (const auto& dimensions : allLayouts()) layouts.emplace_back(layoutName(dimensions), dimensions);
        return layouts;
    }();
    for (const auto& [known, dimensions] : table)
        if (known == name) return dimensions;
    return std::nullopt;
}

std::uint64_t liveCameraReserveMiB(CameraMode mode, std::string_view layout, const CameraUse& use, CameraTier tier)
{
    if (mode != CameraMode::live || !layoutHasCameras(layout)) return 0;
    return cameraHostMemoryReserveMiB(tier, use);
}

std::vector<std::string> allLayoutNames()
{
    std::vector<std::string> names;
    for (const auto& dimensions : allLayouts()) names.push_back(layoutName(dimensions));
    return names;
}

std::vector<std::string> layoutFallbackChain(const LayoutDimensions& wanted)
{
    auto current = normalizedLayout(wanted);
    std::vector<std::string> chain {layoutName(current)};
    if (current.cameras == LayoutCameras::none) return chain;
    const auto step = [&](LayoutDimensions next) {
        current = normalizedLayout(next);
        chain.push_back(layoutName(current));
    };
    if (current.cameras == LayoutCameras::backAndFront) {
        if (current.concurrent) { auto next = current; next.concurrent = false; step(next); }
        if (current.raw) { auto next = current; next.raw = false; step(next); }
        if (current.front2160) { auto next = current; next.front2160 = false; step(next); }
        if (current.flash) { auto next = current; next.flash = false; step(next); }
        if (current.tier == CameraTier::large) { auto next = current; next.tier = CameraTier::small; step(next); }
        if (current.manual) { auto next = current; next.manual = false; step(next); }
        step({LayoutCameras::front});
    } else if (current.manual) {
        step({LayoutCameras::front});
    }
    step({LayoutCameras::none});
    return chain;
}

std::string chooseLayout(const LayoutDimensions& wanted, const std::function<bool(std::string_view)>& available)
{
    for (const auto& name : layoutFallbackChain(wanted))
        if (!available || available(name)) return name;
    return "none";
}

bool meetsConcurrentStreams(const CameraDescription& camera) noexcept
{
    std::vector<Size> natives;
    for (const auto& mode : camera.video) {
        const Size size {mode.width, mode.height};
        if (!contains(natives, size)) natives.push_back(size);
    }
    const auto addCut = [&](std::vector<Size>& list, const Size& size) {
        if (!contains(list, size) && cuttable(natives, size)) list.push_back(size);
    };

    // The JPEG sizes: the host's, the standard ones and, when the largest reaches 1920x1440, that size and 1280x720.
    std::vector<Size> jpeg;
    for (const auto& still : camera.jpegSizes) {
        const Size size {still.width, still.height};
        if (!contains(jpeg, size)) jpeg.push_back(size);
    }
    for (const auto& size : standardJpegSizes) addCut(jpeg, size);
    if (most(jpeg) >= size1440p.pixels()) {
        addCut(jpeg, size1440p);
        addCut(jpeg, size720p);
    }

    // The YUV sizes: the video modes, every JPEG size not larger than the largest video size, and the guaranteed pair.
    std::vector<Size> yuv = natives;
    const auto largestVideo = most(yuv);
    for (const auto& size : jpeg)
        if (size.pixels() <= largestVideo) addCut(yuv, size);
    if (largestVideo >= size1440p.pixels()) {
        addCut(yuv, size1440p);
        addCut(yuv, size720p);
    } else if (largestVideo >= size720p.pixels()) {
        addCut(yuv, size720p);
    }
    return meetsGuaranteedSizes(yuv) && meetsGuaranteedSizes(jpeg);
}

LayoutDimensions layoutDimensionsOf(std::span<const CameraDescription> cameras, CameraTier tier) noexcept
{
    bool back = false;
    bool front = false;
    bool flash = false;
    bool manual = false;
    bool raw = false;
    std::uint32_t frontVideoHeight = 0;
    for (const auto& camera : cameras) {
        const auto& info = camera.info;
        const bool manualSensor = info.manualExposure && info.lens != CameraLensKind::zoom;
        if (info.position == CameraPosition::back) {
            back = true;
            flash = flash || info.flash;
        } else if (info.position == CameraPosition::front) {
            front = true;
            for (const auto& mode : camera.video) frontVideoHeight = std::max(frontVideoHeight, mode.height);
        }
        manual = manual || manualSensor;
        raw = raw || (manualSensor && camera.raw());
    }
    LayoutDimensions dimensions;
    if (!front) return dimensions;
    dimensions.cameras = back ? LayoutCameras::backAndFront : LayoutCameras::front;
    dimensions.manual = manual;
    if (!back) return normalizedLayout(dimensions);
    dimensions.tier = tier;
    dimensions.flash = flash;
    dimensions.raw = raw;
    dimensions.front2160 = tier == CameraTier::large && frontVideoHeight >= 2160;
    // Android's concurrent feature: the cameras "0" and "1", which are the first two of the wire when the host has a back
    // camera (the primary back camera, then the front one), run together and both meet the guaranteed stream sizes.
    if (cameras.size() >= 2 && cameras[0].info.position == CameraPosition::back &&
        cameras[1].info.position == CameraPosition::front) {
        const auto names = [](const CameraDescription& camera, std::uint32_t index) {
            return (camera.wire.concurrent_mask >> index & 1u) != 0;
        };
        dimensions.concurrent = names(cameras[0], cameras[1].index) && names(cameras[1], cameras[0].index) &&
            meetsConcurrentStreams(cameras[0]) && meetsConcurrentStreams(cameras[1]);
    }
    return normalizedLayout(dimensions);
}

ComposedCameras composeCameras(const uml::platform::Camera& host, const CalibrationStore& calibration,
                               const ArrangementOptions& options, const std::function<bool(std::string_view)>& available)
{
    const auto infos = host.cameras();
    const auto sets = host.concurrentSets();
    const auto cameras = arrangeCameras(infos, sets, host.hostModel(), calibration, options);
    ComposedCameras composed;
    composed.layout = chooseLayout(layoutDimensionsOf(cameras, options.tier), available);
    const auto dimensions = layoutDimensionsNamed(composed.layout);
    composed.use.streams = dimensions ? layoutStreams(*dimensions) : concurrentCameras;
    composed.use.rawStill = std::any_of(cameras.begin(), cameras.end(), [](const CameraDescription& camera) {
        return camera.info.rawStills && (camera.raw() || !camera.calibration);
    });
    composed.use.stillWorkingBytesPerPixel = host.stillWorkingBytesPerPixel();
    return composed;
}

ComposedCameras composeCamerasWithin(const uml::platform::Camera& host, const CalibrationStore& calibration,
                                     const ArrangementOptions& options, const CameraReservation& reserved)
{
    ComposedCameras none;
    none.layout = std::string(noCameraLayout);
    none.use = reserved.use;
    if (!reserved.cameras) return none;
    const auto fits = [&](const CameraUse& use) {
        return use.streams <= reserved.use.streams && (!use.rawStill || reserved.use.rawStill) &&
            use.stillWorkingBytesPerPixel <= reserved.use.stillWorkingBytesPerPixel;
    };
    auto composed = composeCameras(host, calibration, options);
    if (!layoutHasCameras(composed.layout) || fits(composed.use)) return composed;
    auto narrower = composeCameras(host, calibration, options, [&](std::string_view name) {
        const auto dimensions = layoutDimensionsNamed(name);
        return dimensions && layoutStreams(*dimensions) <= reserved.use.streams;
    });
    return layoutHasCameras(narrower.layout) && fits(narrower.use) ? narrower : none;
}

std::string composeLayout(const uml::platform::Camera& host, const CalibrationStore& calibration,
                          const ArrangementOptions& options, const std::function<bool(std::string_view)>& available)
{
    return composeCameras(host, calibration, options, available).layout;
}

} // namespace uml::camera
