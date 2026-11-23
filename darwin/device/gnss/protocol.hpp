// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::gnss::protocol {

// The Virtio specification reserves 65535 and downward for local experimental
// devices. The battery has 65535.
inline constexpr std::uint32_t deviceId = 65534;
inline constexpr std::uint32_t version = 1;
inline constexpr std::uint32_t receiveQueue = 0;
inline constexpr std::uint32_t controlQueue = 1;
inline constexpr std::size_t queueCount = 2;

// What the driver sends on the control queue: one message per power change.
struct [[gnu::packed]] Control final {
    std::uint32_t version;
    std::uint32_t power;
};

static_assert(sizeof(Control) == 8);

} // namespace uml::gnss::protocol
