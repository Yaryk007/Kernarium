// SPDX-License-Identifier: GPL-2.0-or-later

// The microphone's input tap, run on the real AVAudioConverter the engine uses, fed buffers a test makes: nothing
// here opens the microphone, an audio engine or an audio session.

#include "apple_microphone_tap.hpp"
#include "capture_ring.hpp"
#include "protocol.hpp"
#include "require.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <time.h>

namespace {

namespace protocol = uml::audio::protocol;

using uml::audio::CaptureRing;
using uml::audio::MicrophoneTap;
using uml::testing::require;

// Sound from before a recording is a loud tone and the recording's own sound a quiet one, so a sample of the first
// kept in the recording shows at a glance. Both are well clear of the converter's ringing at a change between them.
constexpr float earlierLevel = 0.5f;
constexpr float recordingLevel = 0.05f;
constexpr int earlierThreshold = 8000;

constexpr std::uint64_t millisecond = 1'000'000ULL;

struct Case {
    double inputRate;
    std::uint8_t channels;
};

// The rates a microphone input runs at, and the guest's mono and stereo.
constexpr Case cases[] = {
    {48000, 1}, {44100, 1}, {32000, 1}, {16000, 1}, {48000, 2}, {44100, 2},
};

/* The tap as the engine builds it: the converter from the input's format to the guest's, its output buffer, the ring. */
struct Rig {
    explicit Rig(const Case& config)
        : channels(config.channels)
        , input([[AVAudioFormat alloc] initStandardFormatWithSampleRate:config.inputRate channels:config.channels])
        , output([[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatInt16
                                                  sampleRate:protocol::sampleRate
                                                    channels:config.channels
                                                 interleaved:YES])
        , converter([[AVAudioConverter alloc] initFromFormat:input toFormat:output])
        , converted([[AVAudioPCMBuffer alloc] initWithPCMFormat:output frameCapacity:24000])
        , ring(1U << 20)
        , tap(converter, converted, ring, config.channels, protocol::sampleRate)
    {
        require(converter != nil && converted != nil, "the converter and its buffer exist");
    }

    /* A buffer of `milliseconds` of input whose first `earlierMilliseconds` are the earlier tone and the rest the
     * recording's, received with its first sample captured at `capturedNanoseconds` (uptime). */
    void receive(unsigned milliseconds, unsigned earlierMilliseconds, std::uint64_t capturedNanoseconds)
    {
        const auto frames = static_cast<AVAudioFrameCount>(input.sampleRate * milliseconds / 1000);
        const auto earlier = static_cast<AVAudioFrameCount>(input.sampleRate * earlierMilliseconds / 1000);
        AVAudioPCMBuffer* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:input frameCapacity:frames];
        buffer.frameLength = frames;
        for (std::uint8_t channel = 0; channel < channels; ++channel)
            for (AVAudioFrameCount frame = 0; frame < frames; ++frame)
                buffer.floatChannelData[channel][frame] = frame < earlier ? earlierLevel : recordingLevel;
        AVAudioTime* when = [AVAudioTime
            timeWithHostTime:[AVAudioTime hostTimeForSeconds:static_cast<double>(capturedNanoseconds) / 1e9]];
        tap.receive(buffer, when);
    }

    /* Everything the ring holds, as samples. */
    [[nodiscard]] std::vector<std::int16_t> drain()
    {
        std::vector<std::int16_t> samples(ring.size() / sizeof(std::int16_t));
        const auto count = ring.read(std::span(reinterpret_cast<std::uint8_t*>(samples.data()),
                                               samples.size() * sizeof(std::int16_t)));
        require(count == samples.size() * sizeof(std::int16_t), "the ring held whole samples");
        return samples;
    }

    [[nodiscard]] std::string name(const char* what) const
    {
        return std::string(what) + " at " + std::to_string(static_cast<int>(input.sampleRate)) + " Hz, " +
            std::to_string(channels) + " channel(s)";
    }

    std::uint8_t channels;
    AVAudioFormat* input;
    AVAudioFormat* output;
    AVAudioConverter* converter;
    AVAudioPCMBuffer* converted;
    CaptureRing ring;
    MicrophoneTap tap;
};

[[nodiscard]] std::size_t loudSamples(const std::vector<std::int16_t>& samples)
{
    return static_cast<std::size_t>(std::count_if(samples.begin(), samples.end(),
        [](std::int16_t sample) { return std::abs(static_cast<int>(sample)) > earlierThreshold; }));
}

[[nodiscard]] std::size_t recordingSamples(const std::vector<std::int16_t>& samples)
{
    constexpr int level = static_cast<int>(recordingLevel * 32768.0f);
    return static_cast<std::size_t>(std::count_if(samples.begin(), samples.end(),
        [](std::int16_t sample) { return std::abs(static_cast<int>(sample) - level) < 400; }));
}

[[nodiscard]] std::uint64_t uptime() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* What a microphone that was open before the guest started has handed the converter is not in the recording: the
 * converter holds samples from one call to the next, and the tap gives it up when a recording begins. Each
 * recording of the same open microphone is clean, not only the first. */
void theConvertersHistoryIsNotTheRecordings()
{
    for (const Case& config : cases) {
        Rig rig(config);
        const std::uint64_t now = uptime();
        for (int recording = 0; recording < 2; ++recording) {
            // Open, standby: two tenths of a second of the earlier tone, which the converter has not given up.
            rig.receive(100, 100, now - 300 * millisecond);
            rig.receive(100, 100, now - 200 * millisecond);
            require(rig.ring.size() > 0, rig.name("the converter hands over audio in standby"));
            // The guest starts. The next buffer begins at the boundary and is the recording's alone.
            rig.ring.restart(now);
            rig.receive(100, 0, now);
            const auto kept = rig.drain();
            require(!kept.empty(), rig.name("the recording has audio"));
            require(loudSamples(kept) == 0, rig.name("no sample of the earlier tone is in the recording"));
            require(recordingSamples(kept) > 100, rig.name("the recording's own audio is"));
        }
    }
}

/* A buffer that began before the boundary loses the frames that precede it, and keeps the ones after. */
void aBufferThatSpansTheBoundaryKeepsNothingFromBeforeIt()
{
    for (const Case& config : cases) {
        Rig rig(config);
        const std::uint64_t now = uptime();
        rig.receive(100, 100, now - 200 * millisecond);
        // The boundary is 20 ms into the buffer, and a little more than a sample: the first sample after the earlier
        // tone was captured before it.
        rig.ring.restart(now + 20 * millisecond + 10'000);
        rig.receive(100, 20, now);
        const auto kept = rig.drain();
        require(loudSamples(kept) == 0, rig.name("no sample of the earlier tone is in the recording"));
        require(recordingSamples(kept) > 100, rig.name("the part after the boundary is"));
    }
}

/* A sample captured any time before the boundary is left out, a frame period or less before it included. At the
 * guest's own rate the converter changes no timing, so the last sample of the earlier tone is the one in question. */
void aSampleCapturedJustBeforeTheBoundaryIsLeftOut()
{
    for (const std::uint8_t channels : {std::uint8_t {1}, std::uint8_t {2}}) {
        Rig rig({protocol::sampleRate, channels});
        const std::uint64_t now = uptime();
        rig.receive(100, 100, now - 200 * millisecond);
        // The last sample of the earlier tone was captured at 19.979 ms, 11 microseconds before the boundary.
        rig.ring.restart(now + 20 * millisecond - 10'000);
        rig.receive(100, 20, now);
        const auto kept = rig.drain();
        require(loudSamples(kept) == 0, rig.name("the last sample before the boundary is not in the recording"));
        require(recordingSamples(kept) > 100, rig.name("the part after the boundary is"));
    }
}

/* A converter emits fewer frames than it was given and holds the rest for its next call, so a boundary late in a
 * buffer is in the next call's output: the frames to leave out are counted along the epoch's output. */
void aBoundaryLateInABufferReachesIntoTheNextOutput()
{
    for (const Case& config : cases) {
        Rig rig(config);
        const std::uint64_t now = uptime();
        rig.receive(100, 100, now - 200 * millisecond);
        // 90 ms of the buffer is before the boundary, more than the converter emits for one buffer.
        rig.ring.restart(now + 90 * millisecond + 10'000);
        rig.receive(100, 90, now);
        rig.receive(100, 0, now + 100 * millisecond);
        const auto kept = rig.drain();
        require(loudSamples(kept) == 0, rig.name("no sample of the earlier tone is in the recording"));
        require(recordingSamples(kept) > 100, rig.name("the recording's own audio is"));
    }
}

/* Audio before any recording is kept whole: the tap leaves out only what a boundary says. */
void audioBeforeAnyRecordingIsKept()
{
    for (const Case& config : cases) {
        Rig rig(config);
        const std::uint64_t now = uptime();
        rig.receive(100, 100, now - 200 * millisecond);
        rig.receive(100, 100, now - 100 * millisecond);
        const auto kept = rig.drain();
        require(loudSamples(kept) > 4000, rig.name("the tap leaves nothing out of a ring that has had no recording"));
    }
}

} // namespace

int main()
{
    @autoreleasepool {
        theConvertersHistoryIsNotTheRecordings();
        aBufferThatSpansTheBoundaryKeepsNothingFromBeforeIt();
        aSampleCapturedJustBeforeTheBoundaryIsLeftOut();
        aBoundaryLateInABufferReachesIntoTheNextOutput();
        audioBeforeAnyRecordingIsKept();
    }
    return EXIT_SUCCESS;
}
