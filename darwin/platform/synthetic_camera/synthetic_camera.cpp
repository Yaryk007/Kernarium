// SPDX-License-Identifier: GPL-2.0-or-later
#include "synthetic_camera.hpp"

#include "nv12_transform.hpp"
#include "photo_dimensions.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace uml::platform {
namespace {

constexpr std::uint32_t markerCells = 64;
constexpr std::uint32_t markerHeight = 8;
constexpr std::uint8_t markerOne = 235;
constexpr std::uint8_t markerZero = 16;
// The marker's first row: across the middle of the picture, so that the centred crop and the scale a device makes of
// an anchored camera's frame for a stream of another shape or size keep it.
constexpr std::uint32_t markerTop(std::uint32_t height) noexcept
{
    return height > markerHeight ? ((height - markerHeight) / 2) & ~1u : 0;
}
// An exposure of 1/120 s at ISO 100 is the chart's unit brightness.
constexpr double referenceExposure = 1.0 / 120.0 * 100.0;
// What the automatic exposure runs at (1/120 s at ISO 100) without a compensation.
constexpr std::uint64_t automaticExposure = 8'333'333;
constexpr std::uint32_t whiteLevel = 16383;
constexpr std::uint32_t blackLevel = 1024;

std::uint64_t realNow() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// ----------------------------------------------------------------------------
// Profiles
// ----------------------------------------------------------------------------

CameraVideoFormat format(std::uint32_t width, std::uint32_t height, std::uint32_t maximumRate, bool multi = true)
{
    return {width, height, 1, maximumRate, multi};
}

// A camera that lists the capture formats of a platform: the format list, the
// video formats the session's own rule uses (in the same order, so a format's
// index is the same in both) and every JPEG size some format makes.
void listFormats(CameraInfo& info, std::vector<CameraCaptureFormat> formats)
{
    info.captureFormats = std::move(formats);
    info.videoFormats.clear();
    info.stillSizes.clear();
    for (const auto& format : info.captureFormats) {
        info.videoFormats.push_back(
            {format.width, format.height, 1, std::uint32_t(format.frameRates.front().maximum), format.multiCamera});
        for (const auto& size : format.photoSizes)
            if (std::find(info.stillSizes.begin(), info.stillSizes.end(), size) == info.stillSizes.end()) info.stillSizes.push_back(size);
    }
}

struct Profile {
    std::string model;
    std::vector<CameraInfo> cameras;
    std::vector<std::vector<std::size_t>> sets;
};

CameraInfo lens(const char* identifier, CameraPosition position, CameraLensKind kind)
{
    CameraInfo info;
    info.identifier = identifier;
    info.position = position;
    info.lens = kind;
    info.autofocus = position == CameraPosition::back;
    info.zoomMinimum = 1.0f;
    info.zoomMaximum = 8.0f;
    info.horizontalFieldOfView = kind == CameraLensKind::ultraWide ? 106.0f : kind == CameraLensKind::telephoto ? 28.0f : 63.0f;
    info.aperture = kind == CameraLensKind::ultraWide ? 2.2f : kind == CameraLensKind::telephoto ? 2.8f : 1.8f;
    info.minimumFocusDistanceMillimetres = position == CameraPosition::back ? 120.0f : 0.0f;
    info.isoMinimum = 25;
    info.isoMaximum = 3200;
    info.exposureMinimumNanoseconds = 20'000;
    info.exposureMaximumNanoseconds = 1'000'000'000;
    info.maximumWhiteBalanceGain = 4.0f;
    info.sensorOrientation = position == CameraPosition::front ? 270 : 90;
    info.manualExposure = info.manualFocus = info.manualWhiteBalance = true;
    info.manualFocus = info.autofocus;
    info.videoFormats = {format(640, 480, 60), format(1280, 720, 120), format(1920, 1080, 60), format(3840, 2160, 60, false)};
    info.stillSizes = {{4032, 3024}, {2016, 1512}, {1008, 756}};
    info.pixelArrayWidth = 4032;
    info.pixelArrayHeight = 3024;
    return info;
}

Profile profileOf(SyntheticCameraProfile which, bool frontPresentedAt90)
{
    Profile profile;
    switch (which) {
    case SyntheticCameraProfile::phone: {
        profile.model = "SyntheticPhone1,1";
        // The three lenses and the front camera, each with the zoom the system recommends for it on a phone of
        // three lenses. Only the wide lens runs with the front camera, as on that phone.
        CameraInfo wide = lens("synthetic-back-wide", CameraPosition::back, CameraLensKind::wide);
        wide.zoomMaximum = 6.0f;
        wide.flash = wide.torch = true;
        wide.rawStills = true;
        wide.rawStillSize = {2016, 1512};
        wide.stillSizes = {{8064, 6048}, {4032, 3024}, {2016, 1512}, {1008, 756}};
        wide.pixelArrayWidth = 8064;
        wide.pixelArrayHeight = 6048;
        CameraInfo ultra = lens("synthetic-back-ultrawide", CameraPosition::back, CameraLensKind::ultraWide);
        ultra.zoomMaximum = 3.0f;
        ultra.rawStills = true;
        ultra.rawStillSize = {2016, 1512};
        CameraInfo tele = lens("synthetic-back-tele", CameraPosition::back, CameraLensKind::telephoto);
        tele.zoomMaximum = 3.0f;
        tele.rawStills = true;
        tele.rawStillSize = {2016, 1512};
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.zoomMaximum = 4.0f;
        front.flash = front.torch = false;
        front.manualExposure = front.manualWhiteBalance = true;
        front.stillSizes = {{4032, 3024}, {2016, 1512}, {1008, 756}};
        profile.cameras = {wide, ultra, tele, front};
        profile.sets = {{0, 3}};
        break;
    }
    case SyntheticCameraProfile::tablet: {
        profile.model = "SyntheticTablet1,1";
        CameraInfo back = lens("synthetic-back", CameraPosition::back, CameraLensKind::wide);
        back.flash = back.torch = true;
        back.rawStills = true;
        back.rawStillSize = {1008, 756};
        // The formats an iPad mini's back camera lists, so the device anchors it: the 12 MP photos belong to the 4:3
        // formats, and the 1080p format makes none. The front camera lists none (it has one set of facts).
        listFormats(back, {
            {640, 480, {{1, 60}}, {{640, 480}}, true, true},
            {1280, 720, {{1, 120}}, {{1280, 720}}, true, true},
            {1440, 1080, {{1, 30}}, {{1440, 1080}, {2016, 1512}, {1008, 756}}, true, true},
            {1920, 1440, {{1, 30}}, {{1920, 1440}, {4032, 3024}, {2016, 1512}, {1008, 756}}, true, true},
            {4032, 3024, {{1, 30}}, {{4032, 3024}, {4224, 2376}}, true, false},
            {1920, 1080, {{1, 60}}, {{1920, 1080}, {2016, 1134}}, true, true},
            {3840, 2160, {{1, 60}}, {{3840, 2160}}, true, false},
        });
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.videoFormats = {format(640, 480, 60), format(1280, 720, 60), format(1920, 1080, 60)};
        front.stillSizes = {{2016, 1512}, {1008, 756}};
        front.pixelArrayWidth = 2016;
        front.pixelArrayHeight = 1512;
        front.zoomMaximum = 4.0f;
        profile.cameras = {back, front};
        profile.sets = {{0, 1}};
        break;
    }
    case SyntheticCameraProfile::tabletDualFront: {
        profile.model = "SyntheticTablet2,1";
        CameraInfo back = lens("synthetic-back", CameraPosition::back, CameraLensKind::wide);
        back.flash = back.torch = true;
        back.rawStills = true;
        back.rawStillSize = {1008, 756};
        // The smallest size that covers 1600 x 900, and a single camera's alone.
        back.videoFormats = {format(640, 480, 60), format(1280, 720, 120), format(1600, 900, 60, false),
                             format(1920, 1080, 60), format(3840, 2160, 60, false)};
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.videoFormats = {format(640, 480, 60), format(1280, 720, 60), format(1920, 1080, 60)};
        front.stillSizes = {{2016, 1512}, {1008, 756}};
        front.pixelArrayWidth = 2016;
        front.pixelArrayHeight = 1512;
        front.zoomMaximum = 4.0f;
        CameraInfo ultra = lens("synthetic-front-ultrawide", CameraPosition::front, CameraLensKind::ultraWide);
        ultra.videoFormats = {format(640, 480, 60), format(1280, 720, 60), format(1920, 1080, 60)};
        ultra.stillSizes = {{2016, 1512}, {1008, 756}};
        ultra.pixelArrayWidth = 2016;
        ultra.pixelArrayHeight = 1512;
        ultra.zoomMaximum = 2.0f;
        profile.cameras = {back, front, ultra};
        profile.sets = {{0, 1}, {0, 2}};
        break;
    }
    case SyntheticCameraProfile::laptop: {
        profile.model = "SyntheticLaptop1,1";
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.autofocus = false;
        front.manualExposure = front.manualFocus = front.manualWhiteBalance = false;
        front.sensorOrientation = 0;
        front.zoomMaximum = 1.0f;
        front.videoFormats = {format(640, 480, 30, false), format(1280, 720, 30, false), format(1920, 1080, 30, false)};
        front.stillSizes = {{1920, 1080}};
        front.pixelArrayWidth = 1920;
        front.pixelArrayHeight = 1080;
        front.isoMinimum = front.isoMaximum = 0;
        front.exposureMinimumNanoseconds = front.exposureMaximumNanoseconds = 0;
        front.maximumWhiteBalanceGain = 0;
        profile.cameras = {front};
        break;
    }
    }
    if (frontPresentedAt90)
        for (auto& camera : profile.cameras)
            if (camera.position == CameraPosition::front && camera.sensorOrientation == frontCameraOrientation) camera.presentationTurn = 180;
    return profile;
}

// ----------------------------------------------------------------------------
// The chart
// ----------------------------------------------------------------------------

struct Patch {
    std::uint8_t y, cb, cr;
};

// Six by four patches in the order of a colour checker's rows.
constexpr std::array<Patch, 24> patches = {{
    {80, 118, 140}, {150, 120, 138}, {100, 150, 118}, {90, 110, 125}, {120, 145, 130}, {170, 135, 90},
    {140, 80, 175}, {70, 160, 130}, {110, 120, 190}, {60, 150, 150}, {180, 100, 100}, {160, 90, 150},
    {50, 190, 110}, {100, 90, 100}, {90, 100, 190}, {200, 60, 140}, {110, 170, 170}, {120, 140, 60},
    {240, 128, 128}, {200, 128, 128}, {160, 128, 128}, {120, 128, 128}, {80, 128, 128}, {40, 128, 128},
}};

struct Picture {
    std::uint32_t width {};
    std::uint32_t height {};
    std::vector<std::uint8_t> luma;    // width x height
    std::vector<std::uint8_t> chroma;  // width x height / 2, interleaved
};

Picture baseChart(std::uint32_t width, std::uint32_t height)
{
    Picture picture;
    picture.width = width;
    picture.height = height;
    picture.luma.resize(std::size_t(width) * height);
    picture.chroma.resize(std::size_t(width) * (height / 2));
    const std::uint32_t gridTop = markerHeight * 2;
    const std::uint32_t gridHeight = height * 3 / 4 - gridTop;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t value = std::uint8_t(16 + 219 * std::uint64_t(x) / std::max<std::uint32_t>(width - 1, 1));
            std::uint8_t cb = 128, cr = 128;
            if (y >= gridTop && y < gridTop + gridHeight && x >= width / 8 && x < width * 7 / 8) {
                const auto column = (x - width / 8) * 6 / (width * 3 / 4);
                const auto row = (y - gridTop) * 4 / gridHeight;
                const auto& patch = patches[std::min<std::size_t>(row, 3) * 6 + std::min<std::size_t>(column, 5)];
                value = patch.y;
                cb = patch.cb;
                cr = patch.cr;
            } else if (y >= height * 3 / 4) {
                // A slanted edge, for sharpness.
                const bool right = x * 2 + (y - height * 3 / 4) > width;
                value = right ? 220 : 40;
            }
            picture.luma[std::size_t(y) * width + x] = value;
            if ((x & 1) == 0 && (y & 1) == 0) {
                const auto at = (std::size_t(y / 2) * width) + x;
                picture.chroma[at] = cb;
                picture.chroma[at + 1] = cr;
            }
        }
    }
    return picture;
}

struct Scene {
    double gain {1.0};
    int blur {};
    int tintCb {};
    int tintCr {};
    float zoom {1.0f};
    std::uint32_t counter {};
    std::uint16_t tag {};
};

std::uint8_t clampByte(int value) noexcept
{
    return std::uint8_t(std::clamp(value, 0, 255));
}

void drawMarker(std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma, std::uint32_t width, std::uint32_t height,
                std::uint16_t tag, std::uint32_t counter)
{
    if (height < markerHeight || width < 2 * markerCells) return;
    const std::uint32_t top = markerTop(height);
    const std::uint64_t first48 = (std::uint64_t(tag) << 32) | counter;
    const std::uint16_t check = std::uint16_t((first48 >> 32) ^ (first48 >> 16) ^ first48);
    const std::uint64_t bits = (first48 << 16) | check;
    // The cells share the picture's width, so a scale of the picture keeps their edges in proportion.
    for (std::uint32_t index = 0; index < markerCells; ++index) {
        const bool one = (bits >> (markerCells - 1 - index)) & 1;
        const std::size_t from = std::size_t(index) * width / markerCells;
        const std::size_t to = std::size_t(index + 1) * width / markerCells;
        for (std::uint32_t y = 0; y < markerHeight; ++y)
            std::memset(&luma[std::size_t(top + y) * width + from], one ? markerOne : markerZero, to - from);
    }
    for (std::uint32_t y = 0; y < markerHeight / 2; ++y)
        for (std::uint32_t x = 0; x + 1 < width; x += 2) {
            chroma[std::size_t(top / 2 + y) * width + x] = 128;
            chroma[std::size_t(top / 2 + y) * width + x + 1] = 128;
        }
}

// A picture turned half around.
void turnHalfAround(Picture& picture)
{
    std::vector<std::uint8_t> luma(picture.luma.size()), chroma(picture.chroma.size());
    const std::size_t width = picture.width;
    for (std::size_t row = 0; row < picture.height; ++row)
        reverseLumaRow(picture.luma.data() + (picture.height - 1 - row) * width, luma.data() + row * width, width);
    for (std::size_t row = 0; row < picture.height / 2; ++row)
        reverseChromaRow(picture.chroma.data() + (picture.height / 2 - 1 - row) * width, chroma.data() + row * width, width / 2);
    picture.luma = std::move(luma);
    picture.chroma = std::move(chroma);
}

Picture render(const Picture& base, const Scene& scene)
{
    const auto width = base.width;
    const auto height = base.height;
    Picture out;
    out.width = width;
    out.height = height;
    out.luma.resize(base.luma.size());
    out.chroma.resize(base.chroma.size());
    if (scene.zoom > 1.001f) {
        // Digital zoom: the centre of the chart, enlarged without smoothing.
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto sy = std::min<std::uint32_t>(
                height - 1, std::uint32_t(height / 2.0 + (y - height / 2.0) / scene.zoom));
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto sx = std::min<std::uint32_t>(
                    width - 1, std::uint32_t(width / 2.0 + (x - width / 2.0) / scene.zoom));
                out.luma[std::size_t(y) * width + x] = base.luma[std::size_t(sy) * width + sx];
                if (((x | y) & 1) == 0) {
                    const auto from = (std::size_t(sy / 2) * width) + (sx & ~1u);
                    const auto to = (std::size_t(y / 2) * width) + x;
                    out.chroma[to] = base.chroma[from];
                    out.chroma[to + 1] = base.chroma[from + 1];
                }
            }
        }
    } else {
        out.luma = base.luma;
        out.chroma = base.chroma;
    }
    if (scene.blur > 0) {
        const int radius = scene.blur;
        std::vector<std::uint8_t> row(width);
        for (std::uint32_t y = markerHeight; y < height; ++y) {
            auto* line = &out.luma[std::size_t(y) * width];
            for (std::uint32_t x = 0; x < width; ++x) {
                int sum = 0, count = 0;
                for (int d = -radius; d <= radius; ++d) {
                    const int at = int(x) + d;
                    if (at < 0 || at >= int(width)) continue;
                    sum += line[at];
                    ++count;
                }
                row[x] = std::uint8_t(sum / count);
            }
            std::memcpy(line, row.data(), width);
        }
    }
    if (scene.gain != 1.0) {
        std::array<std::uint8_t, 256> table {};
        for (int v = 0; v < 256; ++v) table[std::size_t(v)] = clampByte(int(std::lround(v * scene.gain)));
        for (auto& v : out.luma) v = table[v];
    }
    if (scene.tintCb != 0 || scene.tintCr != 0)
        for (std::size_t at = 0; at + 1 < out.chroma.size(); at += 2) {
            out.chroma[at] = clampByte(out.chroma[at] + scene.tintCb);
            out.chroma[at + 1] = clampByte(out.chroma[at + 1] + scene.tintCr);
        }
    drawMarker(out.luma, out.chroma, width, height, scene.tag, scene.counter);
    return out;
}

// ----------------------------------------------------------------------------
// Bayer stills
// ----------------------------------------------------------------------------

std::vector<std::uint16_t> bayerStill(const Picture& chart, BayerOrder order, std::uint32_t seed)
{
    std::vector<std::uint16_t> raw(std::size_t(chart.width) * chart.height);
    // Which colour each of the four sites of a 2 x 2 cell has: 0 red, 1 green, 2 blue.
    static constexpr std::uint8_t sites[4][4] = {{0, 1, 1, 2}, {0, 0, 0, 0}, {1, 0, 2, 1}, {1, 2, 0, 1}};
    const std::uint8_t* pattern = order == BayerOrder::rggb ? sites[0] : order == BayerOrder::grbg ? sites[2]
        : order == BayerOrder::gbrg ? sites[3] : (static_cast<void>(0), nullptr);
    static constexpr std::uint8_t bggr[4] = {2, 1, 1, 0};
    if (order == BayerOrder::bggr) pattern = bggr;
    if (pattern == nullptr) return {};
    constexpr double weights[3] = {0.9, 1.0, 0.8};
    std::uint32_t state = seed ? seed : 1u;
    for (std::uint32_t y = 0; y < chart.height; ++y)
        for (std::uint32_t x = 0; x < chart.width; ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const int noise = int(state % 17) - 8;
            const double level = chart.luma[std::size_t(y) * chart.width + x] / 255.0 * weights[pattern[(y & 1) * 2 + (x & 1)]];
            raw[std::size_t(y) * chart.width + x] =
                std::uint16_t(std::clamp<int>(int(blackLevel + level * (whiteLevel - blackLevel)) + noise, int(blackLevel),
                                              int(whiteLevel)));
        }
    return raw;
}

// A DNG forward matrix maps white-balanced camera values to XYZ with a D50 white, so equal camera values (1, 1, 1) must
// land on the D50 white point; the DNG validation of the Android CTS rejects a matrix whose rows do not sum to it.
// Every forward matrix of a synthetic calibration goes through here: each row of the shape given is scaled to the
// white point's component, which keeps its zeros and the proportions of its entries.
constexpr std::array<double, 3> d50White {0.9642, 1.0, 0.8249};

std::array<double, 9> mapsEqualValuesToD50(std::array<double, 9> matrix)
{
    for (std::size_t row = 0; row < 3; ++row) {
        const double sum = matrix[row * 3] + matrix[row * 3 + 1] + matrix[row * 3 + 2];
        for (std::size_t column = 0; column < 3; ++column) matrix[row * 3 + column] *= d50White[row] / sum;
    }
    return matrix;
}

CameraColorData syntheticColor(const CameraInfo& info, BayerOrder order)
{
    CameraColorData data;
    data.bayer = order;
    data.blackLevel = blackLevel;
    data.whiteLevel = whiteLevel;
    data.rawWidth = info.rawStillSize.width;
    data.rawHeight = info.rawStillSize.height;
    data.illuminant1 = 17;  // standard light A
    data.illuminant2 = 21;  // D65
    data.colorMatrix1 = {1.2, -0.3, -0.1, -0.4, 1.3, 0.1, -0.1, 0.2, 0.7};
    data.colorMatrix2 = {1.1, -0.2, -0.1, -0.3, 1.2, 0.1, -0.1, 0.2, 0.8};
    data.forwardMatrix1 = mapsEqualValuesToD50({0.7, 0.2, 0.1, 0.3, 0.7, 0.0, 0.0, 0.1, 0.8});
    data.forwardMatrix2 = mapsEqualValuesToD50({0.6, 0.3, 0.1, 0.3, 0.7, 0.0, 0.0, 0.0, 0.9});
    data.focalLengthMillimetres = info.lens == CameraLensKind::ultraWide ? 2.7 : info.lens == CameraLensKind::telephoto ? 9.0 : 6.9;
    data.intrinsics = {3000.0, 3000.0, info.pixelArrayWidth / 2.0, info.pixelArrayHeight / 2.0};
    return data;
}

// ----------------------------------------------------------------------------
// The camera and its sessions
// ----------------------------------------------------------------------------

class State;

struct RunningVideo {
    // The format it runs at.
    std::uint32_t width {};
    std::uint32_t height {};
    // What it was asked for, which the format covers.
    std::uint32_t requestedWidth {};
    std::uint32_t requestedHeight {};
    std::uint32_t frameRate {};
    std::uint32_t counter {};
    // How long the last frame lasted: its period, or its exposure when that was longer.
    std::uint64_t duration {};
    // The start of the frame the timer is producing, on the camera's clock: its timestamp, and the instant that its
    // deadline and the next one are counted from.
    std::uint64_t startNanoseconds {};
    std::chrono::steady_clock::time_point next;
};

struct PendingStill {
    CameraStillRequest request;
};

struct CameraState {
    CameraSettings settings;
    std::optional<RunningVideo> video;
    std::deque<PendingStill> stills;
    // Stills taken and not yet delivered: a camera that only takes stills has
    // an input in the session until the last one is.
    std::uint32_t stillsOut {};
    std::uint32_t stillCounter {};
    std::uint32_t seenFocusSerial {};
    std::uint32_t seenDistanceSerial {};
    // Where the lens is, in dioptres; the chart is at 1.
    float lens {1.0f};
    std::map<std::pair<std::uint32_t, std::uint32_t>, Picture> bases;
};

class Core {
public:
    Core(const SyntheticCameraOptions& options, Profile profile) : options_(options), profile_(std::move(profile)) {}

    const SyntheticCameraOptions options_;
    const Profile profile_;
    std::atomic<unsigned> opened {0};
    std::mutex anchorMutex;
    std::vector<std::optional<std::size_t>> anchors;  // guarded by anchorMutex
    CameraLimits anchorLimits;                        // guarded by anchorMutex
    std::atomic<unsigned> anchorCalls {0};
    // The frames made and not yet freed by their owners, and the most there were at once. A frame
    // can outlive the camera, so each holds the counts.
    struct FrameCounts {
        std::atomic<unsigned> alive {0};
        std::atomic<unsigned> peak {0};
    };
    const std::shared_ptr<FrameCounts> frames {std::make_shared<FrameCounts>()};
    std::atomic<unsigned> open {0};
    std::atomic<std::uint64_t> clock {1'000'000'000};
    std::atomic<bool> rawRefused {false};
    std::atomic<bool> rawFails {false};
    std::atomic<bool> stillsOfAnotherSize {false};
    std::atomic<bool> oversizedStills {false};
    std::atomic<std::uint64_t> stillPadding {0};
    std::atomic<unsigned> stillsTaken {0};
    // The system moved the anchored cameras out of their formats while the cameras were
    // interrupted; a session that opens puts them back by starting afresh.
    std::atomic<bool> movedOutOfAnchor {false};
    std::atomic<bool> repairRefused {false};
    std::atomic<unsigned> repairs {0};
    std::atomic<unsigned> framesOutsideAnchor {0};
    std::mutex mutex;  // guards the sessions list and the interrupted flag
    std::set<class Session*> sessions;
    bool interrupted {};
    CameraEventKind interruptKind {CameraEventKind::interrupted};
};

class Session final : public CameraSession {
public:
    Session(Core& core, CameraSessionDelegate& delegate) : core_(core), delegate_(delegate), cameras_(core.profile_.cameras.size())
    {
        {
            std::lock_guard lock(core_.mutex);
            core_.sessions.insert(this);
        }
        ++core_.opened;
        ++core_.open;
        core_.movedOutOfAnchor = false;  // a session that opens starts every camera in its anchor
        if (!core_.options_.manualClock) worker_ = std::thread([this] { run(); });
    }

    ~Session() override { stop(); }

    void setAnchors(std::span<const std::optional<std::size_t>> formatOfCamera, const CameraLimits& limits) override
    {
        {
            std::lock_guard lock(core_.anchorMutex);
            core_.anchors.assign(formatOfCamera.begin(), formatOfCamera.end());
            core_.anchorLimits = limits;
        }
        ++core_.anchorCalls;
    }

    bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate) override
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || camera >= cameras_.size() || width == 0 || height == 0 || frameRate == 0) return false;
        std::map<std::size_t, const CameraVideoFormat*> picks;
        if (!plan(camera, true, width, height, frameRate, picks)) return false;
        applyPicks(camera, true, width, height, frameRate, picks);
        wake_.notify_all();
        return true;
    }

    void stopVideo(std::size_t camera) override
    {
        std::lock_guard lock(mutex_);
        if (camera >= cameras_.size() || !cameras_[camera].video) return;
        cameras_[camera].video.reset();
        returnAlone();
    }

    void apply(std::size_t camera, const CameraSettings& settings) override
    {
        std::lock_guard lock(mutex_);
        if (!stopped_ && camera < cameras_.size()) cameras_[camera].settings = settings;
    }

    bool takeStill(std::size_t camera, const CameraStillRequest& request) override
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || camera >= cameras_.size()) return false;
        const auto& info = core_.profile_.cameras[camera];
        if (request.raw && (!info.rawStills || info.rawStillSize.width == 0 || core_.rawRefused)) return false;
        if (request.jpegWidth != 0 && !stillSize(info, request.jpegWidth, request.jpegHeight)) return false;
        // An anchored camera makes the sizes its anchor makes, as the platform's output does.
        if (const auto anchor = anchorOf(camera); anchor && request.jpegWidth != 0) {
            const auto made = anchoredStills(camera, *anchor);
            if (std::find(made.begin(), made.end(), CameraStillSize {request.jpegWidth, request.jpegHeight}) == made.end()) return false;
        }
        if (request.jpegWidth == 0 && !request.raw) return false;
        // A camera that only takes stills is an input of the session too: it
        // joins as a streaming camera does, or the still is refused.
        std::map<std::size_t, const CameraVideoFormat*> picks;
        if (!plan(camera, false, 0, 0, 0, picks)) return false;
        applyPicks(camera, false, 0, 0, 0, picks);
        ++cameras_[camera].stillsOut;
        ++core_.stillsTaken;
        cameras_[camera].stills.push_back({request});
        wake_.notify_all();
        return true;
    }

    void stop() override
    {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            wake_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard lock(core_.mutex);
            core_.sessions.erase(this);
        }
        --core_.open;
    }

    // Manual clock: a camera's pending stills, then `frames` frames.
    void pump(std::size_t camera, unsigned frames)
    {
        if (camera >= cameras_.size()) return;
        for (;;) {
            std::optional<CameraStillRequest> still;
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || cameras_[camera].stills.empty()) break;
                still = cameras_[camera].stills.front().request;
                cameras_[camera].stills.pop_front();
            }
            produceStill(camera, *still);
        }
        for (unsigned frame = 0; frame < frames; ++frame) {
            std::uint32_t rate;
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || !cameras_[camera].video) return;
                rate = cameras_[camera].video->frameRate;
            }
            // A camera the system moved out of its anchor goes on capturing in the other format, as the
            // platform's does; the client must not pass those frames on.
            if (!interrupted() || core_.movedOutOfAnchor) produceFrame(camera);
            std::uint64_t advance = 1'000'000'000ull / rate;
            {
                std::lock_guard lock(mutex_);
                if (cameras_[camera].video) advance = std::max(advance, cameras_[camera].video->duration);
            }
            core_.clock += advance;
        }
    }

    CameraSettings settings(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        return camera < cameras_.size() ? cameras_[camera].settings : CameraSettings {};
    }
    bool video(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        return camera < cameras_.size() && cameras_[camera].video.has_value();
    }
    std::pair<std::uint32_t, std::uint32_t> videoSize(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        if (camera >= cameras_.size() || !cameras_[camera].video) return {0, 0};
        return {cameras_[camera].video->width, cameras_[camera].video->height};
    }
    std::uint32_t rate(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        return camera < cameras_.size() && cameras_[camera].video ? cameras_[camera].video->frameRate : 0;
    }
    void deliver(CameraEvent event) { delegate_.cameraEvent(event); }

    // The interruption is over: every anchored camera the system moved out of its format is put
    // back, and only then is the session said to be back (recoverAnchors, which the Apple session
    // runs too). A refused repair says the session failed instead.
    void recover()
    {
        std::vector<std::size_t> running;
        {
            std::lock_guard lock(mutex_);
            for (std::size_t camera = 0; camera < cameras_.size(); ++camera)
                if (cameras_[camera].video) running.push_back(camera);
        }
        RecoverySteps steps;
        steps.outOfAnchor = [this](std::size_t) { return core_.movedOutOfAnchor.load(); };
        steps.repair = [this](std::size_t) {
            if (core_.repairRefused) return false;
            ++core_.repairs;
            return true;
        };
        steps.announce = [this](Recovery outcome) {
            if (outcome == Recovery::resumed) core_.movedOutOfAnchor = false;
            deliver({allCameras, outcome == Recovery::resumed ? CameraEventKind::resumed : CameraEventKind::failed});
        };
        // A failed recovery is final for this session, as the Apple session's is.
        (void)latch_.recover(running, steps);
    }

private:
    RecoveryLatch latch_;

    // The format a camera was told to run in, when it was told one.
    std::optional<std::size_t> anchorOf(std::size_t camera) const
    {
        std::lock_guard lock(core_.anchorMutex);
        const auto& info = core_.profile_.cameras[camera];
        if (camera >= core_.anchors.size() || !core_.anchors[camera] || *core_.anchors[camera] >= info.captureFormats.size()) return std::nullopt;
        return core_.anchors[camera];
    }

    // The sizes the anchor's format makes within the cap the device named.
    std::vector<CameraStillSize> anchoredStills(std::size_t camera, std::size_t anchor) const
    {
        std::lock_guard lock(core_.anchorMutex);
        return stillsOf(core_.profile_.cameras[camera].captureFormats[anchor], core_.anchorLimits.maximumStillPixels);
    }

    // A camera has an input in the session while it streams and while a still
    // it took is on its way.
    static bool hasInput(const CameraState& state) { return state.video.has_value() || state.stillsOut > 0; }

    // The pixels a second the session's cameras ask for, over which a
    // multi-camera session is over its hardware budget (AVFoundation's
    // hardwareCost above 1.0): two 1080p cameras at 60 frames a second are.
    static constexpr double hardwareBudgetPixelsPerSecond = 200e6;

    // What admitting `camera` asks of a multi-camera session, as AVFoundation
    // asks it: every camera with an input lies in one concurrent set, and all
    // of them use a format a multi-camera session takes (a camera that only
    // takes stills needs one to exist; the ones that stream move to the
    // smallest that covers their stream), and together they stay in the
    // hardware budget. `streams` is whether the camera streams video at the
    // asked size. False when the session cannot take it; nothing changes.
    bool plan(std::size_t camera, bool streams, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate,
              std::map<std::size_t, const CameraVideoFormat*>& picks) const
    {
        std::set<std::size_t> inputs {camera};
        for (std::size_t index = 0; index < cameras_.size(); ++index)
            if (hasInput(cameras_[index])) inputs.insert(index);
        const bool several = inputs.size() > 1;
        if (several && !joinable(inputs)) return false;
        double cost = 0;
        for (const auto index : inputs) {
            const bool self = index == camera;
            const auto& state = cameras_[index];
            const auto& info = core_.profile_.cameras[index];
            const auto anchor = anchorOf(index);
            if (!(self && streams) && !state.video) {
                const bool takesMulti = anchor ? info.captureFormats[*anchor].multiCamera
                                               : std::any_of(info.videoFormats.begin(), info.videoFormats.end(),
                                                             [](const auto& candidate) { return candidate.multiCamera; });
                if (several && !takesMulti) return false;
                continue;
            }
            const auto askedWidth = self && streams ? width : state.video->requestedWidth;
            const auto askedHeight = self && streams ? height : state.video->requestedHeight;
            const auto askedRate = self && streams ? frameRate : state.video->frameRate;
            // An anchored camera runs in its anchor, or the stream is refused.
            const auto* pick = anchor ? anchoredFormat(info, *anchor, askedWidth, askedHeight, askedRate, several)
                                      : chooseFormat(info, askedWidth, askedHeight, askedRate, several);
            if (pick == nullptr) return false;
            picks[index] = pick;
            cost += double(pick->width) * pick->height * askedRate;
        }
        return !several || cost <= hardwareBudgetPixelsPerSecond;
    }

    // Brings the cameras that stream to the formats the plan picked.
    void applyPicks(std::size_t camera, bool streams, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate,
                    const std::map<std::size_t, const CameraVideoFormat*>& picks)
    {
        for (const auto& [index, pick] : picks) {
            auto& state = cameras_[index];
            const bool self = index == camera && streams;
            const bool anchored = anchorOf(index).has_value();
            // An anchored camera never leaves its format.
            if (!self && (anchored || (state.video->width == pick->width && state.video->height == pick->height))) continue;
            RunningVideo video;
            video.width = pick->width;
            video.height = pick->height;
            // The platform delivers an anchored camera's frames at the stream's size in the format's own aspect ratio.
            if (anchored) {
                const auto delivered = deliveredFrameSize(pick->width, pick->height, width, height);
                video.width = delivered.width;
                video.height = delivered.height;
            }
            video.requestedWidth = self ? width : state.video->requestedWidth;
            video.requestedHeight = self ? height : state.video->requestedHeight;
            video.frameRate = self ? frameRate : state.video->frameRate;
            video.next = std::chrono::steady_clock::now();
            if (state.video) video.counter = state.video->counter;
            state.video = video;
        }
    }

    // A camera left alone goes back to the smallest format of any kind.
    void returnAlone()
    {
        std::vector<std::size_t> inputs;
        for (std::size_t index = 0; index < cameras_.size(); ++index)
            if (hasInput(cameras_[index])) inputs.push_back(index);
        if (inputs.size() != 1 || !cameras_[inputs.front()].video) return;
        auto& state = cameras_[inputs.front()];
        if (anchorOf(inputs.front())) return;  // an anchored camera never leaves its format
        const auto* pick = chooseFormat(core_.profile_.cameras[inputs.front()], state.video->requestedWidth,
                                        state.video->requestedHeight, state.video->frameRate, false);
        if (pick == nullptr || (pick->width == state.video->width && pick->height == state.video->height)) return;
        state.video->width = pick->width;
        state.video->height = pick->height;
    }

    // Whether the cameras that would run all lie in one concurrent set.
    bool joinable(const std::set<std::size_t>& running) const
    {
        return std::any_of(core_.profile_.sets.begin(), core_.profile_.sets.end(), [&](const auto& set) {
            return std::all_of(running.begin(), running.end(),
                               [&](auto member) { return std::find(set.begin(), set.end(), member) != set.end(); });
        });
    }

    // The anchor's video format when it covers the size at the rate (and a
    // multi-camera session takes it when several cameras run).
    static const CameraVideoFormat* anchoredFormat(const CameraInfo& info, std::size_t anchor, std::uint32_t width,
                                                   std::uint32_t height, std::uint32_t frameRate, bool multiCameraOnly)
    {
        const auto& candidate = info.videoFormats[anchor];
        if (candidate.width < width || candidate.height < height) return nullptr;
        if (frameRate < candidate.minimumFrameRate || frameRate > candidate.maximumFrameRate) return nullptr;
        if (multiCameraOnly && !candidate.multiCamera) return nullptr;
        return &candidate;
    }

    // The smallest format that covers the size at the rate.
    static const CameraVideoFormat* chooseFormat(const CameraInfo& info, std::uint32_t width, std::uint32_t height,
                                                 std::uint32_t frameRate, bool multiCameraOnly)
    {
        const CameraVideoFormat* best = nullptr;
        for (const auto& candidate : info.videoFormats) {
            if (candidate.width < width || candidate.height < height) continue;
            if (frameRate < candidate.minimumFrameRate || frameRate > candidate.maximumFrameRate) continue;
            if (multiCameraOnly && !candidate.multiCamera) continue;
            if (!best || std::uint64_t(candidate.width) * candidate.height < std::uint64_t(best->width) * best->height)
                best = &candidate;
        }
        return best;
    }

    // The JPEG size a request names: exactly one the camera lists, as the real
    // camera takes it, or none.
    static const CameraStillSize* stillSize(const CameraInfo& info, std::uint32_t width, std::uint32_t height)
    {
        const auto found = std::find(info.stillSizes.begin(), info.stillSizes.end(), CameraStillSize {width, height});
        return found == info.stillSizes.end() ? nullptr : &*found;
    }

    // What the settings make of an exposure.
    struct Outcome {
        CameraExposure exposure;
        Scene scene;
    };
    Outcome outcome(std::size_t camera, std::uint32_t frameRate)
    {
        const auto& info = core_.profile_.cameras[camera];
        auto& state = cameras_[camera];
        const auto& settings = state.settings;
        Outcome result;
        auto& exposure = result.exposure;
        exposure.activeCamera = std::uint32_t(camera);
        exposure.aperture = info.aperture;
        // No focal length: the lens's own, which the device takes from the camera's description.
        exposure.noiseProfile = {1e-4f, 1e-6f, 1e-4f, 1e-6f, 1e-4f, 1e-6f, 1e-4f, 1e-6f};
        exposure.neutralPoint = {0.5f, 1.0f, 0.6f};
        // Exposure.
        if (settings.exposureMode == CameraExposureMode::manual && info.manualExposure) {
            exposure.exposureNanoseconds = std::clamp(settings.exposureNanoseconds, info.exposureMinimumNanoseconds,
                                                      info.exposureMaximumNanoseconds);
            exposure.iso = std::clamp(settings.iso, info.isoMinimum, info.isoMaximum);
            exposure.ae = CameraControlState::inactive;
            result.scene.gain = exposure.exposureNanoseconds / 1e9 * exposure.iso / referenceExposure;
        } else {
            // The automatic exposure holds the chart at its unit brightness, and a compensation of n stops
            // moves the picture by n stops. The exposure takes what the frame the rate asks for and the
            // range the camera states allow (a camera that states none runs the exposure as asked), and
            // the sensitivity takes the rest: the exposure times the sensitivity is what the picture's
            // gain follows, and what the result reports.
            const std::uint32_t baseIso = std::max<std::uint32_t>(100, info.isoMinimum);
            const double wanted = double(automaticExposure) * std::exp2(double(settings.exposureBiasStops));
            double run = wanted;
            if (frameRate) run = std::min(run, 1e9 / double(frameRate));
            if (info.exposureMaximumNanoseconds > 0)
                run = std::clamp(run, double(info.exposureMinimumNanoseconds), double(info.exposureMaximumNanoseconds));
            exposure.exposureNanoseconds = std::max<std::uint64_t>(1, std::uint64_t(std::llround(run)));
            double iso = double(baseIso) * wanted / double(exposure.exposureNanoseconds);
            // A camera that states no sensitivity range is stated as [base, 100] by the HAL: it runs at the base.
            if (info.isoMaximum > 0) iso = std::clamp(iso, double(info.isoMinimum), double(info.isoMaximum));
            else iso = std::clamp(iso, double(baseIso), double(std::max<std::uint32_t>(baseIso, 100)));
            exposure.iso = std::uint32_t(std::llround(iso));
            exposure.ae = settings.exposureMode == CameraExposureMode::locked ? CameraControlState::locked
                                                                              : CameraControlState::converged;
            result.scene.gain = double(exposure.exposureNanoseconds) / double(automaticExposure) * double(exposure.iso) /
                                double(baseIso);
        }
        // A frame lasts at least as long as its exposure, whatever the rate asked for.
        if (frameRate)
            exposure.frameDurationNanoseconds = std::max<std::uint64_t>(1'000'000'000ull / frameRate, exposure.exposureNanoseconds);
        // Focus.
        // A new trigger scans in any mode, and the lens ends on the chart; a manual
        // lens moves only when the distance was written anew; a locked or manual
        // lens holds; continuous focus keeps the chart sharp.
        if (info.autofocus) {
            const bool moved = settings.focusDistanceSerial != state.seenDistanceSerial;
            state.seenDistanceSerial = settings.focusDistanceSerial;
            if (settings.focusTriggerSerial != state.seenFocusSerial) {
                state.seenFocusSerial = settings.focusTriggerSerial;
                state.lens = 1.0f;
                exposure.af = CameraControlState::searching;
            } else if (settings.focusMode == CameraFocusMode::manual && info.manualFocus) {
                if (moved) state.lens = settings.focusDistanceDioptres;
                exposure.af = CameraControlState::locked;
            } else if (settings.focusMode == CameraFocusMode::locked) {
                exposure.af = CameraControlState::locked;
            } else {
                state.lens = 1.0f;
                exposure.af = CameraControlState::converged;
            }
            exposure.lensPositionDioptres = state.lens;
            result.scene.blur = std::min(8, int(std::lround(std::fabs(exposure.lensPositionDioptres - 1.0f) * 2.0f)));
        }
        // White balance.
        if (settings.whiteBalanceMode == CameraWhiteBalanceMode::manual && info.manualWhiteBalance) {
            exposure.whiteBalanceGains = settings.whiteBalanceGains;
            exposure.awb = CameraControlState::inactive;
        } else {
            exposure.whiteBalanceGains = {1.8f, 1.0f, 1.6f};
            exposure.awb = settings.whiteBalanceMode == CameraWhiteBalanceMode::locked ? CameraControlState::locked
                                                                                       : CameraControlState::converged;
        }
        exposure.colorTemperature = std::uint32_t(std::clamp(5500.0f + (exposure.whiteBalanceGains[2] - exposure.whiteBalanceGains[0]) * 1000.0f,
                                                             2000.0f, 10000.0f));
        result.scene.tintCb = int(std::lround((exposure.whiteBalanceGains[2] / exposure.whiteBalanceGains[1] - 1.6f) * 20.0f));
        result.scene.tintCr = int(std::lround((exposure.whiteBalanceGains[0] / exposure.whiteBalanceGains[1] - 1.8f) * 20.0f));
        // Zoom.
        exposure.zoomRatio = std::clamp(settings.zoomRatio, info.zoomMinimum, info.zoomMaximum);
        result.scene.zoom = std::max(exposure.zoomRatio, 1.0f);
        result.scene.tag = chartTag(camera);
        return result;
    }

    const Picture& base(CameraState& state, std::uint32_t width, std::uint32_t height)
    {
        auto found = state.bases.find({width, height});
        if (found == state.bases.end()) {
            if (state.bases.size() > 4) state.bases.clear();
            found = state.bases.emplace(std::make_pair(width, height), baseChart(width, height)).first;
        }
        return found->second;
    }

    void produceFrame(std::size_t camera)
    {
        CameraFrame frame;
        std::uint64_t start = 0;
        {
            std::lock_guard lock(mutex_);
            auto& state = cameras_[camera];
            if (stopped_ || !state.video) return;
            start = state.video->startNanoseconds;
            auto outcome = this->outcome(camera, state.video->frameRate);
            outcome.scene.counter = ++state.video->counter;
            // A frame lasts as long as its exposure: a longer one delays the next (the timer has scheduled
            // the next one a period after this one).
            const auto period = 1'000'000'000ull / state.video->frameRate;
            state.video->duration = outcome.exposure.frameDurationNanoseconds;
            if (!core_.options_.manualClock && state.video->duration > period)
                state.video->next += std::chrono::nanoseconds(state.video->duration - period);
            if (core_.movedOutOfAnchor) ++core_.framesOutsideAnchor;
            // The picture counts itself while a frame holds it.
            const auto counts = core_.frames;
            const unsigned alive = ++counts->alive;
            for (unsigned peak = counts->peak; alive > peak && !counts->peak.compare_exchange_weak(peak, alive);) {
            }
            std::shared_ptr<Picture> picture(new Picture(render(base(state, state.video->width, state.video->height), outcome.scene)),
                                             [counts](Picture* freed) {
                                                 delete freed;
                                                 --counts->alive;
                                             });
            frame.width = picture->width;
            frame.height = picture->height;
            frame.luma = picture->luma;
            frame.lumaStride = picture->width;
            frame.chroma = picture->chroma;
            frame.chromaStride = picture->width;
            frame.owner = picture;
            frame.exposure = outcome.exposure;
        }
        const auto latency = std::uint64_t(core_.options_.latencyMilliseconds) * 1'000'000;
        // The timer's frame starts when it was scheduled to (or when it ran, if that was a period late): one instant for
        // the deadline and the timestamp, not the time the frame took to render and reach the delegate.
        const auto now = core_.options_.manualClock ? core_.clock.load() : (start != 0 ? start : realNow());
        frame.timestampNanoseconds = now > latency ? now - latency : 0;
        delegate_.cameraFrame(camera, std::move(frame));
    }

    bool interrupted() const
    {
        std::lock_guard lock(core_.mutex);
        return core_.interrupted;
    }

    void produceStill(std::size_t camera, const CameraStillRequest& request)
    {
        const auto& info = core_.profile_.cameras[camera];
        CameraStillResult result;
        result.token = request.token;
        if (!interrupted()) {
            std::lock_guard lock(mutex_);
            auto& state = cameras_[camera];
            auto outcome = this->outcome(camera, 0);
            outcome.scene.counter = ++state.stillCounter;
            outcome.exposure.flashFired = request.flash == CameraFlashMode::on && info.flash;
            result.exposure = outcome.exposure;
            if (request.jpegWidth != 0) {
                const auto* size = stillSize(info, request.jpegWidth, request.jpegHeight);
                // A platform that made another size than the request names (a test
                // hook): half of it each way.
                const auto divisor = core_.stillsOfAnotherSize ? 2u : 1u;
                auto picture = render(base(state, size->width / divisor, size->height / divisor), outcome.scene);
                // A camera with a presentation turn makes its stills turned, never unturned (the platform's contract).
                if (info.presentationTurn == 180) turnHalfAround(picture);
                result.jpeg = encodeSyntheticJpeg(picture.luma.data(), picture.width, picture.chroma.data(),
                                                  picture.width, picture.width, picture.height, request.jpegQuality);
                if (core_.oversizedStills && !result.jpeg.empty()) {
                    const std::size_t bound = request.maximumJpegBytes != 0 ? std::size_t(request.maximumJpegBytes)
                                                                           : std::size_t(request.jpegWidth) * request.jpegHeight;
                    result.jpeg.resize(std::max(result.jpeg.size(), bound + 1), 0);
                }
                if (const std::uint64_t padding = core_.stillPadding; padding != 0 && !result.jpeg.empty())
                    result.jpeg.resize(std::max<std::size_t>(result.jpeg.size(), padding), 0);
            }
            if (request.raw && !core_.rawFails) {
                const auto picture = render(base(state, info.rawStillSize.width, info.rawStillSize.height), outcome.scene);
                result.raw = bayerStill(picture, BayerOrder::rggb, 0x5eed0000u + state.stillCounter);
                result.color = syntheticColor(info, BayerOrder::rggb);
            }
            result.ok = (request.jpegWidth != 0 && !result.jpeg.empty()) || (request.raw && !result.raw.empty());
        }
        delegate_.cameraStill(camera, std::move(result));
        // The still is delivered: a camera that only took stills leaves the session.
        std::lock_guard lock(mutex_);
        if (cameras_[camera].stillsOut > 0) --cameras_[camera].stillsOut;
        returnAlone();
    }

    void run()
    {
        std::unique_lock lock(mutex_);
        while (!stopped_) {
            const auto now = std::chrono::steady_clock::now();
            const auto nowNanoseconds = realNow();
            std::optional<std::chrono::steady_clock::time_point> wake;
            std::vector<std::size_t> due;
            std::vector<std::pair<std::size_t, CameraStillRequest>> stills;
            for (std::size_t camera = 0; camera < cameras_.size(); ++camera) {
                auto& state = cameras_[camera];
                while (!state.stills.empty()) {
                    stills.emplace_back(camera, state.stills.front().request);
                    state.stills.pop_front();
                }
                if (!state.video) continue;
                if (state.video->next <= now) {
                    due.push_back(camera);
                    // The frame starts at its deadline, unless the timer thread was busy then (a delegate callback that ran
                    // past it): a thread cannot start a frame before it is back, so that one starts when the previous
                    // work ended, which is never before that callback returned. A thread that was only asleep past the
                    // deadline (wake-up latency) keeps the schedule, or the rate would fall by that latency every frame;
                    // one that was asleep a period or more (a stall) starts now, with no burst to catch up. The next
                    // frame is a period after the start, and produceFrame adds what its exposure takes beyond the
                    // period: the next deadline is a whole frame duration after the start of this frame.
                    const auto period = std::chrono::nanoseconds(1'000'000'000ull / state.video->frameRate);
                    auto begin = state.video->next;
                    std::uint64_t beginNanoseconds = nowNanoseconds;
                    if (begin < busyEnd_) {
                        begin = busyEnd_;
                        beginNanoseconds = busyEndNanoseconds_;
                    } else {
                        const auto late = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
                        beginNanoseconds = nowNanoseconds > late ? nowNanoseconds - late : nowNanoseconds;
                    }
                    if (now - begin >= period) {
                        begin = now;
                        beginNanoseconds = nowNanoseconds;
                    }
                    state.video->next = begin + period;
                    state.video->startNanoseconds = beginNanoseconds;
                }
                if (!wake || state.video->next < *wake) wake = state.video->next;
            }
            if (!due.empty() || !stills.empty()) {
                lock.unlock();
                for (auto& [camera, request] : stills) produceStill(camera, request);
                for (const auto camera : due) {
                    if (!interrupted()) produceFrame(camera);
                }
                lock.lock();
                busyEnd_ = std::chrono::steady_clock::now();
                busyEndNanoseconds_ = realNow();
                continue;
            }
            if (wake) wake_.wait_until(lock, *wake);
            else wake_.wait(lock);
        }
    }

    Core& core_;
    CameraSessionDelegate& delegate_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<CameraState> cameras_;
    std::thread worker_;
    bool stopped_ {};
    // When the timer thread last came back from producing frames and stills, on both clocks.
    std::chrono::steady_clock::time_point busyEnd_ {};
    std::uint64_t busyEndNanoseconds_ {};
};

}  // namespace
}  // namespace uml::platform

namespace uml::platform {
namespace {

// Reads the commands of a named pipe on a thread of its own, for as long as it lives. The pipe is opened for reading and
// writing, so it has a reader whether or not a writer is there, and a writer's close ends no read.
class ControlPipe {
public:
    ControlPipe(const std::string& path, std::function<void(const SyntheticControlCommand&)> apply) : apply_(std::move(apply))
    {
        if (::mkfifo(path.c_str(), 0600) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "synthetic camera: cannot make the control pipe: %s\n", std::strerror(errno));
            return;
        }
        descriptor_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (descriptor_ < 0) {
            std::fprintf(stderr, "synthetic camera: cannot open the control pipe: %s\n", std::strerror(errno));
            return;
        }
        thread_ = std::thread([this] { run(); });
    }
    ~ControlPipe()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    ControlPipe(const ControlPipe&) = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;

private:
    void run()
    {
        std::string pending;
        char chunk[256];
        while (!stop_) {
            pollfd waiting {descriptor_, POLLIN, 0};
            if (::poll(&waiting, 1, 50) <= 0 || (waiting.revents & POLLIN) == 0) continue;
            const auto count = ::read(descriptor_, chunk, sizeof(chunk));
            if (count <= 0) continue;
            pending.append(chunk, std::size_t(count));
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                const std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (const auto command = parseSyntheticControlLine(line)) apply_(*command);
                else std::fprintf(stderr, "synthetic camera: unknown control line\n");
            }
            if (pending.size() > 4096) pending.clear();
        }
    }

    std::function<void(const SyntheticControlCommand&)> apply_;
    int descriptor_ {-1};
    std::atomic<bool> stop_ {};
    std::thread thread_;
};

class SyntheticCameraImpl final : public SyntheticCamera {
public:
    explicit SyntheticCameraImpl(const SyntheticCameraOptions& options)
        : core_(options, profileOf(options.profile, options.frontPresentedAt90))
    {
        if (!options.controlPath.empty()) {
            control_ = std::make_unique<ControlPipe>(options.controlPath, [this](const SyntheticControlCommand& command) {
                std::fprintf(stderr, "synthetic camera: %s\n", command.interrupt ? "interrupted" : "resumed");
                if (command.interrupt) interrupt(command.kind);
                else resume();
            });
        }
    }

    std::vector<CameraInfo> cameras() const override { return core_.profile_.cameras; }
    std::vector<std::vector<std::size_t>> concurrentSets() const override { return core_.profile_.sets; }
    std::string hostModel() const override { return core_.profile_.model; }
    std::uint64_t now() const noexcept override { return core_.options_.manualClock ? core_.clock.load() : realNow(); }
    std::unique_ptr<Nv12Transformer> makeFrameTransformer() const override { return makePortableNv12Transformer(); }
    /* The source renders its pictures and converts one to RGBA before it
     * encodes: 130,138,376 bytes of footprint for one 12 MP file, 10.7 bytes a
     * pixel, of which the device's own copies of the file are counted apart. */
    std::uint32_t stillWorkingBytesPerPixel() const noexcept override { return 11; }

    std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) override
    {
        if (!core_.options_.authorized) return nullptr;
        auto session = std::make_unique<Session>(core_, delegate);
        // The cause of an interruption outlives a session: one opened while it
        // lasts is interrupted as it starts, as the host's is. A session opened
        // while the application is away never ran in the foreground, so what it
        // reports is an ordinary interruption: the departure that ended the video
        // of an earlier session is not its own.
        CameraEventKind kind {};
        bool interrupted;
        {
            std::lock_guard lock(core_.mutex);
            interrupted = core_.interrupted;
            kind = core_.interruptKind;
        }
        if (kind == CameraEventKind::interruptedInBackground) kind = CameraEventKind::interrupted;
        if (interrupted) session->deliver({allCameras, kind});
        return session;
    }

    std::vector<std::optional<std::size_t>> sessionAnchors() const override
    {
        std::lock_guard lock(core_.anchorMutex);
        return core_.anchors;
    }
    CameraLimits sessionLimits() const noexcept override
    {
        std::lock_guard lock(core_.anchorMutex);
        return core_.anchorLimits;
    }
    unsigned sessionAnchorCalls() const noexcept override { return core_.anchorCalls; }
    unsigned framesAlive() const noexcept override { return core_.frames->alive; }
    unsigned framePeak() const noexcept override { return core_.frames->peak; }
    void resetFramePeak() noexcept override { core_.frames->peak = core_.frames->alive.load(); }
    unsigned sessionsOpened() const noexcept override { return core_.opened; }
    unsigned sessionsOpen() const noexcept override { return core_.open; }

    CameraSettings appliedSettings(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.settings(camera); }, CameraSettings {});
    }
    bool videoRunning(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.video(camera); }, false);
    }
    std::uint32_t videoFrameRate(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.rate(camera); }, std::uint32_t(0));
    }

    void pump(std::size_t camera, unsigned frames) override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->pump(camera, frames);
    }

    void advanceClock(std::uint64_t nanoseconds) override { core_.clock += nanoseconds; }

    void interrupt(CameraEventKind kind) override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            core_.interrupted = true;
            core_.interruptKind = kind;
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->deliver({allCameras, kind});
    }

    std::pair<std::uint32_t, std::uint32_t> videoSize(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.videoSize(camera); },
                            std::pair<std::uint32_t, std::uint32_t> {0, 0});
    }

    void post(CameraEvent event) override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->deliver(event);
    }

    void refuseRaw(bool refuse) override { core_.rawRefused = refuse; }
    void failRaw(bool fail) override { core_.rawFails = fail; }
    void stillsOfAnotherSize(bool another) override { core_.stillsOfAnotherSize = another; }
    void oversizedStills(bool oversized) override { core_.oversizedStills = oversized; }
    void paddedStills(std::uint64_t bytes) override { core_.stillPadding = bytes; }
    unsigned stillsTaken() const noexcept override { return core_.stillsTaken; }

    void resume() override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            core_.interrupted = false;
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->recover();
    }
    void moveOutOfAnchor(bool moved) override { core_.movedOutOfAnchor = moved; }
    void refuseRepair(bool refuse) override { core_.repairRefused = refuse; }
    unsigned repairs() const noexcept override { return core_.repairs; }
    unsigned framesOutsideAnchor() const noexcept override { return core_.framesOutsideAnchor; }

private:
    template<typename Function, typename Result>
    Result withSessions(Function function, Result fallback) const
    {
        std::lock_guard lock(core_.mutex);
        for (auto* session : core_.sessions) return function(*session);
        return fallback;
    }

    mutable Core core_;
    // After the core: it applies commands to the camera, and stops before the camera does.
    std::unique_ptr<ControlPipe> control_;
};

}  // namespace

std::optional<SyntheticControlCommand> parseSyntheticControlLine(std::string_view line)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!line.empty() && blank(line.front())) line.remove_prefix(1);
    while (!line.empty() && blank(line.back())) line.remove_suffix(1);
    std::string lower(line);
    for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "resume") return SyntheticControlCommand {false, CameraEventKind::resumed};
    if (lower.rfind("interrupt", 0) != 0) return std::nullopt;
    auto cause = std::string_view(lower).substr(9);
    while (!cause.empty() && blank(cause.front())) cause.remove_prefix(1);
    if (cause.empty() || cause == "other") return SyntheticControlCommand {true, CameraEventKind::interrupted};
    if (cause == "background") return SyntheticControlCommand {true, CameraEventKind::interruptedInBackground};
    if (cause == "multitasking") return SyntheticControlCommand {true, CameraEventKind::interruptedByMultitasking};
    if (cause == "pressure") return SyntheticControlCommand {true, CameraEventKind::interruptedBySystemPressure};
    return std::nullopt;
}

std::unique_ptr<SyntheticCamera> makeSyntheticCamera(const SyntheticCameraOptions& options)
{
    return std::make_unique<SyntheticCameraImpl>(options);
}

std::uint16_t chartTag(std::size_t camera) noexcept
{
    return std::uint16_t(0x5a00u + (camera & 0xffu));
}

ChartMarker decodeChartMarker(const std::uint8_t* luma, std::size_t stride, std::uint32_t width,
                              std::uint32_t height) noexcept
{
    ChartMarker marker;
    if (luma == nullptr || height < markerHeight || width < 2 * markerCells) return marker;
    const std::size_t row = std::size_t(markerTop(height) + markerHeight / 2);
    std::uint64_t bits = 0;
    for (std::uint32_t index = 0; index < markerCells; ++index) {
        // The cell's centre.
        const auto sample = luma[row * stride + std::size_t(2 * index + 1) * width / (2 * markerCells)];
        bits = (bits << 1) | (sample >= 128 ? 1u : 0u);
    }
    const std::uint16_t check = std::uint16_t(bits);
    const std::uint64_t first48 = bits >> 16;
    marker.tag = std::uint16_t(first48 >> 32);
    marker.counter = std::uint32_t(first48);
    marker.valid = check == std::uint16_t((first48 >> 32) ^ (first48 >> 16) ^ first48);
    return marker;
}

}  // namespace uml::platform
