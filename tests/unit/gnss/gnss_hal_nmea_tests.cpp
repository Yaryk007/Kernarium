// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest GNSS HAL's reading of the host's NMEA stream (the overlay's nmea.hpp)
// against the host's writer (nmea_writer.cpp): the two ends of the wire cannot
// drift, because every epoch the writer makes for a corpus of synthetic fixes is
// read back here. This test is GPL-2.0-or-later and includes the overlay's
// Apache-2.0 headers; the binary is built and run by the test suite only and is
// never distributed.

#include "named_tests.hpp"
#include "nmea.hpp"
#include "nmea_writer.hpp"
#include "require.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using andarium::gnss::nmea::Epoch;
using andarium::gnss::nmea::FixClass;
using andarium::gnss::nmea::Parser;
using uml::gnss::Fix;
using uml::testing::require;

constexpr std::int64_t nsPerHundredth = 10'000'000;

// 2026-11-10 13:45:30.250 UTC.
constexpr std::int64_t preciseTime = 1'794'318'330'250'000'000;

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

/* The time the stream carries: the fix's own, to the hundredth. */
std::int64_t streamMilliseconds(std::int64_t utcNs)
{
    return utcNs / nsPerHundredth * 10;
}

std::vector<Epoch> readAll(const std::string& stream)
{
    Parser parser;
    return parser.feed(stream);
}

bool near(double value, double expected, double tolerance)
{
    return std::fabs(value - expected) <= tolerance;
}

bool near(const std::optional<double>& value, const std::optional<double>& expected, double tolerance)
{
    if (!expected)
        return !value;
    return value && near(*value, *expected, tolerance);
}

/* The writer's output for `fix`, read back by the parser. */
void requireRoundTrip(const Fix& fix)
{
    const auto epochs = readAll(uml::gnss::nmeaEpoch(fix, fix.utcNs));
    require(epochs.size() == 1, "one epoch");
    const auto& epoch = epochs[0];
    require(epoch.sentences.size() == 6, "six sentences");
    const auto& read = epoch.position;
    require(read.fix_class == (fix.approximate ? FixClass::approximate : FixClass::precise));
    require(near(read.latitude, fix.latitude, 1e-8), "latitude");
    require(near(read.longitude, fix.longitude, 1e-8), "longitude");
    require(read.utc_ms == streamMilliseconds(fix.utcNs), "time");
    require(epoch.utc_ms == read.utc_ms);
    require(near(read.altitude, fix.altitude, 0.051), "altitude");
    std::optional<double> separation;
    if (fix.altitude && fix.ellipsoidalAltitude)
        separation = *fix.ellipsoidalAltitude - *fix.altitude;
    require(near(read.geoid_separation, separation, 0.051), "geoid separation");
    require(near(read.speed, fix.speed, 0.006), "speed");
    require(near(read.course, fix.course, 0.051), "course");
    require(near(read.horizontal_accuracy, fix.horizontalAccuracy, 0.051), "horizontal accuracy");
    require(near(read.vertical_accuracy, fix.verticalAccuracy, 0.051), "vertical accuracy");
    require(near(read.speed_accuracy, fix.speedAccuracy, 0.006), "speed accuracy");
    require(near(read.course_accuracy, fix.courseAccuracy, 0.051), "course accuracy");
}

void roundTripsAPreciseFix()
{
    requireRoundTrip(preciseFix());
}

void roundTripsWhatTheHostDoesNotKnow()
{
    Fix bare;
    bare.utcNs = preciseTime;
    bare.latitude = 48.8566;
    bare.longitude = 2.3522;
    bare.horizontalAccuracy = 30.0;
    requireRoundTrip(bare);
    // The altitude without the geoid separation, and the reverse.
    Fix sea = bare;
    sea.altitude = 35.0;
    requireRoundTrip(sea);
    Fix ellipsoid = bare;
    ellipsoid.ellipsoidalAltitude = 80.0;
    requireRoundTrip(ellipsoid);
}

void roundTripsAnApproximateFix()
{
    Fix fix;
    fix.utcNs = 1'794'355'199'995'000'000;
    fix.latitude = -33.86;
    fix.longitude = -151.20;
    fix.approximate = true;
    fix.horizontalAccuracy = 5000.0;
    requireRoundTrip(fix);
}

void roundTripsEveryHemisphereAndTheDateLine()
{
    for (const double latitude : {0.0, 1e-7, -1e-7, 89.9999999, -89.9999999, 90.0, -90.0, 35.0}) {
        for (const double longitude : {0.0, 1e-7, -1e-7, 179.9999999, -179.9999999, 180.0, -180.0, -70.5}) {
            Fix fix = preciseFix();
            fix.latitude = latitude;
            fix.longitude = longitude;
            requireRoundTrip(fix);
        }
    }
}

void roundTripsAcrossTheCalendar()
{
    // Midnight's last hundredth, a leap day, a year end, and the last year the
    // two-digit date can name.
    for (const std::int64_t seconds : {
             std::int64_t{1'794'355'199}, // 2026-11-10 23:59:59
             std::int64_t{1'709'164'800}, // 2024-02-29 00:00:00
             std::int64_t{1'798'761'599}, // 2026-12-31 23:59:59
             std::int64_t{4'070'908'799}, // 2098-12-31 23:59:59
             std::int64_t{946'684'800},   // 2000-01-01 00:00:00
         }) {
        for (const std::int64_t hundredths : {0, 1, 50, 99}) {
            Fix fix = preciseFix();
            fix.utcNs = seconds * 1'000'000'000 + hundredths * nsPerHundredth + 4'000'000;
            requireRoundTrip(fix);
        }
    }
}

/* A corpus from a fixed generator: positions all over the globe, the optional
 * fields present or not, both classes. */
void roundTripsASyntheticCorpus()
{
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    const auto next = [&state]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    };
    for (int index = 0; index < 3000; ++index) {
        Fix fix;
        fix.utcNs = 1'700'000'000'000'000'000 + static_cast<std::int64_t>(next() * 3.0e17);
        fix.latitude = next() * 180.0 - 90.0;
        fix.longitude = next() * 360.0 - 180.0;
        fix.approximate = next() < 0.3;
        if (next() < 0.7)
            fix.horizontalAccuracy = 0.1 + next() * 6000.0;
        if (next() < 0.5) {
            fix.altitude = next() * 4000.0 - 200.0;
            if (next() < 0.8)
                fix.ellipsoidalAltitude = *fix.altitude + next() * 100.0 - 50.0;
            if (next() < 0.8)
                fix.verticalAccuracy = next() * 50.0;
        }
        if (next() < 0.5) {
            fix.speed = next() * 80.0;
            if (next() < 0.8)
                fix.speedAccuracy = next() * 3.0;
        }
        if (next() < 0.5) {
            fix.course = next() * 359.9;
            if (next() < 0.8)
                fix.courseAccuracy = next() * 90.0;
        }
        const auto text = uml::gnss::nmeaEpoch(fix, fix.utcNs);
        const auto epochs = readAll(text);
        require(epochs.size() == 1);
        const auto& read = epochs[0].position;
        require(read.fix_class == (fix.approximate ? FixClass::approximate : FixClass::precise));
        require(near(read.latitude, fix.latitude, 1e-8));
        require(near(read.longitude, fix.longitude, 1e-8));
        require(read.utc_ms == streamMilliseconds(fix.utcNs));
        require(near(read.altitude, fix.altitude, 0.051));
        require(near(read.speed, fix.speed, 0.006));
        require(near(read.course, fix.course, 0.051));
        require(near(read.horizontal_accuracy, fix.horizontalAccuracy, 0.051));
        require(near(read.vertical_accuracy, fix.verticalAccuracy, 0.051));
    }
}

void readsNoFixAtTheCurrentTime()
{
    // 2026-12-31 23:59:59.000 UTC.
    constexpr std::int64_t now = 1'798'761'599'000'000'000;
    const auto epochs = readAll(uml::gnss::nmeaEpoch(std::nullopt, now));
    require(epochs.size() == 1);
    require(epochs[0].position.fix_class == FixClass::none);
    require(!epochs[0].position.altitude && !epochs[0].position.horizontal_accuracy);
    require(epochs[0].utc_ms == 1'798'761'599'000);
    require(epochs[0].sentences.size() == 6);
}

/* The stream arrives in whatever pieces the read returned. */
void readsAnyChunking()
{
    std::string stream;
    for (int index = 0; index < 4; ++index) {
        Fix fix = preciseFix();
        fix.utcNs += index * 1'000'000'000LL;
        fix.latitude += index * 0.001;
        stream += uml::gnss::nmeaEpoch(fix, fix.utcNs);
    }
    stream += uml::gnss::nmeaEpoch(std::nullopt, preciseTime + 9'000'000'000);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, stream.size()}) {
        Parser parser;
        std::vector<Epoch> epochs;
        for (std::size_t at = 0; at < stream.size(); at += chunk) {
            for (auto& epoch : parser.feed(std::string_view(stream).substr(at, chunk)))
                epochs.push_back(std::move(epoch));
        }
        require(epochs.size() == 5, "five epochs");
        for (int index = 0; index < 4; ++index) {
            require(epochs[static_cast<std::size_t>(index)].position.fix_class == FixClass::precise);
            require(near(epochs[static_cast<std::size_t>(index)].position.latitude, 35.6895 + index * 0.001, 1e-8));
        }
        require(epochs[4].position.fix_class == FixClass::none);
        require(parser.rejected() == 0);
    }
}

std::string withChecksum(const std::string& body)
{
    return andarium::gnss::nmea::sentence_text(body);
}

void dropsASentenceWithABadChecksum()
{
    auto epoch = uml::gnss::nmeaEpoch(preciseFix(), preciseTime);
    // Spoil the GGA's checksum: its latitude no longer matches what it claims.
    const auto star = epoch.find('*');
    epoch[star + 1] = epoch[star + 1] == '0' ? '1' : '0';
    Parser parser;
    const auto epochs = parser.feed(epoch);
    // The five good sentences remain, but without a GGA there is no epoch.
    require(epochs.empty());
    require(parser.rejected() == 1);
    // The epoch after it reads whole.
    require(parser.feed(uml::gnss::nmeaEpoch(preciseFix(), preciseTime)).size() == 1);
}

void skipsGarbageAndLongLines()
{
    Parser parser;
    const std::string good = uml::gnss::nmeaEpoch(preciseFix(), preciseTime);
    std::string stream = "garbage\r\n\xff\xfe,,,*\n$$$\r\n" + std::string("$GNXXX,") + std::string(300, '1') + "\r\n";
    stream += good;
    const auto epochs = parser.feed(stream);
    require(epochs.size() == 1);
    require(epochs[0].position.fix_class == FixClass::precise);
    require(parser.rejected() >= 1);
}

void startsMidEpochAtTheNextOne()
{
    const auto first = uml::gnss::nmeaEpoch(preciseFix(), preciseTime);
    // The stream begins at the third sentence of an epoch (the read missed the rest).
    std::size_t start = 0;
    for (int skipped = 0; skipped < 2; ++skipped)
        start = first.find("\r\n", start) + 2;
    const auto epochs = readAll(first.substr(start) + uml::gnss::nmeaEpoch(preciseFix(), preciseTime));
    require(epochs.size() == 1, "the partial epoch is dropped");
    require(epochs[0].sentences.size() == 6);
}

void endsAnEpochAtTheNextGgaWithoutPkrnf()
{
    const auto full = uml::gnss::nmeaEpoch(preciseFix(), preciseTime);
    // Cut $PKRNF from the first epoch.
    const auto pk = full.rfind("$PKRNF");
    const auto cut = full.substr(0, pk);
    const auto epochs = readAll(cut + full);
    require(epochs.size() == 2);
    require(epochs[0].sentences.size() == 5);
    require(epochs[0].position.fix_class == FixClass::precise);
    // Without $PKRNF there are no accuracies; the class comes from GGA.
    require(!epochs[0].position.horizontal_accuracy);
}

void takesTheLessPreciseClass()
{
    const auto make = [](const std::string& quality, const std::string& letter, const std::string& status) {
        return withChecksum("GNGGA,134530.25,3541.3700000,N,13941.5020000,E," + quality + ",,,40.0,M,40.5,M,,") +
               withChecksum("GNRMC,134530.25," + status + ",3541.3700000,N,13941.5020000,E,2.92,90.0,101126,,,A") +
               withChecksum("PKRNF," + letter + ",8.0,12.0,0.50,10.0");
    };
    const auto read = [&make](const std::string& quality, const std::string& letter, const std::string& status = "A") {
        const auto epochs = readAll(make(quality, letter, status));
        require(epochs.size() == 1);
        return epochs[0].position.fix_class;
    };
    require(read("1", "P") == FixClass::precise);
    require(read("6", "A") == FixClass::approximate);
    require(read("1", "A") == FixClass::approximate, "GGA precise, PKRNF approximate");
    require(read("6", "P") == FixClass::approximate, "GGA approximate, PKRNF precise");
    require(read("1", "N") == FixClass::none, "PKRNF says no fix");
    require(read("0", "P") == FixClass::none, "GGA says no fix");
    require(read("2", "P") == FixClass::none, "a quality this stream never writes");
    require(read("1", "X") == FixClass::precise, "an unreadable PKRNF has no opinion");
    require(read("1", "P", "V") == FixClass::none, "RMC void");
}

void refusesWhatDoesNotParse()
{
    const auto read = [](const std::string& gga, const std::string& rmc) {
        const auto epochs = readAll(withChecksum(gga) + withChecksum(rmc) + withChecksum("PKRNF,P,8.0,,,"));
        require(epochs.size() == 1);
        return epochs[0].position.fix_class;
    };
    const std::string gga = "GNGGA,134530.25,3541.3700000,N,13941.5020000,E,1,,,40.0,M,40.5,M,,";
    const std::string rmc = "GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,101126,,,A";
    require(read(gga, rmc) == FixClass::precise);
    require(read("GNGGA,134530.25,3541.3700000,X,13941.5020000,E,1,,,40.0,M,40.5,M,,", rmc) == FixClass::none);
    require(read("GNGGA,134530.25,9541.3700000,N,13941.5020000,E,1,,,40.0,M,40.5,M,,", rmc) == FixClass::none);
    require(read("GNGGA,134530.25,3561.3700000,N,13941.5020000,E,1,,,40.0,M,40.5,M,,", rmc) == FixClass::none);
    require(read("GNGGA,134530.25,,N,13941.5020000,E,1,,,40.0,M,40.5,M,,", rmc) == FixClass::none);
    require(read("GNGGA,134530.25,35e1.37,N,13941.5020000,E,1,,,40.0,M,40.5,M,,", rmc) == FixClass::none);
    require(read(gga, "GNRMC,254530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,101126,,,A") == FixClass::none);
    require(read(gga, "GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,321126,,,A") == FixClass::none);
    require(read(gga, "GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,101326,,,A") == FixClass::none);
    require(read(gga, "GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,1011,,,A") == FixClass::none);
    // No RMC at all: the date is unknown.
    const auto epochs = readAll(withChecksum(gga) + withChecksum("PKRNF,P,8.0,,,"));
    require(epochs.size() == 1 && epochs[0].position.fix_class == FixClass::none);
}

void readsNumbersAndTimes()
{
    using andarium::gnss::nmea::date_days;
    using andarium::gnss::nmea::number;
    using andarium::gnss::nmea::time_of_day_ms;
    require(number("12.5") == 12.5);
    require(number("-0.25") == -0.25);
    require(number("+3") == 3.0);
    require(number("007.50") == 7.5);
    require(number("0") == 0.0);
    require(!number(""));
    require(!number("."));
    require(!number("-"));
    require(!number("1.2.3"));
    require(!number("1e3"));
    require(!number("12a"));
    require(!number("123456789012345678"));
    require(time_of_day_ms("000000") == 0);
    require(time_of_day_ms("235959.99") == 86'399'990);
    require(time_of_day_ms("134530.250") == 49'530'250);
    require(time_of_day_ms("134530.2") == 49'530'200);
    require(!time_of_day_ms("240000"));
    require(!time_of_day_ms("126000"));
    require(!time_of_day_ms("13453"));
    require(!time_of_day_ms("134530."));
    require(!time_of_day_ms("134530,25"));
    require(date_days("010100") == 10'957);
    require(date_days("290224") == 19'782);
    require(date_days("311299") == 47'481);
    require(!date_days("000126"));
    require(!date_days("011326"));
    require(!date_days("0101"));
}

void checksumsMatchTheWriter()
{
    for (const std::string body : {"A", "GNGGA,1,2,3", "PKRNF,P,8.0,12.0,0.50,10.0"})
        require(andarium::gnss::nmea::sentence_text(body) == uml::gnss::nmeaSentence(body));
    require(andarium::gnss::nmea::checksum("A") == uml::gnss::nmeaChecksum("A"));
    using andarium::gnss::nmea::sentence_body;
    require(sentence_body("$A*41\r\n") == std::optional<std::string_view>("A"));
    require(sentence_body("$A*41") == std::optional<std::string_view>("A"));
    require(sentence_body("$A*41\n") == std::optional<std::string_view>("A"));
    require(sentence_body("$a*61").has_value());
    require(!sentence_body("$A*42"));
    require(!sentence_body("A*41"));
    require(!sentence_body("$A41"));
    require(!sentence_body("$A*4"));
    require(!sentence_body("$A*4G"));
    require(!sentence_body(""));
}

/* A stream that never ends an epoch (no second GGA, no $PKRNF) cannot grow the
 * epoch in progress without bound; one more sentence than an epoch may hold
 * drops it, and the parser carries on with the next epoch. */
void boundsAnEpochThatNeverEnds()
{
    using andarium::gnss::nmea::kMaxEpochSentences;
    const std::string gga = "GNGGA,134530.25,3541.3700000,N,13941.5020000,E,1,,,40.0,M,40.5,M,,";
    const std::string rmc = "GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,2.92,90.0,101126,,,A";
    const std::string filler = withChecksum("GNGSA,A,3,,,,,,,,,,,,,,,");
    const auto epochOf = [&](std::size_t sentences) {
        // GGA, RMC and filler, then $PKRNF as the last of `sentences`.
        std::string stream = withChecksum(gga) + withChecksum(rmc);
        for (std::size_t count = 2; count + 1 < sentences; ++count)
            stream += filler;
        return stream + withChecksum("PKRNF,P,8.0,,,");
    };

    {
        // The most an epoch may hold is still an epoch.
        Parser parser;
        const auto epochs = parser.feed(epochOf(kMaxEpochSentences));
        require(epochs.size() == 1 && epochs[0].sentences.size() == kMaxEpochSentences);
        require(epochs[0].position.fix_class == FixClass::precise);
        require(parser.overflowed() == 0);
    }
    {
        // One sentence more drops the epoch in progress, and $PKRNF starts a new, empty one.
        Parser parser;
        require(parser.feed(epochOf(kMaxEpochSentences + 1)).empty());
        require(parser.overflowed() == 1);
    }
    {
        // A stream of sentences that never end an epoch holds a bounded number.
        Parser parser;
        std::string stream;
        for (int count = 0; count < 5000; ++count)
            stream += filler;
        require(parser.feed(stream).empty());
        require(parser.overflowed() == (5000 - 1) / kMaxEpochSentences, "dropped each time the epoch filled");
    }
    {
        // After the junk, a whole epoch comes through whatever the bound dropped.
        Parser parser;
        std::string stream;
        for (int count = 0; count < 100; ++count)
            stream += filler;
        stream += uml::gnss::nmeaEpoch(preciseFix(), preciseTime);
        const auto epochs = parser.feed(stream);
        require(epochs.size() == 1, "the stream recovers");
        require(epochs[0].position.fix_class == FixClass::precise);
        require(near(epochs[0].position.latitude, 35.6895, 1e-5));
        require(epochs[0].sentences.size() <= kMaxEpochSentences);
    }
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"round-trips-a-precise-fix", roundTripsAPreciseFix},
        {"round-trips-what-the-host-does-not-know", roundTripsWhatTheHostDoesNotKnow},
        {"round-trips-an-approximate-fix", roundTripsAnApproximateFix},
        {"round-trips-every-hemisphere", roundTripsEveryHemisphereAndTheDateLine},
        {"round-trips-across-the-calendar", roundTripsAcrossTheCalendar},
        {"round-trips-a-synthetic-corpus", roundTripsASyntheticCorpus},
        {"reads-no-fix-at-the-current-time", readsNoFixAtTheCurrentTime},
        {"reads-any-chunking", readsAnyChunking},
        {"drops-a-sentence-with-a-bad-checksum", dropsASentenceWithABadChecksum},
        {"skips-garbage-and-long-lines", skipsGarbageAndLongLines},
        {"drops-an-epoch-that-began-before-the-read", startsMidEpochAtTheNextOne},
        {"ends-an-epoch-at-the-next-gga", endsAnEpochAtTheNextGgaWithoutPkrnf},
        {"takes-the-less-precise-class", takesTheLessPreciseClass},
        {"refuses-what-does-not-parse", refusesWhatDoesNotParse},
        {"reads-numbers-and-times", readsNumbersAndTimes},
        {"checksums-match-the-writer", checksumsMatchTheWriter},
        {"bounds-an-epoch-that-never-ends", boundsAnEpochThatNeverEnds},
    };
    return uml::testing::runNamedTests(tests);
}
