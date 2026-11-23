// SPDX-License-Identifier: Apache-2.0
//
// android.hardware.thermal.IThermal (AIDL version 3) on the host's thermal
// state. The HAL has one SKIN sensor whose temperature follows the severity
// the host last sent (thermal_levels.hpp), no other sensor and no cooling
// device. The severity starts at NONE and stays what it was when the host link
// drops. Callbacks follow the AOSP example HAL's rules: a null or a repeated
// registration is refused, a type filter is honoured, and every change of the
// severity is notified.
#pragma once

#include <aidl/android/hardware/thermal/BnThermal.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "thermal_levels.hpp"

namespace aidl::android::hardware::thermal {
class Thermal : public BnThermal {
public:
    // `link_summary` describes the host link for dump().
    explicit Thermal(std::function<std::string()> link_summary)
        : link_summary_(std::move(link_summary)) {}

    // The host's severity. Called from the link's thread.
    void set_severity(andarium::thermal::Severity severity);

    ndk::ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* _aidl_return) override;
    ndk::ScopedAStatus getCoolingDevicesWithType(CoolingType type,
                                                 std::vector<CoolingDevice>* _aidl_return) override;
    ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* _aidl_return) override;
    ndk::ScopedAStatus getTemperaturesWithType(TemperatureType type,
                                               std::vector<Temperature>* _aidl_return) override;
    ndk::ScopedAStatus getTemperatureThresholds(
            std::vector<TemperatureThreshold>* _aidl_return) override;
    ndk::ScopedAStatus getTemperatureThresholdsWithType(
            TemperatureType type, std::vector<TemperatureThreshold>* _aidl_return) override;
    ndk::ScopedAStatus registerThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& callback) override;
    ndk::ScopedAStatus registerThermalChangedCallbackWithType(
            const std::shared_ptr<IThermalChangedCallback>& callback, TemperatureType type) override;
    ndk::ScopedAStatus unregisterThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& callback) override;
    ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& callback, CoolingType type) override;
    ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& callback) override;

    // Left to the system service, which then samples the temperature once a
    // second and caches the headroom, instead of making a call here per
    // application call.
    ndk::ScopedAStatus forecastSkinTemperature(int32_t forecastSeconds, float* _aidl_return) override;

    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

private:
    struct Registration {
        std::shared_ptr<IThermalChangedCallback> callback;
        // The registration's type filter; none means every type.
        std::optional<TemperatureType> type;
    };

    ndk::ScopedAStatus add_callback(const std::shared_ptr<IThermalChangedCallback>& callback,
                                    std::optional<TemperatureType> type);
    Temperature skin_locked() const;

    std::function<std::string()> link_summary_;
    mutable std::mutex mutex_;
    andarium::thermal::Severity severity_ = andarium::thermal::Severity::none;
    std::vector<Registration> callbacks_;
    std::vector<std::shared_ptr<ICoolingDeviceChangedCallback>> cooling_callbacks_;
};
}  // namespace aidl::android::hardware::thermal
