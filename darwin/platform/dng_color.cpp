// SPDX-License-Identifier: GPL-2.0-or-later
#include "dng_color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace uml::platform {
namespace {

constexpr std::uint16_t tagImageWidth = 256;
constexpr std::uint16_t tagImageLength = 257;
constexpr std::uint16_t tagSubIfds = 330;
constexpr std::uint16_t tagCfaPattern = 33422;
constexpr std::uint16_t tagExifIfd = 34665;
constexpr std::uint16_t tagFocalLength = 37386;
constexpr std::uint16_t tagBlackLevel = 50714;
constexpr std::uint16_t tagWhiteLevel = 50717;
constexpr std::uint16_t tagAsShotNeutral = 50728;
constexpr std::uint16_t tagNoiseProfile = 51041;
constexpr std::uint16_t tagColorMatrix1 = 50721;
constexpr std::uint16_t tagColorMatrix2 = 50722;
constexpr std::uint16_t tagCalibrationIlluminant1 = 50778;
constexpr std::uint16_t tagCalibrationIlluminant2 = 50779;
constexpr std::uint16_t tagForwardMatrix1 = 50964;
constexpr std::uint16_t tagForwardMatrix2 = 50965;

constexpr unsigned maximumDirectories = 16;

class Tiff {
public:
    explicit Tiff(std::span<const std::uint8_t> bytes) : bytes_(bytes)
    {
        if (bytes_.size() < 8) return;
        if (bytes_[0] == 'I' && bytes_[1] == 'I') little_ = true;
        else if (bytes_[0] == 'M' && bytes_[1] == 'M') little_ = false;
        else return;
        if (u16(2) != 42) return;
        valid_ = true;
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint32_t firstDirectory() const noexcept { return u32(4); }

    struct Entry {
        std::uint16_t tag {};
        std::uint16_t type {};
        std::uint32_t count {};
        std::size_t data {};  // offset of the value bytes
    };

    // The entries of the directory at `offset`; empty when it is not one.
    [[nodiscard]] std::vector<Entry> directory(std::uint32_t offset, std::uint32_t* next = nullptr) const
    {
        std::vector<Entry> entries;
        if (next) *next = 0;
        if (std::size_t(offset) + 2 > bytes_.size()) return entries;
        const auto count = u16(offset);
        if (std::size_t(offset) + 2 + std::size_t(count) * 12 + 4 > bytes_.size()) return entries;
        for (unsigned index = 0; index < count; ++index) {
            const std::size_t at = std::size_t(offset) + 2 + std::size_t(index) * 12;
            Entry entry;
            entry.tag = u16(at);
            entry.type = u16(at + 2);
            entry.count = u32(at + 4);
            const auto size = typeSize(entry.type) * std::size_t(entry.count);
            entry.data = size <= 4 ? at + 8 : u32(at + 8);
            if (typeSize(entry.type) == 0 || entry.data + size > bytes_.size() || entry.data + size < entry.data) continue;
            entries.push_back(entry);
        }
        if (next) *next = u32(std::size_t(offset) + 2 + std::size_t(count) * 12);
        return entries;
    }

    [[nodiscard]] static std::size_t typeSize(std::uint16_t type) noexcept
    {
        switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
        }
    }

    [[nodiscard]] std::uint16_t u16(std::size_t at) const noexcept
    {
        if (at + 2 > bytes_.size()) return 0;
        return little_ ? std::uint16_t(bytes_[at] | bytes_[at + 1] << 8)
                       : std::uint16_t(bytes_[at] << 8 | bytes_[at + 1]);
    }
    [[nodiscard]] std::uint32_t u32(std::size_t at) const noexcept
    {
        if (at + 4 > bytes_.size()) return 0;
        return little_ ? std::uint32_t(bytes_[at]) | std::uint32_t(bytes_[at + 1]) << 8 |
                std::uint32_t(bytes_[at + 2]) << 16 | std::uint32_t(bytes_[at + 3]) << 24
                       : std::uint32_t(bytes_[at]) << 24 | std::uint32_t(bytes_[at + 1]) << 16 |
                std::uint32_t(bytes_[at + 2]) << 8 | std::uint32_t(bytes_[at + 3]);
    }
    [[nodiscard]] std::uint8_t u8(std::size_t at) const noexcept { return at < bytes_.size() ? bytes_[at] : 0; }

    // The numeric element `index` of an entry, as the tag types state it.
    [[nodiscard]] double number(const Entry& entry, std::size_t index) const noexcept
    {
        const auto size = typeSize(entry.type);
        const std::size_t at = entry.data + index * size;
        switch (entry.type) {
        case 1: case 7: return u8(at);
        case 6: return std::int8_t(u8(at));
        case 3: return u16(at);
        case 8: return std::int16_t(u16(at));
        case 4: return u32(at);
        case 9: return std::int32_t(u32(at));
        case 5: {
            const auto denominator = u32(at + 4);
            return denominator ? double(u32(at)) / denominator : 0.0;
        }
        case 10: {
            const auto denominator = std::int32_t(u32(at + 4));
            return denominator ? double(std::int32_t(u32(at))) / denominator : 0.0;
        }
        case 12: {
            // IEEE-754 binary64, in the file's byte order.
            std::uint64_t bits = 0;
            for (std::size_t byte = 0; byte < 8; ++byte)
                bits |= std::uint64_t(u8(at + byte)) << (little_ ? byte * 8 : (7 - byte) * 8);
            double value = 0;
            std::memcpy(&value, &bits, sizeof value);
            return value;
        }
        default: return 0.0;
        }
    }

private:
    std::span<const std::uint8_t> bytes_;
    bool little_ {true};
    bool valid_ {};
};

bool matrix(const Tiff& tiff, const Tiff::Entry& entry, std::array<double, 9>& out)
{
    if (entry.count != 9 || (entry.type != 10 && entry.type != 5)) return false;
    for (std::size_t index = 0; index < 9; ++index) out[index] = tiff.number(entry, index);
    return true;
}

BayerOrder orderOf(const Tiff& tiff, const Tiff::Entry& entry)
{
    if (entry.count != 4 || entry.type != 1) return BayerOrder::none;
    const std::uint8_t pattern[4] = {tiff.u8(entry.data), tiff.u8(entry.data + 1), tiff.u8(entry.data + 2),
                                     tiff.u8(entry.data + 3)};
    // 0 is red, 1 green, 2 blue.
    if (pattern[0] == 0 && pattern[1] == 1 && pattern[2] == 1 && pattern[3] == 2) return BayerOrder::rggb;
    if (pattern[0] == 1 && pattern[1] == 0 && pattern[2] == 2 && pattern[3] == 1) return BayerOrder::grbg;
    if (pattern[0] == 1 && pattern[1] == 2 && pattern[2] == 0 && pattern[3] == 1) return BayerOrder::gbrg;
    if (pattern[0] == 2 && pattern[1] == 1 && pattern[2] == 1 && pattern[3] == 0) return BayerOrder::bggr;
    return BayerOrder::none;
}

struct Reader {
    const Tiff& tiff;
    CameraColorData data;
    bool haveColor {};
    unsigned visited {};
    DngExposureData exposureData;

    void directory(std::uint32_t offset)
    {
        if (visited++ >= maximumDirectories) return;
        std::uint32_t next = 0;
        for (const auto& entry : tiff.directory(offset, &next)) {
            switch (entry.tag) {
            case tagColorMatrix1: haveColor |= matrix(tiff, entry, data.colorMatrix1); break;
            case tagColorMatrix2: matrix(tiff, entry, data.colorMatrix2); break;
            case tagForwardMatrix1: matrix(tiff, entry, data.forwardMatrix1); break;
            case tagForwardMatrix2: matrix(tiff, entry, data.forwardMatrix2); break;
            case tagCalibrationIlluminant1: data.illuminant1 = std::uint32_t(tiff.number(entry, 0)); break;
            case tagCalibrationIlluminant2: data.illuminant2 = std::uint32_t(tiff.number(entry, 0)); break;
            case tagBlackLevel:
                // The mean of the levels per CFA position when there are several.
                if (entry.count >= 1) {
                    double sum = 0;
                    for (std::size_t index = 0; index < entry.count && index < 4; ++index) sum += tiff.number(entry, index);
                    data.blackLevel = std::uint32_t(sum / double(std::min<std::size_t>(entry.count, 4)) + 0.5);
                }
                break;
            case tagWhiteLevel: data.whiteLevel = std::uint32_t(tiff.number(entry, 0)); break;
            case tagAsShotNeutral: neutral(entry); break;
            case tagNoiseProfile: noise(entry); break;
            case tagCfaPattern: {
                const auto order = orderOf(tiff, entry);
                if (order != BayerOrder::none) data.bayer = order;
                break;
            }
            case tagImageWidth:
                // The raw image's directory is the one with a CFA pattern; the
                // size is taken from the directory that has both.
                pendingWidth = std::uint32_t(tiff.number(entry, 0));
                break;
            case tagImageLength: pendingHeight = std::uint32_t(tiff.number(entry, 0)); break;
            case tagSubIfds:
                for (std::size_t index = 0; index < entry.count && index < 4; ++index)
                    sub.push_back(std::uint32_t(tiff.number(entry, index)));
                break;
            case tagExifIfd: exif = std::uint32_t(tiff.number(entry, 0)); break;
            default: break;
            }
        }
        if (data.bayer != BayerOrder::none && data.rawWidth == 0) {
            data.rawWidth = pendingWidth;
            data.rawHeight = pendingHeight;
        }
        pendingWidth = pendingHeight = 0;
        auto pending = std::move(sub);
        sub.clear();
        for (const auto child : pending) directory(child);
        if (next != 0) directory(next);
    }

    // Three channels, as a rational or a real; a value that is not finite and
    // positive leaves the whole point absent.
    void neutral(const Tiff::Entry& entry)
    {
        if (entry.count != 3 || (entry.type != 5 && entry.type != 10 && entry.type != 11 && entry.type != 12)) return;
        std::array<float, 3> point {};
        for (std::size_t index = 0; index < 3; ++index) {
            const double value = tiff.number(entry, index);
            if (!std::isfinite(value) || value <= 0.0 || value > 1e6) return;
            point[index] = float(value);
        }
        exposureData.neutralPoint = point;
    }

    // Pairs of doubles, one pair for the whole sensor or one per colour plane
    // (red, green, blue) or per CFA position; the scale is positive and the
    // offset is not negative.
    void noise(const Tiff::Entry& entry)
    {
        if (entry.type != 12 || entry.count == 0 || entry.count % 2 != 0) return;
        const std::size_t pairs = entry.count / 2;
        if (pairs != 1 && pairs != 3 && pairs != 4) return;
        std::array<std::array<float, 2>, 4> channel {};
        for (std::size_t pair = 0; pair < pairs; ++pair) {
            const double scale = tiff.number(entry, pair * 2);
            const double offset = tiff.number(entry, pair * 2 + 1);
            if (!std::isfinite(scale) || !std::isfinite(offset) || scale <= 0.0 || offset < 0.0 || scale > 1e6 || offset > 1e12)
                return;
            channel[pair] = {float(scale), float(offset)};
        }
        // One profile covers every position; three are red, green and blue.
        if (pairs == 1) channel = {channel[0], channel[0], channel[0], channel[0]};
        else if (pairs == 3) channel = {channel[0], channel[1], channel[1], channel[2]};
        std::array<float, 8> profile {};
        for (std::size_t index = 0; index < 4; ++index) {
            profile[index * 2] = channel[index][0];
            profile[index * 2 + 1] = channel[index][1];
        }
        exposureData.noiseProfile = profile;
    }

    void exifDirectory()
    {
        if (exif == 0) return;
        for (const auto& entry : tiff.directory(exif))
            if (entry.tag == tagFocalLength) data.focalLengthMillimetres = tiff.number(entry, 0);
    }

    std::uint32_t pendingWidth {};
    std::uint32_t pendingHeight {};
    std::vector<std::uint32_t> sub;
    std::uint32_t exif {};
};

} // namespace

std::optional<CameraColorData> parseDngColorData(std::span<const std::uint8_t> dng)
{
    const Tiff tiff(dng);
    if (!tiff.valid()) return std::nullopt;
    Reader reader {tiff, {}, false, 0, {}, 0, 0, {}, 0};
    reader.directory(tiff.firstDirectory());
    reader.exifDirectory();
    if (!reader.haveColor) return std::nullopt;
    return reader.data;
}

DngExposureData parseDngExposureData(std::span<const std::uint8_t> dng)
{
    const Tiff tiff(dng);
    if (!tiff.valid()) return {};
    Reader reader {tiff, {}, false, 0, {}, 0, 0, {}, 0};
    reader.directory(tiff.firstDirectory());
    return reader.exposureData;
}

} // namespace uml::platform
