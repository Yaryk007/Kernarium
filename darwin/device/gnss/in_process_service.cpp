// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "vhost_user_server.hpp"
#include "virtio_gnss_device.hpp"

namespace uml::gnss {

InProcessService makeInProcessService(std::unique_ptr<FixSource> source)
{
    InProcessService result;
    result.endpoint = std::make_shared<Endpoint>(std::move(source));
    result.service = std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio GNSS", "uml-gnss", [endpoint = result.endpoint] {
            return std::make_unique<VirtioGnssDevice>(endpoint);
        });
    return result;
}

} // namespace uml::gnss
