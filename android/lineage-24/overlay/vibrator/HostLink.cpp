// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.vibrator"

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

namespace andarium::haptics {
using android::base::unique_fd;

HostLink::HostLink(AckHandler on_ack)
    : on_ack_(std::move(on_ack)), wake_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

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

bool HostLink::send(std::span<const uint8_t> frame) {
    std::lock_guard lock(mutex_);
    if (peer_ < 0) return false;
    const auto sent = ::send(peer_, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(frame.size())) return true;
    ALOGW("frame not sent whole (%zd of %zu): %s", sent, frame.size(), strerror(errno));
    shutdown(peer_, SHUT_RDWR);
    return false;
}

void HostLink::set_peer(int fd) {
    std::lock_guard lock(mutex_);
    peer_ = fd;
}

// Waits for a connection or a frame. The service starts at boot whether or not
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
    AckReader reader;
    const auto close_stream = [&] {
        set_peer(-1);
        stream.reset();
        reader.reset();
    };
    for (;;) {
        pollfd descriptors[] = {{wake_.get(), POLLIN, 0}, {listener.get(), POLLIN, 0}, {stream.get(), POLLIN, 0}};
        const auto ready = poll(descriptors, 3, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || descriptors[0].revents != 0) return;
        if (descriptors[1].revents & POLLIN) {
            sockaddr_vm peer{};
            socklen_t length = sizeof(peer);
            unique_fd accepted(accept4(listener.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                                       SOCK_CLOEXEC | SOCK_NONBLOCK));
            // Only the private host channel may play haptics. A host that
            // connects again replaces the connection it may have dropped.
            if (accepted >= 0 && peer.svm_cid == VMADDR_CID_HOST) {
                close_stream();
                stream = std::move(accepted);
                set_peer(stream.get());
                ALOGI("host connected");
                continue;
            }
        }
        if (stream >= 0 && descriptors[2].revents != 0) {
            std::array<uint8_t, 256> buffer;
            const auto count = recv(stream.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) {
                ALOGI("host disconnected");
                close_stream();
                continue;
            }
            const auto output = reader.feed(std::span(buffer).first(std::size_t(count)));
            for (const auto& ack : output.acks) on_ack_(ack);
            if (output.failed) {
                ALOGW("host sent a frame that is not an ACK");
                close_stream();
            }
        }
    }
}
}  // namespace andarium::haptics
