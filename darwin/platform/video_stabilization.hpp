// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Which video stabilization the camera session asks the host for when the guest
// wants the picture stabilized (CameraSettings::stabilization). A pure function:
// the Apple backend (apple/camera_capture.mm) turns the answer into the
// capture connection's preferred mode, and the tests call the same code.
//
// The guest's preview and its recording share one capture output, so the
// stabilization's latency is the viewfinder's. The choice is therefore never the
// system's automatic one, which on an iPhone picks a cinematic mode that holds
// a second or more of frames before the first one is delivered. It is the
// low-latency mode where the format supports it, else the standard one (which
// adds a little latency and crops the view), else none.
//
// The guest asks for it in video mode (Android's record template), so the
// connection is stabilized from its first apply, whatever the other settings
// say, and a later apply of the same wish changes nothing: the view does not
// move when the guest first changes a setting.

#include <cstdint>
#include <string_view>

namespace uml::platform {

enum class VideoStabilization {
    off,
    standard,
    lowLatency,
};

// What the active capture format of a camera supports.
struct VideoStabilizationSupport {
    bool standard {};
    bool lowLatency {};
};

[[nodiscard]] VideoStabilization chooseVideoStabilization(bool wanted, VideoStabilizationSupport support) noexcept;

// What the session record says of a camera's stabilization.
enum class VideoStabilizationState : std::uint8_t {
    // No video connection has been stabilized or left alone yet.
    unknown,
    // The guest left the picture unstabilized.
    notWanted,
    standard,
    lowLatency,
    // The guest wants the picture stabilized and the active format supports no mode: it stays unstabilized.
    unsupported,
};

[[nodiscard]] constexpr std::string_view stabilizationName(VideoStabilizationState state) noexcept
{
    switch (state) {
    case VideoStabilizationState::unknown: break;
    case VideoStabilizationState::notWanted: return "off";
    case VideoStabilizationState::standard: return "standard";
    case VideoStabilizationState::lowLatency: return "low-latency";
    case VideoStabilizationState::unsupported: return "unsupported";
    }
    return "-";
}

// What a capture connection holds of the stabilization, as the backend wrote it. A connection nothing was written to
// holds the system's default, which is none, so a new connection starts at `off`.
struct VideoStabilizationHeld {
    VideoStabilization mode {VideoStabilization::off};
    VideoStabilizationState state {VideoStabilizationState::unknown};
};

// What an apply does about a connection's stabilization.
struct VideoStabilizationStep {
    VideoStabilization choice {};
    // The connection's preferred mode is set to `choice`: it differs from what the connection holds.
    bool write {};
    VideoStabilizationState state {};
    // The record's line for the camera changes to `state`.
    bool note {};
    // The guest wants the picture stabilized and the format supports no mode, newly: said once for the connection.
    bool unsupported {};
};

[[nodiscard]] VideoStabilizationStep stabilizationStep(bool wanted, VideoStabilizationSupport support,
                                                       const VideoStabilizationHeld& held) noexcept;

// The capture connection a stabilization is written to: what its active format supports, and its preferred mode.
class VideoStabilizationPort {
public:
    virtual ~VideoStabilizationPort() = default;
    [[nodiscard]] virtual VideoStabilizationSupport support() = 0;
    virtual void write(VideoStabilization mode) = 0;
};

// Puts a connection's stabilization where the guest wants it: writes the preferred mode when it differs from what the
// connection holds (`held`, updated), and returns the step for the record. Every apply of the camera's settings calls it
// first, and so does the admission of a video connection, before the session's configuration is committed.
VideoStabilizationStep applyVideoStabilization(VideoStabilizationPort& port, bool wanted, VideoStabilizationHeld& held);

// What the guest last asked of a camera's stabilization, as every apply of the camera's settings says it: also an apply
// that leaves the other settings alone because they are the ones the platform holds (a guest that asks for the defaults,
// as video mode does). The admission of a new video connection programs the wish before the connection can deliver; where
// no apply has been made it asks nothing, and the first apply does.
struct StabilizationWish {
    bool known {};
    bool asked {};
    void note(bool wanted) noexcept
    {
        known = true;
        asked = wanted;
    }
    [[nodiscard]] bool atAdmission() const noexcept { return known && asked; }
};

} // namespace uml::platform
