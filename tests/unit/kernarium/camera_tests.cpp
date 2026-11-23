// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumCamera.hpp"
#include "camera/camera_controls.hpp"
#include "camera/camera_description.hpp"
#include "named_tests.hpp"
#include "photo_dimensions.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace kernarium::camera;
using uml::camera::CameraMode;
using uml::camera::CameraState;
using uml::camera::CameraTier;
using uml::testing::require;

kernarium_runtime_boot fullBoot()
{
    kernarium_runtime_boot boot {};
    boot.struct_size = sizeof(boot);
    return boot;
}

void aRequestThatStatesNothingIsBlocked()
{
    auto boot = fullBoot();
    auto request = readRequest(boot);
    require(request.mode == CameraMode::blocked);
    require(request.calibrationDirectory.empty());
    require(!request.unknownMode && !request.invalidCalibrationPath);

    // An application built before the fields states nothing, whatever follows
    // its struct in memory.
    boot.android_camera_mode = KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE;
    static const char directory[] = "/calibration";
    boot.android_camera_calibration_path = directory;
    boot.struct_size = bootSizeBeforeMode;
    request = readRequest(boot);
    require(request.mode == CameraMode::blocked, "the mode is read only if the struct covers it");
    require(request.calibrationDirectory.empty(), "and so is the path");
    boot.struct_size = bootSizeBeforeCalibrationPath;
    request = readRequest(boot);
    require(request.mode == CameraMode::live, "the mode alone");
    require(request.calibrationDirectory.empty());
    boot.struct_size = bootSizeBeforeMode + sizeof(boot.android_camera_mode) - 1;
    require(readRequest(boot).mode == CameraMode::blocked, "half a mode is none");
}

void theRequestStatesTheModeAndThePath()
{
    auto boot = fullBoot();
    static const char directory[] = "/calibration";
    boot.android_camera_mode = KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE;
    boot.android_camera_calibration_path = directory;
    auto request = readRequest(boot);
    require(request.mode == CameraMode::live);
    require(request.calibrationDirectory == directory);
    require(!request.unknownMode && !request.invalidCalibrationPath);

    boot.android_camera_mode = KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED;
    require(readRequest(boot).mode == CameraMode::blocked);
}

void aCameraNeverFailsABoot()
{
    auto boot = fullBoot();
    boot.android_camera_mode = 7;
    static const char empty[] = "";
    boot.android_camera_calibration_path = empty;
    const auto request = readRequest(boot);
    require(request.mode == CameraMode::blocked, "an unknown mode is blocked, the safe one");
    require(request.unknownMode);
    require(request.invalidCalibrationPath && request.calibrationDirectory.empty(),
        "an empty path keeps the colour data in memory");

    const std::string tooLong(PATH_MAX + 1, 'a');
    boot.android_camera_calibration_path = tooLong.c_str();
    require(readRequest(boot).invalidCalibrationPath);
}

void theEnumsAreTheApplicationsWords()
{
    require(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED == 0 && KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE == 1);
    require(KERNARIUM_RUNTIME_CAMERA_STATE_IDLE == 0);
    // Every device state has a distinct runtime value, and the states the
    // application tells apart are not merged.
    std::vector<int> seen;
    for (const auto state :
         {CameraState::idle, CameraState::streaming, CameraState::interrupted,
          CameraState::interruptedByMultitasking, CameraState::interruptedBySystemPressure,
          CameraState::blockedInUse}) {
        const int value = stateValue(state);
        for (const int earlier : seen) require(earlier != value, "distinct");
        seen.push_back(value);
    }
    require(stateValue(CameraState::streaming) == KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING);
    require(stateValue(CameraState::interruptedByMultitasking) ==
        KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING);
    require(stateValue(CameraState::interruptedBySystemPressure) ==
        KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE);
    require(stateValue(CameraState::blockedInUse) == KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE &&
            KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE == 5, "the value the application reads");
}

void aHostWithoutCamerasPresentsNone()
{
    const auto none = present(nullptr, CameraTier::large, "");
    require(none.layout == "none" && !none.hasCameras() && none.source == nullptr);
    require(none.calibration != nullptr);
}

void theLayoutFollowsTheCamerasAndTheTier()
{
    const auto phone = [](CameraTier tier) {
        return present(uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), tier, "");
    };
    const auto large = phone(CameraTier::large);
    require(large.layout == "bf-large-flash-manual-concurrent-front2160", large.layout);
    require(large.hasCameras() && large.source != nullptr && large.tier == CameraTier::large);
    // The layout is discovery alone: no session opened.
    require(phone(CameraTier::small).layout == "bf-small-flash-manual");

    const auto laptop = present(
        uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::laptop), CameraTier::large, "");
    require(laptop.layout == "front", laptop.layout);
    require(laptop.hasCameras());
    // The vendor SKU names a layout of the image's closed set.
    const auto names = uml::camera::allLayoutNames();
    for (const auto* layout : {&large.layout, &laptop.layout})
        require(std::find(names.begin(), names.end(), *layout) != names.end(), *layout);
}

void everyHostComposesTheSmallTierForNow()
{
    // A budget that would have earned the large tier composes the small layout:
    // no 2160p front camera, and no layout of the large tier.
    const auto tierOf = [](std::uint64_t gpuBudgetBytes) { return uml::camera::cameraTierForBudget(gpuBudgetBytes); };
    for (const std::uint64_t budget : {std::uint64_t {534} << 20, std::uint64_t {1249} << 20, std::uint64_t {4096} << 20}) {
        const auto phone = present(
            uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), tierOf(budget), "");
        require(phone.tier == CameraTier::small, "the small tier");
        require(phone.layout == "bf-small-flash-manual", phone.layout);
        require(phone.layout.find("large") == std::string::npos && phone.layout.find("front2160") == std::string::npos);
    }
}

void aLiveSessionReservesMemoryOnlyForCamerasItHas()
{
    using uml::camera::liveCameraReserveMiB;
    const auto tier = CameraTier::small;
    const auto ceiling = uml::camera::cameraHostMemoryReserveMiB(tier);
    const auto phone = present(
        uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), tier, "");
    require(ceiling == 197, "the most a session of the tier holds");
    // The reserve follows what the device is given: this phone's front camera does not run with its back one, and its
    // back lens teaches the store its colour data with its first still, so a RAW capture is possible from the first boot.
    require(phone.hasCameras() && phone.layout == "bf-small-flash-manual" && phone.use.streams == 1 && phone.use.rawStill,
        "the use the device is given");
    const auto reserve = liveCameraReserveMiB(CameraMode::live, phone.layout, phone.reserved, tier);
    require(reserve == uml::camera::cameraHostMemoryReserveMiB(tier, phone.reserved) && reserve > 0,
        "a live session holds what the device lets it hold");
    require(phone.use.stillWorkingBytesPerPixel == 11 && reserve > ceiling,
        "the synthetic source holds working storage that a real camera's ceiling does not count");
    require(liveCameraReserveMiB(CameraMode::blocked, phone.layout, phone.reserved, tier) == 0, "a blocked session opens none");
    // A live session that composes `none` (a host whose cameras the image cannot describe, or a source that is
    // not a name) has no device, so it holds nothing.
    const auto none = present(nullptr, tier, "");
    require(!none.hasCameras());
    require(liveCameraReserveMiB(CameraMode::live, none.layout, none.reserved, tier) == 0, "a live session without cameras");
    require(liveCameraReserveMiB(CameraMode::blocked, none.layout, none.reserved, tier) == 0);
}

// A phone's lenses have taught the store its colour data, in `directory`.
void teachTheStore(const std::string& directory)
{
    const auto phone = uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone);
    uml::camera::CalibrationStore store(directory);
    for (const auto& info : phone->cameras()) {
        if (!info.rawStills)
            continue;
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
        require(store.save(phone->hostModel(), uml::camera::CalibrationStore::lensKey(info), data));
    }
}

void thePlannedReserveIsTheComposedLayoutsFigure()
{
    using uml::camera::CameraUse;
    using uml::camera::cameraHostMemoryReserveMiB;
    const auto tier = CameraTier::small;
    const auto source = [](uml::camera::SyntheticProfile profile) { return uml::camera::makeSyntheticCameraSource(profile); };

    // The figure is the one a boot takes out of the pool (the reserve of the first composition's use), and it
    // is exact.
    for (const auto profile : {uml::camera::SyntheticProfile::phone, uml::camera::SyntheticProfile::laptop,
             uml::camera::SyntheticProfile::tablet}) {
        const auto run = present(source(profile), tier, "");
        const auto plan = planReserve(source(profile), tier, "");
        require(plan.exact && run.hasCameras(), "the host's cameras are discovered");
        require(plan.reserveMiB == liveCameraReserveMiB(CameraMode::live, run.layout, run.reserved, tier),
            "what the boot reserves");
        require(plan.reserveMiB == cameraHostMemoryReserveMiB(tier, run.reserved) && plan.reserveMiB > 0, run.layout);
    }
    // The synthetic phone's front camera does not run with its back one and its back lens can take a RAW still:
    // one stream, RAW, and the working storage its source states (11 B/px).
    const auto phone = planReserve(source(uml::camera::SyntheticProfile::phone), tier, "");
    require(phone.reserveMiB == cameraHostMemoryReserveMiB(tier, CameraUse {1, true, 11}), "the phone's use");
    // The laptop has one camera and no RAW: a lighter figure than the phone's.
    const auto laptop = planReserve(source(uml::camera::SyntheticProfile::laptop), tier, "");
    require(laptop.reserveMiB < phone.reserveMiB, "a laptop's one camera holds less than a phone");

    // A host with no camera holds nothing, and says so as exactly as any other.
    const auto none = planReserve(nullptr, tier, "");
    require(none.exact && none.reserveMiB == 0, "no camera, no reserve");

    // A host whose cameras could not be discovered holds the most a system camera's layout does, and the figure
    // says it is not exact.
    const auto ceiling = ceilingReserve(tier);
    require(!ceiling.exact && ceiling.reserveMiB == 197, "the tier's ceiling");
    require(none.reserveMiB < ceiling.reserveMiB);
}

void theColourDataARunTeachesMovesNoFigure()
{
    using uml::camera::CameraUse;
    using uml::camera::cameraHostMemoryReserveMiB;
    const auto tier = CameraTier::small;
    // A lens that has no colour data yet is counted for the RAW still that would teach it, and one that has it
    // for the RAW it offers: teaching the store moves no figure, so the sheet's does not change under a run.
    const auto phone = [] { return uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone); };
    const auto directory = std::filesystem::temp_directory_path() /
        ("kernarium-camera-reserve-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto before = planReserve(phone(), tier, directory.string());
    teachTheStore(directory.string());
    const auto learned = planReserve(phone(), tier, directory.string());
    require(before.exact && learned.exact, "both are the layout's");
    require(learned.reserveMiB == cameraHostMemoryReserveMiB(tier, CameraUse {1, true, 11}), "the learned use");
    require(before.reserveMiB == learned.reserveMiB, "RAW is counted before the first still teaches it");
    std::filesystem::remove_all(directory);

    // A path that is not a path states no directory.
    bool invalid = false;
    require(readCalibrationPath(nullptr, invalid).empty() && !invalid, "no path");
    require(readCalibrationPath("", invalid).empty() && invalid, "an empty one is invalid");
    invalid = false;
    require(readCalibrationPath("/calibration", invalid) == "/calibration" && !invalid);
}

void theLensCoverFollowsTheSession()
{
    using uml::camera::lensCoverAdvertised;
    const auto cameras = "bf-small-flash-manual";
    require(lensCoverAdvertised(CameraMode::blocked, cameras), "a blocked session with cameras");
    require(!lensCoverAdvertised(CameraMode::live, cameras), "a live one");
    require(!lensCoverAdvertised(CameraMode::blocked, "none"),
        "a host without cameras shows no blocked camera");
    require(!lensCoverAdvertised(CameraMode::live, "none"));
    require(lensCoverAdvertised(CameraMode::blocked, ""), "an unstated layout counts as having cameras");
    require(uml::camera::layoutHasCameras("front") && uml::camera::layoutHasCameras(""));
    require(!uml::camera::layoutHasCameras("none"));
}

void aLaterBootComposesTheLayoutAgain()
{
    const auto phone = uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone);
    auto presentation = present(phone, CameraTier::small, "");
    const auto before = presentation;
    require(presentation.layout == "bf-small-flash-manual", presentation.layout);
    recompose(presentation);
    require(presentation.layout == "bf-small-flash-manual", "nothing learned, nothing changed");

    // A Bayer still teaches the run's store a lens's colour data, and RAW is in
    // the layout only once the store has it: the next boot offers it.
    for (const auto& info : phone->cameras()) {
        if (!info.rawStills)
            continue;
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
        presentation.calibration->save(phone->hostModel(), uml::camera::CalibrationStore::lensKey(info), data);
    }
    require(presentation.layout == "bf-small-flash-manual", "the layout is composed when asked");
    recompose(presentation);
    require(presentation.layout == "bf-small-flash-manual-raw", presentation.layout);
    // The boot after the calibration is given the use the pool was sized for, and no more.
    require(presentation.use == before.use && presentation.reserved == before.reserved,
        "the colour data learned moves a lens from capturing RAW to offering it, and the use is the same");
    require(uml::camera::liveCameraReserveMiB(CameraMode::live, presentation.layout, presentation.reserved, CameraTier::small) ==
            uml::camera::liveCameraReserveMiB(CameraMode::live, before.layout, before.reserved, CameraTier::small),
        "so is the memory the pool gave up");
    const auto names = uml::camera::allLayoutNames();
    require(std::find(names.begin(), names.end(), presentation.layout) != names.end(),
        "a layout of the image's closed set");

    // A presentation with no cameras stays as it is.
    auto none = present(nullptr, CameraTier::small, "");
    recompose(none);
    require(none.layout == "none" && none.source == nullptr && !none.hasCameras());
}

// The cameras of a later boot never exceed what the pool was reduced for, whatever the host's cameras become, and a run that
// had none at its first discovery has none at a later boot.
void aLaterBootIsHeldToTheFirstDiscoverysReservation()
{
    using uml::camera::SyntheticProfile;
    auto presentation = present(uml::camera::makeSyntheticCameraSource(SyntheticProfile::phone), CameraTier::small, "");
    const auto before = presentation;
    require(presentation.hasCameras() && presentation.reservedCameras, "the first discovery's cameras are reserved for");
    // The host's cameras narrow to the laptop's: a layout within the reservation.
    presentation.source = uml::camera::makeSyntheticCameraSource(SyntheticProfile::laptop);
    recompose(presentation);
    require(presentation.layout == "front" && presentation.use.streams <= before.reserved.streams &&
                presentation.reserved == before.reserved,
            presentation.layout);
    require(!presentation.use.rawStill, "and no RAW capture");

    // A run whose first discovery had no cameras gets none later, whatever appears.
    auto none = present(nullptr, CameraTier::small, "");
    none.source = uml::camera::makeSyntheticCameraSource(SyntheticProfile::phone);
    recompose(none);
    require(none.layout == "none" && !none.hasCameras(), "none stays none");
    // A first discovery of a laptop reserved no RAW: a phone that shows up later is not told its back cameras.
    auto laptop = present(uml::camera::makeSyntheticCameraSource(SyntheticProfile::laptop), CameraTier::small, "");
    laptop.source = uml::camera::makeSyntheticCameraSource(SyntheticProfile::phone);
    recompose(laptop);
    require(!laptop.hasCameras() && laptop.reserved.streams == 1 && !laptop.reserved.rawStill,
            "a later host is held to the reservation");
    require(laptop.layout == "none", laptop.layout);
}

struct Reports {
    std::vector<int> states;
    static void callback(void* context, int state) { static_cast<Reports*>(context)->states.push_back(state); }
};

void theStateReceiverReportsChanges()
{
    StateReceiver receiver;
    Reports reports;
    require(receiver.current() == KERNARIUM_RUNTIME_CAMERA_STATE_IDLE);
    receiver.publish(CameraState::streaming);
    require(receiver.current() == KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING, "kept without a receiver");

    receiver.set(Reports::callback, &reports);
    require(reports.states == std::vector<int> {KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING},
        "a new receiver hears the state in force at once");
    receiver.publish(CameraState::streaming);
    require(reports.states.size() == 1, "the same state again is not a change");
    receiver.publish(CameraState::interruptedByMultitasking);
    receiver.publish(CameraState::streaming);
    receiver.publish(CameraState::idle);
    require((reports.states == std::vector<int> {
                KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING,
                KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING,
                KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING,
                KERNARIUM_RUNTIME_CAMERA_STATE_IDLE}),
        "in the order they happened");

    // A blocked guest's opens: reported at each open, idle between, and a guest that ends closes it.
    const auto beforeOpens = reports.states.size();
    receiver.publish(CameraState::blockedInUse);
    receiver.publish(CameraState::blockedInUse);
    receiver.publish(CameraState::idle);
    receiver.publish(CameraState::blockedInUse);
    require((std::vector<int>(reports.states.begin() + static_cast<std::ptrdiff_t>(beforeOpens), reports.states.end())
                == std::vector<int> {
                    KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE,
                    KERNARIUM_RUNTIME_CAMERA_STATE_IDLE,
                    KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE}),
        "each open is reported once, with idle between, and nothing else");
    receiver.ended();
    require(reports.states.back() == KERNARIUM_RUNTIME_CAMERA_STATE_IDLE && receiver.current() == 0,
        "a guest that ends with a camera open leaves none open");
    receiver.publish(CameraState::interrupted);
    receiver.ended();
    require(reports.states.back() == KERNARIUM_RUNTIME_CAMERA_STATE_IDLE && receiver.current() == 0,
        "a guest that ends leaves no camera open");
    const auto count = reports.states.size();
    receiver.ended();
    require(reports.states.size() == count, "and ending twice says it once");

    receiver.set(nullptr, nullptr);
    receiver.publish(CameraState::streaming);
    require(reports.states.size() == count, "a cleared receiver is not called");
    require(receiver.current() == KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING);
}

void theRotationTakesQuarterTurns()
{
    Rotation rotation;
    require(rotation.degrees() == 0);
    for (const std::uint32_t degrees : {90u, 180u, 270u, 0u}) {
        require(rotation.set(degrees));
        require(rotation.degrees() == degrees);
    }
    require(rotation.set(90));
    for (const std::uint32_t bad : {1u, 45u, 91u, 360u, 450u, 0xFFFFFFFFu})
        require(!rotation.set(bad) && rotation.degrees() == 90, "another value changes nothing");
}

uml::platform::CameraInfo lens(uml::platform::CameraPosition position, std::uint32_t sensorOrientation)
{
    uml::platform::CameraInfo info;
    info.position = position;
    info.sensorOrientation = sensorOrientation;
    return info;
}

void theDeviceOptionsCarryTheSessionsStatements()
{
    using uml::platform::CameraPosition;
    const auto presentation =
        present(uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), CameraTier::large, "");
    StateReceiver receiver;
    Rotation rotation;
    Reports reports;
    receiver.set(Reports::callback, &reports);
    const auto back = lens(CameraPosition::back, 90);
    const auto front = lens(CameraPosition::front, 270); // one valid mounting; an iPhone 15 Pro Max's front camera states 90

    // A phone, whose guest rotates itself, states no turn: the platform's values.
    const auto options = deviceOptions(presentation, CameraMode::blocked, receiver, rotation);
    require(options.mode == CameraMode::blocked && options.tier == CameraTier::large);
    require(options.calibration == presentation.calibration, "the one store of the run");
    require(options.automaticRotation && options.onState && options.sensorOrientation);
    require(options.sensorOrientation(back) == 90 && options.sensorOrientation(front) == 270);
    require(options.automaticRotation(back) == 0 && options.automaticRotation(front) == 0);
    options.onState(CameraState::interruptedByMultitasking);
    require(reports.states.back() == KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING);
    require(deviceOptions(presentation, CameraMode::live, receiver, rotation).mode == CameraMode::live);

    // The application's way to end the guest's video when it leaves the foreground is the device's, when the run has one.
    require(options.background == nullptr, "none is stated unless the run has one");
    const auto background = std::make_shared<uml::camera::BackgroundVideo>();
    require(deviceOptions(presentation, CameraMode::live, receiver, rotation, background).background == background);
}

void aBootStatesTheOrientationAndALaterTurnIsADelta()
{
    using uml::platform::CameraPosition;
    const auto presentation =
        present(uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::tablet), CameraTier::small, "");
    StateReceiver receiver;
    Rotation rotation;
    const auto back = lens(CameraPosition::back, 90);
    const auto front = lens(CameraPosition::front, 270); // a front camera stated 270; the next test is one stated 90
    const auto external = lens(CameraPosition::external, 0);

    // The iPad held in landscape (home button on the left, a quarter turn
    // clockwise) at Start: the guest is told that.
    require(rotation.set(90));
    const auto options = deviceOptions(presentation, CameraMode::live, receiver, rotation);
    require(options.sensorOrientation(back) == 180 && options.sensorOrientation(front) == 180);
    require(options.sensorOrientation(external) == 0, "an external camera does not turn with the body");
    require(options.automaticRotation(back) == 0 && options.automaticRotation(front) == 0,
        "nothing has changed since the boot");

    // Later turns reach the picture as the difference from the boot's.
    require(rotation.set(270));
    require(options.sensorOrientation(back) == 180, "the guest keeps the figure it was told");
    require(options.automaticRotation(back) == 180 && options.automaticRotation(front) == 180);
    require(rotation.set(180));
    require(options.automaticRotation(back) == 90 && options.automaticRotation(front) == 270);
    require(options.automaticRotation(external) == 0);
    require(rotation.set(0));
    require(options.automaticRotation(back) == 270 && options.automaticRotation(front) == 90);
    require(rotation.set(90));
    require(options.automaticRotation(back) == 0, "back to the boot's turn, no turn");

    // The next boot is told the turn then.
    require(rotation.set(270));
    const auto next = deviceOptions(presentation, CameraMode::live, receiver, rotation);
    require(next.sensorOrientation(back) == 0 && next.sensorOrientation(front) == 0);
    require(next.automaticRotation(back) == 0);
}

// An iPhone 15 Pro Max's back cameras state 90 (the platform's angle for portrait) and its front camera 270 (the platform's
// 90, turned half around by the host): through the runtime's callbacks, the wire description and the read-only control, a
// boot's turn reaches the guest as 90 + turn for a back camera and 270 - turn for a front camera, and the body's later turns
// as the difference from the boot's.
void aFrontCameraStated270FollowsTheBodyThroughTheRuntime()
{
    using uml::platform::CameraPosition;
    constexpr std::array<std::uint32_t, 4> turns {0, 90, 180, 270};
    constexpr std::array<std::uint32_t, 4> fronts {270, 180, 90, 0};
    constexpr std::array<std::uint32_t, 4> backs {90, 180, 270, 0};
    const auto presentation =
        present(uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::phone), CameraTier::large, "");
    StateReceiver receiver;
    std::vector<uml::platform::CameraInfo> infos;
    for (const auto position : {CameraPosition::front, CameraPosition::back, CameraPosition::external}) {
        uml::platform::CameraInfo info;
        info.position = position;
        info.identifier = std::to_string(infos.size());
        info.lens = position == CameraPosition::external ? uml::platform::CameraLensKind::external
                                                         : uml::platform::CameraLensKind::wide;
        info.sensorOrientation = uml::platform::presentedMountingOf(position, 90).stated;
        info.videoFormats = {{640, 480, 1, 30, false}};
        infos.push_back(info);
    }
    const auto expected = [&](CameraPosition position, std::size_t turn) {
        return position == CameraPosition::front ? fronts[turn] : position == CameraPosition::back ? backs[turn] : 90u;
    };
    Rotation rotation;
    for (std::size_t boot = 0; boot < turns.size(); ++boot) {
        require(rotation.set(turns[boot]));
        const auto options = deviceOptions(presentation, CameraMode::blocked, receiver, rotation);
        uml::camera::ArrangementOptions arrangement;
        arrangement.sensorOrientation = options.sensorOrientation;
        const auto cameras = uml::camera::arrangeCameras(infos, {}, "model", uml::camera::CalibrationStore(), arrangement);
        require(cameras.size() == infos.size(), "every camera is on the wire");
        for (const auto& camera : cameras) {
            const auto position = camera.info.position;
            const auto stated = expected(position, boot);
            require(camera.sensorOrientation == stated && camera.wire.sensor_orientation == stated,
                "the description and the wire state the boot's orientation");
            const uml::camera::CameraControls controls(camera.info, camera.sensorOrientation, true);
            require(controls.defaults().scalars.at(UML_V4L2_CID_CAMERA_SENSOR_ROTATION) == stated,
                "the read-only control states it too");
            for (std::size_t now = 0; now < turns.size(); ++now) {
                require(rotation.set(turns[now]));
                require(options.sensorOrientation(camera.info) == stated, "the guest keeps the figure it was told");
                require((stated + options.automaticRotation(camera.info)) % 360 == expected(position, now),
                    "the boot's figure and the picture's turn are the orientation now");
            }
        }
    }
}

void theEnvironmentNamesTheSyntheticCamera()
{
    require(std::string {sourceVariable} == "KERNARIUM_CAMERA_SOURCE");
    for (const char* host : {static_cast<const char*>(nullptr), "host"}) {
        const auto choice = parseSource(host);
        require(!choice.synthetic && !choice.invalid && !choice.backOnly);
    }
    const auto back = parseSource("host:back");
    require(back.backOnly && !back.synthetic && !back.invalid, "the host's back cameras alone");
    for (const char* bad : {"host:front", "host:Back", "host:back ", "host:", "back"})
        require(parseSource(bad).invalid && !parseSource(bad).backOnly, bad);
    using uml::camera::SyntheticProfile;
    struct Case {
        const char* value;
        SyntheticProfile profile;
    };
    for (const Case& named : {Case {"synthetic", SyntheticProfile::phone}, Case {"synthetic:phone", SyntheticProfile::phone},
                              Case {"synthetic:tablet", SyntheticProfile::tablet},
                              Case {"synthetic:laptop", SyntheticProfile::laptop}}) {
        const auto choice = parseSource(named.value);
        require(choice.synthetic && !choice.invalid && choice.profile == named.profile, named.value);
    }
    for (const char* bad : {"synthetic:watch", "webcam", "Synthetic", "synthetic:", ""}) {
        const auto choice = parseSource(bad);
        require(!choice.synthetic && choice.invalid, bad);
    }
}

void anUnknownSourceNeverFallsBackToTheHostsCamera()
{
    // The host's source is made only for a value that names it: a test knob
    // that cannot be read must never open a real camera.
    int hostSources = 0;
    const auto host = [&hostSources]() -> uml::camera::CameraSourceHandle {
        ++hostSources;
        return uml::camera::makeSyntheticCameraSource(uml::camera::SyntheticProfile::laptop);
    };
    // A typo of the back-only name is not the host's cameras either.
    for (const char* bad : {"synthetic:watch", "webcam", "Synthetic", "synthetic:", "SYNTHETIC:PHONE", " host",
                            "host ", "synthetic:phone:2", "0", "", "host:bak", "host:backs", "host:back:", "host:front", "Host:back",
                            "host:Back", "host:", "host:back "}) {
        require(parseSource(bad).invalid, bad);
        require(chooseSource(parseSource(bad), host) == nullptr, bad);
    }
    require(hostSources == 0, "an unknown value asked for the host's camera");

    // An unknown value presents a host without cameras, and a camera never
    // fails a boot.
    const auto none = present(chooseSource(parseSource("webcam"), host), CameraTier::small, "");
    require(none.layout == "none" && !none.hasCameras() && none.source == nullptr);
    require(!uml::camera::lensCoverAdvertised(CameraMode::blocked, none.layout));

    for (const char* named : {static_cast<const char*>(nullptr), "host"}) {
        require(chooseSource(parseSource(named), host) != nullptr);
    }
    require(hostSources == 2, "the host's own value asks for the host's camera");
    require(chooseSource(parseSource("synthetic:tablet"), host) != nullptr && hostSources == 2,
        "the synthetic camera is not the host's");
}

// A host with a back camera, a front camera and a second back lens. It records
// what its sessions are asked, by camera.
class RecordingHost final : public uml::platform::Camera {
public:
    std::vector<std::string> calls;
    uml::platform::CameraSessionDelegate* delegate {};

    [[nodiscard]] std::vector<uml::platform::CameraInfo> cameras() const override
    {
        using uml::platform::CameraPosition;
        std::vector<uml::platform::CameraInfo> infos(3);
        infos[0].identifier = "back";
        infos[1].identifier = "front";
        infos[1].position = CameraPosition::front;
        infos[2].identifier = "back-tele";
        return infos;
    }
    [[nodiscard]] std::vector<std::vector<std::size_t>> concurrentSets() const override { return {{0, 1}}; }
    [[nodiscard]] std::string hostModel() const override { return "RecordingHost"; }
    [[nodiscard]] std::uint64_t now() const noexcept override { return 7; }
    [[nodiscard]] std::unique_ptr<uml::platform::Nv12Transformer> makeFrameTransformer() const override { return nullptr; }
    [[nodiscard]] std::unique_ptr<uml::platform::CameraSession> openSession(
        uml::platform::CameraSessionDelegate& to) override
    {
        delegate = &to;
        return std::make_unique<Session>(calls);
    }

private:
    class Session final : public uml::platform::CameraSession {
    public:
        explicit Session(std::vector<std::string>& calls) : calls_(calls) {}
        [[nodiscard]] bool startVideo(std::size_t camera, std::uint32_t, std::uint32_t, std::uint32_t) override
        {
            calls_.push_back("start " + std::to_string(camera));
            return true;
        }
        void stopVideo(std::size_t camera) override { calls_.push_back("stop " + std::to_string(camera)); }
        void apply(std::size_t camera, const uml::platform::CameraSettings&) override
        {
            calls_.push_back("apply " + std::to_string(camera));
        }
        [[nodiscard]] bool takeStill(std::size_t camera, const uml::platform::CameraStillRequest&) override
        {
            calls_.push_back("still " + std::to_string(camera));
            return true;
        }
        void setAnchors(std::span<const std::optional<std::size_t>> formatOfCamera,
                        const uml::platform::CameraLimits& limits) override
        {
            std::string anchors;
            for (const auto& anchor : formatOfCamera) anchors += (anchors.empty() ? "" : ",") + (anchor ? std::to_string(*anchor) : std::string("-"));
            calls_.push_back("anchors " + anchors + " limits " + std::to_string(limits.maximumStillPixels) + " " +
                             std::to_string(limits.maximumFrameBytes));
        }
        void stop() override { calls_.push_back("stop all"); }

    private:
        std::vector<std::string>& calls_;
    };
};

class NullDelegate final : public uml::platform::CameraSessionDelegate {
public:
    void cameraFrame(std::size_t, uml::platform::CameraFrame) override {}
    void cameraStill(std::size_t, uml::platform::CameraStillResult) override {}
    void cameraEvent(uml::platform::CameraEvent) override {}
};

void theBackKnobNeverReachesTheFrontCamera()
{
    auto recording = std::make_shared<RecordingHost>();
    const auto source = chooseSource(parseSource("host:back"), [&]() -> uml::camera::CameraSourceHandle { return recording; });
    require(source != nullptr && source != recording, "the host's cameras behind the filter");
    // The cameras are listed as the host has them, so the guest's layout is an image's.
    require(source->cameras().size() == 3 && source->cameras()[1].position == uml::platform::CameraPosition::front);
    require(source->concurrentSets() == recording->concurrentSets() && source->hostModel() == "RecordingHost" && source->now() == 7);

    NullDelegate delegate;
    auto session = source->openSession(delegate);
    require(session != nullptr && recording->delegate == &delegate, "the host's session reports to the caller's delegate");
    const std::optional<std::size_t> anchors[] {2, std::nullopt, 0};
    session->setAnchors(anchors, {12'700'000, 6'220'800});
    require(session->startVideo(0, 1280, 720, 30) && session->startVideo(2, 640, 480, 30), "the back lenses start");
    session->apply(0, {});
    require(session->takeStill(2, {}));
    session->stopVideo(2);
    // Anything that is not a back camera, or no camera, is refused and never asked of the host.
    require(!session->startVideo(1, 1280, 720, 30), "the front camera does not start");
    require(!session->takeStill(1, {}), "nor does it take a still");
    session->apply(1, {});
    session->stopVideo(1);
    require(!session->startVideo(3, 1280, 720, 30) && !session->takeStill(99, {}), "an index past the cameras");
    session->stop();
    const std::vector<std::string> expected {"anchors 2,-,0 limits 12700000 6220800", "start 0", "start 2", "apply 0", "still 2", "stop 2", "stop all"};
    require(recording->calls == expected, "the host saw the anchors and limits and the back cameras' calls and no other");

    // The same cameras give the same layout with or without the filter.
    int hosts = 0;
    const auto plain = present(
        chooseSource(parseSource("host"), [&]() -> uml::camera::CameraSourceHandle { ++hosts; return recording; }),
        CameraTier::small, "");
    const auto filtered = present(source, CameraTier::small, "");
    require(hosts == 1 && plain.layout == filtered.layout, "the layout is the host's");

    // A host that is not there stays none.
    require(chooseSource(parseSource("host:back"), []() -> uml::camera::CameraSourceHandle { return nullptr; }) == nullptr);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"a-request-that-states-nothing-is-blocked", aRequestThatStatesNothingIsBlocked},
        {"the-request-states-the-mode-and-the-path", theRequestStatesTheModeAndThePath},
        {"a-camera-never-fails-a-boot", aCameraNeverFailsABoot},
        {"the-enums-are-the-applications-words", theEnumsAreTheApplicationsWords},
        {"a-host-without-cameras-presents-none", aHostWithoutCamerasPresentsNone},
        {"the-layout-follows-the-cameras-and-the-tier", theLayoutFollowsTheCamerasAndTheTier},
        {"every-host-composes-the-small-tier-for-now", everyHostComposesTheSmallTierForNow},
        {"a-live-session-reserves-memory-only-for-cameras-it-has", aLiveSessionReservesMemoryOnlyForCamerasItHas},
        {"the-planned-reserve-is-the-composed-layouts-figure", thePlannedReserveIsTheComposedLayoutsFigure},
        {"the-colour-data-a-run-teaches-moves-no-figure", theColourDataARunTeachesMovesNoFigure},
        {"the-lens-cover-follows-the-session", theLensCoverFollowsTheSession},
        {"a-later-boot-composes-the-layout-again", aLaterBootComposesTheLayoutAgain},
        {"a-later-boot-is-held-to-the-first-discoverys-reservation", aLaterBootIsHeldToTheFirstDiscoverysReservation},
        {"the-state-receiver-reports-changes", theStateReceiverReportsChanges},
        {"the-rotation-takes-quarter-turns", theRotationTakesQuarterTurns},
        {"the-device-options-carry-the-sessions-statements", theDeviceOptionsCarryTheSessionsStatements},
        {"a-boot-states-the-orientation-and-a-later-turn-is-a-delta", aBootStatesTheOrientationAndALaterTurnIsADelta},
        {"a-front-camera-stated-270-follows-the-body-through-the-runtime", aFrontCameraStated270FollowsTheBodyThroughTheRuntime},
        {"the-environment-names-the-synthetic-camera", theEnvironmentNamesTheSyntheticCamera},
        {"an-unknown-source-never-falls-back-to-the-hosts-camera", anUnknownSourceNeverFallsBackToTheHostsCamera},
        {"the-back-knob-never-reaches-the-front-camera", theBackKnobNeverReachesTheFrontCamera},
    };
    return uml::testing::runNamedTests(tests);
}
