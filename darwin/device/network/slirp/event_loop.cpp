// SPDX-License-Identifier: GPL-2.0-or-later
#include "provider.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

namespace uml::network {
namespace {

std::int64_t monotonic_nanoseconds() noexcept
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

short poll_events_from_slirp(int events) noexcept
{
	short result = 0;
	if ((events & SLIRP_POLL_IN) != 0)
		result |= POLLIN;
	if ((events & SLIRP_POLL_OUT) != 0)
		result |= POLLOUT;
	if ((events & SLIRP_POLL_PRI) != 0)
		result |= POLLPRI;
	if ((events & SLIRP_POLL_ERR) != 0)
		result |= POLLERR;
	if ((events & SLIRP_POLL_HUP) != 0)
		result |= POLLHUP;
	return result;
}

int slirp_events_from_poll(short events) noexcept
{
	int result = 0;
	if ((events & POLLIN) != 0)
		result |= SLIRP_POLL_IN;
	if ((events & POLLOUT) != 0)
		result |= SLIRP_POLL_OUT;
	if ((events & POLLPRI) != 0)
		result |= SLIRP_POLL_PRI;
	if ((events & POLLERR) != 0)
		result |= SLIRP_POLL_ERR;
	if ((events & POLLHUP) != 0)
		result |= SLIRP_POLL_HUP;
	return result;
}

} // namespace

void SlirpProvider::guest_error_callback(const char *message,
					void *opaque) noexcept
{
	static constexpr char prefix[] = "Darwin user network: ";
	(void)opaque;
	(void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
	if (message != nullptr)
		(void)::write(STDERR_FILENO, message, std::strlen(message));
	(void)::write(STDERR_FILENO, "\n", 1);
}

std::int64_t SlirpProvider::clock_callback(void *opaque) noexcept
{
	(void)opaque;
	return monotonic_nanoseconds();
}

void *SlirpProvider::timer_new_callback(SlirpTimerId id,
				       void *callback_opaque,
				       void *opaque) noexcept
{
	auto *emulator = from_opaque(opaque);
	if (emulator == nullptr)
		return nullptr;
	try {
		auto timer = std::make_unique<Timer>();
		timer->id = id;
		timer->callback_opaque = callback_opaque;
		auto *result = timer.get();
		emulator->timers_.push_front(std::move(timer));
		return result;
	} catch (...) {
		return nullptr;
	}
}

void SlirpProvider::timer_free_callback(void *timer, void *opaque) noexcept
{
	auto *emulator = from_opaque(opaque);
	if (emulator == nullptr)
		return;
	const auto match = std::ranges::find_if(
		emulator->timers_, [timer](const auto &candidate) {
			return candidate.get() == timer;
		});
	if (match != emulator->timers_.end())
		emulator->timers_.erase(match);
}

void SlirpProvider::timer_modify_callback(
	void *timer, std::int64_t expires_ms, void *opaque) noexcept
{
	(void)opaque;
	if (timer != nullptr)
		static_cast<Timer *>(timer)->expires_ms = expires_ms;
}

void SlirpProvider::register_poll_socket(
	slirp_os_socket descriptor, void *opaque) noexcept
{
#if UML_SLIRP_TESTING
	if (auto *emulator = from_opaque(opaque); emulator != nullptr) {
		sockaddr_storage address{};
		socklen_t length = sizeof(address);
		if (::getsockname(descriptor,
				  reinterpret_cast<sockaddr *>(&address),
				  &length) == 0 &&
		    (address.ss_family == AF_INET ||
		     address.ss_family == AF_INET6))
			emulator->internet_sockets_for_testing_.fetch_add(
				1, std::memory_order_relaxed);
	}
#else
	(void)opaque;
#endif
	/* libslirp writes the host sockets it makes or accepts with plain send(),
	 * which raises SIGPIPE when the peer has already reset the connection: an
	 * adb server dropping its transport while the guest restarts is enough to
	 * kill the process. The signal's disposition belongs to whoever runs the
	 * guest, so the socket says it wants EPIPE instead. */
	const int enabled = 1;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
			 sizeof(enabled));
}

void SlirpProvider::unregister_poll_socket(
	slirp_os_socket descriptor, void *opaque) noexcept
{
	(void)descriptor;
	(void)opaque;
}

int SlirpProvider::add_poll_descriptor(
	slirp_os_socket descriptor, int events, void *opaque) noexcept
{
	auto &state = *static_cast<PollState *>(opaque);
	const auto index = state.descriptors.size();
	if (index > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		return -1;
	try {
		state.descriptors.push_back(
			{descriptor, poll_events_from_slirp(events), 0});
	} catch (...) {
		/* SLiRP skips a socket it gets no index for, this pass only. */
		return -1;
	}
	return static_cast<int>(index);
}

int SlirpProvider::get_poll_events(int index, void *opaque) noexcept
{
	const auto &state = *static_cast<PollState *>(opaque);
	if (index < 0 ||
	    static_cast<std::size_t>(index) >= state.descriptors.size())
		return 0;
	return slirp_events_from_poll(state.descriptors[index].revents);
}

std::uint32_t SlirpProvider::timer_timeout(
	std::uint32_t timeout) const noexcept
{
	const auto now_ms = monotonic_nanoseconds() / 1'000'000;
	for (const auto &timer : timers_) {
		if (timer->expires_ms < 0)
			continue;
		const auto remaining = timer->expires_ms - now_ms;
		if (remaining <= 0)
			return 0;
		if (static_cast<std::uint64_t>(remaining) < timeout)
			timeout = static_cast<std::uint32_t>(remaining);
	}
	return timeout;
}

void SlirpProvider::handle_expired_timers() noexcept
{
	for (;;) {
		const auto now_ms = monotonic_nanoseconds() / 1'000'000;
		const auto match = std::ranges::find_if(
			timers_, [now_ms](const auto &timer) {
				return timer->expires_ms >= 0 &&
					timer->expires_ms <= now_ms;
			});
		if (match == timers_.end())
			return;
		auto &timer = **match;
		timer.expires_ms = -1;
		slirp_handle_timer(slirp_.get(), timer.id,
				   timer.callback_opaque);
	}
}

std::optional<HostAddresses> SlirpProvider::read_address_table() noexcept
{
	try {
#if UML_SLIRP_TESTING
		if (address_reader_for_testing_)
			return address_reader_for_testing_();
#endif
		return read_host_addresses();
	} catch (...) {
		return std::nullopt;
	}
}

void SlirpProvider::check_address_reset() noexcept
{
	const auto reports = path_state_.reports();
	if (reports != path_reports_seen_) {
		path_reports_seen_ = reports;
		/* An outage that began and ended between two looks leaves no
		 * trace in the state but its count: the connections it
		 * interrupted resume, so the hold starts over. */
		if (const auto outages = path_state_.uncarried_reports();
		    outages != path_outages_seen_) {
			path_outages_seen_ = outages;
			address_reset_hold_.clear();
		}
		/* The table is read at a report and when its hold ends, never in
		 * between. A failed read leaves a running hold the sample it
		 * started with; a hold that starts now has none, and a table of
		 * an earlier hold is not its first sample. */
		bool changed = false;
		if (auto table = read_address_table()) {
			changed = !address_reset_table_ ||
				*table != *address_reset_table_;
			address_reset_table_ = std::move(table);
		} else if (!address_reset_hold_.pending()) {
			address_reset_table_.reset();
		}
		address_reset_hold_.reported(path_state_.carried(), changed,
					     host_clock_now());
	}
	if (address_reset_hold_.take_due(path_state_.carried(),
					 host_clock_now()))
		reset_flows_on_lost_addresses();
#if UML_SLIRP_TESTING
	/* Counted when the pass is over, so that a test that waits for them has
	 * the reports it waited for read and their hold looked at. */
	path_reports_handled_for_testing_.store(path_reports_seen_,
						std::memory_order_release);
	address_reset_checks_for_testing_.fetch_add(
		1, std::memory_order_release);
#endif
}

std::uint32_t SlirpProvider::address_reset_timeout(
	std::uint32_t timeout) const noexcept
{
	const auto remaining = address_reset_hold_.remaining(host_clock_now());
	if (!remaining)
		return timeout;
	/* Round up, so that the pass that wakes finds the hold ended. */
	const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(
		*remaining).count();
	if (milliseconds < static_cast<std::int64_t>(timeout))
		return static_cast<std::uint32_t>(milliseconds);
	return timeout;
}

struct SlirpProvider::LostAddressQuery {
	const SlirpProvider &provider;
	const HostAddresses &first;
	const HostAddresses &now;
};

bool SlirpProvider::lost_address_callback(slirp_os_socket descriptor,
					  void *opaque) noexcept
{
	const auto &query = *static_cast<const LostAddressQuery *>(opaque);
	sockaddr_storage local{};
#if UML_SLIRP_TESTING
	if (query.provider.flow_local_address_for_testing_) {
		const auto address =
			query.provider.flow_local_address_for_testing_(descriptor);
		if (!address)
			return false;
		auto &inet = reinterpret_cast<sockaddr_in &>(local);
		inet.sin_family = AF_INET;
		inet.sin_addr.s_addr = htonl(
			(static_cast<std::uint32_t>((*address)[0]) << 24) |
			(static_cast<std::uint32_t>((*address)[1]) << 16) |
			(static_cast<std::uint32_t>((*address)[2]) << 8) |
			(*address)[3]);
		return local_address_is_gone(local, query.first, query.now);
	}
#endif
	socklen_t length = sizeof(local);
	if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&local),
			  &length) != 0)
		return false;
	return local_address_is_gone(local, query.first, query.now);
}

bool SlirpProvider::path_outage_unread() const noexcept
{
	return !path_state_.carried() ||
		path_state_.uncarried_reports() != path_outages_seen_;
}

void SlirpProvider::reset_flows_on_lost_addresses() noexcept
{
	/* An outage the loop has not read yet is as good as one it has: it
	 * cancels what the hold was for, and the next pass starts over. */
	if (!slirp_ || path_outage_unread())
		return;
	/* Judged against the addresses as they are now, not only as they were
	 * at the report: the hold exists so that an address that came back is
	 * not lost. A table that cannot be read judges nothing. */
	auto fresh = read_address_table();
	if (!fresh || path_outage_unread())
		return;
	/* Moves only: this function cannot throw. */
	static_assert(std::is_nothrow_move_assignable_v<
		      std::optional<HostAddresses>>);
	const auto first = std::move(address_reset_table_);
	address_reset_table_ = std::move(fresh);
	const HostAddresses &now = *address_reset_table_;
	/* Without the table the hold started with, no address is known to
	 * have been missing for the hold. */
	if (first) {
#if UML_SLIRP_TESTING
		address_reset_calls_for_testing_.fetch_add(
			1, std::memory_order_release);
#endif
		LostAddressQuery query{*this, *first, now};
		const int reset = slirp_reset_tcp_flows(
			slirp_.get(), lost_address_callback, &query);
		if (reset > 0)
			std::fprintf(stderr, "Darwin user network: reset %d "
					     "connection%s bound to an address "
					     "the host no longer has\n", reset,
				     reset == 1 ? "" : "s");
	}
	/* An address that went missing since the hold started has not been
	 * gone for the hold: its connections get a hold of their own. */
	if (!first || now.lost_since(*first))
		address_reset_hold_.reported(true, true, host_clock_now());
}

void SlirpProvider::event_loop(std::stop_token stop_token) noexcept
{
	PollState state;
	/* The wake pipe keeps index 0, which SLiRP is never handed. Clearing
	 * keeps the capacity, so the wake entry never allocates after this. */
	state.descriptors.reserve(64);
	while (!stop_token.stop_requested()) {
		std::uint32_t timeout = 1000;
#if UML_SLIRP_TESTING
		event_loop_passes_for_testing_.fetch_add(
			1, std::memory_order_relaxed);
#endif
		process_transmit_queue();
		check_address_reset();
		state.descriptors.clear();
		state.descriptors.push_back({wake_pipe_[0].get(), POLLIN, 0});
		slirp_pollfds_fill_socket(slirp_.get(), &timeout,
					 add_poll_descriptor, &state);
		timeout = timer_timeout(timeout);
		timeout = address_reset_timeout(timeout);
		const int result = poll(state.descriptors.data(),
					static_cast<nfds_t>(state.descriptors.size()),
					static_cast<int>(timeout));
		if ((state.descriptors.front().revents & POLLIN) != 0) {
			std::array<std::byte, 64> buffer{};
			while (::read(wake_pipe_[0].get(), buffer.data(),
				      buffer.size()) > 0)
				;
		}
		if (stop_token.stop_requested())
			break;
		slirp_pollfds_poll(slirp_.get(), result < 0,
				  get_poll_events, &state);
		handle_expired_timers();
	}
}

const SlirpCb &SlirpProvider::callbacks() noexcept
{
	static const SlirpCb callbacks{
		.send_packet = send_packet_callback,
		.guest_error = guest_error_callback,
		.clock_get_ns = clock_callback,
		.timer_free = timer_free_callback,
		.timer_mod = timer_modify_callback,
		.notify = notify_callback,
		.timer_new_opaque = timer_new_callback,
		.register_poll_socket = register_poll_socket,
		.unregister_poll_socket = unregister_poll_socket,
	};
	return callbacks;
}

} // namespace uml::network
