// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.thermal"

#include "HostLink.hpp"

#include <linux/vm_sockets.h>
#include <log/log.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

namespace andarium::thermal {
using android::base::unique_fd;

HostLink::HostLink(RecordHandler on_record)
    : on_record_(std::move(on_record)), wake_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

HostLink::~HostLink() {
    if (thread_.joinable()) {
        eventfd_write(wake_.get(), 1);
        thread_.join();
    }
}

void HostLink::start() {
    if (wake_ < 0) {
        ALOGE("cannot create the wake descriptor: %s", strerror(errno));
        return;
    }
    thread_ = std::thread([this] { serve(); });
}

HostLink::Status HostLink::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void HostLink::update(void (*change)(Status&)) {
    std::lock_guard lock(mutex_);
    change(status_);
}

// Waits for a connection or a record. The service starts at boot whether or not
// a host ever connects, so the thread blocks with no timeout and never wakes to
// poll.
void HostLink::serve() {
    unique_fd listener(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    sockaddr_vm address{};
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_ANY;
    address.svm_port = port;
    if (listener < 0 || bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        listen(listener.get(), 2)) {
        ALOGE("cannot listen on vsock port %u: %s", port, strerror(errno));
        return;
    }
    unique_fd stream;
    RecordReader reader;
    const auto close_stream = [&] {
        update([](Status& status) { status.connected = false; });
        stream.reset();
        reader.reset();
    };
    for (;;) {
        pollfd descriptors[] = {{wake_.get(), POLLIN, 0}, {listener.get(), POLLIN, 0}, {stream.get(), POLLIN, 0}};
        const auto ready = poll(descriptors, 3, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || descriptors[0].revents != 0) return;
        // A listener that reports an error or a hang-up and has nothing to accept would make poll return at once from
        // now on, and nothing consumes the event: stop serving. The severity stays what it was.
        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(descriptors[1].revents & POLLIN)) {
            ALOGE("the listener on vsock port %u failed (poll events 0x%x); no longer serving", port,
                  static_cast<unsigned>(descriptors[1].revents));
            return;
        }
        if (descriptors[1].revents & POLLIN) {
            sockaddr_vm peer{};
            socklen_t length = sizeof(peer);
            unique_fd accepted(accept4(listener.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                                       SOCK_CLOEXEC | SOCK_NONBLOCK));
            // Only the private host channel may set the thermal state. A host
            // that connects again replaces the connection it may have dropped.
            if (accepted >= 0 && peer.svm_cid == VMADDR_CID_HOST) {
                close_stream();
                stream = std::move(accepted);
                update([](Status& status) {
                    status.connected = true;
                    ++status.connections;
                });
                ALOGI("host connected");
                continue;
            }
        }
        if (stream >= 0 && descriptors[2].revents != 0) {
            std::array<uint8_t, 64> buffer;
            const auto count = recv(stream.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) {
                // The severity stays what the host last said: a host that is
                // gone has not said the guest is cool again.
                ALOGI("host disconnected");
                close_stream();
                continue;
            }
            const auto output = reader.feed(std::span(buffer).first(std::size_t(count)));
            for (const auto severity : output.records) {
                update([](Status& status) { ++status.records; });
                on_record_(severity);
            }
            if (output.failed) {
                ALOGW("host sent a record that is not one");
                update([](Status& status) { ++status.refused; });
                close_stream();
            }
        }
    }
}
}  // namespace andarium::thermal
