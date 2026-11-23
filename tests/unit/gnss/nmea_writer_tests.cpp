// SPDX-License-Identifier: GPL-2.0-or-later

#include "named_tests.hpp"
#include "nmea_writer.hpp"
#include "require.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uml::gnss::Fix;
using uml::gnss::nmeaEpoch;
using uml::gnss::nmeaSentence;
using uml::testing::require;

// 2026-11-10 13:45:30.250 UTC.
constexpr std::int64_t preciseTime = 1'794'318'330'250'000'000;
// 2026-11-10 23:59:59.995 UTC.
constexpr std::int64_t approximateTime = 1'794'355'199'995'000'000;
// 2026-12-31 23:59:59.000 UTC.
constexpr std::int64_t noFixTime = 1'798'761'599'000'000'000;

Fix preciseFix()
{
    Fix fix;
    fix.utcNs = preciseTime;
    fix.latitude = 35.6895;
    fix.longitude = 139.6917;
    fix.altitude = 40.0;
    fix.ellipsoidalAltitude = 80.5;
    fix.horizontalAccuracy = 8.0;
    fix.verticalAccuracy = 12.0;
    fix.speed = 1.5;
    fix.speedAccuracy = 0.5;
    fix.course = 90.0;
    fix.courseAccuracy = 10.0;
    return fix;
}

Fix approximateFix()
{
    Fix fix;
    fix.utcNs = approximateTime;
    fix.latitude = -33.86;
    fix.longitude = -151.20;
    fix.approximate = true;
    fix.horizontalAccuracy = 5000.0;
    return fix;
}

/* The golden epochs were made by an independent script that writes each
 * sentence field by field and computes every checksum. */
void writesThePreciseEpoch()
{
    const std::string expected =
        "$GNGGA,134530.25,3541.3700000,N,13941.5020000,E,1,,,40.0,M,40.5,M,,*60\r\n"
        "$GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,101126,,,A*4C\r\n"
        "$GNGSA,A,3,,,,,,,,,,,,,,,*02\r\n"
        "$GNGST,134530.25,,5.66,5.66,0.0,5.66,5.66,12.0*53\r\n"
        "$GNVTG,90.0,T,,M,2.92,N,5.40,K,A*22\r\n"
        "$PKRNF,P,8.0,12.0,0.50,10.0*02\r\n";
    // The fix's own time is the epoch's, whatever `now` is.
    require(nmeaEpoch(preciseFix(), preciseTime + 7'000'000'000) == expected);
}

void writesTheApproximateEpoch()
{
    const std::string expected =
        "$GNGGA,235959.99,3351.6000000,S,15112.0000000,W,6,,,,,,,,*61\r\n"
        "$GNRMC,235959.99,A,3351.6000000,S,15112.0000000,W,,,101126,,,E*4B\r\n"
        "$GNGSA,A,2,,,,,,,,,,,,,,,*03\r\n"
        "$GNGST,235959.99,,3535.53,3535.53,0.0,3535.53,3535.53,*48\r\n"
        "$GNVTG,,T,,M,,N,,K,E*39\r\n"
        "$PKRNF,A,5000.0,,,*37\r\n";
    require(nmeaEpoch(approximateFix(), 0) == expected);
}

void writesTheNoFixEpochAtTheCurrentTime()
{
    const std::string expected =
        "$GNGGA,235959.00,,,,,0,,,,,,,,*57\r\n"
        "$GNRMC,235959.00,V,,,,,,,311226,,,N*67\r\n"
        "$GNGSA,A,1,,,,,,,,,,,,,,,*00\r\n"
        "$GNGST,235959.00,,,,,,,*66\r\n"
        "$GNVTG,,T,,M,,N,,K,N*32\r\n"
        "$PKRNF,N,,,,*23\r\n";
    require(nmeaEpoch(std::nullopt, noFixTime) == expected);
}

void computesChecksums()
{
    require(uml::gnss::nmeaChecksum("") == 0);
    require(uml::gnss::nmeaChecksum("A") == 0x41);
    require(nmeaSentence("A") == "$A*41\r\n");
    require(nmeaSentence("") == "$*00\r\n");
}

/* The comma separated fields of sentence `number` (0 is GGA, 1 RMC) of an
 * epoch, without the checksum. */
std::vector<std::string> fieldsOf(const std::string& epoch, std::size_t number)
{
    std::size_t start = 0;
    for (std::size_t skipped = 0; skipped < number; ++skipped)
        start = epoch.find("\r\n", start) + 2;
    const auto end = epoch.find('*', start);
    std::vector<std::string> fields;
    std::string field;
    for (std::size_t at = start + 1; at < end; ++at) {
        if (epoch[at] == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(epoch[at]);
        }
    }
    fields.push_back(field);
    return fields;
}

/* Latitude, its hemisphere, longitude and its hemisphere, from GGA. */
std::string positionOf(const Fix& fix)
{
    const auto fields = fieldsOf(nmeaEpoch(fix, fix.utcNs), 0);
    return fields[2] + "," + fields[3] + "," + fields[4] + "," + fields[5];
}

void writesTheHemispheres()
{
    Fix fix = preciseFix();
    require(positionOf(fix) == "3541.3700000,N,13941.5020000,E");
    fix.latitude = -35.6895;
    require(positionOf(fix) == "3541.3700000,S,13941.5020000,E");
    fix.longitude = -139.6917;
    require(positionOf(fix) == "3541.3700000,S,13941.5020000,W");
    fix.latitude = 35.6895;
    require(positionOf(fix) == "3541.3700000,N,13941.5020000,W");
}

void writesAnExactDegreeWithoutAFraction()
{
    Fix fix = preciseFix();
    fix.latitude = 0.0;
    fix.longitude = 0.0;
    require(positionOf(fix) == "0000.0000000,N,00000.0000000,E");
    // A value that rounds to zero keeps no sign.
    fix.latitude = -1e-12;
    fix.longitude = -1e-12;
    require(positionOf(fix) == "0000.0000000,N,00000.0000000,E");
}

void carriesMinutesThatRoundToSixty()
{
    Fix fix = preciseFix();
    // 10 degrees 59.99999999... minutes is 11 degrees at 1e-7 minute.
    fix.latitude = 10.9999999999;
    fix.longitude = 99.9999999999;
    require(positionOf(fix) == "1100.0000000,N,10000.0000000,E");
    fix.latitude = 10.9999999;
    require(positionOf(fix).starts_with("1059.9999940,N"));
}

void clampsAPositionOutsideTheGlobe()
{
    Fix fix = preciseFix();
    fix.latitude = 95.0;
    fix.longitude = -190.0;
    require(positionOf(fix) == "9000.0000000,N,18000.0000000,W");
}

/* The time and the date of RMC. */
std::string timeOf(std::int64_t utcNs)
{
    const auto fields = fieldsOf(nmeaEpoch(std::nullopt, utcNs), 1);
    return fields[1] + " " + fields[9];
}

void breaksUtcDownAcrossMidnightAndTheCalendar()
{
    // 23:59:59.995 truncates, never rounds into the next day.
    require(timeOf(approximateTime) == "235959.99 101126");
    // 2026-11-11 00:00:00.000.
    require(timeOf(1'794'355'200'000'000'000) == "000000.00 111126");
    // 2028-02-29 12:00:00, a leap day.
    require(timeOf(1'835'438'400'000'000'000) == "120000.00 290228");
    // 2100-03-01 00:00:00, after a century that is not a leap year.
    require(timeOf(4'107'542'400'000'000'000) == "000000.00 010300");
    // Before the epoch: 1969-12-31 23:59:59.500.
    require(timeOf(-500'000'000) == "235959.50 311269");
    // The epoch itself.
    require(timeOf(0) == "000000.00 010170");
}

void leavesOutWhatTheHostDoesNotKnow()
{
    Fix fix = preciseFix();
    fix.altitude.reset();
    fix.ellipsoidalAltitude.reset();
    fix.verticalAccuracy.reset();
    fix.speed.reset();
    fix.course.reset();
    const auto epoch = nmeaEpoch(fix, fix.utcNs);
    // The fix is two dimensional and says nothing it was not told.
    require(epoch.find("E,1,,,,,,,,*") != std::string::npos);
    require(epoch.find("$GNGSA,A,2,") != std::string::npos);
    require(epoch.find("E,,,101126,,,A*") != std::string::npos);
    require(epoch.find("$GNVTG,,T,,M,,N,,K,A*") != std::string::npos);
    require(epoch.find("$GNGST,134530.25,,5.66,5.66,0.0,5.66,5.66,*") !=
            std::string::npos);
    require(epoch.find("$PKRNF,P,8.0,,0.50,10.0*") != std::string::npos);

    // Only one altitude: the separation is not invented.
    fix = preciseFix();
    fix.ellipsoidalAltitude.reset();
    require(nmeaEpoch(fix, fix.utcNs).find(",40.0,M,,,,*") != std::string::npos);
}

void keepsAZeroSpeedAndNeverWritesNegativeZero()
{
    Fix fix = preciseFix();
    fix.speed = 0.0;
    fix.course = -0.0;
    fix.latitude = 35.6895;
    const auto epoch = nmeaEpoch(fix, fix.utcNs);
    require(epoch.find(",E,0.00,0.0,101126,") != std::string::npos);
    require(epoch.find("-0") == std::string::npos);
    require(epoch.find("$GNVTG,0.0,T,,M,0.00,N,0.00,K,A*") != std::string::npos);
}

void everySentenceEndsWithAChecksumAndCrLf()
{
    for (const auto& epoch : {
             nmeaEpoch(preciseFix(), 0),
             nmeaEpoch(approximateFix(), 0),
             nmeaEpoch(std::nullopt, 0),
         }) {
        std::size_t position = 0;
        int sentences = 0;
        while (position < epoch.size()) {
            require(epoch[position] == '$');
            const auto star = epoch.find('*', position);
            require(star != std::string::npos);
            require(epoch.compare(star + 3, 2, "\r\n") == 0);
            const auto body = std::string_view(epoch).substr(
                position + 1, star - position - 1);
            const auto text = epoch.substr(star + 1, 2);
            require(std::stoul(text, nullptr, 16) ==
                    uml::gnss::nmeaChecksum(body));
            position = star + 5;
            ++sentences;
        }
        require(sentences == 6);
    }
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"writes-the-precise-epoch", writesThePreciseEpoch},
        {"writes-the-approximate-epoch", writesTheApproximateEpoch},
        {"writes-the-no-fix-epoch", writesTheNoFixEpochAtTheCurrentTime},
        {"computes-checksums", computesChecksums},
        {"writes-the-hemispheres", writesTheHemispheres},
        {"writes-an-exact-degree", writesAnExactDegreeWithoutAFraction},
        {"carries-minutes-that-round-to-sixty", carriesMinutesThatRoundToSixty},
        {"clamps-a-position-outside-the-globe", clampsAPositionOutsideTheGlobe},
        {"breaks-utc-down", breaksUtcDownAcrossMidnightAndTheCalendar},
        {"leaves-out-what-the-host-does-not-know", leavesOutWhatTheHostDoesNotKnow},
        {"never-writes-negative-zero", keepsAZeroSpeedAndNeverWritesNegativeZero},
        {"frames-every-sentence", everySentenceEndsWithAChecksumAndCrLf},
    };
    return uml::testing::runNamedTests(tests);
}
