# Virtio camera

`UML::CameraDevice` serves the host's cameras to a guest through the camera
device of the Virtio video wire ([`kernel/common/video`](../../../kernel/common/video/README.md),
device ID `cameraDeviceId`). It reuses the common vhost-user transport and the
video device's guest-buffer writer (`UML::VideoGuestBuffers`), and runs on a
platform camera (`darwin/platform/camera.hpp`) that the caller hands it: the
AVFoundation camera on a real host, the synthetic camera in a test or a
launcher's synthetic mode. The device itself links no capture backend.

`makeInProcessCameraService()` starts it as one transport thread. A caller
chooses a `CameraMode` and a `CameraTier` when it starts a guest and passes the
application's statements (the sensor orientation, the rotation that
`ROTATE_AND_CROP` automatic applies, a state callback) as `CameraDeviceOptions`.

## Contract

- **Modes.** Live opens a platform capture session when a guest stream first
  needs it (a queued video buffer, a still, a torch) and closes it when none
  does. Blocked never calls `Camera::openSession`, so the host's capture
  indicator cannot light: the device publishes the cameras that discovery found,
  states `VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED`, and completes every video
  buffer black and flagged blocked on a timer of its own and every still with an
  error. It tells the application's state callback `blockedInUse` when a guest
  app has a camera open (a video stream runs or a still is asked for; a
  metadata stream alone is not use) and `idle` once the guest has closed every
  camera, so each open is reported once and a live session never reports it. A
  host that has not granted access behaves as an interrupted live session.
- **Cameras.** Wire order is the primary back lens (the wide, else the ultra
  wide, else the telephoto), the front camera, the other back lenses (wide,
  ultra wide, telephoto), then external cameras, at most eight. The device
  publishes no zoom camera (a virtual device that switches lenses): every back
  lens is a camera of its own, so a guest switches lenses by switching
  cameras, and each keeps its own formats, controls and Bayer stills. The
  Android image numbers a phone's cameras 0 wide, 1 front, 3 ultra wide and 4
  telephoto. Cameras run together where the platform's multi-camera sets say
  they can, two at once (the device runs no more, which is the layout's
  `CameraUse::streams`): the guest is told the primary back camera with the
  front camera first, then the front camera with each other camera a set
  names, then other pairs, and every other pair conflicts, so the guest never
  takes a group of more than two as one it may run at once. The log names the
  pairs and the platform's sets at start.
  A lens publishes manual exposure, focus and white balance where the host
  allows them. Of the front lenses (an iPad lists a wide and an ultra wide one
  over a single sensor) the guest sees the wide one.
- **Zoom.** Every camera's ratio range starts at 1.0, its own full field of view.
  A camera that runs in an anchor ends at the zoom the system recommends for the
  anchor's format, and where the system gives none at what the anchor's format
  itself delivers, never above the device's largest.
- **Sensor size and focal length.** A camera publishes them as a pair that
  gives its field of view, which the guest takes its lens labels and 35 mm
  equivalents from. A lens that has learned its colour data publishes the
  physical focal length of its Bayer still, and the size then follows the field
  of view in the pixel array's shape.
- **Tiers.** The small tier limits a session to 1080p video, 12 MP stills and
  four buffers; the large tier is 4K, 48 MP and six, for a fixed GPU budget of
  1 GiB or more. Every host is given the small tier for now
  (`largestOfferedTier`), so no layout of the large tier or with a 2160p front
  camera is composed; the image keeps them, and the launcher's `--camera-tier=`
  exercises them. A front camera's stills are limited to about 3 MP at either
  tier (`frontStillMaximumPixels`: its still session's memory grows with the
  photo's pixels), with no exception: a front camera with no size within it is
  offered no native JPEG, and Android derives its JPEG sizes from the video. The
  back cameras' limit is the tier's.
- **Memory.** A live session holds host memory outside the guest's RAM. The
  device holds the host to what it counts (`camera_tiers.hpp`, `CameraUse`,
  given in `CameraDeviceOptions::use`), and the application takes the matching
  reserve from the GPU's pool while the cameras are live:
  - **Cameras.** At most `use.streams` cameras hold a capture at once; a start
    or a still beyond it is refused as the platform refuses a conflicting
    camera, whatever the guest was told. A camera holds its capture until the
    platform has stopped it, whatever its streams ask: a change of a running
    video that the platform refuses leaves the old capture running and
    counted, and the stream's end stops it.
  - **Frames.** The capture pools, the device's staging and the transformer's
    scratch are bounded for each camera that runs. A camera whose video stops
    gives back the frames the worker has not taken, and a platform callback
    that is still in progress finds it stopped and keeps none, although the
    session stays open for the torch or a still.
  - **Stills.** The captures the device has at once cost no more than
    `stillAdmissionBytes`. A JPEG capture's file is bounded by the guest's JPEG
    buffer for the size asked (`maximumJpegBytes`; a larger file fails the
    capture before it is copied, and the pre-copy check, the device and the
    count use one bound); a RAW capture and the working storage of the
    platform camera's source (`Camera::stillWorkingBytesPerPixel`) are counted,
    and for a camera whose pictures the host presents half around its encoder's
    buffers besides (`Camera::turnedStillWorkingBytesPerPixel`).
    RAW is counted wherever a lens the guest sees can make a RAW still in the
    run. A capture that does not fit waits, and none starts while an older one
    waits. A capture that costs more than the whole budget fails before it
    starts. A capture the guest gave up on keeps its charge until the platform
    ends it (or the session is torn down: one the platform never ends is taken
    as lost after a bounded time), and a result's payload is freed before its
    charge is spent again.
  - Black pictures are written from a chunk of the device's own, so no stream
    or picture size holds one: a blocked session holds none of the memory
    above and opens no capture session.

  The runtime and the launcher compose the use from the cameras the guest is
  told (`composeCameras`). The first discovery's use is the run's reservation,
  and a later boot composes under all of it (`composeCamerasWithin`): the layout
  the guest is told is one the reservation covers (cameras at once, RAW capture,
  source working storage), and a run that began without cameras stays without.
  A reserve that takes all of B refuses the launch. The runtime answers the
  application the same figure before the boot (`plan_camera_reserve`): it
  discovers the cameras, which asks for no permission and opens no session, and
  composes the use a boot would reserve for, while the source, the cameras and
  the calibration data are unchanged; a request to discover again refreshes the
  discovered source and the composed answers together. The loader's ceiling
  bounds the system cameras and stands only where no runtime can say. The
  system camera's own
  processing storage while it makes a picture is counted as working bytes for
  each pixel of a JPEG still (`systemCameraStillWorkingBytesPerPixel`), a figure
  measured on one device with headroom, and the ceiling holds it. The encoder's
  buffers while the host turns a front still half around are counted as the
  same kind of figure (`systemCameraTurnedStillWorkingBytesPerPixel`), added to
  the capture of a turned camera only; the largest such capture is inside the
  ceiling. The platform camera's peak is not otherwise measured.
- **Frames.** Video buffers hold NV12, BT.709 full range, rows at the 16-byte
  pitch of `raw_pitch.hpp`. The device crops (centred, to the stream's aspect),
  rotates and scales with the platform camera's transformer
  (`Camera::makeFrameTransformer()`; the Apple camera's runs on the system's
  scaler, see [`darwin/platform`](../../platform/README.md)), writes a picture
  that needs none of it straight into the guest's rows, and drops frames to a
  stream's rate. A frame with no queued buffer is dropped and its number spent.
  A camera whose pictures the host presents half around
  (`CameraInfo::presentationTurn` 180, a built-in front camera) has that turn
  added to ROTATE_AND_CROP's in the same write: a frame of the stream's own size
  is copied with its rows reversed, and a scaled one takes the combined rotation
  in the transformer. A still of such a camera is turned by the platform before
  it reaches the device.
- **Stabilization.** The guest's request for it (Android's record template
  makes it, so video mode asks and photo mode does not) is met with the host's
  low-latency mode where the active format supports it, else the standard one,
  else none, and never with the system's automatic choice, which on an iPhone
  holds the viewfinder a second or more behind (the preview and the recording
  share one output). The device applies the camera's settings before it starts
  its video, and the platform programs the mode on the new video connection
  before the capture can deliver, so the first frame is cropped as the rest are
  and the view does not move when the guest first changes a setting. A format
  that supports no mode leaves the picture unstabilized, and the session
  record's video line for the camera says `stab unsupported`.
- **Stills.** A JPEG buffer carries the platform's file behind a reserved Exif
  APP1 segment for the guest to fill in place (`jpeg_gap.hpp`); a RAW buffer
  carries 16-bit rows at the 16-byte pitch. Buffers of one non-zero cookie
  queued with `GROUP_MORE` are filled from one capture, which starts with the
  buffer that lacks the flag. A group not complete in two seconds fails; a
  capture has ten seconds to deliver from the request, whether it waits or has
  started. At most as many captures as a tier has buffers are with the platform
  for one camera; one more fails its buffers; a capture that the bytes it costs
  do not leave room for waits (Memory). A still in a blocked or interrupted
  session, or one the platform refuses, completes with the error flag (and the interrupted flag when the
  session failed or was interrupted), and so does one that completes while its
  camera is interrupted or that was asked for before an interruption or
  recovery of its camera that has since been handled. The JPEG ends at the first picture's end
  of image, without a multi-picture index or any picture behind it. A JPEG is
  the size of its stream or its buffer completes with the error flag: the frame
  header the device reads is compared with the configured size, and the
  platform is asked for exactly that size. A host whose photo sizes belong to
  its formats (`CameraInfo::captureFormats`) runs each camera in one of them,
  its **anchor**, and offers only what the anchor makes: its stills up to the
  tier's cap (a front camera's up to `frontStillMaximumPixels`, from the
  formats `limitedStillFormats` returns, which the platform's photo output
  reads too, and in the flat list of a host without formats; whether a camera
  keeps its concurrency is decided on the formats as listed), the video sizes
  inside it whose capture buffers fit the reserve (one byte count,
  `alignedFrameBytes` in `platform/frame_bytes.hpp`, for the offer's bound and
  the reserve), and its whole frame rates up to 30 (`videoRateCap`: photos win
  over fast streams).
  The anchor is chosen once when the cameras are arranged, by the key in
  `photo_dimensions.hpp`: a whole rate within 30, then the largest still, then
  the largest video box, then the smaller format, the lower minimum rate, a
  full-range format, the lower index. Every offered still is made and every
  offered stream starts, whatever else the camera is doing. A camera that could
  run with others keeps that only when the formats a multi-camera session takes
  offer as much as its best format on those first three; otherwise it keeps its
  whole offer, is published as conflicting with the others, and the log says so
  once. A host that lists no formats has no anchors and offers what it states.
  A camera opened while another's still is in flight, which the two conflict
  over, is refused until that still is delivered or has run out its ten seconds.
  A still that does not end in a picture is reported to the device's owner
  (`CameraDeviceOptions::onStillFailure`) with the stage it stopped at, the
  camera, the sizes asked for, resolved and encoded, and what the platform
  said of the capture (`darwin/platform/camera_still_failure.hpp`); the runtime keeps
  the last few in the session record's camera slot. A whole request that waited
  for the host's budget until its time ran out is one of them. One that an
  interruption or a blocked session ends, and one that delivered its JPEG
  (whatever its RAW did), report nothing. How the platform presented each
  camera (its mounting, what its connections and photo output were set to, its
  aspect ratio: `darwin/platform/camera_presentation.hpp`) is reported under the
  guest's number of the camera (`CameraDeviceOptions::onCameraPresentation`) and
  kept in the same slot, with the orientation of the guest's own description
  (the application may turn it for a tablet's boot layout) and the platform's
  as a second number where they differ. A still the platform refuses carries the
  photo output's last compensation reading and when it was taken. The platform
  session opens at the first use of a camera, so a run that used none names
  none.
- **Metadata.** A meta stream completes one `virtio_video_camera_meta` per
  exposure, with the sequence the frames and stills carry; black frames get a
  record too. The exposure time, ISO, aperture, focal length, neutral point and
  noise profile use zero for "the photo gave none" (none of them can be zero),
  and the 3A and flash states have no such value. A still's record carries the
  photo's own exposure time, ISO, aperture and focal length; the device states
  the lens's focal length when the photo gave none.
- **Black and interruptions.** Blocked, interrupted, restarting and test-pattern
  frames are black (`fillNv12Black`) and flagged; they are timed by the device,
  so a stalled platform camera does not stall the guest. A stream's frames are
  black and flagged RESUMING from the first queued buffer until the platform
  delivers its first frame, at a first start and after a resume alike, and
  INTERRUPTED while the camera cannot run (an interruption, system pressure, a
  capture that failed to start, retried once a second). An interruption belongs
  to the platform session it happened in: when the session closes, or a new one
  opens, it is over, and the guest is told it ended. The guest is told each way
  once (`CAMERA_INTERRUPTED` or `CAMERA_UNAVAILABLE`, then `CAMERA_RESUMED` or
  `CAMERA_AVAILABLE`): a second interruption says nothing new, a session that
  fails marks the cameras that have a stream, and a capture that failed is told
  to be back when it delivers a frame, so a start that fails round after round
  tells the guest once. Everything the platform hands over carries the serial
  of the last platform event the inbox held when it arrived, and a camera
  remembers the serial of the last interruption, recovery or failure handled
  for it: a frame of a lower serial was captured before that boundary (it waited
  in the inbox or in the pump's batch while the event was handled) and is
  dropped, never delivered as a live picture after the repair.
- **Lost video.** The platform reports that the application left the
  foreground (`CameraEventKind::interruptedInBackground`, which the Apple
  backend raises for the system's reason that a camera is not available to an
  application in the background). If the guest has a video stream open on a
  camera then (even one a suspend has stopped; nothing else decides it, as the
  host cannot tell a recording from a viewfinder), the camera's video is lost:
  the capture stops, and every buffer of every video stream of the camera
  completes with the error and interrupted flags at the stream's rate, with a
  black interrupted record, whatever the platform does afterwards and for a
  stream the guest restarts after a suspend, until the guest has closed its
  last video stream of the camera. A stream it opens next is an ordinary one, so
  the camera opens again on demand. What the guest's client sees of it is for
  the guest's camera HAL to decide (the wire's README has the rule). Every other
  interruption (another application or a call, iPad multitasking, the system's
  pressure, a restart after an error), and a camera with no video stream, only
  get the dark gap above. A platform session opened while the application is
  away never ran in the foreground, so it reports an ordinary interruption: the
  guest's camera layer, which reopens its streams after the loss, gets one held
  session whose frames are dark and interrupted and whose stills fail until the
  application is back, not a loss for each reopening. A blocked session hears of
  none. The report line names the camera and says the video ended.
  The interrupted record goes to the guest first: the device queues it on the
  meta stream when the loss begins, and the video buffers fail only once the guest
  has queued the meta buffer that carried it again (100 ms at most, then the
  buffers fail and one line says so), so the first failure a guest meets is the
  loss and not an ordinary failed buffer.
- **The application's departure.** The application holds the guest's GPU requests
  as it enters the background, and a guest whose camera layer has not acted on
  the lost video by then stalls in its next GPU request until the application is
  back: a recording runs on, in silence, until the return. A
  `BackgroundVideo` (`CameraDeviceOptions::background`, one for the process, safe
  from any thread) lets the application end the guest's video first:
  `end(deadline)` has the device handle every running video as the platform's
  notice does, and waits until the guest has stopped the video streams of the
  cameras it ended (and of those whose video ended a moment ago and is still being
  acted on), or until the deadline, which covers the device's answer as well.
  With no video running nothing is waited for; with no device attached nothing is
  ended. `holdAfterVideoEnds` is the application's sequence (end, wait at most
  `bound`, which a caller with a deadline sets to what the deadline leaves after
  the hold's own budget and the application's `backgroundVideoBound` caps, then
  hold whatever the wait found, and say once on a wait that ran out); the hold's
  own budget and this bound together stay inside the application's edge budget
  (three seconds from the edge to the held gate at the latest with no deadline;
  a deadline that leaves less than the hold's own budget leaves the wait nothing,
  and the hold keeps its budget).
  A caller that holds without the wait, as the application's bound does when the
  edge is late, ends a wait that is pending with `abandon()`: the call returns
  at once with `abandoned` set and says nothing, since the held gate makes the
  wait of no use. The calls serialize under their deadlines: a call still queued
  behind another at its own returns unanswered.
- **Video mode.** The device tells its owner (`onVideoMode`) whether the guest
  is in video mode: a live camera's video runs, uninterrupted, and the guest
  asks for the picture stabilized, which Android's record template does and its
  preview template does not (the wire carries no other sign of the template).
  The owner opens the microphone for it before the guest records
  ([`darwin/device/audio`](../audio/README.md#microphone)). The device says it
  at every change of the state, however it came: a stream, a start tried again
  after a failure, a rate change that starts the video, an interruption and its
  end. It is told false when the video stops, the camera is interrupted, the
  session is blocked and when the device is reset or goes away.
- **Events and reset.** The event queue never holds a buffer: with no event
  pending it returns retry, so the guest's buffer stays in its ring, and an
  event raised later signals the device's event descriptor so the server runs
  the queue again. The device keeps the requests it deferred while its ring is
  disabled (`keepsDeferredRequestsWhileDisabled()`), as the codecs do, so a
  guest suspend neither resets it nor drops a held buffer or a late answer:
  they are completed once the ring is enabled again (`camera-held-requests`
  drives the server through a disable with a stream's buffer held). What still
  resets the device is a new memory table, a layout message (features, ring
  size, base or address) that finds a request held on a disabled ring, a
  guest's release of the device (`RESET_OWNER`, or `GET_VRING_BASE` with a
  request pending) and the end of the connection; the reset drops every stream,
  group, pending event and control value, and the driver creates them again
  (see the wire's README).
- **Calibration.** RAW is offered only for a lens whose colour data is known.
  The first still of an uncalibrated lens at unit zoom also takes a Bayer still
  and keeps what its DNG reveals in the `CalibrationStore` (per host model and
  lens), with the size of the anchor it was learned in; the next guest sees RAW,
  and a lens learned in another format is learned again. A guest keeps seeing what it saw at start. The
  Bayer part is a courtesy: when the platform refuses it or it fails, the JPEG
  of the same capture is delivered. A host that cannot take a Bayer still
  therefore never learns a lens, and publishes no RAW anywhere: no RAW flag,
  Bayer order, size or levels, no RAW16 in the stills capability, and a layout
  without `-raw`, so the guest has no RAW capability and no RAW16
  configuration (`camera-device`, `andarium-camera-plan`).
- **Controls** belong to the camera, whichever stream carries the command, and a
  set applies every value or none. The table is `camera_controls.cpp`; the
  platform sees one `CameraSettings` per camera.

## Launcher

The macOS launcher starts the device with its `--camera=` option, as the last of
the Virtio devices it creates so that no other device's index moves; its
argument handling in `darwin/app/macos/main.cpp` is the reference for the
options and their values. It reaches the platform cameras through
`UML::CameraSource` (`camera_source.hpp`): the host's camera, the synthetic
camera and a read of the camera permission.
The `host` source needs the system's camera permission granted to the program
(or the terminal it runs in) beforehand: nothing in the launcher or the device
asks for it, and the launcher refuses to start, saying how to grant it, when it
is not. The tier follows the launch's GPU budget within the cap, and a state
line goes to stderr when the host starts or stops capturing, a line for each
change of the ROTATE_AND_CROP setting a camera's frames get, and the process's
footprint when a capture session opens, closes and after each still.

## Layout

An Android image carries a closed set of camera layouts and the host names the
one that describes its cameras as `androidboot.product.vendor.sku`
(`camera_layout.hpp`; `android/lineage-24/tools/camera_layouts.py` is the only
source of the set). `composeLayout()` composes that name from a platform
camera's discovery, as the device will present the cameras
(`arrangeCameras`), and falls back to the nearest layout an image has. It asks
for no permission and opens no session, so the layout is the same whether the
guest's cameras are live or blocked. The macOS launcher composes it with
`--camera-layout=auto`, as the iOS runtime does; a launch otherwise passes the
layout itself. A release's own command line may not carry the argument. A
blocked guest also reports Android's camera lens cover on the side-key input
device (`SW_CAMERA_LENS_COVER`) when its layout has cameras, and a live guest
does not advertise the switch (`lensCoverAdvertised()`, which the launcher and
the iOS runtime both ask). A guest whose layout is `none` gets no device. A
runtime composes the layout again at each boot, so RAW that a Bayer still
earned reaches Android's features at the next boot of the same run.

## Orientation

Where the host owns the guest's rotation (an iPad window) the guest's display
stays upright at rotation 0 while the device body, which carries every built-in
sensor, turns under it. The application states the body's clockwise turn from
portrait (`turn`: 0, 90, 180 or 270; the interface orientation's landscapeLeft
is 90) and a boot states each camera's SENSOR_ORIENTATION from it
(`camera_orientation.hpp`): a back camera's base orientation plus `turn`, a
front camera's minus `turn`, an external camera's unchanged. The base is the
platform camera's own `sensorOrientation`, and with `turn` 0, where the guest
rotates itself, it stands. A built-in front camera's is 270 whatever its
mounting: the host turns its pictures half around where the sensor is mounted
at 90 (`CameraInfo::presentationTurn`), and the guest's metering and focus
regions are mapped back through that turn (`CameraControls`). A turn after the boot reaches the guest only as a
turn of the picture by the difference of the two orientations, which
ROTATE_AND_CROP's automatic setting applies
(`CameraDeviceOptions::automaticRotation`, per camera).

## Tests

`camera-controls`, `camera-description`, `camera-jpeg-gap` and
`camera-calibration` check the pure parts (`camera-controls` the regions mapped
back through a half turn), `camera-apple-turned-still` a photo turned and
encoded in the hardware JPEG encoder, `camera-orientation` the sensor
orientation and picture turn of every interface orientation and facing,
`camera-layout` the layout names, their fallbacks and the composition (against
the image's table), and
`camera-source` the launcher's factories (nothing opened, no prompt).
`camera-device` drives the device
over both queues in front of the synthetic camera, including the invariant that
a blocked session never opens a platform session and reports each open of a
camera once, a hostile guest, a reset with
streams and stills in flight, an event queue that retries and wakes the server,
a ring disable that resets nothing, a still on one camera while another
streams, a join over the hardware budget, a capture that fails to start
round after round, and a video that the application's leaving the foreground
ends, with the interruptions and the modes that it does not, and a camera presented
half around (the synthetic camera's `phone-turned-front` profile) written turned.
`camera-held-requests` runs the vhost-user server over the
device and holds a stream's buffer across a disable and an enable. The Apple
backend's capture is not run by any test.
