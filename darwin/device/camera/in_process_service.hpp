// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera_device.hpp"
#include "in_process_vhost_user_service.hpp"
#include "shared_memory_regions.hpp"

#include <memory>

namespace uml::camera {

/* The camera service: one transport thread around a VirtioCameraDevice, device
 * ID `cameraDeviceId`. The device is made on the service thread, so a host
 * whose discovery is slow does not hold the caller. `regions` is the
 * shared-memory index of the backend that owns the guest's host-visible
 * region, when the guest has one; with it a still can be written into a buffer
 * that lives there. */
[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessCameraService(
    std::shared_ptr<uml::platform::Camera> host, CameraDeviceOptions options,
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions = {});

} // namespace uml::camera
