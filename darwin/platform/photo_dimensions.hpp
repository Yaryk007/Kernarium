// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// How a host's capture formats become what a guest is offered, and what a
// camera session does with them. Every function here is pure: the Apple
// backend (apple/camera_capture.mm) and the tests call the same code, and the
// backend's glue only turns a decision into platform calls.
//
// One format per camera, chosen once. A camera runs in its anchor, the one
// capture format that anchorKeyOf ranks best, whatever it is asked: a stream, a
// still alone, a still while streaming, alone or with other cameras. Everything
// the guest is offered (stills, video sizes, frame rates) is derived from the
// anchor (offerOf), so every offered run starts and every offered still is made.
// A session has no per-run choice and a camera's active format changes only when
// its input joins the session.

#include "camera.hpp"
#include "frame_bytes.hpp"
#include "video_stabilization.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::platform {

using VideoFormatFacts = CameraCaptureFormat;

// ---------------------------------------------------------------------------
// Orientation
// ---------------------------------------------------------------------------

// A camera's SENSOR_ORIENTATION from the platform's statement for it: `portraitAngle` is the clockwise turn, in
// degrees, that makes the camera's unrotated, unmirrored pictures upright with the device held in portrait (the
// rotation coordinator's angle for portrait), and the backend delivers every frame and still in that orientation. The
// mounting differs between cameras (an iPhone 15 Pro Max's front camera is upright with the port on the left, its back
// cameras with the port on the right) and between models, so no position has a fixed value. The angle is rounded to a
// quarter turn, and an angle that is not finite states 0. An external camera, whose mounting the platform does not
// know, states 90 whatever the angle.
[[nodiscard]] std::uint32_t sensorOrientationOf(CameraPosition position, double portraitAngle) noexcept;

// The orientation a built-in front camera states: the angle Android's front cameras have.
inline constexpr std::uint32_t frontCameraOrientation = 270;

// How a camera is presented to the guest: the orientation stated for it, the clockwise turn the backend sets on its
// video connection (the platform turns the buffers itself), whether the camera imitates an earlier front camera, and the
// half turn the host applies to every picture of it (`presentationTurn`: the device turns the video, the backend the stills).
//
// A built-in front camera states 270, the angle Android's front cameras have, and an app that takes it for granted (a
// camera app on the legacy camera API, a face-scan library) turns the picture by it. Its own mounting is brought there:
//  - a front camera whose rotation coordinator states portrait 0 or 180 is mounted with its sensor's top at the device's top,
//    where earlier front cameras were mounted with it at the side (their portrait angle is 90). The platform lets such a
//    camera behave like those: its video connection turns buffers by a fixed angle and its photo output turns its processed
//    stills (the output's compensation, left on). The video connection is turned by `portraitAngle - 90`, so that the
//    buffers are upright where the earlier cameras' are;
//  - a front camera at 90, which an imitated one now is, is turned by 180 degrees by the host and states 270;
//  - a front camera at 270 states it and is not turned.
// Every other camera (a back or an external one, or a camera whose angle is not finite) states sensorOrientationOf, is
// turned by 0 and imitates nothing. `presentationTurn` is only ever 0 or 180.
struct PresentedMounting {
    std::uint32_t stated {};
    std::uint32_t connectionTurn {};
    bool emulatesEarlierFront {};
    std::uint32_t presentationTurn {};
    friend bool operator==(const PresentedMounting&, const PresentedMounting&) = default;
};
[[nodiscard]] PresentedMounting presentedMountingOf(CameraPosition position, double portraitAngle) noexcept;

// A metering or focus region from a picture that is another picture turned clockwise by `turn` (0, 90, 180 or 270),
// as the same area of that other picture. Both are in fractions of their own sides, with the origin at the top left, so a
// quarter turn swaps the region's width and height. The guest's regions refer to the picture it is given, which the host
// has turned: each layer that turned a picture (the device by the camera's `presentationTurn`, the backend by its video
// connection's turn) maps the regions back through its own turn before they go on. A turn that is not a quarter turn is
// none. The weight is carried over, and a region outside its picture stays outside it (a value is never made valid by
// the mapping), as one inside stays inside.
[[nodiscard]] CameraRegion regionBeforeTurn(const CameraRegion& region, std::uint32_t turn) noexcept;

// The angle set on the photo output's connection: 0 for every camera, as before; none for a camera that imitates an
// earlier front camera, whose connection keeps its default (only its mirroring is turned off). The angle changes the
// Exif tag alone and the host drops the platform's Exif.
[[nodiscard]] constexpr std::optional<std::uint32_t> photoConnectionTurnOf(const PresentedMounting& mounting) noexcept
{
    if (mounting.emulatesEarlierFront) return std::nullopt;
    return std::uint32_t {0};
}

// What a still of a camera does about the photo output's orientation compensation, read when it starts. A camera that
// imitates an earlier front camera needs the output to turn its stills: compensation supported and on, which is read
// after the configuration is committed (a reading before it decides nothing). Supported and off is turned on, once;
// anything else refuses the still. Every other camera is not asked.
enum class CompensationStep : std::uint8_t { proceed, enable, refuse };
[[nodiscard]] CompensationStep stillCompensationStep(bool imitatesEarlierFront, CameraFact supported, CameraFact enabled,
                                                     CameraReading reading) noexcept;

// Which reading of the photo output's compensation an admission records (camera_presentation.hpp). The output is
// attached inside the admission, before its configuration is committed, so the reading taken then decides nothing; the
// record states the reading taken once the admission committed, which is of the output the admission added: the note is
// made as the admission returns, when the camera's entry does not hold that output yet. An admission that was refused and
// undone keeps the reading taken as the output was attached, as the output is gone.
[[nodiscard]] constexpr CameraReading admissionCompensationReading(bool admitted, bool outputAdded) noexcept
{
    return admitted && outputAdded ? CameraReading::afterCommit : CameraReading::beforeCommit;
}

// The photo output facts an admission records: the ones taken as the output was attached, with the compensation read again
// from the output the admission added (`added`, null where it added none) once the configuration committed. `read` takes
// the output and returns its facts as they read now. An admission that was undone keeps the facts it has.
template <typename Facts, typename Output, typename Read>
[[nodiscard]] std::optional<Facts> committedPhotoFacts(std::optional<Facts> attached, Output* added, bool admitted, Read&& read)
{
    if (attached && added != nullptr && admissionCompensationReading(admitted, true) == CameraReading::afterCommit) {
        const Facts committed = read(added);
        attached->compensationSupported = committed.compensationSupported;
        attached->compensationEnabled = committed.compensationEnabled;
        attached->reading = committed.reading;
    }
    return attached;
}

// What a still does about the dynamic aspect ratio its camera's anchor pins: nothing where it pins none or the device
// holds it; waits where the change's completion has not arrived; refuses where the completion reported an error, or
// arrived and the device holds another ratio.
enum class RatioStillStep : std::uint8_t { proceed, wait, refuseFailed, refuseMoved };
[[nodiscard]] RatioStillStep ratioStillStep(CameraAspectRatio pinned, CameraAspectRatio now, bool completionPending,
                                            std::int32_t completionError) noexcept;

// A camera's dynamic aspect ratio change, one operation at a time, as the session keeps it from the moment the device is
// asked until the camera leaves the session. The change is under way (`pending`) from before the device is told, so a
// completion that comes at once, or never, is not mistaken for another operation's; a completion that belongs to an
// earlier operation of the camera is ignored. `armedAt` is when the session was running with the camera in it, which is
// when the time the completion may take begins (zero: not begun).
//
// The completion does not make the frames already made right: the device delivers buffers of the ratio it had until the
// first buffer the change applies to, which the completion's timestamp names (on the device's clock, converted to the
// capture's, the timeline of every sample's timestamp). `boundary` says which rule keeps the older samples from the guest:
// `atTime`, the samples captured before `notBefore`; `shape`, where the timestamp could not be converted, the samples
// until the first of the shape the anchor presents (a weaker rule: the output's size is set, so it only sees a buffer of
// another aspect); `none`, where the device already held the ratio and nothing precedes the change.
enum class RatioBoundary : std::uint8_t { none, atTime, shape };
struct RatioPin {
    bool asked {};
    bool pending {};
    // The completion's error code (zero for none).
    std::int32_t error {};
    // No completion arrived within `ratioCompletionBudgetNanoseconds` of being armed.
    bool timedOut {};
    std::uint64_t operation {};
    std::uint64_t armedAt {};
    RatioBoundary boundary {};
    std::int64_t notBefore {};
    // A sample of the anchor's shape arrived (the `shape` rule is over).
    bool shapeSeen {};
};
// How long the completion of a ratio change may take once the session runs with the camera in it: the budget a still
// waits in (stillWaitBudgetNanoseconds), so that no wait of the capture is longer than that.
inline constexpr std::uint64_t ratioCompletionBudgetNanoseconds = 2'000'000'000;
// What a still is told for a change whose completion never came (ratioStillStep's `completionError`).
inline constexpr std::int32_t ratioNoCompletion = std::numeric_limits<std::int32_t>::max();

// The change `operation` is asked for.
void ratioPinAsked(RatioPin& pin, std::uint64_t operation) noexcept;
// The change `operation` completed with `error`. False, and nothing changed, for a completion of another operation than
// the camera's current one (or of none asked); a completion after the time ran out is kept (the record names it) but
// leaves the pin timed out.
bool ratioPinCompleted(RatioPin& pin, std::uint64_t operation, std::int32_t error, RatioBoundary boundary = RatioBoundary::none,
                       std::int64_t notBefore = 0) noexcept;
// The session runs with the camera in it: the budget begins, once for an operation that is still pending. True when it
// began now.
bool ratioPinArmed(RatioPin& pin, std::uint64_t now) noexcept;
// A pending operation whose budget ran out: it is timed out, once. True when it is now.
bool ratioPinTimedOut(RatioPin& pin, std::uint64_t now) noexcept;
// What the camera's frames do: are delivered, are held (the change is under way: the device still delivers frames of the
// ratio it had), or the camera's presentation is not the one the guest was told because the change failed or never
// completed (frames stay held and the camera is interrupted for the guest).
enum class RatioFrameStep : std::uint8_t { deliver, hold, failed, timedOut };
[[nodiscard]] RatioFrameStep ratioFrameStep(const RatioPin& pin) noexcept;
// The completion's error code a still is told for the pin (ratioStillStep).
[[nodiscard]] std::int32_t ratioPinErrorForStill(const RatioPin& pin) noexcept;
// Whether a camera's sample goes to the guest: the frames must be delivering (ratioFrameStep), and a sample made before the
// change took effect does not pass (RatioBoundary). `captureNanoseconds` is the sample's timestamp on the capture's clock,
// where it has one; `shapeIsAnchors` whether its dimensions have the anchor's aspect ratio.
[[nodiscard]] bool ratioSampleDelivered(RatioPin& pin, std::optional<std::int64_t> captureNanoseconds, bool shapeIsAnchors) noexcept;

// What a camera's note says of its ratio change: pending while the change is under way or never completed, else the
// completion's error code (zero for none); nothing for a camera with no change (it left the session), whose note keeps
// what it has. Read where the note is published, in the notes' critical section, never before it.
[[nodiscard]] std::optional<std::int32_t> ratioNoteCode(const std::optional<RatioPin>& pin) noexcept;
// Whether a camera's input leaves the session after a removal that left it `videoOutputRemains` and `photoOutputRemains`
// (a photo output stays for a still in flight): with its last output, and its ratio change with it.
[[nodiscard]] constexpr bool cameraLeavesSession(bool hasInput, bool videoOutputRemains, bool photoOutputRemains) noexcept
{
    return hasInput && !videoOutputRemains && !photoOutputRemains;
}
// Whether a session leaves its devices' colour spaces to the host, given the presentation turn of each camera whose input is in
// it. The stills of a camera presented half around (a turn of 180) are encoded with no colour profile, so its capture is held
// in sRGB, which the session's own choice (wide colour, for photos) undoes at every commit: while such a camera is in the
// session, the choice is the host's. A session without one keeps the system's choice for every device in it, as it always
// had, a back-only or external-only session included.
[[nodiscard]] constexpr bool sessionHoldsColourSpaces(std::span<const std::uint32_t> turnsOfInputs) noexcept
{
    for (const std::uint32_t turn : turnsOfInputs)
        if (turn == 180) return true;
    return false;
}

// The ratio changes of a session's cameras, by camera (the caller holds one lock for it): each camera's operation lives
// from its asking until the camera's last input or output leaves the session (`invalidate`); a watch or a completion of
// an operation that is gone does nothing, so a camera that left cannot fail the session the others are in, and its
// diagnostic history stays in the notes.
class RatioPins {
public:
    // A change is asked for the camera, replacing its earlier operation.
    void asked(std::size_t camera, std::uint64_t operation);
    // What a completion did: nothing (another operation's, or none is asked), the change took effect, it ended in an error
    // that fails the camera, or it came after the time had run out (kept, never reviving or failing it a second time).
    enum class Completion : std::uint8_t { ignored, succeeded, failed, afterTimeout };
    Completion completed(std::size_t camera, std::uint64_t operation, std::int32_t error, RatioBoundary boundary,
                         std::int64_t notBefore);
    // The session runs with the camera in it: the operation the budget began for now, if it did.
    [[nodiscard]] std::optional<std::uint64_t> armed(std::size_t camera, std::uint64_t now);
    // The operation's budget ran out: true once, for a pending operation that is still the camera's.
    [[nodiscard]] bool timedOut(std::size_t camera, std::uint64_t operation, std::uint64_t now);
    // The camera's last input or output left the session, or its admission was undone with nothing of it left.
    void invalidate(std::size_t camera);
    [[nodiscard]] std::optional<RatioPin> find(std::size_t camera) const;
    [[nodiscard]] bool sampleDelivered(std::size_t camera, std::optional<std::int64_t> captureNanoseconds, bool shapeIsAnchors);
    // Whether the camera's frames are held (ratioFrameStep is not `deliver`); a camera with no operation is not held.
    [[nodiscard]] bool holdsFrames(std::size_t camera) const;

private:
    std::map<std::size_t, RatioPin> pins_;
};

// The notes the platform keeps across its sessions by device identifier (the last admission of each camera): which to
// let go. The current session's cameras are kept, and of the rest the `recent` most recently used; `used` is a counter
// that grows with each use.
struct HeldNoteUse {
    std::string identifier;
    std::uint64_t used {};
};
[[nodiscard]] std::vector<std::string> heldNotesToDrop(const std::vector<HeldNoteUse>& held,
                                                       const std::vector<std::string>& current, std::size_t recent);
inline constexpr std::size_t heldNotesRecent = 8;

// A device format as the guest is offered it. A format that lists no dynamic aspect ratio is as listed. A square
// format that lists them is described by the crop the device is held at (pinned, never changed while a stream or a
// still runs): 3x4 where the video connection is turned a quarter turn, 4x3 where it is not, which both give a
// landscape frame after the turn, N x 3N/4 for the N x N format, and its photo sizes are the listed ones that are
// landscape 4:3. A format that lists ratios and cannot be described, because it is not square, its side is not
// divisible by four or it lacks the ratio needed, is dropped (nothing): a picture of another shape is never offered.
[[nodiscard]] std::optional<VideoFormatFacts> presentedFormatOf(const VideoFormatFacts& listed, std::uint32_t connectionTurn);

// The field of view of a pinned crop, in degrees: the larger of the 3x4 and the 4x3 crop's, uncorrected for
// geometric distortion, so that the cropped view is never stated narrower than it is.
[[nodiscard]] float pinnedFieldOfView(float portraitCrop, float landscapeCrop) noexcept;

// ---------------------------------------------------------------------------
// Photo sizes
// ---------------------------------------------------------------------------

// The size to ask the photo output for when a JPEG of `request` is wanted: the
// request itself, when the active format lists it and the output's maximum
// covers it. Nothing otherwise; the nearest larger size is not the request.
[[nodiscard]] std::optional<CameraStillSize> exactPhotoSize(std::span<const CameraStillSize> listed,
                                                            CameraStillSize request, CameraStillSize outputMaximum);

// For a capture that names no JPEG size (a Bayer still alone): the smallest
// listed size that covers `minimum` within the output's maximum.
[[nodiscard]] std::optional<CameraStillSize> coveringPhotoSize(std::span<const CameraStillSize> listed,
                                                               CameraStillSize minimum, CameraStillSize outputMaximum);

// The largest of the listed sizes by area, then by width; nothing for a table
// with none.
[[nodiscard]] std::optional<CameraStillSize> largestPhotoSize(std::span<const CameraStillSize> listed);

// How a photo output is made ready, in the order the platform requires. The
// output's maximum size must be one of the sizes of its source device's active
// format, which the output knows only once it is attached to the session with
// its connection: AVFoundation raises for the maximum of an output that has
// no source. So the output is attached first, the maximum is set second, and
// an output whose maximum is refused is detached again. The steps are the
// platform's calls; `setMaximum` reports a refusal (an exception) as false.
struct PhotoOutputSteps {
    std::function<bool()> attach;
    std::function<bool(CameraStillSize)> setMaximum;
    std::function<void()> detach;
};
enum class PhotoOutputPreparation : std::uint8_t {
    ready,
    notAttached,    // the session took no output: attach failed, nothing else ran
    maximumRefused  // attached, the maximum was refused, and the output was detached
};
[[nodiscard]] PhotoOutputPreparation preparePhotoOutput(std::optional<CameraStillSize> largest,
                                                        const PhotoOutputSteps& steps);

// The largest photo a format's output is set to make: the largest size the
// format lists that is within the still cap (`maximumStillPixels`, zero for
// none). A format that lists a 48 MP size under a 12 MP cap is set to its 12 MP
// one, so no larger pipeline is prepared.
[[nodiscard]] std::optional<CameraStillSize> photoMaximum(const VideoFormatFacts& format, std::uint64_t maximumStillPixels);

// The sizes a format makes within the cap, largest first: those it lists that
// are within the cap and no larger than photoMaximum in either dimension, the
// test a capture applies (exactPhotoSize against the output's maximum).
[[nodiscard]] std::vector<CameraStillSize> stillsOf(const VideoFormatFacts& format, std::uint64_t maximumStillPixels);

// Whether a format's photo output makes `size`: one of stillsOf.
[[nodiscard]] bool formatMakesPhoto(const VideoFormatFacts& format, CameraStillSize size, std::uint64_t maximumStillPixels);

// Every size some format makes (stillsOf), largest first: what a host can do at
// all, before a camera's anchor decides what it offers.
[[nodiscard]] std::vector<CameraStillSize> stillsMadeByAnyFormat(std::span<const VideoFormatFacts> formats,
                                                                 std::uint64_t maximumStillPixels);

// Whether the size the platform resolved for a photo is the size asked for. A
// resolved size of zero by zero is the platform stating none (a Bayer still
// alone has no processed photo), which is not a different size.
[[nodiscard]] bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept;

// The photos a 48 MP sensor makes by binning, 12 MP (4032x3024): the default
// pipeline serves them from the first capture. A photo output whose maximum is
// larger needs the sensor's full-resolution mode, which the pipeline takes its
// time to apply after the session commits the maximum.
inline constexpr std::uint64_t binnedSensorStillPixels = 4032ull * 3024ull;

// The largest JPEG a front camera is offered, in pixels: about 3 MP, unconditionally. A front camera's still session
// holds memory in the system's capture services, outside the application's footprint, and the pixel count of the photo
// drives it; a 4 GiB device has no room for the front lens's full-resolution size. 2016x1512 (3.05 MP), 2304x1296
// (2.99 MP) and 2048x1536 (3.15 MP) are within it; 2560x1440 (3.69 MP) is not. The back cameras' limit is the tier's,
// not this.
inline constexpr std::uint64_t frontStillMaximumPixels = 3'200'000;

// Whether a camera at `position` may be offered a still of `size` under the limit of its position: a front camera's is
// within frontStillMaximumPixels whatever else its formats list; any other camera has no such limit (the tier's cap
// applies to every camera).
[[nodiscard]] bool stillWithinPositionLimit(CameraPosition position, CameraStillSize size) noexcept;

// The formats of a camera as its guest is offered them. A front camera's photo sizes above frontStillMaximumPixels are
// dropped from every format, so that the offer (stillsOf), the photo output's maximum (photoMaximum) and the capture's
// size check read one list and a size is offered, prepared and made together. The limit is a ceiling with no
// exception: a front format that lists no size within it lists none, and a front camera none of whose formats does is
// offered no native JPEG (Android derives its standard JPEG sizes from the offered video). Another camera's formats are
// returned as they are. Only the photo sizes change: a format's own size, rates and multi-camera flag are those
// listed. The anchor and its offer are chosen and made on these formats, by the ranking as it is: it puts the frame
// rate before the still area, so the anchor can be a format that now lists no photo size, or another format than
// before (and its video offer with it). Whether a camera keeps its concurrency (keepsConcurrency) is decided on the
// listed formats, so that the limit is no reason for a camera to start running with others.
[[nodiscard]] std::vector<VideoFormatFacts> limitedStillFormats(CameraPosition position, std::span<const VideoFormatFacts> formats);

// Whether a photo output's maximum needs the full-resolution mode: it is above the binned size, whatever the area
// the still cap of a tier allows. Only this wait depends on it: a photo of another size is made again
// (stillSizeOutcome) for any maximum.
[[nodiscard]] bool photoMaximumNeedsFullResolution(CameraStillSize maximum) noexcept;

// The pipeline takes this long after such a maximum is committed before a
// capture is served at the new size.
inline constexpr std::uint64_t photoOutputSettleNanoseconds = 300'000'000;

// What a camera records when it commits a photo output's maximum (at admission
// and when its anchor is repaired, the same bookkeeping): `now` when the
// maximum needs the full-resolution mode, zero (nothing to wait for) when
// there is none or it does not.
[[nodiscard]] std::uint64_t photoMaximumCommittedAt(std::optional<CameraStillSize> maximum, std::uint64_t now) noexcept;

// How long a capture waits before it starts: the rest of the settle time
// since `committedAt` (photoMaximumCommittedAt), zero once it has passed or
// when there is nothing to wait for.
[[nodiscard]] std::uint64_t stillSettleNanoseconds(std::uint64_t committedAt, std::uint64_t now) noexcept;

// How long a still may spend waiting for the settle time in all, over its attempts. Every commit of the session's
// configuration (another camera's, a rate change while streaming) restarts the settle time, so without a bound a still
// accepted during it could be postponed for as long as commits keep coming. A commit chain of the ordinary kinds
// (admission, a stream start, a resize, a repair) costs a few settle times; two seconds is a fifth of the device's
// capture timeout, which starts when the still is handed over, and leaves it room for two 48 MP captures after the wait.
inline constexpr std::uint64_t stillWaitBudgetNanoseconds = 2'000'000'000;
static_assert(ratioCompletionBudgetNanoseconds == stillWaitBudgetNanoseconds, "one budget for a still and for a ratio change");

// What a capture does when its turn comes (when it is accepted, and again when each wait ends): it starts; waits the
// rest of the settle time of a later commit; fails, because that wait would take it past the budget (the guest is told
// at once, by the same failure as a photo of another size on the last attempt); or is dropped, because the session
// ended or the request is gone (a stop inside the settle time; the device owns ending those requests).
enum class ScheduledStillStep : std::uint8_t {
    fire,
    wait,
    fail,
    drop,
};
struct ScheduledStill {
    ScheduledStillStep step {};
    // For `wait`: how long to wait.
    std::uint64_t waitNanoseconds {};
    // What the still has waited in all, with this wait: kept with the request across its waits and its attempts, so
    // that the next attempt has what is left.
    std::uint64_t waitedNanoseconds {};
};
[[nodiscard]] ScheduledStill scheduledStillStep(bool sessionEnded, bool requestKnown, std::uint64_t committedAt, std::uint64_t now,
                                                std::uint64_t waitedNanoseconds) noexcept;

// The same for a camera whose pinned aspect ratio's change has not completed (ratioStillStep): it waits at least
// `ratioPinPollNanoseconds` more, inside the same budget, so that it never waits longer than a still that waits for the
// settle time.
inline constexpr std::uint64_t ratioPinPollNanoseconds = 50'000'000;
[[nodiscard]] ScheduledStill scheduledStillStep(bool sessionEnded, bool requestKnown, std::uint64_t committedAt, std::uint64_t now,
                                                std::uint64_t waitedNanoseconds, bool ratioPending) noexcept;

// What a finished capture whose photo is not the size asked for leads to. A
// capture that starts before the settle time has passed can still be served at
// the size the output made before; it is made again, and the photo of another
// size on the last attempt fails the capture (the guest is told at once, and
// nothing of another size is passed on).
enum class StillSizeOutcome : std::uint8_t {
    deliver,
    retry,
    fail,
};
inline constexpr unsigned stillAttemptLimit = 2;

// `attempt` counts from one: the capture that finished.
[[nodiscard]] StillSizeOutcome stillSizeOutcome(CameraStillSize resolved, CameraStillSize requested, unsigned attempt) noexcept;

// Takes a photo's JPEG file into `out` if one is wanted and the file is no
// larger than `maximumBytes` (zero: no bound). A file that is refused is not
// copied: nothing of it is allocated, and `out` is left as it was.
[[nodiscard]] bool takeJpegFile(std::span<const std::uint8_t> file, std::uint64_t maximumBytes, bool wanted,
                                std::vector<std::uint8_t>& out);

// ---------------------------------------------------------------------------
// Video sizes
// ---------------------------------------------------------------------------

// The size a video output delivers frames at, for a stream of `width` by
// `height` running in a format of `formatWidth` by `formatHeight`: the
// smallest even multiple of the format's reduced aspect ratio that covers the
// stream, or the format's own size when no smaller one does. The ratio is
// kept exactly (the platform refuses a scaled size of another aspect), so a
// stream of another aspect is delivered larger than it asks in one dimension.
// The result grows with the stream in each dimension.
struct FrameSize {
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const FrameSize&, const FrameSize&) = default;
};
[[nodiscard]] FrameSize deliveredFrameSize(std::uint32_t formatWidth, std::uint32_t formatHeight, std::uint32_t width,
                                           std::uint32_t height) noexcept;

// The bytes of the capture buffer a format delivers for a stream of `width` by
// `height` (deliveredFrameSize), by alignedFrameBytes.
[[nodiscard]] std::uint64_t deliveredFrameBytes(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height,
                                                std::uint32_t rowAlignment) noexcept;

// What the anchor decides about the outputs of the camera that runs in it: the
// video output's explicit size, which AVFoundation requires to be the aspect
// ratio of the device's active format (it raises otherwise), and the photo
// output's maximum, which must be one of the active format's sizes.
struct OutputSizes {
    // The size the video output is told to deliver; nothing when it delivers
    // the format's own size (or the camera has no stream, `width` of zero).
    std::optional<FrameSize> frame;
    // photoMaximum; nothing when the format lists none within the cap, which
    // leaves the output's own maximum.
    std::optional<CameraStillSize> photoMaximum;
};
[[nodiscard]] OutputSizes outputSizesFor(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height,
                                         std::uint64_t maximumStillPixels);

// ---------------------------------------------------------------------------
// Frame rates
// ---------------------------------------------------------------------------

// The fastest frame rate a guest is offered: photos win over fast streams, so a
// rate above it is neither offered nor asked of the device.
inline constexpr std::uint32_t videoRateCap = 30;

// The whole rates of a span. A span's end within 0.15 % of a whole number counts
// as that number (29.97 is 30, 30.00003 is 30, 23.976 is 24); otherwise the
// rates run from the ceiling of the minimum to the floor of the maximum. Nothing
// when the span holds no whole rate.
struct WholeRates {
    std::uint32_t minimum {};
    std::uint32_t maximum {};
    friend bool operator==(const WholeRates&, const WholeRates&) = default;
};
[[nodiscard]] std::optional<WholeRates> wholeRatesOf(const CameraRateSpan& span) noexcept;

// A format's rates as offered: its whole rates up to `rateCap`, ascending and
// merged. A format with whole rates only above the cap offers its slowest alone;
// a format with none offers nothing.
[[nodiscard]] std::vector<WholeRates> offeredRatesOf(const VideoFormatFacts& format, std::uint32_t rateCap);

// How a whole rate is programmed on a span of a format (the index is the span's
// in `frameRates`, which is the platform's own order). A rate inside the span
// sets the reciprocal duration. A rate a hair above the span's maximum (30
// against 29.97) sets the span's own shortest duration, and a hair below its
// minimum (30 against 30.00003) the span's own longest, which the platform
// accepts where the nominal reciprocal may fall outside the span.
enum class FrameDuration : std::uint8_t { reciprocal, spanShortest, spanLongest };
struct RateSetting {
    std::size_t span {};
    FrameDuration duration {};
    friend bool operator==(const RateSetting&, const RateSetting&) = default;
};
[[nodiscard]] std::optional<RateSetting> rateSettingFor(const VideoFormatFacts& format, std::uint32_t rate) noexcept;

// The duration, in seconds, that `setting` programs for `rate` on its span.
[[nodiscard]] double programmedFrameDuration(const CameraRateSpan& span, FrameDuration duration, std::uint32_t rate) noexcept;

// The rate a camera runs at while the system's pressure caps it (`pressureCap`,
// zero for none): the largest offered rate that is not above the lower of the
// request and the cap, else the slowest offered. Zero when nothing is offered.
[[nodiscard]] std::uint32_t throttledRate(std::span<const WholeRates> offered, std::uint32_t requested,
                                          std::uint32_t pressureCap) noexcept;

// A rate as programmed on a format: the whole rate and how the span sets it.
struct ProgrammedRate {
    std::uint32_t rate {};
    RateSetting setting;
    friend bool operator==(const ProgrammedRate&, const ProgrammedRate&) = default;
};

// What a stream asked for at `requested` frames a second runs at under the
// pressure cap (throttledRate, from the format's offered rates) and how that
// rate is programmed (rateSettingFor); nothing when no offered rate can be.
[[nodiscard]] std::optional<ProgrammedRate> programmedRate(const VideoFormatFacts& format, std::span<const WholeRates> offered,
                                                           std::uint32_t requested, std::uint32_t pressureCap);

// What the system's pressure leaves of a running stream's rate. The pressure
// observer (any thread) and the session's queue share one, and both hold it by
// shared ownership: it owns the facts the rate is programmed from, so a
// notification that comes after its session has gone finds them alive, and
// `end()` makes whatever is still queued program nothing.
class PressureWatch {
public:
    PressureWatch(VideoFormatFacts format, std::vector<WholeRates> offered);

    // The rate the guest asked for.
    void request(std::uint32_t rate) noexcept;
    [[nodiscard]] std::uint32_t requested() const noexcept;
    // The most frames a second the system's pressure allows; zero is no limit.
    [[nodiscard]] std::uint32_t cap() const noexcept;
    // The system now allows `cap`: whether that changed the cap of a live watch,
    // which means the rate is to be programmed again.
    [[nodiscard]] bool pressure(std::uint32_t cap) noexcept;
    // The rate to program now; nothing once the watch has ended.
    [[nodiscard]] std::optional<ProgrammedRate> programmed() const;
    [[nodiscard]] bool live() const noexcept;
    // The stream is over or its session is: nothing is programmed afterwards.
    void end() noexcept;

private:
    const VideoFormatFacts format_;
    const std::vector<WholeRates> offered_;
    std::atomic<std::uint32_t> requested_ {0};
    std::atomic<std::uint32_t> cap_ {0};
    std::atomic<bool> live_ {true};
};

// ---------------------------------------------------------------------------
// The anchor and the offer
// ---------------------------------------------------------------------------

// What a guest of a tier may ask, and the sizes video is cut from.
struct OfferRules {
    // The still cap and the largest frame the host may deliver for a stream.
    CameraLimits limits;
    // The tier's video size.
    std::uint32_t maximumVideoWidth {};
    std::uint32_t maximumVideoHeight {};
    std::uint32_t rateCap {videoRateCap};
    // The standard sizes and the native sizes of the camera's formats; sizes the
    // offer cannot use (odd, below 16, above the tier) are dropped by offerOf.
    std::vector<FrameSize> candidateSizes;
};

// What a format offers a guest.
struct CaptureOffer {
    std::vector<CameraStillSize> stills;  // largest first: stillsOf
    std::vector<FrameSize> videoSizes;    // largest first; never empty for a candidate
    std::vector<WholeRates> rates;        // ascending; never empty for a candidate
    // The largest request a stream may make of the format: the width of the
    // widest and the height of the tallest of videoSizes. The frame the format
    // delivers for the box is within the limits' frame bound, and so is the frame
    // for any request inside it (deliveredFrameSize grows with the request).
    FrameSize box;
};
[[nodiscard]] CaptureOffer offerOf(const VideoFormatFacts& format, const OfferRules& rules);

// How a format ranks as an anchor. The fields compare in this order, larger
// first; each is true to its name:
//  1. rate: the format has a whole rate within the cap, then the highest;
//  2. still: the area of the largest still it makes, then its width;
//  3. video: the area of the box of its video sizes;
//  4. cost: the smaller frame (the format's own width by height);
//  5. low rates: the lower minimum whole rate;
//  6. a full-range format.
struct AnchorKey {
    bool hasCappedRate {};
    std::uint32_t rate {};
    std::uint64_t stillArea {};
    std::uint32_t stillWidth {};
    std::uint64_t boxArea {};
    std::int64_t negativeCost {};
    std::int64_t negativeMinimumRate {};
    bool fullRange {};

    friend bool operator==(const AnchorKey&, const AnchorKey&) = default;
    friend auto operator<=>(const AnchorKey&, const AnchorKey&) = default;
    // Whether two formats offer the guest as much: equal on the first three.
    [[nodiscard]] bool offersAsMuchAs(const AnchorKey& other) const noexcept
    {
        return hasCappedRate == other.hasCappedRate && rate == other.rate && stillArea == other.stillArea &&
            stillWidth == other.stillWidth && boxArea == other.boxArea;
    }
};

// The key of a format; nothing for a format that cannot be an anchor: one with
// no whole rate or no video size in its box.
[[nodiscard]] std::optional<AnchorKey> anchorKeyOf(const VideoFormatFacts& format, const OfferRules& rules);

// The anchor of a camera: the index of the best-ranked candidate (the lowest
// index among equals); among the formats a multi-camera session takes when
// `multiCameraOnly`. Nothing when there is no candidate.
[[nodiscard]] std::optional<std::size_t> chooseAnchor(std::span<const VideoFormatFacts> formats, const OfferRules& rules,
                                                      bool multiCameraOnly);

// Whether a camera that could run with others keeps that: the best key among
// its multi-camera formats offers as much as the best key among all its
// formats. A camera with no multi-camera candidate does not.
[[nodiscard]] bool keepsConcurrency(std::span<const VideoFormatFacts> formats, const OfferRules& rules);

// ---------------------------------------------------------------------------
// The session's transactions
// ---------------------------------------------------------------------------

// Whether a camera may join a session that holds other cameras' inputs: a
// multi-camera session takes only the formats it lists, for every input, so
// both this camera's anchor and every other input's must be one of them.
[[nodiscard]] bool mayJoin(bool otherInputsInSession, bool ownAnchorMultiCamera,
                           std::span<const bool> otherAnchorsMultiCamera) noexcept;

// What a device holds that a change of its frame durations can move, as the
// device reports it. A frame duration is a rational number of seconds (the
// platform's CMTime). A custom exposure longer than a new maximum frame
// duration is shortened to it by the platform (AVCaptureDevice.h), so a change
// of the durations that is undone has to put the exposure back with them.
struct CameraTime {
    std::int64_t value {};
    std::int32_t scale {1};
    [[nodiscard]] double seconds() const noexcept { return scale > 0 ? double(value) / double(scale) : 0.0; }
    friend bool operator==(const CameraTime&, const CameraTime&) = default;
};
struct ManualExposure {
    bool custom {};  // the exposure mode is custom
    CameraTime duration;
    float iso {};
    friend bool operator==(const ManualExposure&, const ManualExposure&) = default;
};
struct DeviceTiming {
    // The platform's identity of the active format: only compared, and given back
    // to `setFormat`.
    std::uint32_t format {};
    CameraTime minimum;
    CameraTime maximum;
    ManualExposure exposure;
    friend bool operator==(const DeviceTiming&, const DeviceTiming&) = default;
};

// The calls a restore makes on a device. Each write reports a refusal (an
// exception included) as false.
struct TimingSteps {
    std::function<bool()> lock;  // lockForConfiguration; false: the device would not lock
    std::function<void()> unlock;
    std::function<DeviceTiming()> read;
    std::function<bool(std::uint32_t format)> setFormat;
    std::function<bool(CameraTime)> setMinimum;
    std::function<bool(CameraTime)> setMaximum;
    std::function<bool(CameraTime duration, float iso)> setCustomExposure;
};

// Which of the minimum and the maximum duration is written first when one of
// them moves to `target`: the maximum when the target is longer than the
// current maximum, so that the minimum is never set past it; the minimum
// otherwise, so that the maximum is never set short of it. (Setting both to one
// duration, as a rate does, and putting back a pair that was held.)
[[nodiscard]] bool maximumFirst(CameraTime currentMaximum, CameraTime target) noexcept;

// Puts a device back as `held` (read before anything changed) had it: its
// format when it has changed (a format has its own durations, so that comes
// first), then the durations, the one that moves out of the other's way first so
// that the minimum never exceeds the maximum, and then the custom exposure the
// new durations may have shortened. False when the device would not lock or any
// write is refused; nothing after the refused one is written.
[[nodiscard]] bool putBackTiming(const DeviceTiming& held, const TimingSteps& steps);

// What an input, its outputs and its format need for a camera to run a stream,
// a still or both, and the platform's calls that do it. The platform's calls
// report a refusal (an exception included) as false. Anything a call added when
// it refuses is removed by its `remove` step, which does nothing for what was
// never added. The device's timing (its format, frame durations and manual
// exposure, which `activate` and the input itself can change) is taken before
// the call adds anything (`snapshotTiming`, the device's own values, not what a
// rate would program: a long manual exposure lengthens the maximum) and put back
// by `restoreTiming` when the call refuses, after the input this call added has
// gone.
struct AdmitPlan {
    bool addInput {};  // the camera has no input in the session
    bool activate {};  // the anchor and the rate are set on the device
    bool addVideo {};
    bool addPhoto {};
};
struct AdmitSteps {
    std::function<void()> begin;   // beginConfiguration
    std::function<void()> commit;  // commitConfiguration
    std::function<bool()> addInput;
    std::function<void()> snapshotTiming;  // empty: nothing to take
    std::function<bool()> activate;
    std::function<bool()> addVideo;
    // Empty: nothing to program. Called when the video output is added, still inside the configuration, before it is
    // committed: what the new connection needs so that its first frame is the picture the rest are (a session that runs
    // already starts the output at the commit).
    std::function<void()> programVideo;
    // Attaches the photo output and sets its maximum (preparePhotoOutput).
    std::function<bool()> addPhoto;
    // Whether the session is over its hardware budget with what was added.
    std::function<bool()> overBudget;
    std::function<void()> removeVideo;
    std::function<void()> removePhoto;
    std::function<void()> removeInput;
    std::function<void()> restoreTiming;  // empty: nothing to put back
};
enum class Admission : std::uint8_t { admitted, inputRefused, formatRefused, videoRefused, photoRefused, overBudget };

// Runs the plan inside one configuration. A refusal, or a session over budget
// once the configuration is committed, removes exactly what this call added and
// puts the device's frame durations back as they were before `activate`, and
// nothing else changes: the format and outputs of a camera that was already in
// the session stay as they were.
[[nodiscard]] Admission admit(const AdmitPlan& plan, const AdmitSteps& steps);

// A running stream changes size or rate: the durations first, then the video
// output's size; a refusal puts the device's timing back as `snapshotTiming` took
// it before `setDurations` ran (the device's own values, as for AdmitSteps).
struct ResizeSteps {
    std::function<void()> begin;
    std::function<void()> commit;
    std::function<void()> snapshotTiming;  // empty: nothing to take
    std::function<bool()> setDurations;  // empty: the rate does not change
    std::function<bool()> setVideoSize;  // empty: the size does not change
    std::function<void()> restoreTiming;
};
enum class Resizing : std::uint8_t { resized, durationsRefused, sizeRefused };
[[nodiscard]] Resizing resizeStream(const ResizeSteps& steps);

// What the backend believes a camera's stream and device hold: the stream's
// size and rate, which an identical request finds already made and answers
// without touching the device. A change the device refused is undone with
// `restoreTiming`, and when that is refused too (the device would not lock, a
// setter raised) the device holds something between the old and the new:
// nothing the backend remembers of it is known any longer. Until a change has
// written the format, the durations and the size again (`timingWritten`), an
// identical request is not answered from the cache, the rate and the size are
// both written, and the settings written before (a manual exposure the shorter
// durations may have cut) are written again whatever they were last found to be.
struct StreamCache {
    bool video {};
    std::uint32_t width {}, height {}, frameRate {};
    bool unrestored {};

    enum class Path : std::uint8_t { cached, resize, admit };
    // How a request for a stream is met.
    [[nodiscard]] Path pathFor(std::uint32_t askedWidth, std::uint32_t askedHeight, std::uint32_t askedRate) const noexcept;
    [[nodiscard]] bool rateChanges(std::uint32_t askedRate) const noexcept;
    [[nodiscard]] bool sizeChanges(std::uint32_t askedWidth, std::uint32_t askedHeight) const noexcept;
    // Whether applying settings may be skipped: they are the ones already applied, no write is forced, and the
    // device's state is known.
    [[nodiscard]] bool mayLeaveSettings(bool force, bool unchanged) const noexcept;
    void restoreRefused() noexcept { unrestored = true; }
    // The format and the durations are written again: what the cache says is what the device holds. True when
    // they were not known, and the settings written before are to be written again.
    bool timingWritten() noexcept;
};

// Applies a camera's settings: the stabilization wish is noted and `stabilize` runs first, whatever the other settings
// are, and `write` puts the other settings unless they may be left (`unchanged`: they are the ones applied; see
// StreamCache::mayLeaveSettings). The stabilization comes first because the settings a new video connection's cache holds
// are the defaults, which are what the guest asks for in video mode: an apply that left them as they were left the
// connection unstabilized, and the admission of a connection has the wish only from an apply that left them. Returns
// whether `write` ran.
template <typename Stabilize, typename Write>
bool applyCameraSettings(const StreamCache& cache, StabilizationWish& wish, bool wanted, bool force, bool unchanged,
                         Stabilize&& stabilize, Write&& write)
{
    wish.note(wanted);
    stabilize();
    if (cache.mayLeaveSettings(force, unchanged)) return false;
    write();
    return true;
}

// An interruption has ended, or the session restarted after an error, and the
// system may have moved a device to another format meanwhile. Every camera that
// is out of its anchor is put back, outputs and settings with it, and only then
// is the session said to be back: the client delivers nothing from a camera it
// has been told is interrupted, so no frame outside an anchor reaches the guest,
// and none is announced as resumed that was not repaired. The first repair that
// is refused (the device would not lock, a setter raised) makes the outcome
// `failed`, the session's failure that the client already handles by closing it
// and opening another; no camera is announced as back by a failed recovery.
enum class Recovery : std::uint8_t { resumed, failed };
struct RecoverySteps {
    std::function<bool(std::size_t camera)> outOfAnchor;
    std::function<bool(std::size_t camera)> repair;  // false: refused
    std::function<void(Recovery)> announce;          // told once, after the last repair
};
Recovery recoverAnchors(std::span<const std::size_t> cameras, const RecoverySteps& steps);

// A recovery is asked for by two notifications that may both come (the end of an
// interruption and the restart after an error), and a second one finds the
// cameras where the first left them: a repair that changed a device's format and
// then failed leaves it in its anchor, so the second would skip the repair and
// announce the session back with an output still unrepaired. A failed recovery
// is therefore final for its session, which the client closes on it: the latch
// keeps the failure until the session is torn down, and a later recovery
// repairs nothing and announces nothing (the failure has been announced).
class RecoveryLatch {
public:
    Recovery recover(std::span<const std::size_t> cameras, const RecoverySteps& steps);
    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

private:
    std::atomic<bool> failed_ {false};
};

// Samples the platform's capture hands over after a recovery began can be from
// before it: one already inside the client's callback, waiting for the lock that
// delivery and the announcement of the repair share, and one the capture had
// queued behind it. Both were made by the device as it was moved out of its
// anchor, and none may reach the guest once the session is said to be back.
//
// A callback reads the boundary's generation when it is entered, before it waits
// (`entered`), and delivers only while that is still the current generation: a
// repair made after the callback began rejects it, however long it waited. A
// callback that began after the repair compares the sample's capture time with
// the repair's, both on the capture's own clock (the timeline of every sample's
// timestamps, the session's synchronization clock): a sample captured before the
// repair is rejected at any age, one captured after it is kept. The host's clock
// is never compared with it, so a capture clock that is offset from the host's by
// any amount rejects nothing it should not, and mutes nothing for good. A sample
// without a timestamp, or a repair without a reading of the clock, is judged by
// the generation alone.
class RepairBoundary {
public:
    using Generation = std::uint64_t;

    [[nodiscard]] Generation entered() const noexcept { return generation_.load(std::memory_order_acquire); }
    // The cameras are back in their anchors: `captureClockNanoseconds` is the capture clock's reading now. Called
    // with the lock that delivery holds, before the session is announced as resumed.
    void repaired(std::optional<std::int64_t> captureClockNanoseconds) noexcept;
    // Whether a sample is from before the last repair. Called with the same lock.
    [[nodiscard]] bool predates(Generation callbackEntered, std::optional<std::int64_t> capturedAt) const noexcept;

private:
    std::atomic<Generation> generation_ {0};
    std::optional<std::int64_t> repairedAt_;
};

// What an output callback does with a sample: it reads the boundary when it is entered, before it waits for the
// delivery lock, and hands the sample on only if the boundary lets it pass once it has the lock.
template <typename Mutex, typename Deliver>
bool deliverSample(const RepairBoundary& boundary, Mutex& mutex, std::optional<std::int64_t> capturedAt, Deliver&& deliver)
{
    const RepairBoundary::Generation entered = boundary.entered();
    std::lock_guard lock(mutex);
    if (boundary.predates(entered, capturedAt)) return false;
    deliver();
    return true;
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

enum class Refusal : std::uint8_t {
    noAnchor,
    formatChanged,
    notJoinable,
    inputRefused,
    outputRefused,
    overBudget,
    stillNotMade,
    rawUnavailable,
    raised,
    notRestored,
    ratioMoved,
    turnNotHeld,
    stillsNotTurned,
    ratioFailed,
    ratioTimedOut,
    stillNotTurned,
};
[[nodiscard]] std::string_view refusalName(Refusal reason) noexcept;

// One line per camera and reason until the camera's configuration changes: a
// guest that asks again and again must not fill the log. The photo callback's
// thread and the session's queue both write it (a still that fails, a stream that
// stops), so every access takes the log's own lock.
class RefusalLog {
public:
    // True the first time a camera is refused for a reason since the camera was
    // last reset.
    [[nodiscard]] bool first(std::size_t camera, Refusal reason);
    void reset(std::size_t camera);
    // Every camera is reset.
    void clear();

private:
    std::mutex mutex_;
    std::map<std::size_t, std::uint32_t> seen_;
};

} // namespace uml::platform
