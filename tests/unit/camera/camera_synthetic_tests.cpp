// SPDX-License-Identifier: GPL-2.0-or-later
#include "require.hpp"
#include "scratch_directory.hpp"
#include "synthetic_camera.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

class Recorder final : public CameraSessionDelegate {
public:
    void cameraFrame(std::size_t camera, CameraFrame frame) override
    {
        if (firstFrameDelay.count() > 0) {
            // A delegate that is slow on its first frame (the timer thread waits for it).
            bool first;
            {
                std::lock_guard lock(mutex);
                first = frames.empty();
            }
            if (first) std::this_thread::sleep_for(firstFrameDelay);
        }
        std::lock_guard lock(mutex);
        frames.emplace_back(camera, std::move(frame));
        if (clock) returns.push_back(clock());  // when this callback returns, on the camera's clock
        changed.notify_all();
    }
    void cameraStill(std::size_t camera, CameraStillResult result) override
    {
        std::lock_guard lock(mutex);
        stills.emplace_back(camera, std::move(result));
        changed.notify_all();
    }
    void cameraEvent(CameraEvent event) override
    {
        std::lock_guard lock(mutex);
        events.push_back(event);
        changed.notify_all();
    }
    bool waitFor(std::size_t framesWanted, std::size_t stillsWanted = 0)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10),
                                [&] { return frames.size() >= framesWanted && stills.size() >= stillsWanted; });
    }
    std::chrono::milliseconds firstFrameDelay {0};
    std::function<std::uint64_t()> clock;
    std::vector<std::uint64_t> returns;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::size_t, CameraFrame>> frames;
    std::vector<std::pair<std::size_t, CameraStillResult>> stills;
    std::vector<CameraEvent> events;
};

double meanLuma(const CameraFrame& frame)
{
    // Below the marker rows.
    double sum = 0;
    std::size_t count = 0;
    for (std::uint32_t y = 16; y < frame.height; ++y)
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            sum += frame.luma[std::size_t(y) * frame.lumaStride + x];
            ++count;
        }
    return sum / double(count);
}

double sharpness(const CameraFrame& frame)
{
    double sum = 0;
    for (std::uint32_t y = 16; y < frame.height; ++y)
        for (std::uint32_t x = 1; x < frame.width; ++x)
            sum += std::pow(double(frame.luma[std::size_t(y) * frame.lumaStride + x]) -
                                double(frame.luma[std::size_t(y) * frame.lumaStride + x - 1]), 2.0);
    return sum;
}

double meanChroma(const CameraFrame& frame, unsigned component)
{
    double sum = 0;
    std::size_t count = 0;
    for (std::uint32_t y = 8; y < frame.height / 2; ++y)
        for (std::uint32_t x = 0; x < frame.width; x += 2) {
            sum += frame.chroma[std::size_t(y) * frame.chromaStride + x + component];
            ++count;
        }
    return sum / double(count);
}

SyntheticCameraOptions manual(SyntheticCameraProfile profile)
{
    SyntheticCameraOptions options;
    options.profile = profile;
    options.manualClock = true;
    return options;
}

void profiles()
{
    std::set<std::string> models;
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        const auto camera = makeSyntheticCamera(manual(profile));
        const auto cameras = camera->cameras();
        require(!cameras.empty());
        models.insert(camera->hostModel());
        std::set<std::string> identifiers;
        for (const auto& info : cameras) {
            require(identifiers.insert(info.identifier).second);
            require(!info.videoFormats.empty() && !info.stillSizes.empty());
            require(info.pixelArrayWidth > 0 && info.sensorOrientation % 90 == 0);
            // Every camera's zoom range holds 1.0, and a single lens's 1.0 is its own full field of view.
            require(info.zoomMinimum <= 1.0f && info.zoomMaximum >= 1.0f, "a camera's zoom range holds 1.0");
            if (info.lens != CameraLensKind::zoom) require(info.zoomMinimum == 1.0f, "a single lens's widest view is its ratio 1.0");
        }
        for (const auto& set : camera->concurrentSets()) {
            require(set.size() > 1);
            for (const auto member : set) require(member < cameras.size());
        }
        // A zoom camera names lenses that exist.
        for (const auto& info : cameras)
            for (const auto& name : info.constituents) {
                bool found = false;
                for (const auto& other : cameras) found |= other.identifier == name;
                require(found);
            }
    }
    require(models.size() == 4);
    const auto dual = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront))->cameras();
    require(dual.size() == 3 && dual[1].position == CameraPosition::front && dual[1].lens == CameraLensKind::wide &&
            dual[2].position == CameraPosition::front && dual[2].lens == CameraLensKind::ultraWide);
    // The phone lists its three lenses and its front camera, and no zoom camera, with the zoom the system recommends for
    // each lens of a phone of three lenses.
    const auto phone = makeSyntheticCamera(manual(SyntheticCameraProfile::phone))->cameras();
    require(phone.size() == 4 && phone[0].lens == CameraLensKind::wide && phone[1].lens == CameraLensKind::ultraWide &&
            phone[2].lens == CameraLensKind::telephoto && phone[3].position == CameraPosition::front);
    for (const auto& info : phone) require(info.lens != CameraLensKind::zoom && info.zoomMinimum == 1.0f && info.constituents.empty());
    require(phone[0].zoomMaximum == 6.0f && phone[1].zoomMaximum == 3.0f && phone[2].zoomMaximum == 3.0f);
    require(phone[0].manualExposure && phone[0].rawStills && phone[0].stillSizes.front().width == 8064);
    require(makeSyntheticCamera(manual(SyntheticCameraProfile::laptop))->cameras().size() == 1);
}

/* The mean luma of the left and the right eighth of a JPEG's columns, which the chart's brightness ramp makes lowest on the
 * left and highest on the right, so that a picture turned half around reads the other way. Zero for a file that does not
 * decode. */
std::pair<double, double> sideLumas(const std::vector<std::uint8_t>& jpeg)
{
    CFDataRef data = CFDataCreate(nullptr, jpeg.data(), CFIndex(jpeg.size()));
    CGImageSourceRef source = data != nullptr ? CGImageSourceCreateWithData(data, nullptr) : nullptr;
    CGImageRef image = source != nullptr ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    std::pair<double, double> result {0.0, 0.0};
    if (image != nullptr) {
        const std::size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
        std::vector<std::uint8_t> rgba(width * height * 4);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space,
                                                     CGBitmapInfo(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrderDefault);
        CGColorSpaceRelease(space);
        if (context != nullptr) {
            CGContextDrawImage(context, CGRectMake(0, 0, double(width), double(height)), image);
            CGContextRelease(context);
            const std::size_t eighth = width / 8;
            double left = 0, right = 0;
            for (std::size_t y = 0; y < height; ++y)
                for (std::size_t x = 0; x < eighth; ++x) {
                    const auto* l = &rgba[(y * width + x) * 4];
                    const auto* r = &rgba[(y * width + width - 1 - x) * 4];
                    left += 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
                    right += 0.2126 * r[0] + 0.7152 * r[1] + 0.0722 * r[2];
                }
            result = {left / double(height * eighth), right / double(height * eighth)};
        }
        CGImageRelease(image);
    }
    if (source != nullptr) CFRelease(source);
    if (data != nullptr) CFRelease(data);
    return result;
}

/* With `frontPresentedAt90` every front camera that states 270 is a platform camera mounted at 90 and turned half around by
 * the host: it states 270 and a presentation turn of 180, its video frames are the mounting's and its JPEG stills are turned,
 * where a back camera, a camera of a platform that has no turn, and a laptop's camera that states none are not. */
void aFrontCameraPresentedAt90IsTurnedHalfAround()
{
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        for (const bool presented : {false, true}) {
            auto options = manual(profile);
            options.frontPresentedAt90 = presented;
            for (const auto& info : makeSyntheticCamera(options)->cameras()) {
                const bool turned = presented && info.position == CameraPosition::front && profile != SyntheticCameraProfile::laptop;
                require(info.presentationTurn == (turned ? 180u : 0u), "only a front camera that states 270, by the option, has the turn");
                if (info.position == CameraPosition::front && profile != SyntheticCameraProfile::laptop)
                    require(info.sensorOrientation == 270, "a front camera states 270 with or without it");
                if (info.position == CameraPosition::back) require(info.sensorOrientation == 90, "a back camera states 90");
            }
        }
    }

    // A video frame is the platform's: the marker is readable as the chart drew it, with the turn or without. A still is
    // made turned: the chart's brightness ramp reads from the right to the left.
    for (const bool presented : {false, true}) {
        auto options = manual(SyntheticCameraProfile::tablet);
        options.frontPresentedAt90 = presented;
        const auto camera = makeSyntheticCamera(options);
        Recorder recorder;
        auto session = camera->openSession(recorder);
        require(session->startVideo(1, 640, 480, 30));
        camera->pump(1, 1);
        const auto& frame = recorder.frames.back().second;
        const auto marker = decodeChartMarker(frame.luma.data(), frame.lumaStride, frame.width, frame.height);
        require(marker.valid && marker.tag == chartTag(1) && marker.counter == 1, "the platform's frame is the mounting's");
        CameraStillRequest request;
        request.jpegWidth = 1008;
        request.jpegHeight = 756;
        require(session->takeStill(1, request));
        camera->pump(1, 0);
        require(recorder.waitFor(1, 1));
        const auto [left, right] = sideLumas(recorder.stills.back().second.jpeg);
        require(left > 0.0 && right > 0.0, "the still decodes");
        if (presented) require(left > right + 80.0, "the still of a camera with a half turn is made turned");
        else require(right > left + 80.0, "the still of a camera with none is as it is");
    }
}

void unauthorized()
{
    auto options = manual(SyntheticCameraProfile::tablet);
    options.authorized = false;
    const auto camera = makeSyntheticCamera(options);
    Recorder recorder;
    require(camera->openSession(recorder) == nullptr);
    require(camera->sessionsOpened() == 0 && camera->sessionsOpen() == 0);
    require(camera->cameras().size() == 2);  // discovery needs no authorization
}

void video()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session != nullptr && camera->sessionsOpened() == 1 && camera->sessionsOpen() == 1);
    require(!session->startVideo(0, 640, 480, 500));     // no format runs that fast
    require(!session->startVideo(0, 8000, 6000, 30));    // no format is that large
    require(!session->startVideo(9, 640, 480, 30));      // no such camera
    require(session->startVideo(0, 600, 400, 30));       // the smallest covering format
    require(camera->videoRunning(0) && !camera->videoRunning(1) && camera->videoFrameRate(0) == 30);
    const auto start = camera->now();
    camera->pump(0, 3);
    require(recorder.frames.size() == 3);
    std::uint32_t expected = 1;
    for (const auto& [index, frame] : recorder.frames) {
        require(index == 0 && frame.width == 640 && frame.height == 480);
        require(frame.lumaStride >= frame.width && frame.luma.size() >= frame.lumaStride * frame.height);
        const auto marker = decodeChartMarker(frame.luma.data(), frame.lumaStride, frame.width, frame.height);
        require(marker.valid && marker.tag == chartTag(0) && marker.counter == expected);
        require(frame.exposure.frameDurationNanoseconds == 33'333'333);
        require(frame.exposure.ae == CameraControlState::converged && frame.exposure.iso >= 25);
        require(frame.timestampNanoseconds == start + (expected - 1) * 33'333'333ull);
        ++expected;
    }
    // Another camera's chart carries its own tag.
    require(session->startVideo(1, 640, 480, 30));  // front and back are one set
    camera->pump(1, 1);
    const auto& other = recorder.frames.back();
    require(other.first == 1 && decodeChartMarker(other.second.luma.data(), other.second.lumaStride, 640, 480).tag == chartTag(1));
    session->stopVideo(0);
    require(!camera->videoRunning(0));
    camera->pump(0, 2);
    require(recorder.frames.size() == 4);
    session->stop();
    require(camera->sessionsOpen() == 0 && camera->sessionsOpened() == 1);
    require(!session->startVideo(0, 640, 480, 30));  // a stopped session starts nothing
}

void controls()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    const auto automatic = meanLuma(recorder.frames.back().second);
    const auto crisp = sharpness(recorder.frames.back().second);
    const auto autoCb = meanChroma(recorder.frames.back().second, 0);

    CameraSettings settings;
    settings.exposureMode = CameraExposureMode::manual;
    settings.exposureNanoseconds = 8'333'333 / 2;
    settings.iso = 100;
    session->apply(0, settings);
    camera->pump(0, 1);
    const auto& dim = recorder.frames.back().second;
    require(dim.exposure.exposureNanoseconds == 8'333'333 / 2 && dim.exposure.iso == 100 && dim.exposure.ae == CameraControlState::inactive);
    require(meanLuma(dim) < automatic * 0.7);
    settings.exposureNanoseconds = 8'333'333;
    settings.iso = 200;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(meanLuma(recorder.frames.back().second) > automatic * 1.3);
    require(camera->appliedSettings(0) == settings);

    // A manual focus away from the chart's distance blurs the picture.
    settings = {};
    settings.focusMode = CameraFocusMode::manual;
    settings.focusDistanceDioptres = 5.0f;
    settings.focusDistanceSerial = 1;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);
    require(sharpness(recorder.frames.back().second) < crisp * 0.8);

    // The lens holds while the distance is not written anew, even if the value says
    // otherwise; a locked lens holds too.
    settings.focusDistanceDioptres = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);
    settings.focusMode = CameraFocusMode::locked;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked &&
            recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);

    // A scan in a manual mode puts the lens on the chart, and it holds there.
    settings.focusMode = CameraFocusMode::manual;
    settings.focusTriggerSerial = 1;
    session->apply(0, settings);
    camera->pump(0, 2);
    require(recorder.frames[recorder.frames.size() - 2].second.exposure.af == CameraControlState::searching);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 1.0f);

    // White balance moves the chroma.
    settings = {};
    settings.whiteBalanceMode = CameraWhiteBalanceMode::manual;
    settings.whiteBalanceGains = {1.0f, 1.0f, 3.0f};
    session->apply(0, settings);
    camera->pump(0, 1);
    require(meanChroma(recorder.frames.back().second, 0) > autoCb + 5);
    require(recorder.frames.back().second.exposure.awb == CameraControlState::inactive);

    // A trigger makes one scan.
    settings = {};
    settings.focusTriggerSerial = 1;
    session->apply(0, settings);
    camera->pump(0, 2);
    require(recorder.frames[recorder.frames.size() - 2].second.exposure.af == CameraControlState::searching);
    require(recorder.frames.back().second.exposure.af == CameraControlState::converged);

    // Digital zoom enlarges the middle: the marker is gone from view.
    settings = {};
    settings.zoomRatio = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.zoomRatio == 2.0f);
}

// What the exposure of a frame says: the lens's focal length is left to the device (the camera's own, as its
// description states it), an automatic exposure follows the compensation and the picture with it, and a frame lasts at
// least as long as its exposure, which delays the next one.
void exposureReport()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    const auto unit = recorder.frames.back().second;
    require(unit.exposure.focalLengthMillimetres == 0.0f);  // none: the device states the lens's own
    require(unit.exposure.exposureNanoseconds == 8'333'333 && unit.exposure.frameDurationNanoseconds == 33'333'333);
    const auto unitLuma = meanLuma(unit);

    CameraSettings settings;
    settings.exposureBiasStops = -1.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    const auto& half = recorder.frames.back().second;
    require(std::llabs(std::int64_t(half.exposure.exposureNanoseconds) - 8'333'333 / 2) <= 1 && half.exposure.iso == unit.exposure.iso);
    require(half.exposure.ae == CameraControlState::converged && half.exposure.frameDurationNanoseconds == 33'333'333);
    require(meanLuma(half) < unitLuma * 0.7);
    settings.exposureBiasStops = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    const auto& bright = recorder.frames.back().second;
    require(std::llabs(std::int64_t(bright.exposure.exposureNanoseconds) - 8'333'333 * 4) <= 1);
    const double brightLuma = meanLuma(bright);
    require(brightLuma > unitLuma);
    // The automatic exposure stays inside the frame the rate asks for: a compensation that asks for more than a 30 fps
    // frame holds (+3 stops is twice as long) runs the frame's own length, and the sensitivity takes the rest, so the
    // exposure times the sensitivity still follows the compensation and the rate does not fall.
    settings.exposureBiasStops = 3.0f;
    session->apply(0, settings);
    camera->pump(0, 2);
    const auto& third = recorder.frames[recorder.frames.size() - 2].second;
    const auto& fourth = recorder.frames.back().second;
    require(third.exposure.exposureNanoseconds == 33'333'333 && third.exposure.frameDurationNanoseconds == 33'333'333);
    require(fourth.timestampNanoseconds - third.timestampNanoseconds == 33'333'333);
    require(std::llabs(std::int64_t(third.exposure.iso) - 200) <= 1);  // 3 stops are 2 of exposure (to the frame) and 1 of it
    const double product = double(third.exposure.exposureNanoseconds) * third.exposure.iso;
    require(std::fabs(product / (double(unit.exposure.exposureNanoseconds) * unit.exposure.iso) - 8.0) < 0.1);
    require(meanLuma(third) >= brightLuma);
    // The exposure of the sensor ends where its range does, the sensitivity where its range does, and the picture stops
    // with them.
    const auto info = camera->cameras()[0];
    settings.exposureBiasStops = 20.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.exposureNanoseconds == 33'333'333);
    require(recorder.frames.back().second.exposure.iso == info.isoMaximum);
    settings.exposureBiasStops = -20.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.exposureNanoseconds == info.exposureMinimumNanoseconds);
    require(recorder.frames.back().second.exposure.iso == info.isoMinimum);

    // A manual exposure longer than the frame period: the frame lasts as long as the exposure, and the next frame
    // arrives after it.
    settings = {};
    settings.exposureMode = CameraExposureMode::manual;
    settings.exposureNanoseconds = 333'346'667;
    settings.iso = 100;
    session->apply(0, settings);
    camera->pump(0, 2);
    const auto& first = recorder.frames[recorder.frames.size() - 2].second;
    const auto& second = recorder.frames.back().second;
    require(first.exposure.exposureNanoseconds == 333'346'667 && first.exposure.frameDurationNanoseconds == 333'346'667);
    require(second.timestampNanoseconds - first.timestampNanoseconds == 333'346'667);
    // A shorter one leaves the period alone.
    settings.exposureNanoseconds = 8'333'333;
    session->apply(0, settings);
    camera->pump(0, 2);
    const auto& short1 = recorder.frames[recorder.frames.size() - 2].second;
    require(short1.exposure.frameDurationNanoseconds == 33'333'333 &&
            recorder.frames.back().second.timestampNanoseconds - short1.timestampNanoseconds == 33'333'333);
    // A still's exposure states no focal length either.
    CameraStillRequest request;
    request.jpegWidth = 640;
    request.jpegHeight = 480;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(recorder.waitFor(1, 1));
    require(recorder.stills.back().second.exposure.focalLengthMillimetres == 0.0f);
}

// Every profile's every camera runs an automatic exposure that is not zero and shows a lit chart at no compensation: a
// camera that states no exposure range (a laptop's) runs the exposure as asked and is not clamped to nothing.
void everyProfileExposes()
{
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        const auto camera = makeSyntheticCamera(manual(profile));
        Recorder recorder;
        auto session = camera->openSession(recorder);
        const auto cameras = camera->cameras();
        for (std::size_t index = 0; index < cameras.size(); ++index) {
            const auto& format = cameras[index].videoFormats.front();
            require(session->startVideo(index, format.width, format.height, format.maximumFrameRate));
            camera->pump(index, 1);
            const auto& frame = recorder.frames.back().second;
            require(frame.exposure.exposureNanoseconds == 8'333'333);
            require(frame.exposure.iso >= 100);
            require(frame.exposure.frameDurationNanoseconds >= frame.exposure.exposureNanoseconds);
            require(meanLuma(frame) > 100.0);
            session->stopVideo(index);
        }
    }
    // A laptop's camera states no range: the compensation moves its exposure the same way.
    const auto laptop = makeSyntheticCamera(manual(SyntheticCameraProfile::laptop));
    require(laptop->cameras()[0].exposureMaximumNanoseconds == 0 && laptop->cameras()[0].isoMaximum == 0);
    Recorder recorder;
    auto session = laptop->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    CameraSettings settings;
    settings.exposureBiasStops = -1.0f;
    session->apply(0, settings);
    laptop->pump(0, 1);
    require(std::llabs(std::int64_t(recorder.frames.back().second.exposure.exposureNanoseconds) - 8'333'333 / 2) <= 1);
    // The excess of a long exposure cannot go into a sensitivity it does not state: the exposure takes the frame, and the
    // sensitivity stays at its one value.
    settings.exposureBiasStops = 3.0f;
    session->apply(0, settings);
    laptop->pump(0, 1);
    require(recorder.frames.back().second.exposure.exposureNanoseconds == 33'333'333 && recorder.frames.back().second.exposure.iso == 100);
}

void multiCamera()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::phone));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(3, 1280, 720, 30));    // the front camera
    require(session->startVideo(0, 1280, 720, 30));    // wide: in a set with the front
    require(!session->startVideo(1, 1280, 720, 30));   // ultra wide is in no set with both
    require(!session->startVideo(2, 640, 480, 30));    // nor is the telephoto
    session->stopVideo(0);
    require(!session->startVideo(1, 1280, 720, 30));   // the ultra wide is in no set with the front camera
    require(!session->startVideo(2, 1280, 720, 30));   // nor is the telephoto
    require(session->startVideo(0, 1280, 720, 30));    // the wide lens is
    // A format a multi-camera session does not take is refused while two run.
    require(!session->startVideo(0, 3840, 2160, 30));
    session->stopVideo(3);
    require(session->startVideo(0, 3840, 2160, 30));   // alone, it may use it
}

// A camera alone runs any format; cameras that run together run formats a
// multi-camera session takes. The ones running move to such a format when
// another joins, the newcomer is refused with nothing changed when one has none,
// and a camera left alone goes back to the smallest format of any kind.
void multiCameraFormats()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    require(session->startVideo(0, 1600, 900, 30));
    require(camera->videoSize(0) == Size(1600, 900));  // its own format, which no multi-camera session takes
    camera->pump(0, 1);
    require(recorder.frames.back().second.width == 1600);

    require(session->startVideo(1, 640, 480, 30));     // the front wide camera joins
    require(camera->videoSize(0) == Size(1920, 1080) && camera->videoSize(1) == Size(640, 480));
    camera->pump(0, 1);
    require(recorder.frames.back().second.width == 1920 && recorder.frames.back().first == 0);
    require(!session->startVideo(2, 640, 480, 30));    // the ultra wide front is in no set with both
    require(camera->videoSize(2) == Size(0, 0));

    session->stopVideo(1);
    require(camera->videoSize(0) == Size(1600, 900));  // alone again: its smallest format
    // The other front camera is a set with the back one, in its turn.
    require(session->startVideo(2, 640, 480, 30));
    require(camera->videoSize(0) == Size(1920, 1080) && camera->videoSize(2) == Size(640, 480));
    session->stopVideo(2);

    // A camera that has no format a multi-camera session takes at its size cannot be joined.
    require(session->startVideo(0, 3840, 2160, 30));
    require(camera->videoSize(0) == Size(3840, 2160));
    require(!session->startVideo(1, 640, 480, 30));
    require(camera->videoSize(0) == Size(3840, 2160) && !camera->videoRunning(1));
    // Nor can it be moved by a request for a size no format of its kind covers at that rate.
    session->stopVideo(0);
    require(session->startVideo(1, 1920, 1080, 30));
    require(!session->startVideo(0, 3840, 2160, 30));
    require(camera->videoSize(1) == Size(1920, 1080) && !camera->videoRunning(0));
}

// A camera that was given an anchor (its back camera lists capture formats) runs in it and nowhere else: a stream
// outside the anchor, a still it does not make and an anchor a multi-camera session does not take are refused, and the
// frames are the stream's size in the anchor's aspect ratio. A camera with none keeps the session's own rule.
void anchoredCamera()
{
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    const auto info = camera->cameras();
    require(info[0].captureFormats.size() == 7 && info[0].videoFormats.size() == 7 && info[1].captureFormats.empty());
    CameraStillRequest still;
    still.jpegWidth = 1440;
    still.jpegHeight = 1080;
    {
        // Without an anchor the session picks a format for each request, as it always did.
        Recorder recorder;
        auto session = camera->openSession(recorder);
        require(camera->sessionAnchorCalls() == 0 && camera->sessionAnchors().empty() && camera->sessionLimits().maximumStillPixels == 0);
        require(session->startVideo(0, 1920, 1080, 60) && session->takeStill(0, still));
        session->stop();
    }
    Recorder recorder;
    auto session = camera->openSession(recorder);
    // The 1920x1440 format (1 to 30 fps) of the back camera, in a 12.7 MP cap.
    const std::optional<std::size_t> anchors[] {3, std::nullopt};
    session->setAnchors(anchors, {12'700'000, 6'220'800, 256});
    require(camera->sessionAnchorCalls() == 1 && camera->sessionAnchors() == std::vector<std::optional<std::size_t>>(anchors, anchors + 2) &&
                camera->sessionLimits().maximumStillPixels == 12'700'000 && camera->sessionLimits().maximumFrameBytes == 6'220'800 &&
                camera->sessionLimits().frameRowAlignment == 256,
            "the session records every field it was told");
    require(!session->startVideo(0, 1920, 1080, 60), "a rate the anchor does not take");
    require(!session->startVideo(0, 3840, 2160, 30), "a size larger than the anchor");
    require(session->startVideo(0, 1920, 1080, 30) && camera->videoSize(0) == Size(1920, 1440),
            "1080p is delivered in the anchor's 4:3 aspect ratio");
    require(session->startVideo(0, 640, 480, 30) && camera->videoSize(0) == Size(640, 480), "a resize");
    camera->pump(0, 1);
    require(!recorder.frames.empty() && recorder.frames.back().second.width == 640 && recorder.frames.back().second.height == 480);
    require(!session->takeStill(0, still), "a size another format makes (1440x1080) is not made by the anchor");
    still.jpegWidth = 4224;
    still.jpegHeight = 2376;
    require(!session->takeStill(0, still), "nor the 16:9 size of the 12 MP video format");
    still.jpegWidth = 4032;
    still.jpegHeight = 3024;
    require(session->takeStill(0, still), "a size the anchor makes");
    camera->pump(0, 0);
    require(recorder.stills.size() == 1);
    // The front camera has none: it joins as the session's own rule has it, and the anchored camera stays where it is
    // when it comes and goes.
    require(session->startVideo(0, 1920, 1080, 30) && camera->videoSize(0) == Size(1920, 1440));
    require(session->startVideo(1, 640, 480, 30), "a camera with no anchor beside an anchored one");
    require(camera->videoSize(0) == Size(1920, 1440), "the anchored camera is not moved for a peer");
    session->stopVideo(1);
    require(camera->videoSize(0) == Size(1920, 1440), "nor back to a smaller format when the peer leaves");
    session->stop();

    // An anchor a multi-camera session does not take (the 4032x3024 format) cannot be joined.
    Recorder second;
    auto alone = camera->openSession(second);
    const std::optional<std::size_t> large[] {4, std::nullopt};
    alone->setAnchors(large, {12'700'000, 6'220'800});
    require(alone->startVideo(0, 640, 480, 30), "alone, it runs");
    require(!alone->startVideo(1, 640, 480, 30), "a second camera cannot join an anchor no multi-camera session takes");
    require(camera->videoRunning(0) && !camera->videoRunning(1), "and the first runs on");
    alone->stopVideo(0);
    require(alone->startVideo(1, 640, 480, 30) && !alone->takeStill(0, still), "a still on it is refused too, beside a running camera");
}

// Every camera with an input counts in a multi-camera session, one that only
// takes stills as well: it joins as a streaming camera does, the cameras that
// stream move to formats the session takes (or the still is refused, nothing
// changed), and it leaves with its last still, the others going back.
void stillOnlyCamerasCount()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    CameraStillRequest request;
    request.jpegWidth = 1008;
    request.jpegHeight = 756;
    request.token = 7;

    // The back camera streams at a size of its own; a still on a front camera joins it.
    require(session->startVideo(0, 1600, 900, 30));
    require(camera->videoSize(0) == Size(1600, 900));
    require(session->takeStill(1, request));
    require(camera->videoSize(0) == Size(1920, 1080));  // moved to a format the session takes
    require(recorder.stills.empty());
    camera->pump(1, 0);
    require(recorder.stills.size() == 1 && recorder.stills.back().second.ok);
    require(camera->videoSize(0) == Size(1600, 900));  // alone again once the still is delivered

    // A still on a camera in no concurrent set with the streaming one is refused.
    require(session->startVideo(1, 640, 480, 30));
    require(!session->takeStill(2, request));
    require(camera->videoSize(0) == Size(1920, 1080) && camera->videoSize(1) == Size(640, 480));
    session->stopVideo(1);

    // A camera at a size no multi-camera format covers cannot be joined by a still either.
    session->stopVideo(0);
    require(session->startVideo(0, 3840, 2160, 30));
    request.token = 8;
    require(!session->takeStill(1, request));
    require(camera->videoSize(0) == Size(3840, 2160) && camera->videoRunning(0));
    // Without that stream the still is taken, and a stream cannot join it either while it is on its way.
    session->stopVideo(0);
    request.token = 9;
    require(session->takeStill(1, request));
    require(!session->startVideo(0, 3840, 2160, 30));
    camera->pump(1, 0);
    require(recorder.stills.size() == 2);
    require(session->startVideo(0, 3840, 2160, 30));  // the still has left the session
}

// A join whose cameras ask for more than the hardware gives (AVFoundation's
// hardwareCost over 1.0) is refused, and nothing changes.
void hardwareBudget()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    require(session->startVideo(0, 1920, 1080, 60));
    require(!session->startVideo(1, 1920, 1080, 60));
    require(!camera->videoRunning(1) && camera->videoSize(0) == Size(1920, 1080));
    require(session->startVideo(1, 1920, 1080, 30));  // half the rate fits
    // Raising a camera's rate is a join of its own and is refused the same way.
    require(!session->startVideo(1, 1920, 1080, 60));
    require(camera->videoFrameRate(1) == 30 && camera->videoFrameRate(0) == 60);
}

// A Bayer still the platform refuses, or that fails alone, leaves what else the
// capture asked for.
void rawFailures()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    CameraStillRequest request;
    request.jpegWidth = 1008;
    request.jpegHeight = 756;
    request.raw = true;
    camera->refuseRaw(true);
    require(!session->takeStill(0, request));
    request.raw = false;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(recorder.stills.back().second.ok && !recorder.stills.back().second.jpeg.empty());

    camera->refuseRaw(false);
    camera->failRaw(true);
    request.raw = true;
    request.token = 3;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    const auto& partial = recorder.stills.back().second;
    require(partial.ok && partial.token == 3 && !partial.jpeg.empty() && partial.raw.empty());
    request.jpegWidth = 0;
    request.token = 4;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(!recorder.stills.back().second.ok && recorder.stills.back().second.token == 4);
}

// The cause of an interruption outlives a session: one opened while it lasts is
// interrupted as it starts, with the same reason.
void interruptionOutlivesSession()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    {
        Recorder first;
        auto session = camera->openSession(first);
        camera->interrupt(CameraEventKind::interruptedBySystemPressure);
        require(first.events.size() == 1 && first.events[0].kind == CameraEventKind::interruptedBySystemPressure);
        session->stop();
    }
    Recorder second;
    auto session = camera->openSession(second);
    require(second.events.size() == 1 && second.events[0].kind == CameraEventKind::interruptedBySystemPressure &&
            second.events[0].camera == allCameras);
    camera->resume();
    require(second.events.size() == 2 && second.events[1].kind == CameraEventKind::resumed);
    session->stop();
    Recorder third;
    session = camera->openSession(third);
    require(third.events.empty());
    // post() delivers an event and changes nothing else.
    camera->post({1, CameraEventKind::unavailable});
    require(third.events.size() == 1 && third.events[0].kind == CameraEventKind::unavailable && third.events[0].camera == 1);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    require(third.frames.size() == 1);
}

// The colour data of every synthetic calibration, whatever the profile and lens: a DNG forward matrix maps equal camera
// values (1, 1, 1) to the D50 white point (0.9642, 1, 0.8249), which the DNG validation of the Android CTS checks (its
// tolerance is 0.01; the rule here is exact). A new profile, lens or matrix is covered by taking a Bayer still of each.
void everyCalibrationMapsEqualValuesToD50()
{
    constexpr double white[3] = {0.9642, 1.0, 0.8249};
    std::size_t calibrations = 0, phoneCalibrations = 0;
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        const auto camera = makeSyntheticCamera(manual(profile));
        Recorder recorder;
        auto session = camera->openSession(recorder);
        const auto cameras = camera->cameras();
        for (std::size_t index = 0; index < cameras.size(); ++index) {
            if (!cameras[index].rawStills) continue;
            CameraStillRequest request;
            request.token = std::uint32_t(100 + index);
            request.raw = true;
            require(session->takeStill(index, request));
            camera->pump(index, 0);
            const auto& color = recorder.stills.back().second.color;
            require(recorder.stills.back().second.ok && recorder.stills.back().second.token == request.token);
            for (const auto* matrix : {&color.forwardMatrix1, &color.forwardMatrix2})
                for (std::size_t row = 0; row < 3; ++row)
                    require(std::abs((*matrix)[row * 3] + (*matrix)[row * 3 + 1] + (*matrix)[row * 3 + 2] - white[row]) < 1e-9);
            ++calibrations;
            if (profile == SyntheticCameraProfile::phone) ++phoneCalibrations;
        }
    }
    // The rule is not vacuous: the phone's three lenses and the tablets' back cameras each have one.
    require(phoneCalibrations == 3 && calibrations >= 5);
}

void stills()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    CameraStillRequest request;
    request.token = 7;
    request.jpegWidth = 1008;
    request.jpegHeight = 756;
    require(session->takeStill(0, request));
    require(recorder.stills.empty());  // arrives on the camera's next tick
    camera->pump(0, 0);
    require(recorder.stills.size() == 1);
    const auto& jpeg = recorder.stills[0].second;
    require(jpeg.ok && jpeg.token == 7 && jpeg.raw.empty());
    require(jpeg.jpeg.size() > 1000 && jpeg.jpeg[0] == 0xff && jpeg.jpeg[1] == 0xd8);
    // An Exif segment follows the start of image or the JFIF segment.
    bool exif = false;
    for (std::size_t at = 2; at + 10 < jpeg.jpeg.size() && at < 200; ++at)
        exif |= jpeg.jpeg[at] == 0xff && jpeg.jpeg[at + 1] == 0xe1 && jpeg.jpeg[at + 4] == 'E' && jpeg.jpeg[at + 5] == 'x';
    require(exif);

    // The JPEG is the size asked for, which is one the camera lists; a size it
    // does not list is refused, even when a larger one covers it.
    CameraStillRequest unlisted = request;
    unlisted.token = 70;
    unlisted.jpegWidth = 1000;
    unlisted.jpegHeight = 700;
    require(!session->takeStill(0, unlisted));
    unlisted.jpegWidth = 1008;
    unlisted.jpegHeight = 755;
    require(!session->takeStill(0, unlisted));
    require(recorder.stills.size() == 1);

    // The Bayer still, with the colour data of the sensor.
    request = {};
    request.token = 8;
    request.raw = true;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    const auto& raw = recorder.stills.back().second;
    require(raw.ok && raw.token == 8 && raw.jpeg.empty());
    const auto info = camera->cameras()[0];
    require(raw.raw.size() == std::size_t(info.rawStillSize.width) * info.rawStillSize.height);
    require(raw.color.bayer == BayerOrder::rggb && raw.color.whiteLevel > raw.color.blackLevel);
    require(raw.color.rawWidth == info.rawStillSize.width && raw.color.colorMatrix1[0] != 0);
    for (const auto v : raw.raw) require(v >= raw.color.blackLevel && v <= raw.color.whiteLevel);
    // Red sites are brighter than blue ones in a chart with equal weights of both.
    double red = 0, blue = 0;
    for (std::uint32_t y = 100; y + 1 < info.rawStillSize.height; y += 2)
        for (std::uint32_t x = 0; x + 1 < info.rawStillSize.width; x += 2) {
            red += raw.raw[std::size_t(y) * info.rawStillSize.width + x];
            blue += raw.raw[std::size_t(y + 1) * info.rawStillSize.width + x + 1];
        }
    require(red > blue);
    // JPEG and Bayer in one capture.
    request.jpegWidth = 1008;
    request.jpegHeight = 756;
    request.token = 9;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(recorder.stills.back().second.ok && !recorder.stills.back().second.jpeg.empty() && !recorder.stills.back().second.raw.empty());

    // What the camera cannot take is refused at the request.
    request = {};
    request.jpegWidth = 9000;
    request.jpegHeight = 9000;
    require(!session->takeStill(0, request));
    request = {};
    require(!session->takeStill(0, request));
    request.raw = true;
    require(!session->takeStill(1, request));  // the front camera has no Bayer still
}

void interruptions()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    require(recorder.frames.size() == 1);
    camera->interrupt(CameraEventKind::interruptedByMultitasking);
    require(recorder.events.size() == 1 && recorder.events[0].kind == CameraEventKind::interruptedByMultitasking &&
            recorder.events[0].camera == allCameras);
    camera->pump(0, 3);
    require(recorder.frames.size() == 1);  // nothing flows while interrupted
    CameraStillRequest request;
    request.jpegWidth = 1008;
    request.jpegHeight = 756;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(!recorder.stills.back().second.ok);
    camera->resume();
    require(recorder.events.size() == 2 && recorder.events[1].kind == CameraEventKind::resumed);
    camera->pump(0, 1);
    require(recorder.frames.size() == 2);
}

// R5 (recheck6): the end of an interruption and a restart both ask for a recovery, so a session may be asked twice. A
// failed recovery is final for its session (the client closes it on the failure): a second one repairs nothing and
// announces nothing, and a new session recovers as usual.
void aFailedRecoveryIsFinalForItsSession()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    const auto count = [](const Recorder& recorder, CameraEventKind kind) {
        return std::count_if(recorder.events.begin(), recorder.events.end(), [kind](const CameraEvent& event) { return event.kind == kind; });
    };
    Recorder first;
    auto session = camera->openSession(first);
    require(session->startVideo(0, 640, 480, 30));
    camera->interrupt();
    camera->moveOutOfAnchor(true);
    camera->refuseRepair(true);
    camera->resume();
    require(count(first, CameraEventKind::failed) == 1 && count(first, CameraEventKind::resumed) == 0 && camera->repairs() == 0,
            "the repair is refused: the session failed");
    camera->resume();
    camera->refuseRepair(false);
    camera->resume();
    require(count(first, CameraEventKind::failed) == 1 && count(first, CameraEventKind::resumed) == 0 && camera->repairs() == 0,
            "a second and a third notification, even with the device willing, repair and announce nothing");
    session->stop();
    Recorder second;
    session = camera->openSession(second);
    require(session->startVideo(0, 640, 480, 30));
    camera->interrupt();
    camera->moveOutOfAnchor(true);
    camera->resume();
    require(count(second, CameraEventKind::resumed) == 1 && count(second, CameraEventKind::failed) == 0 && camera->repairs() == 1,
            "a new session recovers: repaired, then resumed");
}

void threaded()
{
    auto options = manual(SyntheticCameraProfile::tablet);
    options.manualClock = false;
    options.latencyMilliseconds = 5;
    const auto camera = makeSyntheticCamera(options);
    Recorder recorder;
    {
        auto session = camera->openSession(recorder);
        require(session->startVideo(0, 640, 480, 60));
        require(recorder.waitFor(4));
        CameraStillRequest request;
        request.jpegWidth = 1008;
        request.jpegHeight = 756;
        require(session->takeStill(0, request));
        require(recorder.waitFor(4, 1));
        std::lock_guard lock(recorder.mutex);
        require(recorder.stills[0].second.ok);
        // The exposure began a few milliseconds before delivery.
        for (const auto& [index, frame] : recorder.frames) {
            (void)index;
            require(frame.timestampNanoseconds + 4'000'000 <= camera->now());
        }
    }
    require(camera->sessionsOpen() == 0);
}

// The luma of a rectangle of a frame, averaged by area to `width` x `height` samples: what a device's scale of an
// anchored camera's frame makes of it.
std::vector<std::uint8_t> boxScaled(const CameraFrame& frame, std::uint32_t top, std::uint32_t rows, std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> out(std::size_t(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t x0 = x * frame.width / width, x1 = std::max(x0 + 1, (x + 1) * frame.width / width);
            const std::uint32_t y0 = top + y * rows / height, y1 = std::max(y0 + 1, top + (y + 1) * rows / height);
            std::uint64_t sum = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy)
                for (std::uint32_t sx = x0; sx < x1; ++sx) sum += frame.luma[std::size_t(sy) * frame.lumaStride + sx];
            out[std::size_t(y) * width + x] = std::uint8_t(sum / (std::uint64_t(x1 - x0) * (y1 - y0)));
        }
    return out;
}

// The marker lies across the middle of the picture, so the centred crop and the scale a device makes of an anchored
// camera's frame for a stream of another shape or size leave it readable (the V4L2 capture probe reads it there).
void chartMarkerSurvivesCropAndScale()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session != nullptr && session->startVideo(0, 1920, 1440, 30));
    camera->pump(0, 1);
    require(recorder.frames.size() == 1);
    const auto& frame = recorder.frames.front().second;
    require(frame.width == 1920 && frame.height == 1440);
    const auto whole = decodeChartMarker(frame.luma.data(), frame.lumaStride, frame.width, frame.height);
    require(whole.valid && whole.tag == chartTag(0) && whole.counter == 1);
    // The 16:9 stream's centred crop: the rows cut away are the picture's top and bottom.
    const std::uint32_t cropTop = (1440 - 1080) / 2;
    const auto cropped = decodeChartMarker(frame.luma.data() + std::size_t(cropTop) * frame.lumaStride, frame.lumaStride, 1920, 1080);
    require(cropped.valid && cropped.tag == whole.tag && cropped.counter == whole.counter);
    // The whole picture and the crop at sizes of both shapes, whole-number and fractional ratios.
    const struct { std::uint32_t top, rows, width, height; } streams[] = {
        {0, 1440, 1280, 960}, {0, 1440, 960, 720}, {0, 1440, 640, 480}, {0, 1440, 480, 360}, {0, 1440, 1000, 750},
        {cropTop, 1080, 1280, 720}, {cropTop, 1080, 960, 540}, {cropTop, 1080, 1920, 1080},
    };
    for (const auto& stream : streams) {
        const auto picture = boxScaled(frame, stream.top, stream.rows, stream.width, stream.height);
        const auto marker = decodeChartMarker(picture.data(), stream.width, stream.width, stream.height);
        require(marker.valid && marker.tag == whole.tag && marker.counter == whole.counter);
    }
    // A picture of the camera's own formats reads back too, at widths the cells do not divide evenly (1440, 4032).
    for (const auto& [width, height] : {std::pair<std::uint32_t, std::uint32_t> {640, 480}, {1280, 720}, {1440, 1080}, {1920, 1080}}) {
        const auto other = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
        Recorder own;
        auto owned = other->openSession(own);
        require(owned != nullptr && owned->startVideo(0, width, height, 30));
        other->pump(0, 1);
        require(own.frames.size() == 1);
        const auto& picture = own.frames.front().second;
        require(picture.width == width && picture.height == height);
        require(decodeChartMarker(picture.luma.data(), picture.lumaStride, picture.width, picture.height).valid);
    }
}

// The real timer's schedule when a frame is late. A frame that lasts 200 ms at 10 fps is followed by the next one a whole
// frame duration after its own start, also when the delegate held the timer thread long enough to make the next frame overdue
// (a period or more, or less), and a frame never starts before the callback that held the thread returned: its timestamp is
// not earlier than that moment. Lower bounds only: a loaded host may start a frame late.
void timerHeldBy(std::chrono::milliseconds hold)
{
    auto options = manual(SyntheticCameraProfile::tablet);
    options.manualClock = false;
    options.latencyMilliseconds = 0;
    const auto camera = makeSyntheticCamera(options);
    Recorder recorder;
    recorder.firstFrameDelay = hold;
    recorder.clock = [&camera] { return camera->now(); };
    {
        auto session = camera->openSession(recorder);
        CameraSettings settings;
        settings.exposureMode = CameraExposureMode::manual;
        settings.exposureNanoseconds = 200'000'000;
        settings.iso = 100;
        session->apply(0, settings);
        require(session->startVideo(0, 640, 480, 10));
        require(recorder.waitFor(4));
        std::lock_guard lock(recorder.mutex);
        for (std::size_t i = 0; i < 4; ++i)
            require(recorder.frames[i].second.exposure.exposureNanoseconds == 200'000'000 &&
                    recorder.frames[i].second.exposure.frameDurationNanoseconds == 200'000'000);
        // Frame 2 starts after the callback of frame 1 returned.
        require(recorder.frames[1].second.timestampNanoseconds >= recorder.returns[0]);
        // Frames 2 to 4 are at least a frame duration apart (the timestamp is the frame's start, and the next deadline is a
        // whole duration after it).
        for (std::size_t i = 2; i < 4; ++i) {
            const auto interval = recorder.frames[i].second.timestampNanoseconds - recorder.frames[i - 1].second.timestampNanoseconds;
            require(interval + 2'000'000 >= recorder.frames[i - 1].second.exposure.frameDurationNanoseconds);
        }
    }
    require(camera->sessionsOpen() == 0);
}

void timerSchedulesWholeDurations()
{
    timerHeldBy(std::chrono::milliseconds(350));  // overdue by more than a period
    timerHeldBy(std::chrono::milliseconds(250));  // overdue by less than one
}

} // namespace

// The control pipe: a test that cannot reach the camera object (a guest in another process) interrupts and resumes it by
// writing lines, and the camera answers as it does to interrupt() and resume().
// A delegate whose events are read from another thread than the one that delivers them.
class EventLog final : public CameraSessionDelegate {
public:
    void cameraFrame(std::size_t, CameraFrame) override {}
    void cameraStill(std::size_t, CameraStillResult) override {}
    void cameraEvent(CameraEvent event) override
    {
        std::lock_guard lock(mutex);
        events.push_back(event);
    }
    std::vector<CameraEvent> snapshot() const
    {
        std::lock_guard lock(mutex);
        return events;
    }

private:
    mutable std::mutex mutex;
    std::vector<CameraEvent> events;
};

void controlPipe()
{
    const auto line = [](std::string_view text) { return parseSyntheticControlLine(text); };
    require(line("interrupt") && line("interrupt")->interrupt && line("interrupt")->kind == CameraEventKind::interrupted);
    require(line(" Interrupt   Background \r") && line("interrupt background")->kind == CameraEventKind::interruptedInBackground);
    require(line("interrupt multitasking")->kind == CameraEventKind::interruptedByMultitasking);
    require(line("interrupt pressure")->kind == CameraEventKind::interruptedBySystemPressure);
    require(line("interrupt other")->kind == CameraEventKind::interrupted);
    require(line("resume") && !line("resume")->interrupt && line("resume")->kind == CameraEventKind::resumed);
    require(!line("") && !line("interrupted") && !line("interrupt later") && !line("resume now") && !line("pause"));

    // A named pipe is a socket-length path: the host temporary directory, not the build tree's.
    uml::testing::ScratchSubdirectory directory("camera-control-XXXXXX", uml::testing::socket_scratch_parent());
    const std::string path = (directory.path() / "control").string();
    {
        auto options = manual(SyntheticCameraProfile::tablet);
        options.controlPath = path;
        const auto camera = makeSyntheticCamera(options);
        EventLog log;
        auto session = camera->openSession(log);
        const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
        require(writer >= 0, "the pipe exists once the camera does");
        const auto send = [&](std::string_view text) { require(::write(writer, text.data(), text.size()) == ssize_t(text.size())); };
        const auto waitForEvents = [&](std::size_t count) {
            for (int attempt = 0; attempt < 200 && log.snapshot().size() < count; ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return log.snapshot().size() >= count;
        };
        send("interrupt back");
        send("ground\nnot a command\n");
        require(waitForEvents(1) && log.snapshot()[0].kind == CameraEventKind::interruptedInBackground &&
                    log.snapshot()[0].camera == allCameras,
                "a line in two writes is one command, and an unknown line is ignored");
        send("resume\n");
        require(waitForEvents(2) && log.snapshot()[1].kind == CameraEventKind::resumed, "and resume ends it");
        ::close(writer);
        // A writer's close does not end the reading: the pipe has a reader of its own.
        const int again = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
        require(again >= 0);
        const std::string_view last = "interrupt pressure\n";
        require(::write(again, last.data(), last.size()) == ssize_t(last.size()));
        require(waitForEvents(3) && log.snapshot()[2].kind == CameraEventKind::interruptedBySystemPressure);
        ::close(again);
        session->stop();
    }
    ::unlink(path.c_str());
}

// The application's departure ends a video only for a session that was running when it left. A session opened while the
// application is away never had the video: it reports an ordinary interruption, whatever cause outlives it, and every other
// cause is reported as it was.
void sessionOpenedWhileAway()
{
    const auto kindsOfFreshSession = [](CameraEventKind cause) {
        const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
        camera->interrupt(cause);
        Recorder recorder;
        auto session = camera->openSession(recorder);
        std::lock_guard lock(recorder.mutex);
        std::vector<CameraEventKind> kinds;
        for (const auto& event : recorder.events) {
            require(event.camera == allCameras, "an interruption belongs to the whole session");
            kinds.push_back(event.kind);
        }
        return kinds;
    };
    using Kinds = std::vector<CameraEventKind>;
    require(kindsOfFreshSession(CameraEventKind::interruptedInBackground) == Kinds {CameraEventKind::interrupted},
            "opened while the application is away: interrupted, not the application's departure");
    require(kindsOfFreshSession(CameraEventKind::interrupted) == Kinds {CameraEventKind::interrupted});
    require(kindsOfFreshSession(CameraEventKind::interruptedByMultitasking) == Kinds {CameraEventKind::interruptedByMultitasking});
    require(kindsOfFreshSession(CameraEventKind::interruptedBySystemPressure) == Kinds {CameraEventKind::interruptedBySystemPressure});

    // A session that was running when the application left hears the departure; the session opened after it, still away,
    // hears an ordinary interruption, and both are told when the application is back.
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder running;
    auto first = camera->openSession(running);
    require(running.events.empty(), "a session opened in the foreground hears nothing");
    camera->interrupt(CameraEventKind::interruptedInBackground);
    {
        std::lock_guard lock(running.mutex);
        require(running.events.size() == 1 && running.events[0].kind == CameraEventKind::interruptedInBackground,
                "the session that was running hears the departure");
    }
    first.reset();
    require(camera->sessionsOpen() == 0);
    Recorder reopened;
    auto second = camera->openSession(reopened);
    {
        std::lock_guard lock(reopened.mutex);
        require(reopened.events.size() == 1 && reopened.events[0].kind == CameraEventKind::interrupted,
                "the session opened while away is interrupted");
    }
    camera->resume();
    // The return ends the interruption of the session opened in the foreground state as well; a later departure is the
    // application's again.
    second.reset();
    Recorder back;
    auto third = camera->openSession(back);
    require(back.events.empty(), "opened with the application back in the foreground: nothing to report");
    camera->interrupt(CameraEventKind::interruptedInBackground);
    {
        std::lock_guard lock(back.mutex);
        require(back.events.size() == 1 && back.events[0].kind == CameraEventKind::interruptedInBackground,
                "a second departure is reported as one again");
    }
    third.reset();
}

int main()
{
    controlPipe();
    sessionOpenedWhileAway();
    profiles();
    aFrontCameraPresentedAt90IsTurnedHalfAround();
    unauthorized();
    video();
    chartMarkerSurvivesCropAndScale();
    controls();
    exposureReport();
    everyProfileExposes();
    multiCamera();
    multiCameraFormats();
    anchoredCamera();
    stillOnlyCamerasCount();
    hardwareBudget();
    rawFailures();
    everyCalibrationMapsEqualValuesToD50();
    interruptionOutlivesSession();
    stills();
    interruptions();
    aFailedRecoveryIsFinalForItsSession();
    threaded();
    timerSchedulesWholeDurations();
    return 0;
}
