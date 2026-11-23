// SPDX-License-Identifier: GPL-2.0-or-later

#include "nmea_writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <vector>

namespace uml::gnss {
namespace {

constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000;
constexpr std::int64_t secondsPerDay = 86'400;
// The coordinates are written to 1e-7 minute.
constexpr std::int64_t minuteUnits = 10'000'000;
constexpr std::int64_t degreeUnits = 60 * minuteUnits;
constexpr double knotsPerMeterPerSecond = 3600.0 / 1852.0;
constexpr double kilometersPerHourPerMeterPerSecond = 3.6;

[[nodiscard]] std::int64_t floorDivide(std::int64_t value, std::int64_t divisor)
{
    auto quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0)))
        --quotient;
    return quotient;
}

struct Utc final {
    int year {};
    int month {};
    int day {};
    int hour {};
    int minute {};
    int second {};
    int hundredth {};
};

// The proleptic Gregorian calendar of the day count, after Howard Hinnant's
// civil_from_days.
[[nodiscard]] Utc utcOf(std::int64_t utcNs)
{
    const auto seconds = floorDivide(utcNs, nanosecondsPerSecond);
    const auto subsecond = utcNs - seconds * nanosecondsPerSecond;
    const auto days = floorDivide(seconds, secondsPerDay);
    const auto secondOfDay = seconds - days * secondsPerDay;

    const auto shifted = days + 719'468;
    const auto era = floorDivide(shifted, 146'097);
    const auto dayOfEra = shifted - era * 146'097;
    const auto yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36'524 - dayOfEra / 146'096) /
        365;
    const auto dayOfYear =
        dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const auto monthIndex = (5 * dayOfYear + 2) / 153;
    const auto day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const auto month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const auto year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

    return {
        .year = static_cast<int>(year),
        .month = static_cast<int>(month),
        .day = static_cast<int>(day),
        .hour = static_cast<int>(secondOfDay / 3600),
        .minute = static_cast<int>(secondOfDay % 3600 / 60),
        .second = static_cast<int>(secondOfDay % 60),
        // Truncated, so that 23:59:59.995 never carries into the next day.
        .hundredth = static_cast<int>(subsecond / 10'000'000),
    };
}

[[nodiscard]] std::string digits(std::int64_t value, int width)
{
    std::array<char, 24> buffer {};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    (void)error;
    std::string text(buffer.data(), end);
    if (static_cast<int>(text.size()) < width)
        text.insert(0, static_cast<std::size_t>(width) - text.size(), '0');
    return text;
}

[[nodiscard]] std::string timeText(const Utc& utc)
{
    return digits(utc.hour, 2) + digits(utc.minute, 2) + digits(utc.second, 2) +
        "." + digits(utc.hundredth, 2);
}

[[nodiscard]] std::string dateText(const Utc& utc)
{
    return digits(utc.day, 2) + digits(utc.month, 2) + digits(utc.year % 100, 2);
}

/* `value` in the fixed notation with `precision` decimals, in the C locale
 * whatever the process set. A value that rounds to zero loses its sign. */
[[nodiscard]] std::string number(double value, int precision)
{
    if (std::fabs(value) < 0.5 * std::pow(10.0, -precision))
        value = 0.0;
    std::array<char, 48> buffer {};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::fixed,
        precision);
    (void)error;
    return std::string(buffer.data(), end);
}

[[nodiscard]] std::string number(
    const std::optional<double>& value,
    int precision)
{
    if (!value || !std::isfinite(*value))
        return {};
    return number(*value, precision);
}

struct Angle final {
    std::string text;
    char hemisphere {};
};

/* ddmm.mmmmmmm (dddmm.mmmmmmm for a longitude) and the hemisphere. The
 * rounding is done in whole 1e-7 minutes, so that 59.99999999 minutes carries
 * into the degree instead of printing 60. */
[[nodiscard]] Angle angle(
    double degrees,
    double limit,
    int degreeWidth,
    char positive,
    char negative)
{
    degrees = std::clamp(degrees, -limit, limit);
    const auto units = static_cast<std::int64_t>(std::llround(
        std::fabs(degrees) * 60.0 * static_cast<double>(minuteUnits)));
    const auto wholeDegrees = units / degreeUnits;
    const auto remainder = units % degreeUnits;
    Angle result;
    result.text = digits(wholeDegrees, degreeWidth) +
        digits(remainder / minuteUnits, 2) + "." +
        digits(remainder % minuteUnits, 7);
    result.hemisphere = degrees < 0.0 && units != 0 ? negative : positive;
    return result;
}

[[nodiscard]] std::string join(const std::vector<std::string>& fields)
{
    std::string text;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0)
            text.push_back(',');
        text += fields[index];
    }
    return text;
}

/* The fix's class; the letter is the mode of RMC and VTG. */
enum class Quality : char {
    none = 'N',
    precise = 'A',
    approximate = 'E',
};

} // namespace

std::uint8_t nmeaChecksum(std::string_view body) noexcept
{
    std::uint8_t checksum = 0;
    for (const char character : body)
        checksum ^= static_cast<std::uint8_t>(character);
    return checksum;
}

std::string nmeaSentence(std::string_view body)
{
    static constexpr char hexadecimal[] = "0123456789ABCDEF";
    const auto checksum = nmeaChecksum(body);
    std::string text;
    text.reserve(body.size() + 7);
    text.push_back('$');
    text.append(body);
    text.push_back('*');
    text.push_back(hexadecimal[checksum >> 4]);
    text.push_back(hexadecimal[checksum & 0x0F]);
    text += "\r\n";
    return text;
}

std::string nmeaEpoch(const std::optional<Fix>& fix, std::int64_t nowUtcNs)
{
    const auto quality = !fix ? Quality::none :
        fix->approximate      ? Quality::approximate :
                                Quality::precise;
    const auto utc = utcOf(fix ? fix->utcNs : nowUtcNs);
    const auto time = timeText(utc);
    const auto date = dateText(utc);
    const auto mode = std::string(1, static_cast<char>(quality));

    std::string latitudeText;
    std::string latitudeHemisphere;
    std::string longitudeText;
    std::string longitudeHemisphere;
    if (fix) {
        const auto latitude = angle(fix->latitude, 90.0, 2, 'N', 'S');
        const auto longitude = angle(fix->longitude, 180.0, 3, 'E', 'W');
        latitudeText = latitude.text;
        latitudeHemisphere = std::string(1, latitude.hemisphere);
        longitudeText = longitude.text;
        longitudeHemisphere = std::string(1, longitude.hemisphere);
    }

    // GGA: quality 1 is a satellite fix, 6 an estimate and 0 none. The
    // satellite count and the dilution are empty, because there are no
    // satellites. The geoid separation is what lies between the two altitudes.
    const auto altitude = fix ? fix->altitude : std::nullopt;
    const auto ellipsoidal = fix ? fix->ellipsoidalAltitude : std::nullopt;
    const std::optional<double> separation = altitude && ellipsoidal ?
        std::optional<double>(*ellipsoidal - *altitude) :
        std::nullopt;
    const bool hasAltitude = altitude && std::isfinite(*altitude);
    const bool hasSeparation = separation && std::isfinite(*separation);
    const auto gga = join({
        "GNGGA",
        time,
        latitudeText,
        latitudeHemisphere,
        longitudeText,
        longitudeHemisphere,
        quality == Quality::none        ? "0" :
            quality == Quality::approximate ? "6" :
                                              "1",
        "",
        "",
        hasAltitude ? number(altitude, 1) : "",
        hasAltitude ? "M" : "",
        hasSeparation ? number(separation, 1) : "",
        hasSeparation ? "M" : "",
        "",
        "",
    });

    // RMC: the status is A while there is a position, and the mode letter
    // says what kind: A a satellite fix, E an estimate, N none.
    const auto speed = fix ? fix->speed : std::nullopt;
    const auto course = fix ? fix->course : std::nullopt;
    const std::optional<double> knots = speed ?
        std::optional<double>(*speed * knotsPerMeterPerSecond) :
        std::nullopt;
    const std::optional<double> kilometers = speed ?
        std::optional<double>(*speed * kilometersPerHourPerMeterPerSecond) :
        std::nullopt;
    const auto rmc = join({
        "GNRMC",
        time,
        fix ? "A" : "V",
        latitudeText,
        latitudeHemisphere,
        longitudeText,
        longitudeHemisphere,
        number(knots, 2),
        number(course, 1),
        date,
        "",
        "",
        mode,
    });

    // GSA: no satellite is used, so only the fix type is filled in.
    const bool threeDimensional = quality == Quality::precise && hasAltitude;
    std::vector<std::string> gsaFields {
        "GNGSA",
        "A",
        quality == Quality::none ? "1" : threeDimensional ? "3" : "2",
    };
    gsaFields.insert(gsaFields.end(), 15, std::string());
    const auto gsa = join(gsaFields);

    // GST: the horizontal accuracy is a radial error, so each axis carries it
    // divided by the square root of two.
    const auto horizontal = fix ? fix->horizontalAccuracy : std::nullopt;
    const auto vertical = fix ? fix->verticalAccuracy : std::nullopt;
    const std::optional<double> axis =
        horizontal && std::isfinite(*horizontal) ?
        std::optional<double>(*horizontal / std::sqrt(2.0)) :
        std::nullopt;
    const auto gst = join({
        "GNGST",
        time,
        "",
        number(axis, 2),
        number(axis, 2),
        axis ? "0.0" : "",
        number(axis, 2),
        number(axis, 2),
        number(vertical, 1),
    });

    const auto vtg = join({
        "GNVTG",
        number(course, 1),
        "T",
        "",
        "M",
        number(knots, 2),
        "N",
        number(kilometers, 2),
        "K",
        mode,
    });

    // $PKRNF carries what NMEA cannot: the fix's class, and the accuracies as
    // iOS reported them.
    const auto proprietary = join({
        "PKRNF",
        quality == Quality::none        ? "N" :
            quality == Quality::approximate ? "A" :
                                              "P",
        number(horizontal, 1),
        number(vertical, 1),
        number(fix ? fix->speedAccuracy : std::nullopt, 2),
        number(fix ? fix->courseAccuracy : std::nullopt, 1),
    });

    return nmeaSentence(gga) + nmeaSentence(rmc) + nmeaSentence(gsa) +
        nmeaSentence(gst) + nmeaSentence(vtg) + nmeaSentence(proprietary);
}

} // namespace uml::gnss
