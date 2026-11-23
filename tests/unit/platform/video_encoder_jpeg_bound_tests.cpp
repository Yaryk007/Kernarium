// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder_test_support.hpp"
#include "video_encoder_testing.hpp"

#include <cstdio>
#include <string>
#include <vector>

/* The size a caller bounds a JPEG to (`maximumJpegBytes`) is the size of the file it gets, with the JFIF segment the
 * hardware encoder leaves out. An image over it is refused before the host copies it, also when the encoder hands it
 * over in two Core Media blocks, which cannot be read in place: only the image's first bytes are read to decide. The
 * encoder of this test variant rebuilds each sample as two blocks on demand and counts what it copies to read one. */

namespace {
using namespace uml::testing::video_encoder;

constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 480;

Output encode(std::uint64_t bound, bool split)
{
    auto settings = settingsFor(VideoEncodeProfile::jpegBaseline, RawVideoLayout::nv12, kWidth, kHeight);
    settings.jpegQuality = 90;
    settings.maximumJpegBytes = bound;
    auto encoder = makeVideoEncoder();
    require(encoder->configure(settings) == VideoEncodeStatus::ok, "configure");
    const auto base = busyPicture(kWidth, kHeight, 3);
    Feed feed;
    feed.count = 1;
    feed.fill = [&](WritableVideoFrame& frame, unsigned) { writePicture(frame, base, RawVideoLayout::nv12); };
    feed.timeoutMilliseconds = 30'000;
    uml::platform::testing::splitSamples(split);
    uml::platform::testing::resetSampleBytesCopied();
    auto output = run(*encoder, feed);
    uml::platform::testing::splitSamples(false);
    return output;
}

bool hasHardwareJpeg()
{
    for (const auto& capability : videoEncodeCapabilities())
        if (capability.codec == VideoCodec::jpeg) return true;
    return false;
}

void aFragmentedImageIsBoundedBeforeAnyCopy()
{
    std::puts("== a JPEG in two blocks");
    const auto whole = encode(0, false);
    require(!whole.timedOut && whole.errors == 0 && whole.pictures().size() == 1, "an unbounded image comes out");
    const auto image = whole.pictures()[0]->bytes;
    require(image.size() > 1000 && image[0] == 0xff && image[1] == 0xd8 && image[2] == 0xff && image[3] == 0xe0, "a JFIF image");
    require(uml::platform::testing::sampleBytesCopied() == 0, "a sample in one block is read in place");

    // The file the caller gets is exactly `image.size()` bytes, so that is the largest bound that delivers it.
    const auto exact = encode(image.size(), true);
    require(!exact.timedOut && exact.errors == 0 && exact.pictures().size() == 1, "the image fits a bound of its own size");
    require(exact.pictures()[0]->bytes == image, "and it is the same file when the sample came in two blocks");
    require(uml::platform::testing::sampleBytesCopied() >= image.size() - 18, "which is copied to be read");

    // One byte under the bound it is refused, and nothing of it was copied: the length with the JFIF segment is known
    // from the image's first bytes. The sample itself is within the bound (the segment is what overflows it).
    const auto over = encode(image.size() - 1, true);
    require(!over.timedOut && over.pictures().empty() && over.errors == 1, "an image one byte over the bound is refused");
    require(uml::platform::testing::sampleBytesCopied() == 0, "without a copy of the sample, however it is split");

    const auto sample = encode(image.size() - 18 - 1, true);
    require(!sample.timedOut && sample.pictures().empty() && sample.errors == 1, "and so is an image whose sample is over");
    require(uml::platform::testing::sampleBytesCopied() == 0, "also without a copy");
    std::printf("  %zu-byte image: bounded exactly, no scratch copy of a refused one\n", image.size());
}
} // namespace

int main()
{
    if (!hasHardwareJpeg()) {
        std::puts("no hardware JPEG encoder on this host; the encodes are skipped");
        return 0;
    }
    try {
        aFragmentedImageIsBoundedBeforeAnyCopy();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video encoder jpeg bound ok");
    return 0;
}
