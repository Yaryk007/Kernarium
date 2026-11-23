// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stand-ins for the Android types that the camera HAL's EmulatedSensorHost.cpp uses, enough to compile that file
// unchanged and run its callback path (tests/tooling/android/test_camera_device_lost.py). The members are public;
// the frame source is a scripted fake.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <variant>
#include <vector>

#include "camera_session.hpp"

using status_t = int;
using nsecs_t = std::int64_t;
constexpr int OK = 0;
constexpr int BAD_VALUE = -22;
constexpr int UNKNOWN_ERROR = INT32_MIN;
constexpr int ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN = 0;
constexpr int ANDROID_SENSOR_EXPOSURE_TIME = 1;
constexpr int HAL_DATASPACE_V0_JFIF = 0;

struct camera_metadata_ro_entry_t {
    std::size_t count = 0;
    struct {
        std::int64_t* i64 = nullptr;
    } data;
};

namespace android {

struct StreamGroupState {};

namespace google_camera_hal {

enum class BufferStatus { kOk, kError };
enum class ErrorCode { kErrorDevice, kErrorRequest, kErrorBuffer };

struct ErrorMessage {
    std::uint32_t frame_number;
    int error_stream_id;
    ErrorCode error_code;
};

struct ShutterMessage {
    std::uint32_t frame_number;
    std::uint64_t timestamp_ns;
    std::uint64_t readout_timestamp_ns;
    std::vector<StreamGroupState> stream_group_state;
};

using NotifyMessage = std::variant<ErrorMessage, ShutterMessage>;

struct HalCameraMetadata {
    int Get(int, camera_metadata_ro_entry_t*) { return BAD_VALUE; }
};

struct StreamBuffer {
    BufferStatus status = BufferStatus::kError;
    void* buffer = nullptr;
    void* acquire_fence = nullptr;
    void* release_fence = nullptr;
    int stream_id = 0;
};

struct HwlPipelineResult {
    std::uint32_t camera_id = 0;
    std::uint32_t pipeline_id = 0;
    std::uint32_t frame_number = 0;
    std::uint32_t partial_result = 0;
    std::unique_ptr<HalCameraMetadata> result_metadata;
    std::vector<StreamBuffer> input_buffers;
    std::vector<StreamBuffer> output_buffers;
};

struct HwlPipelineCallback {
    std::function<void(std::uint32_t, const NotifyMessage&)> notify;
    std::function<void(std::unique_ptr<HwlPipelineResult>)> process_pipeline_result;
    std::function<void(std::uint32_t, const std::vector<StreamGroupState>&)> notify_override_pending_buffer;
};

} // namespace google_camera_hal

using namespace google_camera_hal;

enum class PixelFormat { BLOB, YUV };

struct SensorSettings {
    std::uint32_t timestamp_source = 0;
    nsecs_t exposure_time = 1;
};

struct SensorCharacteristics {};

struct SensorBuffer {
    virtual ~SensorBuffer() = default;
    bool is_input = false;
    bool is_failed_request = false;
    PixelFormat format = PixelFormat::YUV;
    int dataSpace = 0;
    std::uint32_t camera_id = 0;
    std::uint32_t pipeline_id = 0;
    std::uint32_t frame_number = 0;
    StreamBuffer stream_buffer;
    HwlPipelineCallback callback;
};

using Buffers = std::vector<std::unique_ptr<SensorBuffer>>;
using LogicalCameraSettings = std::map<std::uint32_t, SensorSettings>;

// A frame source that the test scripts: what its buffers do, and whether it says the device is lost.
struct ScriptedSource {
    int begun = 0;
    int produced = 0;
    // Result of the n-th ProduceFrame of the test (OK or UNKNOWN_ERROR); beyond the list, OK.
    std::vector<int> results;
    bool lost = false;
    bool fails_when_all_failed = true;

    status_t BeginRequest(const LogicalCameraSettings&, Buffers*) {
        ++begun;
        return OK;
    }
    status_t ProduceFrame(std::uint32_t, nsecs_t, const SensorSettings&, SensorBuffer*, const SensorBuffer*) {
        const std::size_t at = static_cast<std::size_t>(produced++);
        return at < results.size() ? results[at] : OK;
    }
    status_t ProduceBlob(std::uint32_t, const SensorSettings&, SensorBuffer*, HalCameraMetadata*,
                         const SensorCharacteristics&) {
        ++produced;
        return OK;
    }
    bool DeviceLost() const { return lost; }
    bool FailsRequest(std::size_t outputs, std::size_t failed) const {
        return fails_when_all_failed && outputs != 0 && failed >= outputs;
    }
    bool FrameResult(std::uint32_t, HalCameraMetadata*, nsecs_t*) { return true; }
};

struct EmulatedSensor {
    ScriptedSource* frame_source_ = nullptr;
    std::shared_ptr<andarium::camera::SessionHealth> session_health_ =
        std::make_shared<andarium::camera::SessionHealth>();
    std::shared_ptr<std::map<std::uint32_t, SensorCharacteristics>> chars_ =
        std::make_shared<std::map<std::uint32_t, SensorCharacteristics>>();
    std::uint32_t logical_camera_id_ = 0;
    nsecs_t next_capture_time_ = 0;
    nsecs_t next_readout_time_ = 0;
    static constexpr nsecs_t kSupportedExposureTimeRange[] = {1, 100};

    nsecs_t getSystemTimeWithSource(std::uint32_t) { return 1000; }
    void CaptureFromHost(const LogicalCameraSettings& settings, Buffers* buffers, HwlPipelineResult* result,
                         const HwlPipelineCallback& callback, const std::vector<StreamGroupState>& stream_group_state,
                         std::uint32_t frame_number);
};

} // namespace android
