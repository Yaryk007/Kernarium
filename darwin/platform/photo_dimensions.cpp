// SPDX-License-Identifier: GPL-2.0-or-later
#include "photo_dimensions.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace uml::platform {
namespace {

[[nodiscard]] bool within(CameraStillSize size, CameraStillSize maximum) noexcept
{
    return size.width <= maximum.width && size.height <= maximum.height;
}

[[nodiscard]] std::uint64_t areaOf(CameraStillSize size) noexcept
{
    return std::uint64_t(size.width) * size.height;
}

[[nodiscard]] bool largerPhoto(CameraStillSize a, CameraStillSize b) noexcept
{
    return areaOf(a) != areaOf(b) ? areaOf(a) > areaOf(b) : a.width > b.width;
}

[[nodiscard]] bool withinCap(CameraStillSize size, std::uint64_t maximumStillPixels) noexcept
{
    return maximumStillPixels == 0 || areaOf(size) <= maximumStillPixels;
}

// A rate within this fraction of a whole number is that number.
constexpr double wholeRateTolerance = 0.0015;
constexpr double largestRate = 4.0e9;

[[nodiscard]] std::optional<double> snappedRate(double rate) noexcept
{
    if (!(rate >= 0.5) || rate > largestRate) return std::nullopt;
    const double whole = std::round(rate);
    if (std::fabs(rate - whole) <= wholeRateTolerance * whole) return whole;
    return std::nullopt;
}

} // namespace

std::optional<CameraStillSize> exactPhotoSize(std::span<const CameraStillSize> listed, CameraStillSize request,
                                              CameraStillSize outputMaximum)
{
    if (request.width == 0 || request.height == 0 || !within(request, outputMaximum)) return std::nullopt;
    for (const auto& size : listed)
        if (size == request) return size;
    return std::nullopt;
}

std::optional<CameraStillSize> coveringPhotoSize(std::span<const CameraStillSize> listed, CameraStillSize minimum,
                                                 CameraStillSize outputMaximum)
{
    std::optional<CameraStillSize> chosen;
    for (const auto& size : listed) {
        if (size.width == 0 || size.height == 0 || size.width < minimum.width || size.height < minimum.height ||
            !within(size, outputMaximum))
            continue;
        if (!chosen || areaOf(size) < areaOf(*chosen)) chosen = size;
    }
    return chosen;
}

std::optional<CameraStillSize> largestPhotoSize(std::span<const CameraStillSize> listed)
{
    std::optional<CameraStillSize> largest;
    for (const auto& size : listed)
        if (size.width != 0 && size.height != 0 && (!largest || largerPhoto(size, *largest))) largest = size;
    return largest;
}

PhotoOutputPreparation preparePhotoOutput(std::optional<CameraStillSize> largest, const PhotoOutputSteps& steps)
{
    if (!steps.attach()) return PhotoOutputPreparation::notAttached;
    // A format that lists no size leaves the output's own default.
    if (!largest) return PhotoOutputPreparation::ready;
    if (steps.setMaximum(*largest)) return PhotoOutputPreparation::ready;
    steps.detach();
    return PhotoOutputPreparation::maximumRefused;
}

std::optional<CameraStillSize> photoMaximum(const VideoFormatFacts& format, std::uint64_t maximumStillPixels)
{
    std::optional<CameraStillSize> largest;
    for (const auto& size : format.photoSizes)
        if (size.width != 0 && size.height != 0 && withinCap(size, maximumStillPixels) && (!largest || largerPhoto(size, *largest)))
            largest = size;
    return largest;
}

std::vector<CameraStillSize> stillsOf(const VideoFormatFacts& format, std::uint64_t maximumStillPixels)
{
    std::vector<CameraStillSize> sizes;
    const auto maximum = photoMaximum(format, maximumStillPixels);
    if (!maximum) return sizes;
    for (const auto& size : format.photoSizes)
        if (size.width != 0 && size.height != 0 && withinCap(size, maximumStillPixels) && within(size, *maximum) &&
            std::find(sizes.begin(), sizes.end(), size) == sizes.end())
            sizes.push_back(size);
    std::sort(sizes.begin(), sizes.end(), largerPhoto);
    return sizes;
}

bool formatMakesPhoto(const VideoFormatFacts& format, CameraStillSize size, std::uint64_t maximumStillPixels)
{
    const auto maximum = photoMaximum(format, maximumStillPixels);
    return maximum && withinCap(size, maximumStillPixels) && exactPhotoSize(format.photoSizes, size, *maximum).has_value();
}

std::vector<CameraStillSize> stillsMadeByAnyFormat(std::span<const VideoFormatFacts> formats, std::uint64_t maximumStillPixels)
{
    std::vector<CameraStillSize> sizes;
    for (const auto& format : formats)
        for (const auto& size : stillsOf(format, maximumStillPixels))
            if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
    std::sort(sizes.begin(), sizes.end(), largerPhoto);
    return sizes;
}

bool stillWithinPositionLimit(CameraPosition position, CameraStillSize size) noexcept
{
    return position != CameraPosition::front || areaOf(size) <= frontStillMaximumPixels;
}

std::vector<VideoFormatFacts> limitedStillFormats(CameraPosition position, std::span<const VideoFormatFacts> formats)
{
    std::vector<VideoFormatFacts> limited(formats.begin(), formats.end());
    if (position != CameraPosition::front) return limited;
    // A size without a dimension is none (stillsOf and photoMaximum take it so too).
    const auto dropped = [&](CameraStillSize size) {
        return size.width == 0 || size.height == 0 || !stillWithinPositionLimit(position, size);
    };
    for (auto& format : limited)
        format.photoSizes.erase(std::remove_if(format.photoSizes.begin(), format.photoSizes.end(), dropped), format.photoSizes.end());
    return limited;
}

std::uint32_t sensorOrientationOf(CameraPosition position, double portraitAngle) noexcept
{
    if (position == CameraPosition::external) return 90;
    if (!std::isfinite(portraitAngle)) return 0;
    const double quarters = std::fmod(std::round(portraitAngle / 90.0), 4.0);
    return std::uint32_t(quarters < 0 ? quarters + 4 : quarters) * 90;
}

PresentedMounting presentedMountingOf(CameraPosition position, double portraitAngle) noexcept
{
    const std::uint32_t own = sensorOrientationOf(position, portraitAngle);
    if (position != CameraPosition::front || !std::isfinite(portraitAngle)) return {own, 0, false, 0};
    // An imitated camera is at 90 once its connection has turned it; a camera at 90 is turned half around to state 270, the
    // front cameras' own angle, and one at 270 is there already.
    if (own == 0 || own == 180) return {frontCameraOrientation, (own + 270) % 360, true, 180};
    return {frontCameraOrientation, 0, false, own == 90 ? 180u : 0u};
}

CameraRegion regionBeforeTurn(const CameraRegion& region, std::uint32_t turn) noexcept
{
    CameraRegion before = region;
    switch (turn) {
    case 90:
        // The point (x, y) of the picture is (1 - y, x) of the turned one.
        before.x = region.y;
        before.y = 1.0f - region.x - region.width;
        before.width = region.height;
        before.height = region.width;
        break;
    case 180:
        before.x = 1.0f - region.x - region.width;
        before.y = 1.0f - region.y - region.height;
        break;
    case 270:
        // The point (x, y) of the picture is (y, 1 - x) of the turned one.
        before.x = 1.0f - region.y - region.height;
        before.y = region.x;
        before.width = region.height;
        before.height = region.width;
        break;
    default:
        break;
    }
    return before;
}

CompensationStep stillCompensationStep(bool imitatesEarlierFront, CameraFact supported, CameraFact enabled,
                                       CameraReading reading) noexcept
{
    if (!imitatesEarlierFront || reading != CameraReading::afterCommit) return CompensationStep::proceed;
    if (supported == CameraFact::yes && enabled == CameraFact::yes) return CompensationStep::proceed;
    if (supported == CameraFact::yes && enabled == CameraFact::no) return CompensationStep::enable;
    return CompensationStep::refuse;
}

RatioStillStep ratioStillStep(CameraAspectRatio pinned, CameraAspectRatio now, bool completionPending,
                              std::int32_t completionError) noexcept
{
    if (pinned == CameraAspectRatio::none) return RatioStillStep::proceed;
    if (completionPending) return RatioStillStep::wait;
    if (completionError != 0) return RatioStillStep::refuseFailed;
    return now == pinned ? RatioStillStep::proceed : RatioStillStep::refuseMoved;
}

void ratioPinAsked(RatioPin& pin, std::uint64_t operation) noexcept
{
    pin = {};
    pin.asked = true;
    pin.pending = true;
    pin.operation = operation;
}

bool ratioPinCompleted(RatioPin& pin, std::uint64_t operation, std::int32_t error, RatioBoundary boundary,
                       std::int64_t notBefore) noexcept
{
    if (!pin.asked || pin.operation != operation) return false;
    pin.pending = false;
    pin.error = error;
    pin.boundary = boundary;
    pin.notBefore = notBefore;
    return true;
}

bool ratioPinArmed(RatioPin& pin, std::uint64_t now) noexcept
{
    if (!pin.asked || !pin.pending || pin.armedAt != 0) return false;
    pin.armedAt = now == 0 ? 1 : now;
    return true;
}

bool ratioPinTimedOut(RatioPin& pin, std::uint64_t now) noexcept
{
    if (!pin.asked || !pin.pending || pin.timedOut || pin.armedAt == 0) return false;
    if (now < pin.armedAt || now - pin.armedAt < ratioCompletionBudgetNanoseconds) return false;
    pin.timedOut = true;
    return true;
}

RatioFrameStep ratioFrameStep(const RatioPin& pin) noexcept
{
    if (!pin.asked) return RatioFrameStep::deliver;
    if (pin.timedOut) return RatioFrameStep::timedOut;
    if (pin.pending) return RatioFrameStep::hold;
    return pin.error != 0 ? RatioFrameStep::failed : RatioFrameStep::deliver;
}

std::int32_t ratioPinErrorForStill(const RatioPin& pin) noexcept
{
    return pin.timedOut ? ratioNoCompletion : pin.error;
}

bool ratioSampleDelivered(RatioPin& pin, std::optional<std::int64_t> captureNanoseconds, bool shapeIsAnchors) noexcept
{
    if (ratioFrameStep(pin) != RatioFrameStep::deliver) return false;
    if (!pin.asked) return true;
    const bool byShape = pin.boundary == RatioBoundary::shape || (pin.boundary == RatioBoundary::atTime && !captureNanoseconds);
    if (pin.boundary == RatioBoundary::none) return true;
    if (!byShape) return *captureNanoseconds >= pin.notBefore;
    if (pin.shapeSeen) return true;
    if (!shapeIsAnchors) return false;
    pin.shapeSeen = true;
    return true;
}

std::optional<std::int32_t> ratioNoteCode(const std::optional<RatioPin>& pin) noexcept
{
    if (!pin || !pin->asked) return std::nullopt;
    return pin->pending || pin->timedOut ? ratioPending : pin->error;
}

void RatioPins::asked(std::size_t camera, std::uint64_t operation)
{
    ratioPinAsked(pins_[camera], operation);
}

RatioPins::Completion RatioPins::completed(std::size_t camera, std::uint64_t operation, std::int32_t error, RatioBoundary boundary,
                                           std::int64_t notBefore)
{
    const auto found = pins_.find(camera);
    if (found == pins_.end()) return Completion::ignored;
    RatioPin& pin = found->second;
    const bool wasTimedOut = pin.timedOut;
    if (!ratioPinCompleted(pin, operation, error, boundary, notBefore)) return Completion::ignored;
    if (wasTimedOut) return Completion::afterTimeout;
    return error != 0 ? Completion::failed : Completion::succeeded;
}

std::optional<std::uint64_t> RatioPins::armed(std::size_t camera, std::uint64_t now)
{
    const auto found = pins_.find(camera);
    if (found == pins_.end() || !ratioPinArmed(found->second, now)) return std::nullopt;
    return found->second.operation;
}

bool RatioPins::timedOut(std::size_t camera, std::uint64_t operation, std::uint64_t now)
{
    const auto found = pins_.find(camera);
    if (found == pins_.end() || found->second.operation != operation) return false;
    return ratioPinTimedOut(found->second, now);
}

void RatioPins::invalidate(std::size_t camera)
{
    pins_.erase(camera);
}

std::optional<RatioPin> RatioPins::find(std::size_t camera) const
{
    const auto found = pins_.find(camera);
    if (found == pins_.end()) return std::nullopt;
    return found->second;
}

bool RatioPins::sampleDelivered(std::size_t camera, std::optional<std::int64_t> captureNanoseconds, bool shapeIsAnchors)
{
    const auto found = pins_.find(camera);
    return found == pins_.end() || ratioSampleDelivered(found->second, captureNanoseconds, shapeIsAnchors);
}

bool RatioPins::holdsFrames(std::size_t camera) const
{
    const auto found = pins_.find(camera);
    return found != pins_.end() && ratioFrameStep(found->second) != RatioFrameStep::deliver;
}

std::vector<std::string> heldNotesToDrop(const std::vector<HeldNoteUse>& held, const std::vector<std::string>& current,
                                         std::size_t recent)
{
    std::vector<const HeldNoteUse*> others;
    for (const auto& note : held)
        if (std::find(current.begin(), current.end(), note.identifier) == current.end()) others.push_back(&note);
    std::sort(others.begin(), others.end(), [](const HeldNoteUse* a, const HeldNoteUse* b) {
        return a->used != b->used ? a->used > b->used : a->identifier < b->identifier;
    });
    std::vector<std::string> dropped;
    for (std::size_t index = recent; index < others.size(); ++index) dropped.push_back(others[index]->identifier);
    return dropped;
}

std::optional<VideoFormatFacts> presentedFormatOf(const VideoFormatFacts& listed, std::uint32_t connectionTurn)
{
    if (listed.aspectRatios == 0) return listed;
    const CameraAspectRatio needed =
        connectionTurn % 180 == 90 ? CameraAspectRatio::r3x4 : CameraAspectRatio::r4x3;
    if ((listed.aspectRatios & aspectRatioBit(needed)) == 0) return std::nullopt;
    if (listed.width == 0 || listed.width != listed.height || listed.width % 4 != 0) return std::nullopt;
    VideoFormatFacts presented = listed;
    presented.height = listed.width / 4 * 3;
    presented.pinnedRatio = needed;
    presented.photoSizes.clear();
    for (const auto& size : listed.photoSizes)
        if (size.width > size.height && std::uint64_t(size.width) * 3 == std::uint64_t(size.height) * 4)
            presented.photoSizes.push_back(size);
    return presented;
}

float pinnedFieldOfView(float portraitCrop, float landscapeCrop) noexcept
{
    return std::max(portraitCrop, landscapeCrop);
}

bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept
{
    return (resolved.width == 0 && resolved.height == 0) || resolved == requested;
}

bool photoMaximumNeedsFullResolution(CameraStillSize maximum) noexcept
{
    return std::uint64_t(maximum.width) * maximum.height > binnedSensorStillPixels;
}

std::uint64_t photoMaximumCommittedAt(std::optional<CameraStillSize> maximum, std::uint64_t now) noexcept
{
    return maximum && photoMaximumNeedsFullResolution(*maximum) ? now : 0;
}

std::uint64_t stillSettleNanoseconds(std::uint64_t committedAt, std::uint64_t now) noexcept
{
    if (committedAt == 0) return 0;
    const std::uint64_t elapsed = now > committedAt ? now - committedAt : 0;
    return elapsed >= photoOutputSettleNanoseconds ? 0 : photoOutputSettleNanoseconds - elapsed;
}

ScheduledStill scheduledStillStep(bool sessionEnded, bool requestKnown, std::uint64_t committedAt, std::uint64_t now,
                                  std::uint64_t waitedNanoseconds) noexcept
{
    return scheduledStillStep(sessionEnded, requestKnown, committedAt, now, waitedNanoseconds, false);
}

ScheduledStill scheduledStillStep(bool sessionEnded, bool requestKnown, std::uint64_t committedAt, std::uint64_t now,
                                  std::uint64_t waitedNanoseconds, bool ratioPending) noexcept
{
    if (sessionEnded || !requestKnown) return {ScheduledStillStep::drop, 0, waitedNanoseconds};
    std::uint64_t wait = stillSettleNanoseconds(committedAt, now);
    if (ratioPending) wait = std::max(wait, ratioPinPollNanoseconds);
    if (wait == 0) return {ScheduledStillStep::fire, 0, waitedNanoseconds};
    if (waitedNanoseconds >= stillWaitBudgetNanoseconds || wait > stillWaitBudgetNanoseconds - waitedNanoseconds)
        return {ScheduledStillStep::fail, 0, waitedNanoseconds};
    return {ScheduledStillStep::wait, wait, waitedNanoseconds + wait};
}

StillSizeOutcome stillSizeOutcome(CameraStillSize resolved, CameraStillSize requested, unsigned attempt) noexcept
{
    if (resolvedSizeIsRequested(resolved, requested)) return StillSizeOutcome::deliver;
    return attempt < stillAttemptLimit ? StillSizeOutcome::retry : StillSizeOutcome::fail;
}

bool takeJpegFile(std::span<const std::uint8_t> file, std::uint64_t maximumBytes, bool wanted, std::vector<std::uint8_t>& out)
{
    if (!wanted || (maximumBytes != 0 && file.size() > maximumBytes)) return false;
    out.assign(file.begin(), file.end());
    return true;
}

FrameSize deliveredFrameSize(std::uint32_t formatWidth, std::uint32_t formatHeight, std::uint32_t width,
                             std::uint32_t height) noexcept
{
    if (formatWidth == 0 || formatHeight == 0 || width == 0 || height == 0) return {formatWidth, formatHeight};
    // Sizes of the format's aspect are the multiples of its reduced ratio, and
    // an exact ratio is the only scaling the platform accepts.
    const std::uint32_t common = std::gcd(formatWidth, formatHeight);
    const std::uint32_t ratioWidth = formatWidth / common, ratioHeight = formatHeight / common;
    std::uint64_t multiple = std::max((std::uint64_t(width) + ratioWidth - 1) / ratioWidth,
                                      (std::uint64_t(height) + ratioHeight - 1) / ratioHeight);
    // Both sides stay even (a video frame has chroma pairs): a ratio with an
    // odd side wants an even multiple.
    if (((ratioWidth | ratioHeight) & 1u) != 0 && (multiple & 1u) != 0) ++multiple;
    if (multiple >= common) return {formatWidth, formatHeight};
    return {std::uint32_t(multiple * ratioWidth), std::uint32_t(multiple * ratioHeight)};
}

std::uint64_t deliveredFrameBytes(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t rowAlignment) noexcept
{
    const FrameSize frame = deliveredFrameSize(format.width, format.height, width, height);
    return alignedFrameBytes(frame.width, frame.height, rowAlignment);
}

OutputSizes outputSizesFor(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height,
                           std::uint64_t maximumStillPixels)
{
    OutputSizes sizes;
    if (width != 0 && height != 0) {
        const FrameSize delivered = deliveredFrameSize(format.width, format.height, width, height);
        if (delivered.width != format.width || delivered.height != format.height) sizes.frame = delivered;
    }
    sizes.photoMaximum = photoMaximum(format, maximumStillPixels);
    return sizes;
}

std::optional<WholeRates> wholeRatesOf(const CameraRateSpan& span) noexcept
{
    if (!(span.minimum <= span.maximum) || !(span.maximum >= 0.5)) return std::nullopt;
    const auto low = snappedRate(span.minimum);
    const auto high = snappedRate(span.maximum);
    double minimum = low ? *low : std::ceil(span.minimum);
    double maximum = high ? *high : std::floor(span.maximum);
    minimum = std::max(minimum, 1.0);
    maximum = std::min(maximum, largestRate);
    if (minimum > maximum) return std::nullopt;
    return WholeRates {std::uint32_t(minimum), std::uint32_t(maximum)};
}

std::vector<WholeRates> offeredRatesOf(const VideoFormatFacts& format, std::uint32_t rateCap)
{
    const std::uint32_t cap = rateCap != 0 ? rateCap : std::numeric_limits<std::uint32_t>::max();
    std::vector<WholeRates> capped;
    bool any = false;
    std::uint32_t slowest = std::numeric_limits<std::uint32_t>::max();
    for (const auto& span : format.frameRates) {
        const auto whole = wholeRatesOf(span);
        if (!whole) continue;
        any = true;
        slowest = std::min(slowest, whole->minimum);
        if (whole->minimum <= cap) capped.push_back({whole->minimum, std::min(whole->maximum, cap)});
    }
    if (!any) return {};
    if (capped.empty()) return {{slowest, slowest}};
    std::sort(capped.begin(), capped.end(), [](const WholeRates& a, const WholeRates& b) { return a.minimum < b.minimum; });
    std::vector<WholeRates> merged;
    for (const auto& range : capped) {
        if (!merged.empty() && range.minimum <= merged.back().maximum + 1)
            merged.back().maximum = std::max(merged.back().maximum, range.maximum);
        else
            merged.push_back(range);
    }
    return merged;
}

std::optional<RateSetting> rateSettingFor(const VideoFormatFacts& format, std::uint32_t rate) noexcept
{
    // A span that holds the rate exactly first: the nominal duration is then the
    // span's own.
    for (std::size_t index = 0; index < format.frameRates.size(); ++index) {
        const auto& span = format.frameRates[index];
        if (span.minimum <= double(rate) && double(rate) <= span.maximum && wholeRatesOf(span))
            return RateSetting {index, FrameDuration::reciprocal};
    }
    for (std::size_t index = 0; index < format.frameRates.size(); ++index) {
        const auto& span = format.frameRates[index];
        const auto whole = wholeRatesOf(span);
        if (!whole || rate < whole->minimum || rate > whole->maximum) continue;
        return RateSetting {index, double(rate) > span.maximum ? FrameDuration::spanShortest : FrameDuration::spanLongest};
    }
    return std::nullopt;
}

double programmedFrameDuration(const CameraRateSpan& span, FrameDuration duration, std::uint32_t rate) noexcept
{
    switch (duration) {
    case FrameDuration::reciprocal: return rate != 0 ? 1.0 / double(rate) : 0.0;
    case FrameDuration::spanShortest: return span.maximum > 0 ? 1.0 / span.maximum : 0.0;
    case FrameDuration::spanLongest: return span.minimum > 0 ? 1.0 / span.minimum : 0.0;
    }
    return 0.0;
}

std::uint32_t throttledRate(std::span<const WholeRates> offered, std::uint32_t requested, std::uint32_t pressureCap) noexcept
{
    if (offered.empty()) return 0;
    const std::uint32_t limit = pressureCap != 0 && pressureCap < requested ? pressureCap : requested;
    std::uint32_t best = 0;
    std::uint32_t slowest = std::numeric_limits<std::uint32_t>::max();
    for (const auto& range : offered) {
        slowest = std::min(slowest, range.minimum);
        if (range.minimum <= limit) best = std::max(best, std::min(range.maximum, limit));
    }
    return best != 0 ? best : slowest;
}

std::optional<ProgrammedRate> programmedRate(const VideoFormatFacts& format, std::span<const WholeRates> offered,
                                             std::uint32_t requested, std::uint32_t pressureCap)
{
    const std::uint32_t rate = throttledRate(offered, requested, pressureCap);
    if (rate == 0) return std::nullopt;
    const auto setting = rateSettingFor(format, rate);
    if (!setting) return std::nullopt;
    return ProgrammedRate {rate, *setting};
}

PressureWatch::PressureWatch(VideoFormatFacts format, std::vector<WholeRates> offered)
    : format_(std::move(format)), offered_(std::move(offered))
{
}

void PressureWatch::request(std::uint32_t rate) noexcept
{
    requested_ = rate;
}

std::uint32_t PressureWatch::requested() const noexcept
{
    return requested_;
}

std::uint32_t PressureWatch::cap() const noexcept
{
    return cap_;
}

bool PressureWatch::pressure(std::uint32_t cap) noexcept
{
    return cap_.exchange(cap) != cap && live_;
}

std::optional<ProgrammedRate> PressureWatch::programmed() const
{
    if (!live_) return std::nullopt;
    return programmedRate(format_, offered_, requested_, cap_);
}

bool PressureWatch::live() const noexcept
{
    return live_;
}

void PressureWatch::end() noexcept
{
    live_ = false;
    cap_ = 0;
}

namespace {

// The sizes a format's video is cut from and how far they reach: those of the
// candidates that fit inside the format and, taken largest first, keep the
// frame the format delivers for their box within the limits' bound.
struct VideoBox {
    std::vector<FrameSize> sizes;
    FrameSize box;
};

VideoBox boxOf(const VideoFormatFacts& format, const OfferRules& rules)
{
    std::vector<FrameSize> candidates;
    for (const auto& size : rules.candidateSizes) {
        if (size.width < 16 || size.height < 16 || (size.width & 1u) != 0 || (size.height & 1u) != 0) continue;
        if (rules.maximumVideoWidth != 0 && size.width > rules.maximumVideoWidth) continue;
        if (rules.maximumVideoHeight != 0 && size.height > rules.maximumVideoHeight) continue;
        if (std::find(candidates.begin(), candidates.end(), size) == candidates.end()) candidates.push_back(size);
    }
    std::sort(candidates.begin(), candidates.end(), [](const FrameSize& a, const FrameSize& b) {
        const std::uint64_t areaA = std::uint64_t(a.width) * a.height, areaB = std::uint64_t(b.width) * b.height;
        return areaA != areaB ? areaA > areaB : a.width > b.width;
    });
    VideoBox result;
    for (const auto& size : candidates) {
        if (size.width > format.width || size.height > format.height) continue;
        const FrameSize grown {std::max(result.box.width, size.width), std::max(result.box.height, size.height)};
        if (rules.limits.maximumFrameBytes != 0 &&
            deliveredFrameBytes(format, grown.width, grown.height, rules.limits.frameRowAlignment) > rules.limits.maximumFrameBytes)
            continue;
        result.sizes.push_back(size);
        result.box = grown;
    }
    return result;
}

} // namespace

CaptureOffer offerOf(const VideoFormatFacts& format, const OfferRules& rules)
{
    CaptureOffer offer;
    offer.stills = stillsOf(format, rules.limits.maximumStillPixels);
    auto video = boxOf(format, rules);
    offer.videoSizes = std::move(video.sizes);
    offer.box = video.box;
    offer.rates = offeredRatesOf(format, rules.rateCap);
    return offer;
}

std::optional<AnchorKey> anchorKeyOf(const VideoFormatFacts& format, const OfferRules& rules)
{
    const CaptureOffer offer = offerOf(format, rules);
    if (offer.rates.empty() || offer.videoSizes.empty()) return std::nullopt;
    const std::uint32_t cap = rules.rateCap != 0 ? rules.rateCap : std::numeric_limits<std::uint32_t>::max();
    AnchorKey key;
    for (const auto& range : offer.rates)
        if (range.minimum <= cap) {
            key.hasCappedRate = true;
            key.rate = std::max(key.rate, range.maximum);
        }
    if (!offer.stills.empty()) {
        key.stillArea = areaOf(offer.stills.front());
        key.stillWidth = offer.stills.front().width;
    }
    key.boxArea = std::uint64_t(offer.box.width) * offer.box.height;
    key.negativeCost = -std::int64_t(std::uint64_t(format.width) * format.height);
    key.negativeMinimumRate = -std::int64_t(offer.rates.front().minimum);
    key.fullRange = format.fullRange;
    return key;
}

std::optional<std::size_t> chooseAnchor(std::span<const VideoFormatFacts> formats, const OfferRules& rules, bool multiCameraOnly)
{
    std::optional<std::size_t> best;
    std::optional<AnchorKey> bestKey;
    for (std::size_t index = 0; index < formats.size(); ++index) {
        if (multiCameraOnly && !formats[index].multiCamera) continue;
        const auto key = anchorKeyOf(formats[index], rules);
        if (!key) continue;
        if (!bestKey || *key > *bestKey) {
            best = index;
            bestKey = key;
        }
    }
    return best;
}

bool keepsConcurrency(std::span<const VideoFormatFacts> formats, const OfferRules& rules)
{
    const auto multi = chooseAnchor(formats, rules, true);
    const auto any = chooseAnchor(formats, rules, false);
    if (!multi || !any) return false;
    return anchorKeyOf(formats[*multi], rules)->offersAsMuchAs(*anchorKeyOf(formats[*any], rules));
}

bool mayJoin(bool otherInputsInSession, bool ownAnchorMultiCamera, std::span<const bool> otherAnchorsMultiCamera) noexcept
{
    if (!otherInputsInSession) return true;
    return ownAnchorMultiCamera && std::all_of(otherAnchorsMultiCamera.begin(), otherAnchorsMultiCamera.end(), [](bool multi) { return multi; });
}

Admission admit(const AdmitPlan& plan, const AdmitSteps& steps)
{
    bool inputAdded = false, videoTried = false, photoTried = false, timingTaken = false;
    steps.begin();
    // What this call added leaves again, and the device's timing it changed comes
    // back, the newest first: the outputs, the input (adding it can reset the
    // device's durations, and a device that is no longer in the session can be set
    // to the format it was in), and the timing. A step that added nothing (it
    // refused before it added) leaves nothing to remove.
    const auto undo = [&] {
        if (photoTried) steps.removePhoto();
        if (videoTried) steps.removeVideo();
        if (inputAdded) steps.removeInput();
        if (timingTaken && steps.restoreTiming) steps.restoreTiming();
    };
    const auto refuse = [&](Admission reason) {
        undo();
        steps.commit();
        return reason;
    };
    // Before anything changes the device: adding the input can reset its durations.
    if (plan.activate && steps.snapshotTiming) {
        steps.snapshotTiming();
        timingTaken = true;
    }
    if (plan.addInput) {
        if (!steps.addInput()) return refuse(Admission::inputRefused);
        inputAdded = true;
    }
    if (plan.activate && !steps.activate()) return refuse(Admission::formatRefused);
    if (plan.addVideo) {
        videoTried = true;
        if (!steps.addVideo()) return refuse(Admission::videoRefused);
        if (steps.programVideo) steps.programVideo();
    }
    if (plan.addPhoto) {
        photoTried = true;
        if (!steps.addPhoto()) return refuse(Admission::photoRefused);
    }
    steps.commit();
    if (steps.overBudget && steps.overBudget()) {
        steps.begin();
        undo();
        steps.commit();
        return Admission::overBudget;
    }
    return Admission::admitted;
}

bool maximumFirst(CameraTime currentMaximum, CameraTime target) noexcept
{
    return target.seconds() > currentMaximum.seconds();
}

bool putBackTiming(const DeviceTiming& held, const TimingSteps& steps)
{
    if (!steps.lock()) return false;
    bool ok = true;
    DeviceTiming now = steps.read();
    if (now.format != held.format) {
        ok = steps.setFormat(held.format);
        if (ok) now = steps.read();
    }
    if (ok) {
        // The one that moves out of the other's way goes first.
        if (maximumFirst(now.maximum, held.minimum)) ok = steps.setMaximum(held.maximum) && steps.setMinimum(held.minimum);
        else ok = steps.setMinimum(held.minimum) && steps.setMaximum(held.maximum);
    }
    // Shortening the maximum shortened a longer custom exposure with it.
    if (ok && held.exposure.custom && steps.read().exposure != held.exposure)
        ok = steps.setCustomExposure(held.exposure.duration, held.exposure.iso);
    steps.unlock();
    return ok;
}

Resizing resizeStream(const ResizeSteps& steps)
{
    steps.begin();
    if (steps.setDurations && steps.snapshotTiming) steps.snapshotTiming();
    if (steps.setDurations && !steps.setDurations()) {
        if (steps.restoreTiming) steps.restoreTiming();
        steps.commit();
        return Resizing::durationsRefused;
    }
    if (steps.setVideoSize && !steps.setVideoSize()) {
        if (steps.setDurations && steps.restoreTiming) steps.restoreTiming();
        steps.commit();
        return Resizing::sizeRefused;
    }
    steps.commit();
    return Resizing::resized;
}

void RepairBoundary::repaired(std::optional<std::int64_t> captureClockNanoseconds) noexcept
{
    repairedAt_ = captureClockNanoseconds;
    generation_.fetch_add(1, std::memory_order_release);
}

bool RepairBoundary::predates(Generation callbackEntered, std::optional<std::int64_t> capturedAt) const noexcept
{
    if (callbackEntered != generation_.load(std::memory_order_acquire)) return true;
    return repairedAt_ && capturedAt && *capturedAt < *repairedAt_;
}

StreamCache::Path StreamCache::pathFor(std::uint32_t askedWidth, std::uint32_t askedHeight, std::uint32_t askedRate) const noexcept
{
    if (!video) return Path::admit;
    if (!unrestored && askedWidth == width && askedHeight == height && askedRate == frameRate) return Path::cached;
    return Path::resize;
}

bool StreamCache::rateChanges(std::uint32_t askedRate) const noexcept
{
    return unrestored || askedRate != frameRate;
}

bool StreamCache::sizeChanges(std::uint32_t askedWidth, std::uint32_t askedHeight) const noexcept
{
    return unrestored || askedWidth != width || askedHeight != height;
}

bool StreamCache::mayLeaveSettings(bool force, bool unchanged) const noexcept
{
    return !force && !unrestored && unchanged;
}

bool StreamCache::timingWritten() noexcept
{
    const bool was = unrestored;
    unrestored = false;
    return was;
}

Recovery recoverAnchors(std::span<const std::size_t> cameras, const RecoverySteps& steps)
{
    Recovery outcome = Recovery::resumed;
    for (const std::size_t camera : cameras) {
        if (!steps.outOfAnchor(camera)) continue;
        if (!steps.repair(camera)) {
            outcome = Recovery::failed;
            break;
        }
    }
    steps.announce(outcome);
    return outcome;
}

Recovery RecoveryLatch::recover(std::span<const std::size_t> cameras, const RecoverySteps& steps)
{
    if (failed_.load()) return Recovery::failed;
    const Recovery outcome = recoverAnchors(cameras, steps);
    if (outcome == Recovery::failed) failed_ = true;
    return outcome;
}

std::string_view refusalName(Refusal reason) noexcept
{
    switch (reason) {
    case Refusal::noAnchor: return "no anchor format";
    case Refusal::formatChanged: return "the device's formats changed";
    case Refusal::notJoinable: return "the cameras cannot share a multi-camera session";
    case Refusal::inputRefused: return "the session took no input";
    case Refusal::outputRefused: return "the session took no output";
    case Refusal::overBudget: return "over the multi-camera hardware budget";
    case Refusal::stillNotMade: return "the anchor makes no such photo";
    case Refusal::rawUnavailable: return "no Bayer RAW format";
    case Refusal::raised: return "the platform raised";
    case Refusal::notRestored: return "the device could not be put back as it was";
    case Refusal::ratioMoved: return "the device's aspect ratio is not the one pinned";
    case Refusal::turnNotHeld: return "the video connection does not hold the turn";
    case Refusal::stillsNotTurned: return "the photo output does not turn its stills";
    case Refusal::ratioFailed: return "the device's aspect ratio change failed";
    case Refusal::ratioTimedOut: return "the device's aspect ratio change did not complete in time";
    case Refusal::stillNotTurned: return "the still cannot be turned half around";
    }
    return "unknown";
}

bool RefusalLog::first(std::size_t camera, Refusal reason)
{
    std::lock_guard lock(mutex_);
    std::uint32_t& seen = seen_[camera];
    const std::uint32_t bit = 1u << static_cast<unsigned>(reason);
    if ((seen & bit) != 0) return false;
    seen |= bit;
    return true;
}

void RefusalLog::reset(std::size_t camera)
{
    std::lock_guard lock(mutex_);
    seen_.erase(camera);
}

void RefusalLog::clear()
{
    std::lock_guard lock(mutex_);
    seen_.clear();
}

} // namespace uml::platform
