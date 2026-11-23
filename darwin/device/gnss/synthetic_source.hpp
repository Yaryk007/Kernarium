// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "fix.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uml::gnss {

/* The test sources of the launcher's `--location`. Nothing here reads a real
 * location. */

/* A fix that never moves. Each query returns it dated `now`, as a live
 * receiver does. */
[[nodiscard]] std::unique_ptr<FixSource> makeFixedSource(Fix fix);

/* One point of a route: the position `seconds` after the guest powered its
 * receiver. */
struct RoutePoint final {
    double seconds {};
    double latitude {};
    double longitude {};
    std::optional<double> altitude;
    std::optional<double> accuracy;
};

/* A position that moves along `points` (ordered by time, at least one) at
 * the pace the guest has had power for, and stays at the last point after it.
 * Speed and course come from the leg being driven. */
[[nodiscard]] std::unique_ptr<FixSource> makeRouteSource(
    std::vector<RoutePoint> points);

/* The points of a route file's text: one `SECONDS,LAT,LON[,ALT[,ACC]]` a
 * line, blank lines and lines that start with '#' ignored. Returns an empty
 * list and sets `error` for text that is not one. */
[[nodiscard]] std::vector<RoutePoint> parseRoute(
    std::string_view text,
    std::string& error);

/* The source a `--location` value names:
 *   fixed:LAT,LON[,ALT[,ACC]]   a precise fix, ACC meters (default 5)
 *   approximate:LAT,LON         an approximate fix, 5000 m
 *   route:PATH                  a route file (parseRoute)
 * Returns null and sets `error` for any other value. */
[[nodiscard]] std::unique_ptr<FixSource> makeSyntheticSource(
    std::string_view specification,
    std::string& error);

} // namespace uml::gnss
