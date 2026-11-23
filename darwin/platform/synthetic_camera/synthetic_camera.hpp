// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A camera with no hardware. It implements darwin/platform/camera.hpp for unit
// tests and for the launchers that run a guest against a deterministic chart.
// It is never a real camera, on any device.
//
// The chart. Every frame is an NV12 picture of a luma gradient with a 6 x 4
// grid of colour patches and, across the middle of the picture, a row of 64
// marker cells (the picture's width shared among them, and 8 rows high) that
// carry, most significant bit first, a 16-bit tag naming the camera, a 32-bit
// frame counter and a 16-bit check word (the exclusive or of the three 16-bit
// groups before it). A cell is luma 235 for a one and 16 for a zero.
// decodeChartMarker() reads them back from a picture of any size: the marker
// spans the width and lies on the middle row, so the centred crop and the scale
// a device makes of an anchored camera's frame for a stream of another shape or
// size (the rows of a 16:9 stream of a 4:3 picture, a stream half its size)
// leave it readable.

#include "camera.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uml::platform {

enum class SyntheticCameraProfile : std::uint8_t {
    // A phone: wide, ultra wide and telephoto lenses and a front camera, and no
    // zoom camera. Only the wide lens runs with the front camera. The wide lens
    // has 48 MP stills and Bayer stills.
    phone,
    // A tablet: a back wide camera and a front camera. The back camera lists the
    // capture formats an iPad mini's does (CameraInfo::captureFormats), so the
    // device gives it an anchor and it runs only in it; the front camera lists
    // none.
    tablet,
    // An iPad's: a back wide camera and two front cameras (wide and ultra wide,
    // one sensor behind both), each of which can run with the back camera. The
    // back camera also has a size a multi-camera session does not take.
    tabletDualFront,
    // A laptop: a front camera and nothing else.
    laptop,
};

struct SyntheticCameraOptions {
    SyntheticCameraProfile profile {SyntheticCameraProfile::tablet};
    // Frames are produced by SyntheticCamera::pump() instead of by a timer
    // thread, and the clock is the one the test sets.
    bool manualClock {};
    // Added between the start of an exposure and its delivery.
    std::uint32_t latencyMilliseconds {};
    // The system has granted access. A session of a host that has not is null.
    bool authorized {true};
    // Every front camera that states 270 is a platform camera mounted at 90 that the host turns half around, as an
    // iPhone's is: it states 270 and a presentationTurn of 180 (CameraInfo), its frames are the mounting's (the chart
    // unturned: the device turns the video) and its JPEG stills are turned like the platform's (made upright for the
    // stated orientation). A laptop's camera, which states none, is not changed.
    bool frontPresentedAt90 {};
    // A named pipe the camera reads commands from, for a test that cannot reach the camera object (a guest that runs in
    // another process): the camera makes it if the path does not exist, and reads one command a line from it for as long as
    // it lives (parseSyntheticControlLine). Empty: none.
    std::string controlPath;
};

// A command of the control pipe: interrupt every session (for the named cause, the generic one by default) or end the
// interruption.
struct SyntheticControlCommand {
    bool interrupt {};
    CameraEventKind kind {CameraEventKind::interrupted};
};

// `interrupt`, `interrupt background|multitasking|pressure|other` or `resume`, in any case, with the blanks around them
// ignored; nothing for any other line.
[[nodiscard]] std::optional<SyntheticControlCommand> parseSyntheticControlLine(std::string_view line);

// The multi-camera rule it models, as an AVCaptureMultiCamSession asks it:
// every camera with an input (one that streams, or has a still on its way)
// lies in one concurrent set and runs a format a multi-camera session takes; a
// join that cannot is refused with nothing changed, and so is one whose cameras
// ask for more than 200 million pixels a second together (the hardware
// budget). A camera that has an anchor (CameraSession::setAnchors) runs in it
// and nowhere else: a stream outside it, a still it does not make, or an anchor
// a multi-camera session does not take is refused. A camera with none picks the
// smallest of its video formats that covers the stream, moving between them as
// others join and leave.
class SyntheticCamera : public Camera {
public:
    // Sessions opened since the object was made, and sessions open now. A
    // blocked guest never opens one.
    [[nodiscard]] virtual unsigned sessionsOpened() const noexcept = 0;
    [[nodiscard]] virtual unsigned sessionsOpen() const noexcept = 0;
    // What the device last told a session (CameraSession::setAnchors): the
    // format each camera runs in and the limits, and how many times it was
    // told. Empty, zero limits and zero before any session was told.
    [[nodiscard]] virtual std::vector<std::optional<std::size_t>> sessionAnchors() const = 0;
    [[nodiscard]] virtual CameraLimits sessionLimits() const noexcept = 0;
    [[nodiscard]] virtual unsigned sessionAnchorCalls() const noexcept = 0;
    // The frames that exist: made by a session's capture and not yet freed by
    // whoever holds them (the capture's buffer of the real camera), the most
    // there were at once since resetFramePeak(), and the count now.
    [[nodiscard]] virtual unsigned framesAlive() const noexcept = 0;
    [[nodiscard]] virtual unsigned framePeak() const noexcept = 0;
    virtual void resetFramePeak() noexcept = 0;
    // The settings last applied to a camera, and the cameras running video.
    [[nodiscard]] virtual CameraSettings appliedSettings(std::size_t camera) const = 0;
    [[nodiscard]] virtual bool videoRunning(std::size_t camera) const = 0;
    [[nodiscard]] virtual std::uint32_t videoFrameRate(std::size_t camera) const = 0;
    // Produces `frames` frames for a camera, as its timer would have (manual
    // clock only). The clock advances by one frame interval for each.
    virtual void pump(std::size_t camera, unsigned frames = 1) = 0;
    virtual void advanceClock(std::uint64_t nanoseconds) = 0;
    // The size of the format a camera runs video at; 0 x 0 when it does not.
    [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> videoSize(std::size_t camera) const = 0;
    // Interrupts every session, as the host does; no frames flow until resume().
    virtual void interrupt(CameraEventKind kind = CameraEventKind::interrupted) = 0;
    // Ends the interruption as the platform session does: every camera the system
    // moved out of its anchor (moveOutOfAnchor) is repaired first, and only then
    // are the sessions told they are back; a refused repair (refuseRepair) tells
    // them they failed instead (recoverAnchors).
    virtual void resume() = 0;
    // The system has moved the anchored cameras out of their formats, as it may
    // while they are interrupted; a session that opens afterwards starts afresh.
    virtual void moveOutOfAnchor(bool moved) = 0;
    virtual void refuseRepair(bool refuse) = 0;
    // The cameras the sessions put back before saying they were back, and the
    // frames a session produced while a camera was out of its anchor (the
    // capture goes on; the client must not pass them on).
    [[nodiscard]] virtual unsigned repairs() const noexcept = 0;
    [[nodiscard]] virtual unsigned framesOutsideAnchor() const noexcept = 0;
    // Delivers an event to every session, as the host does, and changes
    // nothing else (a failed session or an unavailable camera, for instance).
    virtual void post(CameraEvent event) = 0;
    // A Bayer still is refused when it is asked for; or it is taken and fails
    // alone, leaving the JPEG of the same capture.
    virtual void refuseRaw(bool refuse) = 0;
    virtual void failRaw(bool fail) = 0;
    // The platform makes a JPEG of another size than the request names (the
    // device must fail that still, not pass the picture on).
    virtual void stillsOfAnotherSize(bool another) = 0;
    // The platform's JPEG files are larger than the host takes (padded behind
    // the picture past the request's `maximumJpegBytes`), as a noisy scene at
    // the highest quality can make them: the device must fail that still, not
    // copy the file.
    virtual void oversizedStills(bool oversized) = 0;
    // The platform's JPEG files are at least this many bytes (padded behind the
    // picture): a large file that the host takes, as a noisy scene makes them.
    // Zero: the files are as the encoder wrote them.
    virtual void paddedStills(std::uint64_t bytes) = 0;
    // The stills the sessions have been asked for and accepted since the object
    // was made, so a test sees how many captures the device had with the platform.
    [[nodiscard]] virtual unsigned stillsTaken() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<SyntheticCamera> makeSyntheticCamera(const SyntheticCameraOptions& options = {});

// The chart a frame of the given size carries.
struct ChartMarker {
    std::uint16_t tag {};
    std::uint32_t counter {};
    bool valid {};
};
[[nodiscard]] ChartMarker decodeChartMarker(const std::uint8_t* luma, std::size_t stride, std::uint32_t width,
                                            std::uint32_t height) noexcept;
// The tag the chart of a camera of the given index carries.
[[nodiscard]] std::uint16_t chartTag(std::size_t camera) noexcept;

// Encodes an NV12 picture as a JPEG (a complete JFIF file with an Exif segment).
[[nodiscard]] std::vector<std::uint8_t> encodeSyntheticJpeg(const std::uint8_t* luma, std::size_t lumaStride,
                                                            const std::uint8_t* chroma, std::size_t chromaStride,
                                                            std::uint32_t width, std::uint32_t height, unsigned quality);

} // namespace uml::platform
