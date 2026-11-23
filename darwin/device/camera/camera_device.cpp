// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_device.hpp"

#include "camera_controls.hpp"
#include "guest_buffers.hpp"
#include "jpeg_gap.hpp"
#include "nv12_transform.hpp"
#include "raw_pitch.hpp"
#include "unique_descriptor.hpp"

#include <sys/event.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace uml::camera {
namespace {
using uml::platform::CameraControlState;
using uml::platform::CameraEvent;
using uml::platform::CameraEventKind;
using uml::platform::CameraExposure;
using uml::platform::CameraFrame;
using uml::platform::CameraSettings;
using uml::platform::CameraStillRequest;
using uml::platform::CameraStillResult;
using uml::video::GuestPlanes;
using uml::video::rawPitch;
using uml::virtio::toBytes;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;

constexpr std::size_t maximumStreams = 32;
constexpr std::size_t maximumHeld = 128;
constexpr std::size_t maximumControls = 256;
constexpr std::size_t metaBacklogLimit = 16;
constexpr std::size_t inboxFrameLimit = inboxFramesPerCamera;
/* A group of still buffers whose last buffer has not arrived this long after
 * its first completes with an error. */
constexpr std::uint64_t groupTimeoutNanoseconds = 2'000'000'000;
/* A capture that has started has this long to deliver before its buffers
 * complete with an error: a long exposure, a flash and a 48 MP capture need
 * more than the assembly of a group does. */
constexpr std::uint64_t captureTimeoutNanoseconds = 10'000'000'000;
/* A capture the guest gave up on stays charged until the platform ends it. One
 * that the platform still holds this long after the guest's deadline is taken
 * as lost: the session is closed, which ends every capture it holds. */
constexpr std::uint64_t abandonedCaptureNanoseconds = 30'000'000'000;
/* How long a session that failed to open waits before it is tried again. */
constexpr std::uint64_t sessionRetryNanoseconds = 1'000'000'000;
/* A lost camera's first failed video buffer waits this long for the guest to have read the camera's interrupted record
 * (`lossRecordDelivered`); the buffers fail after it whether the guest has read it or not. */
constexpr std::uint64_t lossRecordReadNanoseconds = 100'000'000;
/* The guest is still acting on a camera's lost video this long after the loss: the application that leaves the foreground
 * then waits for the guest to stop the streams of that video (`BackgroundVideo`). A loss older than this was not acted on,
 * and a later departure does not wait for it. */
constexpr std::uint64_t lossBeingActedOnNanoseconds = 5'000'000'000;
constexpr std::uint32_t userIdent = 0;
constexpr std::uint32_t groupTimerIdent = 0xffffff00u;
constexpr std::uint32_t reservedStreamIds = 0xffff0000u;
/* The black pictures are written to a guest's buffer from a chunk of the
 * device's own, a plane at a time, so no stream or picture size holds a
 * picture of its own. The chunk is among the device's fixed allocations. */
constexpr std::size_t blackChunkBytes = 64 * 1024;
constexpr std::uint32_t defaultVideoWidth = 1280;
constexpr std::uint32_t defaultVideoHeight = 720;

/* The values Android and V4L2 give the states the platform reports. */
constexpr std::uint32_t colorspaceRec709 = 3;
constexpr std::uint32_t xferFunc709 = 1;
constexpr std::uint32_t ycbcrEnc709 = 2;
constexpr std::uint32_t quantizationFullRange = 1;

template<class T> T load(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(T)) throw std::invalid_argument("short camera command");
    return uml::virtio::fromBytes<T>(bytes);
}

std::vector<std::uint8_t> reply(std::uint32_t stream, std::uint32_t type = VIRTIO_VIDEO_RESP_OK_NODATA)
{
    return toBytes(virtio_video_cmd_hdr {type, stream});
}

std::uint32_t q16(double value) noexcept
{
    return std::uint32_t(std::int32_t(std::lround(value * 65536.0)));
}

std::uint32_t floatBits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }

std::uint32_t aeState(CameraControlState state) noexcept
{
    switch (state) {
    case CameraControlState::searching: return 1;
    case CameraControlState::converged: return 2;
    case CameraControlState::locked: return 3;
    default: return 0;
    }
}

std::uint32_t awbState(CameraControlState state) noexcept { return aeState(state); }

std::uint32_t afState(CameraControlState state, bool continuous) noexcept
{
    switch (state) {
    case CameraControlState::searching: return continuous ? 1 : 3;
    case CameraControlState::converged: return continuous ? 2 : 4;
    case CameraControlState::locked: return 4;
    case CameraControlState::failed: return continuous ? 6 : 5;
    default: return 0;
    }
}

} // namespace

class VirtioCameraDevice::Implementation final : public uml::platform::CameraSessionDelegate {
public:
    struct Resource {
        GuestPlanes planes;
        std::uint64_t capacity {};
        bool queued {};
    };
    /* A buffer the guest queued. */
    struct Held {
        std::uint64_t request {};
        std::uint32_t resource {};
        std::uint64_t cookie {};  // the guest's timestamp
        std::uint32_t flags {};
    };
    struct MetaEntry {
        virtio_video_camera_meta meta {};
        std::uint64_t timestamp {};  // of the exposure, on the device's clock; zero: now
    };
    struct Stream {
        std::uint32_t id {};
        std::uint32_t camera {};
        std::uint32_t kind {};
        std::uint32_t format {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t intervalNumerator {1};
        std::uint32_t intervalDenominator {30};
        std::unordered_map<std::uint32_t, Resource> resources;
        std::deque<Held> queued;
        std::deque<MetaEntry> backlog;  // meta streams
        bool started {};
        bool timerArmed {};
        std::uint64_t lastDelivered {};
        bool delivered {};
        std::uint64_t framesOut {};
        std::uint64_t framesDropped {};
    };
    struct Member {
        std::uint32_t stream {};
        Held held;
    };
    struct Group {
        std::uint32_t camera {};
        std::uint64_t cookie {};
        std::vector<Member> members;
        std::uint64_t deadline {};
        std::uint64_t token {};
        /* The platform events there were when the capture was asked for (the
         * inbox's serial): a result that comes after a later boundary of its
         * camera (an interruption, a recovery) is from a capture that spans it. */
        std::uint64_t fireSerial {};
        bool fired {};
        /* The guest's deadline passed with the platform still holding the
         * capture: the buffers have failed, and the group stays, with its
         * cost, until the platform ends the capture (`deliverStill`) or the
         * session does. */
        bool expired {};
        /* The group is whole and waits for the captures the device holds to
         * leave room for its cost (`stillBudget`). */
        bool waiting {};
        /* The order in which groups became whole: they are admitted in it. */
        std::uint64_t whole {};
        /* What the capture holds while it is with the host (`stillCosts`). */
        std::uint64_t cost {};
    };
    enum class Interruption : std::uint8_t { none, interrupted, multitasking, pressure };
    struct Runtime {
        Runtime(CameraDescription d, bool blocked, std::uint32_t orientation)
            : description(std::move(d)), controls(description.info, orientation, blocked), values(controls.defaults())
        {
        }
        CameraDescription description;
        CameraControls controls;
        ControlValues values;
        CameraSettings applied;
        bool appliedValid {};
        std::uint64_t sequence {};
        std::optional<CameraExposure> latest;
        Interruption interruption {Interruption::none};
        /* The inbox serial of the last platform event handled for this camera that
         * began or ended an interruption, or ended a session. A frame the platform
         * handed over before it (a lower serial) was captured before the
         * recovery it announces, whatever the camera's state is by the time the
         * frame is taken. */
        std::uint64_t boundary {};
        /* The capture is starting and has not delivered a frame yet. */
        bool resuming {};
        bool videoRunning {};
        bool failed {};
        /* The host ended the camera's video: the application left the foreground while the guest had a video stream open on
         * the camera (`endsVideoWhenAppLeaves`). Every video stream of the camera, those the guest restarts after a suspend
         * too, completes its buffers with the error flag and the interrupted flag and never gets a picture, until the
         * guest has closed its last video stream of the camera: the guest's camera layer decides what to do with that news.
         * It is not a state the platform's recovery ends. */
        bool lost {};
        /* When the video was lost, on the device's clock. */
        std::uint64_t lostAt {};
        /* The camera's interrupted record goes to the guest ahead of the first failed video buffer of the loss (the guest's
         * camera layer reads the camera's records, and reports the loss only when the record that came with the failed
         * buffer says it): its sequence (none until it is made), when it was made, the meta buffers (stream and resource)
         * that carried it, and whether the guest has queued one of them again, which is how the device knows it was read. */
        std::uint64_t lossRecord {};
        std::uint64_t lossRecordAt {};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> lossRecordCarriers;
        bool lossRecordRead {};
        /* The guest has been told the camera is interrupted (or gone) and not
         * yet that it is back; it is told once each way. */
        bool told {};
        bool toldUnavailable {};
        std::uint64_t videoRetry {};
        std::uint32_t runWidth {};
        std::uint32_t runHeight {};
        std::uint32_t runRate {};
        std::uint32_t calibrationAttempts {};
        /* The first buffer of this configuration's capture has been compared
         * with what the reserve counts. */
        bool bufferChecked {};
        /* The ROTATE_AND_CROP setting and the turn last reported to
         * `onRotateAndCrop`. */
        std::uint32_t reportedSetting {~0u};
        std::uint32_t reportedTurn {~0u};
        /* The torch and the still flash last reported to `onFlash`; off
         * until the guest asks for either. */
        bool reportedTorch {};
        uml::platform::CameraFlashMode reportedFlash {uml::platform::CameraFlashMode::off};
    };
    /* Everything the platform hands over is stamped with the serial of the
     * last event it had handed over (`inboxSerial`): a frame or a result of
     * serial n came after event n and before event n + 1. */
    struct InboxFrame {
        std::size_t camera {};
        CameraFrame frame;
        std::uint64_t serial {};
    };
    struct InboxStill {
        std::size_t camera {};
        CameraStillResult result;
    };
    struct InboxEvent {
        CameraEvent event;
        std::uint64_t serial {};
    };

    std::shared_ptr<uml::platform::Camera> host;
    CameraDeviceOptions options;
    std::shared_ptr<CalibrationStore> calibration;
    std::string model;
    CameraTierLimits limits;
    std::vector<Runtime> runtimes;
    std::unordered_map<std::size_t, std::uint32_t> wireOfPlatform;
    // The SENSOR_ORIENTATION of each platform camera's description, which is what the guest is told (fixed once the device
    // is made, like wireOfPlatform).
    std::unordered_map<std::size_t, std::uint32_t> guestOrientationOfPlatform;
    // The newest reading of each platform camera's photo output compensation (cameraPresentation, on the platform's thread).
    struct CompensationReading {
        uml::platform::CameraFact supported {};
        uml::platform::CameraFact enabled {};
        uml::platform::CameraReading reading {};
    };
    std::mutex compensationMutex;
    std::map<std::size_t, CompensationReading> compensationOf;
    GuestMapper mapper;
    std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions;
    std::unordered_map<std::uint32_t, Stream> streams;
    std::deque<virtio_video_event> events;
    std::deque<VhostUserQueueCompletion> completions;
    std::vector<Group> groups;
    std::uint64_t nextToken {1};
    uml::gpu::UniqueDescriptor queue;
    std::vector<std::uint8_t> staging;
    /* The one row a picture turned half around is written from (`copyRowsHalfTurned`). */
    std::vector<std::uint8_t> turnedRow;
    /* A chunk of black luma and one of neutral chroma, made at the first black
     * picture. */
    std::vector<std::uint8_t> blackLuma;
    std::vector<std::uint8_t> blackChroma;
    /* The most the captures the device admits at once may cost together, from
     * `options.use` and the platform's source (`stillAdmissionBytes`). */
    std::uint64_t stillBudget {};
    std::uint64_t wholeCount {};
    bool firingWaiting {};
    std::unique_ptr<uml::platform::Nv12Transformer> transformer;
    CameraState reported {CameraState::idle};
    /* Whether the application was last told the guest is in video mode (`onVideoMode`). */
    bool reportedVideoMode {};
    std::uint64_t lastRetry {};
    bool sessionFailed {};
    /* The format each platform camera runs in, which a session is told when it opens. */
    std::vector<std::optional<std::size_t>> anchors;
    /* The video streams the guest has yet to stop after the application's departure ended their camera's video, which the
     * application waits for (`options.background`); on the worker. */
    std::set<std::uint32_t> pendingStops;

    // The platform's side: any thread, under the mutex.
    std::mutex inboxMutex;
    std::deque<InboxFrame> inboxFrames;
    /* The platform cameras whose video the device runs: a frame of any other is
     * a late callback of a capture that has stopped, and is not kept. */
    std::set<std::size_t> framesOpen;
    std::deque<InboxStill> inboxStills;
    std::deque<InboxEvent> inboxEvents;
    std::uint64_t inboxSerial {};
    // Last: it is destroyed first, and no callback outlives it.
    std::unique_ptr<uml::platform::CameraSession> session;

    Implementation(std::shared_ptr<uml::platform::Camera> platform, CameraDeviceOptions opts)
        : host(std::move(platform)), options(std::move(opts)), limits(limitsOf(options.tier)), queue(::kqueue())
    {
        if (!host) throw std::invalid_argument("a platform camera is required");
        if (queue.get() < 0) throw std::runtime_error("Virtio camera event queue failed");
        calibration = options.calibration ? options.calibration : std::make_shared<CalibrationStore>();
        options.use.streams = std::max<std::uint32_t>(options.use.streams, 1);
        options.use.stillWorkingBytesPerPixel = host->stillWorkingBytesPerPixel();
        stillBudget = stillAdmissionBytes(limits, options.use);
        model = host->hostModel();
        transformer = host->makeFrameTransformer();
        if (!transformer) throw std::invalid_argument("the platform camera has no frame transformer");
        ArrangementOptions arrangement;
        arrangement.tier = options.tier;
        arrangement.sensorOrientation = options.sensorOrientation;
        const auto infos = host->cameras();
        const auto sets = host->concurrentSets();
        auto cameras = arrangeCameras(infos, sets, model, *calibration, arrangement);
        anchors = anchorsByPlatformCamera(cameras, infos.size());
        const bool blocked = options.mode == CameraMode::blocked;
        runtimes.reserve(cameras.size());
        if (!cameras.empty()) std::fprintf(stderr, "%s\n", concurrencySummary(cameras, sets).c_str());
        for (auto& camera : cameras) {
            if (const auto line = anchorSummary(camera); !line.empty()) std::fprintf(stderr, "%s\n", line.c_str());
            wireOfPlatform[camera.platformIndex] = camera.index;
            guestOrientationOfPlatform[camera.platformIndex] = camera.sensorOrientation;
            const auto orientation = camera.sensorOrientation;
            runtimes.emplace_back(std::move(camera), blocked, orientation);
        }
        struct kevent change {};
        EV_SET(&change, userIdent, EVFILT_USER, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) < 0) throw std::runtime_error("Virtio camera wake-up failed");
        if (options.background) options.background->attach([this] { wake(); });
    }

    ~Implementation() override
    {
        if (options.background) options.background->detach();
        closeSession();
        reportVideoMode(false);
    }

    // ------------------------------------------------------------------------
    // Time and bookkeeping
    // ------------------------------------------------------------------------

    [[nodiscard]] std::uint64_t now() const { return options.clock ? options.clock() : host->now(); }
    [[nodiscard]] bool blocked() const noexcept { return options.mode == CameraMode::blocked; }

    [[nodiscard]] std::vector<std::uint8_t> queued(std::uint32_t stream, std::uint64_t timestamp, std::uint32_t flags,
                                                   std::uint32_t size = 0, std::uint64_t sequence = 0) const
    {
        virtio_video_camera_queue_resp response {};
        response.base.hdr = {VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, stream};
        response.base.timestamp = timestamp;
        response.base.flags = flags;
        response.base.data_sizes[0] = size;
        response.sequence = sequence;
        return toBytes(response);
    }

    void complete(std::uint32_t stream, const Held& held, std::uint64_t timestamp, std::uint32_t flags,
                  std::uint32_t size = 0, std::uint64_t sequence = 0)
    {
        auto found = streams.find(stream);
        if (found != streams.end()) {
            const auto resource = found->second.resources.find(held.resource);
            if (resource != found->second.resources.end()) resource->second.queued = false;
        }
        completions.push_back({held.request, queued(stream, timestamp, flags, size, sequence)});
    }

    void event(std::uint32_t camera, std::uint32_t type)
    {
        /* Called from the wake-up path too, where a throw would leave the
         * transport. A full queue already holds unread events, so the new one
         * adds nothing the guest has not been told. */
        if (events.size() >= 64) return;
        events.push_back({type, camera});
        /* The server runs the event queue when the device's event descriptor
         * fires, and the guest's buffer, if it has posted one, is filled. */
        wake();
    }

    [[nodiscard]] Runtime* runtimeOf(std::uint32_t camera) noexcept
    {
        return camera < runtimes.size() ? &runtimes[camera] : nullptr;
    }

    [[nodiscard]] bool blackMode(const Runtime& runtime) const
    {
        return blocked() || runtime.failed || runtime.lost || runtime.interruption != Interruption::none || runtime.resuming ||
            runtime.controls.testPattern(runtime.values);
    }

    [[nodiscard]] static bool down(const Runtime& runtime) noexcept
    {
        return runtime.interruption != Interruption::none || runtime.failed || runtime.lost;
    }

    /* The dequeue flags of a black frame. A capture that is starting is
     * resuming; one that cannot run is interrupted, whether or not it is also
     * starting. */
    [[nodiscard]] std::uint32_t blackFlags(const Runtime& runtime) const
    {
        std::uint32_t flags = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK;
        if (blocked()) flags |= VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED;
        if (down(runtime)) flags |= VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED;
        else if (runtime.resuming) flags |= VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING;
        return flags;
    }

    /* The same state in the metadata record of the frame. */
    [[nodiscard]] std::uint32_t blackMetaFlags(const Runtime& runtime) const
    {
        std::uint32_t flags = VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK;
        if (blocked()) flags |= VIRTIO_VIDEO_CAMERA_FRAME_F_BLOCKED;
        if (down(runtime)) flags |= VIRTIO_VIDEO_CAMERA_FRAME_F_INTERRUPTED;
        else if (runtime.resuming) flags |= VIRTIO_VIDEO_CAMERA_FRAME_F_RESUMING;
        return flags;
    }

    /* The camera cannot run, for a reason that ends by itself. The guest is
     * told once each way: a second interruption, or a capture that fails to
     * start again and again, says nothing new until the camera has been up.
     * Interrupted and unavailable are two states to the guest, so a camera
     * that goes from one to the other is told. */
    void markDown(Runtime& runtime, std::uint32_t eventType)
    {
        runtime.resuming = false;
        const bool unavailable = eventType == VIRTIO_VIDEO_EVENT_CAMERA_UNAVAILABLE;
        if (runtime.told && runtime.toldUnavailable == unavailable) return;
        runtime.told = true;
        runtime.toldUnavailable = unavailable;
        event(runtime.description.index, eventType);
    }

    /* A camera that was told to be down and is up tells the guest so, once. */
    void settle(Runtime& runtime)
    {
        if (!runtime.told || down(runtime)) return;
        runtime.told = false;
        event(runtime.description.index, runtime.toldUnavailable ? VIRTIO_VIDEO_EVENT_CAMERA_AVAILABLE
                                                                 : VIRTIO_VIDEO_EVENT_CAMERA_RESUMED);
    }

    /* The interruptions belong to the session they happened in: when it ends,
     * so do they. */
    void endInterruptions()
    {
        for (auto& runtime : runtimes) {
            runtime.interruption = Interruption::none;
            runtime.resuming = false;
            // A stream that still waits for frames has no capture now: its
            // camera is down until the capture is started again.
            runtime.failed = wantsVideo(runtime);
            settle(runtime);
            updateBlackTimers(runtime);
        }
    }

    [[nodiscard]] bool wantsVideo(const Runtime& runtime) const
    {
        // A lost camera's streams get no capture.
        if (runtime.lost) return false;
        for (const auto& [id, stream] : streams) {
            (void)id;
            if (stream.camera == runtime.description.index && startedVideo(stream)) return true;
        }
        return false;
    }

    [[nodiscard]] bool hasVideoStream(const Runtime& runtime) const
    {
        return std::any_of(streams.begin(), streams.end(), [&](const auto& entry) {
            return entry.second.camera == runtime.description.index && entry.second.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO;
        });
    }

    /* Whether the application's leaving the foreground ends the guest's video on a camera: a live session and a video stream
     * the guest has open (it may be stopped, as a guest suspend stops it). Nothing else decides it: the host cannot tell a
     * recording from a viewfinder, and the guest's camera HAL decides what its client sees (a session with an encoder stream
     * is told the device is gone; any other closes its video streams, which ends the loss, and opens them again). A camera
     * with no video stream, a still or the torch alone, is not lost. */
    [[nodiscard]] bool endsVideoWhenAppLeaves(const Runtime& runtime) const
    {
        return !blocked() && !runtime.lost && hasVideoStream(runtime);
    }

    /* Whether the guest runs a video stream on a camera (a stream it has started and not stopped). */
    [[nodiscard]] bool runsVideo(const Runtime& runtime) const
    {
        return std::any_of(streams.begin(), streams.end(), [&](const auto& entry) {
            return entry.second.camera == runtime.description.index && startedVideo(entry.second);
        });
    }

    /* The guest closed its last video stream of a camera: the loss ends with it, and the camera, if it was told it is down
     * for the loss only, is told it is back. */
    void endLossIfClosed(Runtime& runtime)
    {
        if (!runtime.lost || hasVideoStream(runtime)) return;
        runtime.lost = false;
        settle(runtime);
        updateBlackTimers(runtime);
        reportState();
    }

    // ------------------------------------------------------------------------
    // The platform session
    // ------------------------------------------------------------------------

    bool ensureSession()
    {
        if (blocked()) return false;
        if (session) return true;
        const auto time = now();
        if (sessionFailed && time - lastRetry < sessionRetryNanoseconds) return false;
        lastRetry = time;
        session = host->openSession(*this);
        sessionFailed = session == nullptr;
        if (session) {
            // The format each camera runs in, whatever the guest asks.
            session->setAnchors(anchors, offerRulesOf(limits).limits);
            checkpoint(CameraCheckpoint::sessionOpened);
            // A session that opens runs; what interrupted the last one is over.
            // A camera with a stream waiting for frames starts its capture again
            // on its next turn, which is now.
            for (auto& runtime : runtimes) {
                runtime.interruption = Interruption::none;
                runtime.failed = wantsVideo(runtime);
                runtime.appliedValid = false;
                runtime.videoRunning = false;
                runtime.videoRetry = 0;
                runtime.bufferChecked = false;
                settle(runtime);
                updateBlackTimers(runtime);
            }
        }
        return session != nullptr;
    }

    void closeSession() noexcept
    {
        if (!session) return;
        session->stop();
        session.reset();
        for (auto& runtime : runtimes) {
            runtime.videoRunning = false;
            runtime.appliedValid = false;
        }
        {
            std::lock_guard lock(inboxMutex);
            inboxFrames.clear();
            framesOpen.clear();
            inboxStills.clear();
            inboxEvents.clear();
        }
        checkpoint(CameraCheckpoint::sessionClosed);
    }

    /* A diagnostic reading, never allowed to fail the device (it also runs
     * from the destructor). */
    void checkpoint(CameraCheckpoint where) noexcept
    {
        if (!options.onCheckpoint) return;
        try {
            options.onCheckpoint(where);
        } catch (...) {
        }
    }

    [[nodiscard]] bool sessionNeeded() const
    {
        if (!groups.empty()) return true;
        for (const auto& runtime : runtimes)
            if (runtime.videoRunning || (runtime.appliedValid && runtime.applied.torch)) return true;
        return false;
    }

    void releaseSessionIfIdle()
    {
        if (!session || sessionNeeded()) return;
        closeSession();
        endInterruptions();
    }

    void applySettings(Runtime& runtime, bool force)
    {
        if (!session || blocked()) return;
        const auto settings = runtime.controls.settings(runtime.values);
        if (!force && runtime.appliedValid && settings == runtime.applied) return;
        session->apply(runtime.description.platformIndex, settings);
        runtime.applied = settings;
        runtime.appliedValid = true;
    }

    [[nodiscard]] bool startedVideo(const Stream& stream) const noexcept
    {
        return stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO && stream.started;
    }

    /* Whether a camera holds a capture of the platform now: its video runs or a
     * still it asked for is with the platform. */
    [[nodiscard]] bool holdsCapture(const Runtime& runtime) const
    {
        if (runtime.videoRunning) return true;
        return std::any_of(groups.begin(), groups.end(), [&](const auto& group) {
            return group.fired && group.camera == runtime.description.index;
        });
    }

    /* Whether the camera may hold a capture: it does already, or fewer than
     * `CameraDeviceOptions::use.streams` other cameras do. A camera beyond that
     * is refused as the platform refuses one that conflicts with a running one,
     * whatever the layout the guest was told or the platform would allow. */
    [[nodiscard]] bool admitsCamera(const Runtime& runtime) const
    {
        if (holdsCapture(runtime)) return true;
        std::size_t others = 0;
        for (const auto& other : runtimes)
            if (&other != &runtime && holdsCapture(other)) ++others;
        return others < options.use.streams;
    }

    /* A camera's video is about to run: its frames are kept. */
    void openFramesOf(std::size_t platformCamera)
    {
        std::lock_guard lock(inboxMutex);
        framesOpen.insert(platformCamera);
    }

    /* A camera's video has stopped or is not run: the frames the worker has not
     * taken are given back, and a callback of the platform that is in progress
     * (it has not reached the inbox) finds the camera closed and keeps nothing. */
    void closeFramesOf(std::size_t platformCamera)
    {
        std::lock_guard lock(inboxMutex);
        framesOpen.erase(platformCamera);
        std::erase_if(inboxFrames, [platformCamera](const InboxFrame& frame) { return frame.camera == platformCamera; });
    }

    /* Brings the platform's video for a camera in line with the streams that
     * are running on it, and tells the application whether a camera is in video
     * mode now: whatever the way in or out (the guest's stream, a timer's retry
     * of a start that failed, a session that failed and came back). */
    void reconfigure(Runtime& runtime)
    {
        reconfigureVideo(runtime);
        reportVideoMode(inVideoMode());
    }

    void reconfigureVideo(Runtime& runtime)
    {
        std::uint32_t width = 0, height = 0, rate = 0;
        for (const auto& [id, stream] : streams) {
            (void)id;
            // The streams of a lost camera want no capture.
            if (stream.camera != runtime.description.index || !startedVideo(stream) || runtime.lost) continue;
            width = std::max(width, stream.width);
            height = std::max(height, stream.height);
            rate = std::max(rate, rateOf(stream));
        }
        if (width == 0) {
            if (runtime.videoRunning && session) session->stopVideo(runtime.description.platformIndex);
            // The session may stay open for a still or the torch, so the frames of a camera that stopped are given back
            // here and not by the session's end, and its callbacks that are still running keep nothing.
            closeFramesOf(runtime.description.platformIndex);
            runtime.videoRunning = false;
            runtime.runWidth = runtime.runHeight = runtime.runRate = 0;
            runtime.failed = false;
            runtime.resuming = false;
            settle(runtime);
            return;
        }
        if (blocked()) return;
        if (!ensureSession()) {
            runtime.failed = true;
            markDown(runtime, VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
            return;
        }
        if (runtime.videoRunning && runtime.runWidth == width && runtime.runHeight == height && runtime.runRate == rate) return;
        const bool wasFailed = runtime.failed;
        const bool wasRunning = runtime.videoRunning;
        bool ok = false;
        if (admitsCamera(runtime)) {
            openFramesOf(runtime.description.platformIndex);
            // The settings first: the platform programs the stabilization the guest asks for on the new video connection
            // before the capture can deliver a frame, so the first picture has the crop the rest will have. (Applied after
            // the start, the picture would change its crop under a guest that already shows it.)
            applySettings(runtime, true);
            ok = session->startVideo(runtime.description.platformIndex, width, height, rate);
        }
        if (ok) {
            runtime.runWidth = width;
            runtime.runHeight = height;
            runtime.runRate = rate;
        } else if (!wasRunning) {
            closeFramesOf(runtime.description.platformIndex);
        }
        // What the platform runs is charged until it has stopped, whatever the streams want: a change it refused leaves
        // the capture it was to replace running (the platform's rollback keeps it), and the stream's end stops it.
        runtime.videoRunning = ok || wasRunning;
        runtime.failed = !ok;
        runtime.videoRetry = now();
        if (ok) runtime.bufferChecked = false;
        if (ok) {
            // The capture has to deliver before the stream has a picture of its own.
            runtime.resuming = true;
            applySettings(runtime, true);
            // A capture that failed is not told to be back until it delivers
            // (pump): a start that fails again and again would otherwise tell
            // the guest at every try.
            if (!wasFailed) settle(runtime);
        } else {
            markDown(runtime, VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
        }
    }

    [[nodiscard]] static std::uint32_t rateOf(const Stream& stream) noexcept
    {
        return std::max<std::uint32_t>(1, (stream.intervalDenominator + stream.intervalNumerator / 2) / stream.intervalNumerator);
    }

    /* Whether the guest has a camera in video mode (`CameraDeviceOptions::onVideoMode`): its video runs, the camera is not
     * interrupted or failing, and the guest's last applied settings ask for the picture stabilized. A blocked session
     * runs no video. */
    [[nodiscard]] bool inVideoMode() const noexcept
    {
        if (blocked()) return false;
        return std::any_of(runtimes.begin(), runtimes.end(), [](const Runtime& runtime) {
            return runtime.videoRunning && !runtime.failed && runtime.interruption == Interruption::none && runtime.appliedValid &&
                runtime.applied.stabilization;
        });
    }

    void reportVideoMode(bool inMode) noexcept
    {
        if (inMode == reportedVideoMode) return;
        reportedVideoMode = inMode;
        try {
            if (options.onVideoMode) options.onVideoMode(inMode);
        } catch (...) {
            // The application's use of it must not stop the device.
        }
    }

    void reportState()
    {
        reportVideoMode(inVideoMode());
        bool active = !groups.empty();
        bool interrupted = false, multitasking = false, pressure = false;
        for (const auto& runtime : runtimes) {
            active |= runtime.videoRunning;
            if (runtime.interruption == Interruption::multitasking) multitasking = true;
            if (runtime.interruption == Interruption::pressure) pressure = true;
            if (runtime.interruption == Interruption::interrupted || runtime.failed || runtime.lost) interrupted = true;
        }
        bool blockedUse = false;
        for (const auto& [id, stream] : streams) {
            (void)id;
            active |= startedVideo(stream);
            // A video stream that runs, or a still that was asked for. A
            // metadata stream is queued for every camera the guest opens and
            // does not say a client is using it.
            blockedUse |= stream.started && stream.kind != VIRTIO_VIDEO_CAMERA_STREAM_META;
        }
        // A blocked session never captures, so nothing is "streaming" however many
        // black frames the guest's streams get; the application hears that a
        // guest app has a camera open (blockedInUse), once for each open.
        CameraState state = blocked() ? (blockedUse ? CameraState::blockedInUse : CameraState::idle)
            : !active ? CameraState::idle
            : multitasking ? CameraState::interruptedByMultitasking
            : pressure ? CameraState::interruptedBySystemPressure
            : interrupted ? CameraState::interrupted : CameraState::streaming;
        if (state == reported) return;
        reported = state;
        if (options.onState) options.onState(state);
    }

    // ------------------------------------------------------------------------
    // The platform's callbacks
    // ------------------------------------------------------------------------

    void wake()
    {
        struct kevent change {};
        EV_SET(&change, userIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
    }

    void cameraFrame(std::size_t camera, CameraFrame frame) override
    {
        {
            std::lock_guard lock(inboxMutex);
            // A frame of a camera whose video has stopped is a callback that was
            // in progress when it did: the capture is not counted any more, so
            // its frame is not kept.
            if (!framesOpen.contains(camera)) return;
            // Frames beyond a few are old by the time the worker takes them,
            // and each holds the capture's buffer: keep the newest.
            std::size_t held = 0;
            for (const auto& entry : inboxFrames) held += entry.camera == camera;
            if (held >= inboxFrameLimit) {
                const auto oldest = std::find_if(inboxFrames.begin(), inboxFrames.end(),
                                                 [camera](const auto& entry) { return entry.camera == camera; });
                inboxFrames.erase(oldest);
            }
            inboxFrames.push_back({camera, std::move(frame), inboxSerial});
        }
        wake();
    }
    void cameraStill(std::size_t camera, CameraStillResult result) override
    {
        {
            std::lock_guard lock(inboxMutex);
            inboxStills.push_back({camera, std::move(result)});
        }
        wake();
    }
    void cameraEvent(CameraEvent platformEvent) override
    {
        {
            std::lock_guard lock(inboxMutex);
            inboxEvents.push_back({platformEvent, ++inboxSerial});
        }
        wake();
    }
    /* What the platform says of how it presents a camera goes to the owner's record as it comes, under the guest's number
     * of the camera, on the platform's thread: it never touches the device's state (`wireOfPlatform` is fixed once the
     * device is made), and a camera the guest does not see is not told. */
    void cameraPresentation(std::size_t camera, const uml::platform::CameraPresentation& presentation) override
    {
        if (!options.onCameraPresentation) return;
        const auto wire = wireOfPlatform.find(camera);
        if (wire == wireOfPlatform.end()) return;
        uml::platform::CameraPresentation told = presentation;
        told.camera = wire->second;
        // What the guest is told is the description's orientation, which the application may have turned; the platform's own
        // is kept as the second number.
        told.platformStated = presentation.stated;
        if (const auto guest = guestOrientationOfPlatform.find(camera); guest != guestOrientationOfPlatform.end())
            told.stated = std::uint16_t(guest->second);
        if (presentation.photoKnown) {
            // The newest reading of the photo output's compensation, for a still the platform refuses before it has one.
            std::lock_guard lock(compensationMutex);
            compensationOf[camera] = {presentation.compensationSupported, presentation.compensationEnabled, presentation.compensationReading};
        }
        try {
            options.onCameraPresentation(told);
        } catch (...) {
            // The owner's record must not fail the platform's capture.
        }
    }

    // ------------------------------------------------------------------------
    // Exposures and their metadata
    // ------------------------------------------------------------------------

    [[nodiscard]] virtio_video_camera_meta metaOf(const Runtime& runtime, std::uint64_t sequence, const CameraExposure& exposure,
                                                  std::uint32_t flags) const
    {
        virtio_video_camera_meta meta {};
        meta.sequence = sequence;
        meta.exposure_ns = exposure.exposureNanoseconds;
        meta.frame_duration_ns = exposure.frameDurationNanoseconds;
        meta.iso = exposure.iso;
        meta.flags = flags;
        const bool continuous = runtime.controls.settings(runtime.values).focusMode == uml::platform::CameraFocusMode::continuousAuto;
        meta.ae_state = aeState(exposure.ae);
        meta.af_state = afState(exposure.af, continuous);
        meta.awb_state = awbState(exposure.awb);
        meta.lens_position_q16 = q16(exposure.lensPositionDioptres);
        for (std::size_t index = 0; index < 3; ++index) meta.wb_gain_q16[index] = q16(exposure.whiteBalanceGains[index]);
        meta.color_temperature_k = exposure.colorTemperature;
        meta.tint_q16 = q16(exposure.tint);
        meta.zoom_ratio_q16 = q16(exposure.zoomRatio);
        const auto active = wireOfPlatform.find(exposure.activeCamera);
        meta.active_camera = active != wireOfPlatform.end() ? active->second : runtime.description.index;
        meta.aperture_x100 = std::uint32_t(std::lround(exposure.aperture * 100.0f));
        meta.focal_length_um = exposure.focalLengthMillimetres > 0
            ? std::uint32_t(std::lround(exposure.focalLengthMillimetres * 1000.0f)) : runtime.description.wire.focal_length_um;
        // Android's FLASH_STATE: unavailable, ready, fired.
        meta.flash_state = exposure.flashFired ? 3 : runtime.description.info.flash ? 2 : 0;
        for (std::size_t index = 0; index < 3; ++index) meta.neutral_point_q16[index] = q16(exposure.neutralPoint[index]);
        for (std::size_t index = 0; index < 8; ++index) meta.noise_profile_f32[index] = floatBits(exposure.noiseProfile[index]);
        return meta;
    }

    void pushMeta(Runtime& runtime, const virtio_video_camera_meta& meta, std::uint64_t timestamp)
    {
        for (auto& [id, stream] : streams) {
            if (stream.camera != runtime.description.index || stream.kind != VIRTIO_VIDEO_CAMERA_STREAM_META || !stream.started)
                continue;
            stream.backlog.push_back({meta, timestamp});
            while (stream.backlog.size() > metaBacklogLimit) stream.backlog.pop_front();
            serveMeta(id, stream);
        }
    }

    void serveMeta(std::uint32_t id, Stream& stream)
    {
        while (!stream.queued.empty() && !stream.backlog.empty()) {
            const auto held = stream.queued.front();
            stream.queued.pop_front();
            const auto entry = stream.backlog.front();
            stream.backlog.pop_front();
            auto& resource = stream.resources.at(held.resource);
            auto bytes = toBytes(entry.meta);
            const bool ok = resource.capacity >= bytes.size() && !resource.planes.empty() &&
                transferSegments(mapper, regions.get(), resource.planes[0], bytes, true);
            const auto time = now();
            const auto age = entry.timestamp != 0 && time > entry.timestamp ? time - entry.timestamp : 0;
            complete(id, held, age, ok ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, ok ? std::uint32_t(bytes.size()) : 0,
                     entry.meta.sequence);
            if (auto* runtime = runtimeOf(stream.camera); runtime && runtime->lossRecord != 0 && entry.meta.sequence == runtime->lossRecord)
                runtime->lossRecordCarriers.emplace_back(id, held.resource);
        }
    }

    /* Whether a started meta stream of the camera is there to carry its records. */
    [[nodiscard]] bool hasStartedMeta(const Runtime& runtime) const
    {
        return std::any_of(streams.begin(), streams.end(), [&](const auto& entry) {
            return entry.second.camera == runtime.description.index && entry.second.kind == VIRTIO_VIDEO_CAMERA_STREAM_META &&
                entry.second.started;
        });
    }

    /* Whether the buffers of a lost camera's video may fail now. The camera's interrupted record goes to the guest first,
     * and they fail once the guest has queued again the meta buffer that carried it (it has read it) or
     * `lossRecordReadNanoseconds` have passed, so that the first failure the guest meets is the loss: a buffer that fails
     * before the guest has read the record is taken for an ordinary failure, and the loss is found only a request later.
     * A camera with no meta stream running has no reader to wait for. */
    [[nodiscard]] bool lossRecordDelivered(Runtime& runtime)
    {
        if (runtime.lossRecordRead) return true;
        if (runtime.lossRecord == 0) {
            if (!hasStartedMeta(runtime)) {
                runtime.lossRecordRead = true;
                return true;
            }
            const auto sequence = ++runtime.sequence;
            CameraExposure record;
            record.activeCamera = std::uint32_t(runtime.description.platformIndex);
            runtime.lossRecord = sequence;
            runtime.lossRecordAt = now();
            pushMeta(runtime, metaOf(runtime, sequence, record, blackMetaFlags(runtime)), now());
            return false;
        }
        if (now() - runtime.lossRecordAt < lossRecordReadNanoseconds) return false;
        runtime.lossRecordRead = true;
        std::fprintf(stderr, "camera %u: the guest did not read the interrupted record of the lost video in %llu ms\n",
                     unsigned(runtime.description.index), static_cast<unsigned long long>(lossRecordReadNanoseconds / 1'000'000));
        return true;
    }

    // ------------------------------------------------------------------------
    // Video
    // ------------------------------------------------------------------------

    [[nodiscard]] static std::uint32_t pitchOf(const Stream& stream) noexcept { return rawPitch(stream.width); }
    [[nodiscard]] static std::uint32_t videoPlaneSize(const Stream& stream) noexcept
    {
        return pitchOf(stream) * (stream.height + stream.height / 2);
    }

    /* A picture of the camera's size and orientation goes into the guest's
     * rows as it is, with no copy of its own in between. */
    bool copyRows(const Resource& resource, const Stream& stream, const CameraFrame& frame, uml::platform::Nv12Rect crop)
    {
        const auto pitch = pitchOf(stream);
        uml::video::GuestPlaneWriter writer(mapper, regions.get(), resource.planes[0]);
        for (std::uint32_t row = 0; row < crop.height; ++row) {
            const std::span<const std::uint8_t> data(frame.luma.data() + std::size_t(crop.y + row) * frame.lumaStride + crop.x, crop.width);
            if (!writer.write(std::uint64_t(row) * pitch, data)) return false;
        }
        const std::uint64_t chromaStart = std::uint64_t(pitch) * stream.height;
        for (std::uint32_t row = 0; row < crop.height / 2; ++row) {
            const std::span<const std::uint8_t> data(frame.chroma.data() + std::size_t(crop.y / 2 + row) * frame.chromaStride + crop.x,
                                                     crop.width);
            if (!writer.write(chromaStart + std::uint64_t(row) * pitch, data)) return false;
        }
        return true;
    }

    /* The same for a picture that goes in turned half around: each row of the guest's buffer is a row of the picture
     * reversed, taken from the last row up, so the turn costs no pass of its own and no picture of memory, only the
     * one row the reversed samples wait in before they are written. */
    bool copyRowsHalfTurned(const Resource& resource, const Stream& stream, const CameraFrame& frame, uml::platform::Nv12Rect crop)
    {
        const auto pitch = pitchOf(stream);
        uml::video::GuestPlaneWriter writer(mapper, regions.get(), resource.planes[0]);
        turnedRow.resize(crop.width);
        for (std::uint32_t row = 0; row < crop.height; ++row) {
            const auto* from = frame.luma.data() + std::size_t(crop.y + crop.height - 1 - row) * frame.lumaStride + crop.x;
            uml::platform::reverseLumaRow(from, turnedRow.data(), crop.width);
            if (!writer.write(std::uint64_t(row) * pitch, std::span<const std::uint8_t>(turnedRow.data(), crop.width))) return false;
        }
        const std::uint64_t chromaStart = std::uint64_t(pitch) * stream.height;
        for (std::uint32_t row = 0; row < crop.height / 2; ++row) {
            const auto* from = frame.chroma.data() + std::size_t(crop.y / 2 + crop.height / 2 - 1 - row) * frame.chromaStride + crop.x;
            uml::platform::reverseChromaRow(from, turnedRow.data(), crop.width / 2);
            if (!writer.write(chromaStart + std::uint64_t(row) * pitch, std::span<const std::uint8_t>(turnedRow.data(), crop.width))) return false;
        }
        return true;
    }

    /* Writes the camera's frame into the guest's buffer of `stream`, turned clockwise by `rotation` (the camera's half turn
     * and ROTATE_AND_CROP's, added): a picture of the stream's size goes in as it is, or reversed, with no copy of its own
     * in between; any other goes through the transformer. */
    bool writeVideo(const Stream& stream, Resource& resource, const CameraFrame* frame, std::uint32_t rotation)
    {
        const auto pitch = pitchOf(stream);
        const auto size = videoPlaneSize(stream);
        if (resource.planes.empty() || resource.capacity < size) return false;
        staging.resize(size);
        uml::platform::Nv12Target target {staging.data(), pitch, staging.data() + std::size_t(pitch) * stream.height, pitch,
                                          stream.width, stream.height};
        if (frame == nullptr) {
            uml::platform::fillNv12Black(target);
            return transferSegments(mapper, regions.get(), resource.planes[0], staging, true);
        }
        if (frame->luma.size() < frame->lumaStride * frame->height || frame->chroma.size() < frame->chromaStride * (frame->height / 2))
            return false;
        const uml::platform::Nv12Source source {frame->luma.data(), frame->lumaStride, frame->chroma.data(), frame->chromaStride,
                                                frame->width, frame->height, frame->native};
        const bool quarter = rotation == 90 || rotation == 270;
        const auto crop = uml::platform::centredCrop(frame->width, frame->height, stream.width, stream.height, quarter);
        if (crop.width == 0) return false;
        const bool sameSize = crop.width == stream.width && crop.height == stream.height;
        if (sameSize && (rotation == 0 || rotation == 180)) {
            // The picture is the buffer's, or its half turn: the rows go straight in.
            if (!uml::platform::validNv12Transform(source, crop, rotation, target)) return false;
            return rotation == 0 ? copyRows(resource, stream, *frame, crop) : copyRowsHalfTurned(resource, stream, *frame, crop);
        }
        if (!transformer->transform(source, crop, rotation, target)) return false;
        return transferSegments(mapper, regions.get(), resource.planes[0], staging, true);
    }

    /* Tells `onFlash` what the controls ask of the camera's light when it
     * differs from what it was told last. */
    void reportLight(Runtime& runtime, bool torch) const
    {
        const auto flash = runtime.controls.stillFlash(runtime.values);
        if (!options.onFlash || (torch == runtime.reportedTorch && flash == runtime.reportedFlash)) return;
        runtime.reportedTorch = torch;
        runtime.reportedFlash = flash;
        options.onFlash(runtime.description.index, torch, flash);
    }

    /* The half turn the platform says every picture of the camera takes before the guest sees it (CameraInfo): the device
     * turns the video by it, whatever else it is asked. Only a half turn or none. */
    [[nodiscard]] static std::uint32_t presentationTurnOf(const Runtime& runtime) noexcept
    {
        return runtime.description.info.presentationTurn == 180 ? 180u : 0u;
    }

    [[nodiscard]] std::uint32_t rotationOf(Runtime& runtime) const
    {
        const auto setting = runtime.controls.rotateAndCrop(runtime.values);
        std::uint32_t degrees = 0;
        if (setting >= 1 && setting <= 3)
            degrees = setting * 90;
        else if (setting == 4 && options.automaticRotation)
            degrees = options.automaticRotation(runtime.description.info) % 360 / 90 * 90;
        if (options.onRotateAndCrop && (setting != runtime.reportedSetting || degrees != runtime.reportedTurn)) {
            runtime.reportedSetting = setting;
            runtime.reportedTurn = degrees;
            options.onRotateAndCrop(runtime.description.index, setting, degrees);
        }
        return degrees;
    }

    void deliverFrame(Runtime& runtime, const CameraFrame& frame)
    {
        if (!runtime.bufferChecked) {
            runtime.bufferChecked = true;
            std::fprintf(stderr, "%s\n",
                         bufferCheckLine(runtime.description.index, frame.width, frame.height,
                                         frame.luma.size() + frame.chroma.size(), frame.lumaStride,
                                         maximumDeliveredFrameBytes(limits)).c_str());
        }
        const auto sequence = ++runtime.sequence;
        runtime.latest = frame.exposure;
        const auto time = now();
        const auto age = time > frame.timestampNanoseconds ? time - frame.timestampNanoseconds : 0;
        // The camera's half turn, if it has one, and ROTATE_AND_CROP's turn, in one rotation.
        const auto rotation = (rotationOf(runtime) + presentationTurnOf(runtime)) % 360;
        for (auto& [id, stream] : streams) {
            if (stream.camera != runtime.description.index || !startedVideo(stream)) continue;
            // A stream slower than the camera takes the frames that fall on its interval.
            const std::uint64_t interval = 1'000'000'000ull * stream.intervalNumerator / stream.intervalDenominator;
            if (stream.delivered && frame.timestampNanoseconds + interval / 4 < stream.lastDelivered + interval) continue;
            if (stream.queued.empty()) {
                ++stream.framesDropped;
                continue;
            }
            const auto held = stream.queued.front();
            stream.queued.pop_front();
            auto& resource = stream.resources.at(held.resource);
            const bool ok = writeVideo(stream, resource, &frame, rotation);
            stream.delivered = true;
            stream.lastDelivered = frame.timestampNanoseconds;
            ++stream.framesOut;
            complete(id, held, age, ok ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
                     ok ? videoPlaneSize(stream) : 0, sequence);
        }
        pushMeta(runtime, metaOf(runtime, sequence, frame.exposure, 0), frame.timestampNanoseconds);
    }

    void blackTick(std::uint32_t id)
    {
        const auto found = streams.find(id);
        if (found == streams.end()) return;
        auto& stream = found->second;
        auto* runtime = runtimeOf(stream.camera);
        if (!runtime || !startedVideo(stream)) return;
        if (!blackMode(*runtime)) {
            disarm(stream);
            return;
        }
        // A capture that could not start is tried again, once a second, whether
        // the session could not open or the camera could not join it.
        if (!blocked() && runtime->failed && now() - runtime->videoRetry >= sessionRetryNanoseconds) reconfigure(*runtime);
        if (!blackMode(*runtime)) {
            disarm(stream);
            return;
        }
        if (stream.queued.empty()) return;
        if (runtime->lost && !lossRecordDelivered(*runtime)) return;
        const auto held = stream.queued.front();
        stream.queued.pop_front();
        auto& resource = stream.resources.at(held.resource);
        const auto size = videoPlaneSize(stream);
        // A lost camera's buffer fails, with the interrupted flag, and the camera's black record says why; the record
        // comes first, so that the guest finds it when it sees the failure.
        const bool lost = runtime->lost;
        bool ok = false;
        if (!lost && !resource.planes.empty() && resource.capacity >= size) ok = writeBlack(stream, resource);
        const auto sequence = ++runtime->sequence;
        const std::uint32_t metaFlags = blackMetaFlags(*runtime);
        ++stream.framesOut;
        CameraExposure black;
        black.activeCamera = std::uint32_t(runtime->description.platformIndex);
        if (lost) pushMeta(*runtime, metaOf(*runtime, sequence, black, metaFlags), now());
        complete(id, held, 0,
                 lost ? std::uint32_t(VIRTIO_VIDEO_DEQUEUE_FLAG_ERR) | std::uint32_t(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED)
                      : (ok ? blackFlags(*runtime) : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR),
                 ok ? size : 0, sequence);
        if (!lost) pushMeta(*runtime, metaOf(*runtime, sequence, black, metaFlags), now());
    }

    /* Writes a black picture to a guest's buffer: the luma plane and then the
     * chroma plane, each from the chunk of black the device keeps, so a picture
     * of any size, and any number of streams, hold nothing of their own. */
    [[nodiscard]] bool writeBlack(const Stream& stream, const Resource& resource)
    {
        if (blackLuma.empty()) {
            // A black picture of full-range BT.709 (`fillNv12Black`): luma 0 and neutral chroma.
            blackLuma.assign(blackChunkBytes, 0);
            blackChroma.assign(blackChunkBytes, 128);
        }
        const std::uint64_t lumaBytes = std::uint64_t(pitchOf(stream)) * stream.height;
        const std::uint64_t total = lumaBytes + lumaBytes / 2;
        for (std::uint64_t offset = 0; offset < total;) {
            const bool inLuma = offset < lumaBytes;
            const auto& chunk = inLuma ? blackLuma : blackChroma;
            const std::uint64_t end = inLuma ? lumaBytes : total;
            const std::size_t length = std::size_t(std::min<std::uint64_t>(chunk.size(), end - offset));
            auto data = std::span<std::uint8_t>(const_cast<std::uint8_t*>(chunk.data()), length);
            if (!transferSegments(mapper, regions.get(), resource.planes[0], data, true, offset)) return false;
            offset += length;
        }
        return true;
    }

    void arm(Stream& stream)
    {
        const std::uint64_t microseconds = std::max<std::uint64_t>(1000, 1'000'000ull * stream.intervalNumerator / stream.intervalDenominator);
        struct kevent change {};
        EV_SET(&change, stream.id, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_USECONDS, static_cast<intptr_t>(microseconds), nullptr);
        if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) == 0) stream.timerArmed = true;
    }

    void disarm(Stream& stream) noexcept
    {
        if (!stream.timerArmed) return;
        struct kevent change {};
        EV_SET(&change, stream.id, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
        ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
        stream.timerArmed = false;
    }

    /* Starts the black-frame timer of each started video stream of a camera
     * whose frames are black, and stops it where they are not. */
    void updateBlackTimers(Runtime& runtime)
    {
        const bool black = blackMode(runtime);
        for (auto& [id, stream] : streams) {
            (void)id;
            if (stream.camera != runtime.description.index || stream.kind != VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) continue;
            if (black && stream.started && !stream.timerArmed) arm(stream);
            else if ((!black || !stream.started) && stream.timerArmed) disarm(stream);
        }
    }

    // ------------------------------------------------------------------------
    // Stills
    // ------------------------------------------------------------------------

    [[nodiscard]] Group* groupOfToken(std::uint64_t token)
    {
        const auto found = std::find_if(groups.begin(), groups.end(), [token](const auto& group) { return group.fired && group.token == token; });
        return found == groups.end() ? nullptr : &*found;
    }

    void failGroup(Group& group, std::uint32_t extra)
    {
        for (const auto& member : group.members) complete(member.stream, member.held, member.held.cookie, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR | extra);
    }

    /* Tells the owner why a capture did not end in a picture
     * (`CameraDeviceOptions::onStillFailure`): `failure` has the stage and what
     * the platform or the device saw of the file, and gets the camera and the
     * JPEG size the guest asked for here. Call it before the group's members
     * are completed or dropped. */
    void reportStillFailure(const Group& group, const Runtime& runtime, uml::platform::CameraStillFailure failure)
    {
        if (!options.onStillFailure) return;
        failure.camera = runtime.description.index;
        failure.position = runtime.description.info.position;
        if (failure.compensationSupported == uml::platform::CameraFact::unknown) {
            // A still refused before the platform had a photo to read it from says what the output last read.
            std::lock_guard lock(compensationMutex);
            if (const auto found = compensationOf.find(runtime.description.platformIndex); found != compensationOf.end()) {
                failure.compensationSupported = found->second.supported;
                failure.compensationEnabled = found->second.enabled;
                failure.compensationReading = found->second.reading;
            }
        }
        for (const auto& member : group.members) {
            const auto found = streams.find(member.stream);
            if (found == streams.end() || found->second.format != VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) continue;
            failure.askedWidth = found->second.width;
            failure.askedHeight = found->second.height;
        }
        try {
            options.onStillFailure(failure);
        } catch (...) {
            // The owner's record must not fail a capture.
        }
    }

    [[nodiscard]] static uml::platform::CameraStillFailure failureAt(uml::platform::CameraStillStage stage) noexcept
    {
        uml::platform::CameraStillFailure failure;
        failure.stage = stage;
        return failure;
    }

    /* Whether an interruption or a recovery of its camera ended a capture: not a fault of the capture, so it is not
     * reported (`reportStillFailure`), whether the platform ended it (`deliverStill`) or its time did (`expireGroups`).
     * A capture that has not started has no serial to span a boundary with: only an interruption that is on counts. */
    [[nodiscard]] static bool endedByInterruption(const Group& group, const Runtime& runtime) noexcept
    {
        return runtime.interruption != Interruption::none || (group.fired && group.fireSerial < runtime.boundary);
    }

    /* A capture that ends makes room for the captures that wait for it. */
    void eraseGroup(std::uint64_t token)
    {
        std::erase_if(groups, [token](const auto& group) { return group.fired && group.token == token; });
        fireWaiting();
    }

    /* One timer, set for the earliest deadline a group has: a group that is
     * still gathering its buffers, one that waits for room or has a capture with
     * the platform, and one the guest gave up on, which is held to the platform's
     * own end of the capture. */
    void armGroupTimer()
    {
        struct kevent change {};
        if (groups.empty()) {
            EV_SET(&change, groupTimerIdent, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
            ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
            return;
        }
        std::uint64_t earliest = ~std::uint64_t {};
        for (const auto& group : groups) earliest = std::min(earliest, group.deadline);
        const auto time = now();
        const std::uint64_t wait = earliest > time ? earliest - time : 0;
        EV_SET(&change, groupTimerIdent, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_USECONDS,
               static_cast<intptr_t>(std::max<std::uint64_t>(1000, wait / 1000)), nullptr);
        ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr);
    }

    /* A still buffer arrived. A buffer without GROUP_MORE completes a group of
     * its cookie and starts the capture. */
    void queueStill(Stream& stream, Runtime& runtime, const Held& held)
    {
        stream.started = true;
        std::uint32_t refusal = 0;
        if (blocked()) refusal = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED | VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK;
        else if (runtime.interruption != Interruption::none) refusal = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED;
        if (refusal != 0) {
            complete(stream.id, held, held.cookie, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR | refusal);
            // A blocked session's still is the guest's use of a camera too.
            if (blocked()) reportState();
            return;
        }
        const bool more = (held.flags & VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE) != 0 && held.cookie != 0;
        auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
            return !group.fired && group.camera == runtime.description.index && group.cookie == held.cookie && held.cookie != 0;
        });
        if (found == groups.end()) {
            Group group;
            group.camera = runtime.description.index;
            group.cookie = held.cookie;
            group.deadline = now() + groupTimeoutNanoseconds;
            groups.push_back(std::move(group));
            found = groups.end() - 1;
            armGroupTimer();
        }
        found->members.push_back({stream.id, held});
        if (more) return;
        admit(*found, runtime);
    }

    /* Whether the first still of this lens also teaches its colour data: it has
     * RAW stills, the store has none of its colour data yet, the capture is at
     * unit zoom and it has not been tried three times. The capture then asks for
     * a RAW with the JPEG, whatever the layout lists. */
    [[nodiscard]] bool teachesColour(const Runtime& runtime) const
    {
        return runtime.description.info.rawStills && !runtime.description.calibration && runtime.calibrationAttempts < 3 &&
            std::fabs(runtime.controls.settings(runtime.values).zoomRatio - 1.0f) < 1e-3f;
    }

    /* What a group's capture holds while it is with the host
     * (`jpegCaptureBytes`, `rawCaptureBytes`), for the sizes of its pictures and the camera's working storage. */
    [[nodiscard]] std::uint64_t costOf(const Group& group, const Runtime& runtime) const
    {
        bool jpeg = false, raw = false;
        std::uint64_t jpegPixels = 0, rawPixels = 0;
        for (const auto& member : group.members) {
            const auto& stream = streams.at(member.stream);
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                jpeg = true;
                jpegPixels = std::uint64_t(stream.width) * stream.height;
            } else {
                raw = true;
                rawPixels = std::uint64_t(stream.width) * stream.height;
            }
        }
        if (!raw && teachesColour(runtime)) {
            raw = true;
            const auto& size = runtime.description.info.rawStillSize;
            rawPixels = size.width != 0 ? std::uint64_t(size.width) * size.height : limits.maximumStillPixels;
        }
        // The still of a camera whose pictures the host presents half around is turned and encoded by the host, which holds
        // its encoder's buffers besides.
        const std::uint32_t turned = runtime.description.info.presentationTurn == 180 ? host->turnedStillWorkingBytesPerPixel() : 0;
        return (jpeg ? jpegCaptureBytes(jpegPixels, options.use.stillWorkingBytesPerPixel + turned) : 0) +
            (raw ? rawCaptureBytes(rawPixels) : 0);
    }

    /* What the captures with the platform cost together. */
    [[nodiscard]] std::uint64_t capturesCost() const
    {
        std::uint64_t cost = 0;
        for (const auto& group : groups)
            if (group.fired) cost += group.cost;
        return cost;
    }

    /* Whether a waiting group is older than `group`. */
    [[nodiscard]] bool olderWaits(const Group& group) const
    {
        return std::any_of(groups.begin(), groups.end(), [&](const auto& other) { return other.waiting && other.whole < group.whole; });
    }

    /* A whole group's capture starts at once if the captures the device holds
     * leave room for it and no older group waits; otherwise it waits, and the
     * groups start in the order they became whole. Its time is the capture's
     * from the request, whether it waits or runs. A capture that costs more than
     * the device admits at all is refused before it starts. */
    void admit(Group& group, Runtime& runtime)
    {
        group.cost = costOf(group, runtime);
        group.whole = ++wholeCount;
        group.deadline = now() + captureTimeoutNanoseconds;
        if (group.cost > stillBudget) {
            reportStillFailure(group, runtime, failureAt(uml::platform::CameraStillStage::refused));
            failGroup(group, 0);
            const auto order = group.whole;
            std::erase_if(groups, [order](const auto& other) { return other.whole == order; });
            armGroupTimer();
            releaseSessionIfIdle();
            reportState();
            return;
        }
        const auto held = capturesCost();
        if (olderWaits(group) || (held != 0 && held + group.cost > stillBudget)) {
            group.waiting = true;
            armGroupTimer();
            return;
        }
        fire(group, runtime);
    }

    void fireWaiting()
    {
        if (firingWaiting) return;
        firingWaiting = true;
        bool refused = false;
        for (;;) {
            // The oldest group that waits goes first.
            auto next = groups.end();
            for (auto group = groups.begin(); group != groups.end(); ++group)
                if (group->waiting && (next == groups.end() || group->whole < next->whole)) next = group;
            if (next == groups.end()) break;
            auto* runtime = runtimeOf(next->camera);
            if (runtime == nullptr) {
                failGroup(*next, 0);
                groups.erase(next);
                continue;
            }
            // Members may have gone while it waited.
            if (next->members.empty()) {
                groups.erase(next);
                continue;
            }
            next->cost = costOf(*next, *runtime);
            // A control changed while it waited (a zoom back to 1 adds the colour data of an uncalibrated lens): the capture
            // may now cost more than the device admits at all, which is refused as it is at the request.
            if (next->cost > stillBudget) {
                reportStillFailure(*next, *runtime, failureAt(uml::platform::CameraStillStage::refused));
                failGroup(*next, 0);
                groups.erase(next);
                refused = true;
                continue;
            }
            const auto held = capturesCost();
            if (held != 0 && held + next->cost > stillBudget) break;
            next->waiting = false;
            fire(*next, *runtime);
        }
        firingWaiting = false;
        armGroupTimer();
        if (refused) {
            releaseSessionIfIdle();
            reportState();
        }
    }

    /* The captures a camera has with the platform. */
    [[nodiscard]] std::size_t capturesInFlight(std::uint32_t camera) const
    {
        return std::size_t(std::count_if(groups.begin(), groups.end(),
                                         [camera](const auto& group) { return group.fired && group.camera == camera; }));
    }

    void fire(Group& group, Runtime& runtime)
    {
        CameraStillRequest request;
        std::uint32_t refusal = 0;
        bool jpeg = false, raw = false, calibrationRaw = false;
        for (const auto& member : group.members) {
            const auto& stream = streams.at(member.stream);
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                if (jpeg) refusal = 1;
                jpeg = true;
                request.jpegWidth = stream.width;
                request.jpegHeight = stream.height;
                request.maximumJpegBytes = maximumJpegBytes(std::uint64_t(stream.width) * stream.height);
            } else {
                if (raw) refusal = 1;
                raw = true;
                request.raw = true;
            }
        }
        request.jpegQuality = runtime.controls.jpegQuality(runtime.values);
        request.flash = runtime.controls.stillFlash(runtime.values);
        // The first still of an uncalibrated lens also teaches its colour data.
        if (!raw && teachesColour(runtime)) {
            request.raw = true;
            request.speedPrioritized = true;
            calibrationRaw = true;
            ++runtime.calibrationAttempts;
        }
        if (raw) request.speedPrioritized = true;
        // A guest holds at most a pipeline's worth of captures with the platform.
        if (capturesInFlight(runtime.description.index) >= limits.buffers) refusal = 1;
        // A camera beyond the cameras the device lets hold a capture at once is refused.
        if (refusal == 0 && !admitsCamera(runtime)) refusal = 1;
        if (refusal == 0 && !ensureSession()) refusal = 1;
        group.token = request.token = nextToken++;
        group.fired = true;
        {
            std::lock_guard lock(inboxMutex);
            group.fireSerial = inboxSerial;
        }
        if (refusal == 0) {
            applySettings(runtime, false);
            bool taken = session->takeStill(runtime.description.platformIndex, request);
            if (!taken && calibrationRaw) {
                // The colour data is a courtesy: the person's photo does not
                // wait for it, so it is asked for again without it.
                request.raw = request.speedPrioritized = false;
                taken = session->takeStill(runtime.description.platformIndex, request);
            }
            if (!taken) refusal = 1;
        }
        if (refusal != 0) {
            reportStillFailure(group, runtime, failureAt(uml::platform::CameraStillStage::refused));
            failGroup(group, sessionFailed ? std::uint32_t(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED) : 0u);
            eraseGroup(group.token);
            releaseSessionIfIdle();
        } else {
            armGroupTimer();
        }
        reportState();
    }

    /* Gives the memory of a result back, the capacity and not only the size: the
     * capture's cost is spent again only after this. */
    static void freePayload(CameraStillResult& result) noexcept
    {
        std::vector<std::uint8_t>().swap(result.jpeg);
        std::vector<std::uint16_t>().swap(result.raw);
    }

    void deliverStill(std::size_t platformCamera, CameraStillResult& result)
    {
        auto* group = groupOfToken(result.token);
        const auto wire = wireOfPlatform.find(platformCamera);
        if (!group || wire == wireOfPlatform.end()) {
            freePayload(result);
            return;
        }
        auto& runtime = runtimes[wire->second];
        // The guest gave up on this capture: its buffers have failed, and the platform's end of it releases its charge.
        if (group->expired) {
            freePayload(result);
            eraseGroup(result.token);
            releaseSessionIfIdle();
            reportState();
            return;
        }
        // A capture that was asked for before an interruption or recovery of its
        // camera that has been handled since spans it, and one that completes while
        // the camera is interrupted was not taken in a repaired camera: neither is
        // delivered, whatever the platform says of it. They fail as a capture that
        // went wrong does, for the guest to ask again.
        const bool spansBoundary = group->fireSerial < runtime.boundary;
        if (!result.ok || spansBoundary || runtime.interruption != Interruption::none) {
            // An interruption or a recovery that ended the capture is not a fault of the capture.
            if (!result.ok && !endedByInterruption(*group, runtime)) {
                auto failure = result.failure;
                if (failure.stage == uml::platform::CameraStillStage::none) failure.stage = uml::platform::CameraStillStage::unknown;
                reportStillFailure(*group, runtime, failure);
            }
            failGroup(*group, runtime.interruption != Interruption::none ? std::uint32_t(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED) : 0u);
            freePayload(result);
            eraseGroup(result.token);
            checkpoint(CameraCheckpoint::stillDelivered);
            releaseSessionIfIdle();
            reportState();
            return;
        }
        // A Bayer still teaches the lens's colour data, which reaches the guest
        // at its next start.
        if (result.color.bayer != uml::platform::BayerOrder::none && !runtime.description.calibration) {
            // Learned in the camera's anchor, which is the only format it runs in.
            auto color = result.color;
            if (runtime.description.anchor) {
                const auto& anchor = runtime.description.info.captureFormats[*runtime.description.anchor];
                color.anchorWidth = anchor.width;
                color.anchorHeight = anchor.height;
            }
            runtime.description.calibration = color;
            calibration->save(model, CalibrationStore::lensKey(runtime.description.info), color);
        }
        const auto sequence = ++runtime.sequence;
        auto meta = metaOf(runtime, sequence, result.exposure, VIRTIO_VIDEO_CAMERA_FRAME_F_STILL);
        runtime.latest = result.exposure;
        // The first member that fails says why the capture did (`reportStillFailure`): what the platform said of it, and
        // the stage the device found.
        std::optional<uml::platform::CameraStillFailure> failure;
        // A capture that delivered its JPEG did not fail, whatever its RAW did: that buffer has failed and the capture is
        // not reported. One whose JPEG did not arrive is, whether its RAW did or not.
        bool jpegAsked = false, jpegFailed = false;
        const auto fails = [&](uml::platform::CameraStillStage stage, std::uint32_t encodedWidth = 0, std::uint32_t encodedHeight = 0) {
            if (failure) return;
            failure = result.failure;
            failure->stage = stage;
            failure->encodedWidth = encodedWidth;
            failure->encodedHeight = encodedHeight;
        };
        for (const auto& member : group->members) {
            auto& stream = streams.at(member.stream);
            auto& resource = stream.resources.at(member.held.resource);
            std::uint32_t size = 0;
            bool ok = false;
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                // The platform's own account of a JPEG that is not here stands when it gave one.
                const auto platformStage = result.failure.stage != uml::platform::CameraStillStage::none
                    ? result.failure.stage : uml::platform::CameraStillStage::missingData;
                auto jpegStage = result.jpeg.empty() ? platformStage : uml::platform::CameraStillStage::none;
                // A file larger than the host takes fails the capture, before anything of it is copied to the guest.
                const auto largest = maximumJpegBytes(std::uint64_t(stream.width) * stream.height);
                if (result.jpeg.size() > largest) {
                    std::fprintf(stderr, "Virtio camera stream %u: the still's file is %zu bytes, more than the %llu a %ux%u still may take\n",
                                 stream.id, result.jpeg.size(), static_cast<unsigned long long>(largest), stream.width, stream.height);
                    result.jpeg.clear();
                    jpegStage = uml::platform::CameraStillStage::fileSize;
                }
                auto plan = reserveExifGap(result.jpeg);
                // A file that has no frame header or scan states no size.
                if (!plan && jpegStage == uml::platform::CameraStillStage::none) jpegStage = uml::platform::CameraStillStage::encodedSize;
                std::uint32_t encodedWidth = 0, encodedHeight = 0;
                // The guest asked for this size and gets this size or an error: a
                // platform picture of another size is not the one it configured.
                if (plan && (plan->frameWidth != stream.width || plan->frameHeight != stream.height)) {
                    std::fprintf(stderr, "Virtio camera stream %u: the still is %ux%u, not the %ux%u that was asked for\n", stream.id,
                                 unsigned(plan->frameWidth), unsigned(plan->frameHeight), stream.width, stream.height);
                    jpegStage = uml::platform::CameraStillStage::encodedSize;
                    encodedWidth = plan->frameWidth;
                    encodedHeight = plan->frameHeight;
                    plan.reset();
                }
                if (plan && plan->size() <= resource.capacity && !resource.planes.empty()) {
                    // The scan is the end of the platform's own bytes, written from where they lie.
                    const std::span<std::uint8_t> tail(result.jpeg.data() + (plan->tail.data() - result.jpeg.data()), plan->tail.size());
                    ok = transferSegments(mapper, regions.get(), resource.planes[0], plan->head, true) &&
                        transferSegments(mapper, regions.get(), resource.planes[0], tail, true, plan->head.size());
                    size = std::uint32_t(plan->size());
                    meta.jpeg_gap_offset = plan->gapOffset;
                    meta.jpeg_gap_length = plan->gapLength;
                    if (!ok) jpegStage = uml::platform::CameraStillStage::guestBuffer;
                } else if (plan) {
                    jpegStage = uml::platform::CameraStillStage::guestBuffer;
                }
                jpegAsked = true;
                if (!ok) {
                    jpegFailed = true;
                    fails(jpegStage, encodedWidth, encodedHeight);
                }
            } else if (!result.raw.empty() && result.color.rawWidth != 0) {
                ok = writeRaw(stream, resource, result, size);
                if (!ok) fails(uml::platform::CameraStillStage::guestBuffer);
            } else {
                fails(result.failure.stage != uml::platform::CameraStillStage::none ? result.failure.stage
                                                                                    : uml::platform::CameraStillStage::missingData);
            }
            complete(stream.id, member.held, member.held.cookie, ok ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, ok ? size : 0, sequence);
        }
        if (failure && (jpegFailed || !jpegAsked)) reportStillFailure(*group, runtime, *failure);
        pushMeta(runtime, meta, 0);
        freePayload(result);
        eraseGroup(result.token);
        checkpoint(CameraCheckpoint::stillDelivered);
        releaseSessionIfIdle();
        reportState();
    }

    bool writeRaw(const Stream& stream, Resource& resource, CameraStillResult& result, std::uint32_t& size)
    {
        const std::uint32_t width = result.color.rawWidth;
        const std::uint32_t height = result.color.rawHeight;
        if (width != stream.width || height != stream.height || result.raw.size() != std::size_t(width) * height) return false;
        const std::size_t rowBytes = std::size_t(width) * 2;
        const auto stride = rawPitch(std::uint32_t(rowBytes));
        const std::uint64_t total = std::uint64_t(stride) * height;
        if (resource.capacity < total || resource.planes.empty()) return false;
        auto* bytes = reinterpret_cast<std::uint8_t*>(result.raw.data());
        if (stride == rowBytes) {
            // Tight rows are the buffer: one pass over the segments.
            if (!transferSegments(mapper, regions.get(), resource.planes[0], std::span<std::uint8_t>(bytes, rowBytes * height), true))
                return false;
        } else {
            uml::video::GuestPlaneWriter writer(mapper, regions.get(), resource.planes[0]);
            for (std::uint32_t row = 0; row < height; ++row)
                if (!writer.write(std::uint64_t(row) * stride, std::span<const std::uint8_t>(bytes + std::size_t(row) * rowBytes, rowBytes)))
                    return false;
        }
        size = std::uint32_t(total);
        return true;
    }

    void expireGroups()
    {
        const auto time = now();
        bool lost = false;
        for (std::size_t index = 0; index < groups.size();) {
            auto& group = groups[index];
            if (group.deadline > time) {
                ++index;
                continue;
            }
            if (group.expired) {
                // The platform did not end a capture the guest gave up on long ago.
                lost = true;
                ++index;
                continue;
            }
            // A capture that was not ended in time, one the platform did not end or a whole one that waited for room, is
            // reported, unless an interruption of its camera ended it; a group the guest did not complete is the guest's
            // own.
            if (group.fired || group.waiting) {
                if (const auto* runtime = runtimeOf(group.camera); runtime != nullptr && !endedByInterruption(group, *runtime))
                    reportStillFailure(group, *runtime, failureAt(uml::platform::CameraStillStage::timeout));
            }
            failGroup(group, 0);
            if (group.fired) {
                // The platform still holds the capture, and cannot be told to drop one: the charge stays until it ends
                // it, or the session does.
                group.expired = true;
                group.members.clear();
                group.deadline = time + abandonedCaptureNanoseconds;
                ++index;
                continue;
            }
            groups.erase(groups.begin() + std::ptrdiff_t(index));
        }
        if (lost) {
            // A completion may have reached the inbox after the pump took its batch: the capture it ends is finished, not
            // lost, and its session is healthy. What is still unfinished past its time was lost.
            deliverQueuedResultsOfAbandoned(time);
            lost = std::any_of(groups.begin(), groups.end(), [time](const auto& group) { return group.expired && group.deadline <= time; });
        }
        if (lost) abandonSession();
        fireWaiting();
        armGroupTimer();
        releaseSessionIfIdle();
        reportState();
    }

    /* Delivers the results already in the inbox of the captures the guest gave up on whose time has run out. The inbox is
     * read under its lock and the results are delivered after it is let go. */
    void deliverQueuedResultsOfAbandoned(std::uint64_t time)
    {
        std::vector<InboxStill> finished;
        {
            std::lock_guard lock(inboxMutex);
            for (auto still = inboxStills.begin(); still != inboxStills.end();) {
                const auto* group = groupOfToken(still->result.token);
                if (group != nullptr && group->expired && group->deadline <= time) {
                    finished.push_back(std::move(*still));
                    still = inboxStills.erase(still);
                } else {
                    ++still;
                }
            }
        }
        for (auto& still : finished) deliverStill(still.camera, still.result);
    }

    /* Ends the platform's session, and with it every capture it holds: the ones
     * the guest is waiting for fail, and the ones it gave up on are released. */
    void endFiredGroups(std::uint32_t extra)
    {
        for (std::size_t index = 0; index < groups.size();) {
            if (!groups[index].fired) {
                ++index;
                continue;
            }
            failGroup(groups[index], extra);
            groups.erase(groups.begin() + std::ptrdiff_t(index));
        }
    }

    /* A capture was lost to the platform: the session is closed and opened again by whatever needs it next, and the cameras
     * with a stream start their video again. */
    void abandonSession()
    {
        closeSession();
        endFiredGroups(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED);
        endInterruptions();
    }

    // ------------------------------------------------------------------------
    // Platform events
    // ------------------------------------------------------------------------

    void handlePlatformEvent(const InboxEvent& inboxEvent)
    {
        const CameraEvent& platformEvent = inboxEvent.event;
        const auto each = [&](auto&& body) {
            const auto bounded = [&](Runtime& runtime) {
                // Whatever the platform handed over before this event is from before it.
                runtime.boundary = inboxEvent.serial;
                body(runtime);
            };
            if (platformEvent.camera == uml::platform::allCameras) {
                for (auto& runtime : runtimes) bounded(runtime);
            } else if (const auto wire = wireOfPlatform.find(platformEvent.camera); wire != wireOfPlatform.end()) {
                bounded(runtimes[wire->second]);
            }
        };
        switch (platformEvent.kind) {
        case CameraEventKind::interrupted:
        case CameraEventKind::interruptedByMultitasking:
        case CameraEventKind::interruptedBySystemPressure:
        case CameraEventKind::interruptedInBackground:
        case CameraEventKind::unavailable: {
            bool ended = false;
            each([&](Runtime& runtime) {
                // The application left the foreground with the guest's video open on this camera: that video ends here, and
                // it is the guest, when it next runs, that finds out (the application suspends it behind this).
                const bool ends = platformEvent.kind == CameraEventKind::interruptedInBackground && endsVideoWhenAppLeaves(runtime);
                runtime.interruption = platformEvent.kind == CameraEventKind::interruptedByMultitasking ? Interruption::multitasking
                    : platformEvent.kind == CameraEventKind::interruptedBySystemPressure ? Interruption::pressure
                                                                                          : Interruption::interrupted;
                if (ends) {
                    runtime.lost = ended = true;
                    runtime.lostAt = now();
                    runtime.lossRecord = 0;
                    runtime.lossRecordCarriers.clear();
                    runtime.lossRecordRead = false;
                    std::fprintf(stderr, "camera %u: the video ended: the application left the foreground\n",
                                 unsigned(runtime.description.index));
                }
                markDown(runtime, platformEvent.kind == CameraEventKind::unavailable ? VIRTIO_VIDEO_EVENT_CAMERA_UNAVAILABLE
                                                                                      : VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
                // The capture of the camera's video stops: the streams it served get no picture.
                if (ends) reconfigure(runtime);
                updateBlackTimers(runtime);
            });
            // The lost camera holds no capture: the session goes unless a still or the torch keeps it.
            if (ended) releaseSessionIfIdle();
            break;
        }
        case CameraEventKind::resumed:
        case CameraEventKind::available:
            each([&](Runtime& runtime) {
                if (runtime.interruption == Interruption::none && !runtime.failed) return;
                runtime.interruption = Interruption::none;
                runtime.failed = false;
                // A capture that went away is started again, and black frames
                // bridge the gap until it delivers.
                reconfigure(runtime);
                runtime.resuming = runtime.videoRunning;
                settle(runtime);
                updateBlackTimers(runtime);
            });
            break;
        case CameraEventKind::failed:
            // The session is gone; it is opened again when a stream needs it.
            // The cameras that have a stream are down until it runs again, and
            // are told so before the session's end could tell them they are
            // back.
            each([&](Runtime& runtime) {
                runtime.videoRunning = false;
                if (!wantsVideo(runtime)) return;
                runtime.failed = true;
                markDown(runtime, VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED);
            });
            closeSession();
            // The captures the session held are gone with it.
            endFiredGroups(VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED);
            sessionFailed = true;
            lastRetry = now();
            each([&](Runtime& runtime) { updateBlackTimers(runtime); });
            fireWaiting();
            break;
        }
        reportState();
    }

    /* The application leaves the foreground and asks for the guest's running video to end now (`options.background`): the
     * video of every camera the guest runs a video stream on ends as the platform's notice of the departure ends it, which
     * finds those cameras lost when it comes. The streams the guest has yet to stop (those of the cameras ended here, and of
     * one whose video ended a moment ago, when the notice came first) are what the application waits for. A blocked session
     * runs no video, and a camera with no running stream is left as it is: the departure is not its news, and no notice
     * of it is made up. */
    void endVideoForBackground(std::uint64_t request)
    {
        pendingStops.clear();
        if (!blocked()) {
            for (auto& runtime : runtimes) {
                if (!runsVideo(runtime) || !endsVideoWhenAppLeaves(runtime)) continue;
                std::uint64_t serial;
                {
                    std::lock_guard lock(inboxMutex);
                    serial = ++inboxSerial;
                }
                handlePlatformEvent({CameraEvent {runtime.description.platformIndex, CameraEventKind::interruptedInBackground}, serial});
            }
            const auto time = now();
            for (const auto& [id, stream] : streams) {
                const auto* runtime = runtimeOf(stream.camera);
                if (runtime && startedVideo(stream) && runtime->lost && time - runtime->lostAt <= lossBeingActedOnNanoseconds)
                    pendingStops.insert(id);
            }
        }
        if (!pendingStops.empty())
            std::fprintf(stderr, "camera: the application left the foreground: %zu video stream(s) of the guest to stop\n",
                         pendingStops.size());
        options.background->ended(request, std::uint32_t(pendingStops.size()));
    }

    /* The guest stopped a video stream: the application no longer waits for it. */
    void videoStreamStopped(std::uint32_t id)
    {
        if (pendingStops.erase(id) == 0 || !options.background) return;
        std::fprintf(stderr, "camera: the guest stopped video stream %u after the application left the foreground (%zu left)\n", id,
                     pendingStops.size());
        options.background->remaining(std::uint32_t(pendingStops.size()));
    }

    /* Takes what the platform produced since the last pass. */
    void pump()
    {
        std::uint64_t request;
        if (options.background && options.background->take(request)) endVideoForBackground(request);
        std::vector<InboxFrame> newest;
        std::deque<InboxStill> stills;
        std::deque<InboxEvent> platformEvents;
        {
            std::lock_guard lock(inboxMutex);
            // Only the newest frame of a camera is kept, and the older ones are
            // freed here, before the lock is let go: a frame is stale by the time
            // the worker takes it, and each holds the capture's buffer. So the
            // pump holds one frame a camera while the inbox refills to its limit,
            // which is the count the reserve assumes (heldFramesPerCamera).
            for (auto& entry : inboxFrames) {
                const auto same = std::find_if(newest.begin(), newest.end(), [&](const InboxFrame& kept) { return kept.camera == entry.camera; });
                if (same != newest.end()) *same = std::move(entry);
                else newest.push_back(std::move(entry));
            }
            inboxFrames.clear();
            stills.swap(inboxStills);
            platformEvents.swap(inboxEvents);
        }
        for (const auto& platformEvent : platformEvents) handlePlatformEvent(platformEvent);
        for (auto& still : stills) deliverStill(still.camera, still.result);
        for (auto& entry : newest) {
            const auto wire = wireOfPlatform.find(entry.camera);
            if (wire == wireOfPlatform.end()) continue;
            auto& runtime = runtimes[wire->second];
            // A frame the platform handed over before the last interruption or
            // recovery of its camera was captured before the repair, however the
            // pump came by it (it waited in the inbox, or in this batch, while the
            // event was handled): it is not the camera's picture now.
            if (entry.serial < runtime.boundary) {
                entry.frame = CameraFrame {};
                continue;
            }
            if (runtime.resuming && runtime.interruption == Interruption::none) {
                runtime.resuming = false;
                settle(runtime);
                updateBlackTimers(runtime);
            }
            if (blackMode(runtime)) continue;
            deliverFrame(runtime, entry.frame);
            // Its buffer goes back to the capture as soon as the guest has the frame.
            entry.frame = CameraFrame {};
        }
    }

    // ------------------------------------------------------------------------
    // Commands
    // ------------------------------------------------------------------------

    VhostUserQueueResult done(std::vector<std::uint8_t> data) const { return {VhostUserQueueAction::complete, std::move(data)}; }

    [[nodiscard]] virtio_video_params paramsOf(const Stream& stream) const
    {
        virtio_video_params p {};
        p.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
        p.format = stream.format;
        p.frame_width = stream.width;
        p.frame_height = stream.height;
        p.min_buffers = 1;
        p.max_buffers = limits.buffers;
        p.crop = {0, 0, stream.width, stream.height};
        p.frame_rate = rateOf(stream);
        p.num_planes = 1;
        switch (stream.kind) {
        case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO:
            p.plane_formats[0] = {videoPlaneSize(stream), pitchOf(stream)};
            break;
        case VIRTIO_VIDEO_CAMERA_STREAM_STILL:
            if (stream.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG)
                p.plane_formats[0] = {std::uint32_t(jpegBufferBytes(std::uint64_t(stream.width) * stream.height)), 0};
            else
                p.plane_formats[0] = {rawPitch(2 * stream.width) * stream.height, rawPitch(2 * stream.width)};
            break;
        default:
            p.plane_formats[0] = {std::uint32_t(sizeof(virtio_video_camera_meta)), 0};
            break;
        }
        return p;
    }

    VhostUserQueueResult queryControls(std::uint32_t id, const Runtime& runtime, std::uint32_t start, std::size_t capacity)
    {
        const auto& descriptors = runtime.controls.descriptors();
        if (start > descriptors.size()) throw std::invalid_argument("invalid control index");
        const auto room = capacity > sizeof(virtio_video_query_v4l2_controls_resp)
            ? (capacity - sizeof(virtio_video_query_v4l2_controls_resp)) / sizeof(virtio_video_v4l2_control_desc) : 0;
        const auto count = std::min<std::size_t>(descriptors.size() - start, room);
        if (count == 0 && start < descriptors.size()) throw std::invalid_argument("control response is too small");
        virtio_video_query_v4l2_controls_resp response {};
        response.hdr = {VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, id};
        response.num_descs = std::uint32_t(count);
        response.next_index = start + count < descriptors.size() ? std::uint32_t(start + count) : 0;
        auto result = toBytes(response);
        for (std::size_t index = 0; index < count; ++index) {
            const auto bytes = toBytes(descriptors[start + index]);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        return done(std::move(result));
    }

    VhostUserQueueResult setControls(std::uint32_t id, Runtime& runtime, std::span<const std::uint8_t> request)
    {
        const auto header = load<virtio_video_v4l2_controls>(request);
        const std::uint32_t count = header.num_controls;
        if (count == 0 || count > maximumControls ||
            request.size() < sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value))
            throw std::invalid_argument("invalid control count");
        const auto fail = [&](std::uint32_t index, std::uint32_t type) {
            return done(toBytes(virtio_video_v4l2_controls_resp {{type, id}, index, count}));
        };
        const auto area = request.subspan(sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value));
        // Every value is checked first, and applies in order to a copy, so a
        // refusal changes nothing.
        ControlValues next = runtime.values;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = load<virtio_video_v4l2_control_value>(
                request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value)));
            const auto* descriptor = runtime.controls.find(entry.id);
            if (!descriptor) return fail(index, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL);
            if (descriptor->flags & VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            std::vector<std::uint8_t> payload;
            if (descriptor->elem_size == 0 && entry.size != 0) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            if (descriptor->elem_size != 0) {
                if (entry.size != descriptor->elem_size || entry.value % 8 != 0 || entry.value > area.size() ||
                    area.size() - entry.value < entry.size)
                    return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
                payload.assign(area.begin() + std::ptrdiff_t(entry.value), area.begin() + std::ptrdiff_t(entry.value + entry.size));
            }
            const auto value = descriptor->elem_size != 0 ? std::int64_t(0) : static_cast<std::int64_t>(entry.value);
            if (runtime.controls.check(entry.id, value, payload.size()) != ControlCheck::ok)
                return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            runtime.controls.store(next, entry.id, value, std::move(payload));
        }
        const bool wasBlack = blackMode(runtime);
        runtime.values = std::move(next);
        // Applied as soon as the host can.
        const auto settings = runtime.controls.settings(runtime.values);
        reportLight(runtime, settings.torch);
        if (!blocked()) {
            if (settings.torch && !session) ensureSession();
            applySettings(runtime, false);
            if (!settings.torch) releaseSessionIfIdle();
        }
        if (wasBlack != blackMode(runtime)) updateBlackTimers(runtime);
        reportState();
        return done(toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, id}, count, count}));
    }

    VhostUserQueueResult getControls(std::uint32_t id, const Runtime& runtime, std::span<const std::uint8_t> request,
                                     std::size_t capacity)
    {
        const auto header = load<virtio_video_v4l2_controls>(request);
        const std::uint32_t count = header.num_controls;
        if (count == 0 || count > maximumControls ||
            request.size() < sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value))
            throw std::invalid_argument("invalid control count");
        std::vector<virtio_video_v4l2_control_value> values;
        std::vector<std::uint8_t> area;
        for (std::uint32_t index = 0; index < count; ++index) {
            auto entry = load<virtio_video_v4l2_control_value>(
                request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value)));
            const auto* descriptor = runtime.controls.find(entry.id);
            if (!descriptor)
                return done(toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, id}, index, count}));
            if (descriptor->elem_size != 0) {
                const auto stored = runtime.values.payloads.find(entry.id);
                std::vector<std::uint8_t> payload = stored == runtime.values.payloads.end()
                    ? std::vector<std::uint8_t>(descriptor->elem_size) : stored->second;
                payload.resize(descriptor->elem_size);
                entry.size = descriptor->elem_size;
                entry.value = area.size();
                area.insert(area.end(), payload.begin(), payload.end());
                area.resize((area.size() + 7) / 8 * 8);
            } else {
                entry.size = 0;
                entry.value = static_cast<std::uint64_t>(
                    runtime.controls.read(runtime.values, entry.id, runtime.latest ? &*runtime.latest : nullptr));
            }
            values.push_back(entry);
        }
        auto result = toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, id}, count, count});
        for (const auto& value : values) {
            const auto bytes = toBytes(value);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        result.insert(result.end(), area.begin(), area.end());
        if (result.size() > capacity) throw std::invalid_argument("control response is too small");
        return done(std::move(result));
    }

    /* A stream's first state. */
    Stream makeStream(std::uint32_t id, const virtio_video_camera_stream_create& create)
    {
        const auto* runtime = runtimeOf(create.camera_index);
        if (!runtime) throw std::invalid_argument("no such camera");
        const auto& description = runtime->description;
        Stream stream;
        stream.id = id;
        stream.camera = create.camera_index;
        stream.kind = create.kind;
        switch (create.kind) {
        case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO: {
            if (description.video.empty()) throw std::invalid_argument("the camera has no video");
            stream.format = VIRTIO_VIDEO_FORMAT_NV12;
            // The size nearest 1280 x 720 without exceeding it, else the smallest.
            const VideoMode* best = nullptr;
            for (const auto& mode : description.video)
                if (mode.width <= defaultVideoWidth && mode.height <= defaultVideoHeight &&
                    (!best || std::uint64_t(mode.width) * mode.height > std::uint64_t(best->width) * best->height))
                    best = &mode;
            if (!best) best = &description.video.back();
            stream.width = best->width;
            stream.height = best->height;
            stream.intervalDenominator = supportedFrameRate(description, stream.width, stream.height, 30);
            if (stream.intervalDenominator == 0) throw std::invalid_argument("the camera has no frame rate");
            break;
        }
        case VIRTIO_VIDEO_CAMERA_STREAM_STILL:
            if (!description.jpegSizes.empty()) {
                stream.format = VIRTIO_VIDEO_FORMAT_CAMERA_JPEG;
                stream.width = description.jpegSizes.front().width;
                stream.height = description.jpegSizes.front().height;
            } else if (description.raw()) {
                stream.format = rawWireFormat(description.calibration->bayer);
                stream.width = description.rawSize.width;
                stream.height = description.rawSize.height;
            } else {
                throw std::invalid_argument("the camera takes no stills");
            }
            break;
        case VIRTIO_VIDEO_CAMERA_STREAM_META:
            stream.format = VIRTIO_VIDEO_FORMAT_CAMERA_META;
            break;
        default: throw std::invalid_argument("invalid camera stream kind");
        }
        return stream;
    }

    VhostUserQueueResult setParams(Stream& stream, const virtio_video_params& p)
    {
        const auto id = stream.id;
        if (p.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("a camera stream has only the output queue");
        const auto& description = runtimes[stream.camera].description;
        std::uint32_t format = p.format, width = p.frame_width, height = p.frame_height;
        switch (stream.kind) {
        case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO:
            if (format != VIRTIO_VIDEO_FORMAT_NV12 || !videoSizeAllowed(description, width, height))
                throw std::invalid_argument("unsupported camera video format");
            break;
        case VIRTIO_VIDEO_CAMERA_STREAM_STILL:
            if (format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG) {
                if (std::find(description.jpegSizes.begin(), description.jpegSizes.end(), uml::platform::CameraStillSize {width, height}) ==
                    description.jpegSizes.end())
                    throw std::invalid_argument("unsupported camera still size");
            } else if (!description.raw() || format != rawWireFormat(description.calibration->bayer) ||
                       width != description.rawSize.width || height != description.rawSize.height) {
                throw std::invalid_argument("unsupported camera still format");
            }
            break;
        default:
            if (format != VIRTIO_VIDEO_FORMAT_CAMERA_META) throw std::invalid_argument("unsupported camera metadata format");
            width = height = 0;
            break;
        }
        const bool same = format == stream.format && width == stream.width && height == stream.height;
        if (same) return done(reply(id));
        if (stream.started) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_BUSY));
        if (!stream.resources.empty()) throw std::invalid_argument("camera format changed with allocated buffers");
        stream.format = format;
        stream.width = width;
        stream.height = height;
        if (stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) {
            const auto rate = supportedFrameRate(description, width, height, rateOf(stream));
            stream.intervalNumerator = 1;
            stream.intervalDenominator = rate != 0 ? rate : 1;
        }
        return done(reply(id));
    }

    /* Everything a stream has queued but not filled goes back as an error. */
    void cancelQueued(Stream& stream)
    {
        for (const auto& held : stream.queued) complete(stream.id, held, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR);
        stream.queued.clear();
        stream.backlog.clear();
        for (auto& group : groups)
            for (auto member = group.members.begin(); member != group.members.end();) {
                if (member->stream == stream.id) {
                    complete(stream.id, member->held, member->held.cookie, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR);
                    member = group.members.erase(member);
                } else {
                    ++member;
                }
            }
        std::erase_if(groups, [](const auto& group) { return group.members.empty() && !group.fired; });
    }

    void stopStream(Stream& stream)
    {
        const bool wasVideo = startedVideo(stream);
        cancelQueued(stream);
        stream.started = false;
        stream.delivered = false;
        disarm(stream);
        if (wasVideo) {
            if (auto* runtime = runtimeOf(stream.camera)) {
                reconfigure(*runtime);
                updateBlackTimers(*runtime);
            }
            videoStreamStopped(stream.id);
        }
        releaseSessionIfIdle();
        reportState();
    }

    VhostUserQueueResult command(std::uint64_t requestId, std::span<const std::uint8_t> request, std::size_t capacity)
    {
        const auto header = load<virtio_video_cmd_hdr>(request);
        const auto id = header.stream_id;
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_CAMERA_QUERY: {
            const auto q = load<virtio_video_camera_query>(request);
            std::vector<CameraDescription> descriptions;
            descriptions.reserve(runtimes.size());
            for (const auto& runtime : runtimes) descriptions.push_back(runtime.description);
            if (q.start_index > descriptions.size()) throw std::invalid_argument("invalid camera index");
            return done(cameraQueryResponse(descriptions, q.start_index, capacity,
                                            blocked() ? VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED : 0));
        }
        case VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS: {
            const auto q = load<virtio_video_camera_query_formats>(request);
            const auto* runtime = runtimeOf(q.camera_index);
            if (!runtime) throw std::invalid_argument("no such camera");
            if (q.kind != VIRTIO_VIDEO_CAMERA_STREAM_VIDEO && q.kind != VIRTIO_VIDEO_CAMERA_STREAM_STILL)
                throw std::invalid_argument("invalid camera stream kind");
            return done(cameraFormatsResponse(runtime->description, q.kind));
        }
        case VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS: {
            const auto q = load<virtio_video_camera_query_controls>(request);
            const auto* runtime = runtimeOf(q.camera_index);
            if (!runtime) throw std::invalid_argument("no such camera");
            return queryControls(id, *runtime, q.start_index, capacity);
        }
        case VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE: {
            const auto q = load<virtio_video_camera_stream_create>(request);
            if (!id || id >= reservedStreamIds || streams.contains(id)) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            if (streams.size() >= maximumStreams) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY));
            streams.emplace(id, makeStream(id, q));
            return done(reply(id));
        }
        default: break;
        }
        const auto found = streams.find(id);
        if (found == streams.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID));
        auto& stream = found->second;
        auto& runtime = runtimes[stream.camera];
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
            stopStream(stream);
            streams.erase(found);
            endLossIfClosed(runtime);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_GET_PARAMS: {
            const auto q = load<virtio_video_get_params>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid parameter queue");
            return done(toBytes(virtio_video_get_params_resp {{VIRTIO_VIDEO_RESP_OK_GET_PARAMS, id}, paramsOf(stream)}));
        }
        case VIRTIO_VIDEO_CMD_SET_PARAMS: return setParams(stream, load<virtio_video_set_params>(request).params);
        case VIRTIO_VIDEO_CMD_SET_STREAM_INFO: {
            const auto q = load<virtio_video_set_stream_info>(request).info;
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid stream information queue");
            if (stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO && q.frame_interval_numerator != 0 && q.frame_interval_denominator != 0) {
                // The interval is adjusted to a rate the camera runs the size at.
                const auto wanted = std::max<std::uint32_t>(1, (q.frame_interval_denominator + q.frame_interval_numerator / 2) / q.frame_interval_numerator);
                const auto rate = supportedFrameRate(runtime.description, stream.width, stream.height, wanted);
                if (rate == 0) throw std::invalid_argument("unsupported camera frame rate");
                const bool changed = rate != rateOf(stream);
                stream.intervalNumerator = 1;
                stream.intervalDenominator = rate;
                if (changed && stream.started) {
                    reconfigure(runtime);
                    updateBlackTimers(runtime);
                    if (stream.timerArmed) arm(stream);
                }
            }
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_GET_STREAM_INFO: {
            const auto q = load<virtio_video_get_stream_info>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid stream information queue");
            virtio_video_stream_info info {};
            info.queue_type = q.queue_type;
            info.frame_interval_numerator = stream.intervalNumerator;
            info.frame_interval_denominator = stream.intervalDenominator;
            if (stream.kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) {
                info.colorspace = colorspaceRec709;
                info.xfer_func = xferFunc709;
                info.ycbcr_enc = ycbcrEnc709;
                info.quantization = quantizationFullRange;
            }
            return done(toBytes(virtio_video_get_stream_info_resp {{VIRTIO_VIDEO_RESP_OK_STREAM_INFO, id}, info}));
        }
        case VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS: {
            const auto q = load<virtio_video_query_v4l2_controls>(request);
            return queryControls(id, runtime, q.start_index, capacity);
        }
        case VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS: return setControls(id, runtime, request);
        case VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS: return getControls(id, runtime, request, capacity);
        case VIRTIO_VIDEO_CMD_RESOURCE_ATTACH: {
            const auto q = load<virtio_video_resource_attach>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT || !q.resource_id) throw std::invalid_argument("invalid camera resource");
            const auto previous = stream.resources.find(q.resource_id);
            if (previous != stream.resources.end() && previous->second.queued)
                throw std::invalid_argument("camera resource is already queued");
            if (previous == stream.resources.end() && stream.resources.size() >= 128) throw std::invalid_argument("invalid camera resource");
            Resource resource;
            resource.planes = uml::video::parseScatterLists(request.subspan(sizeof(q)), 1, mapper, regions.get());
            for (const auto& segment : resource.planes[0]) resource.capacity += segment.length;
            if (resource.capacity < paramsOf(stream).plane_formats[0].plane_size)
                throw std::invalid_argument("camera resource is smaller than the buffer");
            if (previous != stream.resources.end()) previous->second = std::move(resource);
            else stream.resources.emplace(q.resource_id, std::move(resource));
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE: {
            const auto q = load<virtio_video_resource_queue>(request);
            const auto resource = stream.resources.find(q.resource_id);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT || resource == stream.resources.end())
                return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID));
            if (resource->second.queued) throw std::invalid_argument("camera resource is already queued");
            if (stream.queued.size() >= maximumHeld) throw std::invalid_argument("camera queue is full");
            const Held held {requestId, q.resource_id, q.timestamp, q.flags};
            resource->second.queued = true;
            switch (stream.kind) {
            case VIRTIO_VIDEO_CAMERA_STREAM_VIDEO:
                stream.queued.push_back(held);
                if (!stream.started) {
                    stream.started = true;
                    reconfigure(runtime);
                    updateBlackTimers(runtime);
                    reportState();
                }
                break;
            case VIRTIO_VIDEO_CAMERA_STREAM_STILL: queueStill(stream, runtime, held); break;
            default:
                // The guest queues a meta buffer that carried the loss's record again: it has read the record.
                if (runtime.lost && !runtime.lossRecordRead &&
                    std::find(runtime.lossRecordCarriers.begin(), runtime.lossRecordCarriers.end(),
                              std::pair<std::uint32_t, std::uint32_t> {id, q.resource_id}) != runtime.lossRecordCarriers.end())
                    runtime.lossRecordRead = true;
                stream.queued.push_back(held);
                stream.started = true;
                serveMeta(id, stream);
                break;
            }
            return {VhostUserQueueAction::defer, {}};
        }
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        case VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES: {
            const auto q = load<virtio_video_queue_clear>(request);
            if (q.queue_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) throw std::invalid_argument("invalid clear queue");
            stopStream(stream);
            if (header.type == VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES) stream.resources.clear();
            return done(reply(id));
        }
        default: return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION));
        }
    }

    void drainEvents()
    {
        std::array<struct kevent, 8> ready {};
        const timespec none {};
        bool user = false, groupTimer = false;
        std::vector<std::uint32_t> ticks;
        for (;;) {
            int count;
            do {
                count = ::kevent(queue.get(), nullptr, 0, ready.data(), int(ready.size()), &none);
            } while (count < 0 && errno == EINTR);
            if (count < 0) throw std::runtime_error("draining the camera events failed");
            for (int index = 0; index < count; ++index) {
                const auto& entry = ready[std::size_t(index)];
                if (entry.filter == EVFILT_USER) user = true;
                else if (entry.filter == EVFILT_TIMER && entry.ident == groupTimerIdent) groupTimer = true;
                else if (entry.filter == EVFILT_TIMER) ticks.push_back(std::uint32_t(entry.ident));
            }
            if (count < int(ready.size())) break;
        }
        (void)user;
        pump();
        for (const auto id : ticks) blackTick(id);
        if (groupTimer || !groups.empty()) expireGroups();
    }

    void shutdown() noexcept
    {
        for (auto& [id, stream] : streams) {
            (void)id;
            disarm(stream);
        }
        closeSession();
        streams.clear();
        pendingStops.clear();
        if (options.background) options.background->remaining(0);
        groups.clear();
        events.clear();
        completions.clear();
        {
            std::lock_guard lock(inboxMutex);
            inboxFrames.clear();
            inboxStills.clear();
            inboxEvents.clear();
        }
        for (auto& runtime : runtimes) {
            runtime.interruption = Interruption::none;
            runtime.resuming = false;
            runtime.failed = false;
            runtime.lost = false;
            runtime.lossRecord = 0;
            runtime.lossRecordCarriers.clear();
            runtime.lossRecordRead = false;
            runtime.told = false;
            runtime.toldUnavailable = false;
            runtime.videoRetry = 0;
            runtime.calibrationAttempts = 0;
            runtime.values = runtime.controls.defaults();
            runtime.latest.reset();
            runtime.sequence = 0;
            runtime.videoRunning = false;
            runtime.appliedValid = false;
        }
        reportVideoMode(false);
        if (reported != CameraState::idle) {
            reported = CameraState::idle;
            try {
                if (options.onState) options.onState(CameraState::idle);
            } catch (...) {
                // The application's notice must not stop a reset.
            }
        }
    }
};

VirtioCameraDevice::VirtioCameraDevice(std::shared_ptr<uml::platform::Camera> host, CameraDeviceOptions options)
    : implementation_(std::make_unique<Implementation>(std::move(host), std::move(options)))
{
}
VirtioCameraDevice::~VirtioCameraDevice() = default;
const char* VirtioCameraDevice::name() const noexcept { return "Virtio Video camera"; }
std::uint64_t VirtioCameraDevice::features() const noexcept
{
    const std::uint64_t base = (1u << VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES) | (1u << VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG) |
        (1u << VIRTIO_VIDEO_F_V4L2_CONTROLS);
    return implementation_->regions ? base | (1u << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION) : base;
}
std::size_t VirtioCameraDevice::queueCount() const noexcept { return 2; }
std::vector<std::uint8_t> VirtioCameraDevice::configuration() const
{
    /* The longest answers: a format query, and a page of camera or control
     * descriptors, which the device splits to fit the buffer the guest gave. */
    return toBytes(virtio_video_config {UML_VIRTIO_VIDEO_VERSION, 16384, 4096});
}
void VirtioCameraDevice::writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) {}
void VirtioCameraDevice::setGuestMapper(GuestMapper mapper) { implementation_->mapper = std::move(mapper); }
void VirtioCameraDevice::setSharedMemoryRegions(std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions)
{
    implementation_->regions = std::move(regions);
}
VhostUserQueueResult VirtioCameraDevice::processQueue(std::uint32_t index, std::uint64_t id,
                                                      std::span<const std::uint8_t> request, std::size_t capacity)
{
    auto& impl = *implementation_;
    if (index == 1) {
        if (!request.empty() || capacity < sizeof(virtio_video_event)) return {VhostUserQueueAction::complete, {}};
        if (!impl.events.empty()) {
            auto result = toBytes(impl.events.front());
            impl.events.pop_front();
            return {VhostUserQueueAction::complete, std::move(result)};
        }
        /* No event is waiting: the buffer stays in the guest's ring, as an
         * input device's does, and the server offers it again when an event
         * wakes the device. */
        return {VhostUserQueueAction::retry, {}};
    }
    if (index != 0 || request.size() < sizeof(virtio_video_cmd_hdr) || capacity < sizeof(virtio_video_cmd_hdr))
        return {VhostUserQueueAction::complete, {}};
    const auto header = load<virtio_video_cmd_hdr>(request);
    try {
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE && capacity < sizeof(virtio_video_camera_queue_resp))
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        auto result = impl.command(id, request, capacity);
        if (result.response.size() > capacity)
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        return result;
    } catch (const std::invalid_argument&) {
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Virtio camera stream %u: %s\n", header.stream_id, error.what());
        const auto found = impl.streams.find(header.stream_id);
        if (found != impl.streams.end()) impl.stopStream(found->second);
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE)
            return {VhostUserQueueAction::complete, impl.queued(header.stream_id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)};
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION)};
    }
}
bool VirtioCameraDevice::takeQueueCompletion(VhostUserQueueCompletion& completion)
{
    auto& pending = implementation_->completions;
    if (pending.empty()) return false;
    completion = std::move(pending.front());
    pending.pop_front();
    return true;
}
void VirtioCameraDevice::reset() noexcept { implementation_->shutdown(); }
int VirtioCameraDevice::eventDescriptor() const noexcept { return implementation_->queue.get(); }
void VirtioCameraDevice::drainEventSource() { implementation_->drainEvents(); }
std::vector<CameraDescription> VirtioCameraDevice::cameras() const
{
    std::vector<CameraDescription> result;
    result.reserve(implementation_->runtimes.size());
    for (const auto& runtime : implementation_->runtimes) result.push_back(runtime.description);
    return result;
}

} // namespace uml::camera
