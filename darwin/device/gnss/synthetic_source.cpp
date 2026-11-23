// SPDX-License-Identifier: GPL-2.0-or-later

#include "synthetic_source.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>

namespace uml::gnss {
namespace {

constexpr double earthRadius = 6'371'008.8;
constexpr double defaultAccuracy = 5.0;
constexpr double approximateAccuracy = 5000.0;
constexpr double verticalAccuracyOfAltitude = 10.0;
constexpr double degreesToRadians = std::numbers::pi / 180.0;

[[nodiscard]] bool parseNumber(std::string_view text, double& value)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r'))
        text.remove_suffix(1);
    if (text.empty())
        return false;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc {} && end == text.data() + text.size() &&
        std::isfinite(value);
}

[[nodiscard]] std::vector<std::string_view> split(
    std::string_view text,
    char separator)
{
    std::vector<std::string_view> parts;
    for (;;) {
        const auto at = text.find(separator);
        parts.push_back(text.substr(0, at));
        if (at == std::string_view::npos)
            return parts;
        text.remove_prefix(at + 1);
    }
}

[[nodiscard]] bool validPosition(double latitude, double longitude)
{
    return latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 &&
        longitude <= 180.0;
}

[[nodiscard]] double distance(
    const RoutePoint& from,
    const RoutePoint& to,
    double& bearing)
{
    const auto latitudeFrom = from.latitude * degreesToRadians;
    const auto latitudeTo = to.latitude * degreesToRadians;
    const auto latitudeDelta = latitudeTo - latitudeFrom;
    const auto longitudeDelta = (to.longitude - from.longitude) * degreesToRadians;
    const auto a = std::sin(latitudeDelta / 2) * std::sin(latitudeDelta / 2) +
        std::cos(latitudeFrom) * std::cos(latitudeTo) *
            std::sin(longitudeDelta / 2) * std::sin(longitudeDelta / 2);
    const auto meters = 2 * earthRadius * std::asin(std::min(1.0, std::sqrt(a)));
    const auto y = std::sin(longitudeDelta) * std::cos(latitudeTo);
    const auto x = std::cos(latitudeFrom) * std::sin(latitudeTo) -
        std::sin(latitudeFrom) * std::cos(latitudeTo) * std::cos(longitudeDelta);
    bearing = std::fmod(std::atan2(y, x) / degreesToRadians + 360.0, 360.0);
    return meters;
}

class FixedSource final : public FixSource {
public:
    explicit FixedSource(Fix fix)
        : fix_(fix)
    {
    }

    std::optional<Fix> latest(std::int64_t nowUtcNs) override
    {
        auto fix = fix_;
        fix.utcNs = nowUtcNs;
        return fix;
    }

private:
    Fix fix_;
};

class RouteSource final : public FixSource {
public:
    explicit RouteSource(std::vector<RoutePoint> points)
        : points_(std::move(points))
    {
    }

    std::optional<Fix> latest(std::int64_t nowUtcNs) override
    {
        if (!started_) {
            started_ = true;
            originNs_ = nowUtcNs;
        }
        const auto elapsed =
            static_cast<double>(nowUtcNs - originNs_) / 1'000'000'000.0;
        Fix fix;
        fix.utcNs = nowUtcNs;
        // The first point at or after `elapsed`.
        const auto later = std::find_if(
            points_.begin(), points_.end(), [elapsed](const RoutePoint& point) {
                return point.seconds >= elapsed;
            });
        if (later == points_.begin() || later == points_.end()) {
            const auto& point = later == points_.end() ? points_.back() : *later;
            place(fix, point);
            return fix;
        }
        const auto& from = *(later - 1);
        const auto& to = *later;
        const auto span = to.seconds - from.seconds;
        const auto share = span > 0.0 ? (elapsed - from.seconds) / span : 1.0;
        RoutePoint here = to;
        here.latitude = from.latitude + (to.latitude - from.latitude) * share;
        here.longitude = from.longitude + (to.longitude - from.longitude) * share;
        if (from.altitude && to.altitude)
            here.altitude = *from.altitude + (*to.altitude - *from.altitude) * share;
        place(fix, here);
        double bearing = 0.0;
        const auto meters = distance(from, to, bearing);
        if (span > 0.0 && meters > 0.5) {
            fix.speed = meters / span;
            fix.course = bearing;
        }
        return fix;
    }

    void powerChanged(bool powered, std::int64_t nowUtcNs) override
    {
        // Every power-on starts the route again.
        started_ = powered;
        originNs_ = nowUtcNs;
    }

private:
    static void place(Fix& fix, const RoutePoint& point)
    {
        fix.latitude = point.latitude;
        fix.longitude = point.longitude;
        fix.horizontalAccuracy = point.accuracy.value_or(defaultAccuracy);
        if (point.altitude) {
            fix.altitude = point.altitude;
            fix.ellipsoidalAltitude = point.altitude;
            fix.verticalAccuracy = verticalAccuracyOfAltitude;
        }
    }

    std::vector<RoutePoint> points_;
    bool started_ {};
    std::int64_t originNs_ {};
};

} // namespace

std::unique_ptr<FixSource> makeFixedSource(Fix fix)
{
    return std::make_unique<FixedSource>(fix);
}

std::unique_ptr<FixSource> makeRouteSource(std::vector<RoutePoint> points)
{
    if (points.empty())
        return nullptr;
    return std::make_unique<RouteSource>(std::move(points));
}

std::vector<RoutePoint> parseRoute(std::string_view text, std::string& error)
{
    std::vector<RoutePoint> points;
    std::size_t lineNumber = 0;
    for (const auto line : split(text, '\n')) {
        ++lineNumber;
        auto trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == '\r')
            continue;
        const auto fields = split(trimmed, ',');
        RoutePoint point;
        double altitude = 0.0;
        double accuracy = 0.0;
        const bool valid = fields.size() >= 3 && fields.size() <= 5 &&
            parseNumber(fields[0], point.seconds) && point.seconds >= 0.0 &&
            parseNumber(fields[1], point.latitude) &&
            parseNumber(fields[2], point.longitude) &&
            validPosition(point.latitude, point.longitude) &&
            (fields.size() < 4 || parseNumber(fields[3], altitude)) &&
            (fields.size() < 5 || (parseNumber(fields[4], accuracy) && accuracy > 0.0)) &&
            (points.empty() || point.seconds >= points.back().seconds);
        if (!valid) {
            error = "route line " + std::to_string(lineNumber) +
                " is not SECONDS,LAT,LON[,ALT[,ACC]] in time order";
            return {};
        }
        if (fields.size() >= 4)
            point.altitude = altitude;
        if (fields.size() >= 5)
            point.accuracy = accuracy;
        points.push_back(point);
    }
    if (points.empty())
        error = "the route has no points";
    return points;
}

std::unique_ptr<FixSource> makeSyntheticSource(
    std::string_view specification,
    std::string& error)
{
    const auto colon = specification.find(':');
    const auto kind = specification.substr(0, colon);
    const auto rest = colon == std::string_view::npos ?
        std::string_view {} :
        specification.substr(colon + 1);

    if (kind == "route") {
        std::ifstream file{std::string(rest)};
        if (rest.empty() || !file) {
            error = "cannot read the route file";
            return nullptr;
        }
        std::ostringstream text;
        text << file.rdbuf();
        return makeRouteSource(parseRoute(text.str(), error));
    }

    const bool fixed = kind == "fixed";
    if (!fixed && kind != "approximate") {
        error = "expected fixed:, approximate: or route:";
        return nullptr;
    }
    const auto fields = split(rest, ',');
    Fix fix;
    double altitude = 0.0;
    double accuracy = 0.0;
    const bool valid = fixed ?
        (fields.size() >= 2 && fields.size() <= 4 &&
         parseNumber(fields[0], fix.latitude) &&
         parseNumber(fields[1], fix.longitude) &&
         (fields.size() < 3 || parseNumber(fields[2], altitude)) &&
         (fields.size() < 4 || (parseNumber(fields[3], accuracy) && accuracy > 0.0))) :
        (fields.size() == 2 && parseNumber(fields[0], fix.latitude) &&
         parseNumber(fields[1], fix.longitude));
    if (!valid || !validPosition(fix.latitude, fix.longitude)) {
        error = fixed ? "expected fixed:LAT,LON[,ALT[,ACC]]" :
                        "expected approximate:LAT,LON";
        return nullptr;
    }
    if (!fixed) {
        fix.approximate = true;
        fix.horizontalAccuracy = approximateAccuracy;
        return makeFixedSource(fix);
    }
    fix.horizontalAccuracy = fields.size() >= 4 ? accuracy : defaultAccuracy;
    if (fields.size() >= 3) {
        fix.altitude = altitude;
        fix.ellipsoidalAltitude = altitude;
        fix.verticalAccuracy = verticalAccuracyOfAltitude;
    }
    return makeFixedSource(fix);
}

} // namespace uml::gnss
