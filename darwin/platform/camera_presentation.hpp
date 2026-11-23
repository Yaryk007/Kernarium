// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// How a platform camera presented itself to the guest, as numbers and fixed
// words. The platform camera fills what it stated and set for one camera
// (CameraSessionDelegate::cameraPresentation), the camera device gives it the
// guest's index and tells its owner (CameraDeviceOptions::onCameraPresentation),
// and the runtime's session record keeps the newest of each camera in its camera
// slot (runtime/session_record.hpp). Nothing in it is free text: a name, a path
// or a message of the framework never enters it.

#include "camera_still_failure.hpp"
#include "video_stabilization.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace uml::platform {

// The dynamic aspect ratios a capture format may list, which a device holds at
// one of (iOS 26). A square sensor's formats list them; every other format lists
// none.
enum class CameraAspectRatio : std::uint8_t { none, r1x1, r16x9, r9x16, r4x3, r3x4 };

// The bit of a ratio in a mask of the ratios a format lists; none has none.
[[nodiscard]] constexpr std::uint8_t aspectRatioBit(CameraAspectRatio ratio) noexcept
{
    return ratio == CameraAspectRatio::none ? std::uint8_t {0} : std::uint8_t(1u << (std::uint8_t(ratio) - 1));
}

[[nodiscard]] constexpr std::string_view aspectRatioName(CameraAspectRatio ratio) noexcept
{
    switch (ratio) {
    case CameraAspectRatio::none: break;
    case CameraAspectRatio::r1x1: return "1x1";
    case CameraAspectRatio::r16x9: return "16x9";
    case CameraAspectRatio::r9x16: return "9x16";
    case CameraAspectRatio::r4x3: return "4x3";
    case CameraAspectRatio::r3x4: return "3x4";
    }
    return "-";
}

// The kind of an AVFoundation device, as the code the record carries.
enum class CameraDeviceType : std::uint8_t {
    other,
    wide,
    ultraWide,
    telephoto,
    dual,
    dualWide,
    triple,
    external,
    outerUltraWide,
    innerUltraWide,
};

// What the platform stated of one camera and what it set on its connections,
// from discovery on and at each admission, repair and photo. A field a camera
// has not reached is at its default (a size of zero, an angle or mirroring of
// -1, a fact `unknown`): the session record writes `-` for it.
struct CameraPresentation {
    // The camera's index as the guest sees it (the device fills it in).
    std::uint32_t camera {};
    CameraPosition position {CameraPosition::back};

    // The camera as discovered: its device type, the rotation coordinator's
    // fixed clockwise angles for the four interface orientations (portrait,
    // portrait upside down, landscape right, landscape left), the orientation
    // the platform states for it, the turn set on the video connection, the
    // half turn the host applies to every picture of the camera on top of it
    // (0 or 180, CameraInfo::presentationTurn) and whether the camera imitates
    // an earlier front camera (presentedMountingOf).
    // The angles are -1 where the platform has no coordinator. The device that
    // tells the guest sets `stated` to the orientation of the guest's own
    // description (which the application may have turned for the layout it
    // boots in) and keeps the platform's in `platformStated`.
    CameraDeviceType deviceType {};
    std::array<std::int16_t, 4> coordinatorAngles {-1, -1, -1, -1};
    std::uint16_t stated {};
    std::uint16_t platformStated {};
    std::uint16_t turn {};
    std::uint16_t presentationTurn {};
    bool earlier {};
    // The camera's anchor (the one format it runs in): its size, the ratios it
    // lists (a mask of aspectRatioBit) and its field of view in degrees,
    // rounded; `anchorWidth` is zero for a camera that has none.
    std::uint32_t anchorWidth {};
    std::uint32_t anchorHeight {};
    std::uint8_t anchorRatios {};
    std::int16_t anchorFieldOfView {-1};
    // The front devices the platform listed (the guest's or not) and whether any
    // of them has a square format or a format that lists dynamic ratios.
    std::uint8_t frontDevices {};
    bool squareFormats {};
    bool dynamicRatios {};
    // How many formats of the front devices were not offered, because they list dynamic aspect ratios and cannot be
    // described (presentedFormatOf). The same for every camera: a front device whose formats are all dropped is no
    // camera, and these facts are what is left of it.
    std::uint16_t frontFormatsDropped {};

    // How many refusals of the camera's admissions the session logged and the
    // reason of the last (the platform's own numbering: photo_dimensions.hpp,
    // Refusal); -1 for none.
    std::uint16_t refusals {};
    std::int8_t lastRefusal {-1};

    // The device's dynamic aspect ratio, where its anchor lists ratios: the one
    // pinned, the one the device reports afterwards, the output dimensions it
    // reports, and the error code of the completion of the change (0 for none,
    // `ratioPending` while it is awaited).
    bool ratioKnown {};
    CameraAspectRatio ratioPinned {};
    CameraAspectRatio ratioNow {};
    std::uint32_t ratioWidth {};
    std::uint32_t ratioHeight {};
    std::int32_t ratioError {};

    // The video connection of the camera's last admission or repair: the angle
    // and the mirroring it had by default and now, whether the angle asked for
    // is supported, the size the output was asked for and the size of the first
    // frame it delivered.
    bool videoKnown {};
    std::int16_t videoDefaultAngle {-1};
    std::int16_t videoAngle {-1};
    CameraFact videoAngleSupported {};
    std::int8_t videoDefaultMirrored {-1};
    std::int8_t videoMirrored {-1};
    std::uint32_t videoAskedWidth {};
    std::uint32_t videoAskedHeight {};
    std::uint32_t videoFirstWidth {};
    std::uint32_t videoFirstHeight {};
    // What the connection's stabilization is, once an apply has written or kept it (`unknown` until then), and
    // `unsupported` when the guest wants it and the active format supports no mode.
    VideoStabilizationState videoStabilization {};

    // The photo output of the camera's last admission or repair: whether it
    // supports turning its stills to an earlier model's orientation and whether
    // that is on, the photo connection's angle and mirroring by default and now,
    // the largest photo the output makes, the Exif orientation of the last JPEG
    // the host's output gave (0 for none yet), and whether Center Stage is
    // active and the device corrects geometric distortion.
    bool photoKnown {};
    CameraFact compensationSupported {};
    CameraFact compensationEnabled {};
    CameraReading compensationReading {};
    std::int16_t photoDefaultAngle {-1};
    std::int16_t photoAngle {-1};
    std::int8_t photoDefaultMirrored {-1};
    std::int8_t photoMirrored {-1};
    std::uint32_t photoMaximumWidth {};
    std::uint32_t photoMaximumHeight {};
    std::uint8_t exifOrientation {};
    CameraFact centerStageActive {};
    CameraFact distortionCorrected {};

    friend bool operator==(const CameraPresentation&, const CameraPresentation&) = default;
};

// The error code of a ratio change whose completion has not come yet (the
// record writes `pending`).
inline constexpr std::int32_t ratioPending = std::numeric_limits<std::int32_t>::min();

// A camera's note when its platform session opens again (a session that closed
// when the cameras were idle): the facts of its discovery are the new session's,
// and what an earlier session's admissions set, the refusals counted and the
// last photo's Exif orientation stay, as the record of the camera's last use.
[[nodiscard]] inline CameraPresentation withDiscovery(CameraPresentation kept, const CameraPresentation& discovered)
{
    kept.position = discovered.position;
    kept.deviceType = discovered.deviceType;
    kept.coordinatorAngles = discovered.coordinatorAngles;
    kept.stated = discovered.stated;
    kept.platformStated = discovered.platformStated;
    kept.turn = discovered.turn;
    kept.presentationTurn = discovered.presentationTurn;
    kept.earlier = discovered.earlier;
    kept.anchorWidth = discovered.anchorWidth;
    kept.anchorHeight = discovered.anchorHeight;
    kept.anchorRatios = discovered.anchorRatios;
    kept.anchorFieldOfView = discovered.anchorFieldOfView;
    kept.frontDevices = discovered.frontDevices;
    kept.squareFormats = discovered.squareFormats;
    kept.dynamicRatios = discovered.dynamicRatios;
    kept.frontFormatsDropped = discovered.frontFormatsDropped;
    return kept;
}

} // namespace uml::platform
