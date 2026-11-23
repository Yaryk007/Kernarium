// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uml::runtime {

namespace detail {

/* A marker as a Knuth-Morris-Pratt automaton: a byte moves the match to
 * the longest prefix of the marker that ends at it. */
struct Marker final {
	std::string_view text;
	std::array<std::uint8_t, 64> fallback{};

	explicit constexpr Marker(std::string_view marker) : text{marker}
	{
		unsigned matched = 0;
		for (std::size_t index = 1; index < text.size(); ++index) {
			while (matched > 0 && text[matched] != text[index])
				matched = fallback[matched - 1];
			if (text[matched] == text[index])
				++matched;
			fallback[index] = static_cast<std::uint8_t>(matched);
		}
	}

	/* The match after `byte`, from `matched` characters of the marker;
	 * equal to the marker's length when `byte` completes it. */
	[[nodiscard]] constexpr unsigned advance(unsigned matched,
		char byte) const noexcept
	{
		while (matched > 0 && text[matched] != byte)
			matched = fallback[matched - 1];
		if (text[matched] == byte)
			++matched;
		return matched;
	}
};

} // namespace detail

/*
 * What the console of an Android guest says of its boot.
 *
 * Android's init logs `processing action (sys.boot_completed=1)` when the
 * system is up, and `starting service 'recovery'` when the recovery image is.
 * Both are the moments Andarium's boot screen ends at, so they are what the
 * host calls a completed boot (ConsoleScanner in AndariumCore reads the same
 * two lines). apexd logs `markBootCompleted() received by ApexService` when
 * the system reports that it booted, a moment before init's action runs: a
 * restart asked for in between leaves init's line unwritten, and apexd's is
 * a second mark of the same boot. A Linux desktop guest's console says
 * nothing of the kind.
 *
 * A boot that has not completed is only known to be unfinished if init's
 * output reaches the signal at all: a console on a descriptor the record does
 * not keep (ssl0=null, a pty), or one turned down to the kernel's own lines,
 * shows no completion on a healthy boot either. `init_observed` is whether
 * init's earliest line, `init first stage started!`, has been seen, which
 * every boot of the user image prints, system and recovery alike; a boot with
 * neither that nor a completion is unobserved, not unfinished.
 *
 * The signal reads the bytes the kernel writes to its console, as they pass
 * through kernel_write (session_record.hpp), so a marker that a write splits
 * is found all the same. It takes no lock and allocates nothing: a signal
 * handler or a second console thread may feed it, and a chunk that races
 * another is scanned again after it, so no chunk is lost. Once the boot has
 * completed it reads nothing further. The state is one for every kept
 * descriptor: the kernel's console is one stream, printk writes a record to
 * it whole, and what any descriptor of a record shows is that stream.
 *
 * `reset` begins another boot. It needs the quiescence the run loop gives it
 * (the old kernel's threads are gone before the next session begins): a feed
 * in flight across a reset could publish its stale result into the new state.
 */
class ConsoleBootSignal final {
public:
	/* The text of the lines, which a line holds anywhere. */
	static constexpr std::string_view boot_completed_marker{
		"processing action (sys.boot_completed=1)"};
	static constexpr std::string_view apex_marker{
		"apexd: markBootCompleted() received by ApexService"};
	static constexpr std::string_view recovery_marker{
		"init: starting service 'recovery'"};
	static constexpr std::string_view init_marker{
		"init: init first stage started!"};

	/* A new boot begins: nothing has been said. */
	void reset() noexcept { state_.store(0, std::memory_order_release); }

	/* The kernel wrote `length` bytes to a console. */
	void feed(const void *data, std::size_t length) noexcept
	{
		if (data == nullptr || length == 0)
			return;
		const auto *bytes = static_cast<const unsigned char *>(data);
		std::uint64_t state = state_.load(std::memory_order_acquire);
		for (;;) {
			if ((state & completed_bit) != 0)
				return;
			const std::uint64_t next = scan(state, bytes, length);
			if (next == state)
				return;
			if (state_.compare_exchange_weak(state, next,
				    std::memory_order_acq_rel,
				    std::memory_order_acquire))
				return;
		}
	}

	/* Whether a line that ends the boot has been written since the last
	 * reset. */
	[[nodiscard]] bool completed() const noexcept
	{
		return (state_.load(std::memory_order_acquire) & completed_bit) != 0;
	}

	/* Whether init's output has reached the signal since the last reset: its
	 * first line, or any line of a boot that completed. */
	[[nodiscard]] bool init_observed() const noexcept
	{
		return (state_.load(std::memory_order_acquire) &
			(observed_bit | completed_bit)) != 0;
	}

private:
	static constexpr detail::Marker completed_line{boot_completed_marker};
	static constexpr detail::Marker apex_line{apex_marker};
	static constexpr detail::Marker recovery_line{recovery_marker};
	static constexpr detail::Marker init_line{init_marker};
	static_assert(boot_completed_marker.size() < 64 &&
		apex_marker.size() < 64 && recovery_marker.size() < 64 &&
		init_marker.size() < 64, "a marker's progress outgrew its byte");
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
		"the signal is fed where no lock may be taken");

	/* The state: how much of each marker matches at the end of what was read
	 * (a byte each), whether init's first line was seen, and whether any
	 * line that ends the boot was. */
	static constexpr unsigned recovery_shift = 8;
	static constexpr unsigned apex_shift = 16;
	static constexpr unsigned init_shift = 24;
	static constexpr std::uint64_t observed_bit = std::uint64_t{1} << 62;
	static constexpr std::uint64_t completed_bit = std::uint64_t{1} << 63;

	[[nodiscard]] static std::uint64_t scan(std::uint64_t state,
		const unsigned char *bytes, std::size_t length) noexcept
	{
		unsigned boot = state & 0xffU;
		unsigned recovery = (state >> recovery_shift) & 0xffU;
		unsigned apex = (state >> apex_shift) & 0xffU;
		unsigned init = (state >> init_shift) & 0xffU;
		bool observed = (state & observed_bit) != 0;
		for (std::size_t index = 0; index < length; ++index) {
			const char byte = static_cast<char>(bytes[index]);
			boot = completed_line.advance(boot, byte);
			recovery = recovery_line.advance(recovery, byte);
			apex = apex_line.advance(apex, byte);
			if (boot == completed_line.text.size() ||
			    recovery == recovery_line.text.size() ||
			    apex == apex_line.text.size())
				return completed_bit | observed_bit;
			if (!observed) {
				init = init_line.advance(init, byte);
				if (init == init_line.text.size()) {
					observed = true;
					init = 0;
				}
			}
		}
		return boot | (std::uint64_t{recovery} << recovery_shift) |
			(std::uint64_t{apex} << apex_shift) |
			(std::uint64_t{init} << init_shift) |
			(observed ? observed_bit : 0);
	}

	std::atomic<std::uint64_t> state_{0};
};

} // namespace uml::runtime
