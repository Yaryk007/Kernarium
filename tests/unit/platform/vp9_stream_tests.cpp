// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/vp9_stream.hpp"
#include "video_decoder_test_support.hpp"
#include "vp9_test_frames.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

/* The VP9 packet readers of the Apple video decoder, on the checked-in streams
 * and on headers built here. A guest sends the decoder whatever bytes it likes,
 * and VP9 states the profile and the picture size in each frame's own header,
 * so these readers are what the decoder's bounds stand on. Nothing in this
 * binary touches VideoToolbox. */

namespace {
using namespace uml::platform::vp9;
using uml::testing::require;
namespace test = uml::testing::vp9;
using uml::testing::video_decoder::read;
using Bytes = std::vector<std::uint8_t>;

std::optional<FrameHeader> header(const Bytes& frame) { return parseFrameHeader(frame); }

void fixtureHeaders()
{
    {
        const auto frame = read("vp9-profile0.bin");
        const auto parsed = header(frame);
        require(parsed && parsed->key && parsed->profile == 0 && parsed->showFrame && !parsed->showExisting,
                "the profile 0 fixture is not a shown key frame of profile 0");
        require(parsed->size && *parsed->size == FrameSize {128, 96} && parsed->bitDepth == 8,
                "the profile 0 fixture's size or depth was misread");
    }
    for (const auto& [name, width, height] : {std::tuple {"vp9-square-4096x4096.bin", 4096u, 4096u},
                                              std::tuple {"vp9-portrait-2304x4096.bin", 2304u, 4096u},
                                              std::tuple {"vp9-uhd-3840x2160.bin", 3840u, 2160u},
                                              std::tuple {"vp9-too-wide-4112x2304.bin", 4112u, 2304u}}) {
        const auto parsed = header(read(name));
        require(parsed && parsed->key && parsed->size && *parsed->size == FrameSize {width, height},
                "a fixture's stated size was misread");
    }
}

/* The IVF fixture holds the packets of two clips: 64 x 64 with alt-ref frames
 * (the superframes), then 96 x 80. A superframe is a hidden frame and the
 * frame that shows. */
void superframeFixture()
{
    const auto file = read("vp9-superframes.ivf");
    const auto packets = uml::testing::video_decoder::ivfPackets(file);
    require(packets.size() == 40, "the superframe fixture has forty packets");
    unsigned superframes = 0, keys = 0;
    for (const auto& packet : packets) {
        const auto frames = splitSuperframe(packet);
        require(frames && !frames->empty(), "a fixture packet did not split");
        const auto parsed = parsePacket(packet);
        require(parsed && parsed->frames.size() == frames->size(), "a fixture packet did not parse");
        if (frames->size() > 1) {
            ++superframes;
            require(frames->size() == 2 && !parsed->frames[0].showFrame && !parsed->frames[0].key &&
                    parsed->frames[1].showFrame,
                    "a superframe is not a hidden frame and a frame that shows");
            // Both frames predict from references, so neither states its size unless the encoder chose to.
            std::size_t total = 0;
            for (const auto frame : *frames) total += frame.size();
            require(total + 2 + 2 * frames->size() == packet.size(), "the frames and the index leave bytes over");
        }
        if (parsed->frames.front().key) ++keys;
    }
    require(superframes == 3, "the superframe fixture has three superframes");
    require(keys == 2, "the superframe fixture has a key frame for each of its two clips");
    const auto second = parsePacket(packets[30]);
    require(second && second->frames.front().key && second->frames.front().size &&
            *second->frames.front().size == FrameSize {96, 80},
            "the second clip does not begin with a 96 x 80 key frame");
}

void headerKinds()
{
    // A key frame states its size and colour; render size does not enter the picture.
    {
        auto frame = test::keyFrame(0, 640, 360, {.colorSpace = 5, .fullRange = true});
        const auto parsed = header(frame);
        require(parsed && parsed->key && parsed->colorSpace == 5 && parsed->fullRange &&
                *parsed->size == FrameSize {640, 360}, "a key frame was misread");
    }
    // The extreme sizes the sixteen bits allow are read, not judged: the decoder bounds them.
    {
        const auto parsed = header(test::keyFrame(0, 65536, 65536));
        require(parsed && *parsed->size == FrameSize {65536, 65536}, "the largest stated size was misread");
        const auto smallest = header(test::keyFrame(0, 1, 1));
        require(smallest && *smallest->size == FrameSize {1, 1}, "the smallest stated size was misread");
    }
    // Profile 2 is 10 or 12 bit.
    {
        const auto ten = header(test::keyFrame(2, 64, 64, {.twelveBit = false}));
        const auto twelve = header(test::keyFrame(2, 64, 64, {.twelveBit = true}));
        require(ten && ten->profile == 2 && ten->bitDepth == 10 && twelve && twelve->bitDepth == 12,
                "a profile 2 key frame's depth was misread");
    }
    // Profile 1 and 3 are 4:4:4, 4:4:0 or 4:2:2, never 4:2:0, and RGB is 4:4:4.
    {
        require(header(test::keyFrame(1, 64, 64, {.subsamplingX = 0, .subsamplingY = 0})).has_value(), "a 4:4:4 profile 1 frame");
        require(header(test::keyFrame(1, 64, 64, {.subsamplingX = 1, .subsamplingY = 0})).has_value(), "a 4:2:2 profile 1 frame");
        require(header(test::keyFrame(3, 64, 64, {.colorSpace = 7})).has_value(), "an RGB profile 3 frame");
        require(!header(test::keyFrame(1, 64, 64, {.subsamplingX = 1, .subsamplingY = 1})),
                "4:2:0 was accepted in profile 1");
        require(!header(test::keyFrame(1, 64, 64, {.reserved = 1})), "a reserved colour bit was accepted");
        require(!header(test::keyFrame(0, 64, 64, {.colorSpace = 7})), "RGB was accepted in profile 0");
        require(!header(test::keyFrame(2, 64, 64, {.colorSpace = 7})), "RGB was accepted in profile 2");
    }
    // An intra-only frame states its size, and its colour only above profile 0.
    {
        const auto zero = header(test::intraOnly(0, 320, 200));
        require(zero && zero->intraOnly && !zero->key && !zero->showFrame && *zero->size == FrameSize {320, 200} &&
                zero->bitDepth == 8, "an intra-only frame of profile 0 was misread");
        const auto two = header(test::intraOnly(2, 320, 200, {.twelveBit = false}));
        require(two && two->intraOnly && two->bitDepth == 10 && *two->size == FrameSize {320, 200},
                "an intra-only frame of profile 2 was misread");
    }
    // A frame that states no colour carries the depth its profile implies, which
    // is what the decoder checks it against the stream's.
    {
        const auto zero = header(test::inter(true, 0, 0, 0));
        const auto two = header(test::inter(true, 0, 0, 2));
        const auto existing = header(test::showExisting(2));
        require(zero && zero->profile == 0 && zero->bitDepth == 8 && two && two->profile == 2 && two->bitDepth == 10 &&
                    existing && existing->profile == 2 && existing->bitDepth == 10,
                "the depth a profile implies was misread");
    }
    // An inter frame states a size only when none of its references supplies it.
    {
        const auto referenced = header(test::inter(true));
        require(referenced && !referenced->key && !referenced->intraOnly && !referenced->size,
                "an inter frame that takes its size from a reference states one");
        const auto stated = header(test::inter(false, 8192, 8192));
        require(stated && stated->size && *stated->size == FrameSize {8192, 8192},
                "the size an inter frame states was misread");
    }
    // A frame that shows an earlier picture is the one byte that selects it.
    {
        const auto frame = test::showExisting();
        require(frame.size() == 1, "a show-existing frame is one byte");
        const auto parsed = header(frame);
        require(parsed && parsed->showExisting && parsed->showFrame && !parsed->size,
                "a show-existing frame was misread");
    }
}

/* Truncation anywhere is refused, and so is every header a conforming stream
 * never carries. */
void hostileHeaders()
{
    require(!header({}), "an empty frame was accepted");
    require(!header(test::keyFrame(0, 64, 64, {}, test::syncCode, 0)), "frame marker 0 was accepted");
    require(!header(test::keyFrame(0, 64, 64, {}, test::syncCode, 1)), "frame marker 1 was accepted");
    require(!header(test::keyFrame(0, 64, 64, {}, test::syncCode, 3)), "frame marker 3 was accepted");
    require(!header(test::keyFrame(0, 64, 64, {}, 0x498343)), "a wrong sync code was accepted");
    require(!header(test::keyFrame(0, 64, 64, {}, 0)), "a zero sync code was accepted");
    // Colour space 6 is reserved, in a key frame of any profile and in an intra-only frame above profile 0.
    for (const unsigned profile : {0u, 1u, 2u, 3u}) {
        const test::Color reserved {.colorSpace = 6, .subsamplingX = 0, .subsamplingY = 0};
        require(!header(test::keyFrame(profile, 64, 64, reserved)), "the reserved colour space 6 was accepted");
        if (profile > 0)
            require(!header(test::intraOnly(profile, 64, 64, reserved)),
                    "the reserved colour space 6 was accepted in an intra-only frame");
        // The colour spaces on either side of it are not reserved.
        const test::Color bt2020 {.colorSpace = 5, .subsamplingX = 0, .subsamplingY = 0};
        require(header(test::keyFrame(profile, 64, 64, bt2020)).has_value(), "colour space 5 was refused");
    }
    // Profile 4 and above: the high and low bits set and the reserved bit set too.
    {
        test::BitWriter bits;
        bits.put(2, 2).put(1, 1).put(1, 1).put(1, 1).put(0, 1).put(0, 1).put(1, 1).put(0, 1);
        require(!header(bits.bytes()), "profile 4 was accepted");
    }
    /* Cut each kind of frame short at every length. A header is as many bytes
     * as the bits it states take, no fewer: what follows is padding. */
    struct Case { Bytes frame; std::size_t headerBytes; };
    for (const auto& [frame, headerBytes] : {
             Case {test::keyFrame(0, 640, 360), 9},                   // 69 bits
             Case {test::intraOnly(0, 640, 360), 10},                 // 76 bits
             Case {test::inter(false, 640, 360), 9},                  // 65 bits
             Case {test::keyFrame(2, 640, 360), 9},                   // 70 bits
             Case {test::intraOnly(3, 640, 360, {.subsamplingX = 0, .subsamplingY = 0}), 11}}) {   // 85 bits
        for (std::size_t length = 0; length < headerBytes; ++length) {
            const Bytes cut(frame.begin(), frame.begin() + length);
            require(!header(cut), "a truncated frame header was accepted");
        }
        const Bytes whole(frame.begin(), frame.begin() + headerBytes);
        require(header(whole).has_value(), "a frame header of exactly the bytes its bits take was refused");
    }
}

void superframeIndexes()
{
    const auto a = test::key(64, 64);
    const auto b = test::inter(true);
    // A well formed index splits into its frames, whatever the width of its sizes.
    for (unsigned magnitude = 1; magnitude <= 4; ++magnitude) {
        const auto packet = test::superframe({a, b}, magnitude);
        const auto frames = splitSuperframe(packet);
        require(frames && frames->size() == 2 && (*frames)[0].size() == a.size() && (*frames)[1].size() == b.size(),
                "a well formed index did not split");
        require(std::equal((*frames)[0].begin(), (*frames)[0].end(), a.begin()) &&
                std::equal((*frames)[1].begin(), (*frames)[1].end(), b.begin()), "a split frame is not the frame");
    }
    // Eight frames is the most an index names.
    {
        std::vector<Bytes> many(8, b);
        const auto frames = splitSuperframe(test::superframe(many, 1));
        require(frames && frames->size() == 8, "an index of eight frames did not split");
    }
    // A last byte that reads as an index marker states an index, as libvpx takes it. One whose
    // leading copy of the marker is missing, or that the packet is too short to hold, is a
    // corrupt packet and no single frame.
    {
        auto packet = test::superframe({a, b});
        packet[packet.size() - (2 + 2 * 2)] ^= 0x01;              // the first byte of the index
        require(!splitSuperframe(packet), "an index whose leading marker does not match was accepted");
        // A marker byte appended to an intact frame is the same fault.
        for (const std::uint8_t marker : {0xc0, 0xc9, 0xdf}) {
            auto lone = a;
            lone.push_back(marker);
            require(!splitSuperframe(lone), "a lone marker byte after a frame was accepted");
        }
        // The packet holds fewer bytes than the index it states.
        require(!splitSuperframe(Bytes {0x82, 0xc1}) && !splitSuperframe(Bytes {0xc0}) &&
                !splitSuperframe(Bytes {0x82, 0xdf}), "an index longer than its packet was accepted");
        const char* why = nullptr;
        auto lone = a;
        lone.push_back(0xc0);
        require(!parsePacket(lone, &why) && why != nullptr && std::string(why) == "invalid VP9 superframe index",
                "a lone marker byte did not fail the packet as an index fault");
        // A last byte just outside the marker's pattern is the end of a frame.
        for (const std::uint8_t last : {0xbf, 0xe0, 0x00}) {
            auto frame = a;
            frame.push_back(last);
            const auto frames = splitSuperframe(frame);
            require(frames && frames->size() == 1 && (*frames)[0].size() == frame.size(),
                    "a packet that ends in no marker was not taken as one frame");
        }
    }
    // An index that names more than the packet holds, an empty frame, or a size
    // the sum of the others overflows is refused.
    {
        auto big = test::superframe({a, b});
        const std::size_t index = big.size() - (2 + 2 * 2);
        big[index + 1] = 0xff;                                    // the first size, low byte
        big[index + 2] = 0x7f;
        require(!splitSuperframe(big), "an index naming more than the packet holds was accepted");
        auto empty = test::superframe({a, b});
        empty[index + 1] = 0;                                     // the first size becomes 0
        empty[index + 2] = 0;
        require(!splitSuperframe(empty), "an empty frame in the index was accepted");
        auto wide = test::superframe({a, b}, 4);
        const std::size_t wideIndex = wide.size() - (2 + 4 * 2);
        for (unsigned byte = 0; byte < 4; ++byte) wide[wideIndex + 1 + byte] = 0xff;   // 4 GiB - 1
        require(!splitSuperframe(wide), "a four byte size larger than the packet was accepted");
    }
    // Every frame of the index is read, not only the first.
    {
        const auto packet = test::superframe({a, test::intraOnly(0, 8192, 8192)});
        const auto parsed = parsePacket(packet);
        require(parsed && parsed->frames.size() == 2 && parsed->frames[1].size &&
                *parsed->frames[1].size == FrameSize {8192, 8192}, "the second frame's size was not read");
        const char* why = nullptr;
        const auto bad = parsePacket(test::superframe({a, test::keyFrame(0, 64, 64, {}, 0)}), &why);
        require(!bad && why != nullptr && std::string(why) == "invalid VP9 frame header",
                "a bad second frame did not fail the packet with its reason");
        why = nullptr;
        auto torn = test::superframe({a, b});
        torn[torn.size() - 5] = 0;   // the first size becomes 0
        torn[torn.size() - 4] = 0;
        const auto none = parsePacket(torn, &why);
        require(!none && why != nullptr && std::string(why) == "invalid VP9 superframe index",
                "a bad index did not fail the packet with its reason");
    }
}

/* The packet the decoder builds from the frames it keeps splits back into them. */
void joinedFrames()
{
    const auto a = test::key(64, 64);
    const auto b = test::inter(true);
    const auto view = [](const Bytes& frame) { return uml::platform::vp9::Bytes(frame); };
    // One frame needs no index.
    {
        const std::vector<uml::platform::vp9::Bytes> one {view(a)};
        const auto packet = joinFrames(one);
        require(packet == a, "a lone frame was changed");
    }
    // A frame whose last byte would read as an index marker is given an index.
    {
        auto frame = a;
        frame.back() = 0xc3;
        const std::vector<uml::platform::vp9::Bytes> one {view(frame)};
        const auto packet = joinFrames(one);
        const auto split = splitSuperframe(packet);
        require(packet.size() > frame.size() && split && split->size() == 1 && (*split)[0].size() == frame.size() &&
                std::equal(frame.begin(), frame.end(), (*split)[0].begin()),
                "a lone frame that ends like a marker did not come back whole");
    }
    // Two to eight frames come back as they went in, whatever their sizes need to be stated in.
    for (const std::size_t size : {std::size_t(3), std::size_t(300), std::size_t(70000), std::size_t(17000000)}) {
        for (const std::size_t count : {std::size_t(2), std::size_t(3), std::size_t(8)}) {
            std::vector<Bytes> storage;
            for (std::size_t i = 0; i < count; ++i) {
                Bytes frame(i == 0 ? size : 3 + i, std::uint8_t(0x11 * (i + 1)));
                storage.push_back(std::move(frame));
            }
            std::vector<uml::platform::vp9::Bytes> views;
            for (const auto& frame : storage) views.push_back(view(frame));
            const auto packet = joinFrames(views);
            const auto split = splitSuperframe(packet);
            require(split && split->size() == count, "joined frames did not split into as many");
            for (std::size_t i = 0; i < count; ++i)
                require((*split)[i].size() == storage[i].size() &&
                        std::equal(storage[i].begin(), storage[i].end(), (*split)[i].begin()),
                        "a joined frame is not the frame that went in");
        }
    }
    std::puts("joined frames split back into the frames that went in");
}

/* Whatever bytes a guest sends, the readers return a result inside the packet,
 * and a size inside what sixteen bits state, or nothing. Run under a sanitizer
 * this is what finds an overrun. */
void arbitraryBytes()
{
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    const auto next = [&state] {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return state;
    };
    unsigned accepted = 0, keys = 0;
    for (unsigned round = 0; round < 200000; ++round) {
        Bytes packet(1 + next() % 48);
        for (auto& byte : packet) byte = std::uint8_t(next());
        // Bias half of the packets toward a frame that starts like one.
        if (round % 2 == 0) {
            packet[0] = std::uint8_t(0x80 | (next() & 0x3f));
            if (packet.size() > 4 && (next() & 1)) { packet[1] = 0x49; packet[2] = 0x83; packet[3] = 0x42; }
        }
        const auto split = splitSuperframe(packet);
        if (split) {
            const auto* begin = packet.data();
            for (const auto frame : *split) {
                require(!frame.empty() && frame.data() >= begin && frame.data() + frame.size() <= begin + packet.size(),
                        "a split frame lies outside its packet");
            }
        }
        const auto parsed = parsePacket(packet);
        if (!parsed) continue;
        ++accepted;
        for (const auto& frame : parsed->frames) {
            require(frame.profile <= 3, "a profile above 3 was accepted");
            if (frame.key) ++keys;
            if (frame.size)
                require(frame.size->width >= 1 && frame.size->width <= 65536 && frame.size->height >= 1 &&
                        frame.size->height <= 65536, "a stated size is outside what sixteen bits state");
            if (frame.key || frame.intraOnly) require(frame.size.has_value(), "an intra frame states no size");
            if (frame.showExisting) require(!frame.size && !frame.key, "a show-existing frame carries a size");
        }
    }
    std::printf("%u of 200000 arbitrary packets parsed, %u key frames among them\n", accepted, keys);
    require(accepted > 1000 && keys > 100, "the arbitrary packets never exercised the parsers");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_vp9_stream_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    uml::testing::video_decoder::fixtureDirectory = argv[1];
    try {
        fixtureHeaders();
        superframeFixture();
        headerKinds();
        hostileHeaders();
        superframeIndexes();
        joinedFrames();
        arbitraryBytes();
        std::puts("VP9 stream readers pass");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
