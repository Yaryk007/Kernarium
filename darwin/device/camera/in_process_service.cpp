// SPDX-License-Identifier: GPL-2.0-or-later
#include "in_process_service.hpp"

#include "vhost_user_server.hpp"

#include <stdexcept>
#include <utility>

namespace uml::camera {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessCameraService(
    std::shared_ptr<uml::platform::Camera> host, CameraDeviceOptions options,
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
{
    if (!host) throw std::invalid_argument("a platform camera is required");
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio camera", "uml-camera",
        [host = std::move(host), options = std::move(options)]() mutable {
            return std::make_unique<VirtioCameraDevice>(host, options);
        },
        std::move(regions));
}

} // namespace uml::camera
