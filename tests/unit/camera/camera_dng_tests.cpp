// SPDX-License-Identifier: GPL-2.0-or-later
#include "dng_color.hpp"
#include "require.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

// Builds a TIFF in either byte order: IFD0 with the colour tags and a SubIFD
// that holds the raw image's size and CFA pattern, and an Exif directory.
class Builder {
public:
    explicit Builder(bool little) : little_(little) {}

    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u16(std::uint16_t v) { little_ ? (u8(v & 0xff), u8(v >> 8)) : (u8(v >> 8), u8(v & 0xff)); }
    void u32(std::uint32_t v)
    {
        if (little_) { u16(v & 0xffff); u16(v >> 16); } else { u16(v >> 16); u16(v & 0xffff); }
    }
    void patch32(std::size_t at, std::uint32_t v)
    {
        const auto saved = bytes_.size();
        bytes_.resize(at);
        u32(v);
        bytes_.resize(saved);
    }
    std::size_t size() const { return bytes_.size(); }
    std::vector<std::uint8_t> bytes_;

    struct Tag {
        std::uint16_t tag, type;
        std::uint32_t count;
        std::vector<std::uint8_t> payload;  // already in file byte order
    };
    std::vector<std::uint8_t> encode(std::uint32_t v)
    {
        Builder scratch(little_);
        scratch.u32(v);
        return scratch.bytes_;
    }
    std::vector<std::uint8_t> encode16(std::uint16_t v)
    {
        Builder scratch(little_);
        scratch.u16(v);
        return scratch.bytes_;
    }
    std::vector<std::uint8_t> doubles(const std::vector<double>& values)
    {
        Builder scratch(little_);
        for (const double value : values) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            for (std::size_t byte = 0; byte < 8; ++byte) scratch.u8(std::uint8_t(bits >> (little_ ? byte * 8 : (7 - byte) * 8)));
        }
        return scratch.bytes_;
    }
    std::vector<std::uint8_t> srationals(const std::vector<std::pair<int, int>>& values)
    {
        Builder scratch(little_);
        for (const auto& [n, d] : values) { scratch.u32(std::uint32_t(n)); scratch.u32(std::uint32_t(d)); }
        return scratch.bytes_;
    }

    // Writes a directory at the current position; values over four bytes go after the entries.
    std::size_t directory(const std::vector<Tag>& tags, std::uint32_t next = 0)
    {
        const auto start = bytes_.size();
        u16(std::uint16_t(tags.size()));
        const auto tableEnd = start + 2 + tags.size() * 12 + 4;
        std::size_t spill = tableEnd;
        std::vector<std::uint8_t> spilled;
        for (const auto& t : tags) {
            u16(t.tag);
            u16(t.type);
            u32(t.count);
            if (t.payload.size() <= 4) {
                for (std::size_t i = 0; i < 4; ++i) u8(i < t.payload.size() ? t.payload[i] : 0);
            } else {
                u32(std::uint32_t(spill + spilled.size()));
                spilled.insert(spilled.end(), t.payload.begin(), t.payload.end());
            }
        }
        u32(next);
        bytes_.insert(bytes_.end(), spilled.begin(), spilled.end());
        return start;
    }

private:
    bool little_;
};

// The exposure tags a platform DNG carries besides the colour data.
struct ExposureTags {
    bool neutral {};
    std::vector<double> neutralValues {0.5, 1.0, 0.75};
    // Store the neutral values as DOUBLEs, exactly as given (a non-finite one
    // survives); otherwise as the rationals a platform writes.
    bool neutralAsDoubles {};
    bool noise {};
    std::vector<double> noiseValues;
};

std::vector<std::uint8_t> makeDng(bool little, bool withColor = true, const ExposureTags& exposure = {})
{
    Builder b(little);
    // Header.
    if (little) { b.u8('I'); b.u8('I'); } else { b.u8('M'); b.u8('M'); }
    b.u16(42);
    b.u32(8);

    // Lay out the Exif and SubIFD directories after IFD0 by writing them first
    // into a scratch builder at known offsets: simplest is to write IFD0 last.
    // IFD0 lives at 8; it is built after the others, whose offsets follow it.
    // Build the child directories into a second builder placed after a
    // reserved IFD0 region of fixed size.
    const std::size_t reserved = 600;
    Builder children(little);
    children.bytes_.assign(8 + reserved, 0);

    const auto exifOffset = children.directory({{37386, 5, 1, children.srationals({{570, 100}})}});
    // 570/100 above is a rational with the unsigned layout: same bytes.
    const auto subOffset = children.directory({
        {256, 4, 1, children.encode(4032)},
        {257, 4, 1, children.encode(3024)},
        {33422, 1, 4, {2, 1, 1, 0}},  // BGGR
    });

    std::vector<Builder::Tag> tags;
    tags.push_back({330, 4, 1, children.encode(std::uint32_t(subOffset))});
    tags.push_back({34665, 4, 1, children.encode(std::uint32_t(exifOffset))});
    if (withColor) {
        std::vector<std::pair<int, int>> m1, m2, f1, f2;
        for (int i = 0; i < 9; ++i) {
            m1.push_back({i + 1, 10});
            m2.push_back({-(i + 1), 10});
            f1.push_back({i, 100});
            f2.push_back({100 - i, 100});
        }
        tags.push_back({50714, 3, 4, {}});
        {
            Builder v(little);
            for (int i = 0; i < 4; ++i) v.u16(std::uint16_t(1020 + i * 2));
            tags.back().payload = v.bytes_;
        }
        tags.push_back({50717, 3, 1, children.encode16(16383)});
        tags.push_back({50721, 10, 9, children.srationals(m1)});
        tags.push_back({50722, 10, 9, children.srationals(m2)});
        tags.push_back({50778, 3, 1, children.encode16(17)});
        tags.push_back({50779, 3, 1, children.encode16(21)});
        tags.push_back({50964, 10, 9, children.srationals(f1)});
        tags.push_back({50965, 10, 9, children.srationals(f2)});
    }
    if (exposure.neutral && exposure.neutralAsDoubles) {
        tags.push_back({50728, 12, std::uint32_t(exposure.neutralValues.size()), children.doubles(exposure.neutralValues)});
    } else if (exposure.neutral) {
        // Rationals with a denominator of 1000, as a platform writes them.
        std::vector<std::pair<int, int>> values;
        for (const double value : exposure.neutralValues) values.push_back({int(std::lround(value * 1000.0)), 1000});
        tags.push_back({50728, 5, std::uint32_t(values.size()), children.srationals(values)});
    }
    if (exposure.noise) tags.push_back({51041, 12, std::uint32_t(exposure.noiseValues.size()), children.doubles(exposure.noiseValues)});
    // IFD0 must list its tags in ascending order.
    std::sort(tags.begin(), tags.end(), [](const auto& a, const auto& c) { return a.tag < c.tag; });
    Builder top(little);
    top.bytes_.assign(8, 0);
    top.directory(tags);
    require(top.size() <= 8 + reserved);
    std::vector<std::uint8_t> file = children.bytes_;
    std::copy(top.bytes_.begin() + 8, top.bytes_.end(), file.begin() + 8);
    std::copy(b.bytes_.begin(), b.bytes_.end(), file.begin());
    // The spilled values of IFD0 were placed relative to its own offset (8),
    // which is where it sits.
    return file;
}

void parses(bool little)
{
    const auto data = parseDngColorData(makeDng(little));
    require(data.has_value());
    require(data->bayer == BayerOrder::bggr);
    require(data->rawWidth == 4032 && data->rawHeight == 3024);
    require(data->whiteLevel == 16383);
    require(data->blackLevel == 1023);  // the mean of 1020, 1022, 1024, 1026 rounded
    require(data->illuminant1 == 17 && data->illuminant2 == 21);
    for (int i = 0; i < 9; ++i) {
        require(std::abs(data->colorMatrix1[std::size_t(i)] - (i + 1) / 10.0) < 1e-9);
        require(std::abs(data->colorMatrix2[std::size_t(i)] + (i + 1) / 10.0) < 1e-9);
        require(std::abs(data->forwardMatrix1[std::size_t(i)] - i / 100.0) < 1e-9);
        require(std::abs(data->forwardMatrix2[std::size_t(i)] - (100 - i) / 100.0) < 1e-9);
    }
    require(std::abs(data->focalLengthMillimetres - 5.7) < 1e-9);
}

void readsTheExposuresOwnNeutralAndNoise(bool little)
{
    // Three pairs (red, green, blue) are spread to red, green, green, blue.
    ExposureTags tags;
    tags.neutral = tags.noise = true;
    tags.noiseValues = {1e-3, 2e-6, 2e-3, 3e-6, 4e-3, 5e-6};
    const auto data = parseDngExposureData(makeDng(little, true, tags));
    require(data.neutralPoint.has_value() && data.noiseProfile.has_value());
    require(std::abs((*data.neutralPoint)[0] - 0.5f) < 1e-6f && std::abs((*data.neutralPoint)[1] - 1.0f) < 1e-6f &&
        std::abs((*data.neutralPoint)[2] - 0.75f) < 1e-6f, "the as-shot neutral");
    const auto& noise = *data.noiseProfile;
    const float expected[8] = {1e-3f, 2e-6f, 2e-3f, 3e-6f, 2e-3f, 3e-6f, 4e-3f, 5e-6f};
    for (std::size_t index = 0; index < 8; ++index) require(std::abs(noise[index] - expected[index]) < 1e-9f, "the noise profile");

    // One pair covers every position; four are taken as they are.
    tags.noiseValues = {7e-4, 9e-7};
    const auto one = parseDngExposureData(makeDng(little, true, tags));
    require(one.noiseProfile.has_value());
    for (std::size_t index = 0; index < 8; index += 2)
        require(std::abs((*one.noiseProfile)[index] - 7e-4f) < 1e-9f && std::abs((*one.noiseProfile)[index + 1] - 9e-7f) < 1e-12f);
    tags.noiseValues = {1e-3, 1e-6, 2e-3, 2e-6, 3e-3, 3e-6, 4e-3, 4e-6};
    const auto four = parseDngExposureData(makeDng(little, true, tags));
    require(four.noiseProfile.has_value() && std::abs((*four.noiseProfile)[4] - 3e-3f) < 1e-9f &&
        std::abs((*four.noiseProfile)[7] - 4e-6f) < 1e-12f);
}

void absentIsNotZero()
{
    // No exposure tags: nothing is reported, and the colour data is still read.
    const auto without = makeDng(true);
    const auto none = parseDngExposureData(without);
    require(!none.neutralPoint.has_value() && !none.noiseProfile.has_value(), "no tags, no values");
    require(parseDngColorData(without).has_value());

    // A value that is not finite and positive is not a measurement. Zero and
    // negative values are stored as the rationals a platform writes.
    for (const std::vector<double>& bad : {std::vector<double> {0.5, 0.0, 0.75}, {0.5, -1.0, 0.75}}) {
        ExposureTags tags;
        tags.neutral = true;
        tags.neutralValues = bad;
        require(!parseDngExposureData(makeDng(true, true, tags)).neutralPoint.has_value(), "a neutral point that cannot be real");
    }
    // Not-a-number and infinity can only be stored as DOUBLEs: those reach the
    // parser as the non-finite numbers they are.
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    for (const bool little : {true, false}) {
        ExposureTags good;
        good.neutral = good.neutralAsDoubles = true;
        const auto accepted = parseDngExposureData(makeDng(little, true, good));
        require(accepted.neutralPoint && std::abs((*accepted.neutralPoint)[2] - 0.75f) < 1e-6f, "finite DOUBLE neutral values are read");
        for (const std::vector<double>& bad : {std::vector<double> {0.5, nan, 0.75}, {inf, 1.0, 0.75}, {0.5, 1.0, -inf}, {nan, nan, nan}}) {
            ExposureTags tags;
            tags.neutral = tags.neutralAsDoubles = true;
            tags.neutralValues = bad;
            require(!parseDngExposureData(makeDng(little, true, tags)).neutralPoint.has_value(), "a non-finite DOUBLE neutral value");
        }
    }
    for (const std::vector<double>& bad : {std::vector<double> {0.0, 1e-6}, {-1e-3, 1e-6}, {1e-3, -1e-6}, {nan, 1e-6}, {1e-3, inf},
                                           {1e-3, 1e-6, 2e-3}, {1e-3, 1e-6, 2e-3, 2e-6}}) {
        ExposureTags tags;
        tags.noise = true;
        tags.noiseValues = bad;
        require(!parseDngExposureData(makeDng(true, true, tags)).noiseProfile.has_value(), "a noise profile that cannot be real");
    }
    // Not a TIFF: nothing.
    require(!parseDngExposureData({}).neutralPoint.has_value() && !parseDngExposureData({}).noiseProfile.has_value());
    // Cut short, the file states only what its bytes still hold: the neutral
    // point once its three rationals are all there, the noise profile once its
    // doubles are, and each is the whole file's value; never a value from bytes
    // past the cut.
    ExposureTags tags;
    tags.neutral = tags.noise = true;
    tags.noiseValues = {1e-3, 1e-6};
    const auto whole = makeDng(true, true, tags);
    Builder encoder(true);
    const auto endOf = [&](const std::vector<std::uint8_t>& payload) {
        const auto at = std::search(whole.begin(), whole.end(), payload.begin(), payload.end());
        require(at != whole.end(), "the payload is in the file");
        return std::size_t(at - whole.begin()) + payload.size();
    };
    const auto neutralEnd = endOf(encoder.srationals({{500, 1000}, {1000, 1000}, {750, 1000}}));
    const auto noiseEnd = endOf(encoder.doubles(tags.noiseValues));
    const auto full = parseDngExposureData(whole);
    require(full.neutralPoint && full.noiseProfile, "the whole file states both");
    for (std::size_t length = 0; length < whole.size(); ++length) {
        const auto cut = parseDngExposureData(std::vector<std::uint8_t>(whole.begin(), whole.begin() + std::ptrdiff_t(length)));
        require(cut.neutralPoint.has_value() == (length >= neutralEnd), "the neutral point once all its bytes are there");
        require(cut.noiseProfile.has_value() == (length >= noiseEnd), "the noise profile once all its bytes are there");
        if (cut.neutralPoint) require(*cut.neutralPoint == *full.neutralPoint, "the whole file's neutral point");
        if (cut.noiseProfile) require(*cut.noiseProfile == *full.noiseProfile, "the whole file's noise profile");
    }
}

void refuses()
{
    require(!parseDngColorData({}).has_value());
    auto bad = makeDng(true);
    bad[0] = 'X';
    require(!parseDngColorData(bad).has_value());
    bad = makeDng(true);
    bad[2] = 43;
    require(!parseDngColorData(bad).has_value());
    require(!parseDngColorData(makeDng(true, false)).has_value());  // no colour matrix
    // Any truncation fails cleanly rather than reading past the end.
    const auto whole = makeDng(true);
    for (std::size_t length = 0; length < whole.size(); length += 7) {
        const auto cut = std::vector<std::uint8_t>(whole.begin(), whole.begin() + std::ptrdiff_t(length));
        (void)parseDngColorData(cut);
    }
    // A directory that links to itself ends.
    std::vector<std::uint8_t> loop = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 8, 0, 0, 0};
    require(!parseDngColorData(loop).has_value());
}

} // namespace

int main()
{
    parses(true);
    parses(false);
    readsTheExposuresOwnNeutralAndNoise(true);
    readsTheExposuresOwnNeutralAndNoise(false);
    absentIsNotZero();
    refuses();
    return 0;
}
