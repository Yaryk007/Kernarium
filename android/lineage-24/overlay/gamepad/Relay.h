// SPDX-License-Identifier: Apache-2.0
//
// The gamepad relay: the host application's game controllers become HID devices
// of the guest. The relay listens on vsock port 5558 and accepts the host (CID
// 2) and nothing else. For each CREATE the host sends it opens /dev/uhid and
// creates the device the host describes, which the kernel's own driver for that
// device binds; it moves the host's input reports into the device and the
// device's output and feature requests back to the host. One connection is live
// at a time: a host that connects again replaces the one before it, and a
// connection that ends takes every device with it, so Android sees exactly the
// controllers of the host that is there.
//
// The relay is one thread that waits for the listener, the connection and every
// device's descriptor at once, with no timeout: it never wakes on its own. It
// knows no controller family, and the wire is gamepad_protocol.hpp's.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <android-base/unique_fd.h>
#include <linux/uhid.h>

#include "gamepad_protocol.hpp"

namespace andarium::gamepad {
class Relay {
public:
    Relay() = default;
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    // Serves until the listener fails, and returns the exit status then (init
    // starts the service again).
    int run();

private:
    // What the log line at a disconnect shows.
    struct Counters {
        uint32_t frames = 0;
        uint32_t created = 0;
        uint32_t create_failed = 0;
        uint32_t input = 0;
        uint32_t input_dropped = 0;
        uint32_t input_failed = 0;
        uint32_t output = 0;
        uint32_t requests = 0;
        uint32_t replies = 0;
    };
    struct Slot {
        android::base::unique_fd uhid;
        // Failed writes of input reports on this device, for the log's rate limit.
        uint32_t input_failures = 0;
    };

    bool listen_on_vsock();
    // Accepts a connection. Returns false when the listener failed for good.
    bool accept_host();
    void read_host();
    void service_slot(std::size_t slot);

    bool handle_frame(Type type, std::span<const uint8_t> payload);
    void create_slot(const Create& create);
    void destroy_slot(std::size_t slot);
    void write_input(const Input& input);
    void write_reply(std::size_t slot, std::size_t size);
    void handle_uhid_event(std::size_t slot, const uhid_event& event);
    // Answers a request of the driver that the host cannot get, with EIO at
    // once, so that the driver never waits out uhid's five seconds.
    void refuse_request(std::size_t slot, const uhid_event& event);

    // Sends one whole frame to the host without blocking. A frame that cannot
    // be sent whole leaves the stream broken, so the connection is shut down
    // and dropped at the end of the turn.
    bool send_frame(std::span<const uint8_t> frame);
    void drop_host();

    android::base::unique_fd listener_;
    android::base::unique_fd host_;
    FrameReader reader_;
    std::array<Slot, slot_count> slots_;
    bool broken_ = false;
    Counters counters_;
    // Scratch events, so that no turn puts 4 KiB on the stack: one read from a
    // device, one written to it.
    uhid_event in_{};
    uhid_event out_{};
};
}  // namespace andarium::gamepad
