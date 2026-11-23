// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "hardware_input.hpp"

#include <memory>
#include <mutex>

namespace uml::input::virtio {

/* The hardware keyboard and mouse of one run of a guest, across its boots.
 *
 * A boot builds its devices unplugged, and a guest that restarts itself is a
 * new boot, so what the application asked for would be lost with the boot
 * that carried it. The run keeps the last request for each device from its
 * beginning to its end, and plugs it into the input of every boot that
 * attaches, so a restarted guest has the devices plugged the way the
 * application left them. The plug itself waits for the driver's probe
 * (EventQueue), so it never reaches a guest before that guest can answer.
 *
 * Every method may be called from any thread. */
class HardwareInputRun final {
public:
    enum class Device : std::uint8_t {
        keyboard,
        /* The mouse and the pointer, which are one mouse to Android. */
        mouse,
    };

    /* A run begins, and requests are accepted until it ends; its requests are
     * forgotten at the end. */
    void begin();
    void end();

    /* A boot's input exists until `detach`, which returns it. Attaching plugs
     * what the application has asked for. */
    void attach(std::shared_ptr<HardwareInput> input);
    std::shared_ptr<HardwareInput> detach();

    /* Records the request and applies it to the attached input, if a boot has
     * one. False outside a run. */
    [[nodiscard]] bool request(Device device, bool connected);

    /* The attached input: none between boots and none outside a run. */
    [[nodiscard]] std::shared_ptr<HardwareInput> current() const;

private:
    void applyLocked(Device device);

    mutable std::mutex mutex_;
    bool running_ {};
    bool keyboard_ {};
    bool mouse_ {};
    std::shared_ptr<HardwareInput> input_;
};

} // namespace uml::input::virtio
