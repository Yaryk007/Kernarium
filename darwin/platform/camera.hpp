// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The platform's cameras. The Apple backend implements it over AVFoundation and
// a synthetic backend implements it with no hardware (synthetic_camera/);
// nothing here names AVFoundation, V4L2 or the Virtio wire, and the Virtio
// camera device maps these types to and from the wire.
//
// Discovery needs no authorization and never prompts. A capture session does:
// openSession() fails when the system has not granted access, and nothing in
// this interface asks for it (camera_authorization.hpp does, at a user's act).

#include "camera_presentation.hpp"
#include "camera_still_failure.hpp"
#include "nv12_transform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::platform {

// zoom is a virtual device that switches between lenses by zoom ratio and
// accepts automatic exposure, focus and white balance only; every other kind is
// one sensor behind one lens, and lets a guest set them by hand where the
// device can. A host lists its zoom cameras, but the Virtio camera device
// publishes none of them: a guest switches lenses by switching cameras.
enum class CameraLensKind : std::uint8_t { zoom, wide, ultraWide, telephoto, external };

enum class BayerOrder : std::uint8_t { none, rggb, grbg, gbrg, bggr };

// A capture format: the size of a frame and the frame rates it runs at, in
// frames per second.
struct CameraVideoFormat {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t minimumFrameRate {};
    std::uint32_t maximumFrameRate {};
    // The format may join a session that runs several cameras at once.
    bool multiCamera {};
    friend bool operator==(const CameraVideoFormat&, const CameraVideoFormat&) = default;
};

struct CameraStillSize {
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const CameraStillSize&, const CameraStillSize&) = default;
};

// A device format as data, for hosts whose photo sizes and frame rates belong
// to the format a camera runs in: what the choice of the camera's anchor (the
// one format it runs in, photo_dimensions.hpp) needs to know of it.
// `photoSizes` are the sizes its photo output lists.
struct CameraRateSpan {
    double minimum {};
    double maximum {};
    friend bool operator==(const CameraRateSpan&, const CameraRateSpan&) = default;
};
struct CameraCaptureFormat {
    std::uint32_t width {};
    std::uint32_t height {};
    std::vector<CameraRateSpan> frameRates;
    std::vector<CameraStillSize> photoSizes;
    bool fullRange {};
    bool multiCamera {};  // a session that runs several cameras takes it
    // The zoom ratio up to which the system recommends zooming a camera that runs in this format, in the terms of
    // CameraInfo::zoomMaximum; zero where the system states none. It is advice, not part of the format's identity.
    float recommendedZoomMaximum {};
    // The largest zoom ratio the format itself delivers (the platform's own limit for it), in the terms of
    // CameraInfo::zoomMaximum, which is the device's largest over all its formats; zero where the host states none.
    // Advice as well: a camera whose format has no recommendation is stated up to it.
    float maximumZoom {};
    // The dynamic aspect ratios the device lists for the format (a mask of
    // aspectRatioBit; zero for a format with none) and, once the format is
    // described as the guest is offered it (presentedFormatOf), the one the
    // device is held at.
    std::uint8_t aspectRatios {};
    CameraAspectRatio pinnedRatio {};
    friend bool operator==(const CameraCaptureFormat&, const CameraCaptureFormat&) = default;
};

// What a guest of a tier may ask of a camera, which its anchor must serve. Zero
// is no limit.
struct CameraLimits {
    // The largest JPEG the guest is offered, in pixels.
    std::uint64_t maximumStillPixels {};
    // The largest frame the host may deliver for a stream, in the bytes of a
    // capture buffer (alignedFrameBytes): a video size whose frame from the
    // anchor is larger is not offered.
    std::uint64_t maximumFrameBytes {};
    // The row alignment of those capture buffers, in bytes; zero or one for
    // rows with no padding.
    std::uint32_t frameRowAlignment {};
};

// What a Bayer still teaches about a sensor. The matrices are DNG's, row-major.
struct CameraColorData {
    BayerOrder bayer {BayerOrder::none};
    std::uint32_t blackLevel {};
    std::uint32_t whiteLevel {};
    std::uint32_t rawWidth {};
    std::uint32_t rawHeight {};
    // The size of the capture format the camera ran in when this was learned
    // (its anchor), which the device fills in: another format may make another
    // Bayer size. Zero for a host that lists no formats.
    std::uint32_t anchorWidth {};
    std::uint32_t anchorHeight {};
    // DNG LightSource codes of the two calibrations.
    std::uint32_t illuminant1 {};
    std::uint32_t illuminant2 {};
    std::array<double, 9> colorMatrix1 {};
    std::array<double, 9> colorMatrix2 {};
    std::array<double, 9> forwardMatrix1 {};
    std::array<double, 9> forwardMatrix2 {};
    // Optics the same capture reveals; zero when it did not.
    double focalLengthMillimetres {};
    std::array<double, 4> intrinsics {};  // fx, fy, cx, cy in pixels of the pixel array
    friend bool operator==(const CameraColorData&, const CameraColorData&) = default;
};

// One camera of the host, as discovery reports it. Ranges are the camera's
// own; a camera that cannot do a thing leaves its flag false.
struct CameraInfo {
    // The host's stable name for the camera. It identifies the camera across
    // starts and is never shown to a guest.
    std::string identifier;
    CameraPosition position {CameraPosition::back};
    CameraLensKind lens {CameraLensKind::wide};

    bool autofocus {};
    bool flash {};
    bool torch {};
    // The host lets a client set these by hand on this camera. A zoom camera
    // sets none of them.
    bool manualExposure {};
    bool manualFocus {};
    bool manualWhiteBalance {};
    // The host can take a Bayer still with this camera.
    bool rawStills {};

    // The zoom ratio as Android states it: 0.5 is the ultra wide lens of a zoom
    // camera, 1.0 its main lens. switchOvers are the ratios where it changes lens.
    // Every camera's range holds 1.0, and a single lens's 1.0 is its own full
    // field of view (zoomRatioScale). zoomMaximum is the platform's own limit;
    // a camera that runs in an anchor is stated up to the system's recommended
    // maximum for that format where it gives one (CameraCaptureFormat).
    float zoomMinimum {1.0f};
    float zoomMaximum {1.0f};
    std::vector<float> switchOvers;
    // A zoom camera: the identifiers of the lenses it switches between. The
    // device publishes no zoom camera, only its lenses.
    std::vector<std::string> constituents;

    // The full field of view, in pixels, and its horizontal angle in degrees.
    std::uint32_t pixelArrayWidth {};
    std::uint32_t pixelArrayHeight {};
    float horizontalFieldOfView {};
    float aperture {};                // f-number; zero when unknown
    float minimumFocusDistanceMillimetres {};  // zero for a fixed focus
    std::uint32_t isoMinimum {};
    std::uint32_t isoMaximum {};
    std::uint64_t exposureMinimumNanoseconds {};
    std::uint64_t exposureMaximumNanoseconds {};
    float maximumWhiteBalanceGain {};
    // Clockwise degrees (0, 90, 180, 270) from the sensor's top to the upright
    // side of a frame in the portrait orientation of the host's display.
    std::uint32_t sensorOrientation {};
    // The clockwise half turn (0 or 180) that every picture of the camera takes before the guest sees it:
    // `sensorOrientation` counts it, so a built-in front camera whose own mounting is 90 states 270. The platform's
    // frames are in the mounting and the device turns the video; the platform's stills are already turned (the photo is
    // made turned or refused, never delivered unturned).
    std::uint32_t presentationTurn {};

    std::vector<CameraVideoFormat> videoFormats;
    // The JPEG sizes the camera makes. A host that fills captureFormats lists
    // every size some format makes, and the device replaces it with the anchor's
    // stills when it arranges the camera; otherwise every size works with every
    // video format.
    std::vector<CameraStillSize> stillSizes;  // JPEG
    // The formats behind videoFormats and stillSizes, when the sizes a still
    // and the rates a stream can have depend on the format the camera runs in.
    // The device picks one of them as the camera's anchor (setAnchors). Empty:
    // the host has one format's worth of facts and no choice to make.
    std::vector<CameraCaptureFormat> captureFormats;
    CameraStillSize rawStillSize {};          // zero: no Bayer still
};

// 3A states as a platform reports them. The device maps them to a guest's
// numbering.
enum class CameraControlState : std::uint8_t { inactive, searching, converged, locked, failed };

// What one exposure actually did.
struct CameraExposure {
    std::uint64_t exposureNanoseconds {};
    std::uint64_t frameDurationNanoseconds {};
    std::uint32_t iso {};
    float lensPositionDioptres {};
    std::array<float, 3> whiteBalanceGains {1.0f, 1.0f, 1.0f};  // red, green, blue
    std::uint32_t colorTemperature {};
    float tint {};
    CameraControlState ae {CameraControlState::inactive};
    CameraControlState af {CameraControlState::inactive};
    CameraControlState awb {CameraControlState::inactive};
    float zoomRatio {1.0f};
    // The lens that made the exposure: an index of cameras().
    std::uint32_t activeCamera {};
    float aperture {};
    float focalLengthMillimetres {};
    bool flashFired {};
    // Stills only.
    std::array<float, 3> neutralPoint {};
    std::array<float, 8> noiseProfile {};  // (S, O) per colour channel; zero when unknown
};

// An NV12 frame, BT.709 full range. The planes stay valid while `owner` is held,
// and holding it for long starves the capture.
struct CameraFrame {
    std::uint32_t width {};
    std::uint32_t height {};
    std::span<const std::uint8_t> luma;
    std::size_t lumaStride {};
    std::span<const std::uint8_t> chroma;  // interleaved Cb, Cr at half the width and height
    std::size_t chromaStride {};
    std::shared_ptr<const void> owner;
    // The platform's own handle to the picture, valid while `owner` is held;
    // see Nv12Source::native. Null when the frame has none.
    const void* native {};
    // When the exposure began, in nanoseconds of the host's monotonic clock
    // (CameraSession::now()).
    std::uint64_t timestampNanoseconds {};
    CameraExposure exposure;
};

// What a capture produced. A part the request asked for that failed is empty
// and the others stay: `ok` is false only when nothing the request asked for
// arrived, so a Bayer still that fails alone leaves the JPEG that came with it.
struct CameraStillResult {
    std::uint64_t token {};
    bool ok {};
    // A complete JFIF file when a JPEG was asked for.
    std::vector<std::uint8_t> jpeg;
    // A Bayer still when one was asked for: 16-bit samples, right-aligned and
    // tightly packed (rawWidth * 2 bytes a row).
    std::vector<std::uint16_t> raw;
    CameraColorData color;  // bayer is none when the capture taught nothing
    CameraExposure exposure;
    // Why the capture, or the JPEG of it, did not end in a picture, as far as
    // the platform can tell (camera_still_failure.hpp): stage none for a
    // capture that did. The camera, its position and the sizes the platform did
    // not see are the device's to fill.
    CameraStillFailure failure;
};

enum class CameraExposureMode : std::uint8_t { continuousAuto, locked, manual };
enum class CameraFocusMode : std::uint8_t { continuousAuto, locked, manual };
enum class CameraWhiteBalanceMode : std::uint8_t { continuousAuto, locked, manual };
enum class CameraFlashMode : std::uint8_t { off, automatic, on };

// A metering or focus region, as fractions of the frame; weight 0 clears it.
struct CameraRegion {
    float x {};
    float y {};
    float width {};
    float height {};
    std::uint32_t weight {};
    friend bool operator==(const CameraRegion&, const CameraRegion&) = default;
};

// Everything a client may set on one camera. A session applies the whole value:
// it compares with what it applied last and changes what differs.
struct CameraSettings {
    CameraExposureMode exposureMode {CameraExposureMode::continuousAuto};
    std::uint64_t exposureNanoseconds {};
    std::uint32_t iso {};
    float exposureBiasStops {};
    // continuousAuto: the camera focuses by itself. locked: the lens holds where
    // it is. manual: the lens goes to focusDistanceDioptres when the guest writes
    // it (focusDistanceSerial changes) and holds from then on.
    CameraFocusMode focusMode {CameraFocusMode::continuousAuto};
    float focusDistanceDioptres {};
    // Whatever the focus mode, a change of this runs one scan; the lens then
    // holds where the scan ends, or goes back to continuous focus in
    // continuousAuto.
    std::uint32_t focusTriggerSerial {};
    // A change moves the lens in manual mode; an unchanged value leaves the lens alone.
    std::uint32_t focusDistanceSerial {};
    CameraWhiteBalanceMode whiteBalanceMode {CameraWhiteBalanceMode::continuousAuto};
    std::array<float, 3> whiteBalanceGains {1.0f, 1.0f, 1.0f};
    float zoomRatio {1.0f};
    bool torch {};
    std::optional<CameraRegion> aeRegion;
    std::optional<CameraRegion> afRegion;
    bool stabilization {true};
    friend bool operator==(const CameraSettings&, const CameraSettings&) = default;
};

struct CameraStillRequest {
    std::uint64_t token {};
    // Zero: no JPEG. Otherwise exactly a size the camera lists in stillSizes,
    // which the host makes at that size or refuses: a picture of another size
    // is not a result.
    std::uint32_t jpegWidth {};
    std::uint32_t jpegHeight {};
    std::uint32_t jpegQuality {95};
    // The largest JPEG file the host takes, in bytes (the guest's buffer for
    // that size): the platform drops a larger file and the capture fails.
    // Zero: no bound.
    std::uint64_t maximumJpegBytes {};
    bool raw {};
    // The capture must not use multi-frame processing (a Bayer still needs it).
    bool speedPrioritized {};
    CameraFlashMode flash {CameraFlashMode::off};
};

inline constexpr std::size_t allCameras = std::numeric_limits<std::size_t>::max();

// What multiplies a host's own zoom factor (1.0 at its widest lens) into the
// ratio Android sees. Only a zoom camera is scaled by the host's display
// multiplier, which makes 1.0 its main lens; a single lens keeps 1.0 as its own
// full field of view, because Android requires every camera's range to hold 1.0
// (a range that starts above it crashes a client that rebinds at 1.0, and one
// that starts below it opens cropped).
[[nodiscard]] inline constexpr float zoomRatioScale(CameraLensKind lens, double displayMultiplier) noexcept
{
    return lens == CameraLensKind::zoom && displayMultiplier > 0 ? float(displayMultiplier) : 1.0f;
}

enum class CameraEventKind : std::uint8_t {
    // The capture cannot run for a reason that ends by itself: another
    // application, a call, system pressure.
    interrupted,
    // The same, because other applications share the screen (iPad
    // multitasking). A host that cannot tell reports interrupted.
    interruptedByMultitasking,
    // The same, because the system is under pressure (heat): the capture is
    // stopped until the pressure eases.
    interruptedBySystemPressure,
    // The same, because the application left the foreground while the session
    // ran: a camera is not available to an application that has left it. Only a
    // host that can tell reports it; the others report interrupted. A session
    // that starts while the application is away never ran in the foreground, so
    // it reports interrupted: the device holds it until the application is
    // back, and ends no video for it.
    interruptedInBackground,
    // The capture runs again.
    resumed,
    // The camera went away or came back (an external camera).
    unavailable,
    available,
    // The session failed and will not recover.
    failed,
};

struct CameraEvent {
    // An index of cameras(), or allCameras.
    std::size_t camera {allCameras};
    CameraEventKind kind {CameraEventKind::interrupted};
};

// Receives what a session produces. Every method runs on a thread of the
// platform and must return quickly; a frame is taken by holding its owner. A
// delegate must outlive the session it was given to.
class CameraSessionDelegate {
public:
    virtual ~CameraSessionDelegate() = default;
    virtual void cameraFrame(std::size_t camera, CameraFrame frame) = 0;
    virtual void cameraStill(std::size_t camera, CameraStillResult result) = 0;
    virtual void cameraEvent(CameraEvent event) = 0;
    // What the session stated of a camera and set on its connections, once from
    // discovery on and again whenever one of them changes (camera_presentation.hpp):
    // for a diagnostic record; it changes nothing. A session calls it on its own
    // threads, in order for one camera, and the delegate returns quickly. The
    // default ignores it.
    virtual void cameraPresentation(std::size_t camera, const CameraPresentation& presentation)
    {
        (void)camera;
        (void)presentation;
    }
};

// One capture session over the cameras of a host. The Apple backend runs a
// single AVCaptureMultiCamSession whatever the number of cameras.
class CameraSession {
public:
    virtual ~CameraSession() = default;

    // Starts a camera, or restarts it at another size or rate. A host with
    // captureFormats runs the camera in its anchor, whatever the size and the
    // rate (setAnchors), and the size and the rate are ones the offer holds.
    // Fails when the camera cannot join the cameras already running (their
    // anchors must all be formats a multi-camera session takes), changing
    // nothing but the camera itself.
    [[nodiscard]] virtual bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height,
                                          std::uint32_t frameRate) = 0;
    virtual void stopVideo(std::size_t camera) = 0;
    virtual void apply(std::size_t camera, const CameraSettings& settings) = 0;
    // Takes one still; the result arrives through the delegate. A still needs no
    // running video. Fails when the camera cannot take what the request asks.
    [[nodiscard]] virtual bool takeStill(std::size_t camera, const CameraStillRequest& request) = 0;
    // Names the format each camera runs in: for each camera of cameras(), the
    // index of one of its captureFormats, or nothing for a camera the device does
    // not offer; and the limits the anchors were chosen for, whose still cap
    // bounds the photo output of a camera (a format that lists a larger size is
    // set to the largest within the cap). Called once, right after the session
    // opens; a host with no captureFormats has nothing to do with it.
    virtual void setAnchors(std::span<const std::optional<std::size_t>> formatOfCamera, const CameraLimits& limits)
    {
        (void)formatOfCamera;
        (void)limits;
    }
    // Stops every camera. The session cannot be used afterwards.
    virtual void stop() = 0;
};

class Camera {
public:
    virtual ~Camera() = default;

    // The host's cameras. Their order is stable for the life of the object and
    // every index in this interface is an index into it.
    [[nodiscard]] virtual std::vector<CameraInfo> cameras() const = 0;
    // Sets of cameras the host can run at once, as indices of cameras().
    [[nodiscard]] virtual std::vector<std::vector<std::size_t>> concurrentSets() const = 0;
    // Names the host model; calibration is kept per model and lens.
    [[nodiscard]] virtual std::string hostModel() const = 0;
    // Nanoseconds of the monotonic clock that frame timestamps use.
    [[nodiscard]] virtual std::uint64_t now() const noexcept = 0;
    // The fastest crop, rotate and scale of this host's frames (the Apple
    // camera's runs on the system's scaler). The caller keeps the object and
    // uses it from one thread.
    [[nodiscard]] virtual std::unique_ptr<Nv12Transformer> makeFrameTransformer() const = 0;
    // The working storage, in bytes for each pixel of a JPEG still, that this
    // host's source holds while it makes the picture, beyond the encoded file
    // the device counts: `systemCameraStillWorkingBytesPerPixel` for the
    // system's camera. Zero where the source makes the file in place or the
    // host does not say.
    [[nodiscard]] virtual std::uint32_t stillWorkingBytesPerPixel() const noexcept { return 0; }
    // The working storage beyond that, in bytes for each pixel of a JPEG still, that this host's source holds while it
    // turns the still of a camera whose pictures it presents half around (`CameraInfo::presentationTurn` 180): the device
    // counts it in the capture's cost for such a camera. `systemCameraTurnedStillWorkingBytesPerPixel` for the system's
    // camera; zero where the source turns nothing or its figure above covers it.
    [[nodiscard]] virtual std::uint32_t turnedStillWorkingBytesPerPixel() const noexcept { return 0; }
    // Opens a capture session, or returns null when the system has not granted
    // access or the host has no camera. This is the only call that can light a
    // capture indicator, and a blocked guest session never makes it.
    [[nodiscard]] virtual std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) = 0;
};

// The system's cameras through AVFoundation.
[[nodiscard]] std::unique_ptr<Camera> makeAppleCamera();

} // namespace uml::platform
