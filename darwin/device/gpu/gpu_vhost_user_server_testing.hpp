// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "gpu_vhost_user_server.hpp"
#include "virtio_gpu_device_testing.hpp"

#include <cstdint>
#include <memory>

namespace uml::gpu::testing {

/* The Virtio-GPU backend of gpu_vhost_user_server.cpp built with
 * UML_GPU_DEVICE_TESTING, whose device runs on a test's renderer and display
 * dependencies instead of virglrenderer: the production device behind the
 * production transport, with nothing on the host's GPU. The dependencies
 * outlive the server. */
[[nodiscard]] std::unique_ptr<VhostUserServer> makeGpuVhostUserServerOnDependencies(
    std::shared_ptr<Display> display,
    VirtioGpuDeviceDependencies& dependencies,
    std::uint32_t dotsPerInch = 0);

} // namespace uml::gpu::testing
