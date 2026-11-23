// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs the callback path of the camera HAL's EmulatedSensor::CaptureFromHost (the series' own EmulatedSensorHost.cpp,
// compiled unchanged against stub/EmulatedSensor.h) with a scripted frame source, and prints what the framework would
// hear: the notifications and the buffers that come back, in order. tests/tooling/android/test_camera_device_lost.py
// compiles and runs it.
//
// The buffers mirror the stock GrallocSensorBuffer destructor (the stock file is not in this repository): a buffer
// that goes back with an error status and is not part of a failed request sends ERROR_BUFFER, and every buffer returns
// its stream buffer, with the status it has, in a result of its own.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "EmulatedSensor.h"

namespace {

using namespace android;
using andarium::camera::SessionHealth;

std::vector<std::string> events;

struct StockBuffer : SensorBuffer {
    ~StockBuffer() override
    {
        if (stream_buffer.status != BufferStatus::kOk && callback.notify != nullptr && !is_failed_request) {
            callback.notify(pipeline_id, ErrorMessage{frame_number, stream_buffer.stream_id, ErrorCode::kErrorBuffer});
        }
        if (callback.process_pipeline_result != nullptr) {
            auto result = std::make_unique<HwlPipelineResult>();
            result->frame_number = frame_number;
            result->output_buffers.push_back(stream_buffer);
            callback.process_pipeline_result(std::move(result));
        }
    }
};

HwlPipelineCallback make_callback()
{
    HwlPipelineCallback callback;
    callback.notify = [](std::uint32_t, const NotifyMessage& message) {
        if (const auto* error = std::get_if<ErrorMessage>(&message)) {
            const char* name = error->error_code == ErrorCode::kErrorDevice    ? "ERROR_DEVICE"
                               : error->error_code == ErrorCode::kErrorRequest ? "ERROR_REQUEST"
                                                                               : "ERROR_BUFFER";
            events.push_back(std::string(name) + " f" + std::to_string(error->frame_number) +
                             (error->error_code == ErrorCode::kErrorBuffer ? " s" + std::to_string(error->error_stream_id) : ""));
        } else {
            events.push_back("SHUTTER f" + std::to_string(std::get<ShutterMessage>(message).frame_number));
        }
    };
    callback.process_pipeline_result = [](std::unique_ptr<HwlPipelineResult> result) {
        for (const auto& buffer : result->output_buffers) {
            events.push_back(std::string("BUFFER f") + std::to_string(result->frame_number) + " s" +
                             std::to_string(buffer.stream_id) + (buffer.status == BufferStatus::kOk ? " OK" : " ERROR"));
        }
    };
    return callback;
}

// One request of `streams` output buffers on stream ids 1..streams, run through the sensor; true if its metadata was
// still there afterwards.
bool run_request(EmulatedSensor& sensor, std::uint32_t frame, int streams)
{
    Buffers buffers;
    for (int id = 1; id <= streams; ++id) {
        auto buffer = std::make_unique<StockBuffer>();
        buffer->frame_number = frame;
        buffer->stream_buffer.stream_id = id;
        buffer->stream_buffer.status = BufferStatus::kError; // the request processor hands them over marked as failed
        buffer->callback = make_callback();
        buffers.push_back(std::move(buffer));
    }
    HwlPipelineResult result;
    result.result_metadata = std::make_unique<HalCameraMetadata>();
    LogicalCameraSettings settings;
    settings[0] = SensorSettings{};
    sensor.chars_->emplace(0, SensorCharacteristics{});
    sensor.CaptureFromHost(settings, &buffers, &result, make_callback(), {}, frame);
    return result.result_metadata != nullptr;
}

void dump_and_clear(const char* title)
{
    std::printf("%s\n", title);
    for (const auto& event : events) std::printf("  %s\n", event.c_str());
    events.clear();
}

} // namespace

int main(int argc, char** argv)
{
    const std::string scenario = argc > 1 ? argv[1] : "";
    ScriptedSource source;
    EmulatedSensor sensor;
    sensor.frame_source_ = &source;

    if (scenario == "mixed-completion") {
        // One output completes, the other meets the loss: the source says the device is lost.
        source.results = {OK, UNKNOWN_ERROR};
        source.lost = true;
        sensor.session_health_->set_encoder_stream(true);
        const bool kept = run_request(sensor, 7, 2);
        std::printf("metadata %s\nlost %d\n", kept ? "kept" : "reset", sensor.session_health_->lost() ? 1 : 0);
        dump_and_clear("events");
    } else if (scenario == "drain") {
        // The session is over and the framework knows: later requests, accepted before it, drain without touching the source.
        sensor.session_health_->latch_lost();
        std::printf("claimed %d\n", sensor.session_health_->claim_device_error() ? 1 : 0); // the first request told it
        source.results = {OK};
        for (std::uint32_t frame = 8; frame <= 10; ++frame) {
            const bool kept = run_request(sensor, frame, 2);
            std::printf("frame %u metadata %s\n", frame, kept ? "kept" : "reset");
        }
        std::printf("source begun %d produced %d\n", source.begun, source.produced);
        dump_and_clear("events");
    } else if (scenario == "lost-first") {
        // The session is over and no request has told the framework yet: the first request that reaches the sensor does, and
        // fails as a request too; the next one only fails.
        sensor.session_health_->latch_lost();
        source.results = {OK};
        for (std::uint32_t frame = 11; frame <= 12; ++frame) {
            const bool kept = run_request(sensor, frame, 2);
            std::printf("frame %u metadata %s\n", frame, kept ? "kept" : "reset");
        }
        std::printf("source begun %d produced %d\n", source.begun, source.produced);
        dump_and_clear("events");
    } else if (scenario == "reconfigured") {
        // A reconfiguration makes a new sensor and a new source; the session's health is the same one.
        auto health = std::make_shared<SessionHealth>();
        health->set_encoder_stream(true);
        health->latch_lost();
        (void)health->claim_device_error(); // the request that met the loss told the framework
        EmulatedSensor again;
        again.session_health_ = health;
        ScriptedSource fresh;
        again.frame_source_ = &fresh;
        run_request(again, 20, 1);
        std::printf("fresh source begun %d produced %d\n", fresh.begun, fresh.produced);
        dump_and_clear("events");
    } else if (scenario == "healthy") {
        run_request(sensor, 1, 2);
        std::printf("lost %d\n", sensor.session_health_->lost() ? 1 : 0);
        dump_and_clear("events");
    } else if (scenario == "all-failed") {
        // Every output fails and the source does not say the device is gone: a failed request, as before.
        source.results = {UNKNOWN_ERROR, UNKNOWN_ERROR};
        const bool kept = run_request(sensor, 3, 2);
        std::printf("metadata %s\nlost %d\n", kept ? "kept" : "reset", sensor.session_health_->lost() ? 1 : 0);
        dump_and_clear("events");
    } else if (scenario == "some-failed") {
        // One of two outputs fails: a buffer error, the frame goes on.
        source.results = {OK, UNKNOWN_ERROR};
        const bool kept = run_request(sensor, 4, 2);
        std::printf("metadata %s\nlost %d\n", kept ? "kept" : "reset", sensor.session_health_->lost() ? 1 : 0);
        dump_and_clear("events");
    } else if (scenario == "later-loss") {
        // The loss on one frame, then another request: ERROR_DEVICE for the first only.
        source.results = {UNKNOWN_ERROR};
        source.lost = true;
        run_request(sensor, 5, 1);
        run_request(sensor, 6, 1);
        std::printf("source begun %d\n", source.begun);
        dump_and_clear("events");
    } else {
        std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
        return 2;
    }
    return 0;
}
