// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/video_encoder_internal.hpp"
#include "require.hpp"
#include "video_encoder_invariants.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

/* What the encoder's discovery makes of a host that does not always answer.
 *
 * A scripted host stands in for VideoToolbox: it answers every trial encode with
 * a stream that states what was asked for, refuses what the script refuses,
 * and can be busy, which is a probe that gets no session (the status a size
 * past the host's ceiling gets as well). The tests hold that a refusal is
 * stored, that no answer never is, that a ceiling still ends, and that the
 * process asks again, a bounded number of times, for a result that was cut
 * short. No hardware is used. */

namespace {
using namespace uml::platform;
using namespace uml::platform::apple_encoder;
using uml::testing::require;

enum class Answer {
    healthy,    // an encode that finishes and states what was asked
    busy,       // no session: the status of a busy host, and of a size beyond the ceiling
    refused,    // the configuration is refused as it stands
};

// A point the scripted host waits at until the test lets it go.
class Gate {
public:
    void wait()
    {
        std::unique_lock lock(mutex_);
        require(changed_.wait_for(lock, std::chrono::seconds(20), [&] { return open_; }), "the test never opened the gate");
    }
    void open()
    {
        std::lock_guard lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ {};
};

struct Host {
    std::vector<Probe> asked;
    std::atomic<unsigned> pauses {};
    std::atomic<unsigned> discoveries {};      // started, whether or not the list call answered
    std::atomic<unsigned> listedQueries {};    // one per discovery that got an answer to its list call
    std::atomic<std::uint64_t> clock {1'000'000};
    std::vector<Discovery> stored;
    std::optional<Discovery> cache;
    // The ceilings of this host: a larger picture or bitrate gets no session.
    std::uint64_t largestSamples {4096ull * 2304ull};
    std::uint64_t largestBitrate {100'000'000ull};
    std::atomic<unsigned> sessions {8};
    std::atomic<unsigned> loads {};
    std::atomic<unsigned> sessionCalls {};   // measurements of the concurrent sessions
    std::function<void()> onSessions;        // runs when sessions are measured
    std::atomic<bool> listFails {};   // the call that lists the encoders fails
    std::atomic<bool> spawnFails {};  // no thread can be created for a discovery
    std::atomic<unsigned> spawnAttempts {};
    bool hasEncoder {true};           // the list names an H.264 encoder
    std::function<void()> onListed;   // runs when a discovery asks for the list
    unsigned waitMilliseconds {300'000};
    std::function<Answer(const Probe&, std::size_t call)> answer = [](const Probe&, std::size_t) { return Answer::healthy; };

    static bool is(const Probe& probe, VideoEncodeProfile profile, RawVideoLayout input)
    {
        return probe.settings.profile == profile && probe.settings.input == input && probe.settings.width == 128 &&
               probe.settings.height == 96 && probe.frames == 2;
    }
    // The call index of the first probe of this kind, or none yet.
    [[nodiscard]] bool askedBefore(const Probe& probe) const
    {
        return std::any_of(asked.begin(), asked.end(), [&](const Probe& earlier) {
            return earlier.settings.profile == probe.settings.profile && earlier.settings.input == probe.settings.input &&
                   earlier.settings.width == probe.settings.width && earlier.settings.height == probe.settings.height &&
                   earlier.frames == probe.frames && earlier.settings.rateControl == probe.settings.rateControl;
        });
    }

    static unsigned profileIdc(VideoEncodeProfile profile, unsigned& constraints)
    {
        constraints = 0;
        switch (profile) {
        case VideoEncodeProfile::h264Baseline: return 66;
        case VideoEncodeProfile::h264ConstrainedBaseline: constraints = 0x40; return 66;
        case VideoEncodeProfile::h264Main: return 77;
        case VideoEncodeProfile::h264High: return 100;
        case VideoEncodeProfile::h264ConstrainedHigh: constraints = 0x0c; return 100;
        case VideoEncodeProfile::h264High422: return 122;
        case VideoEncodeProfile::h264High444Predictive: return 244;
        default: return 0;
        }
    }

    // What the hardware writes: pictures whose first one carries an SPS that
    // states the profile and the level, and whose size follows the rate control
    // parameter the way a mode that works does.
    [[nodiscard]] static Stream healthyStream(const Probe& probe)
    {
        const auto& s = probe.settings;
        Stream out;
        out.status = VideoEncodeStatus::ok;
        out.encoded = true;
        out.seconds = 0.01 + 0.001 * probe.frames;
        unsigned constraints = 0;
        const unsigned idc = profileIdc(s.profile, constraints);
        std::size_t payload = 400;
        switch (s.rateControl) {
        case VideoRateControl::constantBitrate: payload += s.bitrate / 2000; break;
        case VideoRateControl::constantQuality: payload += s.quality * 20; break;
        case VideoRateControl::constantQp: payload += (60 - s.frameQp[1]) * 50; break;
        case VideoRateControl::variableBitrate: break;
        }
        for (unsigned i = 0; i < probe.frames; ++i) {
            EncodedVideoFrame frame;
            frame.kind = EncodedVideoFrameKind::frame;
            frame.type = i == 0 ? VideoFrameType::key : VideoFrameType::predicted;
            frame.timestamp = i;
            if (i == 0) {
                const std::uint8_t sps[] = {0, 0, 0, 1, 0x67, std::uint8_t(idc), std::uint8_t(constraints),
                                            std::uint8_t(s.level ? s.level : 31)};
                frame.bytes.assign(std::begin(sps), std::end(sps));
            }
            frame.bytes.resize(frame.bytes.size() + payload, 0xab);
            out.frames.push_back(std::move(frame));
        }
        return out;
    }

    [[nodiscard]] DiscoveryHooks hooks()
    {
        DiscoveryHooks h;
        h.run = [this](const Probe& probe) {
            const auto call = asked.size();
            auto what = answer(probe, call);
            // The ceilings are the host's own, whatever the script says.
            if (what == Answer::healthy && (std::uint64_t(probe.settings.width) * probe.settings.height > largestSamples ||
                                            probe.settings.bitrate > largestBitrate))
                what = Answer::busy;
            asked.push_back(probe);
            Stream stream;
            switch (what) {
            case Answer::healthy: return healthyStream(probe);
            case Answer::busy:
                stream.status = VideoEncodeStatus::exhausted;
                stream.diagnostic = "VTCompressionSessionCreate -12903";
                return stream;
            case Answer::refused:
                stream.status = VideoEncodeStatus::unsupported;
                stream.diagnostic = "cannot set ProfileLevel";
                return stream;
            }
            return stream;
        };
        h.countSessions = [this](const VideoEncodeSettings&) {
            ++sessionCalls;
            if (onSessions) onSessions();
            return sessions.load();
        };
        h.hardwareListed = [this](VideoCodec codec) -> std::optional<bool> {
            if (codec == VideoCodec::h264) {
                ++discoveries;
                if (onListed) onListed();
            }
            if (listFails) return std::nullopt;
            if (codec == VideoCodec::h264) ++listedQueries;
            return codec == VideoCodec::h264 && hasEncoder;
        };
        h.listedProfiles = [](VideoCodec) { return std::set<std::string> {}; };
        h.pause = [this](unsigned milliseconds) {
            ++pauses;
            clock += milliseconds;
        };
        h.now = [this] { return clock.load(); };
        h.waitMilliseconds = waitMilliseconds;
        h.spawn = [this](std::function<void()> task) {
            ++spawnAttempts;
            if (spawnFails) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            std::lock_guard lock(threadsMutex_);
            threads_.emplace_back(std::move(task));
        };
        h.load = [this]() -> std::optional<Discovery> {
            ++loads;
            return cache;
        };
        h.store = [this](const Discovery& discovery) { stored.push_back(discovery); };
        return h;
    }

    [[nodiscard]] bool askedFor(RawVideoLayout layout) const
    {
        return std::any_of(asked.begin(), asked.end(), [&](const Probe& probe) { return probe.settings.input == layout; });
    }

    // Waits for every discovery thread started so far, and for the ones they start.
    void join()
    {
        for (;;) {
            std::thread thread;
            {
                std::lock_guard lock(threadsMutex_);
                if (threads_.empty()) return;
                thread = std::move(threads_.back());
                threads_.pop_back();
            }
            thread.join();
        }
    }

    Host() = default;
    Host(const Host&) = delete;
    ~Host() { join(); }

private:
    std::mutex threadsMutex_;
    std::vector<std::thread> threads_;
};

const VideoEncodeCapability* h264Of(const Discovery& discovery)
{
    const auto* entry = discovery.find(VideoCodec::h264);
    return entry ? &entry->capability : nullptr;
}

const VideoEncodeProfileCapability* profileOf(const Discovery& discovery, VideoEncodeProfile profile)
{
    const auto* capability = h264Of(discovery);
    if (!capability) return nullptr;
    for (const auto& entry : capability->profiles)
        if (entry.profile == profile) return &entry;
    return nullptr;
}

bool lists(const VideoEncodeProfileCapability& profile, RawVideoLayout layout)
{
    return std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end();
}

bool has(const std::vector<VideoRateControl>& modes, VideoRateControl mode)
{
    return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

bool permitsSize(const VideoEncodeCapability& capability, std::uint32_t width, std::uint32_t height)
{
    return width >= capability.minimumWidth && width <= capability.maximumWidth && height >= capability.minimumHeight &&
           height <= capability.maximumHeight && width % capability.sizeAlignment == 0 && height % capability.sizeAlignment == 0;
}

// Every profile lists the two packed orders together or neither.
void requirePackedPair(const Discovery& discovery, const char* what)
{
    const auto* capability = h264Of(discovery);
    require(capability != nullptr, what);
    for (const auto& profile : capability->profiles)
        require(lists(profile, RawVideoLayout::bgra) == lists(profile, RawVideoLayout::rgba), what);
}

constexpr unsigned kUnansweredLimit = 4;   // the Prober's limit of consecutive probes with no answer
constexpr unsigned kRetries = 3;           // retries of a probe with no answer
constexpr std::uint64_t kInterval = DiscoveryService::kRerunIntervalMilliseconds;

// ---- A host that answers everything.

void healthyHost()
{
    Host host;
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete && discovery.unanswered == 0, "a host that answers everything gives a complete discovery");
    const auto* capability = h264Of(discovery);
    require(capability != nullptr, "an H.264 capability");
    require(capability->profiles.size() == 7, "every candidate profile proved");
    requirePackedPair(discovery, "a profile lists BGRA and RGBA together");
    require(profileOf(discovery, VideoEncodeProfile::h264High)->inputs.size() == 4, "nv12, i420 and both packed orders");
    // One probe decides both packed orders.
    require(host.askedFor(RawVideoLayout::bgra), "BGRA is probed");
    require(!host.askedFor(RawVideoLayout::rgba), "and RGBA is not probed on its own");
    // The ceilings of the host end the probes that reach them, and are facts.
    require(capability->maximumWidth == 4096 && capability->maximumHeight == 2304, "the size ladder ended at the host's ceiling");
    require(capability->maximumBitrate == 100'000'000ull, "the bitrate search ended at the host's ceiling");
    require(capability->concurrentSessions == 8, "sessions");
    require(capability->rateControls.size() == 4, "every rate control mode takes effect");
    // A ceiling is asked again kRetries times, and only then taken for what it is: three ceilings.
    require(host.pauses == 3 * kRetries, "a ceiling is asked again before it is taken for what it is");
    require(discovery.unansweredCodecs.empty(), "no codec is uncertain");
}

// ---- One probe decides the packed pair, so a failure of it takes both or neither.

void packedPairSurvivesATransientFailure()
{
    // The first answer for BGRA on Main is busy; the retry is answered.
    Host host;
    host.answer = [&host](const Probe& probe, std::size_t) {
        if (Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra) && !host.askedBefore(probe)) return Answer::busy;
        return Answer::healthy;
    };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete, "a probe answered on its retry leaves the discovery complete");
    const auto* main = profileOf(discovery, VideoEncodeProfile::h264Main);
    require(main && lists(*main, RawVideoLayout::bgra) && lists(*main, RawVideoLayout::rgba), "both packed orders are listed");
    requirePackedPair(discovery, "the pair");
}

void aProbeAnsweredOnALaterRetryIsNotARefusal()
{
    // The host is busy for the first `busyFor` attempts of BGRA on Main, with the control healthy
    // in between. Every retry there is gets its chance before the probe is taken for a refusal.
    for (const unsigned busyFor : {2u, 3u}) {
        Host host;
        unsigned attempts = 0;
        host.answer = [&](const Probe& probe, std::size_t) {
            if (Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra)) return ++attempts <= busyFor ? Answer::busy : Answer::healthy;
            return Answer::healthy;
        };
        const auto discovery = runDiscovery(host.hooks());
        const auto* main = profileOf(discovery, VideoEncodeProfile::h264Main);
        require(discovery.complete && main && lists(*main, RawVideoLayout::bgra) && lists(*main, RawVideoLayout::rgba),
                "a probe that is answered on its third or fourth attempt is not a refusal");
        require(attempts == busyFor + 1, "and is not asked again");
    }
    // Busy on every attempt, next to a healthy control, is the host's refusal.
    Host host;
    host.answer = [](const Probe& probe, std::size_t) {
        return Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra) ? Answer::busy : Answer::healthy;
    };
    const auto discovery = runDiscovery(host.hooks());
    const auto* main = profileOf(discovery, VideoEncodeProfile::h264Main);
    require(discovery.complete && main && !lists(*main, RawVideoLayout::bgra) && !lists(*main, RawVideoLayout::rgba),
            "a probe that fails every attempt next to a healthy control is a refusal");
}

void packedPairRefusedTogether()
{
    // The host refuses BGRA on High every time while it answers the others.
    Host host;
    host.answer = [](const Probe& probe, std::size_t) {
        return Host::is(probe, VideoEncodeProfile::h264High, RawVideoLayout::bgra) ? Answer::busy : Answer::healthy;
    };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete, "a refusal the host keeps making, next to probes it answers, is an answer");
    const auto* high = profileOf(discovery, VideoEncodeProfile::h264High);
    require(high && !lists(*high, RawVideoLayout::bgra) && !lists(*high, RawVideoLayout::rgba), "neither packed order is listed");
    require(lists(*high, RawVideoLayout::nv12), "the other layouts are");
    requirePackedPair(discovery, "the pair");
}

// ---- A host that stops answering.

void retriesAreBoundedAndTheDiscoveryIsIncomplete()
{
    // From the second probe on, the host answers nothing.
    Host host;
    host.answer = [](const Probe&, std::size_t call) { return call >= 1 ? Answer::busy : Answer::healthy; };
    const auto discovery = runDiscovery(host.hooks());
    require(!discovery.complete, "probes with no answer make the discovery incomplete");
    require(discovery.unanswered == kUnansweredLimit, "the codec is given up after consecutive probes with no answer");
    // Each probe: asked, asked again kRetries times, and then the control.
    require(host.asked.size() == 1 + kUnansweredLimit * (1 + kRetries + 1), "every probe is asked a bounded number of times");
    require(host.pauses == kUnansweredLimit * kRetries, "with a pause before each retry");
    require(h264Of(discovery) == nullptr, "a codec nothing was proved for is not listed");
    require(discovery.unansweredCodecs == std::vector<VideoCodec> {VideoCodec::h264}, "and the codec is the uncertain one");
}

void aBusyHostIsNoAnswerWithoutAControl()
{
    // Nothing ever proves, so there is nothing to tell a refusal from.
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::busy; };
    const auto discovery = runDiscovery(host.hooks());
    require(!discovery.complete && discovery.codecs.empty(), "a host that never answers is not a host that refuses");
    require(host.asked.size() == kUnansweredLimit * (1 + kRetries), "and is asked a bounded number of times");
}

void aRefusalIsAnAnswerWithoutAControl()
{
    // A host that refuses every configuration says so.
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::refused; };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete && discovery.codecs.empty(), "a host that refuses every configuration is complete and lists nothing");
    require(host.pauses == 0, "a refusal is not asked again");
}

void aFirstRefusalIsRevisitedOnceThereIsAControl()
{
    // The very first probe of the codec is busy every time. There is no control to tell that from a
    // busy host until the next probe proves; the first probe is asked again then.
    Host host;
    host.answer = [](const Probe& probe, std::size_t) {
        return Host::is(probe, VideoEncodeProfile::h264Baseline, RawVideoLayout::nv12) ? Answer::busy : Answer::healthy;
    };
    const auto discovery = runDiscovery(host.hooks());
    require(discovery.complete && discovery.unanswered == 0, "a first refusal is settled once there is a control");
    const auto* baseline = profileOf(discovery, VideoEncodeProfile::h264Baseline);
    require(baseline && !lists(*baseline, RawVideoLayout::nv12) && lists(*baseline, RawVideoLayout::i420), "as a refusal");
}

// ---- The result of the process.

void incompleteIsNeverStoredAndIsAskedAgain()
{
    Host host;
    std::atomic<bool> busy {true};
    host.answer = [&busy](const Probe&, std::size_t) { return busy ? Answer::busy : Answer::healthy; };
    auto service = makeDiscoveryService(host.hooks());
    const auto first = service->query();
    require(first.empty(), "the discovery that found no answer lists nothing");
    require(host.stored.empty(), "and is not stored");
    require(host.listedQueries == 1, "one discovery so far");
    require(!service->current()->complete, "an incomplete answer is the process's answer for now");

    // Not in the same busy moment.
    busy = false;
    (void)service->query();
    require(host.listedQueries == 1, "a query right after the first does not run discovery again");

    host.clock += kInterval;
    const auto second = service->query();
    require(host.listedQueries == 2, "a later query runs it again");
    require(second.size() == 1 && second[0].codec == VideoCodec::h264, "the complete result is the answer");
    require(host.stored.size() == 1 && host.stored[0].complete, "and the complete result is stored");
    require(service->current()->complete, "it replaced the incomplete one");

    // Settled: no more runs.
    host.clock += 10 * kInterval;
    (void)service->query();
    (void)service->query();
    require(host.listedQueries == 2 && host.stored.size() == 1, "a complete answer is final");
}

void rerunsAreBounded()
{
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::busy; };
    auto service = makeDiscoveryService(host.hooks());
    for (unsigned query = 0; query < 10; ++query) {
        (void)service->query();
        host.clock += kInterval;
    }
    require(host.listedQueries == DiscoveryService::kMaximumRuns, "a process runs discovery a bounded number of times");
    require(host.stored.empty(), "and stores none of the incomplete results");
    require(!service->current()->complete, "the answer stays the incomplete one");
}

void rerunsAreBoundedUnderConcurrentQueries()
{
    Host host;
    host.answer = [](const Probe&, std::size_t) { return Answer::busy; };
    auto service = makeDiscoveryService(host.hooks());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (unsigned query = 0; query < 12; ++query) {
                host.clock += kInterval;
                (void)service->query();
            }
        });
    for (auto& thread : threads) thread.join();
    require(host.discoveries <= DiscoveryService::kMaximumRuns, "concurrent queries start no more discoveries than the bound");
    require(host.stored.empty(), "and store none of the incomplete results");
}

void aRefusalIsStored()
{
    Host host;
    host.answer = [](const Probe& probe, std::size_t) {
        return probe.settings.profile == VideoEncodeProfile::h264High422 ? Answer::refused : Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(host.stored.size() == 1, "what the host refuses is an answer, and the complete result is stored");
    require(profileOf(host.stored[0], VideoEncodeProfile::h264High422) == nullptr, "without the refused profile");
    require(profileOf(host.stored[0], VideoEncodeProfile::h264High) != nullptr, "and with the others");
    require(capabilities.size() == 1, "the answer");
    host.clock += 10 * kInterval;
    (void)service->query();
    require(host.listedQueries == 1, "a refusal is not asked again");
}

void aLowBitrateCeilingStillListsTheCodec()
{
    // The host takes 6 Mbit/s and no more: the codec has a ceiling, and is listed with it.
    Host host;
    host.largestBitrate = 6'000'000ull;
    const auto discovery = runDiscovery(host.hooks());
    const auto* capability = h264Of(discovery);
    require(discovery.complete && capability != nullptr, "a codec whose bitrate ceiling is low is listed");
    require(capability->maximumBitrate == 5'000'000ull, "with the highest rung it took");
}

void aCeilingIsStored()
{
    Host host;
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(host.stored.size() == 1 && host.stored[0].complete, "the ceilings of a host are an answer, and the result is stored");
    require(capabilities.size() == 1 && capabilities[0].maximumWidth == 4096 && capabilities[0].maximumBitrate == 100'000'000ull,
            "with the ceilings in it");
}

void aMixedRunKeepsItsCeilingsAndIsAskedAgain()
{
    // The host is busy for exactly the calls of one probe: BGRA on Main.
    Host host;
    unsigned busyLeft = 0;
    bool triggered = false;
    host.answer = [&](const Probe& probe, std::size_t) {
        if (!triggered && Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra)) {
            triggered = true;
            busyLeft = 1 + kRetries + 1;
        }
        if (busyLeft) {
            --busyLeft;
            return Answer::busy;
        }
        return Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    const auto first = service->query();
    const auto early = service->current();
    require(!early->complete && early->unanswered == 1, "one probe with no answer");
    require(host.stored.empty(), "is enough to keep the result out of the cache");
    requirePackedPair(*early, "the incomplete result keeps the pair together");
    const auto* main = profileOf(*early, VideoEncodeProfile::h264Main);
    require(main && !lists(*main, RawVideoLayout::bgra) && !lists(*main, RawVideoLayout::rgba) && lists(*main, RawVideoLayout::nv12),
            "it lacks the layout of the probe that had no answer, both orders of it");
    require(first.size() == 1 && first[0].maximumWidth == 4096 && first[0].maximumBitrate == 100'000'000ull,
            "while the ceilings, answered in the same run, are in it");

    host.clock += kInterval;
    const auto second = service->query();
    require(host.stored.size() == 1, "the run that was whole is stored");
    const auto* better = profileOf(*service->current(), VideoEncodeProfile::h264Main);
    require(better && lists(*better, RawVideoLayout::bgra) && lists(*better, RawVideoLayout::rgba), "with both orders");
    require(second.size() == 1 && second[0].maximumWidth == 4096, "and the same ceilings");
    // What the first answer promised is still promised: the better answer lists a superset.
    for (const auto& profile : early->find(VideoCodec::h264)->capability.profiles) {
        const auto* later = profileOf(*service->current(), profile.profile);
        require(later != nullptr, "a profile of the incomplete answer is in the complete one");
        for (const auto layout : profile.inputs) require(lists(*later, layout), "and so is each layout");
    }
}

void aReplacementNeverShrinksTheAnswer()
{
    // Run 1 offers Main/BGRA, a large picture and bitrate and constant bitrate, and has no answer
    // for High/BGRA. Run 2 is whole and says the opposite about each of them, so it does not contain
    // run 1: the answer stays run 1's entry, as that run described it.
    Host host;
    bool secondRun = false, triggered = false;
    unsigned busyLeft = 0;
    host.answer = [&](const Probe& probe, std::size_t) {
        if (!secondRun) {
            if (!triggered && Host::is(probe, VideoEncodeProfile::h264High, RawVideoLayout::bgra)) {
                triggered = true;
                busyLeft = 1 + kRetries + 1;
            }
            if (busyLeft) {
                --busyLeft;
                return Answer::busy;
            }
            return Answer::healthy;
        }
        if (Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra)) return Answer::busy;
        if (probe.settings.rateControl == VideoRateControl::constantBitrate) return Answer::refused;
        return Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    (void)service->query();
    const auto early = service->current();
    require(!early->complete, "run 1 is incomplete");
    require(lists(*profileOf(*early, VideoEncodeProfile::h264Main), RawVideoLayout::bgra), "and offers Main/BGRA");
    require(!lists(*profileOf(*early, VideoEncodeProfile::h264High), RawVideoLayout::bgra), "and not High/BGRA");
    require(has(h264Of(*early)->rateControls, VideoRateControl::constantBitrate), "and constant bitrate");

    secondRun = true;
    host.largestSamples = 1920ull * 1080ull;
    host.largestBitrate = 50'000'000ull;
    host.clock += kInterval;
    (void)service->query();
    const auto later = service->current();
    require(later->complete, "run 2 is whole");
    require(host.stored.size() == 1 && !lists(*profileOf(host.stored[0], VideoEncodeProfile::h264Main), RawVideoLayout::bgra) &&
                lists(*profileOf(host.stored[0], VideoEncodeProfile::h264High), RawVideoLayout::bgra) &&
                h264Of(host.stored[0])->maximumWidth == 1920 && !has(h264Of(host.stored[0])->rateControls, VideoRateControl::constantBitrate),
            "the cache holds run 2 as it measured it");
    // What the answer offered is still accepted, as run 1 described it.
    require(lists(*profileOf(*later, VideoEncodeProfile::h264Main), RawVideoLayout::bgra) &&
                lists(*profileOf(*later, VideoEncodeProfile::h264Main), RawVideoLayout::rgba),
            "a layout that was offered is still accepted");
    require(h264Of(*later)->maximumWidth == 4096 && h264Of(*later)->maximumHeight == 2304, "a size that was offered is still accepted");
    require(h264Of(*later)->maximumBitrate == 100'000'000ull, "a bitrate too");
    require(has(h264Of(*later)->rateControls, VideoRateControl::constantBitrate), "and a rate control");
    // And nothing of run 2 is mixed into it: the entry is run 1's own.
    require(!lists(*profileOf(*later, VideoEncodeProfile::h264High), RawVideoLayout::bgra), "run 2's layouts are not added to run 1's entry");
}

void aReplacementDoesNotInventSizes()
{
    // Run 1 has no answer for the smallest picture and lists 96..4096; run 2 lists 64..1024 and refuses
    // pictures that are wide and short. Neither lists 4096x64, and no combination of them does.
    Host host;
    host.largestSamples = 4096ull * 4096ull;
    bool secondRun = false, triggered = false;
    unsigned busyLeft = 0;
    host.answer = [&](const Probe& probe, std::size_t) {
        const auto& s = probe.settings;
        if (!secondRun && !triggered && s.width == 64 && s.height == 64) {
            triggered = true;
            busyLeft = 1 + kRetries + 1;
        }
        if (busyLeft) {
            --busyLeft;
            return Answer::busy;
        }
        if ((s.width > 1024 && s.height == 64) || (s.height > 1024 && s.width == 64)) return Answer::refused;
        return Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    (void)service->query();
    const auto early = service->current();
    require(!early->complete && h264Of(*early)->minimumHeight == 96 && h264Of(*early)->maximumWidth == 4096, "run 1 lists 96..4096");
    secondRun = true;
    host.clock += kInterval;
    (void)service->query();
    require(host.stored.size() == 1, "run 2 is whole");
    const auto& fresh = *h264Of(host.stored[0]);
    require(fresh.minimumHeight == 64 && fresh.maximumWidth == 1024, "and lists 64..1024");
    const auto& answer = *h264Of(*service->current());
    require(!permitsSize(*h264Of(*early), 4096, 64) && !permitsSize(fresh, 4096, 64), "neither run lists 4096x64");
    require(!permitsSize(answer, 4096, 64), "so the answer does not");
    require(answer.maximumWidth == 4096 && answer.minimumHeight == 96, "it is run 1's entry, which run 2 does not contain");
}

void aReplacementDoesNotInventControlCombinations()
{
    // Run 1 has no answer for constant bitrate and takes 100 Mbit/s; run 2 has constant bitrate and takes
    // 2 Mbit/s. Constant bitrate at 100 Mbit/s is in neither, and the host refuses it.
    Host host;
    bool secondRun = false, triggered = false;
    unsigned busyLeft = 0;
    host.answer = [&](const Probe& probe, std::size_t) {
        const auto& s = probe.settings;
        if (!secondRun && !triggered && s.rateControl == VideoRateControl::constantBitrate && probe.frames == 4) {
            triggered = true;
            busyLeft = 1 + kRetries + 1;
        }
        if (busyLeft) {
            --busyLeft;
            return Answer::busy;
        }
        if (s.rateControl == VideoRateControl::constantBitrate && s.bitrate > 8'000'000) return Answer::refused;
        if (secondRun && s.rateControl == VideoRateControl::variableBitrate && probe.frames == 4 && !s.minimumQp && s.bitrate > 2'000'000)
            return Answer::refused;
        return Answer::healthy;
    };
    const auto permits = [](const VideoEncodeCapability& capability) {
        return capability.maximumBitrate >= 100'000'000ull && has(capability.rateControls, VideoRateControl::constantBitrate);
    };
    auto service = makeDiscoveryService(host.hooks());
    (void)service->query();
    const auto early = service->current();
    require(!early->complete && h264Of(*early)->maximumBitrate == 100'000'000ull && !has(h264Of(*early)->rateControls, VideoRateControl::constantBitrate),
            "run 1: 100 Mbit/s, no constant bitrate");
    secondRun = true;
    host.clock += kInterval;
    (void)service->query();
    require(host.stored.size() == 1 && h264Of(host.stored[0])->maximumBitrate == 2'000'000ull &&
                has(h264Of(host.stored[0])->rateControls, VideoRateControl::constantBitrate),
            "run 2: 2 Mbit/s, constant bitrate");
    require(!permits(*h264Of(*early)) && !permits(*h264Of(host.stored[0])), "neither run lists constant bitrate at 100 Mbit/s");
    require(!permits(*h264Of(*service->current())), "so the answer does not");
}

void aResultThatContainsTheAnswerReplacesIt()
{
    // Run 1 has no answer for Main/BGRA and measures one session; run 2 is whole, lists that layout too
    // and measures three. It contains run 1, so it replaces the entry, as run 2 measured it.
    Host host;
    bool secondRun = false, triggered = false;
    unsigned busyLeft = 0;
    host.sessions = 1;
    host.answer = [&](const Probe& probe, std::size_t) {
        if (!secondRun && !triggered && Host::is(probe, VideoEncodeProfile::h264Main, RawVideoLayout::bgra)) {
            triggered = true;
            busyLeft = 1 + kRetries + 1;
        }
        if (busyLeft) {
            --busyLeft;
            return Answer::busy;
        }
        return Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    (void)service->query();
    require(!service->current()->complete && h264Of(*service->current())->concurrentSessions == 1, "run 1");
    secondRun = true;
    host.sessions = 3;
    host.clock += kInterval;
    (void)service->query();
    require(host.stored.size() == 1, "run 2 is whole");
    const auto& answer = *h264Of(*service->current());
    const auto& run2 = *h264Of(host.stored[0]);
    require(answer.profiles.size() == run2.profiles.size() && answer.concurrentSessions == 3 && answer.maximumWidth == run2.maximumWidth &&
                answer.rateControls == run2.rateControls,
            "the answer is run 2's entry");
    require(lists(*profileOf(*service->current(), VideoEncodeProfile::h264Main), RawVideoLayout::bgra), "with the layout run 1 lacked");
}

void allBitrateTargetsFail()
{
    // The four-frame variable-bitrate trial works at 2 Mbit/s. Every target above it is refused next to a
    // healthy control, and the 2 Mbit/s one would be too if it were asked again: the codec is not absent.
    Host host;
    unsigned floorCalls = 0;
    host.answer = [&](const Probe& probe, std::size_t) {
        const auto& s = probe.settings;
        if (s.profile == VideoEncodeProfile::h264High && s.input == RawVideoLayout::nv12 && s.width == 128 && s.height == 96 &&
            probe.frames == 4 && s.rateControl == VideoRateControl::variableBitrate && !s.minimumQp && !s.maximumQp) {
            if (s.bitrate > 2'000'000) return Answer::refused;
            if (s.bitrate == 2'000'000 && ++floorCalls > 1) return Answer::busy;
        }
        return Answer::healthy;
    };
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(capabilities.size() == 1 && capabilities[0].codec == VideoCodec::h264, "a codec that encodes at 2 Mbit/s is listed");
    require(capabilities[0].maximumBitrate == 2'000'000ull, "with the rate that worked as its ceiling");
    require(host.stored.size() == 1 && h264Of(host.stored[0]) != nullptr, "and it is not stored as absent");
    require(floorCalls == 1, "the rate that worked is not asked again");
}

// ---- The first discovery and a later one are one discovery at a time.

void anInitialRunThatOutlastsTheWaitCannotOverwriteALaterAnswer()
{
    Host host;
    host.waitMilliseconds = 150;
    Gate gate;
    std::atomic<bool> busy {true};
    host.answer = [&busy](const Probe&, std::size_t) { return busy ? Answer::busy : Answer::healthy; };
    bool blocked = false;
    host.onListed = [&] {
        if (blocked) return;
        blocked = true;
        gate.wait();
    };
    auto service = makeDiscoveryService(host.hooks());
    require(service->query().empty(), "the first query waits, and then answers with nothing");
    host.clock += kInterval;
    require(service->query().empty(), "so does the second");
    require(host.discoveries == 1, "no second discovery starts while the first is still running");
    gate.open();
    // The first discovery ends, with no answer of the host: it is the answer for now.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (service->current()->codecs.empty() && service->current()->complete == true && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(!service->current()->complete, "the first discovery's own result is published");
    busy = false;
    host.clock += kInterval;
    const auto third = service->query();
    require(host.discoveries == 2 && third.size() == 1 && service->current()->complete, "the next query's discovery is the second, and whole");
    host.clock += kInterval;
    (void)service->query();
    require(service->current()->complete && host.discoveries == 2 && host.stored.size() == 1, "nothing replaces a complete answer");
}

void aQueryDuringARefreshAnswersWithTheAnswerItHas()
{
    Host host;
    Gate gate;
    std::atomic<bool> busy {true};
    host.answer = [&busy](const Probe&, std::size_t) { return busy ? Answer::busy : Answer::healthy; };
    std::atomic<bool> blockNext {};
    host.onListed = [&] {
        if (blockNext.exchange(false)) gate.wait();
    };
    auto service = makeDiscoveryService(host.hooks());
    require(service->query().empty(), "the first answer is incomplete");
    busy = false;
    blockNext = true;
    host.clock += kInterval;
    auto refreshing = std::async(std::launch::async, [&] { return service->query(); });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (host.discoveries < 2 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(host.discoveries == 2, "the refresh is running");
    // Another query does not wait for it, does not start another, and does not mistake its snapshot for the new one.
    auto other = std::async(std::launch::async, [&] { return service->query(); });
    require(other.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "a query while a refresh runs is not held up by it");
    require(other.get().empty() && host.discoveries == 2, "it answers with what there is and starts nothing");
    gate.open();
    require(refreshing.get().size() == 1, "the query that ran the refresh answers with the refresh");
    require(service->query().size() == 1 && host.discoveries == 2, "and so does the next");
}

// ---- A discovery that cannot get a thread.

template<class T> bool readyWithin(std::future<T>& future, unsigned seconds)
{
    return future.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready;
}

void aFirstDiscoveryThatCannotGetAThreadIsTriedAgain()
{
    Host host;
    host.spawnFails = true;
    auto service = makeDiscoveryService(host.hooks());
    auto first = std::async(std::launch::async, [&] { return service->query(); });
    require(readyWithin(first, 10), "a query whose discovery cannot start is not left waiting");
    require(first.get().empty() && host.discoveries == 0 && !service->current()->complete, "it answers with nothing, as an incomplete answer");
    host.spawnFails = false;
    host.clock += kInterval;
    const auto later = service->query();
    require(later.size() == 1 && service->current()->complete && host.discoveries == 1, "a later query starts the discovery that never ran");
}

void aRefreshThatCannotGetAThreadKeepsTheAnswerAndIsTriedAgain()
{
    Host host;
    std::atomic<bool> busy {true};
    host.answer = [&busy](const Probe&, std::size_t) { return busy ? Answer::busy : Answer::healthy; };
    auto service = makeDiscoveryService(host.hooks());
    require(service->query().empty() && host.discoveries == 1, "the first answer is incomplete");
    busy = false;
    host.spawnFails = true;
    host.clock += kInterval;
    auto failing = std::async(std::launch::async, [&] { return service->query(); });
    require(readyWithin(failing, 10), "a refresh that cannot start does not hold its query");
    require(failing.get().empty() && host.discoveries == 1 && !service->current()->complete, "the answer stays");
    host.spawnFails = false;
    host.clock += kInterval;
    const auto later = service->query();
    require(host.discoveries == 2 && later.size() == 1 && service->current()->complete, "a later query tries again, and the discovery runs");
}

void aDiscoveryThatNeverGetsAThreadIsTriedABoundedNumberOfTimes()
{
    Host host;
    host.spawnFails = true;
    auto service = makeDiscoveryService(host.hooks());
    for (unsigned query = 0; query < 10; ++query) {
        (void)service->query();
        host.clock += kInterval;
    }
    require(host.spawnAttempts == DiscoveryService::kMaximumRuns, "a host with no thread is asked a bounded number of times");
}

// ---- What is measured in each process.

Discovery cachedH264(unsigned sessions)
{
    Discovery cached;
    CodecDiscovery entry;
    entry.capability.codec = VideoCodec::h264;
    entry.capability.concurrentSessions = sessions;
    entry.capability.profiles.push_back({VideoEncodeProfile::h264High, {RawVideoLayout::nv12}, 52, true});
    cached.codecs.push_back(entry);
    return cached;
}

unsigned sessionsOf(DiscoveryService& service)
{
    return h264Of(*service.current())->concurrentSessions;
}

void aCacheHitOpensNoSessionBeforeTheFirstAnswer()
{
    Host host;
    host.cache = cachedH264(1);
    host.sessions = 8;
    Gate gate;
    host.onSessions = [&] { gate.wait(); };
    auto service = makeDiscoveryService(host.hooks());
    auto first = std::async(std::launch::async, [&] { return service->query(); });
    require(first.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "the first query is not held up by the measurement of the sessions");
    const auto capabilities = first.get();
    require(capabilities.size() == 1 && capabilities[0].concurrentSessions == 1, "it is answered with the count the cache carries");
    require(host.asked.empty() && host.stored.empty() && service->current()->complete, "and nothing was probed or stored");
    // A discovery that is still measuring is the one that runs: nothing else starts.
    host.clock += 10 * kInterval;
    (void)service->query();
    require(host.discoveries == 0 && host.loads == 1, "no discovery and no second read of the file starts meanwhile");
    gate.open();
    host.join();
}

void aCachedCountBelowTheCapRisesInTheProcessAndInTheFile()
{
    // The process that found the sessions held measured one; the host has eight free now.
    Host first;
    first.sessions = 1;
    auto fresh = makeDiscoveryService(first.hooks());
    require(fresh->query().size() == 1 && first.stored.size() == 1 && first.stored[0].codecs[0].capability.concurrentSessions == 1,
            "the first process found one and stored it");
    first.join();

    Host next;
    next.sessions = 8;
    next.cache = first.stored[0];
    auto service = makeDiscoveryService(next.hooks());
    const auto early = service->query()[0].concurrentSessions;
    require(early == 1 || early == 8, "the answer carries the cache's count, or the raised one");
    next.join();
    require(sessionsOf(*service) == 8, "the count of the process rose");
    require(next.sessionCalls == 1, "it was measured once");
    require(next.stored.size() == 1 && next.stored[0].codecs[0].capability.concurrentSessions == 8, "and the file's count was rewritten");
    require(next.asked.empty(), "without a trial encode");
    (void)service->query();
    require(next.sessionCalls == 1, "a later query measures nothing");
}

void aCountAtTheCapIsNotMeasured()
{
    Host host;
    host.cache = cachedH264(16);
    auto service = makeDiscoveryService(host.hooks());
    require(service->query()[0].concurrentSessions == 16, "the cached count is the answer");
    host.join();
    require(host.sessionCalls == 0 && host.stored.empty() && host.asked.empty(), "a codec cached at the cap opens no session and rewrites nothing");
}

void aLowerMeasurementChangesNothing()
{
    Host host;
    host.cache = cachedH264(4);
    host.sessions = 2;
    auto service = makeDiscoveryService(host.hooks());
    (void)service->query();
    host.join();
    require(host.sessionCalls == 1, "the count is measured");
    require(sessionsOf(*service) == 4, "a lower count does not lower the published one");
    require(host.stored.empty(), "and the file is not rewritten");

    Host none;
    none.cache = cachedH264(4);
    none.sessions = 0;
    auto other = makeDiscoveryService(none.hooks());
    require(other->query()[0].concurrentSessions == 4, "a measurement that opens no session is dropped");
    none.join();
    require(sessionsOf(*other) == 4 && none.stored.empty() && other->current()->complete, "and the answer stays complete");
}

// ---- The call that lists the encoders.

void aFailedEncoderListIsNoAnswer()
{
    Host host;
    host.listFails = true;
    auto service = makeDiscoveryService(host.hooks());
    require(service->query().empty(), "no encoder is offered");
    const auto answer = service->current();
    require(!answer->complete && answer->unanswered == 3 && answer->unansweredCodecs.size() == 3, "when the call fails, each codec is unanswered");
    require(host.stored.empty(), "and nothing is stored");
    host.listFails = false;
    host.clock += kInterval;
    require(service->query().size() == 1 && service->current()->complete && host.stored.size() == 1, "a later query asks again");
}

void anEmptyEncoderListIsAnAnswer()
{
    Host host;
    host.hasEncoder = false;
    auto service = makeDiscoveryService(host.hooks());
    require(service->query().empty() && service->current()->complete && service->current()->unanswered == 0,
            "a list that names no encoder is the host's answer");
    require(host.stored.size() == 1 && host.stored[0].codecs.empty(), "and it is stored");
}

// ---- What a codec that is uncertain excuses.

VideoEncodeCapability capabilityOf(VideoCodec codec, VideoEncodeProfile profile, std::vector<RawVideoLayout> inputs)
{
    VideoEncodeCapability capability;
    capability.codec = codec;
    capability.minimumWidth = capability.minimumHeight = 64;
    capability.maximumWidth = capability.maximumHeight = 1920;
    capability.concurrentSessions = 2;
    capability.maximumBitrate = 20'000'000;
    capability.maximumQp = 51;
    const bool jpeg = codec == VideoCodec::jpeg;
    capability.rateControls = {jpeg ? VideoRateControl::constantQuality : VideoRateControl::variableBitrate};
    capability.profiles.push_back({profile, std::move(inputs), jpeg ? 0u : 120u, false});
    return capability;
}

void anUncertainCodecDoesNotExcuseAnother()
{
    using uml::testing::video_encoder::capabilityViolation;
    const auto jpeg = capabilityOf(VideoCodec::jpeg, VideoEncodeProfile::jpegBaseline, {RawVideoLayout::nv12});
    // HEVC Main 10 without 10-bit input is a defect of HEVC.
    const auto main10 = capabilityOf(VideoCodec::hevc, VideoEncodeProfile::hevcMain10, {RawVideoLayout::nv12});
    require(capabilityViolation({main10, jpeg}, {}) != nullptr, "the defect is one");
    require(capabilityViolation({main10, jpeg}, {VideoCodec::jpeg}) != nullptr, "while JPEG is uncertain, it still is");
    require(capabilityViolation({main10, jpeg}, {VideoCodec::hevc}) == nullptr, "and HEVC being uncertain excuses it");
    // The packed pair is never excused.
    const auto lopsided = capabilityOf(VideoCodec::h264, VideoEncodeProfile::h264High, {RawVideoLayout::nv12, RawVideoLayout::bgra});
    require(capabilityViolation({lopsided}, {VideoCodec::h264}) != nullptr, "one packed order alone is a defect of any result");
}

void aCachedResultRunsNothing()
{
    Host host;
    host.cache = cachedH264(16);
    auto service = makeDiscoveryService(host.hooks());
    const auto capabilities = service->query();
    require(capabilities.size() == 1 && capabilities[0].profiles.size() == 1, "the cached capabilities");
    host.join();
    require(host.asked.empty() && host.listedQueries == 0 && host.stored.empty(), "a valid cache is neither probed nor rewritten");
}

} // namespace

int main(int argc, char** argv)
{
    struct Case {
        const char* name;
        void (*run)();
    };
    static const Case cases[] = {
        {"healthyHost", healthyHost},
        {"packedPairSurvivesATransientFailure", packedPairSurvivesATransientFailure},
        {"aProbeAnsweredOnALaterRetryIsNotARefusal", aProbeAnsweredOnALaterRetryIsNotARefusal},
        {"packedPairRefusedTogether", packedPairRefusedTogether},
        {"retriesAreBoundedAndTheDiscoveryIsIncomplete", retriesAreBoundedAndTheDiscoveryIsIncomplete},
        {"aBusyHostIsNoAnswerWithoutAControl", aBusyHostIsNoAnswerWithoutAControl},
        {"aRefusalIsAnAnswerWithoutAControl", aRefusalIsAnAnswerWithoutAControl},
        {"aFirstRefusalIsRevisitedOnceThereIsAControl", aFirstRefusalIsRevisitedOnceThereIsAControl},
        {"incompleteIsNeverStoredAndIsAskedAgain", incompleteIsNeverStoredAndIsAskedAgain},
        {"rerunsAreBounded", rerunsAreBounded},
        {"rerunsAreBoundedUnderConcurrentQueries", rerunsAreBoundedUnderConcurrentQueries},
        {"aRefusalIsStored", aRefusalIsStored},
        {"aLowBitrateCeilingStillListsTheCodec", aLowBitrateCeilingStillListsTheCodec},
        {"aCeilingIsStored", aCeilingIsStored},
        {"aMixedRunKeepsItsCeilingsAndIsAskedAgain", aMixedRunKeepsItsCeilingsAndIsAskedAgain},
        {"aReplacementNeverShrinksTheAnswer", aReplacementNeverShrinksTheAnswer},
        {"aReplacementDoesNotInventSizes", aReplacementDoesNotInventSizes},
        {"aReplacementDoesNotInventControlCombinations", aReplacementDoesNotInventControlCombinations},
        {"aResultThatContainsTheAnswerReplacesIt", aResultThatContainsTheAnswerReplacesIt},
        {"allBitrateTargetsFail", allBitrateTargetsFail},
        {"anInitialRunThatOutlastsTheWaitCannotOverwriteALaterAnswer", anInitialRunThatOutlastsTheWaitCannotOverwriteALaterAnswer},
        {"aQueryDuringARefreshAnswersWithTheAnswerItHas", aQueryDuringARefreshAnswersWithTheAnswerItHas},
        {"aFirstDiscoveryThatCannotGetAThreadIsTriedAgain", aFirstDiscoveryThatCannotGetAThreadIsTriedAgain},
        {"aRefreshThatCannotGetAThreadKeepsTheAnswerAndIsTriedAgain", aRefreshThatCannotGetAThreadKeepsTheAnswerAndIsTriedAgain},
        {"aDiscoveryThatNeverGetsAThreadIsTriedABoundedNumberOfTimes", aDiscoveryThatNeverGetsAThreadIsTriedABoundedNumberOfTimes},
        {"aCacheHitOpensNoSessionBeforeTheFirstAnswer", aCacheHitOpensNoSessionBeforeTheFirstAnswer},
        {"aCachedCountBelowTheCapRisesInTheProcessAndInTheFile", aCachedCountBelowTheCapRisesInTheProcessAndInTheFile},
        {"aCountAtTheCapIsNotMeasured", aCountAtTheCapIsNotMeasured},
        {"aLowerMeasurementChangesNothing", aLowerMeasurementChangesNothing},
        {"aFailedEncoderListIsNoAnswer", aFailedEncoderListIsNoAnswer},
        {"anEmptyEncoderListIsAnAnswer", anEmptyEncoderListIsAnAnswer},
        {"anUncertainCodecDoesNotExcuseAnother", anUncertainCodecDoesNotExcuseAnother},
        {"aCachedResultRunsNothing", aCachedResultRunsNothing},
    };
    // One case by name, for a run of the test against a changed discovery.
    const char* only = argc == 2 ? argv[1] : nullptr;
    unsigned ran = 0;
    for (const auto& entry : cases) {
        if (only && std::string(entry.name) != only) continue;
        try {
            entry.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", entry.name, error.what());
            return 1;
        }
        ++ran;
    }
    if (!ran) {
        std::fprintf(stderr, "no such case\n");
        return 2;
    }
    std::puts("video encoder discovery ok");
    return 0;
}
