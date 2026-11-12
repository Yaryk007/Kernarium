// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace uml::platform {

/* Bytes the host GPU holds for this process right now: every buffer, texture
 * and heap on the system Metal device, whoever allocated it, from its
 * creation, including no-copy buffers over shared memory and textures over
 * IOSurfaces. Zero without a Metal device. The query reads a counter the
 * driver maintains and is cheap enough to make per allocation. */
[[nodiscard]] std::uint64_t hostGpuAllocatedBytes() noexcept;

/* Bytes of this process's footprint the kernel charges as graphics memory:
 * the resident pages of Metal resources, and also the pages the GPU driver has
 * written in its own storage, such as Metal 4 command allocators, which the
 * device counter above omits. A heap counts here only once its pages are
 * resident, and IOSurface memory never does. Zero when the kernel does not
 * report it. One task_info call. */
[[nodiscard]] std::uint64_t hostGraphicsFootprintBytes() noexcept;

/* Gives back the graphics footprint of Metal resources this process has freed.
 * The pages of a heap made resident through a residency set stay in the
 * footprint after the heap is freed, though the device counter has dropped,
 * until the process next asks for an allocation to be made resident (an empty
 * set or an empty command buffer does not do it). This makes that request for
 * a small buffer of its own, and the footprint falls a few milliseconds later.
 * With a `wait` it polls for that, returning once the footprint has fallen by a
 * mebibyte or more or the time has passed; without, it returns at once. Any
 * thread. Does nothing without a Metal device or a device that makes no
 * residency sets, and does not wait while the footprint is under a mebibyte,
 * which is all the iOS Simulator reports. */
void retireFreedGpuMemory(std::chrono::milliseconds wait = {}) noexcept;

} // namespace uml::platform
