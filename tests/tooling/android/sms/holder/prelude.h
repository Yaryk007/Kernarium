// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for the bionic pieces the holder (android/lineage-24/overlay/sms/listener/main.cpp) names and macOS lacks,
// so that the holder's own source compiles and runs on the host under holder_test.cpp. Included before every
// header of the holder (clang -include).
#pragma once
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstddef>

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 04000
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x80000
#endif
// macOS has no POLLRDHUP: the peer's end of a unix connection shows as POLLHUP there.
#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 0x5eed
struct ucred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};
#endif
extern "C" int accept4(int, struct sockaddr*, socklen_t*, int);
