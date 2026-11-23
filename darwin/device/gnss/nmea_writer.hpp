// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "fix.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace uml::gnss {

/* The XOR of the characters between '$' and '*', which `body` is. */
[[nodiscard]] std::uint8_t nmeaChecksum(std::string_view body) noexcept;

/* "$" body "*HH\r\n". */
[[nodiscard]] std::string nmeaSentence(std::string_view body);

/* One epoch of the receiver: GGA, RMC, GSA, GST and VTG of talker GN, then
 * $PKRNF, each sentence ended by CR LF. Without a fix every sentence says so
 * and carries the time of `nowUtcNs`; with one they carry its own UTC time,
 * so an epoch that repeats a fix repeats its time. The README has the forms. */
[[nodiscard]] std::string nmeaEpoch(
    const std::optional<Fix>& fix,
    std::int64_t nowUtcNs);

} // namespace uml::gnss
