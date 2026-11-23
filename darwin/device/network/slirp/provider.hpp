// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_network.h"
#include "address_reset.hpp"
#include "dns_resolver.hpp"
#include "host_addresses.hpp"
#include "network_counters.hpp"
#include "platform/descriptor.hpp"
#include "platform/host_network_path.hpp"

#include <libslirp.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef UML_SLIRP_TESTING
#define UML_SLIRP_TESTING 0
#endif

namespace uml::network {

inline constexpr std::size_t frame_size = DARWIN_UML_NET_FRAME_SIZE;
inline constexpr std::size_t ring_size = 256;
/* How long a guest transmit waits for the event loop to free a ring slot
 * before the frame is refused (see SlirpProvider::write). The loop frees one
 * within its next pass; the bound only stops a wedged loop holding a vCPU, so
 * it must stay well above any scheduling delay a loaded host imposes. */
inline constexpr std::chrono::milliseconds transmit_space_wait{500};

/* The user-mode network the guest sees. `guest_address` is the single DHCP
 * lease, so it is also the default target of a host forward. */
inline constexpr const char *network_address = "10.0.2.0";
inline constexpr const char *network_mask = "255.255.255.0";
inline constexpr const char *host_alias_address = "10.0.2.2";
inline constexpr const char *guest_address = "10.0.2.15";
inline constexpr const char *nameserver_address = "10.0.2.3";
/* The hardware address the guest's interface has unless a caller states one
 * (SlirpProvider::set_mac). */
inline constexpr std::array<unsigned char, 6> default_guest_mac{
	0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
/* The first five bytes of the addresses SLiRP's router and servers answer
 * from (52:55:0a:00:02:xx for 10.0.2.xx): a guest address that began with them
 * could not be told from the gateway's or the name server's.
 * darwin/device/block/android_identity.hpp keeps the same prefix for the
 * identity's own refusal. */
inline constexpr std::array<unsigned char, 5> router_mac_prefix{
	0x52, 0x55, 0x0a, 0x00, 0x02};
inline constexpr dns::Ipv4Address nameserver_ipv4{10, 0, 2, 3};
inline constexpr dns::Ipv4Address host_alias_ipv4{10, 0, 2, 2};
/* A frame that needs the host's addresses (dns::needs_host_check) finds them
 * read again when the last good read is older than this. */
inline constexpr std::chrono::seconds host_address_max_age{1};
/* A read that fails is not a fresh table: the next frame that needs it reads
 * again, at most this often. */
inline constexpr std::chrono::milliseconds host_address_retry_interval{100};
/* A table whose last good read is older than host_address_max_age plus this
 * is no longer trusted: what the table judges is refused until a read
 * succeeds, the datagrams of a flow already open included. */
inline constexpr std::chrono::seconds host_address_failure_grace{3};

/* One host listening socket forwarded into the guest. Registrations are
 * accepted while the provider is not open; the provider owns replaying them
 * onto each SLiRP instance it creates, so a stop/reopen keeps the same
 * forwards. */
struct HostForward final {
	bool udp{};
	std::string host_address{"127.0.0.1"};
	std::uint16_t host_port{};
	std::string guest_address{uml::network::guest_address};
	std::uint16_t guest_port{};
};

/* Parse "[PROTO:][HOSTADDR:]HOSTPORT-[GUESTADDR:]GUESTPORT". PROTO is tcp or
 * udp and defaults to tcp, HOSTADDR defaults to loopback so a forward is not
 * reachable off the machine unless it says so, and GUESTADDR defaults to the
 * DHCP lease. Returns false on any malformed field. */
[[nodiscard]] bool parse_host_forward(
	std::string_view specification, HostForward &forward) noexcept;

struct Frame final {
	std::size_t length{};
	std::array<std::byte, frame_size> data{};
};

struct Timer final {
	SlirpTimerId id{};
	void *callback_opaque{};
	std::int64_t expires_ms{-1};
};

/* The descriptors one event-loop pass polls: the wake pipe first, then one
 * per socket SLiRP asks for, however many flows the guest has open. The
 * vector is kept across passes, so a steady flow count allocates nothing. */
struct PollState final {
	std::vector<pollfd> descriptors;
};

struct SlirpDeleter final {
	void operator()(Slirp *slirp) const noexcept;
};

class SlirpProvider final {
public:
	SlirpProvider() = default;

	SlirpProvider(const SlirpProvider &) = delete;
	SlirpProvider &operator=(const SlirpProvider &) = delete;
	~SlirpProvider();

    int open(std::span<unsigned char, 6> mac, unsigned int &mtu);
	/* The hardware address the next open() gives the guest's interface, in
	 * place of default_guest_mac. Returns 0, or -EINVAL for a multicast
	 * address, an all-zero one or one of the router's and servers' own
	 * (router_mac_prefix). DHCP and DNS answer whatever address the guest
	 * uses, so nothing else depends on the value. */
	int set_mac(std::span<const unsigned char, 6> mac);
	/* Register a host forward. It is recorded and installed on each SLiRP
	 * instance open() creates, before that instance's worker starts: libslirp
	 * takes no lock, so nothing may join an instance its worker is walking.
	 * Returns 0, -EINVAL for a zero port, or -EBUSY while the provider is
	 * open. A host port that cannot be claimed is reported at open(). */
	int add_forward(const HostForward &forward);
	/* The active-session IRQ descriptor is borrowed from the provider and keeps
	 * the same numeric identity across stop/reopen. It closes only when the
	 * provider itself is destroyed, after users have released registrations. */
    [[nodiscard]] int irq_fd() const;
    int read(std::span<std::byte> buffer);
    int write(std::span<const std::byte> frame);
	void stop() noexcept;
	/* What the provider has counted since it was made, and across the
	 * links it brings up; any thread, and it takes no lock. */
	[[nodiscard]] NetworkCounts counters() const noexcept
	{
		return counters_.read();
	}

#if UML_SLIRP_TESTING
	/* A guest forward for a test: a connection the guest opens to
	 * `guest_address:guest_port` goes to the Unix socket `path`, where the
	 * test listens. It stands in for the host listener the guest can no
	 * longer reach through the gateway, which the product has no way to
	 * forward to. Applies to the next open(). The environment variable
	 * UML_TEST_SLIRP_GUEST_FORWARD ("ADDRESS:PORT=PATH", more than one
	 * separated by commas) adds the same forwards, for a test launcher. */
	struct GuestForward final {
		std::string guest_address{host_alias_address};
		std::uint16_t guest_port{};
		std::string path;
	};
	void add_guest_forward_for_testing(GuestForward forward)
	{
		guest_forwards_for_testing_.push_back(std::move(forward));
	}
	/* The host's addresses come from `reader` instead of the system. */
	void set_host_address_reader_for_testing(
		std::function<std::optional<HostAddresses>()> reader)
	{
		address_reader_for_testing_ = std::move(reader);
	}
	/* The age of the host's addresses is told by `clock` instead of the
	 * monotonic clock. The worker calls it, so it must be callable from
	 * another thread. */
	void set_clock_for_testing(
		std::function<std::chrono::steady_clock::time_point()> clock)
	{
		clock_for_testing_ = std::move(clock);
	}
	/* Times the provider has read the host's addresses. */
	[[nodiscard]] std::uint64_t host_address_reads_for_testing() const noexcept
	{
		return host_address_reads_for_testing_.load(
			std::memory_order_acquire);
	}
	void pause_transmit_for_testing() noexcept;
	[[nodiscard]] bool wait_for_transmit_pause_for_testing() noexcept;
	void resume_transmit_for_testing() noexcept;
	[[nodiscard]] std::size_t transmit_space_waiters_for_testing() noexcept;
	void pause_next_write_before_commit_for_testing() noexcept;
	[[nodiscard]] bool wait_for_write_pause_for_testing() noexcept;
	void release_write_for_testing() noexcept;
	void pause_next_read_before_notification_for_testing() noexcept;
	[[nodiscard]] bool wait_for_read_pause_for_testing() noexcept;
	void release_read_for_testing() noexcept;
	[[nodiscard]] bool wait_for_receive_cleanup_for_testing() noexcept;
	[[nodiscard]] slirp_ssize_t enqueue_received_for_testing(
		std::span<const std::byte> frame) noexcept;
	[[nodiscard]] bool readiness_channels_drained_for_testing() const noexcept;
	[[nodiscard]] std::uint64_t processed_transmits_for_testing() const noexcept
	{
		return processed_transmits_for_testing_.load(
			std::memory_order_acquire);
	}
	/* Passes the event loop has made: a loop that never waits on its
	 * sockets goes through them without pause. */
	[[nodiscard]] std::uint64_t event_loop_passes_for_testing() const noexcept
	{
		return event_loop_passes_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* Forwards installed while a worker already ran on the instance. */
	[[nodiscard]] std::size_t
	forwards_installed_while_running_for_testing() const noexcept
	{
		return forwards_installed_while_running_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* Internet-family host sockets SLiRP has made: a guest's DNS must
	 * never add one. */
	[[nodiscard]] std::size_t internet_sockets_for_testing() const noexcept
	{
		return internet_sockets_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* Applies to the next open(). */
	void set_dns_interface_for_testing(std::uint32_t index) noexcept
	{
		dns_interface_for_testing_ = index;
	}
	/* The resolver's wiring checks (dns_resolver.hpp). */
	void inject_dns_submit_error_for_testing(std::int32_t error) noexcept
	{
		resolver_.inject_submit_error_for_testing(error);
	}
	void inject_dns_reply_error_for_testing(std::int32_t error) noexcept
	{
		resolver_.inject_reply_error_for_testing(error);
	}
	void set_dns_deadline_for_testing(
		std::chrono::milliseconds deadline) noexcept
	{
		resolver_.set_deadline_for_testing(deadline);
	}
	void hold_dns_answers_for_testing(bool held) noexcept
	{
		resolver_.hold_answers_for_testing(held);
	}
	[[nodiscard]] std::uint64_t dns_answers_dropped_for_testing() const noexcept
	{
		return resolver_.answers_dropped_for_testing();
	}
	void set_dns_pending_limit_for_testing(std::size_t limit) noexcept
	{
		resolver_.set_pending_limit_for_testing(limit);
	}
	/* The next open() finds no socket path for DNS over TCP. */
	void set_dns_streams_unavailable_for_testing(bool unavailable) noexcept
	{
		dns_streams_unavailable_for_testing_ = unavailable;
	}
	void set_dns_before_answer_for_testing(void (*hook)(void *),
					       void *context) noexcept
	{
		resolver_.set_before_answer_for_testing(hook, context);
	}
	/* The host's network path, which the resolver reads when it answers.
	 * The product starts the system's monitor at open(); the test build does
	 * not unless asked, so a machine with no network behaves as one with
	 * (the probes need none), and a test sets the state itself. Applies to
	 * the next open(), which drops a monitor that is no longer asked for. */
	void set_path_monitor_for_testing(bool enabled) noexcept
	{
		path_monitor_for_testing_ = enabled;
	}
	[[nodiscard]] uml::platform::HostNetworkPathState &
	path_state_for_testing() noexcept
	{
		return path_state_;
	}
	/* The local address of a guest flow's host socket is `reader(descriptor)`
	 * instead of the socket's own, so that flows through Unix-domain guest
	 * forwards, which have no address, stand for flows bound to different
	 * addresses. */
	void set_flow_local_address_for_testing(
		std::function<std::optional<HostIpv4>(slirp_os_socket)> reader)
	{
		flow_local_address_for_testing_ = std::move(reader);
	}
	/* Runs one pass of the event loop now, as a report or a packet does. */
	void wake_for_testing() noexcept
	{
		wake();
	}
	/* Path reports the event loop has read into the hold, and the times it
	 * has looked at the hold: a test waits on them for the loop to have seen
	 * what it did. */
	[[nodiscard]] std::uint64_t path_reports_handled_for_testing() const noexcept
	{
		return path_reports_handled_for_testing_.load(
			std::memory_order_acquire);
	}
	[[nodiscard]] std::uint64_t address_reset_checks_for_testing() const noexcept
	{
		return address_reset_checks_for_testing_.load(
			std::memory_order_acquire);
	}
	/* Times a hold ended with the path still carried and the table read, so
	 * that libslirp was asked which connections to reset. */
	[[nodiscard]] std::uint64_t address_reset_calls_for_testing() const noexcept
	{
		return address_reset_calls_for_testing_.load(
			std::memory_order_acquire);
	}
	/* Monitors this provider has made: one for its life, however often it is
	 * stopped and opened, unless the system gave none (then the next open
	 * makes another). */
	[[nodiscard]] std::uint64_t path_monitors_made_for_testing() const noexcept
	{
		return path_monitors_made_for_testing_.load(
			std::memory_order_relaxed);
	}
	/* The monitor the provider holds now, for a test that waits for what
	 * its thread says of it. */
	[[nodiscard]] const uml::platform::HostNetworkPathMonitor *
	path_monitor_for_testing() const noexcept
	{
		return path_monitor_.get();
	}
	/* Makes each monitor in place of the platform's own, so that a test has
	 * the system give none. Applies to the next monitor made. */
	using PathMonitorFactory = std::function<
		std::unique_ptr<uml::platform::HostNetworkPathMonitor>(
			uml::platform::HostNetworkPathState &)>;
	void set_path_monitor_factory_for_testing(PathMonitorFactory factory)
	{
		path_monitor_factory_for_testing_ = std::move(factory);
	}
	[[nodiscard]] HostResolver &resolver_for_testing() noexcept
	{
		return resolver_;
	}
#endif

private:
	static const SlirpCb &callbacks() noexcept;
	static SlirpProvider *from_opaque(void *opaque) noexcept;
	static slirp_ssize_t send_packet_callback(
		const void *data, std::size_t length, void *opaque) noexcept;
	static void guest_error_callback(const char *message,
					 void *opaque) noexcept;
	static std::int64_t clock_callback(void *opaque) noexcept;
	static void *timer_new_callback(SlirpTimerId id,
					void *callback_opaque,
					void *opaque) noexcept;
	static void timer_free_callback(void *timer, void *opaque) noexcept;
	static void timer_modify_callback(void *timer, std::int64_t expires_ms,
					  void *opaque) noexcept;
	static void notify_callback(void *opaque) noexcept;
	static void register_poll_socket(slirp_os_socket descriptor,
					 void *opaque) noexcept;
	static void unregister_poll_socket(slirp_os_socket descriptor,
					   void *opaque) noexcept;
	static int add_poll_descriptor(slirp_os_socket descriptor, int events,
				       void *opaque) noexcept;
	static int get_poll_events(int index, void *opaque) noexcept;
	/* Called with start_mutex_ held and slirp_ valid. */
	int install_forward(const HostForward &forward) noexcept;
	/* Reads the host's addresses into host_addresses_; false, with the table
	 * as it was and as old, when the system cannot list them. */
	bool read_host_addresses_now() noexcept;
	/* The worker's part, for a frame that needs the table: reads it again
	 * when the last good read is older than host_address_max_age, and after a
	 * failed read not before host_address_retry_interval has passed. */
	void refresh_host_addresses_if_stale() noexcept;
	/* Whether the table can still judge traffic: it can until the last
	 * good read is older than host_address_max_age plus
	 * host_address_failure_grace. */
	[[nodiscard]] bool host_addresses_trusted() const noexcept;
	[[nodiscard]] std::chrono::steady_clock::time_point
	host_clock_now() const noexcept;

    slirp_ssize_t enqueue_received(std::span<const std::byte> frame);
	/* Answers a guest frame meant for the name server; false passes it to
	 * SLiRP. */
	bool answer_locally(std::span<const std::byte> frame) noexcept;
	void process_transmit_queue() noexcept;
	[[nodiscard]] bool transmit_permitted() noexcept;
	void wake() noexcept;
	/* The event loop's part, before it sleeps: reads a new path report into
	 * the hold, and resets the connections bound to a lost address when the
	 * hold has ended. A report wakes the loop (notify_callback), which is the
	 * only thread that calls SLiRP; nothing polls for one. */
	void check_address_reset() noexcept;
	/* How long the loop may sleep: the time left of a hold if that is
	 * less than `timeout`. Nothing exists while no hold is running. */
	[[nodiscard]] std::uint32_t address_reset_timeout(
		std::uint32_t timeout) const noexcept;
	void reset_flows_on_lost_addresses() noexcept;
	/* Whether the system has reported an outage that the loop has not read
	 * into the hold yet, or reports one now. */
	[[nodiscard]] bool path_outage_unread() const noexcept;
	/* The host's addresses, read now; none when the system cannot list
	 * them. */
	[[nodiscard]] std::optional<HostAddresses> read_address_table() noexcept;
	struct LostAddressQuery;
	static bool lost_address_callback(slirp_os_socket descriptor,
					  void *opaque) noexcept;
#if UML_SLIRP_TESTING
	void pause_write_before_commit_for_testing() noexcept;
	void pause_read_before_notification_for_testing() noexcept;
	void notify_receive_cleanup_for_testing() noexcept;
	void install_guest_forwards_for_testing() noexcept;
#endif
	void event_loop(std::stop_token stop_token) noexcept;
	[[nodiscard]] std::uint32_t timer_timeout(
		std::uint32_t timeout) const noexcept;
	void handle_expired_timers() noexcept;

	mutable std::mutex start_mutex_;
	std::mutex receive_mutex_;
	std::mutex transmit_mutex_;
	/* Signalled when the event loop frees a transmit slot while a writer
	 * waits; guarded by transmit_mutex_. */
	std::condition_variable transmit_space_;
	std::size_t transmit_space_waiters_{};
#if UML_SLIRP_TESTING
	std::mutex transmit_pause_mutex_;
	std::condition_variable transmit_pause_condition_;
	std::mutex io_pause_mutex_;
	std::condition_variable io_pause_condition_;
#endif
	std::array<Frame, ring_size> receive_ring_{};
	std::array<Frame, ring_size> transmit_ring_{};
	std::size_t receive_head_{};
	std::size_t receive_tail_{};
	std::size_t transmit_head_{};
	std::size_t transmit_tail_{};
	std::list<std::unique_ptr<Timer>> timers_;
	std::vector<HostForward> forwards_;
	/* Guarded by start_mutex_. */
	std::array<unsigned char, 6> mac_{default_guest_mac};
	std::unique_ptr<Slirp, SlirpDeleter> slirp_;
	std::array<uml::platform::UniqueDescriptor, 2> wake_pipe_;
	std::array<uml::platform::UniqueDescriptor, 2> receive_pipe_;
	/* Declared before what counts into them. */
	NetworkCounters counters_;
	TrafficObserver observer_{counters_};
	/* The host's network path: the monitor writes it from the system's
	 * thread and the resolver reads it when it answers. The first open()
	 * makes the monitor, which then lives as long as the provider and keeps
	 * the state current across a stop and reopen; the state outlives it. With
	 * no monitor, or one the system gave no monitor, the state is usable again
	 * when the provider stops. */
	uml::platform::HostNetworkPathState path_state_;
	std::unique_ptr<uml::platform::HostNetworkPathMonitor> path_monitor_;
	/* The hold and the table it was last judged against, and the reports
	 * already read: the event loop's own, set up by open() before the worker
	 * exists, so that a hold never outlives the instance it was started
	 * for. */
	AddressResetHold address_reset_hold_;
	std::optional<HostAddresses> address_reset_table_;
	std::uint64_t path_reports_seen_{};
	std::uint64_t path_outages_seen_{};
	HostResolver resolver_;
	std::uint16_t next_identification_{1};
	/* Where SLiRP's guest forward carries DNS over TCP to the resolver;
	 * set by open() before the worker exists. */
	bool name_server_stream_{};
	bool gateway_stream_{};
	/* What the guest may not address: open() fills the table before the
	 * worker exists, and the worker alone reads and refreshes it after. */
	HostAddresses host_addresses_;
	/* When the table was last read well; a read that fails leaves it alone. */
	std::chrono::steady_clock::time_point host_addresses_read_at_{};
	/* After a failed read: when to try again. */
	std::chrono::steady_clock::time_point host_address_retry_at_{};
	bool host_addresses_distrust_logged_{};
	std::jthread worker_;
	std::atomic_bool started_{false};
	std::atomic_bool stopping_{true};
#if UML_SLIRP_TESTING
	std::atomic_bool transmit_pause_requested_for_testing_{};
	bool transmit_pause_reached_for_testing_{};
	std::atomic_uint64_t processed_transmits_for_testing_{};
	std::atomic_uint64_t event_loop_passes_for_testing_{};
	std::atomic_size_t forwards_installed_while_running_for_testing_{};
	std::atomic_size_t internet_sockets_for_testing_{};
	std::uint32_t dns_interface_for_testing_{};
	bool dns_streams_unavailable_for_testing_{};
	bool path_monitor_for_testing_{};
	std::atomic_uint64_t path_monitors_made_for_testing_{};
	PathMonitorFactory path_monitor_factory_for_testing_;
	bool write_pause_requested_for_testing_{};
	bool write_pause_reached_for_testing_{};
	bool release_write_for_testing_{};
	bool read_pause_requested_for_testing_{};
	bool read_pause_reached_for_testing_{};
	bool release_read_for_testing_{};
	bool receive_cleanup_reached_for_testing_{};
	std::vector<GuestForward> guest_forwards_for_testing_;
	std::function<std::optional<HostAddresses>()> address_reader_for_testing_;
	std::function<std::optional<HostIpv4>(slirp_os_socket)>
		flow_local_address_for_testing_;
	std::atomic_uint64_t address_reset_calls_for_testing_{};
	std::atomic_uint64_t path_reports_handled_for_testing_{};
	std::atomic_uint64_t address_reset_checks_for_testing_{};
	std::function<std::chrono::steady_clock::time_point()> clock_for_testing_;
	std::atomic_uint64_t host_address_reads_for_testing_{};
#endif
};

} // namespace uml::network
