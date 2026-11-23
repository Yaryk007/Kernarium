// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/* VP9 frame headers built bit by bit, and superframes built from frames, for
 * the cases a fixture cannot give: the headers and indexes a guest can send
 * and no encoder writes. Only the uncompressed header is real; whatever follows
 * it is padding, so a frame built here is parsed and refused by the decoder's
 * readers and is not one a decoder can decode. */
namespace uml::testing::vp9 {
using Bytes = std::vector<std::uint8_t>;

class BitWriter {
public:
    BitWriter& put(std::uint32_t value, unsigned bits)
    {
        while (bits-- > 0) {
            if (position_ % 8 == 0) bytes_.push_back(0);
            if ((value >> bits) & 1) bytes_.back() |= std::uint8_t(0x80 >> (position_ % 8));
            ++position_;
        }
        return *this;
    }
    [[nodiscard]] Bytes bytes(std::size_t padding = 16) const
    {
        Bytes result = bytes_;
        result.insert(result.end(), padding, 0);
        return result;
    }
private:
    Bytes bytes_;
    std::size_t position_ {};
};

inline constexpr std::uint32_t syncCode = 0x498342;

// frame_marker, the profile bits (and the reserved bit of profile 3).
inline BitWriter& begin(BitWriter& bits, unsigned profile, std::uint32_t marker = 2)
{
    bits.put(marker, 2).put(profile & 1, 1).put((profile >> 1) & 1, 1);
    if (profile == 3) bits.put(0, 1);
    return bits;
}

// color_config() for a profile and colour space; `subsampling` is the x, y bits
// that profiles 1 and 3 state, and `reserved` the bit that follows them.
struct Color {
    unsigned colorSpace {2};   // BT.709
    bool fullRange {};
    unsigned subsamplingX {0};
    unsigned subsamplingY {0};
    unsigned reserved {0};
    bool twelveBit {};
};
inline BitWriter& colorConfig(BitWriter& bits, unsigned profile, const Color& color)
{
    if (profile >= 2) bits.put(color.twelveBit ? 1 : 0, 1);
    bits.put(color.colorSpace, 3);
    const bool odd = profile == 1 || profile == 3;
    if (color.colorSpace != 7) {
        bits.put(color.fullRange ? 1 : 0, 1);
        if (odd) bits.put(color.subsamplingX, 1).put(color.subsamplingY, 1).put(color.reserved, 1);
    } else if (odd) {
        bits.put(color.reserved, 1);
    }
    return bits;
}

inline BitWriter& frameSize(BitWriter& bits, std::uint32_t width, std::uint32_t height)
{
    return bits.put(width - 1, 16).put(height - 1, 16);
}

// A key frame header with the size and colour it states.
inline Bytes keyFrame(unsigned profile, std::uint32_t width, std::uint32_t height, const Color& color = {},
                      std::uint32_t sync = syncCode, std::uint32_t marker = 2)
{
    BitWriter bits;
    begin(bits, profile, marker).put(0, 1)        // show_existing_frame
        .put(0, 1).put(1, 1).put(0, 1)            // key frame, shown, not error resilient
        .put(sync, 24);
    colorConfig(bits, profile, color);
    frameSize(bits, width, height);
    bits.put(0, 1);                               // render_and_frame_size_different
    return bits.bytes();
}

// A key frame whose colour description is the profile 0 one (4:2:0, 8-bit).
inline Bytes key(std::uint32_t width, std::uint32_t height)
{
    return keyFrame(0, width, height);
}

// A hidden intra-only frame: it states its own size, and its colour only in a profile above 0.
inline Bytes intraOnly(unsigned profile, std::uint32_t width, std::uint32_t height, const Color& color = {})
{
    BitWriter bits;
    begin(bits, profile).put(0, 1)                // show_existing_frame
        .put(1, 1).put(0, 1).put(0, 1)            // non-key, hidden, not error resilient
        .put(1, 1)                                // intra_only
        .put(0, 2)                                // reset_frame_context
        .put(syncCode, 24);
    if (profile > 0) colorConfig(bits, profile, color);
    bits.put(0x01, 8);                            // refresh_frame_flags
    frameSize(bits, width, height);
    bits.put(0, 1);
    return bits.bytes();
}

// An inter frame. With `fromReference` the first reference supplies the size;
// otherwise the frame states `width` x `height` itself.
inline Bytes inter(bool fromReference, std::uint32_t width = 0, std::uint32_t height = 0, unsigned profile = 0)
{
    BitWriter bits;
    begin(bits, profile).put(0, 1)                // show_existing_frame
        .put(1, 1).put(1, 1).put(0, 1)            // non-key, shown, not error resilient
        .put(0, 2)                                // reset_frame_context (intra_only is 0: it is shown)
        .put(0x01, 8);                            // refresh_frame_flags
    for (unsigned i = 0; i < 3; ++i) bits.put(i, 3).put(0, 1);   // ref_frame_idx, sign_bias
    if (fromReference) {
        bits.put(1, 1);                           // found_ref
    } else {
        bits.put(0, 3);                           // no reference supplies the size
        frameSize(bits, width, height);
    }
    bits.put(0, 1);
    return bits.bytes();
}

inline Bytes showExisting(unsigned profile = 0, unsigned slot = 0)
{
    BitWriter bits;
    begin(bits, profile).put(1, 1).put(slot, 3);
    return bits.bytes(0);
}

// The frames as one packet: the frames, then an index with `magnitude` bytes per size.
inline Bytes superframe(const std::vector<Bytes>& frames, unsigned magnitude = 2)
{
    Bytes result;
    for (const auto& frame : frames) result.insert(result.end(), frame.begin(), frame.end());
    const std::uint8_t marker = std::uint8_t(0xc0 | ((magnitude - 1) << 3) | (frames.size() - 1));
    result.push_back(marker);
    for (const auto& frame : frames)
        for (unsigned byte = 0; byte < magnitude; ++byte)
            result.push_back(std::uint8_t(frame.size() >> (8 * byte)));
    result.push_back(marker);
    return result;
}

} // namespace uml::testing::vp9
