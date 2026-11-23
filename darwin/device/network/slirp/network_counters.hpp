// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * What the guest's user network counted, for the session record and so for a
 * bug report: numbers only. A counter never holds an address, a port, a host
 * name or the name of a query; the provider's tables that pair a reply with
 * its request stay in memory and are never read out.
 *
 * The counters belong to the provider and only grow: a reader that wants one
 * guest session's share subtracts the reading it took when the session began.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::network {

/* One reading. Every field counts events since the provider was made. */
struct NetworkCounts final {
	/* Queries the system resolver answered, a missing name (NXDOMAIN) and a
	 * name with no record of the type (NODATA) included: both are answers,
	 * except one given while the host has no network path, which the guest
	 * is told is SERVFAIL and which counts as failed. */
	std::uint64_t dns_answered{};
	/* Queries the system refused the application (kDNSServiceErr_PolicyDenied
	 * or _NotPermitted): the signal that iOS denies Andarium its network. */
	std::uint64_t dns_policy_denied{};
	/* Queries the system resolver had not answered by the deadline. */
	std::uint64_t dns_timed_out{};
	/* Queries that failed for any other reason, and the negative answers
	 * given while the host has no network path. */
	std::uint64_t dns_failed{};
	/* The guest's DHCPDISCOVER and DHCPREQUEST messages, and the DHCPACK
	 * messages that answered them. */
	std::uint64_t dhcp_discovers{};
	std::uint64_t dhcp_requests{};
	std::uint64_t dhcp_acks{};
	/* The guest's attempts to open a TCP connection that the provider hands
	 * to SLiRP, which connects to the address on the host's network. DNS over
	 * TCP to the name server or the gateway is answered by SLiRP's guest
	 * forward and says nothing about the network, so it is no attempt (DNS has
	 * its own counts); a retransmitted SYN is not another attempt either. Each
	 * attempt ends as the guest was told, by one of:
	 *  - established: a SYN-ACK;
	 *  - unreachable: an ICMP net or host unreachable (codes 0 and 1), which
	 *    SLiRP sends when connect() fails at once with anything but
	 *    ECONNREFUSED: no route, an address the host cannot reach;
	 *  - reset: an RST, which SLiRP sends for ECONNREFUSED at once, for a
	 *    connect that failed after it started (the errno is gone by then, so
	 *    a refusal and any other asynchronous failure look alike) and when
	 *    its own 75 s timer gives up on a connect nothing answered.
	 * The rest, `tcp_unanswered()`, had no answer when the reading was taken:
	 * the attempt is still under way, the guest gave up before SLiRP did, or
	 * a full table of waiting attempts pushed it out. */
	std::uint64_t tcp_attempts{};
	std::uint64_t tcp_established{};
	std::uint64_t tcp_unreachable{};
	std::uint64_t tcp_reset{};

	[[nodiscard]] constexpr std::uint64_t tcp_unanswered() const noexcept
	{
		const std::uint64_t answered =
			tcp_established + tcp_unreachable + tcp_reset;
		return tcp_attempts > answered ? tcp_attempts - answered : 0;
	}
};

/* How a query to the system resolver ended. */
enum class DnsOutcome : std::uint8_t {
	answered,
	policy_denied,
	timed_out,
	failed,
};

/* The outcome of a dns_sd error code, the code of an answer included. */
[[nodiscard]] DnsOutcome classify_dns_error(std::int32_t error) noexcept;

/* Relaxed atomic counters: any thread may add, any thread may read. */
class NetworkCounters final {
public:
	void dns(DnsOutcome outcome) noexcept;
	void dhcp_discover() noexcept { add(dhcp_discovers_); }
	void dhcp_request() noexcept { add(dhcp_requests_); }
	void dhcp_ack() noexcept { add(dhcp_acks_); }
	void tcp_attempt() noexcept { add(tcp_attempts_); }
	void tcp_established() noexcept { add(tcp_established_); }
	void tcp_unreachable() noexcept { add(tcp_unreachable_); }
	void tcp_reset() noexcept { add(tcp_reset_); }

	[[nodiscard]] NetworkCounts read() const noexcept;

private:
	static void add(std::atomic_uint64_t &counter) noexcept
	{
		counter.fetch_add(1, std::memory_order_relaxed);
	}

	std::atomic_uint64_t dns_answered_{};
	std::atomic_uint64_t dns_policy_denied_{};
	std::atomic_uint64_t dns_timed_out_{};
	std::atomic_uint64_t dns_failed_{};
	std::atomic_uint64_t dhcp_discovers_{};
	std::atomic_uint64_t dhcp_requests_{};
	std::atomic_uint64_t dhcp_acks_{};
	std::atomic_uint64_t tcp_attempts_{};
	std::atomic_uint64_t tcp_established_{};
	std::atomic_uint64_t tcp_unreachable_{};
	std::atomic_uint64_t tcp_reset_{};
};

/*
 * Reads the guest's DHCP and TCP connection set-up off the frames that cross
 * the provider, to count them. SLiRP answers a connect it cannot make with a
 * reset or an ICMP error, and the errno is gone by then, so the classes are
 * what the guest was told (NetworkCounts says what each one covers). A
 * connection nothing answered is the difference of the counts.
 *
 * One thread uses an observer: the provider's event loop, which carries every
 * frame both ways. `now` is the clock of that thread.
 */
class TrafficObserver final {
public:
	using Clock = std::chrono::steady_clock;

	/* A pending attempt older than this is not a retransmission of its SYN:
	 * a guest's retransmissions stop well before it (Linux gives up after
	 * about 127 s). */
	static constexpr std::chrono::seconds retransmission_window{130};
	static constexpr std::size_t pending_limit = 64;

	explicit TrafficObserver(NetworkCounters &counters) noexcept
		: counters_{counters}
	{
	}

	/* A frame from the guest that goes to SLiRP, at `now`. */
	void from_guest(std::span<const std::uint8_t> frame,
			Clock::time_point now) noexcept;
	/* The same at the present moment, which it reads for a SYN alone: the
	 * rest of the guest's frames, the bulk, never touch the clock. */
	void from_guest(std::span<const std::uint8_t> frame) noexcept;
	/* A frame from SLiRP to the guest. */
	void from_network(std::span<const std::uint8_t> frame) noexcept;
	/* Forgets the attempts waiting for an answer: a new link, while no frame
	 * crosses. */
	void reset() noexcept;
	/* The addresses (IPv4, host byte order; 0 for none) whose port 53 SLiRP's
	 * guest forward answers itself: a SYN to one is no connection attempt.
	 * Set while no frame crosses. */
	void serve_dns_streams(std::uint32_t first, std::uint32_t second) noexcept
	{
		stream_servers_ = {first, second};
	}

private:
	struct Pending final {
		std::uint32_t destination{};
		std::uint16_t destination_port{};
		std::uint16_t source_port{};
		Clock::time_point first_seen{};
		bool used{};
	};

	template <typename Now>
	void observe_guest(std::span<const std::uint8_t> frame,
			   Now now) noexcept;
	[[nodiscard]] bool serves_dns_stream(std::uint32_t destination) const noexcept
	{
		return destination != 0 &&
			(destination == stream_servers_[0] ||
			 destination == stream_servers_[1]);
	}
	void connection_attempt(std::uint32_t destination,
				std::uint16_t destination_port,
				std::uint16_t source_port,
				Clock::time_point now) noexcept;
	/* The pending attempt for these ends, which a reply resolves; null for
	 * none. */
	Pending *find(std::uint32_t destination, std::uint16_t destination_port,
		      std::uint16_t source_port) noexcept;

	NetworkCounters &counters_;
	std::array<std::uint32_t, 2> stream_servers_{};
	std::array<Pending, pending_limit> pending_{};
};

} // namespace uml::network
