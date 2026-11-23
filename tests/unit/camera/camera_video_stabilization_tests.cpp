// SPDX-License-Identifier: GPL-2.0-or-later
// Which video stabilization a camera is asked for, over every combination of what its format supports, and what
// an apply does about the connection's (no camera is opened).
#include "require.hpp"
#include "video_stabilization.hpp"

#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

void aGuestThatDoesNotWantStabilizationGetsNone()
{
    for (const bool standard : {false, true})
        for (const bool lowLatency : {false, true})
            require(chooseVideoStabilization(false, {standard, lowLatency}) == VideoStabilization::off,
                    "a picture the guest left unstabilized stays so, whatever the format supports");
}

void theLowLatencyModeIsPreferredWhereTheFormatSupportsIt()
{
    require(chooseVideoStabilization(true, {true, true}) == VideoStabilization::lowLatency,
            "low latency wins over standard");
    require(chooseVideoStabilization(true, {false, true}) == VideoStabilization::lowLatency,
            "low latency alone is taken");
}

void theStandardModeIsTheFallback()
{
    require(chooseVideoStabilization(true, {true, false}) == VideoStabilization::standard,
            "standard is taken when low latency is not supported");
}

void aFormatThatSupportsNeitherIsNotStabilized()
{
    require(chooseVideoStabilization(true, {false, false}) == VideoStabilization::off,
            "no mode the format supports leaves the picture unstabilized");
}

// A step as the backend takes it: the connection then holds what the step says.
VideoStabilizationHeld took(const VideoStabilizationStep& step)
{
    return {step.choice, step.state};
}

void aNewConnectionIsStabilizedAtItsFirstApplyWhateverTheOtherSettingsSay()
{
    // The guest asks for stabilization in video mode, which is also what a fresh settings cache holds, so an apply that is
    // skipped for unchanged settings would leave the connection with the system's default, which is none.
    const VideoStabilizationHeld fresh;
    const auto standard = stabilizationStep(true, {true, false}, fresh);
    require(standard.write && standard.choice == VideoStabilization::standard &&
                standard.state == VideoStabilizationState::standard && standard.note && !standard.unsupported,
            "a new connection is stabilized from the first apply, in the standard mode where that is all there is");
    const auto lowLatency = stabilizationStep(true, {true, true}, fresh);
    require(lowLatency.write && lowLatency.choice == VideoStabilization::lowLatency &&
                lowLatency.state == VideoStabilizationState::lowLatency,
            "and in the low-latency mode where the format supports it");
}

void aGuestThatDoesNotAskLeavesTheConnectionAlone()
{
    const auto step = stabilizationStep(false, {true, true}, VideoStabilizationHeld {});
    require(!step.write && step.choice == VideoStabilization::off && step.state == VideoStabilizationState::notWanted &&
                step.note && !step.unsupported,
            "a connection nobody wrote to already holds none, and the record says the guest did not ask");
}

void aWishTheConnectionAlreadyHoldsChangesNothing()
{
    // The view must not jump when the user first taps: the apply that follows the first tap carries the same wish.
    const VideoStabilizationHeld held = took(stabilizationStep(true, {true, false}, VideoStabilizationHeld {}));
    const auto tap = stabilizationStep(true, {true, false}, held);
    require(!tap.write && !tap.note && !tap.unsupported && tap.choice == VideoStabilization::standard,
            "the same wish writes nothing and says nothing");
}

void aChangedWishOrFormatIsWrittenAgain()
{
    const VideoStabilizationHeld held = took(stabilizationStep(true, {true, false}, VideoStabilizationHeld {}));
    const auto left = stabilizationStep(false, {true, false}, held);
    require(left.write && left.choice == VideoStabilization::off && left.state == VideoStabilizationState::notWanted && left.note,
            "a guest that stops asking turns it off");
    const auto better = stabilizationStep(true, {true, true}, held);
    require(better.write && better.choice == VideoStabilization::lowLatency && better.note,
            "a format that supports the low-latency mode moves to it");
    const auto worse = stabilizationStep(true, {false, false}, held);
    require(worse.write && worse.choice == VideoStabilization::off && worse.unsupported,
            "a format that supports nothing turns it off and says so");
}

void aFormatWithNoModeIsToldOnce()
{
    const VideoStabilizationHeld fresh;
    const auto first = stabilizationStep(true, {false, false}, fresh);
    require(!first.write && first.choice == VideoStabilization::off && first.state == VideoStabilizationState::unsupported &&
                first.note && first.unsupported,
            "wanted and unsupported stays unstabilized, and is said");
    const auto again = stabilizationStep(true, {false, false}, took(first));
    require(!again.write && !again.note && !again.unsupported, "it is said once for the connection");
    const auto afterNew = stabilizationStep(true, {false, false}, VideoStabilizationHeld {});
    require(afterNew.unsupported, "a new connection says it again");
}

void noChoiceIsEverTheAutomaticOrACinematicMode()
{
    // The choice type has three values; every combination lands on one of them.
    for (const bool wanted : {false, true})
        for (const bool standard : {false, true})
            for (const bool lowLatency : {false, true})
                for (const auto mode : {VideoStabilization::off, VideoStabilization::standard, VideoStabilization::lowLatency}) {
                    const auto step = stabilizationStep(wanted, {standard, lowLatency}, {mode, VideoStabilizationState::unknown});
                    require(step.choice == chooseVideoStabilization(wanted, {standard, lowLatency}) &&
                                step.write == (step.choice != mode),
                            "a step writes exactly when the choice differs from what the connection holds");
                }
}

void theRecordNamesEveryState()
{
    require(stabilizationName(VideoStabilizationState::unknown) == "-" && stabilizationName(VideoStabilizationState::notWanted) == "off" &&
                stabilizationName(VideoStabilizationState::standard) == "standard" &&
                stabilizationName(VideoStabilizationState::lowLatency) == "low-latency" &&
                stabilizationName(VideoStabilizationState::unsupported) == "unsupported",
            "the record's words");
}

} // namespace

/* The connection the glue writes to, here a recorder. */
struct RecordingPort final : VideoStabilizationPort {
    explicit RecordingPort(VideoStabilizationSupport supported) : supported_(supported) {}
    VideoStabilizationSupport support() override { return supported_; }
    void write(VideoStabilization mode) override { written.push_back(mode); }
    VideoStabilizationSupport supported_;
    std::vector<VideoStabilization> written;
};

// The apply the glue runs (applyVideoStabilization) writes the connection once for a wish and not again for the same wish,
// updates what the connection holds, and starts again on a new connection.
void theApplyWritesAWishOnceForAConnection()
{
    RecordingPort port({true, false});
    VideoStabilizationHeld held;
    auto step = applyVideoStabilization(port, true, held);
    require(port.written == std::vector<VideoStabilization> {VideoStabilization::standard} && step.write && step.note,
            "the first apply of a wish writes the mode");
    require(held.mode == VideoStabilization::standard && held.state == VideoStabilizationState::standard, "and the connection holds it");
    step = applyVideoStabilization(port, true, held);
    require(port.written.size() == 1 && !step.write && !step.note, "the same wish again writes and says nothing");
    step = applyVideoStabilization(port, false, held);
    require(port.written.size() == 2 && port.written.back() == VideoStabilization::off && step.note,
            "a guest that stops asking is written off");
    VideoStabilizationHeld fresh;
    (void)applyVideoStabilization(port, true, fresh);
    require(port.written.size() == 3 && port.written.back() == VideoStabilization::standard, "a new connection is written again");
    RecordingPort none({false, false});
    VideoStabilizationHeld unsupported;
    step = applyVideoStabilization(none, true, unsupported);
    require(none.written.empty() && step.unsupported && unsupported.state == VideoStabilizationState::unsupported,
            "a format with no mode is written nothing and held as unsupported");
}

// The admission of a new video connection programs the stabilization only where an apply has said what the guest asks
// for; a guest that does not ask is not given a stabilization it would then lose.
void anAdmissionProgramsOnlyWhatTheGuestHasAsked()
{
    StabilizationWish wish;
    require(!wish.atAdmission(), "settings never applied say nothing: the first apply decides");
    wish.note(true);
    require(wish.atAdmission(), "a guest that asked for stabilization is programmed");
    wish.note(false);
    require(!wish.atAdmission(), "one that stopped asking (a photo preview) is programmed none");
}

int main()
{
    aGuestThatDoesNotWantStabilizationGetsNone();
    theLowLatencyModeIsPreferredWhereTheFormatSupportsIt();
    theStandardModeIsTheFallback();
    aFormatThatSupportsNeitherIsNotStabilized();
    aNewConnectionIsStabilizedAtItsFirstApplyWhateverTheOtherSettingsSay();
    aGuestThatDoesNotAskLeavesTheConnectionAlone();
    aWishTheConnectionAlreadyHoldsChangesNothing();
    aChangedWishOrFormatIsWrittenAgain();
    aFormatWithNoModeIsToldOnce();
    noChoiceIsEverTheAutomaticOrACinematicMode();
    theRecordNamesEveryState();
    theApplyWritesAWishOnceForAConnection();
    anAdmissionProgramsOnlyWhatTheGuestHasAsked();
    return 0;
}
