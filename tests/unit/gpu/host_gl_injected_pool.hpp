// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstdint>

/* The pool a test states for the host's Vulkan driver in place of the
 * renderer's, to refuse the allocations of Mesa's own GL driver on cue.
 *
 * KosmicKrisp charges every device memory allocation of the process to the
 * pool it is given, and fails the allocation when the pool refuses it. This
 * pool counts only what the test's own objects hold: the bytes of device
 * memory that are live (the driver's count of the heaps it made and has not
 * destroyed), which a freed texture or buffer leaves at once, as the memory
 * the host's budget counts lags behind. A test limits the room above what is
 * live when it says so, and everything that would take the heaps past that
 * is refused until it removes the limit. Only requests of a mebibyte or more
 * are refused, so the small allocations a context makes for itself go on.
 *
 * The declarations are those of KosmicKrisp's kk_device_memory_pool.h, an
 * interface between two libraries of the prefix that is not installed with
 * it. */
extern "C" {

#define UML_STRUCTURE_TYPE_DEVICE_MEMORY_POOL_CREATE_INFO_KK \
    (static_cast<VkStructureType>(1000237900))

struct UmlDeviceMemoryPoolCreateInfoKK {
    VkStructureType sType;
    const void* pNext;
    void* pUserData;
    VkBool32(VKAPI_PTR* pfnReserve)(void* userData, VkDeviceSize size);
    void(VKAPI_PTR* pfnRelease)(void* userData, VkDeviceSize size);
    VkDeviceSize(VKAPI_PTR* pfnHeadroom)(void* userData);
};

void kk_set_device_memory_pool(const UmlDeviceMemoryPoolCreateInfoKK* pool);
std::uint64_t kk_resident_heap_bytes(void);

} // extern "C"

namespace uml::gpu::testing {

class InjectedPool {
public:
    static constexpr std::uint64_t refusedFrom = 1ULL << 20;

    InjectedPool()
    {
        const UmlDeviceMemoryPoolCreateInfoKK pool {
            UML_STRUCTURE_TYPE_DEVICE_MEMORY_POOL_CREATE_INFO_KK,
            nullptr,
            this,
            &InjectedPool::reserve,
            &InjectedPool::release,
            &InjectedPool::headroom,
        };
        kk_set_device_memory_pool(&pool);
    }

    ~InjectedPool() { kk_set_device_memory_pool(nullptr); }

    InjectedPool(const InjectedPool&) = delete;
    InjectedPool& operator=(const InjectedPool&) = delete;

    /* The device memory that is live. */
    [[nodiscard]] static std::uint64_t live() noexcept
    {
        return kk_resident_heap_bytes();
    }

    /* Refuses what would take the live memory more than `room` bytes above
     * what it is now. */
    void limit(std::uint64_t room) noexcept
    {
        ceiling_.store(live() + room);
    }

    /* Refuses nothing. */
    void unlimit() noexcept { ceiling_.store(UINT64_MAX); }

    /* The room left under the limit. */
    [[nodiscard]] std::uint64_t room() const noexcept
    {
        const std::uint64_t ceiling = ceiling_.load();
        const std::uint64_t held = live() + reserved_.load();
        return held >= ceiling ? 0 : ceiling - held;
    }

    /* How many requests have been refused. */
    [[nodiscard]] unsigned refusals() const noexcept
    {
        return refusals_.load();
    }

private:
    static VkBool32 VKAPI_CALL reserve(void* data, VkDeviceSize size)
    {
        auto& self = *static_cast<InjectedPool*>(data);
        if (size >= refusedFrom && size > self.room()) {
            self.refusals_.fetch_add(1);
            return VK_FALSE;
        }
        self.reserved_.fetch_add(size);
        return VK_TRUE;
    }

    static void VKAPI_CALL release(void* data, VkDeviceSize size)
    {
        static_cast<InjectedPool*>(data)->reserved_.fetch_sub(size);
    }

    static VkDeviceSize VKAPI_CALL headroom(void* data)
    {
        // A headroom the driver sizes its own heaps by: large, not unbounded.
        constexpr VkDeviceSize ample = 64ULL << 30;
        const VkDeviceSize room = static_cast<InjectedPool*>(data)->room();
        return room < ample ? room : ample;
    }

    std::atomic<std::uint64_t> ceiling_ {UINT64_MAX};
    std::atomic<std::uint64_t> reserved_ {};
    std::atomic<unsigned> refusals_ {};
};

} // namespace uml::gpu::testing
