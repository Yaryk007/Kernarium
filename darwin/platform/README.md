# Platform video encoder

`video_encoder.hpp` is the platform's hardware video encoder interface; nothing
in it names VideoToolbox, V4L2 or the Virtio wire. The Apple backend is
`apple/video_encoder.mm` (sessions), `apple/video_encoder_discovery.mm`
(capability discovery and its cache) and `apple/video_encoder_bitstream.hpp`
and `apple/video_encoder_levels.hpp` (parameter-set readers and the level
policy). It builds as `UML::VideoEncoder` for macOS, iOS device and iOS
Simulator, and only through public VideoToolbox API. The [Virtio encoder
device](../device/video/README.md#encoder-device-id-30) maps this interface to
the wire and depends on nothing below it. The decoder counterpart is
`video_decoder.hpp` (`UML::VideoDecoder`).

## Discovery

`videoEncodeCapabilities()` returns what an encode on this host proved: a
profile, input layout, size, rate-control mode or other capability is listed only
after a stream that used it came out right. VideoToolbox lists no ranges, so
discovery drives the production encoder, and `prepareVideoEncodeCapabilities()`
runs it on its own thread; the first query waits for it, and a host that
answers nothing reports no encoder. A capability never lists a picture side
below `kVideoEncodeMinimumSide`, and profiles a guest API cannot name (ProRes,
HEVC range extensions, alpha, depth) are not probed.

A probe that does not prove what it asked for is a refusal of the host or no
answer at all (a session the busy hardware could not give or lost, a drain that
ran out of time, a call that failed). Only a refusal is a fact. A probe with no
answer is asked again three times after a short pause; only then, where a
refusal and a busy encoder look alike (a size beyond the ceiling and an encoder
with no session both fail to create one), is it a refusal, when the first probe
that proved in that codec proves again right after the retries. A probe that is
still unanswered makes the discovery **incomplete**: its result lists only what
was proved, and a codec whose probes keep going unanswered is given up. A probe
refused before the codec had a control is asked again once it has one. A failed
call that lists the encoders is no answer; a list that names none is. `bgra` and
`rgba` are one probe, because they are one session and pool with the red and
blue bytes exchanged on submit, so a profile lists both or neither. A session
the host lost, or could not give hardware to, at any property is not a refusal of
that property, optional or not.

The result is cached as JSON keyed by the host model, OS build and a format
revision. `UML_VIDEO_ENCODE_CACHE` names the file and an empty value turns the
cache off; a missing, foreign or unreadable file is ignored and replaced. An
incomplete result is never cached. One discovery runs at a time, whether it is
the first or a later one. An incomplete result serves the query that found it,
and a later query, at least 30 seconds after the last run and at most three runs
in the process, computes discovery again. The answer of a process never shrinks:
a complete result replaces a codec's entry only when it contains that entry,
and a codec the answer lacks is added, so what a guest was offered is still
accepted and each entry is one run's own description; the cache holds the
complete run as measured. A discovery whose thread cannot be created leaves the
answer as it is and is tried again by a later query, within the same bound. A
running guest's device took its list when it was created and keeps it: a better
answer changes what the encoder accepts, not what the guest was offered. A
cache hit is published at once with the concurrent session counts its file
carries, and nothing is opened on the path of a query. What other processes held
is no property of the host, so a count below the 16 the discovery looks for is
measured once per process in the background: a higher one raises the published
count and the file's, and nothing lowers either.

## Sessions

Hardware is required. The caller writes each frame once, into a pixel buffer
from the session's own pool (`acquireFrame()`, null when the pool is in flight),
and the encoder takes it without a second copy. Every submitted frame is
answered exactly once.

- **Output** is Annex B in decode order, with parameter sets placed as the
  settings ask. Frame type is read from the sample.
- **B-frames** are coded in one pattern no property shortens: a `bFrames`
  request at or above the discovered run codes the pattern, below it none.
- **Levels.** A stream states at least the level it was asked for and at least
  the lowest level its picture size, frame rate and bit rate fit, and the
  encoder lowers VideoToolbox's choice only to a level the stream provably
  fits (`apple/video_encoder_levels.hpp`). The level is that of the settings a
  session is made from; a rate or frame-rate update does not restate it, so a
  caller that will raise the rate asks for the level it needs.
- **Input and colour.** A layout is listed for a profile only when an encode
  from it came out right. `rgba` is `bgra` with its red and blue bytes
  exchanged on submit, and a full-range request from either is converted by the
  platform because VideoToolbox converts RGB to video range only. The colour
  description (`VideoEncodeColor`) is attached to the buffers and states itself
  in the stream's VUI, each aspect independently.
- **Rate control.** The standard encoder runs real-time. The low-latency encoder
  is chosen by the platform, never by the caller, when a stream needs temporal
  layers or a QP per frame type. `update()` applies a bitrate, peak or frame
  rate change to the running session; a changed quality or QP bound replaces
  the session between two frames and continues from a key frame.

## Errors and recovery

`configure()` answers `invalid`, `unsupported`, `exhausted` or `failed`. A
session the system revokes is rebuilt at the same settings and the unanswered
frames are encoded again, the first as a key frame. A second loss with no
output in between ends the stream: every frame in flight and every later one
is answered with an error until `reset()` starts a new stream.

## Tests

`platform-video-encoder-bitstream` needs no hardware. `platform-video-encoder`
exercises every advertised capability on the host's encoder and decodes the
streams with the platform decoder. `platform-video-encoder-recovery` links a
variant of the backend that fails sessions on demand, and
`platform-video-encoder-cache` runs child processes against cache files. The
three hardware tests share the `video-encoder-hardware` resource lock.
`Kernarium/KernariumTests/HostVideoEncoderTests.mm` checks the same on an
iPad.

# Platform camera

`camera.hpp` is the platform's camera interface; nothing in it names
AVFoundation, V4L2 or the Virtio wire. `UML::AppleCamera`
(`apple/camera_capture.mm`) implements it for macOS, iOS device and iOS
Simulator, and `camera_authorization.hpp` is the system's camera permission, in
the style of `microphone_authorization.hpp`. A Virtio camera device maps the
interface to the wire.

- **Discovery** (`cameras()`, `concurrentSets()`) reads devices, formats, ranges
  and the multi-camera sets and needs no authorization; it never prompts. A
  discovery that raises an Objective-C exception (`catch (const std::exception&)`
  does not catch it: a start with a catch-all fails, a caller with none is
  ended) lists no cameras and logs one line. The
  iOS 27.1 outer and inner ultra wide device types (the foldable iPhone's) are
  read only behind an availability check, and a device on 27.0 lists none. The
  order is stable, and every index in the interface is an index into it. A
  front camera publishes no flash: its flash is the screen's, which the guest's
  camera unit does not model. On iOS a camera's `sensorOrientation` is the
  platform's statement of its mounting, the rotation coordinator's angle for
  portrait (`sensorOrientationOf`; an external camera states 90), never a value
  per position, and every frame and still is delivered in that orientation,
  unmirrored, whatever turn, mirroring or still compensation the system would
  apply by default; a Mac's cameras take none of this and state 0, and so do
  an iPad app's on a Mac or a Vision Pro, where the coordinator does not take
  the call (it is unavailable there and raises: it is asked, never sent, so the
  camera keeps the mounting its sensor states). A built-in front camera states
  270, which Android's face-scan applications assume of a front camera, and the
  host turns every picture of it half around (`presentedMountingOf`,
  `CameraInfo::presentationTurn` 180): the camera device turns each frame of its
  video in the stream write, and a still is taken as an uncompressed full-range
  4:2:0 photo that the backend turns and encodes itself in the hardware JPEG
  encoder at the guest's quality (`turned_still.hpp`). The file has no colour
  profile, as the platform's own stills have none (a reader takes sRGB): the
  device's capture colour space is selected as sRGB, and the photo is delivered
  only when what it states is BT.709's or sRGB's (BT.709 primaries with an sRGB
  or BT.709 transfer, by its attachments, colour space or ICC profile, the
  colour space Core Video makes of those attachments included); a photo
  that states less is taken as sRGB only where the selection took effect. The
  session's own wide-colour choice is the host's only while such a camera is in
  the session. A photo that cannot be
  turned, and a Bayer still of such a camera, is refused with a recorded reason
  and never passed on unturned. The
  guest's metering and focus regions are mapped back through the turn, and
  through the connection's turn where there is one. A built-in front camera
  mounted with its sensor's top at the device's top (portrait 0 or 180) is
  presented like an earlier front camera as well: its video connection is turned
  by a quarter turn before the output's size is set, and the photo output's
  orientation compensation, which turns the stills, is on, read only after the
  configuration is committed (before that its answer is not valid) and switched
  on where it is supported and off; the photo connection keeps its default
  angle, unmirrored. A connection that does not hold the turn refuses the
  camera, a still whose photo output does not turn its stills is refused before
  its capture, and a still of another shape than the one asked for (the
  resolved size and the file's own are checked; a half turn has the same shape
  and is not caught) is never delivered. A square format that
  lists dynamic aspect ratios is described by the crop the device is pinned at
  at each admission and repair (3x4 for a quarter turn, else 4x3, a landscape
  frame of N x 3N/4), never changed while a stream or a still runs
  (`presentedFormatOf`). The change is one numbered operation, asked before the
  device is told: the camera's frames are held (the device's stand-in frames
  bridge it) until its completion arrives without an error, and then only the
  samples captured at or after the first buffer the change applies to pass (the
  completion's timestamp, converted from the input port's clock to the
  session's synchronization clock; without a conversion, the samples until the
  first of the anchor's shape are held), a still waits for it inside its wait
  budget, and an error, or no completion within that budget of a session running
  with the camera in it, refuses the camera as a connection that does not hold
  its turn does and fails the session, which the device answers by interrupting
  the guest's cameras and opening another (`RatioPins`). The operation ends with
  the camera's last input or output leaving the session, so a camera that left
  cannot fail the session the others are in. The session reports each camera's mounting, the
  settings of its connections and photo output and its aspect ratio as numbers
  to its delegate (`camera_presentation.hpp`), and keeps the last of each camera
  for the sessions that follow in the process (the current cameras and the
  eight most recently used others). Every camera's zoom range holds
  1.0 (Android's rule): a single lens states its own factors, so its 1.0 is its
  own full field of view, and only a zoom camera is scaled by the host's display
  multiplier (`zoomRatioScale`), as the triple camera's ultra wide lens at 0.5.
  A format carries the zoom the system recommends for it
  (`recommendedZoomMaximum`, zero where it states none), which the device
  states as the maximum of a camera that runs in that format, and the zoom the
  format itself delivers (`maximumZoom`), which it states instead where there is
  no recommendation.
- **Sessions.** `openSession()` is the one call that can light the system's
  capture indicator, and it returns null unless the system has granted access.
  One session is a single `AVCaptureMultiCamSession` on iOS (an
  `AVCaptureSession` on macOS) with an input and a video data output per running
  camera and a photo output once a still is asked for. Frames are NV12, full
  range, at the stream's size scaled by the platform from the format's at its
  aspect ratio (`deliveredFrameSize`), delivered through `CameraSessionDelegate`
  with the exposure that made them; holding a frame's `owner` holds the
  capture's buffer, and its `native` is the platform's own handle to the
  picture. A camera runs in one capture format, its **anchor**, whatever it
  is asked: a stream, a still, a still beside a stream, alone or with other
  cameras. Discovery lists the formats as data (`CameraInfo::captureFormats`);
  `chooseAnchor` picks the anchor once from them by the key in
  `photo_dimensions.hpp`, and the device gives it to the session with
  `setAnchors()` together with what the guest may ask. The session has no
  chooser and never moves a camera to another format: a camera's active format
  changes only when its input joins the session. A camera joins through one
  transaction (`admit`: the input, the anchor and its rate, the video output,
  the photo output, inside one configuration), which, when a step is refused or
  the session's hardware cost goes over its budget, removes exactly what that
  call added and puts the device back as it reported itself before anything
  changed, input included (`putBackTiming`: its format, frame durations and
  custom exposure: a long manual exposure may have made the maximum longer than
  the rate asked for, a rate recomputed would not restore that, and a shorter
  maximum shortens the exposure, which the durations alone do not bring back).
  A change of size or rate on a running stream does the same (`resizeStream`).
  When that restore is itself refused, the device holds something between the
  old and the new: the camera's cached stream and settings are no longer known
  (`StreamCache`), so the next request for the stream writes the format, the
  durations and the size again (it is not answered from the cache), the
  settings are written whatever they last were, and a request the device still
  cannot take fails.
  While another camera is in the session every anchor must be a
  format a multi-camera session takes (`multiCamera`); a camera whose best
  such format offers less than its best format does not run with others (the
  device then publishes it as a conflict). A camera that only takes stills
  leaves the session, with its photo output, when its last still is delivered.
  A camera whose format changed has its settings applied again. The video
  output's explicit size has the anchor's aspect ratio and the photo output's
  maximum is a size the anchor lists (`outputSizesFor`), both set after the
  output has its source. A photo output's delegate is held by the session until
  its capture ends.
- **Rates** are whole numbers of frames a second. A span's end within 0.15 % of
  a whole number is that number (29.97 is 30), and the rate is programmed as the
  span's own duration where the reciprocal would fall outside it
  (`rateSettingFor`). The guest is offered the whole rates of the anchor up to
  30 (`videoRateCap`); an anchor with none that low offers its slowest.
- **Controls** arrive as one `CameraSettings` per camera, and the session
  applies what differs. A zoom camera (a virtual device) takes automatic
  exposure, focus and white balance only. A stabilization request takes the
  low-latency mode where the active format supports it, else the standard one,
  else none (`video_stabilization.hpp`), and never the system's automatic choice.
  The mode is written at a video connection's first apply, whatever the other
  settings say, and a later apply of the same wish writes nothing. A new video
  connection is programmed as it is admitted, inside the configuration and
  before it commits (`AdmitSteps::programVideo`), with what the camera's
  settings last said, which the device applies before it starts the video; so a
  lens added to a running session delivers its first frame stabilized as well.
  iOS only: macOS has no connection property for it.
- **Stills** are a JPEG, a Bayer still, or both from one capture. A Bayer still
  carries its DNG's colour data (`dng_color.hpp`), the only place the host
  reveals it. A part that fails leaves the others: a capture fails only when
  nothing it asked for arrived. A JPEG is made at exactly the size asked for
  (`photo_dimensions.hpp`): the capture is refused when the active format does
  not list that size, and the photo is dropped when the platform resolved
  another. A capture that ends without a picture says why in its result
  (`CameraStillResult::failure`, `camera_still_failure.hpp`): the stage, the
  resolved size, whether the photo output's orientation compensation is
  supported and on, and the framework's error as a domain word and a code. A
  photo output gets its maximum size only after it is attached to the
  session, as AVFoundation requires: the largest size the anchor lists within
  the guest's still cap, so no larger pipeline is prepared. The guest is offered
  the sizes the anchor makes (`stillsOf`), and a camera that only takes stills
  runs in the same anchor. The 24 MP size (5712x4284) that AVFoundation services
  only as a deferred photo proxy is not listed, as the backend captures
  synchronously. A
  still's `exposure` is the photo's own where the photo states it: the exposure
  time, ISO, f-number, focal length and flash come from the photo's Exif
  (`apple/photo_metadata.hpp`, `still_exposure.hpp`), and a Bayer still's
  neutral point and noise profile from its DNG. The 3A states, white balance,
  zoom and lens are the session's as it reports them at delivery, not as they
  were at the exposure, and on macOS the session reports none of them, so they
  keep their defaults. A reading the photo does not give, or gives as a number
  that cannot be a measurement (an ISO outside 1 to 1,000,000, an exposure time
  of zero or over an hour, a negative or wider Flash field, not finite), is
  zero: the photo gave none, never a made-up value. A lens has one aperture and
  one focal length, so the lens's own is used when the photo names none. The
  flash fired is the Exif's word, then the resolved settings'.
- **Interruptions** (another application, a call, system pressure, iPad
  multitasking) arrive as events; the session does not stop itself. System
  pressure on a running camera also caps its frame rate (20 at serious, 15 at
  critical), as Apple advises, to the largest rate the camera offers below the
  cap (`throttledRate`), before the session is interrupted; the work a
  pressure notification queues holds what it uses (`PressureWatch`) and does
  nothing once the stream or the session has ended, and the observer that
  runs it (`apple/key_path_observer.h`) keeps the handler alive for as long as
  it runs, so a stop that comes meanwhile frees nothing it is using. A runtime
  error is restarted once, and the restart, like an interruption that ends,
  is a recovery (`recoverAnchors`): every camera the system moved out of its
  anchor is put back, with its outputs and settings, and only then is a
  `resumed` event sent. A repair that is refused, and an error before the
  restart, end the episode with `failed`, so that the client closes the
  session and tries again later, and a failure stays for the session
  (`RecoveryLatch`: a second notification repairs and announces nothing); the
  client delivers nothing from a camera it has been told is interrupted, nor
  anything captured before the repair: an output callback reads the repair
  boundary's generation when it is entered, before it waits for the lock the
  announcement holds, and delivers only while it is current, and a callback that
  began later compares the sample's capture time with the repair's on the
  capture's own clock (the session's synchronization clock, never the host's;
  `RepairBoundary`), with no age beyond which a sample is kept; the client
  discards what it had already been handed.
- `nv12_transform.hpp` crops, rotates and scales NV12 pictures: the portable
  `transformNv12()` is the reference, and `Camera::makeFrameTransformer()` gives
  the host's fastest. The Apple camera's (`apple/accelerated_nv12.hpp`) scales a
  picture that arrives as a pixel buffer on the system's pixel-transfer scaler
  and turns quarter turns with vImage, with its buffers kept between frames; a
  copy and a quarter turn are exact and a scale agrees with the portable one
  within the filters' difference (`camera-apple-transform`).
- The synthetic camera is `synthetic_camera/` (`UML::SyntheticCamera`), a
  platform camera with no hardware: profiles of a phone (three back lenses and a
  front camera, no zoom camera), a tablet (and one with both of an iPad's
  front cameras) and a laptop; the multi-camera rule, which counts cameras that
  only take stills and refuses a join over a hardware budget (200 million
  pixels a second); a deterministic chart
  whose top row encodes the camera and the frame counter (`decodeChartMarker`);
  brightness, blur, tint and zoom that follow the settings, an automatic
  exposure that follows the compensation within the frame the rate allows (the
  sensitivity takes the rest), and a frame that lasts as long as its exposure
  when that is longer than its period; JPEG and Bayer stills;
  simulated interruptions, which a named pipe also drives (a line each:
  `interrupt`, `interrupt background|multitasking|pressure`, `resume`; its path
  is the variable `UML_SYNTHETIC_CAMERA_CONTROL`, read when a launcher makes the
  camera, and nothing else reads it) for a guest in a process of its own; and,
  with a manual clock, frames that a test pumps. It is never a real camera. The test-only Android launcher
  (`uml-darwin-android17-edge-test`; no product launcher compiles the code) that
  makes it also reads a second pipe, `UML_BACKGROUND_EDGE_CONTROL` (a line each:
  `leave`, `return`), which plays an application's leaving and returning to the
  foreground: `leave` does what the Kernarium runtime does at the background edge
  (the camera's running video ends, the launcher waits for the guest to stop its
  video streams, then it holds the guest's GPU requests through its own request
  gate) and `return` releases them (`android-camera-video-leave`).

Tests: `camera-frame-transform` and `camera-dng-color` need nothing;
`camera-key-path-observer` runs the key-value observer on a stand-in object;
`camera-discovery-unavailable-call` requires that a coordinator that does not
take the angles call is never sent it (and one that does states its angles in
the order asked), and makes the platform's device list raise and requires no
cameras and one log line;
`camera-synthetic` exercises the synthetic camera through the interface; and
`camera-apple-discovery` reads the host's own cameras on any host. The Apple
backend's sessions are not run by a test, because a test must never capture from
a real camera.
