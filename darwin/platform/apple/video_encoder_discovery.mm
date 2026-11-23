// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder.hpp"
#include "video_encoder_bitstream.hpp"
#include "video_encoder_internal.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>
#include <poll.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

/* What the hardware encoders of this host do, learned by asking them to encode.
 *
 * VideoToolbox lists the properties an encoder has, and the profile values it
 * knows, but no numeric range, and its lists have been wrong before (the
 * decoder's High 10 is listed on a host whose hardware rejects it). So the
 * lists only say what is worth trying. A profile, an input layout, a size or a
 * rate control mode is published when an encode that used it produced a stream
 * that says so: the profile and level in its SPS, the entropy mode in its PPS,
 * the HDR messages in its SEI.
 *
 * Every probe is the production encoder, so discovery also proves the pipeline
 * a guest's stream takes. The result is cached per host model and OS build.
 *
 * A probe that does not prove what it asked for is either a refusal of the host
 * or no answer at all: a session the busy hardware could not give, a session
 * lost, a drain that never finished. Only a refusal is a fact. Where the two
 * look alike (a refused picture size and a busy encoder both fail to create a
 * session), the probe is asked again, and it is a refusal only when a probe
 * that proved earlier in the same discovery still proves right after it. A
 * discovery with a probe left unanswered is incomplete: it is never cached, and
 * DiscoveryService computes it again later. */

namespace uml::platform {
namespace apple_encoder {
namespace {

using bitstream::Bytes;

// 5: the smallest picture side is kVideoEncodeMinimumSide; earlier results list smaller ones.
// 6: a result that lacked the answer of a probe was cached as if the host had refused it.
constexpr unsigned kDiscoveryRevision = 6;
constexpr std::chrono::seconds kDiscoveryWait {300};   // a discovery takes seconds; the bound only ends a stuck one
constexpr std::uint32_t kProbeWidth = 128;
constexpr std::uint32_t kProbeHeight = 96;
constexpr unsigned kSessionProbeLimit = 16;   // the device bounds a guest's streams at eight
constexpr std::uint64_t kHevcLevelSixLumaSamples = 35'651'584;
constexpr unsigned kProbeRetries = 3;                                  // a probe with no answer is asked this many times more
constexpr unsigned kRetryPauseMilliseconds[kProbeRetries] = {100, 250, 500};
constexpr unsigned kUnansweredLimit = 4;                               // consecutive unanswered probes that end a codec's discovery
constexpr unsigned kReportedLimit = 16;                                // unanswered probes one discovery names on stderr

// ---- Probing.

using Clock = std::chrono::steady_clock;

VideoEncodeSettings baseSettings(VideoCodec codec, VideoEncodeProfile profile, RawVideoLayout layout,
                                 std::uint32_t width = kProbeWidth, std::uint32_t height = kProbeHeight)
{
    VideoEncodeSettings settings;
    settings.codec = codec;
    settings.profile = profile;
    settings.input = layout;
    settings.width = width;
    settings.height = height;
    settings.bitrate = 2'000'000;
    settings.quality = 60;
    settings.frameQp = {26, 26, 26};
    settings.jpegQuality = 85;
    return settings;
}

class Filler {
public:
    void fill(WritableVideoFrame& frame, unsigned index, bool noisy)
    {
        if (!noisy) return;
        for (std::size_t plane = 0; plane < frame.planeCount(); ++plane) {
            auto bytes = frame.plane(plane);
            if (bytes.empty()) continue;
            if (templates_.size() <= plane) templates_.resize(plane + 1);
            auto& pattern = templates_[plane];
            if (pattern.size() != bytes.size()) {
                pattern.resize(bytes.size());
                std::uint32_t state = 0x9e3779b9u * std::uint32_t(plane + 1);
                for (std::size_t i = 0; i < pattern.size(); ++i) {
                    state ^= state << 13;
                    state ^= state >> 17;
                    state ^= state << 5;
                    // Smooth ramps with some noise on top: neither flat nor
                    // incompressible, so a rate target is actually held.
                    pattern[i] = std::uint8_t(60 + (i % frame.stride(plane)) / 3 + (state & 0x1f));
                }
            }
            // Slide the pattern by a few bytes per frame so the pictures differ.
            const std::size_t shift = (std::size_t(index) * 7) % std::max<std::size_t>(1, frame.stride(plane));
            std::memcpy(bytes.data(), pattern.data() + shift, bytes.size() - shift);
            std::memcpy(bytes.data() + (bytes.size() - shift), pattern.data(), shift);
        }
    }
private:
    std::vector<std::vector<std::uint8_t>> templates_;
};

Stream runProbe(const Probe& probe)
{
    Stream out;
    auto encoder = makeProbeEncoder();
    encoder->setDrainWait(probe.timeoutMilliseconds);
    out.status = encoder->configureUnchecked(probe.settings);
    out.diagnostic = encoder->diagnostic();
    if (out.status != VideoEncodeStatus::ok) return out;
    out.encoder = encoder->encoderIdentifier();

    Filler filler;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(probe.timeoutMilliseconds);
    unsigned submitted = 0, pictures = 0, errors = 0;
    bool drained = false, draining = false;
    while (!drained && Clock::now() < deadline) {
        while (submitted < probe.frames) {
            auto frame = encoder->acquireFrame();
            if (!frame) break;
            filler.fill(*frame, submitted, probe.noisy);
            encoder->submit(std::move(frame), submitted, false);
            ++submitted;
        }
        if (submitted == probe.frames && !draining) {
            encoder->drain();
            draining = true;
        }
        pollfd readable {encoder->readinessDescriptor(), POLLIN, 0};
        (void)::poll(&readable, 1, 20);
        for (auto& frame : encoder->takeFrames()) {
            switch (frame.kind) {
            case EncodedVideoFrameKind::frame: ++pictures; out.frames.push_back(std::move(frame)); break;
            case EncodedVideoFrameKind::parameterSets: out.frames.push_back(std::move(frame)); break;
            case EncodedVideoFrameKind::error: ++errors; break;
            case EncodedVideoFrameKind::drained: drained = true; break;
            }
        }
    }
    out.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    out.diagnostic = encoder->diagnostic();
    out.encoded = drained && !errors && pictures == probe.frames;
    return out;
}

// ---- Reading a probe's stream.

struct Facts {
    std::optional<unsigned> profileIdc;
    unsigned constraintFlags {};
    unsigned levelIdc {};
    bool highTier {};
    std::optional<bool> cabac;
    std::vector<bitstream::SeiMessage> sei;
    unsigned keyFrames {};
    unsigned longestBidirectionalRun {};
    unsigned upperLayerFrames {};
    bool sawLowerLayerFrame {};
};

Facts analyze(VideoCodec codec, const std::vector<EncodedVideoFrame>& frames)
{
    Facts facts;
    unsigned run = 0;
    bool firstKey = true;
    for (const auto& frame : frames) {
        if (frame.kind != EncodedVideoFrameKind::frame) continue;
        if (frame.type == VideoFrameType::key) ++facts.keyFrames;
        run = frame.type == VideoFrameType::bidirectional ? run + 1 : 0;
        facts.longestBidirectionalRun = std::max(facts.longestBidirectionalRun, run);
        if (frame.type != VideoFrameType::key) {
            if (frame.temporalLayer > 0) ++facts.upperLayerFrames;
            else facts.sawLowerLayerFrame = true;
        }
        if (!firstKey || frame.type != VideoFrameType::key) continue;
        firstKey = false;
        for (const auto unit : bitstream::splitAnnexB(frame.bytes)) {
            if (codec == VideoCodec::h264) {
                if (const auto sps = bitstream::parseH264Sps(unit)) {
                    facts.profileIdc = sps->profileIdc;
                    facts.constraintFlags = sps->constraintFlags;
                    facts.levelIdc = sps->levelIdc;
                } else if (const auto cabac = bitstream::h264Cabac(unit)) {
                    facts.cabac = cabac;
                }
            } else if (codec == VideoCodec::hevc) {
                if (const auto sps = bitstream::parseHevcSps(unit)) {
                    facts.profileIdc = sps->profileIdc;
                    facts.highTier = sps->highTier;
                    facts.levelIdc = sps->levelIdc;
                } else if (bitstream::hevcNalType(unit) == bitstream::hevcNalPrefixSei) {
                    for (auto& message : bitstream::hevcSeiMessages(unit)) facts.sei.push_back(std::move(message));
                }
            }
        }
    }
    return facts;
}

bool writesProfile(VideoEncodeProfile profile, const Facts& facts)
{
    if (!facts.profileIdc) return false;
    const unsigned idc = *facts.profileIdc;
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return idc == 66;
    case VideoEncodeProfile::h264ConstrainedBaseline: return idc == 66 && (facts.constraintFlags & 0x40);
    case VideoEncodeProfile::h264Main: return idc == 77;
    case VideoEncodeProfile::h264High: return idc == 100 && (facts.constraintFlags & 0x0c) == 0;
    case VideoEncodeProfile::h264ConstrainedHigh: return idc == 100 && (facts.constraintFlags & 0x0c) == 0x0c;
    case VideoEncodeProfile::h264High422: return idc == 122;
    case VideoEncodeProfile::h264High444Predictive: return idc == 244;
    case VideoEncodeProfile::hevcMain: return idc == 1;
    case VideoEncodeProfile::hevcMain10: return idc == 2;
    case VideoEncodeProfile::hevcMainStill: return idc == 3;
    case VideoEncodeProfile::jpegBaseline: return true;
    }
    return false;
}

// The dimensions a JPEG image states, or zeros when it states none.
std::pair<std::uint32_t, std::uint32_t> jpegDimensions(Bytes data)
{
    if (data.size() < 4 || data[0] != 0xff || data[1] != 0xd8) return {};
    for (std::size_t at = 2; at + 4 <= data.size();) {
        if (data[at] != 0xff) return {};
        while (at < data.size() && data[at] == 0xff) ++at;
        if (at >= data.size()) return {};
        const unsigned marker = data[at++];
        if (marker == 0xd9 || marker == 0xda) return {};
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (at + 2 > data.size()) return {};
        const std::size_t length = (std::size_t(data[at]) << 8) | data[at + 1];
        if (length < 2 || at + length > data.size()) return {};
        if ((marker >= 0xc0 && marker <= 0xcf) && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (length < 8) return {};
            return {(std::uint32_t(data[at + 5]) << 8) | data[at + 6], (std::uint32_t(data[at + 3]) << 8) | data[at + 4]};
        }
        at += length;
    }
    return {};
}

bool jpegIsWhole(const Stream& stream, std::uint32_t width, std::uint32_t height)
{
    if (stream.frames.empty()) return false;
    for (const auto& frame : stream.frames) {
        const auto& bytes = frame.bytes;
        if (bytes.size() < 4 || bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9) return false;
        if (jpegDimensions(bytes) != std::pair{width, height}) return false;
    }
    return true;
}

// Whether a probe produced a stream that proves what it was asked for.
bool proves(const Probe& probe, const Stream& stream)
{
    if (!stream.encoded) return false;
    if (probe.settings.codec == VideoCodec::jpeg)
        return jpegIsWhole(stream, probe.settings.width, probe.settings.height);
    return writesProfile(probe.settings.profile, analyze(probe.settings.codec, stream.frames));
}

// Total coded bytes of a stream.
std::size_t codedBytes(const Stream& stream)
{
    std::size_t total = 0;
    for (const auto& frame : stream.frames) total += frame.bytes.size();
    return total;
}

// ---- Telling a refusal of the host from no answer.

const char* statusName(VideoEncodeStatus status)
{
    switch (status) {
    case VideoEncodeStatus::ok: return "ok";
    case VideoEncodeStatus::unsupported: return "unsupported";
    case VideoEncodeStatus::invalid: return "invalid";
    case VideoEncodeStatus::exhausted: return "exhausted";
    case VideoEncodeStatus::failed: return "failed";
    }
    return "?";
}

const char* layoutText(RawVideoLayout layout)
{
    switch (layout) {
    case RawVideoLayout::nv12: return "nv12";
    case RawVideoLayout::i420: return "i420";
    case RawVideoLayout::p010: return "p010";
    case RawVideoLayout::nv16: return "nv16";
    case RawVideoLayout::nv24: return "nv24";
    case RawVideoLayout::bgra: return "bgra";
    case RawVideoLayout::rgba: return "rgba";
    }
    return "?";
}

// A probe is answered when the host said what it does: the encode finished,
// whether or not its stream proves the request, or the configuration was
// refused as it stands. A session the host could not give, a frame that never
// came out and a drain that ran out of time are not answers.
bool answered(const Stream& stream)
{
    return stream.encoded || stream.status == VideoEncodeStatus::unsupported || stream.status == VideoEncodeStatus::invalid;
}

// Runs the probes of one discovery. A probe with no answer is asked again after
// a short pause, kProbeRetries times, and only then is it told from a refusal:
// it is a refusal when the control, the first probe that proved in this codec,
// proves right after the last attempt, so a ceiling (VideoToolbox cannot create
// a session for a picture of 8192 by 8192) is told from a host that cannot
// create sessions at all. Otherwise it stays unanswered, which makes the
// discovery incomplete, and a codec whose probes keep being unanswered is given
// up. A probe left unanswered before any control exists is asked again by
// discoverHost once one does.
class Prober {
public:
    explicit Prober(const DiscoveryHooks& hooks) : hooks_(hooks) {}

    void beginCodec()
    {
        control_.reset();
        consecutive_ = 0;
        deferred_ = false;
    }
    // Starts the codec's probes again with the control it has now, and forgets
    // the probes left unanswered since `unansweredBefore`.
    void rewind(unsigned unansweredBefore) noexcept
    {
        unanswered_ = unansweredBefore;
        consecutive_ = 0;
        deferred_ = false;
    }
    [[nodiscard]] unsigned unanswered() const noexcept { return unanswered_; }
    [[nodiscard]] bool deferred() const noexcept { return deferred_; }
    [[nodiscard]] bool hasControl() const noexcept { return control_.has_value(); }

    Stream run(const Probe& probe)
    {
        if (givenUp()) return {};
        auto stream = hooks_.run(probe);
        if (answered(stream)) return settle(probe, std::move(stream));
        for (unsigned retry = 0; retry < kProbeRetries; ++retry) {
            hooks_.pause(kRetryPauseMilliseconds[retry]);
            stream = hooks_.run(probe);
            if (answered(stream)) return settle(probe, std::move(stream));
        }
        if (control_ && proves(*control_, hooks_.run(*control_))) {
            consecutive_ = 0;
            return stream;
        }
        deferred_ = deferred_ || !control_;
        report(describe(probe), statusName(stream.status), stream.diagnostic);
        return stream;
    }

    // Whether VideoToolbox lists a hardware encoder for the codec. A failed
    // call is no answer, which is not the same as an empty list.
    std::optional<bool> listed(VideoCodec codec)
    {
        auto answer = hooks_.hardwareListed(codec);
        for (unsigned retry = 0; !answer && retry < kProbeRetries; ++retry) {
            hooks_.pause(kRetryPauseMilliseconds[retry]);
            answer = hooks_.hardwareListed(codec);
        }
        if (!answer) report("the list of encoders", "failed", {});
        return answer;
    }

    // Sessions that open together, which the system limits across processes:
    // the count is what this host gave now. Only none at all is no answer.
    unsigned sessions(const VideoEncodeSettings& settings)
    {
        if (givenUp()) return 0;
        auto held = hooks_.countSessions(settings);
        for (unsigned retry = 0; !held && retry < kProbeRetries; ++retry) {
            hooks_.pause(kRetryPauseMilliseconds[retry]);
            held = hooks_.countSessions(settings);
        }
        if (held) consecutive_ = 0;
        else report("concurrent sessions", "exhausted", {});
        return held;
    }

private:
    [[nodiscard]] bool givenUp() const noexcept { return consecutive_ >= kUnansweredLimit; }

    Stream settle(const Probe& probe, Stream stream)
    {
        consecutive_ = 0;
        if (!control_ && proves(probe, stream))
            control_ = Probe {baseSettings(probe.settings.codec, probe.settings.profile, probe.settings.input)};
        return stream;
    }

    static std::string describe(const Probe& probe)
    {
        const auto& s = probe.settings;
        return std::string(s.codec == VideoCodec::h264 ? "h264" : s.codec == VideoCodec::hevc ? "hevc" : "jpeg") + " profile " +
               std::to_string(int(s.profile)) + " input " + layoutText(s.input) + " " + std::to_string(s.width) + "x" +
               std::to_string(s.height);
    }

    // Once per probe left unanswered, so the next report of a missing capability
    // names the host call that failed.
    void report(const std::string& what, const char* status, const std::string& diagnostic)
    {
        ++unanswered_;
        ++consecutive_;
        if (unanswered_ > kReportedLimit) return;
        std::fprintf(stderr, "Video encoder discovery: no answer for %s after %u tries (status %s%s%s); the result is not cached\n",
                     what.c_str(), kProbeRetries + 1, status, diagnostic.empty() ? "" : ", ", diagnostic.c_str());
    }

    const DiscoveryHooks& hooks_;
    std::optional<Probe> control_;
    unsigned consecutive_ {};
    unsigned unanswered_ {};
    bool deferred_ {};   // a probe ended unanswered while there was no control
};

// Whether a rate control mode does what its name says. A property the encoder
// accepts and ignores is not a mode: the stream it makes is the same whatever
// the mode's parameter is, so the mode is published only when its parameter
// moves the size of the stream, the right way round.
bool modeTakesEffect(Prober& prober, VideoCodec codec, const VideoEncodeProfileCapability& reference, VideoRateControl mode)
{
    const auto bytesWith = [&](auto&& choose) -> std::optional<std::size_t> {
        Probe probe {baseSettings(codec, reference.profile, reference.inputs.front()), 24, true};
        probe.settings.rateControl = mode;
        choose(probe.settings);
        const auto stream = prober.run(probe);
        if (!proves(probe, stream)) return std::nullopt;
        return codedBytes(stream);
    };
    std::optional<std::size_t> smaller, larger;
    switch (mode) {
    case VideoRateControl::variableBitrate: return true;
    case VideoRateControl::constantBitrate:
        smaller = bytesWith([](auto& s) { s.bitrate = 200'000; });
        larger = bytesWith([](auto& s) { s.bitrate = 8'000'000; });
        break;
    case VideoRateControl::constantQuality:
        smaller = bytesWith([](auto& s) { s.quality = 10; });
        larger = bytesWith([](auto& s) { s.quality = 90; });
        break;
    case VideoRateControl::constantQp:
        smaller = bytesWith([](auto& s) { s.frameQp = {40, 40, 40}; });
        larger = bytesWith([](auto& s) { s.frameQp = {18, 18, 18}; });
        break;
    }
    return smaller && larger && *larger > *smaller + *smaller / 5;
}

// ---- The tables of what to try.

struct Candidate {
    VideoEncodeProfile profile;
    std::vector<RawVideoLayout> layouts;
};

std::vector<Candidate> candidates(VideoCodec codec)
{
    // bgra stands for both packed orders: rgba is the same session, pool and
    // conversion with its red and blue bytes exchanged on submit, so one probe
    // decides both (discoverCodec).
    const std::vector<RawVideoLayout> common {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::bgra};
    switch (codec) {
    case VideoCodec::h264:
        return {{VideoEncodeProfile::h264Baseline, common}, {VideoEncodeProfile::h264ConstrainedBaseline, common},
                {VideoEncodeProfile::h264Main, common}, {VideoEncodeProfile::h264High, common},
                {VideoEncodeProfile::h264ConstrainedHigh, common},
                {VideoEncodeProfile::h264High422, {RawVideoLayout::nv16}},
                {VideoEncodeProfile::h264High444Predictive, {RawVideoLayout::nv24}}};
    case VideoCodec::hevc:
        return {{VideoEncodeProfile::hevcMain, common},
                {VideoEncodeProfile::hevcMain10, {RawVideoLayout::p010, RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::bgra}},
                {VideoEncodeProfile::hevcMainStill, common}};
    case VideoCodec::jpeg: return {{VideoEncodeProfile::jpegBaseline, common}};
    case VideoCodec::vp9: break;
    }
    return {};
}

// The profile the codec-wide measurements use: one every encoder handles and
// that permits the features measured (B-frames, for one).
const VideoEncodeProfileCapability* referenceProfile(const VideoEncodeCapability& capability)
{
    static const VideoEncodeProfile preferred[] = {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main,
        VideoEncodeProfile::hevcMain, VideoEncodeProfile::jpegBaseline};
    for (const auto wanted : preferred)
        for (const auto& profile : capability.profiles)
            if (profile.profile == wanted) return &profile;
    return capability.profiles.empty() ? nullptr : &capability.profiles.front();
}

RawVideoLayout referenceInput(const VideoEncodeProfileCapability& profile)
{
    return std::find(profile.inputs.begin(), profile.inputs.end(), RawVideoLayout::nv12) != profile.inputs.end()
        ? RawVideoLayout::nv12 : profile.inputs.front();
}

// The profile values the encoder says ProfileLevel takes. An empty set means
// the encoder would not say, and every candidate is tried.
std::set<std::string> listedProfileValues(CMVideoCodecType type)
{
    std::set<std::string> values;
    NSDictionary* specification = @{(__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
    CFDictionaryRef properties = nullptr;
    CFStringRef identifier = nullptr;
    if (VTCopySupportedPropertyDictionaryForEncoder(1280, 720, type, (__bridge CFDictionaryRef)specification, &identifier, &properties) != noErr)
        return values;
    if (identifier) CFRelease(identifier);
    NSDictionary* dictionary = (__bridge_transfer NSDictionary*)properties;
    NSDictionary* level = dictionary[(__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel];
    NSArray* list = [level isKindOfClass:[NSDictionary class]] ? level[(__bridge NSString*)kVTPropertySupportedValueListKey] : nil;
    if ([list isKindOfClass:[NSArray class]])
        for (id value in list)
            if ([value isKindOfClass:[NSString class]]) values.insert([(NSString*)value UTF8String]);
    return values;
}

// Whether VideoToolbox lists a hardware encoder for the codec; none when the
// call failed, which says nothing about the encoders.
std::optional<bool> hardwareEncoderListed(CMVideoCodecType type)
{
    CFArrayRef list = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list) return std::nullopt;
    NSArray* entries = (__bridge_transfer NSArray*)list;
    for (NSDictionary* entry in entries) {
        NSNumber* codec = entry[(__bridge NSString*)kVTVideoEncoderList_CodecType];
        NSNumber* hardware = entry[(__bridge NSString*)kVTVideoEncoderList_IsHardwareAccelerated];
        NSString* identifier = entry[(__bridge NSString*)kVTVideoEncoderList_EncoderID];
        // The depth, disparity and alpha encoders carry the base encoder's
        // identifier after a colon and are other codecs' business.
        if ([codec unsignedIntValue] == type && [hardware boolValue] && ![identifier containsString:@":"]) return true;
    }
    return false;
}

struct Size {
    std::uint32_t width, height;
};

constexpr Size kSizeLadder[] = {{176, 144}, {352, 288}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
                                {4096, 2304}, {4096, 4096}, {7680, 4320}, {8192, 4320}, {8192, 8192}};

unsigned secondsToTimeout(const Size& size)
{
    return size.width * size.height >= 3840u * 2160u ? 45'000 : 20'000;
}

unsigned hardwareSessionCount(const VideoEncodeSettings& settings)
{
    std::vector<std::unique_ptr<ProbeEncoder>> held;
    while (held.size() < kSessionProbeLimit) {
        auto encoder = makeProbeEncoder();
        if (encoder->configureUnchecked(settings) != VideoEncodeStatus::ok) break;
        held.push_back(std::move(encoder));
    }
    return unsigned(held.size());
}

// Frames per second one session sustains: the marginal rate between a short
// and a longer run, which leaves the first frame's latency out.
unsigned measureFramesPerSecond(Prober& prober, const VideoEncodeSettings& settings, unsigned shortRun, unsigned longRun)
{
    Probe a {settings, shortRun, true};
    Probe b {settings, longRun, true};
    const auto first = prober.run(a);
    const auto second = prober.run(b);
    if (!first.encoded || !second.encoded || second.seconds <= first.seconds) return 0;
    return unsigned(double(longRun - shortRun) / (second.seconds - first.seconds) + 0.5);
}

VideoHdrStaticMetadata sampleHdr()
{
    VideoHdrStaticMetadata hdr;
    hdr.displayPrimaries = {13250, 34500, 7500, 3000, 34000, 16000};   // P3 green, blue, red
    hdr.whitePointX = 15635;
    hdr.whitePointY = 16450;
    hdr.maximumLuminance = 10'000'000;
    hdr.minimumLuminance = 50;
    hdr.maximumContentLightLevel = 1000;
    hdr.maximumFrameAverageLightLevel = 400;
    return hdr;
}

std::optional<CodecDiscovery> discoverCodec(Prober& prober, const DiscoveryHooks& hooks, VideoCodec codec)
{
    const auto listed = prober.listed(codec);
    if (!listed || !*listed) return std::nullopt;
    const bool jpeg = codec == VideoCodec::jpeg;
    CodecDiscovery found;
    auto& capability = found.capability;
    capability.codec = codec;
    capability.sizeAlignment = 2;

    // Profiles and the layouts each accepts.
    const auto profileNames = jpeg ? std::set<std::string> {} : hooks.listedProfiles(codec);
    for (const auto& candidate : candidates(codec)) {
        if (!profileNames.empty() && !profileNames.count(autoLevelName(candidate.profile))) continue;
        VideoEncodeProfileCapability profile;
        profile.profile = candidate.profile;
        for (const auto layout : candidate.layouts) {
            const Probe probe {baseSettings(codec, candidate.profile, layout)};
            if (!proves(probe, prober.run(probe))) continue;
            profile.inputs.push_back(layout);
            if (layout == RawVideoLayout::bgra) profile.inputs.push_back(RawVideoLayout::rgba);
        }
        if (!profile.inputs.empty()) capability.profiles.push_back(std::move(profile));
    }
    if (capability.profiles.empty()) return std::nullopt;
    const auto reference = *referenceProfile(capability);
    const auto input = referenceInput(reference);
    const auto referenceSettings = [&](std::uint32_t width, std::uint32_t height) {
        return baseSettings(codec, reference.profile, input, width, height);
    };

    // Levels. A level is selectable where the stream states the one it was
    // asked for, which the encoder guarantees by raising the level in the
    // parameter sets VideoToolbox wrote (H.264, 5.2; HEVC, 4.0, which no
    // picture of a probe's size needs).
    for (auto& profile : capability.profiles) {
        if (codec == VideoCodec::jpeg) continue;
        const unsigned asked = codec == VideoCodec::h264 ? 52 : 120;
        Probe probe {baseSettings(codec, profile.profile, profile.inputs.front())};
        probe.settings.level = asked;
        const auto stream = prober.run(probe);
        if (codec == VideoCodec::h264) profile.maximumLevel = asked;
        profile.levelSelectable = stream.encoded && analyze(codec, stream.frames).levelIdc == asked;
    }

    // Sizes: the smallest square that encodes, then a ladder up to the first
    // size that does not, then the extreme aspect ratios of the result. A
    // video stream is never listed below kVideoEncodeMinimumSide, so its probe
    // starts there: smaller pictures encode, but a guest's quality check
    // rejects them. A JPEG still has no such check.
    capability.minimumWidth = kProbeWidth;
    capability.minimumHeight = kProbeHeight;
    const std::vector<std::uint32_t> smallest = jpeg ? std::vector<std::uint32_t> {16, 32, 48, 64, 96}
                                                     : std::vector<std::uint32_t> {kVideoEncodeMinimumSide, 96};
    for (const std::uint32_t side : smallest) {
        const Probe probe {referenceSettings(side, side), 2};
        if (proves(probe, prober.run(probe))) {
            capability.minimumWidth = capability.minimumHeight = side;
            break;
        }
    }
    capability.maximumWidth = kProbeWidth;
    capability.maximumHeight = kProbeHeight;
    unsigned observedLevel = 0;
    for (const auto& size : kSizeLadder) {
        Probe probe {referenceSettings(size.width, size.height), 1};
        probe.timeoutMilliseconds = secondsToTimeout(size);
        const auto stream = prober.run(probe);
        if (!proves(probe, stream)) break;
        capability.maximumWidth = std::max(capability.maximumWidth, size.width);
        capability.maximumHeight = std::max(capability.maximumHeight, size.height);
        if (codec == VideoCodec::hevc && std::uint64_t(size.width) * size.height <= kHevcLevelSixLumaSamples)
            observedLevel = std::max(observedLevel, analyze(codec, stream.frames).levelIdc);
    }
    const auto narrow = [&](std::uint32_t width, std::uint32_t height) {
        const Probe probe {referenceSettings(width, height), 1};
        return proves(probe, prober.run(probe));
    };
    while (capability.maximumWidth > 1280 && !narrow(capability.maximumWidth, capability.minimumHeight))
        capability.maximumWidth /= 2;
    while (capability.maximumHeight > 1280 && !narrow(capability.minimumWidth, capability.maximumHeight))
        capability.maximumHeight /= 2;
    if (codec == VideoCodec::hevc)
        for (auto& profile : capability.profiles) profile.maximumLevel = observedLevel;
    if (jpeg) {
        // The hardware JPEG encoder keeps an odd picture exactly.
        const Probe probe {referenceSettings(65, 33), 1};
        if (proves(probe, prober.run(probe))) capability.sizeAlignment = 1;
    }

    // Rate control. Variable bitrate is the encoder's baseline: without it
    // the codec is not usable.
    if (jpeg) {
        capability.rateControls = {VideoRateControl::constantQuality};
    } else {
        for (const auto mode : {VideoRateControl::variableBitrate, VideoRateControl::constantBitrate,
                                VideoRateControl::constantQuality, VideoRateControl::constantQp}) {
            Probe probe {referenceSettings(kProbeWidth, kProbeHeight), 4};
            probe.settings.rateControl = mode;
            if (!proves(probe, prober.run(probe))) continue;
            if (mode != VideoRateControl::variableBitrate && !modeTakesEffect(prober, codec, reference, mode)) continue;
            capability.rateControls.push_back(mode);
        }
        if (capability.rateControls.empty() || capability.rateControls.front() != VideoRateControl::variableBitrate) return std::nullopt;

        Probe bounds {referenceSettings(kProbeWidth, kProbeHeight), 4};
        bounds.settings.minimumQp = 0;
        bounds.settings.maximumQp = 51;
        if (proves(bounds, prober.run(bounds))) {
            capability.minimumQp = 0;
            capability.maximumQp = 51;
        }
        // The ladder starts from what the variable-bitrate trial above proved: that rate is the
        // floor, so a codec that encodes at all has a ceiling, and it is never probed again,
        // where a refusal would contradict the trial that just worked.
        capability.maximumBitrate = referenceSettings(kProbeWidth, kProbeHeight).bitrate;
        for (const std::uint64_t bitrate : {300'000'000ull, 240'000'000ull, 100'000'000ull, 50'000'000ull, 20'000'000ull, 10'000'000ull,
                                            5'000'000ull}) {
            if (bitrate <= capability.maximumBitrate) break;
            Probe probe {referenceSettings(kProbeWidth, kProbeHeight), 4};
            probe.settings.bitrate = bitrate;
            if (proves(probe, prober.run(probe))) {
                capability.maximumBitrate = bitrate;
                break;
            }
        }

        // B-frames: whatever the encoder does when it is allowed to reorder.
        // The hardware encoders refuse every property that would limit the run
        // (MaxFrameDelayCount, ReferenceBufferCount), so the longest one seen
        // is the run, which a stream codes whole or not at all.
        Probe reorder {referenceSettings(kProbeWidth, kProbeHeight), 36, true};
        reorder.settings.bFrames = 4;
        const auto reordered = prober.run(reorder);
        if (reordered.encoded) capability.maximumBFrames = std::min(analyze(codec, reordered.frames).longestBidirectionalRun, 4u);

        // Temporal layers exist only on the low-latency encoder, which also
        // writes fewer profiles than the standard one.
        Probe layered {referenceSettings(kProbeWidth, kProbeHeight), 24, true};
        layered.settings.temporalLayers = 2;
        layered.settings.realTime = true;
        const auto layeredStream = prober.run(layered);
        const auto layeredFacts = analyze(codec, layeredStream.frames);
        if (layeredStream.encoded && writesProfile(reference.profile, layeredFacts) && layeredFacts.upperLayerFrames > 0 &&
            layeredFacts.sawLowerLayerFrame) {
            capability.maximumTemporalLayers = 2;
            for (const auto& profile : capability.profiles) {
                Probe each {baseSettings(codec, profile.profile, profile.inputs.front()), 2};
                each.settings.temporalLayers = 2;
                each.settings.realTime = true;
                if (proves(each, prober.run(each))) found.lowLatencyProfiles.push_back(profile.profile);
            }
        }
    }

    if (codec == VideoCodec::h264) {
        // The entropy mode is selectable when each choice shows in the PPS.
        const bool baselineOnly = reference.profile == VideoEncodeProfile::h264Baseline ||
                                  reference.profile == VideoEncodeProfile::h264ConstrainedBaseline;
        if (!baselineOnly) {
            bool selectable = true;
            for (const bool cabac : {true, false}) {
                Probe probe {referenceSettings(kProbeWidth, kProbeHeight), 2};
                probe.settings.cabac = cabac;
                const auto stream = prober.run(probe);
                const auto facts = analyze(codec, stream.frames);
                selectable = selectable && stream.encoded && facts.cabac && *facts.cabac == cabac;
            }
            capability.entropyModeSelectable = selectable;
        }
    }

    if (codec == VideoCodec::hevc) {
        const auto main10 = std::find_if(capability.profiles.begin(), capability.profiles.end(),
            [](const auto& profile) { return profile.profile == VideoEncodeProfile::hevcMain10; });
        if (main10 != capability.profiles.end()) {
            Probe probe {baseSettings(codec, VideoEncodeProfile::hevcMain10, main10->inputs.front()), 2};
            probe.settings.color = {VideoColorPrimaries::bt2020, VideoTransferFunction::pq, VideoMatrixCoefficients::bt2020, false};
            probe.settings.hdr = sampleHdr();
            const auto stream = prober.run(probe);
            bool mastering = false, light = false;
            for (const auto& message : analyze(codec, stream.frames).sei) {
                mastering = mastering || (message.type == 137 && message.payload.size() == 24);
                light = light || (message.type == 144 && message.payload.size() == 4);
            }
            capability.hdrStaticMetadata = proves(probe, stream) && mastering && light;
        }
    }

    // The sessions are measured by measureSessions, in every process.

    // Throughput, for the codecs a guest streams in real time.
    if (!jpeg) {
        auto settings = referenceSettings(1280, 720);
        settings.bitrate = 8'000'000;
        capability.measuredFramesPerSecond720p = measureFramesPerSecond(prober, settings, 10, 70);
        settings = referenceSettings(1920, 1080);
        settings.bitrate = 16'000'000;
        capability.measuredFramesPerSecond1080p = measureFramesPerSecond(prober, settings, 10, 50);
    }
    return found;
}

// ---- The cache.

NSString* cachePath()
{
    if (const char* override_ = std::getenv("UML_VIDEO_ENCODE_CACHE")) return override_[0] ? [NSString stringWithUTF8String:override_] : nil;
#if TARGET_OS_OSX
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"uml-video-encoder-capabilities.json"];
#else
    NSArray* directories = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    return directories.count ? [directories[0] stringByAppendingPathComponent:@"uml-video-encoder-capabilities.json"] : nil;
#endif
}

std::string sysctlString(const char* name)
{
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0 || size > 256) return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    value.resize(std::strlen(value.c_str()));
    return value;
}

// What the cached result is valid for: this machine and this build of the OS.
NSString* hostKey()
{
    return [NSString stringWithFormat:@"%s/%s/%s", sysctlString("hw.machine").c_str(), sysctlString("hw.model").c_str(),
            sysctlString("kern.osversion").c_str()];
}

NSArray* numbers(const std::vector<int>& values)
{
    NSMutableArray* array = [NSMutableArray array];
    for (const int value : values) [array addObject:@(value)];
    return array;
}

NSDictionary* encodeCapability(const CodecDiscovery& entry)
{
    const auto& c = entry.capability;
    NSMutableArray* profiles = [NSMutableArray array];
    for (const auto& profile : c.profiles) {
        std::vector<int> inputs;
        for (const auto layout : profile.inputs) inputs.push_back(int(layout));
        [profiles addObject:@{@"profile": @(int(profile.profile)), @"inputs": numbers(inputs), @"maximumLevel": @(profile.maximumLevel),
                              @"levelSelectable": @(profile.levelSelectable)}];
    }
    std::vector<int> modes, lowLatency;
    for (const auto mode : c.rateControls) modes.push_back(int(mode));
    for (const auto profile : entry.lowLatencyProfiles) lowLatency.push_back(int(profile));
    return @{
        @"codec": @(int(c.codec)), @"profiles": profiles,
        @"minimumWidth": @(c.minimumWidth), @"minimumHeight": @(c.minimumHeight),
        @"maximumWidth": @(c.maximumWidth), @"maximumHeight": @(c.maximumHeight), @"sizeAlignment": @(c.sizeAlignment),
        @"rateControls": numbers(modes), @"maximumBitrate": @(c.maximumBitrate),
        @"minimumQp": @(c.minimumQp), @"maximumQp": @(c.maximumQp), @"maximumBFrames": @(c.maximumBFrames),
        @"maximumTemporalLayers": @(c.maximumTemporalLayers), @"entropyModeSelectable": @(c.entropyModeSelectable),
        @"hdrStaticMetadata": @(c.hdrStaticMetadata), @"concurrentSessions": @(c.concurrentSessions),
        @"fps720": @(c.measuredFramesPerSecond720p), @"fps1080": @(c.measuredFramesPerSecond1080p),
        @"lowLatencyProfiles": numbers(lowLatency),
    };
}

// A reader that refuses a cache file at the first field that is missing or not
// what a writer of this revision stores. A cache is a courtesy: whatever it
// cannot vouch for is rediscovered.
class CacheReader {
public:
    bool good() const { return good_; }
    NSNumber* number(NSDictionary* from, NSString* key)
    {
        id value = from[key];
        if (![value isKindOfClass:[NSNumber class]]) good_ = false;
        return good_ ? value : @0;
    }
    std::uint64_t unsignedValue(NSDictionary* from, NSString* key, std::uint64_t maximum)
    {
        const auto value = [number(from, key) doubleValue];
        if (!(value >= 0) || value > double(maximum)) good_ = false;
        return good_ ? std::uint64_t(value) : 0;
    }
    template<class E> E enumeration(id value, int last)
    {
        const int index = [value isKindOfClass:[NSNumber class]] ? [value intValue] : -1;
        if (index < 0 || index > last) good_ = false;
        return static_cast<E>(good_ ? index : 0);
    }
    template<class E> std::vector<E> enumerations(NSDictionary* from, NSString* key, int last)
    {
        std::vector<E> out;
        NSArray* array = from[key];
        if (![array isKindOfClass:[NSArray class]] || array.count > 32) {
            good_ = false;
            return out;
        }
        for (id value in array) out.push_back(enumeration<E>(value, last));
        return out;
    }
    NSArray* array(NSDictionary* from, NSString* key)
    {
        NSArray* value = from[key];
        if (![value isKindOfClass:[NSArray class]] || value.count > 32) {
            good_ = false;
            return @[];
        }
        return value;
    }
private:
    bool good_ {true};
};

std::optional<Discovery> decodeDiscovery(NSDictionary* root)
{
    if (![root isKindOfClass:[NSDictionary class]]) return std::nullopt;
    CacheReader reader;
    if (reader.unsignedValue(root, @"revision", 1000) != kDiscoveryRevision) return std::nullopt;
    if (![root[@"host"] isKindOfClass:[NSString class]] || ![root[@"host"] isEqualToString:hostKey()]) return std::nullopt;
    Discovery discovery;
    for (NSDictionary* entry in reader.array(root, @"codecs")) {
        if (![entry isKindOfClass:[NSDictionary class]]) return std::nullopt;
        CodecDiscovery decoded;
        auto& c = decoded.capability;
        c.codec = reader.enumeration<VideoCodec>(entry[@"codec"], int(VideoCodec::jpeg));
        for (NSDictionary* item in reader.array(entry, @"profiles")) {
            if (![item isKindOfClass:[NSDictionary class]]) return std::nullopt;
            VideoEncodeProfileCapability profile;
            profile.profile = reader.enumeration<VideoEncodeProfile>(item[@"profile"], int(VideoEncodeProfile::jpegBaseline));
            profile.inputs = reader.enumerations<RawVideoLayout>(item, @"inputs", int(RawVideoLayout::rgba));
            profile.maximumLevel = std::uint32_t(reader.unsignedValue(item, @"maximumLevel", 1000));
            profile.levelSelectable = [reader.number(item, @"levelSelectable") boolValue];
            c.profiles.push_back(std::move(profile));
        }
        c.minimumWidth = std::uint32_t(reader.unsignedValue(entry, @"minimumWidth", 65536));
        c.minimumHeight = std::uint32_t(reader.unsignedValue(entry, @"minimumHeight", 65536));
        c.maximumWidth = std::uint32_t(reader.unsignedValue(entry, @"maximumWidth", 65536));
        c.maximumHeight = std::uint32_t(reader.unsignedValue(entry, @"maximumHeight", 65536));
        c.sizeAlignment = std::uint32_t(reader.unsignedValue(entry, @"sizeAlignment", 64));
        c.rateControls = reader.enumerations<VideoRateControl>(entry, @"rateControls", int(VideoRateControl::constantQp));
        c.maximumBitrate = reader.unsignedValue(entry, @"maximumBitrate", 4'000'000'000ull);
        c.minimumQp = std::uint32_t(reader.unsignedValue(entry, @"minimumQp", 64));
        c.maximumQp = std::uint32_t(reader.unsignedValue(entry, @"maximumQp", 64));
        c.maximumBFrames = std::uint32_t(reader.unsignedValue(entry, @"maximumBFrames", 16));
        c.maximumTemporalLayers = std::uint32_t(reader.unsignedValue(entry, @"maximumTemporalLayers", 8));
        c.entropyModeSelectable = [reader.number(entry, @"entropyModeSelectable") boolValue];
        c.hdrStaticMetadata = [reader.number(entry, @"hdrStaticMetadata") boolValue];
        c.concurrentSessions = std::uint32_t(reader.unsignedValue(entry, @"concurrentSessions", 1024));
        c.measuredFramesPerSecond720p = std::uint32_t(reader.unsignedValue(entry, @"fps720", 100000));
        c.measuredFramesPerSecond1080p = std::uint32_t(reader.unsignedValue(entry, @"fps1080", 100000));
        decoded.lowLatencyProfiles = reader.enumerations<VideoEncodeProfile>(entry, @"lowLatencyProfiles", int(VideoEncodeProfile::jpegBaseline));
        if (!reader.good() || c.profiles.empty() || c.minimumWidth > c.maximumWidth || c.minimumHeight > c.maximumHeight) return std::nullopt;
        // A video size below the floor is a result that predates it, or a file that is not one.
        if (c.codec != VideoCodec::jpeg && (c.minimumWidth < kVideoEncodeMinimumSide || c.minimumHeight < kVideoEncodeMinimumSide))
            return std::nullopt;
        discovery.codecs.push_back(std::move(decoded));
    }
    if (!reader.good()) return std::nullopt;
    return discovery;
}

std::optional<Discovery> loadCache()
{
    NSString* path = cachePath();
    if (!path) return std::nullopt;
    NSData* data = [NSData dataWithContentsOfFile:path options:0 error:nil];
    if (!data || data.length > (1u << 20)) return std::nullopt;
    id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return decodeDiscovery(root);
}

void storeCache(const Discovery& discovery)
{
    NSString* path = cachePath();
    if (!path) return;
    NSMutableArray* codecs = [NSMutableArray array];
    for (const auto& entry : discovery.codecs) [codecs addObject:encodeCapability(entry)];
    NSDictionary* root = @{@"revision": @(kDiscoveryRevision), @"host": hostKey(), @"codecs": codecs};
    NSData* data = [NSJSONSerialization dataWithJSONObject:root options:NSJSONWritingSortedKeys error:nil];
    if (!data) return;
    // Written aside and renamed in, so a reader never sees half a file and a
    // second process racing this one leaves one whole file or the other.
    NSString* temporary = [path stringByAppendingFormat:@".%d.tmp", int(::getpid())];
    if ([data writeToFile:temporary options:0 error:nil]) {
        if (std::rename(temporary.fileSystemRepresentation, path.fileSystemRepresentation) != 0) (void)::unlink(temporary.fileSystemRepresentation);
    }
}

// ---- What the product asks of the host.

CMVideoCodecType codecType(VideoCodec codec)
{
    return codec == VideoCodec::h264 ? kCMVideoCodecType_H264 : codec == VideoCodec::hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_JPEG;
}

DiscoveryHooks productionHooks()
{
    DiscoveryHooks hooks;
    hooks.run = runProbe;
    hooks.countSessions = hardwareSessionCount;
    hooks.hardwareListed = [](VideoCodec codec) { return hardwareEncoderListed(codecType(codec)); };
    hooks.listedProfiles = [](VideoCodec codec) { return listedProfileValues(codecType(codec)); };
    hooks.pause = [](unsigned milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); };
    hooks.now = [] {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    };
    hooks.load = loadCache;
    hooks.store = storeCache;
    hooks.waitMilliseconds = unsigned(std::chrono::milliseconds(kDiscoveryWait).count());
    return hooks;
}

// ---- Discovery of the whole host.

// The sessions that open together are what the host has free: other processes
// hold some of them, so a count is a figure of its moment. A discovery of the
// host measures it for each codec it found. A codec that cannot open a session
// is left out and is unanswered.
void measureSessions(Prober& prober, Discovery& discovery)
{
    for (auto codec = discovery.codecs.begin(); codec != discovery.codecs.end();) {
        auto& capability = codec->capability;
        const auto* reference = referenceProfile(capability);
        if (!reference || reference->inputs.empty()) {
            ++codec;
            continue;
        }
        prober.beginCodec();
        const auto before = prober.unanswered();
        const auto held = prober.sessions(baseSettings(capability.codec, reference->profile, referenceInput(*reference)));
        if (held) {
            capability.concurrentSessions = held;
            ++codec;
            continue;
        }
        if (prober.unanswered() > before) discovery.unansweredCodecs.push_back(capability.codec);
        codec = discovery.codecs.erase(codec);
    }
    discovery.unanswered = prober.unanswered();
    discovery.complete = discovery.unanswered == 0;
}

Discovery discoverHost(const DiscoveryHooks& hooks)
{
    @autoreleasepool {
        Prober prober(hooks);
        Discovery discovery;
        for (const auto codec : {VideoCodec::h264, VideoCodec::hevc, VideoCodec::jpeg}) {
            prober.beginCodec();
            const auto before = prober.unanswered();
            auto found = discoverCodec(prober, hooks, codec);
            if (prober.deferred() && prober.hasControl()) {
                // A probe went unanswered before there was a control to tell
                // its refusal from a busy host; there is one now.
                prober.rewind(before);
                found = discoverCodec(prober, hooks, codec);
            }
            if (prober.unanswered() > before) discovery.unansweredCodecs.push_back(codec);
            if (found) discovery.codecs.push_back(std::move(*found));
        }
        measureSessions(prober, discovery);
        return discovery;
    }
}

// ---- Replacing an answer of one process.

template<class T> bool containsAll(const std::vector<T>& outer, const std::vector<T>& inner)
{
    return std::all_of(inner.begin(), inner.end(), [&](const T& value) { return std::find(outer.begin(), outer.end(), value) != outer.end(); });
}

// Whether `next` offers every configuration `old` did. A capability is a set of
// configurations, and a codec's entry is one run's coherent description of it:
// the entries are not combined field by field, which would offer sizes and
// rates no run proved together.
bool contains(const CodecDiscovery& next, const CodecDiscovery& old)
{
    const auto& n = next.capability;
    const auto& o = old.capability;
    for (const auto& profile : o.profiles) {
        const auto found = std::find_if(n.profiles.begin(), n.profiles.end(), [&](const auto& entry) { return entry.profile == profile.profile; });
        if (found == n.profiles.end() || !containsAll(found->inputs, profile.inputs) || found->maximumLevel < profile.maximumLevel ||
            (profile.levelSelectable && !found->levelSelectable))
            return false;
    }
    return n.minimumWidth <= o.minimumWidth && n.minimumHeight <= o.minimumHeight && n.maximumWidth >= o.maximumWidth &&
           n.maximumHeight >= o.maximumHeight && n.sizeAlignment && o.sizeAlignment % n.sizeAlignment == 0 &&
           containsAll(n.rateControls, o.rateControls) && n.maximumBitrate >= o.maximumBitrate && n.minimumQp <= o.minimumQp &&
           n.maximumQp >= o.maximumQp && n.maximumBFrames >= o.maximumBFrames && n.maximumTemporalLayers >= o.maximumTemporalLayers &&
           (!o.entropyModeSelectable || n.entropyModeSelectable) && (!o.hdrStaticMetadata || n.hdrStaticMetadata) &&
           containsAll(next.lowLatencyProfiles, old.lowLatencyProfiles);
}

// The complete result that replaces an answer. A guest that was offered a
// configuration may still use it, so a codec the answer has stays as it is
// unless the new entry contains it, and a codec the answer lacks is added. The
// session count is a measurement, not part of what an entry offers: it is
// never lowered.
Discovery replaceAnswer(const Discovery& served, Discovery fresh)
{
    for (const auto& old : served.codecs) {
        auto found = std::find_if(fresh.codecs.begin(), fresh.codecs.end(),
                                  [&](const auto& entry) { return entry.capability.codec == old.capability.codec; });
        if (found == fresh.codecs.end()) {
            fresh.codecs.push_back(old);
            continue;
        }
        const auto sessions = std::max(found->capability.concurrentSessions, old.capability.concurrentSessions);
        if (contains(*found, old)) {
            // Measured speeds describe the host, not what an entry offers: the one that was measured stays.
            auto& c = found->capability;
            if (!c.measuredFramesPerSecond720p) c.measuredFramesPerSecond720p = old.capability.measuredFramesPerSecond720p;
            if (!c.measuredFramesPerSecond1080p) c.measuredFramesPerSecond1080p = old.capability.measuredFramesPerSecond1080p;
        } else {
            *found = old;
        }
        found->capability.concurrentSessions = sessions;
    }
    std::sort(fresh.codecs.begin(), fresh.codecs.end(),
              [](const auto& a, const auto& b) { return int(a.capability.codec) < int(b.capability.codec); });
    return fresh;
}

// ---- The process-wide result.

class ServiceImpl final : public DiscoveryService {
public:
    explicit ServiceImpl(DiscoveryHooks hooks) : state_(std::make_shared<State>(std::move(hooks))) {}

    void start() override
    {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->started) return;
            state_->started = true;
            state_->reserveLocked();
        }
        state_->launch();
    }

    std::shared_ptr<const Discovery> current() override
    {
        start();
        std::unique_lock lock(state_->mutex);
        // Every probe has its own deadline, so discovery ends; the bound here keeps
        // a caller from waiting on a VideoToolbox that never answers, in which case
        // the host reports no encoder rather than hanging the guest's probe. A
        // discovery whose thread could not be created is not running, and nothing
        // is waited for.
        if (!state_->condition.wait_for(lock, std::chrono::milliseconds(state_->hooks.waitMilliseconds),
                                        [&] { return state_->result != nullptr || !state_->running; }))
            return State::unknown();
        return state_->result ? state_->result : State::unknown();
    }

    std::vector<VideoEncodeCapability> query() override
    {
        (void)current();
        std::unique_lock lock(state_->mutex);
        auto& state = *state_;
        // A later query computes again once an incomplete answer, or none, has been
        // served, while no discovery runs, a few times and not in the same busy moment.
        if ((!state.result || !state.result->complete) && state.served && !state.running && state.runs < kMaximumRuns &&
            state.hooks.now() - state.lastRun >= kRerunIntervalMilliseconds) {
            state.reserveLocked();
            lock.unlock();
            state.launch();
            lock.lock();
            state.condition.wait_for(lock, std::chrono::milliseconds(state.hooks.waitMilliseconds), [&] { return !state.running; });
        }
        // The answer as it is now, not as it was when this query began.
        const auto answer = state.result ? state.result : State::unknown();
        if (!answer->complete) state.served = true;
        std::vector<VideoEncodeCapability> capabilities;
        for (const auto& entry : answer->codecs) capabilities.push_back(entry.capability);
        return capabilities;
    }

private:
    struct State : std::enable_shared_from_this<State> {
        explicit State(DiscoveryHooks given) : hooks(std::move(given)) {}

        static std::shared_ptr<const Discovery> unknown()
        {
            auto none = std::make_shared<Discovery>();
            none->complete = false;
            return none;
        }

        // The one discovery that runs is reserved, and counted, when it starts.
        void reserveLocked()
        {
            running = true;
            ++runs;
        }

        // Starts the discovery reserved by the caller. When no thread can be created the
        // reservation is released and the waiting callers woken, the answer stays as it
        // is, and a later query may try again: the run still counts.
        void launch()
        {
            try {
                auto task = [state = shared_from_this()] { state->work(); };
                if (hooks.spawn) hooks.spawn(std::move(task));
                else std::thread(std::move(task)).detach();
            } catch (...) {
                std::fprintf(stderr, "Video encoder discovery: no thread to run it on; a later query tries again\n");
                std::lock_guard lock(mutex);
                lastRun = hooks.now();
                running = false;
                condition.notify_all();
            }
        }

        // A count the cache carries is that of the process that wrote it, which
        // may have found sessions held elsewhere. Where it is below the most the
        // host gives, this process measures it once, after the answer is published
        // and off every caller's wait. A higher count raises the published one and
        // the file's, for the next process; a lower one changes nothing, and
        // neither does a measurement that opens no session.
        void raiseSessions(Discovery cached)
        {
            bool raised = false;
            for (auto& entry : cached.codecs) {
                auto& capability = entry.capability;
                const auto* reference = referenceProfile(capability);
                if (capability.concurrentSessions >= kSessionProbeLimit || !reference || reference->inputs.empty()) continue;
                const auto held = hooks.countSessions(baseSettings(capability.codec, reference->profile, referenceInput(*reference)));
                if (held <= capability.concurrentSessions) continue;
                capability.concurrentSessions = held;
                raised = true;
                std::lock_guard lock(mutex);
                auto next = std::make_shared<Discovery>(*result);
                const auto published = std::find_if(next->codecs.begin(), next->codecs.end(),
                                                    [&](const auto& other) { return other.capability.codec == capability.codec; });
                if (published != next->codecs.end())
                    published->capability.concurrentSessions = std::max(published->capability.concurrentSessions, held);
                result = std::move(next);
            }
            if (raised) hooks.store(cached);
        }

        void work()
        {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
            @autoreleasepool {
                if (auto cached = hooks.load()) {
                    // A cache hit is published at once with the counts its file carries,
                    // and nothing is opened until a caller has the answer. The discovery
                    // stays the one that runs until the sessions are measured.
                    {
                        std::lock_guard lock(mutex);
                        lastRun = hooks.now();
                        publishLocked(Discovery(*cached));
                        condition.notify_all();
                    }
                    raiseSessions(std::move(*cached));
                    std::lock_guard lock(mutex);
                    running = false;
                    condition.notify_all();
                    return;
                }
                auto discovery = discoverHost(hooks);
                if (discovery.complete) hooks.store(discovery);
                else std::fprintf(stderr, "Video encoder discovery is incomplete: %u probe(s) had no answer\n", discovery.unanswered);
                std::lock_guard lock(mutex);
                lastRun = hooks.now();
                publishLocked(std::move(discovery));
                running = false;
                condition.notify_all();
            }
        }

        // Nothing replaces a complete answer, an incomplete one replaces nothing,
        // and a complete one that replaces an incomplete answer keeps what it offered.
        void publishLocked(Discovery next)
        {
            if (!result) {
                result = std::make_shared<const Discovery>(std::move(next));
            } else if (!result->complete && next.complete) {
                result = std::make_shared<const Discovery>(replaceAnswer(*result, std::move(next)));
            }
        }

        DiscoveryHooks hooks;
        std::mutex mutex;
        std::condition_variable condition;
        bool started {};
        bool running {};   // a discovery is in flight, the first or a later one
        bool served {};    // the last query was answered with an incomplete discovery
        unsigned runs {};
        std::uint64_t lastRun {};
        std::shared_ptr<const Discovery> result;
    };

    std::shared_ptr<State> state_;
};

// Never destroyed: the discovery thread may still be running at exit.
DiscoveryService& service()
{
    static DiscoveryService* instance = new ServiceImpl(productionHooks());
    return *instance;
}

} // namespace

#if UML_VIDEO_ENCODER_TESTING
Discovery runDiscovery(const DiscoveryHooks& hooks)
{
    return discoverHost(hooks);
}

std::unique_ptr<DiscoveryService> makeDiscoveryService(DiscoveryHooks hooks)
{
    return std::make_unique<ServiceImpl>(std::move(hooks));
}
#endif

std::shared_ptr<const Discovery> discovery()
{
    return service().current();
}

void invalidateDiscoveryCache() noexcept
{
    @autoreleasepool {
        if (NSString* path = cachePath()) (void)::unlink(path.fileSystemRepresentation);
    }
}

} // namespace apple_encoder

void prepareVideoEncodeCapabilities()
{
    apple_encoder::service().start();
}

std::vector<VideoEncodeCapability> videoEncodeCapabilities()
{
    return apple_encoder::service().query();
}

} // namespace uml::platform
