// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera_device.hpp"

#include <mach/mach.h>

#include <cstdint>
#include <cstdio>

namespace uml::camera {

/* Diagnostic readings for a development run (the launcher and the runtime
 * print them). */

[[nodiscard]] constexpr const char* checkpointName(CameraCheckpoint where) noexcept
{
    switch (where) {
    case CameraCheckpoint::sessionOpened:
        return "capture session opened";
    case CameraCheckpoint::stillDelivered:
        return "still delivered";
    case CameraCheckpoint::sessionClosed:
        return "capture session closed";
    }
    return "unknown";
}

/* The process's physical footprint in bytes, the figure the system holds
 * against the process's memory limit (task_vm_info's phys_footprint); zero when
 * it cannot be read. */
[[nodiscard]] inline std::uint64_t physicalFootprintBytes() noexcept
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

/* The hooks of a development run, printed to stderr: what ROTATE_AND_CROP
 * values reach the host, what the guest asks of a camera's torch and flash, and
 * the footprint at each checkpoint. A launcher or a runtime assigns
 * them to the device's options. */
inline void printRotateAndCrop(std::uint32_t camera, std::uint32_t setting, std::uint32_t degrees) noexcept
{
    std::fprintf(stderr, "Camera: camera %u: ROTATE_AND_CROP %u, picture turned %u degrees\n", camera, setting, degrees);
}

/* What the guest asks of a camera's light: the platform prints nothing for the
 * torch or the flash. */
inline void printFlash(std::uint32_t camera, bool torch, uml::platform::CameraFlashMode flash) noexcept
{
    const char* mode = flash == uml::platform::CameraFlashMode::on ? "on"
        : flash == uml::platform::CameraFlashMode::automatic      ? "automatic"
                                                                  : "off";
    std::fprintf(stderr, "Camera: camera %u: torch %s, still flash %s\n", camera, torch ? "on" : "off", mode);
}

inline void printCheckpoint(CameraCheckpoint where) noexcept
{
    std::fprintf(stderr, "Camera: memory: %s, footprint %llu MiB\n", checkpointName(where),
                 static_cast<unsigned long long>(physicalFootprintBytes() >> 20));
}

} // namespace uml::camera
