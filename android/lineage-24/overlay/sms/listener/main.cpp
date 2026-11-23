// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "kernarium.sms.listener"

// The holder of vsock port 5560 (README, "Text messages"). It keeps the port from boot so that the host's
// connections queue and no other guest process can take it, and it never reads it: it never accepts, reads,
// writes, shuts down or connects a vsock socket, and never closes the listening one but by ending. The
// peer's CID check and every byte of the host's requests stay in KernariumSms (overlay/sms), which runs only
// while messages are in flight. When a connection waits and no session runs, the holder starts the waker
// through a control property; the session connects to the holder's own unix socket, and the holder passes
// it the listening socket in a SCM_RIGHTS message. The session ends with its process, which the holder sees
// as the end of that connection, and only then wakes another: two sessions never accept at once.

#include <errno.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <selinux/selinux.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <utility>

#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>
#include <log/log.h>

namespace {

constexpr unsigned kPort = 5560;
constexpr int kBacklog = 4;
// The handoff socket takes one connection at a time, the service's.
constexpr int kHandoffBacklog = 2;
// The control property that starts the waker, a oneshot service of init (kernarium_sms_wake.sh).
constexpr const char* kWakeProperty = "ctl.start";
constexpr const char* kWakeService = "kernarium_sms_wake";
// The socket init made for the holder (/dev/socket/kernarium_sms).
constexpr const char* kHandoffSocket = "kernarium_sms";
// Who may take the listening socket: the service, as the system user in its own domain.
constexpr uid_t kSystemUid = 1000;
constexpr const char* kServiceContext = "u:r:kernarium_sms:s0";
// A wake that no session answered is repeated after this long. A session that ended is followed by a wake after
// the shorter wait, which doubles for every session in a row that ended before the service's idle window
// (HostService.IDLE_MILLIS) with a connection still waiting: it ended abnormally, because a session that serves
// lasts the window after its last message, and a service that fails at once is not started every second. The host
// waits 30 s for an answer (GuestSmsChannel.deadlineNanoseconds), and a fault that clears inside that window must
// not leave its request for a long wait: the wait stops doubling at 2 s for the first kQuickEnds such sessions, which
// outlast the window (0.5 + 1 + 15 * 2 s), and doubles to the attach timeout after them. The holder cannot see how
// long a queued connection has waited (the kernel keeps no arrival time), so the count stands for the window.
constexpr int kAttachTimeoutMillis = 20000;
constexpr int kAfterSessionMillis = 500;
constexpr int kQuickMaxAfterSessionMillis = 2000;
constexpr int kQuickEnds = 16;
constexpr int kMaxAfterSessionMillis = kAttachTimeoutMillis;
constexpr int64_t kIdleWindowMillis = 30000;
constexpr int kBindRetryMillis = 1000;
// Wakes and ended sessions are logged for the first and then one in this many.
constexpr unsigned kLogEvery = 256;

int64_t nowMillis() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// The listening socket. Bound and listening before this returns; tried again every second, and said once.
android::base::unique_fd listenOnPort() {
    bool complained = false;
    for (;;) {
        android::base::unique_fd fd(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (fd.get() >= 0) {
            sockaddr_vm address{};
            address.svm_family = AF_VSOCK;
            address.svm_port = kPort;
            address.svm_cid = VMADDR_CID_ANY;
            if (bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                listen(fd.get(), kBacklog) == 0) {
                ALOGI("listening on vsock port %u", kPort);
                return fd;
            }
        }
        if (!complained) {
            complained = true;
            ALOGE("cannot listen on vsock port %u: errno %d", kPort, errno);
        }
        usleep(kBindRetryMillis * 1000);
    }
}

// Whether the peer of a connection on the handoff socket is the service: the system user, in its own domain.
bool isService(int connection) {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
        credentials.uid != kSystemUid) {
        return false;
    }
    char* context = nullptr;
    if (getpeercon(connection, &context) != 0 || context == nullptr) return false;
    const std::string peer(context);
    freecon(context);
    const std::string wanted(kServiceContext);
    // The service has no categories, but a category suffix would not make it another domain.
    return peer == wanted || peer.compare(0, wanted.size() + 1, wanted + ":") == 0;
}

// One byte and the listening socket, which is all a session needs; false when it was not sent.
bool passListener(int connection, int listener) {
    char byte = 'L';
    iovec vector{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &listener, sizeof(int));
    ssize_t sent;
    do {
        sent = sendmsg(connection, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == 1;
}

// Whether the session's connection has ended: the service closed it, or it failed. Bytes on it are read and dropped
// and do not end the session (the service sends none, and a peer that did cannot lengthen the session or make it end).
bool sessionEnded(int connection, short events) {
    if ((events & (POLLHUP | POLLERR | POLLNVAL | POLLRDHUP)) != 0) return true;
    if ((events & POLLIN) == 0) return false;
    char sink[64];
    const ssize_t count = recv(connection, sink, sizeof(sink), MSG_DONTWAIT);
    return count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR);
}

class Holder {
  public:
    explicit Holder(android::base::unique_fd handoff) : handoff_(std::move(handoff)) {}

    int run() {
        listener_ = listenOnPort();
        for (;;) {
            pollfd fds[2] = {};
            nfds_t count = 0;
            int timeout = -1;
            fds[count++] = {handoff_.get(), POLLIN, 0};
            const bool attached = session_.get() >= 0;
            const int64_t now = nowMillis();
            const bool throttled = !attached && now < nextWake_;
            if (attached) {
                fds[count++] = {session_.get(), POLLIN | POLLRDHUP, 0};
            } else if (!throttled) {
                fds[count++] = {listener_.get(), POLLIN, 0};
            } else {
                timeout = static_cast<int>(nextWake_ - now);
            }
            const int ready = poll(fds, count, timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                ALOGE("poll failed: errno %d", errno);
                return 1;
            }
            if (ready == 0) continue;
            if (fds[0].revents != 0) {
                if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    ALOGE("the handoff socket failed (events %#x)", static_cast<unsigned>(fds[0].revents));
                    return 1;
                }
                acceptSession();
                continue;
            }
            if (count < 2 || fds[1].revents == 0) continue;
            if (attached) {
                if (sessionEnded(session_.get(), fds[1].revents)) endSession();
            } else if ((fds[1].revents & POLLIN) != 0) {
                wake();
            } else {
                // The listening socket reports an error and nothing to accept: the socket is no use. Init starts
                // the holder again, with a new one.
                ALOGE("the listening socket failed (events %#x)", static_cast<unsigned>(fds[1].revents));
                return 1;
            }
        }
    }

  private:
    // A connection on the handoff socket: the service, which gets the listening socket, or nobody.
    void acceptSession() {
        android::base::unique_fd connection(accept4(handoff_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (connection.get() < 0) {
            if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED) {
                ALOGE("accept on the handoff socket failed: errno %d", errno);
                usleep(kBindRetryMillis * 1000);
            }
            return;
        }
        if (session_.get() >= 0) {
            // A second session while one is attached is none of the service's: it asks for what it must not have.
            ALOGW("a second connection on the handoff socket while a session is attached: closed");
            return;
        }
        if (!isService(connection.get())) {
            ALOGW("a connection on the handoff socket that is not the service: closed");
            return;
        }
        if (!passListener(connection.get(), listener_.get())) {
            ALOGW("the listening socket was not passed: errno %d", errno);
            return;
        }
        session_ = std::move(connection);
        attachedAt_ = nowMillis();
        if (attachedLogged_++ % kLogEvery == 0) ALOGI("handed the listening socket to the service");
    }

    void endSession() {
        const int64_t now = nowMillis();
        const bool lasted = now - attachedAt_ >= kIdleWindowMillis;
        session_.reset();
        // The connections that queued in the meantime are the next session's. After a session that served (it lasted
        // the idle window, or took every connection that waited) the wait is the shortest; after one that ended early
        // with a connection still waiting it is the current one, and the next is twice as long (2 s at most for the first
        // kQuickEnds, the attach timeout after), so that a service that fails at once is woken once in 20 s, a fault
        // that clears inside the host's window is answered inside it, and a single lost session in half a second.
        if (lasted || !connectionWaits()) {
            afterSessionMillis_ = kAfterSessionMillis;
            earlyEnds_ = 0;
            nextWake_ = now + afterSessionMillis_;
        } else {
            nextWake_ = now + afterSessionMillis_;
            ++earlyEnds_;
            afterSessionMillis_ = std::min(
                afterSessionMillis_ * 2, earlyEnds_ <= kQuickEnds ? kQuickMaxAfterSessionMillis : kMaxAfterSessionMillis);
        }
        if (endedLogged_++ % kLogEvery == 0) ALOGI("the session ended");
    }

    // Whether a connection is queued on the listening socket.
    bool connectionWaits() const {
        pollfd waiting = {listener_.get(), POLLIN, 0};
        return poll(&waiting, 1, 0) > 0 && (waiting.revents & POLLIN) != 0;
    }

    void wake() {
        nextWake_ = nowMillis() + kAttachTimeoutMillis;
        if (!android::base::SetProperty(kWakeProperty, kWakeService)) {
            ALOGE("cannot start %s", kWakeService);
            return;
        }
        if (wokenLogged_++ % kLogEvery == 0) ALOGI("a connection waits: started %s", kWakeService);
    }

    android::base::unique_fd handoff_;
    android::base::unique_fd listener_;
    android::base::unique_fd session_;
    int64_t nextWake_ = 0;
    int64_t attachedAt_ = 0;
    int afterSessionMillis_ = kAfterSessionMillis;
    int earlyEnds_ = 0;
    unsigned attachedLogged_ = 0;
    unsigned endedLogged_ = 0;
    unsigned wokenLogged_ = 0;
};

}  // namespace

int main() {
    const int handoff = android_get_control_socket(kHandoffSocket);
    if (handoff < 0) {
        ALOGE("init gave no socket %s", kHandoffSocket);
        // Init starts the holder again; not at once.
        sleep(1);
        return 1;
    }
    fcntl(handoff, F_SETFD, FD_CLOEXEC);
    fcntl(handoff, F_SETFL, fcntl(handoff, F_GETFL) | O_NONBLOCK);
    // Init creates and binds a stream socket and does not listen on it: that is the daemon's. A socket that is only
    // bound polls as hung up and takes no connection, which would end the holder at once, in a loop.
    if (listen(handoff, kHandoffBacklog) != 0) {
        ALOGE("cannot listen on the handoff socket %s: errno %d", kHandoffSocket, errno);
        sleep(1);
        return 1;
    }
    Holder holder{android::base::unique_fd(handoff)};
    const int status = holder.run();
    ALOGE("the holder stopped (status %d)", status);
    return status;
}
