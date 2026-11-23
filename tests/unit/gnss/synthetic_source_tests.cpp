// SPDX-License-Identifier: GPL-2.0-or-later

#include "named_tests.hpp"
#include "require.hpp"
#include "synthetic_source.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

using uml::gnss::Fix;
using uml::gnss::FixSource;
using uml::testing::require;

constexpr std::int64_t second = 1'000'000'000;
constexpr std::int64_t start = 1'794'318'330 * second;

std::unique_ptr<FixSource> make(const std::string& specification)
{
    std::string error;
    auto source = uml::gnss::makeSyntheticSource(specification, error);
    require((source != nullptr) == error.empty());
    return source;
}

bool refused(const std::string& specification)
{
    std::string error;
    const auto source = uml::gnss::makeSyntheticSource(specification, error);
    require(source == nullptr && !error.empty(), "an error is given");
    return true;
}

void fixedDatesItsFixWithTheQueryTime()
{
    auto source = make("fixed:35.5,139.5");
    auto fix = source->latest(start);
    require(fix && !fix->approximate);
    require(fix->latitude == 35.5 && fix->longitude == 139.5);
    require(fix->utcNs == start);
    require(fix->horizontalAccuracy == 5.0);
    require(!fix->altitude && !fix->verticalAccuracy && !fix->speed && !fix->course);
    fix = source->latest(start + 3 * second);
    require(fix->utcNs == start + 3 * second);
}

void fixedTakesAnAltitudeAndAnAccuracy()
{
    auto source = make("fixed: -33.5 , 151.25 , 100.5 , 7.5");
    const auto fix = source->latest(start);
    require(fix->latitude == -33.5 && fix->longitude == 151.25);
    require(fix->altitude == 100.5 && fix->ellipsoidalAltitude == 100.5);
    require(fix->horizontalAccuracy == 7.5);
    require(fix->verticalAccuracy == 10.0);
}

void approximateIsMarkedAndCoarse()
{
    auto source = make("approximate:48.85,2.35");
    const auto fix = source->latest(start);
    require(fix->approximate);
    require(fix->horizontalAccuracy == 5000.0);
    require(!fix->altitude && !fix->speed && !fix->course);
}

void refusesWhatItDoesNotUnderstand()
{
    for (const char* bad : {
             "", "fixed", "fixed:", "fixed:1", "fixed:1,2,3,4,5", "fixed:91,0",
             "fixed:-91,0", "fixed:0,181", "fixed:0,-181", "fixed:1,2,3,0",
             "fixed:1,2,3,-4", "fixed:abc,1", "fixed:1,nan", "fixed:1,inf",
             "fixed:1,2x", "approximate:1", "approximate:1,2,3", "nowhere:1,2",
             "route:", "route:/nonexistent/route.csv"})
        require(refused(bad));
}

void parsesARoute()
{
    std::string error;
    const auto points = uml::gnss::parseRoute(
        "# a walk\n\n 0,35.0,139.0\r\n10, 35.0 ,139.001, 12.5\n20,35.0,139.002,12.5,3\n",
        error);
    require(error.empty() && points.size() == 3);
    require(points[0].seconds == 0.0 && !points[0].altitude);
    require(points[1].seconds == 10.0 && points[1].longitude == 139.001);
    require(points[1].altitude == 12.5 && !points[1].accuracy);
    require(points[2].accuracy == 3.0);
}

void refusesABadRoute()
{
    for (const char* bad : {
             "", "# nothing\n", "0,1\n", "0,1,2,3,4,5\n", "x,1,2\n", "-1,1,2\n",
             "0,91,2\n", "0,1,181\n", "5,1,2\n4,1,2\n", "0,1,2,3,0\n"}) {
        std::string error;
        require(uml::gnss::parseRoute(bad, error).empty());
        require(!error.empty());
    }
}

void movesAlongARouteAndHoldsAtTheEnd()
{
    std::string error;
    auto source = uml::gnss::makeRouteSource(
        uml::gnss::parseRoute("0,0,0\n10,0,0.0001\n20,0,0.0001\n", error));
    require(source != nullptr);
    source->powerChanged(true, start);

    auto fix = source->latest(start);
    require(fix->latitude == 0.0 && fix->longitude == 0.0);

    fix = source->latest(start + 5 * second);
    require(std::fabs(fix->longitude - 0.00005) < 1e-9);
    require(fix->speed && std::fabs(*fix->speed - 1.11195) < 0.01);
    require(fix->course && std::fabs(*fix->course - 90.0) < 0.01);
    require(fix->utcNs == start + 5 * second);

    // The second leg stands still.
    fix = source->latest(start + 15 * second);
    require(fix->longitude == 0.0001 && !fix->speed && !fix->course);

    // Past the last point it stays there.
    fix = source->latest(start + 600 * second);
    require(fix->longitude == 0.0001 && !fix->speed);

    // A new power-on starts the route again.
    source->powerChanged(false, start + 700 * second);
    source->powerChanged(true, start + 800 * second);
    fix = source->latest(start + 801 * second);
    require(fix->longitude < 0.00002);
}

void followsTheCourseOfEachLeg()
{
    std::string error;
    auto source = uml::gnss::makeRouteSource(uml::gnss::parseRoute(
        "0,10,20\n10,10.0001,20\n20,10.0001,19.9999\n", error));
    source->powerChanged(true, start);
    auto fix = source->latest(start + 5 * second);
    require(std::fabs(*fix->course - 0.0) < 0.01 ||
            std::fabs(*fix->course - 360.0) < 0.01);
    fix = source->latest(start + 15 * second);
    require(std::fabs(*fix->course - 270.0) < 0.1);
}

void readsARouteFile()
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto path = directory / ("gnss-route-" + std::to_string(::getpid()) + ".csv");
    {
        std::ofstream file(path);
        file << "0,40,-74\n10,40.001,-74\n";
    }
    std::string error;
    auto source = uml::gnss::makeSyntheticSource("route:" + path.string(), error);
    std::filesystem::remove(path);
    require(source != nullptr && error.empty());
    source->powerChanged(true, start);
    const auto fix = source->latest(start + 20 * second);
    require(fix->latitude == 40.001 && fix->longitude == -74.0);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"fixed-dates-its-fix-with-the-query-time", fixedDatesItsFixWithTheQueryTime},
        {"fixed-takes-an-altitude-and-an-accuracy", fixedTakesAnAltitudeAndAnAccuracy},
        {"approximate-is-marked-and-coarse", approximateIsMarkedAndCoarse},
        {"refuses-what-it-does-not-understand", refusesWhatItDoesNotUnderstand},
        {"parses-a-route", parsesARoute},
        {"refuses-a-bad-route", refusesABadRoute},
        {"moves-along-a-route-and-holds-at-the-end", movesAlongARouteAndHoldsAtTheEnd},
        {"follows-the-course-of-each-leg", followsTheCourseOfEachLeg},
        {"reads-a-route-file", readsARouteFile},
    };
    return uml::testing::runNamedTests(tests);
}
