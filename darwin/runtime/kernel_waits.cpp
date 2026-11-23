// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_waits.hpp"

#include "carrier_activity.hpp"
#include "platform/host_wait.hpp"
#include "vcpu_carrier_name.hpp"

#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string_view>
#include <sys/event.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

using ::uml::platform::HostWait;
using ::uml::platform::ScopedHostWait;

/* The system's functions, found by the first resolution of each import. */
struct Real final {
	int (*pselect)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *){};
	int (*pause)(){};
	int (*cond_wait)(pthread_cond_t *, pthread_mutex_t *){};
	int (*cond_timedwait)(pthread_cond_t *, pthread_mutex_t *,
		const timespec *){};
	int (*poll)(pollfd *, nfds_t, int){};
	ssize_t (*read)(int, void *, size_t){};
	ssize_t (*recvfrom)(int, void *, size_t, int, sockaddr *, socklen_t *){};
	ssize_t (*sendmsg)(int, const msghdr *, int){};
	ssize_t (*sendto)(int, const void *, size_t, int, const sockaddr *,
		socklen_t){};
	ssize_t (*pread)(int, void *, size_t, off_t){};
	ssize_t (*pwrite)(int, const void *, size_t, off_t){};
	int (*fsync)(int){};
	int (*setname)(const char *){};
	int (*kevent)(int, const struct kevent *, int, struct kevent *, int,
		const timespec *){};
};

Real real;

int kernel_pselect(int count, fd_set *readable, fd_set *writable,
	fd_set *failed, const timespec *timeout, const sigset_t *mask)
{
	/* An immediate poll waits for nothing. */
	if (timeout != nullptr && timeout->tv_sec == 0 && timeout->tv_nsec == 0)
		return real.pselect(count, readable, writable, failed, timeout, mask);
	/* The mark is made first and undone last, so a carrier that is counted
	 * in its idle wait shows the idle mark too. */
	const ScopedHostWait wait{HostWait::idle};
	const CarrierIdle carrier{timeout, CarrierIdle::Limit::relative};
	return real.pselect(count, readable, writable, failed, timeout, mask);
}

/* The limit of the kernel's idle kevent: the relative NOTE_NSECONDS timer its
 * change list adds or its timeout, whichever ends first. False for a timer of
 * any other shape, whose wait then is not counted idle, so the stall watch
 * keeps sampling the carrier instead of trusting a deadline it cannot read. */
bool idle_limit(const struct kevent *changes, int change_count,
	const timespec *timeout, timespec &timer, const timespec *&limit) noexcept
{
	limit = timeout;
	for (int index = 0; changes != nullptr && index < change_count; ++index) {
		const struct kevent &change = changes[index];
		if (change.filter != EVFILT_TIMER || (change.flags & EV_ADD) == 0)
			continue;
		const auto unit = change.fflags &
			(NOTE_SECONDS | NOTE_USECONDS | NOTE_NSECONDS | NOTE_MACHTIME);
		if (unit != NOTE_NSECONDS || change.data < 0 ||
		    (change.fflags & (NOTE_ABSOLUTE | NOTE_MACH_CONTINUOUS_TIME)) != 0)
			return false;
		timer = {static_cast<time_t>(change.data / 1'000'000'000),
			static_cast<long>(change.data % 1'000'000'000)};
		if (limit == nullptr || timer.tv_sec < limit->tv_sec ||
		    (timer.tv_sec == limit->tv_sec && timer.tv_nsec < limit->tv_nsec))
			limit = &timer;
	}
	return true;
}

/* A vCPU's idle wait is a kevent() whose timer is in its change list, so the
 * call carries no timeout. Only a call that asks for events and may block is
 * a wait: a change list alone, and an immediate poll, wait for nothing. */
int kernel_kevent(int queue, const struct kevent *changes, int change_count,
	struct kevent *events, int event_count, const timespec *timeout)
{
	if (event_count <= 0 ||
	    (timeout != nullptr && timeout->tv_sec == 0 && timeout->tv_nsec == 0))
		return real.kevent(queue, changes, change_count, events, event_count,
			timeout);
	/* The mark is made first and undone last, as in pselect. */
	const ScopedHostWait wait{HostWait::idle};
	timespec timer{};
	const timespec *limit = nullptr;
	if (!idle_limit(changes, change_count, timeout, timer, limit))
		return real.kevent(queue, changes, change_count, events, event_count,
			timeout);
	const CarrierIdle carrier{limit, CarrierIdle::Limit::relative};
	return real.kevent(queue, changes, change_count, events, event_count,
		timeout);
}

int kernel_pause()
{
	const ScopedHostWait wait{HostWait::idle};
	const CarrierIdle carrier;
	return real.pause();
}

int kernel_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	const ScopedHostWait wait{HostWait::idle};
	const CarrierIdle carrier;
	return real.cond_wait(condition, mutex);
}

int kernel_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
	const timespec *deadline)
{
	const ScopedHostWait wait{HostWait::idle};
	const CarrierIdle carrier{deadline, CarrierIdle::Limit::realtime};
	return real.cond_timedwait(condition, mutex, deadline);
}

int kernel_poll(pollfd *descriptors, nfds_t count, int timeout)
{
	if (timeout == 0)
		return real.poll(descriptors, count, timeout);
	const ScopedHostWait wait{HostWait::answer};
	return real.poll(descriptors, count, timeout);
}

ssize_t kernel_read(int descriptor, void *buffer, size_t length)
{
	const ScopedHostWait wait{HostWait::receive};
	return real.read(descriptor, buffer, length);
}

ssize_t kernel_recvfrom(int descriptor, void *buffer, size_t length,
	int flags, sockaddr *address, socklen_t *address_length)
{
	const ScopedHostWait wait{HostWait::receive};
	return real.recvfrom(descriptor, buffer, length, flags, address,
		address_length);
}

ssize_t kernel_sendmsg(int descriptor, const msghdr *message, int flags)
{
	const ScopedHostWait wait{HostWait::send};
	return real.sendmsg(descriptor, message, flags);
}

ssize_t kernel_sendto(int descriptor, const void *buffer, size_t length,
	int flags, const sockaddr *address, socklen_t address_length)
{
	const ScopedHostWait wait{HostWait::send};
	return real.sendto(descriptor, buffer, length, flags, address,
		address_length);
}

ssize_t kernel_pread(int descriptor, void *buffer, size_t length, off_t offset)
{
	const ScopedHostWait wait{HostWait::disk_read};
	return real.pread(descriptor, buffer, length, offset);
}

ssize_t kernel_pwrite(int descriptor, const void *buffer, size_t length,
	off_t offset)
{
	const ScopedHostWait wait{HostWait::disk_write};
	return real.pwrite(descriptor, buffer, length, offset);
}

int kernel_fsync(int descriptor)
{
	const ScopedHostWait wait{HostWait::flush};
	return real.fsync(descriptor);
}

/* A thread that names itself as a vCPU's carrier says so to the watch's
 * activity record (carrier_activity.hpp). */
int kernel_setname(const char *name)
{
	const int result = real.setname(name);
	if (result == 0 && name != nullptr) {
		const int vcpu = vcpu_carrier_index(std::string_view{name});
		if (vcpu >= 0)
			carrier_named(static_cast<unsigned>(vcpu));
	}
	return result;
}

struct Import final {
	const char *name;
	void *wrapper;
	/* Where the system's function goes, as the address of the member. */
	void **target;
};

template<typename Function>
void **slot(Function *&member) noexcept
{
	return reinterpret_cast<void **>(&member);
}

} // namespace

void *kernel_wait_import(const char *name) noexcept
{
	if (name == nullptr)
		return nullptr;
	/* The system's name for the call is the kernel's, less the underscore the
	 * SDK's asm labels add. */
	const char *const system_name = name[0] == '_' ? name + 1 : name;
	const Import imports[] = {
		{"pselect$DARWIN_EXTSN", reinterpret_cast<void *>(&kernel_pselect),
			slot(real.pselect)},
		{"pselect", reinterpret_cast<void *>(&kernel_pselect),
			slot(real.pselect)},
		{"kevent", reinterpret_cast<void *>(&kernel_kevent), slot(real.kevent)},
		{"pause", reinterpret_cast<void *>(&kernel_pause), slot(real.pause)},
		{"pthread_cond_wait", reinterpret_cast<void *>(&kernel_cond_wait),
			slot(real.cond_wait)},
		{"pthread_cond_timedwait",
			reinterpret_cast<void *>(&kernel_cond_timedwait),
			slot(real.cond_timedwait)},
		{"poll", reinterpret_cast<void *>(&kernel_poll), slot(real.poll)},
		{"read", reinterpret_cast<void *>(&kernel_read), slot(real.read)},
		{"recvfrom", reinterpret_cast<void *>(&kernel_recvfrom),
			slot(real.recvfrom)},
		{"sendmsg", reinterpret_cast<void *>(&kernel_sendmsg),
			slot(real.sendmsg)},
		{"sendto", reinterpret_cast<void *>(&kernel_sendto),
			slot(real.sendto)},
		{"pread", reinterpret_cast<void *>(&kernel_pread), slot(real.pread)},
		{"pwrite", reinterpret_cast<void *>(&kernel_pwrite),
			slot(real.pwrite)},
		{"fsync", reinterpret_cast<void *>(&kernel_fsync), slot(real.fsync)},
		{"pthread_setname_np", reinterpret_cast<void *>(&kernel_setname),
			slot(real.setname)},
	};
	for (const auto &import : imports) {
		if (std::strcmp(import.name, system_name) != 0)
			continue;
		if (*import.target == nullptr)
			*import.target = dlsym(RTLD_DEFAULT, import.name);
		/* Without the system's function the call goes unmarked. */
		return *import.target != nullptr ? import.wrapper : nullptr;
	}
	return nullptr;
}

} // namespace uml::runtime
