// SPDX-License-Identifier: GPL-2.0-or-later
// How the host's cameras become the wire's: their order, which run together,
// what the tier and the calibration allow, and the pages of the answers.

#include "calibration_store.hpp"
#include "camera_description.hpp"
#include "camera_ids.hpp"
#include "front_tables.hpp"
#include "require.hpp"
#include "photo_dimensions.hpp"
#include "synthetic_camera.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace uml::camera;
using namespace uml::platform;
using uml::testing::load;
using uml::testing::require;

namespace {

struct Host {
    std::unique_ptr<SyntheticCamera> camera;
    std::vector<CameraInfo> infos;
    std::vector<std::vector<std::size_t>> sets;
    std::string model;
    explicit Host(SyntheticCameraProfile profile)
    {
        SyntheticCameraOptions options;
        options.profile = profile;
        camera = makeSyntheticCamera(options);
        infos = camera->cameras();
        sets = camera->concurrentSets();
        model = camera->hostModel();
    }
    std::vector<CameraDescription> arrange(CameraTier tier, const CalibrationStore& store = CalibrationStore(),
                                           std::function<std::uint32_t(const CameraInfo&)> orientation = {}) const
    {
        ArrangementOptions options;
        options.tier = tier;
        options.sensorOrientation = std::move(orientation);
        return arrangeCameras(infos, sets, model, store, options);
    }
};

bool bit(std::uint32_t mask, std::uint32_t index) { return (mask >> index & 1u) != 0; }

CameraColorData colorOf(std::uint32_t width, std::uint32_t height)
{
    CameraColorData data;
    data.bayer = BayerOrder::grbg;
    data.blackLevel = 64;
    data.whiteLevel = 1023;
    data.rawWidth = width;
    data.rawHeight = height;
    data.illuminant1 = 17;
    data.illuminant2 = 21;
    data.colorMatrix1 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    data.focalLengthMillimetres = 6.9;
    data.intrinsics = {3000, 3000, 1008, 756};
    return data;
}

void testPhoneOrder()
{
    const Host host(SyntheticCameraProfile::phone);
    const auto cameras = host.arrange(CameraTier::small);
    require(cameras.size() == 4, "four cameras");
    // The wide lens, the front camera, then the other lenses: the host publishes no zoom camera.
    require(cameras[0].info.lens == CameraLensKind::wide && cameras[0].wire.lens == VIRTIO_VIDEO_CAMERA_LENS_WIDE &&
                cameras[0].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_BACK,
            "camera 0 is the back wide lens");
    require(cameras[1].info.position == CameraPosition::front && cameras[1].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_FRONT,
            "camera 1 is the front camera");
    require(cameras[2].info.lens == CameraLensKind::ultraWide && cameras[3].info.lens == CameraLensKind::telephoto,
            "then the ultra wide and telephoto lenses");
    for (std::uint32_t index = 0; index < cameras.size(); ++index)
        require(cameras[index].index == index && cameras[index].wire.index == index, "the index is the position");
    require(cameras[0].platformIndex == 0 && cameras[1].platformIndex == 3 && cameras[2].platformIndex == 1 &&
                cameras[3].platformIndex == 2,
            "the platform's index is kept");
    for (const auto& camera : cameras)
        require(camera.wire.lens != VIRTIO_VIDEO_CAMERA_LENS_ZOOM && camera.wire.constituent_mask == 0,
                "no camera is a zoom camera or made of others");

    // Only the wide lens runs with the front camera, as on the phone whose sets the profile has; the other lenses
    // conflict with the front camera and with each other.
    require(bit(cameras[1].wire.concurrent_mask, 0) && bit(cameras[0].wire.concurrent_mask, 1),
            "the front camera runs with the wide lens");
    for (const auto lens : {2u, 3u})
        require(bit(cameras[1].wire.conflict_mask, lens) && bit(cameras[lens].wire.conflict_mask, 1) &&
                    !bit(cameras[1].wire.concurrent_mask, lens),
                "and not with another lens");
    for (const auto [a, b] : {std::make_pair(0u, 2u), std::make_pair(0u, 3u), std::make_pair(2u, 3u)})
        require(bit(cameras[a].wire.conflict_mask, b) && bit(cameras[b].wire.conflict_mask, a), "two back lenses conflict");
    for (const auto& camera : cameras)
        require((camera.wire.conflict_mask & camera.wire.concurrent_mask) == 0 && !bit(camera.wire.conflict_mask, camera.index) &&
                    !bit(camera.wire.concurrent_mask, camera.index),
                "a camera is not its own neighbour, and a pair is one or the other");

    // The flags follow the camera.
    for (const auto lens : {0u, 2u, 3u})
        require((cameras[lens].wire.flags & VIRTIO_VIDEO_CAMERA_F_ZOOM) && (cameras[lens].wire.flags & VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE) &&
                    (cameras[lens].wire.flags & VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS),
                "a lens zooms, has manual exposure and focuses");
    require(!(cameras[1].wire.flags & VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS), "the front camera does not focus");
    // Each lens zooms from 1.0, its own full field of view, up to the zoom the host states for it.
    require(cameras[0].wire.zoom_min_q16 == 65536 && cameras[0].wire.zoom_max_q16 == 6 * 65536, "the wide lens: 1 to 6");
    require(cameras[2].wire.zoom_min_q16 == 65536 && cameras[2].wire.zoom_max_q16 == 3 * 65536, "the ultra wide lens: 1 to 3");
    require(cameras[3].wire.zoom_min_q16 == 65536 && cameras[3].wire.zoom_max_q16 == 3 * 65536, "the telephoto lens: 1 to 3");
    require(cameras[0].wire.num_switch_overs == 0 && cameras[2].wire.num_switch_overs == 0 && cameras[3].wire.num_switch_overs == 0,
            "no lens switches to another");
    require(cameras[0].wire.sensor_orientation == 90 && cameras[1].wire.sensor_orientation == 270, "the sensor orientation");
    require(cameras[0].wire.focal_length_um > 0 && cameras[0].wire.sensor_width_um > 0, "the optics are stated");
    // A wider field of view gives a shorter focal length on the same sensor size.
    require(cameras[2].wire.focal_length_um < cameras[0].wire.focal_length_um &&
                cameras[3].wire.focal_length_um > cameras[0].wire.focal_length_um,
            "ultra wide is shorter, telephoto longer");
}

// A host lists its zoom cameras (virtual devices that switch between lenses), and the device publishes none: the wire
// is the same as without it, whatever sets name it.
void testAZoomCameraIsNotPublished()
{
    const Host phone(SyntheticCameraProfile::phone);
    CameraInfo triple = phone.infos[0];
    triple.identifier = "synthetic-back-triple";
    triple.lens = CameraLensKind::zoom;
    triple.zoomMinimum = 0.5f;
    triple.zoomMaximum = 123.75f;
    triple.switchOvers = {1.0f, 3.0f};
    triple.constituents = {phone.infos[1].identifier, phone.infos[0].identifier, phone.infos[2].identifier};
    triple.manualExposure = triple.manualFocus = triple.manualWhiteBalance = false;
    triple.rawStills = false;
    // The platform's order puts the zoom camera first of the back cameras; its set names the front camera too.
    std::vector<CameraInfo> listed {triple};
    listed.insert(listed.end(), phone.infos.begin(), phone.infos.end());
    const std::vector<std::vector<std::size_t>> sets {{0, 4}, {1, 4}};

    const auto cameras = arrangeCameras(listed, sets, phone.model, CalibrationStore(), {});
    const auto without = phone.arrange(CameraTier::small);
    require(cameras.size() == without.size() && cameras.size() == 4, "the same four cameras");
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        require(cameras[index].info.identifier == without[index].info.identifier && cameras[index].info.lens != CameraLensKind::zoom &&
                    cameras[index].wire.lens != VIRTIO_VIDEO_CAMERA_LENS_ZOOM && cameras[index].wire.constituent_mask == 0,
                "in the same order, none of them the zoom camera");
        require(cameras[index].platformIndex == without[index].platformIndex + 1, "the platform's index counts the zoom camera");
        require(cameras[index].wire.zoom_min_q16 == without[index].wire.zoom_min_q16 &&
                    cameras[index].wire.zoom_max_q16 == without[index].wire.zoom_max_q16,
                "with the lens's own range");
    }
    require(bit(cameras[0].wire.concurrent_mask, 1) && bit(cameras[1].wire.concurrent_mask, 0),
            "the wide lens runs with the front camera, as the platform's set says");
    require(cameras[1].wire.conflict_mask == (1u << 2 | 1u << 3), "and the front camera conflicts with no zoom camera, only the other lenses");

    // The zoom camera alone is no camera, and a host with one lens and a virtual device publishes the lens.
    const auto only = arrangeCameras(std::vector<CameraInfo> {triple}, {}, phone.model, CalibrationStore(), {});
    require(only.empty(), "a virtual device is not published for want of a lens");
    const auto lensAndZoom = arrangeCameras(std::vector<CameraInfo> {triple, phone.infos[1]}, {}, phone.model, CalibrationStore(), {});
    require(lensAndZoom.size() == 1 && lensAndZoom[0].info.lens == CameraLensKind::ultraWide && lensAndZoom[0].index == 0 &&
                lensAndZoom[0].platformIndex == 1,
            "the one lens is the primary back camera, whichever it is");
}

// Android requires every camera's zoom ratio range to hold 1.0, and a single lens's 1.0 is its own full field of view:
// a client that rebinds a camera re-applies the ratio 1.0, so a lens whose range starts above it fails the client, and one
// whose range starts below it (an ultra wide stated as 0.5) opens at a crop of its own view. Every published camera is a
// single lens, so every range starts at 1.0.
void testEveryZoomRangeHoldsOne()
{
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet,
                               SyntheticCameraProfile::tabletDualFront, SyntheticCameraProfile::laptop}) {
        const Host host(profile);
        for (const auto tier : {CameraTier::small, CameraTier::large})
            for (const auto& camera : host.arrange(tier)) {
                require(camera.wire.zoom_min_q16 <= 65536 && camera.wire.zoom_max_q16 >= 65536,
                        "every described camera's zoom range holds 1.0");
                require(camera.wire.zoom_min_q16 == 65536, "a single lens's widest view is its ratio 1.0");
            }
    }
}

void testOtherHosts()
{
    const Host tablet(SyntheticCameraProfile::tablet);
    const auto cameras = tablet.arrange(CameraTier::small);
    require(cameras.size() == 2 && cameras[0].info.position == CameraPosition::back &&
                cameras[0].wire.lens == VIRTIO_VIDEO_CAMERA_LENS_WIDE && cameras[1].info.position == CameraPosition::front,
            "a tablet's back wide camera is camera 0, the front camera 1");
    require(bit(cameras[0].wire.concurrent_mask, 1) && bit(cameras[1].wire.concurrent_mask, 0), "they run together");
    require(cameras[0].wire.constituent_mask == 0 && cameras[0].wire.conflict_mask == 0, "and nothing conflicts");

    const Host laptop(SyntheticCameraProfile::laptop);
    const auto only = laptop.arrange(CameraTier::large);
    require(only.size() == 1 && only[0].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_FRONT, "a laptop has one front camera");
    require(only[0].wire.concurrent_mask == 0 && only[0].wire.conflict_mask == 0, "alone");

    // The application states the sensor orientation when it knows better.
    const auto stated = tablet.arrange(CameraTier::small, CalibrationStore(), [](const CameraInfo& info) {
        return info.position == CameraPosition::front ? 180u : 0u;
    });
    require(stated[0].sensorOrientation == 0 && stated[0].wire.sensor_orientation == 0 && stated[1].wire.sensor_orientation == 180,
            "the application's orientation");

    // No cameras at all is a valid, empty device.
    require(arrangeCameras({}, {}, "none", CalibrationStore(), {}).empty(), "no cameras");

    // More than the wire holds are cut off, back cameras first.
    std::vector<CameraInfo> many;
    CameraInfo wide = tablet.infos[0];
    many.push_back(wide);
    for (int index = 0; index < 12; ++index) {
        CameraInfo external = tablet.infos[1];
        external.identifier = "external-" + std::to_string(index);
        external.position = CameraPosition::external;
        external.lens = CameraLensKind::external;
        many.push_back(external);
    }
    const auto capped = arrangeCameras(many, {}, "many", CalibrationStore(), {});
    require(capped.size() == VIRTIO_VIDEO_CAMERA_MAX_CAMERAS && capped[0].info.position == CameraPosition::back &&
                capped[1].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL,
            "at most eight cameras, the back one first");
}

void testTiers()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto small = phone.arrange(CameraTier::small);
    const auto large = phone.arrange(CameraTier::large);
    // The wide lens takes 48 MP stills only in the large tier.
    require(small[0].jpegSizes.front() == CameraStillSize {4032, 3024}, "12 MP at most in the small tier");
    require(large[0].jpegSizes.front() == CameraStillSize {8064, 6048}, "48 MP in the large tier");
    const auto largest = [](const CameraDescription& camera) {
        std::uint32_t width = 0, height = 0;
        for (const auto& mode : camera.video) {
            width = std::max(width, mode.width);
            height = std::max(height, mode.height);
        }
        return std::make_pair(width, height);
    };
    require(largest(small[0]) == std::make_pair(1920u, 1080u), "1080p video at most in the small tier");
    require(largest(large[0]) == std::make_pair(3840u, 2160u), "4K video in the large tier");
    // Every host is given the small tier for now, whatever its GPU budget: the cap is one constant, and the
    // tier the budget would earn is still the large one at 1 GiB.
    require(largestOfferedTier == CameraTier::small, "the cap");
    for (const std::uint64_t budget : {std::uint64_t {0}, (1ull << 30) - 1, 1ull << 30, 4ull << 30, ~std::uint64_t {0}})
        require(cameraTierForBudget(budget) == CameraTier::small, "every host composes the small tier");
    require(limitsOf(CameraTier::small).buffers == 4 && limitsOf(CameraTier::large).buffers == 6, "buffers");

    // The sizes include the usual ones the device scales to, never above the camera's own.
    const auto& modes = small[0].video;
    for (const auto size : {std::make_pair(1280u, 720u), std::make_pair(640u, 480u), std::make_pair(320u, 240u),
                            std::make_pair(176u, 144u)})
        require(std::any_of(modes.begin(), modes.end(), [&](const auto& mode) { return mode.width == size.first && mode.height == size.second; }),
                "a standard size is offered");
    for (const auto& mode : modes)
        require(mode.width % 2 == 0 && mode.height % 2 == 0 && mode.minimumFrameRate >= 1 && mode.maximumFrameRate >= mode.minimumFrameRate,
                "sizes are even and have a rate");
    for (std::size_t index = 1; index < modes.size(); ++index)
        require(std::uint64_t(modes[index - 1].width) * modes[index - 1].height >= std::uint64_t(modes[index].width) * modes[index].height,
                "largest first");
}

// An iPad mini's back camera as the platform reports it: the photo sizes belong to the format a camera runs in.
CameraInfo tabletCamera()
{
    CameraInfo info;
    info.identifier = "tablet-back";
    info.position = CameraPosition::back;
    info.lens = CameraLensKind::wide;
    info.captureFormats = {
        {1440, 1080, {{1, 30}}, {{1440, 1080}, {2016, 1512}}, true, true},
        {1920, 1440, {{1, 30}}, {{1920, 1440}, {4032, 3024}}, true, true},
        {4032, 3024, {{1, 30}}, {{4032, 3024}, {4224, 2376}}, true, false},
        {1920, 1080, {{1, 60}}, {{1920, 1080}, {2016, 1134}}, true, true},
    };
    for (const auto& format : info.captureFormats)
        info.videoFormats.push_back({format.width, format.height, 1, std::uint32_t(format.frameRates.front().maximum), format.multiCamera});
    // Every size some format makes, as the platform lists them.
    info.stillSizes = {{4032, 3024}, {2016, 1512}, {1920, 1440}, {1920, 1080}, {1440, 1080}, {2016, 1134}};
    return info;
}

std::uint32_t fastestRate(const CameraDescription& camera)
{
    std::uint32_t rate = 0;
    for (const auto& mode : camera.video) rate = std::max(rate, mode.maximumFrameRate);
    return rate;
}

ArrangementOptions optionsOf(CameraTier tier)
{
    ArrangementOptions options;
    options.tier = tier;
    return options;
}

// What a camera is offered is derived from its anchor, the one format it runs in: the sizes that format makes, the
// video sizes inside it and its rates up to the cap, so every offered run starts and every offered still is made.
void testOfferedStillsFollowTheAnchor()
{
    const std::vector<CameraInfo> infos {tabletCamera()};
    const auto cameras = arrangeCameras(infos, {}, "tablet", CalibrationStore(), {});
    require(cameras.size() == 1);
    const auto& camera = cameras[0];
    require(camera.anchor == std::optional<std::size_t>(1), "the 1920x1440 format: it makes the 12 MP photo and is smaller than the other that does");
    require(camera.jpegSizes == std::vector<CameraStillSize>({{4032, 3024}, {1920, 1440}}),
            "the sizes the 1920x1440 format makes: not 4224x2376, which only the 12 MP video format lists");
    require(camera.info.stillSizes == camera.jpegSizes, "the camera's own list is the offer");
    require(fastestRate(camera) == 30, "no stream above 30 fps: the cap, and the anchor's own");
    require(!camera.video.empty() && camera.video.front().width == 1920 && camera.video.front().height == 1080, "1080p video at most");
    // The offer holds for what remains: arranging the arranged camera changes nothing.
    const std::vector<CameraInfo> again {camera.info};
    const auto second = arrangeCameras(again, {}, "tablet", CalibrationStore(), {});
    require(second[0].jpegSizes == camera.jpegSizes && fastestRate(second[0]) == 30 && second[0].anchor == camera.anchor, "stable");
    // The guest's cap bounds the offer: the small tier's 12 MP, the large tier's 48 MP.
    require(arrangeCameras(infos, {}, "tablet", CalibrationStore(), optionsOf(CameraTier::large))[0].jpegSizes.front() ==
                CameraStillSize({4032, 3024}),
            "a camera that lists no 48 MP offers none");
}

// A camera that runs in an anchor is stated up to the zoom the system recommends for that format, and up to the
// platform's own limit where the format has none; every range holds 1.0.
void testZoomFollowsTheAnchorsRecommendation()
{
    // The tablet camera's anchor is its format 1 (testOfferedStillsFollowTheAnchor); the other formats advise otherwise.
    const auto advised = [](float anchorsAdvice, float othersAdvice) {
        auto info = tabletCamera();
        info.zoomMinimum = 1.0f;
        info.zoomMaximum = 123.75f;
        for (auto& format : info.captureFormats) format.recommendedZoomMaximum = othersAdvice;
        info.captureFormats[1].recommendedZoomMaximum = anchorsAdvice;
        const auto cameras = arrangeCameras(std::vector<CameraInfo> {info}, {}, "tablet", CalibrationStore(), {});
        require(cameras.size() == 1 && cameras[0].anchor == std::optional<std::size_t>(1), "the camera runs in its format 1");
        require(cameras[0].info.zoomMinimum == 1.0f, "from 1.0");
        return std::make_pair(cameras[0].wire.zoom_min_q16, cameras[0].wire.zoom_max_q16);
    };
    constexpr std::uint32_t one = 65536;
    require(advised(6.0f, 2.0f) == std::make_pair(one, 6 * one), "the anchor's recommended maximum, whatever the other formats say");
    require(advised(3.0f, 0.0f) == std::make_pair(one, 3 * one), "a recommendation under the platform's own limit");
    require(advised(200.0f, 0.0f).second == 200 * one, "one over it is the system's word for the format");
    require(advised(0.0f, 5.0f) == std::make_pair(one, std::uint32_t(123.75f * 65536.0f)),
            "no recommendation for the anchor: the platform's own maximum, whatever the other formats say");
    const auto below = advised(0.5f, 0.0f);
    require(below.first <= one && below.second >= one, "a recommendation under 1.0 still leaves a range that holds 1.0");
    require(below.second == one, "which is no zoom at all");

    // No recommendation (a front camera): the format the camera runs in states its own maximum, which is below the
    // device's largest over all its formats (128.875 on the phone, of which the format delivers 94.5). It is never
    // above the device's, and the range holds 1.0.
    const auto own = [](float anchorsMaximum, float othersMaximum, float deviceMaximum) {
        auto info = tabletCamera();
        info.zoomMinimum = 1.0f;
        info.zoomMaximum = deviceMaximum;
        for (auto& format : info.captureFormats) format.maximumZoom = othersMaximum;
        info.captureFormats[1].maximumZoom = anchorsMaximum;
        const auto cameras = arrangeCameras(std::vector<CameraInfo> {info}, {}, "phone", CalibrationStore(), {});
        require(cameras.size() == 1 && cameras[0].anchor == std::optional<std::size_t>(1), "the camera runs in its format 1");
        return std::make_pair(cameras[0].wire.zoom_min_q16, cameras[0].wire.zoom_max_q16);
    };
    require(own(94.5f, 128.875f, 128.875f) == std::make_pair(one, std::uint32_t(94.5f * 65536.0f)),
            "no recommendation: the anchor's own maximum, below the device's, whatever the other formats deliver");
    require(own(0.0f, 94.5f, 128.875f) == std::make_pair(one, std::uint32_t(128.875f * 65536.0f)),
            "a format that states none leaves the device's maximum");
    require(own(200.0f, 0.0f, 128.875f).second == std::uint32_t(128.875f * 65536.0f), "and the device's maximum is the ceiling");
    require(own(0.5f, 0.0f, 128.875f) == std::make_pair(one, one), "a maximum under 1.0 still leaves a range that holds 1.0");
    // A recommendation wins over the format's own maximum, as it does for the back lenses.
    auto lens = tabletCamera();
    lens.zoomMinimum = 1.0f;
    lens.zoomMaximum = 123.75f;
    for (auto& format : lens.captureFormats) format.maximumZoom = 94.5f;
    lens.captureFormats[1].recommendedZoomMaximum = 6.0f;
    const auto recommended = arrangeCameras(std::vector<CameraInfo> {lens}, {}, "phone", CalibrationStore(), {});
    require(recommended[0].wire.zoom_max_q16 == 6 * one, "a recommendation is the maximum, whatever the format delivers");
}

// The reserve holds for what the platform delivers: for every stream a tier offers, at every rate, the format a
// camera runs in delivers a frame the capture pools were counted for.
void testTheFramesAnAnchorDeliversNeverExceedTheReservesBuffer()
{
    // A camera whose only 12 MP format is square, and which has a 1080p format that makes none: a 1080p stream from the
    // square one would be delivered 1920x1920, more than a buffer of the reserve holds, so no size that large is offered.
    CameraInfo square;
    square.identifier = "square";
    square.captureFormats = {
        {1920, 1920, {{1, 30}}, {{4032, 3024}}, true, true},
        {1920, 1080, {{1, 60}}, {{1920, 1080}}, true, true},
    };
    for (const auto& format : square.captureFormats)
        square.videoFormats.push_back({format.width, format.height, 1, std::uint32_t(format.frameRates.front().maximum), true});
    square.stillSizes = {{4032, 3024}, {1920, 1080}};
    for (const auto tier : {CameraTier::small, CameraTier::large}) {
        const auto limits = limitsOf(tier);
        for (const auto& info : {tabletCamera(), square}) {
            const auto cameras = arrangeCameras(std::vector<CameraInfo> {info}, {}, "tablet", CalibrationStore(), optionsOf(tier));
            require(cameras.size() == 1 && !cameras[0].video.empty() && cameras[0].anchor);
            const auto& anchor = info.captureFormats[*cameras[0].anchor];
            for (const auto& mode : cameras[0].video) {
                const auto delivered = deliveredFrameSize(anchor.width, anchor.height, mode.width, mode.height);
                require(deliveredFrameBytes(anchor, mode.width, mode.height, captureRowAlignment) <= maximumDeliveredFrameBytes(limits),
                        "the capture buffer of an offered stream is within the one the reserve counts");
                require(alignedFrameBytes(delivered.width, delivered.height) <= maximumDeliveredFrameBytes(limits),
                        "by the reserve's own function");
            }
        }
    }
    require(maximumDeliveredFrameBytes(limitsOf(CameraTier::small)) == 4'423'680 &&
                deliveredFrameBytes(tabletCamera().captureFormats[1], 1920, 1080, 0) == 1920ull * 1440 * 3 / 2,
            "and the iPad mini's 4:3 format at 1080p is 4,147,200 bytes in 4,423,680 of buffer");
    // 1802x1534 delivers 1802x1534 to every stream: 4,146,402 bytes tightly packed, under the old bound, but 4,712,448
    // in 256-byte rows, which the reserve does not count. A camera that has only it is not offered.
    CameraInfo odd;
    odd.identifier = "odd";
    odd.captureFormats = {{1802, 1534, {{1, 30}}, {{4032, 3024}}, true, true}};
    odd.videoFormats.push_back({1802, 1534, 1, 30, true});
    odd.stillSizes = {{4032, 3024}};
    require(arrangeCameras(std::vector<CameraInfo> {odd}, {}, "tablet", CalibrationStore(), optionsOf(CameraTier::small)).empty(),
            "a camera whose only format delivers more than the reserve counts is not offered");
}

// A camera runs with others only when the formats a multi-camera session takes offer as much as its best format.
void testAConcurrentCameraKeepsItsOfferOrRunsAlone()
{
    CameraInfo other = tabletCamera();
    other.identifier = "tablet-front";
    other.position = CameraPosition::front;
    const std::vector<std::vector<std::size_t>> together {{0, 1}};

    // The 12 MP-making format takes a multi-camera session too: the camera runs with the other and offers what it did.
    const std::vector<CameraInfo> both {tabletCamera(), other};
    const auto joined = arrangeCameras(both, together, "tablet", CalibrationStore(), {});
    require(joined[0].anchor == std::optional<std::size_t>(1) && joined[0].jpegSizes.front() == CameraStillSize({4032, 3024}) &&
                !joined[0].concurrencyDropped && bit(joined[0].wire.concurrent_mask, 1) && bit(joined[1].wire.concurrent_mask, 0),
            "they run together, in the anchor they have alone");

    // It does not: the best offer is a format no multi-camera session takes. The camera keeps the whole offer and runs
    // alone; the other camera, which would, conflicts with it.
    auto lonely = tabletCamera();
    lonely.captureFormats[1].multiCamera = false;
    const std::vector<CameraInfo> dropped {lonely, other};
    const auto apart = arrangeCameras(dropped, together, "tablet", CalibrationStore(), {});
    require(apart[0].anchor == std::optional<std::size_t>(1) && apart[0].jpegSizes.front() == CameraStillSize({4032, 3024}) &&
                apart[0].concurrencyDropped,
            "alone it offers its 12 MP photo, as it always did");
    require(!bit(apart[0].wire.concurrent_mask, 1) && bit(apart[0].wire.conflict_mask, 1) && !bit(apart[1].wire.concurrent_mask, 0) &&
                bit(apart[1].wire.conflict_mask, 0) && !apart[1].concurrencyDropped,
            "and conflicts with the other camera, which keeps its own offer");
    require(!anchorSummary(apart[0]).empty() && anchorSummary(apart[0]).find("not concurrent") != std::string::npos &&
                anchorSummary(apart[1]).find("not concurrent") == std::string::npos,
            "and the log line says so for that camera only");

    // A camera with no multi-camera format at all is no worse off than one whose formats offer less.
    auto none = tabletCamera();
    for (auto& format : none.captureFormats) format.multiCamera = false;
    require(arrangeCameras(std::vector<CameraInfo> {none, other}, together, "tablet", CalibrationStore(), {})[0].concurrencyDropped,
            "no multi-camera format: not concurrent");
    // A multi-camera format that offers as much (here the same 12 MP, in a larger format) keeps the camera concurrent.
    auto equal = tabletCamera();
    equal.captureFormats[1].multiCamera = false;
    equal.captureFormats[2].multiCamera = true;
    const auto kept = arrangeCameras(std::vector<CameraInfo> {equal, other}, together, "tablet", CalibrationStore(), {});
    require(!kept[0].concurrencyDropped && kept[0].anchor == std::optional<std::size_t>(2) && bit(kept[0].wire.concurrent_mask, 1),
            "a multi-camera format offering as much on the first three keys keeps concurrency, and anchors there");
}

// The iPhone 15 Pro Max as discovery lists it: three virtual cameras (triple, dual wide, dual), the three single
// lenses and the front camera, and the multi-camera sets AVFoundation names for them: the dual camera and the wide
// lens each with the front camera, and the dual wide camera with the front camera. No set holds the ultra wide or
// the telephoto lens, nor two single lenses.
std::vector<CameraInfo> proMaxCameras()
{
    const auto make = [](const char* identifier, CameraPosition position, CameraLensKind lens) {
        CameraInfo info = tabletCamera();
        info.identifier = identifier;
        info.position = position;
        info.lens = lens;
        return info;
    };
    return {make("triple", CameraPosition::back, CameraLensKind::zoom),     make("dual-wide", CameraPosition::back, CameraLensKind::zoom),
            make("dual", CameraPosition::back, CameraLensKind::zoom),       make("wide", CameraPosition::back, CameraLensKind::wide),
            make("ultra-wide", CameraPosition::back, CameraLensKind::ultraWide), make("telephoto", CameraPosition::back, CameraLensKind::telephoto),
            make("front", CameraPosition::front, CameraLensKind::wide)};
}

// What the image's HAL announces from the wire's masks (camera_ids.hpp): the largest sets of cameras in which each two
// run together, by wire index.
std::vector<std::vector<std::uint32_t>> halSetsOf(const std::vector<CameraDescription>& cameras)
{
    std::vector<andarium::camera::Concurrency> concurrency;
    for (const auto& camera : cameras)
        concurrency.push_back({camera.index, camera.index, camera.wire.concurrent_mask, camera.wire.conflict_mask});
    return andarium::camera::concurrent_sets(concurrency);
}

using Sets = std::vector<std::vector<std::uint32_t>>;

// The guest takes every group of cameras in which each two are told to run together as one it may run at once, and the
// device runs `concurrentCameras` at once: it refuses a third. So no group the guest is told is larger, whatever the
// platform lists; of the pairs a set names, the primary back camera with the front camera is kept first, then the
// other pairs with the front camera.
void testTheGuestIsToldNoMoreThanTheDeviceRuns()
{
    const auto infos = proMaxCameras();
    const auto arrange = [&](std::vector<std::vector<std::size_t>> sets, std::vector<CameraInfo> from = {}) {
        if (from.empty()) from = infos;
        return arrangeCameras(from, sets, "phone", CalibrationStore(), {});
    };
    const auto check = [&](const std::vector<CameraDescription>& cameras) {
        for (const auto& set : halSetsOf(cameras))
            require(set.size() <= concurrentCameras, "no more cameras are told to run together than the device runs");
        for (const auto& camera : cameras)
            for (const auto& other : cameras) {
                if (camera.index == other.index) continue;
                const bool together = bit(camera.wire.concurrent_mask, other.index);
                require(together == bit(other.wire.concurrent_mask, camera.index) && together != bit(camera.wire.conflict_mask, other.index) &&
                            bit(camera.wire.conflict_mask, other.index) == bit(other.wire.conflict_mask, camera.index),
                        "a pair is told as one or the other, the same from both cameras");
            }
    };

    // The phone's platform lists the wide, ultra wide and telephoto lenses and the front camera as one set (every pair
    // of them runs together). The guest is told two at once: the wide lens and the front camera, and the front camera
    // with each other lens; the lenses conflict with each other.
    const auto all = arrange({{3, 4, 5, 6}});
    require(all.size() == 4 && all[0].info.identifier == "wide" && all[1].info.identifier == "front" &&
                all[2].info.identifier == "ultra-wide" && all[3].info.identifier == "telephoto",
            "wide, front, ultra wide, telephoto");
    check(all);
    require(halSetsOf(all) == Sets({{0, 1}, {1, 2}, {1, 3}}), "the wide lens and the front camera, the front camera and each other lens");
    for (const auto [a, b] : {std::make_pair(0u, 2u), std::make_pair(0u, 3u), std::make_pair(2u, 3u)})
        require(bit(all[a].wire.conflict_mask, b) && bit(all[b].wire.conflict_mask, a), "the lenses conflict with each other");

    // The log says which pairs the guest is told and the sets they come from.
    const std::vector<std::vector<std::size_t>> allSets {{3, 4, 5, 6}};
    require(concurrencySummary(all, allSets) ==
                "Camera: runs together: 0+1, 1+2, 1+3 (the device runs 2 cameras at once); the platform's multi-camera sets: {3 4 5 6}",
            "the log names the pairs and the platform's sets");
    require(concurrencySummary(arrange({}), {}) ==
                "Camera: runs together: none (the device runs 2 cameras at once); the platform's multi-camera sets: none",
            "and says when there are none");

    // Pairs listed one by one, in any order, are the same host.
    const auto paired = arrange({{4, 5}, {3, 5}, {5, 6}, {4, 6}, {3, 4}, {3, 6}});
    check(paired);
    require(halSetsOf(paired) == halSetsOf(all), "pairs of a set are the set");

    // A platform that lists the wide lens with the front camera and nothing else: the one pair.
    const auto probed = arrange({{2, 6}, {3, 6}, {1, 6}});
    check(probed);
    require(halSetsOf(probed) == Sets({{0, 1}}), "the wide lens and the front camera alone");
    for (const std::uint32_t lens : {2u, 3u})
        for (std::uint32_t other = 0; other < 4; ++other)
            if (other != lens) require(bit(probed[lens].wire.conflict_mask, other), "a lens no set names conflicts with every camera");

    // No set names the primary back camera: the front camera keeps the pairs it has.
    const auto frontOnly = arrange({{4, 6}, {5, 6}});
    check(frontOnly);
    require(halSetsOf(frontOnly) == Sets({{1, 2}, {1, 3}}), "the front camera with the lenses a set names");

    // A host with no front camera: its back cameras pair in wire order, and no group is larger than the device runs.
    const auto back = arrange({{0, 1, 2}}, {infos[3], infos[4], infos[5]});
    check(back);
    require(halSetsOf(back) == Sets({{0, 1}, {0, 2}}), "back cameras alone pair with the first");
}

// The session is told the anchors by the platform's camera index, not the wire's: the front camera is wire camera 1
// whatever place the platform lists it in.
void testAnchorsAreNamedByThePlatformsCameraIndex()
{
    CameraInfo front = tabletCamera();
    front.identifier = "tablet-front";
    front.position = CameraPosition::front;
    // Its own anchor: the 1080p format alone (a front camera's 12 MP photos are above its still limit).
    front.captureFormats.erase(front.captureFormats.begin(), front.captureFormats.begin() + 3);
    CameraInfo noFormats = tabletCamera();
    noFormats.identifier = "plain";
    noFormats.position = CameraPosition::external;
    noFormats.lens = CameraLensKind::external;
    noFormats.captureFormats.clear();
    // The platform lists the front camera first, then a camera with no formats, then the back camera.
    const std::vector<CameraInfo> infos {front, noFormats, tabletCamera()};
    const auto cameras = arrangeCameras(infos, {}, "tablet", CalibrationStore(), {});
    require(cameras.size() == 3 && cameras[0].platformIndex == 2 && cameras[1].platformIndex == 0 && cameras[2].platformIndex == 1,
            "the wire's order is the back camera, the front camera, the others");
    const auto anchors = anchorsByPlatformCamera(cameras, infos.size());
    require(anchors.size() == 3 && anchors[2] == cameras[0].anchor && anchors[0] == cameras[1].anchor && !anchors[1] &&
                anchors[2] == std::optional<std::size_t>(1) && anchors[0] == std::optional<std::size_t>(0),
            "each anchor is at its camera's platform index; a camera with no formats has none");
    require(anchorsByPlatformCamera({}, 2) == std::vector<std::optional<std::size_t>>(2), "no cameras, no anchors");
}

// A camera none of whose formats carries a stream is not offered, and the rest are numbered without it.
void testACameraWithoutAnAnchorIsNotOffered()
{
    CameraInfo broken = tabletCamera();
    broken.identifier = "broken";
    for (auto& format : broken.captureFormats) format.frameRates = {{7.5, 7.9}};
    CameraInfo front = tabletCamera();
    front.identifier = "tablet-front";
    front.position = CameraPosition::front;
    const auto cameras = arrangeCameras(std::vector<CameraInfo> {broken, front}, {}, "tablet", CalibrationStore(), {});
    require(cameras.size() == 1 && cameras[0].info.identifier == "tablet-front" && cameras[0].index == 0 && cameras[0].platformIndex == 1,
            "only the camera with an anchor, as camera 0 with its platform index");
}

// A host with one format per camera states its sizes and nothing else: they are all offered up to the cap.
void testHostsWithoutCaptureFormatsOfferTheirSizesUpToTheCap()
{
    auto info = tabletCamera();
    info.captureFormats.clear();
    info.stillSizes = {{4032, 3024}, {8064, 6048}, {640, 480}};
    const auto cameras = arrangeCameras(std::vector<CameraInfo> {info}, {}, "plain", CalibrationStore(), {});
    require(cameras[0].jpegSizes == std::vector<CameraStillSize>({{4032, 3024}, {640, 480}}), "the sizes under the cap");
    require(fastestRate(cameras[0]) == 60 && !cameras[0].anchor, "and every rate, with no anchor");
}

void testRawNeedsCalibration()
{
    const Host phone(SyntheticCameraProfile::phone);
    CalibrationStore store;
    auto cameras = phone.arrange(CameraTier::small, store);
    for (const auto& camera : cameras)
        require(!camera.raw() && !(camera.wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) && camera.wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_NONE,
                "no RAW without calibration");

    const auto key = CalibrationStore::lensKey(phone.infos[0]);
    require(store.save(phone.model, key, colorOf(2016, 1512)), "calibrate the wide lens");
    cameras = phone.arrange(CameraTier::small, store);
    require(cameras[0].raw() && (cameras[0].wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) &&
                cameras[0].wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_GRBG && cameras[0].wire.raw_width == 2016 &&
                cameras[0].wire.raw_height == 1512 && cameras[0].wire.black_level == 64 && cameras[0].wire.white_level == 1023,
            "a calibrated lens has RAW");
    require(cameras[0].wire.color_matrix1_q16[0] == 65536 && cameras[0].wire.reference_illuminant1 == 17, "with its colour data");
    require(cameras[0].wire.focal_length_um == 6900, "and the focal length the still revealed");
    require(!cameras[2].raw() && !cameras[3].raw(), "another lens has none");

    // Data that cannot describe a sensor is not enough.
    auto bad = colorOf(2016, 1512);
    bad.whiteLevel = bad.blackLevel;
    require(store.save(phone.model, key, bad), "save bad data");
    require(!phone.arrange(CameraTier::small, store)[0].raw(), "a white level at the black level is not calibration");
    bad = colorOf(2016, 1512);
    bad.bayer = BayerOrder::none;
    require(store.save(phone.model, key, bad), "save data with no pattern");
    require(!phone.arrange(CameraTier::small, store)[0].raw(), "nor is a missing pattern");
    // A calibration beyond the tier's still size is not advertised either.
    require(store.save(phone.model, key, colorOf(8064, 6048)), "save a 48 MP calibration");
    require(!phone.arrange(CameraTier::small, store)[0].raw() && phone.arrange(CameraTier::large, store)[0].raw(),
            "48 MP RAW needs the large tier");
}

// The sensor size and the focal length a camera publishes are a pair that describes its field of view: the guest takes
// the lens labels (focal length over the sensor's diagonal) and the 35 mm equivalents from it. A lens that has learned its
// colour data publishes the physical focal length of its still, and the size then follows the field of view, so the pair
// is the same ratio before and after the first still.
void testTheCalibratedPairKeepsTheFieldOfView()
{
    const Host phone(SyntheticCameraProfile::phone);
    struct Lens {
        std::size_t platform;
        std::size_t wire;
        double focalMillimetres;
    };
    // A wide, an ultra wide and a telephoto lens, with the physical focal lengths of a phone's.
    const Lens lenses[] = {{0, 0, 6.8}, {1, 2, 2.2}, {2, 3, 15.7}};
    const auto before = phone.arrange(CameraTier::small);
    CalibrationStore store;
    for (const auto& lens : lenses) {
        auto color = colorOf(2016, 1512);
        color.focalLengthMillimetres = lens.focalMillimetres;
        require(store.save(phone.model, CalibrationStore::lensKey(phone.infos[lens.platform]), color), "calibrate the lens");
    }
    const auto after = phone.arrange(CameraTier::small, store);
    for (const auto& lens : lenses) {
        const auto& uncalibrated = before[lens.wire].wire;
        const auto& calibrated = after[lens.wire].wire;
        require(calibrated.focal_length_um == std::uint32_t(std::lround(lens.focalMillimetres * 1000.0)),
                "the calibrated lens publishes its physical focal length");
        const double ratioBefore = double(uncalibrated.focal_length_um) / uncalibrated.sensor_width_um;
        const double ratioAfter = double(calibrated.focal_length_um) / calibrated.sensor_width_um;
        require(std::abs(ratioAfter - ratioBefore) <= ratioBefore * 0.002,
                "the focal length over the sensor's width is the field of view's ratio, before and after the first still");
        // The size keeps the pixel array's shape, and the pair gives the field of view the host states.
        const double shape = double(calibrated.sensor_height_um) / calibrated.sensor_width_um;
        require(std::abs(shape - double(calibrated.pixel_array_height) / calibrated.pixel_array_width) < 0.001,
                "the sensor keeps the pixel array's shape");
        const double fieldOfView = 2.0 * std::atan(calibrated.sensor_width_um / 2.0 / calibrated.focal_length_um) * 180.0 / 3.14159265358979;
        require(std::abs(fieldOfView - after[lens.wire].info.horizontalFieldOfView) < 0.1, "and the pair's field of view is the host's");
    }
    // A lens with no field of view stated keeps the nominal size beside the calibration's focal length.
    auto unstated = phone.infos[0];
    unstated.horizontalFieldOfView = 0;
    auto color = colorOf(2016, 1512);
    color.focalLengthMillimetres = 6.8;
    require(store.save(phone.model, CalibrationStore::lensKey(unstated), color), "calibrate it");
    const auto none = arrangeCameras(std::vector<CameraInfo> {unstated}, {}, phone.model, store, {});
    require(none[0].wire.focal_length_um == 6800 && none[0].wire.sensor_width_um == std::uint32_t(std::lround(unstated.pixelArrayWidth * 1.2)),
            "where the host states none, the size is the nominal one");
}

// Colour data is a fact of the format it was learned in: a camera is offered RAW from data learned in its anchor, and
// learns again when it was learned in another format.
void testACalibrationIsAdvertisedOnlyForTheAnchorItWasLearnedIn()
{
    auto info = tabletCamera();
    info.rawStills = true;
    info.rawStillSize = {1008, 756};
    const auto key = CalibrationStore::lensKey(info);
    const auto arrangedRaw = [&](const CalibrationStore& store) {
        return arrangeCameras(std::vector<CameraInfo> {info}, {}, "tablet", store, {})[0].raw();
    };
    CalibrationStore store;
    require(!arrangedRaw(store), "nothing learned, no RAW");
    auto color = colorOf(1008, 756);
    color.anchorWidth = 1920;
    color.anchorHeight = 1440;
    require(store.save("tablet", key, color) && arrangedRaw(store), "learned in the anchor, 1920x1440: RAW");
    color.anchorWidth = 4032;
    color.anchorHeight = 3024;
    require(store.save("tablet", key, color) && !arrangedRaw(store), "learned in another format: not offered, to be learned again");
    color.anchorWidth = 0;
    color.anchorHeight = 0;
    require(store.save("tablet", key, color) && !arrangedRaw(store), "learned in none that is stated: the same");
    // A host that lists no formats has no anchor to disagree with.
    info.captureFormats.clear();
    require(arrangedRaw(store), "a camera without formats takes the data as it is");
}

// What a live session holds on the host outside the guest's RAM, which the GPU budget gives up (the pool is B less
// the reserve): derived from the tier's limits, so a larger tier needs more.
void testHostMemoryReserve()
{
    const auto small = cameraHostMemory(CameraTier::small);
    require(maximumDeliveredFrameBytes(limitsOf(CameraTier::small)) == 4'423'680,
            "the capture buffer of the frame a 4:3 format delivers to a 1080p stream");
    require(offerRulesOf(limitsOf(CameraTier::small)).limits.maximumFrameBytes == maximumDeliveredFrameBytes(limitsOf(CameraTier::small)) &&
                offerRulesOf(limitsOf(CameraTier::small)).limits.frameRowAlignment == captureRowAlignment,
            "the offer is bounded by the reserve's buffer, in the reserve's rows");
    require(alignedFrameBytes(1920, 1440) == 4'423'680 && alignedFrameBytes(640, 480) == 768ull * 480 * 3 / 2 && alignedFrameBytes(256, 2) == 768,
            "the capture buffer's rows are padded to 256 bytes: a 1920x1440 buffer is 4,423,680 bytes where its pixels are 4,147,200");
    require(heldFramesPerCamera == 6 && concurrentCameras == 2, "six frames of each of two cameras are held");
    require(small.capturePools == 4'423'680ull * 6 * 2, "the aligned buffer, six held, two cameras");
    require(small.staging == 1920ull * 1080 * 3 / 2, "one frame");
    require(small.transformerScratch == 3'110'400ull * 2 + 3'732'480ull,
            "the transformer's scratch: a scaled buffer and a rotation scratch of a frame each, and vImage's temporaries (3,691,696 bytes measured, counted as 1.2 frames)");
    require(small.transformerScratch >= 3'110'400ull * 2 + 3'691'696ull, "which is at least what was measured");
    // The stills are what the device admits at once (`stillAdmissionBytes`): a JPEG capture holds the file twice, a RAW
    // capture its pixels three times (the platform's, the copy, the DNG).
    const auto pixels = limitsOf(CameraTier::small).maximumStillPixels;
    require(pixels == 12'700'000ull && jpegBufferBytes(pixels) == pixels * 3 / 2 + 65'000,
            "the guest's JPEG buffer holds a byte and a half a pixel and the Exif gap");
    require(jpegBufferBytes(1008ull * 756) == 1008ull * 756 * 3 / 2 + exifGapLength, "which is the plane size the device states for the stream");
    require(maximumJpegBytes(pixels) == pixels * 3 / 2 - 4 && maximumJpegBytes(pixels) + exifGapLength + 4 == jpegBufferBytes(pixels),
            "a file larger than the buffer less the gap fails its capture before it is copied: the gap's payload and header fit with it");
    require(jpegCaptureBytes(pixels) == maximumJpegBytes(pixels) * 2 && rawCaptureBytes(pixels) == pixels * 2 * 3, "the costs of one capture of each kind");
    require(jpegCaptureBytes(pixels, 11) == maximumJpegBytes(pixels) * 2 + pixels * 11,
            "a source that holds working storage for each pixel adds it to its JPEG capture");
    const auto system = uml::platform::systemCameraStillWorkingBytesPerPixel;
    require(CameraUse {}.stillWorkingBytesPerPixel == system && system == 2, "the ceiling holds the system camera's working storage");
    require(small.stills == jpegCaptureBytes(pixels, system) + rawCaptureBytes(pixels), "the fullest capture: a JPEG with a RAW");
    // The still the host turns half around holds its encoder's buffers beside the file; the front camera's largest, which
    // is turned, costs the more for them and is still inside what the ceiling admits, with no RAW still to share it.
    const auto turned = uml::platform::systemCameraTurnedStillWorkingBytesPerPixel;
    require(turned == 4, "four bytes a pixel for the encoder's frame and its image");
    for (const auto tier : {CameraTier::small, CameraTier::large}) {
        const auto frontStill = jpegCaptureBytes(uml::platform::frontStillMaximumPixels, system + turned);
        require(frontStill > jpegCaptureBytes(uml::platform::frontStillMaximumPixels, system), "a turned still costs more");
        require(frontStill <= stillAdmissionBytes(limitsOf(tier), CameraUse {2, false, system}),
                "and the ceiling of a tier admits it whatever else it holds");
    }
    require(small.total() == 53'084'160ull + 3'110'400ull + 9'953'280ull + small.stills, "the parts add up");
    require(cameraHostMemoryReserveMiB(CameraTier::small) == 197, "196.31 MiB, rounded up");
    // The ceiling is the fullest use: both cameras streaming together and a RAW still. A use that holds less is less.
    require(CameraUse {}.streams == concurrentCameras && CameraUse {}.rawStill, "the default use is the fullest");
    require(cameraHostMemory(CameraTier::small, {2, true, system}).total() == small.total(), "which is the ceiling");
    require(cameraHostMemory(CameraTier::small, {1, true, system}).total() == small.total() - 4'423'680ull * 6, "one stream fewer");
    require(cameraHostMemory(CameraTier::small, {2, false, system}).total() == small.total() - 76'200'000ull, "no RAW still");
    require(cameraHostMemoryReserveMiB(CameraTier::small, {1, false, 0}) == 75 && cameraHostMemoryReserveMiB(CameraTier::small, {1, true, 0}) == 147 &&
                cameraHostMemoryReserveMiB(CameraTier::small, {2, false, 0}) == 100 && cameraHostMemoryReserveMiB(CameraTier::small, {2, true, 0}) == 173,
            "the four figures of the small tier, for a source that holds nothing more");
    require(cameraHostMemoryReserveMiB(CameraTier::small, {1, false, system}) == 99 && cameraHostMemoryReserveMiB(CameraTier::small, {1, true, system}) == 171 &&
                cameraHostMemoryReserveMiB(CameraTier::small, {2, false, system}) == 124 && cameraHostMemoryReserveMiB(CameraTier::small, {2, true, system}) == 197,
            "and for the system camera's");
    require(cameraHostMemoryReserveMiB(CameraTier::small, {1, false, 11}) == 208,
            "a source that holds 11 bytes a pixel while it makes a JPEG (the synthetic camera) holds more than the ceiling of one that does not");
    // The tier every host gets is the small one, whatever its budget, and so is the reserve it holds.
    require(cameraHostMemoryReserveMiB(cameraTierForBudget(534ull << 20)) == 197);
    require(cameraHostMemoryReserveMiB(cameraTierForBudget(1249ull << 20)) == 197);
    const auto large = cameraHostMemory(CameraTier::large);
    require(large.capturePools == alignedFrameBytes(3840, 2880) * 6 * 2 && large.staging == 3840ull * 2160 * 3 / 2);
    require(large.transformerScratch == 12'441'600ull * 2 + 14'929'920ull);
    require(cameraHostMemoryReserveMiB(CameraTier::large) > 598 && large.total() > small.total(),
            "lifting the cap raises it with the tier");
}

// The first buffer of a configuration is checked against what the reserve counts for it.
void testTheBufferCheckLine()
{
    const std::uint64_t reserved = maximumDeliveredFrameBytes(limitsOf(CameraTier::small));
    const auto line = bufferCheckLine(2, 1920, 1440, 4'423'680, 2048, reserved);
    require(line.find("camera 2") != std::string::npos && line.find("1920x1440") != std::string::npos &&
                line.find("the reserve counts 4423680 a buffer") != std::string::npos &&
                line.find("larger than the reserve counts") == std::string::npos,
            "a buffer the reserve counts for says so without complaint");
    require(bufferCheckLine(0, 1920, 1440, 4'423'681, 2048, reserved).find("larger than the reserve counts") != std::string::npos,
            "a buffer above the tier's reserved one says that it is larger");
    require(bufferCheckLine(0, 1920, 1440, 4'147'200, 1920, reserved).find("larger than the reserve counts") == std::string::npos,
            "a tightly packed one is below it");
    // The comparison is with the tier's buffer, not with what a frame of the delivered size would take: a 1802x1534
    // frame's buffer of 4,712,448 bytes is over it.
    require(bufferCheckLine(0, 1802, 1534, 4'712'448, 2048, reserved).find("larger than the reserve counts") != std::string::npos,
            "the check is against the reserved buffer, whatever the frame's size");
}

void testWireFormats()
{
    for (const auto order : {BayerOrder::rggb, BayerOrder::grbg, BayerOrder::gbrg, BayerOrder::bggr})
        require(bayerOrderOf(rawWireFormat(order)) == order, "a Bayer order round-trips through its format");
    require(rawWireFormat(BayerOrder::none) == 0 && bayerOrderOf(VIRTIO_VIDEO_FORMAT_NV12) == BayerOrder::none, "none is none");
}

void testQueryPages()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto cameras = phone.arrange(CameraTier::small);
    const auto whole = cameraQueryResponse(cameras, 0, 4096, 0);
    const auto header = load<virtio_video_camera_query_resp>(whole);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY && header.num_cameras == 4 && header.num_descs == 4 &&
                header.next_index == 0 && header.session_flags == 0 && header.desc_size == sizeof(virtio_video_camera_desc),
            "the whole answer fits");
    require(whole.size() == sizeof(header) + 4 * sizeof(virtio_video_camera_desc), "four records");
    const auto third = load<virtio_video_camera_desc>(whole, sizeof(header) + 2 * sizeof(virtio_video_camera_desc));
    require(third.index == 2 && third.lens == VIRTIO_VIDEO_CAMERA_LENS_ULTRA_WIDE, "records are in order");

    // The answer pages when the guest's buffer is short, and says it is blocked.
    const std::size_t room = sizeof(header) + 2 * sizeof(virtio_video_camera_desc);
    auto page = cameraQueryResponse(cameras, 0, room, VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED);
    auto first = load<virtio_video_camera_query_resp>(page);
    require(first.num_descs == 2 && first.next_index == 2 && first.num_cameras == 4 &&
                (first.session_flags & VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED) && page.size() == room,
            "the first page");
    page = cameraQueryResponse(cameras, 2, room, 0);
    first = load<virtio_video_camera_query_resp>(page);
    require(first.num_descs == 2 && first.next_index == 0, "the last page ends the walk");
    page = cameraQueryResponse(cameras, 3, room, 0);
    first = load<virtio_video_camera_query_resp>(page);
    require(first.num_descs == 1 && first.next_index == 0, "a walk may start on the last camera");
    page = cameraQueryResponse(cameras, 4, room, 0);
    require(load<virtio_video_camera_query_resp>(page).num_descs == 0, "past the end is empty");
    // A buffer with no room for a record is refused: "complete" would end the walk.
    bool refused = false;
    try {
        (void)cameraQueryResponse(cameras, 1, sizeof(header) + sizeof(virtio_video_camera_desc) - 1, 0);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    require(refused, "no room for a record is refused");
}

void testFormatAnswers()
{
    const Host phone(SyntheticCameraProfile::phone);
    CalibrationStore store;
    store.save(phone.model, CalibrationStore::lensKey(phone.infos[0]), colorOf(2016, 1512));
    const auto cameras = phone.arrange(CameraTier::small, store);

    const auto video = cameraFormatsResponse(cameras[0], VIRTIO_VIDEO_CAMERA_STREAM_VIDEO);
    auto header = load<virtio_video_query_capability_resp>(video);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY && header.num_descs == 1, "video is one format");
    std::size_t at = sizeof(header);
    const auto desc = load<virtio_video_format_desc>(video, at);
    require(desc.format == VIRTIO_VIDEO_FORMAT_NV12 && (desc.planes_layout & VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER) &&
                desc.plane_align == 16 && desc.num_frames > 3,
            "NV12 in one buffer, rows at 16 bytes");
    at += sizeof(desc);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    for (std::uint32_t frame = 0; frame < desc.num_frames; ++frame) {
        const auto record = load<virtio_video_format_frame>(video, at);
        require(record.width.min == record.width.max && record.height.min == record.height.max && record.num_rates >= 1,
                "a frame record is one size with its rates");
        sizes.emplace_back(record.width.min, record.height.min);
        at += sizeof(record);
        for (std::uint32_t rate = 0; rate < record.num_rates; ++rate) {
            const auto range = load<virtio_video_format_range>(video, at);
            require(range.min >= 1 && range.max >= range.min, "a rate range");
            at += sizeof(range);
        }
    }
    require(at == video.size(), "the answer holds exactly its records");
    require(std::find(sizes.begin(), sizes.end(), std::make_pair(1920u, 1080u)) != sizes.end() &&
                std::find(sizes.begin(), sizes.end(), std::make_pair(1280u, 720u)) != sizes.end(),
            "1080p and 720p are offered");

    // Stills: JPEG sizes, and RAW only for the calibrated lens.
    const auto walk = [](const std::vector<std::uint8_t>& bytes, std::vector<std::uint32_t>& formats,
                         std::vector<std::pair<std::uint32_t, std::uint32_t>>& stillSizes) {
        const auto head = load<virtio_video_query_capability_resp>(bytes);
        std::size_t pos = sizeof(head);
        for (std::uint32_t index = 0; index < head.num_descs; ++index) {
            const auto d = load<virtio_video_format_desc>(bytes, pos);
            pos += sizeof(d);
            formats.push_back(d.format);
            for (std::uint32_t frame = 0; frame < d.num_frames; ++frame) {
                const auto record = load<virtio_video_format_frame>(bytes, pos);
                stillSizes.emplace_back(record.width.min, record.height.min);
                pos += sizeof(record) + record.num_rates * sizeof(virtio_video_format_range);
            }
        }
        return pos == bytes.size();
    };
    std::vector<std::uint32_t> formats;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stills;
    require(walk(cameraFormatsResponse(cameras[0], VIRTIO_VIDEO_CAMERA_STREAM_STILL), formats, stills), "the stills answer is whole");
    require(formats == std::vector<std::uint32_t> {VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG},
            "JPEG, and RAW for the calibrated lens");
    require(stills.front() == std::make_pair(4032u, 3024u) && stills.back() == std::make_pair(2016u, 1512u), "their sizes");
    formats.clear();
    stills.clear();
    require(walk(cameraFormatsResponse(cameras[2], VIRTIO_VIDEO_CAMERA_STREAM_STILL), formats, stills), "another lens' answer");
    require(formats == std::vector<std::uint32_t> {VIRTIO_VIDEO_FORMAT_CAMERA_JPEG}, "JPEG only");
    // The kinds that carry no format list answer with an empty list.
    require(load<virtio_video_query_capability_resp>(cameraFormatsResponse(cameras[0], VIRTIO_VIDEO_CAMERA_STREAM_META)).num_descs == 0,
            "a metadata stream has no formats");
}

void testRates()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto cameras = phone.arrange(CameraTier::small);
    const auto& wide = cameras[0];
    require(supportedFrameRate(wide, 1280, 720, 30) == 30, "30 fps is run at 720p");
    require(supportedFrameRate(wide, 1280, 720, 240) == 120, "a rate above the camera's is the camera's highest");
    require(supportedFrameRate(wide, 1920, 1080, 120) == 60, "1080p runs at 60");
    require(supportedFrameRate(wide, 640, 480, 0) == 1, "and below, the lowest");
    require(supportedFrameRate(wide, 3840, 2160, 30) == 0 && supportedFrameRate(wide, 1281, 720, 30) == 0 &&
                supportedFrameRate(wide, 8, 8, 30) == 0,
            "a size the camera does not offer has no rate");
    require(videoSizeAllowed(wide, 320, 240) && videoSizeAllowed(wide, 1920, 1080) && !videoSizeAllowed(wide, 1922, 1080) &&
                !videoSizeAllowed(wide, 1919, 1080),
            "a size may be below the camera's but not above, and is even");
}

// An anchored camera offers its anchor's rates up to the cap at every size.
void testAnAnchoredCamerasRates()
{
    const auto cameras = arrangeCameras(std::vector<CameraInfo> {tabletCamera()}, {}, "tablet", CalibrationStore(), {});
    const auto& camera = cameras[0];
    require(supportedFrameRate(camera, 1920, 1080, 60) == 30, "60 fps is not offered: the cap");
    require(supportedFrameRate(camera, 1920, 1080, 30) == 30 && supportedFrameRate(camera, 640, 480, 0) == 1, "30, and the lowest");
    require(supportedFrameRate(camera, 4032, 3024, 30) == 0 && supportedFrameRate(camera, 1922, 1080, 30) == 0,
            "a size the anchor does not carry has no rate");
    require(videoSizeAllowed(camera, 1440, 1080) && !videoSizeAllowed(camera, 1920, 1440),
            "a size inside the anchor's box: a 1920x1440 stream is more than 1080p");
}

void testLayout()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto small = phone.arrange(CameraTier::small);
    auto layout = summarizeLayout(small, CameraTier::small);
    require(layout.back && layout.front && layout.autofocus && layout.flash && layout.manualSensor && !layout.raw && layout.concurrent &&
                layout.frontMaximumVideoHeight == 1080 && layout.tier == CameraTier::small,
            "a phone's layout");
    layout = summarizeLayout(phone.arrange(CameraTier::large), CameraTier::large);
    require(layout.frontMaximumVideoHeight == 2160, "the front camera's best height follows the tier");

    const Host laptop(SyntheticCameraProfile::laptop);
    layout = summarizeLayout(laptop.arrange(CameraTier::small), CameraTier::small);
    require(!layout.back && layout.front && !layout.autofocus && !layout.flash && !layout.concurrent, "a laptop's layout");
    layout = summarizeLayout({}, CameraTier::small);
    require(!layout.back && !layout.front && !layout.manualSensor && !layout.raw && !layout.concurrent, "no cameras");
}

// ---------------------------------------------------------------------------
// The front camera's still limit, as the guest is told
// ---------------------------------------------------------------------------

bool sameVideo(const std::vector<VideoMode>& a, const std::vector<VideoMode>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const VideoMode& x, const VideoMode& y) {
        return x.width == y.width && x.height == y.height && x.minimumFrameRate == y.minimumFrameRate && x.maximumFrameRate == y.maximumFrameRate;
    });
}

const CameraDescription& ofPlatformCamera(const std::vector<CameraDescription>& cameras, std::size_t platform)
{
    for (const auto& camera : cameras)
        if (camera.platformIndex == platform) return camera;
    throw std::runtime_error("no such camera");
}

std::uint64_t areaOfStill(CameraStillSize size) { return std::uint64_t(size.width) * size.height; }

// The iPad mini's front camera (12 MP, ultra wide) with the tablet's back camera, as the platform lists them, the
// platform's concurrent set naming both.
CameraInfo frontCameraOf(const std::vector<CameraCaptureFormat>& formats, const char* identifier)
{
    CameraInfo front;
    front.identifier = identifier;
    front.position = CameraPosition::front;
    front.lens = CameraLensKind::wide;
    front.pixelArrayWidth = 4032;
    front.pixelArrayHeight = 3024;
    front.captureFormats = formats;
    for (const auto& format : front.captureFormats)
        front.videoFormats.push_back({format.width, format.height, 1, std::uint32_t(format.frameRates.front().maximum), format.multiCamera});
    // Every size some format makes, as the platform lists them.
    for (const auto& format : front.captureFormats)
        for (const auto& size : format.photoSizes)
            if (std::find(front.stillSizes.begin(), front.stillSizes.end(), size) == front.stillSizes.end()) front.stillSizes.push_back(size);
    return front;
}

std::vector<CameraInfo> miniCameras()
{
    CameraInfo back = tabletCamera();
    back.pixelArrayWidth = 4032;
    back.pixelArrayHeight = 3024;
    return {back, frontCameraOf(uml::testing::miniFrontFormats(), "mini-front")};
}

// Decision 2, what the guest is told of a front camera with the limit. The JPEG sizes it is offered are the limited
// ones (the guest can ask for nothing larger and the host makes every size it is offered). The pixel array it is told
// is the camera's full field of view, as it is for every camera: the guest's HAL takes it for the field of view and
// the RAW size, not as a promise of a JPEG that large. That is how the 12 MP tier cap already treats a 48 MP back
// camera, whose pixel array stays the sensor's while its JPEG sizes are capped; the two are shown side by side.
void testAFrontCameraIsToldItsLimitedStillsAndItsFullFieldOfView()
{
    const auto infos = miniCameras();
    const auto cameras = arrangeCameras(infos, std::vector<std::vector<std::size_t>> {{0, 1}}, "tablet", CalibrationStore(), {});
    require(cameras.size() == 2);
    const auto& front = ofPlatformCamera(cameras, 1);
    require(front.info.position == CameraPosition::front && front.index == 1 && front.anchor, "the front camera is wire camera 1, anchored");
    const auto& anchor = infos[1].captureFormats[*front.anchor];
    require(anchor.width == 1920 && anchor.height == 1440 && anchor.multiCamera, "it runs in the 1920x1440 multi-camera format");
    require(front.jpegSizes == std::vector<CameraStillSize>({{2016, 1512}, {1920, 1440}}), "it offers 2016x1512 and 1920x1440, not 3088x2316");
    require(front.info.stillSizes == front.jpegSizes, "and its own list is the offer");
    for (const auto& size : front.jpegSizes) require(areaOfStill(size) <= uml::platform::frontStillMaximumPixels, "within the limit");
    require(front.wire.pixel_array_width == 4032 && front.wire.pixel_array_height == 3024,
            "the pixel array is the field of view's, as listed, as the back camera's");
    // The same table as a back camera: 12 MP, and the same pixel array. The tier's 12 MP cap and the limit treat the
    // pixel array alike.
    auto asBack = infos[1];
    asBack.position = CameraPosition::back;
    asBack.identifier = "mini-as-back";
    const auto back = arrangeCameras(std::vector<CameraInfo> {asBack}, {}, "tablet", CalibrationStore(), {});
    require(back.size() == 1 && back[0].jpegSizes.front() == CameraStillSize({3088, 2316}) && back[0].wire.pixel_array_width == 4032 &&
                back[0].wire.pixel_array_height == 3024,
            "listed, the camera offers 3088x2316 and the same pixel array");
    CameraInfo big;
    big.identifier = "big-back";
    big.position = CameraPosition::back;
    big.lens = CameraLensKind::wide;
    big.pixelArrayWidth = 8064;
    big.pixelArrayHeight = 6048;
    big.captureFormats = {{4032, 3024, {{1, 30}}, {{4032, 3024}, {8064, 6048}}, true, false}};
    big.videoFormats.push_back({4032, 3024, 1, 30, false});
    big.stillSizes = {{8064, 6048}, {4032, 3024}};
    const auto capped = arrangeCameras(std::vector<CameraInfo> {big}, {}, "tablet", CalibrationStore(), optionsOf(CameraTier::small));
    require(capped.size() == 1 && capped[0].jpegSizes == std::vector<CameraStillSize>({{4032, 3024}}) &&
                capped[0].wire.pixel_array_width == 8064 && capped[0].wire.pixel_array_height == 6048,
            "a 48 MP back camera under the 12 MP tier cap: capped JPEG sizes, the sensor's pixel array");
    // The wire's formats are the offer: the JPEG descriptors list the limited sizes only.
    const auto frontSummary = anchorSummary(front);
    require(frontSummary.find("offers stills 2016x1512 1920x1440") != std::string::npos, "and the log line shows them");
    // A camera that lists no size within the limit is offered no native JPEG at all: the ceiling has no exception. It
    // still has its anchor and its video (Android derives its standard JPEG sizes from the offered video); the same table
    // as a back camera keeps its 12 MP photo.
    CameraInfo only = infos[1];
    only.identifier = "only-large";
    for (auto& format : only.captureFormats) format.photoSizes = {{4032, 3024}};
    only.stillSizes = {{4032, 3024}};
    const auto large = arrangeCameras(std::vector<CameraInfo> {only}, {}, "tablet", CalibrationStore(), {});
    require(large.size() == 1 && large[0].anchor && large[0].jpegSizes.empty() && large[0].info.stillSizes.empty() && !large[0].video.empty() &&
                anchorSummary(large[0]).find("offers stills none") != std::string::npos,
            "a front camera with only large sizes is offered no native JPEG, and keeps its video");
    auto largeAsBack = only;
    largeAsBack.position = CameraPosition::back;
    const auto control = arrangeCameras(std::vector<CameraInfo> {largeAsBack}, {}, "tablet", CalibrationStore(), {});
    require(control.size() == 1 && control[0].jpegSizes == std::vector<CameraStillSize>({{4032, 3024}}) && sameVideo(control[0].video, large[0].video),
            "as a back camera it offers the 12 MP photo, and the same video");
}

// Decision 3, concurrency is unchanged from before the limit. Whether a camera keeps it is decided on the formats as
// the platform lists them. The same table as a back camera is the control: the limit applies to the front camera only,
// so what the control is told is what the front camera was told before, and the front camera is told the same.
void testTheFrontStillLimitChangesNoConcurrency()
{
    const auto infos = miniCameras();
    const std::vector<std::vector<std::size_t>> together {{0, 1}};
    const auto limited = arrangeCameras(infos, together, "tablet", CalibrationStore(), {});
    const auto& front = ofPlatformCamera(limited, 1);
    // The listed formats do not keep the front camera's concurrency (its best format makes a 12 MP photo and no
    // multi-camera format does), and the limit does not change that.
    require(front.concurrencyDropped && !bit(front.wire.concurrent_mask, ofPlatformCamera(limited, 0).index) &&
                !bit(ofPlatformCamera(limited, 0).wire.concurrent_mask, front.index) && bit(front.wire.conflict_mask, ofPlatformCamera(limited, 0).index),
            "the front camera still runs alone and conflicts with the back camera, as before");
    require(anchorSummary(front).find("not concurrent") != std::string::npos, "and its log line says so");
    // The control: the same camera, listed as a back one, is not limited. Its concurrency is what the front camera's
    // was before the limit; the two agree, as do the back camera's.
    auto control = infos;
    control[1].position = CameraPosition::back;
    control[1].identifier = "mini-front-as-back";
    const auto unlimited = arrangeCameras(control, together, "tablet", CalibrationStore(), {});
    const auto& same = ofPlatformCamera(unlimited, 1);
    require(same.concurrencyDropped == front.concurrencyDropped && same.jpegSizes.front() == CameraStillSize({3088, 2316}),
            "the control is not limited, and drops its concurrency");
    // The whole video offer is the control's, although the anchor moved (2016x1512 to 1920x1440): only the stills differ.
    require(sameVideo(front.video, same.video) && front.video.front().width == 1920 && front.video.front().height == 1080,
            "the video offer, sizes and rates, is what it was");
    // The platform session's own predicate for the pair (the anchor's multi-camera flag) changes with the anchor, but the
    // device runs at most the cameras of its use: the layout of this pair is not concurrent, so it composes one slot (the
    // camera-device test testTheDeviceRunsOnlyTheCamerasItsUseAllows shows the second camera of such a use refused, whatever
    // the platform would run, and camera-layout that a pair which does not run together composes use.streams == 1).
    const auto layout = summarizeLayout(limited, CameraTier::small);
    const auto controlLayout = summarizeLayout(unlimited, CameraTier::small);
    require(!layout.concurrent && layout.concurrent == controlLayout.concurrent && layout.back && layout.front,
            "the layout of the pair is not concurrent, as it was");
    const auto& backBefore = ofPlatformCamera(unlimited, 0);
    const auto& backNow = ofPlatformCamera(limited, 0);
    require(backBefore.concurrencyDropped == backNow.concurrencyDropped && backBefore.jpegSizes == backNow.jpegSizes && backBefore.anchor == backNow.anchor &&
                backBefore.video.size() == backNow.video.size(),
            "the back camera is told what it was told");
    // The tables where the listed formats keep it: it stays, in a format that makes the same stills as the limit leaves.
    auto keeps = infos;
    for (auto& format : keeps[1].captureFormats)
        if (format.width == 1920 && format.height == 1440 && format.multiCamera) format.photoSizes = {{1920, 1440}, {2016, 1512}, {3088, 2316}};
    const auto kept = arrangeCameras(keeps, together, "tablet", CalibrationStore(), {});
    const auto& keptFront = ofPlatformCamera(kept, 1);
    require(!keptFront.concurrencyDropped && bit(keptFront.wire.concurrent_mask, ofPlatformCamera(kept, 0).index) && !bit(keptFront.wire.conflict_mask, ofPlatformCamera(kept, 0).index),
            "a front camera whose multi-camera format lists the photo as much as its best keeps its concurrency");
    require(keptFront.jpegSizes == std::vector<CameraStillSize>({{2016, 1512}, {1920, 1440}}), "and offers the limited stills, in a multi-camera format");
    require(keeps[1].captureFormats[*keptFront.anchor].multiCamera, "in a multi-camera format");
    // The rest of what the tests model: a table's concurrency as a front camera and as a back camera agree.
    for (const auto& table : {tabletCamera(), miniCameras()[1]}) {
        for (const bool dropOne : {false, true}) {
            auto asFront = table;
            asFront.identifier = "front";
            asFront.position = CameraPosition::front;
            if (dropOne)
                for (auto& format : asFront.captureFormats) format.multiCamera = format.multiCamera && format.width < 1500;
            auto asBack = asFront;
            asBack.identifier = "back";
            asBack.position = CameraPosition::back;
            CameraInfo partner = tabletCamera();
            partner.identifier = "partner";
            partner.position = CameraPosition::back;
            partner.lens = CameraLensKind::ultraWide;
            const auto f = arrangeCameras(std::vector<CameraInfo> {asFront, partner}, together, "tablet", CalibrationStore(), {});
            const auto b = arrangeCameras(std::vector<CameraInfo> {asBack, partner}, together, "tablet", CalibrationStore(), {});
            require(ofPlatformCamera(f, 0).concurrencyDropped == ofPlatformCamera(b, 0).concurrencyDropped,
                    "the same concurrency as the unlimited control");
        }
    }
}

// The second real front table (an iPhone 15 Pro Max's): the camera keeps its concurrency listed, so it keeps it with the
// limit, runs with the other camera in a multi-camera format, and is offered the limited stills; as a back camera (the
// control, unlimited) its concurrency, anchor and video are the same.
void testTheFrontStillLimitOnTheSecondRealFrontTable()
{
    CameraInfo back = tabletCamera();
    back.pixelArrayWidth = 4032;
    back.pixelArrayHeight = 3024;
    const auto front = frontCameraOf(uml::testing::phoneFrontFormats(), "phone-front");
    const std::vector<std::vector<std::size_t>> together {{0, 1}};
    const auto limited = arrangeCameras(std::vector<CameraInfo> {back, front}, together, "phone", CalibrationStore(), {});
    const auto& camera = ofPlatformCamera(limited, 1);
    require(!camera.concurrencyDropped && bit(camera.wire.concurrent_mask, ofPlatformCamera(limited, 0).index) &&
                !bit(camera.wire.conflict_mask, ofPlatformCamera(limited, 0).index),
            "it keeps its concurrency");
    require(camera.anchor && front.captureFormats[*camera.anchor].multiCamera && front.captureFormats[*camera.anchor].width == 1920 &&
                front.captureFormats[*camera.anchor].height == 1440,
            "in a 1920x1440 multi-camera format");
    require(camera.jpegSizes == std::vector<CameraStillSize>({{2016, 1512}, {1920, 1440}}), "it offers 2016x1512 and 1920x1440");
    require(camera.wire.pixel_array_width == 4032 && camera.wire.pixel_array_height == 3024, "the pixel array is the field of view's");
    auto asBack = front;
    asBack.position = CameraPosition::back;
    asBack.identifier = "phone-front-as-back";
    const auto unlimited = arrangeCameras(std::vector<CameraInfo> {back, asBack}, together, "phone", CalibrationStore(), {});
    const auto& control = ofPlatformCamera(unlimited, 1);
    require(control.concurrencyDropped == camera.concurrencyDropped && control.concurrencyDropped == false &&
                control.wire.concurrent_mask == camera.wire.concurrent_mask && control.wire.conflict_mask == camera.wire.conflict_mask,
            "the same concurrency as the unlimited control");
    require(control.anchor && front.captureFormats[*control.anchor].multiCamera && sameVideo(control.video, camera.video) &&
                control.jpegSizes == std::vector<CameraStillSize>({{4032, 3024}, {1920, 1440}}),
            "whose video offer is the same and whose stills are the 12 MP and 1920x1440 ones");
    require(front.captureFormats[*control.anchor].width == front.captureFormats[*camera.anchor].width &&
                front.captureFormats[*control.anchor].height == front.captureFormats[*camera.anchor].height,
            "and whose anchor is a format of the same size");
}

// A front camera's format that the limit leaves with no photo size, and the ranking as it is. A (1080p at 1-30 fps, a 12 MP
// photo) beats B (VGA, a VGA photo at 1-15 fps) before the limit and after it: it keeps its video and loses its native
// JPEG, which Android derives from the video. When B runs at 1-30 fps too, A's lost photo makes B win: the video offer
// falls from 1080p to VGA and the camera is offered B's photo. Both are what the ranking does on these tables, stated here;
// the iPad mini's and the iPhone's front tables are not like this (their anchors keep their video).
void testAFormatThatTheLimitLeavesWithNoPhotoMayStillBeTheAnchor()
{
    const auto camera = [](CameraPosition position, std::uint32_t slowMaximum) {
        CameraInfo info;
        info.identifier = "two-formats";
        info.position = position;
        info.lens = CameraLensKind::wide;
        info.captureFormats = {
            {1920, 1080, {{1, 30}}, {{4032, 3024}}, true, false},
            {640, 480, {{1, double(slowMaximum)}}, {{640, 480}}, true, false},
        };
        for (const auto& format : info.captureFormats)
            info.videoFormats.push_back({format.width, format.height, 1, std::uint32_t(format.frameRates.front().maximum), format.multiCamera});
        info.stillSizes = {{4032, 3024}, {640, 480}};
        return info;
    };
    const auto arranged = [&](CameraPosition position, std::uint32_t slowMaximum) {
        return arrangeCameras(std::vector<CameraInfo> {camera(position, slowMaximum)}, {}, "tablet", CalibrationStore(), {});
    };
    // B at 1-15 fps: A wins before (as a back camera) and after (as a front one).
    const auto controlA = arranged(CameraPosition::back, 15);
    const auto limitedA = arranged(CameraPosition::front, 15);
    require(controlA.size() == 1 && limitedA.size() == 1 && controlA[0].anchor == std::optional<std::size_t>(0) &&
                limitedA[0].anchor == std::optional<std::size_t>(0),
            "A wins before and after");
    require(controlA[0].jpegSizes == std::vector<CameraStillSize>({{4032, 3024}}) && limitedA[0].jpegSizes.empty(), "A's photo is offered before, not after");
    require(sameVideo(controlA[0].video, limitedA[0].video) && limitedA[0].video.front().width == 1920 && limitedA[0].video.front().height == 1080 &&
                limitedA[0].video.front().maximumFrameRate == 30,
            "and its video, 1080p at 1-30 fps, is the offer either way");
    require(anchorSummary(limitedA[0]).find("offers stills none") != std::string::npos, "the log line says no stills");
    // B at 1-30 fps: B wins after the limit.
    const auto controlB = arranged(CameraPosition::back, 30);
    const auto limitedB = arranged(CameraPosition::front, 30);
    require(controlB[0].anchor == std::optional<std::size_t>(0) && controlB[0].video.front().width == 1920, "A wins before");
    require(limitedB[0].anchor == std::optional<std::size_t>(1), "B wins after");
    require(limitedB[0].jpegSizes == std::vector<CameraStillSize>({{640, 480}}), "the camera is offered B's photo");
    require(!limitedB[0].video.empty() && limitedB[0].video.front().width == 640 && limitedB[0].video.front().height == 480 &&
                std::all_of(limitedB[0].video.begin(), limitedB[0].video.end(), [](const VideoMode& mode) { return mode.width <= 640 && mode.height <= 480; }),
            "and its video falls from 1080p to VGA");
}

// A front camera without capture formats (the synthetic phone's) offers a flat list of sizes, and the limit applies to that
// list too, with the tier's cap; a back camera's list has the tier's cap alone.
void testAFrontCameraWithoutFormatsIsLimitedToo()
{
    const Host phone(SyntheticCameraProfile::phone);
    const auto cameras = phone.arrange(CameraTier::small);
    std::size_t checked = 0;
    for (const auto& camera : cameras) {
        if (camera.info.position == CameraPosition::front) {
            require(camera.info.captureFormats.empty() && camera.jpegSizes == std::vector<CameraStillSize>({{2016, 1512}, {1008, 756}}),
                    "the front camera's flat list loses its 12 MP size");
            ++checked;
        } else if (camera.info.lens == CameraLensKind::wide) {
            require(camera.jpegSizes.front() == CameraStillSize({4032, 3024}), "the back wide camera's list has the tier's cap alone");
            ++checked;
        }
    }
    require(checked == 2, "both cameras were seen");
}

} // namespace

int main()
{
    testPhoneOrder();
    testAZoomCameraIsNotPublished();
    testEveryZoomRangeHoldsOne();
    testOtherHosts();
    testTiers();
    testOfferedStillsFollowTheAnchor();
    testZoomFollowsTheAnchorsRecommendation();
    testTheFramesAnAnchorDeliversNeverExceedTheReservesBuffer();
    testAConcurrentCameraKeepsItsOfferOrRunsAlone();
    testTheGuestIsToldNoMoreThanTheDeviceRuns();
    testAnchorsAreNamedByThePlatformsCameraIndex();
    testACameraWithoutAnAnchorIsNotOffered();
    testHostsWithoutCaptureFormatsOfferTheirSizesUpToTheCap();
    testHostMemoryReserve();
    testTheBufferCheckLine();
    testRawNeedsCalibration();
    testTheCalibratedPairKeepsTheFieldOfView();
    testACalibrationIsAdvertisedOnlyForTheAnchorItWasLearnedIn();
    testWireFormats();
    testQueryPages();
    testFormatAnswers();
    testRates();
    testAnAnchoredCamerasRates();
    testLayout();
    testAFrontCameraIsToldItsLimitedStillsAndItsFullFieldOfView();
    testTheFrontStillLimitChangesNoConcurrency();
    testTheFrontStillLimitOnTheSecondRealFrontTable();
    testAFormatThatTheLimitLeavesWithNoPhotoMayStillBeTheAnchor();
    testAFrontCameraWithoutFormatsIsLimitedToo();
    return 0;
}
