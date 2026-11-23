// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Types shared by the encoder session and its discovery. Not part of the
// platform interface: nothing outside the Apple video encoder backend and its
// tests includes this.

#include "video_encoder.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace uml::platform::apple_encoder {

// What discovery learned about one codec: the interface-visible capability
// plus what only the backend needs.
struct CodecDiscovery {
    VideoEncodeCapability capability;
    // Profiles the low-latency encoder writes, which is a subset of the
    // profiles above: it silently turns High 4:2:2 into High, for example.
    // Empty when there is no low-latency encoder for the codec.
    std::vector<VideoEncodeProfile> lowLatencyProfiles;
};

struct Discovery {
    std::vector<CodecDiscovery> codecs;
    // False when a probe of the discovery could not be answered, because the
    // host was busy or lost the session, which is not an answer of the host. An
    // incomplete result lists no more than the host proved, so it is a subset
    // of the complete one; it is never cached.
    bool complete {true};
    unsigned unanswered {};   // the probes that stayed unanswered
    // The codecs with a probe left unanswered, whether or not they are listed
    // above: what is uncertain is those codecs, not the host.
    std::vector<VideoCodec> unansweredCodecs;

    [[nodiscard]] const CodecDiscovery* find(VideoCodec codec) const noexcept
    {
        for (const auto& entry : codecs)
            if (entry.capability.codec == codec) return &entry;
        return nullptr;
    }
};

// The discovery of this process, from the cache or computed; waits for it. It is
// the answer the encoder checks a configuration against, and a better one
// replaces it only whole (see DiscoveryService).
[[nodiscard]] std::shared_ptr<const Discovery> discovery();

// Removes the cache file, so the next process rediscovers the host. Called when
// a session the discovery promised cannot be created.
void invalidateDiscoveryCache() noexcept;

// The session discovery drives. It is the production encoder with one
// difference: `configureUnchecked` skips the check against the discovery, which
// is what discovery is still computing. Every other call is the public one, so
// a probe exercises the same pipeline a guest's stream does.
class ProbeEncoder : public VideoEncoder {
public:
    [[nodiscard]] virtual VideoEncodeStatus configureUnchecked(const VideoEncodeSettings& settings) = 0;
    // Identifier of the VideoToolbox encoder behind the configured session,
    // for example "com.apple.videotoolbox.videoencoder.ave.avc"; empty when
    // none is configured.
    [[nodiscard]] virtual std::string encoderIdentifier() const = 0;
    // Why the last configure or session step failed, for the line a test or
    // a probe prints; empty when nothing failed.
    [[nodiscard]] virtual std::string diagnostic() const = 0;
    // How long a drain waits for the frames in flight. A session starts with
    // five seconds; a probe passes its own time budget.
    virtual void setDrainWait(unsigned milliseconds) = 0;
#if UML_VIDEO_ENCODER_TESTING
    virtual void loseSessionForTest(unsigned count) = 0;
    virtual void invalidateSessionForTest() = 0;
#endif
};

[[nodiscard]] std::unique_ptr<ProbeEncoder> makeProbeEncoder();

// The VideoToolbox ProfileLevel value naming a profile with the level left to
// the encoder, for example "H264_High_AutoLevel".
[[nodiscard]] std::string autoLevelName(VideoEncodeProfile profile);

// ---- What discovery asks of the host.

// What one trial encode produced.
struct Stream {
    VideoEncodeStatus status {VideoEncodeStatus::failed};
    bool encoded {};   // every frame produced a coded picture and the drain completed
    std::vector<EncodedVideoFrame> frames;
    std::string encoder;
    std::string diagnostic;
    double seconds {};
};

struct Probe {
    VideoEncodeSettings settings;
    unsigned frames {2};
    bool noisy {};   // fill with a pattern rather than leave the pool's zeros
    unsigned timeoutMilliseconds {15'000};
};

// Everything discovery and its process-wide result take from outside: the
// host's answers, time, and the cache file. The product's are the VideoToolbox
// session, the clock and the JSON file; the unit test variant scripts them.
struct DiscoveryHooks {
    std::function<Stream(const Probe&)> run;                                 // one trial encode
    std::function<unsigned(const VideoEncodeSettings&)> countSessions;       // sessions opened together, up to a limit
    std::function<std::optional<bool>(VideoCodec)> hardwareListed;           // VideoToolbox lists a hardware encoder; none: the call failed
    std::function<std::set<std::string>(VideoCodec)> listedProfiles;         // empty: the encoder would not say
    std::function<void(unsigned)> pause;                                     // milliseconds
    std::function<std::uint64_t()> now;                                      // monotonic milliseconds
    std::function<std::optional<Discovery>()> load;                          // the cache file, when it is valid
    std::function<void(const Discovery&)> store;                             // writes the cache file
    // How long a query waits for a discovery that runs; the discovery goes on
    // in the background when it passes.
    unsigned waitMilliseconds {300'000};
    // Runs a task on a new thread, and throws when it cannot. Empty: a detached thread.
    std::function<void(std::function<void()>)> spawn;
};

// The result of discovery for a process, and when it is computed again.
//
// An incomplete discovery (Discovery::complete) is never stored. It serves the
// query that found it. A later query computes the discovery again, at most
// kMaximumRuns times in the process and not sooner than kRerunInterval after
// the last one, and a complete result replaces the answer. One discovery runs
// at a time, whoever started it; a run counts when it starts. The answer of
// a process never shrinks: what was offered stays valid, so a codec's entry is
// replaced only by one that contains it, and nothing replaces a complete
// answer. A discovery whose thread cannot be created releases its place, keeps
// the answer, and counts as a run. A cache hit is published at once with the
// session counts its file carries; a count below the most the host gives is
// measured once afterwards, in the background, and raises the published count
// and the file's, never lowers them. `discovery()` never computes: the encoder
// checks against the answer it has.
class DiscoveryService {
public:
    static constexpr unsigned kMaximumRuns = 3;
    static constexpr std::uint64_t kRerunIntervalMilliseconds = 30'000;
    virtual ~DiscoveryService() = default;
    // Starts the first discovery on its own thread; idempotent.
    virtual void start() = 0;
    // The current answer, waiting for the first one.
    [[nodiscard]] virtual std::shared_ptr<const Discovery> current() = 0;
    // The capabilities a caller who offers them to a guest is told.
    [[nodiscard]] virtual std::vector<VideoEncodeCapability> query() = 0;
};

#if UML_VIDEO_ENCODER_TESTING
[[nodiscard]] Discovery runDiscovery(const DiscoveryHooks& hooks);
[[nodiscard]] std::unique_ptr<DiscoveryService> makeDiscoveryService(DiscoveryHooks hooks);
#endif

} // namespace uml::platform::apple_encoder
