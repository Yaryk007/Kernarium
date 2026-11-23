// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "vhost_user_server.hpp"

namespace uml::input::virtio {

namespace {

const char* threadName(DeviceKind kind) noexcept
{
    switch (kind) {
    case DeviceKind::touchscreen:
        return "uml-touch";
    case DeviceKind::keyboard:
        return "uml-keyboard";
    case DeviceKind::hardwareKeyboard:
        return "uml-hwkeyboard";
    case DeviceKind::mouse:
        return "uml-mouse";
    case DeviceKind::pointer:
        return "uml-pointer";
    }
    return "uml-input";
}

} // namespace

InProcessInputService::InProcessInputService(DeviceKind kind)
    : events_(std::make_shared<EventQueue>(
          pluggable(kind) ? EventQueue::Attachment::pluggable :
                            EventQueue::Attachment::fixed))
    , service_(std::make_unique<uml::gpu::InProcessVhostUserService>(
          "virtio-input",
          threadName(kind),
          [kind, events = events_] {
              return std::make_unique<VirtioInputDevice>(kind, events);
          }))
{
}

InProcessInputService::~InProcessInputService() = default;

std::string InProcessInputService::takeConnectionName()
{
    return service_->takeConnectionName();
}

void InProcessInputService::send(
    std::span<const protocol::InputEvent> events) noexcept
{
    events_->enqueue(events);
}

void InProcessInputService::stop() noexcept
{
    service_->stop();
}

} // namespace uml::input::virtio
