// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.gnss"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <fcntl.h>
#include <log/log.h>
#include <time.h>
#include <wakelock/wakelock.h>

#include <cerrno>
#include <memory>
#include <optional>
#include <string>

#include "Gnss.hpp"

using aidl::android::hardware::gnss::Gnss;

namespace {

// The receiver node the guest kernel registers for the host's receiver
// (kernel/common: drivers/gnss/kernarium.c). It always exists on an Android
// guest, with or without a position source behind it on the host.
constexpr char kReceiver[] = "/dev/gnss0";
// The name system-suspend lists the lock under.
constexpr char kWakelockName[] = "AndariumGnss";

// A libpower wakelock cannot be moved, so the holder builds it in place.
struct HeldWakelock {
    explicit HeldWakelock(const std::string& name) : lock(android::wakelock::WakeLock::tryGet(name)) {}
    std::optional<android::wakelock::WakeLock> lock;
};

int64_t now_ns(clockid_t clock) {
    timespec now{};
    clock_gettime(clock, &now);
    return static_cast<int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

andarium::gnss::Env android_env() {
    andarium::gnss::Env env;
    env.open_device = [] {
        const int fd = open(kReceiver, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        return fd >= 0 ? fd : -errno;
    };
    env.hold_wakelock = []() -> std::shared_ptr<void> {
        auto held = std::make_shared<HeldWakelock>(kWakelockName);
        if (!held->lock) return nullptr;
        return held;
    };
    env.realtime_ns = [] { return now_ns(CLOCK_REALTIME); };
    env.boottime_ns = [] { return now_ns(CLOCK_BOOTTIME); };
    env.log = [](const std::string& message) { ALOGW("%s", message.c_str()); };
    return env;
}

}  // namespace

// The vendor GNSS HAL of the guest. It is declared in the vendor manifest and
// started at boot on every device, because /dev/gnss0 is there on every device:
// whether the host has a position to give is the receiver's matter, and the
// HAL reports nothing until it has one. Nothing opens the receiver before the
// framework's start(), so the host does not locate for an idle guest.
int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();

    auto gnss = ndk::SharedRefBase::make<Gnss>(android_env());
    const std::string instance = std::string(Gnss::descriptor) + "/default";
    if (AServiceManager_addService(gnss->asBinder().get(), instance.c_str()) != STATUS_OK) {
        ALOGE("cannot register %s", instance.c_str());
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;  // joinThreadPool does not return.
}
