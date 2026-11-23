// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * When the guest's TCP connections that left from an address the host has
 * lost are ended with a reset.
 *
 * A host socket keeps the local address it connected from. When the system
 * moves the default path to another interface (Wi-Fi to cellular), the old
 * address goes away and the socket is useless: one the guest writes on
 * hangs until the host stack gives up, and ends in a clean close; one that
 * only the peer writes on is never told at all. The guest is better served
 * by a reset, so that its application sees an error and retries over the new
 * path.
 *
 * The rule is the pair below. A path report that says an interface carries
 * the path starts a hold; when the hold has elapsed and the path is still
 * carried, the connections whose local address the host has not had for the
 * whole hold are reset. An outage (no interface carries the path) cancels the
 * hold and resets nothing, since the same address may come back and the
 * connections resume; so does an address that comes back before the hold
 * ends. An address first found missing when the hold ends has not been gone
 * for the hold: it starts one of its own. Both parts are functions of
 * values, so a unit test needs no socket and no clock.
 */

#include "host_addresses.hpp"

#include <sys/socket.h>

#include <chrono>
#include <optional>

namespace uml::network {

/* How long the default path must stay carried before a connection bound to a
 * lost address is reset. A Wi-Fi rejoin gives the address back within a few
 * seconds, and the host stack gives up a connection the guest writes on after
 * about 18 s: the hold is more than twice the first and under half the
 * second. */
inline constexpr std::chrono::seconds address_reset_hold{8};

class AddressResetHold final {
public:
	using Clock = std::chrono::steady_clock;

	explicit AddressResetHold(Clock::duration hold = address_reset_hold) noexcept
		: hold_{hold}
	{
	}

	/* The system reported its path at `now`. `carried` is whether an
	 * interface carries it; `addresses_changed` is whether the host's
	 * address set differs from the one at the previous report. A path that
	 * is not carried ends the hold. A carried one starts it, and an address
	 * set that changed during it starts it again, so the hold is always the
	 * time since the addresses last changed; a report that changes
	 * nothing leaves the hold running. */
	void reported(bool carried, bool addresses_changed,
		      Clock::time_point now) noexcept
	{
		if (!carried)
			deadline_.reset();
		else if (!deadline_ || addresses_changed)
			deadline_ = now + hold_;
	}

	/* Whether a hold is running. */
	[[nodiscard]] bool pending() const noexcept
	{
		return deadline_.has_value();
	}

	/* The time left of a running hold, none when there is none. */
	[[nodiscard]] std::optional<Clock::duration>
	remaining(Clock::time_point now) const noexcept
	{
		if (!deadline_)
			return std::nullopt;
		return *deadline_ > now ? *deadline_ - now : Clock::duration::zero();
	}

	/* True once when the hold has elapsed and the path is still carried:
	 * the moment to reset. Either way an elapsed hold ends. */
	[[nodiscard]] bool take_due(bool carried, Clock::time_point now) noexcept
	{
		if (!deadline_ || now < *deadline_)
			return false;
		deadline_.reset();
		return carried;
	}

	/* Ends a running hold: the provider that held it is gone. */
	void clear() noexcept
	{
		deadline_.reset();
	}

private:
	Clock::duration hold_;
	std::optional<Clock::time_point> deadline_;
};

/* Whether a host socket's local address is one the host has lost: an IPv4
 * address, not the unspecified one a socket that has not connected yet
 * reports, that no interface held in `first`, the table the hold started
 * with, or in `now`, the table read when it ended. An address that was there
 * at the start and is gone now is not lost yet: the hold starts again for
 * it. An interface's broadcast address is not an address a socket connects
 * from, so it holds nothing. Anything else (a Unix-domain socket, an address
 * of another family) is not the host's address to lose. */
[[nodiscard]] bool local_address_is_gone(const sockaddr_storage &local,
					 const HostAddresses &first,
					 const HostAddresses &now) noexcept;

} // namespace uml::network
