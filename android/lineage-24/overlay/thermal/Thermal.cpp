// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.thermal"

#include "Thermal.hpp"

#include <android/binder_auto_utils.h>
#include <log/log.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aidl::android::hardware::thermal {
namespace {
using andarium::thermal::Severity;
namespace levels = andarium::thermal;

// The host's severities are the framework's ThrottlingSeverity values.
static_assert(int32_t(ThrottlingSeverity::NONE) == int32_t(Severity::none));
static_assert(int32_t(ThrottlingSeverity::LIGHT) == int32_t(Severity::light));
static_assert(int32_t(ThrottlingSeverity::MODERATE) == int32_t(Severity::moderate));
static_assert(int32_t(ThrottlingSeverity::SEVERE) == int32_t(Severity::severe));
static_assert(int32_t(ThrottlingSeverity::CRITICAL) == int32_t(Severity::critical));
static_assert(levels::severity_count == 7, "one threshold for each ThrottlingSeverity");

// Two interfaces are one registration when they are the same binder.
template <typename Interface>
bool same_interface(const std::shared_ptr<Interface>& left, const std::shared_ptr<Interface>& right) {
    if (left == nullptr || right == nullptr || !left->isRemote() || !right->isRemote()) {
        return left == right;
    }
    return left->asBinder() == right->asBinder();
}

ndk::ScopedAStatus illegal_argument(const char* message) {
    return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT, message);
}

TemperatureThreshold skin_threshold() {
    TemperatureThreshold threshold;
    threshold.type = TemperatureType::SKIN;
    threshold.name = levels::sensor_name;
    threshold.hotThrottlingThresholds.assign(levels::hot_thresholds.begin(), levels::hot_thresholds.end());
    threshold.coldThrottlingThresholds.assign(levels::cold_thresholds.begin(), levels::cold_thresholds.end());
    return threshold;
}
}  // namespace

Temperature Thermal::skin_locked() const {
    Temperature temperature;
    temperature.type = TemperatureType::SKIN;
    temperature.name = levels::sensor_name;
    temperature.value = levels::skin_temperature(severity_);
    temperature.throttlingStatus = ThrottlingSeverity(levels::throttling_status(severity_));
    return temperature;
}

void Thermal::set_severity(Severity severity) {
    std::vector<std::shared_ptr<IThermalChangedCallback>> targets;
    Temperature temperature;
    {
        std::lock_guard lock(mutex_);
        if (severity == severity_) return;
        severity_ = severity;
        temperature = skin_locked();
        for (const auto& registration : callbacks_) {
            if (!registration.type || *registration.type == TemperatureType::SKIN) {
                targets.push_back(registration.callback);
            }
        }
    }
    ALOGI("severity %d, skin %.1f", int(temperature.throttlingStatus), temperature.value);
    // The calls are oneway, so no callback holds this thread; one whose process
    // has died is dropped.
    std::vector<std::shared_ptr<IThermalChangedCallback>> dead;
    for (const auto& callback : targets) {
        const auto status = callback->notifyThrottling(temperature);
        if (!status.isOk() && status.getStatus() == STATUS_DEAD_OBJECT) dead.push_back(callback);
    }
    if (!dead.empty()) {
        std::lock_guard lock(mutex_);
        std::erase_if(callbacks_, [&](const Registration& registration) {
            return std::any_of(dead.begin(), dead.end(), [&](const auto& callback) {
                return same_interface(registration.callback, callback);
            });
        });
    }
}

ndk::ScopedAStatus Thermal::getCoolingDevices(std::vector<CoolingDevice>*) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getCoolingDevicesWithType(CoolingType, std::vector<CoolingDevice>*) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getTemperatures(std::vector<Temperature>* _aidl_return) {
    std::lock_guard lock(mutex_);
    *_aidl_return = {skin_locked()};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getTemperaturesWithType(TemperatureType type,
                                                    std::vector<Temperature>* _aidl_return) {
    std::lock_guard lock(mutex_);
    if (type == TemperatureType::SKIN) *_aidl_return = {skin_locked()};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getTemperatureThresholds(std::vector<TemperatureThreshold>* _aidl_return) {
    *_aidl_return = {skin_threshold()};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getTemperatureThresholdsWithType(
        TemperatureType type, std::vector<TemperatureThreshold>* _aidl_return) {
    if (type == TemperatureType::SKIN) *_aidl_return = {skin_threshold()};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::add_callback(const std::shared_ptr<IThermalChangedCallback>& callback,
                                         std::optional<TemperatureType> type) {
    if (callback == nullptr) return illegal_argument("Invalid nullptr callback");
    std::lock_guard lock(mutex_);
    if (std::any_of(callbacks_.begin(), callbacks_.end(), [&](const Registration& registration) {
            return same_interface(registration.callback, callback);
        })) {
        return illegal_argument("Callback already registered");
    }
    callbacks_.push_back({callback, type});
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::registerThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& callback) {
    return add_callback(callback, std::nullopt);
}

ndk::ScopedAStatus Thermal::registerThermalChangedCallbackWithType(
        const std::shared_ptr<IThermalChangedCallback>& callback, TemperatureType type) {
    return add_callback(callback, type);
}

ndk::ScopedAStatus Thermal::unregisterThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& callback) {
    if (callback == nullptr) return illegal_argument("Invalid nullptr callback");
    std::lock_guard lock(mutex_);
    const auto removed = std::erase_if(callbacks_, [&](const Registration& registration) {
        return same_interface(registration.callback, callback);
    });
    if (removed == 0) return illegal_argument("Callback wasn't registered");
    return ndk::ScopedAStatus::ok();
}

// The HAL has no cooling device, so nothing is ever notified, but the
// registration rules are the interface's.
ndk::ScopedAStatus Thermal::registerCoolingDeviceChangedCallbackWithType(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& callback, CoolingType) {
    if (callback == nullptr) return illegal_argument("Invalid nullptr callback");
    std::lock_guard lock(mutex_);
    if (std::any_of(cooling_callbacks_.begin(), cooling_callbacks_.end(), [&](const auto& registered) {
            return same_interface(registered, callback);
        })) {
        return illegal_argument("Callback already registered");
    }
    cooling_callbacks_.push_back(callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::unregisterCoolingDeviceChangedCallback(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& callback) {
    if (callback == nullptr) return illegal_argument("Invalid nullptr callback");
    std::lock_guard lock(mutex_);
    const auto removed = std::erase_if(cooling_callbacks_, [&](const auto& registered) {
        return same_interface(registered, callback);
    });
    if (removed == 0) return illegal_argument("Callback wasn't registered");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::forecastSkinTemperature(int32_t, float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

binder_status_t Thermal::dump(int fd, const char**, uint32_t) {
    std::string text;
    {
        std::lock_guard lock(mutex_);
        const auto temperature = skin_locked();
        char line[160];
        snprintf(line, sizeof(line), "severity: %d, %s %.1f C\ncallbacks: %zu, cooling callbacks: %zu\n",
                 int(temperature.throttlingStatus), temperature.name.c_str(), temperature.value,
                 callbacks_.size(), cooling_callbacks_.size());
        text = line;
    }
    text += link_summary_();
    // The descriptor belongs to the caller. A short write only shortens a
    // diagnostic, so the result is not an error.
    if (write(fd, text.data(), text.size()) < 0) ALOGW("dump not written");
    return STATUS_OK;
}
}  // namespace aidl::android::hardware::thermal
