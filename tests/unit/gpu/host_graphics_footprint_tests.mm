// SPDX-License-Identifier: GPL-2.0-or-later
/* A Metal 4 command allocator keeps what its command buffers encoded until it
 * is released, and that storage never shows in the device's allocated size.
 * One large recording once held gigabytes of it and pushed an iOS app past
 * its memory limit, while the device memory budget, counting the allocated
 * size alone, did not see it. The graphics footprint charges the storage that
 * recorded commands wrote, which is why the budget's usage takes the larger of
 * the two. A heap made resident keeps its pages in the footprint after it is
 * freed until the process next makes an allocation resident, which
 * retireFreedGpuMemory() does. */

#include "host_gpu_memory.hpp"
#include "require.hpp"

#import <Metal/Metal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using uml::testing::require;

constexpr double mib = 1024.0 * 1024.0;
/* Dense recordings write nearly all of the storage they grow; an encoder
 * with no commands reserves far more than it writes. */
constexpr unsigned encoders = 16;
constexpr unsigned copiesPerEncoder = 65536;

/* Grows one allocator by recording many copy commands and returns its
 * storage, checking what the graphics footprint charges while it is held. */
std::uint64_t recordAndCheck(id<MTLDevice> device,
    std::uint64_t footprintBefore, std::uint64_t allocatedBefore)
{
    @autoreleasepool {
        id<MTLBuffer> source = [device newBufferWithLength:4096
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> destination = [device newBufferWithLength:4096
                                                        options:MTLResourceStorageModeShared];
        id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
        id<MTL4CommandBuffer> commands = [device newCommandBuffer];
        require(source != nil && destination != nil && allocator != nil &&
                commands != nil,
            "no buffers, command allocator or command buffer");
        [commands beginCommandBufferWithAllocator:allocator];
        for (unsigned index = 0; index < encoders; ++index) {
            id<MTL4ComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            require(encoder != nil, "no compute encoder");
            for (unsigned copy = 0; copy < copiesPerEncoder; ++copy) {
                const NSUInteger offset = (copy % 256) * 16;
                [encoder copyFromBuffer:source
                           sourceOffset:offset
                               toBuffer:destination
                      destinationOffset:offset
                                   size:16];
            }
            [encoder endEncoding];
        }
        [commands endCommandBuffer];
        const std::uint64_t storage = allocator.allocatedSize;

        const std::uint64_t allocated = uml::platform::hostGpuAllocatedBytes();
        const std::uint64_t footprint = uml::platform::hostGraphicsFootprintBytes();
        std::printf("%u encoders of %u copies: allocator %.1f MiB, allocated size %+.1f MiB, "
                    "graphics footprint %+.1f MiB\n",
            encoders, copiesPerEncoder, storage / mib,
            (static_cast<double>(allocated) - allocatedBefore) / mib,
            (static_cast<double>(footprint) - footprintBefore) / mib);
        require(storage >= 64 * 1024 * 1024,
            "the recording grew its allocator by less than 64 MiB");
        require(footprint >= footprintBefore + storage * 9 / 10,
            "the graphics footprint omits the allocator's storage");
        return storage;
    }
}

/* A heap made resident through a residency set, as a pooled KosmicKrisp
 * device makes each of its heaps, keeps its pages in the graphics footprint
 * after it is freed, until the process next makes an allocation resident.
 * retireFreedGpuMemory() is that request, so the footprint follows the free. */
void checkRetire(id<MTLDevice> device)
{
    constexpr std::uint64_t size = 64 * 1024 * 1024;
    const std::uint64_t before = uml::platform::hostGraphicsFootprintBytes();
    @autoreleasepool {
        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.size = size;
        descriptor.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
        NSError* error = nil;
        id<MTLResidencySet> set = [device
            newResidencySetWithDescriptor:[[MTLResidencySetDescriptor alloc] init]
                                    error:&error];
        require(heap != nil && set != nil, "no heap or residency set");
        [set addAllocation:heap];
        [set commit];
        [set requestResidency];
        [set endResidency];
        require(uml::platform::hostGraphicsFootprintBytes() >= before + size * 9 / 10,
            "a resident heap is missing from the graphics footprint");
    }
    const std::uint64_t freed = uml::platform::hostGraphicsFootprintBytes();
    uml::platform::retireFreedGpuMemory(std::chrono::milliseconds {100});
    const std::uint64_t retired = uml::platform::hostGraphicsFootprintBytes();
    std::printf("resident heap freed: graphics footprint %+.1f MiB, after the retire %+.1f MiB\n",
        (static_cast<double>(freed) - before) / mib,
        (static_cast<double>(retired) - before) / mib);
    require(retired < before + size / 2,
        "a freed resident heap stays in the graphics footprint after the retire");
}

/* A retire with nothing freed gives back nothing that is in use, and one given
 * no wait returns without polling: a refusal with nothing to give back stalls
 * its caller for no time. A live resident heap keeps the footprint above the
 * poll's threshold, so a call that polled would wait out the time it was
 * given. */
void checkNothingFreed(id<MTLDevice> device)
{
    constexpr std::uint64_t size = 64 * 1024 * 1024;
    @autoreleasepool {
        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.size = size;
        descriptor.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
        NSError* error = nil;
        id<MTLResidencySet> set = [device
            newResidencySetWithDescriptor:[[MTLResidencySetDescriptor alloc] init]
                                    error:&error];
        require(heap != nil && set != nil, "no heap or residency set");
        [set addAllocation:heap];
        [set commit];
        [set requestResidency];
        [set endResidency];
        const std::uint64_t held = uml::platform::hostGraphicsFootprintBytes();
        require(held >= size, "a resident heap is missing from the graphics footprint");
        constexpr int calls = 20;
        const auto begun = std::chrono::steady_clock::now();
        for (int call = 0; call < calls; ++call)
            uml::platform::retireFreedGpuMemory();
        const auto elapsed = std::chrono::steady_clock::now() - begun;
        const std::uint64_t after = uml::platform::hostGraphicsFootprintBytes();
        std::printf("%d retires with nothing freed: %.1f ms, graphics footprint %+.1f MiB\n", calls,
            std::chrono::duration<double, std::milli>(elapsed).count(),
            (static_cast<double>(after) - held) / mib);
        require(after + size / 2 >= held, "a retire gave back a heap that is still in use");
        require(elapsed < std::chrono::milliseconds {50} * calls,
            "a retire given no wait waited for the footprint to fall");
    }
}

} // namespace

int main()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::puts("SKIP host graphics footprint: no Metal device");
            return 77;
        }
        const std::uint64_t allocatedBefore =
            uml::platform::hostGpuAllocatedBytes();
        const std::uint64_t footprintBefore =
            uml::platform::hostGraphicsFootprintBytes();
        const std::uint64_t storage = recordAndCheck(device,
            footprintBefore, allocatedBefore);

        /* Releasing the allocator returns its storage. */
        const std::uint64_t released =
            uml::platform::hostGraphicsFootprintBytes();
        std::printf("released: graphics footprint %+.1f MiB against the start\n",
            (static_cast<double>(released) - footprintBefore) / mib);
        require(released < footprintBefore + storage / 2,
            "the released allocator stays in the graphics footprint");

        checkRetire(device);
        checkNothingFreed(device);
    }
    std::puts("PASS host graphics footprint");
    return 0;
}
