// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.thermal"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <cstdio>
#include <string>

#include "HostLink.hpp"
#include "Thermal.hpp"

using aidl::android::hardware::thermal::IThermal;
using aidl::android::hardware::thermal::Thermal;

// The vendor thermal HAL of the guest. Android's thermal manager reads the
// host's thermal state through it: the host application sends the severity over
// vsock (HostLink), and with no host connected the severity stays what it last
// was, NONE at boot.
int main() {
    // Every call returns at once and nothing waits on the host, so the thread
    // that joins the pool serves them all.
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    andarium::thermal::HostLink* link = nullptr;
    auto thermal = ndk::SharedRefBase::make<Thermal>([&link] {
        const auto status = link->status();
        char line[160];
        snprintf(line, sizeof(line), "host link: %s, %u connections, %u records, %u refused\n",
                 status.connected ? "connected" : "not connected", status.connections, status.records,
                 status.refused);
        return std::string(line);
    });
    andarium::thermal::HostLink host_link(
            [&thermal](andarium::thermal::Severity severity) { thermal->set_severity(severity); });
    link = &host_link;
    host_link.start();

    const std::string instance = std::string(IThermal::descriptor) + "/default";
    if (AServiceManager_addService(thermal->asBinder().get(), instance.c_str()) != STATUS_OK) {
        ALOGE("cannot register %s", instance.c_str());
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;  // joinThreadPool does not return.
}
