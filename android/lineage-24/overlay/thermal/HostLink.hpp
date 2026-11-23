// SPDX-License-Identifier: Apache-2.0
//
// The guest end of the private thermal stream: the HAL listens on vsock port
// 5559, accepts the host application (CID 2) and nothing else, and reads its
// records. One host is connected at a time, and a host that connects again
// replaces the one before it, so a connection the host dropped without a close
// is never kept. The stream runs one way: the HAL sends nothing.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include <android-base/unique_fd.h>

#include "thermal_protocol.hpp"

namespace andarium::thermal {
class HostLink {
public:
    using RecordHandler = std::function<void(Severity)>;

    // What dump() shows.
    struct Status {
        bool connected = false;
        uint32_t connections = 0;
        uint32_t records = 0;
        uint32_t refused = 0;
    };

    // `on_record` runs on the link's reading thread, with no lock of the link held.
    explicit HostLink(RecordHandler on_record);
    ~HostLink();

    HostLink(const HostLink&) = delete;
    HostLink& operator=(const HostLink&) = delete;

    // Starts listening. Called once, after everything `on_record` touches exists.
    void start();

    Status status() const;

private:
    void serve();
    void update(void (*change)(Status&));

    RecordHandler on_record_;
    // Declared before the thread, so it outlives the join in the destructor.
    android::base::unique_fd wake_;
    mutable std::mutex mutex_;
    Status status_;
    std::thread thread_;
};
}  // namespace andarium::thermal
