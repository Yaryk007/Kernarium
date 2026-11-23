// SPDX-License-Identifier: GPL-2.0-or-later
// The camera layouts a host composes for the Android image: the closed set and its fallback order against the
// image's own table (layouts.tsv, the generator's output, which the test takes as its argument), what a host's
// cameras earn, and the concurrent feature against the HAL's own rule.

#include "calibration_store.hpp"
#include "camera_description.hpp"
#include "camera_ids.hpp"
#include "camera_layout.hpp"
#include "camera_plan.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace uml::camera;
using uml::platform::BayerOrder;
using uml::platform::CameraColorData;
using uml::platform::CameraInfo;
using uml::platform::CameraPosition;
using uml::platform::SyntheticCameraOptions;
using uml::platform::SyntheticCameraProfile;
using uml::testing::require;

namespace {

struct Row {
    std::string name;
    std::string cameras;
    std::string tier;
    bool flash {};
    bool manual {};
    bool raw {};
    bool concurrent {};
    bool front2160 {};
    std::string variant;
    std::vector<std::string> fallback;
};

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator)) parts.push_back(part);
    return parts;
}

std::vector<Row> readTable(const char* path)
{
    std::ifstream file(path);
    require(file.good(), "the layout table opens");
    std::vector<Row> rows;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto columns = split(line, '\t');
        require(columns.size() == 10, "ten columns");
        Row row;
        row.name = columns[0];
        row.cameras = columns[1];
        row.tier = columns[2];
        row.flash = columns[3] == "1";
        row.manual = columns[4] == "1";
        row.raw = columns[5] == "1";
        row.concurrent = columns[6] == "1";
        row.front2160 = columns[7] == "1";
        row.variant = columns[8];
        if (columns[9] != "-") row.fallback = split(columns[9], ',');
        rows.push_back(std::move(row));
    }
    return rows;
}

LayoutDimensions dimensionsOf(const Row& row)
{
    LayoutDimensions dimensions;
    dimensions.cameras = row.cameras == "none" ? LayoutCameras::none
        : row.cameras == "front"               ? LayoutCameras::front
                                               : LayoutCameras::backAndFront;
    dimensions.tier = row.tier == "large" ? CameraTier::large : CameraTier::small;
    dimensions.flash = row.flash;
    dimensions.manual = row.manual;
    dimensions.raw = row.raw;
    dimensions.concurrent = row.concurrent;
    dimensions.front2160 = row.front2160;
    return dimensions;
}

// The set, its order and every fallback chain are the image's table.
void testTheImagesTable(const char* path)
{
    const auto rows = readTable(path);
    const auto layouts = allLayouts();
    const auto names = allLayoutNames();
    require(rows.size() == 39 && layouts.size() == 39 && names.size() == 39, "thirty-nine layouts");
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        require(names[index] == row.name, "the same name in the same place");
        require(layouts[index] == dimensionsOf(row), "and the same dimensions");
        require(layoutName(dimensionsOf(row)) == row.name, "a row's dimensions compose its name");
        auto chain = std::vector<std::string> {row.name};
        chain.insert(chain.end(), row.fallback.begin(), row.fallback.end());
        require(layoutFallbackChain(dimensionsOf(row)) == chain, "and fall back as the table says");
    }
    const std::set<std::string> distinct(names.begin(), names.end());
    require(distinct.size() == names.size(), "names are distinct");
}

void testNormalization()
{
    // A tier, flash, RAW, concurrency or 2160p means nothing without back cameras.
    LayoutDimensions front {LayoutCameras::front, CameraTier::large, true, false, true, true, true};
    require(layoutName(front) == "front", "a front camera alone says nothing of the back");
    front.manual = true;
    require(layoutName(front) == "front-manual", "manual control is the one thing it may say");
    require(layoutName({LayoutCameras::none, CameraTier::large, true, true, true, true, true}) == "none", "no cameras is none");
    // RAW needs manual control and 2160p the large tier.
    require(layoutName({LayoutCameras::backAndFront, CameraTier::small, false, false, true, false, false}) == "bf-small", "no RAW without manual");
    require(layoutName({LayoutCameras::backAndFront, CameraTier::small, false, true, true, false, true}) == "bf-small-manual-raw",
            "no 2160p in the small tier");
    require(layoutName({LayoutCameras::backAndFront, CameraTier::large, true, true, true, true, true}) ==
                "bf-large-flash-manual-raw-concurrent-front2160",
            "the name with every dimension");
}

bool isIn(const std::set<std::string>& available, std::string_view name) { return available.count(std::string(name)) != 0; }

// A host whose combination an image lacks takes the nearest layout the image has.
void testMissingCombinations()
{
    const LayoutDimensions phone {LayoutCameras::backAndFront, CameraTier::large, true, true, true, true, true};
    const auto choose = [&](const std::set<std::string>& available) {
        return chooseLayout(phone, [&](std::string_view name) { return isIn(available, name); });
    };
    const auto all = allLayoutNames();
    require(choose(std::set<std::string>(all.begin(), all.end())) == "bf-large-flash-manual-raw-concurrent-front2160", "an image with all of them");
    require(chooseLayout(phone, {}) == "bf-large-flash-manual-raw-concurrent-front2160", "no restriction accepts the first");

    // An image without the 2160p layouts: concurrency goes first, then RAW, then 2160p, and the image has the rest.
    std::set<std::string> no2160;
    for (const auto& name : all)
        if (name.find("front2160") == std::string::npos) no2160.insert(name);
    require(choose(no2160) == "bf-large-flash-manual", "concurrency and RAW go before 2160p does");

    // An image without concurrent streaming or RAW.
    std::set<std::string> plain;
    for (const auto& name : all)
        if (name.find("concurrent") == std::string::npos && name.find("raw") == std::string::npos) plain.insert(name);
    require(choose(plain) == "bf-large-flash-manual-front2160", "without concurrency and RAW both go");

    // An image of the small tier only: the flash is dropped before the tier, and the tier before manual control.
    std::set<std::string> small;
    for (const auto& name : all)
        if (name.find("bf-large") == std::string::npos) small.insert(name);
    require(choose(small) == "bf-small-manual", "the large tier gives way before manual control and the flash goes first");
    small.erase("bf-small-manual");
    require(choose(small) == "bf-small", "then manual control");
    small.erase("bf-small");
    require(choose(small) == "front", "then the back cameras");
    require(choose({"none"}) == "none", "then every camera");
    require(choose({}) == "none", "an image with nothing takes none");
}

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

struct Host {
    std::unique_ptr<uml::platform::SyntheticCamera> camera;
    std::vector<CameraInfo> infos;
    explicit Host(SyntheticCameraProfile profile)
    {
        SyntheticCameraOptions options;
        options.profile = profile;
        camera = uml::platform::makeSyntheticCamera(options);
        infos = camera->cameras();
    }
    // The host's cameras at a tier, with every lens that can take a Bayer still calibrated or none.
    [[nodiscard]] std::vector<CameraDescription> arrange(CameraTier tier, bool calibrated) const
    {
        CalibrationStore store;
        ArrangementOptions options;
        options.tier = tier;
        if (calibrated) {
            // The colour data was learned in the camera's anchor, which the arrangement names.
            const auto bare = arrangeCameras(infos, camera->concurrentSets(), camera->hostModel(), store, options);
            for (const auto& arranged : bare)
                if (arranged.info.rawStills) {
                    const auto size = arranged.info.rawStillSize;
                    auto color = colorOf(size.width, size.height);
                    if (arranged.anchor) {
                        color.anchorWidth = arranged.info.captureFormats[*arranged.anchor].width;
                        color.anchorHeight = arranged.info.captureFormats[*arranged.anchor].height;
                    }
                    store.save(camera->hostModel(), CalibrationStore::lensKey(arranged.info), color);
                }
        }
        return arrangeCameras(infos, camera->concurrentSets(), camera->hostModel(), store, options);
    }
};

// What the synthetic hosts earn. These are the layouts the Android runner expects of the same profiles
// (tests/integration/android/run_android_camera.py).
// The layout a host composes, named; a mismatch says which.
std::string composed(const std::vector<CameraDescription>& cameras, CameraTier tier, const char* expected)
{
    auto name = layoutName(layoutDimensionsOf(cameras, tier));
    if (name != expected) std::fprintf(stderr, "composed %s, expected %s\n", name.c_str(), expected);
    return name;
}

void testHostsEarnTheirLayouts()
{
    const Host phone(SyntheticCameraProfile::phone);
    require(composed(phone.arrange(CameraTier::large, true), CameraTier::large, "bf-large-flash-manual-raw-concurrent-front2160") ==
                "bf-large-flash-manual-raw-concurrent-front2160",
            "a phone with calibrated lenses");
    require(composed(phone.arrange(CameraTier::large, false), CameraTier::large, "bf-large-flash-manual-concurrent-front2160") ==
                "bf-large-flash-manual-concurrent-front2160",
            "and before the first photo it has no RAW");
    require(composed(phone.arrange(CameraTier::small, true), CameraTier::small, "bf-small-flash-manual-raw") ==
                "bf-small-flash-manual-raw",
            "the small tier caps video at 1080p: no 2160p, and no 1920x1440 for the pair that concurrency needs");

    const Host tablet(SyntheticCameraProfile::tablet);
    require(composed(tablet.arrange(CameraTier::small, true), CameraTier::small, "bf-small-flash-manual-raw") ==
                "bf-small-flash-manual-raw",
            "a tablet whose front camera cannot make the guaranteed concurrent sizes");

    const Host laptop(SyntheticCameraProfile::laptop);
    require(layoutName(layoutDimensionsOf(laptop.arrange(CameraTier::large, true), CameraTier::large)) == "front",
            "a laptop has a front camera, with no manual control");

    require(layoutName(layoutDimensionsOf({}, CameraTier::large)) == "none", "a host without cameras");

    // A back camera with no front one has no layout of its own.
    auto backOnly = tablet.arrange(CameraTier::small, false);
    backOnly.erase(std::remove_if(backOnly.begin(), backOnly.end(),
                                  [](const CameraDescription& camera) { return camera.info.position == CameraPosition::front; }),
                   backOnly.end());
    require(layoutName(layoutDimensionsOf(backOnly, CameraTier::small)) == "none", "a back camera alone composes none");

    // Cameras that only run one at a time do not earn concurrency.
    auto apart = phone.arrange(CameraTier::large, true);
    apart[0].wire.concurrent_mask = 0;
    require(!layoutDimensionsOf(apart, CameraTier::large).concurrent, "concurrency needs both to name each other");
}

// The layout of a platform camera's host is the one the device's own arrangement earns, restricted to what the
// image has.
void testComposeLayoutOfAHost()
{
    CalibrationStore store;
    ArrangementOptions options;
    options.tier = CameraTier::large;

    SyntheticCameraOptions phoneOptions;
    phoneOptions.profile = SyntheticCameraProfile::phone;
    const auto phone = uml::platform::makeSyntheticCamera(phoneOptions);
    require(composeLayout(*phone, store, options) == "bf-large-flash-manual-concurrent-front2160", "discovery composes the name");
    require(phone->sessionsOpened() == 0, "and opens no session");
    const auto smallOnly = [](std::string_view name) { return name.find("bf-large") == std::string_view::npos; };
    require(composeLayout(*phone, store, options, smallOnly) == "bf-small-manual", "from the layouts an image has (the flash goes before the tier)");
    options.tier = CameraTier::small;
    require(composeLayout(*phone, store, options) == "bf-small-flash-manual", "the tier is the launch's");

    SyntheticCameraOptions laptopOptions;
    laptopOptions.profile = SyntheticCameraProfile::laptop;
    require(composeLayout(*uml::platform::makeSyntheticCamera(laptopOptions), store, options) == "front", "a laptop");
    require(layoutArgumentPrefix == "androidboot.product.vendor.sku=", "the argument Android imports");
}

// The guaranteed concurrent sizes: this file's rule is the HAL's.
andarium::camera::Formats halFormats(const CameraDescription& camera)
{
    andarium::camera::Formats formats;
    for (const auto& mode : camera.video)
        formats.video.push_back({mode.width, mode.height, mode.minimumFrameRate, mode.maximumFrameRate});
    for (const auto& size : camera.jpegSizes) formats.jpeg.push_back({size.width, size.height});
    return formats;
}

bool halMeets(const CameraDescription& camera)
{
    return andarium::camera::meets_concurrent_streams(andarium::camera::derive_sizes(halFormats(camera)));
}

CameraDescription describe(std::vector<std::pair<std::uint32_t, std::uint32_t>> video,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> stills)
{
    CameraDescription camera;
    for (const auto& [width, height] : video) camera.video.push_back({width, height, 1, 30});
    for (const auto& [width, height] : stills) camera.jpegSizes.push_back({width, height});
    return camera;
}

void testConcurrentRuleIsTheHals()
{
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        const Host host(profile);
        for (const auto tier : {CameraTier::small, CameraTier::large})
            for (const auto& camera : host.arrange(tier, false))
                require(meetsConcurrentStreams(camera) == halMeets(camera), "a camera of a profile meets the sizes as the HAL says");
    }
    const std::vector<CameraDescription> cases = {
        describe({}, {}),
        describe({{1280, 720}}, {}),
        describe({{640, 480}}, {{640, 480}}),
        describe({{1920, 1080}}, {{4032, 3024}}),
        describe({{1920, 1080}, {1280, 720}}, {{1920, 1080}}),
        describe({{1920, 1440}}, {{4032, 3024}}),
        describe({{1920, 1440}, {1280, 960}}, {{4032, 3024}}),
        describe({{2016, 1512}, {1920, 1080}, {1280, 720}}, {{2016, 1512}}),
        describe({{3840, 2160}, {1920, 1080}}, {{4032, 3024}, {8064, 6048}}),
        describe({{3840, 2160}}, {{3840, 2160}}),
        describe({{1280, 960}}, {{4032, 3024}}),
        describe({{2736, 2052}}, {{2736, 2052}}),
        describe({{1920, 1080}}, {{1920, 1080}}),
        describe({{1280, 720}}, {{1280, 720}}),
        describe({{1281, 721}}, {{1281, 721}}),
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const bool ours = meetsConcurrentStreams(cases[index]);
        const bool hals = halMeets(cases[index]);
        if (ours != hals) std::fprintf(stderr, "case %zu: ours %d, the HAL's %d\n", index, ours, hals);
        require(ours == hals, "the rule is the HAL's on a made camera");
    }
    // A camera that covers 1920x1440 with a video mode meets them; one limited to 1080p does not.
    require(meetsConcurrentStreams(describe({{1920, 1440}, {1280, 720}}, {{4032, 3024}})), "a video mode of 1920x1440 covers the pair");
    require(!meetsConcurrentStreams(describe({{1920, 1080}}, {{4032, 3024}})), "1080p cannot make 1920x1440");
}

// The reserve is what the device lets the host hold (`CameraUse`), which `composeCameras` derives from the cameras and
// their layout: no layout holds more than the tier's ceiling, and the ceiling is the use with both cameras running
// together and RAW.
void testTheReserveFollowsTheUse()
{
    const auto tier = CameraTier::small;
    const auto ceiling = cameraHostMemoryReserveMiB(tier);
    require(ceiling == 197, "the tier's ceiling");
    require(cameraHostMemoryReserveMiB(tier, CameraUse {}) == ceiling, "the default use is the ceiling");

    std::set<std::uint64_t> figures;
    bool reachesTheCeiling = false;
    for (const auto& dimensions : allLayouts()) {
        const auto name = layoutName(dimensions);
        const auto streams = layoutStreams(dimensions);
        require((name.find("concurrent") != std::string::npos) == dimensions.concurrent, name);
        require(streams == (dimensions.cameras == LayoutCameras::backAndFront && dimensions.concurrent ? 2u : 1u), name);
        const auto named = layoutDimensionsNamed(name);
        require(named && layoutName(*named) == name, "every layout of the set is found by its name");
        for (const bool raw : {false, true}) {
            const CameraUse use {streams, raw, uml::platform::systemCameraStillWorkingBytesPerPixel};
            const auto reserve = liveCameraReserveMiB(CameraMode::live, name, use, tier);
            require(liveCameraReserveMiB(CameraMode::blocked, name, use, tier) == 0, "a blocked session holds none");
            if (!layoutHasCameras(name)) {
                require(reserve == 0, "no cameras, no device, no reserve");
                continue;
            }
            require(reserve == cameraHostMemoryReserveMiB(tier, use) && reserve <= ceiling, "no use holds more than the ceiling");
            figures.insert(reserve);
            reachesTheCeiling = reachesTheCeiling || reserve == ceiling;
        }
    }
    require(figures == std::set<std::uint64_t> {99, 124, 171, 197}, "the four figures are all there are");
    require(reachesTheCeiling, "the ceiling is a use's own figure");
    require(!layoutDimensionsNamed("").has_value() && !layoutDimensionsNamed("bf-small-nothing").has_value());
}

// What a host's cameras are given: a layout the device holds to, and a RAW capture wherever a lens can make one.
void testComposedCamerasAreGivenTheirUse()
{
    const auto tier = CameraTier::small;
    uml::camera::ArrangementOptions options;
    options.tier = tier;
    const auto compose = [&](SyntheticCameraProfile profile, CalibrationStore& store) {
        SyntheticCameraOptions synthetic;
        synthetic.profile = profile;
        return composeCameras(*uml::platform::makeSyntheticCamera(synthetic), store, options);
    };

    // The tablet's front camera does not run with the back one in the layout the guest is told, so the device lets one
    // camera run: the layout is the stated one, not the host's own concurrent masks.
    CalibrationStore store;
    const auto tablet = compose(SyntheticCameraProfile::tablet, store);
    require(tablet.layout == "bf-small-flash-manual" && tablet.use.streams == 1, tablet.layout);
    // Its back lens makes RAW stills and has taught nothing yet, so its first still takes one with the JPEG: the layout
    // lists no RAW and the use has it, from the first boot.
    require(tablet.use.rawStill, "an uncalibrated RAW-capable lens captures RAW although the layout lists none");
    require(tablet.use.stillWorkingBytesPerPixel == 11, "the synthetic source's working storage is counted");
    const auto phone = compose(SyntheticCameraProfile::phone, store);
    require(phone.use.streams == (phone.layout.find("concurrent") != std::string::npos ? 2u : 1u), phone.layout);
    // A laptop's one front camera, with no RAW, holds the least.
    const auto laptop = compose(SyntheticCameraProfile::laptop, store);
    require(laptop.layout == "front" && laptop.use.streams == 1 && !laptop.use.rawStill, laptop.layout);

    // Teaching the store every lens's colour data moves a lens from capturing it to offering it: the use is the same, so
    // the pool a later boot of the run gets was sized for it.
    SyntheticCameraOptions synthetic;
    synthetic.profile = SyntheticCameraProfile::phone;
    const auto host = uml::platform::makeSyntheticCamera(synthetic);
    for (const auto& info : host->cameras()) {
        if (!info.rawStills) continue;
        uml::platform::CameraColorData data;
        data.bayer = uml::platform::BayerOrder::grbg;
        data.blackLevel = 64;
        data.whiteLevel = 1023;
        data.rawWidth = info.rawStillSize.width;
        data.rawHeight = info.rawStillSize.height;
        data.illuminant1 = 17;
        data.illuminant2 = 21;
        data.colorMatrix1 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        data.focalLengthMillimetres = 6.9;
        data.intrinsics = {3000, 3000, 1008, 756};
        require(store.save(host->hostModel(), CalibrationStore::lensKey(info), data));
    }
    const auto learned = composeCameras(*host, store, options);
    require(learned.layout == "bf-small-flash-manual-raw", learned.layout);
    require(learned.use == phone.use, "the boot after the calibration is held to the use the first was sized for");
    require(liveCameraReserveMiB(CameraMode::live, learned.layout, learned.use, tier) ==
                liveCameraReserveMiB(CameraMode::live, phone.layout, phone.use, tier),
            "and so is the memory it reserves");
}

// A later boot of a run is held to what the first discovery paid for: the layout the guest is told is one the reservation
// allows, and where no layout of the host's cameras does, none.
void testALaterBootIsHeldToTheReservation()
{
    uml::camera::ArrangementOptions options;
    options.tier = CameraTier::large;  // The phone's back and front cameras run at once only at the large tier.
    CalibrationStore store;
    const auto host = [](SyntheticCameraProfile profile) {
        SyntheticCameraOptions synthetic;
        synthetic.profile = profile;
        return uml::platform::makeSyntheticCamera(synthetic);
    };
    const auto fits = [](const CameraUse& use, const CameraReservation& reserved) {
        return use.streams <= reserved.use.streams && (!use.rawStill || reserved.use.rawStill) &&
            use.stillWorkingBytesPerPixel <= reserved.use.stillWorkingBytesPerPixel;
    };
    const auto phoneHost = host(SyntheticCameraProfile::phone);
    const auto first = composeCameras(*phoneHost, store, options);
    const auto reserved = reservationOf(first);
    require(reserved.cameras && first.use.streams == 2 && first.use.rawStill,
            first.layout + " " + std::to_string(first.use.streams) + (first.use.rawStill ? " raw" : " no raw"));

    // The same host composes the same layout.
    const auto again = composeCamerasWithin(*phoneHost, store, options, reserved);
    require(again.layout == first.layout && again.use == first.use, "an unchanged host composes what it did");

    // A host that stays within the reservation is told its own layout.
    const auto laptop = composeCamerasWithin(*host(SyntheticCameraProfile::laptop), store, options, reserved);
    require(laptop.layout == "front" && fits(laptop.use, reserved), laptop.layout);

    // The first discovery that had no cameras reserved nothing: none stays none, whatever the host offers later.
    CameraReservation nothing, roomy;
    roomy.use = reserved.use;  // Whatever use the none composition carries, no camera was reserved for.
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::laptop}) {
        for (const auto* none : {&nothing, &roomy}) {
            const auto later = composeCamerasWithin(*host(profile), store, options, *none);
            require(!layoutHasCameras(later.layout) && later.layout == noCameraLayout, "none stays none");
        }
    }

    // More cameras at once than the reservation counts: the widest layout that runs within it.
    CameraReservation oneStream = reserved;
    oneStream.use.streams = 1;
    const auto narrowed = composeCamerasWithin(*phoneHost, store, options, oneStream);
    require(layoutHasCameras(narrowed.layout) && narrowed.use.streams == 1 && fits(narrowed.use, oneStream) &&
                narrowed.layout.find("concurrent") == std::string::npos,
            narrowed.layout);

    // A RAW capture or source working storage the first discovery did not count is never reached, so none.
    CameraReservation noRaw = reserved;
    noRaw.use.rawStill = false;
    const auto raw = composeCamerasWithin(*phoneHost, store, options, noRaw);
    require(!layoutHasCameras(raw.layout), "a RAW capture the reservation did not count is not offered");
    CameraReservation noWorking = reserved;
    noWorking.use.stillWorkingBytesPerPixel = 0;
    const auto working = composeCamerasWithin(*phoneHost, store, options, noWorking);
    require(!layoutHasCameras(working.layout), "nor a source whose stills hold more than the reservation counted");

    // A host whose first discovery was the laptop and which later shows a phone: nothing the reservation did not count.
    const auto laptopFirst = reservationOf(composeCameras(*host(SyntheticCameraProfile::laptop), store, options));
    const auto grown = composeCamerasWithin(*phoneHost, store, options, laptopFirst);
    require(!layoutHasCameras(grown.layout) || fits(grown.use, laptopFirst), "a host that grew is held to the first's use");
    require(!layoutHasCameras(grown.layout), "and its RAW-capable back cameras are not told");
}

// The cameras the image's HAL numbers (camera_ids.hpp) from a phone of three lenses, which the host publishes with no zoom
// camera: "0" the wide lens, "1" the front camera, "3" the ultra wide and "4" the telephoto. Concurrency comes from the wide
// lens: it and the front camera are the one set, the layout's `concurrent` feature is the HAL's own, and the other lenses
// conflict with the front camera.
void testTheHalNumbersAThreeLensPhone()
{
    const Host phone(SyntheticCameraProfile::phone);
    for (const auto tier : {CameraTier::small, CameraTier::large}) {
        const auto cameras = phone.arrange(tier, false);
        std::vector<andarium::camera::CameraKey> keys;
        for (const auto& camera : cameras) keys.push_back({camera.wire.index, camera.wire.facing, camera.wire.lens});
        const auto ids = andarium::camera::assign_ids(keys);
        require(ids.size() == 4, "the HAL lists four cameras");
        const auto indexOf = [&](std::uint32_t id) {
            for (const auto& assignment : ids)
                if (assignment.id == id) return int(assignment.index);
            return -1;
        };
        require(indexOf(0) == 0 && cameras[0].info.lens == uml::platform::CameraLensKind::wide, "\"0\" is the wide lens");
        require(indexOf(1) == 1 && cameras[1].info.position == CameraPosition::front, "\"1\" is the front camera");
        require(indexOf(2) == -1, "there is no \"2\"");
        require(indexOf(3) == 2 && cameras[2].info.lens == uml::platform::CameraLensKind::ultraWide, "\"3\" is the ultra wide lens");
        require(indexOf(4) == 3 && cameras[3].info.lens == uml::platform::CameraLensKind::telephoto, "\"4\" is the telephoto");

        std::vector<andarium::camera::Concurrency> concurrency;
        for (const auto& assignment : ids)
            concurrency.push_back({assignment.id, assignment.index, cameras[assignment.index].wire.concurrent_mask,
                                   cameras[assignment.index].wire.conflict_mask});
        const auto meets = [&](std::uint32_t id) { return halMeets(cameras[indexOf(id)]); };
        require(andarium::camera::concurrent_feature(concurrency, meets) == layoutDimensionsOf(cameras, tier).concurrent,
                "the layout's concurrent feature is the HAL's");
        // Android is told the one combination, [0, 1], where the pair makes the guaranteed sizes (the large tier's).
        const auto reported = andarium::camera::reported_concurrent_sets(concurrency, meets);
        require(reported.empty() != layoutDimensionsOf(cameras, tier).concurrent &&
                    (reported.empty() || reported == std::vector<std::vector<std::uint32_t>>({{0, 1}})),
                "the combinations Android is told follow the layout's concurrent feature");
        const auto sets = andarium::camera::concurrent_sets(concurrency);
        require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1}), "the wide lens and the front camera are the one set");
        require(andarium::camera::conflicting_ids(concurrency, 0) == std::vector<std::uint32_t>({3, 4}) &&
                    andarium::camera::conflicting_ids(concurrency, 1) == std::vector<std::uint32_t>({3, 4}) &&
                    andarium::camera::conflicting_ids(concurrency, 3) == std::vector<std::uint32_t>({0, 1, 4}) &&
                    andarium::camera::conflicting_ids(concurrency, 4) == std::vector<std::uint32_t>({0, 1, 3}),
                "the other lenses conflict with the front camera and with each other");
    }
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "the layout table is the argument");
    testTheImagesTable(argv[1]);
    testNormalization();
    testMissingCombinations();
    testHostsEarnTheirLayouts();
    testComposeLayoutOfAHost();
    testConcurrentRuleIsTheHals();
    testTheHalNumbersAThreeLensPhone();
    testTheReserveFollowsTheUse();
    testComposedCamerasAreGivenTheirUse();
    testALaterBootIsHeldToTheReservation();
    std::puts("camera-layout: passed");
    return 0;
}
