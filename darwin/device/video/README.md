# Virtio video decoder

`UML::VideoDevice` exposes a stateful V4L2 decoder through Virtio device 31. The
device owns command validation, stream state, guest buffer mappings, and
completion events. It reuses the common vhost-user transport; decoding belongs
to `UML::VideoDecoder` in the platform layer, which requires VideoToolbox
hardware decoding. No GPU context is required. Encoding is a separate device,
ID 30, described [below](#encoder-device-id-30).

macOS enables the service with `--video`. Kernarium supplies it to each
supported kernel profile when hardware decoding is available. The shared
mainline and Android driver creates `/dev/video0`; the Steam image provides the
`/dev/video-dec0` udev alias used by CEF.

The device advertises the H.264, HEVC, VP9 and JPEG profiles the platform
capability reports and the wire can name, and drops any other rather than
publishing an unnamed format. Decoded surfaces are copied into guest NV12,
planar YUV420, P010, or packed BGRA buffers. Rows lie at the pitch
`raw_pitch.hpp` states and the guest drivers report as `bytesperline`, so a
client and the guest's allocator agree on a layout the host GPU can sample
without asking each other.

## Decoder contract

- **Capture format.** A client chooses its capture format only after the
  source-change event. The device reads each packet's headers without decoding,
  announces the size, and holds the packet, within a bounded count, until the
  client names a format, so an ordinary session converts nothing. A client that
  changes format mid-stream gets the new layout at the next random access
  point, and frames that still arrive planar are converted to what the client
  asked for.
- **Shared-memory buffers.** With `VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` a capture
  plane may name a region and offset of a host-visible Virtio-GPU blob. The
  device offers the feature only when it was given the shared-memory index, and a
  buffer whose blob has gone completes with an error.
- **Display order.** The H.264 and HEVC decoders derive output order from the
  stream (`apple/h264_stream.cpp`, `apple/hevc_stream.cpp`), because a V4L2
  client cannot reorder. H.264 field pictures are not modelled and leave at once
  in decode order; VP9 and JPEG pictures leave as decoded.
- **Capture minimum.** From the size announcement on, the capture queue's
  `min_buffers` is the stream's reorder depth plus one, and four at the least.
- **Seek.** `QUEUE_CLEAR` on the coded queue drops pictures, references and
  order state, and keeps the parameter sets and the capture format.
- **Resolution change.** The device delivers every picture of the old size,
  then one empty capture buffer flagged `VIRTIO_VIDEO_DEQUEUE_FLAG_EOS` (V4L2
  LAST), then `VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED`. It holds back the
  coded buffer that carries the new size until the client sets up the new
  capture queue, stops it, drains or clears the coded queue. A drain a client
  asked for ends at the first LAST buffer, so a change inside it gets no marker
  of its own. The first size of a stream is the event alone, unless the client
  set the capture queue up for another size before it (v4l2_codec2's provisional
  capture), which the device treats as a size change. The wire side is in
  [`../../../kernel/common/video/README.md`](../../../kernel/common/video/README.md).
- **Limits.** The device lists a format's levels from the lowest up to the
  highest the platform capability reports, and picture sizes up to its maximum
  per axis. HEVC states levels 1 to 6.2 with no tier. H.264 stops at 5.2,
  because the hardware does not open the 8192 x 4320 frame a level 6 stream may
  carry. VP9 is Profile 0 (8-bit 4:2:0) and, where the hardware decodes it,
  Profile 2 (10-bit 4:2:0), up to 4096 x 4096 each; the driver's VP9 profile
  menu lists the profiles the host lists and it publishes no VP9 level
  control. VideoToolbox's supplemental VP9 decoder is registered from iOS 26.2;
  on an iOS below it the host reports no VP9 hardware decode. The range is the
  square Android states for `c2.v4l2.vp9.decoder`,
  which it applies to both axes, from 64 samples on each. A 10-bit stream
  reaches an 8-bit capture format narrowed with rounding, as HEVC Main 10 does.
  `platform-video-decoder` decodes at the stated maximums, so a host whose
  hardware does less fails that test and not a guest.
- **Flow.** Input is complete Annex B access units, complete JPEG images, or VP9
  packets: one frame, or a superframe whose hidden frames the hardware decodes
  with the shown one. The coded queue's buffer size follows the picture size the
  client states. The device holds an encoded packet back while the hardware has
  no capture buffer to decode into.
- **Guest input.** The platform decoder reads the headers of every VP9 frame of
  a packet, not only the first, and refuses before the hardware sees it, with
  the decoder as it was, a packet with a truncated or reserved header
  (colour space 6 included), a last byte that reads as a superframe index
  marker without an index that fits the packet, a profile other than 0 or 2, a
  12-bit or non-4:2:0 stream, frames of more than one profile in one packet, or
  a picture size outside the range, as it refuses an oversized H.264 or HEVC
  picture. Decoding starts at a packet's last key frame, which replaces what
  the frames before it refer to; frames before it are validated by header only,
  so damaged data in one of them goes unnoticed. A packet shows what its last
  frame shows: nothing when that frame is not shown, as libvpx shows none. A
  packet that ends in a hidden key frame of another size changes the stream's
  size with no picture, and its coded buffer comes back once a later picture
  of that size has been announced and the client has set it, or when the
  client seeks or drains. An intra-only frame that changes the picture size is
  not supported, because the size comes from the key frame. Whatever follows a
  valid header is the hardware's to decode, and its failure ends the stream's
  pictures with an error until the client seeks.

`UML_VIDEO_ACTIVITY_LOG` names a file in which the device appends the profiles
it advertised and each stream's codec and totals, since a guest that falls back
to software decoding looks the same from inside. Unset, nothing is written.

`virtio-video-protocol` drives the decoder with a synthetic decoder;
`platform-video-decoder` decodes the checked-in fixtures on Apple hardware; the
optional `guest-v4l2-decode` runs the real V4L2 driver against every advertised
profile (see [testing](../../../docs/testing.md) for the guest probe input). The
tests that use the host's hardware share the `video-encoder-hardware` resource
lock and run in `check-gpu`.

## Suspend and session loss

A guest suspend disables the rings one after another and enables them in the
same order, and a ring disable resets a device that still holds a request.
Neither video device is reset: the event queue never defers, so with no event
waiting the guest's buffer stays in its ring and the device raises its event
descriptor when an event comes, and the device asks the transport to keep the
requests it holds (`keepsDeferredRequestsWhileDisabled()`), the buffers a
stream has queued. What the device completes while a ring is disabled is
written to the ring at its enable. A stream therefore goes on after a suspend
with the next picture or coded buffer, a descriptor opened before it works
after it, and a session started after it gets its source change. A message that
changes what a stream refers to, or a driver's teardown, still resets the
device.

The system may invalidate a platform session, as iOS does to an app that was in
the background. The decoder then rebuilds it from the stream's stored parameter
sets, shows nothing until the next sync picture and reports no error; a second
loss with no picture between is an error to the client. The encoder starts
again at a key frame. A final loss completes the stream's frames with `ERR` and
sends `VIRTIO_VIDEO_EVENT_ERROR`.

## Encoder (device ID 30)

The encoder is a second Virtio device instance with the standard encoder ID,
beside the decoder's 31. It has its own vhost-user service, so its asynchronous
callbacks and queues stay apart from the synchronous decoder, a guest kernel
without the encoder never binds it, and a host without a hardware encoder never
creates it. The [wire contract](../../../kernel/common/video/README.md#encoder-device-id-30)
and the platform interface (`darwin/platform/video_encoder.hpp`) are its
contract; the device depends only on that interface, the memory layer and the
device contracts, never on VideoToolbox. The capability and control tables are
pure functions of the platform's capabilities (`encoder_capabilities.cpp`,
`encoder_controls.cpp`), so tests check them without a device.

macOS enables the encoder with `--video-encoder`; `--video` stays decoder-only.
The Android launcher passes both, and Kernarium creates the encoder service
beside the decoder's whenever the platform reports a hardware encoder. An
explicit `--video-encoder` on a host without one fails to start.

A guest reaches the encoder as a stateful V4L2 encoder (`stateful-encoder` video
node). Raw frames are copied once into a buffer the platform owns, and the
guest's input buffer completes as soon as that copy is done. Coded buffers are
Annex B for H.264 and HEVC and a complete JFIF image for JPEG, in decode order,
each carrying its source frame's timestamp. Discovery is the platform's, so a
control, format or menu item that no encode on this host proved is absent, and
the guest learns the host's capability from what exists.

The device guarantees:

- **Session.** The platform session is created at the stream's first raw frame
  from the controls and formats settled by then; afterwards controls that are
  not dynamic and the formats answer `ERR_BUSY`. A set of controls applies
  entirely or not at all. `QUEUE_CLEAR` ends the session, returns the buffers
  that queue held with an error, and the next frame starts a new session at a key
  frame.
- **Layout.** The device decides each raw plane's size and stride and reports
  them in `GET_PARAMS`; a smaller buffer is refused at attach.
- **Buffers.** A raw buffer completes once copied, before encoding. When the
  platform has no free frame the device holds the raw buffers; a bounded number
  of coded frames wait in the device for coded buffers.
- **Output.** A frame larger than its buffer completes with `ERR` and the next
  frame is a key frame. `PREPEND_SPSPPS_TO_IDR` and `HEADER_MODE_SEPARATE`
  decide where parameter sets appear.
- **Drain and failure.** `STREAM_DRAIN` completes when every earlier frame has
  returned, and the next coded buffer completes empty with `EOS`. A session the
  platform refuses completes the stream's frames with `ERR` and sends
  `VIRTIO_VIDEO_EVENT_ERROR`; the guest recovers with `QUEUE_CLEAR`.
- **Limits.** At most eight streams, or the platform's concurrent session limit
  if lower.

With `UML_VIDEO_ACTIVITY_LOG` set the encoder also appends lines marked
`encoder`: what it advertised, each stream's settings and its totals at close.
They are the evidence that the hardware encoded.

### Encoder controls

The host publishes controls through `VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS` and
the guest driver registers exactly what is published. IDs and menu indices are
the Linux UAPI values in
[`virtio_video_v4l2_ids.h`](../../../kernel/common/video/virtio_video_v4l2_ids.h),
which `sync-protocol.py` checks against the kernel. Every control is grabbed
while the stream encodes unless it is dynamic (bit rate, peak, quality, force
key frame, parameter-set prepending, JPEG quality, maximum QP). Which controls
exist, and when, is `encoder_controls.cpp`:

- Bit-rate mode, bit rate, GOP size, force key frame, header mode and
  parameter-set prepending are always published; B-frames only for a codec with
  a B-frame run; the peak, constant-quality, constant-QP, VBV/CPB and
  hierarchical-layer controls only when discovery verified the matching platform
  capability; H.264 and HEVC controls only for a codec the host encodes.
- The profile, level and tier menus list only items an encode wrote a matching
  SPS for. The stream states the level asked for, or the lowest its size, frame
  rate and bit rate fit if that is higher.
- Raw formats are NV12, YUV420, P010 (HEVC Main 10), packed BGRA and RGBA (the
  memory order of Android's AB24 surfaces), and NV16 and NV24 for H.264 High
  4:2:2 and 4:4:4; each is listed only when some profile encoded from it. The
  coded formats are `H264`, `HEVC` and `JPEG`.
- Colour and HDR travel as V4L2 colorspace, transfer, matrix and quantization
  values in `SET_STREAM_INFO` and as the HEVC Main 10 HDR10 compound controls;
  they are written into every pixel buffer and the stream's VUI, and
  `GET_STREAM_INFO` on the coded queue returns what was written.
- The device publishes nothing the host can neither honor nor emulate:
  multi-slice, intra refresh, long-term references, average QP, frame skip and
  B-frame hierarchical coding. Profiles no guest API can name (ProRes, HEVC
  range extensions and alpha) are not exposed.

`virtio-video-encoder-protocol` drives both queues in front of a synthetic
encoder (`tests/support/video`) and then the host's hardware encoder;
`video-encoder-capabilities` checks the capability and control tables without a
device and that the wire lists exactly what discovery found.

`virtio-video-suspend` runs the decoder behind the real transport through a ring
disable and enable, and `vhost-user-held-requests` checks the transport's side
of it: requests held, completions and events written at the enable, kicks with
and without an acknowledgement, and the messages that reset.
`platform-video-decoder-recovery` and `platform-video-encoder-recovery` lose the
VideoToolbox session, and `guest-video-suspend` suspends a Linux guest by the
host's route and its own with both devices idle, with a node opened ahead of
use, and with a decode and an encode stream in progress.
