// SPDX-License-Identifier: GPL-2.0-or-later

#import "KernariumRuntimeLoader.h"

#include "../../darwin/device/camera/camera_tiers.hpp"
#include "../../darwin/platform/apple/os_address_space.hpp"
#include "../../darwin/platform/apple/os_memory_limit.hpp"
#include "../../darwin/runtime/guest_memory_budget.hpp"

#import <Foundation/Foundation.h>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

std::mutex selectionMutex;
std::atomic<const kernarium_runtime_api*> selectedApi;
uint32_t selectedFlavor;
bool selectionAttempted;
/* The selected runtime's automation extensions (KernariumRuntime.h), or null:
 * a release runtime does not export them. */
int (*automationJobAtStart)(void);
int (*automationHoldDelayMs)(void);
/* The runtime relay can still call into the loader while exit() destroys
 * ordinary statics. Keep its lock and retained handler alive together. */
struct ConsoleOutputHandlerState final {
    std::mutex mutex;
    KernariumConsoleOutputHandler handler {nil};
};
ConsoleOutputHandlerState& consoleOutput = *new ConsoleOutputHandlerState;
/* The handler is read on the runtime's callback path, under the runtime's own
 * lock, so it has a mutex of its own that is held only to copy it. Changing
 * the registration, which takes the runtime's lock, is serialized by the
 * other one, which the callback path never takes. */
std::mutex restartHandlerMutex;
std::mutex restartRegistrationMutex;
KernariumRestartHandler restartHandler;
/* The same shape for the receiver-power events. */
std::mutex locationHandlerMutex;
std::mutex locationRegistrationMutex;
KernariumLocationReceiverHandler locationHandler;
/* And for the camera state. */
std::mutex cameraStateHandlerMutex;
std::mutex cameraStateRegistrationMutex;
KernariumCameraStateHandler cameraStateHandler;
/* And for the memory pressure, with the console handler's lifetime: the pressure
 * queue can still deliver a report while exit() destroys ordinary statics. */
struct MemoryPressureHandlerState final {
    std::mutex handlerMutex;
    std::mutex registrationMutex;
    KernariumMemoryPressureHandler handler {nil};
};
MemoryPressureHandlerState& memoryPressure = *new MemoryPressureHandlerState;
/* And for the input status. */
std::mutex inputStatusHandlerMutex;
std::mutex inputStatusRegistrationMutex;
KernariumInputStatusHandler inputStatusHandler;

void forwardConsoleOutput(
    void*,
    const void* buffer,
    std::size_t length)
{
    if (buffer == nullptr || length == 0U)
        return;
    NSData* data = [NSData dataWithBytes:buffer length:length];
    KernariumConsoleOutputHandler handler;
    {
        std::lock_guard lock(consoleOutput.mutex);
        handler = consoleOutput.handler;
    }
    if (handler == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(data);
    });
}

void updateConsoleOutputForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard lock(consoleOutput.mutex);
    runtime->set_console_output_callback(
        consoleOutput.handler == nil ? nullptr : forwardConsoleOutput,
        nullptr);
}

/* Runs on a runtime thread, inside the runtime's call, so it only hands the
 * event to the main queue. */
void forwardRestart(
    void*,
    uint32_t event,
    uint32_t restartCount)
{
    KernariumRestartHandler handler;
    {
        std::lock_guard lock(restartHandlerMutex);
        handler = restartHandler;
    }
    if (handler == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(event, restartCount);
    });
}

void updateRestartForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard registration(restartRegistrationMutex);
    bool wanted;
    {
        std::lock_guard lock(restartHandlerMutex);
        wanted = restartHandler != nil;
    }
    runtime->set_restart_callback(wanted ? forwardRestart : nullptr, nullptr);
}

/* Runs on a runtime thread, inside the runtime's call, so it only hands the
 * event to the main queue. */
void forwardLocationReceiver(void*, int powered)
{
    KernariumLocationReceiverHandler handler;
    {
        std::lock_guard lock(locationHandlerMutex);
        handler = locationHandler;
    }
    if (handler == nil)
        return;
    const BOOL on = powered != 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(on);
    });
}

void updateLocationReceiverForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard registration(locationRegistrationMutex);
    bool wanted;
    {
        std::lock_guard lock(locationHandlerMutex);
        wanted = locationHandler != nil;
    }
    runtime->set_location_receiver_callback(
        wanted ? forwardLocationReceiver : nullptr, nullptr);
}

/* Runs on a runtime thread, inside the runtime's call, so it only hands the
 * state to the main queue. */
void forwardCameraState(void*, int state)
{
    KernariumCameraStateHandler handler;
    {
        std::lock_guard lock(cameraStateHandlerMutex);
        handler = cameraStateHandler;
    }
    if (handler == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(state);
    });
}

void updateCameraStateForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard registration(cameraStateRegistrationMutex);
    bool wanted;
    {
        std::lock_guard lock(cameraStateHandlerMutex);
        wanted = cameraStateHandler != nil;
    }
    runtime->set_camera_state_callback(
        wanted ? forwardCameraState : nullptr, nullptr);
}

/* Runs on a runtime thread, inside the runtime's call, so it only hands the
 * report to the main queue. */
void forwardMemoryPressure(void*, int level, int response)
{
    KernariumMemoryPressureHandler handler;
    {
        std::lock_guard lock(memoryPressure.handlerMutex);
        handler = memoryPressure.handler;
    }
    if (handler == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(level, response);
    });
}

void updateMemoryPressureForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard registration(memoryPressure.registrationMutex);
    bool wanted;
    {
        std::lock_guard lock(memoryPressure.handlerMutex);
        wanted = memoryPressure.handler != nil;
    }
    runtime->set_memory_pressure_callback(
        wanted ? forwardMemoryPressure : nullptr, nullptr);
}

/* Runs on a runtime thread, inside the runtime's call, so it only hands the
 * event to the main queue. */
void forwardInputStatus(void*, int status, int on)
{
    KernariumInputStatusHandler handler;
    {
        std::lock_guard lock(inputStatusHandlerMutex);
        handler = inputStatusHandler;
    }
    if (handler == nil)
        return;
    const BOOL lit = on != 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        handler(status, lit);
    });
}

void updateInputStatusForwarder(const kernarium_runtime_api* runtime)
{
    std::lock_guard registration(inputStatusRegistrationMutex);
    bool wanted;
    {
        std::lock_guard lock(inputStatusHandlerMutex);
        wanted = inputStatusHandler != nil;
    }
    runtime->set_input_status_callback(
        wanted ? forwardInputStatus : nullptr, nullptr);
}

[[noreturn]] void guestTimerMaskFailure() noexcept
{
    static constexpr char message[] =
        "Kernarium: unable to isolate the UML timer signal\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    _exit(127);
}

void blockGuestTimer() noexcept
{
    sigset_t blocked;
    if (sigemptyset(&blocked) != 0 ||
        sigaddset(&blocked, SIGALRM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0)
        guestTimerMaskFailure();
}

/*
 * The process-directed early ITIMER_REAL calibration must not land on a
 * UIKit or Swift worker. This hook stays in the always-linked application so
 * it runs before main even though the selected emulator framework is loaded
 * later. Only the kernel's vCPU0 carrier temporarily unmasks SIGALRM.
 */
__attribute__((constructor)) void isolateGuestTimerFromApplicationThreads()
{
    blockGuestTimer();
}

/* The flavour leads the framework name. Linux additionally requires a numeric
 * release after its prefix because the specialized names share its stem. */
[[nodiscard]] NSString* flavorPrefix(uint32_t guestFlavor)
{
    switch (guestFlavor) {
    case KERNARIUM_RUNTIME_GUEST_LINUX:
        return @"KRuntime_";
    case KERNARIUM_RUNTIME_GUEST_ANDROID:
        return @"KRuntime_Android_";
    default:
        return nil;
    }
}

[[nodiscard]] bool matchesFlavor(
    NSString* name,
    NSString* prefix,
    uint32_t guestFlavor)
{
    if (![name hasPrefix:prefix])
        return false;
    if (guestFlavor != KERNARIUM_RUNTIME_GUEST_LINUX)
        return true;

    NSUInteger releaseIndex = prefix.length;
    if (name.length <= releaseIndex)
        return false;
    unichar releaseFirst = [name characterAtIndex:releaseIndex];
    return releaseFirst >= '0' && releaseFirst <= '9';
}

[[nodiscard]] NSURL* findRuntimeFramework(
    NSString* prefix,
    uint32_t guestFlavor)
{
    NSURL* frameworks = NSBundle.mainBundle.privateFrameworksURL;
    if (frameworks == nil)
        return nil;

    NSError* error = nil;
    NSArray<NSURL*>* contents = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:frameworks
        includingPropertiesForKeys:@[NSURLIsDirectoryKey]
        options:NSDirectoryEnumerationSkipsHiddenFiles
        error:&error];
    if (contents == nil) {
        std::fprintf(
            stderr,
            "Kernarium: cannot inspect embedded frameworks: %s\n",
            error.localizedDescription.UTF8String ?: "unknown error");
        return nil;
    }

    NSMutableArray<NSURL*>* matches = [NSMutableArray array];
    for (NSURL* candidate in contents) {
        NSString* name = candidate.lastPathComponent;
        if (matchesFlavor(name, prefix, guestFlavor) &&
            [name hasSuffix:@"_4K.framework"])
            [matches addObject:candidate];
    }
    [matches sortUsingComparator:^NSComparisonResult(NSURL* first, NSURL* second) {
        return [first.lastPathComponent compare:second.lastPathComponent];
    }];
    if (matches.count != 1U) {
        std::fprintf(
            stderr,
            "Kernarium: expected one embedded %s4K runtime framework, "
            "found %lu\n",
            prefix.UTF8String,
            static_cast<unsigned long>(matches.count));
        return nil;
    }
    return matches.firstObject;
}

/* Every embedded runtime has 4 KiB guest pages; a framework that says
 * otherwise is stale. */
[[nodiscard]] bool validApi(
    const kernarium_runtime_api* api,
    uint32_t guestFlavor)
{
    if (api == nullptr ||
        api->abi_version != KERNARIUM_RUNTIME_API_VERSION ||
        api->struct_size < sizeof(kernarium_runtime_api) ||
        api->guest_page_size != 4096U ||
        api->guest_flavor != guestFlavor ||
        api->run == nullptr ||
        api->request_stop == nullptr ||
        api->sync == nullptr ||
        api->quiesce == nullptr ||
        api->resume == nullptr ||
        api->send_touch == nullptr ||
        api->send_key == nullptr ||
        api->set_console_output_callback == nullptr ||
        api->send_console_input == nullptr ||
        api->set_gpu_hold == nullptr ||
        api->set_frame_sink == nullptr ||
        api->set_background_throttle == nullptr ||
        api->set_page_cache_trimming == nullptr ||
        api->set_microphone == nullptr ||
        api->set_restart_callback == nullptr ||
        api->guest_display_active == nullptr ||
        api->plan_gpu_budget == nullptr ||
        api->connect_vsock == nullptr ||
        api->read_statistics == nullptr ||
        api->read_guest_memory == nullptr ||
        api->cancel_touch == nullptr ||
        api->quiesce_resident == nullptr ||
        api->read_graphics_memory == nullptr ||
        api->prepare_jit == nullptr ||
        api->set_location_receiver_callback == nullptr ||
        api->report_location == nullptr ||
        api->set_input_connected == nullptr ||
        api->send_keyboard_usage == nullptr ||
        api->send_pointer == nullptr ||
        api->send_mouse == nullptr ||
        api->set_input_status_callback == nullptr ||
        api->set_network_notice == nullptr ||
        api->set_camera_state_callback == nullptr ||
        api->set_camera_rotation == nullptr ||
        api->plan_camera_reserve == nullptr ||
        api->set_memory_pressure_callback == nullptr) {
        std::fputs("Kernarium: runtime API validation failed\n", stderr);
        return false;
    }
    return true;
}

/* `prefix` names the framework for `guestFlavor`; the caller has resolved it.
 * The handle stays loaded for the life of the process. */
[[nodiscard]] int loadRuntime(NSString* prefix, uint32_t guestFlavor)
{
    NSURL* framework = findRuntimeFramework(prefix, guestFlavor);
    NSBundle* bundle = framework == nil ? nil : [NSBundle bundleWithURL:framework];
    NSString* executable = bundle.executablePath;
    if (executable == nil) {
        std::fputs("Kernarium: selected runtime has no framework executable\n", stderr);
        return -1;
    }

    dlerror();
    void* handle = dlopen(executable.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* error = dlerror();
        std::fprintf(
            stderr,
            "Kernarium: cannot load selected runtime: %s\n",
            error == nullptr ? "unknown dlopen error" : error);
        return -1;
    }

    dlerror();
    void* symbol = dlsym(handle, "kernarium_runtime_get_api_v28");
    const char* symbolError = dlerror();
    if (symbol == nullptr || symbolError != nullptr) {
        std::fprintf(
            stderr,
            "Kernarium: selected runtime lacks API v28: %s\n",
            symbolError == nullptr ? "symbol unavailable" : symbolError);
        (void)dlclose(handle);
        return -1;
    }
    const auto getter = reinterpret_cast<
        const kernarium_runtime_api* (*)(void)>(symbol);
    const kernarium_runtime_api* api = getter();
    if (!validApi(api, guestFlavor)) {
        (void)dlclose(handle);
        return -1;
    }

    updateConsoleOutputForwarder(api);
    updateRestartForwarder(api);
    updateLocationReceiverForwarder(api);
    updateCameraStateForwarder(api);
    updateMemoryPressureForwarder(api);
    updateInputStatusForwarder(api);
    /* Optional: only a runtime built for automation has it. */
    automationJobAtStart = reinterpret_cast<int (*)(void)>(
        dlsym(handle, "kernarium_runtime_automation_job_at_start"));
    (void)dlerror();
    automationHoldDelayMs = reinterpret_cast<int (*)(void)>(
        dlsym(handle, "kernarium_runtime_automation_hold_delay_ms"));
    (void)dlerror();
    selectedFlavor = guestFlavor;
    selectedApi.store(api, std::memory_order_release);
    std::printf(
        "Kernarium: loaded runtime %s\n",
        framework.lastPathComponent.UTF8String);
    return 0;
}

[[nodiscard]] const kernarium_runtime_api* api() noexcept
{
    return selectedApi.load(std::memory_order_acquire);
}

} // namespace

extern "C" int kernarium_runtime_loader_select(uint32_t guestFlavor)
{
    NSString* prefix = flavorPrefix(guestFlavor);
    if (prefix == nil) {
        std::fprintf(
            stderr,
            "Kernarium: unsupported guest flavour %u\n",
            guestFlavor);
        return -1;
    }

    std::lock_guard lock(selectionMutex);
    if (api() != nullptr)
        return selectedFlavor == guestFlavor ? 0 : -1;
    if (selectionAttempted)
        return -1;
    selectionAttempted = true;
    return loadRuntime(prefix, guestFlavor);
}

extern "C" int kernarium_runtime_loader_run(
    MTKView* view,
    const kernarium_runtime_boot* boot)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime == nullptr) {
        std::fputs("Kernarium: no runtime framework is selected\n", stderr);
        return -1;
    }
    return runtime->run(view, boot);
}

extern "C" void kernarium_runtime_loader_request_stop(void)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->request_stop();
}

/* With no runtime loaded there is no guest, so nothing to settle: a report that
 * fits says NO_GUEST, and one that does not is refused as the runtime refuses
 * it. */
static int noRuntime(kernarium_runtime_settle* settle)
{
    if (settle != nullptr) {
        if (settle->struct_size < sizeof(*settle))
            return -EINVAL;
        settle->reserved = 0U;
        settle->guest_storage = KERNARIUM_RUNTIME_GUEST_STORAGE_NO_GUEST;
        settle->guest_suspension = KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED;
    }
    return -1;
}

extern "C" int kernarium_runtime_loader_sync(kernarium_runtime_settle* settle)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? noRuntime(settle) : runtime->sync(settle);
}

extern "C" int kernarium_runtime_loader_quiesce(kernarium_runtime_settle* settle)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? noRuntime(settle) : runtime->quiesce(settle);
}

extern "C" int kernarium_runtime_loader_quiesce_resident(kernarium_runtime_settle* settle)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? noRuntime(settle) : runtime->quiesce_resident(settle);
}

extern "C" void kernarium_runtime_loader_resume(void)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->resume();
}

extern "C" int kernarium_runtime_loader_set_gpu_hold(int held)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->set_gpu_hold(held);
}

extern "C" int kernarium_runtime_loader_set_background_throttle(int enabled)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->set_background_throttle(enabled);
}

extern "C" int kernarium_runtime_loader_set_frame_sink(
    kernarium_runtime_frame_sink sink,
    void* context,
    uint32_t maximumWidth,
    uint32_t framesPerSecond)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -1 :
        runtime->set_frame_sink(sink, context, maximumWidth, framesPerSecond);
}

extern "C" int kernarium_runtime_loader_set_page_cache_trimming(int enabled)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->set_page_cache_trimming(enabled);
}

extern "C" int kernarium_runtime_loader_plan_gpu_budget(
    uint32_t guestMemoryMiB,
    uint32_t guestProcessorCount,
    uint32_t applicationMiB,
    uint32_t engineMiB,
    uint32_t headroomMiB,
    kernarium_runtime_gpu_budget* plan)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -1 :
        runtime->plan_gpu_budget(
            guestMemoryMiB,
            guestProcessorCount,
            applicationMiB,
            engineMiB,
            headroomMiB,
            plan);
}

namespace {

kernarium_launch_memory_set launchMemorySet(
    const uml::runtime::guest_memory::LaunchMemorySet& set)
{
    namespace budget = uml::runtime::guest_memory;
    return {
        .application_mib =
            static_cast<uint32_t>(set.application / budget::mebibyte),
        .engine_mib =
            static_cast<uint32_t>(set.engine_rest / budget::mebibyte),
        .headroom_mib =
            static_cast<uint32_t>(set.headroom / budget::mebibyte),
    };
}

uml::runtime::guest_memory::LaunchMemorySet launchMemorySet(
    kernarium_launch_memory_set parameters)
{
    return uml::runtime::guest_memory::launch_memory_mib(
        parameters.application_mib,
        parameters.engine_mib,
        parameters.headroom_mib);
}

/* C of runtime/guest_memory_budget.hpp for this process: the window the
 * runtime puts the kernel in (a macOS process, and an iOS application on a Mac,
 * get the macOS window; a device and the Simulator get the signed iOS
 * envelope), the RAM of the host and, for an application the system limits to
 * a small span of address space (signed without Extended Virtual Addressing),
 * that span: the widest run of it free above the lowest address a window may
 * begin at. Every other process, the Simulator and an entitled application
 * included, is answered as it was before the span was measured. It is measured
 * once, at the first answer, before any run of the process has reserved a
 * window of its own, so every size, default and bound the settings show, and
 * the check before a start, follow the process's own address space and stay as
 * they were while a guest runs. The ceiling is zero where the span holds no
 * window of even the smallest guest: no size is admitted, offered or
 * defaulted, and a start is refused. The runtime's run admits the sizes of the
 * ceiling without the address space: its reservation of the window is the
 * check there. */
uint32_t guestMemoryCeilingMiB()
{
    static const uint32_t ceiling = [] {
        NSProcessInfo* info = NSProcessInfo.processInfo;
#if TARGET_OS_OSX
        const bool macosWindow = true;
#else
        const bool macosWindow = info.isiOSAppOnMac;
#endif
        uint64_t addressSpace = 0;
        if (!macosWindow) {
            const auto run = uml::platform::os_limited_free_address_run(
                uml::runtime::guest_memory::native_window_floor_bytes);
            /* A measured run is never taken for an unmeasured one: a run of
             * none is a byte, and no window fits it. */
            if (run)
                addressSpace = *run > 0 ? *run : 1;
        }
        const uint32_t answer =
            uml::runtime::guest_memory::process_guest_memory_ceiling_mib(
                macosWindow, info.physicalMemory, addressSpace);
        if (addressSpace != 0)
            std::fprintf(stderr,
                "Kernarium: the process has a run of %llu MiB of address "
                "space; %s%u MiB\n",
                static_cast<unsigned long long>(addressSpace >> 20),
                answer == 0 ? "no guest fits it, a guest may have up to " :
                              "a guest may have up to ",
                answer);
        return answer;
    }();
    return ceiling;
}

} // namespace

extern "C" kernarium_launch_memory_set
kernarium_runtime_loader_launch_memory_set(void)
{
    namespace budget = uml::runtime::guest_memory;
    static_assert(
        budget::launch_memory_set.application % budget::mebibyte == 0 &&
            budget::launch_memory_set.engine_rest % budget::mebibyte == 0 &&
            budget::launch_memory_set.headroom % budget::mebibyte == 0,
        "the launch set is whole mebibytes");
    return launchMemorySet(budget::launch_memory_set);
}

extern "C" kernarium_launch_memory_set
kernarium_runtime_loader_maximum_memory_set(void)
{
    return launchMemorySet(
        uml::runtime::guest_memory::maximum_memory_set);
}

extern "C" uint32_t kernarium_runtime_loader_application_vcpu_limit(
    uint32_t physicalProcessorCount)
{
    return uml::runtime::guest_memory::application_vcpu_limit(
        physicalProcessorCount);
}

extern "C" uint32_t kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
    uint64_t limitBytes,
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return uml::runtime::guest_memory::maximum_guest_memory_mib(
        limitBytes,
        guestProcessorCount,
        launchMemorySet(parameters),
        guestMemoryCeilingMiB());
}

extern "C" uint32_t kernarium_runtime_loader_guest_memory_ceiling_mib(void)
{
    return guestMemoryCeilingMiB();
}

extern "C" uint32_t kernarium_runtime_loader_guest_memory_admitted(
    uint32_t memoryMiB)
{
    return uml::runtime::guest_memory::guest_memory_admitted(
               memoryMiB, guestMemoryCeilingMiB())
        ? 1
        : 0;
}

extern "C" kernarium_guest_memory_range
kernarium_runtime_loader_guest_memory_range(void)
{
    namespace budget = uml::runtime::guest_memory;
    return {
        .minimum_mib = budget::guest_memory_minimum_mib,
        .step_mib = budget::guest_memory_step_mib,
        .largest_mib = budget::guest_memory_range_maximum_mib,
    };
}

extern "C" uint32_t kernarium_runtime_loader_maximum_guest_memory_mib(
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
        uml::platform::os_memory_limit_bytes(),
        guestProcessorCount,
        parameters);
}

extern "C" uint32_t kernarium_runtime_loader_default_guest_memory_mib_for_limit(
    uint64_t limitBytes,
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return uml::runtime::guest_memory::default_guest_memory_mib(
        limitBytes,
        guestProcessorCount,
        launchMemorySet(parameters),
        guestMemoryCeilingMiB());
}

extern "C" uint32_t kernarium_runtime_loader_default_guest_memory_mib(
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return kernarium_runtime_loader_default_guest_memory_mib_for_limit(
        uml::platform::os_memory_limit_bytes(),
        guestProcessorCount,
        parameters);
}

extern "C" uint32_t
kernarium_runtime_loader_default_guest_memory_holds_gpu_budget_for_limit(
    uint64_t limitBytes,
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return uml::runtime::guest_memory::default_guest_memory(
               limitBytes,
               guestProcessorCount,
               launchMemorySet(parameters),
               guestMemoryCeilingMiB())
                   .holds_gpu_budget
        ? 1
        : 0;
}

extern "C" uint32_t
kernarium_runtime_loader_default_guest_memory_holds_gpu_budget(
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return kernarium_runtime_loader_default_guest_memory_holds_gpu_budget_for_limit(
        uml::platform::os_memory_limit_bytes(),
        guestProcessorCount,
        parameters);
}

extern "C" uint32_t
kernarium_runtime_loader_conservative_guest_memory_mib_for_limit(
    uint64_t limitBytes,
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return uml::runtime::guest_memory::conservative_guest_memory_mib(
        limitBytes,
        guestProcessorCount,
        launchMemorySet(parameters),
        guestMemoryCeilingMiB());
}

extern "C" uint32_t kernarium_runtime_loader_conservative_guest_memory_mib(
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return kernarium_runtime_loader_conservative_guest_memory_mib_for_limit(
        uml::platform::os_memory_limit_bytes(),
        guestProcessorCount,
        parameters);
}

extern "C" uint32_t kernarium_runtime_loader_gpu_budget_mib_for_limit(
    uint64_t limitBytes,
    uint32_t guestMemoryMiB,
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    namespace budget = uml::runtime::guest_memory;
    return static_cast<uint32_t>(
        budget::gpu_budget(
            limitBytes,
            guestMemoryMiB,
            guestProcessorCount,
            launchMemorySet(parameters))
            .bytes /
        budget::mebibyte);
}

extern "C" uint32_t kernarium_runtime_loader_gpu_budget_mib(
    uint32_t guestMemoryMiB,
    uint32_t guestProcessorCount,
    kernarium_launch_memory_set parameters)
{
    return kernarium_runtime_loader_gpu_budget_mib_for_limit(
        uml::platform::os_memory_limit_bytes(),
        guestMemoryMiB,
        guestProcessorCount,
        parameters);
}

extern "C" uint32_t kernarium_runtime_loader_camera_reserve_mib(
    uint32_t gpuBudgetMiB)
{
    return static_cast<uint32_t>(uml::camera::cameraHostMemoryReserveMiB(
        uml::camera::cameraTierForBudget(
            static_cast<uint64_t>(gpuBudgetMiB) << 20)));
}

extern "C" uint32_t kernarium_runtime_loader_plan_camera_reserve_mib(
    uint32_t gpuBudgetMiB,
    const char* calibrationPath,
    uint32_t rediscover,
    uint32_t* exact)
{
    const auto ceiling = [&]() -> uint32_t {
        if (exact != nullptr)
            *exact = 0;
        return kernarium_runtime_loader_camera_reserve_mib(gpuBudgetMiB);
    };
    const kernarium_runtime_api* runtime = api();
    if (runtime == nullptr)
        return ceiling();
    kernarium_runtime_camera_reserve plan {};
    plan.struct_size = sizeof(plan);
    if (rediscover != 0)
        plan.flags = KERNARIUM_RUNTIME_CAMERA_PLAN_DISCOVER_AGAIN;
    if (runtime->plan_camera_reserve(gpuBudgetMiB, calibrationPath, &plan) != 0 ||
        plan.basis != KERNARIUM_RUNTIME_CAMERA_RESERVE_EXACT)
        return ceiling();
    if (exact != nullptr)
        *exact = 1;
    return plan.reserve_mib;
}

extern "C" int kernarium_runtime_loader_set_microphone(int enabled)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->set_microphone(enabled);
}

extern "C" void kernarium_runtime_loader_send_touch(
    uint32_t slot,
    uint32_t x,
    uint32_t y,
    int pressed)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->send_touch(slot, x, y, pressed);
}

extern "C" void kernarium_runtime_loader_cancel_touch(uint32_t slot)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->cancel_touch(slot);
}

extern "C" void kernarium_runtime_loader_send_key(uint16_t code, int pressed)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        runtime->send_key(code, pressed);
}

extern "C" void kernarium_runtime_loader_set_console_output_handler(
    KernariumConsoleOutputHandler handler)
{
    {
        std::lock_guard lock(consoleOutput.mutex);
        consoleOutput.handler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateConsoleOutputForwarder(runtime);
}

extern "C" void kernarium_runtime_loader_set_restart_handler(
    KernariumRestartHandler handler)
{
    {
        std::lock_guard lock(restartHandlerMutex);
        restartHandler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateRestartForwarder(runtime);
}

extern "C" void kernarium_runtime_loader_set_location_receiver_handler(
    KernariumLocationReceiverHandler handler)
{
    {
        std::lock_guard lock(locationHandlerMutex);
        locationHandler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateLocationReceiverForwarder(runtime);
}

extern "C" void kernarium_runtime_loader_set_camera_state_handler(
    KernariumCameraStateHandler handler)
{
    {
        std::lock_guard lock(cameraStateHandlerMutex);
        cameraStateHandler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateCameraStateForwarder(runtime);
}

extern "C" void kernarium_runtime_loader_set_memory_pressure_handler(
    KernariumMemoryPressureHandler handler)
{
    {
        std::lock_guard lock(memoryPressure.handlerMutex);
        memoryPressure.handler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateMemoryPressureForwarder(runtime);
}

extern "C" int kernarium_runtime_loader_set_camera_rotation(uint32_t degrees)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -ENODEV : runtime->set_camera_rotation(degrees);
}

extern "C" void kernarium_runtime_loader_set_input_status_handler(
    KernariumInputStatusHandler handler)
{
    {
        std::lock_guard lock(inputStatusHandlerMutex);
        inputStatusHandler = [handler copy];
    }
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr)
        updateInputStatusForwarder(runtime);
}

extern "C" int kernarium_runtime_loader_set_input_connected(
    int device,
    BOOL connected)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -ENODEV : runtime->set_input_connected(device, connected ? 1 : 0);
}

extern "C" int kernarium_runtime_loader_send_keyboard_usage(
    uint32_t usage,
    BOOL pressed)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -ENODEV : runtime->send_keyboard_usage(usage, pressed ? 1 : 0);
}

extern "C" int kernarium_runtime_loader_send_pointer(
    uint32_t x,
    uint32_t y,
    uint32_t buttons,
    BOOL inRange)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -ENODEV : runtime->send_pointer(x, y, buttons, inRange ? 1 : 0);
}

extern "C" int kernarium_runtime_loader_send_mouse(
    int32_t dx,
    int32_t dy,
    uint32_t buttons,
    int32_t wheel,
    int32_t horizontalWheel)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -ENODEV :
        runtime->send_mouse(dx, dy, buttons, wheel, horizontalWheel);
}

extern "C" int kernarium_runtime_loader_report_location(
    const kernarium_runtime_location_fix* fix)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -ENODEV : runtime->report_location(fix);
}

extern "C" void kernarium_runtime_loader_wake_for_input(void)
{
    /* Optional: the wake is a convenience, and a table without the entry still
     * runs the guest. */
    const kernarium_runtime_api* runtime = api();
    if (runtime != nullptr && runtime->wake_for_input != nullptr)
        runtime->wake_for_input();
}

extern "C" int kernarium_runtime_loader_set_network_notice(int notice)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -ENODEV : runtime->set_network_notice(notice);
}

extern "C" int kernarium_runtime_loader_connect_vsock(
    uint32_t port,
    uint32_t timeoutMilliseconds)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ?
        -ENODEV :
        runtime->connect_vsock(port, timeoutMilliseconds);
}

extern "C" int kernarium_runtime_loader_automation_hold_delay_ms(void)
{
    return api() != nullptr && automationHoldDelayMs != nullptr
        ? automationHoldDelayMs()
        : 0;
}

extern "C" int kernarium_runtime_loader_prepare_jit(
    int enable,
    uint32_t guestMemoryMiB,
    uint32_t guestProcessorCount)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr
        ? KERNARIUM_RUNTIME_JIT_UNSUPPORTED
        : runtime->prepare_jit(enable, guestMemoryMiB, guestProcessorCount);
}

extern "C" int kernarium_runtime_loader_debugger_attached(void)
{
    struct kinfo_proc information {};
    std::size_t length = sizeof(information);
    int name[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(name, 4, &information, &length, nullptr, 0) != 0)
        return 0;
    return (information.kp_proc.p_flag & P_TRACED) != 0 ? 1 : 0;
}

extern "C" int kernarium_runtime_loader_guest_display_active(void)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -1 : runtime->guest_display_active();
}

extern "C" int kernarium_runtime_loader_read_statistics(
    kernarium_runtime_statistics* statistics)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -ENODEV : runtime->read_statistics(statistics);
}

extern "C" int kernarium_runtime_loader_read_graphics_memory(
    kernarium_runtime_graphics_memory* memory)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -ENODEV : runtime->read_graphics_memory(memory);
}

extern "C" int kernarium_runtime_loader_read_guest_memory(
    kernarium_runtime_guest_memory* memory)
{
    const kernarium_runtime_api* runtime = api();
    return runtime == nullptr ? -ENODEV : runtime->read_guest_memory(memory);
}

extern "C" int kernarium_runtime_loader_automation_job_at_start(void)
{
    return api() != nullptr && automationJobAtStart != nullptr
        ? automationJobAtStart()
        : 0;
}

extern "C" int kernarium_runtime_loader_send_console_input(
    const void* buffer,
    std::size_t length)
{
    const kernarium_runtime_api* runtime = api();
    if (runtime == nullptr)
        return -1;
    return runtime->send_console_input(buffer, length);
}
