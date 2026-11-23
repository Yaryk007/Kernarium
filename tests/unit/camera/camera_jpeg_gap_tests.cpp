// SPDX-License-Identifier: GPL-2.0-or-later
// The Exif gap the camera device reserves in a JPEG still: its place, its
// length field, what happens to the segments around it, and that the scan is
// left exactly as the platform made it.

#include "camera_tiers.hpp"
#include "jpeg_gap.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace uml::camera;
using uml::testing::require;

namespace {

void marker(std::vector<std::uint8_t>& out, std::uint8_t code, const std::vector<std::uint8_t>& payload)
{
    out.push_back(0xff);
    out.push_back(code);
    const std::size_t length = payload.size() + 2;
    out.push_back(std::uint8_t(length >> 8));
    out.push_back(std::uint8_t(length));
    out.insert(out.end(), payload.begin(), payload.end());
}

/* A JPEG's segments, in an order with an Exif segment, an XMP segment and a
 * JFIF segment before the tables; the scan carries a byte-stuffed 0xff. */
std::vector<std::uint8_t> sample(bool withScan = true)
{
    std::vector<std::uint8_t> jpeg = {0xff, 0xd8};
    marker(jpeg, 0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    marker(jpeg, 0xe1, {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0});
    marker(jpeg, 0xe1, {'h', 't', 't', 'p', ':', '/', '/', 'n', 's', '.', 'a', 'd', 'o', 'b', 'e'});
    marker(jpeg, 0xdb, std::vector<std::uint8_t>(65, 3));
    marker(jpeg, 0xc0, {8, 0, 16, 0, 16, 1, 1, 0x11, 0});
    marker(jpeg, 0xc4, std::vector<std::uint8_t>(20, 1));
    if (withScan) {
        marker(jpeg, 0xda, {1, 1, 0, 0, 63, 0});
        for (const std::uint8_t byte : {0x12, 0xff, 0x00, 0x34, 0xff, 0xd0, 0x56}) jpeg.push_back(byte);
        jpeg.push_back(0xff);
        jpeg.push_back(0xd9);
    }
    return jpeg;
}

std::size_t find(const std::vector<std::uint8_t>& bytes, std::uint8_t code, std::size_t from = 2)
{
    for (std::size_t at = from; at + 1 < bytes.size(); ++at)
        if (bytes[at] == 0xff && bytes[at + 1] == code) return at;
    return std::size_t(-1);
}

void testGap()
{
    const auto jpeg = sample();
    const auto plan = reserveExifGap(jpeg);
    require(plan.has_value(), "a JPEG with a scan gets a gap");
    require(plan->gapLength == exifGapLength && plan->gapOffset == 6, "the gap starts after the segment header");
    require(plan->head[0] == 0xff && plan->head[1] == 0xd8 && plan->head[2] == 0xff && plan->head[3] == 0xe1,
            "the Exif segment follows the start of image");
    require(((plan->head[4] << 8) | plan->head[5]) == exifGapLength + 2, "the length field covers the payload and itself");
    require(std::memcmp(plan->head.data() + plan->gapOffset, "Exif\0\0", 6) == 0, "the payload starts as an Exif header");
    for (std::size_t at = plan->gapOffset + 6; at < plan->gapOffset + plan->gapLength; ++at)
        require(plan->head[at] == 0, "the rest of the gap is zero");
}

void testSegmentsAroundTheGap()
{
    const auto jpeg = sample();
    const auto plan = reserveExifGap(jpeg);
    require(plan.has_value(), "plan");
    // JFIF, DQT, SOF0 and DHT keep their order; both APP1 segments of the platform go.
    std::vector<std::uint8_t> rest(plan->head.begin() + plan->gapOffset + plan->gapLength, plan->head.end());
    const auto jfif = find(rest, 0xe0, 0), dqt = find(rest, 0xdb, 0), sof = find(rest, 0xc0, 0), dht = find(rest, 0xc4, 0);
    require(jfif == 0 && dqt != size_t(-1) && sof != size_t(-1) && dht != size_t(-1), "the other segments are kept");
    require(jfif < dqt && dqt < sof && sof < dht, "their order is kept");
    require(find(rest, 0xe1, 0) == size_t(-1), "the platform's own APP1 segments are dropped");
    // The scan and the end of image are the platform's bytes.
    require(plan->tail.size() > 0 && plan->tail[0] == 0xff && plan->tail[1] == 0xda, "the tail starts at the scan header");
    require(plan->tail.data() >= jpeg.data() && plan->tail.data() + plan->tail.size() == jpeg.data() + jpeg.size(),
            "the tail is the platform's bytes to its end, not a copy");
    require(plan->size() == plan->head.size() + plan->tail.size(), "size is the whole file");
    require(plan->tail[plan->tail.size() - 2] == 0xff && plan->tail.back() == 0xd9, "the end of image is last");
}

/* A phone's JPEG can carry a multi-picture index and a second picture (a gain
 * map or a depth image) after the first one's end of image: the file the guest
 * gets is the first picture. */
void testOtherPicturesAreNotPassedOn()
{
    std::vector<std::uint8_t> jpeg = {0xff, 0xd8};
    marker(jpeg, 0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    marker(jpeg, 0xe2, {'M', 'P', 'F', 0, 'I', 'I', 42, 0, 8, 0, 0, 0});
    marker(jpeg, 0xe2, {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0, 1, 1});
    marker(jpeg, 0xdb, std::vector<std::uint8_t>(65, 3));
    marker(jpeg, 0xc0, {8, 0, 16, 0, 16, 1, 1, 0x11, 0});
    marker(jpeg, 0xc4, std::vector<std::uint8_t>(20, 1));
    marker(jpeg, 0xda, {1, 1, 0, 0, 63, 0});
    // Stuffing and a restart marker in the data, and a fill byte before the end of image.
    for (const std::uint8_t byte : {0x12, 0xff, 0x00, 0x34, 0xff, 0xd0, 0x56, 0x78, 0xff, 0xff, 0xd9}) jpeg.push_back(byte);
    const std::size_t firstEnd = jpeg.size();
    // The second picture: a whole JPEG of its own.
    for (const std::uint8_t byte : {0xff, 0xd8}) jpeg.push_back(byte);
    marker(jpeg, 0xdb, std::vector<std::uint8_t>(65, 5));
    marker(jpeg, 0xda, {1, 1, 0, 0, 63, 0});
    for (const std::uint8_t byte : {0x99, 0xff, 0xd9}) jpeg.push_back(byte);

    const auto plan = reserveExifGap(jpeg);
    require(plan.has_value(), "a file with a second picture is accepted");
    require(plan->tail.data() + plan->tail.size() == jpeg.data() + firstEnd, "and ends at the first picture's end of image");
    std::vector<std::uint8_t> rest(plan->head.begin() + plan->gapOffset + plan->gapLength, plan->head.end());
    require(find(rest, 0xe2, 0) != size_t(-1), "the profile (APP2 ICC) is kept");
    std::size_t index = 0;
    for (; index + 8 < rest.size(); ++index)
        if (rest[index] == 0xff && rest[index + 1] == 0xe2 && rest[index + 4] == 'M' && rest[index + 5] == 'P' && rest[index + 6] == 'F') break;
    require(index + 8 >= rest.size(), "the multi-picture index is dropped");

    // A progressive image: more scans and a table between the first scan and the end of image.
    std::vector<std::uint8_t> progressive = {0xff, 0xd8};
    marker(progressive, 0xdb, std::vector<std::uint8_t>(65, 3));
    marker(progressive, 0xc2, {8, 0, 16, 0, 16, 1, 1, 0x11, 0});
    marker(progressive, 0xc4, std::vector<std::uint8_t>(20, 1));
    marker(progressive, 0xda, {1, 1, 0, 0, 0, 0});
    for (const std::uint8_t byte : {0x11, 0xff, 0x00, 0x22}) progressive.push_back(byte);
    marker(progressive, 0xc4, std::vector<std::uint8_t>(20, 2));
    marker(progressive, 0xda, {1, 1, 0, 1, 63, 0});
    for (const std::uint8_t byte : {0x33, 0x44, 0xff, 0xd9}) progressive.push_back(byte);
    const std::size_t progressiveEnd = progressive.size();
    for (const std::uint8_t byte : {0xff, 0xd8, 0xff, 0xd9}) progressive.push_back(byte);
    const auto many = reserveExifGap(progressive);
    require(many && many->tail.data() + many->tail.size() == progressive.data() + progressiveEnd,
            "every scan of a progressive picture is kept, and nothing after its end of image");
}

void testFillBytesAndOtherLengths()
{
    auto jpeg = sample();
    // Fill bytes before a marker are legal.
    jpeg.insert(jpeg.begin() + 2, {0xff, 0xff});
    require(reserveExifGap(jpeg).has_value(), "fill bytes before a marker are accepted");
    const auto small = reserveExifGap(sample(), 1024);
    require(small && small->gapLength == 1024 && small->head[4] == 0x04 && small->head[5] == 0x02, "another gap length");
}

/* The picture's size is the first frame header's, big-endian, the height first;
 * a header that is missing or cut short states none. */
void testFrameSize()
{
    const auto plain = reserveExifGap(sample());
    require(plain && plain->frameWidth == 16 && plain->frameHeight == 16, "the sample's SOF0 states 16 by 16");

    // A rectangular picture of the size of a 12 MP still, in each frame type.
    for (const std::uint8_t code : {0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc9, 0xcf}) {
        std::vector<std::uint8_t> jpeg = {0xff, 0xd8};
        marker(jpeg, 0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
        marker(jpeg, code, {8, 0x0b, 0xd8, 0x0f, 0xc0, 1, 1, 0x11, 0});  // 3032 high, 4032 wide
        marker(jpeg, 0xda, {1, 1, 0, 0, 63, 0});
        for (const std::uint8_t byte : {0x12, 0x34, 0xff, 0xd9}) jpeg.push_back(byte);
        const auto plan = reserveExifGap(jpeg);
        require(plan && plan->frameWidth == 4032 && plan->frameHeight == 3032, "a frame header of any type states the size");
    }

    // The tables and the arithmetic conditioning are not frame headers.
    std::vector<std::uint8_t> tables = {0xff, 0xd8};
    marker(tables, 0xc4, {0, 0, 0x0b, 0xd8, 0x0f, 0xc0, 1, 1, 0x11, 0});
    marker(tables, 0xcc, {0, 0, 0x0b, 0xd8, 0x0f, 0xc0, 1, 1, 0x11, 0});
    marker(tables, 0xda, {1, 1, 0, 0, 63, 0});
    for (const std::uint8_t byte : {0x12, 0xff, 0xd9}) tables.push_back(byte);
    const auto none = reserveExifGap(tables);
    require(none && none->frameWidth == 0 && none->frameHeight == 0, "no frame header, no size");

    // The first frame header counts; a short one states nothing.
    std::vector<std::uint8_t> twice = {0xff, 0xd8};
    marker(twice, 0xc0, {8, 0, 4, 0, 6, 1, 1, 0x11, 0});
    marker(twice, 0xc2, {8, 0, 9, 0, 9, 1, 1, 0x11, 0});
    marker(twice, 0xda, {1, 1, 0, 0, 63, 0});
    for (const std::uint8_t byte : {0x12, 0xff, 0xd9}) twice.push_back(byte);
    const auto first = reserveExifGap(twice);
    require(first && first->frameWidth == 6 && first->frameHeight == 4, "the first frame header");
    std::vector<std::uint8_t> shortHeader = {0xff, 0xd8};
    marker(shortHeader, 0xc0, {8, 0, 4});
    marker(shortHeader, 0xda, {1, 1, 0, 0, 63, 0});
    for (const std::uint8_t byte : {0x12, 0xff, 0xd9}) shortHeader.push_back(byte);
    const auto cut = reserveExifGap(shortHeader);
    require(cut && cut->frameWidth == 0 && cut->frameHeight == 0, "a frame header too short to hold a size");
}

// The largest file the device takes (`maximumJpegBytes`) is the largest that, with the reserved gap in and a segment of the
// platform's own gone, still fits the buffer the guest is told (`jpegBufferBytes`): a file with no Exif of its own grows by
// the new segment's marker, length field and payload, and no more.
void testTheLargestFileFillsTheGuestsBuffer()
{
    for (const std::uint64_t pixels : {std::uint64_t(256), std::uint64_t(1920) * 1080, std::uint64_t(4032) * 3024}) {
        const auto bound = maximumJpegBytes(pixels);
        std::vector<std::uint8_t> jpeg = {0xff, 0xd8};
        marker(jpeg, 0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
        marker(jpeg, 0xdb, std::vector<std::uint8_t>(65, 3));
        marker(jpeg, 0xc0, {8, 0, 16, 0, 16, 1, 1, 0x11, 0});
        marker(jpeg, 0xda, {1, 1, 0, 0, 63, 0});
        while (jpeg.size() + 2 < bound) jpeg.push_back(0x12);
        jpeg.push_back(0xff);
        jpeg.push_back(0xd9);
        require(jpeg.size() == bound, "the file is of the bound's size");
        const auto plan = reserveExifGap(jpeg);
        require(plan.has_value() && plan->size() == jpegBufferBytes(pixels), "the largest file fills the buffer exactly");
        jpeg.insert(jpeg.end() - 2, std::uint8_t(0x12));
        const auto over = reserveExifGap(jpeg);
        require(over.has_value() && over->size() == jpegBufferBytes(pixels) + 1, "and a byte more does not fit it");
    }
}

void testRefusals()
{
    require(!reserveExifGap({}).has_value(), "empty data");
    require(!reserveExifGap(std::vector<std::uint8_t> {0x89, 'P', 'N', 'G', 0, 0}).has_value(), "not a JPEG");
    require(!reserveExifGap(sample(false)).has_value(), "no scan");
    auto unfinished = sample();
    unfinished.resize(unfinished.size() - 2);
    require(!reserveExifGap(unfinished).has_value(), "a scan with no end of image");
    auto truncated = sample();
    truncated.resize(30);
    require(!reserveExifGap(truncated).has_value(), "a segment cut short");
    auto lone = std::vector<std::uint8_t> {0xff, 0xd8, 0xff, 0xd8, 0xff, 0xda, 0, 2};
    require(!reserveExifGap(lone).has_value(), "a stand-alone marker before the scan");
    require(!reserveExifGap(sample(), 4).has_value(), "a gap too short to hold an Exif header");
    require(!reserveExifGap(sample(), 70000).has_value(), "a gap longer than a segment");
}

} // namespace

int main()
{
    testGap();
    testSegmentsAroundTheGap();
    testOtherPicturesAreNotPassedOn();
    testFillBytesAndOtherLengths();
    testFrameSize();
    testTheLargestFileFillsTheGuestsBuffer();
    testRefusals();
    return 0;
}
