// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "endpoint.hpp"
#include "in_process_vhost_user_service.hpp"

#include <memory>

namespace uml::gnss {

/* The device's service and the receiver's two ends. The endpoint outlives the
 * service. */
struct InProcessService final {
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service;
    std::shared_ptr<Endpoint> endpoint;
};

/* Starts the device on a service thread named "uml-gnss". A non-null
 * `source` replaces the host's own fixes (Endpoint). */
[[nodiscard]] InProcessService makeInProcessService(
    std::unique_ptr<FixSource> source = {});

} // namespace uml::gnss
