// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera_description.hpp"
#include "camera_device.hpp"
#include "camera_tiers.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::camera {

/* The camera layouts of the Android image. One image serves every host: the
 * host discovers its cameras and passes the layout that describes them as
 * androidboot.product.vendor.sku, a closed set of 39 names whose permission
 * directories and media profiles the image carries
 * (android/lineage-24/README.md, "Camera"). android/lineage-24/tools/camera_layouts.py
 * is the only source of the set; this file composes a name from a host's
 * cameras the way that file says, and a test holds the two together
 * (camera-layout reads overlay/camera/generated/layouts.tsv).
 *
 * The dimensions are the host's cameras (none, the front camera alone, or back
 * and front), and for back and front the memory tier, the back flash, manual
 * sensor control, RAW (only with manual control), the concurrent streaming of
 * the front and the back camera, and for the large tier the front camera's
 * video at 2160p. A front camera alone has manual control or not. */
enum class LayoutCameras : std::uint8_t { none, front, backAndFront };

struct LayoutDimensions {
    LayoutCameras cameras {LayoutCameras::none};
    /* Only back and front have a tier. */
    CameraTier tier {CameraTier::small};
    bool flash {};
    bool manual {};
    bool raw {};
    bool concurrent {};
    /* Only the large tier has it. */
    bool front2160 {};

    friend bool operator==(const LayoutDimensions&, const LayoutDimensions&) = default;
};

/* The dimensions with what the set cannot say taken away: no tier, flash,
 * RAW, concurrency or 2160p without back cameras; no RAW without manual
 * control; no 2160p in the small tier. */
[[nodiscard]] LayoutDimensions normalizedLayout(LayoutDimensions dimensions) noexcept;

/* The name of the layout of `dimensions` (normalized first). */
[[nodiscard]] std::string layoutName(const LayoutDimensions& dimensions);

/* Every layout, in the order of the image's table. */
[[nodiscard]] std::vector<LayoutDimensions> allLayouts();
[[nodiscard]] std::vector<std::string> allLayoutNames();

/* The layout and then, nearest first, each layout it falls back to when an
 * image lacks it: dropping concurrent streaming, then RAW, then the front
 * camera's 2160p, then the flash, then the large tier, then manual control,
 * then the back cameras ("front"), then every camera ("none"). */
[[nodiscard]] std::vector<std::string> layoutFallbackChain(const LayoutDimensions& dimensions);

/* The first layout of the chain of `wanted` that `available` accepts; "none"
 * when it accepts none. */
[[nodiscard]] std::string chooseLayout(
    const LayoutDimensions& wanted, const std::function<bool(std::string_view)>& available);

/* Whether the camera meets the sizes the camera API guarantees a camera that
 * streams beside another, as the Android camera HAL derives them from the
 * camera's video modes and stills (camera_plan.hpp derive_sizes and
 * meets_concurrent_streams): a 1280x720 size when its largest size reaches that,
 * and 1920x1440 with it when its largest reaches 1920x1440, for the YUV list and
 * for the JPEG list. A size is cut from a video mode that covers it. */
[[nodiscard]] bool meetsConcurrentStreams(const CameraDescription& camera) noexcept;

/* The kernel argument the layout travels in; Android's init imports it as
 * ro.boot.product.vendor.sku. */
inline constexpr std::string_view layoutArgumentPrefix = "androidboot.product.vendor.sku=";

/* The layout of a host with no cameras the image can describe. */
inline constexpr std::string_view noCameraLayout = "none";

/* Whether a guest with this layout gets a camera device at all. A layout that
 * is not stated counts as having cameras. */
[[nodiscard]] constexpr bool layoutHasCameras(std::string_view layout) noexcept
{
    return layout != noCameraLayout;
}

/* Whether the side keys advertise Android's camera lens cover
 * (SW_CAMERA_LENS_COVER) and report it set: a blocked session whose guest has
 * cameras. A live session leaves the switch out, and a host with no cameras
 * reports no cover, so the guest shows no blocked camera it does not have. The
 * launcher and the application's runtime both decide it here. */
[[nodiscard]] constexpr bool lensCoverAdvertised(CameraMode mode, std::string_view layout) noexcept
{
    return mode == CameraMode::blocked && layoutHasCameras(layout);
}

/* The cameras a layout lets the device run at once (`CameraUse::streams`): two
 * only for back and front cameras with the concurrent feature, one otherwise.
 * The device enforces it; flash, manual control, RAW and the front camera's
 * 2160p say nothing of how many cameras run. */
[[nodiscard]] inline std::uint32_t layoutStreams(const LayoutDimensions& dimensions) noexcept
{
    const auto layout = normalizedLayout(dimensions);
    return layout.cameras == LayoutCameras::backAndFront && layout.concurrent ? 2u : 1u;
}

/* The dimensions of a layout of the image's set by its name; nothing for a name
 * that is not in it. */
[[nodiscard]] std::optional<LayoutDimensions> layoutDimensionsNamed(std::string_view name);

/* The host memory, in whole mebibytes, that a session's cameras hold outside
 * the GPU's pool (`cameraHostMemoryReserveMiB`): only a live session whose
 * layout has cameras holds any, and it holds what `use` lets the device hold. A
 * blocked session opens no capture session, and a host with no cameras gets no
 * device. `use` is the one the device is given (`CameraDeviceOptions::use`),
 * from `composeCameras`; a layout stated by name, which the device does not
 * enforce, is given the default `CameraUse`, the tier's ceiling. The launcher
 * and the application's runtime both decide it here, once the cameras are
 * composed. */
[[nodiscard]] std::uint64_t liveCameraReserveMiB(
    CameraMode mode, std::string_view layout, const CameraUse& use, CameraTier tier);

/* The dimensions a host's cameras (in wire order, `arrangeCameras`) earn at a
 * tier. A host with a back camera and no front one, or with no camera that is
 * front or back, has none: the set has no layout that says so, and a front
 * camera that is not there would be declared falsely. */
[[nodiscard]] LayoutDimensions layoutDimensionsOf(std::span<const CameraDescription> cameras, CameraTier tier) noexcept;

/* What the device of a host's cameras is given: the layout the guest is told,
 * and what the device lets the host hold (`CameraUse`), which the reserve
 * counts. */
struct ComposedCameras {
    std::string layout;
    CameraUse use;
};

/* The layout of a platform camera's host, composed as the camera device will
 * present its cameras (`arrangeCameras` with `options` and what `calibration`
 * has learned), from the layouts `available` accepts (all of them when it is
 * empty). Discovery only: it opens no session and asks the system for nothing.
 * It is the one place a host's layout is composed, so the application's runtime
 * and the launcher agree with the device. */
[[nodiscard]] std::string composeLayout(
    const uml::platform::Camera& host, const CalibrationStore& calibration, const ArrangementOptions& options,
    const std::function<bool(std::string_view)>& available = {});

/* The layout (`composeLayout`) and the use of the cameras the device will
 * present. The device runs at most the layout's cameras at once. A RAW capture
 * is possible wherever a lens the guest sees has RAW stills and either offers
 * them or has not taught the store its colour data (its first still takes
 * them): that is the most any boot of a run reaches, because colour data learned
 * later only moves a lens from the second case to the first, so the use of a
 * later boot never exceeds this one's. */
[[nodiscard]] ComposedCameras composeCameras(
    const uml::platform::Camera& host, const CalibrationStore& calibration, const ArrangementOptions& options,
    const std::function<bool(std::string_view)>& available = {});

/* What a run paid for at its first discovery: the use the GPU's pool was reduced
 * for, or no cameras at all (no reduction). A later boot is held to it. */
struct CameraReservation {
    bool cameras {};
    CameraUse use;
};

[[nodiscard]] inline CameraReservation reservationOf(const ComposedCameras& composed) noexcept
{
    return {layoutHasCameras(composed.layout), composed.use};
}

/* The cameras of a later boot of a run (`composeCameras`), held to `reserved`:
 * the layout the guest is told is one whose use fits the reservation (no more
 * cameras at once, no RAW capture or source working storage it did not count),
 * the widest of the layouts that fit if the first choice does not, and none
 * where none fits or the first discovery had no cameras. The host's cameras that
 * change under a running application therefore never reach the device with a
 * use the pool was not reduced for. */
[[nodiscard]] ComposedCameras composeCamerasWithin(
    const uml::platform::Camera& host, const CalibrationStore& calibration, const ArrangementOptions& options,
    const CameraReservation& reserved);

} // namespace uml::camera
