// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "background_video.hpp"
#include "calibration_store.hpp"
#include "camera.hpp"
#include "camera_description.hpp"
#include "camera_tiers.hpp"
#include "vhost_user_device.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace uml::camera {

/* The Virtio camera: the video device type VIRTIO_VIDEO_DEVICE_CAMERA, under
 * UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID. */
inline constexpr std::uint32_t cameraDeviceId = UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID;

/* How a session treats the host's cameras, fixed when the guest starts.
 *
 * Live opens a platform capture session when an application in the guest
 * streams, and closes it when none does.
 *
 * Blocked never opens one, so the host's capture indicator cannot light: the
 * device publishes the cameras that discovery found, and every video buffer
 * completes black and flagged blocked. It reports CameraState::blockedInUse
 * while the guest has a camera open. */
enum class CameraMode : std::uint8_t { live, blocked };

/* What the host is doing for the guest, for the application's notice. */
enum class CameraState : std::uint8_t {
    idle,
    streaming,
    /* The capture cannot run for a reason that ends by itself. */
    interrupted,
    /* The same, because other applications share the screen. */
    interruptedByMultitasking,
    /* The same, because the system is under pressure (heat). */
    interruptedBySystemPressure,
    /* A blocked session, and an application in the guest has a camera open
     * (a video stream is running, or a still was asked for): nothing is
     * captured, the guest gets black frames, and the application can say that
     * the camera is off. It lasts until the guest has closed every camera, so
     * a new open reports it again. A live session never reports it. */
    blockedInUse,
};

/* Where a diagnostic reading of the process's memory is useful, to measure
 * what the host's capture costs outside the guest's allowance. */
enum class CameraCheckpoint : std::uint8_t {
    /* The platform's capture session has just opened. */
    sessionOpened,
    /* A still has been delivered (or has failed) and its session is still open. */
    stillDelivered,
    /* The platform's capture session has just closed. */
    sessionClosed,
};

struct CameraDeviceOptions {
    CameraMode mode {CameraMode::live};
    CameraTier tier {CameraTier::small};
    /* What the device lets the host hold, which the GPU's pool was reduced for
     * (`liveCameraReserveMiB`, with this value): at most `use.streams` cameras
     * run at once, and a start beyond it is refused as a camera that conflicts
     * with a running one is; stills are admitted while the captures the device
     * holds cost no more than `stillAdmissionBytes` allows, the others waiting
     * their turn. The default is the tier's ceiling, which a device whose
     * budget was not taken from a composed layout is given. */
    CameraUse use;
    /* Where learned colour data is kept; null keeps it in memory. */
    std::shared_ptr<CalibrationStore> calibration;
    /* The application's statement of a camera's sensor orientation; the
     * platform's where empty. */
    std::function<std::uint32_t(const uml::platform::CameraInfo&)> sensorOrientation;
    /* The clockwise rotation, in degrees, that ROTATE_AND_CROP's automatic
     * setting applies to a camera's picture; none where empty. It differs per
     * camera because a front and a back camera turn opposite ways
     * (camera_orientation.hpp). */
    std::function<std::uint32_t(const uml::platform::CameraInfo&)> automaticRotation;
    /* Told on the transport thread, for a diagnostic line, when the setting
     * and the turn a camera's frames get change: the camera's wire index, the
     * guest's ROTATE_AND_CROP setting (0 none, 1 to 3 the quarter turns, 4
     * automatic) and the clockwise degrees applied. It shows what reaches the
     * host; it changes nothing. */
    std::function<void(std::uint32_t camera, std::uint32_t setting, std::uint32_t degrees)> onRotateAndCrop;
    /* Told on the transport thread, for a diagnostic line, when what a camera's
     * controls ask of its light changes: the camera's wire index, whether the
     * torch is on and the flash mode a still is asked to use. The platform
     * hears of the torch in a setting and of the flash only with a still, and
     * prints nothing for either; this shows what the guest asked. It changes
     * nothing. */
    std::function<void(std::uint32_t camera, bool torch, uml::platform::CameraFlashMode flash)> onFlash;
    /* Told on the transport thread at each CameraCheckpoint, for a diagnostic
     * line; it changes nothing and must not block. */
    std::function<void(CameraCheckpoint)> onCheckpoint;
    /* Told on the transport thread, once for each capture the guest asked for
     * that did not end in a picture, why: the stage it stopped at, the camera,
     * the sizes asked for, resolved and encoded and what the platform said of
     * the capture (`uml::platform::CameraStillFailure`; numbers and fixed
     * words, never a name or a message). The runtime keeps the last few in its
     * session record. A capture that the camera's interruption or a blocked
     * session ends is not one of them, and neither is one that delivered its
     * JPEG, whatever happened to its RAW. It changes nothing. The worker waits
     * for it, so it is brief: the runtime's writes the session record, a
     * synchronous write, once for each. */
    std::function<void(const uml::platform::CameraStillFailure&)> onStillFailure;
    /* Told for a diagnostic record whenever the platform states or sets something about a camera it presents to the guest:
     * the camera's mounting, its anchor, what an admission set on its connections, the photo output's compensation, the
     * device's aspect ratio (`uml::platform::CameraPresentation`: numbers and fixed words, the camera as the guest numbers
     * it; the camera's whole note each time, so the newest of a camera replaces the older). Only cameras the guest sees
     * are told. It runs on a thread of the platform, in order for one camera, and returns quickly: the runtime's writes the
     * session record. It changes nothing. */
    std::function<void(const uml::platform::CameraPresentation&)> onCameraPresentation;
    /* Called on the transport thread when the state changes. */
    std::function<void(CameraState)> onState;
    /* Called on the transport thread, with the value it has now, whenever whether a camera of the guest is in video
     * mode changes. The guest is in video mode when a live camera's video runs, uninterrupted, and the guest asks for
     * the picture stabilized: Android's record template asks and its preview template does not, so a camera
     * application's video mode asks and its photo mode does not, and so does an application that records with no
     * preview. An application that records without asking is not seen as in video mode. The application opens the
     * microphone for it before the guest records, so the recording does not begin in silence. It is told false when
     * the device is reset or goes away; it changes nothing and must not block. */
    std::function<void(bool)> onVideoMode;
    /* The clock frame ages are measured on; the platform's where empty. */
    std::function<std::uint64_t()> clock;
    /* The application's way to end the guest's video from a thread of its own when it leaves the foreground, and to wait
     * until the guest has stopped it (`BackgroundVideo`); the device attaches while it exists. Empty: the video ends only
     * when the platform reports the departure. */
    std::shared_ptr<BackgroundVideo> background;
};

/* The camera device. Every method runs on the transport worker, which is also
 * the only thread that touches guest memory; the platform camera calls the
 * device back on its own threads and the device hands those calls to the worker
 * through its event descriptor. */
class VirtioCameraDevice final : public uml::virtio::VhostUserDevice {
public:
    VirtioCameraDevice(std::shared_ptr<uml::platform::Camera> host, CameraDeviceOptions options);
    ~VirtioCameraDevice() override;
    const char* name() const noexcept override;
    std::uint64_t features() const noexcept override;
    std::size_t queueCount() const noexcept override;
    std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override;
    void setGuestMapper(GuestMapper mapper) override;
    void setSharedMemoryRegions(std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions) override;
    uml::virtio::VhostUserQueueResult processQueue(std::uint32_t index, std::uint64_t requestId,
                                                   std::span<const std::uint8_t> request,
                                                   std::size_t responseCapacity) override;
    bool takeQueueCompletion(uml::virtio::VhostUserQueueCompletion&) override;
    void reset() noexcept override;
    int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    /* A camera stream outlives a guest suspend: the buffers it holds stay with
     * the device, and the transport keeps their completions until the ring is
     * enabled again, as it does for the codecs. Without it, disabling a ring
     * while a buffer is queued would reset the device, and every request it
     * held would be dropped with no answer. */
    bool keepsDeferredRequestsWhileDisabled() const noexcept override { return true; }

    /* The cameras the guest sees, in wire order. Call it on the transport
     * worker, or while the device is not serving a guest. */
    [[nodiscard]] std::vector<CameraDescription> cameras() const;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::camera
