// SPDX-License-Identifier: GPL-2.0-or-later

#include "hid_keyboard.hpp"
#include "require.hpp"

#include <array>
#include <bitset>
#include <cstdlib>
#include <initializer_list>

namespace {

namespace hid = uml::input::virtio::hid;
using uml::testing::require;

struct Row {
    std::uint32_t usage;
    std::uint16_t key;
};

// Rows of hid_keyboard[] in the Linux kernel's drivers/hid/hid-input.c, read
// from the kernel's source (the Linux key codes are the numbers of
// linux/input-event-codes.h).
constexpr std::array rows {
    // Letters, digits and the keys a typist uses.
    Row {0x04, 30},  // a
    Row {0x05, 48},  // b
    Row {0x1d, 44},  // z
    Row {0x1e, 2},   // 1
    Row {0x27, 11},  // 0
    Row {0x28, 28},  // Return
    Row {0x29, 1},   // Escape
    Row {0x2a, 14},  // Backspace
    Row {0x2b, 15},  // Tab
    Row {0x2c, 57},  // Space
    Row {0x35, 41},  // Grave accent
    Row {0x39, 58},  // Caps Lock
    Row {0x3a, 59},  // F1
    Row {0x44, 87},  // F11
    Row {0x45, 88},  // F12
    Row {0x46, 99},  // Print Screen
    Row {0x47, 70},  // Scroll Lock
    Row {0x48, 119}, // Pause
    Row {0x49, 110}, // Insert
    Row {0x4c, 111}, // Delete Forward
    Row {0x4f, 106}, // Right Arrow
    Row {0x50, 105}, // Left Arrow
    Row {0x51, 108}, // Down Arrow
    Row {0x52, 103}, // Up Arrow
    Row {0x53, 69},  // Num Lock
    Row {0x58, 96},  // Keypad Enter
    Row {0x63, 83},  // Keypad period
    // The key an ISO keyboard has beside the left Shift, and the one a US
    // keyboard has instead of it.
    Row {0x31, 43},
    Row {0x32, 43},
    Row {0x64, 86},
    Row {0x65, 127}, // Application
    Row {0x66, 116}, // Power
    Row {0x67, 117}, // Keypad equal
    // F13 through F24.
    Row {0x68, 183},
    Row {0x6f, 190},
    Row {0x73, 194},
    // International and the Japanese keys.
    Row {0x85, 121}, // Keypad comma
    Row {0x87, 89},  // International1 (Ro)
    Row {0x88, 93},  // International2 (Katakana/Hiragana)
    Row {0x89, 124}, // International3 (Yen)
    Row {0x8a, 92},  // International4 (Henkan)
    Row {0x8b, 94},  // International5 (Muhenkan)
    Row {0x8c, 95},  // International6 (Keypad Jp comma)
    // The Korean keys, and the Katakana, Hiragana and Zenkaku/Hankaku keys.
    Row {0x90, 122},
    Row {0x91, 123},
    Row {0x92, 90},
    Row {0x93, 91},
    Row {0x94, 85},
    // The modifiers.
    Row {0xe0, 29},  // Left Control
    Row {0xe1, 42},  // Left Shift
    Row {0xe2, 56},  // Left Alt
    Row {0xe3, 125}, // Left GUI (Command)
    Row {0xe4, 97},  // Right Control
    Row {0xe5, 54},  // Right Shift
    Row {0xe6, 100}, // Right Alt
    Row {0xe7, 126}, // Right GUI
    // The consumer keys that end the table.
    Row {0xe8, 164},
    Row {0xf0, 150},
    Row {0xfb, 140},
};

void rowsMatchTheKernelsTable()
{
    for (const auto& row : rows)
        require(hid::linuxKey(row.usage) == row.key);
    // Several usages are one key in the kernel's table.
    require(hid::linuxKey(0x4c) == hid::linuxKey(0x9c));
    require(hid::linuxKey(0x4c) == hid::linuxKey(0xd8));
}

void usagesTheTableHasNoKeyForAreDropped()
{
    // Locking keys and the usages between and beyond the table's rows: the
    // kernel maps them to KEY_UNKNOWN (240), which nothing here forwards.
    for (const std::uint32_t usage :
         {0x00u, 0x01u, 0x02u, 0x03u, 0x82u, 0x83u, 0x84u, 0x86u, 0x8du,
          0xa5u, 0xdfu, 0xfcu, 0xffu})
        require(hid::linuxKey(usage) == 0);
    // Usages of other pages, and of the page past the table.
    for (const std::uint32_t usage :
         {0x100u, 0x1e0u, 0x10004u, 0xffffffffu})
        require(hid::linuxKey(usage) == 0);
    require(hid::linuxKey(0x03) == 0 && hid::linuxKey(0x04) != 0);
}

void theKeyImageIsExactlyWhatTheTableCanProduce()
{
    std::bitset<256> produced;
    for (std::uint32_t usage = 0; usage < hid::keyboardUsageCount; ++usage) {
        const auto key = hid::linuxKey(usage);
        require(key < 256);
        if (key != 0)
            produced.set(key);
    }
    const auto& image = hid::keyImage();
    std::size_t count = 0;
    for (std::uint32_t code = 0; code < 256; ++code) {
        const bool inImage = (image[code / 8U] & (1U << (code % 8U))) != 0;
        require(inImage == produced.test(code));
        require(hid::producesKey(static_cast<std::uint16_t>(code)) == inImage);
        count += inImage ? 1U : 0U;
    }
    require(count == produced.count());
    // 162 distinct keys, from Escape to F24.
    require(count == 162);
    require(hid::producesKey(1) && hid::producesKey(194));
    require(!hid::producesKey(0));
    // KEY_UNKNOWN, the keys above the table's highest, and codes beyond the
    // image are not produced.
    for (const std::uint16_t code : std::initializer_list<std::uint16_t> {
             240, 195, 255, 256, 0x110, 0xffff})
        require(!hid::producesKey(code));
    // Every key the usage rows produce is in the image.
    for (const auto& row : rows)
        require(hid::producesKey(row.key));
}

} // namespace

int main()
{
    rowsMatchTheKernelsTable();
    usagesTheTableHasNoKeyForAreDropped();
    theKeyImageIsExactlyWhatTheTableCanProduce();
    return EXIT_SUCCESS;
}
