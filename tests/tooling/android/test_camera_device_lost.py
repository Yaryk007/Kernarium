#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What the camera HAL does when the host ends a camera's video.

When the application leaves the foreground the host ends the video of a camera that has a video stream open: its video buffers
fail under a black, interrupted metadata record for as long as the guest keeps the stream (the wire README of kernel/common/video).
The HAL (route p0030) decides by the session's streams. A session with a stream whose usage carries the video encoder bit is over:
the request that met the loss fails as a whole with every output buffer returned as an error, the framework is told that the
request failed (ERROR_REQUEST) and, once per session, ERROR_DEVICE besides, later requests drain as ERROR_REQUEST, and the session
answers every later request, flush and configuration with the internal error (a flush first ends what the session holds as failed
requests). Any other session is told nothing: the frame source closes the camera's video streams between requests and opens them again.

What this row holds. The callback path of the sensor is executed: the series' own EmulatedSensorHost.cpp is compiled unchanged
against stub headers (camera_hal_probe/stub) and run with a scripted frame source, so the notifications and the buffers that come
back are the real code's. The rest is the wiring of the series' sources, read as text and therefore structural: the frame
source's decision and its closing of the streams, the guards in the opened session (a base file, read from the lines the series
adds), and nothing else reporting ERROR_DEVICE. The rules themselves (which streams make an encoder session, the once-only
announcement, the reopening) are unit rows of the overlay header camera_session.hpp. Nothing here boots anything.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPOSITORY = Path(__file__).resolve().parents[3]
# UML_CAMERA_HAL_SERIES names another directory of the series (a staged one); the default is the repository's.
SERIES = Path(os.environ.get("UML_CAMERA_HAL_SERIES") or REPOSITORY / "android/lineage-24/patches/projects/p0030/series")
OVERLAY = REPOSITORY / "android/lineage-24/overlay/camera/hal"
PROBE = Path(__file__).resolve().parent / "camera_hal_probe"
HWL = "devices/EmulatedCamera/hwl"
FRAME_SOURCE = f"{HWL}/IFrameSource.h"
SESSION = f"{HWL}/EmulatedCameraDeviceSessionHWLImpl.cpp"
SENSOR_HEADER = f"{HWL}/EmulatedSensor.h"
SENSOR_SETUP = f"{HWL}/EmulatedSensor.cpp"
V4L2_HEADER = f"{HWL}/V4l2FrameSource.h"
V4L2_SOURCE = f"{HWL}/V4l2FrameSource.cpp"
SENSOR = f"{HWL}/EmulatedSensorHost.cpp"
TOPIC = "handle-the-host-ending-the-video-of-a-camera.patch"


def patch_names() -> list[str]:
    return [line.split("\t", 1)[1] for line in (SERIES / "series.tsv").read_text(encoding="utf-8").splitlines()
            if line.startswith("patch\t")]


def rebuilt_files() -> dict[str, str]:
    """The three sources that the series creates, as the whole series leaves them: every patch that touches one is applied, in
    the order of series.tsv, to an empty repository, limited to those paths."""
    paths = (V4L2_HEADER, V4L2_SOURCE, SENSOR)
    with tempfile.TemporaryDirectory(prefix="camera-lost-") as directory:
        subprocess.run(["git", "init", "-q", directory], check=True)
        for name in patch_names():
            text = (SERIES / name).read_text(encoding="utf-8")
            if not any(f"diff --git a/{path} " in text for path in paths):
                continue
            subprocess.run(["git", "-C", directory, "apply", *(f"--include={path}" for path in paths), str(SERIES / name)],
                           check=True)
        return {path: (Path(directory) / path).read_text(encoding="utf-8") for path in paths}


def added_lines(path: str) -> str:
    """The lines that the series adds to a file that the base already has, in series order."""
    added: list[str] = []
    for name in patch_names():
        inside = False
        for line in (SERIES / name).read_text(encoding="utf-8").splitlines():
            if line.startswith("diff --git "):
                inside = line.startswith(f"diff --git a/{path} ")
            elif inside and line.startswith("+") and not line.startswith("+++"):
                added.append(line[1:])
    return "\n".join(added)


def block(source: str, opening: str) -> str:
    """The text between the braces that follow the first `opening` (up to and including its `{`), by brace depth."""
    start = source.index(opening) + len(opening)
    depth = 1
    for position in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[position], 0)
        if depth == 0:
            return source[start:position]
    raise AssertionError(f"{opening!r} is never closed")


class SensorCallbackTests(unittest.TestCase):
    """The series' EmulatedSensorHost.cpp, compiled and run: what the framework hears and which buffers come back."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.directory = tempfile.mkdtemp(prefix="camera-sensor-")
        sensor = Path(cls.directory) / "EmulatedSensorHost.cpp"
        sensor.write_text(rebuilt_files()[SENSOR], encoding="utf-8")
        compiler = os.environ.get("UML_CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("no C++ compiler")
        cls.probe = Path(cls.directory) / "probe"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I", str(PROBE / "stub"),
                        "-I", str(OVERLAY), str(sensor), str(PROBE / "sensor_probe.cpp"), "-o", str(cls.probe)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.directory, ignore_errors=True)

    def assert_every_device_error_is_a_failed_request(self, events: list[str]) -> None:
        """The framework returns the buffers of a request only when it was told that the request failed: a request that only the
        device error named keeps them (and the references to their streams) until the camera service ends."""
        for event in events:
            if event.startswith("ERROR_DEVICE f"):
                frame = event.removeprefix("ERROR_DEVICE ")
                self.assertIn(f"ERROR_REQUEST {frame}", events, f"{event} names a request the framework is never told failed")
                self.assertLess(events.index(event), events.index(f"ERROR_REQUEST {frame}"), "after the device error")

    def run_raw(self, scenario: str) -> str:
        return subprocess.run([str(self.probe), scenario], check=True, capture_output=True, text=True, timeout=60).stdout

    def run_probe(self, scenario: str) -> tuple[dict[str, str], list[str]]:
        """The probe's `key value` lines and the events it listed, in order."""
        values: dict[str, str] = {}
        events: list[str] = []
        listing = False
        for line in self.run_raw(scenario).splitlines():
            if line == "events":
                listing = True
            elif listing:
                events.append(line.strip())
            elif " " in line:
                key, _, value = line.partition(" ")
                values[key] = value
        return values, events

    def test_one_output_completes_and_another_meets_the_loss(self) -> None:
        # The mixed completion of the HAL review: the first output was produced, the second failed under the lost camera.
        values, events = self.run_probe("mixed-completion")
        self.assertEqual(events[:2], ["ERROR_DEVICE f7", "ERROR_REQUEST f7"],
                         "the framework is told the device is gone, first, and that the request failed")
        self.assertEqual(sorted(events[2:]), ["BUFFER f7 s1 ERROR", "BUFFER f7 s2 ERROR"],
                         "and every buffer of the request goes back with an error status, the one that was filled too")
        self.assert_every_device_error_is_a_failed_request(events)
        self.assertNotIn("SHUTTER", " ".join(events), "a failed request has no shutter")
        self.assertNotIn("ERROR_BUFFER", " ".join(events), "and no buffer error of its own")
        self.assertEqual(values["metadata"], "reset", "nor results")
        self.assertEqual(values["lost"], "1", "the loss is latched in the session's health")

    def test_the_requests_a_lost_session_still_holds_drain_without_the_source(self) -> None:
        raw = self.run_raw("drain")
        _, events = self.run_probe("drain")
        self.assertEqual([e for e in events if e.startswith("ERROR")], ["ERROR_REQUEST f8", "ERROR_REQUEST f9", "ERROR_REQUEST f10"],
                         "each request drains as a failed request, in order, and none tells the device is gone again")
        for frame in (8, 9, 10):
            buffers = sorted(e for e in events if e.startswith(f"BUFFER f{frame} "))
            self.assertEqual(buffers, [f"BUFFER f{frame} s1 ERROR", f"BUFFER f{frame} s2 ERROR"], "every buffer goes back as an error")
            self.assertLess(events.index(f"ERROR_REQUEST f{frame}"), events.index(buffers[0]), "after the request's error")
        self.assertEqual(raw.count("metadata reset"), 3, "a drained request carries no results")
        self.assertIn("source begun 0 produced 0", raw, "and the source is not asked for anything: production is not restarted")

    def test_the_device_gone_is_told_once_per_session(self) -> None:
        _, events = self.run_probe("later-loss")
        self.assertEqual(events.count("ERROR_DEVICE f5"), 1)
        self.assertNotIn("ERROR_DEVICE f6", events)
        self.assertIn("ERROR_REQUEST f6", events)
        self.assertEqual(events.count("ERROR_REQUEST f5"), 1, "the request that met the loss failed as a request as well")
        self.assertLess(events.index("ERROR_DEVICE f5"), events.index("ERROR_REQUEST f5"))
        self.assertLess(events.index("ERROR_REQUEST f5"), events.index("ERROR_REQUEST f6"), "in order")
        self.assert_every_device_error_is_a_failed_request(events)

    def test_the_first_request_of_a_session_that_is_over_tells_the_device_gone_and_fails_as_a_request(self) -> None:
        # The loss was latched earlier (by the frame source of an earlier sensor, or the session): the first request that reaches
        # the sensor carries the news, and every buffer of it and of the next goes back as an error.
        raw = self.run_raw("lost-first")
        _, events = self.run_probe("lost-first")
        self.assertEqual(events[:2], ["ERROR_DEVICE f11", "ERROR_REQUEST f11"])
        self.assertEqual([e for e in events if e.startswith("ERROR")], ["ERROR_DEVICE f11", "ERROR_REQUEST f11", "ERROR_REQUEST f12"],
                         "the next request only fails")
        for frame in (11, 12):
            buffers = sorted(e for e in events if e.startswith(f"BUFFER f{frame} "))
            self.assertEqual(buffers, [f"BUFFER f{frame} s1 ERROR", f"BUFFER f{frame} s2 ERROR"])
            self.assertLess(events.index(f"ERROR_REQUEST f{frame}"), events.index(buffers[0]), "after the request's error")
        self.assertEqual(raw.count("metadata reset"), 2, "neither carries results")
        self.assertIn("source begun 0 produced 0", raw)
        self.assert_every_device_error_is_a_failed_request(events)

    def test_a_reconfiguration_does_not_clear_the_state_of_the_session(self) -> None:
        # A new sensor with a new source, the session's own health: the request drains, the new source is not touched.
        done = subprocess.run([str(self.probe), "reconfigured"], check=True, capture_output=True, text=True, timeout=60)
        self.assertIn("fresh source begun 0 produced 0", done.stdout)
        self.assertIn("ERROR_REQUEST f20", done.stdout, "the request that met the loss told the framework; this one only fails")
        self.assertIn("BUFFER f20 s1 ERROR", done.stdout)

    def test_a_healthy_session_and_the_other_failures_are_as_before(self) -> None:
        _, healthy = self.run_probe("healthy")
        self.assertEqual(healthy[0], "SHUTTER f1")
        self.assertEqual(sorted(healthy[1:]), ["BUFFER f1 s1 OK", "BUFFER f1 s2 OK"])
        values, events = self.run_probe("all-failed")
        self.assertEqual(events[0], "ERROR_REQUEST f3", "a request none of whose outputs was produced is a failed request")
        self.assertEqual(values["lost"], "0", "and not a lost device")
        self.assertEqual(values["metadata"], "reset")
        values, events = self.run_probe("some-failed")
        self.assertEqual(events[0], "SHUTTER f4", "a request that lost some of its outputs goes on")
        self.assertIn("ERROR_BUFFER f4 s2", events)
        self.assertEqual(values["metadata"], "kept")


class WiringTests(unittest.TestCase):
    """The rest of the series, read as text: structural, not executed."""

    @classmethod
    def setUpClass(cls) -> None:
        files = rebuilt_files()
        cls.header = files[V4L2_HEADER]
        cls.source = files[V4L2_SOURCE]
        cls.sensor = files[SENSOR]
        cls.interface = added_lines(FRAME_SOURCE)
        cls.session = added_lines(SESSION)
        cls.sensor_header = added_lines(SENSOR_HEADER)
        cls.sensor_setup = added_lines(SENSOR_SETUP)

    def test_a_frame_source_is_not_lost_unless_it_says_so(self) -> None:
        self.assertRegex(self.interface, r"virtual bool DeviceLost\(\) const \{\s*return false;\s*\}")
        self.assertRegex(self.interface, r"virtual void SetSessionHealth\(std::shared_ptr<andarium::camera::SessionHealth>")

    def test_the_v4l2_source_reports_the_loss_and_keeps_the_sessions_health(self) -> None:
        self.assertRegex(self.header, r"bool DeviceLost\(\) const override \{\s*return device_lost_;\s*\}")
        self.assertRegex(self.header, r"std::atomic<bool> device_lost_\{false\};")
        self.assertRegex(self.header, r"void SetSessionHealth\(std::shared_ptr<andarium::camera::SessionHealth> health\) override")
        self.assertIn("andarium::camera::ReopenGate reopen;", self.header)
        self.assertIn("camera_request.hpp", self.header, "the rule lives in the overlay's request header")

    def test_only_a_failed_buffer_of_a_video_stream_is_asked_about_the_loss(self) -> None:
        # The one place that notes a failed buffer is the video path's error branch: a failed still (ProduceBlob) never ends a
        # camera, and nothing that does not fail is asked.
        self.assertEqual(self.source.count("NoteFailedBuffer(camera, *pending.stream)"), 1)
        produce_frame = block(self.source[self.source.index("status_t V4l2FrameSource::ProduceFrame("):], ") {")
        failed = block(produce_frame, "if (completion.error) {")
        self.assertRegex(failed, r"NoteFailedBuffer\(camera, \*pending\.stream\);\s*return UNKNOWN_ERROR;")
        produce_blob = self.source[self.source.index("status_t V4l2FrameSource::ProduceBlob("):]
        produce_blob = produce_blob[:produce_blob.index("\n}\n")]
        self.assertNotIn("NoteFailedBuffer", produce_blob)

    def test_the_decision_is_the_overlay_rule_over_the_newest_record_and_the_sessions_kind(self) -> None:
        note = block(self.source, "void V4l2FrameSource::NoteFailedBuffer(Camera* camera, const V4l2Stream& stream) {")
        self.assertIn("camera->meta->Latest(&latest)", note, "the camera's newest metadata record decides, not a guess")
        self.assertRegex(note, r"andarium::camera::camera_lost\(\s*stream\.params\(\)\.kind == V4l2Stream::Kind::kVideo,\s*true,")
        self.assertIn("health_->response() == andarium::camera::LossResponse::device_error", note)
        self.assertIn("device_lost_.exchange(true)", note, "an encoder session latches the loss, and logs it once")
        self.assertIn("camera->reopen.note_loss();", note, "any other session asks for its streams to be reopened")

    def test_the_streams_are_closed_between_requests_and_only_the_video_ones(self) -> None:
        begin = block(self.source, "status_t V4l2FrameSource::BeginRequest(const LogicalCameraSettings& settings, Buffers* buffers) {")
        taken = begin.index("pending_.clear();")
        reopen = begin.index("entry.second.reopen.take(reopen_now)")
        self.assertLess(taken, reopen, "after the buffers of earlier requests were taken back")
        self.assertIn("CloseVideoStreams(&entry.second);", begin)
        close = block(self.source, "void V4l2FrameSource::CloseVideoStreams(Camera* camera) {")
        self.assertIn("V4l2Stream::Kind::kVideo", close)
        self.assertIn("camera->streams.erase(it)", close)

    def test_the_sensor_latches_the_loss_in_the_session_and_reports_the_device_gone_once(self) -> None:
        self.assertIn("session_health_->latch_lost();", self.sensor)
        self.assertEqual(self.sensor.count("claim_device_error()"), 2, "the draining path and the loss path claim it")
        self.assertEqual(self.sensor.count("fail_request(session_health_->claim_device_error());"), 1)
        self.assertEqual(self.sensor.count("fail_request(device_lost && session_health_->claim_device_error());"), 1)
        self.assertNotIn("frame_source_->DeviceLost() ? ErrorCode::kErrorDevice", self.sensor)

    def test_a_failed_request_is_always_told_to_the_framework_and_the_device_gone_comes_besides(self) -> None:
        fail = block(self.sensor, "auto fail_request = [&](bool device_error) {")
        self.assertEqual(fail.count("ErrorCode::kErrorDevice"), 1, "the one place that names the device error")
        self.assertEqual(fail.count("ErrorCode::kErrorRequest"), 1)
        device = block(fail, "if (device_error) {")
        self.assertIn("ErrorCode::kErrorDevice", device, "only the request that claimed it")
        self.assertNotIn("ErrorCode::kErrorRequest", device, "the request error does not depend on the claim")
        self.assertLess(fail.index("ErrorCode::kErrorDevice"), fail.index("ErrorCode::kErrorRequest"), "after the device error")
        self.assertEqual(self.sensor.count("ErrorCode::kErrorDevice"), 1)

    def test_the_opened_session_owns_the_health_and_guards_its_entries(self) -> None:
        self.assertRegex(self.session, r"emulated_sensor->SetSessionHealth\(health_\);")
        # processCaptureRequest (SubmitRequestLocked), flush, configureStreams (ConfigurePipeline) and BuildPipelines.
        self.assertGreaterEqual(self.session.count("if (health_->lost()) {"), 4)
        self.assertEqual(self.session.count("return UNKNOWN_ERROR;"), 4, "each answers the internal error")
        self.assertRegex(self.session, r"health_->set_encoder_stream\(HasEncoderStream\(request_config\) \|\|\s*HasEncoderStream\(overall_config\)\);")
        self.assertIn("static_assert(GRALLOC_USAGE_HW_VIDEO_ENCODER == andarium::camera::usage_video_encoder", self.session)
        self.assertIn("std::shared_ptr<andarium::camera::SessionHealth> health_ =", added_lines(f"{HWL}/EmulatedCameraDeviceSessionHWLImpl.h"))
        self.assertIn("SetSessionHealth(std::shared_ptr<andarium::camera::SessionHealth> health)", self.sensor_header)
        self.assertIn("frame_source_->SetSessionHealth(session_health_);", self.sensor_setup)

    def test_a_flush_of_a_session_that_is_over_ends_its_requests_and_answers_the_internal_error(self) -> None:
        # The interface answers an error for a device that met a serious error (only close may follow), and a successful flush
        # promises that no request or buffer is outstanding, which a sensor that holds a frame between its stages cannot promise.
        self.assertRegex(self.session, r"if \(health_->lost\(\)\) \{\n(?:\s*//[^\n]*\n)+\s*if \(request_processor_ != nullptr\) \{\n"
                                       r"\s*request_processor_->Flush\(\);\n\s*\}\n\s*return UNKNOWN_ERROR;\n\s*\}")

    def test_nothing_else_reports_the_device_gone(self) -> None:
        # ERROR_DEVICE ends the session for the framework; an ordinary hiccup must never take that path.
        self.assertNotIn("kErrorDevice", self.source)
        self.assertNotIn("kErrorDevice", self.session)

    def test_the_topic_is_a_patch_of_the_series(self) -> None:
        # Later topics may stack on it: the row names the patch, not its place.
        self.assertIn(TOPIC, patch_names())
        text = (SERIES / TOPIC).read_text(encoding="utf-8")
        self.assertIn("DeviceLost", text)
        self.assertIn("SessionHealth", text)


if __name__ == "__main__":
    unittest.main()
