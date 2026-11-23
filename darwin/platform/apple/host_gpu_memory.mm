// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gpu_memory.hpp"

#import <Metal/Metal.h>
#include <mach/mach.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace uml::platform {

namespace {

/* Apple silicon has one GPU and Metal hands every caller the same device
 * object. */
id<MTLDevice> systemDevice() noexcept
{
    static id<MTLDevice> const device = MTLCreateSystemDefaultDevice();
    return device;
}

} // namespace

std::uint64_t hostGpuAllocatedBytes() noexcept
{
    /* Its counter covers the renderer, the host driver and the presentation
     * path alike. */
    id<MTLDevice> const device = systemDevice();
    return device == nil ? 0 : static_cast<std::uint64_t>(device.currentAllocatedSize);
}

std::uint64_t hostGraphicsFootprintBytes() noexcept
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
        count < TASK_VM_INFO_COUNT)
        return 0;
    const std::int64_t bytes = info.ledger_tag_graphics_footprint +
        info.ledger_tag_graphics_footprint_compressed;
    return static_cast<std::uint64_t>(std::max<std::int64_t>(bytes, 0));
}

namespace {

/* What the retire asks to be made resident: a residency set holding one small
 * buffer, kept for the life of the process and never freed. */
struct Retirement {
    std::mutex mutex;
    id<MTLResidencySet> set = nil;
    id<MTLBuffer> buffer = nil;
    bool unavailable = false;
};

/* Makes the set on first use and returns whether there is one. A device that
 * cannot make it is not asked again. */
bool prepare(Retirement& retirement) noexcept
{
    if (retirement.set != nil)
        return true;
    if (retirement.unavailable)
        return false;
    retirement.unavailable = true;
    id<MTLDevice> const device = systemDevice();
    if (device == nil ||
        ![device respondsToSelector:@selector(newResidencySetWithDescriptor:error:)])
        return false;
    NSError* error = nil;
    id<MTLResidencySet> const set =
        [device newResidencySetWithDescriptor:[[MTLResidencySetDescriptor alloc] init]
                                        error:&error];
    id<MTLBuffer> const buffer = [device newBufferWithLength:16 * 1024
                                                     options:MTLResourceStorageModeShared];
    if (set == nil || buffer == nil)
        return false;
    [set addAllocation:buffer];
    [set commit];
    retirement.set = set;
    retirement.buffer = buffer;
    return true;
}

} // namespace

void retireFreedGpuMemory(std::chrono::milliseconds wait) noexcept
{
    static Retirement* const retirement = new Retirement;
    const std::uint64_t before = hostGraphicsFootprintBytes();
    @autoreleasepool {
        std::lock_guard lock(retirement->mutex);
        if (!prepare(*retirement))
            return;
        [retirement->set requestResidency];
        [retirement->set endResidency];
    }
    /* The footprint falls a few milliseconds after the request. */
    constexpr std::uint64_t fall = 1U << 20;
    if (before < fall)
        return;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (hostGraphicsFootprintBytes() + fall <= before)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace uml::platform
