// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the guest GNSS HAL lets the framework know (the overlay's
// location_policy.hpp): the Q1 rule for an approximate fix, whatever the
// stream carries, on the location and on the NMEA sentences; the fields a
// precise fix keeps; the age; and which epochs become reports. The inputs are
// the host writer's own output. This test is GPL-2.0-or-later and includes the
// overlay's Apache-2.0 headers; the binary is built and run by the test suite
// only and is never distributed.

#include "location_policy.hpp"
#include "named_tests.hpp"
#include "nmea.hpp"
#include "nmea_writer.hpp"
#include "require.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using andarium::gnss::ReportPacer;
using andarium::gnss::make_location;
using andarium::gnss::listener_sentences;
namespace nmea = andarium::gnss::nmea;
using uml::gnss::Fix;
using uml::testing::require;

// 2026-11-10 13:45:30.250 UTC.
constexpr std::int64_t fixNs = 1'794'318'330'250'000'000;
constexpr std::int64_t fixMs = 1'794'318'330'250;
constexpr std::int64_t second = 1'000'000'000;

Fix preciseFix()
{
    Fix fix;
    fix.utcNs = fixNs;
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

/* An approximate fix that carries every field a precise one would, as a
 * careless source could make: the HAL must hold the line whatever arrives. */
Fix loadedApproximateFix(double accuracy)
{
    Fix fix = preciseFix();
    fix.approximate = true;
    fix.horizontalAccuracy = accuracy;
    return fix;
}

nmea::Epoch read(const Fix& fix)
{
    nmea::Parser parser;
    auto epochs = parser.feed(uml::gnss::nmeaEpoch(fix, fix.utcNs));
    require(epochs.size() == 1);
    return epochs[0];
}

bool has(const andarium::gnss::Location& location, std::int32_t flag)
{
    return (location.flags & flag) != 0;
}

bool near(double value, double expected, double tolerance)
{
    return std::fabs(value - expected) <= tolerance;
}

constexpr std::int64_t uptime = 3'600 * second;

void reportsAPreciseFixWithWhatTheHostKnew()
{
    const auto epoch = read(preciseFix());
    // The fix is half a second old by the guest's clock.
    const auto location = make_location(epoch.position, fixNs + second / 2, uptime);
    require(location.has_value());
    using namespace andarium::gnss;
    require(location->flags ==
            (kHasLatLong | kHasAltitude | kHasSpeed | kHasBearing | kHasHorizontalAccuracy |
             kHasVerticalAccuracy | kHasSpeedAccuracy | kHasBearingAccuracy));
    require(near(location->latitude, 35.6895, 1e-8));
    require(near(location->longitude, 139.6917, 1e-8));
    // The ellipsoidal altitude: 40.0 above sea level plus the separation 40.5.
    require(near(location->altitude, 80.5, 1e-9));
    require(near(location->speed, 1.5, 0.006));
    require(near(location->bearing, 90.0, 0.051));
    require(near(location->horizontal_accuracy, 8.0, 1e-9));
    require(near(location->vertical_accuracy, 12.0, 1e-9));
    require(near(location->speed_accuracy, 0.5, 1e-9));
    require(near(location->bearing_accuracy, 10.0, 1e-9));
    require(location->timestamp_ms == fixMs);
    require(location->elapsed_flags == (kHasTimestampNs | kHasTimeUncertaintyNs));
    require(location->elapsed_realtime_ns == uptime - second / 2);
    require(location->elapsed_uncertainty_ns > 0);
}

void leavesOutWhatThePreciseFixLacked()
{
    Fix fix = preciseFix();
    fix.altitude.reset();
    fix.ellipsoidalAltitude.reset();
    fix.verticalAccuracy.reset();
    fix.speed.reset();
    fix.speedAccuracy.reset();
    fix.course.reset();
    fix.courseAccuracy.reset();
    const auto location = make_location(read(fix).position, fixNs, uptime);
    require(location.has_value());
    require(location->flags == (andarium::gnss::kHasLatLong | andarium::gnss::kHasHorizontalAccuracy));
    // A fix with a speed and no accuracy for it keeps the speed alone.
    fix = preciseFix();
    fix.speedAccuracy.reset();
    fix.courseAccuracy.reset();
    const auto partial = make_location(read(fix).position, fixNs, uptime);
    require(partial && has(*partial, andarium::gnss::kHasSpeed) && !has(*partial, andarium::gnss::kHasSpeedAccuracy));
    require(has(*partial, andarium::gnss::kHasBearing) && !has(*partial, andarium::gnss::kHasBearingAccuracy));
}

void doesNotGuessTheAltitudeDatum()
{
    // Android's altitude is above the ellipsoid. With only the sea level altitude
    // (or only the separation) the datum would be wrong, so there is no altitude.
    Fix sea = preciseFix();
    sea.ellipsoidalAltitude.reset();
    const auto seaLocation = make_location(read(sea).position, fixNs, uptime);
    require(seaLocation && !has(*seaLocation, andarium::gnss::kHasAltitude));
    require(!has(*seaLocation, andarium::gnss::kHasVerticalAccuracy));
    Fix ellipsoid = preciseFix();
    ellipsoid.altitude.reset();
    const auto ellipsoidLocation = make_location(read(ellipsoid).position, fixNs, uptime);
    require(ellipsoidLocation && !has(*ellipsoidLocation, andarium::gnss::kHasAltitude));
}

void refusesAPreciseFixWithoutAnAccuracy()
{
    // The framework drops a location without one, so a fix the host could not
    // bound is not reported as if it were known.
    Fix fix = preciseFix();
    fix.horizontalAccuracy.reset();
    require(!make_location(read(fix).position, fixNs, uptime));
    nmea::Position broken = read(preciseFix()).position;
    broken.horizontal_accuracy = -1.0;
    require(!make_location(broken, fixNs, uptime));
    broken.horizontal_accuracy = std::nan("");
    require(!make_location(broken, fixNs, uptime));
}

void reportsNothingWithoutAFix()
{
    nmea::Parser parser;
    const auto epochs = parser.feed(uml::gnss::nmeaEpoch(std::nullopt, fixNs));
    require(epochs.size() == 1);
    require(!make_location(epochs[0].position, fixNs, uptime));
}

void holdsTheApproximateLine()
{
    // Q1: at least 2 km uncertain, no altitude, speed or bearing, and no
    // accuracy for what is not there, whatever the fix carried.
    using namespace andarium::gnss;
    for (const double accuracy : {5000.0, 2000.0, 1999.9, 100.0, 1.0}) {
        const auto location = make_location(read(loadedApproximateFix(accuracy)).position, fixNs, uptime);
        require(location.has_value());
        require(location->flags == (kHasLatLong | kHasHorizontalAccuracy), "only the position and its accuracy");
        require(location->horizontal_accuracy >= kApproximateAccuracyFloorM);
        require(near(location->horizontal_accuracy, std::max(accuracy, 2000.0), 0.051));
        require(location->altitude == 0 && location->speed == 0 && location->bearing == 0);
        require(location->vertical_accuracy == 0 && location->speed_accuracy == 0 && location->bearing_accuracy == 0);
    }
    // A host that gave no accuracy at all still gets the floor.
    Fix none = loadedApproximateFix(0);
    none.horizontalAccuracy.reset();
    const auto unbounded = make_location(read(none).position, fixNs, uptime);
    require(unbounded && unbounded->horizontal_accuracy == kApproximateAccuracyFloorM);
}

void ageIsHowFarTheFixLiesBehindTheClock()
{
    const auto position = read(preciseFix()).position;
    // Delivered at once, ten seconds later, and from the future.
    require(make_location(position, fixNs, uptime)->elapsed_realtime_ns == uptime - 0);
    require(make_location(position, fixNs + 10 * second, uptime)->elapsed_realtime_ns == uptime - 10 * second);
    require(make_location(position, fixNs - 5 * second, uptime)->elapsed_realtime_ns == uptime);
    // The fix is older than the guest: no field is zero, and none is negative.
    require(make_location(position, fixNs + 2 * uptime, uptime)->elapsed_realtime_ns == 1);
}

std::vector<nmea::Epoch> readEpochs(const std::vector<std::string>& sentences)
{
    std::string text;
    for (const auto& sentence : sentences)
        text += sentence;
    nmea::Parser parser;
    return parser.feed(text);
}

void passesAPreciseAndANoFixEpochAsWritten()
{
    const auto precise = read(preciseFix());
    require(listener_sentences(precise) == precise.sentences);
    nmea::Parser parser;
    const auto noFix = parser.feed(uml::gnss::nmeaEpoch(std::nullopt, fixNs));
    require(noFix.size() == 1);
    require(listener_sentences(noFix[0]) == noFix[0].sentences);
}

void holdsTheApproximateLineInTheSentences()
{
    using namespace andarium::gnss;
    const auto epoch = read(loadedApproximateFix(100.0));
    const auto sentences = listener_sentences(epoch);
    require(sentences.size() == 6);
    for (const auto& sentence : sentences)
        require(nmea::sentence_body(sentence).has_value(), "every rewritten sentence has a valid checksum");
    // Read back, they say the same as the location: an approximate position and
    // nothing that betrays precision.
    const auto epochs = readEpochs(sentences);
    require(epochs.size() == 1);
    const auto& position = epochs[0].position;
    require(position.fix_class == nmea::FixClass::approximate);
    require(near(position.latitude, 35.6895, 1e-8) && near(position.longitude, 139.6917, 1e-8));
    require(!position.altitude && !position.geoid_separation && !position.speed && !position.course);
    require(position.horizontal_accuracy && *position.horizontal_accuracy >= kApproximateAccuracyFloorM);
    require(!position.vertical_accuracy && !position.speed_accuracy && !position.course_accuracy);
    require(epochs[0].utc_ms == fixMs);
    // And as text, from an independent script: no altitude, speed, course or
    // vertical error, the kind named approximate, every error at the floor.
    const std::string expected =
        "$GNGGA,134530.25,3541.3700000,N,13941.5020000,E,6,,,,,,,,*62\r\n"
        "$GNRMC,134530.25,A,3541.3700000,N,13941.5020000,E,,,101126,,,E*48\r\n"
        "$GNGSA,A,2,,,,,,,,,,,,,,,*03\r\n"
        "$GNGST,134530.25,,1414.22,1414.22,0.0,1414.22,1414.22,*4E\r\n"
        "$GNVTG,,T,,M,,N,,K,E*39\r\n"
        "$PKRNF,A,2000.0,,,*30\r\n";
    std::string joined;
    for (const auto& sentence : sentences)
        joined += sentence;
    require(joined == expected);
}

void raisesAnAlreadyHighAccuracyNotAtAll()
{
    // iOS's own 5 km stays 5 km; the floor only raises.
    const auto epoch = read(loadedApproximateFix(5000.0));
    const auto epochs = readEpochs(listener_sentences(epoch));
    require(epochs.size() == 1);
    require(near(*epochs[0].position.horizontal_accuracy, 5000.0, 0.051));
}

void dropsASentenceItCannotVouchFor()
{
    const auto epoch = read(loadedApproximateFix(5000.0));
    auto extended = epoch;
    extended.sentences.push_back(nmea::sentence_text("GNGSV,1,1,00"));
    extended.sentences.push_back(nmea::sentence_text("PXYZ,1,2,3"));
    const auto kept = listener_sentences(extended);
    require(kept.size() == 6, "the two unknown sentences are left out");
    for (const auto& sentence : kept) {
        require(sentence.find("GSV") == std::string::npos && sentence.find("PXYZ") == std::string::npos);
    }
}

void namesTheApproximateKindWhereverASentenceDoes()
{
    // GGA quality 1 and RMC mode A with PKRNF class A: the epoch is approximate,
    // and the rewritten sentences say so everywhere.
    const auto precise = read(preciseFix());
    auto sentences = precise.sentences;
    sentences.back() = nmea::sentence_text("PKRNF,A,8.0,12.0,0.50,10.0");
    const auto epochs = readEpochs(sentences);
    require(epochs.size() == 1 && epochs[0].position.fix_class == nmea::FixClass::approximate);
    const auto rewritten = listener_sentences(epochs[0]);
    require(rewritten.size() == 6);
    const auto after = readEpochs(rewritten);
    require(after.size() == 1);
    require(after[0].position.fix_class == nmea::FixClass::approximate);
    require(rewritten[0].find(",6,") != std::string::npos, "GGA quality 6");
    require(rewritten[1].find(",E*") != std::string::npos, "RMC mode E");
    require(rewritten[2].find("GNGSA,A,2,") != std::string::npos, "GSA fix type 2");
    require(rewritten[4].find(",E*") != std::string::npos, "VTG mode E");
    require(rewritten[5].find("PKRNF,A,2000.0,") != std::string::npos);
}

void reportsEachNewFixOnceAtTheInterval()
{
    ReportPacer pacer;
    // The first fix is reported; a repeat of it (the host repeats a held fix with
    // its own time) is not.
    require(pacer.admit(1000, 10 * second, 1000));
    require(!pacer.admit(1000, 11 * second, 1000));
    require(!pacer.admit(1000, 12 * second, 1000));
    // A newer fix a period later, with the jitter of the host's epochs.
    require(pacer.admit(2000, 11 * second + 50'000'000, 1000));
    // A newer fix too soon waits; the same fix is then reported at the next tick.
    require(!pacer.admit(2500, 11 * second + 600'000'000, 1000));
    require(pacer.admit(2500, 12 * second + 100'000'000, 1000));
    // An older fix is never reported.
    require(!pacer.admit(2400, 20 * second, 1000));
    // A slower interval.
    require(pacer.admit(5000, 20 * second, 5000));
    require(!pacer.admit(6000, 22 * second, 5000));
    require(pacer.admit(7000, 25 * second, 5000));
    // A new session forgets all of it.
    pacer.reset();
    require(pacer.admit(100, 0, 1000));
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"reports-a-precise-fix-with-what-the-host-knew", reportsAPreciseFixWithWhatTheHostKnew},
        {"leaves-out-what-the-precise-fix-lacked", leavesOutWhatThePreciseFixLacked},
        {"does-not-guess-the-altitude-datum", doesNotGuessTheAltitudeDatum},
        {"refuses-a-precise-fix-without-an-accuracy", refusesAPreciseFixWithoutAnAccuracy},
        {"reports-nothing-without-a-fix", reportsNothingWithoutAFix},
        {"holds-the-approximate-line", holdsTheApproximateLine},
        {"age-is-how-far-the-fix-lies-behind-the-clock", ageIsHowFarTheFixLiesBehindTheClock},
        {"passes-precise-and-no-fix-sentences", passesAPreciseAndANoFixEpochAsWritten},
        {"holds-the-approximate-line-in-the-sentences", holdsTheApproximateLineInTheSentences},
        {"does-not-lower-a-high-accuracy", raisesAnAlreadyHighAccuracyNotAtAll},
        {"drops-a-sentence-it-cannot-vouch-for", dropsASentenceItCannotVouchFor},
        {"names-the-approximate-kind-everywhere", namesTheApproximateKindWhereverASentenceDoes},
        {"reports-each-new-fix-once-at-the-interval", reportsEachNewFixOnceAtTheInterval},
    };
    return uml::testing::runNamedTests(tests);
}
