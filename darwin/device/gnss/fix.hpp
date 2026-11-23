// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace uml::gnss {

/* One position of the host. An absent optional field is left out of the
 * stream, never written as zero. */
struct Fix final {
    /* The fix's own time: UTC, in nanoseconds since 1970-01-01. */
    std::int64_t utcNs {};
    /* WGS 84, in degrees. */
    double latitude {};
    double longitude {};
    /* The host knows the position only roughly (iOS's reduced accuracy),
     * and the stream says so. */
    bool approximate {};
    /* Meters above sea level, and above the WGS 84 ellipsoid. */
    std::optional<double> altitude;
    std::optional<double> ellipsoidalAltitude;
    /* Meters of radius for the horizontal accuracy, and of the vertical one. */
    std::optional<double> horizontalAccuracy;
    std::optional<double> verticalAccuracy;
    /* Meters per second, and its accuracy. */
    std::optional<double> speed;
    std::optional<double> speedAccuracy;
    /* Degrees clockwise from true north, and its accuracy. */
    std::optional<double> course;
    std::optional<double> courseAccuracy;
};

/* How long the receiver repeats its newest fix before it reports no fix. A
 * precise fix goes stale at once, and iOS delivers an approximate one up to
 * twenty minutes old. */
inline constexpr std::int64_t preciseHoldNs =
    std::chrono::nanoseconds(std::chrono::seconds(10)).count();
inline constexpr std::int64_t approximateHoldNs =
    std::chrono::nanoseconds(std::chrono::minutes(20)).count();

/* The newest fix still worth reporting at `nowUtcNs`, or none. */
[[nodiscard]] inline std::optional<Fix> heldFix(
    const std::optional<Fix>& fix,
    std::int64_t nowUtcNs) noexcept
{
    if (!fix)
        return std::nullopt;
    const auto hold = fix->approximate ? approximateHoldNs : preciseHoldNs;
    // A fix from the future, as a clock that moved back makes, is not stale.
    if (nowUtcNs - fix->utcNs > hold)
        return std::nullopt;
    return fix;
}

/* A source of fixes that stands in for the host's own, such as the
 * launcher's `--location` or a test's. Its calls are made one at a time, on
 * the device's thread. */
class FixSource {
public:
    virtual ~FixSource() = default;

    /* The newest fix as of `nowUtcNs`, if there is one. */
    [[nodiscard]] virtual std::optional<Fix> latest(std::int64_t nowUtcNs) = 0;
    /* The guest powered its receiver on or off at `nowUtcNs`. */
    virtual void powerChanged(bool /* powered */, std::int64_t /* nowUtcNs */)
    {
    }
};

} // namespace uml::gnss
