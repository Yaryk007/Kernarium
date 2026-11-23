// SPDX-License-Identifier: GPL-2.0-or-later
#include "calibration_store.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <unistd.h>

namespace uml::camera {
namespace {
using uml::platform::BayerOrder;
using uml::platform::CameraColorData;

// A file of another version fails the check and is ignored, so its lens is
// learned again.
constexpr char magic[8] = {'U', 'M', 'L', 'C', 'A', 'L', '0', '2'};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

template<typename T> void put(std::vector<std::uint8_t>& out, const T& value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template<typename T> bool take(const std::vector<std::uint8_t>& in, std::size_t& at, T& value)
{
    if (at + sizeof(T) > in.size()) return false;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

std::vector<std::uint8_t> encode(const CameraColorData& data)
{
    std::vector<std::uint8_t> out(magic, magic + sizeof(magic));
    put(out, std::uint32_t(data.bayer));
    for (const auto value : {data.blackLevel, data.whiteLevel, data.rawWidth, data.rawHeight, data.illuminant1,
                             data.illuminant2, data.anchorWidth, data.anchorHeight})
        put(out, std::uint32_t(value));
    for (const auto* matrix : {&data.colorMatrix1, &data.colorMatrix2, &data.forwardMatrix1, &data.forwardMatrix2})
        for (const auto value : *matrix) put(out, double(value));
    put(out, double(data.focalLengthMillimetres));
    for (const auto value : data.intrinsics) put(out, double(value));
    put(out, crc32(out.data(), out.size()));
    return out;
}

std::optional<CameraColorData> decode(const std::vector<std::uint8_t>& in)
{
    if (in.size() < sizeof(magic) + 4 || std::memcmp(in.data(), magic, sizeof(magic)) != 0) return std::nullopt;
    std::uint32_t stored;
    std::memcpy(&stored, in.data() + in.size() - 4, 4);
    if (stored != crc32(in.data(), in.size() - 4)) return std::nullopt;
    CameraColorData data;
    std::size_t at = sizeof(magic);
    std::uint32_t bayer = 0;
    std::uint32_t fields[8];
    bool ok = take(in, at, bayer);
    for (auto& field : fields) ok = ok && take(in, at, field);
    for (auto* matrix : {&data.colorMatrix1, &data.colorMatrix2, &data.forwardMatrix1, &data.forwardMatrix2})
        for (auto& value : *matrix) ok = ok && take(in, at, value);
    ok = ok && take(in, at, data.focalLengthMillimetres);
    for (auto& value : data.intrinsics) ok = ok && take(in, at, value);
    if (!ok || at + 4 != in.size() || bayer > std::uint32_t(BayerOrder::bggr)) return std::nullopt;
    data.bayer = BayerOrder(bayer);
    data.blackLevel = fields[0];
    data.whiteLevel = fields[1];
    data.rawWidth = fields[2];
    data.rawHeight = fields[3];
    data.illuminant1 = fields[4];
    data.illuminant2 = fields[5];
    data.anchorWidth = fields[6];
    data.anchorHeight = fields[7];
    return data;
}

std::string sanitized(std::string_view text)
{
    std::string out;
    for (const char c : text)
        out += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.'
            ? c : '_';
    return out;
}

} // namespace

CalibrationStore::CalibrationStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::string CalibrationStore::lensKey(const uml::platform::CameraInfo& info)
{
    using uml::platform::CameraLensKind;
    using uml::platform::CameraPosition;
    const char* position = info.position == CameraPosition::back ? "back" : info.position == CameraPosition::front ? "front"
                                                                                                                    : "external";
    const char* lens = info.lens == CameraLensKind::zoom ? "zoom" : info.lens == CameraLensKind::wide ? "wide"
        : info.lens == CameraLensKind::ultraWide ? "ultrawide" : info.lens == CameraLensKind::telephoto ? "tele" : "external";
    return std::string(position) + "-" + lens;
}

std::filesystem::path CalibrationStore::pathOf(std::string_view model, std::string_view lens) const
{
    return directory_ / (sanitized(model) + "." + sanitized(lens) + ".cal");
}

std::optional<CameraColorData> CalibrationStore::load(std::string_view model, std::string_view lens) const
{
    std::lock_guard lock(mutex_);
    const std::string key = std::string(model) + "/" + std::string(lens);
    if (const auto found = memory_.find(key); found != memory_.end()) return found->second;
    if (directory_.empty()) return std::nullopt;
    std::ifstream file(pathOf(model, lens), std::ios::binary);
    if (!file) return std::nullopt;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto data = decode(bytes);
    if (data) memory_[key] = *data;
    return data;
}

bool CalibrationStore::save(std::string_view model, std::string_view lens, const CameraColorData& data)
{
    std::lock_guard lock(mutex_);
    memory_[std::string(model) + "/" + std::string(lens)] = data;
    if (directory_.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) return false;
    const auto path = pathOf(model, lens);
    auto temporary = path;
    temporary += ".tmp." + std::to_string(::getpid());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const auto bytes = encode(data);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace uml::camera
