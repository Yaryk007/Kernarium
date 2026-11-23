// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The table is transcribed from hid_keyboard[] in the Linux kernel's
 * drivers/hid/hid-input.c (Copyright (c) 2000-2001 Vojtech Pavlik, Copyright
 * (c) 2006-2010 Jiri Kosina), so this file keeps that file's licence
 * identifier. Row for row and value for value it is the kernel's table, with
 * the kernel's `unk` for KEY_UNKNOWN; only the lookups around it are new.
 */

#include "hid_keyboard.hpp"

namespace uml::input::virtio::hid {
namespace {

constexpr std::uint8_t unk = 240; // KEY_UNKNOWN

constexpr std::array<std::uint8_t, keyboardUsageCount> hidKeyboard {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,unk,unk,unk,121,unk, 89, 93,124, 92, 94, 95,unk,unk,unk,
    122,123, 90, 91, 85,unk,unk,unk,unk,unk,unk,unk,111,unk,unk,unk,
    unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,
    unk,unk,unk,unk,unk,unk,179,180,unk,unk,unk,unk,unk,unk,unk,unk,
    unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,
    unk,unk,unk,unk,unk,unk,unk,unk,111,unk,unk,unk,unk,unk,unk,unk,
     29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
    150,158,159,128,136,177,178,176,142,152,173,140,unk,unk,unk,unk,
};

constexpr std::uint16_t keyFor(std::uint32_t usage) noexcept
{
    if (usage >= keyboardUsageCount)
        return 0;
    const auto code = hidKeyboard[usage];
    return code == unk ? 0 : code;
}

constexpr std::array<std::uint8_t, 32> makeImage() noexcept
{
    std::array<std::uint8_t, 32> image {};
    for (std::uint32_t usage = 0; usage < keyboardUsageCount; ++usage) {
        const auto code = keyFor(usage);
        image[code / 8U] |= static_cast<std::uint8_t>(1U << (code % 8U));
    }
    // Code 0 is not a key: it stands for a usage with none.
    image[0] &= static_cast<std::uint8_t>(~1U);
    return image;
}

constexpr std::array<std::uint8_t, 32> image = makeImage();

} // namespace

std::uint16_t linuxKey(std::uint32_t usage) noexcept
{
    return keyFor(usage);
}

const std::array<std::uint8_t, 32>& keyImage() noexcept
{
    return image;
}

bool producesKey(std::uint16_t code) noexcept
{
    return code < image.size() * 8U &&
        (image[code / 8U] & (1U << (code % 8U))) != 0;
}

} // namespace uml::input::virtio::hid
