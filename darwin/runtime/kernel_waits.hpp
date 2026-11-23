// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::runtime {

/*
 * The calls an embedded kernel makes that can block its host thread, marked.
 *
 * The loader resolves each of the kernel's blocking imports to a wrapper that
 * says what the thread waits for (platform/host_wait.hpp) for the length of
 * the call and then makes the same call. A call that cannot block, a poll or
 * select with a zero timeout, or a kevent that asks for no event or has a zero
 * timeout, is made unmarked. A vCPU's idle wait is a kevent() whose deadline
 * timer is in its change list. The stall watch reads the marks to tell a vCPU
 * that waits for the host from one the system did not run
 * (runtime/stall_watch.hpp):
 *
 *   pselect, kevent, pause, pthread_cond_wait,
 *   pthread_cond_timedwait                                      idle
 *   poll                                                        answer
 *   read, recvfrom                                              receive
 *   sendmsg, sendto                                             send
 *   pread, pwrite                                               disk read, write
 *   fsync                                                       flush
 *
 * An idle wait of a vCPU carrier is also reported to the activity record the
 * watch sleeps by (runtime/carrier_activity.hpp), and so is the call that names
 * a carrier, pthread_setname_np. The time limit a kevent reports is its
 * relative nanosecond timer or its timeout, whichever ends first, none when
 * the change list adds no timer; a timer of any other shape is not read, and
 * its carrier stays counted outside its wait.
 *
 * recvmsg is marked by the resource ledger (kernel_resources.hpp), write
 * (send) by the session record (session_record.hpp) and the commit of guest
 * RAM by the memory layer.
 */

/* The wrapper for a kernel import, or nullptr when the call is not marked or
 * the system's own cannot be found. `name` is the symbol as the kernel names
 * it; a leading underscore, which the Darwin SDK's asm labels add, is
 * ignored. The wrapper calls the system's function of that name. */
[[nodiscard]] void *kernel_wait_import(const char *name) noexcept;

} // namespace uml::runtime
