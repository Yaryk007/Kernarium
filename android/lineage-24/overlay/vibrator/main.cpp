// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.vibrator"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <string>

#include "HostLink.hpp"
#include "Vibrator.hpp"
#include "haptics_player.hpp"

using aidl::android::hardware::vibrator::IVibrator;
using aidl::android::hardware::vibrator::Vibrator;

// The vendor vibrator HAL of devices that have haptics. The service is declared
// in the ODM manifest of the "haptics" SKU and started by init for that SKU
// alone, so it is the only vibrator the framework ever sees. Its frames reach
// the host application over vsock (HostLink); with no host connected the
// framework's view of the vibrator is the same and nothing plays.
int main() {
    // Every call returns at once and nothing waits on the host, so the thread
    // that joins the pool serves them all.
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    andarium::haptics::Player* player = nullptr;
    andarium::haptics::HostLink link([&player](const andarium::haptics::Ack& ack) { player->on_ack(ack); });
    andarium::haptics::Player service_player(
        [&link](std::span<const uint8_t> frame) { return link.send(frame); });
    player = &service_player;
    link.start();

    auto vibrator = ndk::SharedRefBase::make<Vibrator>(service_player);
    const std::string instance = std::string(IVibrator::descriptor) + "/default";
    if (AServiceManager_addService(vibrator->asBinder().get(), instance.c_str()) != STATUS_OK) {
        ALOGE("cannot register %s", instance.c_str());
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;  // joinThreadPool does not return.
}
