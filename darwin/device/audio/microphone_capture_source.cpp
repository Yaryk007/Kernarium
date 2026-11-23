// SPDX-License-Identifier: GPL-2.0-or-later

#include "microphone_capture_source.hpp"

#include <time.h>

#include <utility>

namespace uml::audio {
namespace {

// The clock the system stamps captured audio with (AVAudioTime's host time, in nanoseconds).
std::uint64_t uptimeNanoseconds() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// Room for 340 ms of 48 kHz stereo. The system delivers the microphone in
// bursts of 100 ms or more.
constexpr std::size_t ringBytes = 64U * 1024U;
// The oldest audio beyond this much is dropped, so that a microphone running
// a little fast, or a guest that stalled, cannot add latency: a burst and a
// half, at the stream's rate.
constexpr std::uint32_t backlogMilliseconds = 250;
// The channels a microphone opened for a guest that is not recording yet is
// opened with, until a recording has said otherwise (the image offers mono and
// stereo; its camera app records mono). A recording that asks for other
// channels than the open microphone has opens it again, and that costs the
// recording the system's opening time of silence.
constexpr std::uint8_t standbyChannels = 1;

class MicrophoneCaptureSource final : public CaptureSource {
public:
    MicrophoneCaptureSource(
        std::shared_ptr<MicrophoneSwitch> microphone,
        std::unique_ptr<MicrophoneInput> input,
        std::shared_ptr<const Waker> waker)
        : microphone_(std::move(microphone))
        , input_(std::move(input))
        , waker_(std::move(waker))
        , ring_(ringBytes)
    {
        microphone_->setWaker(waker_);
    }

    ~MicrophoneCaptureSource() override
    {
        microphone_->clearWaker(waker_.get());
        stopInput();
    }

    void configure(const CaptureParameters& parameters) noexcept override
    {
        parameters_ = parameters;
        if (parameters.channels != 0)
            lastChannels_ = parameters.channels;
    }

    void prepare() noexcept override {}

    void start() noexcept override
    {
        recording_ = true;
        // A microphone that has been open since before the guest started holds
        // audio from before the recording, and its audio thread may hold a
        // buffer it captured earlier: the recording begins with audio captured
        // after this start, so the sound is not behind the picture by what
        // came before it.
        ring_.restart(uptimeNanoseconds());
        reconcile();
    }

    void stop() noexcept override { finish(); }
    void release() noexcept override { finish(); }
    void reset() noexcept override { finish(); }
    void handleEvents() noexcept override { reconcile(); }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> pcm) noexcept override
    {
        if (!capturing_)
            return 0;
        const std::size_t limit = std::size_t {parameters_.sampleRate} *
            parameters_.channels * parameters_.bytesPerSample *
            backlogMilliseconds / 1000U;
        if (const auto held = ring_.size(); held > limit)
            ring_.discard(held - limit);
        return ring_.read(pcm);
    }

private:
    void finish() noexcept
    {
        recording_ = false;
        reconcile();
    }

    /* The microphone runs exactly while the guest records, or a camera of the
     * guest is in video mode and the guest is about to (the switch's
     * standby), and the user's setting allows it; a start that fails is tried
     * again on the next change. A microphone opened for a guest that is not
     * recording yet has the channels of its last recording, else mono, and
     * is opened again if the recording asks for others. */
    void reconcile() noexcept
    {
        const bool wanted = (recording_ || microphone_->standby()) && microphone_->active();
        const std::uint8_t channels = recording_ && parameters_.channels != 0 ? parameters_.channels
            : lastChannels_ != 0                                              ? lastChannels_
                                                                              : standbyChannels;
        if (capturing_ && (!wanted || channels != openChannels_))
            stopInput();
        if (wanted && !capturing_) {
            ring_.clear();
            capturing_ = input_->start(channels, ring_);
            openChannels_ = capturing_ ? channels : std::uint8_t {};
        } else if (wanted && capturing_) {
            input_->revive();
        }
    }

    void stopInput() noexcept
    {
        if (!capturing_)
            return;
        input_->stop();
        capturing_ = false;
        openChannels_ = 0;
        ring_.clear();
    }

    std::shared_ptr<MicrophoneSwitch> microphone_;
    std::unique_ptr<MicrophoneInput> input_;
    std::shared_ptr<const Waker> waker_;
    CaptureRing ring_;
    CaptureParameters parameters_ {};
    std::uint8_t lastChannels_ {};
    std::uint8_t openChannels_ {};
    bool recording_ {};
    bool capturing_ {};
};

} // namespace

std::unique_ptr<CaptureSource> makeMicrophoneCaptureSource(
    std::shared_ptr<MicrophoneSwitch> microphone,
    std::unique_ptr<MicrophoneInput> input,
    std::shared_ptr<const Waker> waker)
{
    return std::make_unique<MicrophoneCaptureSource>(
        std::move(microphone), std::move(input), std::move(waker));
}

} // namespace uml::audio
