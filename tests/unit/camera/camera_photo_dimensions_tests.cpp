// SPDX-License-Identifier: GPL-2.0-or-later
// Which photo size a camera is asked for, and whether the photo is that size,
// on tables of the shape the platform lists per format.
#include "camera_still_failure.hpp"
#include "photo_dimensions.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

using Sizes = std::vector<CameraStillSize>;

// A back camera's active format as the photo output lists it: 12 MP, 24 MP and
// 48 MP, and a binned 3 MP.
const Sizes backFormat {{4032, 3024}, {5712, 4284}, {8064, 6048}, {2016, 1512}};
// A high-frame-rate format of the same camera, which lists no 48 MP.
const Sizes fastFormat {{4032, 3024}, {2016, 1512}};

void anExactSizeTheFormatListsIsAsked()
{
    const CameraStillSize maximum {8064, 6048};
    for (const auto& size : backFormat) {
        const auto chosen = exactPhotoSize(backFormat, size, maximum);
        require(chosen && *chosen == size, "every listed size is asked for as it is");
    }
}

void aSizeAnotherFormatListsIsNotAsked()
{
    // The union of the two formats is what discovery advertises; the fast
    // format cannot make the 48 MP the union has.
    const CameraStillSize maximum {4032, 3024};
    require(!exactPhotoSize(fastFormat, {8064, 6048}, {8064, 6048}).has_value(), "the active format lacks the size");
    require(!exactPhotoSize(fastFormat, {5712, 4284}, {8064, 6048}), "nor the 24 MP");
    require(exactPhotoSize(fastFormat, {4032, 3024}, maximum).has_value(), "but it makes the 12 MP");
}

void aLargerSizeThatCoversIsNotTheRequest()
{
    const CameraStillSize maximum {8064, 6048};
    require(!exactPhotoSize(backFormat, {4000, 3000}, maximum), "4000 by 3000 is not 4032 by 3024");
    require(!exactPhotoSize(backFormat, {4032, 3000}, maximum), "one side that differs");
    require(!exactPhotoSize(backFormat, {3024, 4032}, maximum), "a portrait picture is not the listed landscape one");
    require(!exactPhotoSize(backFormat, {1, 1}, maximum), "nor does a small one fit a larger size");
}

void theOutputsMaximumBoundsTheRequest()
{
    require(!exactPhotoSize(backFormat, {8064, 6048}, {4032, 3024}), "an output that makes up to 12 MP cannot make 48 MP");
    require(exactPhotoSize(backFormat, {4032, 3024}, {4032, 3024}).has_value(), "and makes its own maximum");
    require(!exactPhotoSize(backFormat, {4032, 3024}, {}), "an output that states no maximum makes nothing");
}

void anEmptyOrZeroSizedTableAsksForNothing()
{
    require(!exactPhotoSize({}, {4032, 3024}, {8064, 6048}), "no sizes listed");
    require(!exactPhotoSize(backFormat, {}, {8064, 6048}), "a request of no size");
    const Sizes zeroes {{0, 0}, {4032, 0}, {0, 3024}};
    require(!exactPhotoSize(zeroes, {0, 0}, {8064, 6048}) && !exactPhotoSize(zeroes, {4032, 0}, {8064, 6048}),
            "a listed size of zero is no size");
    require(!coveringPhotoSize(zeroes, {}, {8064, 6048}), "and covers nothing");
}

void aCaptureWithoutAJpegTakesTheSmallestSize()
{
    const auto smallest = coveringPhotoSize(backFormat, {}, {8064, 6048});
    require(smallest && *smallest == CameraStillSize(2016, 1512), "the smallest listed size, as the output defaults to");
    const auto covering = coveringPhotoSize(backFormat, {3000, 2000}, {8064, 6048});
    require(covering && *covering == CameraStillSize(4032, 3024), "the smallest that covers a minimum");
    require(!coveringPhotoSize(backFormat, {9000, 7000}, {8064, 6048}), "none covers a larger minimum");
    require(!coveringPhotoSize(backFormat, {5000, 4000}, {4032, 3024}), "and the output's maximum bounds the choice");
    require(!coveringPhotoSize({}, {}, {8064, 6048}), "an empty table");
}

void theResolvedSizeIsTheRequestOrItIsRefused()
{
    require(resolvedSizeIsRequested({4032, 3024}, {4032, 3024}), "the size asked for");
    require(!resolvedSizeIsRequested({2016, 1512}, {4032, 3024}), "a smaller photo than the one configured");
    require(!resolvedSizeIsRequested({8064, 6048}, {4032, 3024}), "a larger one");
    require(!resolvedSizeIsRequested({3024, 4032}, {4032, 3024}), "a turned one");
    require(resolvedSizeIsRequested({0, 0}, {4032, 3024}), "the platform stated no size (a Bayer still alone)");
    require(!resolvedSizeIsRequested({4032, 0}, {4032, 3024}), "half a size is a size that differs");
}

// The captures a still takes, as the backend runs them: one more for each
// outcome of `retry`, and the last outcome is the capture's end. `served` is
// the size each capture was made at, in order; the last one repeats.
StillSizeOutcome runStill(const std::vector<CameraStillSize>& served, CameraStillSize requested, unsigned& captures)
{
    captures = 0;
    for (;;) {
        const CameraStillSize resolved = served[std::min<std::size_t>(captures, served.size() - 1)];
        ++captures;
        const StillSizeOutcome outcome = stillSizeOutcome(resolved, requested, captures);
        if (outcome != StillSizeOutcome::retry) return outcome;
    }
}

void aPhotoOfAnotherSizeIsMadeAgainOnceAndThenFails()
{
    constexpr CameraStillSize full {8064, 6048};
    constexpr CameraStillSize served {4032, 3024};
    require(stillSizeOutcome(full, full, 1) == StillSizeOutcome::deliver, "the size asked for is delivered at once");
    require(stillSizeOutcome(full, full, stillAttemptLimit) == StillSizeOutcome::deliver, "and on the last attempt");
    require(stillSizeOutcome(served, full, 1) == StillSizeOutcome::retry, "a photo served at the size from before is made again");
    require(stillSizeOutcome(served, full, stillAttemptLimit) == StillSizeOutcome::fail, "and on the last attempt it fails");
    require(stillSizeOutcome({0, 0}, full, 1) == StillSizeOutcome::deliver, "no size stated (a Bayer still alone) is not a different size");
    require(stillSizeOutcome({8064, 0}, full, 1) == StillSizeOutcome::retry, "half a size differs");

    unsigned captures = 0;
    require(runStill({full}, full, captures) == StillSizeOutcome::deliver && captures == 1, "a pipeline that has the size: one capture");
    // The first capture after the output's maximum was raised is served at the old size.
    require(runStill({served, full}, full, captures) == StillSizeOutcome::deliver && captures == 2,
            "a first capture at the old size: the second is the photo");
    // A configuration that cannot make the size fails the capture after the attempts, never by waiting for it.
    require(runStill({served}, full, captures) == StillSizeOutcome::fail && captures == stillAttemptLimit,
            "a size that is never made fails after the attempt limit");
    require(stillAttemptLimit >= 2, "one retry at least");
}

void aFullResolutionMaximumIsGivenTimeToTakeEffectBeforeTheFirstStill()
{
    require(photoMaximumNeedsFullResolution({8064, 6048}), "48 MP needs the sensor's full-resolution mode");
    require(photoMaximumNeedsFullResolution({5712, 4284}), "so does 24 MP");
    require(!photoMaximumNeedsFullResolution({4032, 3024}), "the binned 12 MP is served from the first capture, as at the small tier");
    require(!photoMaximumNeedsFullResolution({1920, 1440}), "so is a smaller one");
    require(!photoMaximumNeedsFullResolution({0, 0}), "no size needs nothing");
    // A product that does not overflow 32 bits.
    require(photoMaximumNeedsFullResolution({65535, 65536}), "wide sizes compare in 64 bits");

    constexpr std::uint64_t settle = photoOutputSettleNanoseconds;
    constexpr std::uint64_t raisedAt = 5'000'000'000;
    require(stillSettleNanoseconds(0, raisedAt) == 0, "a maximum that was not raised waits for nothing");
    require(stillSettleNanoseconds(raisedAt, raisedAt) == settle, "a still at the commit waits the whole time");
    require(stillSettleNanoseconds(raisedAt, raisedAt + settle / 3) == settle - settle / 3, "and the rest of it after a part");
    require(stillSettleNanoseconds(raisedAt, raisedAt + settle) == 0, "none once it has passed");
    require(stillSettleNanoseconds(raisedAt, raisedAt + 10 * settle) == 0, "nor long after");
    require(stillSettleNanoseconds(raisedAt, raisedAt - 1) == settle, "a clock that reads before the commit waits the whole time");
    require(settle > 0 && settle < 1'000'000'000, "a wait the guest does not notice as a stall");

    // What a camera records at admission and at the repair of its anchor, by the same function.
    require(photoMaximumCommittedAt(CameraStillSize {8064, 6048}, 7) == 7, "a full-resolution maximum is stamped with the time");
    require(photoMaximumCommittedAt(CameraStillSize {4032, 3024}, 7) == 0, "the binned size records nothing to wait for");
    require(photoMaximumCommittedAt(std::nullopt, 7) == 0, "a format that lists no size records none");
    // A repair commits again: the stamp moves forward, and the wait with it.
    const std::uint64_t admitted = photoMaximumCommittedAt(CameraStillSize {8064, 6048}, raisedAt);
    const std::uint64_t repaired = photoMaximumCommittedAt(CameraStillSize {8064, 6048}, raisedAt + 5 * settle);
    require(stillSettleNanoseconds(admitted, raisedAt + 5 * settle) == 0, "the admission's time has passed");
    require(stillSettleNanoseconds(repaired, raisedAt + 5 * settle) == settle, "a still right after the repair waits the whole time again");
}

void aScheduledStillFiresWaitsFailsOrIsDropped()
{
    constexpr std::uint64_t committed = 5'000'000'000;
    constexpr std::uint64_t settle = photoOutputSettleNanoseconds;
    using Step = ScheduledStillStep;
    auto step = [](bool ended, bool known, std::uint64_t committedAt, std::uint64_t now, std::uint64_t waited = 0) {
        return scheduledStillStep(ended, known, committedAt, now, waited);
    };
    require(step(false, true, committed, committed + settle).step == Step::fire, "its time has come: it starts");
    require(step(false, true, committed, committed + settle + 1).step == Step::fire, "and after");
    const auto again = step(false, true, committed + 2 * settle, committed + settle);
    require(again.step == Step::wait && again.waitNanoseconds == settle && again.waitedNanoseconds == settle,
            "a commit since: it waits the rest of that, and counts it");
    require(step(true, true, committed, committed + settle).step == Step::drop, "a stop inside the settle time drops it");
    require(step(true, true, committed, committed).step == Step::drop, "also at once");
    require(step(false, false, committed, committed + settle).step == Step::drop, "a request that is gone is not started");
    require(step(true, false, committed, committed + settle).step == Step::drop, "nor when the session ended and it is gone");
    require(step(false, true, 0, committed).step == Step::fire, "a camera with nothing to wait for starts it");
    // What it has waited is kept by every step that waits no more, and by none that ends it differently.
    for (const bool ended : {false, true}) {
        const auto kept = step(ended, true, 0, committed, 123);
        require(kept.waitedNanoseconds == 123 && kept.waitNanoseconds == 0, "a step that does not wait leaves what was waited");
    }
}

// The chain of one scheduled still, as the backend runs it: the decision when the still is accepted, and one at the end
// of each wait, until a step that is not a wait. `commits` are the times the session committed its configuration (a
// photo output admitted, a stream started or resized, another camera's change), the first of them the admission; each
// restarts the settle time. The session ends at `stopAt` (zero: never).
struct ScheduledRun {
    unsigned waits {}, fires {}, fails {}, drops {};
    std::uint64_t endedAt {};
    std::uint64_t waited {};
    unsigned completions() const { return fires + fails; }
};

ScheduledRun runScheduled(std::uint64_t accepted, const std::vector<std::uint64_t>& commits, std::uint64_t stopAt,
                          std::uint64_t waitedBefore = 0)
{
    constexpr std::uint64_t horizon = 100'000'000'000;  // a chain that has not ended by then does not end
    ScheduledRun run;
    run.waited = waitedBefore;
    std::uint64_t now = accepted;
    while (now <= accepted + horizon) {
        std::uint64_t committedAt = 0;
        for (const auto commit : commits)
            if (commit <= now) committedAt = commit;
        const auto decision = scheduledStillStep(stopAt != 0 && now >= stopAt, true, committedAt, now, run.waited);
        run.waited = decision.waitedNanoseconds;
        run.endedAt = now;
        switch (decision.step) {
        case ScheduledStillStep::fire:
            ++run.fires;
            return run;
        case ScheduledStillStep::fail:
            ++run.fails;
            return run;
        case ScheduledStillStep::drop:
            ++run.drops;
            return run;
        case ScheduledStillStep::wait:
            ++run.waits;
            now += decision.waitNanoseconds;
            break;
        }
    }
    return run;
}

std::vector<std::uint64_t> commitsEvery(std::uint64_t first, std::uint64_t period, std::uint64_t until)
{
    std::vector<std::uint64_t> commits;
    for (std::uint64_t time = first; time <= until; time += period) commits.push_back(time);
    return commits;
}

void aStillThatKeepsBeingPostponedFailsOnceWithinItsBudget()
{
    constexpr std::uint64_t ms = 1'000'000, begin = 1'000 * ms;
    // The device's capture timeout, which starts when the still is handed over: the wait ends before it, by far.
    constexpr std::uint64_t deviceCaptureTimeout = 10'000 * ms;
    static_assert(stillWaitBudgetNanoseconds * 4 < deviceCaptureTimeout);

    // The ordinary case: the admission's commit and nothing since: one wait, then it starts.
    auto run = runScheduled(begin, {begin}, 0);
    require(run.waits == 1 && run.fires == 1 && run.fails == 0 && run.drops == 0, "a settled output starts the still after one wait");
    require(run.endedAt == begin + photoOutputSettleNanoseconds && run.waited == photoOutputSettleNanoseconds, "after the settle time");

    // A short chain of ordinary commits (a stream starts, is resized, another camera joins): it starts after the last.
    run = runScheduled(begin, {begin, begin + 250 * ms, begin + 450 * ms, begin + 650 * ms}, 0);
    require(run.fires == 1 && run.fails == 0 && run.completions() == 1, "a few commits only delay it");
    require(run.endedAt == begin + 650 * ms + photoOutputSettleNanoseconds && run.waited <= stillWaitBudgetNanoseconds,
            "it starts 300 ms after the last, within its budget");

    // The reviewer's case: a stream's rate changes and commits every 200 ms, for as long as the still is pending (once
    // that never ended: 204 deferrals over 41 simulated seconds).
    for (const std::uint64_t period : {100 * ms, 200 * ms, 250 * ms, 299 * ms}) {
        run = runScheduled(begin, commitsEvery(begin, period, begin + 20'000 * ms), 0);
        require(run.fails == 1 && run.fires == 0 && run.drops == 0 && run.completions() == 1,
                "commits that do not stop end in one failure, never a start");
        require(run.waits >= 1, "it waited first");
        require(run.waited <= stillWaitBudgetNanoseconds, "it waited no more than its budget");
        require(run.endedAt - begin <= stillWaitBudgetNanoseconds, "and failed within it");
        require(run.endedAt - begin < deviceCaptureTimeout / 4, "well before the device's capture timeout");
    }

    // Commits as often as the settle time never let it start, either.
    run = runScheduled(begin, commitsEvery(begin, photoOutputSettleNanoseconds, begin + 20'000 * ms), 0);
    require(run.fails == 1 && run.fires == 0 && run.endedAt - begin <= stillWaitBudgetNanoseconds, "also at the settle time's own period");

    // The commits end in time: it starts, even late in its budget.
    run = runScheduled(begin, commitsEvery(begin, 200 * ms, begin + 1'400 * ms), 0);
    require(run.fires == 1 && run.fails == 0, "a chain that ends within the budget starts the still");
    require(run.endedAt == begin + 1'400 * ms + photoOutputSettleNanoseconds, "300 ms after its last commit");
}

void aRetryHasWhatIsLeftOfTheWaitBudget()
{
    constexpr std::uint64_t ms = 1'000'000, begin = 1'000 * ms;
    constexpr std::uint64_t budget = stillWaitBudgetNanoseconds;
    // The first attempt waited 1.5 s, and the retry's own commit needs 300 ms: 1.8 s is within the budget.
    auto run = runScheduled(begin, {begin}, 0, budget - 500 * ms);
    require(run.fires == 1 && run.waited == budget - 200 * ms, "the retry waits what is left and starts");
    // Only 200 ms are left and 300 ms are needed: the retry fails at once, without waiting at all.
    run = runScheduled(begin, {begin}, 0, budget - 200 * ms);
    require(run.fails == 1 && run.waits == 0 && run.fires == 0 && run.endedAt == begin, "a wait past the budget fails the still at once");
    require(run.waited == budget - 200 * ms, "and counts nothing for the wait it did not make");
    // The budget used up does not stop a retry that has nothing to wait for.
    run = runScheduled(begin + 10 * photoOutputSettleNanoseconds, {begin}, 0, budget);
    require(run.fires == 1 && run.waits == 0 && run.fails == 0, "a settled output starts the retry whatever it waited");
    // Waits are counted across the attempts: a first attempt that waited 1.2 s and a retry that needs 1.2 s again do not
    // fit in 2 s, though each of them would alone.
    const auto first = runScheduled(begin, commitsEvery(begin, 100 * ms, begin + 900 * ms), 0);
    require(first.fires == 1 && first.waited == 1'200 * ms, "the first attempt starts after its commits");
    const auto commits = commitsEvery(begin + 5'000 * ms, 100 * ms, begin + 5'900 * ms);
    require(runScheduled(begin + 5'000 * ms, commits, 0).fires == 1, "the same commits alone leave a still its start");
    run = runScheduled(begin + 5'000 * ms, commits, 0, first.waited);
    require(run.fails == 1 && run.fires == 0 && run.waited == 1'800 * ms && run.waited <= budget,
            "with the first attempt's waits counted, the retry runs out of what is left");
}

void aStopDuringTheWaitDropsTheStillAndCompletesNothing()
{
    constexpr std::uint64_t ms = 1'000'000, begin = 1'000 * ms;
    // The session ends while commits keep postponing the still: the next step drops it, and nothing more happens.
    auto run = runScheduled(begin, commitsEvery(begin, 200 * ms, begin + 20'000 * ms), begin + 700 * ms);
    require(run.drops == 1 && run.fires == 0 && run.fails == 0 && run.completions() == 0, "a stop drops it: no start, no failure");
    require(run.endedAt >= begin + 700 * ms && run.endedAt - begin < stillWaitBudgetNanoseconds, "at the first step after the stop");
    // A stop before the first step.
    run = runScheduled(begin, {begin}, begin);
    require(run.drops == 1 && run.waits == 0 && run.completions() == 0, "also at once");
    // A stop after the budget has run out is no longer a stop of a pending still: it was failed once, before.
    run = runScheduled(begin, commitsEvery(begin, 200 * ms, begin + 20'000 * ms), begin + 30'000 * ms);
    require(run.fails == 1 && run.drops == 0 && run.completions() == 1, "a late stop finds the still already failed");
}

// A photo output as AVFoundation treats it: its maximum is refused (it raises)
// unless the output is attached to the session with a source whose active
// format lists the size.
struct FakeOutput {
    explicit FakeOutput(Sizes format) : sourceFormat(std::move(format)) {}
    Sizes sourceFormat;
    bool attachable {true};
    bool attached {false};
    std::vector<std::string> calls;
    std::optional<CameraStillSize> maximum;

    PhotoOutputSteps steps()
    {
        return {
            [this] {
                calls.push_back("attach");
                attached = attachable;
                return attached;
            },
            [this](CameraStillSize size) {
                calls.push_back("set " + std::to_string(size.width) + "x" + std::to_string(size.height));
                if (!attached) return false;  // no source device: the setter raises
                for (const auto& listed : sourceFormat)
                    if (listed == size) {
                        maximum = size;
                        return true;
                    }
                return false;
            },
            [this] {
                calls.push_back("detach");
                attached = false;
            },
        };
    }
};

void theOutputIsAttachedBeforeItsMaximumIsSet()
{
    FakeOutput output(backFormat);
    const auto largest = largestPhotoSize(backFormat);
    require(largest && *largest == CameraStillSize(8064, 6048), "the largest by area");
    require(preparePhotoOutput(largest, output.steps()) == PhotoOutputPreparation::ready, "an attached output takes its maximum");
    require(output.calls == std::vector<std::string> {"attach", "set 8064x6048"}, "attach, then set the maximum");
    require(output.attached && output.maximum && *output.maximum == CameraStillSize(8064, 6048), "and keeps it");
}

void aMaximumTheSourceRefusesDetachesTheOutput()
{
    FakeOutput output(fastFormat);
    // A maximum the source's active format does not list: the setter raises.
    require(preparePhotoOutput(CameraStillSize {8064, 6048}, output.steps()) == PhotoOutputPreparation::maximumRefused,
            "the refused maximum is reported");
    require(output.calls == std::vector<std::string> {"attach", "set 8064x6048", "detach"}, "attach, set, then detach");
    require(!output.attached && !output.maximum, "the output is not left on the session");
}

void anOutputThatCannotBeAttachedGetsNoMaximum()
{
    FakeOutput output(backFormat);
    output.attachable = false;
    require(preparePhotoOutput(CameraStillSize {4032, 3024}, output.steps()) == PhotoOutputPreparation::notAttached,
            "the session took no output");
    require(output.calls == std::vector<std::string> {"attach"}, "nothing is set on it and nothing is detached");
}

void aFormatThatListsNothingLeavesTheDefault()
{
    FakeOutput output((Sizes()));
    require(!largestPhotoSize(output.sourceFormat) && !largestPhotoSize(Sizes {{0, 0}, {4032, 0}}), "no usable size");
    require(preparePhotoOutput(std::nullopt, output.steps()) == PhotoOutputPreparation::ready, "the output stands as it is");
    require(output.calls == std::vector<std::string> {"attach"}, "attached, with no maximum set");
}

// The order the platform requires, stated against a fake that enforces it: the
// same steps in the wrong order are refused.
void aMaximumSetBeforeAttachIsRefused()
{
    FakeOutput output(backFormat);
    const auto steps = output.steps();
    require(!steps.setMaximum({4032, 3024}), "an unattached output refuses its maximum");
    require(steps.attach() && steps.setMaximum({4032, 3024}), "and takes it once attached");
}

// The platform raises for a scaled size of another aspect, so the delivered
// size is compared with the format's by cross multiplication.
bool keepsAspect(FrameSize delivered, std::uint32_t formatWidth, std::uint32_t formatHeight)
{
    return std::uint64_t(delivered.width) * formatHeight == std::uint64_t(delivered.height) * formatWidth;
}

void aStreamSmallerThanItsFormatIsDeliveredScaled()
{
    require(deliveredFrameSize(1920, 1440, 1440, 1080) == FrameSize({1440, 1080}), "the preview of a 12 MP format is 1440x1080");
    require(deliveredFrameSize(1920, 1440, 1280, 720) == FrameSize({1280, 960}), "a wider stream keeps the format's aspect ratio");
    require(deliveredFrameSize(4032, 3024, 1920, 1080) == FrameSize({1920, 1440}), "and covers the stream's longer side");
}

void aStreamOfAnotherAspectIsDeliveredAtTheFormatsAspect()
{
    // 640x480 from a 16:9 format: a multiple of 16:9 that covers 480 lines.
    require(deliveredFrameSize(1920, 1080, 640, 480) == FrameSize({864, 486}), "an even multiple of 16:9, not 854x480");
    require(deliveredFrameSize(1920, 1440, 1000, 1000) == FrameSize({1336, 1002}), "a square stream from 4:3");
    // The old rule rounded each side on its own: 1334x1000 is not 4:3.
    require(!keepsAspect({1334, 1000}, 1920, 1440), "independent rounding does not keep the ratio");
    require(keepsAspect(deliveredFrameSize(1920, 1440, 1000, 1000), 1920, 1440), "the delivered size does");
}

void everyStandardSizeIsDeliveredAtEveryFormatsAspect()
{
    const std::pair<std::uint32_t, std::uint32_t> standard[] {{176, 144}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}};
    const std::pair<std::uint32_t, std::uint32_t> formats[] {{1440, 1080}, {1920, 1440}, {4032, 3024}, {1920, 1080}, {3840, 2160}, {4224, 2376}, {1088, 1088}};
    for (const auto& [formatWidth, formatHeight] : formats)
        for (const auto& [width, height] : standard) {
            if (formatWidth < width || formatHeight < height) continue;  // no format of this size takes the stream
            const auto delivered = deliveredFrameSize(formatWidth, formatHeight, width, height);
            require(keepsAspect(delivered, formatWidth, formatHeight), "the format's aspect, by cross multiplication");
            require(delivered.width >= width && delivered.height >= height, "covers the stream");
            require(delivered.width <= formatWidth && delivered.height <= formatHeight, "never above the format");
            require(delivered.width % 2 == 0 && delivered.height % 2 == 0, "even sides");
        }
}

void aRatioWithAnOddSideTakesAnEvenMultiple()
{
    // A 3:2 ratio has an odd side, so its multiple is even: 427 would give
    // the odd width 1281.
    require(deliveredFrameSize(1088, 1088, 640, 480) == FrameSize({640, 640}), "the multiple of a 1:1 format");
    require(deliveredFrameSize(3000, 2000, 1280, 720) == FrameSize({1284, 856}), "3:2 at 1280x720 is the multiple 428");
    require(keepsAspect(deliveredFrameSize(3000, 2000, 1280, 720), 3000, 2000), "of 3:2");
}

void aStreamTheFormatJustCoversIsDeliveredAsItIs()
{
    require(deliveredFrameSize(1440, 1080, 1440, 1080) == FrameSize({1440, 1080}), "the same size");
    require(deliveredFrameSize(1440, 1080, 1920, 1440) == FrameSize({1440, 1080}), "never above the format's own size");
    require(deliveredFrameSize(1440, 1080, 0, 0) == FrameSize({1440, 1080}), "a stream of no size leaves the format's");
    require(deliveredFrameSize(0, 0, 640, 480) == FrameSize({0, 0}), "a format of no size has none");
}

// The file the platform made is copied only if it is wanted and within the bound the guest's buffer sets: a refused file
// allocates nothing.
void aJpegFileBeyondTheGuestsBufferIsNotCopied()
{
    const std::vector<std::uint8_t> file(1000, 0x5a);
    std::vector<std::uint8_t> taken, unbounded, over, unwanted;
    require(takeJpegFile(file, 1000, true, taken) && taken == file, "a file of the bound's size is taken");
    require(takeJpegFile(file, 0, true, unbounded) && unbounded == file, "no bound takes any file");
    require(!takeJpegFile(file, 999, true, over) && over.capacity() == 0, "a byte over the bound is refused and not copied");
    require(!takeJpegFile(file, 1000, false, unwanted) && unwanted.capacity() == 0, "a file nobody asked for is not copied");
    std::vector<std::uint8_t> kept(3, 7);
    require(!takeJpegFile(file, 10, true, kept) && kept == std::vector<std::uint8_t>(3, 7), "a refusal leaves the result as it was");
}

// What a still that failed says of itself is a stage, a few fixed words and numbers: the framework's error becomes a
// domain word, so no name of the system's is kept, and every stage and word is its own.
void aFailureIsNamedInFixedWords()
{
    require(errorDomainOf("AVFoundationErrorDomain") == CameraErrorDomain::avFoundation &&
                errorDomainOf("NSOSStatusErrorDomain") == CameraErrorDomain::osStatus &&
                errorDomainOf("NSCocoaErrorDomain") == CameraErrorDomain::cocoa &&
                errorDomainOf("NSPOSIXErrorDomain") == CameraErrorDomain::posix &&
                errorDomainOf("NSMachErrorDomain") == CameraErrorDomain::mach,
            "the framework's domains have their words");
    require(errorDomainOf("") == CameraErrorDomain::other && errorDomainOf("com.example.SomeOtherDomain") == CameraErrorDomain::other &&
                errorDomainOf("avfoundationerrordomain") == CameraErrorDomain::other,
            "any other domain is `other`, whatever it is called");
    const std::vector<CameraStillStage> stages {CameraStillStage::none,       CameraStillStage::unknown,
                                                CameraStillStage::refused,    CameraStillStage::notStarted,
                                                CameraStillStage::frameworkError, CameraStillStage::missingData,
                                                CameraStillStage::resolvedSize,   CameraStillStage::encodedSize,
                                                CameraStillStage::fileSize,   CameraStillStage::guestBuffer,
                                                CameraStillStage::timeout};
    std::vector<std::string> words, domains;
    for (const auto stage : stages) words.emplace_back(stillStageName(stage));
    for (const auto domain : {CameraErrorDomain::none, CameraErrorDomain::avFoundation, CameraErrorDomain::osStatus,
                              CameraErrorDomain::cocoa, CameraErrorDomain::posix, CameraErrorDomain::mach, CameraErrorDomain::other})
        domains.emplace_back(errorDomainName(domain));
    for (auto* list : {&words, &domains}) {
        auto sorted = *list;
        std::sort(sorted.begin(), sorted.end());
        require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "every stage and every domain has its own word");
    }
    words.insert(words.end(), domains.begin(), domains.end());
    for (const auto& word : words)
        for (const char c : word)
            require((c >= 'a' && c <= 'z') || c == '-', "a word is lower case letters and hyphens");
    require(stillStageName(CameraStillStage::resolvedSize) == "resolved-size" && stillStageName(CameraStillStage::encodedSize) == "encoded-size" &&
                stillStageName(CameraStillStage::frameworkError) == "framework-error" && stillStageName(CameraStillStage::none) == "none",
            "the stages the readers know are spelt as they read them");
    require(factName(CameraFact::unknown) == "unknown" && factName(CameraFact::no) == "no" && factName(CameraFact::yes) == "yes" &&
                cameraPositionName(CameraPosition::front) == "front" && cameraPositionName(CameraPosition::back) == "back" &&
                cameraPositionName(CameraPosition::external) == "external",
            "the facts and positions have their words");
    // A result that is a picture says nothing: its failure is the stage none.
    require(CameraStillFailure {}.stage == CameraStillStage::none && CameraStillFailure {}.compensationSupported == CameraFact::unknown &&
                CameraStillFailure {}.errorDomain == CameraErrorDomain::none,
            "a failure that says nothing is stage none, unknown and no error");
}

} // namespace

int main()
{
    aJpegFileBeyondTheGuestsBufferIsNotCopied();
    anExactSizeTheFormatListsIsAsked();
    aSizeAnotherFormatListsIsNotAsked();
    aLargerSizeThatCoversIsNotTheRequest();
    theOutputsMaximumBoundsTheRequest();
    anEmptyOrZeroSizedTableAsksForNothing();
    aCaptureWithoutAJpegTakesTheSmallestSize();
    theResolvedSizeIsTheRequestOrItIsRefused();
    aPhotoOfAnotherSizeIsMadeAgainOnceAndThenFails();
    aFullResolutionMaximumIsGivenTimeToTakeEffectBeforeTheFirstStill();
    aScheduledStillFiresWaitsFailsOrIsDropped();
    aStillThatKeepsBeingPostponedFailsOnceWithinItsBudget();
    aRetryHasWhatIsLeftOfTheWaitBudget();
    aStopDuringTheWaitDropsTheStillAndCompletesNothing();
    theOutputIsAttachedBeforeItsMaximumIsSet();
    aMaximumTheSourceRefusesDetachesTheOutput();
    anOutputThatCannotBeAttachedGetsNoMaximum();
    aFormatThatListsNothingLeavesTheDefault();
    aMaximumSetBeforeAttachIsRefused();
    aStreamSmallerThanItsFormatIsDeliveredScaled();
    aStreamOfAnotherAspectIsDeliveredAtTheFormatsAspect();
    everyStandardSizeIsDeliveredAtEveryFormatsAspect();
    aRatioWithAnOddSideTakesAnEvenMultiple();
    aStreamTheFormatJustCoversIsDeliveredAsItIs();
    aFailureIsNamedInFixedWords();
    return 0;
}
