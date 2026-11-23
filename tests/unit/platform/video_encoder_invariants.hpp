// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"
#include "video_encoder.hpp"

#include <algorithm>
#include <set>
#include <vector>

/* What every list of encoder capabilities satisfies, whatever host it came
 * from. The hardware test requires it of the list its host reported; the
 * discovery test asks for the violation in lists it built, so that a defect of
 * one codec is one however the discovery of another went. */

namespace uml::testing::video_encoder {

inline uml::platform::VideoCodec invariantCodecOf(uml::platform::VideoEncodeProfile profile)
{
    using uml::platform::VideoEncodeProfile;
    using uml::platform::VideoCodec;
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: case VideoEncodeProfile::h264ConstrainedBaseline:
    case VideoEncodeProfile::h264Main: case VideoEncodeProfile::h264High: case VideoEncodeProfile::h264ConstrainedHigh:
    case VideoEncodeProfile::h264High422: case VideoEncodeProfile::h264High444Predictive:
        return VideoCodec::h264;
    case VideoEncodeProfile::hevcMain: case VideoEncodeProfile::hevcMain10: case VideoEncodeProfile::hevcMainStill:
        return VideoCodec::hevc;
    case VideoEncodeProfile::jpegBaseline:
        return VideoCodec::jpeg;
    }
    return VideoCodec::vp9;
}

// The first invariant the list breaks, or null. `uncertain` names the codecs
// whose discovery left a probe unanswered: the host was busy, which says
// nothing about that codec. For those only the invariants that hold of any
// result are required; for every other codec all of them are, so a defect in
// one codec is not hidden by the state of another.
[[nodiscard]] inline const char* capabilityViolation(const std::vector<uml::platform::VideoEncodeCapability>& capabilities,
                                                     const std::set<uml::platform::VideoCodec>& uncertain)
{
    using namespace uml::platform;
    for (const auto& capability : capabilities) {
        const bool jpeg = capability.codec == VideoCodec::jpeg;
        const bool certain = uncertain.count(capability.codec) == 0;
        if (capability.codec != VideoCodec::h264 && capability.codec != VideoCodec::hevc && !jpeg)
            return "capability for a codec the interface cannot encode";
        if (capability.profiles.empty()) return "a codec with no profile is not listed";
        if (capability.minimumWidth == 0 || capability.minimumHeight == 0) return "minimum size";
        // A video picture below the floor encodes but fails a guest's quality check, so it is never listed.
        if (!jpeg && (capability.minimumWidth < kVideoEncodeMinimumSide || capability.minimumHeight < kVideoEncodeMinimumSide))
            return "a video capability lists a picture below the floor";
        if (capability.minimumWidth > capability.maximumWidth || capability.minimumHeight > capability.maximumHeight) return "size range";
        if (capability.sizeAlignment < 1) return "alignment";
        if (capability.minimumWidth % capability.sizeAlignment || capability.minimumHeight % capability.sizeAlignment)
            return "the minimum size is a multiple of the alignment";
        if (capability.maximumWidth % capability.sizeAlignment || capability.maximumHeight % capability.sizeAlignment)
            return "the maximum size is a multiple of the alignment";
        if (capability.concurrentSessions < 1) return "at least one session";
        if (capability.rateControls.empty()) return "rate controls";
        for (const auto& profile : capability.profiles) {
            const auto lists = [&](RawVideoLayout layout) {
                return std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end();
            };
            if (invariantCodecOf(profile.profile) != capability.codec) return "profile belongs to the codec";
            if (profile.inputs.empty()) return "a profile with no input layout is not listed";
            if (certain && jpeg != (profile.maximumLevel == 0)) return "only JPEG has no level";
            if (jpeg && profile.levelSelectable) return "JPEG has no level to select";
            // Both packed orders reach the encoder through one pool, so one is not offered without the other.
            if (lists(RawVideoLayout::bgra) != lists(RawVideoLayout::rgba)) return "BGRA and RGBA input are offered together";
            if (certain && profile.profile == VideoEncodeProfile::hevcMain10 && !lists(RawVideoLayout::p010))
                return "HEVC Main 10 takes 10-bit input";
        }
        if (jpeg) {
            if (capability.rateControls.size() != 1 || capability.rateControls[0] != VideoRateControl::constantQuality)
                return "JPEG is constant quality";
            continue;
        }
        if (capability.maximumBitrate == 0) return "maximum bitrate";
        if (capability.minimumQp > capability.maximumQp || capability.maximumQp > 51) return "QP range";
        if (capability.maximumTemporalLayers < 1) return "temporal layers";
    }
    return nullptr;
}

inline void requireCapabilityInvariants(const std::vector<uml::platform::VideoEncodeCapability>& capabilities,
                                        const std::set<uml::platform::VideoCodec>& uncertain)
{
    const char* violation = capabilityViolation(capabilities, uncertain);
    uml::testing::require(violation == nullptr, violation ? violation : "");
}

} // namespace uml::testing::video_encoder
