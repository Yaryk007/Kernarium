// SPDX-License-Identifier: GPL-2.0-or-later
/* What the host reads of a guest without asking it: the CPU time of the vCPU
 * carriers, found by their kernel-given names among the process's threads,
 * and the RAM the guest's commitment says the host holds. */
#include "cpu/gadget_cache_budget.hpp"
#include "guest_statistics.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_spinner.hpp"
#include "named_tests.hpp"
#include "platform/frame_figures.hpp"
#include "platform/host_state_figures.hpp"
#include "require.hpp"
#include "vcpu_carrier_name.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::GuestCpuTime;
using uml::runtime::GuestRamRetirementScope;
using uml::runtime::GuestRamUsage;
using uml::testing::NamedSpinner;
using uml::testing::require;
using uml::testing::waitUntil;

constexpr std::chrono::seconds bound {30};

void a_carrier_is_named_uml_vcpu_n_carrier()
{
	using uml::runtime::is_vcpu_carrier_name;
	using uml::runtime::vcpu_carrier_index;
	require(vcpu_carrier_index("uml-vcpu-0-carrier") == 0);
	require(vcpu_carrier_index("uml-vcpu-13-carrier") == 13);
	require(vcpu_carrier_index("uml-vcpu-99999999999999999999-carrier") == 1001);
	require(vcpu_carrier_index("uml-vcpu-x-carrier") == -1);
	require(vcpu_carrier_index("uml-irq-notify") == -1);
	require(is_vcpu_carrier_name("uml-vcpu-0-carrier"));
	require(is_vcpu_carrier_name("uml-vcpu-7-carrier"));
	require(is_vcpu_carrier_name("uml-vcpu-63-carrier"));
	require(!is_vcpu_carrier_name(""));
	require(!is_vcpu_carrier_name("uml-vcpu--carrier"));
	require(!is_vcpu_carrier_name("uml-vcpu-0"));
	require(!is_vcpu_carrier_name("uml-vcpu-0-carrier2"));
	require(!is_vcpu_carrier_name("uml-vcpu-x-carrier"));
	require(!is_vcpu_carrier_name("uml-vcpu-0x1-carrier"));
	require(!is_vcpu_carrier_name("uml-vcpu-0-carrier-extra"));
	require(!is_vcpu_carrier_name("xuml-vcpu-0-carrier"));
	require(!is_vcpu_carrier_name("uml-irq-notify"));
	require(!is_vcpu_carrier_name("uml-rtc-alarm"));
	require(!is_vcpu_carrier_name("uml-vcpu-carrier"));
}

/* The sum is the carriers' own time and no other thread's, however much the
 * others spend: a carrier counts, and a thread of the kernel or the host that
 * is not one does not, named uml-* or not. */
void only_carriers_count_and_their_time_is_their_own()
{
	constexpr std::chrono::milliseconds carrier_spend {60};
	constexpr std::chrono::milliseconds decoy_spend {400};
	NamedSpinner first {"uml-vcpu-0-carrier", carrier_spend};
	NamedSpinner second {"uml-vcpu-1-carrier", carrier_spend};
	NamedSpinner notify {"uml-irq-notify", decoy_spend};
	NamedSpinner rtc {"uml-rtc-alarm", decoy_spend};
	NamedSpinner unnamed {"", decoy_spend};
	NamedSpinner lookalike {"uml-vcpu-0-carrier-extra", decoy_spend};
	const std::uint64_t expected =
		first.awaitSpent() + second.awaitSpent();
	(void)notify.awaitSpent();
	(void)rtc.awaitSpent();
	(void)unnamed.awaitSpent();
	(void)lookalike.awaitSpent();

	const GuestCpuTime time = uml::runtime::guest_cpu_time();
	require(time.listed, "the process's threads must be listed");
	require(time.carriers == 2, "exactly the two carriers are found");
	/* The system adds the little a thread used after it last read its own
	 * clock, and keeps microseconds. */
	require(time.nanoseconds + 1000000ULL >= expected,
		"the sum must cover what the carriers used");
	require(time.nanoseconds <= expected + 10000000ULL,
		"the sum must not include any other thread's time");
	require(time.nanoseconds <
			expected + 2 * static_cast<std::uint64_t>(
				decoy_spend.count()) * 1000000ULL,
		"a decoy's time was counted");
}

void an_ended_carrier_takes_its_time_with_it()
{
	NamedSpinner first {"uml-vcpu-0-carrier", std::chrono::milliseconds {40}};
	auto second = std::make_unique<NamedSpinner>("uml-vcpu-1-carrier",
		std::chrono::milliseconds {40});
	const std::uint64_t first_used = first.awaitSpent();
	const std::uint64_t second_used = second->awaitSpent();
	const GuestCpuTime both = uml::runtime::guest_cpu_time();
	require(both.carriers == 2 &&
		both.nanoseconds + 1000000ULL >= first_used + second_used,
		"both carriers count");
	second.reset();
	require(waitUntil([] {
		return uml::runtime::guest_cpu_time().carriers == 1;
	}, bound), "an ended carrier must drop out");
	const GuestCpuTime alone = uml::runtime::guest_cpu_time();
	require(alone.nanoseconds + 1000000ULL >= first_used &&
		alone.nanoseconds < both.nanoseconds,
		"the sum starts again from the carriers that are left");
}

void no_carrier_is_no_time()
{
	const GuestCpuTime time = uml::runtime::guest_cpu_time();
	require(time.listed && time.carriers == 0 && time.nanoseconds == 0,
		"a process with no carrier has no guest CPU time");
}

class Mapping final {
public:
	explicit Mapping(std::size_t size) : size_ {size}
	{
		void *const mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED, "anonymous mapping failed");
		data_ = static_cast<std::byte *>(mapping);
	}
	~Mapping() { (void)::munmap(data_, size_); }
	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return data_; }

private:
	std::byte *data_ {};
	std::size_t size_ {};
};

void publish(Mapping &ram, std::size_t pages, std::size_t page)
{
	uml::memory::publish_guest_ram_commitment(
		std::make_unique<uml::memory::GuestRamCommitment>(
			ram.data(), pages * page, 0, pages * page, false, page));
	require(uml::memory::guest_ram_commitment() != nullptr,
		"the commitment must be published");
}

void retire()
{
	const GuestRamRetirementScope retirement;
	uml::memory::retire_guest_ram_commitment();
}

/* The held RAM is the guest's RAM less what it released, and nothing is
 * claimed before the guest's kernel has claimed it or after it was handed
 * back. */
void the_held_ram_follows_the_commitment()
{
	const std::size_t page = static_cast<std::size_t>(::getpagesize());
	constexpr std::size_t pages = 64;
	Mapping ram {pages * page};
	require(!uml::runtime::guest_ram_usage().has_value(),
		"nothing is held before the guest claims its RAM");

	publish(ram, pages, page);
	auto usage = uml::runtime::guest_ram_usage();
	require(usage.has_value() && usage->ram_bytes == pages * page &&
		usage->held_bytes == pages * page,
		"a fresh commitment holds all of the RAM");

	auto *const commitment = uml::memory::guest_ram_commitment();
	require(commitment->release(8 * page, 16 * page) == 0, "release failed");
	usage = uml::runtime::guest_ram_usage();
	require(usage.has_value() && usage->ram_bytes == pages * page &&
		usage->held_bytes == (pages - 16) * page,
		"released pages leave the held RAM");

	require(commitment->commit(8 * page, 4 * page) == 0, "commit failed");
	usage = uml::runtime::guest_ram_usage();
	require(usage.has_value() && usage->held_bytes == (pages - 12) * page,
		"committed pages are held again");

	retire();
	require(!uml::runtime::guest_ram_usage().has_value(),
		"nothing is held once the guest is handed back");
}

/* A reader that arrives while the commitment is retired and published again
 * (a restart's hand-back) sees nothing or a whole commitment, never one that
 * is being deleted: the retirement scope excludes it. Under a sanitizer a
 * reader that met the deleted object would be reported. */
void a_reader_never_meets_a_commitment_being_retired()
{
	const std::size_t page = static_cast<std::size_t>(::getpagesize());
	constexpr std::size_t pages = 32;
	Mapping ram {pages * page};
	std::atomic_bool stop {};
	std::atomic<unsigned> seen {};
	std::thread reader {[&] {
		while (!stop.load(std::memory_order_acquire)) {
			const auto usage = uml::runtime::guest_ram_usage();
			if (!usage)
				continue;
			require(usage->ram_bytes == pages * page &&
				usage->held_bytes <= usage->ram_bytes,
				"a reading is a whole commitment's");
			seen.fetch_add(1, std::memory_order_relaxed);
		}
	}};
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds {300};
	while (std::chrono::steady_clock::now() < deadline) {
		publish(ram, pages, page);
		std::this_thread::sleep_for(std::chrono::microseconds {200});
		retire();
	}
	stop.store(true, std::memory_order_release);
	reader.join();
	require(seen.load() != 0, "the reader never saw a commitment");
	require(!uml::memory::guest_ram_commitment(), "the last one is retired");
}

/* The held RAM is not read while a hand-back retires it, and the reader
 * does not wait for the hand-back to end. */
void a_hand_back_in_progress_is_not_waited_for()
{
	const std::size_t page = static_cast<std::size_t>(::getpagesize());
	constexpr std::size_t pages = 16;
	Mapping ram {pages * page};
	publish(ram, pages, page);
	std::mutex mutex;
	std::condition_variable changed;
	bool held {};
	bool read {};
	std::thread retiring {[&] {
		const GuestRamRetirementScope retirement;
		std::unique_lock lock {mutex};
		held = true;
		changed.notify_all();
		changed.wait(lock, [&] { return read; });
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, bound, [&] { return held; }),
			"the hand-back never started");
	}
	require(!uml::runtime::guest_ram_usage().has_value(),
		"the RAM was read while it was being handed back");
	{
		const std::lock_guard lock {mutex};
		read = true;
	}
	changed.notify_all();
	retiring.join();
	require(uml::runtime::guest_ram_usage().has_value(),
		"the RAM was not read once the scope ended");
	retire();
}


/* ---- The figures the session record's checkpoint reads. ---- */

constexpr std::uint64_t millisecond_ns = 1'000'000;

/* The guest's flushes are counted from the device's note of each, and the
 * longest gap between two is taken at the checkpoint: it starts again from
 * there, so a gap that falls on the boundary is in the interval it ends in. */
void frames_and_the_longest_gap_across_a_checkpoint()
{
	using uml::runtime::RunFigures;
	namespace platform = uml::platform;
	platform::forgetFrameGaps();
	const auto first = uml::runtime::run_figures(true);
	require((first.sources & RunFigures::frames) != 0 &&
		first.longest_gap_ns == 0, "a guest that never flushed has no gap");
	/* The flushes are timestamped ahead of the clock, so that the time
	 * since the last one is not a gap yet. */
	const auto at = [](std::uint64_t base, std::uint64_t milliseconds) {
		return platform::frameNsToTicks(base + milliseconds * millisecond_ns);
	};
	const std::uint64_t base = platform::frameTicksToNs(
		platform::frameClockTicks()) + 60'000 * millisecond_ns;
	for (const std::uint64_t milliseconds : {0ULL, 16ULL, 32ULL, 5032ULL})
		platform::noteScanoutFlushAt(at(base, milliseconds));
	const auto second = uml::runtime::run_figures(false);
	require(second.flushes == first.flushes + 4 &&
		second.longest_gap_ns >= 4999 * millisecond_ns &&
		second.longest_gap_ns <= 5001 * millisecond_ns,
		"the interval's frames or its longest gap are wrong");
	/* The next interval starts afresh: its longest gap is the longest of the
	 * gaps that end in it, the one from the last flush of the interval
	 * before included. */
	platform::noteScanoutFlushAt(at(base, 5048));
	platform::noteScanoutFlushAt(at(base, 5300));
	platform::noteScanoutFlushAt(at(base, 5316));
	const auto third = uml::runtime::run_figures(false);
	require(third.flushes == second.flushes + 3 &&
		third.longest_gap_ns >= 251 * millisecond_ns &&
		third.longest_gap_ns <= 253 * millisecond_ns,
		"a gap across the boundary was lost or counted twice");
	const auto quiet = uml::runtime::run_figures(false);
	require(quiet.flushes == third.flushes && quiet.longest_gap_ns == 0,
		"an interval with no flush has frames or a gap");

	/* A guest that stopped flushing shows how long it has been silent. */
	platform::forgetFrameGaps();
	platform::noteScanoutFlushAt(platform::frameClockTicks() -
		platform::frameNsToTicks(3000 * millisecond_ns));
	const auto silent = uml::runtime::run_figures(false);
	require(silent.longest_gap_ns >= 3000 * millisecond_ns &&
		silent.longest_gap_ns < 600'000 * millisecond_ns,
		"a guest that fell silent shows no gap");
	/* A guest that begins has no gap behind its first flush. */
	const auto begun = uml::runtime::run_figures(true);
	require(begun.longest_gap_ns == 0, "a new session inherited a gap");
	platform::noteScanoutFlush();
	require(uml::runtime::run_figures(false).longest_gap_ns < 600'000 *
		millisecond_ns, "a new session's first flush has a gap behind it");
	platform::forgetFrameGaps();
}

/* The host's state is the stall watch's own observation, read where it was
 * published; nothing is known until it is. */
void the_host_state_is_the_one_the_watch_published()
{
	using uml::runtime::RunFigures;
	namespace platform = uml::platform;
	platform::withdrawHostState();
	require((uml::runtime::run_figures(false).sources & RunFigures::host) == 0,
		"a host state was read before any was observed");
	platform::HostThermalState state;
	state.level = platform::HostThermalLevel::serious;
	state.lowPowerMode = true;
	state.activeProcessors = 4;
	platform::publishHostThermal(state);
	platform::publishMemoryPressure(2);
	auto figures = uml::runtime::run_figures(false);
	require((figures.sources & RunFigures::host) != 0 &&
		figures.thermal == 2 && figures.pressure == 2,
		"the published thermal state or pressure was misread");
	platform::withdrawHostState();
	figures = uml::runtime::run_figures(false);
	require((figures.sources & RunFigures::host) == 0 &&
		figures.thermal == 0 && figures.pressure == 0,
		"a withdrawn host state was read");
}

struct CacheOwner final {
	static void notify(void *) noexcept {}
};

/* The gadget cache's relief counters and its largest open account, after a
 * relief that the test forces. */
void the_cache_ledger_is_read_after_a_forced_relief()
{
	using uml::runtime::RunFigures;
	namespace cache = uml::cpu::gadget_cache;
	constexpr std::size_t kib = 1024;
	cache::set_budget(64 * kib);
	cache::Account small {&CacheOwner::notify, nullptr};
	cache::Account large {&CacheOwner::notify, nullptr};
	small.charge(10 * kib);
	large.charge(70 * kib);
	const auto before = uml::runtime::run_figures(false);
	require((before.sources & RunFigures::cache) != 0 &&
		(before.sources & RunFigures::largest) != 0 &&
		before.largest_open == 70 * kib, "the largest account is wrong");
	cache::relieve();
	const auto after = uml::runtime::run_figures(false);
	require(after.reliefs == before.reliefs + 1 &&
		after.shed_accounts == before.shed_accounts + 1 &&
		after.renewals == before.renewals &&
		after.largest_open == 10 * kib,
		"the relief or the largest open account was not read");
	cache::set_budget(cache::fallback_budget);
}

/* The process's CPU time and usage come from the system, and the carriers'
 * from the threads' names; none runs backwards. The CPU time is the task's
 * power information in mach ticks converted to nanoseconds, and agrees with
 * what getrusage says of the same process; the cycles have a source on macOS
 * only. */
void the_cpu_figures_come_from_the_system()
{
	using uml::runtime::RunFigures;
	const auto rusage_ns = [] {
		struct rusage usage {};
		require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
		const auto ns = [](const timeval &time) {
			return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ULL +
				static_cast<std::uint64_t>(time.tv_usec) * 1000ULL;
		};
		return ns(usage.ru_utime) + ns(usage.ru_stime);
	};
	NamedSpinner carrier {"uml-vcpu-0-carrier", std::chrono::milliseconds {60}};
	const std::uint64_t spent = carrier.awaitSpent();
	const std::uint64_t before = rusage_ns();
	const auto first = uml::runtime::run_figures(false);
	const std::uint64_t after = rusage_ns();
	require((first.sources & RunFigures::cpu) != 0 &&
		(first.sources & RunFigures::usage) != 0 &&
		(first.sources & RunFigures::carriers) != 0,
		"the system gave no usage for the process");
	require(first.cpu_ns > 0 && first.performance_ns <= first.cpu_ns &&
		first.carrier_ns + millisecond_ns >= spent,
		"the CPU figures are implausible");
	/* The unit is nanoseconds: the same process's time between two readings of
	 * it by another interface (getrusage), give or take their granularity. */
	require(first.cpu_ns + 5 * millisecond_ns >= before &&
		first.cpu_ns <= after + 5 * millisecond_ns,
		"the process's CPU time is not in nanoseconds of the process's own");
#if __has_include(<libproc.h>)
	require((first.sources & RunFigures::cycle_count) != 0 && first.cycles > 0,
		"macOS gave no cycles for the process");
#else
	require((first.sources & RunFigures::cycle_count) == 0 && first.cycles == 0,
		"a platform without libproc.h reported cycles");
#endif
	/* This thread's own work moves the process's time forward. */
	volatile std::uint64_t sink = 0;
	const auto until = std::chrono::steady_clock::now() +
		std::chrono::milliseconds {40};
	while (std::chrono::steady_clock::now() < until)
		sink = sink + 1;
	const auto second = uml::runtime::run_figures(false);
	require(second.cpu_ns > first.cpu_ns && second.cycles >= first.cycles &&
		second.energy_nj >= first.energy_nj &&
		second.performance_ns >= first.performance_ns &&
		second.carrier_ns >= first.carrier_ns,
		"a counter ran backwards");
}

} // namespace

int main()
{
	constexpr uml::testing::NamedTest tests[] = {
		{"a-carrier-is-named-uml-vcpu-n-carrier",
			a_carrier_is_named_uml_vcpu_n_carrier},
		{"no-carrier-is-no-time", no_carrier_is_no_time},
		{"only-carriers-count-and-their-time-is-their-own",
			only_carriers_count_and_their_time_is_their_own},
		{"an-ended-carrier-takes-its-time-with-it",
			an_ended_carrier_takes_its_time_with_it},
		{"the-held-ram-follows-the-commitment",
			the_held_ram_follows_the_commitment},
		{"a-reader-never-meets-a-commitment-being-retired",
			a_reader_never_meets_a_commitment_being_retired},
		{"a-hand-back-in-progress-is-not-waited-for",
			a_hand_back_in_progress_is_not_waited_for},
		{"frames-and-the-longest-gap-across-a-checkpoint",
			frames_and_the_longest_gap_across_a_checkpoint},
		{"the-host-state-is-the-one-the-watch-published",
			the_host_state_is_the_one_the_watch_published},
		{"the-cache-ledger-is-read-after-a-forced-relief",
			the_cache_ledger_is_read_after_a_forced_relief},
		{"the-cpu-figures-come-from-the-system",
			the_cpu_figures_come_from_the_system},
	};
	return uml::testing::runNamedTests(tests);
}
