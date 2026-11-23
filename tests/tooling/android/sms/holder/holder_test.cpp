// SPDX-License-Identifier: GPL-2.0-or-later
//
// Drives the text message holder (android/lineage-24/overlay/sms/listener/main.cpp), compiled as it is with its main
// renamed holder_main, on the host. test_sms_holder.py builds and runs it. The holder's world is the one init gives
// it and the platform's calls it makes, stood in for here:
//   - the handoff socket (/dev/socket/kernarium_sms) is what init makes: a unix stream socket that is BOUND and NOT
//     listening (init does not listen on a stream socket; the daemon does), handed over by android_get_control_socket;
//   - the vsock socket of port 5560 is a unix stream socket at a path of the test (socket, bind and getsockopt are
//     interposed for it and for the peer's credentials; listen, poll, accept, sendmsg and recv are the host's own);
//   - the properties it sets, the peer's user and SELinux context, and its log lines are the test's to read and set.
// The scenarios run one after the other against one holder thread; the first one that does not hold ends the test.
// The holder's clock and its poll timeouts run kTimeScale times faster than the test's, so that the wake backoff
// (half a second to 20 s, a 30 s idle window) shows in seconds; only the holder's thread is scaled.

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int holder_main();  // main.cpp's main, renamed by -Dmain=holder_main

namespace {

std::mutex gLock;
std::vector<std::string> gProperties;
std::string gLastError;
std::atomic<int> gVsockFd{-1};
std::atomic<int> gHandoffFd{-1};
std::atomic<unsigned> gPeerUid{1000};
std::string gPeerContext = "u:r:kernarium_sms:s0";
std::string gVsockPath;
std::atomic<bool> gReturned{false};
std::atomic<int> gStatus{-1};
// The holder's time runs this many times faster than the test's.
constexpr long long kTimeScale = 20;
thread_local bool tHolderThread = false;
std::vector<long long> gWakeTimes;  // the holder's milliseconds at each property set

template <typename Function>
Function realFunction(const char* name) {
    return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

long long nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The holder's milliseconds now: the test's monotonic clock, times kTimeScale.
long long holderNow() {
    static const auto real = realFunction<int (*)(clockid_t, timespec*)>("clock_gettime");
    timespec now{};
    real(CLOCK_MONOTONIC, &now);
    return (static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000) * kTimeScale;
}

template <typename Condition>
bool until(int millis, Condition condition) {
    const long long end = nowMillis() + millis;
    for (;;) {
        if (condition()) return true;
        if (nowMillis() >= end) return false;
        usleep(10 * 1000);
    }
}

[[noreturn]] void fail(const char* scenario, const std::string& what) {
    std::string tail;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (!gLastError.empty()) tail = "; the holder's last error line: '" + gLastError + "'";
    }
    if (gReturned.load()) tail += "; the holder returned status " + std::to_string(gStatus.load());
    std::fprintf(stderr, "FAIL %s: %s%s\n", scenario, what.c_str(), tail.c_str());
    std::fflush(stderr);
    _exit(1);
}

void pass(const char* scenario, const std::string& what) { std::printf("PASS %s: %s\n", scenario, what.c_str()); }

int connectTo(const std::string& path, int* error = nullptr) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) *error = errno;
        close(fd);
        return -1;
    }
    return fd;
}

// What a descriptor-passing peer gets from the holder: the byte and the descriptor, or the end of the connection.
struct Received {
    bool closed = false;
    bool timedOut = false;
    char byte = 0;
    int descriptor = -1;
};

Received receive(int connection, int millis) {
    Received result;
    pollfd waiting{connection, POLLIN, 0};
    if (poll(&waiting, 1, millis) <= 0) {
        result.timedOut = true;
        return result;
    }
    char byte = 0;
    iovec vector{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t count = recvmsg(connection, &message, 0);
    if (count <= 0) {
        result.closed = true;
        return result;
    }
    result.byte = byte;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            std::memcpy(&result.descriptor, CMSG_DATA(header), sizeof(int));
        }
    }
    return result;
}

size_t propertyCount() {
    std::lock_guard<std::mutex> guard(gLock);
    return gProperties.size();
}

}  // namespace

// ---- The platform's calls that the holder makes and the host does not have, and the ones that are redirected.

extern "C" void holder_test_log(char level, const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> guard(gLock);
    if (level == 'E') gLastError = line;
    std::fprintf(stderr, "holder %c: %s\n", level, line);
}

extern "C" int android_get_control_socket(const char* name) {
    return std::strcmp(name, "kernarium_sms") == 0 ? gHandoffFd.load() : -1;
}

extern "C" int getpeercon(int, char** context) {
    std::lock_guard<std::mutex> guard(gLock);
    *context = strdup(gPeerContext.c_str());
    return 0;
}

extern "C" void freecon(char* context) { std::free(context); }

namespace android {
namespace base {
bool SetProperty(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> guard(gLock);
    gProperties.push_back(key + "=" + value);
    gWakeTimes.push_back(holderNow());
    return true;
}
}  // namespace base
}  // namespace android

// The holder's thread sees time and poll timeouts kTimeScale times faster; every other thread sees them as they are.
extern "C" int clock_gettime(clockid_t clock, timespec* out) {
    static const auto real = realFunction<int (*)(clockid_t, timespec*)>("clock_gettime");
    const int result = real(clock, out);
    if (result != 0 || clock != CLOCK_MONOTONIC || !tHolderThread) return result;
    const long long nanos = (static_cast<long long>(out->tv_sec) * 1000000000LL + out->tv_nsec) * kTimeScale;
    out->tv_sec = nanos / 1000000000LL;
    out->tv_nsec = nanos % 1000000000LL;
    return 0;
}

extern "C" int poll(pollfd* fds, nfds_t count, int timeout) {
    static const auto real = realFunction<int (*)(pollfd*, nfds_t, int)>("poll");
    if (tHolderThread && timeout > 0) timeout = static_cast<int>((timeout + kTimeScale - 1) / kTimeScale);
    return real(fds, count, timeout);
}

extern "C" int accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    const int connection = accept(fd, address, length);
    if (connection < 0) return -1;
    if ((flags & SOCK_CLOEXEC) != 0) fcntl(connection, F_SETFD, FD_CLOEXEC);
    if ((flags & SOCK_NONBLOCK) != 0) fcntl(connection, F_SETFL, fcntl(connection, F_GETFL) | O_NONBLOCK);
    return connection;
}

// A vsock socket is a unix stream socket at the test's path; every other socket is the host's.
extern "C" int socket(int domain, int type, int protocol) {
    static const auto real = realFunction<int (*)(int, int, int)>("socket");
    if (domain != AF_VSOCK) return real(domain, type, protocol);
    const int fd = real(AF_UNIX, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), 0);
    if (fd < 0) return fd;
    if ((type & SOCK_CLOEXEC) != 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if ((type & SOCK_NONBLOCK) != 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    gVsockFd.store(fd);
    return fd;
}

extern "C" int bind(int fd, const sockaddr* address, socklen_t length) {
    static const auto real = realFunction<int (*)(int, const sockaddr*, socklen_t)>("bind");
    if (fd != gVsockFd.load()) return real(fd, address, length);
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    std::strncpy(local.sun_path, gVsockPath.c_str(), sizeof(local.sun_path) - 1);
    return real(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
}

extern "C" int getsockopt(int fd, int level, int name, void* value, socklen_t* length) {
    static const auto real = realFunction<int (*)(int, int, int, void*, socklen_t*)>("getsockopt");
    if (level != SOL_SOCKET || name != SO_PEERCRED) return real(fd, level, name, value, length);
    ucred credentials{};
    credentials.uid = gPeerUid.load();
    std::memcpy(value, &credentials, sizeof(credentials));
    *length = sizeof(credentials);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: holder_test DIRECTORY\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const std::string directory = argv[1];
    const std::string handoffPath = directory + "/handoff";
    gVsockPath = directory + "/vsock";

    // The socket init makes for the holder: created, bound, and nothing more.
    const int handoff = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, handoffPath.c_str(), sizeof(address.sun_path) - 1);
    if (bind(handoff, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("bind of the handoff socket");
        return 2;
    }
    gHandoffFd.store(handoff);

    std::thread([] {
        tHolderThread = true;
        gStatus.store(holder_main());
        gReturned.store(true);
    }).detach();

    // S1: init does not listen, so the holder does: it must stay in its loop on a socket that is bound and not
    // listening (on Linux such a socket polls as hung up, which ended the holder at once; macOS reports nothing for
    // it, so there the missing listen shows in S3, which finds the handoff socket refusing connections).
    until(400, [] { return gReturned.load(); });
    if (gReturned.load()) fail("S1", "the holder ended on the handoff socket that init made (bound, not listening)");
    pass("S1", "the holder keeps running on the handoff socket as init makes it");

    // S2: a connection that waits on the vsock port starts the waker, through the control property.
    int host = -1;
    until(3000, [&] { return (host = connectTo(gVsockPath)) >= 0; });
    if (host < 0) fail("S2", "the vsock socket of the holder does not take a connection");
    if (!until(3000, [] { return propertyCount() >= 1; })) fail("S2", "no waker was started for a waiting connection");
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (gProperties[0] != "ctl.start=kernarium_sms_wake") fail("S2", "the property set is " + gProperties[0]);
    }
    pass("S2", "a waiting connection sets ctl.start=kernarium_sms_wake");

    // S3: the service connects to the handoff socket and is passed the listening socket, which is the vsock one.
    int error = 0;
    const int service = connectTo(handoffPath, &error);
    if (service < 0) {
        fail("S3", std::string("the handoff socket takes no connection: ") + std::strerror(error));
    }
    const Received passed = receive(service, 3000);
    if (passed.timedOut || passed.closed || passed.byte != 'L' || passed.descriptor < 0) {
        fail("S3", "the service was not passed the listening socket");
    }
    pollfd waiting{passed.descriptor, POLLIN, 0};
    if (poll(&waiting, 1, 3000) <= 0) fail("S3", "the passed socket does not show the waiting connection");
    const int accepted = accept(passed.descriptor, nullptr, nullptr);
    if (accepted < 0) fail("S3", "the passed socket does not accept the waiting connection");
    char ping[4] = {'p', 'i', 'n', 'g'};
    char echo[4] = {};
    if (write(host, ping, 4) != 4 || read(accepted, echo, 4) != 4 || std::memcmp(ping, echo, 4) != 0) {
        fail("S3", "the accepted connection is not the host's");
    }
    close(accepted);
    close(host);
    pass("S3", "the service is passed the listening socket and accepts the host's connection from it");

    // S4: a second connection on the handoff socket while a session is attached gets nothing.
    const int second = connectTo(handoffPath);
    if (second < 0) fail("S4", "the handoff socket takes no second connection");
    const Received refused = receive(second, 3000);
    if (!refused.closed || refused.descriptor >= 0) fail("S4", "a second connection was not closed without a descriptor");
    close(second);
    pass("S4", "a second connection during a session is closed and passed nothing");

    // S5: the session ends with the service's connection, and the next waiting connection wakes the waker again.
    close(service);
    usleep(150 * 1000);  // the holder has seen the session end; nothing waited, so the wait after it is the shortest
    const int next = connectTo(gVsockPath);
    if (next < 0) fail("S5", "the vsock socket takes no connection after the session");
    if (!until(5000, [] { return propertyCount() >= 2; })) fail("S5", "no second waker after the session ended");
    pass("S5", "after the session ends the next waiting connection starts the waker again");

    // S6 and S7: only the system user in the service's own domain is passed the socket.
    gPeerUid.store(1001);
    const int stranger = connectTo(handoffPath);
    if (stranger < 0) fail("S6", "the handoff socket takes no connection from another user");
    const Received other = receive(stranger, 3000);
    if (!other.closed || other.descriptor >= 0) fail("S6", "a connection of another user was passed the socket");
    close(stranger);
    gPeerUid.store(1000);
    {
        std::lock_guard<std::mutex> guard(gLock);
        gPeerContext = "u:r:shell:s0";
    }
    const int wrongDomain = connectTo(handoffPath);
    if (wrongDomain < 0) fail("S7", "the handoff socket takes no connection from another domain");
    const Received foreign = receive(wrongDomain, 3000);
    if (!foreign.closed || foreign.descriptor >= 0) fail("S7", "a connection of another domain was passed the socket");
    close(wrongDomain);
    pass("S6", "a connection of another user is closed and passed nothing");
    pass("S7", "a connection of another domain is closed and passed nothing");

    // The service, as a peer of the handoff socket: connects, is passed the listening socket, and is the session.
    struct Session {
        int connection = -1;
        int listener = -1;
    };
    auto attach = [&](const char* scenario) {
        Session session;
        session.connection = connectTo(handoffPath);
        if (session.connection < 0) fail(scenario, "the handoff socket takes no connection from the service");
        const Received got = receive(session.connection, 3000);
        if (got.timedOut || got.closed || got.descriptor < 0) fail(scenario, "the service was not passed the socket");
        session.listener = got.descriptor;
        return session;
    };
    // Waits for the next wake, and returns the holder's time at which it happened.
    size_t woken = propertyCount();
    auto nextWake = [&](const char* scenario) {
        if (!until(8000, [&] { return propertyCount() > woken; })) fail(scenario, "no waker was started again");
        std::lock_guard<std::mutex> guard(gLock);
        return gWakeTimes[woken++];
    };

    // S8: a session that ends before it accepts anything, with a connection still waiting, is woken again after a wait that
    // doubles from half a second: to 2 s for the first sixteen such sessions, which outlast the host's 30 s window, and then
    // to 20 s. A service that cannot serve is not started once a second without end, and one that clears within the
    // window is not left for 16 s. The first wake here is the one that no session answered (20 s); a session then attaches
    // and ends at once, each time. At most 17 wakes fall in the 30 s after the first, and most of the 17 do.
    {
        std::lock_guard<std::mutex> guard(gLock);
        gPeerContext = "u:r:kernarium_sms:s0";
    }
    const long long first = nextWake("S8");
    long long previous = first;
    std::vector<long long> expected = {500, 1000};
    expected.insert(expected.end(), 15, 2000);
    expected.insert(expected.end(), {4000, 8000, 16000, 20000, 20000});
    int inWindow = 1;
    for (size_t index = 0; index < expected.size(); ++index) {
        const Session lost = attach("S8");
        close(lost.connection);
        close(lost.listener);
        const long long at = nextWake("S8");
        const long long gap = at - previous;
        const long long wait = expected[index];
        if (gap < wait - 100 || gap > wait + wait / 4 + 700) {
            fail("S8", "the wake after the session that ended early number " + std::to_string(index + 1) +
                           " came after " + std::to_string(gap) + " ms, not about " + std::to_string(wait) + " ms");
        }
        if (at - first < 30000) ++inWindow;
        previous = at;
    }
    if (inWindow > 17 || inWindow < 12) {
        fail("S8", std::to_string(inWindow) + " wakes fell in the 30 s after the first, not 12 to 17");
    }
    pass("S8", "sessions that end early are woken again after 0.5, 1 and 2 s (sixteen times), then 4, 8, 16 and 20 s, "
               "with " + std::to_string(inWindow) + " wakes in the first 30 s");

    // S9: a session that lasted the idle window (30 s) worked: the wait after it is the first again.
    const Session served = attach("S9");
    until(1700, [] { return false; });  // 34 s of the holder's time
    close(served.connection);
    close(served.listener);
    const long long ended = holderNow();
    const long long afterServed = nextWake("S9") - ended;
    if (afterServed < 300 || afterServed > 1500) {
        fail("S9", "the wake after a session of the idle window came after " + std::to_string(afterServed) + " ms");
    }
    pass("S9", "a session that lasted the idle window resets the wait to half a second");

    // S10: a session that took every waiting connection worked, however short: it does not lengthen the wait. Two early
    // ends first (0.5 s and 1 s, the next would be 2 s); then a session that accepts what waits and ends at once.
    for (int index = 0; index < 2; ++index) {
        const Session lost = attach("S10");
        close(lost.connection);
        close(lost.listener);
        nextWake("S10");
    }
    const Session drain = attach("S10");
    for (;;) {
        const int taken = accept(drain.listener, nullptr, nullptr);
        if (taken < 0) break;
        close(taken);
    }
    close(drain.connection);
    close(drain.listener);
    const long long drained = holderNow();
    usleep(20 * 1000);  // the holder sees the session end with nothing waiting (0.4 s of its time, under the 2 s doubled)
    const int fresh = connectTo(gVsockPath);
    if (fresh < 0) fail("S10", "the vsock socket takes no connection after the drained session");
    const long long afterDrained = nextWake("S10") - drained;
    if (afterDrained < 300 || afterDrained > 1500) {
        fail("S10", "the wake after a short session that took every connection came after " +
                        std::to_string(afterDrained) + " ms");
    }
    close(fresh);
    pass("S10", "a short session that took every waiting connection does not lengthen the wait");

    // S12: a fault that clears at 17 s is answered inside the host's 30 s window: sessions end at once until then, and the
    // first one after takes the waiting connection. (Waits of 0.5, 1, 2, 4, 8 and 16 s woke the service next at 31.5 s.)
    {
        const Session reset = attach("S12");  // takes whatever waits, so that the first wake after it is the first again
        for (;;) {
            const int taken = accept(reset.listener, nullptr, nullptr);
            if (taken < 0) break;
            close(taken);
        }
        close(reset.connection);
        close(reset.listener);
        usleep(20 * 1000);
        const long long arrival = holderNow();
        const int host = connectTo(gVsockPath);
        if (host < 0) fail("S12", "the vsock socket takes no connection");
        long long answered = -1;
        for (int step = 0; step < 60 && answered < 0; ++step) {
            nextWake("S12");
            const Session session = attach("S12");
            if (holderNow() - arrival >= 17000) {
                const int taken = accept(session.listener, nullptr, nullptr);
                if (taken >= 0) {
                    answered = holderNow();
                    close(taken);
                }
            }
            close(session.connection);
            close(session.listener);
        }
        if (answered < 0) fail("S12", "the waiting connection was never taken");
        if (answered - arrival >= 30000) {
            fail("S12", "a fault that cleared at 17 s was answered at " + std::to_string(answered - arrival) +
                            " ms, after the host's 30 s window");
        }
        close(host);
        pass("S12", "a fault that clears at 17 s is answered at " + std::to_string((answered - arrival) / 1000) +
                        " s, inside the host's window");
    }

    if (gReturned.load()) fail("S11", "the holder ended during the scenarios");
    pass("S11", "the holder ran through every scenario");
    close(next);
    std::printf("PASS\n");
    std::fflush(stdout);
    _exit(0);
}
