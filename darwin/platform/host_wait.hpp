// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace uml::platform {

/*
 * What a host thread of the guest is waiting for, published by the thread
 * itself.
 *
 * A thread that enters a call which can block, a poll for a device service's
 * answer, a disk read, the commit of guest RAM, says so in its WaitCell for as
 * long as the call lasts. The stall watch (runtime/stall_watch.hpp) reads the
 * cells from another thread and so tells a vCPU that waits for the host from
 * one the system did not run. Nothing here takes a lock or allocates: a mark
 * is two stores to a word only its own thread writes, and a thread without a
 * cell marks nothing.
 */
enum class HostWait : std::uint8_t {
	none = 0,
	/* A vCPU with nothing to run, or parked: the wait is how it is meant to
	 * spend its time. */
	idle,
	/* A device service's answer to a kick (a poll on its socket). */
	answer,
	/* A read or receive on a host descriptor. */
	receive,
	/* A write or send on a host descriptor. */
	send,
	disk_read,
	disk_write,
	flush,
	/* Committing or releasing guest RAM. */
	memory,
	count,
};

/* The word of a cell: the kind in the low byte and a count of the thread's
 * marks above it, so that two readings with the same count and a kind are one
 * wait, and two with the same count are a stretch with no new wait in it. */
[[nodiscard]] constexpr HostWait wait_kind(std::uint64_t word) noexcept
{
	const auto kind = static_cast<std::uint8_t>(word & 0xffU);
	return kind < static_cast<std::uint8_t>(HostWait::count)
		? static_cast<HostWait>(kind) : HostWait::none;
}

[[nodiscard]] constexpr std::uint64_t wait_sequence(std::uint64_t word) noexcept
{
	return word >> 8;
}

class WaitCell final {
public:
	void begin(HostWait kind) noexcept
	{
		const std::uint64_t sequence = wait_sequence(
			word_.load(std::memory_order_relaxed)) + 1;
		word_.store(sequence << 8 | static_cast<std::uint8_t>(kind),
			std::memory_order_release);
	}

	/* Ends the wait begun last. `enclosing` is the kind of the wait it was
	 * begun inside, none for a mark that has no outer one; the sequence stays
	 * advanced, so a reader takes the outer wait for a new one. */
	void end(HostWait enclosing = HostWait::none) noexcept
	{
		word_.store((word_.load(std::memory_order_relaxed) & ~std::uint64_t{0xff}) |
				static_cast<std::uint8_t>(enclosing),
			std::memory_order_release);
	}

	/* Any thread. */
	[[nodiscard]] std::uint64_t word() const noexcept
	{
		return word_.load(std::memory_order_acquire);
	}

	/* Only before the cell is handed to a thread. */
	void reset() noexcept
	{
		word_.store(0, std::memory_order_relaxed);
	}

private:
	std::atomic<std::uint64_t> word_{0};
};

/* The calling thread's cell, null for a thread that has none. The kernel
 * thread registry (runtime/kernel_threads.hpp) sets it as a thread starts. */
inline thread_local WaitCell *current_wait_cell = nullptr;

/* Marks the calling thread as waiting for the length of the scope. A mark
 * made inside another, as a signal handler's write inside a wait the handler
 * interrupted, gives the thread back to the outer wait at its end. */
class ScopedHostWait final {
public:
	explicit ScopedHostWait(HostWait kind) noexcept
		: cell_{current_wait_cell}
	{
		if (cell_ != nullptr) {
			enclosing_ = wait_kind(cell_->word());
			cell_->begin(kind);
		}
	}

	~ScopedHostWait()
	{
		if (cell_ != nullptr)
			cell_->end(enclosing_);
	}

	ScopedHostWait(const ScopedHostWait &) = delete;
	ScopedHostWait &operator=(const ScopedHostWait &) = delete;

private:
	WaitCell *cell_;
	HostWait enclosing_{HostWait::none};
};

} // namespace uml::platform
