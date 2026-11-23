// SPDX-License-Identifier: Apache-2.0

#include "device_orientation.hpp"
#include "motion_protocol.hpp"
#include <V2_1/SubHal.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <log/log.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace {
using namespace android::hardware;
using namespace android::hardware::sensors;
using V1_0::Result;
using V1_0::OperationMode;
using V1_0::RateLevel;
using V1_0::SensorFlagBits;
using V1_0::SharedMemInfo;
using V2_1::Event;
using V2_1::SensorType;
using V2_1::implementation::IHalProxyCallback;
using android::base::unique_fd;
namespace motion = andarium::motion;

int64_t nowNs() {
    timespec time{};
    clock_gettime(CLOCK_BOOTTIME, &time);
    return int64_t(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
// The sensors that read at a period, in `states_`: the accelerometer, the
// gyroscope and, for a host with a magnetometer, the magnetometer. The
// device-orientation sensor is on-change and has none. -1 for a handle that is
// not one of them.
constexpr size_t timed_sensor_count = 3;
constexpr int state_index(int32_t handle) {
    switch (handle) {
    case motion::accelerometer_handle: return 0;
    case motion::gyroscope_handle: return 1;
    case motion::magnetometer_handle: return 2;
    default: return -1;
    }
}
struct SensorState {
    bool active = false;
    int64_t period = motion::minimum_period_ns;
    int64_t next = 0;
};
// Every sensor not in use, at the shortest period it is listed with.
std::array<SensorState, timed_sensor_count> idle_states() {
    std::array<SensorState, timed_sensor_count> states{};
    states[state_index(motion::magnetometer_handle)].period = motion::minimum_magnetometer_period_ns;
    return states;
}

class MotionSubHal final : public V2_1::implementation::ISensorsSubHal {
public:
    MotionSubHal()
        : wake_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
          worker_([this](std::stop_token stop) { serve(stop); }) {}

    Return<void> getSensorsList_2_1(getSensorsList_2_1_cb callback) override {
        hidl_vec<V2_1::SensorInfo> sensors;
        sensors.resize(listing_.count);
        for (size_t i = 0; i < listing_.count; ++i) {
            auto& sensor = sensors[i];
            sensor.sensorHandle = listing_.handles[i];
            sensor.vendor = "Andarium";
            sensor.version = 1;
            switch (sensor.sensorHandle) {
            case motion::accelerometer_handle:
            case motion::gyroscope_handle: {
                const bool accelerometer = sensor.sensorHandle == motion::accelerometer_handle;
                sensor.name = accelerometer ? "Host Accelerometer" : "Host Gyroscope";
                sensor.type = accelerometer ? SensorType::ACCELEROMETER : SensorType::GYROSCOPE;
                sensor.maxRange = accelerometer ? 156.9064f : 34.9066f;
                sensor.resolution = 0.001f;
                sensor.power = 0.5f;
                sensor.minDelay = 10'000;
                sensor.maxDelay = 1'000'000;
                // Continuous, non-wake-up, no FIFO, injection or direct channel.
                sensor.flags = 0;
                break;
            }
            case motion::orientation_handle:
                sensor.name = "Host Device Orientation";
                sensor.type = SensorType::DEVICE_ORIENTATION;
                sensor.maxRange = motion::orientation_count - 1;
                sensor.resolution = 1.0f;
                sensor.power = 0.001f;
                sensor.minDelay = 0;
                sensor.maxDelay = 0;
                // On-change, non-wake-up, no FIFO, injection or direct channel.
                sensor.flags = static_cast<uint32_t>(SensorFlagBits::ON_CHANGE_MODE);
                break;
            case motion::magnetometer_handle:
                sensor.name = "Host Magnetometer";
                sensor.type = SensorType::MAGNETIC_FIELD;
                sensor.maxRange = motion::magnetometer_attributes.max_range;
                sensor.resolution = motion::magnetometer_attributes.resolution;
                sensor.power = motion::magnetometer_attributes.power_ma;
                sensor.minDelay = motion::magnetometer_attributes.min_delay_us;
                sensor.maxDelay = motion::magnetometer_attributes.max_delay_us;
                // Continuous, non-wake-up, no FIFO, injection or direct channel.
                sensor.flags = 0;
                break;
            }
        }
        // The line also says, to anyone who reads the log or the library, which
        // properties decide the list.
        ALOGI("Andarium motion: ro.boot.kernarium.orientation=%d, %zu sensors, ro.boot.kernarium.compass=%d",
              features_.orientation, sensors.size(), features_.compass);
        callback(sensors);
        return Void();
    }
    Return<Result> initialize(const android::sp<IHalProxyCallback>& callback) override {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        states_ = idle_states();
        orientation_.reset();
        // Every subscription ended with the framework's restart.
        sendControl();
        return Result::OK;
    }
    const std::string getName() override { return "Andarium Motion"; }
    Return<Result> setOperationMode(OperationMode mode) override {
        return mode == OperationMode::NORMAL ? Result::OK : Result::BAD_VALUE;
    }
    Return<Result> activate(int32_t handle, bool enabled) override {
        if (!valid(handle)) return Result::BAD_VALUE;
        std::lock_guard lock(mutex_);
        if (handle == motion::orientation_handle) {
            const bool changed = orientation_.active() != enabled;
            // The framework is given the host's last value at once.
            if (const auto value = orientation_.activate(enabled)) postOrientation(*value);
            if (changed) sendControl();
            return Result::OK;
        }
        auto& state = states_[state_index(handle)];
        const bool changed = state.active != enabled;
        state.active = enabled;
        state.next = 0;
        if (changed) sendControl();
        return Result::OK;
    }
    Return<Result> batch(int32_t handle, int64_t period, int64_t latency) override {
        if (!valid(handle) || period < 0 || latency < 0) return Result::BAD_VALUE;
        // An on-change sensor has no period, and the host does not pace it.
        if (handle == motion::orientation_handle) return Result::OK;
        std::lock_guard lock(mutex_);
        auto& state = states_[state_index(handle)];
        const auto previous = state.period;
        // Each sensor is listed with its own limits, and the host is told the
        // period within them.
        state.period = motion::clamp_period(handle, period);
        // A shorter period starts a new deadline: the old one lies up to a
        // whole old period ahead, and would hold the faster client back.
        if (state.period < previous) state.next = 0;
        // The host paces its readings by the period of a sensor in use; one
        // that is not in use is told its period when it is activated.
        if (state.active && state.period != previous) sendControl();
        return Result::OK;
    }
    Return<Result> flush(int32_t handle) override {
        if (!valid(handle)) return Result::BAD_VALUE;
        std::lock_guard lock(mutex_);
        const bool active = handle == motion::orientation_handle ? orientation_.active()
                                                                 : states_[state_index(handle)].active;
        if (!active || !callback_) return Result::BAD_VALUE;
        Event event{};
        event.sensorHandle = handle;
        event.sensorType = SensorType::META_DATA;
        event.timestamp = nowNs();
        event.u.meta.what = V1_0::MetaDataEventType::META_DATA_FLUSH_COMPLETE;
        post(event);
        return Result::OK;
    }
    Return<Result> injectSensorData_2_1(const Event&) override { return Result::INVALID_OPERATION; }
    Return<void> registerDirectChannel(const SharedMemInfo&, registerDirectChannel_cb callback) override {
        callback(Result::INVALID_OPERATION, -1);
        return Void();
    }
    Return<Result> unregisterDirectChannel(int32_t) override { return Result::INVALID_OPERATION; }
    Return<void> configDirectReport(int32_t, int32_t, RateLevel, configDirectReport_cb callback) override {
        callback(Result::INVALID_OPERATION, 0);
        return Void();
    }
    Return<void> debug(const hidl_handle& fd, const hidl_vec<hidl_string>&) override {
        if (fd.getNativeHandle() && fd->numFds > 0) {
            std::lock_guard lock(mutex_);
            dprintf(fd->data[0], "Andarium motion: accelerometer=%d gyroscope=%d\n",
                    states_[0].active, states_[1].active);
            if (features_.compass) {
                dprintf(fd->data[0], "Andarium motion: magnetometer=%d\n", states_[2].active);
            }
            if (features_.orientation) {
                dprintf(fd->data[0], "Andarium motion: orientation=%d host=%d\n",
                        orientation_.active(), orientation_.latest().value_or(-1));
            }
        }
        return Void();
    }

private:
    bool valid(int32_t handle) const { return listing_.contains(handle); }
    // Tells the host which sensors applications use and how often, on the
    // connection it made. The frame is the whole state, so a later frame
    // replaces an earlier one. Called with mutex_ held. A frame that cannot
    // be sent whole would leave the host with a stale picture, so the
    // connection is shut down: the receiving loop ends, the host connects
    // again and the state is sent on that connection. A guest with the
    // orientation sensor or the magnetometer says so in the frame, which is
    // also how a host learns that it may send orientations or magnetic fields.
    void sendControl() {
        if (peer_ < 0) return;
        const auto frame = motion::control_frame(
            features_, {states_[0].active, states_[1].active, states_[2].active},
            {states_[0].period, states_[1].period, states_[2].period}, orientation_.active());
        const auto sent = send(peer_, frame.bytes.data(), frame.size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(frame.size)) {
            ALOGW("Andarium motion: control frame not sent (%zd): %s", sent, strerror(errno));
            shutdown(peer_, SHUT_RDWR);
        }
    }
    // The connection the host made, for sendControl. It is set after the
    // host is accepted and cleared before the descriptor closes.
    void attach(int fd) {
        std::lock_guard lock(mutex_);
        peer_ = fd;
        sendControl();
    }
    void detach() {
        std::lock_guard lock(mutex_);
        peer_ = -1;
    }
    // Serialize events with activate, initialize and flush.
    void post(const Event& event) {
        callback_->postEvents({event}, callback_->createScopedWakelock(false));
    }
    // Posts the orientation the framework is to see. Called with mutex_ held.
    void postOrientation(int value) {
        if (!callback_) return;
        Event event{};
        event.sensorHandle = motion::orientation_handle;
        event.sensorType = SensorType::DEVICE_ORIENTATION;
        event.timestamp = nowNs();
        event.u.scalar = static_cast<float>(value);
        post(event);
    }
    // Answers false for a record that ends the connection: one of a sensor
    // the guest did not list, and so did not tell the host that it takes.
    bool consume(const motion::Sample& sample) {
        std::lock_guard lock(mutex_);
        if (!motion::takes_record(features_, sample.type)) return false;
        if (sample.type == motion::orientation_type) {
            if (const auto value = orientation_.host_value(static_cast<int>(sample.values[0]))) {
                postOrientation(*value);
            }
            return true;
        }
        const auto handle = *motion::handle_of_type(sample.type);
        auto& state = states_[state_index(handle)];
        if (!callback_ || !state.active) return true;
        const auto timestamp = nowNs();
        // Keep a sampling deadline instead of rounding every jittered host
        // interval up to two periods. Late samples never cause a catch-up burst.
        if (timestamp + state.period / 10 < state.next) return true;
        state.next = state.next == 0 || timestamp > state.next + state.period
            ? timestamp + state.period : state.next + state.period;
        Event event{};
        event.sensorHandle = handle;
        event.sensorType = static_cast<SensorType>(sample.type);
        event.timestamp = timestamp;
        event.u.vec3.x = sample.values[0];
        event.u.vec3.y = sample.values[1];
        event.u.vec3.z = sample.values[2];
        // The magnetic field carries the host's own account of how far it can
        // be trusted (the decoder keeps it within SensorStatus' 0 to 3); the
        // other two are what the device's own sensors measure.
        event.u.vec3.status = sample.type == motion::magnetic_field_type
            ? static_cast<V1_0::SensorStatus>(sample.status) : V1_0::SensorStatus::ACCURACY_HIGH;
        post(event);
        return true;
    }
    // Blocks until fd is readable, with no timeout: the service is started at
    // boot whether or not a host ever connects, so it must not wake to poll.
    // Returns false when the descriptor failed or the wake descriptor, written
    // when a stop is requested, became readable. Nothing drains the wake
    // descriptor, so every later call returns false at once.
    static bool readable(int fd, int wake) {
        for (;;) {
            pollfd descriptors[] = {{fd, POLLIN, 0}, {wake, POLLIN, 0}};
            const auto result = poll(descriptors, 2, -1);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || descriptors[1].revents != 0) return false;
            return (descriptors[0].revents & POLLIN) != 0;
        }
    }
    void receive(int fd) {
        std::array<uint8_t, motion::record_size> record{};
        size_t used = 0;
        while (readable(fd, wake_.get())) {
            const auto count = recv(fd, record.data() + used, record.size() - used, MSG_DONTWAIT);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) return;
            used += count;
            if (used != record.size()) continue;
            const auto sample = motion::decode(record);
            if (!sample || !consume(*sample)) return;
            used = 0;
        }
    }
    void serve(std::stop_token stop) {
        if (wake_ < 0) {
            ALOGE("Andarium motion: cannot create the wake descriptor: %s", strerror(errno));
            return;
        }
        // A stop request wakes whichever poll the thread is blocked in.
        const std::stop_callback wake_on_stop(stop, [this] { eventfd_write(wake_.get(), 1); });
        unique_fd listener(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        sockaddr_vm address{};
        address.svm_family = AF_VSOCK;
        address.svm_cid = VMADDR_CID_ANY;
        address.svm_port = motion::port;
        if (listener < 0 || bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            listen(listener.get(), 1)) {
            ALOGE("Andarium motion: cannot listen on vsock: %s", strerror(errno));
            return;
        }
        while (readable(listener.get(), wake_.get())) {
            sockaddr_vm peer{};
            socklen_t length = sizeof(peer);
            unique_fd stream(accept4(listener.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                                     SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (stream < 0) continue;
            // Only the private host channel may supply hardware samples.
            if (peer.svm_cid != VMADDR_CID_HOST) continue;
            attach(stream.get());
            receive(stream.get());
            detach();
        }
    }
    // What the host asked for at boot: the device-orientation sensor
    // (androidboot.kernarium.orientation=1) and the magnetometer
    // (androidboot.kernarium.compass=1). Read once: the framework picks the
    // way it follows rotation from the list, as it starts, and an image that
    // lists the magnetometer has said so to the host. Declared before the
    // worker, which uses them.
    const motion::Features features_{
        android::base::GetBoolProperty(motion::orientation_property, false),
        android::base::GetBoolProperty(motion::compass_property, false)};
    const motion::Listing listing_ = motion::listing(features_);
    std::mutex mutex_;
    android::sp<IHalProxyCallback> callback_;
    std::array<SensorState, timed_sensor_count> states_ = idle_states();
    motion::DeviceOrientation orientation_;
    int peer_ = -1;
    // Declared before the worker, so it outlives the join in the worker's
    // destructor, which requests a stop and so writes to it.
    unique_fd wake_;
    // Destroyed first; its stop request wakes the polling loop, which is then
    // joined before state teardown.
    std::jthread worker_;
};
} // namespace

V2_1::implementation::ISensorsSubHal* sensorsHalGetSubHal_2_1(uint32_t* version) {
    static MotionSubHal hal;
    *version = SUB_HAL_2_1_VERSION;
    return &hal;
}
