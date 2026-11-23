// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "runtime/memory_pressure_response.hpp"

#include <mutex>

namespace kernarium::memory_pressure {

/* The runtime's word for the level the host's response reports. */
[[nodiscard]] constexpr int levelValue(uml::runtime::PressureLevel level) noexcept
{
    switch (level) {
    case uml::runtime::PressureLevel::normal:
        return KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL;
    case uml::runtime::PressureLevel::warn:
        return KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING;
    case uml::runtime::PressureLevel::critical:
        return KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL;
    }
    return KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL;
}

/* The runtime's word for what the response does about it. */
[[nodiscard]] constexpr int responseValue(uml::runtime::PressureReaction reaction) noexcept
{
    switch (reaction) {
    case uml::runtime::PressureReaction::none:
        return KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE;
    case uml::runtime::PressureReaction::responding:
        return KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED;
    case uml::runtime::PressureReaction::underway:
        return KERNARIUM_RUNTIME_MEMORY_RESPONSE_UNDER_WAY;
    }
    return KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE;
}

/* The one receiver of the reports. The callback runs with the mutex held,
 * which is what makes clearing it wait for a call in progress. The response
 * delivers a report with its own lock held (uml::runtime::set_pressure_observer)
 * and takes this one inside it; nothing takes the response's lock while
 * holding this one, so the order holds. */
class Receiver final {
public:
    void set(kernarium_runtime_memory_pressure_callback callback, void* context)
    {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        context_ = context;
    }

    /* On the response's thread, as the report is made. */
    void publish(const uml::runtime::PressureReport& report)
    {
        std::lock_guard lock(mutex_);
        if (callback_ != nullptr)
            callback_(context_, levelValue(report.level), responseValue(report.reaction));
    }

private:
    std::mutex mutex_;
    kernarium_runtime_memory_pressure_callback callback_ {};
    void* context_ {};
};

} // namespace kernarium::memory_pressure
