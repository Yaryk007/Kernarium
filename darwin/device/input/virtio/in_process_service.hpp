// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "virtio_input_device.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace uml::gpu {
class InProcessVhostUserService;
}

namespace uml::input::virtio {

class InProcessInputService final {
public:
    explicit InProcessInputService(DeviceKind kind);
    ~InProcessInputService();

    InProcessInputService(const InProcessInputService&) = delete;
    InProcessInputService& operator=(const InProcessInputService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void send(std::span<const protocol::InputEvent> events) noexcept;
    /* States the device's one switch before the guest probes it
     * (EventQueue::setSwitch). */
    void setSwitch(EventQueue::Switch state) noexcept { events_->setSwitch(state); }
    /* The host's side of the device, for what a plain send does not cover:
     * plugging, the status sink and the registration (EventQueue). */
    [[nodiscard]] const std::shared_ptr<EventQueue>& events() const noexcept
    {
        return events_;
    }
    void stop() noexcept;

private:
    std::shared_ptr<EventQueue> events_;
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service_;
};

} // namespace uml::input::virtio
