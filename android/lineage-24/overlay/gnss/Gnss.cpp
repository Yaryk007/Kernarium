// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.gnss"

#include "Gnss.hpp"

#include <android/binder_status.h>
#include <log/log.h>

#include <algorithm>
#include <string>
#include <utility>

namespace aidl::android::hardware::gnss {

namespace {

using andarium::gnss::Engine;
using andarium::gnss::Status;

// The policy header carries the AIDL's flag values as its own, so that a host
// test can compile it. The AIDL is frozen at version 7; these catch a change.
static_assert(andarium::gnss::kHasLatLong == GnssLocation::HAS_LAT_LONG);
static_assert(andarium::gnss::kHasAltitude == GnssLocation::HAS_ALTITUDE);
static_assert(andarium::gnss::kHasSpeed == GnssLocation::HAS_SPEED);
static_assert(andarium::gnss::kHasBearing == GnssLocation::HAS_BEARING);
static_assert(andarium::gnss::kHasHorizontalAccuracy == GnssLocation::HAS_HORIZONTAL_ACCURACY);
static_assert(andarium::gnss::kHasVerticalAccuracy == GnssLocation::HAS_VERTICAL_ACCURACY);
static_assert(andarium::gnss::kHasSpeedAccuracy == GnssLocation::HAS_SPEED_ACCURACY);
static_assert(andarium::gnss::kHasBearingAccuracy == GnssLocation::HAS_BEARING_ACCURACY);
static_assert(andarium::gnss::kHasTimestampNs == ElapsedRealtime::HAS_TIMESTAMP_NS);
static_assert(andarium::gnss::kHasTimeUncertaintyNs == ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS);
static_assert(static_cast<int32_t>(Status::session_begin) ==
              static_cast<int32_t>(IGnssCallback::GnssStatusValue::SESSION_BEGIN));
static_assert(static_cast<int32_t>(Status::session_end) ==
              static_cast<int32_t>(IGnssCallback::GnssStatusValue::SESSION_END));
static_assert(static_cast<int32_t>(Status::engine_on) ==
              static_cast<int32_t>(IGnssCallback::GnssStatusValue::ENGINE_ON));
static_assert(static_cast<int32_t>(Status::engine_off) ==
              static_cast<int32_t>(IGnssCallback::GnssStatusValue::ENGINE_OFF));

// The year the virtual hardware is dated to and the name Android shows for it
// (getGnssHardwareModelName needs more than three characters).
constexpr int32_t kYearOfHardware = 2026;
constexpr char kHardwareName[] = "Andarium virtual GNSS";

ndk::ScopedAStatus unsupported(const char* extension) {
    ALOGV("no %s", extension);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

GnssLocation to_aidl(const andarium::gnss::Location& location) {
    GnssLocation out;
    out.gnssLocationFlags = location.flags;
    out.latitudeDegrees = location.latitude;
    out.longitudeDegrees = location.longitude;
    out.altitudeMeters = location.altitude;
    out.speedMetersPerSec = location.speed;
    out.bearingDegrees = location.bearing;
    out.horizontalAccuracyMeters = location.horizontal_accuracy;
    out.verticalAccuracyMeters = location.vertical_accuracy;
    out.speedAccuracyMetersPerSecond = location.speed_accuracy;
    out.bearingAccuracyDegrees = location.bearing_accuracy;
    out.timestampMillis = location.timestamp_ms;
    out.elapsedRealtime.flags = location.elapsed_flags;
    out.elapsedRealtime.timestampNs = location.elapsed_realtime_ns;
    out.elapsedRealtime.timeUncertaintyNs = location.elapsed_uncertainty_ns;
    return out;
}

void check(const ndk::ScopedAStatus& status, const char* call) {
    if (!status.isOk()) ALOGE("%s failed: %s", call, status.getDescription().c_str());
}

}  // namespace

ndk::ScopedAStatus GnssDebug::getDebugData(DebugData* debug_data) {
    const int64_t realtime = realtime_ns_();
    debug_data->time.timeEstimateMs = realtime / 1'000'000;
    debug_data->time.timeUncertaintyNs = static_cast<float>(andarium::gnss::kTimeUncertaintyNs);
    debug_data->time.frequencyUncertaintyNsPerSec = 0.0f;
    debug_data->satelliteDataArray.clear();
    debug_data->position = PositionDebug();
    const auto fix = last_fix_();
    if (!fix) return ndk::ScopedAStatus::ok();
    const auto& position = fix->position;
    PositionDebug& out = debug_data->position;
    out.valid = true;
    out.latitudeDegrees = position.latitude;
    out.longitudeDegrees = position.longitude;
    // What the framework would see, not what the stream carried.
    const auto location = andarium::gnss::make_location(position, realtime, boottime_ns_());
    if (location) {
        out.altitudeMeters = static_cast<float>(location->altitude);
        out.speedMetersPerSec = static_cast<float>(location->speed);
        out.bearingDegrees = static_cast<float>(location->bearing);
        out.horizontalAccuracyMeters = location->horizontal_accuracy;
        out.verticalAccuracyMeters = location->vertical_accuracy;
        out.speedAccuracyMetersPerSecond = location->speed_accuracy;
        out.bearingAccuracyDegrees = location->bearing_accuracy;
    }
    out.ageSeconds = static_cast<float>(std::max<int64_t>(boottime_ns_() - fix->boottime_ns, 0)) / 1e9f;
    return ndk::ScopedAStatus::ok();
}

Gnss::Gnss(andarium::gnss::Env env) : env_(env), hal_(std::move(env)) {}

Gnss::~Gnss() = default;

// The HAL's reporter thread makes these calls, one at a time and in order, with
// no lock of the HAL held (gnss_reporter.hpp); no entry point of Gnss does.
andarium::gnss::Sink Gnss::sink_for(const std::shared_ptr<IGnssCallback>& callback) const {
    andarium::gnss::Sink sink;
    // Positions only: no scheduling, no measurements, no geofencing, nothing
    // that needs a satellite.
    sink.ready = [callback] {
        check(callback->gnssSetCapabilitiesCb(0), "gnssSetCapabilitiesCb");
        IGnssCallback::GnssSystemInfo info;
        info.yearOfHw = kYearOfHardware;
        info.name = kHardwareName;
        check(callback->gnssSetSystemInfoCb(info), "gnssSetSystemInfoCb");
    };
    sink.status = [callback](Status status) {
        check(callback->gnssStatusCb(static_cast<IGnssCallback::GnssStatusValue>(status)), "gnssStatusCb");
    };
    sink.location = [callback](const andarium::gnss::Location& location) {
        check(callback->gnssLocationCb(to_aidl(location)), "gnssLocationCb");
    };
    sink.nmea = [callback](int64_t utc_ms, const std::string& sentence) {
        check(callback->gnssNmeaCb(utc_ms, sentence), "gnssNmeaCb");
    };
    sink.sv_status = [callback]() { check(callback->gnssSvStatusCb({}), "gnssSvStatusCb"); };
    return sink;
}

ndk::ScopedAStatus Gnss::setCallback(const std::shared_ptr<IGnssCallback>& callback) {
    if (callback == nullptr) {
        ALOGE("setCallback: null callback");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    // A setCallback that follows another without a close replaces its session.
    hal_.set_callback(sink_for(callback));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::start() {
    switch (hal_.start()) {
        case andarium::gnss::Hal::StartResult::started:
            return ndk::ScopedAStatus::ok();
        case andarium::gnss::Hal::StartResult::no_callback:
            ALOGE("start before setCallback");
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        case andarium::gnss::Hal::StartResult::no_receiver:
            break;
    }
    return ndk::ScopedAStatus::fromServiceSpecificError(ERROR_GENERIC);
}

ndk::ScopedAStatus Gnss::stop() {
    hal_.stop();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::close() {
    hal_.close();
    return ndk::ScopedAStatus::ok();
}

// The receiver is the only source of time and position: what the framework
// injects (network time, a coarse location, a best location) is not used, and
// deleting aiding data has nothing to delete.
ndk::ScopedAStatus Gnss::injectTime(int64_t, int64_t, int32_t) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus Gnss::injectLocation(const GnssLocation&) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus Gnss::injectBestLocation(const GnssLocation&) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus Gnss::deleteAidingData(GnssAidingData) { return ndk::ScopedAStatus::ok(); }

ndk::ScopedAStatus Gnss::setPositionMode(const PositionModeOptions& options) {
    if (!hal_.set_position_mode(options.minIntervalMs)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::startSvStatus() {
    hal_.set_sv_status(true);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::stopSvStatus() {
    hal_.set_sv_status(false);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::startNmea() {
    hal_.set_nmea(true);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::stopNmea() {
    hal_.set_nmea(false);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionGnssConfiguration(std::shared_ptr<IGnssConfiguration>* result) {
    *result = ndk::SharedRefBase::make<GnssConfiguration>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* result) {
    *result = ndk::SharedRefBase::make<GnssDebug>([this] { return hal_.last_fix(); }, env_.realtime_ns,
                                                  env_.boottime_ns);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionPsds(std::shared_ptr<IGnssPsds>*) { return unsupported("psds"); }
ndk::ScopedAStatus Gnss::getExtensionGnssPowerIndication(std::shared_ptr<IGnssPowerIndication>*) {
    return unsupported("power indication");
}
ndk::ScopedAStatus Gnss::getExtensionGnssMeasurement(std::shared_ptr<IGnssMeasurementInterface>*) {
    return unsupported("measurements");
}
ndk::ScopedAStatus Gnss::getExtensionGnssBatching(std::shared_ptr<IGnssBatching>*) {
    return unsupported("batching");
}
ndk::ScopedAStatus Gnss::getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>*) {
    return unsupported("geofencing");
}
ndk::ScopedAStatus Gnss::getExtensionGnssNavigationMessage(std::shared_ptr<IGnssNavigationMessageInterface>*) {
    return unsupported("navigation messages");
}
ndk::ScopedAStatus Gnss::getExtensionAGnss(std::shared_ptr<IAGnss>*) { return unsupported("agnss"); }
ndk::ScopedAStatus Gnss::getExtensionAGnssRil(std::shared_ptr<IAGnssRil>*) { return unsupported("agnss ril"); }
ndk::ScopedAStatus Gnss::getExtensionGnssVisibilityControl(
        std::shared_ptr<visibility_control::IGnssVisibilityControl>*) {
    return unsupported("visibility control");
}
ndk::ScopedAStatus Gnss::getExtensionGnssAntennaInfo(std::shared_ptr<IGnssAntennaInfo>*) {
    return unsupported("antenna info");
}
ndk::ScopedAStatus Gnss::getExtensionMeasurementCorrections(
        std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>*) {
    return unsupported("measurement corrections");
}
ndk::ScopedAStatus Gnss::getExtensionGnssAssistanceInterface(
        std::shared_ptr<gnss_assistance::IGnssAssistanceInterface>*) {
    return unsupported("assistance");
}

}  // namespace aidl::android::hardware::gnss
