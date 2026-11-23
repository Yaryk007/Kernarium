#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Smoke-test an Android release's cameras against the launcher's synthetic camera.

The guest is the release on a fresh data disk, started with the macOS launcher, ``--camera=synthetic``
(or ``blocked``) and ``--camera-profile=``, and with the camera layout on the vendor SKU
(``androidboot.product.vendor.sku=<layout>``), as the host's application composes it. The contract is
android/lineage-24/README.md ("Camera") and the layout set is android/lineage-24/tools/camera_layouts.py.
The runner checks, in this order:

- the layout reached Android: the SKU, the media profiles variant that its init trigger selects, and
  exactly the camera features the layout earns (and none beside them), which are also the features the
  characteristics of the cameras Android lists earn (so that a layout that disagrees with the cameras
  is caught, not only a guest that disagrees with the layout);
- the cameras Android lists (``dumpsys media.camera``) are the profile's: "0" the primary back camera (the
  wide lens; the front camera of a host without a back camera), "1" the front camera, and "3" and "4" the
  ultra wide and telephoto lenses a phone has (the host publishes no zoom camera);
- the provider's life (live and blocked): it is a lazy service that init starts with cameraserver, so it
  exits once cameraserver has listed the cameras and holds none, an open of the camera starts it again
  through its interface, and it exits again when the application is stopped, with the cameras still listed
  (``ps -A`` is all a user build offers; a restart of cameraserver itself needs root, so that path is
  the init action the target-files verifier requires, not something this runner exercises);
- live: the camera application previews the synthetic chart (a screenshot is not black), and the camera
  key takes one still, a complete JPEG with Exif data inside the layout's size cap; then, on the same
  boot, it records about four seconds of video with the back camera (a movie file of at least a second
  with a video track), previews and takes a still with the front camera when the profile has one, switches
  the application from photo to video mode (``--switch-rounds`` times, each from a freshly started
  application whose capture is running; a round passes only when the photo activity's pause, the video
  activity's resume and capture after them were seen, and the camera service logged no "Unable to submit
  capture request" and the camera HAL's domain no denial of the composer's file descriptors in the whole
  interval), and the camera HAL has logged no failed buffer ("every buffer of the stream is in flight", a
  request "not queued completely", a buffer "never collected"; a wait that the framework's flush cut short
  is not one);
- a still while the preview runs leaves the preview's stream open: the guest is stopped for a second inside the
  request of the still (three rounds that count, at a delay after the key that the step finds from the camera HAL's
  own report of what it waits for: the synthetic still makes its request in a tenth of a second or so, and a host
  that is slow with the picture holds the camera HAL's request pipeline as long), and the launcher's log shows
  no capture session that closed (the HAL closed no stream, so the host did not close the session and open it again),
  while the preview resumes within the stop and half a second, as the frames SurfaceFlinger presented show;
- recordings keep their sound to real time: the audio track of a recording holds as much audio as its time
  line spans (its stretch, the time line over the coded audio, stays within a bound either way), because a
  player that follows the audio clock skips ahead at every gap in a track that holds less. The check runs on the
  recording above and on a recording during which the thread that serves the Virtio sound device is held for
  1.5 s three times while the guest runs (``--audio-hold-library``: the video goes on and the audio input has
  nothing to read), with the audio capture seen to run first; the audio table has no jump of a stall's length,
  the audio ends with the video, and the HAL says it made silence up. A third recording stops the whole
  emulator process three times for 1.5 s (the guest stands still, and the video has the same gap): the two
  tracks have gaps of about the same length and the audio does not end after the video (no burst);
- blocked: the same cameras are listed, the preview is not the chart on two samples, each taken only while
  the camera application is the resumed activity and the device is awake (the application is brought back
  once; a second absence fails the step), and the camera key takes nothing;
- restart: a live session that previews and takes one still of the primary back camera (the application is asked for
  the back camera and must hold it), and of the last lens of a phone, with a calibration directory of its own that
  the host keeps the colour data of the stills in, then a second boot of the same instance over the directory (the
  layout the host composes for lenses with colour data, RAW among the capabilities), which must list the same camera
  IDs as before and at least one camera with the RAW capability, each with the metadata Android requires of one (the
  lens shading map and hot pixel map modes ON, whether the lens shading is applied). A camera the HAL cannot create
  while it builds its list is gone until the next restart, and the host states RAW for exactly the lenses that have
  colour data, so a RAW camera that the HAL rejects takes the lens it was taught from the guest for good. The second
  session ends with a RAW still of the primary back camera: the application's RAW capture is switched on and the
  still must leave a DNG beside its JPEG;
- the camera switch and the platform's dialog: a blocked session is started with the launcher's hardware input,
  as the host's application starts it, so that the side-key device reports the camera lens cover; Android then
  holds the camera's hardware toggle (``dumpsys sensor_privacy``) and a live one does not. Opening the camera
  application shows no sensor privacy dialog in either (SystemUI's "Unblock device camera?" cannot lift a
  hardware toggle): no window of SystemUI with that dialog's title or Unblock button in the interface dump,
  and SystemUI's SensorUseStartedActivity never started. The guest's main and events logs are captured
  continuously to a file (``--output-dir``, else the run's temporary directory) from before the camera
  application opens until the last check, so those two judgements cover the whole session and not what a
  finite buffer still holds. In a blocked session the sensor privacy service says, within a deadline, that it
  withheld the reminder of the camera application (its log line, which the framework patch adds); in a live
  one that line is absent;
- none: no camera feature and no camera at all.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the
guest is reached through the shared adb server, always named with ``-s``.

The runner is opt-in: it needs the macos-android17 executable, an installed release and an adb client.
usage: run_android_camera.py --emulator uml-darwin-android17 [--release DIR] [--layout NAME|auto]
       [--camera-profile phone|phone-turned-front|tablet|laptop] [--mode live|blocked|none|restart]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import importlib.util
import math
import os
import pathlib
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree
import xml.etree.ElementTree as ET
import zlib
from collections.abc import Callable
from dataclasses import dataclass

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


boot_android = load("boot_android", REPOSITORY / "tools" / "boot_android.py")
camera_layouts = load("camera_layouts", REPOSITORY / "android/lineage-24/tools/camera_layouts.py")

DEFAULT_ADB_PORT = 5580
DEFAULT_VNC_PORT = 5993
# The guest's VNC display (only with --hardware-input) takes the slot's second port.
VNC_OFFSET = 1
HARDWARE_INPUT_OPTION = "--hardware-input"
CAMERA_PACKAGE = "org.lineageos.aperture"
CAMERA_PERMISSIONS = ("android.permission.CAMERA", "android.permission.RECORD_AUDIO")
CAMERA_KEYCODE = 27  # KEYCODE_CAMERA: the camera application's default shutter key
STILL_DIRECTORY = "/sdcard/DCIM/Camera"
# The lazy provider's process, as `ps -A` names it (its binary: android.hardware.camera.provider@2.7-service-google-lazy),
# and how long it may run after the last camera is closed: the service manager tells a lazy service that it
# has no clients on a timer of seconds.
PROVIDER_PROCESS = "android.hardware.camera.provider"
PROVIDER_START_SECONDS = 60
PROVIDER_EXIT_SECONDS = 240
COMMAND_SECONDS = 120
PREVIEW_SECONDS = 90
STILL_SECONDS = 60
NO_STILL_SECONDS = 25
SETTLE_SECONDS = 30
SCREENSHOT_SECONDS = 120
DISPLAY_WIDTH = 720
DISPLAY_HEIGHT = 1280
# The chart of the synthetic camera has colour patches and slanted edges: its luma varies a lot, where
# a black frame, or a blocked session's, varies by compression noise at most.
CHART_MINIMUM_DEVIATION = 12.0
BLACK_MAXIMUM_DEVIATION = 6.0
BLACK_MAXIMUM_MEAN = 24.0
# The preview's share of the screen: the middle, clear of the application's bars.
PREVIEW_BOX = (0.2, 0.25, 0.8, 0.6)
# The stills a layout's tier allows, in pixels (small: 12 megapixels, large: 48), and the least a still
# can be for the picture to mean anything.
STILL_MAXIMUM_PIXELS = {"small": 12_600_000, "large": 50_400_000}
STILL_MINIMUM_SIDE = 480
# A blocked session's preview is sampled twice, this far apart, so that a chart that appears late
# (a camera that opened after all) is seen.
BLOCKED_SAMPLES = 2
BLOCKED_SAMPLE_GAP_SECONDS = 10
# A blocked sample counts only while the camera application is in front; this long after it was brought back.
BRING_BACK_SETTLE_SECONDS = 8
# What is on the screen: the resumed activity, the focused window and the display's power state.
FOREGROUND_COMMAND = ("dumpsys activity activities | grep -a -E 'ResumedActivity'; "
                      "dumpsys window | grep -a -E 'mCurrentFocus|mFocusedApp'; "
                      "dumpsys power | grep -a -E 'mWakefulness='")
# The camera application's own events at a failure: its crashes, ANRs, process deaths and the windows
# (dialogs) that came in front of it.
APP_EVENTS_PATTERN = (f"{CAMERA_PACKAGE}|FATAL EXCEPTION|ANR in|am_crash|am_anr|am_proc_died|am_kill|"
                      "ActivityTaskManager|WindowManager: (Resumed|Destroying|Changing focus)")
APP_EVENTS_LINES = 80
# The sensor privacy service's state (dumpsys sensor_privacy lists, for each user and sensor, each toggle that
# is set) and the activities it starts over an application that uses a blocked sensor.
SENSOR_CAMERA = 2
TOGGLE_HARDWARE = 2
STATE_ENABLED = 1
SENSOR_DIALOG_ACTIVITY = "com.android.systemui/.sensorprivacy.SensorUseStartedActivity"
SENSOR_DIALOG_TITLE_ID = "sensor_use_started_title_message"
SENSOR_DIALOG_PACKAGE = "com.android.systemui"
SENSOR_DIALOG_TEXTS = ("Unblock", "Unblock device camera?")
# The line the sensor privacy service logs (SensorPrivacyService, the framework patch of route p0007) when it
# withholds the reminder of a camera that the hardware toggle blocks, with the package in parentheses.
SENSOR_REMINDER_LINE = "kernarium: no reminder for a camera that the hardware toggle blocks"
SENSOR_REMINDER_SECONDS = 90
# The continuous capture of the guest's main and events logs: its first lines come within this long of its start.
CAPTURE_START_SECONDS = 30
CAPTURE_STOP_SECONDS = 10
CAPTURE_BUFFERS = ("main", "events")
SENSOR_CAPTURE_PATTERN = "SensorPrivacy|SensorUseStartedActivity|kernarium: no reminder"
SENSOR_CAPTURE_LINES = 30
UI_DUMP_PATH = "/data/local/tmp/camera-runner-ui.xml"
UI_DUMP_ATTEMPTS = 4
UI_DUMP_GAP_SECONDS = 4
# The application's own log lines (its errors, what it logged before it left) come by process id: they do not name the package.
APP_OWN_LOG_LINES = 80
CONCURRENT_FEATURE = "android.hardware.camera.concurrent"
# The camera application's intents: its video mode, and the extra that picks the front camera.
VIDEO_ACTION = "android.media.action.VIDEO_CAMERA"
FRONT_EXTRA = "android.intent.extra.USE_FRONT_CAMERA"
# Video: how long the shutter key's recording runs, how long the file may take to appear, the movie
# duration it must have (the recording is shorter than the hold by the application's start-up), and the
# average frame rate of its video track (the preview and the recording share one camera exposure: a rate
# near half of the camera's means they took an exposure each).
VIDEO_HOLD_SECONDS = 5
VIDEO_SECONDS = 90
VIDEO_MINIMUM_SECONDS = 1.0
VIDEO_MAXIMUM_SECONDS = 15.0
VIDEO_MINIMUM_FPS = 20.0
VIDEO_URI = "content://media/external/video/media"
# The audio of a recording: the AAC-LC encoder takes this many samples a frame, so the audio a track holds is its
# sample count times this, and its time line the total duration of its samples. The time line over the coded audio
# is the track's stretch: 1.0 for a track that holds what its time stamps span. A recorder stamps each buffer it reads
# with the time of the read, so a gap of stamps with no audio behind it (an input that makes up too few frames for a
# stall) stretches the time line, and a player that follows the audio clock skips ahead at every such gap and plays
# the clip too fast. A good recording stays within a percent of 1.0; the bounds leave room for the encoder's rounding
# and are the same either way (a time line shorter than the audio it holds is as wrong as a longer one).
AAC_FRAME_SAMPLES = 1024
AUDIO_STRETCH_MINIMUM = 0.95
AUDIO_STRETCH_MAXIMUM = 1.05
# A jump in the audio's sample table (the longest time between two audio samples) of about a stall's length is what an
# input that does not make a stalled time up leaves; the encoder's own steps are a few hundredths of a second, and
# half a second is a third of a stall.
AUDIO_JUMP_MAXIMUM_SECONDS = 0.5
# Where the tracks end: a recording's audio ends with its video (a control's differ by a few hundredths of a second).
# Frames that were made up for a stall and arrive in a burst are stamped at its end and laid after it, so the audio
# then ends as many seconds after the video as the stall lasted.
AUDIO_END_SKEW_MAXIMUM_SECONDS = 0.25
# The recordings with a disturbance. Each runs DISTURB_HOLD_SECONDS from the start key to the stop key. The audio
# capture is seen to run first (the audio flinger's record thread is out of standby, has an active client and reads
# more frames between two looks); one second later, and then at DISTURB_GAP_SECONDS after each end, DISTURB_COUNT
# disturbances follow. A stall holds the thread that serves the Virtio sound device for STALL_SECONDS while the guest
# runs, as a busy host does to the worker the device's capture timeline runs on: the video goes on and the audio
# input has nothing to read. A freeze stops the whole emulator process for FREEZE_SECONDS: the guest stands still and
# the video has the same gap. The runner cannot tell where in the input's read cycle a disturbance lands (in a read
# or between two); the model of the input covers the phases, and the step reports when each disturbance began.
STALL_SECONDS = 1.5
FREEZE_SECONDS = 1.5
# A still that holds the camera HAL's request pipeline: the emulator process is stopped HELD_STILL_SECONDS inside
# the request of the camera key (a host that is slow with the picture holds the pipeline as long). The HAL used to close
# the stream of an output that its request pipeline had left unused for half a second, which a pipeline held by a
# still does at the next request, and the host then closed its capture session and opened it again. The stop has to
# land inside the still's own request, which the synthetic still makes in a tenth of a second or so, reached some
# 0.15 to 0.3 s after the key command returns and by an amount that varies by 0.03 s from key to key and with the
# size of the still: no fixed delay lands there often enough. So a round counts only when the stop demonstrably
# overlapped a still's request, and the delay after the key is found as the rounds go. The camera HAL's result
# dispatcher reports the buffers it waits for once they are half a second late (the stop makes them late): when the
# oldest of them is the still's, the stop was inside the request; when a still's buffer waits behind older ones of the
# preview, it came too early (the delay grows); when no still's buffer is pending, too late (it shrinks). The step runs
# rounds until HELD_STILL_ROUNDS of them counted, at most HELD_STILL_ATTEMPTS. In every round the preview must come
# back within the stop as it lasted (measured, not as requested) and HELD_STILL_RESUME_SECONDS (the host's restart of
# its capture session, which a closed stream costs, is not in that on the synthetic camera: the launcher's lines tell).
HELD_STILL_DELAY_START = 0.20
HELD_STILL_DELAY_STEP = 0.04
HELD_STILL_DELAY_SMALLEST_STEP = 0.01
HELD_STILL_DELAY_RANGE = (0.06, 0.60)
HELD_STILL_ROUNDS = 3
HELD_STILL_ATTEMPTS = 16
HELD_STILL_SECONDS = 1.0
HELD_STILL_RESUME_SECONDS = 0.5
HELD_STILL_OBSERVE_SECONDS = 4.0
# SurfaceFlinger keeps the last 128 presented frames of a layer (about 4 seconds of a 30 fps preview), so the times are
# read every HELD_STILL_POLL_SECONDS from the moment the emulator is resumed, and put together.
HELD_STILL_POLL_SECONDS = 0.5
# A report of the dispatcher belongs to a round when it comes within this much of the end of its stop.
HELD_STILL_REPORT_SECONDS = 1.0
DISTURB_COUNT = 3
DISTURB_LEAD_SECONDS = 1.0
DISTURB_GAP_SECONDS = 1.5
DISTURB_HOLD_SECONDS = 14
DISTURB_MAXIMUM_SECONDS = 25.0
AUDIO_CAPTURE_SECONDS = 30
AUDIO_HOLD_WAIT_SECONDS = 15
# The library must have held the thread for most of the time asked, and the HAL must say it made silence up for at
# least half of the time that the stalls took (the host's capture credit leaves a little real audio at each thaw).
AUDIO_HOLD_MINIMUM_FRACTION = 0.9
MADE_UP_MINIMUM_FRACTION = 0.5
# A stall must leave the video alone (a stalled guest would excuse the audio's gap), a freeze must show in the video
# as a gap, and the audio's gaps must be about as long as the video's.
VIDEO_GAP_MAXIMUM_SECONDS = 0.5
FREEZE_GAP_MINIMUM_SECONDS = 1.0
GAP_AGREEMENT_MAXIMUM_SECONDS = 0.35
# The input's own log lines (the Stream's tag in the audio HAL).
HAL_STREAM_TAG = "AHAL_StreamAlsaMonoPipe"
# The camera HAL's own tags: a failed buffer is an error or a warning of one of these.
HAL_TAGS = ("V4l2Stream", "V4l2FrameSource", "EmulatedSensorHost")
# The photo to video switch: the application's video activity covers its photo activity while that activity's
# preview still repeats, and the last preview buffers come back with the display composer's fence. The camera
# service hands such a fence to the HAL with the next request, which the policy must let the HAL use (overlay/
# sepolicy/vendor/hal_camera_default.te). A refusal shows as an SELinux denial of the camera HAL's domain using
# the composer's file descriptor and as the camera service's "Unable to submit capture request" for the request
# it could not submit; the application then gets ERROR_CAMERA_DEVICE and reopens the camera, which costs the
# switch about a second more. It happens in some switches only (in six rounds each, from a freshly started
# application, 2 were refused with the window animations on and 4 with them off, which does not tell the two
# apart), so the step repeats the switch, each time from a freshly started application and with the animations
# off (restored afterwards). A round counts only when the switch was seen: the photo activity's pause and the
# video activity's resume, with capture running before and after.
CAMERA_HAL_DOMAIN = "hal_camera_default"
COMPOSER_HAL_DOMAIN = "hal_graphics_composer_default"
SWITCH_ROUNDS = 4
SWITCH_HOLD_SECONDS = 10
SWITCH_SETTLE_SECONDS = 8
SWITCH_OBSERVE_SECONDS = 30
SWITCH_DRAIN_SECONDS = 5
SWITCH_MARKER_TAG = "CamSwitch"
HELD_STILL_MARKER_TAG = "CamHeldStill"
ANIMATION_SCALES = ("window_animation_scale", "transition_animation_scale", "animator_duration_scale")

# The cameras each synthetic profile presents, and the camera IDs Android gives them
# (android/lineage-24/overlay/camera/hal/camera_ids.hpp).
PROFILE_CAMERAS = {
    "phone": ("back+front", ("0", "1", "3", "4")),
    "phone-turned-front": ("back+front", ("0", "1", "3", "4")),
    "tablet": ("back+front", ("0", "1")),
    "laptop": ("front", ("0",)),
}
# The layout each synthetic profile earns on a host that has not learnt its lenses' colour data: the phone's
# back lenses have flash and manual control, its wide and front cameras stream together, and its front camera
# records 2160p; the tablet's back camera has flash and manual control, and its front camera cannot make the
# guaranteed concurrent stream sizes; the laptop has one front camera (darwin/platform/synthetic_camera). The
# host offers RAW only for a lens whose colour data an earlier still has kept (darwin/device/camera/README.md,
# Calibration), so a fresh host composes these layouts and the same host after a still the -raw ones, which
# --layout states for a host that has a calibration directory.
DEFAULT_LAYOUTS = {
    "phone": "bf-large-flash-manual-concurrent-front2160",
    "phone-turned-front": "bf-large-flash-manual-concurrent-front2160",
    "tablet": "bf-small-flash-manual",
    "laptop": "front",
}
MODES = ("live", "blocked", "none", "restart", "video-lost", "video-leave")
# The video-lost mode: the synthetic camera reads its test-only control pipe, and the runner tells it that the application
# left the foreground (the host ends the video of a camera that has a video stream open; docs/testing.md). The recording
# ends by itself and the clip, with encoded video samples, is saved, the application stays the resumed activity, and the
# camera opens again when the application is started again after the person returns: LOST_HOLD_SECONDS of video before
# the loss, a clip of at least LOST_MINIMUM_SECONDS, and LOST_WAIT_SECONDS for the clip to appear. The clip ends with the
# camera: it lasts at most LOST_END_SECONDS more than the video before the loss (the camera application ends the recording
# when the camera reports its recoverable error, and CameraX opens the camera again about a second after the loss; what the
# camera delivers after that must not reach the clip), its video has no step of VIDEO_JUMP_MAXIMUM_SECONDS (a gap where the
# camera was away) and the frame rate of a recording, and its audio holds what its time stamps span and ends with its video.
# None of it looks at the pictures: the recording does not go on over the camera's reopening is what the row shows.
CONTROL_VARIABLE = "UML_SYNTHETIC_CAMERA_CONTROL"
# The video-leave mode: the launcher plays the application's leaving the foreground to the guest through a second pipe
# (EDGE_VARIABLE, darwin/app/macos/edge_test/background_edge_control.hpp): the camera's running video ends, the launcher waits until
# the guest has stopped its video streams, and then it holds the guest's GPU requests (the request gate the application
# closes as it enters the background) for LEAVE_AWAY_SECONDS. A guest that had not acted on the lost video when the hold
# landed stalls its camera pipeline until the requests are released: the recording stops only then, and its sound goes on
# in silence for as long as the hold lasts. The row makes LEAVE_TRIALS recordings and leaves each, with the platform's
# notice of the departure (`interrupt background`) first or LEAVE_NOTICE_GAPS later, and expects of every one: the launcher
# found a video to end and the camera answered, the camera HAL's report of the loss and the recorder's stop came before the
# hold landed (the gate's closing, which the launcher times on its own; not the end of its wait for a request that was
# running), the loss was the HAL's first failed buffer, and the clip's audio ends with its video. Whether the guest had
# stopped its video streams when the hold landed is told, not required: a loaded host can outlast the wait's bound, and the
# recording that ended before the hold is what the row is for.
EDGE_VARIABLE = "UML_BACKGROUND_EDGE_CONTROL"
LEAVE_TRIALS = 10
LEAVE_AWAY_SECONDS = 3.0
LEAVE_NOTICE_GAPS = (0.0, 0.02)
LEAVE_AUDIO_TAIL_MAXIMUM_SECONDS = 1.0
# How far the guest's clock, read against the host's, may be out at least (a busy host's round trips are longer: the
# shortest of seven, halved, is what a trial allows when it is more).
LEAVE_CLOCK_MARGIN_SECONDS = 0.15
# The camera device's line when the guest did not read the interrupted record of a lost video within its bound.
RECORD_BOUND_LINE = "the guest did not read the interrupted record of the lost video"
LEAVE_EDGE_SECONDS = 20
LOST_HOLD_SECONDS = 4
LOST_MINIMUM_SECONDS = 2.0
LOST_END_SECONDS = 2.0
LOST_WAIT_SECONDS = 60
# The recording is closed when the application stops it, and its screen shows it until it finalizes, about a second later: the
# pause and resume button is pressed LOST_TAPS times at once after the loss, and the application ignores the presses in that window
# (it logs each one: the log counts how many landed inside it). The button is on the screen only while a recording runs, so its
# place is read from the screen before the loss.
LOST_TAPS = 3
UI_STATE_BUTTON = "videoRecordingStateButton"
# The application's log lines of the stop (CameraViewModel, debug level) and of an error (CameraActivity).
APP_LOG_TAGS = "CameraViewModel:D CameraActivity:E"
APP_IGNORED_LINE = "is ignored, the video recording ends"
# A press of the pause and resume button that finds no recording (the Finalize had reset the state): logged and ignored too.
APP_NO_RECORDING_LINE = "is ignored, no recording runs"
APP_CANCELLED_LINE = "The start of the video recording is cancelled"
APP_STOPPED_LINE = "The video recording is stopped"
APP_FINALIZE_LINE = "Video capture succeeded"
# A loss inside the half second between the start key and the creation of the recording (the start sound): there is no recording
# to stop, the application cancels the start. START_WINDOW_ATTEMPTS keys, each followed at once by the loss, hit the window
# probably, and the application's log says which did. After the camera is back and START_WINDOW_SETTLE_SECONDS, the key either
# starts a recording (the start was cancelled) or stops one that survived the loss, which lasts as long as the wait. A recording
# lasts no longer than the time between the key that starts it and the key that stops it, which the runner measures (at least
# START_WINDOW_HOLD_SECONDS: a guest under load can be slow to deliver a key), and the clip of the attempt ends within
# START_WINDOW_MARGIN_SECONDS of that; it holds encoded video, as the recording after the return does.
START_WINDOW_ATTEMPTS = 5
START_WINDOW_SETTLE_SECONDS = 4
START_WINDOW_HOLD_SECONDS = 3
START_WINDOW_MINIMUM_SECONDS = 2.0
START_WINDOW_MARGIN_SECONDS = 1.5
# An attempt is conclusive when the application's log says what the loss did: it cancelled the start, or it stopped a recording that
# had started. Without either (the loss came before the key reached the application, or the image lacks the topic) the clip of the
# attempt is not credited as the recording after the return. The attempts are a probabilistic hit on a window half a second wide: the
# row needs START_WINDOW_CONCLUSIVE_MINIMUM conclusive ones, and one of them must have cancelled a start.
START_WINDOW_CONCLUSIVE_MINIMUM = 3
# The longest time between two samples of a video track that a recording may have: a camera that stalls or is away leaves a
# step of its length, where the encoder's own steps are a frame (a few hundredths of a second).
VIDEO_JUMP_MAXIMUM_SECONDS = 0.5
# The viewfinder cases. The person is away LOST_AWAY_SECONDS, and when they return the viewfinder is live again within
# VIEWFINDER_RECOVERY_SECONDS of the return (one deadline for the camera, the picture and the frame counters), in the same
# process and with no error window. The video mode's viewfinder (no recording) binds an encoder surface, so the HAL ends the
# camera for it as for a recording; the photo mode's viewfinder has no encoder stream, so the HAL must not: it closes the
# camera's video streams and opens them again, which its log says.
# A coded video sample is at least this many bytes on average: a file whose `mdat` boxes hold less than its declared samples need
# has a sample table and no encoded video.
MINIMUM_SAMPLE_BYTES = 16
LOST_AWAY_SECONDS = 8
VIEWFINDER_RECOVERY_SECONDS = 45
HAL_END_LINE = "the host ended the camera: its video buffers fail"
HAL_REOPEN_LINE = "the host ended the camera's video: its video streams are closed and opened again"
# The camera service logs, when it closes a device (CameraService's Camera3Device::disconnectImpl, tag Camera3-Device, error level),
# every stream that something still holds a reference to. A frame whose request the HAL ended without telling the framework that
# the request failed keeps its buffers, and with them its streams' references and the application's preview surface, until the
# camera service ends: the application cannot open the camera again. The line does not depend on how fast the application reacts.
SERVICE_LOG_TAG = "Camera3-Device"
STREAM_LEAK = re.compile(r"\bStream \d+ leaked! strong reference \(\d+\)!")
STILL_ACTION = "android.media.action.STILL_IMAGE_CAMERA"
ERROR_WINDOW = re.compile(r"Application (Error|Not Responding)|isn't responding|has stopped|keeps stopping")
# The cameras Android lists again after the restart: the lazy provider starts when cameraserver first asks.
RESTART_LIST_SECONDS = 60
CALIBRATION_SECONDS = 30  # the host writes the colour data of a still when the still is done
# The application starts on the primary back camera, whose first still teaches the host its colour data (a lens
# keeps what a Bayer still of it shows). A phone has more lenses, and the restart mode takes a still on the last
# lens button too, the telephoto lens, which is the camera with this ID, so that the colour data of more than one
# lens has to survive the restart. A host with a single back camera lists no buttons.
LENS_SELECTOR = "lensSelectorLayout"
LAST_LENS_CAMERA = {"phone": "4", "phone-turned-front": "4"}
# The first session starts the application on the primary back camera: it keeps the camera it last used, which an
# instance of an earlier run may have left on the front one.
PRIMARY_BACK_CAMERA = "0"
UI_DUMP = "/sdcard/runner-ui.xml"
UI_BOUNDS = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")
UI_DUMP_ATTEMPTS = 3
# The RAW still: the application writes a DNG beside its JPEG once its setting is on (the pro mode button opens a
# row with the settings button, and the setting is a row of the settings list, below the first screen).
PRO_BUTTON = "proButton"
SETTINGS_BUTTON = "settingsButton"
RAW_SETTING = "Enable RAW image capture"
RAW_STILL_SECONDS = 90


class Failure(RuntimeError):
    pass


# ---- The plan: what a run expects, from the layout and the profile alone.

@dataclass(frozen=True)
class Plan:
    layout: object  # camera_layouts.Layout
    profile: str
    mode: str
    camera_ids: tuple[str, ...]
    features: frozenset[str]
    media_variant: str
    switch_rounds: int = SWITCH_ROUNDS
    leave_trials: int = LEAVE_TRIALS

    @property
    def still_maximum_pixels(self) -> int:
        return STILL_MAXIMUM_PIXELS[self.layout.tier or "small"]


def make_plan(layout_name: str, profile: str, mode: str, switch_rounds: int = SWITCH_ROUNDS,
              leave_trials: int = LEAVE_TRIALS) -> Plan:
    """The expectations of one run; a layout that does not describe the profile's cameras is refused,
    because the features would then promise cameras the guest lacks."""
    layouts = camera_layouts.by_name()
    if layout_name not in layouts:
        raise ValueError(f"{layout_name!r} is not a camera layout")
    if profile not in PROFILE_CAMERAS:
        raise ValueError(f"{profile!r} is not a synthetic camera profile")
    if mode not in MODES:
        raise ValueError(f"{mode!r} is not a mode")
    if switch_rounds < 1:
        raise ValueError("a run makes at least one photo to video switch")
    if leave_trials < 1:
        raise ValueError("a run makes at least one leaving the foreground")
    layout = layouts[layout_name]
    if mode == "none":
        if layout.cameras != "none":
            raise ValueError("the layout of a guest without a camera device is none")
        return Plan(layout, profile, mode, (), frozenset(), layout.media_variant, switch_rounds, leave_trials)
    cameras, ids = PROFILE_CAMERAS[profile]
    if layout.cameras != cameras:
        raise ValueError(
            f"the {profile} profile presents {cameras} cameras and the layout {layout_name} describes "
            f"{layout.cameras}")
    return Plan(layout, profile, mode, ids, frozenset(layout.features), layout.media_variant, switch_rounds, leave_trials)


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

def parse_camera_features(text: str) -> set[str]:
    """`pm list features`: the camera features."""
    return {match[1] for line in text.splitlines()
            if (match := re.fullmatch(r"feature:(android\.hardware\.camera[\w.]*)(?:=\d+)?", line.strip()))}


def parse_camera_ids(text: str) -> list[str]:
    """`dumpsys media.camera`: the IDs of the camera devices, in the order the service lists them."""
    return re.findall(r'^\s*Device \d+ maps to "([^"]+)"', text, re.MULTILINE)


def active_client_packages(text: str) -> set[str]:
    """`dumpsys media.camera`: the packages in the Active Camera Clients block. The service prints a
    list in brackets, `[]` when empty and otherwise one `(Camera ID: .., Client Package Name: .., ..)`
    line per client between a `[` line and a `]` line."""
    lines = text.splitlines()
    packages: set[str] = set()
    for index, line in enumerate(lines):
        if line.strip() != "Active Camera Clients:":
            continue
        for client in lines[index + 1:]:
            if client.strip() in ("[]", "]"):
                break
            if match := re.search(r"Client Package Name: ([\w.]+)", client):
                packages.add(match[1])
        break
    return packages


def active_client_camera_ids(text: str, package: str) -> set[str]:
    """`dumpsys media.camera`: the IDs of the cameras the package holds, from its lines of the Active
    Camera Clients block (`(Camera ID: 1, Cost: 100, PID: .., Client Package Name: .., ..)`)."""
    return set(re.findall(rf"Camera ID: (\w+),[^\n]*?Client Package Name: {re.escape(package)}\b", text))


def parse_content_rows(text: str) -> list[dict[str, str]]:
    """`content query`: one dict per `Row: N key=value, key=value` line. A value runs to the next `, key=`."""
    rows = []
    for line in text.splitlines():
        if not (head := re.match(r"Row: \d+ (.*)", line.strip())):
            continue
        rows.append({match[1]: match[2] for match in re.finditer(r"(\w+)=(.*?)(?=, \w+=|$)", head[1])})
    return rows


@dataclass
class Movie:
    seconds: float
    video: bool
    audio: bool
    frames: int = 0
    video_seconds: float = 0.0
    audio_samples: int = 0       # the audio track's samples (AAC frames)
    audio_seconds: float = 0.0   # its time line: the total duration of those samples
    audio_rate: int = 0          # its sample rate, from its sample entry (the time scale when there is none)
    video_end: float = 0.0       # where each track ends on the movie's time line (edit list applied)
    audio_end: float = 0.0
    video_gap: float = 0.0       # the longest time between two samples of each track
    audio_gap: float = 0.0
    media_bytes: int = 0         # the bytes of the file's `mdat` boxes: the encoded samples themselves

    @property
    def fps(self) -> float:
        """The average frame rate of the video track: its frames over its duration, 0 without a sample table."""
        return self.frames / self.video_seconds if self.video_seconds > 0 else 0.0

    @property
    def audio_coded_seconds(self) -> float:
        """The audio the track holds: its AAC frames at AAC_FRAME_SAMPLES samples each, 0 without a sample table."""
        return self.audio_samples * AAC_FRAME_SAMPLES / self.audio_rate if self.audio_rate > 0 else 0.0

    @property
    def audio_stretch(self) -> float:
        """The audio track's time line over the audio it holds: 1.0 for a track whose time stamps span what it
        holds, more when stamped time has no audio behind it; 0 without a sample table."""
        coded = self.audio_coded_seconds
        return self.audio_seconds / coded if coded > 0 else 0.0

    @property
    def audio_skew(self) -> float:
        """Where the audio ends minus where the video ends, in seconds: 0 for a recording whose tracks end together,
        positive when the audio runs on past the video (made-up frames laid after a gap of stamps)."""
        return self.audio_end - self.video_end


def mp4_boxes(data: bytes, start: int, end: int):
    """The boxes of an MP4 between two offsets: (type, payload start, box end)."""
    position = start
    while position + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, position)
        header = 8
        if size == 1:
            size, header = struct.unpack_from(">Q", data, position + 8)[0], 16
        elif size == 0:
            size = end - position
        if size < header or position + size > end:
            raise ValueError(f"a box of {size} bytes at {position} does not fit")
        yield kind, position + header, position + size
        position += size


def audio_sample_rate(data: bytes, start: int, end: int) -> int:
    """The sample rate that the first sample entry of an `stsd` box (`start` to `end`) states for its audio: the
    16.16 field at the end of an audio sample entry's fixed part; 0 when the box has no such entry."""
    entry = start + 8  # the box's version and flags, and its entry count
    if entry + 8 > end:
        return 0
    size = struct.unpack_from(">I", data, entry)[0]
    if size < 36 or entry + size > end:
        return 0
    return struct.unpack_from(">I", data, entry + 32)[0] >> 16


@dataclass(frozen=True)
class Track:
    samples: int = 0
    seconds: float = 0.0   # the total duration of its samples
    rate: int = 0          # an audio track's sample rate: its sample entry's, else its time scale
    gap: float = 0.0       # the longest duration of one sample: the longest step of the sample times
    end: float = 0.0       # where its last sample ends on the movie's time line


def edit_offsets(data: bytes, trak: dict, movie_scale: int, media_scale: int) -> tuple[float, float]:
    """(start, skip) of a track from its edit list: the movie time at which its first sample plays (the empty edits
    before it) and the media time its first edit skips; (0, 0) without an edit list."""
    start = skip = 0.0
    edts = trak.get(b"edts")
    if edts is None:
        return start, skip
    boxes = {k: (p, s) for k, p, s in mp4_boxes(data, *edts)}
    if b"elst" not in boxes:
        return start, skip
    position = boxes[b"elst"][0]
    version = data[position]
    entries = struct.unpack_from(">I", data, position + 4)[0]
    position += 8
    seen = False
    for _ in range(entries):
        if version == 1:
            segment, media_time = struct.unpack_from(">Qq", data, position)
            position += 20
        else:
            segment, media_time = struct.unpack_from(">Ii", data, position)
            position += 12
        if media_time == -1:
            start += segment / movie_scale if movie_scale else 0.0
        elif not seen:
            skip, seen = media_time / media_scale, True
    return start, skip


def track_samples(data: bytes, start: int, end: int, handler_type: bytes, movie_scale: int = 0) -> Track:
    """The first track of a `moov` box whose handler is `handler_type` (`vide` or `soun`): the sample count and the
    total sample duration of its `stts`, in the time scale of its `mdhd`, the longest single sample duration, where
    the last sample ends on the movie's time line (its edit list applied) and the sample rate of an audio track's
    sample entry (its time scale when there is no entry); an empty Track when it has no sample table."""
    for kind, payload, stop in mp4_boxes(data, start, end):
        if kind != b"trak":
            continue
        trak = {k: (p, s) for k, p, s in mp4_boxes(data, payload, stop)}
        mdia = trak.get(b"mdia")
        if mdia is None:
            continue
        parts = {k: (p, s) for k, p, s in mp4_boxes(data, *mdia)}
        handler, header = parts.get(b"hdlr"), parts.get(b"mdhd")
        if handler is None or header is None or data[handler[0] + 8:handler[0] + 12] != handler_type:
            continue
        scale = struct.unpack_from(">I", data, header[0] + (20 if data[header[0]] == 1 else 12))[0]
        minf = {k: (p, s) for k, p, s in mp4_boxes(data, *parts[b"minf"])} if b"minf" in parts else {}
        stbl = {k: (p, s) for k, p, s in mp4_boxes(data, *minf[b"stbl"])} if b"stbl" in minf else {}
        if scale == 0 or b"stts" not in stbl:
            return Track()
        table, table_end = stbl[b"stts"]
        entries = struct.unpack_from(">I", data, table + 4)[0]
        if table + 8 + 8 * entries > table_end:
            raise ValueError("an stts table that does not fit its box")
        samples = [struct.unpack_from(">II", data, table + 8 + 8 * index) for index in range(entries)]
        rate = audio_sample_rate(data, *stbl[b"stsd"]) if handler_type == b"soun" and b"stsd" in stbl else 0
        total = sum(count * delta for count, delta in samples) / scale
        offset, skip = edit_offsets(data, trak, movie_scale, scale)
        return Track(sum(count for count, _ in samples), total, rate or scale,
                     max((delta for _, delta in samples), default=0) / scale, offset + total - skip)
    return Track()


def parse_mp4(data: bytes) -> Movie:
    """The duration and the track kinds of an MP4 file: its `ftyp`, and in `moov` the `mvhd`, the handlers and the
    sample tables of the video and the audio track."""
    boxes = {kind: (payload, end) for kind, payload, end in mp4_boxes(data, 0, len(data))}
    if b"ftyp" not in boxes or b"moov" not in boxes:
        raise ValueError("no ftyp and moov: not a finished movie")
    start, end = boxes[b"moov"]
    inner = {kind: (payload, stop) for kind, payload, stop in mp4_boxes(data, start, end)}
    if b"mvhd" not in inner:
        raise ValueError("no mvhd")
    payload, _ = inner[b"mvhd"]
    if data[payload] == 1:
        scale, duration = struct.unpack_from(">IQ", data, payload + 20)
    else:
        scale, duration = struct.unpack_from(">II", data, payload + 12)
    if scale == 0:
        raise ValueError("a time scale of zero")
    moov = data[start:end]
    video = track_samples(data, start, end, b"vide", scale)
    audio = track_samples(data, start, end, b"soun", scale)
    media = sum(stop - payload for kind, payload, stop in mp4_boxes(data, 0, len(data)) if kind == b"mdat")
    return Movie(duration / scale, b"vide" in moov, b"soun" in moov, video.samples, video.seconds,
                 audio.samples, audio.seconds, audio.rate, video.end, audio.end, video.gap, audio.gap, media)


def active_capture_frames(text: str) -> int | None:
    """The `Frames read` of the audio flinger's record thread that is capturing in a `dumpsys media.audio_flinger`:
    an input thread of type RECORD that is out of standby with an active client. None when no record thread is.
    The history of removed threads (lines that start with a dash) is not a thread."""
    capturing: int | None = None
    section: list[str] | None = None
    sections: list[list[str]] = []
    for line in text.splitlines():
        if re.match(r"Input thread .*\(RECORD\):\s*$", line):
            section = []
            sections.append(section)
        elif section is not None and line[:1] not in (" ", "\t", ""):
            section = None
        elif section is not None:
            section.append(line)
    for lines in sections:
        body = "\n".join(lines)
        active = re.search(r"(\d+) Tracks? of which (\d+) are active", body)
        frames = re.search(r"Frames read: (\d+)", body)
        if re.search(r"^\s*Standby: no\s*$", body, re.M) and active and int(active.group(2)) > 0 and frames:
            capturing = max(capturing or 0, int(frames.group(1)))
    return capturing


def made_up_frames(text: str) -> list[int]:
    """The frames of silence that the input made up in each stall its log says ended: `readInput: the device
    delivers again, after N frames of silence made up`."""
    return [int(count) for count in re.findall(r"the device delivers again, after (\d+) frames of silence made up", text)]


# A wait that the framework's flush or the sensor's shutdown cut short (the wait returns DEAD_OBJECT, -32): the
# request was being dropped already, as when the application leaves photo mode, so no buffer failed. The other
# way to that status, a slot that an earlier wait's reset took back, follows a line this check counts.
INTERRUPTED_WAIT = re.compile(r"^WaitFor: the camera did not deliver a buffer \(-32\)$")


def hal_failures(text: str) -> list[str]:
    """`logcat -d` lines of the camera HAL's tags that say a buffer failed: an error of any of them, a request
    that was not queued completely, a buffer that never arrived or was never collected. An interrupted wait
    is not one."""
    failures = []
    for line in text.splitlines():
        if not (match := re.search(r"\s([EW])\s+(\w+)\s*: (.*)$", line)):
            continue
        level, tag, message = match.groups()
        if tag not in HAL_TAGS or INTERRUPTED_WAIT.match(message.strip()):
            continue
        if level == "E" or any(part in message for part in (
                "was not queued completely", "did not deliver", "never collected")):
            failures.append(line.strip())
    return failures


SWITCH_DENIAL_KIND = f"an SELinux denial naming {CAMERA_HAL_DOMAIN}"
SWITCH_REFUSAL_KIND = 'the camera service\'s "Unable to submit capture request"'
# The events of the switch: the photo activity pauses and the video activity resumes (ActivityTaskManager's
# wm_on_paused_called and wm_on_resume_called, in the events buffer `logcat -b all` merges).
PHOTO_PAUSED = re.compile(rf"wm_on_paused_called: \[\d+,{re.escape(CAMERA_PACKAGE)}\.CameraActivity,")
VIDEO_RESUMED = re.compile(rf"wm_on_resume_called: \[\d+,{re.escape(CAMERA_PACKAGE)}\.VideoCamera,")


def composer_fence_denial(line: str) -> bool:
    """Whether a log line is the SELinux denial of the camera HAL using a file descriptor of the display composer
    HAL: source hal_camera_default, target hal_graphics_composer_default, class fd, permission use."""
    denied = re.search(r"avc:\s+denied\s+\{([^}]*)\}", line)
    if not denied or "use" not in denied[1].split():
        return False
    fields = dict(re.findall(r"\b(scontext|tcontext|tclass)=(\S+)", line))
    return (fields.get("tclass") == "fd"
            and fields.get("scontext", "").split(":")[:3] == ["u", "r", CAMERA_HAL_DOMAIN]
            and fields.get("tcontext", "").split(":")[:3] == ["u", "r", COMPOSER_HAL_DOMAIN])


def switch_failures(text: str) -> list[tuple[str, str]]:
    """The lines of a `logcat -d` taken across photo to video switches that show a capture request refused, as
    (what, line): the denial of composer_fence_denial, and the camera service's (cameraserver logs under the tag
    Camera3-Device) "Unable to submit capture request", which is what the refused binder transaction becomes.
    Nothing else counts: the service logs "Broken pipe (-32)" for the buffers of every stream whose application
    is stopped, and other denials are not this one."""
    found: list[tuple[str, str]] = []
    for line in text.splitlines():
        line = line.strip()
        if composer_fence_denial(line):
            found.append((SWITCH_DENIAL_KIND, line))
        elif re.search(r"\s[EWF]\s+Camera3-Device\s*: .*Unable to submit capture request", line):
            found.append((SWITCH_REFUSAL_KIND, line))
    return found


def switch_observed(text: str) -> list[str]:
    """What a `logcat -d` taken after a switch's marker lacks of the switch: the photo activity's pause and the
    video activity's resume."""
    missing = []
    if not PHOTO_PAUSED.search(text):
        missing.append(f"pause of {CAMERA_PACKAGE}.CameraActivity")
    if not VIDEO_RESUMED.search(text):
        missing.append(f"resume of {CAMERA_PACKAGE}.VideoCamera")
    return missing


def log_after(text: str, marker: str) -> str | None:
    """The part of a `logcat -d` from the last line that holds `marker`, or None when no line holds it."""
    index = text.rfind(marker)
    return None if index < 0 else text[index:]


def frames_produced(dump: str) -> int:
    """`dumpsys media.camera`: the frames the camera service counts for all the streams it lists."""
    return sum(int(count) for count in re.findall(r"Frames produced: (\d+)", dump))


STREAM_HEADER = re.compile(r"^\s*Stream\[(\d+)\]:")


def preview_stream_frames(dump: str, package: str = CAMERA_PACKAGE) -> dict[int, int]:
    """`dumpsys media.camera`: the frames produced by each output stream whose consumer is the application's own preview
    surface (a `SurfaceView[<package>/...]`), by stream id. Only that stream shows that the viewfinder is live: another
    stream (a still, an encoder's surface) may run while the preview stands still."""
    consumers: dict[int, str] = {}
    counts: dict[int, int] = {}
    current: int | None = None
    for line in dump.splitlines():
        if (header := STREAM_HEADER.match(line)):
            current = int(header[1])
        if current is None:
            continue
        if (consumer := re.search(r"Consumer name: (.*)$", line)):
            consumers[current] = consumer[1]
        if (produced := re.search(r"Frames produced: (\d+)", line)):
            counts[current] = int(produced[1])
    return {stream: counts[stream] for stream, consumer in consumers.items()
            if f"SurfaceView[{package}" in consumer and stream in counts}


SESSION_LINE = re.compile(r"^Camera: memory: capture session (closed|opened)\b", re.MULTILINE)
PREVIEW_LAYER = re.compile(r"((?:[0-9a-f]+ )?SurfaceView\[[^\n{}]*\(BLAST\)#(\d+))")
PENDING_PRESENT = 9 * 10 ** 18


def session_events(text: str) -> list[str]:
    """The launcher's lines about the host's capture session, in order: "closed" or "opened" for each
    `Camera: memory: capture session closed|opened, footprint N MiB` line (a development launcher prints them)."""
    return SESSION_LINE.findall(text)


def parse_preview_layer(text: str) -> str | None:
    """The newest SurfaceView layer of the camera application in `dumpsys SurfaceFlinger --list` (the highest
    layer number), which is the preview's. The name is the layer's own, with the window's hash in front of it where
    the list gives one (`RequestedLayerState{7c01cb3 SurfaceView[...](BLAST)#136 ...}`): `--latency` finds a layer by
    that name and answers a name without the hash with the refresh period alone."""
    found = PREVIEW_LAYER.findall(text)
    return max(found, key=lambda item: int(item[1]))[0].strip() if found else None


def parse_present_times(text: str) -> list[int]:
    """The times in nanoseconds at which SurfaceFlinger presented the frames of a layer
    (`dumpsys SurfaceFlinger --latency LAYER`: a line of the refresh period, then three times per frame, the second
    the present time; a frame not yet presented has a time at the end of the range, or zero)."""
    times = []
    for line in text.strip().splitlines()[1:]:
        part = line.split()
        if len(part) == 3 and part[1].isdigit() and 0 < int(part[1]) < PENDING_PRESENT:
            times.append(int(part[1]))
    return sorted(set(times))


def longest_gap_seconds(times: list[int]) -> float:
    """The longest time between two consecutive times (nanoseconds), in seconds; zero for fewer than two."""
    return max(((b - a) / 1e9 for a, b in zip(times, times[1:])), default=0.0)


STILL_STREAM = re.compile(r"Stream\[(\d+)\]: Output\n(?:(?!\s*Stream\[\d+\]: Output).*\n)*?\s+Dims: \d+ x \d+, format (0x[0-9a-f]+)")
STILL_FORMATS = ("0x20", "0x21")  # RAW16 and BLOB (the JPEG)
PENDING_BUFFER = re.compile(r"pending buffer of stream (\d+) for frames? \[?(\d+)")
LOGCAT_LINE = re.compile(r"^\s*(\d+\.\d+)\s+\d+\s+\d+\s+[A-Z]\s+(\S+)\s*:\s?(.*)$")
REPORT_SPREAD_SECONDS = 0.1


def parse_still_streams(text: str) -> set[int]:
    """The ids of the still output streams (a JPEG or a RAW) that `dumpsys media.camera` lists for the open camera."""
    return {int(found[0]) for found in STILL_STREAM.findall(text) if found[1] in STILL_FORMATS}


def pending_phase(log: str, marker: str, still_streams: set[int],
                  interval: float | None = None) -> tuple[str | None, str]:
    """Where the stop that followed `marker` in the log came, by the first report of the camera HAL's result dispatcher
    after it, which lists the buffers the dispatcher still waits for (`pending buffer of stream S for frames [A - B]`):
    "inside" a still's request when the oldest frame among them has a still's buffer (the buffers of one frame are one
    request: a request that has the preview and the JPEG holds the pipeline as the JPEG alone does), "before" it when a
    still's buffer waits behind older ones (a stop that caught the preview's frames ahead of the still), "after" it
    when no still's buffer is pending. `interval` is how long after the marker (the guest's clock stands still in a
    stop, and the report comes as it resumes) a report may be: a report later than the round's stop is another
    event's. None, and the reason, when the log has no marker or no report of the round: nothing is shown then."""
    lines = log.splitlines()
    start = next((index for index, line in enumerate(lines) if marker in line), None)
    if start is None:
        return None, "the log has no marker of the round"
    marked = LOGCAT_LINE.match(lines[start])
    marker_time = float(marked[1]) if marked else None
    first: float | None = None
    pending: list[tuple[int, int]] = []
    late = False
    for line in lines[start + 1:]:
        found = LOGCAT_LINE.match(line)
        buffer = PENDING_BUFFER.search(found[3]) if found else None
        if found is None or buffer is None:
            continue
        when = float(found[1])
        if interval is not None and marker_time is not None and when - marker_time > interval:
            late = True
            break
        if first is None:
            first = when
        elif when - first > REPORT_SPREAD_SECONDS:
            break
        pending.append((int(buffer[2]), int(buffer[1])))
    if not pending:
        return None, ("the result dispatcher reported no pending buffer inside the round's interval "
                      f"({interval:.1f} s after the marker)" if late else
                      "the result dispatcher reported no pending buffer after the marker")
    oldest = min(frame for frame, _ in pending)
    streams = sorted({stream for frame, stream in pending if frame == oldest})
    names = " and ".join(str(stream) for stream in streams)
    head = (f"the oldest pending buffer is stream {names}'s frame {oldest}" if len(streams) == 1 else
            f"the oldest pending buffers are of frame {oldest} (streams {names})")
    if any(stream in still_streams for stream in streams):
        return "inside", head
    behind = [(frame, stream) for frame, stream in pending if stream in still_streams]
    if behind:
        frame, stream = min(behind)
        return "before", f"{head}; the still's buffer (stream {stream}, frame {frame}) waits behind it"
    return "after", f"{head}; no buffer of a still is pending"


def pipeline_head(log: str, marker: str, still_streams: set[int],
                  interval: float | None = None) -> tuple[bool | None, str]:
    """Whether the stop that followed `marker` overlapped a still's request (see `pending_phase`)."""
    phase, evidence = pending_phase(log, marker, still_streams, interval)
    return (None if phase is None else phase == "inside"), evidence


def camera_connected(text: str, package: str) -> bool:
    """Whether `dumpsys media.camera` shows the package holding a camera: among the active clients, or
    in the service's event log with a connect as the last event of some device (the log is most recent
    first, each line indented by two spaces and starting with the time; a disconnect after the connect
    ends it)."""
    if package in active_client_packages(text):
        return True
    escaped = re.escape(package)
    latest: dict[str, tuple[str, bool]] = {}  # device -> (time of the newest event, it was a connect)
    stamp = r"(?:(\d\d-\d\d \d\d:\d\d:\d\d)\s*:\s*)?"
    event = rf"^[ \t]*{stamp}\b(CONNECT|DISCONNECT) device (\S+) client for package {escaped}\b"
    for match in re.finditer(event, text, re.MULTILINE):
        when, kind, device = match[1] or "", match[2], match[3]
        # The first line seen is the newest unless a later one carries a newer time.
        if device not in latest or when > latest[device][0]:
            latest[device] = (when, kind == "CONNECT")
    return any(connected for _, connected in latest.values())


@dataclass(frozen=True)
class StaticInfo:
    """What a camera's static characteristics say, from the metadata dump of `dumpsys media.camera`."""
    facing: str
    capabilities: frozenset[str]
    flash: bool
    af_modes: frozenset[str]
    device: str = ""
    hot_pixel_map_modes: frozenset[str] = frozenset()
    lens_shading_map_modes: frozenset[str] = frozenset()
    lens_shading_applied: tuple[str, ...] = ()


# The dump prints each camera under a `== Camera HAL device <id> (v..) static information: ==` header,
# as `name (tag): type[count]` and the values on the lines under it in brackets (enum values by name).
# The entries follow the order the HAL's metadata was built in, not the tag order, so only the header
# starts the next camera; the next `== ` header of another kind ends the camera.
# Android 17 prints the autofocus modes of the dump as numbers, the capabilities and the facing as names.
AF_MODE_NAMES = {"0": "OFF", "1": "AUTO", "2": "MACRO", "3": "CONTINUOUS_VIDEO", "4": "CONTINUOUS_PICTURE",
                 "5": "EDOF"}
STATIC_INFO_HEADER = re.compile(r"^\s*== Camera HAL device (\S+) .* static information: ==\s*$")
# The dump prints these enums as names or as numbers.
MAP_MODE_NAMES = {"0": "OFF", "1": "ON"}
BOOLEAN_NAMES = {"0": "FALSE", "1": "TRUE"}
STATIC_INFO_ENTRY = re.compile(r"^\s*(android\.[A-Za-z0-9_.]+) \(([0-9a-f]+)\): ")
STATIC_INFO_VALUES = re.compile(r"^\s*\[([^\]]*)\]\s*$")


def parse_static_info(text: str) -> list[StaticInfo]:
    """`dumpsys media.camera`: the characteristics of each camera it dumps, in the order it dumps them."""
    cameras: list[dict[str, list[str]]] = []
    current: dict[str, list[str]] | None = None
    entry: list[str] | None = None
    for line in text.splitlines():
        if line.lstrip().startswith("== "):
            header = STATIC_INFO_HEADER.match(line)
            current = {"device": [header[1].rsplit("/", 1)[-1]]} if header else None  # "device@1.1/internal/4" is "4"
            if current is not None:
                cameras.append(current)
            entry = None
            continue
        header = STATIC_INFO_ENTRY.match(line)
        if header and current is not None:
            entry = current.setdefault(header[1], [])
            continue
        values = STATIC_INFO_VALUES.match(line)
        if values and entry is not None:
            entry.extend(values[1].split())
        else:
            entry = None
    return [StaticInfo(
        facing=camera["android.lens.facing"][0],
        capabilities=frozenset(camera.get("android.request.availableCapabilities", ())),
        flash=camera.get("android.flash.info.available", ["FALSE"])[:1] == ["TRUE"],
        af_modes=frozenset(AF_MODE_NAMES.get(mode, mode) for mode in camera.get("android.control.afAvailableModes", ())),
        device=camera["device"][0],
        hot_pixel_map_modes=frozenset(MAP_MODE_NAMES.get(mode, mode)
                                      for mode in camera.get("android.statistics.info.availableHotPixelMapModes", ())),
        lens_shading_map_modes=frozenset(MAP_MODE_NAMES.get(mode, mode)
                                         for mode in camera.get("android.statistics.info.availableLensShadingMapModes", ())),
        lens_shading_applied=tuple(BOOLEAN_NAMES.get(value, value)
                                   for value in camera.get("android.sensor.info.lensShadingApplied", ())))
        for camera in cameras if camera.get("android.lens.facing")]


def raw_metadata_problems(infos: list[StaticInfo]) -> list[str]:
    """What a camera with the RAW capability leaves out of the metadata that Android requires of one: both map modes
    ON (the lens shading map and the hot pixel map) and whether its lens shading is applied."""
    problems = []
    for info in infos:
        if "RAW" not in info.capabilities:
            continue
        label = f"camera {info.device or '?'}"
        if "ON" not in info.lens_shading_map_modes:
            problems.append(f"{label} states RAW but the lens shading map modes {sorted(info.lens_shading_map_modes)} lack ON")
        if "ON" not in info.hot_pixel_map_modes:
            problems.append(f"{label} states RAW but the hot pixel map modes {sorted(info.hot_pixel_map_modes)} lack ON")
        if len(info.lens_shading_applied) != 1 or info.lens_shading_applied[0] not in BOOLEAN_NAMES.values():
            problems.append(f"{label} states RAW but not whether its lens shading is applied "
                            f"(android.sensor.info.lensShadingApplied {list(info.lens_shading_applied)})")
    return problems


def features_of(infos: list[StaticInfo]) -> set[str]:
    """The camera features the characteristics earn, by the rules of PackageManager's feature docs
    (the back camera has the flash and the autofocus; some camera has the capabilities). The concurrent
    feature is a property of combinations, not of a camera, and is not derived."""
    features: set[str] = set()
    back = [info for info in infos if info.facing == "BACK"]
    if infos:
        features.add("android.hardware.camera.any")
    if back:
        features.add("android.hardware.camera")
    if any(info.facing == "FRONT" for info in infos):
        features.add("android.hardware.camera.front")
    if any("AUTO" in info.af_modes for info in back):
        features.add("android.hardware.camera.autofocus")
    if any(info.flash for info in back):
        features.add("android.hardware.camera.flash")
    if any("MANUAL_SENSOR" in info.capabilities for info in infos):
        features.add("android.hardware.camera.capability.manual_sensor")
    if any("RAW" in info.capabilities for info in infos):
        features.add("android.hardware.camera.capability.raw")
    return features


def parse_provider_pids(text: str) -> list[int]:
    """`ps -A -o PID,NAME`: the pids of the camera provider (its binary's name holds PROVIDER_PROCESS)."""
    pids = []
    for line in text.splitlines():
        words = line.split(None, 1)
        if len(words) == 2 and words[0].isdecimal() and PROVIDER_PROCESS in words[1]:
            pids.append(int(words[0]))
    return pids


def parse_listing(text: str) -> set[str]:
    """`ls`: the names it prints, none for a directory that is not there."""
    return {line.strip() for line in text.splitlines()
            if line.strip() and "No such file" not in line and "Not a directory" not in line}


# ---- PNG (the screenshot) and JPEG (the still).

@dataclass
class Image:
    width: int
    height: int
    channels: int
    rows: list[bytes]


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def paeth(left: int, up: int, corner: int) -> int:
    estimate = left + up - corner
    distance_left, distance_up, distance_corner = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
    if distance_left <= distance_up and distance_left <= distance_corner:
        return left
    return up if distance_up <= distance_corner else corner


def decode_png(data: bytes) -> Image:
    """An 8-bit grey, RGB or RGBA PNG without interlacing, which is what screencap writes."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    position, header, compressed = len(PNG_SIGNATURE), None, bytearray()
    while position + 8 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, position)
        body = data[position + 8:position + 8 + length]
        if len(body) != length:
            raise ValueError("truncated PNG chunk")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        position += 12 + length
    if header is None or not compressed:
        raise ValueError("PNG without header or data")
    width, height, depth, color, _, _, interlace = header
    channels = {0: 1, 2: 3, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError(f"unsupported PNG (depth {depth}, colour type {color}, interlace {interlace})")
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise ValueError("PNG data has the wrong size")
    rows: list[bytes] = []
    previous = bytearray(stride)
    for y in range(height):
        offset = y * (stride + 1)
        kind, line = raw[offset], bytearray(raw[offset + 1:offset + 1 + stride])
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 255
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                corner = previous[i - channels] if i >= channels else 0
                line[i] = (line[i] + paeth(left, previous[i], corner)) & 255
        elif kind != 0:
            raise ValueError(f"unknown PNG filter {kind}")
        rows.append(bytes(line))
        previous = line
    return Image(width, height, channels, rows)


def luma_statistics(image: Image, box: tuple[float, float, float, float] = PREVIEW_BOX) -> tuple[float, float]:
    """The mean and the standard deviation of the luma inside a box of fractions of the image."""
    left, top = int(image.width * box[0]), int(image.height * box[1])
    right, bottom = max(left + 1, int(image.width * box[2])), max(top + 1, int(image.height * box[3]))
    count, total, squares = 0, 0.0, 0.0
    for row in image.rows[top:bottom]:
        for x in range(left, right):
            pixel = row[x * image.channels:x * image.channels + image.channels]
            if image.channels == 1:
                luma = pixel[0]
            else:
                luma = 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2]
            total += luma
            squares += luma * luma
            count += 1
    mean = total / count
    return mean, math.sqrt(max(0.0, squares / count - mean * mean))


@dataclass
class Jpeg:
    width: int
    height: int
    exif: bool
    complete: bool


def parse_jpeg(data: bytes) -> Jpeg:
    """The size, the Exif segment and the end marker of a JPEG file."""
    if data[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG")
    position, width, height, exif = 2, 0, 0, False
    while position + 4 <= len(data):
        if data[position] != 0xFF:
            raise ValueError(f"no marker at {position}")
        marker = data[position + 1]
        if marker == 0xFF:
            position += 1
            continue
        if marker == 0xD9 or marker == 0xDA:
            break
        length = struct.unpack_from(">H", data, position + 2)[0]
        body = data[position + 4:position + 2 + length]
        if marker == 0xE1 and body.startswith(b"Exif\x00\x00"):
            exif = True
        if marker in (0xC0, 0xC1, 0xC2) and len(body) >= 5:
            height, width = struct.unpack_from(">HH", body, 1)
        position += 2 + length
    if not width or not height:
        raise ValueError("no frame header")
    return Jpeg(width, height, exif, data.rstrip(b"\x00").endswith(b"\xff\xd9"))


# ---- The guest.

class SessionCapture:
    """The guest's main and events logs, captured continuously to a file by one `adb logcat` process that the
    runner starts and stops (its pid is recorded beside the file). The platform's buffers are finite and a read
    at the end of a scenario sees only what has not rolled out, so the checks that need the whole session read
    this file; a capture that ends before it is stopped is a Failure of its own."""

    def __init__(self, adb: str, serial: str, path: pathlib.Path) -> None:
        self.adb, self.serial, self.path = adb, serial, path
        self.pid_path = path.with_name(path.name + ".pid")
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        arguments = [self.adb, "-s", self.serial, "logcat",
                     *(item for buffer in CAPTURE_BUFFERS for item in ("-b", buffer)), "-v", "threadtime"]
        stream = self.path.open("wb")
        try:
            self.process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=stream,
                                            stderr=subprocess.STDOUT, start_new_session=True)
        finally:
            stream.close()
        self.pid_path.write_text(f"{self.process.pid}\n", encoding="utf-8")
        deadline = time.monotonic() + CAPTURE_START_SECONDS
        while self.path.stat().st_size == 0:
            self.check()
            if time.monotonic() >= deadline:
                self.stop()
                raise Failure(f"the log capture (pid {self.process.pid}) wrote nothing in {CAPTURE_START_SECONDS} s")
            time.sleep(0.5)

    def check(self) -> None:
        """Raises when the capture is not running: it ended on its own (the device went away, adb died)."""
        if self.process is None:
            raise Failure("the log capture was never started")
        status = self.process.poll()
        if status is not None:
            tail = self.path.read_text(errors="replace")[-300:] if self.path.is_file() else ""
            raise Failure(f"the log capture (pid {self.process.pid}) ended early with status {status}; "
                          f"its last output: {tail!r}")

    def text(self) -> str:
        self.check()
        return self.path.read_text(errors="replace")

    def stop(self) -> None:
        """Stops the process this object started, and only it."""
        process = self.process
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(CAPTURE_STOP_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(CAPTURE_STOP_SECONDS)


class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 private: pathlib.Path, hold_directory: pathlib.Path | None = None,
                 control: pathlib.Path | None = None, hardware_input: bool = False,
                 edge: pathlib.Path | None = None) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.private = private
        # Where the launcher's audio hold library (--audio-hold-library) takes its requests; None without one.
        self.hold_directory = hold_directory
        # The time (a time.monotonic() value) by which every command must have finished, while a step holds one (`bounded`).
        self.limit: float | None = None
        # The synthetic camera's control pipe (the video-lost and video-leave modes); None without one.
        self.control = control
        # The launcher's pipe that plays the application's leaving the foreground to it (the video-leave mode).
        self.edge = edge
        # Whether the launcher gave the guest the side keys (--hardware-input): the device that reports the lens cover.
        self.hardware_input = hardware_input
        # The continuous capture of the guest's main and events logs, once the runner has started it.
        self.capture: SessionCapture | None = None
        # Where the runner keeps what a failed check is read from, when it was given a directory for it (--output-dir).
        self.output_directory: pathlib.Path | None = None

    def camera_control(self, line: str) -> None:
        """Writes one line to the synthetic camera's control pipe: `interrupt background`, `interrupt`, `resume`."""
        if self.control is None:
            raise Failure("no control pipe: the run was not started in the video-lost mode")
        wait_for("the synthetic camera's control pipe", COMMAND_SECONDS,
                 lambda: None if self.control.exists() else f"{self.control} does not exist", self)
        descriptor = os.open(self.control, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(descriptor, f"{line}\n".encode())
        finally:
            os.close(descriptor)

    def edge_control(self, line: str) -> None:
        """Writes one line to the launcher's edge pipe: `leave` (the camera's video ends and the guest's GPU requests are
        held, as the application's leaving the foreground does it) or `return` (the requests are released)."""
        if self.edge is None:
            raise Failure("no edge pipe: the run was not started in the video-leave mode")
        wait_for("the launcher's edge pipe", COMMAND_SECONDS,
                 lambda: None if self.edge.exists() else f"{self.edge} does not exist", self)
        descriptor = os.open(self.edge, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(descriptor, f"{line}\n".encode())
        finally:
            os.close(descriptor)

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS,
                binary: bool = False) -> subprocess.CompletedProcess:
        if self.limit is not None:
            # A step with a deadline: no command starts after it, and none runs past it.
            left = self.limit - time.monotonic()
            if left <= 0:
                raise Failure(f"the deadline passed before 'adb {' '.join(arguments)[:80]}' ran")
            timeout = min(timeout, left)
        try:
            return subprocess.run(
                [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE if binary else subprocess.STDOUT, text=not binary, timeout=timeout,
                check=False)
        except subprocess.TimeoutExpired as problem:
            if self.limit is None:
                raise
            raise Failure(f"'adb {' '.join(arguments)[:80]}' did not finish by the step's deadline") from problem

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()[-500:]}")
        return result.stdout

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])

    def prop(self, name: str) -> str:
        return self.shell(f"getprop {name}").strip()

    def features(self) -> set[str]:
        return parse_camera_features(self.shell("pm list features"))

    def media_camera(self) -> str:
        return self.shell("dumpsys media.camera")

    def sensor_privacy(self) -> str:
        return self.shell("dumpsys sensor_privacy")

    def session_log(self) -> str:
        """The main and events logs since the capture began (all of them, however long ago); a capture that is
        absent or has ended is a Failure, never an empty log."""
        if self.capture is None:
            raise Failure("the session's logs are not being captured")
        return self.capture.text()

    def interface_dump(self) -> str:
        """The XML of `uiautomator dump`, retried while the platform answers that it has no idle state."""
        problem = ""
        for attempt in range(UI_DUMP_ATTEMPTS):
            if attempt:
                time.sleep(UI_DUMP_GAP_SECONDS)
            result = self.shell(f"rm -f {UI_DUMP_PATH}; uiautomator dump {UI_DUMP_PATH} 2>&1; "
                                f"cat {UI_DUMP_PATH} 2>/dev/null; rm -f {UI_DUMP_PATH}")
            start = result.find("<?xml")
            if start >= 0:
                return result[start:]
            problem = result.strip()[-300:]
        raise Failure(f"uiautomator produced no interface dump in {UI_DUMP_ATTEMPTS} attempts: {problem}")

    def provider_pids(self) -> list[int]:
        return parse_provider_pids(self.shell("ps -A -o PID,NAME"))

    def stills(self) -> set[str]:
        return {name for name in parse_listing(self.shell(f"ls {STILL_DIRECTORY} 2>&1 || true"))
                if name.lower().endswith((".jpg", ".jpeg"))}

    def videos(self) -> tuple[dict[str, dict[str, str]], str]:
        """The movies MediaStore lists, by their `_id`, and the raw text of the query."""
        text = self.shell(f"content query --uri {VIDEO_URI} --projection _id:_display_name:_data:duration:_size "
                          "2>&1 || true")
        return {row["_id"]: row for row in parse_content_rows(text) if "_id" in row}, text

    def launcher_log(self) -> str:
        """What the launcher has printed so far (its own lines about the host's camera among them)."""
        return self.log.read_text(errors="replace")

    def preview_layer(self) -> str | None:
        return parse_preview_layer(self.shell(
            f"dumpsys SurfaceFlinger --list | grep 'SurfaceView\\[{CAMERA_PACKAGE}' | grep BLAST || true"))

    def present_times(self, layer: str) -> list[int]:
        return parse_present_times(self.shell(f"dumpsys SurfaceFlinger --latency '{layer}'"))

    def hal_log(self) -> str:
        return self.shell("logcat -d -b all " + " ".join(f"{tag}:W" for tag in HAL_TAGS) + " '*:S' 2>&1 || true")

    def stream_leaks(self) -> list[str]:
        """The camera service's lines that say a stream outlived the close of its device (see STREAM_LEAK)."""
        return parse_stream_leaks(self.shell(f"logcat -d -v threadtime -b all {SERVICE_LOG_TAG}:E '*:S' 2>&1 || true"))

    def screenshot(self) -> Image:
        result = self.adb_run("exec-out", "screencap", "-p", timeout=SCREENSHOT_SECONDS, binary=True)
        if result.returncode != 0:
            raise Failure(f"screencap exited {result.returncode}: {result.stderr[-300:]!r}")
        try:
            return decode_png(result.stdout)
        except (ValueError, zlib.error) as problem:
            raise Failure(f"the screenshot is unreadable: {problem}") from problem

    def freeze(self, seconds: float) -> float:
        """Stops the emulator process for `seconds` and continues it: the whole guest, its sound device and its
        camera with it, stands still, as when the host holds the application up. The process goes on whatever
        happens in between. Returns how long it stood still, as measured."""
        stopped = time.monotonic()
        os.kill(self.process.pid, signal.SIGSTOP)
        try:
            time.sleep(seconds)
        finally:
            os.kill(self.process.pid, signal.SIGCONT)
        return time.monotonic() - stopped

    def dispatcher_log(self) -> str:
        """The camera HAL's result dispatcher's warnings and the runner's own markers, with the guest's monotonic time."""
        return self.shell(f"logcat -d -v monotonic -b all GCH_ResultDispatcher:W {HELD_STILL_MARKER_TAG}:I '*:S' 2>&1 || true")

    def audio_flinger(self) -> str:
        return self.shell("dumpsys media.audio_flinger")

    def stream_log(self) -> str:
        """The audio HAL's own lines about its streams, from the log's start."""
        return self.shell(f"logcat -d -v time -s {HAL_STREAM_TAG}:V 2>&1 || true")

    def hold_audio(self, seconds: float) -> float:
        """Has the library in the launcher hold the thread that serves the Virtio sound device for `seconds` while
        the guest runs, and returns how long it was held. The request is written to another name and renamed, so the
        library never reads half of it; the library says what it held in the file `holds`: a count of threads that
        is not one, or a hold much shorter than asked, is a hold that did not happen."""
        if self.hold_directory is None:
            raise Failure("no audio hold library: the run was not given --audio-hold-library")
        log = self.hold_directory / "holds"
        before = len(log.read_text().splitlines()) if log.is_file() else 0
        request = self.hold_directory / "hold.request"
        request.write_text(f"{round(seconds * 1000)}\n")
        os.replace(request, self.hold_directory / "hold")
        deadline = time.monotonic() + seconds + AUDIO_HOLD_WAIT_SECONDS
        while time.monotonic() < deadline:
            self.alive()
            lines = log.read_text().splitlines() if log.is_file() else []
            if len(lines) > before:
                fields = lines[before].split()
                held, threads = int(fields[1]) / 1e9, int(fields[3])
                if threads != 1 or held < seconds * AUDIO_HOLD_MINIMUM_FRACTION:
                    raise Failure(f"the audio hold library held {threads} thread(s) named for the sound device "
                                  f"for {held:.2f} s of the {seconds:g} s asked: a stall that did not happen "
                                  f"cannot judge the input ({lines[before]!r})")
                return held
            time.sleep(0.05)
        raise Failure(f"the audio hold library reported no hold within {seconds + AUDIO_HOLD_WAIT_SECONDS:g} s "
                      f"(requested {seconds:g} s)")

    def pull(self, remote: str) -> bytes:
        local = self.private / pathlib.PurePosixPath(remote).name
        result = self.adb_run("pull", remote, str(local), timeout=COMMAND_SECONDS)
        if result.returncode != 0 or not local.is_file():
            raise Failure(f"cannot pull {remote}: {result.stdout.strip()[-300:]}")
        return local.read_bytes()


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: Guest | None = None, strict: bool = False) -> None:
    """Calls `check` until it returns None (done); its last message is the failure. With `strict` the limit holds
    through the end of each check: one that returns after it does not pass."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        message = check()
        if message is None:
            if strict and time.monotonic() > deadline:
                raise Failure(f"{description}: the check finished {time.monotonic() - deadline:.1f} s after the limit "
                              f"of {seconds:.0f} s")
            return
        time.sleep(1.0)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


@contextlib.contextmanager
def bounded(guest, deadline: float):
    """While the block runs, no command of the guest starts after `deadline` (a time.monotonic() value) or runs past it."""
    previous = getattr(guest, "limit", None)
    guest.limit = deadline if previous is None else min(previous, deadline)
    try:
        yield
    finally:
        guest.limit = previous


# ---- The checks.

def check_layout(guest: Guest, plan: Plan) -> None:
    sku = guest.prop(camera_layouts.SKU_PROPERTY)
    if sku != plan.layout.name:
        raise Failure(f"the guest booted with the layout {sku!r}, not {plan.layout.name!r}")
    variant = guest.prop(camera_layouts.MEDIA_PROPERTY)
    if variant != plan.media_variant:
        raise Failure(f"the media profiles variant is {variant!r}, not {plan.media_variant!r}")
    features = guest.features()
    if features != plan.features:
        raise Failure(f"the camera features are {sorted(features)}, not {sorted(plan.features)}: "
                      f"extra {sorted(features - plan.features)}, missing {sorted(plan.features - features)}")
    if guest.prop("ro.vendor.camera.config") != "v4l2":
        raise Failure("ro.vendor.camera.config is not v4l2: the camera HAL would list the stock cameras")


def check_camera_list(guest: Guest, plan: Plan) -> None:
    def listed() -> str | None:
        ids = parse_camera_ids(guest.media_camera())
        if sorted(ids) != sorted(plan.camera_ids):
            return f"cameras {ids}, not {list(plan.camera_ids)}"
        return None

    # The lazy provider starts when cameraserver first asks, and the cameras appear after it enumerates.
    wait_for("the cameras Android lists", SETTLE_SECONDS if plan.camera_ids else 5, listed, guest)


def wait_provider_exit(guest: Guest, when: str) -> float:
    """Waits for the lazy provider to be gone; returns how long it took. `dumpsys media.camera` can wake the provider,
    so nothing that asks cameraserver runs between the close and this wait."""
    began = time.monotonic()
    wait_for(f"the camera provider to exit {when}", PROVIDER_EXIT_SECONDS,
             lambda: None if not guest.provider_pids() else f"it still runs as {guest.provider_pids()}", guest)
    return time.monotonic() - began


def check_characteristics(guest: Guest, plan: Plan) -> None:
    """The features that the layout states are the ones the cameras' own characteristics earn."""
    infos = parse_static_info(guest.media_camera())
    if len(infos) != len(plan.camera_ids):
        raise Failure(f"dumpsys media.camera has the characteristics of {len(infos)} cameras, not "
                      f"{len(plan.camera_ids)}: the dump's format is not the one the runner reads")
    derived = features_of(infos)
    stated = set(plan.features) - {CONCURRENT_FEATURE}
    if derived != stated:
        raise Failure(f"the layout {plan.layout.name} states {sorted(stated)} but the characteristics of the "
                      f"cameras earn {sorted(derived)}: extra {sorted(stated - derived)}, "
                      f"missing {sorted(derived - stated)}")


def blocked_preview(sample: Callable[[], tuple[float, float]], sleep: Callable[[float], None]) -> str | None:
    """A blocked session's preview must not be the chart on any of BLOCKED_SAMPLES samples; the message
    of the first sample that is, else None."""
    for attempt in range(BLOCKED_SAMPLES):
        if attempt:
            sleep(BLOCKED_SAMPLE_GAP_SECONDS)
        mean, deviation = sample()
        if deviation >= CHART_MINIMUM_DEVIATION and mean > BLACK_MAXIMUM_MEAN:
            return f"the preview shows a picture (mean {mean:.1f}, deviation {deviation:.1f}, sample {attempt + 1})"
    return None


@dataclass(frozen=True)
class Foreground:
    """What the guest shows: the resumed activity ("package/.Class"), the focused window, the wakefulness."""
    resumed: str
    focus: str
    wakefulness: str

    def camera_in_front(self) -> bool:
        return self.resumed.startswith(CAMERA_PACKAGE + "/") and self.wakefulness == "Awake"

    def describe(self) -> str:
        return (f"resumed activity {self.resumed or 'none'}, focused window {self.focus or 'none'}, "
                f"wakefulness {self.wakefulness or 'unknown'}")


def parse_foreground(text: str) -> Foreground:
    """The `FOREGROUND_COMMAND` output. The top resumed activity is preferred, any resumed activity next."""
    resumed = ""
    for pattern in (r"topResumedActivity=ActivityRecord\{\S+ u\d+ (\S+)", r"ResumedActivity[=:] *ActivityRecord\{\S+ u\d+ (\S+)"):
        match = re.search(pattern, text)
        if match:
            resumed = match.group(1)
            break
    focus = re.search(r"mCurrentFocus=Window\{\S+ u\d+ ([^}]*)\}", text)
    wakefulness = re.search(r"mWakefulness=(\w+)", text)
    return Foreground(resumed, focus.group(1) if focus else "", wakefulness.group(1) if wakefulness else "")


def parse_sensor_privacy(text: str) -> list[tuple[int, int, int, int]]:
    """The (user, sensor, toggle type, state) of every toggle `dumpsys sensor_privacy` lists, in order."""
    entries: list[tuple[int, int, int, int]] = []
    user = sensor = toggle = -1
    for raw in text.splitlines():
        match = re.fullmatch(r"\s*(user_id|sensor|toggle_type|state_type)=(-?\d+)\s*", raw)
        if not match:
            continue
        key, value = match.group(1), int(match.group(2))
        if key == "user_id":
            user = value
        elif key == "sensor":
            sensor = value
        elif key == "toggle_type":
            toggle = value
        else:
            entries.append((user, sensor, toggle, value))
    return entries


def camera_hardware_blocked(entries: list[tuple[int, int, int, int]]) -> bool:
    """Whether the camera's hardware toggle is enabled for any user: what the lens cover becomes in Android."""
    return any(sensor == SENSOR_CAMERA and toggle == TOGGLE_HARDWARE and state == STATE_ENABLED
               for _, sensor, toggle, state in entries)


def sensor_dialog_nodes(dump: str) -> list[str]:
    """The nodes of an interface dump that belong to SystemUI's sensor privacy dialog: its title view, its
    Unblock button and its title and button texts. An unreadable dump raises ValueError."""
    try:
        root = ET.fromstring(dump)
    except ET.ParseError as problem:
        raise ValueError(f"the interface dump is not XML: {problem}") from problem
    found = []
    for node in root.iter("node"):
        if node.get("package") != SENSOR_DIALOG_PACKAGE:
            continue
        identifier, text = node.get("resource-id", ""), node.get("text", "")
        if identifier.endswith(SENSOR_DIALOG_TITLE_ID) or text in SENSOR_DIALOG_TEXTS:
            found.append(f"{identifier or '(no id)'} {text!r}")
    return found


def sensor_dialog_starts(log: str) -> int:
    """How many times the captured log created SystemUI's sensor privacy dialog, from its wm_create_activity
    lines (the events buffer)."""
    return sum(SENSOR_DIALOG_ACTIVITY in line for line in log.splitlines() if "wm_create_activity" in line)


def withheld_reminders(log: str) -> int:
    """How many times the sensor privacy service logged that it withheld the reminder of the camera
    application's use of a blocked camera."""
    return sum(SENSOR_REMINDER_LINE in line and f"({CAMERA_PACKAGE})" in line for line in log.splitlines())


class BlockedSampler:
    """One blocked-preview sample: taken only while the camera application is the resumed activity and the
    device is awake, checked before and after the screenshot. When it is not, the application is brought
    back once for the whole step and the sample is taken again; a second absence is a Failure of its own
    (never a pass and never a skip), naming what was in front."""

    def __init__(self, read: Callable[[], Foreground], take: Callable[[], tuple[float, float]],
                 bring_back: Callable[[], None], sleep: Callable[[float], None]) -> None:
        self.read, self.take, self.bring_back, self.sleep = read, take, bring_back, sleep
        self.brought_back = False

    def __call__(self) -> tuple[float, float]:
        while True:
            state = self.read()
            if state.camera_in_front():
                value = self.take()
                state = self.read()
                if state.camera_in_front():
                    return value
            if self.brought_back:
                raise Failure(f"a blocked session cannot be judged: the camera application is not in front "
                              f"({state.describe()}) after it was brought back once")
            print(f"  blocked: the camera application is not in front ({state.describe()}); bringing it back")
            self.brought_back = True
            self.bring_back()
            self.sleep(BRING_BACK_SETTLE_SECONDS)


def start_camera_application(guest: Guest, action: str = "android.media.action.STILL_IMAGE_CAMERA",
                             front: bool | None = None) -> None:
    """Starts the application: with its `front` extra (the application keeps its last choice otherwise)."""
    for permission in CAMERA_PERMISSIONS:
        guest.shell(f"pm grant {CAMERA_PACKAGE} {permission} 2>&1 || true")
    guest.shell("input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard 2>&1 || true")
    extra = "" if front is None else f" --ez {FRONT_EXTRA} {'true' if front else 'false'}"
    guest.shell(f"am start -a {action}{extra}")


def wait_for_withheld_reminder(guest: Guest) -> None:
    """The camera application has been started in a blocked session: the sensor privacy service says, before
    SENSOR_REMINDER_SECONDS have passed since this call began, that it withheld the application's reminder. The
    wait reads the capture file, never a guest command; a look that completes after the deadline does not count,
    and a capture that has ended is a Failure, not a wait that went on."""
    deadline = time.monotonic() + SENSOR_REMINDER_SECONDS
    while time.monotonic() < deadline:
        guest.alive()
        seen = withheld_reminders(guest.session_log())
        if seen and time.monotonic() <= deadline:
            return
        time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
    raise Failure(f"the sensor privacy service did not withhold the reminder of the blocked camera: no log line "
                  f"{SENSOR_REMINDER_LINE!r} for {CAMERA_PACKAGE} (after {SENSOR_REMINDER_SECONDS} s)")


def check_preview(guest: Guest, plan: Plan, front: bool | None = None) -> None:
    """Live: the application holds a camera and its preview is the chart. Blocked: it is not. `front` is the
    application's camera choice (it keeps its last choice otherwise)."""
    start_camera_application(guest, front=front)
    if plan.mode == "live":
        wait_for("the camera application holding a camera", PREVIEW_SECONDS,
                 lambda: None if camera_connected(guest.media_camera(), CAMERA_PACKAGE)
                 else "no client of the camera service", guest)
    else:
        if guest.hardware_input:
            # The application opens the camera and is told it cannot; the sensor privacy service answers with
            # its log line instead of a dialog, however long the guest takes to get there.
            wait_for_withheld_reminder(guest)
        else:
            time.sleep(15)  # the application opens the camera or is told it cannot; either is judged below
        check_sensor_dialog(guest, plan)
    statistics: list[tuple[float, float]] = []

    def previewing() -> str | None:
        mean, deviation = luma_statistics(guest.screenshot())
        statistics.append((mean, deviation))
        if plan.mode == "live":
            if deviation < CHART_MINIMUM_DEVIATION:
                return f"the preview is flat (mean {mean:.1f}, deviation {deviation:.1f})"
            return None
        if deviation >= CHART_MINIMUM_DEVIATION and mean > BLACK_MAXIMUM_MEAN:
            return f"the preview shows a picture (mean {mean:.1f}, deviation {deviation:.1f})"
        return None

    if plan.mode == "live":
        wait_for("the preview showing the chart", PREVIEW_SECONDS, previewing, guest)
        print(f"  preview: mean {statistics[-1][0]:.1f}, deviation {statistics[-1][1]:.1f}")
        check_sensor_dialog(guest, plan)
    else:
        def take() -> tuple[float, float]:
            guest.alive()
            statistics.append(luma_statistics(guest.screenshot()))
            return statistics[-1]

        def bring_back() -> None:
            # The application left the camera on its own: what the platform logged about it goes out before it is
            # started again, since a restart is also a teardown of that evidence.
            print_application_events(guest, sys.stdout)
            start_camera_application(guest)

        sample = BlockedSampler(lambda: parse_foreground(guest.shell(FOREGROUND_COMMAND, timeout=60)), take,
                                bring_back, time.sleep)
        message = blocked_preview(sample, time.sleep)
        if message:
            raise Failure(f"a blocked session {message}")
        print(f"  blocked preview ({BLOCKED_SAMPLES} samples): mean {statistics[-1][0]:.1f}, "
              f"deviation {statistics[-1][1]:.1f}")


def check_sensor_dialog(guest: Guest, plan: Plan) -> None:
    """The camera application is open (the caller started it and, in a blocked session, waited for the service's
    log line). The camera switch reaches Android as the camera's hardware toggle exactly when the session is
    blocked, and SystemUI's "Unblock" dialog, which cannot lift a hardware toggle, is not on the screen and was
    never started; the sensor privacy service has logged that it withheld the camera's reminder in a blocked
    session, and nothing in a live one."""
    if not guest.hardware_input:
        print("  sensor privacy: not run, the guest has no side keys (start it with --hardware-input)")
        return
    blocked = camera_hardware_blocked(parse_sensor_privacy(guest.sensor_privacy()))
    if blocked != (plan.mode == "blocked"):
        raise Failure(f"the camera's hardware toggle is {'set' if blocked else 'not set'} in a {plan.mode} "
                      "session: the lens cover did not reach Android as the session says")
    nodes = sensor_dialog_nodes(guest.interface_dump())
    if nodes:
        raise Failure(f"SystemUI's sensor privacy dialog is on the screen of the {plan.mode} session: {nodes}")
    withheld = withheld_reminders(guest.session_log())
    if plan.mode == "blocked" and not withheld:
        raise Failure(f"the sensor privacy service did not log {SENSOR_REMINDER_LINE!r} for {CAMERA_PACKAGE} in "
                      "the blocked session: the framework patch is absent, or the reminder was not withheld")
    check_sensor_history(guest, plan)
    print(f"  sensor privacy: hardware toggle {'set' if blocked else 'not set'}; no dialog on the screen, "
          f"SystemUI's dialog never started; reminder withheld {withheld} time(s)")


def check_sensor_history(guest: Guest, plan: Plan) -> None:
    """The absence checks over the whole captured session so far: SystemUI's dialog was never created, and a
    live session never had the camera's reminder withheld."""
    if not guest.hardware_input:
        return
    log = guest.session_log()
    dialogs = sensor_dialog_starts(log)
    if dialogs:
        raise Failure(f"SystemUI's {SENSOR_DIALOG_ACTIVITY} was started {dialogs} time(s) in the {plan.mode} "
                      "session: the platform offered to unblock a camera that no guest setting can unblock")
    withheld = withheld_reminders(log)
    if plan.mode != "blocked" and withheld:
        raise Failure(f"the sensor privacy service withheld the camera's reminder {withheld} time(s) in a {plan.mode} "
                      "session: the camera is not under the hardware toggle there")


def check_still(guest: Guest, plan: Plan, before: set[str]) -> None:
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    if plan.mode == "blocked":
        deadline = time.monotonic() + NO_STILL_SECONDS
        while time.monotonic() < deadline:
            guest.alive()
            added = guest.stills() - before
            if added:
                raise Failure(f"a blocked session took a still: {sorted(added)}")
            time.sleep(2.0)
        print("  blocked: the camera key took nothing")
        return
    found: list[str] = []

    def taken() -> str | None:
        added = sorted(guest.stills() - before)
        if not added:
            return "no new still"
        found[:] = added
        return None

    wait_for("a still in the camera folder", STILL_SECONDS, taken, guest)
    time.sleep(3)  # the file is written through; give the last write time to land
    remote = f"{STILL_DIRECTORY}/{found[-1]}"
    try:
        jpeg = parse_jpeg(guest.pull(remote))
    except ValueError as problem:
        raise Failure(f"{remote} is not a JPEG: {problem}") from problem
    if not jpeg.complete:
        raise Failure(f"{remote} has no end marker: the still was cut short")
    if not jpeg.exif:
        raise Failure(f"{remote} has no Exif data: the HAL writes it into the host's JPEG")
    if min(jpeg.width, jpeg.height) < STILL_MINIMUM_SIDE or jpeg.width * jpeg.height > plan.still_maximum_pixels:
        raise Failure(f"{remote} is {jpeg.width}x{jpeg.height}, outside the {plan.layout.tier or 'small'} "
                      f"tier's stills ({STILL_MINIMUM_SIDE} pixels a side to {plan.still_maximum_pixels} pixels)")
    print(f"  still: {found[-1]} {jpeg.width}x{jpeg.height}")


def parse_lens_buttons(dump: str) -> list[tuple[str, int, int]]:
    """`uiautomator dump`: the buttons inside the application's lens selector, left to right, as (text, x, y) of
    their centres; none without a selector."""
    elements = []
    for element in xml.etree.ElementTree.fromstring(dump).iter("node"):
        match = UI_BOUNDS.fullmatch(element.get("bounds", ""))
        if match:
            elements.append((element, tuple(map(int, match.groups()))))
    selector = [box for element, box in elements if element.get("resource-id", "").endswith(f"/{LENS_SELECTOR}")]
    if not selector:
        return []
    left, top, right, bottom = selector[0]
    inside = [(element.get("text", ""), box) for element, box in elements
              if element.get("class", "").endswith("Button") and box != selector[0]
              and box[0] >= left and box[1] >= top and box[2] <= right and box[3] <= bottom]
    return [(text, (box[0] + box[2]) // 2, (box[1] + box[3]) // 2) for text, box in sorted(inside, key=lambda item: (item[1][0], item[1][1]))]


def ui_dump(guest: Guest) -> str:
    """`uiautomator dump`: the hierarchy of the screen. The tool fails now and then while the screen changes, so it is
    asked again a few times; the file is removed first, so that a failed dump cannot be read as the last one."""
    for attempt in range(UI_DUMP_ATTEMPTS):
        dump = guest.shell(f"rm -f {UI_DUMP}; uiautomator dump {UI_DUMP} >/dev/null 2>&1; cat {UI_DUMP}", timeout=120)
        if "<hierarchy" in dump:
            return dump[dump.index("<hierarchy"):]
        if attempt + 1 < UI_DUMP_ATTEMPTS:
            guest.alive()
            time.sleep(2)
    raise Failure(f"uiautomator printed no hierarchy in {UI_DUMP_ATTEMPTS} attempts")


@dataclass(frozen=True)
class UiNode:
    """One element of `uiautomator dump`, as the runner taps and reads it."""
    id: str  # the resource id without its package
    text: str
    kind: str  # the class
    checked: bool
    x: int  # the centre
    y: int


def parse_ui_nodes(dump: str) -> list[UiNode]:
    nodes = []
    for element in xml.etree.ElementTree.fromstring(dump).iter("node"):
        match = UI_BOUNDS.fullmatch(element.get("bounds", ""))
        if match:
            left, top, right, bottom = map(int, match.groups())
            nodes.append(UiNode(element.get("resource-id", "").split("/")[-1], element.get("text", ""),
                                element.get("class", ""), element.get("checked") == "true",
                                (left + right) // 2, (top + bottom) // 2))
    return nodes


def tap_node(guest: Guest, what: str, *, id: str | None = None, text: str | None = None) -> UiNode:
    """Taps the first element of the screen with this resource id or text."""
    found = [node for node in parse_ui_nodes(ui_dump(guest)) if (id is not None and node.id == id) or (text is not None and node.text == text)]
    if not found:
        raise Failure(f"the application shows no {what}")
    guest.shell(f"input tap {found[0].x} {found[0].y}")
    return found[0]


def select_the_lens_that_learns(guest: Guest, plan: Plan) -> str | None:
    """Makes the application use the last lens of a phone, the last lens button (the application then holds that
    lens's camera); nothing on a host whose application has no buttons. Returns the label of the button it tapped."""
    buttons = parse_lens_buttons(ui_dump(guest))
    if not buttons:
        if plan.profile in LAST_LENS_CAMERA:
            raise Failure(f"the application lists no lens buttons on the {plan.profile} profile, which has more than one back lens")
        return None
    camera = LAST_LENS_CAMERA.get(plan.profile)
    if camera is None:
        raise Failure(f"the application lists lens buttons {[text for text, _, _ in buttons]} and the {plan.profile} profile "
                      "has no lens that the runner knows how to reach")
    text, x, y = buttons[-1]
    guest.shell(f"input tap {x} {y}")
    wait_for(f"the application holding camera {camera} after the tap on the lens button {text!r}", PREVIEW_SECONDS,
             lambda: None if active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE) == {camera}
             else f"it holds {sorted(active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE))}", guest)
    time.sleep(3)  # the preview of the new lens settles before the camera key
    return text


def check_primary_back_camera(guest: Guest) -> None:
    """The application holds the primary back camera, whatever camera an earlier session of the instance left it on:
    its still is the one whose colour data the restart expects to come back as RAW."""
    wait_for("the camera application holding the primary back camera", PREVIEW_SECONDS,
             lambda: None if active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE) == {PRIMARY_BACK_CAMERA}
             else f"it holds {sorted(active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE))}", guest)


def run_restart_checks(guest: Guest, plan: Plan, calibration: pathlib.Path) -> list[str]:
    """The first session of the restart mode: the layout, the cameras, one preview, a still of the primary back
    camera and of the last lens, and the colour data those stills taught in the calibration directory. Returns the
    camera IDs Android lists."""
    print(f"layout {plan.layout.name}, profile {plan.profile}, restart (first session)")
    check_layout(guest, plan)
    check_camera_list(guest, plan)
    before = guest.stills()
    check_preview(guest, plan, front=False)
    check_primary_back_camera(guest)
    check_still(guest, plan, before)
    taught = 1
    lens = select_the_lens_that_learns(guest, plan)
    if lens is not None:
        print(f"  lens: the button {lens!r}")
        check_still(guest, plan, guest.stills())
        taught += 1
    wait_for(f"the colour data of the back stills ({taught}) in the calibration directory", CALIBRATION_SECONDS,
             lambda: None if len(list(calibration.glob("*.cal"))) >= taught
             else f"{len(list(calibration.glob('*.cal')))} .cal files in {calibration}", guest)
    ids = parse_camera_ids(guest.media_camera())
    print(f"  calibration: {sorted(path.name for path in calibration.glob('*.cal'))}")
    return ids


def check_restart_keeps_cameras(guest: Guest, plan: Plan, before: list[str]) -> None:
    """The guest has restarted over the calibration directory that a back still taught: the host composes the
    layout of lenses with colour data (RAW among the capabilities) and Android lists the cameras it listed before,
    one of them stating RAW. A camera that the HAL cannot create while it builds its list is not listed again by
    that boot, and the colour data keeps it out of every later one."""
    try:
        check_layout(guest, plan)
    except Failure as problem:
        raise Failure(f"after the restart: {problem} (did the back still of the first session teach no colour data?)") from problem

    def listed() -> str | None:
        ids = parse_camera_ids(guest.media_camera())
        if sorted(ids) != sorted(before):
            return (f"cameras {ids}, not the {before} of the session before: missing {sorted(set(before) - set(ids))}, "
                    f"extra {sorted(set(ids) - set(before))}")
        return None

    wait_for("the cameras Android lists after the restart (the HAL logs a camera it rejects while it builds its list, "
             "as EmulatedCameraDeviceInfo, and it is not listed again)", RESTART_LIST_SECONDS, listed, guest)
    stating_raw = [info for info in parse_static_info(guest.media_camera()) if "RAW" in info.capabilities]
    if not stating_raw:
        raise Failure("after the restart no camera states the RAW capability, though the host kept a back lens's colour "
                      "data: the run did not exercise a RAW camera")
    problems = raw_metadata_problems(parse_static_info(guest.media_camera()))
    if problems:
        raise Failure("after the restart the cameras that state RAW lack metadata Android requires of one: " + "; ".join(problems))
    check_characteristics(guest, plan)
    print(f"  restart: the cameras {sorted(before)} are listed again, {len(stating_raw)} of them with RAW and the metadata of one")


def enable_raw_capture(guest: Guest) -> None:
    """Switches the camera application's setting "Enable RAW image capture" on, through its pro mode button and its
    settings (the application keeps the choice). A setting that is on already is left on."""
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    start_camera_application(guest, front=False)
    wait_for_chart(guest, PRIMARY_BACK_CAMERA, "photo, before the settings")
    time.sleep(3)
    tap_node(guest, "pro mode button", id=PRO_BUTTON)
    time.sleep(2)
    tap_node(guest, "settings button", id=SETTINGS_BUTTON)
    time.sleep(3)
    guest.shell(f"input swipe {DISPLAY_WIDTH // 2} {DISPLAY_HEIGHT * 3 // 4} {DISPLAY_WIDTH // 2} {DISPLAY_HEIGHT // 4} 400")
    time.sleep(2)

    def switch_of_row() -> UiNode | None:
        nodes = parse_ui_nodes(ui_dump(guest))
        title = next((node for node in nodes if node.text == RAW_SETTING), None)
        if title is None:
            raise Failure(f"the application's settings show no row {RAW_SETTING!r}")
        return next((node for node in nodes if node.kind.endswith("Switch") and abs(node.y - title.y) < 60), None)

    switch = switch_of_row()
    if switch is None:
        raise Failure(f"the row {RAW_SETTING!r} has no switch")
    if not switch.checked:
        tap_node(guest, f"row {RAW_SETTING!r}", text=RAW_SETTING)
        time.sleep(2)
        switch = switch_of_row()
        if switch is None or not switch.checked:
            raise Failure(f"the setting {RAW_SETTING!r} is not on after the tap on its row")
    time.sleep(2)  # the application writes its preference asynchronously
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")


TIFF_SUB_IFDS = 0x014A
TIFF_PHOTOMETRIC = 0x0106
PHOTOMETRIC_CFA = 32803
DNG_VERSION = 0xC612
DNG_COLOR_MATRIX1 = 0xC621


@dataclass(frozen=True)
class Dng:
    """A DNG read as a TIFF: the tags of its first image directory, and the size and depth of its colour filter array image."""
    tags: frozenset[int]
    width: int
    height: int
    bits: int


def parse_dng(data: bytes) -> Dng:
    if data[:4] not in (b"II*\x00", b"MM\x00*"):
        raise ValueError(f"not a TIFF file: it starts with {data[:4]!r}")
    endian = "<" if data[:2] == b"II" else ">"
    kinds = {1: ("B", 1), 3: ("H", 2), 4: ("I", 4), 6: ("b", 1), 8: ("h", 2), 9: ("i", 4), 16: ("Q", 8)}

    def directory(offset: int) -> dict[int, list[int]]:
        try:
            (count,) = struct.unpack_from(endian + "H", data, offset)
            tags: dict[int, list[int]] = {}
            for index in range(count):
                entry = offset + 2 + 12 * index
                tag, kind, number, value = struct.unpack_from(endian + "HHII", data, entry)
                if kind not in kinds or number > 64:
                    tags[tag] = []
                    continue
                letter, size = kinds[kind]
                tags[tag] = list(struct.unpack_from(endian + letter * number, data, entry + 8 if size * number <= 4 else value))
            return tags
        except struct.error as problem:
            raise ValueError(f"a truncated image directory: {problem}") from problem

    top = directory(struct.unpack_from(endian + "I", data, 4)[0])
    images = [top] + [directory(offset) for offset in top.get(TIFF_SUB_IFDS, [])]
    cfa = next((tags for tags in images if tags.get(TIFF_PHOTOMETRIC, [None])[0] == PHOTOMETRIC_CFA), None)
    if cfa is None:
        raise ValueError("no colour filter array image in the file")
    return Dng(frozenset(top), cfa.get(0x0100, [0])[0], cfa.get(0x0101, [0])[0], cfa.get(0x0102, [0])[0])


def check_raw_still(guest: Guest, plan: Plan) -> None:
    """After the restart: a still of the primary back camera, which states RAW, with the application's RAW capture on.
    The application writes a DNG beside its JPEG; the DNG is a TIFF with the DNG version and the camera's colour matrix
    and a colour filter array image that is not smaller than a still can be."""
    raw = [info for info in parse_static_info(guest.media_camera()) if "RAW" in info.capabilities]
    if PRIMARY_BACK_CAMERA not in {info.device for info in raw}:
        raise Failure(f"the primary back camera {PRIMARY_BACK_CAMERA} states no RAW (the cameras with RAW are "
                      f"{sorted(info.device for info in raw)}): there is no RAW request to take")
    enable_raw_capture(guest)
    start_camera_application(guest, front=False)
    wait_for_chart(guest, PRIMARY_BACK_CAMERA, "RAW still")
    time.sleep(3)
    before = parse_listing(guest.shell(f"ls {STILL_DIRECTORY} 2>&1 || true"))
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    found: list[str] = []

    def written() -> str | None:
        added = parse_listing(guest.shell(f"ls {STILL_DIRECTORY} 2>&1 || true")) - before
        dng = sorted(name for name in added if name.lower().endswith(".dng"))
        jpeg = sorted(name for name in added if name.lower().endswith((".jpg", ".jpeg")))
        if not dng or not jpeg:
            return f"new files {sorted(added)}: DNG {dng}, JPEG {jpeg}"
        found[:] = dng
        return None

    wait_for("a DNG and a JPEG in the camera folder", RAW_STILL_SECONDS, written, guest)
    time.sleep(3)  # the files are written through; give the last write time to land
    remote = f"{STILL_DIRECTORY}/{found[-1]}"
    try:
        dng = parse_dng(guest.pull(remote))
    except ValueError as problem:
        raise Failure(f"{remote} is not a DNG: {problem}") from problem
    missing = [name for name, tag in (("DNGVersion", DNG_VERSION), ("ColorMatrix1", DNG_COLOR_MATRIX1)) if tag not in dng.tags]
    if missing:
        raise Failure(f"{remote} has no {' and no '.join(missing)}")
    if min(dng.width, dng.height) < STILL_MINIMUM_SIDE or dng.bits < 10:
        raise Failure(f"{remote} has a colour filter array image of {dng.width}x{dng.height} at {dng.bits} bits")
    print(f"  RAW still: {found[-1]} {dng.width}x{dng.height} at {dng.bits} bits")


@dataclass
class HeldStill:
    phase: str | None  # "inside", "before" or "after" the still's request (see pending_phase); None: no evidence
    evidence: str
    stop: float
    gap: float
    frames: int

    @property
    def overlapped(self) -> bool | None:
        return None if self.phase is None else self.phase == "inside"


def next_held_still_delay(delay: float, phase: str | None, step: float, direction: int) -> tuple[float, float, int]:
    """The delay after the key for the next round, its step and the direction of the last move. A stop that came before
    the still's request moves the delay up, one that came after it moves it down, and a change of direction halves the
    step (to HELD_STILL_DELAY_SMALLEST_STEP at least): the window is narrower than the first step. A stop inside the
    request, or without a report, leaves the delay where it is."""
    move = {"before": 1, "after": -1}.get(phase or "", 0)
    if move == 0:
        return delay, step, direction
    if direction and move != direction:
        step = max(step / 2, HELD_STILL_DELAY_SMALLEST_STEP)
    low, high = HELD_STILL_DELAY_RANGE
    return min(max(delay + move * step, low), high), step, move


def held_still_round(guest: Guest, layer: str, still_streams: set[int], number: int, delay: float) -> HeldStill:
    """One camera key, the emulator stopped HELD_STILL_SECONDS `delay` after it. The preview's frame times are read
    from the moment the emulator is resumed. Raises where the host closed its capture session behind the still or the
    preview stayed away longer than the stop (as measured) and HELD_STILL_RESUME_SECONDS."""
    label = f"held still {number}"
    before = guest.stills()
    log_start = len(guest.launcher_log())
    known = guest.present_times(layer)
    if not known:
        raise Failure(f"{label}: the preview layer has presented no frame")
    started = time.monotonic()
    marker = f"{label.replace(' ', '-')}-{time.monotonic_ns()}"
    guest.shell(f"log -t {HELD_STILL_MARKER_TAG} '{marker}'")
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    time.sleep(delay)
    lead = time.monotonic() - started
    stop = guest.freeze(HELD_STILL_SECONDS)
    seen: set[int] = set()
    observed = round(HELD_STILL_OBSERVE_SECONDS / HELD_STILL_POLL_SECONDS)
    for poll in range(max(observed, round(STILL_SECONDS / HELD_STILL_POLL_SECONDS))):
        frames = guest.present_times(layer)
        if poll == 0 and frames and frames[0] > known[-1]:
            # SurfaceFlinger remembers the last 128 frames: a marker or key command that returned late (an adb that
            # took seconds) left the sample taken before it out of that history, and the stretch between the two
            # is unseen, not a freeze.
            raise Failure(f"{label}: inconclusive: the observation does not cover the round: the frame history no "
                          f"longer reaches back to the sample taken before the key (its oldest frame is "
                          f"{(frames[0] - known[-1]) / 1e9:.1f} s after it; the marker and the key took "
                          f"{lead - delay:.1f} s), so a frame that is missing cannot be told from a freeze")
        seen.update(frames)
        if poll + 1 >= observed and guest.stills() - before:
            break
        time.sleep(HELD_STILL_POLL_SECONDS)
    else:
        raise Failure(f"{label}: a still in the camera folder: no new still")
    closed = session_events(guest.launcher_log()[log_start:]).count("closed")
    if closed:
        raise Failure(f"{label}: the host closed its capture session ({closed} so far) behind a still that held the "
                      "request pipeline: the camera HAL closed the preview's stream")
    newer = sorted(t for t in seen if t > known[-1])
    if not newer:
        raise Failure(f"{label}: the preview presented no frame after the still")
    gap = longest_gap_seconds([known[-1], *newer])
    limit = stop + HELD_STILL_RESUME_SECONDS
    if gap > limit:
        raise Failure(f"{label}: the preview was away {gap:.2f} s, more than the stop ({stop:.2f} s) and "
                      f"{HELD_STILL_RESUME_SECONDS:g} s")
    phase, evidence = pending_phase(guest.dispatcher_log(), marker, still_streams,
                                    lead + stop + HELD_STILL_REPORT_SECONDS)
    return HeldStill(phase, evidence, stop, gap, len(newer))


def check_still_keeps_preview(guest: Guest, plan: Plan) -> None:
    """A still while the preview runs leaves the preview's stream open. The camera HAL's request pipeline is held by
    stopping the emulator process inside the request of the camera key (a host that is slow with the picture does the
    same for as long): a stream that the HAL closed, because it had been unused longer than its limit when the next
    request began, shows as a capture session that the host closed and opened again (the launcher's log). Only
    rounds whose stop demonstrably overlapped a still's request count, HELD_STILL_ROUNDS of them, and the delay after
    the key is found from the evidence of the rounds before (the still's request is a tenth of a second long)."""
    if "opened" not in session_events(guest.launcher_log()):
        raise Failure("the launcher has printed no capture session line, so a check that none closed proves nothing "
                      "(a development launcher prints `Camera: memory: capture session opened|closed`)")
    layer = guest.preview_layer()
    if layer is None:
        raise Failure("the camera application has no preview layer in SurfaceFlinger")
    still_streams = parse_still_streams(guest.media_camera())
    if not still_streams:
        raise Failure("the camera service lists no still output stream (JPEG or RAW) of the open camera")
    counted = 0
    delay, step, direction = HELD_STILL_DELAY_START, HELD_STILL_DELAY_STEP, 0
    for number in range(1, HELD_STILL_ATTEMPTS + 1):
        result = held_still_round(guest, layer, still_streams, number, delay)
        verdict = {"inside": "its stop overlapped a still's request: counted",
                   "before": "its stop came before the still's request began: not counted",
                   "after": "its stop came after the still's request ended: not counted"}.get(
            result.phase or "", "no evidence that its stop overlapped a still's request: not counted")
        print(f"  held still {number} (stop {result.stop:.2f} s, {delay:.2f} s after the key): the preview was away "
              f"{result.gap:.2f} s, {result.frames} frames after it; {result.evidence}; {verdict}")
        counted += result.phase == "inside"
        if counted >= HELD_STILL_ROUNDS:
            break
        delay, step, direction = next_held_still_delay(delay, result.phase, step, direction)
    if counted < HELD_STILL_ROUNDS:
        raise Failure(f"only {counted} of {HELD_STILL_ATTEMPTS} stops overlapped a still's request "
                      f"({HELD_STILL_ROUNDS} are needed): the step cannot show that a still leaves the stream open")
    print(f"  {counted} stops of {HELD_STILL_SECONDS:g} s inside a still's request: the preview's stream stayed open "
          "(no capture session closed)")


def wait_for_camera_held(guest: Guest, camera_id: str, what: str, seconds: float, strict: bool = False) -> None:
    """The application holds camera `camera_id`, within `seconds`."""
    wait_for(f"the camera application holding camera {camera_id} ({what})", seconds,
             lambda: None if camera_id in active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE)
             else f"it holds {sorted(active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE))}", guest, strict)


def wait_for_picture(guest: Guest, what: str, seconds: float, strict: bool = False) -> None:
    """The screen shows the chart (a picture taken now), within `seconds`."""
    seen: list[tuple[float, float]] = []

    def previewing() -> str | None:
        seen.append(luma_statistics(guest.screenshot()))
        mean, deviation = seen[-1]
        return None if deviation >= CHART_MINIMUM_DEVIATION else \
            f"the preview is flat (mean {mean:.1f}, deviation {deviation:.1f})"

    wait_for(f"the {what} preview showing the chart", seconds, previewing, guest, strict)
    print(f"  {what} preview: mean {seen[-1][0]:.1f}, deviation {seen[-1][1]:.1f}")


def wait_for_chart(guest: Guest, camera_id: str, what: str, seconds: float | None = None,
                   deadline: float | None = None) -> None:
    """The application holds camera `camera_id` and its preview is the chart, within `seconds` (PREVIEW_SECONDS) for each
    of the two; with `deadline` (an absolute time.monotonic() value) both share what is left of it."""
    seconds = PREVIEW_SECONDS if seconds is None else seconds

    def left() -> float:
        return seconds if deadline is None else max(0.0, deadline - time.monotonic())

    wait_for_camera_held(guest, camera_id, what, left())
    wait_for_picture(guest, what, left())


def wait_for_fresh_preview(guest: Guest, camera_id: str, what: str, deadline: float) -> None:
    """The viewfinder is live again by `deadline` (an absolute time.monotonic() value), in three stages that share it: the
    application holds the camera; the stream of ITS preview surface produces a new frame (that stream's own counter rises
    between two reads taken after the return: another stream running proves nothing, and the old picture stays on the
    screen when the preview stops); then a picture taken now shows the chart. Every command is bounded by the time left,
    and a check that ends after the deadline does not pass."""
    def left() -> float:
        return max(0.0, deadline - time.monotonic())

    with bounded(guest, deadline):
        wait_for_camera_held(guest, camera_id, what, left(), strict=True)
        seen: list[dict[int, int]] = []

        def producing() -> str | None:
            counts = preview_stream_frames(guest.media_camera())
            previous = seen[-1] if seen else {}
            seen.append(counts)
            if not counts:
                return "the camera service lists no stream of the application's preview surface"
            if any(stream in previous and count > previous[stream] for stream, count in counts.items()):
                return None
            return f"the preview stream's frame counters stand at {counts}"

        wait_for(f"live capture: a new frame of the {what} preview stream", left(), producing, guest, strict=True)
        wait_for_picture(guest, what, left(), strict=True)


def record_video(guest: Guest, plan: Plan, hold_seconds: float, disturb: Callable[[Guest], None] | None = None,
                 maximum: float = VIDEO_MAXIMUM_SECONDS) -> tuple[dict[str, str], Movie]:
    """The camera key starts and stops a recording of the application's video mode on the profile's first camera,
    `hold_seconds` apart; `disturb`, when given, runs right after the start key (and takes part of the hold). Returns
    MediaStore's row of the new movie and the movie as parsed: a finished file of at least VIDEO_MINIMUM_SECONDS and
    at most `maximum` with a video track."""
    before, _ = guest.videos()
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    start_camera_application(guest, VIDEO_ACTION, front=PROFILE_CAMERAS[plan.profile][0] == "front")
    wait_for_chart(guest, plan.camera_ids[0], "video")
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    if disturb is None:
        time.sleep(hold_seconds)
    else:
        started = time.monotonic()
        disturb(guest)
        time.sleep(max(0.0, hold_seconds - (time.monotonic() - started)))
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    raw = ["not queried"]
    found: list[dict[str, str]] = []

    def recorded() -> str | None:
        rows, raw[0] = guest.videos()
        added = [row for _, row in sorted(rows.items()) if row["_id"] not in before]
        if not added:
            return f"MediaStore lists no new movie (query: {raw[0].strip()[-200:]!r})"
        found[:] = added
        return None

    wait_for("a movie in MediaStore", VIDEO_SECONDS, recorded, guest)
    time.sleep(3)  # the file is finished when the application has closed it; give the last write time to land
    path = found[-1].get("_data", "")
    try:
        movie = parse_mp4(guest.pull(path))
    except ValueError as problem:
        raise Failure(f"{path} is not a finished movie: {problem}") from problem
    if not movie.video:
        raise Failure(f"{path} has no video track")
    if not VIDEO_MINIMUM_SECONDS <= movie.seconds <= maximum:
        raise Failure(f"{path} lasts {movie.seconds:.2f} s; the recording ran {hold_seconds:g} s "
                      f"and must last between {VIDEO_MINIMUM_SECONDS} and {maximum:g} s")
    return found[-1], movie


def wait_for_audio_capture(guest: Guest) -> tuple[int, int]:
    """The audio flinger's record thread captures now, not only once did: it is out of standby with an active client
    and its `Frames read` rises between two looks. Returns the two counts."""
    seen: list[int] = []

    def capturing() -> str | None:
        frames = active_capture_frames(guest.audio_flinger())
        if frames is None:
            return "no record thread of the audio flinger is out of standby with an active client"
        previous = seen[-1] if seen else None
        seen.append(frames)
        return None if previous is not None and frames > previous else f"the record thread has read {frames} frames"

    wait_for("audio capture", AUDIO_CAPTURE_SECONDS, capturing, guest)
    return seen[-2], seen[-1]


def disturb_recording(guest: Guest, kind: str, events: list[float]) -> None:
    """Right after the start key of a recording: the audio capture is seen to run, then DISTURB_COUNT disturbances
    (`stall` or `freeze`), the first DISTURB_LEAD_SECONDS later, the others DISTURB_GAP_SECONDS after each end.
    `events` gets the seconds after the start key at which each began."""
    started = time.monotonic()
    first, last = wait_for_audio_capture(guest)
    print(f"  audio capture runs: the record thread has read {first}, then {last} frames")
    for index in range(DISTURB_COUNT):
        time.sleep(DISTURB_LEAD_SECONDS if index == 0 else DISTURB_GAP_SECONDS)
        events.append(time.monotonic() - started)
        if kind == "stall":
            held = guest.hold_audio(STALL_SECONDS)
            print(f"  stall {index + 1}: the sound device's thread held {held:.2f} s from {events[-1]:.1f} s after the start key")
        else:
            guest.freeze(FREEZE_SECONDS)
            print(f"  freeze {index + 1}: the emulator stopped {FREEZE_SECONDS:g} s from {events[-1]:.1f} s after the start key")


def check_audio(path: str, movie: Movie) -> None:
    """The audio track of a recording holds what its time stamps span and ends with its video: its stretch (the time
    line over the coded audio) is within AUDIO_STRETCH_MINIMUM and AUDIO_STRETCH_MAXIMUM, its sample table has no jump
    of AUDIO_JUMP_MAXIMUM_SECONDS, and it ends within AUDIO_END_SKEW_MAXIMUM_SECONDS of the video. The track's sample
    accounting is printed before the checks."""
    if not movie.audio:
        raise Failure(f"{path} has no audio track: the application records with the microphone permission granted")
    if movie.audio_samples == 0 or movie.audio_rate == 0:
        raise Failure(f"{path} has no sample table for its audio track: its stretch cannot be told")
    print(f"  audio: {movie.audio_samples} AAC frames = {movie.audio_coded_seconds:.2f} s of audio over a "
          f"{movie.audio_seconds:.2f} s time line at {movie.audio_rate} Hz, stretch {movie.audio_stretch:.3f}; "
          f"longest step of its sample times {movie.audio_gap:.3f} s, ends {movie.audio_skew:+.3f} s after the video")
    if movie.audio_stretch > AUDIO_STRETCH_MAXIMUM:
        raise Failure(f"{path}: the audio track holds {movie.audio_coded_seconds:.2f} s of audio under a "
                      f"{movie.audio_seconds:.2f} s time line (stretch {movie.audio_stretch:.3f}, above "
                      f"{AUDIO_STRETCH_MAXIMUM}): stamped time has no audio behind it, and a player that follows the "
                      "audio clock skips ahead at every such gap and plays the clip too fast")
    if movie.audio_stretch < AUDIO_STRETCH_MINIMUM:
        raise Failure(f"{path}: the audio track holds {movie.audio_coded_seconds:.2f} s of audio under a "
                      f"{movie.audio_seconds:.2f} s time line (stretch {movie.audio_stretch:.3f}, below "
                      f"{AUDIO_STRETCH_MINIMUM}): more audio than its time stamps span")
    if movie.audio_gap > AUDIO_JUMP_MAXIMUM_SECONDS:
        raise Failure(f"{path}: the audio's sample table jumps {movie.audio_gap:.2f} s between two samples (above "
                      f"{AUDIO_JUMP_MAXIMUM_SECONDS} s): time that the audio does not hold")
    check_audio_end(path, movie)


def check_audio_end(path: str, movie: Movie) -> None:
    if abs(movie.audio_skew) > AUDIO_END_SKEW_MAXIMUM_SECONDS:
        raise Failure(f"{path}: the audio ends {movie.audio_skew:+.2f} s after the video (more than "
                      f"{AUDIO_END_SKEW_MAXIMUM_SECONDS} s either way): the audio and the video are out of step by "
                      "as much as a stall, which frames made up late and stamped in a burst leave")


def check_video(guest: Guest, plan: Plan) -> None:
    """The camera key starts and stops a recording of the application's video mode on the profile's first
    camera: MediaStore lists a new movie of at least VIDEO_MINIMUM_SECONDS with a video track, at the frame rate
    floor, and with an audio track that holds what its time stamps span and ends with the video."""
    row, movie = record_video(guest, plan, VIDEO_HOLD_SECONDS)
    path = row.get("_data", "")
    print(f"  video: {row.get('_display_name', path)} {movie.seconds:.2f} s, {movie.frames} frames over "
          f"{movie.video_seconds:.2f} s = {movie.fps:.1f} fps average, audio {'yes' if movie.audio else 'no'}")
    if movie.frames == 0:
        raise Failure(f"{path} has no sample table for its video track: the frame rate cannot be told")
    if movie.fps < VIDEO_MINIMUM_FPS:
        raise Failure(f"{path} averages {movie.fps:.1f} fps ({movie.frames} frames over {movie.video_seconds:.2f} s), "
                      f"below {VIDEO_MINIMUM_FPS:.0f}: the preview and the recording do not share one exposure")
    check_audio(path, movie)


def start_recording(guest: Guest, plan: Plan, restart_application: bool) -> None:
    """The video mode of the application on the profile's first camera is in front with the chart in its preview, and
    the shutter key has started a recording. With `restart_application` the application is stopped first."""
    if restart_application:
        guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    start_camera_application(guest, VIDEO_ACTION, front=PROFILE_CAMERAS[plan.profile][0] == "front")
    wait_for_chart(guest, plan.camera_ids[0], "video")
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")


def new_movie(guest: Guest, before: dict[str, dict[str, str]], seconds: float, settle: float = 3.0) -> dict[str, str]:
    """MediaStore's row of the movie that appeared since `before` was read, waiting at most `seconds`, then `settle` seconds
    for the application to close the file (a caller that waits for the file itself passes 0: finished_movie)."""
    found: list[dict[str, str]] = []

    def recorded() -> str | None:
        rows, query = guest.videos()
        added = [row for _, row in sorted(rows.items()) if row["_id"] not in before]
        if not added:
            return f"MediaStore lists no new movie (query: {query.strip()[-200:]!r})"
        found[:] = added
        return None

    wait_for("a movie in MediaStore", seconds, recorded, guest)
    if settle > 0:
        time.sleep(settle)  # the file is finished when the application has closed it
    return found[-1]


def finished_movie(guest: Guest, path: str, seconds: float) -> Movie:
    """The movie at `path` once it is a finished one, looked at every half second for at most `seconds` (and at least once). The
    muxer writes the `moov` box when it closes the file, so a file that is still being written does not parse; one that
    never does is the failure at the bound ("is not a finished movie"), as for a clip the application never closed."""
    deadline = time.monotonic() + seconds
    while True:
        guest.alive()
        try:
            return parse_mp4(guest.pull(path))
        except ValueError as problem:
            message = f"{path} is not a finished movie: {problem}"
        if time.monotonic() >= deadline:
            raise Failure(f"{message} (after {seconds:.0f} s)")
        time.sleep(0.5)


def parse_stream_leaks(log: str) -> list[str]:
    """The lines of a `logcat -d -v threadtime` of the camera service's Camera3-Device tag that report a leaked stream."""
    return [line.strip() for line in log.splitlines() if STREAM_LEAK.search(line)]


def require_no_stream_leak(guest: Guest, known: list[str], case: str) -> None:
    """The camera service reports no stream leaked since `known` was taken. A stream is leaked when the HAL ended a request
    without the framework being told that it failed (ERROR_REQUEST): the buffers stay with the framework, and with them the
    references to the stream, so the application's own reopening of the camera fails. Unlike the application's reaction,
    which takes seconds and shows only as a camera application that is no longer in front, the line is there or it is not."""
    new = [line for line in guest.stream_leaks() if line not in known]
    if new:
        raise Failure(f"the camera service reports a leaked stream after the loss in the {case} ({len(new)} line(s), the first: "
                      f"{new[0]!r}): the HAL ended a request of the lost session without the framework being told that it "
                      "failed, so the framework kept its buffers and the stream's references, and the application cannot open "
                      "the camera again")


def check_video_lost(guest: Guest, plan: Plan) -> None:
    """The host ends the video of a camera that records when the application leaves the foreground: the recording ends and its
    clip, which must hold encoded video samples, is saved without a second key press, and the clip does not go on over the
    camera's reopening (no video step across it, the frame rate of a recording, an audio track in step with its video:
    check_clip_of_the_loss; the pictures are not looked at). The pause and resume button is pressed at once after the loss,
    while the stopped recording is not finalized yet: the application ignores it (check_taps_in_the_window), stays in front,
    and a recording works again. That last step STARTS THE APPLICATION AGAIN (a new activity start after the return): it shows
    that the camera can be opened again, not that the viewfinder recovered by itself; the viewfinder cases that follow show
    that. After the loss the camera service's log holds no leaked stream (require_no_stream_leak). The viewfinder cases are
    followed by the losses in the half second before a recording exists, which cancel its start (check_start_window).
    Needs an image whose HAL reports the loss (route p0030) and whose camera application ends a recording on the camera's
    recoverable error (route p0031): CameraX alone keeps the recording over the camera's reopening."""
    before, _ = guest.videos()
    leaks = guest.stream_leaks()
    since = guest_time(guest)
    start_recording(guest, plan, restart_application=True)
    began = time.monotonic()
    button = recording_controls(guest)
    time.sleep(LOST_HOLD_SECONDS)
    held = max(float(LOST_HOLD_SECONDS), time.monotonic() - began)
    guest.camera_control("interrupt background")
    tapped = tap_the_recording_controls(guest, button)
    limit = time.monotonic() + LOST_WAIT_SECONDS
    row = new_movie(guest, before, LOST_WAIT_SECONDS, settle=0)
    path = row.get("_data", "")
    require_no_crash(guest, since, "the pause and resume button was pressed while the recording ended")
    # The clip is finished a second or so after the loss and the runner has no better signal than the file itself: it is
    # pulled until it parses, within what is left of LOST_WAIT_SECONDS, not a fixed time after the row appeared.
    movie = finished_movie(guest, path, max(0.0, limit - time.monotonic()))
    if not movie.video:
        raise Failure(f"{path} has no video track")
    require_media_payload(path, movie)
    if not LOST_MINIMUM_SECONDS <= movie.seconds <= held + LOST_END_SECONDS:
        raise Failure(f"{path} lasts {movie.seconds:.2f} s; the recording ran {held:g} s before the loss "
                      f"and must last between {LOST_MINIMUM_SECONDS:g} s and {held + LOST_END_SECONDS:g} s: "
                      "the clip ends with the camera, and a recording that goes on over the camera's reopening keeps "
                      "what the new session delivers")
    print(f"  the recording ended by itself: {row.get('_display_name', path)} {movie.seconds:.2f} s, {movie.frames} frames")
    check_clip_of_the_loss(path, movie)
    check_taps_in_the_window(guest, since, tapped)
    foreground = parse_foreground(guest.shell(FOREGROUND_COMMAND))
    if not foreground.resumed.startswith(CAMERA_PACKAGE):
        raise Failure(f"the camera application is not in front after the recording ended: {foreground.describe()}")
    print(f"  the application stays in front: {foreground.describe()}")
    # What the application does with its camera is its own: the open camera is reported, not required.
    held_cameras = sorted(active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE))
    print(f"  cameras the application holds after the loss: {held_cameras or 'none'}")
    require_no_stream_leak(guest, leaks, "recording")
    guest.camera_control("resume")
    after, _ = guest.videos()
    start_recording(guest, plan, restart_application=False)  # a new activity start: see the docstring
    time.sleep(VIDEO_HOLD_SECONDS)
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    again = new_movie(guest, after, VIDEO_SECONDS)
    reopened = parse_mp4(guest.pull(again.get("_data", "")))
    if not reopened.video or reopened.frames <= 0 or reopened.seconds < VIDEO_MINIMUM_SECONDS:
        raise Failure(f"the recording after the person returned is not a movie of at least {VIDEO_MINIMUM_SECONDS:g} s "
                      f"with encoded video samples: {reopened.seconds:.2f} s, video track {'yes' if reopened.video else 'no'}, "
                      f"{reopened.frames} frames")
    require_media_payload(again.get("_data", ""), reopened)
    print(f"  the camera opened again for a new start of the application: {again.get('_display_name', '')} "
          f"{reopened.seconds:.2f} s, {reopened.frames} frames")
    require_no_stream_leak(guest, leaks, "recording")
    print("  the camera service reports no leaked stream")
    if HAL_END_LINE not in guest.hal_log():
        raise Failure("the camera HAL logged no end of the camera: the image lacks the HAL topic of route p0030")
    print("  the HAL reported the end of the camera (ERROR_DEVICE besides the request errors)")
    check_viewfinder_returns(guest, plan, "video")
    check_viewfinder_returns(guest, plan, "photo")
    check_start_window(guest, plan)


def guest_time(guest: Guest) -> float:
    """The guest's clock in seconds since the epoch, the time base of `logcat -v epoch`."""
    return float(guest.shell("date +%s.%N").strip())


# ---- The video-leave mode.

# The launcher's own lines about the leaving (background_edge_control.hpp); the time is the host's clock, in seconds since the epoch.
EDGE_LEFT = re.compile(r"edge control: (\d+\.\d+) the application leaves the foreground")
# The gate closes at once; the launcher's call then waits (a second at most) for a request that is running, and says when it
# returned. The ordering of the guest's events against the hold is judged against the closing.
EDGE_CLOSED = re.compile(r"edge control: (\d+\.\d+) the GPU gate closed")
EDGE_DRAINED = re.compile(r"edge control: (\d+\.\d+) GPU requests drained \((-?\d+)\) after (\d+) ms: video streams (\d+), "
                          r"stopped (yes|no), answered (yes|no)")
# What the guest's log says of the loss, by tag and message: the camera HAL's report of it, an ordinary failed buffer (the
# loss must come first), the recorder's stop, the video encoder's, and the clip's finalization.
LEAVE_LOG_TAGS = ("V4l2FrameSource:W EmulatedSensorHost:W Camera3-Device:E CameraActivity:E CameraViewModel:D Recorder:D "
                  "VideoEncoder:D AudioEncoder:D")
LEAVE_EVENTS = (
    ("hal_end", "V4l2FrameSource", "the host ended the camera"),
    ("hal_frame_failed", "EmulatedSensorHost", "failed: none of its"),
    ("rec_stopping", "Recorder", "RECORDING --> STOPPING"),
    ("venc_stopped", "VideoEncoder", "STOPPING --> CONFIGURED"),
    ("finalize", "Recorder", "VideoRecordEvent$Finalize"),
)


@dataclass
class Edge:
    """What the launcher said of one leaving: the host's time of the leaving, of the gate's closing and of the end of the wait
    for a request that was running (the hold's return), how long the launcher waited for the guest, the video streams it
    ended, whether the guest stopped them and whether the camera device answered."""
    left: float
    closed: float
    drained: float
    result: int
    waited_ms: int
    streams: int
    stopped: bool
    answered: bool


def parse_edge(log: str, since: int = 0) -> Edge | None:
    """The first leaving in the launcher's `log` after offset `since` whose hold has landed, or None."""
    text = log[since:]
    left = EDGE_LEFT.search(text)
    if not left:
        return None
    closed = EDGE_CLOSED.search(text, left.end())
    if not closed:
        return None
    drained = EDGE_DRAINED.search(text, closed.end())
    if not drained:
        return None
    return Edge(float(left[1]), float(closed[1]), float(drained[1]), int(drained[2]), int(drained[3]), int(drained[4]),
                drained[5] == "yes", drained[6] == "yes")


def leave_events(log: str, since: float) -> dict[str, float]:
    """The guest time of the first line of each LEAVE_EVENTS at or after `since`, from a `logcat -v epoch`."""
    found: dict[str, float] = {}
    for when, tag, message in parse_application_events(log, since):
        for key, want_tag, want in LEAVE_EVENTS:
            if key not in found and tag == want_tag and want in message:
                found[key] = when
    return found


@dataclass
class LeaveTrial:
    number: int
    notice_gap: float          # the platform's notice came this long after the leaving (0: first)
    edge: Edge
    lost_at: float             # the host's time of the first line written to a pipe
    skew: float                # the guest's clock minus the host's
    clock_error: float         # how far out that is at most
    events: dict[str, float]   # the guest's time of each LEAVE_EVENTS
    clip_seconds: float
    video_end: float
    audio_end: float
    record_bound_hit: bool = False   # the launcher said that the guest did not read the interrupted record in time


def leave_failures(trial: LeaveTrial) -> list[str]:
    """Why a leaving does not pass, in words: empty for one that does."""
    reasons = []
    edge, events = trial.edge, trial.events
    margin = max(LEAVE_CLOCK_MARGIN_SECONDS, trial.clock_error)
    closed = edge.closed + trial.skew + margin
    if not edge.answered:
        reasons.append("the camera device did not answer the end of the video")
    elif edge.streams < 1:
        reasons.append("the launcher found no video stream to end although a recording ran")
    for key, what in (("hal_end", "the camera HAL's report of the loss"), ("rec_stopping", "the recorder's stop")):
        if key not in events:
            reasons.append(f"{what} is not in the guest's log")
        elif events[key] > closed:
            reasons.append(f"{what} came {events[key] - closed + margin:.2f} s after the gate closed")
    if "hal_end" in events and events.get("hal_frame_failed", events["hal_end"]) < events["hal_end"]:
        reasons.append("the first failed buffer of the camera HAL was an ordinary failure, not the loss")
    if trial.record_bound_hit:
        reasons.append("the guest did not read the interrupted record of the lost video within the device's bound")
    tail = trial.audio_end - trial.video_end
    if tail > LEAVE_AUDIO_TAIL_MAXIMUM_SECONDS:
        reasons.append(f"the clip's audio ends {tail:.2f} s after its video (above {LEAVE_AUDIO_TAIL_MAXIMUM_SECONDS:g} s): the "
                       "recording went on in silence")
    return reasons


def clock_offset(guest: Guest) -> tuple[float, float]:
    """The guest's clock minus the host's, from the one of seven `date` calls with the shortest round trip, and that
    round trip (half of it is how far out the offset is at most)."""
    best: tuple[float, float] | None = None
    for _ in range(7):
        before = time.time()
        guest_now = guest_time(guest)
        after = time.time()
        if best is None or after - before < best[1]:
            best = (guest_now - (before + after) / 2, after - before)
    assert best is not None
    return best


def leave_trial(guest: Guest, plan: Plan, number: int, notice_gap: float) -> LeaveTrial:
    """One recording that ends as the application leaves: the launcher ends its video and holds the guest's GPU requests
    for LEAVE_AWAY_SECONDS, the person returns, and the clip is read."""
    before, _ = guest.videos()
    mark = len(guest.launcher_log())
    since = guest_time(guest) - 1.0
    start_recording(guest, plan, restart_application=True)
    time.sleep(LOST_HOLD_SECONDS)
    skew, round_trip = clock_offset(guest)
    lost_at = time.time()
    if notice_gap == 0.0:
        guest.camera_control("interrupt background")
        guest.edge_control("leave")
    else:
        guest.edge_control("leave")
        time.sleep(notice_gap)
        guest.camera_control("interrupt background")
    found: list[Edge] = []

    def held() -> str | None:
        edge = parse_edge(guest.launcher_log(), mark)
        if edge is None:
            return "the launcher has not said that it held the GPU requests"
        found.append(edge)
        return None

    wait_for("the hold of the guest's GPU requests", LEAVE_EDGE_SECONDS, held, guest)
    edge = found[0]
    # The person is away LEAVE_AWAY_SECONDS from the hold, then back: the requests are released and the camera is the guest's again.
    time.sleep(max(0.0, edge.drained + LEAVE_AWAY_SECONDS - time.time()))
    guest.edge_control("return")
    guest.camera_control("resume")
    row = new_movie(guest, before, LOST_WAIT_SECONDS, settle=0)
    movie = finished_movie(guest, row.get("_data", ""), LOST_WAIT_SECONDS)
    time.sleep(2)
    log = guest.shell(f"logcat -d -v epoch -b all {LEAVE_LOG_TAGS} '*:S' 2>&1 || true")
    events = leave_events(log, since)
    keep_leave_log(guest, number)
    bound_hit = RECORD_BOUND_LINE in guest.launcher_log()[mark:]
    time.sleep(3)
    return LeaveTrial(number, notice_gap, edge, lost_at, skew, round_trip / 2, events, movie.seconds, movie.video_end,
                      movie.audio_end, bound_hit)


def keep_leave_log(guest: Guest, number: int) -> None:
    """The guest's whole log up to now, in the run's output directory when it has one: what to read when a trial fails."""
    directory = guest.output_directory
    if directory is None:
        return
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"leave-{number}.log").write_text(guest.shell("logcat -d -v epoch -b all 2>&1 || true"), errors="replace")


def describe_leave(trial: LeaveTrial) -> str:
    """One line for a trial: the times from the loss, in milliseconds, of what the guest and the launcher did."""
    base = trial.lost_at + trial.skew

    def rel(key: str) -> str:
        return f"{(trial.events[key] - base) * 1000:+.0f}" if key in trial.events else "-"
    edge = trial.edge
    return (f"leave {trial.number} (notice {trial.notice_gap * 1000:+.0f} ms): the launcher waited {edge.waited_ms} ms for "
            f"{edge.streams} stream(s) ({'stopped' if edge.stopped else 'NOT stopped'}); ms from the loss: HAL report "
            f"{rel('hal_end')}, first ordinary failure {rel('hal_frame_failed')}, Recorder STOPPING {rel('rec_stopping')}, "
            f"VideoEncoder stopped {rel('venc_stopped')}, Finalize {rel('finalize')}, GPU gate closed "
            f"{(edge.closed + trial.skew - base) * 1000:+.0f}, drained {(edge.drained + trial.skew - base) * 1000:+.0f}, released "
            f"{(edge.drained + trial.skew - base + LEAVE_AWAY_SECONDS) * 1000:+.0f}; "
            f"clip {trial.clip_seconds:.2f} s, video ends {trial.video_end:.2f} s, audio ends {trial.audio_end:.2f} s "
            f"({trial.audio_end - trial.video_end:+.2f} s)")


def check_video_leave(guest: Guest, plan: Plan) -> None:
    """The application leaves the foreground while the guest records, and the guest's GPU requests are held, `leave_trials`
    times: in every one the recording ends when the application leaves (the camera HAL reports the loss and the recorder
    stops before the hold lands, which the launcher waited for) and no sound goes on in silence behind a stalled picture."""
    time.sleep(1)
    failed = []
    for number in range(1, plan.leave_trials + 1):
        try:
            trial = leave_trial(guest, plan, number, LEAVE_NOTICE_GAPS[(number - 1) % len(LEAVE_NOTICE_GAPS)])
        except Failure as problem:
            # A leaving that left the guest with no clip (the recording went on stalled) fails its own trial, not the ones after
            # it: the guest is given its GPU requests and its camera back, and the next trial starts the application again.
            print(f"  leave {number}: the trial itself failed: {str(problem)[:300]} -- FAILS", flush=True)
            failed.append((number, [f"the trial itself failed: {str(problem)[:200]}"]))
            for give_back in (lambda: guest.edge_control("return"), lambda: guest.camera_control("resume")):
                try:
                    give_back()
                except Failure:
                    pass
            time.sleep(10)
            continue
        reasons = leave_failures(trial)
        print(f"  {describe_leave(trial)}{'' if not reasons else ' -- FAILS: ' + '; '.join(reasons)}", flush=True)
        if reasons:
            failed.append((number, reasons))
    if failed:
        raise Failure(f"{len(failed)} of {plan.leave_trials} leavings did not end the recording at the leave: "
                      + " | ".join(f"leave {number}: {'; '.join(reasons)}" for number, reasons in failed))
    print(f"  all {plan.leave_trials} leavings ended the recording before the GPU was held")


APP_LOG_LINE = re.compile(r"^\s*(\d+\.\d+)\s+\d+\s+\d+\s+[VDIWEF]\s+(\S+?)\s*:\s(.*)$")


def parse_application_events(log: str, since: float) -> list[tuple[float, str, str]]:
    """The (time, tag, message) of the lines of a `logcat -v epoch` at or after `since`."""
    events = []
    for line in log.splitlines():
        hit = APP_LOG_LINE.match(line)
        if hit and float(hit[1]) >= since:
            events.append((float(hit[1]), hit[2], hit[3]))
    return events


def application_events(guest: Guest, since: float) -> list[tuple[float, str, str]]:
    return parse_application_events(guest.shell(f"logcat -d -v epoch -b all {APP_LOG_TAGS} '*:S' 2>&1 || true"), since)


def recording_controls(guest: Guest) -> tuple[int, int]:
    """The centre of the pause and resume button, which the screen shows while a recording runs."""
    for node in parse_ui_nodes(ui_dump(guest)):
        if node.id == UI_STATE_BUTTON:
            return node.x, node.y
    raise Failure("the application shows no pause and resume button while it records")


def tap_the_recording_controls(guest: Guest, button: tuple[int, int]) -> int:
    """LOST_TAPS presses of the button at once (separate input processes started together, so that they land within the
    second after the loss that the stopped recording needs to finalize). Returns how many were sent."""
    x, y = button
    guest.shell(" ".join(f"input tap {x} {y} &" for _ in range(LOST_TAPS)) + " wait")
    return LOST_TAPS


def require_no_crash(guest: Guest, since: float, what: str) -> None:
    """The camera application has not crashed since `since`: the crash buffer holds no fatal exception of its process."""
    lines = [line.strip() for line in guest.shell("logcat -d -v epoch -b crash 2>&1 || true").splitlines()
             if line.strip()[:1].isdigit() and float(line.split()[0]) >= since]
    if any(f"Process: {CAMERA_PACKAGE}" in line for line in lines):
        detail = [line.split(None, 5)[-1] for line in lines if "Exception" in line or "Error" in line][:2]
        raise Failure(f"the camera application crashed after the loss ({what}): {' | '.join(detail) or lines[:2]}")


def check_taps_in_the_window(guest: Guest, since: float, sent: int) -> None:
    """The application's log counts the presses it ignored for the stopped recording (it logs each one, as long as the recording's
    ending mark stands) and the presses that found no recording at all. The Finalize line does not end the mark: the application
    broadcasts the clip and resets its state after it, so a press that came after that line and was ignored is an ignored press
    too, counted apart for the line. At least one press must have been ignored: otherwise the row has shown nothing."""
    events = [(when, message) for when, tag, message in application_events(guest, since) if tag == "CameraViewModel"]
    finalized = next((when for when, message in events if APP_FINALIZE_LINE in message), None)
    ignored = [when for when, message in events if APP_IGNORED_LINE in message]
    inside = [when for when in ignored if finalized is None or when <= finalized]
    without = sum(APP_NO_RECORDING_LINE in message for _, message in events)
    print(f"  the pause and resume button was pressed {sent} times at once after the loss: {len(inside)} presses landed "
          f"while the recording ended and were ignored, {len(ignored) - len(inside)} more were ignored after its Finalize line "
          f"but before the state was reset, {without} found no recording (the application's log)")
    if not ignored:
        raise Failure("no press of the pause and resume button was ignored for the stopped recording, so the application's "
                      "handling of it was not exercised (the image lacks the application's topic of route p0031, or every "
                      "press came after the state was reset)")


def finished_new_movies(guest: Guest, before: dict[str, dict[str, str]], seconds: float,
                        minimum: float = 0.0) -> list[tuple[dict[str, str], Movie]]:
    """The movies that appeared since `before` and parse as finished ones, looked at every half second until one lasts at least
    `minimum` seconds or every new row parses (and there is one), for at most `seconds`; then what parsed (the caller judges it),
    or the failure if nothing did. A file that is still being written is not a finished movie and is left out: a clip that a loss
    ended can be finished while the one a later key stopped is not yet, so the first look does not decide."""
    deadline = time.monotonic() + seconds
    while True:
        guest.alive()
        found, unfinished = [], 0
        rows, _ = guest.videos()
        for _, row in sorted(rows.items()):
            if row["_id"] in before:
                continue
            try:
                found.append((row, parse_mp4(guest.pull(row.get("_data", "")))))
            except ValueError:
                unfinished += 1
        if found and (not unfinished or any(movie.seconds >= minimum for _, movie in found)):
            return found
        if time.monotonic() >= deadline:
            if found:
                return found
            raise Failure(f"no movie that appeared since the start key is a finished one after {seconds:.0f} s")
        time.sleep(0.5)


def check_start_window(guest: Guest, plan: Plan) -> None:
    """A loss in the half second between the start key and the creation of the recording (the start sound) finds no recording to
    stop: the application cancels the start and stays idle. START_WINDOW_ATTEMPTS times: the start key, the loss at once, the
    return, a wait, then the key again, a hold and the key again. A cancelled start leaves the viewfinder idle, so the second key
    starts the recording that is held and the third stops it: one clip of about START_WINDOW_HOLD_SECONDS, no longer than the
    measured time between those two keys and START_WINDOW_MARGIN_SECONDS, and with encoded video in it. A start that was not
    cancelled makes a recording that goes on over the loss, which the second key stops: a clip as long as the wait. The
    application's log says which attempts cancelled a start or stopped a recording (the conclusive ones: START_WINDOW_CONCLUSIVE_MINIMUM
    are needed, one of them a cancel) and the clip of any other attempt is not credited."""
    camera = plan.camera_ids[0]
    leaks = guest.stream_leaks()
    start_camera_application(guest, VIDEO_ACTION, front=PROFILE_CAMERAS[plan.profile][0] == "front")
    wait_for_chart(guest, camera, "video")
    cancelled = concluded = 0
    for number in range(1, START_WINDOW_ATTEMPTS + 1):
        before, _ = guest.videos()
        since = guest_time(guest)
        guest.shell(f"input keyevent {CAMERA_KEYCODE}")
        guest.camera_control("interrupt background")
        time.sleep(LOST_END_SECONDS)
        guest.camera_control("resume")
        wait_for_chart(guest, camera, "video viewfinder", VIEWFINDER_RECOVERY_SECONDS)
        time.sleep(START_WINDOW_SETTLE_SECONDS)
        keys_began = time.monotonic()
        guest.shell(f"input keyevent {CAMERA_KEYCODE}")
        time.sleep(START_WINDOW_HOLD_SECONDS)
        guest.shell(f"input keyevent {CAMERA_KEYCODE}")
        # From before the key that starts the recording to after the key that stops it: the recording cannot last longer.
        held = max(float(START_WINDOW_HOLD_SECONDS), time.monotonic() - keys_began)
        longest = held + START_WINDOW_MARGIN_SECONDS
        require_no_crash(guest, since, f"start window attempt {number}")
        movies = finished_new_movies(guest, before, LOST_WAIT_SECONDS, START_WINDOW_MINIMUM_SECONDS)
        messages = [message for _, tag, message in application_events(guest, since) if tag == "CameraViewModel"]
        # The key that stops the recording of the attempt logs a stop of its own, so a loss that stopped a recording that had
        # started is the second stop line.
        stops = sum(APP_STOPPED_LINE in message for message in messages)
        verdict = ("the start was cancelled" if any(APP_CANCELLED_LINE in message for message in messages)
                   else "the recording had started and the loss stopped it" if stops >= 2
                   else "the loss cancelled and stopped nothing in the log" if stops == 1
                   else "no cancel or stop in the log")
        cancelled += verdict == "the start was cancelled"
        conclusive = verdict in ("the start was cancelled", "the recording had started and the loss stopped it")
        concluded += conclusive
        clips = ", ".join(f"{row.get('_display_name', '')} {movie.seconds:.2f} s" for row, movie in movies)
        print(f"  start window {number} of {START_WINDOW_ATTEMPTS}: {verdict if conclusive else 'inconclusive, ' + verdict}; clips: {clips}")
        for row, movie in movies:
            if movie.seconds > longest:
                raise Failure(f"{row.get('_data', '')} lasts {movie.seconds:.2f} s (above {longest:.1f} s: the {held:.1f} s between "
                              f"the keys that start and stop the recording and {START_WINDOW_MARGIN_SECONDS:g} s): the recording "
                              "that was started just before the loss went on over the camera's reopening instead of ending, and "
                              "the key that should have started a recording stopped it")
        recordings = [(row, movie) for row, movie in movies if movie.seconds >= START_WINDOW_MINIMUM_SECONDS]
        if not conclusive:
            recordings = []  # not credited as the recording after the return, and not judged: see START_WINDOW_CONCLUSIVE_MINIMUM
        elif not recordings:
            raise Failure(f"the recording started after the loss in attempt {number} lasts under {START_WINDOW_MINIMUM_SECONDS:g} s "
                          f"({clips}): the key did not start a recording")
        # The recording that follows the loss holds encoded video, not only a container of the right length.
        for row, movie in recordings:
            path = row.get("_data", "")
            if not movie.video:
                raise Failure(f"{path} (start window attempt {number}) has no video track")
            require_media_payload(path, movie)
        foreground = parse_foreground(guest.shell(FOREGROUND_COMMAND))
        if not foreground.resumed.startswith(CAMERA_PACKAGE):
            raise Failure(f"the camera application is not in front after start window attempt {number}: {foreground.describe()}")
    require_no_stream_leak(guest, leaks, "start window")
    print(f"  the loss cancelled the start in {cancelled} of {START_WINDOW_ATTEMPTS} attempts ({concluded} conclusive); the application "
          "stayed in front and a recording worked after each conclusive one")
    if concluded < START_WINDOW_CONCLUSIVE_MINIMUM:
        raise Failure(f"the application's log says what the loss did in {concluded} of {START_WINDOW_ATTEMPTS} attempts, "
                      f"{START_WINDOW_CONCLUSIVE_MINIMUM} needed: the other clips are not credited as the recording after the return "
                      "(the loss came before the key reached the application, or the image lacks the application's topic of route p0031)")
    if not cancelled:
        raise Failure(f"no loss cancelled a start in {START_WINDOW_ATTEMPTS} attempts (the application's log): the loss came after "
                      "the recording was created each time, or the image lacks the application's topic of route p0031")


def check_clip_of_the_loss(path: str, movie: Movie) -> None:
    """The clip a loss ends does not go on over the camera's reopening: its frames over its duration reach VIDEO_MINIMUM_FPS
    and no two video samples are more than VIDEO_JUMP_MAXIMUM_SECONDS apart (a recording that goes on over the camera's
    reopening has the camera's absence as a step, and then the frames of the session the host holds), and its audio holds
    what its time stamps span and ends with its video (check_audio). These are timing and cadence checks: the pictures are not
    looked at (the runner reads the container's tables and decodes no video), so a steady dark tail would pass. The track's numbers are
    printed before the checks."""
    print(f"  clip: {movie.frames} frames over {movie.video_seconds:.2f} s = {movie.fps:.1f} fps average, longest step of "
          f"its video sample times {movie.video_gap:.3f} s")
    if movie.fps < VIDEO_MINIMUM_FPS:
        raise Failure(f"{path} averages {movie.fps:.1f} fps ({movie.frames} frames over {movie.video_seconds:.2f} s), below "
                      f"{VIDEO_MINIMUM_FPS:.0f}: the camera was away for a part of the clip")
    if movie.video_gap > VIDEO_JUMP_MAXIMUM_SECONDS:
        raise Failure(f"{path}: the video's sample table steps {movie.video_gap:.2f} s between two samples (above "
                      f"{VIDEO_JUMP_MAXIMUM_SECONDS} s): a gap where the camera was away, which the recording went on over")
    check_audio(path, movie)


def require_media_payload(path: str, movie: Movie) -> None:
    """A clip passes only with encoded video in it: its video track declares samples (the sample table) and its `mdat` boxes
    hold the bytes they need, MINIMUM_SAMPLE_BYTES for each, not a table of counts over an empty file."""
    if movie.frames <= 0:
        raise Failure(f"{path} has a video track but no encoded video samples")
    needed = movie.frames * MINIMUM_SAMPLE_BYTES
    if movie.media_bytes < needed:
        raise Failure(f"{path} declares {movie.frames} video samples but its media data holds {movie.media_bytes} bytes "
                      f"(at least {needed} needed): the clip has no encoded video")


def application_pids(guest: Guest) -> str:
    return guest.shell(f"pidof {CAMERA_PACKAGE} || true").strip()


def check_viewfinder_returns(guest: Guest, plan: Plan, mode: str) -> None:
    """A viewfinder with NO recording. The application leaves the foreground and comes back; the viewfinder is live
    again within VIEWFINDER_RECOVERY_SECONDS of the return (one deadline for the camera, the picture and the frame
    counters) without anything restarting the application (the same process, the same window, no new activity start)
    and with no error window. `mode` is "video" (the video mode's viewfinder, which binds an encoder surface: the HAL
    ends the camera for it as for a recording, printed, not required) or "photo" (the photo mode's viewfinder has no
    encoder stream: the HAL must not tell it the device is gone, it closes the camera's video streams and opens them
    again, once for the loss, and the log says so)."""
    camera = plan.camera_ids[0]
    photo = mode == "photo"
    label = "photo viewfinder" if photo else "video viewfinder"
    if photo:
        start_camera_application(guest, STILL_ACTION, front=PROFILE_CAMERAS[plan.profile][0] == "front")
    wait_for_chart(guest, camera, label)
    process = application_pids(guest)
    crashes = guest.shell("logcat -d -b events | grep -a -E 'am_crash|am_anr|am_proc_died' || true")
    log = guest.hal_log()
    ended, reopened = log.count(HAL_END_LINE), log.count(HAL_REOPEN_LINE)
    leaks = guest.stream_leaks()
    guest.camera_control("interrupt background")
    time.sleep(LOST_AWAY_SECONDS)
    log = guest.hal_log()
    lost, reopening = log.count(HAL_END_LINE) > ended, log.count(HAL_REOPEN_LINE) > reopened
    held = sorted(active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE))
    print(f"  {label}: the HAL {'ended' if lost else 'did not end'} the camera and "
          f"{'reopened' if reopening else 'did not reopen'} its video streams; the application holds {held or 'none'} while away")
    if photo and lost:
        raise Failure("the HAL told the photo viewfinder that the device is gone (ERROR_DEVICE): only a session with an encoder "
                      "stream may be ended")
    require_no_stream_leak(guest, leaks, label)
    guest.camera_control("resume")
    returned = time.monotonic()
    deadline = returned + VIEWFINDER_RECOVERY_SECONDS
    wait_for_fresh_preview(guest, camera, f"{label} after the return", deadline)
    print(f"  {label}: live again {time.monotonic() - returned:.1f} s after the return (limit {VIEWFINDER_RECOVERY_SECONDS} s)")
    if application_pids(guest) != process:
        raise Failure(f"the application was restarted (process {process!r}, then {application_pids(guest)!r}) "
                      "instead of showing its viewfinder again")
    foreground = parse_foreground(guest.shell(FOREGROUND_COMMAND))
    if not foreground.resumed.startswith(CAMERA_PACKAGE) or ERROR_WINDOW.search(foreground.focus):
        raise Failure(f"the application does not show its viewfinder after the return: {foreground.describe()}")
    after = guest.shell("logcat -d -b events | grep -a -E 'am_crash|am_anr|am_proc_died' || true")
    if after != crashes:
        raise Failure(f"the application crashed or stopped responding around the loss: {after[len(crashes):][-300:]!r}")
    require_no_stream_leak(guest, leaks, label)
    print(f"  {label}: the camera service reports no leaked stream")
    reopens = guest.hal_log().count(HAL_REOPEN_LINE) - reopened
    if photo and reopens == 0:
        raise Failure("the HAL logged no reopening of its video streams: the host did not end the camera's video, or the image "
                      "lacks the HAL topic of route p0030")
    if photo and reopens != 1:
        raise Failure(f"the HAL closed and reopened its video streams {reopens} times for one loss (one expected): the host "
                      "ended the camera's video again for each session the HAL opened while the application was away, instead "
                      "of holding one session, interrupted, until the return")
    print(f"  {label}: live again after the return, same process {process}, no error window"
          + (", one reopening of the video streams" if photo else ""))


def check_audio_stall(guest: Guest, plan: Plan) -> None:
    """A recording during which the thread that serves the Virtio sound device is held STALL_SECONDS, DISTURB_COUNT
    times, with the audio capture seen to run first: the video goes on (no gap of VIDEO_GAP_MAXIMUM_SECONDS) while the
    audio input has nothing to read, and the audio track must still hold what its time stamps span, have no jump and
    end with the video, as the input makes the stalled time up. The HAL must have said it made silence up, for at least
    MADE_UP_MINIMUM_FRACTION of the stalls' time. Without --audio-hold-library the step says it did not run."""
    if guest.hold_directory is None:
        print("  audio stall: not run (no --audio-hold-library)")
        return
    events: list[float] = []
    before = made_up_frames(guest.stream_log())
    row, movie = record_video(guest, plan, DISTURB_HOLD_SECONDS, lambda g: disturb_recording(g, "stall", events),
                              DISTURB_MAXIMUM_SECONDS)
    path = row.get("_data", "")
    print(f"  video with the sound device held {DISTURB_COUNT} x {STALL_SECONDS:g} s: {row.get('_display_name', path)} "
          f"{movie.seconds:.2f} s, {movie.frames} frames over {movie.video_seconds:.2f} s, longest gap in the video "
          f"{movie.video_gap:.3f} s")
    if movie.video_gap > VIDEO_GAP_MAXIMUM_SECONDS:
        raise Failure(f"{path}: the video has a gap of {movie.video_gap:.2f} s (above {VIDEO_GAP_MAXIMUM_SECONDS} s): "
                      "the guest stood still as well, and its audio gap could not be told from the video's")
    check_audio(path, movie)
    made_up = made_up_frames(guest.stream_log())[len(before):]
    needed = MADE_UP_MINIMUM_FRACTION * DISTURB_COUNT * STALL_SECONDS * (movie.audio_rate or 48000)
    print(f"  the HAL made {sum(made_up)} frames of silence up in {len(made_up)} stall(s) ({needed:.0f} needed)")
    if sum(made_up) < needed:
        raise Failure(f"the input's log says it made {sum(made_up)} frames of silence up in {len(made_up)} stall(s) "
                      f"during {DISTURB_COUNT} stalls of {STALL_SECONDS:g} s (at least {needed:.0f} frames wanted): "
                      "it did not make the stalled time up")


def check_video_freeze(guest: Guest, plan: Plan) -> None:
    """A recording during which the emulator process is stopped FREEZE_SECONDS, DISTURB_COUNT times, with the audio
    capture seen to run first. The whole guest stands still, so the video has gaps, and a clip with the same gaps in
    its audio is a faithful one: the video's longest gap shows (FREEZE_GAP_MINIMUM_SECONDS), the audio's is within
    GAP_AGREEMENT_MAXIMUM_SECONDS of it, and the audio ends with the video: frames made up for the stood-still time
    and stamped in a burst at the thaw would run it on past the video. Only the audio is held to this, not the frame
    rate."""
    events: list[float] = []
    row, movie = record_video(guest, plan, DISTURB_HOLD_SECONDS, lambda g: disturb_recording(g, "freeze", events),
                              DISTURB_MAXIMUM_SECONDS)
    path = row.get("_data", "")
    print(f"  video with the emulator stopped {DISTURB_COUNT} x {FREEZE_SECONDS:g} s: {row.get('_display_name', path)} "
          f"{movie.seconds:.2f} s, {movie.frames} frames over {movie.video_seconds:.2f} s, longest gap in the video "
          f"{movie.video_gap:.3f} s")
    if not movie.audio or movie.audio_samples == 0:
        raise Failure(f"{path} has no audio track with a sample table")
    print(f"  audio: {movie.audio_samples} AAC frames, longest step of its sample times {movie.audio_gap:.3f} s, "
          f"ends {movie.audio_skew:+.3f} s after the video")
    if movie.video_gap < FREEZE_GAP_MINIMUM_SECONDS:
        raise Failure(f"{path}: the freeze left a gap of only {movie.video_gap:.2f} s in the video (below "
                      f"{FREEZE_GAP_MINIMUM_SECONDS} s): the emulator did not stand still, and nothing can be judged")
    if abs(movie.audio_gap - movie.video_gap) > GAP_AGREEMENT_MAXIMUM_SECONDS:
        raise Failure(f"{path}: the audio's longest step is {movie.audio_gap:.2f} s and the video's "
                      f"{movie.video_gap:.2f} s (they differ by more than {GAP_AGREEMENT_MAXIMUM_SECONDS} s): "
                      "a stood-still guest leaves the same gap in both tracks")
    check_audio_end(path, movie)


def wait_for_capture(guest: Guest, what: str) -> None:
    """The camera service's frame counters rise between two reads: capture is running now, not only once was."""
    seen: list[int] = []

    def producing() -> str | None:
        count = frames_produced(guest.media_camera())
        previous = seen[-1] if seen else None
        seen.append(count)
        return None if previous is not None and count > previous else f"the frame counters stand at {count}"

    wait_for(f"live capture ({what})", SWITCH_OBSERVE_SECONDS, producing, guest)


def read_switch_log(guest: Guest, marker: str) -> str:
    """The log from the line of `marker`. A log that no longer holds the line has lost what followed it, which may
    be an error nobody read: the switches cannot be judged then."""
    after = log_after(guest.shell("logcat -d -b all", timeout=180), marker)
    if after is None:
        raise Failure(f"the log has no line for the {marker}: it was lost or never written, so the photo to video "
                      "switches cannot be judged")
    return after


def refusal_failure(failures: list[tuple[str, str]], where: str) -> Failure:
    """The denial is the cause and the refusal its effect; either may come first in the log."""
    first = {kind: line for kind, line in reversed(failures)}
    kinds = [kind for kind in (SWITCH_DENIAL_KIND, SWITCH_REFUSAL_KIND) if kind in first]
    return Failure(f"{' and '.join(kinds)} {where}: " + " | ".join(first[kind] for kind in kinds))


def restore_animation_scales(guest: Guest, saved: dict[str, str]) -> list[str]:
    """Puts back every scale that was read, each on its own: what could not be put back, as messages."""
    problems = []
    for key, value in saved.items():
        try:
            guest.shell(f"settings delete global {key}" if value in ("", "null")
                        else f"settings put global {key} {value}")
        except (Failure, subprocess.TimeoutExpired) as problem:
            problems.append(f"the animation scale {key} was not restored: {problem}")
    return problems


def check_mode_switch(guest: Guest, plan: Plan) -> None:
    """Photo to video with the preview running, `plan.switch_rounds` times, each from a freshly started
    application and with the window animations off (restored on every exit; a restore that fails fails the
    step unless the step failed already). The video activity covers the photo activity's preview while it
    repeats, which is the case the policy rule for the display composer's fence is for. A round passes only when
    capture was running before the switch, the photo activity's pause and the video activity's resume were seen,
    and capture runs after them, each within a deadline. The log is read from the previous round's marker (the
    first from its own) on every read and once more after the last round, so what is logged between two reads,
    or after the last, is read by the next; a log that lost that marker fails the step."""
    saved: dict[str, str] = {}
    try:
        for key in ANIMATION_SCALES:
            saved[key] = guest.shell(f"settings get global {key}").strip()
        for key in ANIMATION_SCALES:
            guest.shell(f"settings put global {key} 0")
        observe_mode_switches(guest, plan)
    except BaseException:
        for problem in restore_animation_scales(guest, saved):
            print(f"  mode switch: {problem}")
        raise
    problems = restore_animation_scales(guest, saved)
    if problems:
        raise Failure("; ".join(problems))


def observe_mode_switches(guest: Guest, plan: Plan) -> None:
    front = PROFILE_CAMERAS[plan.profile][0] == "front"
    markers: list[str] = []
    for number in range(1, plan.switch_rounds + 1):
        guest.shell(f"am force-stop {CAMERA_PACKAGE}")
        start_camera_application(guest, front=front)
        wait_for_chart(guest, plan.camera_ids[0], "photo")
        time.sleep(SWITCH_HOLD_SECONDS)
        wait_for_capture(guest, f"photo, before switch {number}")
        marker = f"photo to video switch {number} of {plan.switch_rounds}"
        guest.shell(f"log -t {SWITCH_MARKER_TAG} '{marker}'")
        lower = markers[-1] if markers else marker
        markers.append(marker)
        start_camera_application(guest, VIDEO_ACTION, front=front)
        time.sleep(SWITCH_SETTLE_SECONDS)

        def switched() -> str | None:
            after = read_switch_log(guest, lower)
            failures = switch_failures(after)
            if failures:
                raise refusal_failure(failures, f"since the {lower} (read after the {marker})")
            missing = switch_observed(log_after(after, marker) or "")
            return f"the log after its marker shows no {' and no '.join(missing)}" if missing else None

        wait_for(f"no mode switch observed in the {marker}", SWITCH_OBSERVE_SECONDS, switched, guest)
        wait_for_chart(guest, plan.camera_ids[0], "video")
        wait_for_capture(guest, f"video, after the {marker}")
        print(f"  mode switch {number} of {plan.switch_rounds}: photo activity paused, video activity resumed, "
              f"capture before and after; no refused capture request, no {CAMERA_HAL_DOMAIN} denial")
    time.sleep(SWITCH_DRAIN_SECONDS)
    failures = switch_failures(read_switch_log(guest, markers[-1]))
    if failures:
        raise refusal_failure(failures, f"in the final read after the last switch ({markers[-1]})")


def check_front(guest: Guest, plan: Plan) -> None:
    """With a front camera (the profile's camera "1"): the application opens it, the preview is the chart and
    the camera key takes a still."""
    if PROFILE_CAMERAS[plan.profile][0] != "back+front":
        print("  front: the profile's only camera is the front one, checked above")
        return
    before = guest.stills()
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    start_camera_application(guest, front=True)
    wait_for_chart(guest, "1", "front")
    check_still(guest, plan, before)


def check_hal_log(guest: Guest) -> None:
    failures = hal_failures(guest.hal_log())
    if failures:
        raise Failure(f"the camera HAL logged {len(failures)} failed buffer lines; the first: {failures[0]}")
    print("  the camera HAL logged no failed buffer")


def run_checks(guest: Guest, plan: Plan) -> None:
    print(f"layout {plan.layout.name}, profile {plan.profile}, {plan.mode}")
    check_layout(guest, plan)
    check_camera_list(guest, plan)
    if plan.camera_ids:
        check_characteristics(guest, plan)
    print(f"  cameras {list(plan.camera_ids)} and features {sorted(plan.features)}")
    if plan.mode == "video-lost":
        check_video_lost(guest, plan)
        return
    if plan.mode == "video-leave":
        check_video_leave(guest, plan)
        return
    if plan.mode == "none":
        if guest.provider_pids():
            raise Failure(f"a layout without cameras runs the camera provider: {guest.provider_pids()}")
        return
    # The provider was started with cameraserver, which listed the cameras, and holds none: it exits. The wait
    # asks cameraserver nothing (a dump can wake it), so the open below is what starts it again.
    exited = wait_provider_exit(guest, "once cameraserver has listed the cameras and none is open")
    print(f"  provider: gone {exited:.0f} s after the listing and the characteristics (host load {os.getloadavg()[0]:.1f})")
    before = guest.stills()
    check_preview(guest, plan)
    running = guest.provider_pids()
    if not running:
        raise Failure("the camera application holds a camera and no camera provider runs: the open did not start it")
    print(f"  provider: started again by the open, pid {running}")
    check_still(guest, plan, before)
    if plan.mode == "live":
        check_still_keeps_preview(guest, plan)
        check_video(guest, plan)
        check_audio_stall(guest, plan)
        check_video_freeze(guest, plan)
        check_front(guest, plan)
        check_mode_switch(guest, plan)
        check_hal_log(guest)
    check_sensor_history(guest, plan)
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    exited = wait_provider_exit(guest, "after the camera application is stopped")
    print(f"  provider: gone {exited:.0f} s after the application stopped")
    check_camera_list(guest, plan)
    print("  the cameras are still listed with the provider gone")
    check_cameraserver_restart(guest, plan)


def cameraserver_restart_skip(debuggable: str) -> str | None:
    """Why a restart of cameraserver cannot be run on this build, or None when it can: stopping a system
    service needs root, which only a debuggable build gives the shell."""
    if debuggable == "1":
        return None
    return "not run on a user build (stopping cameraserver needs root); the action that restarts the " \
        "provider is in the image, which the target-files verifier checks"


def check_cameraserver_restart(guest: Guest, plan: Plan) -> None:
    """cameraserver starts again with the provider gone: the action restarts the provider and the cameras are
    listed again."""
    skipped = cameraserver_restart_skip(guest.prop("ro.debuggable"))
    if skipped:
        print(f"  skip: cameraserver restarted with the provider gone: {skipped}")
        return
    guest.shell("su 0 stop cameraserver")
    wait_for("cameraserver to be stopped", COMMAND_SECONDS, lambda: None if guest.prop(
        "init.svc.cameraserver") == "stopped" else "it is not stopped", guest)
    exited = wait_provider_exit(guest, "after cameraserver stopped")
    guest.shell("su 0 start cameraserver")
    wait_for("the camera provider to be started by cameraserver's start", PROVIDER_START_SECONDS,
             lambda: None if guest.provider_pids() else "no provider process", guest)
    check_camera_list(guest, plan)
    print(f"  cameraserver restarted {exited:.0f} s after the provider was gone: the provider started again "
          "and the cameras are listed again")


# ---- Starting and stopping the guest.

CAMERA_SOURCES = {"live": "synthetic", "video-lost": "synthetic", "video-leave": "synthetic", "blocked": "blocked",
                  "none": "none"}


AUTO_LAYOUT = "auto"


# The layout the launcher composes for each profile's cameras: every host is given the small tier for now
# (darwin/device/camera/camera_tiers.hpp, largestOfferedTier), so the phone has no 2160p front camera and no
# concurrent stream pair, whatever the launcher's graphics budget.
AUTO_LAYOUTS = {
    "phone": "bf-small-flash-manual-raw",
    "phone-turned-front": "bf-small-flash-manual-raw",
    "tablet": "bf-small-flash-manual-raw",
    "laptop": "front",
}


def wants_hardware_input(options: argparse.Namespace) -> bool:
    """Whether the guest gets the launcher's hardware input (the side keys with the lens cover switch): a blocked
    session always, as the host's application starts it, and any other when the option says so."""
    return bool(options.hardware_input) or options.mode == "blocked"


def auto_layout(profile: str, taught: bool) -> str:
    """The layout the launcher composes for the profile's cameras at the tier every host is given, without
    RAW until a Bayer still has taught the colour data (the first session of a calibration directory)."""
    name = AUTO_LAYOUTS[profile]
    if taught:
        return name
    plain = "-".join(part for part in name.split("-") if part != "raw")
    if plain not in camera_layouts.by_name():
        raise ValueError(f"{plain!r} is not a camera layout")
    return plain


def launcher_arguments(plan: Plan, auto: bool = False, calibration: pathlib.Path | None = None) -> list[str]:
    """What the plan adds to the launcher's command: the camera source and profile, and the layout,
    which the launcher forwards to the kernel as given, as the host's application states the layout it
    composed. With ``auto`` the launcher composes it from the cameras the source presents, through the
    function the iOS runtime uses, and the run checks that Android booted with the plan's."""
    # A guest with no camera device has no profile to give, and the launcher refuses one.
    return [
        f"--camera={CAMERA_SOURCES[plan.mode]}",
        *([] if plan.mode == "none" else [f"--camera-profile={plan.profile}"]),
        "--camera-layout=auto" if auto else f"{camera_layouts.SKU_BOOT_ARGUMENT}{plan.layout.name}",
        # Every host is given the small tier for now; a layout of the large tier is exercised with it.
        *(["--camera-tier=large"] if plan.mode != "none" and plan.layout.tier == "large" else []),
        *([f"--camera-calibration-dir={calibration}"] if calibration else []),
    ]


def start_guest(options: argparse.Namespace, plan: Plan, adb: str, private: pathlib.Path) -> Guest:
    # The side keys (and the VNC display they need) come from the hardware_input option, which main() sets
    # with wants_hardware_input. An importer that builds its own options and does not set it starts the guest
    # offscreen, without the side keys: a blocked session then holds no hardware toggle.
    hardware_input = bool(getattr(options, "hardware_input", False))
    instance = options.instance or private / "instance"
    boot_options = boot_android.parse_arguments([
        *(["--release", str(options.release)] if options.release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(instance),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        *(["--display", "vnc", "--vnc-port", str(getattr(options, "vnc_port", DEFAULT_VNC_PORT))] if hardware_input else
          ["--display", "offscreen"]),
        "--width", str(DISPLAY_WIDTH),
        "--height", str(DISPLAY_HEIGHT),
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = [
        *boot_android.emulator_command(
            boot_options, release, instance / boot_android.android_instance.DATA_DISK, emulator,
            boot_android.data_capacity(boot_options, identity)),
        *launcher_arguments(plan, options.layout == AUTO_LAYOUT, options.calibration_dir),
        *([HARDWARE_INPUT_OPTION] if hardware_input else []),
    ]
    log = private / "emulator.log"
    environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
    hold_directory = None
    if options.audio_hold_library is not None:
        # The library holds the thread of the sound device for as long as a request in this directory says.
        hold_directory = private / "audio-hold"
        hold_directory.mkdir()
        environment["DYLD_INSERT_LIBRARIES"] = str(options.audio_hold_library.resolve())
        environment["UML_AUDIO_HOLD_DIRECTORY"] = str(hold_directory)
    control = edge = None
    if plan.mode in ("video-lost", "video-leave"):
        # The pipe is made by the camera device of the launcher, under this name.
        control = private / "camera-control"
        environment[CONTROL_VARIABLE] = str(control)
    if plan.mode == "video-leave":
        # The launcher makes this one too, and plays the application's leaving the foreground through it.
        edge = private / "edge-control"
        environment[EDGE_VARIABLE] = str(edge)
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True, env=environment)
    serial = f"127.0.0.1:{options.adb_port}"
    # The emulator is this function's until it returns the guest: the caller's cleanup knows a guest only
    # from that return, so a failure or an interrupt (a timeout, Ctrl-C) before it stops the emulator here.
    try:
        guest = Guest(adb, serial, process, log, private, hold_directory, control, hardware_input=hardware_input,
                      edge=edge)
        boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
        if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
            raise Failure("Android did not reach sys.boot_completed")
        boot_android.android_instance.booted(instance, identity)
        if not options.instance:
            boot_android.provision_guest(adb, serial)
        print(f"booted; settling for {SETTLE_SECONDS} s")
        time.sleep(SETTLE_SECONDS)
    except BaseException:
        boot_android.disconnect_adb(adb, serial)
        boot_android.stop_guest(process)
        raise
    return guest


def restart_guest(options: argparse.Namespace, plan: Plan, adb: str, private: pathlib.Path,
                  guest: Guest) -> tuple[Guest, Plan]:
    """Stops the guest and boots the same instance again, the way the host's application restarts a guest: over the
    calibration directory the first session kept, with the launcher composing the layout from the cameras (the
    taught one). Returns the new guest and its plan."""
    boot_android.disconnect_adb(adb, guest.serial)
    boot_android.stop_guest(guest.process)
    again = make_plan(auto_layout(plan.profile, True), plan.profile, "live", plan.switch_rounds)
    second = private / "restart"
    second.mkdir()
    restarted = argparse.Namespace(**{**vars(options), "instance": options.instance or private / "instance",
                                      "layout": AUTO_LAYOUT})
    return start_guest(restarted, again, adb, second), again


def positive_integer(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a whole number") from None
    if value < 1:
        raise argparse.ArgumentTypeError(f"{text!r} is not positive")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--instance", type=pathlib.Path,
                        help="an existing instance to boot; the default is a fresh one that is removed at the end")
    parser.add_argument("--camera-profile", choices=sorted(PROFILE_CAMERAS), default="phone",
                        help="the cameras the synthetic source presents")
    parser.add_argument("--layout", help="the camera layout to state on the vendor SKU "
                        "(default: the profile's, or none for --mode none); `auto` has the launcher compose it "
                        "from the cameras, as the iOS runtime does, and expects the profile's")
    parser.add_argument("--calibration-dir", type=pathlib.Path,
                        help="where the colour data a Bayer still teaches is kept across runs; with --layout auto a "
                        "directory that holds none yet expects the layout without RAW, one that holds some the "
                        "profile's own")
    parser.add_argument("--mode", choices=MODES, default="live",
                        help="live: the synthetic chart; blocked: a session started with Camera off; "
                        "restart: a preview and a back still, then a second boot over the colour data it taught; "
                        "none: no camera device")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--hardware-input", action="store_true",
                        help="start the guest on a loopback VNC display with the launcher's --hardware-input, as "
                        "the host's application does, so that it has the side-key device (the camera lens cover "
                        "switch); a blocked session always does, a live one by this option")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        help="where the captured session log (and its pid file) is kept; without it the log is in the "
                        "run's temporary directory and goes with it")
    parser.add_argument("--vnc-port", type=int, default=slot_ports.slot_port(VNC_OFFSET, DEFAULT_VNC_PORT),
                        help="loopback port of the guest's VNC display with --hardware-input (default: the guest "
                        f"slot's second port when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_VNC_PORT})")
    parser.add_argument("--switch-rounds", type=positive_integer, default=SWITCH_ROUNDS,
                        help="how many photo to video switches the live mode makes (each can miss a race that "
                        f"an image without the policy rule loses only some of the time; default {SWITCH_ROUNDS})")
    parser.add_argument("--audio-hold-library", type=pathlib.Path,
                        help="the test library (uml_audio_hold) that holds the thread of the Virtio sound device; "
                        "without it the audio stall step does not run")
    parser.add_argument("--leave-trials", type=positive_integer, default=LEAVE_TRIALS,
                        help=f"how many recordings the video-leave mode leaves (default {LEAVE_TRIALS})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


# The tags of the camera path and of the buffers it allocates; the application's own noise (the surface
# composer's per-frame lines, adb's own polling of the camera folder) is left out.
FAILURE_LOG_PATTERN = ("V4l2Stream|V4l2FrameSource|EmulatedSensor|EmulatedRequest|CameraService|Camera2Client|Camera3|"
                       "cameraserver|camera.provider|CameraProvider|CameraX|Aperture|vrend|gralloc|minigbm|virtio|BufferQueue|"
                       "EmulatedCameraDeviceInfo|V4l2Characteristics")
FAILURE_LOG_NOISE = ("SurfaceFlinger|adbd|out of bounds transparent region|minigbm:virtgpu_init|minigbm:virgl_get_caps|"
                     "minigbm:virgl_add_combination|vendor.minigbm.debug")
FAILURE_LOG_LINES = 120

def print_camera_log(guest: Guest) -> None:
    """At a failure, the guest's own log lines about the camera path and its buffers, then what was in front and
    the camera application's own events (nothing is asserted on them): they tell a failure of the image from
    one of the runner."""
    try:
        text = guest.shell(f"logcat -d -b all | grep -a -E '{FAILURE_LOG_PATTERN}' | grep -a -v -E '{FAILURE_LOG_NOISE}' "
                           f"| tail -n {FAILURE_LOG_LINES}", timeout=120)
    except Exception as problem:  # the guest may be gone; the failure being reported is the one that matters
        print(f"(no camera log lines: {problem})", file=sys.stderr)
        return
    print("camera log lines of the guest:\n" + text.rstrip(), file=sys.stderr)
    try:
        foreground = guest.shell(FOREGROUND_COMMAND, timeout=60)
        print("in front at the failure:\n" + foreground.rstrip(), file=sys.stderr)
    except Exception as problem:
        print(f"(no foreground state: {problem})", file=sys.stderr)
    print_application_events(guest, sys.stderr)


def print_sensor_capture(guest: Guest) -> None:
    """At a failure, where the session's captured log is and what it says about the sensor privacy service."""
    capture = guest.capture
    if capture is None:
        return
    try:
        lines = [line for line in capture.path.read_text(errors="replace").splitlines()
                 if re.search(SENSOR_CAPTURE_PATTERN, line)]
        pid = capture.pid_path.read_text().strip()
    except OSError as problem:
        print(f"(no captured session log: {problem})", file=sys.stderr)
        return
    print(f"sensor privacy lines of the captured session ({capture.path}, capture pid {pid}):\n"
          + ("\n".join(lines[-SENSOR_CAPTURE_LINES:]) or "(none)"), file=sys.stderr)


def print_application_events(guest: Guest, stream) -> None:
    """The camera application's own log lines: its crashes, ANRs, process deaths and the windows (dialogs) that
    came in front of it. A guest that is gone is said, not raised."""
    try:
        events = guest.shell(f"logcat -d -b all | grep -a -E '{APP_EVENTS_PATTERN}' | grep -a -v -E '{FAILURE_LOG_NOISE}' "
                             f"| tail -n {APP_EVENTS_LINES}", timeout=120)
        print("events of the camera application and its windows:\n" + events.rstrip(), file=stream)
        own = guest.shell(f"for pid in $(pidof {CAMERA_PACKAGE}); do logcat -d -b all --pid=$pid; done "
                          f"| grep -a -v -E '{FAILURE_LOG_NOISE}' | tail -n {APP_OWN_LOG_LINES}", timeout=120)
        print("the camera application's own log lines (by process id):\n" + (own.rstrip() or "(none: no process)"),
              file=stream)
    except Exception as problem:
        print(f"(no application events: {problem})", file=stream)


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    if options.mode == "restart" and options.hardware_input:
        print("the restart mode runs without the side keys; --hardware-input is for the live and blocked modes",
              file=sys.stderr)
        return 2
    # A session started with Camera off has the side keys, whose device reports the camera lens cover, as the
    # host's application starts it; without them nothing of the camera switch would reach Android.
    options.hardware_input = wants_hardware_input(options)
    if options.mode == "restart" and AUTO_LAYOUTS[options.camera_profile] == auto_layout(options.camera_profile, False):
        print(f"the {options.camera_profile} profile has no lens with colour data to restart over", file=sys.stderr)
        return 2
    default_layout = AUTO_LAYOUT if options.mode == "restart" else DEFAULT_LAYOUTS[options.camera_profile]
    layout = options.layout or ("none" if options.mode == "none" else default_layout)
    if layout == AUTO_LAYOUT:
        if options.mode == "none":
            print("--layout auto needs a camera to compose it from", file=sys.stderr)
            return 2
        taught = bool(options.calibration_dir and options.calibration_dir.is_dir()
                      and any(options.calibration_dir.iterdir()))
        try:
            layout = auto_layout(options.camera_profile, taught)
        except ValueError as problem:
            print(f"{problem}", file=sys.stderr)
            return 2
    try:
        plan = make_plan(layout, options.camera_profile, "live" if options.mode == "restart" else options.mode,
                         options.switch_rounds, options.leave_trials)
    except ValueError as problem:
        print(f"{problem}", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-camera-"))
    guest: Guest | None = None
    try:
        if options.mode == "restart" and options.calibration_dir is None:
            options.calibration_dir = private / "calibration"
            options.calibration_dir.mkdir()
        guest = start_guest(options, plan, adb, private)
        guest.output_directory = options.output_dir
        if guest.hardware_input:
            # The sensor privacy checks judge the whole session: capture its logs from before the camera opens.
            output = options.output_dir or private
            output.mkdir(parents=True, exist_ok=True)
            guest.capture = SessionCapture(adb, guest.serial, output / f"camera-session-{plan.mode}.log")
            guest.capture.start()
        if options.mode == "restart":
            before = run_restart_checks(guest, plan, options.calibration_dir)
            guest, again = restart_guest(options, plan, adb, private, guest)
            check_restart_keeps_cameras(guest, again, before)
            check_raw_still(guest, again)
        else:
            run_checks(guest, plan)
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        if guest is not None and not isinstance(problem, boot_android.LauncherError):
            print_camera_log(guest)
            print_sensor_capture(guest)
        log = guest.log if guest is not None else private / "emulator.log"
        if log.is_file():
            print("launcher log tail:\n" + "\n".join(log.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            if guest.capture is not None:
                guest.capture.stop()
            boot_android.disconnect_adb(adb, guest.serial)
            boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
