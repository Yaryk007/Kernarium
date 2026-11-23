// SPDX-License-Identifier: Apache-2.0
//
// The guest end of the private haptics stream: the HAL listens on vsock port
// 5557, accepts the host application (CID 2) and nothing else, sends its frames
// and reads the host's ACKs. One host is connected at a time, and a host that
// connects again replaces the one before it, so a connection the host dropped
// without a close is never kept.
#pragma once

#include <functional>
#include <mutex>
#include <span>
#include <thread>

#include <android-base/unique_fd.h>

#include "haptics_protocol.hpp"

namespace andarium::haptics {
class HostLink {
public:
    using AckHandler = std::function<void(const Ack&)>;

    // `on_ack` runs on the link's reading thread, with no lock of the link held.
    explicit HostLink(AckHandler on_ack);
    ~HostLink();

    HostLink(const HostLink&) = delete;
    HostLink& operator=(const HostLink&) = delete;

    // Starts listening. Called once, after everything `on_ack` touches exists.
    void start();

    // Sends one frame to the connected host without waiting. False when no host
    // is connected or the frame did not go whole; a frame that goes in part
    // would leave the host reading garbage, so that connection is shut down and
    // the host connects again.
    bool send(std::span<const uint8_t> frame);

private:
    void serve();
    void set_peer(int fd);

    AckHandler on_ack_;
    // Declared before the thread, so it outlives the join in the destructor.
    android::base::unique_fd wake_;
    std::mutex mutex_;
    // The connected host's descriptor, or -1. The serving thread owns the
    // descriptor and clears this before it closes one.
    int peer_ = -1;
    std::thread thread_;
};
}  // namespace andarium::haptics
