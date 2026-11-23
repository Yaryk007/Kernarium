// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_decoder.hpp"
#include "video_decoder_test_support.hpp"
#include "video_decoder_testing.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

/* The decoder's answer to losing its VideoToolbox session. iOS invalidates the
 * session of an app that was in the background, and the platform decoder is
 * built with a hook that does the same to the session its next picture uses.
 * The pictures that need the lost session are dropped without an error, the
 * session is made anew from the parameter sets the stream stated, and decoding
 * goes on at the next random access point. A second loss with no picture
 * decoded between is an error, and a seek starts over. An error of the stream
 * is not a loss and stays until the seek. */

namespace {
using uml::testing::require;
using namespace uml::testing::video_decoder;
using namespace uml::platform;
namespace hook = uml::platform::testing;

// A picture index is the number of its access unit; the fixtures begin a new
// group of pictures at 12 (the seek tests rely on it too).
constexpr std::size_t lossAt = 7;
constexpr std::size_t syncAt = 12;

struct Stream {
    const char* name;
    VideoCodec codec;
    std::vector<Bytes> units;
};

Stream streamOf(const char* name, VideoCodec codec)
{
    const auto stream = read(name);
    auto units = codec == VideoCodec::hevc ? accessUnits(stream) : h264AccessUnits(stream);
    require(units.size() == 20, "the order fixtures have twenty pictures");
    return {name, codec, std::move(units)};
}

std::vector<DecodedVideoFrame> submit(VideoDecoder& decoder, const Stream& stream, std::size_t first, std::size_t last)
{
    std::vector<DecodedVideoFrame> frames;
    for (std::size_t i = first; i < last; ++i) {
        decoder.submit(stream.units[i], (i + 1) * 1000ull);
        append(frames, decoder.takeFrames());
    }
    return frames;
}

// Frames in the order they are shown are brighter one after the other.
void requireAscending(const std::vector<DecodedVideoFrame>& frames, const char* what)
{
    for (std::size_t i = 1; i < frames.size(); ++i)
        require(meanLuma(frames[i]) > meanLuma(frames[i - 1]), what);
}

/* The pictures sent before the loss that the decoder had decoded all come
 * out, the one that found the session lost and the ones that followed it
 * until the next random access point do not, and the stream goes on from that
 * point with its own timestamps in display order. */
void checkResumed(const std::vector<DecodedVideoFrame>& frames, std::size_t before, const char* what)
{
    std::vector<DecodedVideoFrame> early, late;
    for (const auto& frame : frames) {
        require(frame.timestamp <= before * 1000ull || frame.timestamp > syncAt * 1000ull,
                "a picture the lost session was to decode was shown");
        (frame.timestamp <= before * 1000ull ? early : late).push_back(frame);
    }
    std::printf("%s: %zu pictures before the loss, %zu after the sync picture\n", what, early.size(), late.size());
    require(early.size() == before, "a picture decoded before the loss was dropped with it");
    require(late.size() == 20 - syncAt, "the stream did not go on at its next sync picture");
    requireAscending(early, "pictures decoded before the loss leave in order");
    requireAscending(late, "pictures after the loss leave in display order");
}

/* One loss in the middle of a group of pictures: no error, the format kept. */
void lossMidStream(const Stream& stream)
{
    std::printf("== %s: loss mid-stream\n", stream.name);
    auto decoder = makeVideoDecoder(stream.codec);
    auto frames = submit(*decoder, stream, 0, lossAt);
    const auto format = decoder->format();
    const auto depth = decoder->reorderDepth();
    hook::loseSession(*decoder, 1);
    // The picture that finds the session lost, and those that depend on it.
    append(frames, submit(*decoder, stream, lossAt, syncAt));
    require(decoder->format() == format && decoder->reorderDepth() == depth, "a lost session changed what the stream states");
    append(frames, submit(*decoder, stream, syncAt, stream.units.size()));
    decoder->drain();
    append(frames, decoder->takeFrames());
    checkResumed(frames, lossAt, stream.name);
}

/* A loss the next sync picture meets again is final: the client is told, and
 * a seek, which is what the guest's queue clear is, starts the stream over. */
void secondLossIsFinal(const Stream& stream)
{
    std::printf("== %s: two losses in a row\n", stream.name);
    auto decoder = makeVideoDecoder(stream.codec);
    auto frames = submit(*decoder, stream, 0, lossAt);
    hook::loseSession(*decoder, 2);
    append(frames, submit(*decoder, stream, lossAt, syncAt));
    bool reported = false;
    try {
        decoder->submit(stream.units[syncAt], (syncAt + 1) * 1000ull);
    } catch (const std::runtime_error&) {
        reported = true;
    }
    require(reported, "a loss with no picture since the last one was not reported");

    decoder->discard();
    require(decoder->takeFrames().empty(), "a seek shows nothing decoded before it");
    frames = submit(*decoder, stream, syncAt, stream.units.size());
    decoder->drain();
    append(frames, decoder->takeFrames());
    require(frames.size() == 20 - syncAt, "the stream did not start over after a seek");
    requireAscending(frames, "pictures after a seek leave in display order");
}

/* An error of the stream, which the hardware reports for data it cannot decode,
 * is not a lost session: it is reported for the picture and for every picture
 * after it until the client's seek, as it always was. */
void streamErrorStays(const Stream& stream)
{
    std::printf("== %s: error of the stream\n", stream.name);
    constexpr std::int32_t badData = -12909;   // kVTVideoDecoderBadDataErr
    const auto fails = [&](VideoDecoder& decoder, std::size_t picture) {
        try {
            decoder.submit(stream.units[picture], (picture + 1) * 1000ull);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    auto decoder = makeVideoDecoder(stream.codec);
    auto frames = submit(*decoder, stream, 0, lossAt);
    hook::failOutput(*decoder, badData);
    require(fails(*decoder, lossAt), "an error of the stream was not reported");
    require(fails(*decoder, lossAt + 1), "an error of the stream was reported once and the stream went on");
    require(fails(*decoder, syncAt), "an error of the stream ended at a sync picture without a seek");

    decoder->discard();
    require(decoder->takeFrames().empty(), "a seek shows nothing decoded before it");
    frames = submit(*decoder, stream, syncAt, stream.units.size());
    decoder->drain();
    append(frames, decoder->takeFrames());
    require(frames.size() == 20 - syncAt, "the stream did not start over after a seek");
}

/* A loss followed by a drain, as a client that ends its stream asks for. */
void lossThenDrain(const Stream& stream)
{
    std::printf("== %s: loss then drain\n", stream.name);
    auto decoder = makeVideoDecoder(stream.codec);
    auto frames = submit(*decoder, stream, 0, lossAt);
    hook::loseSession(*decoder, 1);
    append(frames, submit(*decoder, stream, lossAt, lossAt + 1));
    decoder->drain();
    append(frames, decoder->takeFrames());
    require(frames.size() == lossAt, "the pictures decoded before the loss are shown by the drain");
    // The stream is still there for the sync picture that follows.
    append(frames, submit(*decoder, stream, syncAt, stream.units.size()));
    decoder->drain();
    append(frames, decoder->takeFrames());
    require(frames.size() == lossAt + 20 - syncAt, "the drain ended the stream");
}

/* Pictures that stand alone: the next one decodes. */
void standAlone(const char* name, VideoCodec codec)
{
    std::printf("== %s: loss between independent pictures\n", name);
    const auto picture = read(name);
    auto decoder = makeVideoDecoder(codec);
    decoder->submit(picture, 1);
    require(decoder->takeFrames().size() == 1, "the first picture");
    hook::loseSession(*decoder, 1);
    decoder->submit(picture, 2);
    require(decoder->takeFrames().empty(), "the picture that found the session lost was shown");
    decoder->submit(picture, 3);
    const auto frames = decoder->takeFrames();
    require(frames.size() == 1 && frames[0].timestamp == 3, "the next picture did not decode");
    // And again, a loss now and then being an ordinary event.
    for (unsigned round = 0; round < 3; ++round) {
        hook::loseSession(*decoder, 1);
        decoder->submit(picture, 10 + 2 * round);
        require(decoder->takeFrames().empty(), "a lost picture was shown");
        decoder->submit(picture, 11 + 2 * round);
        require(decoder->takeFrames().size() == 1, "a picture after a loss did not decode");
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_decoder_recovery_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        const auto capabilities = videoDecodeCapabilities();
        const auto has = [&](VideoCodec codec) {
            return std::any_of(capabilities.begin(), capabilities.end(),
                               [codec](const auto& c) { return c.codec == codec; });
        };
        std::vector<Stream> streams;
        if (has(VideoCodec::h264)) streams.push_back(streamOf("h264-bframes.bin", VideoCodec::h264));
        if (has(VideoCodec::hevc)) streams.push_back(streamOf("hevc-bframes.bin", VideoCodec::hevc));
        require(!streams.empty(), "this host has no hardware decoder to lose a session of");
        for (const auto& stream : streams) {
            lossMidStream(stream);
            secondLossIsFinal(stream);
            lossThenDrain(stream);
            streamErrorStays(stream);
        }
        if (has(VideoCodec::jpeg)) standAlone("jpeg.bin", VideoCodec::jpeg);
        if (has(VideoCodec::vp9)) standAlone("vp9-profile0.bin", VideoCodec::vp9);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video decoder recovery ok");
    return 0;
}
