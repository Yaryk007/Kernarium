// SPDX-License-Identifier: GPL-2.0-or-later
#include "vp9_stream.hpp"

#include <algorithm>

namespace uml::platform::vp9 {
namespace {

using bitstream::BitReader;

constexpr std::uint32_t frameSyncCode = 0x498342;
constexpr unsigned colorSpaceReserved = 6;
constexpr unsigned colorSpaceRgb = 7;

/* color_config() of the uncompressed header. Profile 0 and 2 are 4:2:0 and RGB
 * is allowed only in profile 1 and 3. Returns false for a value the standard
 * reserves or disallows. */
bool readColorConfig(BitReader& reader, FrameHeader& header)
{
    if (header.profile >= 2) header.bitDepth = reader.bits(1) ? 12 : 10;
    header.colorSpace = reader.bits(3);
    if (header.colorSpace == colorSpaceReserved) return false;
    const bool oddProfile = header.profile == 1 || header.profile == 3;
    if (header.colorSpace != colorSpaceRgb) {
        header.fullRange = reader.bits(1) != 0;
        if (oddProfile) {
            const unsigned subsamplingX = reader.bits(1);
            const unsigned subsamplingY = reader.bits(1);
            if (reader.bits(1) != 0) return false;
            // 4:4:0, 4:2:2 and 4:4:4 are what a profile 1 or 3 stream may carry
            // besides 4:2:0, which those profiles do not allow.
            if (subsamplingX == 1 && subsamplingY == 1) return false;
        }
    } else {
        header.fullRange = true;
        // RGB is 4:4:4, which profile 0 and 2 cannot carry.
        if (!oddProfile) return false;
        if (reader.bits(1) != 0) return false;
    }
    return reader.ok();
}

FrameSize readFrameSize(BitReader& reader)
{
    FrameSize size;
    size.width = reader.bits(16) + 1;
    size.height = reader.bits(16) + 1;
    return size;
}

/* render_size() states the size to show the picture at, which the decoded
 * surface does not take, so only its length matters. */
void skipRenderSize(BitReader& reader)
{
    if (reader.bits(1)) reader.bits(32);
}

} // namespace

std::optional<std::vector<Bytes>> splitSuperframe(Bytes packet)
{
    if (packet.empty()) return std::nullopt;
    const std::uint8_t marker = packet.back();
    if ((marker & 0xe0) != 0xc0) return std::vector<Bytes> {packet};
    const std::size_t frames = std::size_t(marker & 7) + 1;
    const std::size_t magnitude = std::size_t((marker >> 3) & 3) + 1;
    const std::size_t indexSize = 2 + magnitude * frames;
    /* A last byte that reads as a marker states an index, and the index starts
     * with the same byte. libvpx calls a packet that has no room for the index
     * or no such byte at its front corrupt, and so does this. */
    if (packet.size() < indexSize || packet[packet.size() - indexSize] != marker) return std::nullopt;
    const std::size_t available = packet.size() - indexSize;
    std::vector<Bytes> result;
    std::size_t offset = 0;
    std::size_t position = packet.size() - indexSize + 1;
    for (std::size_t i = 0; i < frames; ++i) {
        std::size_t size = 0;
        for (std::size_t byte = 0; byte < magnitude; ++byte)
            size |= std::size_t(packet[position++]) << (8 * byte);
        // An empty frame is how libvpx is told that one was lost.
        if (size == 0 || size > available - offset) return std::nullopt;
        result.push_back(packet.subspan(offset, size));
        offset += size;
    }
    return result;
}

std::vector<std::uint8_t> joinFrames(std::span<const Bytes> frames)
{
    std::vector<std::uint8_t> packet;
    std::size_t largest = 0;
    for (const auto frame : frames) {
        packet.insert(packet.end(), frame.begin(), frame.end());
        largest = std::max(largest, frame.size());
    }
    if (frames.size() == 1 && (packet.back() & 0xe0) != 0xc0) return packet;
    std::size_t magnitude = 1;
    while (magnitude < 4 && (largest >> (8 * magnitude)) != 0) ++magnitude;
    const auto marker = std::uint8_t(0xc0 | ((magnitude - 1) << 3) | (frames.size() - 1));
    packet.push_back(marker);
    for (const auto frame : frames)
        for (std::size_t byte = 0; byte < magnitude; ++byte)
            packet.push_back(std::uint8_t(frame.size() >> (8 * byte)));
    packet.push_back(marker);
    return packet;
}

std::optional<FrameHeader> parseFrameHeader(Bytes frame)
{
    BitReader reader(frame);
    if (reader.bits(2) != 2) return std::nullopt;
    FrameHeader header;
    const unsigned low = reader.bits(1);
    const unsigned high = reader.bits(1);
    header.profile = (high << 1) | low;
    if (header.profile == 3 && reader.bits(1) != 0) return std::nullopt;   // profile 4 and above
    // What the profile implies, until a color_config() states otherwise.
    header.bitDepth = header.profile >= 2 ? 10 : 8;
    if (reader.bits(1)) {                                                    // show_existing_frame
        reader.bits(3);
        if (!reader.ok()) return std::nullopt;
        header.showExisting = true;
        header.showFrame = true;
        return header;
    }
    header.key = reader.bits(1) == 0;
    header.showFrame = reader.bits(1) != 0;
    const bool errorResilient = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    if (header.key) {
        if (reader.bits(24) != frameSyncCode) return std::nullopt;
        if (!readColorConfig(reader, header)) return std::nullopt;
        header.size = readFrameSize(reader);
        skipRenderSize(reader);
        return reader.ok() ? std::optional<FrameHeader>(header) : std::nullopt;
    }
    header.intraOnly = header.showFrame ? false : reader.bits(1) != 0;
    if (!errorResilient) reader.bits(2);                                     // reset_frame_context
    if (header.intraOnly) {
        if (reader.bits(24) != frameSyncCode) return std::nullopt;
        if (header.profile > 0) {
            if (!readColorConfig(reader, header)) return std::nullopt;
        }
        reader.bits(8);                                                      // refresh_frame_flags
        header.size = readFrameSize(reader);
        skipRenderSize(reader);
        return reader.ok() ? std::optional<FrameHeader>(header) : std::nullopt;
    }
    reader.bits(8);                                                          // refresh_frame_flags
    for (unsigned i = 0; i < 3; ++i) reader.bits(4);                         // ref_frame_idx, sign_bias
    // frame_size_with_refs(): the size is a reference's when one of the three says so.
    bool found = false;
    for (unsigned i = 0; i < 3 && reader.ok() && !found; ++i) found = reader.bits(1) != 0;
    if (!found) header.size = readFrameSize(reader);
    return reader.ok() ? std::optional<FrameHeader>(header) : std::nullopt;
}

std::optional<Packet> parsePacket(Bytes bytes, const char** why)
{
    const auto fail = [why](const char* reason) -> std::optional<Packet> {
        if (why) *why = reason;
        return std::nullopt;
    };
    const auto frames = splitSuperframe(bytes);
    if (!frames) return fail("invalid VP9 superframe index");
    Packet packet;
    for (const auto frame : *frames) {
        const auto header = parseFrameHeader(frame);
        if (!header) return fail("invalid VP9 frame header");
        packet.frames.push_back(*header);
        packet.data.push_back(frame);
    }
    return packet;
}

} // namespace uml::platform::vp9
