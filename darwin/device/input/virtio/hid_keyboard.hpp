// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace uml::input::virtio::hid {

/* The usages of the HID Keyboard/Keypad page (0x07) that the table covers:
 * 0 through 0xff. A key's position on a keyboard is its usage, whatever the
 * layout prints on it, and the host sends positions: the guest's own key
 * layout turns them into characters. */
inline constexpr std::uint32_t keyboardUsageCount = 256;

/* The Linux key code that `usage` produces, as a USB or Bluetooth keyboard
 * with the usage in its report descriptor does through Linux's HID driver, or
 * 0 when it produces none. The Linux table maps a usage it has no key for to
 * KEY_UNKNOWN; this returns 0 for those too, which a guest's key layout
 * ignores anyway. */
[[nodiscard]] std::uint16_t linuxKey(std::uint32_t usage) noexcept;

/* The keys the table can produce, one bit per Linux key code from 0 to 255:
 * the key bitmap the hardware keyboard advertises, so that a guest's input
 * core delivers exactly these keys. */
[[nodiscard]] const std::array<std::uint8_t, 32>& keyImage() noexcept;

/* Whether `code` is in the image. */
[[nodiscard]] bool producesKey(std::uint16_t code) noexcept;

} // namespace uml::input::virtio::hid
