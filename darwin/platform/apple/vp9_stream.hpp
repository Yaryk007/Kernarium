// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What the Apple video decoder needs to know about a VP9 packet before it
// reaches VideoToolbox: where its frames begin and end, and what each frame's
// uncompressed header states about the picture. A guest hands the decoder
// whatever bytes it likes, and VP9 carries no parameter sets, so these headers
// are the only place the profile and the picture size are stated. It is plain
// C++ over the bitstream reader the video encoder backend shares, so headers
// built by a test can exercise it without VideoToolbox.

#include "video_encoder_bitstream.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::platform::vp9 {

using bitstream::Bytes;

struct FrameSize {
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const FrameSize&, const FrameSize&) = default;
};

// The part of a frame's uncompressed header that decides how the frame is
// decoded and how large its picture is.
struct FrameHeader {
    unsigned profile {};
    // A one-byte frame that shows a picture already decoded.
    bool showExisting {};
    bool key {};
    // A non-key frame that predicts from nothing, and so states its own size.
    bool intraOnly {};
    bool showFrame {};
    // The bit depth, colour space (CS_* as the header states it) and range of a
    // key frame, or of an intra-only frame of a profile above 0. Other frames
    // keep what the stream's key frame said, which these leave at what the
    // profile implies: 8 bits for profile 0 and 1, 10 for profile 2 and 3.
    unsigned bitDepth {8};
    unsigned colorSpace {};
    bool fullRange {};
    // Set for a frame that states its picture size: a key frame, an intra-only
    // frame, and an inter frame none of whose references the size is taken
    // from. The size of the others is one an earlier frame stated, so a bound
    // checked on every stated size holds for every picture of the stream.
    std::optional<FrameSize> size;
};

// The frames of a packet: the frames a superframe index names, or the packet
// itself when its last byte is not an index marker. A last byte that is one
// states an index, and nullopt is the answer, as libvpx's corrupt frame, when
// the packet is shorter than that index, the index lacks its leading copy of
// the marker, its frames do not fit the packet or it names an empty frame.
[[nodiscard]] std::optional<std::vector<Bytes>> splitSuperframe(Bytes packet);

// The packet that carries exactly these frames (one to eight, none empty): a
// lone frame as it is, unless its last byte would read as an index marker,
// and any other number under a superframe index.
[[nodiscard]] std::vector<std::uint8_t> joinFrames(std::span<const Bytes> frames);

// The header of one frame, up to and including the size it states. nullopt for
// a frame that is truncated, has a wrong frame marker or synchronization code,
// or states a reserved value. Profile 4 and above is reserved. A profile other
// than 0 is read in full, because refusing it is the decoder's decision.
[[nodiscard]] std::optional<FrameHeader> parseFrameHeader(Bytes frame);

// Every frame of a packet, in order. nullopt when the packet or any of its
// frames is invalid; `why` then names the first fault. The caller's bounds on
// profile and size apply to each header returned.
struct Packet {
    std::vector<FrameHeader> frames;
    // Each frame's bytes, in the packet that was parsed, which they point into.
    std::vector<Bytes> data;
};
[[nodiscard]] std::optional<Packet> parsePacket(Bytes bytes, const char** why = nullptr);

} // namespace uml::platform::vp9
