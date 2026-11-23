// SPDX-License-Identifier: GPL-2.0-or-later
// The Virtio camera device driven the way the guest driver drives it, over its
// two queues, in front of the synthetic platform camera. The platform camera
// has its own test; every case here is about what the device does with the
// guest's commands, buffers and controls, what it asks of the platform, and what
// it writes back. A blocked guest must never reach the platform's capture
// session at all.

#include "camera_controls.hpp"
#include "camera_device.hpp"
#include "camera_layout.hpp"
#include "camera_orientation.hpp"
#include "in_process_service.hpp"
#include "jpeg_gap.hpp"
#include "nv12_transform.hpp"
#include "raw_pitch.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"
#include "virtio_video_v4l2_ids.h"
#include "wire_bytes.hpp"

#include <mach/mach.h>
#include <poll.h>
#include <sys/event.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

using namespace uml::camera;
using namespace uml::platform;
using namespace uml::virtio;
using uml::testing::append;
using uml::testing::load;
using uml::testing::require;
using uml::video::rawPitch;

// The memory of the payloads a test follows (a still result's JPEG and RAW vectors), by address: the global deallocation
// functions below tell when a vector's memory is really given back, which clearing its size would not.
namespace payload_watch {
std::atomic<const void*> pointers[32] {};
std::atomic<std::size_t> sizes[32] {};

void track(const void* pointer, std::size_t size)
{
    if (pointer == nullptr || size == 0) return;
    for (unsigned index = 0; index < 32; ++index) {
        const void* expected = nullptr;
        if (pointers[index].compare_exchange_strong(expected, pointer)) {
            sizes[index] = size;
            return;
        }
    }
    std::abort();
}

void freed(void* pointer) noexcept
{
    for (auto& tracked : pointers) {
        const void* expected = pointer;
        tracked.compare_exchange_strong(expected, nullptr);
    }
}

std::size_t bytes()
{
    std::size_t total = 0;
    for (unsigned index = 0; index < 32; ++index)
        if (pointers[index].load() != nullptr) total += sizes[index].load();
    return total;
}
} // namespace payload_watch

void operator delete(void* pointer) noexcept
{
    payload_watch::freed(pointer);
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept
{
    payload_watch::freed(pointer);
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept
{
    payload_watch::freed(pointer);
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept
{
    payload_watch::freed(pointer);
    std::free(pointer);
}

namespace {

constexpr std::size_t page = 4096;
constexpr auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
constexpr std::uint32_t videoKind = VIRTIO_VIDEO_CAMERA_STREAM_VIDEO;
constexpr std::uint32_t stillKind = VIRTIO_VIDEO_CAMERA_STREAM_STILL;
constexpr std::uint32_t metaKind = VIRTIO_VIDEO_CAMERA_STREAM_META;
constexpr std::uint32_t errorFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
constexpr std::uint32_t blackFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK;
constexpr std::uint32_t interruptedFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED;
constexpr std::uint32_t blockedFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED;
constexpr std::uint32_t resumingFlag = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING;
constexpr std::uint32_t metaSize = sizeof(virtio_video_camera_meta);

struct RigOptions {
    SyntheticCameraProfile profile {SyntheticCameraProfile::tablet};
    CameraMode mode {CameraMode::live};
    CameraTier tier {CameraTier::small};
    bool authorized {true};
    std::shared_ptr<CalibrationStore> store;
    /* What the device is given (`CameraDeviceOptions::use`): the ceiling unless a test says. */
    CameraUse use;
    /* The working storage a source holds for each pixel of a still it makes, where a test is to state it (the synthetic
     * source's is 11 bytes). */
    std::optional<std::uint32_t> workingBytes;
    /* The working storage a source holds besides that while it turns a still of a camera presented half around. */
    std::optional<std::uint32_t> turnedWorkingBytes;
    /* The RAW size the host declares for its first camera, where a test is to state it. */
    std::optional<uml::platform::CameraStillSize> rawStillSize;
    /* The application's statement of each camera's SENSOR_ORIENTATION, where a test is to state it. */
    std::function<std::uint32_t(const uml::platform::CameraInfo&)> sensorOrientation;
    /* The synthetic front camera is mounted at 90 and states 270 with a half turn (SyntheticCameraOptions). */
    bool frontPresentedAt90 {};
};

class Recorder final : public CameraSessionDelegate {
public:
    void cameraFrame(std::size_t, CameraFrame frame) override { frames.push_back(std::move(frame)); }
    void cameraStill(std::size_t, CameraStillResult) override {}
    void cameraEvent(CameraEvent) override {}
    std::vector<CameraFrame> frames;
};

/* The host the device is given: the synthetic camera, watched and faulted. It sees what the platform holds, can refuse the
 * start of a video that is to replace a running one (the platform's rollback keeps the old capture), and can hold a frame
 * callback while the device acts. */
class WatchedCamera final : public Camera, public CameraSessionDelegate {
public:
    explicit WatchedCamera(std::shared_ptr<SyntheticCamera> synthetic) : host(std::move(synthetic)) {}

    std::shared_ptr<SyntheticCamera> host;
    CameraSessionDelegate* sink {};
    /* The next startVideo is refused, and the capture it was to replace keeps running. */
    bool refuseNextStart {};
    /* While it is set the platform takes no still (a still without the colour data is asked for again, so one refusal is two). */
    bool refuseStills {};
    /* The stills the platform holds, by token. */
    std::set<std::uint64_t> holding;
    /* The most the delivered results' payloads held when a capture was started. */
    std::size_t payloadWhenStarting {};
    /* What the platform was asked, in order: a camera's settings applied (with the stabilization they ask for) or its video
     * started (the platform camera). */
    struct PlatformCall {
        enum class Kind : std::uint8_t { apply, startVideo } kind;
        std::size_t camera;
        bool stabilization;
    };
    std::vector<PlatformCall> calls;
    /* Called inside startVideo once the capture runs, where a platform's output queue can deliver a frame before the call
     * returns (AVFoundation's does). */
    std::function<void(std::size_t)> afterStart;
    /* What the settings applied for a camera were when each frame of it arrived: how many applies, and the stabilization
     * of the latest. */
    struct FrameSight {
        std::size_t applies;
        bool stabilization;
    };
    std::vector<FrameSight> framesSeen;

    std::mutex pauseMutex;
    std::condition_variable pauseCondition;
    bool pauseFrame {}, frameEntered {}, releaseFrame {};
    void pauseNextFrame()
    {
        std::lock_guard lock(pauseMutex);
        pauseFrame = true;
        frameEntered = releaseFrame = false;
    }
    void awaitFrame()
    {
        std::unique_lock lock(pauseMutex);
        require(pauseCondition.wait_for(lock, std::chrono::seconds(2), [&] { return frameEntered; }), "a frame callback is held");
    }
    void releaseHeldFrame()
    {
        std::lock_guard lock(pauseMutex);
        releaseFrame = true;
        pauseCondition.notify_all();
    }

    std::optional<uml::platform::CameraStillSize> rawStillSize;
    std::vector<CameraInfo> cameras() const override
    {
        auto infos = host->cameras();
        if (rawStillSize && !infos.empty()) infos[0].rawStillSize = *rawStillSize;
        return infos;
    }
    std::vector<std::vector<std::size_t>> concurrentSets() const override { return host->concurrentSets(); }
    std::string hostModel() const override { return host->hostModel(); }
    std::uint64_t now() const noexcept override { return host->now(); }
    std::unique_ptr<Nv12Transformer> makeFrameTransformer() const override { return host->makeFrameTransformer(); }
    std::optional<std::uint32_t> workingBytes;
    std::optional<std::uint32_t> turnedWorkingBytes;
    std::uint32_t stillWorkingBytesPerPixel() const noexcept override
    {
        return workingBytes ? *workingBytes : host->stillWorkingBytesPerPixel();
    }
    std::uint32_t turnedStillWorkingBytesPerPixel() const noexcept override
    {
        return turnedWorkingBytes ? *turnedWorkingBytes : host->turnedStillWorkingBytesPerPixel();
    }

    class Session final : public CameraSession {
    public:
        Session(WatchedCamera& owner, std::unique_ptr<CameraSession> session) : camera(owner), inner(std::move(session)) {}
        bool startVideo(std::size_t index, std::uint32_t width, std::uint32_t height, std::uint32_t rate) override
        {
            camera.calls.push_back({PlatformCall::Kind::startVideo, index, false});
            if (camera.refuseNextStart) {
                camera.refuseNextStart = false;
                return false;
            }
            const bool started = inner->startVideo(index, width, height, rate);
            if (started && camera.afterStart) camera.afterStart(index);
            return started;
        }
        void stopVideo(std::size_t index) override { inner->stopVideo(index); }
        void apply(std::size_t index, const CameraSettings& settings) override
        {
            camera.calls.push_back({PlatformCall::Kind::apply, index, settings.stabilization});
            inner->apply(index, settings);
        }
        bool takeStill(std::size_t index, const CameraStillRequest& request) override
        {
            camera.payloadWhenStarting = std::max(camera.payloadWhenStarting, payload_watch::bytes());
            if (camera.refuseStills) return false;
            const bool taken = inner->takeStill(index, request);
            if (taken) camera.holding.insert(request.token);
            return taken;
        }
        void setAnchors(std::span<const std::optional<std::size_t>> formats, const CameraLimits& limits) override
        {
            inner->setAnchors(formats, limits);
        }
        void stop() override
        {
            inner->stop();
            camera.holding.clear();
        }

    private:
        WatchedCamera& camera;
        std::unique_ptr<CameraSession> inner;
    };

    std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) override
    {
        sink = &delegate;
        auto inner = host->openSession(*this);
        if (!inner) return nullptr;
        return std::make_unique<Session>(*this, std::move(inner));
    }
    void cameraFrame(std::size_t index, CameraFrame frame) override
    {
        FrameSight sight {};
        for (const auto& call : calls)
            if (call.kind == PlatformCall::Kind::apply && call.camera == index) {
                ++sight.applies;
                sight.stabilization = call.stabilization;
            }
        framesSeen.push_back(sight);
        {
            std::unique_lock lock(pauseMutex);
            if (pauseFrame) {
                pauseFrame = false;
                frameEntered = true;
                pauseCondition.notify_all();
                require(pauseCondition.wait_for(lock, std::chrono::seconds(2), [&] { return releaseFrame; }), "the held frame is released");
            }
        }
        sink->cameraFrame(index, std::move(frame));
    }
    /* Changes a result before the device gets it, as a platform that fails a still in a way the synthetic camera has no
     * hook for would. */
    std::function<void(CameraStillResult&)> alterStill;
    void cameraStill(std::size_t index, CameraStillResult result) override
    {
        if (alterStill) alterStill(result);
        payload_watch::track(result.jpeg.data(), result.jpeg.capacity());
        payload_watch::track(result.raw.data(), result.raw.capacity() * sizeof(std::uint16_t));
        holding.erase(result.token);
        sink->cameraStill(index, std::move(result));
    }
    void cameraEvent(CameraEvent event) override { sink->cameraEvent(event); }
};

struct Rig final {
    explicit Rig(RigOptions rigOptions = {}) : memory(24u << 20, 0xcc)
    {
        SyntheticCameraOptions synthetic;
        synthetic.profile = rigOptions.profile;
        synthetic.manualClock = true;
        synthetic.authorized = rigOptions.authorized;
        synthetic.frontPresentedAt90 = rigOptions.frontPresentedAt90;
        camera = makeSyntheticCamera(synthetic);
        store = rigOptions.store ? rigOptions.store : std::make_shared<CalibrationStore>();
        CameraDeviceOptions options;
        options.mode = rigOptions.mode;
        options.tier = rigOptions.tier;
        options.use = rigOptions.use;
        options.calibration = store;
        options.sensorOrientation = rigOptions.sensorOrientation;
        options.onState = [this](CameraState state) {
            states.push_back(state);
            if (onStateChange) onStateChange();
        };
        options.onVideoMode = [this](bool inMode) { videoModes.push_back(inMode); };
        options.automaticRotation = [this](const uml::platform::CameraInfo&) { return automaticDegrees ? *automaticDegrees : 0u; };
        options.onCheckpoint = [this](CameraCheckpoint where) { checkpoints.push_back(where); };
        options.onStillFailure = [this](const CameraStillFailure& failure) { stillFailures.push_back(failure); };
        options.background = background;
        options.onCameraPresentation = [this](const CameraPresentation& presentation) {
            presentations.push_back(presentation);
            if (presentationThrows) throw std::runtime_error("the owner's record failed");
        };
        options.onRotateAndCrop = [this](std::uint32_t camera, std::uint32_t setting, std::uint32_t degrees) {
            rotations.push_back({camera, setting, degrees});
        };
        options.onFlash = [this](std::uint32_t camera, bool torch, uml::platform::CameraFlashMode flash) {
            lights.push_back({camera, torch, flash});
        };
        watched = std::make_shared<WatchedCamera>(camera);
        watched->workingBytes = rigOptions.workingBytes;
        watched->turnedWorkingBytes = rigOptions.turnedWorkingBytes;
        watched->rawStillSize = rigOptions.rawStillSize;
        device = std::make_unique<VirtioCameraDevice>(watched, std::move(options));
        device->setGuestMapper([this](std::uint64_t offset, std::size_t size) {
            if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t> {};
            return std::span(memory).subspan(offset, size);
        });
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    // The device closes a session that is open as it goes, and says so to the
    // recorders below, which must outlive it.
    ~Rig() { device.reset(); }

    std::shared_ptr<SyntheticCamera> camera;
    std::shared_ptr<WatchedCamera> watched;
    std::shared_ptr<CalibrationStore> store;
    /* The application's way to end the guest's video when it leaves the foreground (`CameraDeviceOptions::background`). */
    std::shared_ptr<BackgroundVideo> background {std::make_shared<BackgroundVideo>()};
    std::unique_ptr<VirtioCameraDevice> device;
    std::vector<CameraState> states;
    /* What the device told onVideoMode, in order. */
    std::vector<bool> videoModes;
    /* Called on the worker where the device tells the application its state changed, which is
     * while it handles a platform event, with the frames of its batch in hand. */
    std::function<void()> onStateChange;
    /* The diagnostic checkpoints the device reported, in order. */
    std::vector<CameraCheckpoint> checkpoints;
    /* What the device told onStillFailure, in order. */
    std::vector<CameraStillFailure> stillFailures;
    /* What the device told onCameraPresentation, in order; the owner may be made to fail. */
    std::vector<CameraPresentation> presentations;
    bool presentationThrows {};
    /* What the device reported to onRotateAndCrop: camera, setting, degrees. */
    std::vector<std::array<std::uint32_t, 3>> rotations;
    /* What the device reported to onFlash. */
    struct Light {
        std::uint32_t camera {};
        bool torch {};
        uml::platform::CameraFlashMode flash {};
    };
    std::vector<Light> lights;
    std::uint32_t* automaticDegrees {};
    std::vector<std::uint8_t> memory;
    std::size_t bump {16 * page};
    std::uint64_t next {1};
    std::map<std::uint64_t, std::vector<std::uint8_t>> completed;
    /* The requests in the order the device completed them. */
    std::vector<std::uint64_t> completionOrder;
    // The server's side of the queues (vhost_user_server.cpp): the event buffers
    // the guest has posted wait in its ring in order and are offered again
    // whenever the device may have an event; a request the device defers is
    // held until a completion answers it.
    std::deque<std::uint64_t> eventRing;
    std::set<std::uint64_t> held;
    unsigned resets {};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::pair<std::uint64_t, std::uint32_t>>> segments;

    // --- Commands ----------------------------------------------------------

    template<class T> VhostUserQueueResult send(const T& value, std::size_t capacity = 16384)
    {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device->processQueue(0, next++, request, capacity);
    }
    static std::uint32_t type(const VhostUserQueueResult& result) { return load<virtio_video_cmd_hdr>(result.response).type; }
    template<class T> void ok(const T& value, std::source_location where = std::source_location::current())
    {
        const auto result = send(value);
        require(result.action == VhostUserQueueAction::complete && type(result) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "a command failed", where);
    }
    template<class T> void refused(const T& value, std::uint32_t expected, const char* what,
                                   std::source_location where = std::source_location::current())
    {
        const auto result = send(value);
        require(result.action == VhostUserQueueAction::complete && type(result) == expected, what, where);
    }

    void create(std::uint32_t stream, std::uint32_t cameraIndex, std::uint32_t kind,
                std::source_location where = std::source_location::current())
    {
        virtio_video_camera_stream_create command {};
        command.hdr = {VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, stream};
        command.camera_index = cameraIndex;
        command.kind = kind;
        ok(command, where);
    }
    void destroy(std::uint32_t stream) { ok(virtio_video_stream_destroy {{VIRTIO_VIDEO_CMD_STREAM_DESTROY, stream}}); }
    virtio_video_params params(std::uint32_t stream, std::source_location where = std::source_location::current())
    {
        const auto result = send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, stream}, output, {}});
        require(type(result) == VIRTIO_VIDEO_RESP_OK_GET_PARAMS, "GET_PARAMS failed", where);
        return load<virtio_video_get_params_resp>(result.response).params;
    }
    VhostUserQueueResult setParams(std::uint32_t stream, std::uint32_t format, std::uint32_t width, std::uint32_t height)
    {
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, stream};
        set.params.queue_type = output;
        set.params.format = format;
        set.params.frame_width = width;
        set.params.frame_height = height;
        return send(set);
    }
    void setVideo(std::uint32_t stream, std::uint32_t width, std::uint32_t height,
                  std::source_location where = std::source_location::current())
    {
        const auto result = setParams(stream, VIRTIO_VIDEO_FORMAT_NV12, width, height);
        require(type(result) == VIRTIO_VIDEO_RESP_OK_NODATA, "SET_PARAMS failed", where);
    }
    VhostUserQueueResult setInterval(std::uint32_t stream, std::uint32_t numerator, std::uint32_t denominator)
    {
        virtio_video_set_stream_info set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, stream};
        set.info.queue_type = output;
        set.info.frame_interval_numerator = numerator;
        set.info.frame_interval_denominator = denominator;
        return send(set);
    }

    // --- Guest memory ------------------------------------------------------

    /* A buffer of `size` bytes in two pieces with a page between them. */
    void attach(std::uint32_t stream, std::uint32_t resource, std::uint32_t size,
                std::source_location where = std::source_location::current())
    {
        const std::uint32_t first = (size / 2) & ~15u;
        const std::uint32_t second = size - first;
        const auto place = [&](std::uint32_t length) {
            const auto at = bump;
            bump += (length + page - 1) / page * page + page;
            require(bump <= memory.size(), "the guest memory is full", where);
            return at;
        };
        const auto firstAt = place(first), secondAt = place(second);
        segments[{stream, resource}] = {{firstAt, first}, {secondAt, second}};
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, stream, output, resource});
        append(request, std::uint32_t(2));
        append(request, std::uint32_t(0));
        append(request, virtio_video_resource_sg_entry {firstAt, first, 0});
        append(request, virtio_video_resource_sg_entry {secondAt, second, 0});
        const auto result = device->processQueue(0, next++, request, 16384);
        require(result.action == VhostUserQueueAction::complete && type(result) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "RESOURCE_ATTACH failed", where);
    }
    std::vector<std::uint8_t> bytes(std::uint32_t stream, std::uint32_t resource, std::size_t count)
    {
        std::vector<std::uint8_t> out;
        for (const auto& [address, length] : segments.at({stream, resource})) {
            const auto take = std::min<std::size_t>(length, count - out.size());
            out.insert(out.end(), memory.begin() + std::ptrdiff_t(address), memory.begin() + std::ptrdiff_t(address + take));
            if (out.size() == count) break;
        }
        return out;
    }

    // --- Buffers -----------------------------------------------------------

    std::uint64_t queue(std::uint32_t stream, std::uint32_t resource, std::uint64_t timestamp = 0, std::uint32_t flags = 0,
                        std::source_location where = std::source_location::current())
    {
        virtio_video_resource_queue command {};
        command.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        command.stream_id = stream;
        command.queue_type = output;
        command.resource_id = resource;
        command.flags = flags;
        command.timestamp = timestamp;
        std::vector<std::uint8_t> request;
        append(request, command);
        const auto id = next++;
        const auto result = device->processQueue(0, id, request, sizeof(virtio_video_camera_queue_resp));
        require(result.action == VhostUserQueueAction::defer, "RESOURCE_QUEUE did not defer", where);
        held.insert(id);
        collect();
        return id;
    }
    /* The guest posts event buffers. The device never keeps one: an event that is
     * not there yet leaves the buffer in the ring (retry). */
    std::vector<std::uint64_t> postEventBuffers(int count)
    {
        std::vector<std::uint64_t> ids;
        for (int index = 0; index < count; ++index) {
            ids.push_back(next++);
            eventRing.push_back(ids.back());
        }
        serveEvents();
        return ids;
    }
    /* What the server does with the event queue each time the device wakes it. */
    void serveEvents()
    {
        while (!eventRing.empty()) {
            const auto result = device->processQueue(1, eventRing.front(), {}, sizeof(virtio_video_event));
            require(result.action != VhostUserQueueAction::defer, "the event queue held a buffer");
            if (result.action == VhostUserQueueAction::retry) break;
            completed[eventRing.front()] = result.response;
            eventRing.pop_front();
        }
    }
    void collect()
    {
        VhostUserQueueCompletion completion;
        while (device->takeQueueCompletion(completion)) {
            completionOrder.push_back(completion.requestId);
            completed[completion.requestId] = completion.response;
            held.erase(completion.requestId);
        }
        serveEvents();
    }
    /* A guest suspend, as the server sees it: the rings are disabled. The device
     * keeps its deferred requests across the disable (it says so), so nothing is
     * reset and what it completes meanwhile waits for the enable. */
    void disableRings() { collect(); }
    void enableRings() { collect(); }
    /* What does reset the device: a change of the guest's memory or of a ring, a
     * teardown, or the end of the connection. The requests it held are dropped,
     * the buffers waiting in the ring are not. */
    void resetByTransport()
    {
        collect();
        device->reset();
        held.clear();
        ++resets;
    }
    void step()
    {
        device->drainEventSource();
        collect();
    }
    /* `frames` frames of a camera, one at a time: the device writes the newest
     * frame of a camera each time it runs, so a burst of frames is one. */
    void pumpSteps(std::size_t platformCamera, unsigned frames)
    {
        for (unsigned frame = 0; frame < frames; ++frame) {
            camera->pump(platformCamera, 1);
            step();
        }
    }
    bool done(std::uint64_t request) const { return completed.contains(request); }
    virtio_video_camera_queue_resp result(std::uint64_t request, std::source_location where = std::source_location::current()) const
    {
        const auto found = completed.find(request);
        require(found != completed.end(), "a buffer did not complete", where);
        require(load<virtio_video_cmd_hdr>(found->second).type == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, "not a queue response", where);
        return load<virtio_video_camera_queue_resp>(found->second);
    }
    /* Waits, with a bound, for the device's own wake-up (a timer or the platform). */
    bool waitFor(const std::function<bool()>& condition, int milliseconds = 4000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        step();
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            pollfd descriptor {device->eventDescriptor(), POLLIN, 0};
            ::poll(&descriptor, 1, 20);
            step();
        }
        return true;
    }

    // --- Controls ----------------------------------------------------------

    struct Item {
        std::uint32_t id {};
        std::int64_t value {};
        std::vector<std::uint8_t> payload;
    };
    VhostUserQueueResult controls(std::uint32_t command, std::uint32_t stream, const std::vector<Item>& items)
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_v4l2_controls {{command, stream}, std::uint32_t(items.size()), {}});
        std::vector<std::uint8_t> area;
        for (const auto& item : items) {
            virtio_video_v4l2_control_value entry {item.id, 0, std::uint64_t(item.value)};
            if (!item.payload.empty()) {
                entry.size = std::uint32_t(item.payload.size());
                entry.value = area.size();
                area.insert(area.end(), item.payload.begin(), item.payload.end());
                area.resize((area.size() + 7) / 8 * 8);
            }
            append(request, entry);
        }
        request.insert(request.end(), area.begin(), area.end());
        return device->processQueue(0, next++, request, 16384);
    }
    std::uint32_t set(std::uint32_t stream, const std::vector<Item>& items)
    {
        const auto result = controls(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, stream, items);
        require(result.action == VhostUserQueueAction::complete, "SET_V4L2_CONTROLS did not complete");
        return type(result);
    }
    void setOk(std::uint32_t stream, const std::vector<Item>& items, std::source_location where = std::source_location::current())
    {
        require(set(stream, items) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "a control set failed", where);
    }
    std::vector<std::int64_t> get(std::uint32_t stream, const std::vector<std::uint32_t>& ids,
                                  std::source_location where = std::source_location::current())
    {
        std::vector<Item> items;
        for (const auto id : ids) items.push_back({id, 0, {}});
        const auto result = controls(VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, stream, items);
        require(type(result) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "a control get failed", where);
        std::vector<std::int64_t> values;
        for (std::size_t index = 0; index < ids.size(); ++index)
            values.push_back(std::int64_t(load<virtio_video_v4l2_control_value>(
                                              result.response, sizeof(virtio_video_v4l2_controls_resp) +
                                                  index * sizeof(virtio_video_v4l2_control_value)).value));
        return values;
    }
};

virtio_video_camera_meta metaOf(Rig& rig, std::uint32_t stream, std::uint32_t resource)
{
    return load<virtio_video_camera_meta>(rig.bytes(stream, resource, metaSize));
}

std::uint32_t nv12Size(std::uint32_t width, std::uint32_t height) { return rawPitch(width) * (height + height / 2); }

/* What the device should have written for a frame the platform produced. */
std::vector<std::uint8_t> expectedNv12(const CameraFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t rotation)
{
    const auto pitch = rawPitch(width);
    std::vector<std::uint8_t> out(nv12Size(width, height));
    const Nv12Target target {out.data(), pitch, out.data() + std::size_t(pitch) * height, pitch, width, height};
    const Nv12Source source {frame.luma.data(), frame.lumaStride, frame.chroma.data(), frame.chromaStride, frame.width, frame.height};
    const bool quarter = rotation == 90 || rotation == 270;
    const auto crop = centredCrop(frame.width, frame.height, width, height, quarter);
    require(transformNv12(source, crop, rotation, target), "the reference transform failed");
    return out;
}

// ---------------------------------------------------------------------------

void testHandshake()
{
    Rig rig;
    const auto& device = *rig.device;
    require(device.queueCount() == 2, "commands and events");
    require(device.keepsDeferredRequestsWhileDisabled(), "a camera stream outlives a guest suspend");
    const std::uint64_t features = device.features();
    require((features >> VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES & 1) && (features >> VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG & 1) &&
                (features >> VIRTIO_VIDEO_F_V4L2_CONTROLS & 1) && !(features >> VIRTIO_VIDEO_F_RESOURCE_SHM_REGION & 1),
            "the features");
    const auto config = load<virtio_video_config>(device.configuration());
    require(config.version == UML_VIRTIO_VIDEO_VERSION && config.max_caps_length == 16384 && config.max_resp_length == 4096,
            "the configuration");
    require(cameraDeviceId == UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID, "the device ID matches the wire");
    require(device.cameras().size() == 2, "a tablet has two cameras");

    const auto query = rig.send(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 0, {}}, 4096);
    require(query.action == VhostUserQueueAction::complete, "the query completes");
    const auto header = load<virtio_video_camera_query_resp>(query.response);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY && header.num_cameras == 2 && header.num_descs == 2 &&
                header.session_flags == 0 && header.next_index == 0,
            "the camera list");
    const auto back = load<virtio_video_camera_desc>(query.response, sizeof(header));
    require(back.index == 0 && back.facing == VIRTIO_VIDEO_CAMERA_FACING_BACK && (back.flags & VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS),
            "camera 0 is the back camera");
    // A short buffer pages the list.
    const auto paged = rig.send(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 0, {}},
                                sizeof(header) + sizeof(virtio_video_camera_desc));
    require(load<virtio_video_camera_query_resp>(paged.response).num_descs == 1 &&
                load<virtio_video_camera_query_resp>(paged.response).next_index == 1,
            "a short buffer gets one record and the next index");
    rig.refused(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 9, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a start index past the end");
    rig.refused(virtio_video_get_params {{0x7777, 0}, output, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
                "an unknown command on no stream");

    // Formats.
    const auto formats = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, videoKind});
    require(load<virtio_video_query_capability_resp>(formats.response).num_descs == 1 &&
                load<virtio_video_format_desc>(formats.response, sizeof(virtio_video_query_capability_resp)).format ==
                    VIRTIO_VIDEO_FORMAT_NV12,
            "video is NV12");
    const auto stills = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, stillKind});
    require(load<virtio_video_query_capability_resp>(stills.response).num_descs == 1, "an uncalibrated camera offers JPEG only");
    rig.refused(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, metaKind},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "formats of a metadata stream");
    rig.refused(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 5, videoKind},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "formats of a camera that is not there");

    // The controls, in pages, are the controls whole.
    std::vector<std::uint32_t> whole, paging;
    const auto walk = [&](std::size_t capacity, std::vector<std::uint32_t>& ids) {
        std::uint32_t start = 0;
        for (int guard = 0; guard < 100; ++guard) {
            const auto answer =
                rig.send(virtio_video_camera_query_controls {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS, 0}, 0, start}, capacity);
            require(Rig::type(answer) == VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, "the controls answer");
            const auto page = load<virtio_video_query_v4l2_controls_resp>(answer.response);
            for (std::uint32_t index = 0; index < page.num_descs; ++index)
                ids.push_back(load<virtio_video_v4l2_control_desc>(answer.response, sizeof(page) +
                                                                       index * sizeof(virtio_video_v4l2_control_desc)).id);
            require(answer.response.size() == sizeof(page) + page.num_descs * sizeof(virtio_video_v4l2_control_desc),
                    "exactly the records");
            if (page.next_index == 0) return;
            start = page.next_index;
        }
        require(false, "the control walk does not end");
    };
    walk(8192, whole);
    walk(sizeof(virtio_video_query_v4l2_controls_resp) + 3 * sizeof(virtio_video_v4l2_control_desc), paging);
    require(whole.size() > 20 && whole == paging, "pages add up to the whole list");
    require(std::set<std::uint32_t>(whole.begin(), whole.end()).size() == whole.size(), "no control twice");
    rig.refused(virtio_video_camera_query_controls {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS, 0}, 7, 0},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "controls of a camera that is not there");
    require(rig.camera->sessionsOpened() == 0, "questions open no capture session");
}

void testStreamsAndParameters()
{
    Rig rig;
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 0}, 0, videoKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "stream 0");
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 1}, 9, videoKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a camera that is not there");
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 1}, 0, 9, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a stream kind that is not one");
    rig.create(1, 0, videoKind);
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 1}, 0, videoKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a stream ID in use");

    // A new video stream starts at 720p, 30 frames a second, rows at 16 bytes.
    auto p = rig.params(1);
    require(p.queue_type == output && p.format == VIRTIO_VIDEO_FORMAT_NV12 && p.frame_width == 1280 && p.frame_height == 720 &&
                p.frame_rate == 30 && p.num_planes == 1 && p.plane_formats[0].stride == 1280 &&
                p.plane_formats[0].plane_size == 1280 * 1080 && p.min_buffers == 1 && p.max_buffers == 4,
            "the default video format");
    rig.setVideo(1, 640, 480);
    p = rig.params(1);
    require(p.frame_width == 640 && p.frame_height == 480 && p.plane_formats[0].plane_size == 640 * 720, "640 x 480");
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 3840, 2160)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "4K is above the small tier");
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 641, 480)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an odd size");
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_H264, 640, 480)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a coded format");
    require(rig.params(1).frame_width == 640, "a refused change changes nothing");

    // The interval, and the colour the frames carry.
    const auto info = rig.send(virtio_video_get_stream_info {{VIRTIO_VIDEO_CMD_GET_STREAM_INFO, 1}, output, {}});
    const auto stream = load<virtio_video_get_stream_info_resp>(info.response).info;
    require(Rig::type(info) == VIRTIO_VIDEO_RESP_OK_STREAM_INFO && stream.frame_interval_numerator == 1 &&
                stream.frame_interval_denominator == 30 && stream.colorspace == 3 && stream.xfer_func == 1 &&
                stream.ycbcr_enc == 2 && stream.quantization == 1,
            "BT.709 full range at 30 frames a second");
    require(Rig::type(rig.setInterval(1, 1, 15)) == VIRTIO_VIDEO_RESP_OK_NODATA && rig.params(1).frame_rate == 15, "15 frames a second");
    require(Rig::type(rig.setInterval(1, 1, 1000)) == VIRTIO_VIDEO_RESP_OK_NODATA && rig.params(1).frame_rate == 30,
            "a rate above the camera's is the highest it offers: the cap, 30");
    require(Rig::type(rig.setInterval(1, 0, 30)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a zero interval changes nothing");

    // A buffer smaller than the format is refused; one the size of it is taken.
    std::vector<std::uint8_t> request;
    append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 1, output, 1});
    append(request, std::uint32_t(1));
    append(request, std::uint32_t(0));
    append(request, virtio_video_resource_sg_entry {page * 20, 4096, 0});
    require(Rig::type(rig.device->processQueue(0, 900, request, 4096)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a short buffer");
    rig.attach(1, 2, nv12Size(640, 480));
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 320, 240)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "the format cannot change with buffers allocated");

    // Still and metadata streams.
    rig.create(2, 0, stillKind);
    p = rig.params(2);
    require(p.format == VIRTIO_VIDEO_FORMAT_CAMERA_JPEG && p.frame_width == 4032 && p.frame_height == 3024 &&
                p.plane_formats[0].plane_size == 4032u * 3024u * 3 / 2 + exifGapLength,
            "a still stream starts at the largest JPEG");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a JPEG size");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1000, 700)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a size the camera does not take stills at");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB, 1008, 756)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "RAW before the lens is calibrated");
    rig.create(3, 0, metaKind);
    p = rig.params(3);
    require(p.format == VIRTIO_VIDEO_FORMAT_CAMERA_META && p.plane_formats[0].plane_size == metaSize, "a metadata stream");
    // The front camera is a camera of its own.
    rig.create(4, 1, videoKind);
    require(Rig::type(rig.send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 99}, output, {}})) ==
                VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
            "an unknown stream");
    rig.destroy(1);
    rig.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, output, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
                "a destroyed stream");
    require(rig.camera->sessionsOpened() == 0, "no capture without a buffer");
}

void testVideoAndMetadata()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.create(2, 0, metaKind);
    for (std::uint32_t resource = 1; resource <= 5; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    for (std::uint32_t resource = 1; resource <= 4; ++resource) rig.attach(2, resource, metaSize);
    std::vector<std::uint64_t> metas, frames;
    for (std::uint32_t resource = 1; resource <= 3; ++resource) metas.push_back(rig.queue(2, resource));
    require(rig.camera->sessionsOpened() == 0, "a metadata buffer alone starts no capture");
    for (std::uint32_t resource = 1; resource <= 3; ++resource) frames.push_back(rig.queue(1, resource));
    require(rig.camera->sessionsOpened() == 1 && rig.camera->videoRunning(0) && rig.camera->videoFrameRate(0) == 30,
            "the first video buffer starts the camera at the stream's rate");
    require(rig.camera->sessionAnchorCalls() == 1 && rig.camera->sessionAnchors() == std::vector<std::optional<std::size_t>> {3, std::nullopt} &&
                rig.camera->sessionLimits().maximumStillPixels == limitsOf(CameraTier::small).maximumStillPixels &&
                rig.camera->sessionLimits().maximumFrameBytes == maximumDeliveredFrameBytes(limitsOf(CameraTier::small)) &&
                rig.camera->sessionLimits().frameRowAlignment == captureRowAlignment,
            "the session is told each camera's anchor (the back camera's 1920x1440 format; the front camera lists none) and what the guest may ask, once, before it starts anything");
    require(rig.states == std::vector<CameraState> {CameraState::streaming}, "the host is streaming");

    rig.pumpSteps(0, 3);
    const std::uint64_t interval = 1'000'000'000ull / 30;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto frame = rig.result(frames[index]);
        require(frame.base.flags == 0 && frame.base.data_sizes[0] == nv12Size(640, 480), "a frame fills its buffer");
        require(frame.sequence == index + 1, "frames are numbered from 1 in capture order");
        require(frame.base.timestamp == interval, "the timestamp is the frame's age");
        const auto picture = rig.bytes(1, std::uint32_t(index + 1), nv12Size(640, 480));
        const auto marker = decodeChartMarker(picture.data(), 640, 640, 480);
        require(marker.valid && marker.tag == chartTag(0) && marker.counter == index + 1, "the buffer holds that frame's picture");

        const auto record = rig.result(metas[index]);
        require(record.base.flags == 0 && record.base.data_sizes[0] == metaSize && record.sequence == index + 1 &&
                    record.base.timestamp == frame.base.timestamp,
                "the metadata completes with its frame");
        const auto meta = metaOf(rig, 2, std::uint32_t(index + 1));
        require(meta.sequence == index + 1 && meta.flags == 0 && meta.active_camera == 0 && meta.frame_duration_ns > 0 &&
                    meta.exposure_ns > 0 && meta.iso >= 25 && meta.zoom_ratio_q16 == 65536 && meta.focal_length_um > 0 &&
                    meta.jpeg_gap_offset == 0 && meta.jpeg_gap_length == 0,
                "the record states what the exposure did");
    }

    // A frame with no buffer is dropped and its number is spent; the record is kept for the next metadata buffer.
    rig.camera->pump(0, 1);
    rig.step();
    const auto lateMeta = rig.queue(2, 4);
    require(rig.done(lateMeta) && rig.result(lateMeta).sequence == 4, "the record of the dropped frame is served from the backlog");
    const auto fifth = rig.queue(1, 4);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.result(fifth).sequence == 5, "and the next frame is number 5");

    // Clearing the queue returns what is queued, with an error, and releases the capture.
    const auto queued = rig.queue(1, 5);
    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    rig.collect();
    require(rig.done(queued) && (rig.result(queued).base.flags & errorFlag), "a cleared buffer completes with an error");
    require(!rig.camera->videoRunning(0) && rig.camera->sessionsOpen() == 0, "with no stream left the capture is released");
    require(rig.states == std::vector<CameraState> {CameraState::streaming, CameraState::idle}, "the host is idle again");
    require(rig.checkpoints == std::vector<CameraCheckpoint> {CameraCheckpoint::sessionOpened, CameraCheckpoint::sessionClosed},
            "the memory checkpoints are the session opening and closing");
    rig.destroy(1);
    rig.destroy(2);
}

void testDecimation()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.create(2, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.setVideo(2, 640, 480);
    require(Rig::type(rig.setInterval(2, 1, 15)) == VIRTIO_VIDEO_RESP_OK_NODATA, "15 frames a second");
    for (std::uint32_t resource = 1; resource <= 6; ++resource) {
        rig.attach(1, resource, nv12Size(640, 480));
        rig.attach(2, resource, nv12Size(640, 480));
    }
    std::vector<std::uint64_t> fast, slow;
    for (std::uint32_t resource = 1; resource <= 6; ++resource) fast.push_back(rig.queue(1, resource));
    for (std::uint32_t resource = 1; resource <= 3; ++resource) slow.push_back(rig.queue(2, resource));
    require(rig.camera->videoFrameRate(0) == 30, "the camera runs at the faster stream's rate");
    for (int frame = 0; frame < 6; ++frame) {
        rig.camera->pump(0, 1);
        rig.step();
    }
    for (std::size_t index = 0; index < 6; ++index) require(rig.result(fast[index]).sequence == index + 1, "the fast stream has every frame");
    require(rig.result(slow[0]).sequence == 1 && rig.result(slow[1]).sequence == 3 && rig.result(slow[2]).sequence == 5,
            "the slow stream has every other frame");
}

void testPipeline()
{
    struct Case {
        std::uint32_t width, height, control, degrees;
        const char* what;
    };
    const Case cases[] = {
        {640, 480, 0, 0, "native size"},
        {320, 240, 0, 0, "scaled down"},
        {176, 144, 0, 0, "scaled to a small size"},
        {640, 360, 0, 0, "cropped to 16:9"},
        {640, 480, 2, 180, "turned 180 degrees"},
        {480, 640, 1, 90, "turned 90 degrees, as a portrait"},
        {480, 640, 3, 270, "turned 270 degrees, as a portrait"},
    };
    for (const auto& test : cases) {
        Rig rig;
        Recorder recorder;
        auto reference = rig.camera->openSession(recorder);
        require(reference != nullptr, test.what);
        rig.create(1, 0, videoKind);
        rig.setVideo(1, test.width, test.height);
        if (test.control != 0) rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, std::int64_t(test.control), {}}});
        rig.attach(1, 1, nv12Size(test.width, test.height));
        rig.attach(1, 2, nv12Size(test.width, test.height));
        const auto first = rig.queue(1, 1);
        const auto second = rig.queue(1, 2);
        require(reference->startVideo(0, test.width, test.height, 30), test.what);
        rig.pumpSteps(0, 2);
        require(recorder.frames.size() == 2, test.what);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto answer = rig.result(index == 0 ? first : second);
            require(answer.base.flags == 0 && answer.base.data_sizes[0] == nv12Size(test.width, test.height), test.what);
            const auto wanted = expectedNv12(recorder.frames[index], test.width, test.height, test.degrees);
            require(rig.bytes(1, std::uint32_t(index + 1), wanted.size()) == wanted, test.what);
        }
        reference->stop();
    }

    // The automatic setting follows the application's rotation, picked up at the next frame.
    Rig rig;
    std::uint32_t degrees = 90;
    rig.automaticDegrees = &degrees;
    Recorder recorder;
    auto reference = rig.camera->openSession(recorder);
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 480, 640);
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, VIRTIO_VIDEO_CAMERA_ROTATE_AUTOMATIC, {}}});
    for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.attach(1, resource, nv12Size(480, 640));
    rig.queue(1, 1);
    rig.queue(1, 2);
    require(reference->startVideo(0, 480, 640, 30), "reference");
    rig.camera->pump(0, 1);
    rig.step();
    degrees = 270;
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.bytes(1, 1, nv12Size(480, 640)) == expectedNv12(recorder.frames[0], 480, 640, 90), "automatic: 90 degrees");
    require(rig.bytes(1, 2, nv12Size(480, 640)) == expectedNv12(recorder.frames[1], 480, 640, 270), "automatic: now 270 degrees");
    // What reaches the diagnostic hook: the automatic setting (4) and its turn, once per change.
    require(rig.rotations.size() == 2 && rig.rotations[0] == std::array<std::uint32_t, 3> {0, 4, 90} &&
                rig.rotations[1] == std::array<std::uint32_t, 3> {0, 4, 270},
            "the rotation hook says automatic with 90 and then 270 degrees, once each");
    reference->stop();
}

/* A picture of the camera's frames and the guest's buffers alike, turned half around. */
std::vector<std::uint8_t> halfTurned(const std::vector<std::uint8_t>& picture, std::uint32_t width, std::uint32_t height)
{
    const auto pitch = rawPitch(width);
    std::vector<std::uint8_t> out(picture.size());
    const Nv12Source source {picture.data(), pitch, picture.data() + std::size_t(pitch) * height, pitch, width, height};
    const Nv12Target target {out.data(), pitch, out.data() + std::size_t(pitch) * height, pitch, width, height};
    require(transformNv12(source, {0, 0, width, height}, 180, target), "the reference half turn failed");
    return out;
}

/* A front camera whose picture the host turns by 180 degrees (it states 270 over a mounting at 90): every video stream the
 * device writes is the platform's frame turned half around, at the capture's size (the fused row copy) and scaled or cropped
 * (the transformer), alone and with ROTATE_AND_CROP's turn added to it. The chart's marker is readable in the guest's buffer
 * only after the half turn is undone; a camera with no turn (a back camera, a front camera that states 270 as it is) is
 * written as it was. */
void testAFrontCameraWithAHalfTurnIsWrittenTurned()
{
    struct Case {
        std::uint32_t width, height, control, degrees;
        const char* what;
    };
    // `degrees` is the turn the device must apply in all: the half turn plus ROTATE_AND_CROP's.
    const Case cases[] = {
        {640, 480, 0, 180, "native size: the rows go in reversed"},
        {640, 360, 0, 180, "cropped to 16:9 at the capture's width: the rows of the crop go in reversed"},
        {320, 240, 0, 180, "scaled down"},
        {176, 144, 0, 180, "scaled to a small size"},
        {640, 480, 2, 0, "the half turn and ROTATE_AND_CROP's 180 cancel"},
        {480, 640, 1, 270, "ROTATE_AND_CROP's 90 on top of the half turn, as a portrait"},
        {480, 640, 3, 90, "ROTATE_AND_CROP's 270 on top of the half turn, as a portrait"},
    };
    RigOptions options;
    options.frontPresentedAt90 = true;
    for (const auto& test : cases) {
        Rig rig(options);
        Recorder recorder;
        auto reference = rig.camera->openSession(recorder);
        require(reference != nullptr, test.what);
        const auto front = rig.camera->cameras()[1];
        require(front.position == CameraPosition::front && front.sensorOrientation == 270 && front.presentationTurn == 180, test.what);
        rig.create(1, 1, videoKind);
        rig.setVideo(1, test.width, test.height);
        if (test.control != 0) rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, std::int64_t(test.control), {}}});
        rig.attach(1, 1, nv12Size(test.width, test.height));
        rig.attach(1, 2, nv12Size(test.width, test.height));
        const auto first = rig.queue(1, 1);
        const auto second = rig.queue(1, 2);
        require(reference->startVideo(1, test.width, test.height, 30), test.what);
        rig.pumpSteps(1, 2);
        require(recorder.frames.size() == 2, test.what);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto answer = rig.result(index == 0 ? first : second);
            require(answer.base.flags == 0 && answer.base.data_sizes[0] == nv12Size(test.width, test.height), test.what);
            const auto wanted = expectedNv12(recorder.frames[index], test.width, test.height, test.degrees);
            require(rig.bytes(1, std::uint32_t(index + 1), wanted.size()) == wanted, test.what);
        }
        reference->stop();
    }

    // The chart: the platform's frame carries the marker as it was drawn, the guest's buffer carries it turned, and it reads
    // again once the guest's picture is turned half around. Every stream of the camera is turned (a second one alongside).
    {
        Rig rig(options);
        for (std::uint32_t stream : {1u, 2u}) {
            rig.create(stream, 1, videoKind);
            rig.setVideo(stream, 640, 480);
            rig.attach(stream, 1, nv12Size(640, 480));
            rig.queue(stream, 1);
        }
        rig.pumpSteps(1, 1);
        for (std::uint32_t stream : {1u, 2u}) {
            const auto picture = rig.bytes(stream, 1, nv12Size(640, 480));
            // (The check word survives a reversal of the cells, so what tells the turned marker is the tag and counter.)
            const auto turned = decodeChartMarker(picture.data(), 640, 640, 480);
            require(!(turned.valid && turned.tag == chartTag(1)) && turned.counter != 1, "the marker does not read in the turned picture");
            const auto upright = halfTurned(picture, 640, 480);
            const auto marker = decodeChartMarker(upright.data(), 640, 640, 480);
            require(marker.valid && marker.tag == chartTag(1) && marker.counter == 1, "it reads once the half turn is undone");
        }
    }

    // The guest's metering and focus regions are in the picture it is given: the platform is told the area of its own
    // picture, which is the opposite one.
    {
        Rig rig(options);
        rig.create(1, 1, videoKind);
        rig.setVideo(1, 640, 480);
        rig.attach(1, 1, nv12Size(640, 480));
        rig.queue(1, 1);
        const auto wire = [](std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t weight) {
            std::vector<std::uint8_t> region(regionPayloadSize);
            const std::uint32_t words[5] = {x, y, width, height, weight};
            std::memcpy(region.data(), words, sizeof(words));
            return region;
        };
        rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, wire(16384, 32768, 16384, 8192, 5)}});
        const auto applied = rig.camera->appliedSettings(1);
        require(applied.aeRegion && applied.aeRegion->weight == 5 && applied.aeRegion->x == 0.5f && applied.aeRegion->y == 0.375f &&
                    applied.aeRegion->width == 0.25f && applied.aeRegion->height == 0.125f,
                "the metering region reaches the platform mapped back");
    }

    // A back camera, and a front camera that states 270 as it is, are not turned: the marker reads as it was written.
    for (const bool presented : {true, false}) {
        RigOptions plain;
        plain.frontPresentedAt90 = presented;
        Rig rig(plain);
        const std::uint32_t camera = presented ? 0 : 1;
        rig.create(1, camera, videoKind);
        rig.setVideo(1, 640, 480);
        rig.attach(1, 1, nv12Size(640, 480));
        rig.queue(1, 1);
        rig.pumpSteps(camera, 1);
        const auto picture = rig.bytes(1, 1, nv12Size(640, 480));
        const auto marker = decodeChartMarker(picture.data(), 640, 640, 480);
        require(marker.valid && marker.tag == chartTag(camera) && marker.counter == 1,
                presented ? "a back camera is written as it is" : "a front camera that is not turned is written as it is");
    }
}

void testBlockedNeverOpensASession()
{
    RigOptions options;
    options.mode = CameraMode::blocked;
    Rig rig(options);
    const auto query = rig.send(virtio_video_camera_query {{VIRTIO_VIDEO_CMD_CAMERA_QUERY, 0}, 0, {}}, 4096);
    const auto header = load<virtio_video_camera_query_resp>(query.response);
    require(header.num_cameras == 2 && (header.session_flags & VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED),
            "a blocked session still publishes the cameras it found, and says it is blocked");

    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.create(2, 0, metaKind);
    rig.create(3, 0, stillKind);
    require(Rig::type(rig.setParams(3, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a still size");
    for (std::uint32_t resource = 1; resource <= 3; ++resource) {
        rig.attach(1, resource, nv12Size(640, 480));
        rig.attach(2, resource, metaSize);
    }
    rig.attach(3, 1, std::uint32_t(rig.params(3).plane_formats[0].plane_size));

    std::vector<std::uint64_t> metas, frames;
    for (std::uint32_t resource = 1; resource <= 3; ++resource) metas.push_back(rig.queue(2, resource));
    for (std::uint32_t resource = 1; resource <= 3; ++resource) frames.push_back(rig.queue(1, resource));
    require(rig.waitFor([&] { return std::all_of(frames.begin(), frames.end(), [&](auto id) { return rig.done(id); }); }),
            "black frames flow on the device's own timer");
    for (std::size_t index = 0; index < 3; ++index) {
        const auto frame = rig.result(frames[index]);
        require(frame.base.flags == (blackFlag | blockedFlag), "a blocked frame is black and blocked");
        require(frame.base.data_sizes[0] == nv12Size(640, 480) && frame.base.timestamp == 0, "a whole picture, no age");
        const auto picture = rig.bytes(1, std::uint32_t(index + 1), nv12Size(640, 480));
        require(std::all_of(picture.begin(), picture.begin() + 640 * 480, [](auto byte) { return byte == 0; }) &&
                    std::all_of(picture.begin() + 640 * 480, picture.end(), [](auto byte) { return byte == 128; }),
                "the picture is black");
    }
    require(rig.waitFor([&] { return std::all_of(metas.begin(), metas.end(), [&](auto id) { return rig.done(id); }); }),
            "the metadata follows");
    for (std::size_t index = 0; index < 3; ++index) {
        const auto record = rig.result(metas[index]);
        const auto meta = metaOf(rig, 2, std::uint32_t(index + 1));
        require(record.sequence == meta.sequence && meta.sequence >= 1 &&
                    meta.flags == (VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK | VIRTIO_VIDEO_CAMERA_FRAME_F_BLOCKED),
                "metadata says black and blocked");
    }
    // Black frames are not capture, but the guest has a camera open: the application hears it once, however many frames flow.
    require(rig.states == std::vector<CameraState> {CameraState::blockedInUse},
            "a blocked session with streams running reports the open once and nothing else");
    // A still is refused at once.
    const auto still = rig.queue(3, 1, 4321);
    require(rig.done(still), "a still in a blocked session completes at once");
    const auto refusal = rig.result(still);
    require(refusal.base.flags == (errorFlag | blackFlag | blockedFlag) && refusal.base.timestamp == 4321, "with an error and its cookie");

    // Controls are accepted and kept, and reach no platform session.
    rig.setOk(1, {{UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 5'000'000, {}},
                  {UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    require(rig.get(1, {VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, UML_V4L2_CID_PRIVACY}) == std::vector<std::int64_t> {5'000'000, 1},
            "the values are kept, and privacy reads as on");
    require(rig.set(1, {{UML_V4L2_CID_PRIVACY, 0, {}}}) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "privacy is read-only");

    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    rig.destroy(1);
    rig.destroy(2);
    rig.destroy(3);
    require(rig.states == std::vector<CameraState> {CameraState::blockedInUse, CameraState::idle},
            "and that the guest closed every camera");
    rig.device->reset();
    require(rig.camera->sessionsOpened() == 0 && rig.camera->sessionsOpen() == 0,
            "a blocked session never opened a platform capture session");
}

/* The application says the camera is off when the guest opens one while the
 * session is blocked: once for each open, from the first video stream or still
 * to the last camera the guest closes, and never from a metadata stream alone. */
void testBlockedSaysWhenTheGuestOpensACamera()
{
    RigOptions options;
    options.mode = CameraMode::blocked;
    Rig rig(options);
    using States = std::vector<CameraState>;
    // Listing the cameras and holding a metadata stream open is not using one.
    rig.create(2, 0, metaKind);
    for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.attach(2, resource, metaSize);
    (void)rig.queue(2, 1);
    require(rig.states.empty(), "a metadata stream alone says nothing");

    // The first video buffer is the open.
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    for (std::uint32_t resource = 1; resource <= 4; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    (void)rig.queue(1, 1);
    require(rig.states == States {CameraState::blockedInUse}, "the first video buffer reports it");
    (void)rig.queue(1, 2);
    rig.step();
    require(rig.states == States {CameraState::blockedInUse}, "once for the open, however many buffers follow");

    // A second camera in use is the same open; the guest closing one of them is not the end.
    rig.create(3, 1, videoKind);
    rig.setVideo(3, 640, 480);
    rig.attach(3, 1, nv12Size(640, 480));
    (void)rig.queue(3, 1);
    require(rig.states == States {CameraState::blockedInUse}, "a second camera is not a second report");
    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    require(rig.states == States {CameraState::blockedInUse}, "one camera still open");
    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 3}, output, {}});
    require(rig.states == States {CameraState::blockedInUse, CameraState::idle}, "the last one closed ends the open");

    // The guest opens again: a new open.
    (void)rig.queue(1, 3);
    require(rig.states == States {CameraState::blockedInUse, CameraState::idle, CameraState::blockedInUse},
            "a new open is reported again");
    rig.destroy(1);
    require(rig.states.back() == CameraState::idle, "destroying the stream ends it");

    // A still alone is use too, and it ends when the stream goes.
    rig.create(4, 0, stillKind);
    require(Rig::type(rig.setParams(4, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a still size");
    rig.attach(4, 1, std::uint32_t(rig.params(4).plane_formats[0].plane_size));
    const auto before = rig.states.size();
    const auto still = rig.queue(4, 1, 99);
    require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "the still is refused at once");
    require(rig.states.size() == before + 1 && rig.states.back() == CameraState::blockedInUse, "and it is the guest using a camera");
    rig.destroy(4);
    require(rig.states.back() == CameraState::idle, "the still stream going ends it");

    // An open when the guest's transport resets is over, and the application is told.
    rig.create(5, 0, videoKind);
    rig.setVideo(5, 640, 480);
    rig.attach(5, 1, nv12Size(640, 480));
    (void)rig.queue(5, 1);
    require(rig.states.back() == CameraState::blockedInUse, "open again");
    const auto beforeReset = rig.states.size();
    rig.device->reset();
    require(rig.states.size() == beforeReset + 1 && rig.states.back() == CameraState::idle, "a reset closes it, and says so once");
    rig.device->reset();
    require(rig.states.size() == beforeReset + 1, "a reset with nothing open says nothing");
    // The driver recreates its stream after the reset and the guest opens a camera again: a new open.
    rig.create(5, 0, videoKind);
    rig.setVideo(5, 640, 480);
    rig.attach(5, 1, nv12Size(640, 480));
    (void)rig.queue(5, 1);
    require(rig.states.size() == beforeReset + 2 && rig.states.back() == CameraState::blockedInUse,
            "an open after a reset is reported again, exactly once");
    require(rig.camera->sessionsOpened() == 0, "and none of it opened a capture session, so the system's indicator stayed off");
}

/* A live session never reports the blocked state, whatever the guest does. */
void testLiveNeverReportsBlockedUse()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    rig.create(2, 0, stillKind);
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a still size");
    rig.attach(2, 1, std::uint32_t(rig.params(2).plane_formats[0].plane_size));
    (void)rig.queue(1, 1);
    (void)rig.queue(2, 1, 5);
    rig.step();
    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    rig.destroy(2);
    rig.destroy(1);
    require(std::find(rig.states.begin(), rig.states.end(), CameraState::blockedInUse) == rig.states.end(),
            "a live session reports streaming and idle, never the blocked state");
    require(!rig.states.empty() && rig.states.front() == CameraState::streaming, "it did report the capture");
}

void testInterruption()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    for (std::uint32_t resource = 1; resource <= 6; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    // The guest keeps event buffers posted.
    const auto eventBuffers = rig.postEventBuffers(8);
    const auto eventOf = [&](std::size_t index) {
        require(rig.done(eventBuffers[index]), "an event did not arrive");
        return load<virtio_video_event>(rig.completed.at(eventBuffers[index]));
    };
    const auto a = rig.queue(1, 1), b = rig.queue(1, 2);
    rig.pumpSteps(0, 2);
    require(rig.result(a).base.flags == 0 && rig.result(b).base.flags == 0, "frames flow");

    rig.camera->interrupt();
    rig.step();
    // The platform interrupts every camera, and each is an event, named by its camera index.
    require(eventOf(0).event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED && eventOf(0).stream_id == 0 &&
                eventOf(1).event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED && eventOf(1).stream_id == 1,
            "the interruption is an event for each camera");
    require(rig.states.back() == CameraState::interrupted, "the application is told");
    const auto c = rig.queue(1, 3), d = rig.queue(1, 4);
    require(rig.waitFor([&] { return rig.done(c) && rig.done(d); }), "black frames bridge the interruption");
    for (const auto id : {c, d}) {
        require(rig.result(id).base.flags == (blackFlag | interruptedFlag), "black and interrupted");
        const auto picture = rig.bytes(1, id == c ? 3u : 4u, 640 * 480);
        require(std::all_of(picture.begin(), picture.end(), [](auto byte) { return byte == 0; }), "black");
    }
    rig.pumpSteps(0, 2);

    rig.camera->resume();
    rig.step();
    require(eventOf(2).event_type == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED && eventOf(2).stream_id == 0 &&
                eventOf(3).event_type == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED && eventOf(3).stream_id == 1,
            "and so is the resumption");
    require(rig.states.back() == CameraState::streaming, "streaming again");
    const auto e = rig.queue(1, 5);
    require(rig.waitFor([&] { return rig.done(e); }), "black frames flag the restart");
    require(rig.result(e).base.flags == (blackFlag | resumingFlag), "black and resuming");
    // The first real frame ends it.
    const auto f = rig.queue(1, 6);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(f) && rig.result(f).base.flags == 0, "a captured frame ends the restart");
    require(rig.result(f).sequence > rig.result(e).sequence, "numbers keep counting");

    // Multitasking has its own state.
    rig.camera->interrupt(CameraEventKind::interruptedByMultitasking);
    rig.step();
    require(rig.states.back() == CameraState::interruptedByMultitasking, "the multitasking interruption");
    require(eventOf(4).event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, "is an interruption to the guest");
    rig.camera->resume();
    rig.step();
    require(rig.states.back() == CameraState::streaming, "resumed");
}

void testAccessDenied()
{
    RigOptions options;
    options.authorized = false;
    Rig rig(options);
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.create(2, 0, stillKind);
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a still size");
    rig.attach(1, 1, nv12Size(640, 480));
    rig.attach(2, 1, std::uint32_t(rig.params(2).plane_formats[0].plane_size));
    const auto frame = rig.queue(1, 1);
    require(rig.waitFor([&] { return rig.done(frame); }), "a guest of a host that has not granted access still gets frames");
    require(rig.result(frame).base.flags == (blackFlag | interruptedFlag), "black and interrupted");
    require(rig.states.back() == CameraState::interrupted, "and the application is told");
    const auto still = rig.queue(2, 1, 8);
    require(rig.done(still) && (rig.result(still).base.flags & errorFlag) && (rig.result(still).base.flags & interruptedFlag),
            "a still is refused");
    require(rig.camera->sessionsOpen() == 0, "nothing is left open");
}

/* Reads the little-endian 16-bit samples of a RAW row. */
std::vector<std::uint16_t> rawRow(Rig& rig, std::uint32_t stream, std::uint32_t resource, std::uint32_t width, std::uint32_t row)
{
    const auto stride = rawPitch(2 * width);
    const auto data = rig.bytes(stream, resource, std::size_t(stride) * (row + 1));
    std::vector<std::uint16_t> samples(width);
    std::memcpy(samples.data(), data.data() + std::size_t(stride) * row, std::size_t(width) * 2);
    return samples;
}

void testStillsAndCalibration()
{
    auto store = std::make_shared<CalibrationStore>();
    std::string model;
    CameraInfo info;
    {
        RigOptions options;
        options.store = store;
        Rig rig(options);
        model = rig.camera->hostModel();
        info = rig.camera->cameras()[0];
        require(!rig.device->cameras()[0].raw(), "no RAW until the lens is calibrated");
        rig.create(1, 0, stillKind);
        require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        const auto bufferSize = rig.params(1).plane_formats[0].plane_size;
        require(bufferSize == 1008u * 756u * 3 / 2 + exifGapLength, "the buffer holds a YUV-sized JPEG and the gap");
        rig.create(2, 0, metaKind);
        rig.attach(1, 1, bufferSize);
        rig.attach(2, 1, metaSize);
        const auto meta = rig.queue(2, 1);
        const auto still = rig.queue(1, 1, 555);
        require(!rig.done(still) && rig.camera->sessionsOpened() == 1, "the capture is asked for when the buffer is queued");
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(still), "the still completed");
        const auto answer = rig.result(still);
        require(answer.base.flags == 0 && answer.base.timestamp == 555 && answer.sequence == 1 && answer.base.data_sizes[0] > 4000 &&
                    answer.base.data_sizes[0] < bufferSize,
                "a still completes with the guest's cookie and its size");
        const auto file = rig.bytes(1, 1, answer.base.data_sizes[0]);
        require(file[0] == 0xff && file[1] == 0xd8 && file[2] == 0xff && file[3] == 0xe1 && file[4] == 0xfd && file[5] == 0xea,
                "a JPEG that starts with the reserved Exif segment (length 65002)");
        require(std::memcmp(file.data() + 6, "Exif\0\0", 6) == 0, "which holds an Exif header");
        require(file[file.size() - 2] == 0xff && file[file.size() - 1] == 0xd9, "and ends with the end of image");
        require(rig.done(meta), "the still's metadata");
        const auto record = metaOf(rig, 2, 1);
        require(rig.result(meta).sequence == 1 && record.sequence == 1 && record.flags == VIRTIO_VIDEO_CAMERA_FRAME_F_STILL &&
                    record.jpeg_gap_offset == 6 && record.jpeg_gap_length == exifGapLength && record.active_camera == 0,
                "describes the still and where its gap is");
        require(rig.camera->sessionsOpen() == 0 && rig.states.back() == CameraState::idle, "the capture session is released");
        require(rig.checkpoints == std::vector<CameraCheckpoint> {CameraCheckpoint::sessionOpened, CameraCheckpoint::stillDelivered,
                                                                  CameraCheckpoint::sessionClosed},
                "a still is bracketed by its session's checkpoints, read while the session is open");

        // The first still of a lens taught the device its colour data.
        const auto learned = store->load(model, CalibrationStore::lensKey(info));
        require(learned && learned->bayer == BayerOrder::rggb && learned->rawWidth == 1008 && learned->rawHeight == 756 &&
                    learned->whiteLevel > learned->blackLevel,
                "the lens is calibrated");
        require(learned->anchorWidth == 1920 && learned->anchorHeight == 1440, "in the format the camera runs in, which is stored with it");
        require(!rig.device->cameras()[0].raw(), "but this guest keeps seeing what it saw at the start");
    }

    // A later guest sees RAW, and takes it with a JPEG from one capture.
    RigOptions options;
    options.store = store;
    Rig rig(options);
    const auto camera = rig.device->cameras()[0];
    require(camera.raw() && (camera.wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) && camera.wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_RGGB &&
                camera.wire.raw_width == 1008,
            "RAW is offered once the lens is known");
    const auto formats = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, 0, stillKind});
    require(load<virtio_video_query_capability_resp>(formats.response).num_descs == 2, "JPEG and RAW");
    rig.create(1, 0, stillKind);
    rig.create(2, 0, stillKind);
    rig.create(3, 0, metaKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "JPEG");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "RAW");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG, 1008, 756)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "another pattern is refused");
    const auto raw = rig.params(2);
    require(raw.plane_formats[0].stride == rawPitch(2 * 1008) && raw.plane_formats[0].plane_size == rawPitch(2 * 1008) * 756,
            "RAW rows are padded to 16 bytes");
    rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
    rig.attach(2, 1, raw.plane_formats[0].plane_size);
    rig.attach(2, 2, raw.plane_formats[0].plane_size);
    rig.attach(1, 2, rig.params(1).plane_formats[0].plane_size);
    rig.attach(3, 1, metaSize);
    const auto meta = rig.queue(3, 1);

    const auto rawBuffer = rig.queue(2, 1, 77, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
    rig.camera->pump(0, 0);
    rig.step();
    require(!rig.done(rawBuffer), "a group waits for its last buffer");
    const auto jpegBuffer = rig.queue(1, 1, 77);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(rawBuffer) && rig.done(jpegBuffer), "the last buffer starts one capture that fills both");
    const auto rawAnswer = rig.result(rawBuffer), jpegAnswer = rig.result(jpegBuffer);
    require(rawAnswer.base.flags == 0 && jpegAnswer.base.flags == 0 && rawAnswer.base.timestamp == 77 &&
                jpegAnswer.base.timestamp == 77 && rawAnswer.sequence == jpegAnswer.sequence,
            "with the cookie and one exposure");
    require(rawAnswer.base.data_sizes[0] == rawPitch(2 * 1008) * 756, "a whole RAW picture");
    std::uint64_t sum = 0;
    std::uint16_t largest = 0;
    for (const std::uint32_t row : {0u, 1u, 377u, 755u})
        for (const auto sample : rawRow(rig, 2, 1, 1008, row)) {
            sum += sample;
            largest = std::max(largest, sample);
        }
    require(sum > 0 && largest <= camera.calibration->whiteLevel, "RAW samples in range");
    const auto record = metaOf(rig, 3, 1);
    require(rig.done(meta) && record.sequence == rawAnswer.sequence && record.flags == VIRTIO_VIDEO_CAMERA_FRAME_F_STILL &&
                record.jpeg_gap_offset == 6 && record.neutral_point_q16[1] != 0 && record.noise_profile_f32[0] != 0,
            "one record for the capture, with the white point and noise model of a RAW");

    // RAW alone, without a cookie.
    const auto alone = rig.queue(2, 2, 0);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(alone) && rig.result(alone).base.flags == 0, "RAW alone");

    // A group left unfinished is failed after two seconds.
    const auto stuck = rig.queue(2, 1, 99, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
    rig.step();
    require(!rig.done(stuck), "waiting");
    rig.camera->advanceClock(1'500'000'000);
    rig.step();
    require(!rig.done(stuck), "not yet");
    rig.camera->advanceClock(600'000'000);
    rig.step();
    require(rig.done(stuck) && (rig.result(stuck).base.flags & errorFlag) && rig.result(stuck).base.timestamp == 99,
            "failed with its cookie");

    // A capture the platform cannot finish fails its buffers.
    const auto doomed = rig.queue(1, 2, 31);
    rig.camera->interrupt();
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(doomed) && (rig.result(doomed).base.flags & errorFlag), "an interrupted capture fails");
    // The cause outlives the session that was open: a session opened for the next
    // still is interrupted as it starts, and the still fails with the reason.
    const auto refused = rig.queue(1, 2, 32);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(refused) && rig.result(refused).base.flags == (errorFlag | interruptedFlag), "and a new one fails while it lasts");
    rig.camera->resume();
    rig.step();
    require(rig.camera->sessionsOpen() == 0, "no session is held for stills");
}

void testControlsOnStreams()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.queue(1, 1);

    rig.setOk(1, {{UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 5'000'000, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_ISO, 800, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 2 * 65536, {}},
                  {UML_V4L2_CID_FOCUS_AUTO, 0, {}},
                  {UML_V4L2_CID_FOCUS_ABSOLUTE, 2000, {}}});
    auto applied = rig.camera->appliedSettings(0);
    require(applied.exposureMode == CameraExposureMode::manual && applied.exposureNanoseconds == 5'000'000 && applied.iso == 800 &&
                applied.zoomRatio == 2.0f && applied.focusMode == CameraFocusMode::manual && applied.focusDistanceDioptres == 2.0f,
            "the platform camera is told, as soon as the set completes");
    require(rig.get(1, {UML_V4L2_CID_EXPOSURE_ABSOLUTE, UML_V4L2_CID_ISO_SENSITIVITY, UML_V4L2_CID_ZOOM_ABSOLUTE}) ==
                std::vector<std::int64_t> {50, 800, 200},
            "and the aliases read back");

    // A set applies every value or none.
    require(rig.set(1, {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 3 * 65536, {}}, {VIRTIO_VIDEO_CAMERA_CID_ISO, 5, {}}}) ==
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a value out of range fails the set");
    require(rig.get(1, {UML_V4L2_CID_ZOOM_ABSOLUTE})[0] == 200 && rig.camera->appliedSettings(0).zoomRatio == 2.0f,
            "and the valid value before it was not applied");
    const auto bad = rig.controls(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1,
                                  {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 3 * 65536, {}}, {VIRTIO_VIDEO_CAMERA_CID_ISO, 5, {}}});
    const auto failure = load<virtio_video_v4l2_controls_resp>(bad.response);
    require(failure.error_index == 1 && failure.num_controls == 2, "the failing index is reported");
    require(rig.set(1, {{UML_V4L2_CID_PRIVACY, 0, {}}}) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a read-only control");
    require(rig.set(1, {{UML_V4L2_CID_MPEG_VIDEO_BITRATE, 1, {}}}) == VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, "an encoder control");
    require(rig.set(1, {{UML_V4L2_CID_FOCUS_ABSOLUTE, 99999, {}}}) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "beyond the nearest focus");

    // Compound controls keep the guest's bytes; the metering region reaches the platform.
    std::vector<std::uint8_t> region(regionPayloadSize);
    const std::uint32_t words[5] = {16384, 16384, 32768, 32768, 7};
    std::memcpy(region.data(), words, sizeof(words));
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, region}});
    applied = rig.camera->appliedSettings(0);
    require(applied.aeRegion && applied.aeRegion->weight == 7 && applied.aeRegion->width == 0.5f, "the region");
    const auto read = rig.controls(VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, 1, {{VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, {}}});
    const auto value = load<virtio_video_v4l2_control_value>(read.response, sizeof(virtio_video_v4l2_controls_resp));
    require(value.size == regionPayloadSize && std::vector<std::uint8_t>(read.response.begin() +
                                                                          std::ptrdiff_t(sizeof(virtio_video_v4l2_controls_resp) +
                                                                                         sizeof(value) + value.value),
                                                                      read.response.begin() +
                                                                          std::ptrdiff_t(sizeof(virtio_video_v4l2_controls_resp) +
                                                                                         sizeof(value) + value.value +
                                                                                         regionPayloadSize)) == region,
            "and reads back");

    // A button counts presses; the focus status follows the exposure.
    rig.setOk(1, {{UML_V4L2_CID_FOCUS_AUTO, 0, {}}});
    for (int press = 0; press < 3; ++press) rig.setOk(1, {{UML_V4L2_CID_AUTO_FOCUS_START, 1, {}}});
    require(rig.camera->appliedSettings(0).focusTriggerSerial == 3, "three presses of the focus button");
    rig.setOk(1, {{UML_V4L2_CID_FOCUS_AUTO, 1, {}}});
    rig.camera->pump(0, 1);
    rig.step();
    const auto status = rig.get(1, {UML_V4L2_CID_AUTO_FOCUS_STATUS})[0];
    require(status == UML_V4L2_AUTO_FOCUS_STATUS_REACHED || status == UML_V4L2_AUTO_FOCUS_STATUS_BUSY ||
                status == UML_V4L2_AUTO_FOCUS_STATUS_IDLE,
            "the focus status is the camera's");

    // Controls belong to the camera: a metadata stream of it reads them.
    rig.create(2, 0, metaKind);
    require(rig.get(2, {UML_V4L2_CID_ISO_SENSITIVITY})[0] == 800, "any stream of the camera reaches its controls");
    rig.create(3, 1, videoKind);
    require(rig.get(3, {UML_V4L2_CID_ISO_SENSITIVITY})[0] != 800, "another camera has its own");

    // The test pattern makes the frames black, and ends.
    rig.setOk(1, {{UML_V4L2_CID_TEST_PATTERN, 1, {}}});
    const auto patterned = rig.queue(1, 1);
    require(rig.waitFor([&] { return rig.done(patterned); }), "the pattern is delivered on the timer");
    require(rig.result(patterned).base.flags == blackFlag, "a plain black frame");
    rig.setOk(1, {{UML_V4L2_CID_TEST_PATTERN, 0, {}}});
    const auto after = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(after) && rig.result(after).base.flags == 0, "and real frames return");
}

void testTorchHoldsTheSession()
{
    Rig rig;
    rig.create(1, 0, metaKind);
    require(rig.camera->sessionsOpened() == 0, "a stream with no buffer opens nothing");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    require(rig.camera->sessionsOpen() == 1 && rig.camera->appliedSettings(0).torch, "the torch needs a session");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_NONE, {}}});
    require(rig.camera->sessionsOpen() == 0, "which is released when the torch goes out");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    rig.device->reset();
    require(rig.camera->sessionsOpen() == 0, "and a reset releases it");
}

void testTheLightTheGuestAsksForIsTold()
{
    using uml::platform::CameraFlashMode;
    Rig rig;
    rig.create(1, 0, metaKind);
    require(rig.lights.empty(), "a camera nobody asked anything of says nothing");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    require(rig.lights.size() == 1 && rig.lights[0].torch && rig.lights[0].flash == CameraFlashMode::off, "the torch");
    // A still's flash is a control of its own, and the torch stays as it was.
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, 2, {}}});
    require(rig.lights.size() == 2 && rig.lights[1].torch && rig.lights[1].flash == CameraFlashMode::on, "the flash");
    // The same ask again changes nothing, so it is not told again.
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, 2, {}}});
    require(rig.lights.size() == 2, "an ask that changes nothing is not told");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_NONE, {}},
                  {VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, 1, {}}});
    require(rig.lights.size() == 3 && !rig.lights[2].torch && rig.lights[2].flash == CameraFlashMode::automatic,
        "both change together, and the device tells the pair once");
    require(rig.lights[0].camera == rig.lights[2].camera, "of the same camera");
}

void testResetAndDestroy()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.setOk(1, {{UML_V4L2_CID_ISO_SENSITIVITY, 400, {}}});
    const auto frame = rig.queue(1, 1);
    require(rig.camera->videoRunning(0) && rig.states.back() == CameraState::streaming, "running");
    rig.device->reset();
    require(rig.camera->sessionsOpen() == 0 && !rig.camera->videoRunning(0), "a reset stops the capture");
    require(rig.states.back() == CameraState::idle, "and tells the application");
    rig.collect();
    require(!rig.done(frame), "a buffer of the old guest is not completed to the new one");
    rig.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, output, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
                "streams are gone");
    rig.create(1, 0, videoKind);
    require(rig.get(1, {UML_V4L2_CID_ISO_SENSITIVITY})[0] != 400, "and so are the controls' values");
    rig.device->reset();
    rig.device->reset();
}

/* A camera is in video mode while its video runs, uninterrupted, and the guest asks for the picture stabilized (Android's
 * record template does and its preview template does not): the application opens the microphone then, before the guest
 * records. */
void testVideoModeIsWhatTheGuestAsksStabilizationFor()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.attach(1, 2, nv12Size(640, 480));
    // A camera nothing is open on is not in video mode, and a photo mode (a preview that asks for no stabilization) is not.
    require(rig.videoModes.empty(), "nothing is said before a camera is open");
    rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 0, {}}});
    rig.queue(1, 1);
    require(rig.camera->videoRunning(0) && rig.videoModes.empty(), "a preview that does not ask is not video mode");
    // The record template asks.
    rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 1, {}}});
    require(rig.videoModes == std::vector<bool> {true}, "asking while the video runs is video mode");
    rig.setOk(1, {{UML_V4L2_CID_ISO_SENSITIVITY, 400, {}}});
    require(rig.videoModes.size() == 1, "another setting says nothing");
    // The camera is interrupted (the application left the foreground): the microphone is not held for it.
    rig.camera->interrupt(CameraEventKind::interruptedByMultitasking);
    rig.step();
    require(rig.videoModes == std::vector<bool> {true, false}, "an interruption ends video mode");
    rig.camera->resume();
    rig.step();
    require(rig.videoModes == std::vector<bool> {true, false, true}, "and its end returns to it");
    // Back to photo mode.
    rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 0, {}}});
    require(rig.videoModes == std::vector<bool> {true, false, true, false}, "a guest that stops asking leaves video mode");
    rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 1, {}}});
    require(rig.videoModes.back(), "and enters it");
    // The guest closes the camera, or the device is reset.
    rig.device->reset();
    require(!rig.videoModes.back() && rig.videoModes.size() == 6, "a reset ends video mode");
    rig.device->reset();
    require(rig.videoModes.size() == 6, "and says it once");
}

void testVideoModeIsNeverToldForABlockedSessionOrAfterTheStreamStops()
{
    RigOptions blockedOptions;
    blockedOptions.mode = CameraMode::blocked;
    Rig blockedRig(blockedOptions);
    blockedRig.create(1, 0, videoKind);
    blockedRig.setVideo(1, 640, 480);
    blockedRig.attach(1, 1, nv12Size(640, 480));
    blockedRig.queue(1, 1);
    require(blockedRig.videoModes.empty(), "a blocked session opens no capture: the guest is not in video mode");

    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.queue(1, 1);
    require(rig.videoModes == std::vector<bool> {true}, "the default asks for stabilization");
    rig.destroy(1);
    require(rig.videoModes == std::vector<bool> {true, false}, "the stream's end ends video mode");
}

/* The event buffers a guest keeps posted, and the events that have reached it. */
struct EventQueue {
    Rig& rig;
    std::vector<std::uint64_t> ids;
    EventQueue(Rig& owner, int count) : rig(owner), ids(owner.postEventBuffers(count)) {}
    /* (event type, camera) in the order they arrived. */
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> arrived() const
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> events;
        for (const auto id : ids) {
            if (!rig.done(id)) break;
            const auto event = load<virtio_video_event>(rig.completed.at(id));
            events.emplace_back(event.event_type, event.stream_id);
        }
        return events;
    }
};

using EventList = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

/* A video stream of a camera at a size, with `buffers` resources attached. */
void videoStream(Rig& rig, std::uint32_t stream, std::uint32_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t buffers)
{
    rig.create(stream, camera, videoKind);
    rig.setVideo(stream, width, height);
    for (std::uint32_t resource = 1; resource <= buffers; ++resource) rig.attach(stream, resource, nv12Size(width, height));
}

void clearStream(Rig& rig, std::uint32_t stream)
{
    rig.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, stream}, output, {}});
    rig.collect();
}

constexpr std::uint32_t endedFlags = errorFlag | interruptedFlag;
constexpr std::uint32_t blackRecord = VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK | VIRTIO_VIDEO_CAMERA_FRAME_F_INTERRUPTED;

/* The application leaves the foreground while the guest has a video stream open on a camera (a recording, which asks for
 * the picture stabilized, or a viewfinder; the host cannot tell them apart): the video of that camera ends. Its buffers
 * fail with the interrupted flag, the camera's records say black and interrupted, the platform's capture stops, and none
 * of it is undone by the platform's recovery, by a guest suspend that stops the stream and starts it again, or by the
 * guest's next buffer: only the guest's close of its last video stream of the camera ends it. */
void testTheApplicationLeavingTheForegroundEndsTheVideoOfACamera()
{
    Rig rig;
    videoStream(rig, 1, 0, 640, 480, 8);
    rig.create(2, 0, metaKind);
    for (std::uint32_t resource = 1; resource <= 8; ++resource) rig.attach(2, resource, metaSize);
    rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 1, {}}});
    for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.queue(2, resource);
    const auto a = rig.queue(1, 1), b = rig.queue(1, 2);
    rig.pumpSteps(0, 2);
    require(rig.result(a).base.flags == 0 && rig.result(b).base.flags == 0 && rig.videoModes.back(), "live video in video mode");

    rig.camera->interrupt(CameraEventKind::interruptedInBackground);
    rig.step();
    require(rig.states.back() == CameraState::interrupted && !rig.videoModes.back(), "the application is told, and it is not video mode");
    require(!rig.camera->videoRunning(0) && rig.camera->sessionsOpen() == 0, "the platform's capture is stopped and its session goes");
    // The black timer answers the guest's next buffers, one at the stream's rate, and says why. The guest's camera layer
    // keeps meta buffers queued, and the camera's interrupted record is the first thing it is given: the video buffers fail
    // once it has queued the buffer that carried the record again.
    const auto first = rig.queue(2, 3), second = rig.queue(2, 4);
    const auto c = rig.queue(1, 3), d = rig.queue(1, 4);
    require(rig.waitFor([&] { return rig.done(first); }), "the record of the lost video is served first");
    require(metaOf(rig, 2, 3).flags == blackRecord, "the camera's record is black and interrupted");
    require(!rig.done(c) && !rig.done(d), "and the video's buffers wait for the guest to have read it");
    rig.queue(2, 3);
    require(rig.waitFor([&] { return rig.done(c) && rig.done(d); }), "the buffers of the lost video are answered");
    for (const auto id : {c, d}) {
        const auto answer = rig.result(id);
        require(answer.base.flags == endedFlags && answer.base.data_sizes[0] == 0, "a lost video's buffer fails, interrupted");
    }
    require(rig.waitFor([&] { return rig.done(second); }), "the records of the lost video go on");
    require(metaOf(rig, 2, 4).flags == blackRecord, "black and interrupted");

    // The platform recovers: the video does not come back to the stream that was lost.
    rig.camera->resume();
    rig.step();
    require(rig.states.back() == CameraState::interrupted, "the camera stays down for the guest");
    const auto e = rig.queue(1, 5);
    require(rig.waitFor([&] { return rig.done(e); }), "answered");
    require(rig.result(e).base.flags == endedFlags && !rig.camera->videoRunning(0), "after the recovery too, and no capture is started for it");
    // A guest suspend stops the stream and starts it again.
    clearStream(rig, 1);
    const auto f = rig.queue(1, 6);
    require(rig.waitFor([&] { return rig.done(f); }), "answered after a suspend");
    require(rig.result(f).base.flags == endedFlags && !rig.camera->videoRunning(0), "a restarted stream is still lost");

    // The guest closes its video: the camera is its again, and a stream it opens next is an ordinary one.
    rig.destroy(1);
    require(rig.states.back() == CameraState::idle, "closed, the host is idle");
    videoStream(rig, 3, 0, 640, 480, 3);
    const auto g = rig.queue(3, 1);
    require(rig.camera->videoRunning(0), "the camera opens again on demand");
    require(rig.waitFor([&] { return rig.done(g); }), "a first picture");
    require(rig.result(g).base.flags == (blackFlag | resumingFlag), "the new stream starts as any does");
    const auto h = rig.queue(3, 2);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(h) && rig.result(h).base.flags == 0 && rig.states.back() == CameraState::streaming, "and then it is live");

    // A reset forgets the loss as it forgets the streams.
    Rig reset;
    videoStream(reset, 1, 0, 640, 480, 2);
    reset.queue(1, 1);
    reset.camera->interrupt(CameraEventKind::interruptedInBackground);
    reset.step();
    reset.resetByTransport();
    // The application is back in the foreground: a session opened now is not interrupted.
    reset.camera->resume();
    reset.step();
    videoStream(reset, 1, 0, 640, 480, 2);
    const auto k = reset.queue(1, 1);
    require(reset.camera->videoRunning(0), "after a reset the camera runs again");
    require(reset.waitFor([&] { return reset.done(k); }) && reset.result(k).base.flags == (blackFlag | resumingFlag),
            "and its stream is an ordinary one");
}

/* The loss does not depend on what the guest asks for the picture: a recording that asks for no stabilization, and a
 * viewfinder, are ended as a recording that asks for it is. The guest's camera HAL decides what its client sees; one that
 * keeps its client closes its video streams and opens them again, which ends the loss on the host and gives it an ordinary
 * stream, dark while the application is away and live when it is back. */
void testTheLossDoesNotDependOnTheStabilization()
{
    for (const std::uint32_t stabilization : {0u, 1u}) {
        Rig rig;
        videoStream(rig, 1, 0, 640, 480, 6);
        rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, stabilization, {}}});
        rig.queue(1, 1);
        rig.pumpSteps(0, 1);
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        const auto a = rig.queue(1, 2);
        require(rig.waitFor([&] { return rig.done(a); }), "answered");
        require(rig.result(a).base.flags == endedFlags && !rig.camera->videoRunning(0), "the video ended, whatever it asked for");
        rig.camera->resume();
        rig.step();
        const auto b = rig.queue(1, 3);
        require(rig.waitFor([&] { return rig.done(b); }) && rig.result(b).base.flags == endedFlags,
                "and the application's return does not undo it");
        // The guest's camera HAL closes its video streams and opens them again.
        rig.destroy(1);
        require(rig.states.back() == CameraState::idle, "closed, the host is idle");
        videoStream(rig, 2, 0, 640, 480, 4);
        const auto c = rig.queue(2, 1);
        require(rig.waitFor([&] { return rig.done(c); }) && rig.result(c).base.flags == (blackFlag | resumingFlag),
                "the new stream starts as any does");
        const auto d = rig.queue(2, 2);
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.done(d) && rig.result(d).base.flags == 0 && rig.states.back() == CameraState::streaming, "and is live");
    }
    {
        // The application leaves again while the guest's camera HAL has just opened its streams again: the new stream is lost
        // as well, and a second reopening gives an ordinary stream once the application is back.
        Rig rig;
        videoStream(rig, 1, 0, 640, 480, 4);
        rig.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 0, {}}});
        rig.queue(1, 1);
        rig.pumpSteps(0, 1);
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        const auto a = rig.queue(1, 2);
        require(rig.waitFor([&] { return rig.done(a); }) && rig.result(a).base.flags == endedFlags, "ended");
        rig.camera->resume();
        rig.step();
        rig.destroy(1);
        videoStream(rig, 2, 0, 640, 480, 4);
        const auto b = rig.queue(2, 1);
        require(rig.waitFor([&] { return rig.done(b); }) && rig.result(b).base.flags == (blackFlag | resumingFlag),
                "the new stream starts as any does");
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        const auto c = rig.queue(2, 2);
        require(rig.waitFor([&] { return rig.done(c); }) && rig.result(c).base.flags == endedFlags, "and is lost by the second departure");
        rig.camera->resume();
        rig.step();
        rig.destroy(2);
        videoStream(rig, 3, 0, 640, 480, 4);
        const auto d = rig.queue(3, 1);
        require(rig.waitFor([&] { return rig.done(d); }) && rig.result(d).base.flags == (blackFlag | resumingFlag),
                "the next stream starts as any does");
        const auto e = rig.queue(3, 2);
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.waitFor([&] { return rig.done(e); }) && rig.result(e).base.flags == 0 && rig.camera->videoRunning(0),
                "and is live");
    }
}

/* Only the application's leaving the foreground ends a video, and only that of a camera that has a video stream: another
 * application or a call, the multitasking of an iPad and the system's pressure are dark gaps that end by themselves, a
 * camera with no video stream is not lost, and the camera beside one that is keeps its dark gap. */
void testOnlyTheBackgroundEndsAVideoAndOnlyOfACameraWithAVideoStream()
{
    for (const auto kind : {CameraEventKind::interrupted, CameraEventKind::interruptedByMultitasking,
                            CameraEventKind::interruptedBySystemPressure}) {
        Rig rig;
        videoStream(rig, 1, 0, 640, 480, 6);
        rig.queue(1, 1);
        rig.pumpSteps(0, 1);
        rig.camera->interrupt(kind);
        rig.step();
        const auto a = rig.queue(1, 2);
        require(rig.waitFor([&] { return rig.done(a); }), "answered");
        require(rig.result(a).base.flags == (blackFlag | interruptedFlag), "a dark gap, not an end");
        rig.camera->resume();
        rig.step();
        const auto b = rig.queue(1, 3);
        require(rig.waitFor([&] { return rig.done(b); }) && rig.result(b).base.flags == (blackFlag | resumingFlag), "a restart");
        const auto c = rig.queue(1, 4);
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.done(c) && rig.result(c).base.flags == 0 && rig.camera->videoRunning(0), "and live again");
    }
    {
        // Two cameras with a video stream each are both lost, and each ends its loss by closing its own streams.
        RigOptions options;
        options.profile = SyntheticCameraProfile::tabletDualFront;
        Rig rig(options);
        videoStream(rig, 1, 0, 1600, 900, 4);
        videoStream(rig, 2, 1, 640, 480, 4);
        rig.queue(1, 1);
        rig.queue(2, 1);
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        const auto back = rig.queue(1, 2), front = rig.queue(2, 2);
        require(rig.waitFor([&] { return rig.done(back) && rig.done(front); }), "answered");
        require(rig.result(back).base.flags == endedFlags && rig.result(front).base.flags == endedFlags,
                "every camera with a video stream is lost");
        rig.camera->resume();
        rig.step();
        rig.destroy(1);
        videoStream(rig, 3, 0, 1600, 900, 4);
        const auto back2 = rig.queue(3, 1), front2 = rig.queue(2, 3);
        require(rig.waitFor([&] { return rig.done(back2) && rig.done(front2); }), "answered again");
        require(rig.result(back2).base.flags != endedFlags, "the camera whose streams were closed is an ordinary one again");
        require(rig.result(front2).base.flags == endedFlags, "and the other stays lost until its own streams are closed");
    }
    {
        // A blocked session opens no capture, hears of no interruption and ends nothing.
        RigOptions options;
        options.mode = CameraMode::blocked;
        Rig rig(options);
        videoStream(rig, 1, 0, 640, 480, 3);
        rig.queue(1, 1);
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        const auto a = rig.queue(1, 2);
        require(rig.waitFor([&] { return rig.done(a); }), "answered");
        require(rig.result(a).base.flags == (blackFlag | blockedFlag), "a blocked session is black and blocked, never lost");
    }
}

/* The application's thread that asks for the guest's video to end as it leaves the foreground, and waits for the guest to
 * stop it (`BackgroundVideo::end`), on a thread of its own: the test thread is the device's worker. */
class Departure final {
public:
    Departure(BackgroundVideo& control, std::chrono::milliseconds bound) : began_(BackgroundVideo::Clock::now())
    {
        thread_ = std::thread([this, &control, deadline = began_ + bound] {
            outcome_ = control.end(deadline);
            finished_ = BackgroundVideo::Clock::now();
            returned_.store(true);
        });
    }
    ~Departure()
    {
        if (thread_.joinable()) thread_.join();
    }
    bool returned() const { return returned_.load(); }
    BackgroundVideo::Outcome join()
    {
        thread_.join();
        return outcome_;
    }
    std::chrono::milliseconds took() const { return std::chrono::duration_cast<std::chrono::milliseconds>(finished_ - began_); }

private:
    BackgroundVideo::Clock::time_point began_, finished_;
    std::atomic<bool> returned_ {};
    BackgroundVideo::Outcome outcome_;
    std::thread thread_;
};

/* A video stream with a meta stream beside it and a few buffers running, as a camera application's recording has. */
void runningVideo(Rig& rig, std::uint32_t video = 1, std::uint32_t camera = 0)
{
    videoStream(rig, video, camera, 640, 480, 8);
    rig.create(video + 1, camera, metaKind);
    for (std::uint32_t resource = 1; resource <= 8; ++resource) rig.attach(video + 1, resource, metaSize);
    rig.setOk(video, {{UML_V4L2_CID_IMAGE_STABILIZATION, 1, {}}});
    rig.queue(video + 1, 1);
    rig.queue(video + 1, 2);
    rig.queue(video, 1);
    rig.queue(video, 2);
    rig.pumpSteps(camera, 2);
}

/* The guest's camera layer reads the camera's interrupted record, which a lost video's failed buffers wait for: a meta buffer
 * queued, the record received in it, and the buffer queued again. The lost video's first buffer has been queued, which is
 * what makes the device make the record. */
void readTheLossRecord(Rig& rig, std::uint32_t metaStream, std::uint32_t resource)
{
    const auto request = rig.queue(metaStream, resource);
    require(rig.waitFor([&] { return rig.done(request); }), "the interrupted record is served");
    rig.queue(metaStream, resource);
}

/* The application leaves the foreground while the guest records: the device ends the video at once, as the platform's
 * notice would, and the application's call returns only when the guest has stopped the video stream it ran, which the
 * guest's camera layer does when it has seen the loss. The platform's notice, which comes later, finds the camera lost. */
void testTheDepartureEndsTheRunningVideoAndWaitsForTheGuestToStopIt()
{
    Rig rig;
    runningVideo(rig);
    require(rig.camera->videoRunning(0) && rig.states.back() == CameraState::streaming, "the video runs");
    Departure leaving(*rig.background, std::chrono::seconds(8));
    require(rig.waitFor([&] { return rig.states.back() == CameraState::interrupted; }), "the application is told at once");
    require(!rig.camera->videoRunning(0) && !rig.videoModes.back(), "the platform's capture is stopped, and the video mode ends");
    require(!leaving.returned(), "the application waits: the guest has not acted on the loss");
    const auto a = rig.queue(1, 3), b = rig.queue(1, 4);
    readTheLossRecord(rig, 2, 3);
    require(rig.waitFor([&] { return rig.done(a) && rig.done(b); }), "the buffers of the lost video are answered");
    require(rig.result(a).base.flags == endedFlags && rig.result(b).base.flags == endedFlags, "and fail, interrupted");
    require(!leaving.returned(), "answering the guest's buffers is not the guest's stopping the stream");
    clearStream(rig, 1);
    const auto outcome = leaving.join();
    require(outcome.answered && outcome.stopped && outcome.streams == 1, "the call returns when the guest has stopped its stream");
    require(leaving.took() < std::chrono::seconds(8), "within the deadline");

    // The platform's own notice comes after: the camera is lost already, and nothing is undone or told twice.
    const auto states = rig.states.size();
    rig.camera->interrupt(CameraEventKind::interruptedInBackground);
    rig.step();
    require(rig.states.size() == states || rig.states.back() == CameraState::interrupted, "the notice finds the camera down");
    const auto c = rig.queue(1, 5);
    require(rig.waitFor([&] { return rig.done(c); }) && rig.result(c).base.flags == endedFlags && !rig.camera->videoRunning(0),
            "and the video stays lost");
    // The guest closes its video: the camera is its again.
    rig.destroy(1);
    require(rig.states.back() == CameraState::idle, "closed, the host is idle");
}

/* How long a test lets the device's black timer tick (several periods of a 30 fps stream). */
void letTheTimersTick(Rig& rig)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    require(rig.waitFor([&] { return std::chrono::steady_clock::now() > until; }), "stepped");
}

/* A lost video's first failure is the loss: the camera's interrupted record reaches the guest first, and the video's buffers
 * fail only once the guest has queued again the meta buffer that carried it. A guest that has not read the record would take
 * the failure for an ordinary one, and find the loss only a request later. */
void testTheInterruptedRecordGoesToTheGuestBeforeTheFirstFailedBuffer()
{
    Rig rig;
    runningVideo(rig);
    rig.camera->interrupt(CameraEventKind::interruptedInBackground);
    rig.step();
    require(rig.states.back() == CameraState::interrupted, "the video ended");
    // No meta buffer is queued (the running video used both): the record has nowhere to go, and the buffers wait for it.
    const auto a = rig.queue(1, 3), b = rig.queue(1, 4);
    letTheTimersTick(rig);
    require(!rig.done(a) && !rig.done(b), "the buffers wait for a guest that has not read the record");
    const auto carrier = rig.queue(2, 3);
    require(rig.waitFor([&] { return rig.done(carrier); }), "the record is served in the first meta buffer the guest queues");
    require(metaOf(rig, 2, 3).flags == blackRecord, "black and interrupted");
    letTheTimersTick(rig);
    require(!rig.done(a) && !rig.done(b), "and the buffers wait until the guest has queued it again");
    // Another meta buffer is not the one that carried the record.
    rig.queue(2, 5);
    letTheTimersTick(rig);
    require(!rig.done(a) && !rig.done(b), "another buffer's queueing is not the record's reading");
    rig.queue(2, 3);
    require(rig.waitFor([&] { return rig.done(a) && rig.done(b); }), "the buffers fail once the record has been read");
    require(rig.result(a).base.flags == endedFlags && rig.result(b).base.flags == endedFlags, "interrupted");
    const auto& order = rig.completionOrder;
    const auto record = std::find(order.begin(), order.end(), carrier);
    const auto failure = std::find(order.begin(), order.end(), a);
    require(record != order.end() && failure != order.end() && record < failure, "the record was completed first");
}

/* A guest that does not read the record is waited for 100 ms (the device's clock), and the buffers fail then. */
void testTheFailedBuffersWaitForTheRecordForAtMostTheBound()
{
    Rig rig;
    runningVideo(rig);
    const auto carrier = rig.queue(2, 3);
    rig.camera->interrupt(CameraEventKind::interruptedInBackground);
    rig.step();
    const auto a = rig.queue(1, 3);
    require(rig.waitFor([&] { return rig.done(carrier); }), "the record is served");
    rig.camera->advanceClock(99'000'000);
    letTheTimersTick(rig);
    require(!rig.done(a), "within the bound the buffer waits");
    rig.camera->advanceClock(2'000'000);
    require(rig.waitFor([&] { return rig.done(a); }), "past it the buffer fails");
    require(rig.result(a).base.flags == endedFlags, "interrupted");
}

/* With no meta stream running there is no reader to wait for, and the first failure is at once. */
void testNoMetaStreamNoWaitForTheRecord()
{
    Rig rig;
    videoStream(rig, 1, 0, 640, 480, 4);
    rig.queue(1, 1);
    rig.pumpSteps(0, 1);
    rig.camera->interrupt(CameraEventKind::interruptedInBackground);
    rig.step();
    const auto a = rig.queue(1, 2);
    require(rig.waitFor([&] { return rig.done(a); }, 1000), "the buffer fails with no wait");
    require(rig.result(a).base.flags == endedFlags, "interrupted");
}

/* No video runs: the call returns at once with nothing ended, whatever else the guest has open on the camera. */
void testTheDepartureWithNoRunningVideoEndsAndWaitsForNothing()
{
    {
        Rig rig;
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return leaving.returned(); }), "a guest with no stream is not waited for");
        const auto outcome = leaving.join();
        require(outcome.answered && outcome.stopped && outcome.streams == 0, "nothing was ended");
    }
    {
        // A video stream the guest has created and not started, and a still stream, run no video.
        Rig rig;
        videoStream(rig, 1, 0, 640, 480, 4);
        rig.create(2, 0, stillKind);
        const auto states = rig.states.size();
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return leaving.returned(); }), "no wait");
        const auto outcome = leaving.join();
        require(outcome.answered && outcome.stopped && outcome.streams == 0, "nothing was ended");
        require(rig.states.size() == states, "the application is told nothing");
        // The camera is not lost: the stream starts as any does.
        const auto a = rig.queue(1, 1);
        require(rig.camera->videoRunning(0), "the camera opens when the guest asks");
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.done(a) && rig.result(a).base.flags != endedFlags, "an ordinary stream, not a lost one");
    }
    {
        // A stream the guest stopped before the departure is not running either.
        Rig rig;
        runningVideo(rig);
        clearStream(rig, 1);
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return leaving.returned(); }), "a stopped stream is not waited for");
        require(leaving.join().streams == 0, "and ends nothing");
    }
    {
        // A blocked session runs no video.
        RigOptions options;
        options.mode = CameraMode::blocked;
        Rig rig(options);
        videoStream(rig, 1, 0, 640, 480, 3);
        rig.queue(1, 1);
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return leaving.returned(); }), "a blocked session is not waited for");
        require(leaving.join().streams == 0, "and ends nothing");
        const auto a = rig.queue(1, 2);
        require(rig.waitFor([&] { return rig.done(a); }) && rig.result(a).base.flags == (blackFlag | blockedFlag), "it stays blocked");
    }
}

/* The deadline is the call's own: a guest that never stops its stream, and a device that never answers, hold the
 * application for the deadline and no longer. */
void testTheDepartureEndsAtItsDeadline()
{
    {
        Rig rig;
        runningVideo(rig);
        Departure leaving(*rig.background, std::chrono::milliseconds(300));
        require(rig.waitFor([&] { return leaving.returned(); }, 3000), "the call returns by its deadline");
        const auto outcome = leaving.join();
        require(outcome.answered && !outcome.stopped && outcome.streams == 1, "the guest did not stop its stream");
        require(leaving.took() >= std::chrono::milliseconds(280) && leaving.took() < std::chrono::milliseconds(2500),
                "after the deadline and not much later");
        require(rig.states.back() == CameraState::interrupted, "the video was ended all the same");
    }
    {
        // The device's worker does not run: the request is not taken.
        Rig rig;
        runningVideo(rig);
        Departure leaving(*rig.background, std::chrono::milliseconds(200));
        const auto outcome = leaving.join();
        require(!outcome.answered && !outcome.stopped && outcome.streams == 0, "an unanswered request is said so");
        require(leaving.took() < std::chrono::milliseconds(2500), "by the deadline");
    }
}

/* The platform's notice may come first: the camera is lost already, the guest has not stopped its stream, and the
 * application waits for that. A loss the guest has had its time for is not waited for again. */
void testTheDepartureWaitsForALossTheNoticeBegan()
{
    {
        Rig rig;
        runningVideo(rig);
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        require(rig.states.back() == CameraState::interrupted, "the notice ended the video");
        Departure leaving(*rig.background, std::chrono::seconds(8));
        // The request is taken within this time, and the call still waits for the guest.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        require(rig.waitFor([&] { return leaving.returned() || std::chrono::steady_clock::now() > until; }), "stepped");
        require(!leaving.returned(), "the application waits for the guest's stop of the stream the notice began");
        clearStream(rig, 1);
        const auto outcome = leaving.join();
        require(outcome.answered && outcome.stopped && outcome.streams == 1, "and goes on when it is stopped");
    }
    {
        Rig rig;
        runningVideo(rig);
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        rig.camera->advanceClock(6'000'000'000);
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return leaving.returned(); }), "an old loss is not waited for");
        require(leaving.join().streams == 0, "the guest has had its time for it");
    }
}

/* What the guest's end of the run, or the device's, does to a call that waits: it returns, with nothing left to wait for.
 * It shows the wait returning and nothing about the guest that restarts: that the restarted guest does not boot behind the
 * hold that was waiting is kernarium-gpu-hold's. */
void testTheDepartureEndsWithTheGuestOrTheDevice()
{
    {
        Rig rig;
        runningVideo(rig);
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return rig.states.back() == CameraState::interrupted; }), "ended");
        require(!leaving.returned(), "waiting");
        rig.resetByTransport();
        const auto outcome = leaving.join();
        require(outcome.stopped && outcome.answered, "a reset of the device ends the wait");
        require(leaving.took() < std::chrono::seconds(4), "at once");
    }
    {
        Rig rig;
        runningVideo(rig);
        Departure leaving(*rig.background, std::chrono::seconds(8));
        require(rig.waitFor([&] { return rig.states.back() == CameraState::interrupted; }), "ended");
        rig.device.reset();
        const auto outcome = leaving.join();
        require(outcome.stopped, "a device that goes away ends the wait");
        require(leaving.took() < std::chrono::seconds(4), "at once");
        // And with no device the call waits for nothing.
        const auto none = rig.background->end(BackgroundVideo::Clock::now() + std::chrono::seconds(8));
        require(none.streams == 0 && none.stopped && none.answered, "no device, no wait");
    }
}

/* An iPad lists a wide and an ultra wide front camera over one sensor: the
 * guest sees the wide one, and a stream on it runs that device. */
void testFrontCameraChoice()
{
    RigOptions options;
    options.profile = SyntheticCameraProfile::tabletDualFront;
    Rig rig(options);
    const auto cameras = rig.device->cameras();
    require(cameras.size() == 2, "two cameras are published, not three");
    require(cameras[0].info.position == CameraPosition::back && cameras[1].info.position == CameraPosition::front &&
                cameras[1].info.lens == CameraLensKind::wide && cameras[1].platformIndex == 1,
            "the front one is the wide device");
    require(cameras[1].wire.facing == VIRTIO_VIDEO_CAMERA_FACING_FRONT, "facing front");
    videoStream(rig, 1, 1, 640, 480, 1);
    rig.queue(1, 1);
    require(rig.camera->videoRunning(1) && !rig.camera->videoRunning(2), "a stream on it runs the wide front device");
}

/* Cameras that run together need formats a multi-camera session takes: the
 * first camera moves to one when a second joins, a second that cannot be
 * joined is refused and told so, and a camera left alone goes back. */
void testCamerasJoin()
{
    {
        RigOptions options;
        options.profile = SyntheticCameraProfile::tabletDualFront;
        Rig rig(options);
        EventQueue events(rig, 4);
        videoStream(rig, 1, 0, 1600, 900, 2);
        videoStream(rig, 2, 1, 640, 480, 3);
        const auto first = rig.queue(1, 1);
        require(rig.camera->videoSize(0) == std::make_pair(1600u, 900u), "alone, the back camera runs the format that covers 1600 x 900");
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.done(first) && rig.result(first).base.flags == 0, "and delivers");

        const auto second = rig.queue(2, 1);
        require(rig.camera->videoRunning(1) && rig.camera->videoSize(1) == std::make_pair(640u, 480u), "the front camera joins");
        require(rig.camera->videoSize(0) == std::make_pair(1920u, 1080u), "the back camera moved to a format a multi-camera session takes");
        require(rig.camera->sessionsOpened() == 1 && events.arrived().empty(), "in the same session, and the guest has nothing to be told");
        const auto third = rig.queue(1, 2);
        rig.camera->pump(0, 1);
        rig.camera->pump(1, 1);
        rig.step();
        require(rig.done(second) && rig.result(second).base.flags == 0, "the front camera's frame");
        require(rig.done(third) && rig.result(third).base.flags == 0 && rig.result(third).base.data_sizes[0] == nv12Size(1600, 900),
                "and the back camera's, of the size its stream asked for");
        const auto picture = rig.bytes(1, 2, nv12Size(1600, 900));
        std::uint64_t lumaSum = 0;
        for (std::size_t index = 0; index < 1600u * 900u; ++index) lumaSum += picture[index];
        require(lumaSum / (1600u * 900u) > 20, "a picture of the chart, scaled from the camera that moved, not a black one");

        clearStream(rig, 2);
        require(!rig.camera->videoRunning(1) && rig.camera->videoSize(0) == std::make_pair(1600u, 900u),
                "when the front camera stops the back one goes back to its own format");
        require(rig.camera->videoRunning(0), "and keeps running");
    }

    // A back camera at a size no multi-camera format covers cannot be joined.
    RigOptions options;
    options.profile = SyntheticCameraProfile::tabletDualFront;
    options.tier = CameraTier::large;
    Rig rig(options);
    EventQueue events(rig, 6);
    videoStream(rig, 1, 0, 3840, 2160, 1);
    videoStream(rig, 2, 1, 640, 480, 3);
    rig.queue(1, 1);
    require(rig.camera->videoSize(0) == std::make_pair(3840u, 2160u), "4K on the back camera");
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    const auto refused = rig.queue(2, 1);
    require(!rig.camera->videoRunning(1) && rig.camera->videoSize(0) == std::make_pair(3840u, 2160u),
            "the second camera is refused and nothing changes");
    require(events.arrived() == EventList {{VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 1}}, "the guest is told that camera cannot run");
    require(rig.waitFor([&] { return rig.done(refused); }), "its stream gets black frames meanwhile");
    require(rig.result(refused).base.flags == (blackFlag | interruptedFlag), "black and interrupted");

    // The torch holds the session while the 4K stream ends; the front camera is tried again in it.
    clearStream(rig, 1);
    require(rig.camera->sessionsOpen() == 1, "the torch holds the session");
    rig.camera->advanceClock(1'500'000'000);
    require(rig.waitFor([&] { return rig.camera->videoRunning(1); }), "the camera that could not start is tried again, in the open session");
    require(rig.camera->sessionsOpened() == 1, "without a new session");
    require(events.arrived() == EventList {{VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 1}},
            "the guest is not told it is back before the capture delivers");
    const auto live = rig.queue(2, 2);
    rig.camera->pump(1, 1);
    rig.step();
    require(rig.done(live) && rig.result(live).base.flags == 0, "frames flow");
    require(events.arrived() == EventList {{VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 1}, {VIRTIO_VIDEO_EVENT_CAMERA_RESUMED, 1}},
            "and then the guest is told it is back");
}

/* A camera that only takes stills is an input of the platform's session too:
 * with another camera streaming, it joins as a streaming camera does. The
 * camera that streams moves to a format a multi-camera session takes while the
 * still is on its way, and goes back after; a still that cannot be taken fails
 * its buffer and leaves the stream alone. */
void testStillOnAnotherCamera()
{
    RigOptions options;
    options.profile = SyntheticCameraProfile::tabletDualFront;
    options.tier = CameraTier::large;
    Rig rig(options);
    const auto stillStream = [&](std::uint32_t stream, std::uint32_t camera) {
        rig.create(stream, camera, stillKind);
        require(Rig::type(rig.setParams(stream, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.attach(stream, resource, rig.params(stream).plane_formats[0].plane_size);
    };
    using Size = std::pair<std::uint32_t, std::uint32_t>;

    // The back camera streams at a size of its own, and the front camera takes a still. The first
    // buffer starts the capture, and the stream's frames are black until the capture delivers one,
    // on a timer in real time: a captured frame ends that, so that the frames below are the
    // camera's whatever time this thread takes between the steps.
    videoStream(rig, 1, 0, 1600, 900, 3);
    stillStream(2, 1);
    rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    const auto frame = rig.queue(1, 2);
    require(rig.camera->videoSize(0) == Size(1600, 900), "alone, the back camera runs the format that covers 1600 x 900");
    const auto still = rig.queue(2, 1, 777);
    require(rig.camera->videoSize(0) == Size(1920, 1080), "the back camera moved to a format a multi-camera session takes");
    require(rig.camera->sessionsOpened() == 1 && !rig.done(still), "in the same session, and the still is with the platform");
    rig.camera->pump(1, 0);
    rig.step();
    require(rig.done(still) && rig.result(still).base.flags == 0 && rig.result(still).base.timestamp == 777, "the still is taken");
    require(rig.camera->videoSize(0) == Size(1600, 900), "and the back camera goes back to its own format once it is delivered");
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(frame) && rig.result(frame).base.flags == 0, "the stream never stopped");

    // A 4K stream has no format a multi-camera session takes: the still is refused.
    clearStream(rig, 1);
    rig.destroy(1);
    videoStream(rig, 1, 0, 3840, 2160, 1);
    rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    const auto big = rig.queue(1, 1);
    require(rig.camera->videoSize(0) == Size(3840, 2160), "4K on the back camera");
    const auto refused = rig.queue(2, 2, 778);
    require(rig.done(refused) && rig.result(refused).base.flags == errorFlag, "the still fails its buffer");
    require(rig.camera->videoRunning(0) && rig.camera->videoSize(0) == Size(3840, 2160), "and the stream is left alone");
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(big) && rig.result(big).base.flags == 0, "frames still flow");

    // With that stream ended the still is taken, and the camera's session ends with it.
    clearStream(rig, 1);
    const auto later = rig.queue(2, 1, 779);
    rig.camera->pump(1, 0);
    rig.step();
    require(rig.done(later) && rig.result(later).base.flags == 0, "the still is taken");
    require(rig.camera->sessionsOpen() == 0, "and the session is released");
}

/* Cameras that ask for more than the hardware gives together are not all run:
 * the camera that joins is refused, and the one that runs is left alone. */
void testHardwareBudget()
{
    RigOptions options;
    options.profile = SyntheticCameraProfile::tabletDualFront;
    Rig rig(options);
    EventQueue events(rig, 4);
    videoStream(rig, 1, 0, 1920, 1080, 3);
    videoStream(rig, 2, 1, 1920, 1080, 3);
    require(Rig::type(rig.setInterval(1, 1, 60)) == VIRTIO_VIDEO_RESP_OK_NODATA, "60 frames a second on the back camera");
    require(Rig::type(rig.setInterval(2, 1, 60)) == VIRTIO_VIDEO_RESP_OK_NODATA, "and on the front camera");
    const auto back = rig.queue(1, 1);
    const auto front = rig.queue(2, 1);
    require(rig.camera->videoRunning(0) && !rig.camera->videoRunning(1), "the second camera is refused");
    require(events.arrived() == EventList {{VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 1}}, "and told that camera cannot run");
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(back) && rig.result(back).base.flags == 0, "the first camera delivers");
    require(rig.waitFor([&] { return rig.done(front); }) && rig.result(front).base.flags == (blackFlag | interruptedFlag),
            "the second gets black frames");

    // At half the rate both fit.
    clearStream(rig, 2);
    require(Rig::type(rig.setInterval(2, 1, 30)) == VIRTIO_VIDEO_RESP_OK_NODATA, "30 frames a second");
    rig.queue(2, 2);
    require(rig.camera->videoRunning(1) && rig.camera->videoRunning(0), "both run");
}

/* The colour data a still may teach is a courtesy: a Bayer part that the
 * platform refuses or that fails alone leaves the person's JPEG, and leaves the
 * lens uncalibrated. */
void testCalibrationRawIsACourtesy()
{
    for (const bool refuse : {true, false}) {
        auto store = std::make_shared<CalibrationStore>();
        RigOptions options;
        options.store = store;
        Rig rig(options);
        if (refuse) rig.camera->refuseRaw(true);
        else rig.camera->failRaw(true);
        rig.create(1, 0, stillKind);
        require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
        const auto still = rig.queue(1, 1, 555);
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(still) && rig.result(still).base.flags == 0 && rig.result(still).base.data_sizes[0] > 4000,
                "the person's photo is delivered");
        require(!store->load(rig.camera->hostModel(), CalibrationStore::lensKey(rig.camera->cameras()[0])), "and nothing was learned");
    }

    // A guest that asks for RAW and a JPEG gets the JPEG when the RAW fails alone.
    auto store = std::make_shared<CalibrationStore>();
    {
        RigOptions options;
        options.store = store;
        Rig learner(options);
        learner.create(1, 0, stillKind);
        require(Rig::type(learner.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        learner.attach(1, 1, learner.params(1).plane_formats[0].plane_size);
        learner.queue(1, 1);
        learner.camera->pump(0, 0);
        learner.step();
    }
    RigOptions options;
    options.store = store;
    Rig rig(options);
    rig.create(1, 0, stillKind);
    rig.create(2, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "JPEG");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "RAW");
    rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
    rig.attach(2, 1, rig.params(2).plane_formats[0].plane_size);
    rig.camera->failRaw(true);
    const auto raw = rig.queue(2, 1, 9, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
    const auto jpeg = rig.queue(1, 1, 9);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(raw) && rig.done(jpeg), "the group completes");
    require(rig.result(jpeg).base.flags == 0 && rig.result(jpeg).base.data_sizes[0] > 4000, "the JPEG is whole");
    require((rig.result(raw).base.flags & errorFlag) != 0 && rig.result(raw).base.timestamp == 9, "and the RAW buffer fails alone");
}

/* A host that cannot take a Bayer still (the Apple session refuses the RAW request when the photo output lists no
 * Bayer format) never learns a lens's colour data, so nothing RAW is published for it: the camera has no RAW flag, no
 * Bayer order, size or levels, its stills capability lists JPEG only, and the layout the guest's image is chosen by
 * has no `-raw`. The guest's half (no RAW capability, no RAW16 configuration) is pinned in andarium-camera-plan. */
void testAHostThatCannotTakeABayerStillPublishesNoRaw()
{
    auto store = std::make_shared<CalibrationStore>();
    {
        RigOptions options;
        options.store = store;
        Rig learner(options);
        learner.camera->refuseRaw(true);
        learner.create(1, 0, stillKind);
        require(Rig::type(learner.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        learner.attach(1, 1, learner.params(1).plane_formats[0].plane_size);
        const auto still = learner.queue(1, 1, 555);
        learner.camera->pump(0, 0);
        learner.step();
        require(learner.done(still) && learner.result(still).base.flags == 0, "the person's photo is delivered");
        require(!store->load(learner.camera->hostModel(), CalibrationStore::lensKey(learner.camera->cameras()[0])),
                "and nothing was learned");
    }
    RigOptions options;
    options.store = store;
    Rig rig(options);
    const auto cameras = rig.device->cameras();
    require(!cameras.empty(), "the host has cameras");
    for (const auto& camera : cameras) {
        const auto& wire = camera.wire;
        require(!camera.raw() && (wire.flags & VIRTIO_VIDEO_CAMERA_F_RAW) == 0, "no RAW feature");
        require(wire.bayer_order == VIRTIO_VIDEO_CAMERA_BAYER_NONE && wire.raw_width == 0 && wire.raw_height == 0 &&
                    wire.black_level == 0 && wire.white_level == 0 && wire.reference_illuminant1 == 0 && wire.reference_illuminant2 == 0,
                "no Bayer order, RAW size, levels or illuminants");
        for (const auto value : wire.color_matrix1_q16) require(value == 0, "no colour matrix");
        const auto stills = rig.send(virtio_video_camera_query_formats {{VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, 0}, camera.index, stillKind});
        const auto head = load<virtio_video_query_capability_resp>(stills.response);
        require(head.num_descs == 1 &&
                    load<virtio_video_format_desc>(stills.response, sizeof(virtio_video_query_capability_resp)).format ==
                        VIRTIO_VIDEO_FORMAT_CAMERA_JPEG,
                "the stills capability lists JPEG alone, no RAW16");
    }
    const auto layout = layoutDimensionsOf(cameras, CameraTier::small);
    require(!layout.raw && layoutName(layout).find("raw") == std::string::npos, "the layout has no RAW feature");
    require(layoutName(layout) == "bf-small-flash-manual", "the tablet runs the layout without it");
}

/* The guest asked for a JPEG of one size: a platform picture of another size
 * fails its buffer, and the next still, of the right size, is delivered. */
void testAStillOfAnotherSizeFails()
{
    Rig rig;
    rig.create(1, 0, stillKind);
    rig.create(2, 0, metaKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    const auto bufferSize = rig.params(1).plane_formats[0].plane_size;
    rig.attach(1, 1, bufferSize);
    rig.attach(2, 1, metaSize);
    rig.camera->stillsOfAnotherSize(true);
    const auto meta = rig.queue(2, 1);
    const auto wrong = rig.queue(1, 1, 31);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(wrong) && rig.result(wrong).base.flags == errorFlag && rig.result(wrong).base.data_sizes[0] == 0 &&
                rig.result(wrong).base.timestamp == 31,
            "a picture of another size fails the guest's buffer, without bytes");
    require(rig.done(meta) && metaOf(rig, 2, 1).jpeg_gap_length == 0, "and no gap is described for it");
    require(rig.camera->sessionsOpen() == 0, "the capture session is released");

    rig.camera->stillsOfAnotherSize(false);
    rig.attach(1, 2, bufferSize);
    const auto right = rig.queue(1, 2, 32);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(right) && rig.result(right).base.flags == 0 && rig.result(right).base.data_sizes[0] > 4000,
            "the same stream takes a picture of the size it asked for");
}

/* An interruption belongs to the platform session it happened in. */
void testInterruptionEndsWithTheSession()
{
    Rig rig;
    EventQueue events(rig, 8);
    videoStream(rig, 1, 0, 640, 480, 4);
    const auto a = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(a) && rig.result(a).base.flags == 0, "frames flow");
    rig.camera->interrupt();
    rig.step();
    require(events.arrived() == EventList {{VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 0}, {VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 1}},
            "both cameras are interrupted");
    require(rig.states.back() == CameraState::interrupted, "the application is told");

    // The guest stops its streams: the session closes and what interrupted it is over with it.
    clearStream(rig, 1);
    require(rig.camera->sessionsOpen() == 0 && rig.states.back() == CameraState::idle, "the session is released");
    require(events.arrived().size() == 4 && events.arrived()[2].first == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED &&
                events.arrived()[3].first == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED,
            "and the guest is told the interruption ended");
    rig.camera->resume();  // the cause itself goes, with no session open
    rig.step();
    require(events.arrived().size() == 4, "nothing more is said");

    // A stream that starts again finds live frames and no stale state.
    const auto b = rig.queue(1, 2);
    require(rig.camera->sessionsOpen() == 1 && rig.states.back() == CameraState::streaming, "a new session");
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(b) && rig.result(b).base.flags == 0, "frames are live, not black");
    require(events.arrived().size() == 4, "and no stale event comes");

    // A session that is lost is opened again, a second later, and the guest hears of both.
    rig.camera->post({allCameras, CameraEventKind::failed});
    rig.step();
    require(rig.camera->sessionsOpen() == 0, "a failed session is closed");
    require(events.arrived().size() == 5 && events.arrived()[4] == std::make_pair(VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED, 0u),
            "the camera that has a stream is told, and the other, which has none, is not");
    const auto c = rig.queue(1, 3);
    require(rig.waitFor([&] { return rig.done(c); }), "black frames bridge it");
    require(rig.result(c).base.flags == (blackFlag | interruptedFlag), "black and interrupted");
    rig.camera->advanceClock(1'200'000'000);
    require(rig.waitFor([&] { return rig.camera->videoRunning(0); }), "the session is opened again");
    require(rig.camera->sessionsOpened() == 3, "a new one");
    require(events.arrived().size() == 5, "and the guest is not told it is back before the capture delivers");
    const auto d = rig.queue(1, 4);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(d) && rig.result(d).base.flags == 0, "frames are live again");
    require(events.arrived().size() == 6 && events.arrived()[5] == std::make_pair(VIRTIO_VIDEO_EVENT_CAMERA_RESUMED, 0u),
            "and then the guest is told it is back");
}

/* A capture that cannot start fails round after round: the platform's session
 * reports an interruption and then its failure, and the device opens another a
 * second later. The camera with the stream is told it is interrupted once, and
 * told it is back when a frame comes. */
void testFailingStartTellsOnce()
{
    Rig rig;
    EventQueue events(rig, 24);
    videoStream(rig, 1, 0, 640, 480, 4);
    const auto first = rig.queue(1, 1);
    require(rig.camera->sessionsOpened() == 1, "a session");
    const auto told = [&](std::uint32_t type) {
        const auto all = events.arrived();
        return std::count(all.begin(), all.end(), std::make_pair(type, 0u));
    };
    for (unsigned round = 0; round < 3; ++round) {
        rig.camera->post({allCameras, CameraEventKind::interrupted});
        rig.camera->post({allCameras, CameraEventKind::failed});
        rig.step();
        require(rig.camera->sessionsOpen() == 0, "a failed session is closed");
        require(told(VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED) == 1, "told it is interrupted once, however many rounds have failed");
        rig.camera->advanceClock(1'200'000'000);
        require(rig.waitFor([&] { return rig.camera->sessionsOpened() == round + 2; }), "the camera is tried again a second later");
        require(told(VIRTIO_VIDEO_EVENT_CAMERA_RESUMED) == 0, "and not told it is back by a session that opened and failed");
    }
    // A session that stays up delivers: now it is back, and told so once.
    const auto live = rig.queue(1, 2);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(first) && rig.done(live) && rig.result(live).base.flags == 0, "frames are live");
    require(told(VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED) == 1 && told(VIRTIO_VIDEO_EVENT_CAMERA_RESUMED) == 1,
            "told interrupted once and back once");
}

/* System pressure is an interruption with a state of its own. */
void testSystemPressure()
{
    Rig rig;
    EventQueue events(rig, 4);
    videoStream(rig, 1, 0, 640, 480, 3);
    const auto a = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(a), "frames flow");
    rig.camera->interrupt(CameraEventKind::interruptedBySystemPressure);
    rig.step();
    require(rig.states.back() == CameraState::interruptedBySystemPressure, "the application is told why");
    require(events.arrived().size() == 2 && events.arrived()[0].first == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED,
            "an interruption to the guest");
    const auto b = rig.queue(1, 2);
    require(rig.waitFor([&] { return rig.done(b); }), "black frames bridge it");
    require(rig.result(b).base.flags == (blackFlag | interruptedFlag), "black and interrupted");
    rig.camera->resume();
    rig.step();
    require(rig.states.back() == CameraState::streaming && events.arrived().size() == 4, "and it ends as the others do");
}

/* A guest suspend that finds buffers held resets the device. The device keeps
 * nothing, the driver creates its streams again, and the new streams' first
 * frames are black and flagged as resuming until the capture delivers. The
 * event buffers the guest posted are not held by the device: they are still in
 * the ring, and the first event after the resume reaches them. */
void testResetAndTheDriverRecreates()
{
    Rig rig;
    const auto build = [&](std::uint32_t firstResource) {
        videoStream(rig, 1, 0, 640, 480, 3);
        rig.create(2, 0, stillKind);
        require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        rig.attach(2, firstResource, rig.params(2).plane_formats[0].plane_size);
        rig.create(3, 0, metaKind);
        rig.attach(3, firstResource, metaSize);
        rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 2 * 65536, {}}});
    };
    EventQueue before(rig, 2);
    build(1);
    const auto video = rig.queue(1, 1), spare = rig.queue(1, 2);
    const auto still = rig.queue(2, 1, 777);  // with the platform, not yet taken
    const auto meta = rig.queue(3, 1);
    require(rig.camera->videoRunning(0) && rig.camera->sessionsOpen() == 1 && !rig.done(still), "streams and a still are in flight");
    require(rig.held.size() == 4, "the device holds the four buffers, and no event buffer");
    rig.disableRings();
    require(rig.resets == 0 && rig.held.size() == 4, "a suspend resets nothing and the device keeps what it holds");
    rig.resetByTransport();
    require(rig.resets == 1, "a change of the guest's memory resets the device");
    require(rig.camera->sessionsOpen() == 0 && !rig.camera->videoRunning(0) && rig.states.back() == CameraState::idle,
            "the capture is stopped");
    rig.camera->pump(0, 1);  // nothing of the old capture is left to deliver
    rig.step();
    for (const auto id : {video, spare, still, meta})
        require(!rig.done(id), "no buffer of the old guest completes to the new one");
    require(rig.eventRing.size() == 2 && !rig.done(before.ids[0]) && !rig.done(before.ids[1]),
            "the event buffers are still in the ring");
    rig.enableRings();

    // The driver creates everything again.
    build(1);
    require(rig.camera->sessionsOpen() == 0 && rig.get(1, {UML_V4L2_CID_ZOOM_ABSOLUTE})[0] == 200,
            "the driver's controls are kept, and open no session");
    const auto a = rig.queue(1, 1), b = rig.queue(1, 2);
    require(rig.camera->videoRunning(0), "the camera runs again");
    require(rig.camera->appliedSettings(0).zoomRatio == 2.0f, "with the controls the driver applied again");
    require(rig.waitFor([&] { return rig.done(a) && rig.done(b); }), "black frames come at once");
    for (const auto id : {a, b}) require(rig.result(id).base.flags == (blackFlag | resumingFlag), "black and resuming");
    const auto live = rig.queue(1, 3);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(live) && rig.result(live).base.flags == 0, "then the first captured frame ends it");
    require(rig.result(live).sequence > rig.result(b).sequence, "and the numbers keep counting");
    // The first event after the resume reaches a buffer posted before the suspend.
    rig.camera->interrupt();
    rig.step();
    require(before.arrived().size() == 2 && before.arrived()[0].first == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED,
            "events reach the buffers the guest posted before it suspended");
}

/* The event queue never holds a buffer: an empty one is retried, so the buffer
 * stays in the ring, and an event raised later wakes the device and reaches it. */
void testEventQueueRetries()
{
    Rig rig;
    const auto id = rig.next++;
    for (int offer = 0; offer < 3; ++offer) {
        const auto result = rig.device->processQueue(1, id, {}, sizeof(virtio_video_event));
        require(result.action == VhostUserQueueAction::retry && result.response.empty(), "with no event the buffer stays in the ring");
    }
    // A buffer too small for an event is no buffer for one.
    require(rig.device->processQueue(1, rig.next++, {}, sizeof(virtio_video_event) - 1).action == VhostUserQueueAction::complete,
            "a short buffer is completed");

    videoStream(rig, 1, 0, 640, 480, 2);
    rig.queue(1, 1);
    // Whether the device has signalled its event descriptor with its wake-up since the last look.
    const auto signalled = [&] {
        std::array<struct kevent, 16> ready {};
        const timespec none {};
        const int count = ::kevent(rig.device->eventDescriptor(), nullptr, 0, ready.data(), int(ready.size()), &none);
        return std::any_of(ready.begin(), ready.begin() + std::max(count, 0),
                           [](const struct kevent& entry) { return entry.filter == EVFILT_USER; });
    };
    rig.device->drainEventSource();
    (void)signalled();
    rig.camera->interrupt();
    require(signalled(), "the platform's interruption wakes the device");
    // The interruption becomes an event inside the device's drain; the server must run the queue again.
    rig.device->drainEventSource();
    require(signalled(), "and the event it raises signals the event descriptor");
    const auto offered = rig.device->processQueue(1, id, {}, sizeof(virtio_video_event));
    require(offered.action == VhostUserQueueAction::complete, "the buffer offered again receives it");
    const auto event = load<virtio_video_event>(offered.response);
    require(event.event_type == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED && event.stream_id == 0, "the interruption of the first camera");
    // The second camera's event waits for the next buffer.
    const auto following = rig.device->processQueue(1, rig.next++, {}, sizeof(virtio_video_event));
    require(following.action == VhostUserQueueAction::complete && load<virtio_video_event>(following.response).stream_id == 1,
            "the second camera's event is kept for the next buffer");
    require(rig.device->processQueue(1, rig.next++, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::retry,
            "and then the queue is empty again");
}

/* A guest whose driver stopped its streams holds nothing at a suspend: the
 * rings are disabled and enabled again with no reset, the device keeps what the
 * driver set, and an event after the resume reaches the buffer posted before. */
void testSuspendResetsNothing()
{
    Rig rig;
    EventQueue events(rig, 2);
    videoStream(rig, 1, 0, 640, 480, 2);
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 2 * 65536, {}}});
    const auto frame = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(frame) && rig.camera->sessionsOpen() == 1, "a stream runs");
    rig.queue(1, 2);
    require(rig.held.size() == 1, "with a buffer held");
    // The driver stops it before the suspend.
    clearStream(rig, 1);
    rig.destroy(1);
    require(rig.held.empty() && rig.camera->sessionsOpen() == 0, "stopped, the camera holds nothing and the session is released");

    rig.disableRings();
    rig.enableRings();
    require(rig.resets == 0, "the suspend reset nothing");
    require(rig.eventRing.size() == 2 && events.arrived().empty(), "the event buffers wait in the ring");

    // The device is not reset: the driver's controls are what it set.
    videoStream(rig, 1, 0, 640, 480, 2);
    require(rig.get(1, {UML_V4L2_CID_ZOOM_ABSOLUTE})[0] == 200, "the camera kept its controls");
    // A later event still arrives: the interruption is the cause that lasts when the stream starts again.
    rig.camera->interrupt();
    rig.queue(1, 1);
    rig.step();
    require(events.arrived().size() >= 1 && events.arrived()[0].first == VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED,
            "a later event reaches the buffer posted before the suspend");
    // An interrupted stream's buffer is completed black by a timer in real time, so the held request
    // is made, and used, before the device is stepped again.
    clearStream(rig, 1);
    rig.queue(1, 2);
    require(rig.held.size() == 1, "the stream's buffer is the one request the device holds now");
    rig.disableRings();
    require(rig.resets == 0 && rig.held.size() == 1, "a suspend that finds a request held keeps it");
}

/* A group has two seconds to assemble; a capture that has started has ten. */
void testStillDeadlines()
{
    Rig rig;
    rig.create(1, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    for (std::uint32_t resource = 1; resource <= 2; ++resource) rig.attach(1, resource, rig.params(1).plane_formats[0].plane_size);
    const auto slow = rig.queue(1, 1, 31);  // fired at once, and the platform is slow
    rig.camera->advanceClock(5'000'000'000);
    rig.step();
    require(!rig.done(slow), "a capture under way is not held to the two seconds of a group");
    rig.camera->advanceClock(5'600'000'000);
    rig.step();
    require(rig.done(slow) && rig.result(slow).base.flags == errorFlag && rig.result(slow).base.timestamp == 31,
            "but its buffer fails when ten seconds have passed");
    require(rig.camera->sessionsOpen() == 1, "the session stays open while the platform holds the capture");
    // A result that comes after that is dropped.
    const auto next = rig.queue(1, 2, 32);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(next) && rig.result(next).base.flags == 0, "and the next still is taken");
    require(rig.camera->sessionsOpen() == 0, "the platform's end of the late capture releases the session");
}

/* The platform holds at most a pipeline's worth of captures for a camera. */
void testStillsInFlight()
{
    Rig rig;
    rig.create(1, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    const auto size = rig.params(1).plane_formats[0].plane_size;
    const std::uint32_t buffers = rig.params(1).max_buffers;
    for (std::uint32_t resource = 1; resource <= buffers + 2; ++resource) rig.attach(1, resource, size);
    std::vector<std::uint64_t> requests;
    for (std::uint32_t resource = 1; resource <= buffers + 2; ++resource) requests.push_back(rig.queue(1, resource, 100 + resource));
    for (std::uint32_t index = 0; index < buffers; ++index) require(!rig.done(requests[index]), "a capture with the platform");
    for (std::uint32_t index = buffers; index < buffers + 2; ++index)
        require(rig.done(requests[index]) && rig.result(requests[index]).base.flags == errorFlag, "one past the pipeline fails at once");
    rig.camera->pump(0, 0);
    rig.step();
    for (std::uint32_t index = 0; index < buffers; ++index)
        require(rig.done(requests[index]) && rig.result(requests[index]).base.flags == 0, "the others complete");
    // Room again.
    const auto again = rig.queue(1, buffers + 1, 500);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(again) && rig.result(again).base.flags == 0, "and the next one is taken");
}

/* What a guest that means harm can send, and what the device does with it. */
void testHostileGuest()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    const auto isError = [](const VhostUserQueueResult& result) {
        return result.action == VhostUserQueueAction::complete && Rig::type(result) >= VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    };

    // Commands cut short: every one that has a body, with the header alone.
    for (const auto type : {VIRTIO_VIDEO_CMD_CAMERA_QUERY, VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS, VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS,
                            VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, VIRTIO_VIDEO_CMD_GET_PARAMS, VIRTIO_VIDEO_CMD_SET_PARAMS,
                            VIRTIO_VIDEO_CMD_SET_STREAM_INFO, VIRTIO_VIDEO_CMD_GET_STREAM_INFO, VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS,
                            VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, VIRTIO_VIDEO_CMD_RESOURCE_ATTACH,
                            VIRTIO_VIDEO_CMD_RESOURCE_QUEUE, VIRTIO_VIDEO_CMD_QUEUE_CLEAR}) {
        for (const std::size_t extra : {std::size_t(0), std::size_t(3)}) {
            std::vector<std::uint8_t> request;
            append(request, virtio_video_cmd_hdr {type, 1});
            request.insert(request.end(), extra, 0);
            require(isError(rig.device->processQueue(0, rig.next++, request, 4096)), "a command cut short is refused");
        }
    }
    // A request shorter than a header, and a response buffer too short for one, are completed with nothing.
    require(rig.device->processQueue(0, rig.next++, std::vector<std::uint8_t>(3), 4096).response.empty(), "a request shorter than a header");
    require(rig.device->processQueue(0, rig.next++, std::vector<std::uint8_t> {1, 0, 0, 0, 1, 0, 0, 0}, 3).response.empty(),
            "a response buffer shorter than a header");
    // A response that does not fit the buffer is refused rather than cut.
    require(isError(rig.send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, output, {}}, sizeof(virtio_video_cmd_hdr))) ||
                rig.send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, output, {}}, sizeof(virtio_video_cmd_hdr))
                    .response.size() <= sizeof(virtio_video_cmd_hdr),
            "a response longer than the buffer");

    // Compound controls that leave the request.
    const auto setRegion = [&](std::uint64_t offset, std::uint32_t size, std::size_t areaBytes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_v4l2_controls {{VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1}, 1, {}});
        append(request, virtio_video_v4l2_control_value {VIRTIO_VIDEO_CAMERA_CID_AE_REGION, size, offset});
        request.insert(request.end(), areaBytes, 0);
        return rig.device->processQueue(0, rig.next++, request, 4096);
    };
    require(Rig::type(setRegion(0, regionPayloadSize, regionPayloadSize)) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "the region as it should be");
    require(Rig::type(setRegion(8 * 1000, regionPayloadSize, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an offset past the request");
    require(Rig::type(setRegion(~0ull - 7, regionPayloadSize, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an offset that wraps");
    require(Rig::type(setRegion(3, regionPayloadSize, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an offset that is not aligned");
    require(Rig::type(setRegion(8, regionPayloadSize, 24)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a payload that runs past the request");
    require(Rig::type(setRegion(0, regionPayloadSize + 4, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a size that is not the control's");
    require(Rig::type(setRegion(0, 0, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a compound control with no payload");
    // Control counts.
    for (const std::uint32_t count : {0u, 257u, 0xffffffffu}) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_v4l2_controls {{VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1}, count, {}});
        request.insert(request.end(), 4096, 0);
        require(Rig::type(rig.device->processQueue(0, rig.next++, request, 4096)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a count the device does not take");
    }
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_v4l2_controls {{VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, 1}, 200, {}});
        require(Rig::type(rig.device->processQueue(0, rig.next++, request, 4096)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "more controls than the request holds");
    }

    // Buffers that are not in the guest's memory.
    const auto attachWith = [&](std::uint32_t resource, std::uint32_t entries, std::uint64_t address, std::uint32_t length,
                                std::uint32_t flags) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 1, output, resource});
        append(request, entries);
        append(request, std::uint32_t(0));
        append(request, virtio_video_resource_sg_entry {address, length, flags});
        return rig.device->processQueue(0, rig.next++, request, 4096);
    };
    const auto size = nv12Size(640, 480);
    require(Rig::type(attachWith(1, 1, rig.memory.size() + 10 * page, size, 0)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "past guest memory");
    require(Rig::type(attachWith(1, 1, ~0ull - 100, size, 0)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an address that wraps");
    require(Rig::type(attachWith(1, 1, 16 * page, 0, 0)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an empty entry");
    require(Rig::type(attachWith(1, 1, 16 * page, size, 0x80000000u)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "unknown entry flags");
    require(Rig::type(attachWith(1, 1000, 16 * page, size, 0)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "more entries than the request holds");
    require(Rig::type(attachWith(0, 1, 16 * page, size, 0)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "resource 0");
    rig.attach(1, 1, size);
    // Memory the guest gives back after attaching: the frame completes with an error, and the device goes on.
    rig.attach(1, 2, size);
    const auto lost = rig.queue(1, 2);
    const auto kept = rig.memory;
    rig.memory.resize(page);
    rig.camera->pump(0, 1);
    rig.step();
    rig.memory = kept;
    require(rig.done(lost) && (rig.result(lost).base.flags & errorFlag), "a buffer whose memory is gone completes with an error");
    const auto good = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(good) && rig.result(good).base.flags == 0, "and the next buffer is filled");

    // A resource queued twice, queued before it exists, and attached while it is queued.
    const auto queueRaw = [&](std::uint32_t resource) {
        virtio_video_resource_queue command {};
        command.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        command.stream_id = 1;
        command.queue_type = output;
        command.resource_id = resource;
        std::vector<std::uint8_t> request;
        append(request, command);
        return rig.device->processQueue(0, rig.next++, request, sizeof(virtio_video_camera_queue_resp));
    };
    require(queueRaw(1).action == VhostUserQueueAction::defer, "a resource is queued");
    require(Rig::type(queueRaw(1)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "and not twice");
    require(Rig::type(queueRaw(77)) == VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID, "an unknown resource");
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 1, output, 1});
        append(request, std::uint32_t(1));
        append(request, std::uint32_t(0));
        append(request, virtio_video_resource_sg_entry {16 * page, size, 0});
        require(Rig::type(rig.device->processQueue(0, rig.next++, request, 4096)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a resource cannot be attached again while it is queued");
    }
    // The format of a stream with buffers cannot change, and a queue clear gives them back.
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 320, 240)) == VIRTIO_VIDEO_RESP_ERR_BUSY ||
                Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_NV12, 320, 240)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a stream that runs keeps its format");
    clearStream(rig, 1);

    // The ceilings: resources of a stream, streams of a device, records of a meta stream.
    rig.create(2, 0, metaKind);
    for (std::uint32_t resource = 1; resource <= 128; ++resource) rig.attach(2, resource, metaSize);
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 2, output, 129});
        append(request, std::uint32_t(1));
        append(request, std::uint32_t(0));
        append(request, virtio_video_resource_sg_entry {16 * page, metaSize, 0});
        require(Rig::type(rig.device->processQueue(0, rig.next++, request, 4096)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "no more than 128 resources on a stream");
    }
    for (std::uint32_t stream = 3; stream <= 32; ++stream) rig.create(stream, 0, metaKind);
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 33}, 0, metaKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY, "no more than 32 streams");
    rig.refused(virtio_video_camera_stream_create {{VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, 0xffff0001u}, 0, metaKind, 0, {}, {}},
                VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a reserved stream ID");
    for (std::uint32_t stream = 3; stream <= 32; ++stream) rig.destroy(stream);

    // A metadata stream keeps the last sixteen records of the frames it had no buffer for.
    rig.attach(1, 3, size);
    const auto first = rig.queue(2, 1);
    const auto frame = rig.queue(1, 3);
    for (int count = 0; count < 21; ++count) {
        rig.camera->pump(0, 1);
        rig.step();
    }
    require(rig.done(first) && rig.done(frame), "the first record is served");
    const auto late = rig.queue(2, 2);
    require(rig.done(late) && rig.result(late).sequence == rig.result(first).sequence + 5,
            "of the 20 records after the first, the last 16 are kept: the next one is the sixth");
}

/* The anchor model end to end, against a platform that runs an anchored camera in its anchor only and refuses
 * anything outside it: every still and every stream the guest is offered works, alone and beside another camera. */
void testEveryOfferedRunStartsInTheAnchor()
{
    Rig rig;
    const auto camera = rig.device->cameras()[0];
    require(camera.anchor == std::optional<std::size_t>(3), "the back camera is anchored in its 1920x1440 format");
    require(camera.jpegSizes.front() == CameraStillSize({4032, 3024}) && !camera.video.empty(), "it offers 12 MP stills and video");
    std::uint32_t stream = 10;
    for (const auto& size : camera.jpegSizes) {
        const auto id = stream++;
        rig.create(id, 0, stillKind);
        require(Rig::type(rig.setParams(id, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, size.width, size.height)) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "an offered size is accepted");
        rig.attach(id, 1, rig.params(id).plane_formats[0].plane_size);
        const auto buffer = rig.queue(id, 1);
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(buffer) && rig.result(buffer).base.flags == 0, "an offered still is made");
        rig.destroy(id);
        rig.bump = 16 * page;  // the guest's memory holds one 12 MP buffer
    }
    for (const auto& mode : camera.video)
        for (const auto rate : {mode.minimumFrameRate, mode.maximumFrameRate}) {
            const auto id = stream++;
            videoStream(rig, id, 0, mode.width, mode.height, 1);
            require(Rig::type(rig.setInterval(id, 1, rate)) == VIRTIO_VIDEO_RESP_OK_NODATA, "an offered rate is accepted");
            const auto buffer = rig.queue(id, 1);
            require(rig.camera->videoRunning(0) && rig.camera->videoFrameRate(0) == rate, "an offered size and rate start");
            rig.camera->pump(0, 1);
            rig.step();
            require(rig.done(buffer) && rig.result(buffer).base.flags == 0, "and deliver");
            rig.destroy(id);
            rig.bump = 16 * page;
        }

    // Beside the front camera, with a 12 MP still in the middle of a stream.
    videoStream(rig, 1, 0, 1280, 720, 2);
    videoStream(rig, 2, 1, 640, 480, 1);
    rig.create(3, 0, stillKind);
    require(Rig::type(rig.setParams(3, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 4032, 3024)) == VIRTIO_VIDEO_RESP_OK_NODATA, "12 MP");
    rig.attach(3, 1, rig.params(3).plane_formats[0].plane_size);
    const auto back = rig.queue(1, 1);
    const auto front = rig.queue(2, 1);
    require(rig.camera->videoRunning(0) && rig.camera->videoRunning(1), "both cameras stream");
    rig.camera->pump(0, 1);
    rig.camera->pump(1, 1);
    rig.step();
    require(rig.done(back) && rig.done(front) && rig.result(back).base.flags == 0 && rig.result(front).base.flags == 0, "and deliver");
    const auto still = rig.queue(3, 1, 4242);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(still) && rig.result(still).base.flags == 0 && rig.result(still).base.timestamp == 4242,
            "the 12 MP still is made while both cameras stream");
    const auto after = rig.queue(1, 2);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(after) && rig.result(after).base.flags == 0 && rig.camera->videoRunning(0), "and the stream goes on");
}

/* A burst of frames from one camera is one frame to the guest: the newest. The older ones are not held. */
void testABurstOfFramesIsTheNewestFrame()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    for (std::uint32_t resource = 1; resource <= 3; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    const auto first = rig.queue(1, 1), second = rig.queue(1, 2), third = rig.queue(1, 3);
    rig.camera->pump(0, 3);
    rig.step();
    require(rig.done(first) && !rig.done(second) && !rig.done(third), "three frames at once fill one buffer");
    require(rig.result(first).sequence == 1, "numbered as one frame");
    const auto marker = decodeChartMarker(rig.bytes(1, 1, nv12Size(640, 480)).data(), 640, 640, 480);
    require(marker.valid && marker.counter == 3, "the picture is the newest of the three");
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(second) && rig.result(second).sequence == 2, "the next frame is the next");
}

// R3 (recheck5): the reserve counts six held frames a camera (heldFramesPerCamera): the inbox's three, the one the
// pump writes, the one arriving and the one the capture fills. The pump takes the newest frame of a camera and frees
// the older ones before it lets go of the inbox, so the capture can refill the inbox to its limit while the pump
// handles a platform event and no more than one frame of the pump is held beside it. (A pump that kept its whole
// batch while the inbox refilled held seven: three, three, and the arriving one.)
void testTheMostFramesHeldAtOnceIsWhatTheReserveCounts()
{
    Rig rig;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    for (std::uint32_t resource = 1; resource <= 3; ++resource) rig.attach(1, resource, nv12Size(640, 480));
    for (std::uint32_t resource = 1; resource <= 3; ++resource) (void)rig.queue(1, resource);
    // A burst of three frames waits in the inbox with an event behind it.
    rig.camera->pump(0, 3);
    require(rig.camera->framesAlive() == inboxFramesPerCamera, "the inbox holds the newest three");
    rig.camera->post({uml::platform::allCameras, CameraEventKind::interrupted});
    bool refilled = false;
    rig.onStateChange = [&] {
        if (refilled) return;
        refilled = true;
        // The capture goes on while the event is handled: more frames than the inbox holds.
        rig.camera->pump(0, 4);
    };
    rig.camera->resetFramePeak();
    rig.step();
    require(refilled, "the event was handled with frames in the pump's hand");
    require(rig.camera->framePeak() <= inboxFramesPerCamera + 2,
            "at most the inbox's three, the pump's one and the arriving one are held at once");
    require(rig.camera->framePeak() <= heldFramesPerCamera, "and so never more than the reserve counts");
    require(rig.camera->framesAlive() <= inboxFramesPerCamera, "what is left afterwards is the inbox's");
    rig.onStateChange = nullptr;
}

/* The memory the process holds, which a test reads before and after something that could grow it. */
std::uint64_t physicalFootprint()
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    require(task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS, "task_info");
    return info.phys_footprint;
}

// The device runs at most the cameras its use allows, whatever the platform would run: a camera beyond them is refused as
// one that conflicts with a running one is, until a running camera stops. The synthetic tablet's platform runs both
// together, so what holds the second back is the device.
void testTheDeviceRunsOnlyTheCamerasItsUseAllows()
{
    {
        Rig rig;
        videoStream(rig, 1, 0, 640, 480, 3);
        videoStream(rig, 2, 1, 640, 480, 3);
        (void)rig.queue(1, 1);
        (void)rig.queue(2, 1);
        require(rig.camera->videoRunning(0) && rig.camera->videoRunning(1), "the ceiling's use lets both cameras run");
    }

    RigOptions options;
    options.use = {1, true, 0};
    Rig rig(options);
    videoStream(rig, 1, 0, 640, 480, 3);
    videoStream(rig, 2, 1, 640, 480, 3);
    (void)rig.queue(1, 1);
    const auto second = rig.queue(2, 1);
    require(rig.camera->videoRunning(0), "the first camera runs");
    require(!rig.camera->videoRunning(1), "a second one is not started, although the platform would run both");
    require(rig.waitFor([&] { return rig.done(second); }) && rig.result(second).base.flags == (blackFlag | interruptedFlag),
            "and its stream gets black pictures flagged interrupted, as when the platform refuses a camera");

    // A still on that camera is refused too, and the camera that runs is unharmed.
    rig.create(3, 1, stillKind);
    require(Rig::type(rig.setParams(3, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    rig.attach(3, 1, rig.params(3).plane_formats[0].plane_size);
    const auto still = rig.queue(3, 1, 9);
    require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "a still on a second camera fails");
    require(rig.camera->videoRunning(0) && rig.camera->stillsTaken() == 0, "without reaching the platform");

    // The first camera stops: the second is admitted at its next try, and the pair never ran together.
    rig.camera->resetFramePeak();
    rig.destroy(1);
    require(!rig.camera->videoRunning(0), "the first camera stopped");
    rig.camera->advanceClock(1'100'000'000);
    require(rig.waitFor([&] { return rig.camera->videoRunning(1); }), "the second camera starts once the first has stopped");
    rig.camera->pump(1, 4);
    rig.step();
    require(rig.camera->framePeak() <= inboxFramesPerCamera + 1, "and the host held one camera's frames at a time");
}

// A camera that stops gives its frames back although the session stays open (for the torch, or a still on its way), so
// a switch to the other camera never holds both cameras' frames: the pool counts one camera's.
void testASwitchWithTheTorchOnHoldsOneCamerasFrames()
{
    RigOptions options;
    options.use = {1, true, 0};
    Rig rig(options);
    videoStream(rig, 1, 0, 640, 480, 3);
    for (std::uint32_t resource = 1; resource <= 3; ++resource) (void)rig.queue(1, resource);
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    rig.camera->resetFramePeak();
    rig.camera->pump(0, 3);
    require(rig.camera->framesAlive() == inboxFramesPerCamera, "three frames of the first camera wait for the worker");
    rig.destroy(1);
    require(rig.camera->sessionsOpen() == 1, "the torch keeps the session open");
    require(!rig.camera->videoRunning(0) && rig.camera->framesAlive() == 0, "and the camera that stopped holds no frame");
    videoStream(rig, 2, 1, 640, 480, 3);
    for (std::uint32_t resource = 1; resource <= 3; ++resource) (void)rig.queue(2, resource);
    rig.camera->pump(1, 4);
    require(rig.camera->framePeak() <= inboxFramesPerCamera + 1, "at no time more than one camera's inbox and the frame arriving");
    rig.step();
    require(rig.camera->framesAlive() == 0, "and the worker frees them");
}

// The captures the device holds at once are bounded by what they cost, not by how many the guest's pipeline has: a
// capture that does not fit waits for the ones with the platform and then goes, in order, and none of them fails.
void testStillsAreAdmittedByTheBytesTheyCost()
{
    Rig rig;
    rig.create(1, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 4032, 3024)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a 12 MP size");
    const auto size = rig.params(1).plane_formats[0].plane_size;
    std::vector<std::uint64_t> requests;
    for (std::uint32_t resource = 1; resource <= 3; ++resource) {
        rig.bump = 16 * page;  // The buffers alias: the test reads none of the pictures.
        rig.attach(1, resource, size);
    }
    for (std::uint32_t resource = 1; resource <= 3; ++resource) requests.push_back(rig.queue(1, resource, 200 + resource));
    require(rig.camera->stillsTaken() == 1, "one 12 MP capture is with the platform: the synthetic source holds 11 bytes a pixel while it makes one");
    require(!rig.done(requests[0]) && !rig.done(requests[1]) && !rig.done(requests[2]), "the others wait, and none has failed");
    for (std::size_t index = 0; index < requests.size(); ++index) {
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(requests[index]) && rig.result(requests[index]).base.flags == 0 &&
                    rig.result(requests[index]).base.timestamp == 201 + index,
                "the capture is delivered");
        require(rig.camera->stillsTaken() == std::min<std::size_t>(index + 2, 3), "and the next one waiting goes");
    }
    require(rig.camera->sessionsOpen() == 0, "the session is released after the last");

    // Pictures of a small size cost little, so the pipeline's depth is what bounds them, as it was.
    Rig small;
    small.create(1, 0, stillKind);
    require(Rig::type(small.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    const auto smallSize = small.params(1).plane_formats[0].plane_size;
    for (std::uint32_t resource = 1; resource <= 4; ++resource) small.attach(1, resource, smallSize);
    for (std::uint32_t resource = 1; resource <= 4; ++resource) (void)small.queue(1, resource, 300 + resource);
    require(small.camera->stillsTaken() == 4, "four small captures are with the platform at once");
}

// A file larger than the guest's JPEG buffer for the size it asked for (a byte and a half a pixel and the Exif gap) fails
// its capture, and nothing of it reaches the guest.
void testAnOversizedStillFailsItsCapture()
{
    Rig rig;
    rig.camera->oversizedStills(true);
    rig.create(1, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    const auto size = rig.params(1).plane_formats[0].plane_size;
    rig.attach(1, 1, size);
    const auto before = rig.bytes(1, 1, size);
    const auto still = rig.queue(1, 1, 400);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "the capture fails");
    require(rig.bytes(1, 1, size) == before, "and the guest's buffer is untouched");
    rig.camera->oversizedStills(false);
    rig.attach(1, 2, size);
    const auto next = rig.queue(1, 2, 401);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(next) && rig.result(next).base.flags == 0, "the next capture is taken");
}

// A photo the host took before keeps working: a file larger than a byte a pixel, the bound it once had, and a file of
// exactly the largest that always fits the guest's buffer once the Exif gap is put in it are taken.
void testAFileWithinTheGuestsBufferIsTaken()
{
    constexpr std::uint32_t width = 1008, height = 756;
    const auto bound = maximumJpegBytes(std::uint64_t(width) * height);
    for (const std::uint64_t padded : {std::uint64_t(width) * height + 1, bound}) {
        Rig rig;
        rig.camera->paddedStills(padded);
        rig.create(1, 0, stillKind);
        require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, width, height)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        const auto size = rig.params(1).plane_formats[0].plane_size;
        require(size == jpegBufferBytes(std::uint64_t(width) * height) && bound + exifGapLength + 4 == size,
                "the buffer the device states holds the bound and the gap");
        rig.attach(1, 1, size);
        const auto still = rig.queue(1, 1, 410);
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(still) && rig.result(still).base.flags == 0, "a file of this size is taken");
    }
}

void stillStream(Rig& rig, std::uint32_t stream, std::uint32_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t buffers)
{
    rig.create(stream, camera, stillKind);
    require(Rig::type(rig.setParams(stream, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, width, height)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    const auto size = rig.params(stream).plane_formats[0].plane_size;
    for (std::uint32_t resource = 1; resource <= buffers; ++resource) {
        rig.bump = 16 * page;  // The buffers alias: the tests read none of the pictures.
        rig.attach(stream, resource, size);
    }
}

// The guest's camera layer answers the application's departure by closing the video stream that was lost and opening
// another, while the application is still away. The platform session that opens for it is held, interrupted, until the
// application is back: one close for the departure and one opening, dark frames that say why meanwhile, a still that fails
// with an error, live frames on the same session after the return, and the same again for a second departure. An
// interruption that is not the application's departure keeps its session and ends no video.
void testASessionOpenedWhileTheApplicationIsAwayIsHeldUntilItIsBack()
{
    Rig rig;
    std::uint32_t video = 1;
    videoStream(rig, video, 0, 640, 480, 6);
    stillStream(rig, 10, 0, 1008, 756, 4);
    rig.queue(video, 1);
    rig.pumpSteps(0, 1);
    unsigned sessions = 1;
    require(rig.camera->sessionsOpened() == sessions && rig.camera->sessionsOpen() == 1, "one session runs");

    for (std::uint32_t round = 1; round <= 2; ++round) {
        // The application leaves: the running video is lost and its session closes.
        rig.camera->interrupt(CameraEventKind::interruptedInBackground);
        rig.step();
        require(rig.camera->sessionsOpen() == 0 && rig.camera->sessionsOpened() == sessions, "one close for the departure");
        const auto lost = rig.queue(video, round == 1 ? 2 : 5);
        require(rig.waitFor([&] { return rig.done(lost); }) && rig.result(lost).base.flags == endedFlags, "the video is lost");

        // The guest closes the lost stream and opens another, with the application still away.
        rig.destroy(video);
        ++video;
        videoStream(rig, video, 0, 640, 480, 6);
        const auto first = rig.queue(video, 1), second = rig.queue(video, 2);
        rig.step();
        require(rig.waitFor([&] { return rig.done(first) && rig.done(second); }), "the new stream is answered");
        require(rig.result(first).base.flags == (blackFlag | interruptedFlag) && rig.result(second).base.flags == (blackFlag | interruptedFlag),
                "with dark frames that say the camera is interrupted, not that the video ended");
        ++sessions;
        require(rig.camera->sessionsOpened() == sessions && rig.camera->sessionsOpen() == 1, "one opening, held while the application is away");
        require(rig.states.back() == CameraState::interrupted, "the guest is told the camera is interrupted");

        // A still taken meanwhile fails, and takes nothing from the session.
        const auto away = rig.queue(10, 2 * round - 1, 70 + round);
        require(rig.waitFor([&] { return rig.done(away); }) && rig.result(away).base.flags == endedFlags, "a still while away fails");
        require(rig.camera->sessionsOpened() == sessions && rig.camera->sessionsOpen() == 1, "and the session is the one held");

        // The application is back: the same session delivers live frames and takes stills.
        rig.camera->resume();
        rig.step();
        const auto bridge = rig.queue(video, 3);
        require(rig.waitFor([&] { return rig.done(bridge); }) && rig.result(bridge).base.flags == (blackFlag | resumingFlag),
                "the stream is bridged until the platform delivers");
        const auto live = rig.queue(video, 4);
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.done(live) && rig.result(live).base.flags == 0 && rig.states.back() == CameraState::streaming, "then it is live");
        require(rig.camera->sessionsOpened() == sessions && rig.camera->sessionsOpen() == 1, "on the one session");
        const auto again = rig.queue(10, 2 * round, 80 + round);
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.waitFor([&] { return rig.done(again); }) && rig.result(again).base.flags == 0, "and a still is taken");
    }

    // A call, or any other interruption that is not the application's departure, keeps the session and ends no video.
    rig.camera->interrupt(CameraEventKind::interrupted);
    rig.step();
    const auto gap = rig.queue(video, 5);
    require(rig.waitFor([&] { return rig.done(gap); }) && rig.result(gap).base.flags == (blackFlag | interruptedFlag), "a dark gap, not an end");
    require(rig.camera->sessionsOpened() == sessions && rig.camera->sessionsOpen() == 1, "the session stays");
    rig.camera->resume();
    rig.step();
    const auto bridge = rig.queue(video, 6);
    require(rig.waitFor([&] { return rig.done(bridge); }) && rig.result(bridge).base.flags == (blackFlag | resumingFlag), "and the video comes back");
    require(rig.camera->sessionsOpened() == sessions, "on the same session");
}

// A change of a running video that the platform refuses leaves the capture it was to replace running (the platform's
// rollback keeps it): the camera still holds it, so a second camera is not admitted under a use of one, and the stream's
// end stops it.
void testARefusedChangeLeavesTheCaptureChargedUntilItStops()
{
    RigOptions options;
    options.use = {1, true, 0};
    Rig rig(options);
    videoStream(rig, 1, 0, 1920, 1080, 1);
    (void)rig.queue(1, 1);
    require(rig.camera->videoRunning(0), "the first camera runs");
    rig.watched->refuseNextStart = true;
    require(Rig::type(rig.setInterval(1, 1, 15)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a change of the rate");
    require(rig.camera->videoRunning(0), "the platform refused it and its capture runs on");
    videoStream(rig, 2, 1, 1920, 1080, 1);
    (void)rig.queue(2, 1);
    require(!rig.camera->videoRunning(1), "the capture the refused change left is still the camera's: no second camera");
    rig.destroy(1);
    require(!rig.camera->videoRunning(0), "and the stream's end stops it, whatever the last change did");
}

// The guest's stabilization wish reaches the platform before the video starts, so the platform programs the new video
// connection before the capture can deliver a frame: a platform whose output queue delivers inside startVideo makes a
// first frame that has the crop the rest have. (Applied after the start, the first picture came unstabilized and the crop
// changed under a guest that already showed it.) This is the device's order; the platform programs what it was told in the
// admission.
void testTheSettingsReachThePlatformBeforeTheVideoStarts()
{
    using Kind = WatchedCamera::PlatformCall::Kind;
    Rig rig;
    rig.watched->afterStart = [&](std::size_t camera) { rig.camera->pump(camera, 1); };
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.attach(1, 2, nv12Size(640, 480));
    rig.queue(1, 1);
    const auto& calls = rig.watched->calls;
    require(calls.size() >= 2 && calls[0].kind == Kind::apply && calls[0].camera == 0 && calls[0].stabilization &&
                calls[1].kind == Kind::startVideo && calls[1].camera == 0,
            "the settings, asking for stabilization (the default), are applied before the video starts");
    require(!rig.watched->framesSeen.empty() && rig.watched->framesSeen.front().applies >= 1 && rig.watched->framesSeen.front().stabilization,
            "the first frame the platform made found the stabilization asked for");
    require(rig.videoModes == std::vector<bool> {true}, "and the device is in video mode");

    // A preview that asks for none (photo mode) is told so before the start: the platform stabilizes nothing it would lose.
    Rig photo;
    photo.create(1, 0, videoKind);
    photo.setVideo(1, 640, 480);
    photo.attach(1, 1, nv12Size(640, 480));
    photo.attach(1, 2, nv12Size(640, 480));
    photo.setOk(1, {{UML_V4L2_CID_IMAGE_STABILIZATION, 0, {}}});
    photo.queue(1, 1);
    require(photo.watched->calls.size() >= 2 && photo.watched->calls[0].kind == Kind::apply && !photo.watched->calls[0].stabilization &&
                photo.watched->calls[1].kind == Kind::startVideo,
            "a guest that does not ask is applied without it before the start");
}

// A lens added to a session that runs already starts its output at the commit of the platform's configuration: its settings
// are applied before its video starts too, not after the first camera's.
void testALensAddedToARunningSessionIsAppliedBeforeItsVideoStarts()
{
    using Kind = WatchedCamera::PlatformCall::Kind;
    Rig rig;
    rig.watched->afterStart = [&](std::size_t camera) { rig.camera->pump(camera, 1); };
    videoStream(rig, 1, 0, 640, 480, 2);
    (void)rig.queue(1, 1);
    require(rig.camera->videoRunning(0), "the first camera runs");
    rig.watched->calls.clear();
    rig.watched->framesSeen.clear();
    videoStream(rig, 2, 1, 640, 480, 2);
    (void)rig.queue(2, 1);
    require(rig.camera->videoRunning(1), "the second camera joins the running session");
    const auto& calls = rig.watched->calls;
    const auto applied = std::find_if(calls.begin(), calls.end(), [](const auto& call) { return call.kind == Kind::apply && call.camera == 1; });
    const auto started = std::find_if(calls.begin(), calls.end(), [](const auto& call) { return call.kind == Kind::startVideo && call.camera == 1; });
    require(applied != calls.end() && started != calls.end() && applied < started && applied->stabilization,
            "its settings, asking for stabilization, are applied before its video starts");
}

// Video mode is told whenever a camera comes to run, whatever brought it: a start the platform refused that succeeds when the
// device tries again, and a rate change that starts a video that never ran. The microphone is held for it either way.
void testVideoModeFollowsAStartThatSucceedsOnRetry()
{
    Rig rig;
    rig.watched->refuseNextStart = true;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.attach(1, 2, nv12Size(640, 480));
    rig.queue(1, 1);
    require(!rig.camera->videoRunning(0) && rig.videoModes.empty(), "a start the platform refused is not video mode");
    rig.camera->advanceClock(1'100'000'000);
    require(rig.waitFor([&] { return rig.camera->videoRunning(0); }), "the camera is tried again a second later");
    require(rig.videoModes == std::vector<bool> {true}, "and the retry that succeeded is video mode, told");
}

void testVideoModeFollowsARateChangeThatStartsAVideoThatNeverRan()
{
    Rig rig;
    rig.watched->refuseNextStart = true;
    rig.create(1, 0, videoKind);
    rig.setVideo(1, 640, 480);
    rig.attach(1, 1, nv12Size(640, 480));
    rig.attach(1, 2, nv12Size(640, 480));
    rig.queue(1, 1);
    require(!rig.camera->videoRunning(0) && rig.videoModes.empty(), "a start the platform refused is not video mode");
    require(Rig::type(rig.setInterval(1, 1, 15)) == VIRTIO_VIDEO_RESP_OK_NODATA, "the guest changes the rate");
    require(rig.camera->videoRunning(0), "which starts the video");
    require(rig.videoModes == std::vector<bool> {true}, "and that is video mode, told");
}

// A platform callback that is in progress when a camera's video stops reaches the device after the stop: it keeps no frame,
// for the camera is no longer counted and the frame holds the capture's buffer.
void testALateFrameOfAStoppedCameraIsNotKept()
{
    RigOptions options;
    options.use = {1, true, 0};
    Rig rig(options);
    videoStream(rig, 1, 0, 640, 480, 3);
    (void)rig.queue(1, 1);
    // The torch keeps the session open when the video stops, as a still on its way would.
    rig.setOk(1, {{UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH, {}}});
    rig.watched->pauseNextFrame();
    std::thread worker([&] { rig.camera->pump(0, 1); });
    rig.watched->awaitFrame();
    rig.destroy(1);
    require(!rig.camera->videoRunning(0) && rig.camera->sessionsOpen() == 1, "the camera stopped, the session stays");
    rig.watched->releaseHeldFrame();
    worker.join();
    require(rig.camera->framesAlive() == 0, "the callback that was in progress kept no frame of the stopped camera");
    // A camera that runs again keeps its frames.
    videoStream(rig, 2, 0, 640, 480, 3);
    (void)rig.queue(2, 1);
    rig.camera->pump(0, 1);
    require(rig.camera->framesAlive() == 1, "the camera that runs again keeps its frame");
    rig.step();
}

// The guest's deadline for a capture runs from its request, whether it waits or has started. A capture the guest gave up
// on is still with the platform: its charge stays until the platform ends it, so no second capture starts on top of it, and
// one the platform never ends is taken as lost with the session.
void testAnExpiredCaptureStaysChargedUntilThePlatformEndsIt()
{
    Rig rig;
    stillStream(rig, 1, 0, 4032, 3024, 4);
    const auto first = rig.queue(1, 1, 101);
    const auto second = rig.queue(1, 2, 102);
    const auto third = rig.queue(1, 3, 103);
    require(rig.camera->stillsTaken() == 1 && rig.watched->holding.size() == 1, "one 12 MP capture is with the platform, two wait");
    rig.camera->advanceClock(10'100'000'000);
    rig.step();
    require(rig.done(first) && (rig.result(first).base.flags & errorFlag), "the capture's buffer fails at its deadline");
    require(rig.done(second) && (rig.result(second).base.flags & errorFlag) && rig.done(third) && (rig.result(third).base.flags & errorFlag),
            "and so do the waiting ones, whose time runs from their request");
    require(rig.camera->stillsTaken() == 1 && rig.watched->holding.size() == 1, "the platform still holds the first capture and gets no other");
    require(rig.camera->sessionsOpen() == 1, "the session stays open for it");
    const auto fourth = rig.queue(1, 4, 104);
    require(rig.camera->stillsTaken() == 1 && !rig.done(fourth), "a new capture waits for what the platform holds");
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.camera->stillsTaken() == 2, "the platform's end of the first capture makes room");
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(fourth) && rig.result(fourth).base.flags == 0, "and the new capture is delivered");

    Rig lost;
    stillStream(lost, 1, 0, 4032, 3024, 2);
    const auto gone = lost.queue(1, 1, 201);
    lost.camera->advanceClock(10'100'000'000);
    lost.step();
    require(lost.done(gone) && lost.camera->sessionsOpen() == 1, "a capture the guest gave up on keeps the session");
    lost.camera->advanceClock(30'100'000'000);
    lost.step();
    require(lost.camera->sessionsOpen() == 0 && lost.watched->holding.empty(), "one the platform never ends is lost with the session");
    const auto again = lost.queue(1, 2, 202);
    require(lost.camera->stillsTaken() == 2, "and the next capture starts at once");
    lost.camera->pump(0, 0);
    lost.step();
    require(lost.done(again) && lost.result(again).base.flags == 0, "and is delivered");
}

// The guest's deadline runs from the request for a capture that waited: when the capture that was ahead of it ends late, the
// one that then starts has only what is left.
void testAWaitingCapturesTimeRunsFromItsRequest()
{
    Rig rig;
    stillStream(rig, 1, 0, 4032, 3024, 2);
    const auto first = rig.queue(1, 1, 101);
    const auto second = rig.queue(1, 2, 102);
    rig.camera->advanceClock(6'000'000'000);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(first) && rig.result(first).base.flags == 0, "the first capture is delivered in its time");
    require(rig.camera->stillsTaken() == 2 && !rig.done(second), "and the capture that waited starts");
    rig.camera->advanceClock(4'100'000'000);
    rig.step();
    require(rig.done(second) && (rig.result(second).base.flags & errorFlag), "its buffer fails ten seconds after its request");
}

// A capture the guest gave up on may end just as the device decides it was lost: the completion reaches the inbox after the
// pump took its batch and before the thirty seconds are over, and expiry then runs. The capture is finished, and the
// session it ended in is healthy: it is not closed, and the preview on it runs on.
void testACompletionQueuedWhenExpiryDecidesIsNotALoss()
{
    Rig rig;
    videoStream(rig, 1, 0, 640, 480, 1);
    (void)rig.queue(1, 1);
    stillStream(rig, 2, 0, 1008, 756, 1);
    const auto gone = rig.queue(2, 1, 101);
    rig.camera->advanceClock(10'100'000'000);
    rig.step();
    require(rig.done(gone) && rig.camera->sessionsOpen() == 1, "the guest gave up on a capture the platform still holds");
    const auto closes = [&] { return std::count(rig.checkpoints.begin(), rig.checkpoints.end(), CameraCheckpoint::sessionClosed); };
    // A recovery event gives a point after the pump took its batch: the platform ends the capture there, 29.9 s on, and
    // the clock passes the thirty seconds before expiry runs.
    rig.camera->post({uml::platform::allCameras, CameraEventKind::interrupted});
    rig.step();
    rig.camera->post({uml::platform::allCameras, CameraEventKind::resumed});
    rig.camera->advanceClock(29'900'000'000);
    bool once = false;
    rig.onStateChange = [&] {
        if (once) return;
        once = true;
        require(rig.camera->videoRunning(0), "the preview runs when the platform ends the capture");
        rig.camera->pump(0, 0);
        rig.camera->advanceClock(200'000'000);
    };
    rig.step();
    rig.onStateChange = nullptr;
    require(once && rig.watched->holding.empty(), "the platform ended the capture before expiry ran");
    require(closes() == 0 && rig.camera->videoRunning(0), "the session was not closed and the preview runs on");
    // The charge went with the capture: a new still starts at once.
    const auto next = rig.queue(2, 1, 102);
    require(rig.camera->stillsTaken() == 2 && !rig.done(next), "and the next capture starts");
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(next) && rig.result(next).base.flags == 0, "and is delivered");
}

// A waiting capture's cost is worked out again when it comes to start, for a control the guest set while it waited changes
// what the capture asks for (a zoom back to 1 adds the colour data of an uncalibrated lens). One that now costs more than the
// device admits at all is refused, as it is at the request, and does not start.
void testAWaitingCaptureThatGrowsBeyondTheWholeBudgetIsRefused()
{
    RigOptions options;
    options.use = {1, true, 0};
    options.workingBytes = 0;
    options.rawStillSize = uml::platform::CameraStillSize {6000, 4000};
    Rig rig(options);
    stillStream(rig, 1, 0, 4032, 3024, 4);
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 2 * 65536, {}}});
    std::vector<std::uint64_t> cookies;
    for (unsigned index = 1; index <= 4; ++index) cookies.push_back(rig.queue(1, index, 100 + index));
    require(rig.camera->stillsTaken() == 3, "three JPEG captures fit the budget at zoom 2 and the fourth waits");
    rig.setOk(1, {{VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 65536, {}}});
    for (unsigned index = 0; index < 3; ++index) {
        rig.camera->pump(0, 0);
        rig.step();
    }
    require(rig.done(cookies[3]) && (rig.result(cookies[3]).base.flags & errorFlag), "the fourth fails once it would cost more than the budget");
    require(rig.camera->stillsTaken() == 3, "and never reaches the platform");
    require(rig.camera->sessionsOpen() == 0, "the session closes with nothing left to do");
}

// A delivered result's payload is given back (the memory, not only its size) before the capture that waited for it
// starts: the charge of a capture covers its payload.
void testAResultsPayloadIsFreedBeforeTheNextCaptureStarts()
{
    Rig rig;
    stillStream(rig, 1, 0, 4032, 3024, 2);
    const auto first = rig.queue(1, 1, 101);
    (void)rig.queue(1, 2, 102);
    require(rig.camera->stillsTaken() == 1, "the second capture waits");
    rig.camera->pump(0, 0);
    require(payload_watch::bytes() > 0, "the delivered result holds its JPEG and RAW");
    rig.step();
    require(rig.done(first) && rig.camera->stillsTaken() == 2, "the first is delivered and the second starts");
    require(rig.watched->payloadWhenStarting == 0, "the delivered payload was freed before the second capture started");
}

// Captures start in the order they were asked for: a small one that would fit does not pass a large one that waits.
void testStillsAreAdmittedInTheOrderTheyAsked()
{
    Rig rig;
    stillStream(rig, 1, 0, 4032, 3024, 2);
    stillStream(rig, 2, 0, 2016, 1512, 1);
    const auto first = rig.queue(1, 1, 101);
    const auto second = rig.queue(1, 2, 102);
    const auto third = rig.queue(2, 1, 103);
    require(rig.camera->stillsTaken() == 1, "the small request does not start ahead of the large one that waits");
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(first) && !rig.done(second) && !rig.done(third) && rig.camera->stillsTaken() == 3,
            "the large one starts first, and the small one with it as it fits");
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(second) && rig.done(third) && rig.result(second).base.flags == 0 && rig.result(third).base.flags == 0, "all are delivered");
}

// A still the host turns half around and encodes holds the encoder's buffers beside the file: the capture of a camera whose
// pictures are presented half around costs the more for them, and the same capture of a camera that is not turned costs no more.
void testATurnedStillHoldsTheEncodersBuffersBesideItsFile()
{
    // The budget is the 12.7 MP ceiling with no working storage and no RAW: 38.1 MB, where one 2016x1512 JPEG capture is 9.1 MB
    // and the encoder's four bytes a pixel add 12.2 MB to a turned one.
    const auto run = [](bool presented, std::uint32_t turnedBytes) {
        RigOptions options;
        options.use = {1, false, 0};
        options.workingBytes = 0;
        options.turnedWorkingBytes = turnedBytes;
        options.frontPresentedAt90 = presented;
        Rig rig(options);
        stillStream(rig, 1, 1, 2016, 1512, 4);
        for (unsigned index = 1; index <= 4; ++index) (void)rig.queue(1, index, 100 + index);
        return rig.camera->stillsTaken();
    };
    require(run(true, 0) == 4, "four 9.1 MB captures fit the budget while the encoder's buffers are not counted");
    require(run(true, 4) == 1, "a turned still's buffers are counted: one capture of 21.3 MB fits, and the rest wait");
    require(run(false, 4) == 4, "a camera that is not turned is charged no more");
}

// A capture that costs more than the device admits at all is refused before it starts, and is not admitted as the first.
void testACaptureBeyondTheWholeBudgetIsRefusedBeforeItStarts()
{
    RigOptions options;
    options.use = {2, false, 0};
    options.workingBytes = 0;
    Rig rig(options);
    // A 12 MP JPEG, and the colour data the first still of an uncalibrated lens also asks for: more than a JPEG of that size.
    stillStream(rig, 1, 0, 4032, 3024, 1);
    const auto still = rig.queue(1, 1, 301);
    require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "the capture fails");
    require(rig.camera->stillsTaken() == 0 && rig.camera->sessionsOpen() == 0, "without reaching the platform");
}

// Black pictures hold nothing of their own, however many streams are black and however large their pictures are: twenty
// 1080p streams of one camera used to keep twenty pictures (62 MB) the reserve did not count.
void testBlackPicturesHoldNothingOfTheirOwn()
{
    for (const bool blockedSession : {false, true}) {
        RigOptions options;
        options.authorized = false;
        options.mode = blockedSession ? CameraMode::blocked : CameraMode::live;
        Rig rig(options);
        std::vector<std::uint64_t> requests;
        for (std::uint32_t stream = 1; stream <= 20; ++stream) {
            rig.bump = 16 * page;  // Guest buffers may alias: only the host's would not.
            videoStream(rig, stream, 0, 1920, 1080, 1);
        }
        const auto before = physicalFootprint();
        for (std::uint32_t stream = 1; stream <= 20; ++stream) requests.push_back(rig.queue(stream, 1));
        require(rig.waitFor([&] { return std::all_of(requests.begin(), requests.end(), [&](auto id) { return rig.done(id); }); }),
                "every stream gets its black picture");
        const auto growth = physicalFootprint() > before ? physicalFootprint() - before : 0;
        for (const auto id : requests) require((rig.result(id).base.flags & blackFlag) != 0, "black");
        require(growth < (8u << 20), "no picture of a stream stays behind: twenty 1080p pictures are 62 MB");
        // And the picture is black: luma 0, neutral chroma, in a buffer a chunk of the device's own does not cover.
        const auto picture = rig.bytes(20, 1, nv12Size(1920, 1080));
        require(std::all_of(picture.begin(), picture.begin() + 1920 * 1080, [](auto byte) { return byte == 0; }), "luma");
        require(std::all_of(picture.begin() + 1920 * 1080, picture.end(), [](auto byte) { return byte == 128; }), "chroma");
    }
}

/* R5 (recheck5): the interruption ends and the system has moved the camera out of its format meanwhile. The platform
 * session puts it back and only then says it is back, so the guest is told resumed after the repair, and the frames the
 * capture produced meanwhile (outside the anchor) never reach it: its buffers get black ones, flagged interrupted. */
void testARecoveryRepairsTheCameraBeforeItIsToldItIsBack()
{
    Rig rig;
    EventQueue events(rig, 8);
    videoStream(rig, 1, 0, 640, 480, 6);
    const auto live = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(live) && rig.result(live).base.flags == 0, "frames flow");
    rig.camera->interrupt();
    rig.step();
    rig.camera->moveOutOfAnchor(true);
    // The capture goes on in the wrong format while the cameras are interrupted.
    const auto outside = rig.queue(1, 2);
    rig.camera->pump(0, 2);
    rig.step();
    require(rig.camera->framesOutsideAnchor() == 2, "the platform produced frames outside the anchor");
    require(rig.waitFor([&] { return rig.done(outside); }) && rig.result(outside).base.flags == (blackFlag | interruptedFlag),
            "the guest's buffer got a black one, not the frame");
    require(events.arrived().size() == 2, "the guest has only been told of the interruption");
    rig.camera->resume();
    rig.step();
    require(rig.camera->repairs() == 1, "the camera was put back");
    require(events.arrived().size() == 4 && events.arrived()[2].first == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED,
            "and then the guest is told it is back");
    const auto after = rig.queue(1, 3);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(after) && rig.result(after).base.flags == 0 && rig.camera->framesOutsideAnchor() == 2,
            "its next frame is live, from the anchor");
}

/* A repair the device refuses is the session's failure: the guest is never told it is back, the session is closed,
 * and a new one opened a second later starts every camera in its anchor. */
void testARecoveryThatFailsIsAFailureAndNeverResumes()
{
    Rig rig;
    EventQueue events(rig, 8);
    videoStream(rig, 1, 0, 640, 480, 6);
    const auto live = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(live) && rig.result(live).base.flags == 0, "frames flow");
    rig.camera->interrupt();
    rig.step();
    rig.camera->moveOutOfAnchor(true);
    rig.camera->refuseRepair(true);
    rig.camera->resume();
    rig.step();
    require(rig.camera->repairs() == 0, "the device refused the repair");
    require(rig.camera->sessionsOpen() == 0, "the failed session is closed");
    // The camera that has the stream (camera 0); the other has none and is told its interruption ended with the session.
    const auto resumed = [&] {
        const auto all = events.arrived();
        return std::count(all.begin(), all.end(), std::make_pair(std::uint32_t(VIRTIO_VIDEO_EVENT_CAMERA_RESUMED), 0u));
    };
    require(resumed() == 0, "the guest is never told it is back");
    const auto black = rig.queue(1, 2);
    require(rig.waitFor([&] { return rig.done(black); }) && rig.result(black).base.flags == (blackFlag | interruptedFlag),
            "black frames bridge the failure");
    // The retry opens a session whose cameras start in their anchors.
    rig.camera->refuseRepair(false);
    rig.camera->advanceClock(1'200'000'000);
    require(rig.waitFor([&] { return rig.camera->videoRunning(0); }), "a new session is opened");
    require(resumed() == 0, "and the guest is still not told it is back before the capture delivers");
    const auto fresh = rig.queue(1, 3);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(fresh) && rig.result(fresh).base.flags == 0 && rig.camera->framesOutsideAnchor() == 0,
            "a live frame, from the anchor");
    require(resumed() == 1, "now it is back");
}

/* R5 (recheck6): the worker had not drained its inbox when the recovery completed, so one batch holds the interruption,
 * a frame the capture made outside the anchor, and the resumed event. The events are handled first and clear the
 * interruption, so a frame that was only stamped by the state of the camera when the pump took it would be delivered
 * live. It was captured before the repair: it is never delivered, and the next frame, made in the anchor, is. */
void testAFrameCapturedBeforeTheRepairIsNeverDeliveredAfterIt()
{
    Rig rig;
    EventQueue events(rig, 8);
    videoStream(rig, 1, 0, 640, 480, 6);
    const auto live = rig.queue(1, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(live) && rig.result(live).base.flags == 0, "frames flow");
    const auto waiting = rig.queue(1, 2);
    rig.camera->interrupt();
    rig.camera->moveOutOfAnchor(true);
    rig.camera->pump(0, 1);
    require(rig.camera->framesOutsideAnchor() == 1, "the capture made a frame outside the anchor, which waits in the inbox");
    rig.camera->resume();
    require(rig.camera->repairs() == 1, "the camera is put back before the worker has run");
    rig.step();
    const bool done = rig.done(waiting);
    require(!done || rig.result(waiting).base.flags != 0, "the frame from before the repair is not delivered as a live one");
    if (!done) {
        rig.camera->pump(0, 1);
        rig.step();
        require(rig.done(waiting) && rig.result(waiting).base.flags == 0, "the next frame is live");
        const auto marker = decodeChartMarker(rig.bytes(1, 2, nv12Size(640, 480)).data(), 640, 640, 480);
        require(marker.valid && marker.counter == 3, "and it is the frame made after the repair, not the one before it");
    }
    require(events.arrived().size() >= 4 && events.arrived()[2].first == VIRTIO_VIDEO_EVENT_CAMERA_RESUMED,
            "the guest is told the camera is back");
}

/* R5 (recheck6): a capture that completes successfully on the platform's side while the camera is interrupted, or that
 * was asked for before an interruption that has since been handled, is not delivered as a photo: it fails as a capture
 * that went wrong does (flagged interrupted while the interruption lasts), for the guest to ask again. */
void testAStillThatSpansAnInterruptionFails()
{
    for (const bool resumed : {false, true}) {
        Rig rig;
        rig.create(1, 0, stillKind);
        require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
        const auto still = rig.queue(1, 1, 555);
        require(!rig.done(still) && rig.camera->sessionsOpened() == 1, "the capture is asked for");
        rig.camera->pump(0, 0);  // the platform completes it with a good picture
        rig.camera->interrupt();
        if (resumed) rig.camera->resume();
        rig.step();
        require(rig.done(still), "the buffer completes");
        const auto flags = rig.result(still).base.flags;
        require((flags & errorFlag) != 0 && rig.result(still).base.data_sizes[0] == 0, "with an error, and no picture");
        require(((flags & interruptedFlag) != 0) == !resumed, "flagged interrupted while the interruption lasts, not after it");
    }
    // The interruption event waits in the inbox when the capture is asked for, so the camera is not yet known to be
    // interrupted: the capture that the platform then completes is handled after the event, and fails with the camera.
    {
        Rig early;
        early.create(1, 0, stillKind);
        require(Rig::type(early.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
        early.attach(1, 1, early.params(1).plane_formats[0].plane_size);
        // A stream that runs keeps the session open, so that the event reaches it.
        videoStream(early, 2, 0, 640, 480, 2);
        const auto running = early.queue(2, 1);
        early.camera->pump(0, 1);
        early.step();
        require(early.done(running) && early.result(running).base.flags == 0, "the camera streams");
        early.camera->post({uml::platform::allCameras, CameraEventKind::interrupted});
        const auto still = early.queue(1, 1, 557);
        early.camera->pump(0, 0);
        early.step();
        require(early.done(still) && (early.result(still).base.flags & (errorFlag | interruptedFlag)) == (errorFlag | interruptedFlag) &&
                    early.result(still).base.data_sizes[0] == 0,
                "a capture asked for as the interruption begins fails with it");
    }
    // A capture asked for after the recovery is delivered as before.
    Rig rig;
    rig.create(1, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
    videoStream(rig, 2, 0, 640, 480, 2);
    const auto running = rig.queue(2, 1);
    rig.camera->pump(0, 1);
    rig.step();
    require(rig.done(running), "the camera streams, which keeps the session open");
    rig.camera->interrupt();
    rig.camera->resume();
    rig.step();
    const auto still = rig.queue(1, 1, 556);
    rig.camera->pump(0, 0);
    rig.step();
    require(rig.done(still) && rig.result(still).base.flags == 0 && rig.result(still).base.data_sizes[0] > 4000,
            "a capture asked for after the recovery is delivered");
}

// The picture of a flat grey scene, as a platform's JPEG file of that size.
std::vector<std::uint8_t> flatJpeg(std::uint32_t width, std::uint32_t height)
{
    const std::vector<std::uint8_t> luma(std::size_t(width) * height, 128);
    const std::vector<std::uint8_t> chroma(std::size_t(width) * (height / 2), 128);
    return encodeSyntheticJpeg(luma.data(), width, chroma.data(), width, width, height, 90);
}

// The front camera of the owner's newest phone states orientation 0, asks for 2016x1512 stills, and its photo output
// delivers something the device refuses; the cases are the ones the investigation's probe tried. Each ends in an error
// buffer without bytes, as it always did, and in one report of why that carries only numbers and fixed words. A still
// that is right reports nothing.
void testAStillThatFailsSaysWhy()
{
    constexpr std::uint32_t front = 1;
    struct Case {
        const char* what;
        std::function<void(CameraStillResult&)> alter;
        CameraStillStage stage;
        std::uint32_t encodedWidth, encodedHeight;
        CameraStillFailure platform;
    };
    CameraStillFailure resolvedTransposed;
    resolvedTransposed.stage = CameraStillStage::resolvedSize;
    resolvedTransposed.resolvedWidth = 1512;
    resolvedTransposed.resolvedHeight = 2016;
    resolvedTransposed.compensationSupported = CameraFact::yes;
    resolvedTransposed.compensationEnabled = CameraFact::no;
    CameraStillFailure frameworkError;
    frameworkError.stage = CameraStillStage::frameworkError;
    frameworkError.resolvedWidth = 2016;
    frameworkError.resolvedHeight = 1512;
    frameworkError.compensationSupported = CameraFact::yes;
    frameworkError.compensationEnabled = CameraFact::no;
    frameworkError.errorDomain = CameraErrorDomain::avFoundation;
    frameworkError.errorCode = -11800;
    CameraStillFailure missing;
    missing.stage = CameraStillStage::missingData;
    missing.compensationSupported = CameraFact::yes;
    missing.compensationEnabled = CameraFact::no;
    const std::vector<Case> cases {
        {"the photo is transposed (1512x2016)",
         [](CameraStillResult& result) {
             result.jpeg = flatJpeg(1512, 2016);
             result.failure.compensationSupported = CameraFact::yes;
             result.failure.compensationEnabled = CameraFact::no;
         },
         CameraStillStage::encodedSize, 1512, 2016, missing},
        {"the photo is square (1512x1512)", [](CameraStillResult& result) { result.jpeg = flatJpeg(1512, 1512); },
         CameraStillStage::encodedSize, 1512, 1512, {}},
        {"the file has no frame header", [](CameraStillResult& result) { result.jpeg = {0xff, 0xd8, 0xff, 0xd9}; },
         CameraStillStage::encodedSize, 0, 0, {}},
        {"the photo resolved transposed",
         [&](CameraStillResult& result) {
             result.ok = false;
             result.jpeg.clear();
             result.failure = resolvedTransposed;
         },
         CameraStillStage::resolvedSize, 0, 0, resolvedTransposed},
        {"the framework reports an error",
         [&](CameraStillResult& result) {
             result.ok = false;
             result.jpeg.clear();
             result.failure = frameworkError;
         },
         CameraStillStage::frameworkError, 0, 0, frameworkError},
        {"the photo has no data",
         [&](CameraStillResult& result) {
             result.ok = false;
             result.jpeg.clear();
             result.failure = missing;
         },
         CameraStillStage::missingData, 0, 0, missing},
        {"the platform gives no reason",
         [](CameraStillResult& result) {
             result.ok = false;
             result.jpeg.clear();
         },
         CameraStillStage::unknown, 0, 0, {}},
    };
    for (const auto& test : cases) {
        Rig rig;
        rig.watched->alterStill = test.alter;
        stillStream(rig, 1, front, 2016, 1512, 1);
        const auto size = rig.params(1).plane_formats[0].plane_size;
        const auto before = rig.bytes(1, 1, size);
        const auto still = rig.queue(1, 1, 61);
        rig.camera->pump(front, 0);
        rig.step();
        require(rig.done(still) && rig.result(still).base.flags == errorFlag && rig.result(still).base.data_sizes[0] == 0 &&
                    rig.bytes(1, 1, size) == before,
                test.what);
        require(rig.stillFailures.size() == 1, test.what);
        const auto& failure = rig.stillFailures.front();
        require(failure.stage == test.stage && failure.camera == front && failure.position == CameraPosition::front &&
                    failure.askedWidth == 2016 && failure.askedHeight == 1512 && failure.encodedWidth == test.encodedWidth &&
                    failure.encodedHeight == test.encodedHeight && failure.resolvedWidth == test.platform.resolvedWidth &&
                    failure.resolvedHeight == test.platform.resolvedHeight &&
                    failure.compensationSupported == test.platform.compensationSupported &&
                    failure.compensationEnabled == test.platform.compensationEnabled &&
                    failure.errorDomain == test.platform.errorDomain && failure.errorCode == test.platform.errorCode,
                test.what);
    }

    // A file larger than the host takes is a failure with its own stage.
    {
        Rig rig;
        rig.camera->oversizedStills(true);
        stillStream(rig, 1, front, 2016, 1512, 1);
        const auto still = rig.queue(1, 1, 62);
        rig.camera->pump(front, 0);
        rig.step();
        require(rig.done(still) && rig.result(still).base.flags == errorFlag, "an oversized file fails");
        require(rig.stillFailures.size() == 1 && rig.stillFailures[0].stage == CameraStillStage::fileSize &&
                    rig.stillFailures[0].askedWidth == 2016,
                "an oversized file is reported as one");
    }
    // A still that is right is delivered untouched and reports nothing, whatever the platform said of a part of it.
    {
        Rig rig;
        rig.watched->alterStill = [](CameraStillResult& result) {
            result.failure.stage = CameraStillStage::frameworkError;  // a part that failed alone
            result.failure.errorDomain = CameraErrorDomain::avFoundation;
        };
        stillStream(rig, 1, front, 2016, 1512, 2);
        const auto still = rig.queue(1, 1, 63);
        rig.camera->pump(front, 0);
        rig.step();
        require(rig.done(still) && rig.result(still).base.flags == 0 && rig.result(still).base.data_sizes[0] > 4000,
                "a still of the size asked for is delivered");
        require(rig.stillFailures.empty(), "and says nothing");
    }
    // A capture that is refused before it reaches the platform says so.
    {
        RigOptions options;
        options.use = {2, false, 0};
        options.workingBytes = 0;
        Rig rig(options);
        stillStream(rig, 1, 0, 4032, 3024, 1);
        const auto still = rig.queue(1, 1, 64);
        require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "the capture is refused");
        require(rig.stillFailures.size() == 1 && rig.stillFailures[0].stage == CameraStillStage::refused &&
                    rig.stillFailures[0].camera == 0 && rig.stillFailures[0].position == CameraPosition::back &&
                    rig.stillFailures[0].askedWidth == 4032 && rig.stillFailures[0].askedHeight == 3024,
                "a refusal is reported with the size asked for");
    }
    // A refusal says what the photo output last read of its compensation and when it was read, as the platform's notes
    // told it, so a bug report names the reading that refused the still; before any note it says nothing of it.
    {
        Rig rig;
        stillStream(rig, 1, 0, 1008, 756, 3);
        rig.watched->refuseStills = true;
        const auto before = rig.queue(1, 1, 66);
        require(rig.waitFor([&] { return rig.done(before); }), "the refused still ends");
        require(rig.done(before) && rig.stillFailures.size() == 1 &&
                    rig.stillFailures[0].stage == CameraStillStage::refused &&
                    rig.stillFailures[0].compensationSupported == CameraFact::unknown &&
                    rig.stillFailures[0].compensationReading == CameraReading::unknown,
                "a refusal before any reading says nothing of the compensation");
        rig.watched->refuseStills = false;
        (void)rig.queue(1, 2, 67);
        require(rig.watched->sink != nullptr, "the session is open");
        for (std::size_t platform = 0; platform < rig.camera->cameras().size(); ++platform) {
            CameraPresentation note;
            note.photoKnown = true;
            note.compensationSupported = CameraFact::yes;
            note.compensationEnabled = CameraFact::no;
            note.compensationReading = CameraReading::afterCommit;
            rig.watched->sink->cameraPresentation(platform, note);
        }
        rig.watched->refuseStills = true;
        const auto later = rig.queue(1, 3, 68);
        require(rig.waitFor([&] { return rig.done(later); }), "the later refused still ends");
        require(rig.done(later) && rig.stillFailures.size() == 2 &&
                    rig.stillFailures[1].stage == CameraStillStage::refused &&
                    rig.stillFailures[1].compensationSupported == CameraFact::yes &&
                    rig.stillFailures[1].compensationEnabled == CameraFact::no &&
                    rig.stillFailures[1].compensationReading == CameraReading::afterCommit,
                "a refusal after a reading carries it and says when it was taken");
    }
    // A capture the platform does not end in time says so; an interruption that ends one is not a fault of the capture.
    {
        Rig rig;
        stillStream(rig, 1, front, 1008, 756, 1);
        const auto still = rig.queue(1, 1, 65);
        rig.camera->advanceClock(10'600'000'000);
        rig.step();
        require(rig.done(still) && rig.result(still).base.flags == errorFlag, "a capture that takes too long fails");
        require(rig.stillFailures.size() == 1 && rig.stillFailures[0].stage == CameraStillStage::timeout &&
                    rig.stillFailures[0].askedWidth == 1008,
                "a timeout is reported");
    }
    {
        Rig rig;
        stillStream(rig, 1, 0, 1008, 756, 1);
        const auto still = rig.queue(1, 1, 66);
        rig.camera->pump(0, 0);
        rig.camera->interrupt();
        rig.step();
        require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "an interrupted capture fails");
        require(rig.stillFailures.empty(), "an interruption is not reported as a failure of the capture");
    }
}

/* The colour data of the first camera's lens, learned by one still of its own, so that a rig that is given the store has a
 * camera with RAW stills. */
std::shared_ptr<CalibrationStore> learnedColour()
{
    auto store = std::make_shared<CalibrationStore>();
    RigOptions options;
    options.store = store;
    Rig learner(options);
    learner.create(1, 0, stillKind);
    require(Rig::type(learner.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "a size");
    learner.attach(1, 1, learner.params(1).plane_formats[0].plane_size);
    learner.queue(1, 1);
    learner.camera->pump(0, 0);
    learner.step();
    return store;
}

/* A RAW and a JPEG of the first camera asked for as one capture (streams 2 and 1, cookie 9). */
struct RawAndJpeg {
    std::uint64_t raw, jpeg;
};
RawAndJpeg askForRawAndJpeg(Rig& rig)
{
    rig.create(1, 0, stillKind);
    rig.create(2, 0, stillKind);
    require(Rig::type(rig.setParams(1, VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "JPEG");
    require(Rig::type(rig.setParams(2, VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB, 1008, 756)) == VIRTIO_VIDEO_RESP_OK_NODATA, "RAW");
    rig.attach(1, 1, rig.params(1).plane_formats[0].plane_size);
    rig.attach(2, 1, rig.params(2).plane_formats[0].plane_size);
    const auto raw = rig.queue(2, 1, 9, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
    const auto jpeg = rig.queue(1, 1, 9);
    return {raw, jpeg};
}

/* Which captures `onStillFailure` is told of: each one the guest asked for that did not end in a picture, once. A whole
 * capture that waited for the host's budget until its time ran out is one, as the capture the platform held is; one that an
 * interruption of its camera ended is not, whether its time ran out or the platform ended it; a group the guest did not
 * complete is not a capture; and one that delivered its JPEG is not, whatever its RAW did. One whose JPEG did not arrive is,
 * whether its RAW did or not. */
void testOnlyACaptureThatEndedInNoPictureIsReported()
{
    constexpr std::uint32_t front = 1;
    // Three whole 12 MP captures: the platform holds one and the host's budget keeps two waiting. Each is reported when its
    // time runs out, and its buffer fails as it does.
    {
        Rig rig;
        stillStream(rig, 1, 0, 4032, 3024, 3);
        const auto first = rig.queue(1, 1, 101);
        const auto second = rig.queue(1, 2, 102);
        const auto third = rig.queue(1, 3, 103);
        require(rig.camera->stillsTaken() == 1, "one capture is with the platform, two wait");
        rig.camera->advanceClock(10'100'000'000);
        rig.step();
        require(rig.done(first) && rig.done(second) && rig.done(third), "the three complete at their deadline");
        require((rig.result(first).base.flags & errorFlag) && (rig.result(second).base.flags & errorFlag) &&
                    (rig.result(third).base.flags & errorFlag),
                "and their buffers fail");
        require(rig.camera->stillsTaken() == 1, "the waiting ones never reached the platform");
        require(rig.stillFailures.size() == 3, "each of the three is reported");
        for (const auto& failure : rig.stillFailures)
            require(failure.stage == CameraStillStage::timeout && failure.camera == 0 && failure.position == CameraPosition::back &&
                        failure.askedWidth == 4032 && failure.askedHeight == 3024,
                    "as a timeout of the camera, with the size asked for");
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.stillFailures.size() == 3, "and the platform's late end of the first reports nothing more");
    }
    // A group the guest did not complete is not a capture.
    {
        Rig rig;
        stillStream(rig, 1, 0, 1008, 756, 1);
        const auto unfinished = rig.queue(1, 1, 68, VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE);
        rig.camera->advanceClock(2'100'000'000);
        rig.step();
        require(rig.done(unfinished) && (rig.result(unfinished).base.flags & errorFlag), "an unfinished group fails");
        require(rig.stillFailures.empty(), "and is not reported");
    }
    // A capture whose camera was interrupted is not reported when its time runs out, while the camera is interrupted or
    // after it recovered, and neither when the platform ends it late.
    for (const bool resume : {false, true}) {
        Rig rig;
        stillStream(rig, 1, front, 1008, 756, 1);
        const auto still = rig.queue(1, 1, 67);
        rig.camera->interrupt();
        rig.step();
        if (resume) {
            rig.camera->resume();
            rig.step();
        }
        require(!rig.done(still), "the capture is held until its time runs out");
        rig.camera->advanceClock(10'600'000'000);
        rig.step();
        require(rig.done(still) && (rig.result(still).base.flags & errorFlag), "its buffer fails at its deadline");
        require(rig.stillFailures.empty(), resume ? "a recovery that spans it is not a failure of the capture"
                                                  : "an interruption is not a failure of the capture");
        rig.camera->pump(front, 0);
        rig.step();
        require(rig.stillFailures.empty(), "and the platform's late end of it reports nothing");
    }
    // The same for a whole capture that waits for room when its camera is interrupted.
    {
        Rig rig;
        stillStream(rig, 1, 0, 4032, 3024, 2);
        const auto first = rig.queue(1, 1, 101);
        const auto second = rig.queue(1, 2, 102);
        require(rig.camera->stillsTaken() == 1, "one capture is with the platform, one waits");
        rig.camera->interrupt();
        rig.step();
        rig.camera->advanceClock(10'100'000'000);
        rig.step();
        require(rig.done(first) && rig.done(second) && (rig.result(second).base.flags & errorFlag), "both fail at their deadline");
        require(rig.stillFailures.empty(), "and an interruption that is on says they are not reported");
    }
    // A RAW and a JPEG: a RAW that fails alone is not a failure of a capture that delivered its JPEG.
    {
        auto store = learnedColour();
        RigOptions options;
        options.store = store;
        Rig rig(options);
        rig.camera->failRaw(true);
        const auto group = askForRawAndJpeg(rig);
        rig.camera->pump(0, 0);
        rig.step();
        require(rig.done(group.raw) && rig.done(group.jpeg), "the group completes");
        require(rig.result(group.jpeg).base.flags == 0 && rig.result(group.jpeg).base.data_sizes[0] > 4000, "the JPEG is delivered");
        require((rig.result(group.raw).base.flags & errorFlag) != 0, "and the RAW buffer fails alone");
        require(rig.stillFailures.empty(), "a capture that delivered the JPEG reports nothing");
    }
    {
        // The other way round: the photo did not appear, whatever the RAW did.
        auto store = learnedColour();
        RigOptions options;
        options.store = store;
        Rig rig(options);
        rig.watched->alterStill = [](CameraStillResult& result) { result.jpeg.clear(); };
        const auto group = askForRawAndJpeg(rig);
        rig.camera->pump(0, 0);
        rig.step();
        require((rig.result(group.jpeg).base.flags & errorFlag) != 0 && rig.result(group.raw).base.flags == 0,
                "the JPEG buffer fails and the RAW is delivered");
        require(rig.stillFailures.size() == 1 && rig.stillFailures[0].stage == CameraStillStage::missingData &&
                    rig.stillFailures[0].camera == 0 && rig.stillFailures[0].askedWidth == 1008 &&
                    rig.stillFailures[0].askedHeight == 756,
                "a capture whose JPEG did not arrive is reported with the JPEG's size, though the RAW did");
    }
    // Both parts failing is a capture that ended in no picture, reported once.
    {
        auto store = learnedColour();
        RigOptions options;
        options.store = store;
        Rig rig(options);
        rig.camera->failRaw(true);
        rig.watched->alterStill = [](CameraStillResult& result) { result.jpeg.clear(); };
        const auto group = askForRawAndJpeg(rig);
        rig.camera->pump(0, 0);
        rig.step();
        require((rig.result(group.jpeg).base.flags & errorFlag) != 0 && (rig.result(group.raw).base.flags & errorFlag) != 0,
                "both buffers fail");
        require(rig.stillFailures.size() == 1 && rig.stillFailures[0].stage == CameraStillStage::missingData &&
                    rig.stillFailures[0].askedWidth == 1008 && rig.stillFailures[0].askedHeight == 756,
                "and the capture is reported once");
    }
}

/* What the platform says of how it presents a camera goes to the owner under the guest's number of the camera, in the
 * order it came, with nothing else changed; a camera the guest does not see is not told, and an owner that fails does
 * not fail the platform. */
void testThePlatformsPresentationReachesTheOwnerUnderTheGuestsNumber()
{
    Rig rig;
    stillStream(rig, 1, 0, 1008, 756, 1);
    (void)rig.queue(1, 1, 61);
    require(rig.watched->sink != nullptr, "the session is open");
    const auto infos = rig.camera->cameras();
    require(infos.size() >= 2, "the profile has a back and a front camera");
    for (std::size_t platform = 0; platform < infos.size(); ++platform) {
        CameraPresentation note;
        note.camera = 99;
        note.position = infos[platform].position;
        note.stated = std::uint16_t(100 + platform);
        note.refusals = 7;
        rig.watched->sink->cameraPresentation(platform, note);
    }
    require(rig.presentations.size() == infos.size(), "every camera the guest sees is told once");
    std::set<std::uint32_t> numbers;
    for (std::size_t told = 0; told < rig.presentations.size(); ++told) {
        const auto& note = rig.presentations[told];
        require(note.camera < infos.size() && numbers.insert(note.camera).second, "each under its own number of the guest's");
        require(note.platformStated == 100 + told && note.position == infos[told].position && note.refusals == 7,
                "and as the platform said it");
    }
    rig.presentations.clear();
    CameraPresentation lost;
    rig.watched->sink->cameraPresentation(infos.size() + 5, lost);
    require(rig.presentations.empty(), "a camera the guest does not see is not told");
    rig.presentationThrows = true;
    rig.watched->sink->cameraPresentation(0, lost);
    require(rig.presentations.size() == 1, "an owner that fails is told, and the platform goes on");
}

/* The orientation the record states is the one of the guest's description, which the application turns for the layout a
 * tablet boots in; the platform's own is the second number. The tablet's values as measured: the platform states 90 for
 * both cameras and the guest is told 180 for the front camera and 0 for the back. */
void testThePresentationStatesWhatTheGuestIsTold()
{
    constexpr std::uint32_t bootTurn = 270;
    RigOptions options;
    options.sensorOrientation = [](const CameraInfo& info) { return turnedSensorOrientation(info.position, 90, bootTurn); };
    Rig rig(options);
    stillStream(rig, 1, 0, 1008, 756, 1);
    (void)rig.queue(1, 1, 61);
    require(rig.watched->sink != nullptr, "the session is open");
    const auto infos = rig.camera->cameras();
    bool back = false, front = false;
    for (std::size_t platform = 0; platform < infos.size(); ++platform) {
        CameraPresentation note;
        note.position = infos[platform].position;
        note.stated = 90;
        rig.watched->sink->cameraPresentation(platform, note);
    }
    require(rig.presentations.size() == infos.size(), "every camera is told");
    for (std::size_t told = 0; told < rig.presentations.size(); ++told) {
        const auto& note = rig.presentations[told];
        require(note.platformStated == 90, "the platform's own orientation is the second number");
        if (note.position == CameraPosition::back) {
            require(note.stated == 0, "the guest is told 0 for the back camera");
            back = true;
        } else if (note.position == CameraPosition::front) {
            require(note.stated == 180, "and 180 for the front camera");
            front = true;
        }
    }
    require(back && front, "the profile has both");
    // Without a statement of the application's the guest is told the platform's own, and the two numbers agree.
    Rig plain;
    stillStream(plain, 1, 0, 1008, 756, 1);
    (void)plain.queue(1, 1, 61);
    require(plain.watched->sink != nullptr, "the session is open");
    CameraPresentation note;
    note.position = plain.camera->cameras()[0].position;
    note.stated = std::uint16_t(plain.camera->cameras()[0].sensorOrientation);
    plain.watched->sink->cameraPresentation(0, note);
    require(plain.presentations.size() == 1 && plain.presentations[0].stated == note.stated &&
                plain.presentations[0].platformStated == note.stated,
            "the platform's value is what the guest is told where nothing turns it");
}

void testService()
{
    SyntheticCameraOptions options;
    options.profile = SyntheticCameraProfile::laptop;
    std::shared_ptr<Camera> camera = makeSyntheticCamera(options);
    CameraDeviceOptions device;
    device.mode = CameraMode::blocked;
    auto service = makeInProcessCameraService(camera, device);
    require(service != nullptr && !service->takeConnectionName().empty(), "the service names its connection");
    service->stop();
    service->stop();
    require(std::dynamic_pointer_cast<SyntheticCamera>(camera)->sessionsOpened() == 0, "a blocked service opens no session");
}

} // namespace

int main()
{
    testThePlatformsPresentationReachesTheOwnerUnderTheGuestsNumber();
    testThePresentationStatesWhatTheGuestIsTold();
    testVideoModeIsWhatTheGuestAsksStabilizationFor();
    testVideoModeIsNeverToldForABlockedSessionOrAfterTheStreamStops();
    testTheSettingsReachThePlatformBeforeTheVideoStarts();
    testALensAddedToARunningSessionIsAppliedBeforeItsVideoStarts();
    testVideoModeFollowsAStartThatSucceedsOnRetry();
    testVideoModeFollowsARateChangeThatStartsAVideoThatNeverRan();
    testHandshake();
    testStreamsAndParameters();
    testVideoAndMetadata();
    testDecimation();
    testPipeline();
    testAFrontCameraWithAHalfTurnIsWrittenTurned();
    testBlockedNeverOpensASession();
    testBlockedSaysWhenTheGuestOpensACamera();
    testLiveNeverReportsBlockedUse();
    testInterruption();
    testTheApplicationLeavingTheForegroundEndsTheVideoOfACamera();
    testTheLossDoesNotDependOnTheStabilization();
    testOnlyTheBackgroundEndsAVideoAndOnlyOfACameraWithAVideoStream();
    testTheDepartureEndsTheRunningVideoAndWaitsForTheGuestToStopIt();
    testTheDepartureWithNoRunningVideoEndsAndWaitsForNothing();
    testTheDepartureEndsAtItsDeadline();
    testTheDepartureWaitsForALossTheNoticeBegan();
    testTheDepartureEndsWithTheGuestOrTheDevice();
    testTheInterruptedRecordGoesToTheGuestBeforeTheFirstFailedBuffer();
    testTheFailedBuffersWaitForTheRecordForAtMostTheBound();
    testNoMetaStreamNoWaitForTheRecord();
    testASessionOpenedWhileTheApplicationIsAwayIsHeldUntilItIsBack();
    testAccessDenied();
    testStillsAndCalibration();
    testControlsOnStreams();
    testTorchHoldsTheSession();
    testTheLightTheGuestAsksForIsTold();
    testResetAndDestroy();
    testFrontCameraChoice();
    testCamerasJoin();
    testCalibrationRawIsACourtesy();
    testAHostThatCannotTakeABayerStillPublishesNoRaw();
    testAStillOfAnotherSizeFails();
    testInterruptionEndsWithTheSession();
    testSystemPressure();
    testResetAndTheDriverRecreates();
    testEventQueueRetries();
    testSuspendResetsNothing();
    testFailingStartTellsOnce();
    testStillOnAnotherCamera();
    testHardwareBudget();
    testStillDeadlines();
    testStillsInFlight();
    testHostileGuest();
    testEveryOfferedRunStartsInTheAnchor();
    testABurstOfFramesIsTheNewestFrame();
    testARecoveryRepairsTheCameraBeforeItIsToldItIsBack();
    testARecoveryThatFailsIsAFailureAndNeverResumes();
    testAFrameCapturedBeforeTheRepairIsNeverDeliveredAfterIt();
    testAStillThatSpansAnInterruptionFails();
    testTheMostFramesHeldAtOnceIsWhatTheReserveCounts();
    testTheDeviceRunsOnlyTheCamerasItsUseAllows();
    testASwitchWithTheTorchOnHoldsOneCamerasFrames();
    testStillsAreAdmittedByTheBytesTheyCost();
    testAnOversizedStillFailsItsCapture();
    testAFileWithinTheGuestsBufferIsTaken();
    testARefusedChangeLeavesTheCaptureChargedUntilItStops();
    testALateFrameOfAStoppedCameraIsNotKept();
    testAnExpiredCaptureStaysChargedUntilThePlatformEndsIt();
    testAWaitingCapturesTimeRunsFromItsRequest();
    testACompletionQueuedWhenExpiryDecidesIsNotALoss();
    testAWaitingCaptureThatGrowsBeyondTheWholeBudgetIsRefused();
    testAResultsPayloadIsFreedBeforeTheNextCaptureStarts();
    testStillsAreAdmittedInTheOrderTheyAsked();
    testATurnedStillHoldsTheEncodersBuffersBesideItsFile();
    testACaptureBeyondTheWholeBudgetIsRefusedBeforeItStarts();
    testBlackPicturesHoldNothingOfTheirOwn();
    testAStillThatFailsSaysWhy();
    testOnlyACaptureThatEndedInNoPictureIsReported();
    testService();
    return 0;
}
