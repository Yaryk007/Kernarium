// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "camera/background_video.hpp"
#include "camera/calibration_store.hpp"
#include "camera/camera_device.hpp"
#include "camera/camera_diagnostics.hpp"
#include "camera/camera_layout.hpp"
#include "camera/camera_orientation.hpp"
#include "camera/camera_source.hpp"
#include "camera/camera_tiers.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kernarium::camera {

/* The cameras an Android guest is given (runtime API 37): what the boot request
 * says about them, the layout composed from the host's cameras, and the two
 * statements the application makes while the guest runs, the camera state it
 * hears and the rotation it gives. darwin/device/camera/README.md owns the
 * device and the layouts. */

/* What a request states before the camera mode and the calibration path were
 * added to it. */
inline constexpr std::size_t bootSizeBeforeMode =
    offsetof(kernarium_runtime_boot, android_camera_mode);
inline constexpr std::size_t bootSizeBeforeCalibrationPath =
    offsetof(kernarium_runtime_boot, android_camera_calibration_path);

/* The camera part of a boot request. */
struct Request {
    uml::camera::CameraMode mode {uml::camera::CameraMode::blocked};
    /* Where the colour data a Bayer still teaches is kept; in memory when
     * empty. */
    std::string calibrationDirectory;
    /* The request stated a mode or a path that is not one: the runtime drops
     * it (a camera never fails a boot) and says so. */
    bool unknownMode {};
    bool invalidCalibrationPath {};
};

/* A calibration directory the application states: empty for none (a null
 * path, or one that is not a path, which sets `invalid`). */
[[nodiscard]] inline std::string readCalibrationPath(const char* path, bool& invalid)
{
    if (path == nullptr)
        return {};
    const std::size_t length = ::strnlen(path, PATH_MAX);
    if (length == 0 || length == PATH_MAX) {
        invalid = true;
        return {};
    }
    return std::string(path, length);
}

/* The camera fields of `boot`, read only as far as its struct_size covers. A
 * request that states nothing runs blocked. */
[[nodiscard]] inline Request readRequest(const kernarium_runtime_boot& boot)
{
    Request request;
    if (boot.struct_size >= bootSizeBeforeMode + sizeof(boot.android_camera_mode)) {
        switch (boot.android_camera_mode) {
        case KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED:
            break;
        case KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE:
            request.mode = uml::camera::CameraMode::live;
            break;
        default:
            request.unknownMode = true;
            break;
        }
    }
    if (boot.struct_size >=
            bootSizeBeforeCalibrationPath + sizeof(boot.android_camera_calibration_path))
        request.calibrationDirectory = readCalibrationPath(
            boot.android_camera_calibration_path, request.invalidCalibrationPath);
    return request;
}

/* The cameras a run presents to its guest, discovered once: the platform
 * camera that serves them, the colour data store, the memory tier and the
 * layout that describes them. Discovery asks the system for no permission and
 * opens no session, so the layout is the same whether the session is live or
 * blocked. */
struct Presentation {
    uml::camera::CameraSourceHandle source;
    std::shared_ptr<uml::camera::CalibrationStore> calibration;
    uml::camera::CameraTier tier {uml::camera::CameraTier::small};
    /* The vendor SKU's value: one of the image's layouts, "none" when the host
     * has no cameras the image can describe. */
    std::string layout {uml::camera::noCameraLayout};
    /* What the device is given (`CameraDeviceOptions::use`): the cameras it lets
     * run at once and whether a RAW capture is possible, from the cameras and
     * the colour data of this composition. */
    uml::camera::CameraUse use;
    /* The use the GPU's pool was reduced for: the first composition's, which no
     * later boot of the run exceeds (`composeCameras`). */
    uml::camera::CameraUse reserved;
    /* Whether the first composition had cameras: a run that had none has no
     * reserve, and a later boot has none either. */
    bool reservedCameras {};

    /* Whether the guest gets a camera device at all. */
    [[nodiscard]] bool hasCameras() const noexcept { return uml::camera::layoutHasCameras(layout); }
};

/* The layout of `presentation`'s cameras now: what the calibration store has
 * learned since decides whether RAW is among them. */
[[nodiscard]] inline uml::camera::ComposedCameras composeNow(const Presentation& presentation)
{
    uml::camera::ArrangementOptions arrangement;
    arrangement.tier = presentation.tier;
    return uml::camera::composeCameras(*presentation.source, *presentation.calibration, arrangement);
}

/* Discovers `source`'s cameras at `tier` and composes the layout. A null
 * source is a host with no camera. */
[[nodiscard]] inline Presentation present(
    uml::camera::CameraSourceHandle source,
    uml::camera::CameraTier tier,
    const std::string& calibrationDirectory)
{
    Presentation presentation;
    presentation.tier = tier;
    presentation.calibration = std::make_shared<uml::camera::CalibrationStore>(
        calibrationDirectory);
    if (source == nullptr)
        return presentation;
    presentation.source = std::move(source);
    auto composed = composeNow(presentation);
    presentation.layout = std::move(composed.layout);
    presentation.use = presentation.reserved = composed.use;
    presentation.reservedCameras = uml::camera::layoutHasCameras(presentation.layout);
    return presentation;
}

/* Composes the layout again, at each boot of a run: the device is made anew
 * from the same store, and a Bayer still taught it a lens's colour data after
 * the last boot, so the guest has to be told of the RAW the camera device now
 * offers. A presentation with no cameras stays as it is. */
inline void recompose(Presentation& presentation)
{
    if (presentation.source == nullptr)
        return;
    /* The pool was sized for the first composition's use, and a later boot is
     * held to all of it: the layout the guest is told is one the reservation
     * allows (`composeCamerasWithin`). The colour data a still taught moves a lens
     * from capturing RAW to offering it, which is within the first use. */
    uml::camera::ArrangementOptions arrangement;
    arrangement.tier = presentation.tier;
    auto composed = uml::camera::composeCamerasWithin(
        *presentation.source, *presentation.calibration, arrangement, {presentation.reservedCameras, presentation.reserved});
    presentation.layout = std::move(composed.layout);
    presentation.use = composed.use;
}

/* The host memory a live session of a host's cameras would hold, which the
 * application asks for before the boot (runtime API 40). It is the figure the
 * boot takes out of the GPU's pool: the use composed from the cameras and the
 * colour data the store holds, and what that use lets a session hold
 * (`liveCameraReserveMiB`, of the first composition's use, which is the run's
 * reservation). A host with no camera holds none, and that is as exact as any
 * other figure. Discovery needs no permission and opens no session. */
struct ReservePlan {
    std::uint32_t reserveMiB {};
    /* False where the cameras could not be discovered and the figure is the
     * tier's ceiling. */
    bool exact {true};
};

/* The reserve of `source`'s cameras (a null source is a host with no camera)
 * at `tier`, with the colour data `calibrationDirectory` holds. */
[[nodiscard]] inline ReservePlan planReserve(
    uml::camera::CameraSourceHandle source,
    uml::camera::CameraTier tier,
    const std::string& calibrationDirectory)
{
    const auto cameras = present(std::move(source), tier, calibrationDirectory);
    return {
        static_cast<std::uint32_t>(uml::camera::liveCameraReserveMiB(
            uml::camera::CameraMode::live, cameras.layout, cameras.reserved, cameras.tier)),
        true};
}

/* The reserve of a host whose cameras could not be discovered: the tier's
 * ceiling, which bounds the layouts of the system cameras (a development
 * synthetic source can hold more). */
[[nodiscard]] inline ReservePlan ceilingReserve(uml::camera::CameraTier tier)
{
    return {
        static_cast<std::uint32_t>(uml::camera::cameraHostMemoryReserveMiB(tier)),
        false};
}

/* The runtime's state value of the device's. */
[[nodiscard]] constexpr int stateValue(uml::camera::CameraState state) noexcept
{
    switch (state) {
    case uml::camera::CameraState::idle:
        return KERNARIUM_RUNTIME_CAMERA_STATE_IDLE;
    case uml::camera::CameraState::streaming:
        return KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING;
    case uml::camera::CameraState::interrupted:
        return KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED;
    case uml::camera::CameraState::interruptedByMultitasking:
        return KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING;
    case uml::camera::CameraState::interruptedBySystemPressure:
        return KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE;
    case uml::camera::CameraState::blockedInUse:
        return KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE;
    }
    return KERNARIUM_RUNTIME_CAMERA_STATE_IDLE;
}

/* The one receiver of the camera state. The callback runs with the mutex held,
 * which is what makes clearing it wait for a call in progress; it reports a
 * state when it changes, and the state in force to a new callback at once. */
class StateReceiver final {
public:
    void set(kernarium_runtime_camera_state_callback callback, void* context)
    {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        context_ = context;
        if (callback != nullptr)
            callback(context, state_);
    }

    /* The device's state, on its transport thread. */
    void publish(uml::camera::CameraState state)
    {
        publishValue(stateValue(state));
    }

    /* The guest ended or restarts: no camera is open any more. */
    void ended() { publishValue(KERNARIUM_RUNTIME_CAMERA_STATE_IDLE); }

    [[nodiscard]] int current()
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

private:
    void publishValue(int state)
    {
        std::lock_guard lock(mutex_);
        if (state == state_)
            return;
        state_ = state;
        if (callback_ != nullptr)
            callback_(context_, state);
    }

    std::mutex mutex_;
    kernarium_runtime_camera_state_callback callback_ {};
    void* context_ {};
    int state_ {KERNARIUM_RUNTIME_CAMERA_STATE_IDLE};
};

/* The turn of the device body the application states: the clockwise degrees
 * from portrait, as its user sees it, where the host owns the guest's rotation
 * (an iPad window: the guest stays upright in the window while the device,
 * which carries every built-in sensor, turns under it); 0 where the guest owns
 * the rotation, which leaves every camera as the platform states it. A boot
 * reads it once, to derive each camera's SENSOR_ORIENTATION; a change after
 * the boot reaches the cameras as a turn of the picture from that figure
 * (camera_orientation.hpp). */
class Rotation final {
public:
    /* 0, 90, 180 or 270; false for another value, which changes nothing. */
    bool set(std::uint32_t degrees) noexcept
    {
        if (degrees >= 360 || degrees % 90 != 0)
            return false;
        degrees_.store(degrees, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] std::uint32_t degrees() const noexcept
    {
        return degrees_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint32_t> degrees_ {0};
};

/* The device's options for a boot of a session of `mode`. The receiver and the
 * rotation outlive every device, and so does `background`, the application's way
 * to end the guest's video when it leaves the foreground. The turn the application has stated now is the
 * boot's: the guest's SENSOR_ORIENTATIONs follow it, and the picture turns by
 * the change since. */
[[nodiscard]] inline uml::camera::CameraDeviceOptions deviceOptions(
    const Presentation& presentation,
    uml::camera::CameraMode mode,
    StateReceiver& receiver,
    const Rotation& rotation,
    std::shared_ptr<uml::camera::BackgroundVideo> background = {})
{
    uml::camera::CameraDeviceOptions options;
    options.mode = mode;
    options.background = std::move(background);
    options.tier = presentation.tier;
    options.use = presentation.use;
    options.calibration = presentation.calibration;
    const auto bootTurn = rotation.degrees();
    options.sensorOrientation = [bootTurn](const uml::platform::CameraInfo& info) {
        return uml::camera::turnedSensorOrientation(
            info.position, info.sensorOrientation, bootTurn);
    };
    options.automaticRotation = [&rotation, bootTurn](const uml::platform::CameraInfo& info) {
        return uml::camera::pictureTurn(info.position, bootTurn, rotation.degrees());
    };
    options.onState = [&receiver](uml::camera::CameraState state) {
        receiver.publish(state);
    };
    /* What reaches the host and what it costs, for a development run to read. */
    options.onRotateAndCrop = uml::camera::printRotateAndCrop;
    options.onFlash = uml::camera::printFlash;
    options.onCheckpoint = uml::camera::printCheckpoint;
    return options;
}

/* Which camera a runtime serves, from the environment: the host's (the
 * default), the host's back cameras alone, or the synthetic one that tests and
 * the Simulator use, which is no camera of the machine's and never captures
 * one. */
inline constexpr const char* sourceVariable = "KERNARIUM_CAMERA_SOURCE";

/* The host's cameras with only the back-facing ones openable, for a test on a
 * device whose front camera must never be opened. The host's cameras are
 * listed as they are, so the layout the guest is given is the one an image
 * carries (a layout needs the front camera), but a session call for any
 * camera that is not a back camera is refused here and never reaches the
 * host: no capture device of the front camera is started, however the guest
 * asks. The guest sees a front camera that fails to open. */
class BackCamerasOnly final : public uml::platform::Camera {
public:
    explicit BackCamerasOnly(uml::camera::CameraSourceHandle host)
        : host_(std::move(host))
    {
        for (const auto& info : host_->cameras())
            openable_.push_back(info.position == uml::platform::CameraPosition::back);
        /* A test that must never open the front camera requires this line in
         * the log before it lets any camera open. */
        std::fprintf(stderr, "Kernarium: cameras from the host, back cameras alone openable\n");
    }

    [[nodiscard]] std::vector<uml::platform::CameraInfo> cameras() const override
    {
        return host_->cameras();
    }
    [[nodiscard]] std::vector<std::vector<std::size_t>> concurrentSets() const override
    {
        return host_->concurrentSets();
    }
    [[nodiscard]] std::string hostModel() const override { return host_->hostModel(); }
    [[nodiscard]] std::uint64_t now() const noexcept override { return host_->now(); }
    [[nodiscard]] std::unique_ptr<uml::platform::Nv12Transformer> makeFrameTransformer() const override
    {
        return host_->makeFrameTransformer();
    }
    [[nodiscard]] std::unique_ptr<uml::platform::CameraSession> openSession(
        uml::platform::CameraSessionDelegate& delegate) override
    {
        auto session = host_->openSession(delegate);
        if (session == nullptr)
            return nullptr;
        return std::make_unique<Session>(std::move(session), openable_);
    }

private:
    class Session final : public uml::platform::CameraSession {
    public:
        Session(std::unique_ptr<uml::platform::CameraSession> host, std::vector<bool> openable)
            : host_(std::move(host)), openable_(std::move(openable))
        {
        }

        [[nodiscard]] bool startVideo(
            std::size_t camera, std::uint32_t width, std::uint32_t height,
            std::uint32_t frameRate) override
        {
            return openable(camera, true) && host_->startVideo(camera, width, height, frameRate);
        }
        void stopVideo(std::size_t camera) override
        {
            if (openable(camera, false))
                host_->stopVideo(camera);
        }
        void apply(std::size_t camera, const uml::platform::CameraSettings& settings) override
        {
            if (openable(camera, false))
                host_->apply(camera, settings);
        }
        [[nodiscard]] bool takeStill(
            std::size_t camera, const uml::platform::CameraStillRequest& request) override
        {
            return openable(camera, true) && host_->takeStill(camera, request);
        }
        void setAnchors(std::span<const std::optional<std::size_t>> formatOfCamera,
                        const uml::platform::CameraLimits& limits) override
        {
            host_->setAnchors(formatOfCamera, limits);
        }
        void stop() override { host_->stop(); }

    private:
        /* Whether the host may be asked for `camera`; a refused request to
         * open one says so, once for each camera. */
        [[nodiscard]] bool openable(std::size_t camera, bool opens)
        {
            if (camera < openable_.size() && openable_[camera])
                return true;
            if (opens && !reported_.contains(camera)) {
                reported_.insert(camera);
                std::fprintf(
                    stderr,
                    "Kernarium: camera %zu is not a back camera: refused (%s=host:back)\n",
                    camera, sourceVariable);
            }
            return false;
        }

        std::unique_ptr<uml::platform::CameraSession> host_;
        std::vector<bool> openable_;
        std::set<std::size_t> reported_;
    };

    uml::camera::CameraSourceHandle host_;
    std::vector<bool> openable_;
};

struct SourceChoice {
    bool synthetic {};
    /* The host's cameras, the back ones alone openable (`BackCamerasOnly`). */
    bool backOnly {};
    uml::camera::SyntheticProfile profile {uml::camera::SyntheticProfile::phone};
    /* The value was not one of the names. A test knob that cannot be read must
     * never fall back to a real camera, so the guest gets no cameras
     * (`chooseSource`). */
    bool invalid {};
};

/* "host" or nothing (the variable is not set), "host:back" (the host's, back
 * cameras alone openable), "synthetic" (the phone) or "synthetic:" with phone,
 * tablet or laptop. A variable that is set but empty is not a name: a harness
 * writes one when its own variable is unset. */
[[nodiscard]] inline SourceChoice parseSource(const char* value)
{
    SourceChoice choice;
    if (value == nullptr)
        return choice;
    const std::string_view text {value};
    if (text == "host")
        return choice;
    if (text == "host:back") {
        choice.backOnly = true;
        return choice;
    }
    if (text == "synthetic" || text == "synthetic:phone") {
        choice.synthetic = true;
    } else if (text == "synthetic:tablet") {
        choice.synthetic = true;
        choice.profile = uml::camera::SyntheticProfile::tablet;
    } else if (text == "synthetic:laptop") {
        choice.synthetic = true;
        choice.profile = uml::camera::SyntheticProfile::laptop;
    } else {
        choice.invalid = true;
    }
    return choice;
}

/* The source the choice names: the synthetic camera, the host's (made by
 * `host`, which is not called for any other choice), or none, a host with no
 * cameras, for a value that is not a name. */
template <class HostSource>
[[nodiscard]] uml::camera::CameraSourceHandle chooseSource(
    const SourceChoice& choice, HostSource&& host)
{
    if (choice.invalid)
        return nullptr;
    if (choice.synthetic)
        return uml::camera::makeSyntheticCameraSource(choice.profile);
    auto source = host();
    if (choice.backOnly && source != nullptr)
        return std::make_shared<BackCamerasOnly>(std::move(source));
    return source;
}

} // namespace kernarium::camera
