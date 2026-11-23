// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_decoder.hpp"
#include "video_decoder_test_support.hpp"
#include "apple/h264_stream.hpp"
#include "apple/hevc_stream.hpp"
#include "apple/vp9_stream.hpp"
#include "require.hpp"
#include "vp9_test_frames.hpp"

#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using uml::testing::require;
using namespace uml::testing::video_decoder;

/* Each picture of the order fixtures is a flat gray 10 levels brighter than the
 * one before it in display order, so the brightness is the display position. */
const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 10, 9, 12, 16, 14, 13, 15, 19, 18, 17};
const unsigned openDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 12, 10, 9, 11, 16, 14, 13, 15,
                                20, 18, 17, 19, 23, 22, 21};

/* Pictures leave in display order whatever order they were decoded in, and each
 * keeps the timestamp of the buffer that carried it. */
void checkDisplayOrder(const std::vector<uml::platform::DecodedVideoFrame>& frames, const unsigned* display,
                       std::size_t pictures, const std::vector<unsigned>& absent, const char* what)
{
    // The pictures that should be shown, by display position.
    std::vector<unsigned> shown;
    for (unsigned au = 0; au < pictures; ++au)
        if (std::find(absent.begin(), absent.end(), au) == absent.end()) shown.push_back(au);
    std::sort(shown.begin(), shown.end(), [display](unsigned a, unsigned b) { return display[a] < display[b]; });
    std::printf("%s: %zu pictures, timestamps", what, frames.size());
    for (const auto& frame : frames) std::printf(" %llu", static_cast<unsigned long long>(frame.timestamp / 1000 - 1));
    std::printf("\n");
    require(frames.size() == shown.size(), what);
    for (std::size_t k = 0; k < frames.size(); ++k) {
        require(frames[k].timestamp == (shown[k] + 1) * 1000ull, "a picture lost its timestamp or left out of order");
        require(std::abs(meanLuma(frames[k]) - (16.0 + 10.0 * display[shown[k]])) <= 3.0,
                "a picture is not the one its timestamp names");
    }
}

void hevcDisplayOrder()
{
    using namespace uml::platform;
    // B-pictures in closed groups.
    {
        const auto units = accessUnits(read("hevc-bframes.bin"));
        require(units.size() == 20, "the closed fixture has twenty pictures");
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        std::vector<DecodedVideoFrame> frames;
        for (std::size_t i = 0; i < units.size(); ++i) {
            decoder->submit(units[i], (i + 1) * 1000ull);
            append(frames, decoder->takeFrames());
        }
        // Two pictures wait for the ones that precede them in display order.
        require(frames.size() == units.size() - 2, "the last pictures wait until the stream is drained");
        decoder->drain();
        append(frames, decoder->takeFrames());
        checkDisplayOrder(frames, closedDisplay, 20, {}, "closed groups leave in display order");
    }
    // Open groups: the RASL pictures after a CRA in mid-stream are decoded, and
    // after a CRA the decoder starts at they are not.
    {
        const auto units = accessUnits(read("hevc-open-gop.bin"));
        require(units.size() == 24, "the open fixture has twenty-four pictures");
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        std::vector<DecodedVideoFrame> frames;
        for (std::size_t i = 0; i < units.size(); ++i) decoder->submit(units[i], (i + 1) * 1000ull);
        decoder->drain();
        append(frames, decoder->takeFrames());
        checkDisplayOrder(frames, openDisplay, 24, {}, "open groups, decoded from the start");

        auto cut = makeVideoDecoder(VideoCodec::hevc);
        std::vector<DecodedVideoFrame> fromCra;
        for (std::size_t i = 5; i < units.size(); ++i) cut->submit(units[i], (i + 1) * 1000ull);
        cut->drain();
        append(fromCra, cut->takeFrames());
        checkDisplayOrder(fromCra, openDisplay, 24, {0, 1, 2, 3, 4, 6, 7, 8}, "open groups, decoded from a CRA");
    }
    // A packet that carries several pictures is decoded picture by picture.
    {
        const auto units = accessUnits(read("hevc-bframes.bin"));
        Bytes joined;
        for (std::size_t i = 0; i < 5; ++i) joined.insert(joined.end(), units[i].begin(), units[i].end());
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        decoder->submit(joined, 777000);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 5, "all five pictures of a joined packet were decoded");
        for (std::size_t k = 0; k < frames.size(); ++k)
            require(std::abs(meanLuma(frames[k]) - (16.0 + 10.0 * k)) <= 3.0, "joined pictures leave in display order");
    }
    // A seek keeps the parameter sets: the first picture after it carries none.
    {
        const auto units = accessUnits(read("hevc-bframes.bin"));
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        for (std::size_t i = 0; i < 7; ++i) decoder->submit(units[i], (i + 1) * 1000ull);
        const auto before = decoder->format();
        decoder->discard();
        require(decoder->takeFrames().empty(), "a seek shows nothing decoded before it");
        require(decoder->format() == before, "a seek keeps the format");
        std::vector<DecodedVideoFrame> frames;
        for (std::size_t i = 12; i < units.size(); ++i) {
            decoder->submit(i == 12 ? withoutParameterSets(units[i]) : units[i], (i + 1) * 1000ull);
            append(frames, decoder->takeFrames());
        }
        decoder->drain();
        append(frames, decoder->takeFrames());
        require(frames.size() == 8, "the eight pictures after the seek");
        for (std::size_t k = 0; k < frames.size(); ++k) {
            require(frames[k].timestamp > 12000, "a picture decoded before the seek was shown after it");
            require(std::abs(meanLuma(frames[k]) - (16.0 + 10.0 * (12 + k))) <= 3.0, "pictures after a seek leave in order");
        }
        // And a second seek does the same thing again.
        decoder->discard();
        for (std::size_t i = 12; i < units.size(); ++i) decoder->submit(withoutParameterSets(units[i]), (i + 1) * 1000ull);
        decoder->drain();
        require(decoder->takeFrames().size() == 8, "a second seek");
    }
}

// The 10-bit stream of an 8-bit request, and a full-range stream, a window, and sizes
// that the capability list states.
double sampleError(const Bytes& reference, const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& chroma)
{
    double worst = 0;
    for (std::size_t i = 0; i < luma.size(); ++i) worst = std::max(worst, std::abs(double(luma[i]) - reference[i]));
    for (std::size_t i = 0; i < chroma.size(); ++i)
        worst = std::max(worst, std::abs(double(chroma[i]) - reference[luma.size() + i]));
    return worst;
}

void hevcRawFormats()
{
    using namespace uml::platform;
    // A Main 10 stream decoded to 8-bit NV12 when the client asks for it.
    {
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
        decoder->submit(read("hevc-main10.bin"), 5);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1, "one 8-bit picture of a Main 10 stream");
        const auto& frame = frames[0];
        require(frame.layout == VideoPixelLayout::planarYuv && frame.format == VideoFormat {128, 96, 8},
                "a Main 10 stream decoded to 8 bits is NV12");
        require(frame.luma.size() == 128u * 96u && frame.chroma.size() == 128u * 48u, "NV12 plane sizes");
        require(decoder->format().bitDepth == 10, "the stream is still a 10-bit one");
        const auto reference = read("hevc-main10.raw");
        double worst = 0;
        const std::size_t lumaSamples = 128 * 96;
        for (std::size_t i = 0; i < frame.luma.size() + frame.chroma.size(); ++i) {
            const auto& plane = i < lumaSamples ? frame.luma : frame.chroma;
            const auto got = double(plane[i < lumaSamples ? i : i - lumaSamples]);
            const auto wanted = double((reference[2 * i] | (reference[2 * i + 1] << 8)) >> 6) / 4.0;
            worst = std::max(worst, std::abs(got - wanted));
        }
        std::printf("hevc-main10 as NV12: max error %.2f levels\n", worst);
        require(worst <= 2.0, "the 8-bit picture is the 10-bit one narrowed");
        // Without the request the stream keeps its depth.
        auto native = makeVideoDecoder(VideoCodec::hevc);
        native->submit(read("hevc-main10.bin"), 5);
        native->drain();
        require(native->takeFrames().at(0).format.bitDepth == 10, "10-bit by default");
        // An 8-bit stream is the same either way.
        auto eight = makeVideoDecoder(VideoCodec::hevc);
        eight->requestOutputLayout(VideoPixelLayout::planar8Bit);
        eight->submit(read("hevc-main.bin"), 5);
        eight->drain();
        require(eight->takeFrames().at(0).format.bitDepth == 8, "8-bit stays 8-bit");
    }
    // A stream that states full range is decoded to full range.
    {
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        decoder->submit(read("hevc-full-range.bin"), 5);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1 && frames[0].color.fullRange, "the frame says it is full range");
        const auto error = sampleError(read("hevc-full-range.raw"), frames[0].luma, frames[0].chroma);
        std::printf("hevc-full-range: max error %.0f levels\n", error);
        require(error <= 2.0, "full-range samples are not converted to video range");
        // A stream that says nothing stays video range.
        auto limited = makeVideoDecoder(VideoCodec::hevc);
        limited->submit(read("hevc-main.bin"), 5);
        limited->drain();
        require(!limited->takeFrames().at(0).color.fullRange, "video range by default");
    }
}

void hevcWindow()
{
    using namespace uml::platform;
    // A conformance window with offsets on the left and the top.
    {
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        const auto stream = read("hevc-window.bin");
        decoder->inspect(stream);
        require(decoder->format() == VideoFormat {120, 90, 8}, "the stream reports the picture it shows");
        require(decoder->takeFrames().empty(), "inspecting decodes nothing");
        decoder->submit(stream, 5);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1 && frames[0].format == VideoFormat {120, 90, 8}, "the picture is the window");
        require(frames[0].luma.size() == 120u * 90u && frames[0].chroma.size() == 120u * 45u, "cropped plane sizes");
        const auto error = sampleError(read("hevc-window.raw"), frames[0].luma, frames[0].chroma);
        std::printf("hevc-window: max error %.0f levels\n", error);
        require(error <= 2.0, "the window is cut at its own origin");
        auto packed = makeVideoDecoder(VideoCodec::hevc);
        packed->requestOutputLayout(VideoPixelLayout::packedBgra);
        packed->submit(stream, 5);
        packed->drain();
        const auto pictures = packed->takeFrames();
        require(pictures.size() == 1 && pictures[0].pixels.size() == 120u * 90u * 4u, "a cropped packed picture");
    }
}

/* The same fixtures for H.264, in the order the encoder coded them: picture N
 * of the stream is source frame display[N]. */
const unsigned h264ClosedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 9, 10, 12, 16, 14, 13, 15, 19, 17, 18};

void h264DisplayOrder()
{
    using namespace uml::platform;
    // B-pictures with the limits the stream states, and with the level's.
    for (const char* name : {"h264-bframes.bin", "h264-no-restriction.bin"}) {
        const auto units = h264AccessUnits(read(name));
        require(units.size() == 20, "the H.264 order fixtures have twenty pictures");
        auto decoder = makeVideoDecoder(VideoCodec::h264);
        std::vector<DecodedVideoFrame> frames;
        for (std::size_t i = 0; i < units.size(); ++i) {
            decoder->submit(units[i], (i + 1) * 1000ull);
            append(frames, decoder->takeFrames());
        }
        // Pictures wait for the ones that precede them in display order.
        const bool stated = std::string(name) == "h264-bframes.bin";
        if (stated) require(frames.size() == units.size() - 2, "the last pictures wait until the stream is drained");
        else require(frames.size() < units.size(), "the level's limits hold the last pictures back");
        decoder->drain();
        append(frames, decoder->takeFrames());
        checkDisplayOrder(frames, h264ClosedDisplay, 20, {}, name);
    }
    // Order count type 2 is decoding order, and nothing is held back.
    {
        const auto units = h264AccessUnits(read("h264-poc2.bin"));
        auto decoder = makeVideoDecoder(VideoCodec::h264);
        for (std::size_t i = 0; i < units.size(); ++i) {
            decoder->submit(units[i], (i + 1) * 1000ull);
            const auto frames = decoder->takeFrames();
            require(frames.size() == 1 && frames[0].timestamp == (i + 1) * 1000ull,
                    "a stream that cannot reorder shows each picture as it is decoded");
            require(std::abs(meanLuma(frames[0]) - (16.0 + 10.0 * i)) <= 3.0, "the picture is the one that was sent");
        }
    }
    // A packet that carries several pictures is decoded picture by picture.
    {
        const auto units = h264AccessUnits(read("h264-bframes.bin"));
        Bytes joined;
        for (std::size_t i = 0; i < 5; ++i) joined.insert(joined.end(), units[i].begin(), units[i].end());
        auto decoder = makeVideoDecoder(VideoCodec::h264);
        decoder->submit(joined, 777000);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 5, "all five pictures of a joined packet were decoded");
        for (std::size_t k = 0; k < frames.size(); ++k)
            require(std::abs(meanLuma(frames[k]) - (16.0 + 10.0 * k)) <= 3.0, "joined pictures leave in display order");
    }
    // A seek keeps the parameter sets: the first picture after it carries none.
    {
        const auto units = h264AccessUnits(read("h264-bframes.bin"));
        auto decoder = makeVideoDecoder(VideoCodec::h264);
        for (std::size_t i = 0; i < 7; ++i) decoder->submit(units[i], (i + 1) * 1000ull);
        const auto before = decoder->format();
        decoder->discard();
        require(decoder->takeFrames().empty(), "a seek shows nothing decoded before it");
        require(decoder->format() == before, "a seek keeps the format");
        std::vector<DecodedVideoFrame> frames;
        for (std::size_t i = 12; i < units.size(); ++i) {
            decoder->submit(i == 12 ? withoutH264ParameterSets(units[i]) : units[i], (i + 1) * 1000ull);
            append(frames, decoder->takeFrames());
        }
        decoder->drain();
        append(frames, decoder->takeFrames());
        require(frames.size() == 8, "the eight pictures after the seek");
        for (std::size_t k = 1; k < frames.size(); ++k) {
            require(frames[k].timestamp > 12000, "a picture decoded before the seek was shown after it");
            require(meanLuma(frames[k]) > meanLuma(frames[k - 1]), "pictures after a seek leave in order");
        }
        // And a second seek does the same thing again.
        decoder->discard();
        for (std::size_t i = 12; i < units.size(); ++i) decoder->submit(withoutH264ParameterSets(units[i]), (i + 1) * 1000ull);
        decoder->drain();
        require(decoder->takeFrames().size() == 8, "a second seek");
    }
}

/* The reorder depth a client keeps its pipeline deep enough for: what the
 * stream states, the level's limit when it states none, and nothing for a
 * stream whose pictures leave as they are decoded. It is known from the
 * parameter sets, before any picture is decoded. */
void reorderDepth(const char* name, uml::platform::VideoCodec codec, unsigned expected)
{
    using namespace uml::platform;
    auto decoder = makeVideoDecoder(codec);
    require(decoder->reorderDepth() == 0, "a decoder that has read nothing reports no reorder depth");
    const auto units = codec == VideoCodec::hevc ? accessUnits(read(name)) : h264AccessUnits(read(name));
    decoder->inspect(units.front());
    require(decoder->takeFrames().empty(), "inspecting decodes nothing");
    if (decoder->reorderDepth() != expected)
        std::fprintf(stderr, "%s: reorder depth %u, expected %u\n", name, decoder->reorderDepth(), expected);
    require(decoder->reorderDepth() == expected, "the reorder depth is not the stream's");
}

void reorderDepths(bool hevc, bool h264)
{
    using namespace uml::platform;
    if (hevc) reorderDepth("hevc-bframes.bin", VideoCodec::hevc, 2);
    if (h264) {
        reorderDepth("h264-bframes.bin", VideoCodec::h264, 2);
        reorderDepth("h264-no-restriction.bin", VideoCodec::h264, 16);
        reorderDepth("h264-poc2.bin", VideoCodec::h264, 0);
    }
}

void h264Window()
{
    using namespace uml::platform;
    // Frame cropping with offsets on the left and the top.
    auto decoder = makeVideoDecoder(VideoCodec::h264);
    const auto stream = read("h264-window.bin");
    decoder->inspect(stream);
    require(decoder->format() == VideoFormat {120, 90, 8}, "the stream reports the picture it shows");
    require(decoder->takeFrames().empty(), "inspecting decodes nothing");
    decoder->submit(stream, 5);
    decoder->drain();
    const auto frames = decoder->takeFrames();
    require(frames.size() == 1 && frames[0].format == VideoFormat {120, 90, 8}, "the picture is the window");
    require(frames[0].luma.size() == 120u * 90u && frames[0].chroma.size() == 120u * 45u, "cropped plane sizes");
    const auto error = sampleError(read("h264-window.raw"), frames[0].luma, frames[0].chroma);
    std::printf("h264-window: max error %.0f levels\n", error);
    require(error <= 2.0, "the window is cut at its own origin");
    auto packed = makeVideoDecoder(VideoCodec::h264);
    packed->requestOutputLayout(VideoPixelLayout::packedBgra);
    packed->submit(stream, 5);
    packed->drain();
    const auto pictures = packed->takeFrames();
    require(pictures.size() == 1 && pictures[0].pixels.size() == 120u * 90u * 4u, "a cropped packed picture");
}

/* What the host says it decodes is checked by decoding it: a picture of the
 * largest size the capability list states, in the highest tier and level. */
void hevcLimits(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    const auto main = std::find_if(capabilities.begin(), capabilities.end(),
                                   [](const auto& c) { return c.profile == VideoProfile::hevcMain; });
    if (main == capabilities.end()) return;
    struct Size { const char* name; std::uint32_t width, height; };
    for (const auto& size : {Size {"hevc-high-tier-4k", 4096, 2160}, Size {"hevc-high-tier-8k", 8192, 4320},
                             Size {"hevc-portrait-4320x8192", 4320, 8192}}) {
        if (main->maximumWidth < size.width || main->maximumHeight < size.height) continue;
        auto decoder = makeVideoDecoder(VideoCodec::hevc);
        decoder->submit(read(std::string(size.name) + ".bin"), 5);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1 && frames[0].format == VideoFormat {size.width, size.height, 8},
                "a size the host advertises did not decode");
        require(std::abs(meanLuma(frames[0]) - 128.0) <= 3.0, "an advertised size decoded to the wrong picture");
        std::printf("%s: %ux%u decoded in hardware\n", size.name, size.width, size.height);
    }
}

/* The hardware limits a dimension and not the area, so the host accepts every
 * picture within the advertised rectangle: a portrait one as a landscape one,
 * and a square one the level's frame area would not allow. One a dimension
 * beyond it is refused. */
void h264Shapes(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                                  [](const auto& c) { return c.profile == VideoProfile::h264High; });
    if (cap == capabilities.end()) return;
    struct Shape { const char* name; std::uint32_t width, height; };
    for (const auto& shape : {Shape {"h264-portrait-2304x4096", 2304, 4096}, Shape {"h264-square-4096x4096", 4096, 4096}}) {
        require(cap->maximumWidth >= shape.width && cap->maximumHeight >= shape.height,
                "the advertised H.264 size range leaves out a picture the hardware decodes");
        auto decoder = makeVideoDecoder(VideoCodec::h264);
        decoder->submit(read(std::string(shape.name) + ".bin"), 5);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1 && frames[0].format == VideoFormat {shape.width, shape.height, 8},
                "an H.264 picture within the advertised range did not decode");
        require(std::abs(meanLuma(frames[0]) - 128.0) <= 3.0, "an H.264 picture within the range decoded wrongly");
        std::printf("%s: %ux%u decoded in hardware\n", shape.name, shape.width, shape.height);
    }
    /* 4112 is the first width the hardware refuses. */
    require(cap->maximumWidth < 4112 && cap->maximumHeight < 4112, "the advertised H.264 size range reaches 4112");
    auto decoder = makeVideoDecoder(VideoCodec::h264);
    bool refused = false;
    try { decoder->submit(read("h264-too-wide-4112x2304.bin"), 5); } catch (const std::runtime_error&) { refused = true; }
    require(refused, "an H.264 picture beyond the advertised range was accepted");
}

/* The H.264 level the host states is the one its hardware was seen to decode:
 * the largest frame level 5.2 allows, 4096 x 2304 (36864 macroblocks), at
 * that level. Level 6 allows frames up to 8192 x 4352, which the hardware
 * refuses to open, so no capability may state it until a fixture decodes one. */
void h264Limits(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    for (const auto& cap : capabilities) {
        if (cap.codec != VideoCodec::h264) continue;
        require(cap.maximumLevel <= 52, "an H.264 level above 5.2 is advertised without a fixture that decodes it");
        /* The wire states one level per format, and High is the profile whose
         * stream shows it: the profiles share the hardware and the frame size. */
        if (cap.profile != VideoProfile::h264High) continue;
        require(cap.maximumLevel == 52 && cap.maximumWidth >= 4096 && cap.maximumHeight >= 2304,
                "High does not state the level its fixture decodes");
        auto decoder = makeVideoDecoder(VideoCodec::h264);
        decoder->submit(read("h264-high-level-5-2.bin"), 5);
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1 && frames[0].format == VideoFormat {4096, 2304, 8},
                "an H.264 level the host advertises did not decode");
        require(std::abs(meanLuma(frames[0]) - 128.0) <= 3.0, "an advertised level decoded to the wrong picture");
        std::puts("h264-high-level-5-2: 4096x2304 level 5.2 decoded in hardware");
    }
}

/* The VP9 size range the host states is the rectangle the guest's symmetric
 * Android size range requires, and every shape inside it decodes: the square
 * the area of a level would not allow, a portrait picture and a 4K one. One
 * a dimension beyond it is refused before the hardware sees it. */
void vp9Limits(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                                  [](const auto& c) { return c.profile == VideoProfile::vp9Profile0; });
    if (cap == capabilities.end()) return;
    require(cap->maximumWidth == 4096 && cap->maximumHeight == 4096 && cap->bitDepth == 8 && cap->maximumLevel == 0,
            "the advertised VP9 range is not the 4096 x 4096 square Android is given");
    struct Shape { const char* name; std::uint32_t width, height; };
    for (const auto& shape : {Shape {"vp9-uhd-3840x2160", 3840, 2160}, Shape {"vp9-portrait-2304x4096", 2304, 4096},
                              Shape {"vp9-square-4096x4096", 4096, 4096}}) {
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        decoder->submit(read(std::string(shape.name) + ".bin"), 5);
        require(decoder->format() == VideoFormat {shape.width, shape.height, 8}, "the stream size was not reported");
        decoder->drain();
        const auto frames = decoder->takeFrames();
        require(frames.size() == 1 && frames[0].format == VideoFormat {shape.width, shape.height, 8},
                "a VP9 picture within the advertised range did not decode");
        require(std::abs(meanLuma(frames[0]) - 128.0) <= 3.0, "a VP9 picture within the range decoded wrongly");
        std::printf("%s: %ux%u decoded in hardware\n", shape.name, shape.width, shape.height);
    }
    /* 4112 is the first width past it: refused for the guest as the other codecs are. */
    auto decoder = makeVideoDecoder(VideoCodec::vp9);
    bool refused = false;
    try { decoder->submit(read("vp9-too-wide-4112x2304.bin"), 5); } catch (const std::runtime_error&) { refused = true; }
    require(refused, "a VP9 picture beyond the advertised range was accepted");
    require(decoder->format() == VideoFormat {}, "a refused VP9 picture changed the stream size");
}

/* The hash of each picture of vp9-superframes.ivf as the NV12 libvpx writes it. */
constexpr std::uint32_t vp9ClipHashes[] = {
        0x43dbd20b, 0xe42453cf, 0xf304143b, 0x331687a7, 0x0503b4ac, 0xf4edc0e0, 0x78f2e9e6, 0xdabd9c03,
        0xe8f2e510, 0x2663e199, 0x3b85cde4, 0xc4fda8fa, 0xd4aeb5af, 0x363446ca, 0xfa24799c, 0x74a9a138,
        0x0a5b021e, 0x3fdcc5a3, 0xd87b1ee1, 0xc0f26d58, 0x320305ad, 0x7142fceb, 0xd4e512e0, 0x5731ebe9,
        0xa95c8819, 0x9b8b1dae, 0x9e009210, 0x2d025e6e, 0xe04c90ad, 0x9eadcae1, 0xcbc865fc, 0xb0565c12,
        0xe552355f, 0x46538305, 0x17c97a35, 0xf153862e, 0x4e533ad4, 0x743f009a, 0x84979f4d, 0x50eec70a};

/* A clip of 40 packets: three of them superframes that carry a hidden
 * alternate-reference frame ahead of the shown one, and a key frame at the
 * 31st that changes the size from 64 x 64 to 96 x 80. Each packet shows one
 * picture, and a VP9 decoder is exact, so the picture's hash is the hash of
 * the NV12 a reference decoder (libvpx) writes for it. */
void vp9Clip(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    if (std::none_of(capabilities.begin(), capabilities.end(),
                     [](const auto& c) { return c.profile == VideoProfile::vp9Profile0; }))
        return;
    const auto packets = ivfPackets(read("vp9-superframes.ivf"));
    require(packets.size() == 40, "the VP9 clip is not 40 packets");
    auto decoder = makeVideoDecoder(VideoCodec::vp9);
    unsigned superframes = 0;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto split = vp9::splitSuperframe(packets[i]);
        require(split.has_value(), "a packet of the clip is not one the readers accept");
        if (split->size() > 1) ++superframes;
        decoder->submit(packets[i], (i + 1) * 1000);
        /* A packet's picture is complete once the caller drains or the next
         * arrives; the decoder holds nothing back for a VP9 stream. */
        auto frames = decoder->takeFrames();
        require(frames.size() == 1, "a VP9 packet did not show exactly one picture");
        const VideoFormat size = i < 30 ? VideoFormat {64, 64, 8} : VideoFormat {96, 80, 8};
        require(frames[0].format == size && decoder->format() == size, "a VP9 picture has the wrong size");
        require(frames[0].timestamp == (i + 1) * 1000, "a VP9 picture lost its timestamp");
        require(frameHash(frames[0]) == vp9ClipHashes[i], "a VP9 picture differs from the reference decoder's");
    }
    decoder->drain();
    require(decoder->takeFrames().empty(), "a VP9 clip left a picture behind");
    require(superframes == 3, "the VP9 clip does not hold its three superframes");
    std::puts("vp9-superframes: 40 pictures bit-exact, 3 superframes, one resolution change");
}

/* The hash of a 10-bit frame as the 8-bit NV12 it narrows to. The rule is the
 * one the host applies to every 10-bit picture an 8-bit client takes: a sample
 * rounds to (sample + 2) >> 2, limited to 255. It is written here from the
 * 10-bit value, where the decoder works on the 16-bit word that holds it. */
std::uint32_t narrowedHash(const uml::platform::DecodedVideoFrame& frame)
{
    std::uint32_t hash = 0x811c9dc5u;
    for (const auto* plane : {&frame.luma, &frame.chroma})
        for (std::size_t i = 0; i + 1 < plane->size(); i += 2) {
            const unsigned sample = (unsigned((*plane)[i]) | (unsigned((*plane)[i + 1]) << 8)) >> 6;
            hash = (hash ^ std::min(255u, (sample + 2) >> 2)) * 0x01000193u;
        }
    return hash;
}

/* VP9 Profile 2 is 10-bit 4:2:0. It is advertised at the size range of Profile
 * 0, and an 8-bit client takes it narrowed as it takes HEVC Main 10. */
void vp9Profile2Limits(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                                  [](const auto& c) { return c.profile == VideoProfile::vp9Profile2; });
    if (cap == capabilities.end()) return;
    require(cap->codec == VideoCodec::vp9 && cap->maximumWidth == 4096 && cap->maximumHeight == 4096 &&
                cap->bitDepth == 10 && cap->maximumLevel == 0,
            "the advertised VP9 Profile 2 range is not the 4096 x 4096 square of Profile 0, in 10 bits");
    for (const bool narrow : {false, true}) {
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        if (narrow) decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
        decoder->submit(read("vp9-profile2-square-4096x4096.bin"), 5);
        require(decoder->format() == VideoFormat {4096, 4096, 10}, "the 10-bit stream size was not reported");
        decoder->drain();
        const auto frames = decoder->takeFrames();
        const auto depth = narrow ? 8u : 10u;
        require(frames.size() == 1 && frames[0].format == VideoFormat {4096, 4096, depth},
                "a VP9 Profile 2 picture within the advertised range did not decode");
        require(std::abs(meanLuma(frames[0]) - 127.0) <= 3.0, "a VP9 Profile 2 picture within the range decoded wrongly");
        std::printf("vp9-profile2-square-4096x4096: decoded in hardware, %s\n", narrow ? "narrowed to 8 bits" : "10-bit");
    }
    auto decoder = makeVideoDecoder(VideoCodec::vp9);
    bool refused = false;
    try { decoder->submit(uml::testing::vp9::keyFrame(2, 4112, 2304), 5); } catch (const std::runtime_error&) { refused = true; }
    require(refused, "a VP9 Profile 2 picture beyond the advertised range was accepted");
    require(decoder->format() == VideoFormat {}, "a refused VP9 Profile 2 picture changed the stream size");
}

/* The hash of each picture of vp9-profile2-superframes.ivf: libvpx's 10-bit output narrowed to 8 bits. */
constexpr std::uint32_t vp9Profile2ClipHashes[] = {
        0xae5a53b4, 0xc3ce3063, 0xf4897d3b, 0x32ac1857, 0xfe99c050, 0xcac607f8, 0xe03fb722, 0xa00eb758,
        0x83afd197, 0x275106a1, 0x84afa6da, 0x8e9f7b34, 0xdc260ada, 0xbc697744, 0x4080cf19, 0x3db77c5a,
        0x30c01b41, 0xf64a9489, 0xaa5f4b52, 0xc9ca0514, 0x577c0ee1, 0x1a2e7a27, 0xceafc4f7, 0x3a014f66,
        0xa3164370, 0xbe498402, 0xb5339302, 0xb0b29424, 0x3ec23bab, 0x4041b52c, 0xd1d82011, 0x3b44a9d4,
        0x42c519c2, 0x7ca235c3, 0xc2aadcb0, 0xaff21620, 0x51fd0062, 0x88b0994f, 0x1ecc7b61, 0xc7bd600b};

/* A clip of 40 packets of Profile 2, built as the Profile 0 one is: four
 * superframes that carry a hidden alternate-reference frame, and a key frame at
 * the 31st that changes the size from 64 x 64 to 96 x 80. The expected hashes
 * are libvpx's 10-bit output narrowed by (sample + 2) >> 2, limited to 255, with
 * no tolerance: the hardware's 10-bit pictures equal libvpx's, and the
 * narrowing is exact integer arithmetic. They are checked both as the narrowed
 * NV12 an 8-bit client takes and as the 10-bit pictures narrowed here. */
void vp9Profile2Clip(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    if (std::none_of(capabilities.begin(), capabilities.end(),
                     [](const auto& c) { return c.profile == VideoProfile::vp9Profile2; }))
        return;
    const auto packets = ivfPackets(read("vp9-profile2-superframes.ivf"));
    require(packets.size() == 40, "the VP9 Profile 2 clip is not 40 packets");
    for (const bool narrow : {true, false}) {
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        if (narrow) decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
        unsigned superframes = 0;
        for (std::size_t i = 0; i < packets.size(); ++i) {
            const auto split = vp9::splitSuperframe(packets[i]);
            require(split.has_value(), "a packet of the Profile 2 clip is not one the readers accept");
            if (split->size() > 1) ++superframes;
            decoder->submit(packets[i], (i + 1) * 1000);
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "a VP9 Profile 2 packet did not show exactly one picture");
            const VideoFormat size = i < 30 ? VideoFormat {64, 64, narrow ? 8u : 10u} : VideoFormat {96, 80, narrow ? 8u : 10u};
            require(frames[0].format == size, "a VP9 Profile 2 picture has the wrong size or depth");
            require(decoder->format().bitDepth == 10, "a VP9 Profile 2 stream is not reported as 10-bit");
            require(frames[0].timestamp == (i + 1) * 1000, "a VP9 Profile 2 picture lost its timestamp");
            require((narrow ? frameHash(frames[0]) : narrowedHash(frames[0])) == vp9Profile2ClipHashes[i],
                    "a VP9 Profile 2 picture differs from libvpx's, narrowed");
        }
        decoder->drain();
        require(decoder->takeFrames().empty(), "a VP9 Profile 2 clip left a picture behind");
        require(superframes == 4, "the VP9 Profile 2 clip does not hold its four superframes");
        std::printf("vp9-profile2-superframes (%s): 40 pictures bit-exact against libvpx narrowed, 4 superframes, one "
                    "resolution change\n", narrow ? "NV12" : "P010 narrowed here");
    }
}

/* A packet whose hidden frame comes ahead of a key frame that changes the size
 * decodes as libvpx decodes it: the key frame is where the stream's
 * configuration changes and the picture is its own. The packet is the hidden
 * alternate-reference frame of the clip's first superframe ahead of the key
 * frame that starts the clip's second size, 96 x 80, in a stream that has
 * decoded the 64 x 64 pictures before it. libvpx shows the key frame's own
 * picture for such a packet, which is what the clip's packet of that key frame
 * shows, and the inter frame after it is decoded from it. */
void vp9ResizeInSuperframe(const std::vector<uml::platform::VideoDecodeCapability>& capabilities,
                           uml::platform::VideoProfile profile, const char* clipName,
                           const std::uint32_t* expected)
{
    using namespace uml::platform;
    namespace frames = uml::testing::vp9;
    if (std::none_of(capabilities.begin(), capabilities.end(), [profile](const auto& c) { return c.profile == profile; }))
        return;
    const bool tenBit = profile == VideoProfile::vp9Profile2;
    const auto packets = ivfPackets(read(clipName));
    require(packets.size() == 40, "the VP9 clip is not 40 packets");
    std::size_t superframe = 0;
    while (superframe < 30 && vp9::splitSuperframe(packets[superframe])->size() < 2) ++superframe;
    require(superframe < 30, "the VP9 clip has no superframe among its 64 x 64 pictures");
    const auto hidden = (*vp9::splitSuperframe(packets[superframe]))[0];
    const auto packet = frames::superframe({std::vector<std::uint8_t>(hidden.begin(), hidden.end()), packets[30]});
    for (const bool narrow : {false, true}) {
        if (!tenBit && narrow) continue;
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        if (narrow) decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
        for (std::size_t i = 0; i < superframe; ++i) decoder->submit(packets[i], i + 1);
        decoder->takeFrames();
        require(decoder->format().width == 64, "the stream did not start at 64 x 64");
        decoder->submit(packet, 100);
        const unsigned depth = tenBit && !narrow ? 10u : 8u;
        require(decoder->format() == VideoFormat {96, 80, tenBit ? 10u : 8u},
                "a key frame after a hidden frame did not change the stream size");
        auto shown = decoder->takeFrames();
        require(shown.size() == 1 && shown[0].timestamp == 100 && shown[0].format == VideoFormat {96, 80, depth},
                "a key frame after a hidden frame did not show its own picture");
        const auto hashOf = [&](const DecodedVideoFrame& frame) { return tenBit && !narrow ? narrowedHash(frame) : frameHash(frame); };
        require(hashOf(shown[0]) == expected[30], "a key frame after a hidden frame differs from the reference decoder's");
        // The next picture is predicted from the key frame the packet carried.
        decoder->submit(packets[31], 101);
        decoder->drain();
        const auto next = decoder->takeFrames();
        require(next.size() == 1 && next[0].timestamp == 101 && hashOf(next[0]) == expected[31],
                "the picture after a resizing superframe differs from the reference decoder's");
    }
    std::printf("vp9-resize-in-superframe (%s): hidden frame, then a 96 x 80 key frame, bit-exact\n",
                tenBit ? "profile 2" : "profile 0");
}

/* A packet shows what its last frame shows, as libvpx shows it: nothing when
 * that frame is not shown, even a key frame of another size, which is still
 * decoded for the references it refreshes, and the picture of a slot when it
 * is a show-existing frame. The hidden key frame here is the clip's 96 x 80 one
 * with show_frame cleared, behind a shown 64 x 64 key frame, and the inter
 * frame after it is predicted from it. */
void vp9FinalFrame(const std::vector<uml::platform::VideoDecodeCapability>& capabilities,
                   uml::platform::VideoProfile profile, const char* clipName, const std::uint32_t* expected)
{
    using namespace uml::platform;
    namespace frames = uml::testing::vp9;
    if (std::none_of(capabilities.begin(), capabilities.end(), [profile](const auto& c) { return c.profile == profile; }))
        return;
    const bool tenBit = profile == VideoProfile::vp9Profile2;
    const unsigned profileBits = tenBit ? 2 : 0;
    const auto packets = ivfPackets(read(clipName));
    require(packets.size() == 40, "the VP9 clip is not 40 packets");
    auto hiddenKey = packets[30];
    hiddenKey[0] &= 0xfd;                                   // show_frame
    const auto hiddenKeyHeader = vp9::parseFrameHeader(hiddenKey);
    require(hiddenKeyHeader && hiddenKeyHeader->key && !hiddenKeyHeader->showFrame, "the hidden key frame is not one");
    std::size_t superframe = 0;
    while (vp9::splitSuperframe(packets[superframe])->size() < 2) ++superframe;
    const auto arf = (*vp9::splitSuperframe(packets[superframe]))[0];
    for (const bool narrow : {false, true}) {
        if (!tenBit && narrow) continue;
        const auto hashOf = [&](const DecodedVideoFrame& frame) { return tenBit && !narrow ? narrowedHash(frame) : frameHash(frame); };
        const auto primed = [&] {
            auto decoder = makeVideoDecoder(VideoCodec::vp9);
            if (narrow) decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
            decoder->submit(packets[0], 1);
            require(decoder->takeFrames().size() == 1, "the stream's key frame did not show a picture");
            return decoder;
        };
        {
            auto decoder = primed();
            decoder->submit(frames::superframe({packets[0], hiddenKey}), 50);
            decoder->drain();
            require(decoder->takeFrames().empty(), "a packet that ends in a hidden key frame showed a picture");
            require(decoder->format() == VideoFormat {96, 80, tenBit ? 10u : 8u},
                    "a hidden key frame did not change the stream size");
            decoder->submit(packets[31], 51);
            decoder->drain();
            const auto next = decoder->takeFrames();
            require(next.size() == 1 && next[0].timestamp == 51 && next[0].format.width == 96 &&
                    hashOf(next[0]) == expected[31],
                    "the picture after a hidden key frame is not predicted from it");
        }
        // A show-existing frame as the whole packet, and as the last of two entries behind a hidden frame.
        for (const bool behindHidden : {false, true}) {
            auto decoder = primed();
            const auto entry = frames::showExisting(profileBits, behindHidden ? 0 : 3);
            decoder->submit(behindHidden ? frames::superframe({std::vector<std::uint8_t>(arf.begin(), arf.end()), entry})
                                         : entry, 60);
            decoder->drain();
            const auto shown = decoder->takeFrames();
            require(shown.size() == 1 && shown[0].timestamp == 60 && hashOf(shown[0]) == expected[0],
                    "a show-existing frame at the end of a packet did not show the slot's picture");
        }
        /* The clip's first alternate-reference frame is hidden and refreshes
         * slot 2. Show-existing of that slot shows the frame, whether the
         * hidden frame came in an earlier packet or in the same one. */
        constexpr unsigned arfSlot = 2;
        const std::uint32_t arfHash = tenBit ? 0xdc260adau : 0xd4aeb5afu;
        for (const bool samePacket : {false, true}) {
            auto decoder = primed();
            const auto entry = frames::showExisting(profileBits, arfSlot);
            if (samePacket) {
                decoder->submit(frames::superframe({std::vector<std::uint8_t>(arf.begin(), arf.end()), entry}), 70);
            } else {
                decoder->submit(arf, 70);
                decoder->drain();
                require(decoder->takeFrames().empty(), "a hidden alternate-reference frame showed a picture");
                decoder->submit(entry, 71);
            }
            decoder->drain();
            const auto shown = decoder->takeFrames();
            require(shown.size() == 1 && shown[0].timestamp == (samePacket ? 70u : 71u) && hashOf(shown[0]) == arfHash,
                    "show-existing of a slot a hidden alternate-reference frame refreshed differs from the reference decoder's");
        }
    }
    std::printf("vp9-final-frame (%s): a hidden last key frame shows nothing, show-existing shows its slot, "
                "also one a hidden alternate-reference frame refreshed\n",
                tenBit ? "profile 2" : "profile 0");
}

/* One decoder carries a stream through a change of profile at a key frame, as a
 * player does between two clips: the session is rebuilt for the new profile and
 * depth, and the pictures are the right ones. */
void vp9ProfileSwitch(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    const auto has = [&](VideoProfile profile) {
        return std::any_of(capabilities.begin(), capabilities.end(),
                           [profile](const auto& c) { return c.profile == profile; });
    };
    if (!has(VideoProfile::vp9Profile0) || !has(VideoProfile::vp9Profile2)) return;
    const auto eight = read("vp9-profile0.bin");
    const auto ten = read("vp9-profile2.bin");
    const auto reference = read("vp9-profile2.raw");
    for (const bool narrow : {false, true}) {
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        if (narrow) decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
        std::uint64_t timestamp = 0;
        for (const auto* packet : {&eight, &ten, &ten, &eight, &ten}) {
            const bool tenBit = packet == &ten;
            decoder->submit(*packet, ++timestamp);
            decoder->drain();
            const auto frames = decoder->takeFrames();
            require(frames.size() == 1 && frames[0].timestamp == timestamp,
                    "a key frame after a change of VP9 profile did not decode");
            require(frames[0].format == VideoFormat {128, 96, tenBit && !narrow ? 10u : 8u},
                    "a key frame after a change of VP9 profile has the wrong depth");
            require(decoder->format().bitDepth == (tenBit ? 10u : 8u), "the stream depth did not follow the profile");
            if (!tenBit) continue;
            // The picture is the reference's own, narrowed: exactly.
            std::uint32_t wanted = 0x811c9dc5u;
            for (std::size_t i = 0; i < reference.size(); i += 2) {
                const unsigned sample = (unsigned(reference[i]) | (unsigned(reference[i + 1]) << 8)) >> 6;
                wanted = (wanted ^ std::min(255u, (sample + 2) >> 2)) * 0x01000193u;
            }
            require((narrow ? frameHash(frames[0]) : narrowedHash(frames[0])) == wanted,
                    "a Profile 2 key frame after a change of profile differs from the reference");
        }
    }
    std::puts("vp9-profile-switch: profiles 0 and 2 alternate at key frames, native and narrowed");
}

/* What a guest can send that no encoder writes, each of which the decoder must
 * refuse before the hardware sees it, with the stream it had left as it was.
 * `real` is a decodable key frame of the profile the stream is primed with, and
 * `shown` what the decoder reports once it has decoded it. */
struct VP9Refusal { const char* what; Bytes packet; };
void vp9Refusals(const std::vector<VP9Refusal>& refused, const Bytes& real, const uml::platform::VideoFormat& shown,
                 const char* stream)
{
    using namespace uml::platform;
    for (const auto& item : refused) {
        // Both a fresh stream and one that has decoded a picture refuse it.
        for (const bool primed : {false, true}) {
            auto decoder = makeVideoDecoder(VideoCodec::vp9);
            if (primed) {
                decoder->submit(real, 1);
                decoder->takeFrames();
            }
            const auto before = decoder->format();
            bool threw = false;
            try { decoder->submit(item.packet, 2); } catch (const std::runtime_error&) { threw = true; }
            if (!threw) std::fprintf(stderr, "accepted: %s (%s, %s)\n", item.what, stream, primed ? "primed" : "fresh");
            require(threw, "a VP9 packet the decoder must refuse was accepted");
            require(decoder->format() == before, "a refused VP9 packet changed the stream size");
            require(decoder->takeFrames().empty(), "a refused VP9 packet produced a picture");
            // The refusal leaves the stream usable: the next key frame decodes.
            decoder->submit(real, 3);
            decoder->drain();
            const auto after = decoder->takeFrames();
            require(after.size() == 1 && after[0].timestamp == 3 && after[0].format == shown,
                    "a refused VP9 packet left the decoder unusable");
        }
    }
    std::printf("vp9-hostile (%s): %zu packets refused, fresh and mid-stream\n", stream, refused.size());
}

void vp9Hostile(const std::vector<uml::platform::VideoDecodeCapability>& capabilities)
{
    using namespace uml::platform;
    namespace frames = uml::testing::vp9;
    const auto has = [&](VideoProfile profile) {
        return std::any_of(capabilities.begin(), capabilities.end(),
                           [profile](const auto& c) { return c.profile == profile; });
    };
    if (!has(VideoProfile::vp9Profile0)) return;
    const auto real = read("vp9-profile0.bin");
    const auto real2 = read("vp9-profile2.bin");
    const auto clip = ivfPackets(read("vp9-superframes.ivf"));
    auto truncated = real;
    truncated.resize(6);
    auto badMarker = real;
    badMarker[0] &= 0x3f;
    auto badSync = real;
    badSync[2] ^= 0x10;
    auto tooLong = frames::superframe({real, real});
    tooLong[tooLong.size() - 5] = 0xff;      // the first frame's size, 65535 bytes
    tooLong[tooLong.size() - 4] = 0xff;
    // Refused whatever the stream is primed with.
    std::vector<VP9Refusal> common = {
        {"a packet of zeros", Bytes(64, 0)},
        {"a truncated key frame header", truncated},
        {"a frame marker that is not 2", badMarker},
        {"a wrong synchronization code", badSync},
        {"a key frame 4097 wide", frames::key(4097, 16)},
        {"a key frame 8192 x 8192", frames::key(8192, 8192)},
        {"a key frame 65536 x 65536", frames::key(65536, 65536)},
        {"a profile 2 key frame 4097 wide", frames::keyFrame(2, 4097, 16)},
        {"a profile 2 key frame 8192 x 8192", frames::keyFrame(2, 8192, 8192)},
        // Below the 64 samples the hardware opens a session for, on either axis and in whichever frame it is stated.
        {"a key frame 63 wide", frames::key(63, 64)},
        {"a key frame 63 high", frames::key(64, 63)},
        {"a key frame 32 x 64", frames::key(32, 64)},
        {"a key frame 1 x 1", frames::key(1, 1)},
        {"a profile 2 key frame 63 wide", frames::keyFrame(2, 63, 64)},
        {"a hidden frame below the minimum in a superframe", frames::superframe({real, frames::inter(false, 64, 32)})},
        {"an intra-only frame below the minimum in a superframe",
            frames::superframe({real, frames::intraOnly(0, 48, 200)})},
        {"a key frame below the minimum after a shown frame", frames::superframe({real, frames::key(63, 63)})},
        // The profiles and depths the hardware path does not carry.
        {"a 12-bit profile 2 key frame", frames::keyFrame(2, 64, 64, {.twelveBit = true})},
        {"a profile 2 key frame of RGB", frames::keyFrame(2, 64, 64, {.colorSpace = 7})},
        {"a profile 1 stream", frames::keyFrame(1, 64, 64, {.subsamplingX = 1, .subsamplingY = 0})},
        {"a profile 3 stream", frames::keyFrame(3, 64, 64, {.subsamplingX = 1, .subsamplingY = 0})},
        {"a reserved profile", Bytes {0xb8, 0, 0, 0, 0, 0, 0, 0}},
        {"a real 12-bit profile 2 stream", read("vp9-profile2-12bit.bin")},
        {"a real 4:4:4 profile 1 stream", read("vp9-profile1-444.bin")},
        {"a real 4:2:2 profile 1 stream", read("vp9-profile1-422.bin")},
        {"a real 10-bit 4:4:4 profile 3 stream", read("vp9-profile3-444-10bit.bin")},
        {"a superframe whose frame is too long", tooLong},
        {"a superframe that names an empty frame", frames::superframe({real, Bytes {}})},
        // The sizes a later frame states count as much as the first one's.
        {"a hidden frame past the range in a superframe", frames::superframe({real, frames::inter(false, 4112, 2304)})},
        {"an intra-only frame past the range in a superframe",
            frames::superframe({real, frames::intraOnly(0, 4097, 64)})},
        {"a key frame past the range after a shown frame", frames::superframe({real, frames::key(8192, 4320)})},
        {"a profile 2 intra-only frame of 12 bits", frames::intraOnly(2, 64, 64, {.twelveBit = true})},
        // One packet is one profile: a stream's frames do not change it between them.
        {"a profile 2 frame after a profile 0 one", frames::superframe({real, frames::keyFrame(2, 64, 64)})},
        {"a profile 0 frame after a profile 2 one", frames::superframe({real2, real})},
        {"a profile 0 frame after a hidden profile 2 one", frames::superframe({frames::keyFrame(2, 64, 64), real})},
        {"a 12-bit profile 2 frame after a profile 0 one",
            frames::superframe({real, frames::keyFrame(2, 64, 64, {.twelveBit = true})})},
        {"a 12-bit profile 2 frame after a profile 2 one",
            frames::superframe({real2, frames::keyFrame(2, 64, 64, {.twelveBit = true})})},
        {"a profile 1 frame after a profile 2 one",
            frames::superframe({real2, frames::keyFrame(1, 64, 64, {.subsamplingX = 1, .subsamplingY = 0})})},
    };
    {
        auto refused = common;
        refused.push_back({"an inter frame of profile 2 on a profile 0 stream", frames::inter(true, 0, 0, 2)});
        vp9Refusals(refused, real, VideoFormat {128, 96, 8}, "profile 0 stream");
    }
    if (has(VideoProfile::vp9Profile2)) {
        auto refused = common;
        refused.push_back({"an inter frame of profile 0 on a profile 2 stream", frames::inter(true, 0, 0, 0)});
        vp9Refusals(refused, real2, VideoFormat {128, 96, 10}, "profile 2 stream");
    }
    /* A stream that does not begin with a key frame is refused whole; one that
     * does and then continues from frames it never decoded is the hardware's to
     * refuse, which it does with an error the decoder reports until a seek. */
    {
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        bool threw = false;
        try { decoder->submit(clip[2], 2); } catch (const std::runtime_error&) { threw = true; }
        require(threw, "an inter frame with no key frame before it was accepted");
    }
    {
        auto decoder = makeVideoDecoder(VideoCodec::vp9);
        decoder->submit(real, 1);
        decoder->takeFrames();
        bool failed = false;
        try {
            decoder->submit(clip[2], 2);
            decoder->drain();
            decoder->takeFrames();
        } catch (const std::runtime_error& error) {
            failed = true;
            std::printf("vp9-hostile: an inter frame from another stream: %s\n", error.what());
        }
        std::printf("vp9-hostile: the hardware %s an inter frame from another stream\n", failed ? "refused" : "took");
        // The stream's error stays until the client seeks, as it does for every codec.
        decoder->discard();
        decoder->submit(real, 3);
        decoder->drain();
        const auto after = decoder->takeFrames();
        require(after.size() == 1 && after[0].timestamp == 3 && after[0].format == VideoFormat {128, 96, 8},
                "a key frame after a hardware refusal did not decode");
    }
}

/* Packets cut and corrupted at random from the clip's real ones: whatever the
 * decoder does with them, it answers with an error of its own or with pictures
 * inside the advertised rectangle, and a seek leaves it able to decode the
 * next key frame. The seed is fixed so a failure repeats. */
void vp9Mutations(const std::vector<uml::platform::VideoDecodeCapability>& capabilities,
                  uml::platform::VideoProfile profile, const char* clipName, bool narrow)
{
    using namespace uml::platform;
    if (std::none_of(capabilities.begin(), capabilities.end(), [profile](const auto& c) { return c.profile == profile; }))
        return;
    const auto clip = ivfPackets(read(clipName));
    std::uint32_t state = 0x9e3779b9u;
    const auto next = [&state](std::uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % bound;
    };
    auto decoder = makeVideoDecoder(VideoCodec::vp9);
    if (narrow) decoder->requestOutputLayout(VideoPixelLayout::planar8Bit);
    unsigned refused = 0, decoded = 0;
    const unsigned rounds = 240;
    for (unsigned round = 0; round < rounds; ++round) {
        // Each round starts at a key frame and then plays one mutated packet.
        decoder->discard();
        decoder->submit(clip[round % 2 ? 0 : 30], 1);
        decoder->drain();
        require(decoder->takeFrames().size() == 1, "the key frame of a mutation round did not decode");
        auto packet = clip[1 + next(29)];
        switch (next(3)) {
        case 0:   // a few bytes of the header changed
            for (unsigned i = 0, count = 1 + next(3); i < count; ++i) packet[next(12)] ^= std::uint8_t(1u << next(8));
            break;
        case 1:   // cut short anywhere
            packet.resize(1 + next(std::uint32_t(packet.size())));
            break;
        default:  // bytes anywhere, the index among them
            for (unsigned i = 0, count = 1 + next(4); i < count; ++i)
                packet[next(std::uint32_t(packet.size()))] = std::uint8_t(next(256));
            break;
        }
        try {
            decoder->submit(packet, 2);
            decoder->drain();
            for (const auto& frame : decoder->takeFrames()) {
                require(frame.format.width >= 1 && frame.format.width <= 4096 && frame.format.height >= 1 &&
                            frame.format.height <= 4096 && !frame.luma.empty() && !frame.chroma.empty(),
                        "a corrupted VP9 packet produced a picture outside the advertised rectangle");
                ++decoded;
            }
        } catch (const std::runtime_error&) {
            ++refused;
        }
    }
    decoder->discard();
    decoder->submit(clip[0], 3);
    decoder->drain();
    require(decoder->takeFrames().size() == 1, "the decoder did not recover from corrupted VP9 packets");
    std::printf("vp9-mutations (%s): %u packets, %u refused, %u pictures\n", clipName, rounds, refused, decoded);
}
}

int main(int argc, char** argv)
{
    using namespace uml::platform;
    /* The registration names the fixture directory, so the binary carries no
     * source path and runs wherever a copy of the fixtures is. */
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_decoder_tests FIXTURE_DIRECTORY\n"
                             "  FIXTURE_DIRECTORY holds a copy of tests/support/video\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        const auto capabilities = videoDecodeCapabilities();
        require(!capabilities.empty(), "no hardware video decoder available");
        VideoColorimetry colorimetry[16] {};
        const char* names[] = {"h264-baseline", "h264-constrained-baseline", "h264-main",
            "h264-high", "h264-constrained-high", "h264-high10", "hevc-main", "hevc-main10", "jpeg",
            "vp9-profile0", "vp9-profile2"};
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            std::printf("Decoding %s\n", name);
            std::fflush(stdout);
            auto decoder = makeVideoDecoder(cap.codec);
            const auto encoded = read(std::string(name) + ".bin");
            decoder->submit(encoded, 123456789);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "decoder did not produce exactly one frame");
            const auto& frame = frames[0];
            require(frame.timestamp == 123456789, "decoder changed the frame timestamp");
            require(frame.format == VideoFormat{128, 96, cap.bitDepth}, "decoder output format differs");
            /* The hardware states which description it applied. These fixtures
             * carry no color description and are below the standard-definition
             * line, so the answer is limited-range BT.601; the point of
             * reading it back is that the conversion below never guesses. */
            require(frame.color.matrix == VideoColorMatrix::bt601 && !frame.color.fullRange,
                    "decoder reported an unexpected color description");
            colorimetry[unsigned(cap.profile)] = frame.color;
            const auto reference = read(std::string(name) + ".raw");
            std::vector<std::uint8_t> actual = frame.luma;
            actual.insert(actual.end(), frame.chroma.begin(), frame.chroma.end());
            require(actual.size() == reference.size(), "decoder output plane sizes differ");
            unsigned maximum = 0;
            std::uint64_t sum = 0;
            const unsigned step = cap.bitDepth == 10 ? 2 : 1;
            for (std::size_t i = 0; i < actual.size(); i += step) {
                const int got = step == 1 ? actual[i] : (actual[i] | (actual[i + 1] << 8)) >> 6;
                const int wanted = step == 1 ? reference[i] : (reference[i] | (reference[i + 1] << 8)) >> 6;
                const unsigned difference = unsigned(std::abs(got - wanted));
                maximum = std::max(maximum, difference);
                sum += difference;
            }
            std::printf("%s: max pixel error %u, total %llu\n", name, maximum,
                        static_cast<unsigned long long>(sum));
            require(maximum <= (cap.codec == VideoCodec::jpeg ? 4u : 2u),
                    "hardware decoded pixels differ from the reference");
            require(decoder->takeFrames().empty(), "decoder returned a frame twice");
        }
        /* The same fixtures decoded straight to packed BGRA. A client that
         * draws through a 32-bit RGB surface asks for this so that no frame is
         * converted by a processor, and the reference is the planar fixture
         * taken through the limited-range BT.601 matrix a frame this small
         * carries when the stream states nothing. */
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            auto decoder = makeVideoDecoder(cap.codec);
            decoder->requestOutputLayout(VideoPixelLayout::packedBgra);
            decoder->submit(read(std::string(name) + ".bin"), 123456789);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "packed decoder did not produce exactly one frame");
            const auto& frame = frames[0];
            require(frame.layout == VideoPixelLayout::packedBgra, "decoder did not produce packed pixels");
            require(frame.format == VideoFormat{128, 96, 8}, "packed output format differs");
            require(frame.chroma.empty() && frame.luma.empty(), "packed frame also carries planes");
            require(frame.pixels.size() == 128u * 96u * 4u, "packed frame size differs");
            const auto reference = read(std::string(name) + ".raw");
            const unsigned step = cap.bitDepth == 10 ? 2 : 1;
            const auto sample = [&](std::size_t index) {
                return int(reference[step * index + step - 1]);
            };
            /* Converted with the description the hardware reported for the
             * planar decode of this same fixture, so the two paths cannot
             * disagree about matrix or range. */
            const auto color = colorimetry[unsigned(cap.profile)];
            double kr = 0.2126, kb = 0.0722;
            if (color.matrix == VideoColorMatrix::bt601) { kr = 0.299; kb = 0.114; }
            else if (color.matrix == VideoColorMatrix::bt2020) { kr = 0.2627; kb = 0.0593; }
            const double kg = 1.0 - kr - kb;
            const double lumaGain = color.fullRange ? 1.0 : 255.0 / 219.0;
            const double lumaFloor = color.fullRange ? 0.0 : 16.0;
            const double chromaGain = color.fullRange ? 1.0 : 255.0 / 224.0;
            const double vToR = chromaGain * 2.0 * (1.0 - kr);
            const double uToB = chromaGain * 2.0 * (1.0 - kb);
            const double uToG = chromaGain * 2.0 * kb * (1.0 - kb) / kg;
            const double vToG = chromaGain * 2.0 * kr * (1.0 - kr) / kg;
            /* Hardware interpolates the subsampled chroma field, so only a
             * pixel whose chroma neighborhood is constant has one defined
             * answer. Everywhere else the two upsamplings legitimately
             * disagree, and the reference cannot say which is right. */
            const auto chroma = [&](std::ptrdiff_t cellRow, std::ptrdiff_t cellColumn) {
                cellRow = std::clamp<std::ptrdiff_t>(cellRow, 0, 47);
                cellColumn = std::clamp<std::ptrdiff_t>(cellColumn, 0, 63);
                const auto pair = std::size_t(128 * 96 + cellRow * 128 + cellColumn * 2);
                return std::pair{sample(pair), sample(pair + 1)};
            };
            unsigned maximum = 0, checked = 0;
            for (std::size_t row = 0; row < 96; ++row) {
                for (std::size_t column = 0; column < 128; ++column) {
                    const auto* got = frame.pixels.data() + (row * 128 + column) * 4;
                    require(got[3] == 0xff, "packed frame is not opaque");
                    const auto cell = chroma(std::ptrdiff_t(row / 2), std::ptrdiff_t(column / 2));
                    bool flat = true;
                    for (std::ptrdiff_t dy = -1; dy <= 1; ++dy)
                        for (std::ptrdiff_t dx = -1; dx <= 1; ++dx)
                            flat = flat && chroma(std::ptrdiff_t(row / 2) + dy,
                                                  std::ptrdiff_t(column / 2) + dx) == cell;
                    if (!flat) continue;
                    ++checked;
                    const double luma = lumaGain * (sample(row * 128 + column) - lumaFloor);
                    const double u = cell.first - 128, v = cell.second - 128;
                    const double wanted[3] = {luma + uToB * u, luma - uToG * u - vToG * v,
                                              luma + vToR * v};
                    for (unsigned channel = 0; channel < 3; ++channel)
                        maximum = std::max(maximum, unsigned(std::abs(
                            int(got[channel]) - int(std::clamp(wanted[channel], 0.0, 255.0) + 0.5))));
                }
            }
            std::printf("%s packed: max channel error %u over %u pixels\n", name, maximum, checked);
            std::fflush(stdout);
            require(checked >= 128 * 96 / 8, "too little of the packed frame could be verified");
            require(maximum <= 8, "packed pixels differ from the converted reference");
        }
        /* A V4L2 client negotiates its capture format only after the decoder
         * has reported the stream's size, which is the middle of a group of
         * pictures. Asking there must not disturb decoding: restarting the
         * session would throw away the reference frames the next picture needs
         * and the hardware would answer kVTVideoDecoderBadDataErr. */
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            const auto encoded = read(std::string(name) + ".bin");
            auto decoder = makeVideoDecoder(cap.codec);
            decoder->submit(encoded, 1);
            require(decoder->format().width == 128, "decoder did not report the stream size");
            decoder->requestOutputLayout(VideoPixelLayout::packedBgra);
            decoder->submit(encoded, 2);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 2, "late format negotiation lost a frame");
            for (const auto& frame : frames)
                require(!frame.pixels.empty() || !frame.luma.empty(), "late negotiation emptied a frame");
            /* Each fixture is a single random access picture, so the request
             * takes effect on the next one. A stream whose next picture needs
             * an earlier one keeps its layout until the next such point, which
             * every frame states and the caller converts. */
            require(frames[1].layout == VideoPixelLayout::packedBgra,
                    "a random access picture did not adopt the requested layout");
            require(frames[0].layout == VideoPixelLayout::planarYuv,
                    "a frame decoded before the request changed layout");
        }
        /* The order the device uses so that nothing has to be converted: read
         * the size out of the packet's headers, report it to the client, and
         * submit the same packet once the client has chosen a layout. Because
         * no picture has been decoded yet, the first one already carries that
         * layout, even though the request would otherwise have to wait for a
         * random access point. */
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            const auto encoded = read(std::string(name) + ".bin");
            auto decoder = makeVideoDecoder(cap.codec);
            decoder->inspect(encoded);
            require(decoder->format().width == 128 && decoder->format().height == 96,
                    "inspecting a packet did not report the stream size");
            require(decoder->takeFrames().empty(), "inspecting a packet decoded it");
            decoder->requestOutputLayout(VideoPixelLayout::packedBgra);
            decoder->submit(encoded, 7);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "an inspected packet did not decode exactly once");
            require(frames[0].timestamp == 7, "an inspected packet changed its timestamp");
            require(frames[0].layout == VideoPixelLayout::packedBgra,
                    "the first picture after an inspected packet was not packed");
        }
        if (std::any_of(capabilities.begin(), capabilities.end(),
                        [](const auto& c) { return c.codec == VideoCodec::hevc; })) {
            hevcDisplayOrder();
            reorderDepths(true, false);
            hevcWindow();
            hevcRawFormats();
            hevcLimits(capabilities);
        }
        const auto has = [&](VideoProfile profile) {
            return std::any_of(capabilities.begin(), capabilities.end(),
                               [profile](const auto& c) { return c.profile == profile; });
        };
        if (has(VideoProfile::h264Main) && has(VideoProfile::h264High)) {
            h264DisplayOrder();
            reorderDepths(false, true);
            h264Window();
        }
        h264Limits(capabilities);
        h264Shapes(capabilities);
        vp9Limits(capabilities);
        vp9Clip(capabilities);
        vp9ResizeInSuperframe(capabilities, VideoProfile::vp9Profile0, "vp9-superframes.ivf", vp9ClipHashes);
        vp9FinalFrame(capabilities, VideoProfile::vp9Profile0, "vp9-superframes.ivf", vp9ClipHashes);
        vp9Hostile(capabilities);
        vp9Mutations(capabilities, VideoProfile::vp9Profile0, "vp9-superframes.ivf", false);
        vp9Profile2Limits(capabilities);
        vp9Profile2Clip(capabilities);
        vp9ResizeInSuperframe(capabilities, VideoProfile::vp9Profile2, "vp9-profile2-superframes.ivf",
                              vp9Profile2ClipHashes);
        vp9FinalFrame(capabilities, VideoProfile::vp9Profile2, "vp9-profile2-superframes.ivf", vp9Profile2ClipHashes);
        vp9ProfileSwitch(capabilities);
        vp9Mutations(capabilities, VideoProfile::vp9Profile2, "vp9-profile2-superframes.ivf", true);
        auto decoder = makeVideoDecoder(capabilities.front().codec);
        bool rejected = false;
        try { decoder->submit({}, 0); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "empty encoded packet was accepted");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
