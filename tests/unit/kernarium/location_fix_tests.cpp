// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumLocation.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cerrno>
#include <cmath>
#include <limits>

namespace {

using kernarium::location::toFix;
using uml::gnss::Fix;
using uml::testing::require;

kernarium_runtime_location_fix valid()
{
    kernarium_runtime_location_fix fix {};
    fix.struct_size = sizeof(fix);
    fix.utc_ns = 1'794'318'330'250'000'000;
    fix.latitude = 35.6895;
    fix.longitude = 139.6917;
    return fix;
}

void carriesTheRequiredFieldsAlone()
{
    Fix fix;
    require(toFix(valid(), fix) == 0);
    require(fix.utcNs == 1'794'318'330'250'000'000);
    require(fix.latitude == 35.6895 && fix.longitude == 139.6917);
    require(!fix.approximate);
    // A field without its flag is absent, whatever the struct holds.
    auto source = valid();
    source.altitude = 12.0;
    source.speed = 3.0;
    source.course = 90.0;
    source.horizontal_accuracy = 5.0;
    require(toFix(source, fix) == 0);
    require(!fix.altitude && !fix.speed && !fix.course && !fix.horizontalAccuracy);
}

void carriesEveryFlaggedField()
{
    auto source = valid();
    source.flags = KERNARIUM_RUNTIME_LOCATION_APPROXIMATE |
        KERNARIUM_RUNTIME_LOCATION_HAS_ALTITUDE |
        KERNARIUM_RUNTIME_LOCATION_HAS_ELLIPSOIDAL_ALTITUDE |
        KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY |
        KERNARIUM_RUNTIME_LOCATION_HAS_VERTICAL_ACCURACY |
        KERNARIUM_RUNTIME_LOCATION_HAS_SPEED |
        KERNARIUM_RUNTIME_LOCATION_HAS_SPEED_ACCURACY |
        KERNARIUM_RUNTIME_LOCATION_HAS_COURSE |
        KERNARIUM_RUNTIME_LOCATION_HAS_COURSE_ACCURACY;
    source.altitude = 40.0;
    source.ellipsoidal_altitude = 80.5;
    source.horizontal_accuracy = 8.0;
    source.vertical_accuracy = 12.0;
    source.speed = 1.5;
    source.speed_accuracy = 0.5;
    source.course = 90.0;
    source.course_accuracy = 10.0;
    Fix fix;
    require(toFix(source, fix) == 0);
    require(fix.approximate);
    require(fix.altitude == 40.0 && fix.ellipsoidalAltitude == 80.5);
    require(fix.horizontalAccuracy == 8.0 && fix.verticalAccuracy == 12.0);
    require(fix.speed == 1.5 && fix.speedAccuracy == 0.5);
    require(fix.course == 90.0 && fix.courseAccuracy == 10.0);
}

void leavesOutAFlaggedFieldThatCannotBeTrue()
{
    constexpr auto flags = KERNARIUM_RUNTIME_LOCATION_HAS_ALTITUDE |
        KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY |
        KERNARIUM_RUNTIME_LOCATION_HAS_VERTICAL_ACCURACY |
        KERNARIUM_RUNTIME_LOCATION_HAS_SPEED |
        KERNARIUM_RUNTIME_LOCATION_HAS_SPEED_ACCURACY |
        KERNARIUM_RUNTIME_LOCATION_HAS_COURSE |
        KERNARIUM_RUNTIME_LOCATION_HAS_COURSE_ACCURACY;
    auto source = valid();
    source.flags = flags;
    source.altitude = std::numeric_limits<double>::quiet_NaN();
    source.horizontal_accuracy = -1.0;
    source.vertical_accuracy = -1.0;
    source.speed = -1.0;
    source.speed_accuracy = -1.0;
    source.course = -1.0;
    source.course_accuracy = -1.0;
    Fix fix;
    require(toFix(source, fix) == 0);
    require(!fix.altitude && !fix.horizontalAccuracy && !fix.verticalAccuracy);
    require(!fix.speed && !fix.speedAccuracy && !fix.course && !fix.courseAccuracy);
    // A course of exactly 360 is not a course; zero is.
    source.course = 360.0;
    require(toFix(source, fix) == 0 && !fix.course);
    source.course = 0.0;
    source.speed = 0.0;
    require(toFix(source, fix) == 0 && fix.course == 0.0 && fix.speed == 0.0);
}

void refusesWhatItCannotUse()
{
    Fix fix;
    auto source = valid();
    source.struct_size = sizeof(source) - 1;
    require(toFix(source, fix) == -EINVAL);
    source = valid();
    source.utc_ns = 0;
    require(toFix(source, fix) == -EINVAL);
    source = valid();
    source.latitude = 90.5;
    require(toFix(source, fix) == -EINVAL);
    source = valid();
    source.longitude = -180.5;
    require(toFix(source, fix) == -EINVAL);
    source = valid();
    source.latitude = std::numeric_limits<double>::infinity();
    require(toFix(source, fix) == -EINVAL);
    source = valid();
    source.longitude = std::nan("");
    require(toFix(source, fix) == -EINVAL);
    // The edges of the globe are positions.
    source = valid();
    source.latitude = -90.0;
    source.longitude = 180.0;
    require(toFix(source, fix) == 0);
}

void acceptsAStructFromALaterVersion()
{
    struct Later {
        kernarium_runtime_location_fix fix;
        double extra;
    } later {};
    later.fix = valid();
    later.fix.struct_size = sizeof(later);
    Fix fix;
    require(toFix(later.fix, fix) == 0);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"carries-the-required-fields-alone", carriesTheRequiredFieldsAlone},
        {"carries-every-flagged-field", carriesEveryFlaggedField},
        {"leaves-out-a-flagged-field-that-cannot-be-true", leavesOutAFlaggedFieldThatCannotBeTrue},
        {"refuses-what-it-cannot-use", refusesWhatItCannotUse},
        {"accepts-a-struct-from-a-later-version", acceptsAStructFromALaterVersion},
    };
    return uml::testing::runNamedTests(tests);
}
