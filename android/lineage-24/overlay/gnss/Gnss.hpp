// SPDX-License-Identifier: Apache-2.0
//
// The AIDL IGnss (version 7) of the host's virtual receiver. Everything the
// framework can ask of a receiver that gives positions and nothing else: it
// answers the session, the position mode and the sentence and satellite
// toggles, and every extension of a real receiver's that this one lacks
// (measurements, navigation messages, batching, geofencing, assistance data,
// antenna information, corrections, visibility control, power indication)
// answers EX_UNSUPPORTED_OPERATION. The framework wraps an extension that came
// back OK but null, so a missing extension must be an error status.
#pragma once

#include <aidl/android/hardware/gnss/BnGnss.h>
#include <aidl/android/hardware/gnss/BnGnssConfiguration.h>
#include <aidl/android/hardware/gnss/BnGnssDebug.h>
#include <aidl/android/hardware/gnss/IGnssCallback.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "gnss_engine.hpp"
#include "gnss_hal.hpp"

namespace aidl::android::hardware::gnss {

// There is no assistance server, SUPL, positioning protocol or satellite sky to
// configure, so every call is accepted and has no effect.
class GnssConfiguration : public BnGnssConfiguration {
public:
    ndk::ScopedAStatus setSuplVersion(int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setSuplMode(int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setLppProfile(int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setGlonassPositioningProtocol(int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setEmergencySuplPdn(bool) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setEsExtensionSec(int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setBlocklist(const std::vector<BlocklistedSource>&) override {
        return ndk::ScopedAStatus::ok();
    }
};

// The last position the session reported, with no satellites.
class GnssDebug : public BnGnssDebug {
public:
    using LastFix = std::function<std::optional<andarium::gnss::DebugFix>()>;
    GnssDebug(LastFix last_fix, std::function<int64_t()> realtime_ns, std::function<int64_t()> boottime_ns)
        : last_fix_(std::move(last_fix)),
          realtime_ns_(std::move(realtime_ns)),
          boottime_ns_(std::move(boottime_ns)) {}

    ndk::ScopedAStatus getDebugData(DebugData* debug_data) override;

private:
    LastFix last_fix_;
    std::function<int64_t()> realtime_ns_;
    std::function<int64_t()> boottime_ns_;
};

class Gnss : public BnGnss {
public:
    explicit Gnss(andarium::gnss::Env env);
    ~Gnss() override;

    ndk::ScopedAStatus setCallback(const std::shared_ptr<IGnssCallback>& callback) override;
    ndk::ScopedAStatus start() override;
    ndk::ScopedAStatus stop() override;
    ndk::ScopedAStatus close() override;

    ndk::ScopedAStatus injectTime(int64_t time_ms, int64_t time_reference_ms, int32_t uncertainty_ms) override;
    ndk::ScopedAStatus injectLocation(const GnssLocation& location) override;
    ndk::ScopedAStatus injectBestLocation(const GnssLocation& location) override;
    ndk::ScopedAStatus deleteAidingData(GnssAidingData aiding_data_flags) override;
    ndk::ScopedAStatus setPositionMode(const PositionModeOptions& options) override;
    ndk::ScopedAStatus startSvStatus() override;
    ndk::ScopedAStatus stopSvStatus() override;
    ndk::ScopedAStatus startNmea() override;
    ndk::ScopedAStatus stopNmea() override;

    ndk::ScopedAStatus getExtensionPsds(std::shared_ptr<IGnssPsds>* result) override;
    ndk::ScopedAStatus getExtensionGnssConfiguration(std::shared_ptr<IGnssConfiguration>* result) override;
    ndk::ScopedAStatus getExtensionGnssPowerIndication(std::shared_ptr<IGnssPowerIndication>* result) override;
    ndk::ScopedAStatus getExtensionGnssMeasurement(std::shared_ptr<IGnssMeasurementInterface>* result) override;
    ndk::ScopedAStatus getExtensionGnssBatching(std::shared_ptr<IGnssBatching>* result) override;
    ndk::ScopedAStatus getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>* result) override;
    ndk::ScopedAStatus getExtensionGnssNavigationMessage(
            std::shared_ptr<IGnssNavigationMessageInterface>* result) override;
    ndk::ScopedAStatus getExtensionAGnss(std::shared_ptr<IAGnss>* result) override;
    ndk::ScopedAStatus getExtensionAGnssRil(std::shared_ptr<IAGnssRil>* result) override;
    ndk::ScopedAStatus getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* result) override;
    ndk::ScopedAStatus getExtensionGnssVisibilityControl(
            std::shared_ptr<visibility_control::IGnssVisibilityControl>* result) override;
    ndk::ScopedAStatus getExtensionGnssAntennaInfo(std::shared_ptr<IGnssAntennaInfo>* result) override;
    ndk::ScopedAStatus getExtensionMeasurementCorrections(
            std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>* result) override;
    ndk::ScopedAStatus getExtensionGnssAssistanceInterface(
            std::shared_ptr<gnss_assistance::IGnssAssistanceInterface>* result) override;

private:
    andarium::gnss::Sink sink_for(const std::shared_ptr<IGnssCallback>& callback) const;

    const andarium::gnss::Env env_;
    // The state behind the interface and the locks that guard it.
    andarium::gnss::Hal hal_;
};

}  // namespace aidl::android::hardware::gnss
