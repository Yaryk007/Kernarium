// SPDX-License-Identifier: GPL-2.0-or-later
#include "session_record.hpp"

#include "console_boot_signal.hpp"
#include "fixed_text.hpp"
#include "platform/host_wait.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

/* The kernel output a record keeps: enough for a panic's message, its stack
 * and the lines before it. */
constexpr std::size_t output_size = session_kernel_output_bytes;
constexpr std::size_t console_slots = 4;
/* Darwin's limit on a thread name, its terminator included. */
constexpr std::size_t thread_name_size = 64;
constexpr std::string_view panic_prefix{"Darwin UML panic: "};
/* What the kernel's handler (os-Darwin/signal.c) writes, one write each, when
 * a host thread that owns no vCPU faults: the registers, then this. */
constexpr std::string_view host_fault_prefix{"Darwin host fault: "};
constexpr std::string_view host_fault_registers{"Darwin host fault: signal="};
constexpr std::string_view host_fault_line{
	"Darwin host fault: taken on a host thread which owns no virtual CPU"};

std::atomic<char> output[output_size];
std::atomic<std::uint64_t> output_end{0};
/* Where `output_end` stood when the session began: the output of a boot starts
 * there, so a record quotes nothing a boot before it printed. */
std::atomic<std::uint64_t> output_start{0};
/* Set when the ring dropped output from its head: a write longer than the ring
 * keeps only its last bytes, while output_end counts only what was kept. */
std::atomic<bool> output_clipped{false};
/* What the same output says of the boot (console_boot_signal.hpp). */
ConsoleBootSignal boot_signal;
/* What the host that runs the guest says of its boots (set_session_boot_facts):
 * whether this guest's console reports a finished boot, and how many times the
 * guest restarted before the session that begins. */
std::atomic<bool> boot_reported{false};
std::atomic<std::uint32_t> boot_restarts{0};
/* The ramdisk the boot that begins uses (set_session_boot_ramdisk). */
std::atomic<BootRamdisk> boot_ramdisk{BootRamdisk::unreported};
std::atomic<int> console_descriptors[console_slots] = {-1, -1, -1, -1};

std::atomic<int> record_descriptor{-1};
/* True while no end has been recorded for the session that began last. */
std::atomic<bool> session_open{false};
std::atomic<std::int64_t> session_started{0};
std::atomic<bool> exit_hook_installed{false};
std::atomic<bool> crash_recorded{false};

/* The record and the output it quotes: static, so an end formats nothing on
 * a stack it does not know the size of. One end writes them, the first. */
char record_text[session_state_max_bytes];
char tail_text[output_size];

/* The state while the boot of a guest that reports it is unfinished: the
 * checkpoint thread rewrites it, with the output so far, whenever the output
 * grew, so a process that is killed mid-boot leaves where the boot had got.
 * Its own buffers and one writer at a time (`state_checkpointing`): the
 * checkpoint, or the session's begin, which waits for the flag. An end never
 * takes the flag; it waits a bounded time for a checkpoint under way to
 * finish (wait_out_state_checkpoint). The wait does not make the end the last
 * write, since a checkpoint may be delayed past it between its last look at
 * `session_open` and its write. What does is `terminal_size`: the end formats
 * its record (`record_text`) and publishes its size before it writes, and a
 * checkpoint that has written looks, and writes that record again if it is
 * there. A write of the state that lands after an end's is thus followed by
 * the end's, whatever the delay. */
char checkpoint_text[session_state_max_bytes];
char checkpoint_tail[output_size];
/* What a state's text is matched on before it is stored (keep_masked): one per
 * writer of a state, because the end may format its record while a checkpoint
 * formats its own. */
char record_scratch[session_state_max_bytes];
char checkpoint_scratch[session_state_max_bytes];
std::atomic_flag state_checkpointing = ATOMIC_FLAG_INIT;
/* The size of the end's record in `record_text`, 0 while the session runs. */
std::atomic<std::size_t> terminal_size{0};
std::atomic<std::uint64_t> checkpointed_output_end{0};
std::atomic<bool> checkpointed_completed{false};
/* The most an end waits for a checkpoint of the state under way, in
 * milliseconds. */
constexpr unsigned state_checkpoint_wait_ms = 50;

/* The record's first bytes are a fixed slot that only the checkpoints write,
 * the next are the stall slot that only the stall watch writes, the network
 * slot after it is written by the checkpoints and the session's begin and
 * end, the storage slot after that is written by the settle calls
 * (note_guest_settle) and the session's begin, the camera slot after that by
 * the camera's failure reports (note_camera_still_failure) and the session's
 * begin, and the rest is the session's
 * state that only its begin and its end write, so none ever overwrites
 * another's bytes, whichever finishes last. */
constexpr std::size_t memory_slot_size = session_memory_slot_bytes;
constexpr std::size_t stall_slot_offset = session_memory_slot_bytes;
std::atomic<bool> checkpoint_thread_started{false};

std::atomic<MemoryTermsReader> terms_reader{nullptr};
std::atomic<RunFiguresReader> figures_reader{nullptr};

/* The network slot: the counts since the session began, and the notice. The
 * baseline is the reading at the begin, written before the session opens and
 * read after. */
std::atomic<SessionNetworkReader> network_reader{nullptr};
SessionNetworkCounts network_baseline{};
std::atomic<NetworkNotice> notice_now{NetworkNotice::none};
std::atomic<std::uint32_t> notice_times{0};
/* The slot is formatted in static storage, so one writer at a time: the
 * checkpoint thread, or the session's end, which may run in a signal handler
 * and so never waits. A writer that finds it taken leaves the slot to the
 * other, whose reading is as new. */
std::atomic_flag network_slot_busy = ATOMIC_FLAG_INIT;

/* What the run did in the time a row covers, from two readings of the run
 * figures: counts and times are the differences, so a row of the older history
 * adds the rows it stands for (`add`). `known` holds the sources that gave a
 * difference (RunFigures bits); `valid` is false where there was no reading
 * before. */
struct Interval final {
	bool valid{};
	std::uint32_t known{};
	std::uint64_t span_ms{};
	std::uint64_t frames{};
	/* The longest gap between two flushes; the longest of the rows added. */
	std::uint64_t gap_ns{};
	std::uint64_t reliefs{};
	std::uint64_t shed_accounts{};
	std::uint64_t renewals{};
	std::uint64_t cpu_ns{};
	std::uint64_t carrier_ns{};
	std::uint64_t performance_ns{};
	std::uint64_t cycles{};
	std::uint64_t energy_nj{};
};

/* One checkpoint's figures in MiB, as the history keeps them. */
struct HistoryEntry final {
	std::int64_t seconds;
	std::uint64_t footprint;
	std::uint64_t available;
	std::uint64_t guest;
	std::uint64_t gpu;
	std::uint64_t graphics;
	std::uint64_t cache;
	std::uint64_t heaps;
	std::uint64_t malloc;
	std::uint64_t other;
	/* The states at the checkpoint: RunFigures sources, the host's thermal
	 * state and memory pressure, and MiB. */
	std::uint32_t sources;
	std::uint8_t thermal;
	std::uint8_t pressure;
	std::uint64_t compressed;
	std::uint64_t internal;
	std::uint64_t largest;
	/* The time since the checkpoint before, and, for a checkpoint the older
	 * history may keep, the time since the one kept before it. */
	Interval interval;
	Interval window;
};

/* Under history_lock: the checkpoints of the session that began last. The
 * thread is the only writer but for tests, which call the checkpoint
 * directly. */
os_unfair_lock history_lock = OS_UNFAIR_LOCK_INIT;
HistoryEntry recent[memory_history_recent];
HistoryEntry older[memory_history_older];
std::uint64_t checkpoints_taken{};
std::size_t older_kept{};
/* The reading the next row is measured from (previous_valid), when it was
 * taken on the steady clock, and what the checkpoints since the last one the
 * older history may keep have added up to. */
RunFigures previous_figures{};
std::int64_t previous_figures_ms{};
bool previous_valid{};
Interval stride_total{};
/* The highest footprint a checkpoint read, and the terms beside it. */
std::uint64_t peak_footprint{};
std::int64_t peak_at{};
MemoryTerms peak_terms{};
std::int64_t peak_malloc_age_ms{-1};
/* The last reading of malloc's term (memory_malloc_reading_due): when, on
 * the steady clock (negative for none), and the highest footprint any
 * reading was taken at. */
std::int64_t malloc_read_ms{-1};
std::uint64_t malloc_read_peak{};
std::uint64_t malloc_allocated{};
std::uint64_t malloc_in_use{};

bool kept(int descriptor) noexcept
{
	if (descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO)
		return true;
	for (const auto &console : console_descriptors)
		if (console.load(std::memory_order_relaxed) == descriptor)
			return true;
	return false;
}

void keep_output(const void *data, std::size_t length) noexcept
{
	const auto *bytes = static_cast<const char *>(data);
	if (length > output_size) {
		bytes += length - output_size;
		length = output_size;
		output_clipped.store(true, std::memory_order_release);
	}
	const std::uint64_t start =
		output_end.fetch_add(length, std::memory_order_acq_rel);
	for (std::size_t index = 0; index < length; ++index)
		output[(start + index) % output_size].store(bytes[index],
			std::memory_order_relaxed);
}

using Text = FixedText;

std::int64_t now() noexcept
{
	timespec time{};
	(void)clock_gettime(CLOCK_REALTIME, &time);
	return static_cast<std::int64_t>(time.tv_sec);
}

constexpr std::uint64_t mebibytes(std::uint64_t bytes) noexcept
{
	return bytes >> 20;
}

/* A row's figure: its number, at most six digits, or "-" where its source had
 * nothing to say. */
Text &figure(Text &text, bool known, std::uint64_t value) noexcept
{
	constexpr std::uint64_t most = 999999;
	text.text(" ");
	if (!known)
		return text.text("-");
	return text.number(static_cast<std::int64_t>(std::min(value, most)));
}

constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000;

/* "h SECONDS ... OTHER SPAN FRAMES GAP THERMAL PRESSURE COMPRESSED INTERNAL
 * RELIEFS SHED RENEWALS LARGEST CPU VCPU PERF MHZ ENERGY": the figures in MiB,
 * milliseconds and counts; the CPU times in milliseconds; PERF the percent of
 * the CPU time spent on performance cores; MHZ the cycles per CPU second;
 * ENERGY in millijoules. The row of the older history sums its window. */
Text &history_line(Text &text, const HistoryEntry &entry, bool older) noexcept
{
	text.text("h ").number(entry.seconds)
		.text(" ").number(static_cast<std::int64_t>(entry.footprint))
		.text(" ").number(static_cast<std::int64_t>(entry.available))
		.text(" ").number(static_cast<std::int64_t>(entry.guest))
		.text(" ").number(static_cast<std::int64_t>(entry.gpu))
		.text(" ").number(static_cast<std::int64_t>(entry.graphics))
		.text(" ").number(static_cast<std::int64_t>(entry.cache))
		.text(" ").number(static_cast<std::int64_t>(entry.heaps))
		.text(" ").number(static_cast<std::int64_t>(entry.malloc))
		.text(" ").number(static_cast<std::int64_t>(entry.other));
	const Interval &run = older ? entry.window : entry.interval;
	const auto has = [&](std::uint32_t source) {
		return run.valid && (run.known & source) != 0;
	};
	const bool host = (entry.sources & RunFigures::host) != 0;
	figure(text, run.valid, run.span_ms);
	figure(text, has(RunFigures::frames), run.frames);
	figure(text, has(RunFigures::frames),
		run.gap_ns / nanoseconds_per_millisecond);
	figure(text, host, entry.thermal);
	figure(text, host, entry.pressure);
	figure(text, true, entry.compressed);
	figure(text, true, entry.internal);
	figure(text, has(RunFigures::cache), run.reliefs);
	figure(text, has(RunFigures::cache), run.shed_accounts);
	figure(text, has(RunFigures::cache), run.renewals);
	figure(text, (entry.sources & RunFigures::largest) != 0, entry.largest);
	figure(text, has(RunFigures::cpu),
		run.cpu_ns / nanoseconds_per_millisecond);
	figure(text, has(RunFigures::carriers),
		run.carrier_ns / nanoseconds_per_millisecond);
	const bool used = has(RunFigures::usage) && has(RunFigures::cpu) &&
		run.cpu_ns != 0;
	figure(text, used, run.performance_ns * 100 / std::max<std::uint64_t>(
		run.cpu_ns, 1));
	figure(text, has(RunFigures::cycle_count) && has(RunFigures::cpu) &&
		run.cpu_ns != 0 && run.cycles != 0, run.cycles * 1000 /
		std::max<std::uint64_t>(run.cpu_ns, 1));
	figure(text, has(RunFigures::usage), run.energy_nj / 1'000'000);
	return text.text("\n");
}

Text &mib(Text &text, std::string_view name, std::uint64_t bytes) noexcept
{
	return text.text(" ").text(name).text(" ")
		.number(static_cast<std::int64_t>(mebibytes(bytes)));
}

std::int64_t steady_ms() noexcept
{
	return static_cast<std::int64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

/* The footprint less the terms read at every checkpoint: malloc and what no
 * term names. */
std::uint64_t other_bytes(std::uint64_t footprint,
	const MemoryTerms &terms) noexcept
{
	const std::uint64_t named = terms.guest_held + terms.graphics +
		terms.media + terms.cache + terms.heaps;
	return footprint > named ? footprint - named : 0;
}

/* "guest G gpu H/B [age S] ... malloc A/U [age S] other R ... compressed K",
 * in MiB and seconds. */
Text &terms_words(Text &text, const MemoryTerms &terms,
	std::int64_t malloc_age_ms, std::uint64_t footprint) noexcept
{
	mib(text, "guest", terms.guest_held);
	if (terms.gpu_budget != 0) {
		mib(text, "gpu", terms.gpu_held).text("/").number(
			static_cast<std::int64_t>(mebibytes(terms.gpu_budget)));
		if (terms.gpu_age_ms >= 0)
			text.text(" age ").number(terms.gpu_age_ms / 1000);
	}
	mib(text, "graphics", terms.graphics);
	mib(text, "media", terms.media);
	mib(text, "cache", terms.cache);
	mib(text, "heaps", terms.heaps);
	mib(text, "malloc", terms.malloc_allocated).text("/").number(
		static_cast<std::int64_t>(mebibytes(terms.malloc_in_use)));
	if (malloc_age_ms >= 0)
		text.text(" age ").number(malloc_age_ms / 1000);
	mib(text, "other", other_bytes(footprint, terms));
	mib(text, "internal", terms.internal);
	return mib(text, "compressed", terms.compressed);
}

void forget_history() noexcept
{
	os_unfair_lock_lock(&history_lock);
	checkpoints_taken = 0;
	older_kept = 0;
	peak_footprint = 0;
	peak_at = 0;
	peak_terms = {};
	peak_malloc_age_ms = -1;
	malloc_read_ms = -1;
	malloc_read_peak = 0;
	malloc_allocated = 0;
	malloc_in_use = 0;
	previous_valid = false;
	stride_total = {};
	os_unfair_lock_unlock(&history_lock);
}

constexpr std::uint64_t delta(std::uint64_t now, std::uint64_t before) noexcept
{
	return now > before ? now - before : 0;
}

/* What happened between two readings of the run figures, `span_ms` apart. A
 * counter that fell belongs to something that ended in between, such as a
 * carrier, and the interval counts nothing for it. */
Interval interval_between(const RunFigures &now, const RunFigures &before,
	std::uint64_t span_ms) noexcept
{
	Interval interval;
	interval.valid = true;
	interval.span_ms = span_ms;
	const auto both = [&](std::uint32_t source) {
		return (now.sources & before.sources & source) != 0;
	};
	if (both(RunFigures::frames)) {
		interval.known |= RunFigures::frames;
		interval.frames = delta(now.flushes, before.flushes);
		interval.gap_ns = now.longest_gap_ns;
	}
	if (both(RunFigures::cache)) {
		interval.known |= RunFigures::cache;
		interval.reliefs = delta(now.reliefs, before.reliefs);
		interval.shed_accounts =
			delta(now.shed_accounts, before.shed_accounts);
		interval.renewals = delta(now.renewals, before.renewals);
	}
	if (both(RunFigures::cpu)) {
		interval.known |= RunFigures::cpu;
		interval.cpu_ns = delta(now.cpu_ns, before.cpu_ns);
	}
	if (both(RunFigures::carriers)) {
		interval.known |= RunFigures::carriers;
		interval.carrier_ns = delta(now.carrier_ns, before.carrier_ns);
	}
	if (both(RunFigures::usage)) {
		interval.known |= RunFigures::usage;
		interval.performance_ns =
			delta(now.performance_ns, before.performance_ns);
		interval.energy_nj = delta(now.energy_nj, before.energy_nj);
	}
	if (both(RunFigures::cycle_count)) {
		interval.known |= RunFigures::cycle_count;
		interval.cycles = delta(now.cycles, before.cycles);
	}
	return interval;
}

void add(Interval &total, const Interval &part) noexcept
{
	if (!part.valid)
		return;
	total.valid = true;
	total.known |= part.known;
	total.span_ms += part.span_ms;
	total.frames += part.frames;
	total.gap_ns = std::max(total.gap_ns, part.gap_ns);
	total.reliefs += part.reliefs;
	total.shed_accounts += part.shed_accounts;
	total.renewals += part.renewals;
	total.cpu_ns += part.cpu_ns;
	total.carrier_ns += part.carrier_ns;
	total.performance_ns += part.performance_ns;
	total.cycles += part.cycles;
	total.energy_nj += part.energy_nj;
}

/* A row of the history and which kind it is. */
struct Row final {
	const HistoryEntry *entry;
	bool older;
};

/* The rows of the history, oldest first. */
constexpr std::size_t history_rows = memory_history_older + memory_history_recent;

std::size_t collect_rows(Row (&rows)[history_rows]) noexcept
{
	std::size_t count = 0;
	const std::size_t olders = std::min<std::size_t>(older_kept,
		memory_history_older);
	for (std::size_t n = 0; n < olders; ++n)
		rows[count++] = {&older[(older_kept - olders + n) %
			memory_history_older], true};
	const std::uint64_t recents = std::min<std::uint64_t>(
		checkpoints_taken, memory_history_recent);
	for (std::uint64_t n = 0; n < recents; ++n)
		rows[count++] = {&recent[(checkpoints_taken - recents + n) %
			memory_history_recent], false};
	return count;
}

/* The first of the `count` rows that fits, with all the rows after it, in
 * `room` bytes: a history that is too long loses its oldest rows, never its
 * newest. */
std::size_t first_row_that_fits(const Row (&rows)[history_rows],
	std::size_t count, std::size_t room) noexcept
{
	std::size_t first = count;
	std::size_t used = 0;
	while (first > 0) {
		char line[512];
		Text text{line, sizeof(line)};
		history_line(text, *rows[first - 1].entry, rows[first - 1].older);
		if (text.size() > room - used)
			break;
		used += text.size();
		--first;
	}
	return first;
}

/* The memory slot: the format line, then the process's footprint, its peak
 * and what its limit leaves, in bytes; with a terms reader, the terms now and
 * at the highest checkpoint and the history, in MiB and the figures of the run
 * (session_record.hpp); padded to the slot's size. `beginning` is the
 * session's first, which the history's intervals are measured from. */
void write_memory_slot(int descriptor, bool beginning) noexcept
{
	static char slot[memory_slot_size];
	os_unfair_lock_lock(&history_lock);
	Text text{slot, sizeof(slot) - 1};
	const std::int64_t at = now();
	text.text("uml-session 8\nmemory at ").number(at);
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	std::uint64_t footprint = 0;
	std::uint64_t available = 0;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		footprint = info.phys_footprint;
		text.text(" footprint ")
			.number(static_cast<std::int64_t>(info.phys_footprint))
			.text(" peak ")
			.number(static_cast<std::int64_t>(
				info.ledger_phys_footprint_peak));
		/* Zero where the host sets no limit, as on a Mac. */
		if (count >= TASK_VM_INFO_COUNT && info.limit_bytes_remaining != 0) {
			available = info.limit_bytes_remaining;
			text.text(" available ").number(static_cast<std::int64_t>(
				info.limit_bytes_remaining));
		}
	}
	text.text("\n");
	if (const auto reader = terms_reader.load(std::memory_order_acquire)) {
		const std::int64_t clock = steady_ms();
		const bool read_malloc = memory_malloc_reading_due(
			malloc_read_ms >= 0, footprint, malloc_read_peak);
		MemoryTerms terms = reader(read_malloc);
		if (read_malloc) {
			malloc_read_ms = clock;
			malloc_read_peak = std::max(malloc_read_peak, footprint);
			malloc_allocated = terms.malloc_allocated;
			malloc_in_use = terms.malloc_in_use;
		} else {
			terms.malloc_allocated = malloc_allocated;
			terms.malloc_in_use = malloc_in_use;
		}
		const std::int64_t malloc_age = clock - malloc_read_ms;
		RunFigures figures{};
		if (const auto read_figures =
			    figures_reader.load(std::memory_order_acquire))
			figures = read_figures(beginning);
		Interval interval;
		if (previous_valid)
			interval = interval_between(figures, previous_figures,
				static_cast<std::uint64_t>(std::max<std::int64_t>(
					clock - previous_figures_ms, 0)));
		previous_figures = figures;
		previous_figures_ms = clock;
		previous_valid = true;
		HistoryEntry entry{
			.seconds = at - session_started.load(std::memory_order_relaxed),
			.footprint = mebibytes(footprint),
			.available = mebibytes(available),
			.guest = mebibytes(terms.guest_held),
			.gpu = mebibytes(terms.gpu_held),
			.graphics = mebibytes(terms.graphics),
			.cache = mebibytes(terms.cache),
			.heaps = mebibytes(terms.heaps),
			.malloc = mebibytes(terms.malloc_allocated),
			.other = mebibytes(other_bytes(footprint, terms)),
			.sources = figures.sources,
			.thermal = figures.thermal,
			.pressure = figures.pressure,
			.compressed = mebibytes(terms.compressed),
			.internal = mebibytes(terms.internal),
			.largest = mebibytes(figures.largest_open),
			.interval = interval,
			.window = {},
		};
		/* A recent entry about to be overwritten moves to the older
		 * history when its turn in the stride comes. The entries that may
		 * move carry the sum of the intervals since the one before. */
		const std::uint64_t index = checkpoints_taken++;
		add(stride_total, interval);
		if (index % memory_history_older_stride == 0) {
			entry.window = stride_total;
			stride_total = {};
		}
		if (index >= memory_history_recent) {
			const std::uint64_t leaving = index - memory_history_recent;
			if (leaving % memory_history_older_stride == 0)
				older[older_kept++ % memory_history_older] =
					recent[leaving % memory_history_recent];
		}
		recent[index % memory_history_recent] = entry;
		if (footprint >= peak_footprint) {
			peak_footprint = footprint;
			peak_at = at;
			peak_terms = terms;
			peak_malloc_age_ms = malloc_age;
		}
		terms_words(text.text("terms"), terms, malloc_age, footprint)
			.text("\n");
		text.text("peak-terms at ").number(peak_at).text(" footprint ")
			.number(static_cast<std::int64_t>(mebibytes(peak_footprint)));
		terms_words(text, peak_terms, peak_malloc_age_ms, peak_footprint)
			.text("\n");
		text.text("history seconds footprint available guest gpu graphics "
			"cache heaps malloc other span frames gap thermal pressure "
			"compressed internal reliefs shed renewals largest cpu vcpu "
			"perf mhz energy\n");
		Row rows[history_rows];
		const std::size_t count = collect_rows(rows);
		for (std::size_t n = first_row_that_fits(rows, count,
			     sizeof(slot) - 1 - text.size());
		     n < count; ++n)
			history_line(text, *rows[n].entry, rows[n].older);
	}
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', sizeof(slot) - 1 - used);
	slot[sizeof(slot) - 1] = '\n';
	ssize_t written;
	do {
		written = pwrite(descriptor, slot, sizeof(slot), 0);
	} while (written < 0 && errno == EINTR);
	os_unfair_lock_unlock(&history_lock);
}

/* The stall slot of a session that has seen no stall, so that nothing of the
 * file's earlier session stays in it. */
void write_blank_stall_slot(int descriptor) noexcept
{
	static char slot[session_stall_slot_bytes];
	Text text{slot, sizeof(slot) - 1};
	const std::int64_t at = now();
	text.text("stalls 1 base ").number(at).text(" at ").number(at)
		.text(" total 0 shown 0\n");
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', sizeof(slot) - 1 - used);
	slot[sizeof(slot) - 1] = '\n';
	ssize_t written;
	do {
		written = pwrite(descriptor, slot, sizeof(slot), stall_slot_offset);
	} while (written < 0 && errno == EINTR);
}

/* The counts of `now` since `baseline`; a count that fell below its baseline
 * belongs to a provider made since, and is counted whole. */
SessionNetworkCounts network_since(const SessionNetworkCounts &now,
	const SessionNetworkCounts &baseline) noexcept
{
	const bool replaced = now.dns_answered < baseline.dns_answered ||
		now.dns_policy_denied < baseline.dns_policy_denied ||
		now.dns_timed_out < baseline.dns_timed_out ||
		now.dns_failed < baseline.dns_failed ||
		now.dhcp_discovers < baseline.dhcp_discovers ||
		now.dhcp_requests < baseline.dhcp_requests ||
		now.dhcp_acks < baseline.dhcp_acks ||
		now.tcp_attempts < baseline.tcp_attempts ||
		now.tcp_established < baseline.tcp_established ||
		now.tcp_unreachable < baseline.tcp_unreachable ||
		now.tcp_reset < baseline.tcp_reset;
	if (replaced)
		return now;
	return {
		.dns_answered = now.dns_answered - baseline.dns_answered,
		.dns_policy_denied =
			now.dns_policy_denied - baseline.dns_policy_denied,
		.dns_timed_out = now.dns_timed_out - baseline.dns_timed_out,
		.dns_failed = now.dns_failed - baseline.dns_failed,
		.dhcp_discovers = now.dhcp_discovers - baseline.dhcp_discovers,
		.dhcp_requests = now.dhcp_requests - baseline.dhcp_requests,
		.dhcp_acks = now.dhcp_acks - baseline.dhcp_acks,
		.tcp_attempts = now.tcp_attempts - baseline.tcp_attempts,
		.tcp_established = now.tcp_established - baseline.tcp_established,
		.tcp_unreachable = now.tcp_unreachable - baseline.tcp_unreachable,
		.tcp_reset = now.tcp_reset - baseline.tcp_reset,
	};
}

std::string_view notice_word(NetworkNotice notice) noexcept
{
	switch (notice) {
	case NetworkNotice::none:
		return "none";
	case NetworkNotice::cellular_denied:
		return "cellular";
	case NetworkNotice::wifi_denied:
		return "wifi";
	}
	return "none";
}

/* The network slot: a format line, the counts when the guest has a network
 * (one line each for DNS, DHCP and TCP), and the notice; padded to the slot's
 * size. */
void write_network_slot(int descriptor) noexcept
{
	static char slot[session_network_slot_bytes];
	if (network_slot_busy.test_and_set(std::memory_order_acquire))
		return;
	Text text{slot, sizeof(slot) - 1};
	text.text("network 1\n");
	SessionNetworkCounts counts{};
	const auto reader = network_reader.load(std::memory_order_acquire);
	if (reader != nullptr && reader(counts)) {
		counts = network_since(counts, network_baseline);
		const auto n = [](std::uint64_t value) {
			return static_cast<std::int64_t>(std::min<std::uint64_t>(
				value, INT64_MAX));
		};
		text.text("net-dns answered ").number(n(counts.dns_answered))
			.text(" policy-denied ").number(n(counts.dns_policy_denied))
			.text(" timed-out ").number(n(counts.dns_timed_out))
			.text(" failed ").number(n(counts.dns_failed)).text("\n");
		text.text("net-dhcp discovers ").number(n(counts.dhcp_discovers))
			.text(" requests ").number(n(counts.dhcp_requests))
			.text(" acks ").number(n(counts.dhcp_acks)).text("\n");
		text.text("net-tcp attempts ").number(n(counts.tcp_attempts))
			.text(" established ").number(n(counts.tcp_established))
			.text(" unreachable ").number(n(counts.tcp_unreachable))
			.text(" reset ").number(n(counts.tcp_reset)).text("\n");
	}
	text.text("net-notice denials ")
		.number(notice_times.load(std::memory_order_relaxed))
		.text(" now ")
		.text(notice_word(notice_now.load(std::memory_order_relaxed)))
		.text("\n");
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', sizeof(slot) - 1 - used);
	slot[sizeof(slot) - 1] = '\n';
	ssize_t written;
	do {
		written = pwrite(descriptor, slot, sizeof(slot),
			session_network_offset);
	} while (written < 0 && errno == EINTR);
	network_slot_busy.clear(std::memory_order_release);
}

/* The storage slot: how many times the guest's storage was settled and how
 * many of those it did not confirm, and the last one. One writer at a time,
 * and the session's begin, which blanks it, waits for the flag. */
std::atomic_flag storage_slot_busy = ATOMIC_FLAG_INIT;
std::uint64_t storage_settles{};
std::uint64_t storage_unsettled{};
bool storage_has_last{};
SettleKind storage_last_kind{};
GuestStorage storage_last_storage{};
SettleSuspension storage_last_suspension{};
std::int64_t storage_last_at{};

std::string_view settle_kind_word(SettleKind kind) noexcept
{
	switch (kind) {
	case SettleKind::sync:
		return "sync";
	case SettleKind::quiesce:
		return "quiesce";
	case SettleKind::quiesce_resident:
		return "quiesce-resident";
	}
	return "sync";
}

std::string_view guest_storage_word(GuestStorage storage) noexcept
{
	switch (storage) {
	case GuestStorage::not_reported:
		break;
	case GuestStorage::confirmed:
		return "confirmed";
	case GuestStorage::no_guest:
		return "no-guest";
	case GuestStorage::unconfirmed:
		return "unconfirmed";
	case GuestStorage::refused:
		return "refused";
	case GuestStorage::unreachable:
		return "unreachable";
	}
	return "unconfirmed";
}

std::string_view settle_suspension_word(SettleSuspension suspension) noexcept
{
	switch (suspension) {
	case SettleSuspension::not_asked:
		return "not-asked";
	case SettleSuspension::suspended:
		return "suspended";
	case SettleSuspension::not_suspended:
		return "not-suspended";
	}
	return "not-asked";
}

/* The storage slot, padded to its size: blank before the first settle, else a
 * format line, the counts and the last settle. Held under
 * `storage_slot_busy`. */
void write_storage_slot(int descriptor) noexcept
{
	static char slot[session_storage_slot_bytes];
	Text text{slot, sizeof(slot) - 1};
	if (storage_settles != 0U) {
		const auto n = [](std::uint64_t value) {
			return static_cast<std::int64_t>(std::min<std::uint64_t>(
				value, INT64_MAX));
		};
		text.text("storage 1\nsettles ").number(n(storage_settles))
			.text(" unsettled ").number(n(storage_unsettled)).text("\n");
		if (storage_has_last)
			text.text("last ").text(settle_kind_word(storage_last_kind))
				.text(" ").text(guest_storage_word(storage_last_storage))
				.text(" ")
				.text(settle_suspension_word(storage_last_suspension))
				.text(" at ").number(storage_last_at).text("\n");
	}
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', sizeof(slot) - 1 - used);
	slot[sizeof(slot) - 1] = '\n';
	ssize_t written;
	do {
		written = pwrite(descriptor, slot, sizeof(slot),
			session_storage_offset);
	} while (written < 0 && errno == EINTR);
}

/* The camera slot: how the platform presented each of the guest's cameras
 * (note_camera_presentation), how many stills of the guest's cameras did not
 * end in a picture, and why for the last few. One writer at a time, and the
 * session's begin, which blanks it, waits for the flag. */
std::atomic_flag camera_slot_busy = ATOMIC_FLAG_INIT;
struct CameraNote final {
	std::int64_t at{};
	uml::platform::CameraStillFailure failure{};
};
CameraNote camera_recent[session_camera_failures_kept];
std::uint64_t camera_failures{};
/* The newest note of each camera, by the guest's number of it. */
uml::platform::CameraPresentation camera_presented[session_cameras_kept];
bool camera_presented_has[session_cameras_kept];
/* What the newest note says of the front devices, which every camera's note of
 * a session says alike. */
struct CameraFronts final {
	std::uint8_t devices{};
	bool square{};
	bool dynamic{};
	std::uint16_t dropped{};
};
CameraFronts camera_fronts;

/* "WxH" for a size, or "-" for one that is not known. */
void camera_size(FixedText &text, std::uint32_t width,
	std::uint32_t height) noexcept
{
	if (width == 0 && height == 0) {
		text.text("-");
		return;
	}
	text.number(width).text("x").number(height);
}

/* An angle or a mirroring, or "-" where the platform stated none (negative). */
void camera_figure(FixedText &text, std::int64_t value) noexcept
{
	if (value < 0)
		text.text("-");
	else
		text.number(value);
}

void camera_yes_no(FixedText &text, bool value) noexcept
{
	text.text(value ? "yes" : "no");
}

/* One failure's line. A reader takes the words as they are written. */
void camera_failure_line(FixedText &text, const CameraNote &note) noexcept
{
	const auto &failure = note.failure;
	text.text("still at ").number(note.at).text(" camera ")
		.number(failure.camera).text(" ")
		.text(uml::platform::cameraPositionName(failure.position))
		.text(" stage ").text(uml::platform::stillStageName(failure.stage))
		.text(" asked ");
	camera_size(text, failure.askedWidth, failure.askedHeight);
	text.text(" resolved ");
	camera_size(text, failure.resolvedWidth, failure.resolvedHeight);
	text.text(" encoded ");
	camera_size(text, failure.encodedWidth, failure.encodedHeight);
	text.text(" compensation supported ")
		.text(uml::platform::factName(failure.compensationSupported))
		.text(" enabled ")
		.text(uml::platform::factName(failure.compensationEnabled))
		.text(" read ")
		.text(uml::platform::readingName(failure.compensationReading))
		.text(" exif ");
	if (failure.hostExifOrientation == 0)
		text.text("-");
	else
		text.number(failure.hostExifOrientation);
	text.text(" error ")
		.text(uml::platform::errorDomainName(failure.errorDomain));
	if (failure.errorDomain != uml::platform::CameraErrorDomain::none)
		text.text(" ").number(failure.errorCode);
	text.text("\n");
}

/* The lines that state how a camera is presented, in the order a reader takes
 * them: the camera as discovered and given its anchor, the device's aspect
 * ratio (only where its anchor lists ratios), the video connection, the photo
 * output and the refusals of its admissions (only where there were some).
 * Each is written when it has something to say. */
enum class CameraLineKind : std::uint8_t {
	mount, ratio, video, photo, refused
};
constexpr std::size_t camera_line_kinds = 5;
constexpr std::size_t camera_line_size = 320;
struct CameraLine final {
	char text[camera_line_size];
	std::size_t size{};
	bool present{};
};

void camera_line(FixedText &text, CameraLineKind kind,
	const uml::platform::CameraPresentation &c) noexcept
{
	using uml::platform::factName;
	text.text(" camera ").number(c.camera);
	switch (kind) {
	case CameraLineKind::mount:
		text.text(" ").text(uml::platform::cameraPositionName(c.position))
			.text(" type ").number(static_cast<std::uint8_t>(c.deviceType))
			.text(" angles");
		for (const auto angle : c.coordinatorAngles) {
			text.text(" ");
			camera_figure(text, angle);
		}
		text.text(" stated ").number(c.stated);
		/* The platform's own orientation, where the guest is told another
		 * (a tablet that boots turned). */
		if (c.platformStated != c.stated)
			text.text(" platform ").number(c.platformStated);
		text.text(" turn ").number(c.turn);
		/* The half turn the host applies to every picture of the camera
		 * (a front camera mounted at 90 states 270), where it does. */
		if (c.presentationTurn != 0)
			text.text(" hostturn ").number(c.presentationTurn);
		text.text(" earlier ");
		camera_yes_no(text, c.earlier);
		text.text(" anchor ");
		camera_size(text, c.anchorWidth, c.anchorHeight);
		text.text(" ratios ").number(c.anchorRatios).text(" fov ");
		camera_figure(text, c.anchorFieldOfView);
		break;
	case CameraLineKind::ratio:
		text.text(" pinned ")
			.text(uml::platform::aspectRatioName(c.ratioPinned))
			.text(" now ")
			.text(uml::platform::aspectRatioName(c.ratioNow))
			.text(" dims ");
		camera_size(text, c.ratioWidth, c.ratioHeight);
		text.text(" error ");
		if (c.ratioError == uml::platform::ratioPending)
			text.text("pending");
		else
			text.number(c.ratioError);
		break;
	case CameraLineKind::video:
		text.text(" default ");
		camera_figure(text, c.videoDefaultAngle);
		text.text(" set ");
		camera_figure(text, c.videoAngle);
		text.text(" supported ").text(factName(c.videoAngleSupported))
			.text(" mirror ");
		camera_figure(text, c.videoDefaultMirrored);
		text.text(" ");
		camera_figure(text, c.videoMirrored);
		text.text(" asked ");
		camera_size(text, c.videoAskedWidth, c.videoAskedHeight);
		text.text(" first ");
		camera_size(text, c.videoFirstWidth, c.videoFirstHeight);
		text.text(" stab ")
			.text(uml::platform::stabilizationName(c.videoStabilization));
		break;
	case CameraLineKind::photo:
		text.text(" compensation supported ")
			.text(factName(c.compensationSupported)).text(" enabled ")
			.text(factName(c.compensationEnabled)).text(" read ")
			.text(uml::platform::readingName(c.compensationReading))
			.text(" default ");
		camera_figure(text, c.photoDefaultAngle);
		text.text(" set ");
		camera_figure(text, c.photoAngle);
		text.text(" mirror ");
		camera_figure(text, c.photoDefaultMirrored);
		text.text(" ");
		camera_figure(text, c.photoMirrored);
		text.text(" max ");
		camera_size(text, c.photoMaximumWidth, c.photoMaximumHeight);
		text.text(" exif ");
		if (c.exifOrientation == 0)
			text.text("-");
		else
			text.number(c.exifOrientation);
		text.text(" center ").text(factName(c.centerStageActive))
			.text(" distortion ").text(factName(c.distortionCorrected));
		break;
	case CameraLineKind::refused:
		text.text(" count ").number(c.refusals).text(" last ");
		camera_figure(text, c.lastRefusal);
		break;
	}
	text.text("\n");
}

constexpr std::string_view camera_line_word(CameraLineKind kind) noexcept
{
	switch (kind) {
	case CameraLineKind::mount: return "mount";
	case CameraLineKind::ratio: return "ratio";
	case CameraLineKind::video: return "video";
	case CameraLineKind::photo: return "photo";
	case CameraLineKind::refused: break;
	}
	return "refused";
}

/* The camera slot, padded to its size: blank before the first note or failure,
 * else a format line, the count of failed stills and the lines that fit, in
 * this order of importance: the front cameras' and the imitating ones' (the
 * ones the guest sees turned or mirrored), the two newest failures, the other
 * cameras' mounting and then their other lines, and the older failures, newest
 * first (so that a longer line costs the oldest). Written in the order the
 * cameras are numbered, then the failures, oldest first. Held under
 * `camera_slot_busy`. */
void write_camera_slot(int descriptor) noexcept
{
	static char slot[session_camera_slot_bytes];
	static CameraLine lines[session_cameras_kept][camera_line_kinds];
	static CameraLine failures[session_camera_failures_kept];
	static bool taken[session_cameras_kept][camera_line_kinds];
	static bool taken_failure[session_camera_failures_kept];
	Text text{slot, sizeof(slot) - 1};
	bool any_note = false;
	for (std::size_t camera = 0; camera < session_cameras_kept; ++camera)
		any_note = any_note || camera_presented_has[camera];
	if (camera_failures != 0U || any_note) {
		const auto n = [](std::uint64_t value) {
			return static_cast<std::int64_t>(std::min<std::uint64_t>(
				value, INT64_MAX));
		};
		text.text("camera 1\nstills failed ").number(n(camera_failures))
			.text("\n");
		/* What the platform found of the front devices, in a line of its
		 * own that stays whichever camera the guest used: a front device
		 * whose formats were all dropped is no camera, and this is what is
		 * left of it. */
		if (any_note) {
			text.text("fronts ").number(camera_fronts.devices)
				.text(" square ");
			camera_yes_no(text, camera_fronts.square);
			text.text(" dynamic ");
			camera_yes_no(text, camera_fronts.dynamic);
			text.text(" dropped ").number(camera_fronts.dropped).text("\n");
		}
		std::size_t room = sizeof(slot) - 1 - text.size();
		/* The lines of each camera, as written for the camera's last note. */
		for (std::size_t camera = 0; camera < session_cameras_kept; ++camera) {
			const auto &c = camera_presented[camera];
			for (std::size_t kind = 0; kind < camera_line_kinds; ++kind) {
				auto &line = lines[camera][kind];
				taken[camera][kind] = false;
				line.size = 0;
				const auto which = static_cast<CameraLineKind>(kind);
				line.present = camera_presented_has[camera] &&
					(which == CameraLineKind::mount ||
					(which == CameraLineKind::ratio && c.ratioKnown) ||
					(which == CameraLineKind::video && c.videoKnown) ||
					(which == CameraLineKind::photo && c.photoKnown) ||
					(which == CameraLineKind::refused && c.refusals != 0));
				if (!line.present)
					continue;
				Text probe{line.text, sizeof(line.text)};
				probe.text(camera_line_word(which));
				camera_line(probe, which, c);
				line.size = probe.size();
			}
		}
		/* The failures, the newest first: the line of the failure `age`
		 * places from the newest (1 being the newest) is failures[age - 1]. */
		const std::size_t kept = static_cast<std::size_t>(std::min<
			std::uint64_t>(camera_failures, session_camera_failures_kept));
		for (std::size_t age = 1; age <= kept; ++age) {
			auto &line = failures[age - 1];
			taken_failure[age - 1] = false;
			Text probe{line.text, sizeof(line.text)};
			camera_failure_line(probe, camera_recent[(camera_failures - age) %
				session_camera_failures_kept]);
			line.size = probe.size();
		}
		const auto take = [&](CameraLine &line, bool &flag) {
			if (flag || line.size == 0 || line.size > room)
				return;
			room -= line.size;
			flag = true;
		};
		const auto front = [&](std::size_t camera) {
			const auto &c = camera_presented[camera];
			return c.position == uml::platform::CameraPosition::front ||
				c.earlier;
		};
		for (std::size_t camera = 0; camera < session_cameras_kept; ++camera)
			if (front(camera))
				for (std::size_t kind = 0; kind < camera_line_kinds; ++kind)
					take(lines[camera][kind], taken[camera][kind]);
		/* The newest failures that fit: the first that does not ends it. */
		std::size_t counted = 0;
		for (std::size_t age = 1; age <= kept && counted < 2; ++age, ++counted) {
			take(failures[age - 1], taken_failure[age - 1]);
			if (!taken_failure[age - 1])
				break;
		}
		for (std::size_t camera = 0; camera < session_cameras_kept; ++camera)
			if (!front(camera))
				take(lines[camera][0], taken[camera][0]);
		for (std::size_t camera = 0; camera < session_cameras_kept; ++camera)
			if (!front(camera))
				for (std::size_t kind = 1; kind < camera_line_kinds; ++kind)
					take(lines[camera][kind], taken[camera][kind]);
		for (std::size_t age = counted + 1; age <= kept; ++age) {
			take(failures[age - 1], taken_failure[age - 1]);
			if (!taken_failure[age - 1])
				break;
		}
		for (std::size_t camera = 0; camera < session_cameras_kept; ++camera)
			for (std::size_t kind = 0; kind < camera_line_kinds; ++kind)
				if (taken[camera][kind])
					text.text({lines[camera][kind].text,
						lines[camera][kind].size});
		for (std::size_t age = kept; age > 0; --age)
			if (taken_failure[age - 1])
				text.text({failures[age - 1].text, failures[age - 1].size});
	}
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', sizeof(slot) - 1 - used);
	slot[sizeof(slot) - 1] = '\n';
	ssize_t written;
	do {
		written = pwrite(descriptor, slot, sizeof(slot),
			session_camera_offset);
	} while (written < 0 && errno == EINTR);
}

/* Waits one checkpoint interval inside libsystem_kernel alone: the thread
 * fence defers a thread inside libsystem_c (nanosleep, usleep) on every pass,
 * and so never holds this uml-* thread. */
void wait_for_next_checkpoint() noexcept
{
	mach_timebase_info_data_t base{};
	(void)mach_timebase_info(&base);
	const std::uint64_t deadline = mach_absolute_time() +
		static_cast<std::uint64_t>(memory_checkpoint_interval_ms) *
		1000000ULL * base.denom / base.numer;
	while (mach_wait_until(deadline) == KERN_ABORTED) {
	}
}

void *checkpoint_memory(void *) noexcept
{
	(void)pthread_setname_np("uml-session-record");
	for (;;) {
		wait_for_next_checkpoint();
		if (session_open.load(std::memory_order_acquire))
			checkpoint_session_memory();
	}
	return nullptr;
}

/* One thread for the process's life: it never touches a session that has
 * ended, and blocks every signal, so the kernel's process-directed signals
 * never pick it. */
void start_checkpoints() noexcept
{
	if (checkpoint_thread_started.exchange(true))
		return;
	sigset_t all;
	sigset_t previous;
	sigfillset(&all);
	if (pthread_sigmask(SIG_SETMASK, &all, &previous) != 0)
		return;
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) == 0) {
		(void)pthread_attr_setdetachstate(&attributes,
			PTHREAD_CREATE_DETACHED);
		(void)pthread_attr_setstacksize(&attributes, 64 * 1024);
		if (pthread_create(&thread, &attributes, checkpoint_memory,
			    nullptr) != 0)
			checkpoint_thread_started.store(false);
		(void)pthread_attr_destroy(&attributes);
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

/* What tells this installation from another, as the kernel's own arguments
 * state it when the session begins: the Android guest's serial number, the
 * host's adb public key and the time zone the guest was given. The kernel
 * echoes its command line, and init prints a property it cannot set with its
 * value, so the output the record quotes can carry them in places no rule
 * knows by name: the record takes out the values themselves, wherever they
 * stand. Static, so that an end formats nothing it must allocate; written only
 * by the session's begin, under `state_checkpointing`, and read by whoever
 * formats a state. A value that does not fit or is too short to tell from an
 * ordinary word is not kept; the names below mask its assignments all the
 * same. */
template <std::size_t Capacity>
class KnownValue final {
public:
	void clear() noexcept
	{
		length_ = 0;
	}

	void set(std::string_view value, std::size_t shortest) noexcept
	{
		length_ = 0;
		if (value.size() < shortest || value.size() > Capacity)
			return;
		std::memcpy(text_, value.data(), value.size());
		length_ = value.size();
	}

	[[nodiscard]] std::string_view view() const noexcept
	{
		return {text_, length_};
	}

private:
	char text_[Capacity]{};
	std::size_t length_{};
};

/* The longest adb key the runtime accepts is 1024 characters
 * (kernarium::adb::maximumKeyLength); a serial number has 6 to 20, and a time
 * zone's name stays well under 64. */
constexpr std::size_t known_key_capacity = 1024;
constexpr std::size_t known_serial_capacity = 64;
constexpr std::size_t known_zone_capacity = 64;
/* The values a text must hold to be taken out by value, and the start of one
 * that is taken out at the end of a text: shorter ones are ordinary words. */
constexpr std::size_t known_key_shortest = 8;
constexpr std::size_t known_serial_shortest = 4;
constexpr std::size_t known_zone_shortest = 4;
KnownValue<known_key_capacity> known_key;
KnownValue<known_serial_capacity> known_serial;
KnownValue<known_zone_capacity> known_zone;

/* What stands where a value was taken out. */
constexpr std::string_view masked_value{"<masked>"};
constexpr std::string_view masked_zone{"<time zone>"};
constexpr std::string_view masked_address{"<address>"};

/* The arguments whose value is an identity value. */
constexpr std::string_view serial_argument{"androidboot.serialno="};
constexpr std::string_view key_argument{"androidboot.kernarium.adb_key="};
constexpr std::string_view zone_argument{"androidboot.kernarium.timezone="};

/* The word after `name` in `argument` (the kernel takes several words in one
 * argument), without a pair of quotes around it; empty when none. */
std::string_view argument_value(std::string_view argument,
	std::string_view name) noexcept
{
	for (auto at = argument.find(name); at != std::string_view::npos;
	     at = argument.find(name, at + 1)) {
		if (at != 0 && argument[at - 1] != ' ' && argument[at - 1] != '\t')
			continue;
		auto value = argument.substr(at + name.size());
		value = value.substr(0, value.find_first_of(" \t"));
		if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
		    value.back() == value.front())
			value = value.substr(1, value.size() - 2);
		return value;
	}
	return {};
}

/* The session's own values, from its arguments. Clears the ones an earlier
 * session had. */
void learn_known_values(std::span<const std::string> arguments) noexcept
{
	known_key.clear();
	known_serial.clear();
	known_zone.clear();
	for (const std::string &argument : arguments) {
		if (const auto value = argument_value(argument, key_argument);
		    !value.empty())
			known_key.set(value, known_key_shortest);
		if (const auto value = argument_value(argument, serial_argument);
		    !value.empty())
			known_serial.set(value, known_serial_shortest);
		/* A zone with no `/` (UTC) is too short a word to take out of a
		 * text. */
		if (const auto value = argument_value(argument, zone_argument);
		    value.find('/') != std::string_view::npos)
			known_zone.set(value, 1);
	}
}

/* One piece of the text to replace: [start, end) becomes `replacement`. */
struct Mask final {
	std::size_t start{std::string_view::npos};
	std::size_t end{};
	std::string_view replacement;
};

/* The earlier of two masks, the longer when they start together. */
void keep_earlier(Mask &best, const Mask &other) noexcept
{
	if (other.start == std::string_view::npos)
		return;
	if (best.start == std::string_view::npos || other.start < best.start ||
	    (other.start == best.start && other.end > best.end))
		best = other;
}

[[nodiscard]] bool alphanumeric(char character) noexcept
{
	return (character >= '0' && character <= '9') ||
		(character >= 'A' && character <= 'Z') ||
		(character >= 'a' && character <= 'z');
}

[[nodiscard]] bool word_character(char character) noexcept
{
	return alphanumeric(character) || character == '_';
}

[[nodiscard]] bool zone_character(char character) noexcept
{
	return alphanumeric(character) || character == '_' || character == '/' ||
		character == '+' || character == '-';
}

[[nodiscard]] bool hex_digit(char character) noexcept
{
	return (character >= '0' && character <= '9') ||
		(character >= 'A' && character <= 'F') ||
		(character >= 'a' && character <= 'f');
}

/* What continues a word of the adb key (base64: letters, digits, `+`, `/`, and
 * `=` as the padding that ends it). A key that stands against one is part of a
 * longer word and no key. An `=` in front of it is an assignment's, not the
 * padding of a word before it, so it does not continue the word there. */
[[nodiscard]] bool key_before(char character) noexcept
{
	return word_character(character) || character == '+' || character == '/';
}

[[nodiscard]] bool key_after(char character) noexcept
{
	return key_before(character) || character == '=';
}

/* The value of the assignment `name` (up to the next white space) at or after
 * `from`. */
Mask by_name(std::string_view text, std::size_t from, std::string_view name,
	std::string_view replacement) noexcept
{
	const auto at = text.find(name, from);
	if (at == std::string_view::npos)
		return {};
	const std::size_t value = at + name.size();
	const auto stop = text.find_first_of(" \t\r\n", value);
	return {value, stop == std::string_view::npos ? text.size() : stop,
		replacement};
}

/* `value` where it stands at or after `from`, with no character of `before`'s
 * kind in front of it and none of `after`'s behind it: a longer word that
 * holds it is not the value. */
template <class Before, class After>
Mask by_value(std::string_view text, std::size_t from, std::string_view value,
	std::string_view replacement, Before before, After after) noexcept
{
	if (value.empty())
		return {};
	for (auto at = text.find(value, from); at != std::string_view::npos;
	     at = text.find(value, at + 1)) {
		const std::size_t end = at + value.size();
		if ((at == 0 || !before(text[at - 1])) &&
		    (end == text.size() || !after(text[end])))
			return {at, end, replacement};
	}
	return {};
}

/* The start of `value` at the end of the text, `shortest` characters or more:
 * a write that ended inside it, whose rest the next write brings. */
template <class Before>
Mask by_partial_value(std::string_view text, std::size_t from,
	std::string_view value, std::size_t shortest, std::string_view replacement,
	Before before) noexcept
{
	if (value.size() < 2 || shortest == 0)
		return {};
	const std::size_t most =
		std::min(value.size() - 1, text.size() - std::min(from, text.size()));
	for (std::size_t length = most; length >= shortest; --length) {
		const std::size_t at = text.size() - length;
		if (text.compare(at, length, value.substr(0, length)) == 0 &&
		    (at == 0 || !before(text[at - 1])))
			return {at, text.size(), replacement};
	}
	return {};
}

/* A hardware address in its colon spelling (`aa:bb:cc:dd:ee:ff`) as a word of
 * its own, which names an installation's network interface whichever it is. */
Mask by_address(std::string_view text, std::size_t from) noexcept
{
	constexpr std::size_t length = 17;
	for (std::size_t at = from; at + length <= text.size(); ++at) {
		if (at != 0 && word_character(text[at - 1]))
			continue;
		bool address = true;
		for (std::size_t index = 0; index < length && address; ++index)
			address = index % 3 == 2 ? text[at + index] == ':' :
						   hex_digit(text[at + index]);
		if (address &&
		    (at + length == text.size() || !word_character(text[at + length])))
			return {at, at + length, masked_address};
	}
	return {};
}

/* The first thing at or after `from` that the record does not keep: the value
 * of an assignment of the serial number, the adb key or the time zone, those
 * values wherever they stand, and any hardware address. */
Mask next_mask(std::string_view text, std::size_t from) noexcept
{
	Mask best;
	keep_earlier(best, by_name(text, from, "serialno=", masked_value));
	keep_earlier(best, by_name(text, from, "adb_key=", masked_value));
	keep_earlier(best, by_name(text, from, "timezone=", masked_zone));
	keep_earlier(best, by_value(text, from, known_key.view(), masked_value,
				key_before, key_after));
	keep_earlier(best, by_value(text, from, known_serial.view(), masked_value,
				word_character, word_character));
	keep_earlier(best, by_value(text, from, known_zone.view(), masked_zone,
				zone_character, zone_character));
	keep_earlier(best, by_address(text, from));
	keep_earlier(best, by_partial_value(text, from, known_key.view(),
				known_key_shortest, masked_value, key_before));
	keep_earlier(best, by_partial_value(text, from, known_serial.view(),
				known_serial_shortest, masked_value, word_character));
	keep_earlier(best, by_partial_value(text, from, known_zone.view(),
				known_zone_shortest, masked_zone, zone_character));
	return best;
}

/* `text` as the record keeps it: printable, without what identifies the
 * installation. The kernel echoes its command line, which carries the guest's
 * serial number (`androidboot.serialno=`), the host's adb key and the guest's
 * time zone; the record and the bug report archive that copies it do not keep
 * the values, only that they were there. The masks are matched on the text as
 * it is stored (`scratch`, which holds at most a state's bytes): a byte that
 * the record drops, as a carriage return, must not join the two halves of a
 * value that the guest printed apart. */
void keep_masked(Text &record, char *scratch, std::string_view text,
	bool lines) noexcept
{
	const std::string_view stored = Text::printable_text(scratch,
		session_state_max_bytes, text, lines);
	std::size_t from = 0;
	for (Mask mask = next_mask(stored, from); mask.start != std::string_view::npos;
	     mask = next_mask(stored, from)) {
		record.text(stored.substr(from, mask.start - from));
		record.text(mask.replacement);
		from = mask.end;
	}
	record.text(stored.substr(from));
}

/* `tail` from its first complete line when the ring has dropped output of the
 * boot: its first bytes are then the middle of a line, perhaps of
 * `androidboot.serialno=` or of its value, which keep_masked cannot know to
 * mask. A tail that begins where the boot's output begins (`output_start`, the
 * session's first byte) starts at its first line, and so does one the ring has
 * dropped nothing of. */
std::string_view from_whole_line(std::string_view tail) noexcept
{
	const std::uint64_t end = output_end.load(std::memory_order_acquire);
	const std::uint64_t begun = output_start.load(std::memory_order_acquire);
	if (!output_clipped.load(std::memory_order_acquire) &&
	    end - begun <= output_size)
		return tail;
	const auto newline = tail.find('\n');
	return newline == std::string_view::npos ? std::string_view{} :
						   tail.substr(newline + 1);
}

/* The state: the session's lines, what it says of the boot where the guest's
 * console reports one, a message and the quoted output. The text is followed
 * by newlines to the whole of the state's area (write_state), which the
 * readers ignore; the size returned is the text's. */
std::size_t format_state(char *buffer, char *scratch, SessionEnd end,
	int status, std::string_view message, std::string_view tail) noexcept
{
	Text record{buffer, session_state_max_bytes};
	record.text("state ")
		.text(end == SessionEnd::running ? "running" : "ended")
		.text("\nend ").text(session_end_name(end))
		.text("\nstatus ").number(status)
		.text("\npid ").number(getpid())
		.text("\nstarted ")
		.number(session_started.load(std::memory_order_relaxed));
	if (end != SessionEnd::running)
		record.text("\nended ").number(now());
	if (boot_reported.load(std::memory_order_relaxed))
		record.text("\nboot ")
			.text(boot_signal.completed() ? "completed" :
				boot_signal.init_observed() ? "unfinished" : "unobserved")
			.text("\nrestarts ")
			.number(boot_restarts.load(std::memory_order_relaxed));
	if (boot_reported.load(std::memory_order_relaxed)) {
		switch (boot_ramdisk.load(std::memory_order_relaxed)) {
		case BootRamdisk::stock: record.text("\nramdisk stock"); break;
		case BootRamdisk::root: record.text("\nramdisk root"); break;
		case BootRamdisk::recovery: record.text("\nramdisk recovery"); break;
		case BootRamdisk::unreported: break;
		}
	}
	if (!message.empty()) {
		record.text("\nmessage ");
		keep_masked(record, scratch, message, false);
	}
	record.text("\n");
	tail = from_whole_line(tail);
	if (!tail.empty()) {
		record.text("kernel-output\n");
		keep_masked(record, scratch, tail, true);
	}
	const std::size_t size = std::min(record.size(), session_state_max_bytes);
	std::memset(buffer + size, '\n', session_state_max_bytes - size);
	return size;
}

/* Publishes a state (formatted into a buffer of session_state_max_bytes) in
 * one pwrite that covers the whole of the state's area. The area is never
 * shortened between two states: a ftruncate after a shorter state left a
 * reader that came between the two calls with the new state's prefix followed
 * by the old state's suffix. Every writer covers the same bytes, so whichever
 * lands last is the whole of what a reader sees, a checkpoint delayed past an
 * end included. */
void write_state(int descriptor, const char *text) noexcept
{
	ssize_t written;
	do {
		written = pwrite(descriptor, text, session_state_max_bytes,
			session_state_offset);
	} while (written < 0 && errno == EINTR);
}

/* A checkpoint under way (it set `state_checkpointing` before it looked at
 * `session_open`, which the end has already cleared) usually finishes before
 * the end writes. The wait is bounded because an end may run where the
 * checkpoint thread no longer does, so the end does not rely on it:
 * write_record's publication and the checkpoint's repair make it the last
 * write. */
void wait_out_state_checkpoint() noexcept
{
	const std::int64_t deadline = steady_ms() + state_checkpoint_wait_ms;
	while (state_checkpointing.test(std::memory_order_seq_cst) &&
	       steady_ms() < deadline)
		sched_yield();
}

/* The session's begin and its ends; the begin's is written under
 * `state_checkpointing` (begin_session_record). */
void write_record_unguarded(int descriptor, SessionEnd end, int status,
	std::string_view message, std::string_view tail) noexcept
{
	(void)format_state(record_text, record_scratch, end, status, message,
		tail);
	write_state(descriptor, record_text);
}

void write_record(SessionEnd end, int status, std::string_view message,
	std::string_view tail) noexcept
{
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	/* The last counts of the session, which no checkpoint may follow. */
	write_network_slot(descriptor);
	wait_out_state_checkpoint();
	const std::size_t size = format_state(record_text, record_scratch, end,
		status, message, tail);
	/* A checkpoint whose write lands from here on writes this record again
	 * (checkpoint_state); it reads the size, so the record is formatted. */
	terminal_size.store(size, std::memory_order_seq_cst);
	write_state(descriptor, record_text);
}

/* Rewrites the state of a guest whose boot has not completed when the output
 * grew since the last time, and once more when the boot finished, without the
 * output: the record then says the boot completed and holds nothing that
 * grows. Nothing else changes the state while the session runs, so a silent
 * boot costs no write. */
void checkpoint_state(int descriptor) noexcept
{
	if (!boot_reported.load(std::memory_order_relaxed) ||
	    !session_open.load(std::memory_order_seq_cst))
		return;
	const bool completed = boot_signal.completed();
	const std::uint64_t output_now = output_end.load(std::memory_order_acquire);
	if (checkpointed_completed.load(std::memory_order_relaxed) ||
	    (!completed &&
		    output_now == checkpointed_output_end.load(
			    std::memory_order_relaxed)))
		return;
	if (state_checkpointing.test_and_set(std::memory_order_seq_cst))
		return;
	/* The flag is held: an end that clears `session_open` from here on waits
	 * for this write, and one that did before is seen now. */
	if (session_open.load(std::memory_order_seq_cst)) {
		const bool done = boot_signal.completed();
		const std::size_t quoted = done ? 0 :
			kernel_output_tail({checkpoint_tail, sizeof(checkpoint_tail)});
		(void)format_state(checkpoint_text, checkpoint_scratch,
			SessionEnd::running, 0, {}, {checkpoint_tail, quoted});
		if (session_open.load(std::memory_order_seq_cst)) {
			write_state(descriptor, checkpoint_text);
			checkpointed_output_end.store(output_now,
				std::memory_order_relaxed);
			checkpointed_completed.store(done, std::memory_order_relaxed);
			/* An end that came while this write was delayed has written
			 * its record, or will: if this write landed after it, write
			 * it again, which is the same bytes as the end's own. The
			 * window that remains is accepted: a process that dies after
			 * this write lands and before the next line writes leaves
			 * "running" on disk, which is what a record that was never
			 * ended says. */
			if (terminal_size.load(std::memory_order_seq_cst) != 0)
				write_state(descriptor, record_text);
		}
	}
	state_checkpointing.clear(std::memory_order_seq_cst);
}

std::string_view output_tail() noexcept
{
	return {tail_text, kernel_output_tail(tail_text)};
}

/* Records the session's first end; `quote_output` adds the kernel's last
 * output, which only the first end reads into the static copy. */
void end_session(SessionEnd end, int status, std::string_view message,
	bool quote_output) noexcept
{
	if (!session_open.exchange(false, std::memory_order_seq_cst))
		return;
	write_record(end, status, message,
		quote_output ? output_tail() : std::string_view{});
}

void record_process_exit()
{
	end_session(SessionEnd::process_exit, 0,
		"exit() was called while the guest ran", true);
}

/* The signal number in a host-fault report's registers, else 0. */
int fault_signal(std::string_view registers) noexcept
{
	constexpr std::string_view key{"signal="};
	if (!registers.starts_with(key))
		return 0;
	int signal_number = 0;
	(void)std::from_chars(registers.data() + key.size(),
		registers.data() + registers.size(), signal_number);
	return signal_number;
}

void record_host_fault() noexcept
{
	if (crash_recorded.exchange(true, std::memory_order_acq_rel))
		return;
	if (!session_open.exchange(false, std::memory_order_seq_cst))
		return;
	const auto tail = output_tail();
	const auto registers = host_fault_message(tail);
	write_record(SessionEnd::host_fault, fault_signal(registers), registers,
		tail);
}

void name_thread(std::string_view message) noexcept
{
	char name[thread_name_size];
	Text text{name, sizeof(name) - 1};
	text.text("panic: ").printable(message, false);
	name[text.size()] = '\0';
	(void)pthread_setname_np(name);
}

} // namespace

std::string_view session_end_name(SessionEnd end) noexcept
{
	switch (end) {
	case SessionEnd::running:
		return "running";
	case SessionEnd::powered_off:
		return "powered-off";
	case SessionEnd::restarted:
		return "restarted";
	case SessionEnd::stopped:
		return "stopped";
	case SessionEnd::failed:
		return "failed";
	case SessionEnd::guest_panic:
		return "guest-panic";
	case SessionEnd::kernel_abort:
		return "kernel-abort";
	case SessionEnd::kernel_exit:
		return "kernel-exit";
	case SessionEnd::hibernation_restore_failed:
		return "hibernation-restore-failed";
	case SessionEnd::process_exit:
		return "process-exit";
	case SessionEnd::host_fault:
		return "host-fault";
	}
	return "unknown";
}

std::size_t console_output_descriptors(
	std::span<const std::string> kernel_arguments, std::span<int> descriptors)
	noexcept
{
	std::size_t found = 0;
	for (const auto &argument : kernel_arguments) {
		const std::string_view text{argument};
		const auto equals = text.find('=');
		if (equals == std::string_view::npos)
			continue;
		const auto name = text.substr(0, equals);
		const auto channel = name.starts_with("con") ? name.substr(3) :
			name.starts_with("ssl") ? name.substr(3) :
			std::string_view{"-"};
		if (!std::all_of(channel.begin(), channel.end(),
			    [](char c) { return c >= '0' && c <= '9'; }))
			continue;
		/* fd:IN,fd:OUT writes to OUT; fd:BOTH to BOTH. */
		auto value = text.substr(equals + 1);
		if (const auto comma = value.find(','); comma != value.npos)
			value = value.substr(comma + 1);
		if (!value.starts_with("fd:"))
			continue;
		value.remove_prefix(3);
		int descriptor = -1;
		const auto parsed = std::from_chars(value.data(),
			value.data() + value.size(), descriptor);
		if (parsed.ec != std::errc{} ||
		    parsed.ptr != value.data() + value.size() || descriptor < 0)
			continue;
		if (found < descriptors.size())
			descriptors[found] = descriptor;
		++found;
	}
	return std::min(found, descriptors.size());
}

bool begin_session_record(std::string_view path,
	std::span<const std::string> kernel_arguments) noexcept
{
	int consoles[console_slots] = {-1, -1, -1, -1};
	(void)console_output_descriptors(kernel_arguments, consoles);
	for (std::size_t slot = 0; slot < console_slots; ++slot)
		console_descriptors[slot].store(consoles[slot],
			std::memory_order_relaxed);
	boot_signal.reset();
	output_start.store(output_end.load(std::memory_order_acquire),
		std::memory_order_release);
	output_clipped.store(false, std::memory_order_release);
	if (path.empty())
		return true;
	const std::string name{path};
	int descriptor;
	do {
		descriptor = open(name.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC,
			0600);
	} while (descriptor < 0 && errno == EINTR);
	if (descriptor < 0) {
		const int error = errno;
		dprintf(STDERR_FILENO,
			"uml: the session record cannot be opened: %s\n",
			strerror(error));
		return false;
	}
	/* The record keeps one descriptor number for the process's life: a
	 * checkpoint may hold it at any time, and a closed number another open
	 * reused would take the checkpoint's bytes. A later session moves its
	 * file onto that number. */
	if (const int kept_descriptor =
		    record_descriptor.load(std::memory_order_acquire);
	    kept_descriptor >= 0) {
		const int moved = dup2(descriptor, kept_descriptor);
		(void)close(descriptor);
		if (moved < 0)
			return false;
		descriptor = kept_descriptor;
		(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	} else {
		record_descriptor.store(descriptor, std::memory_order_release);
	}
	if (!exit_hook_installed.exchange(true))
		(void)atexit(record_process_exit);
	session_started.store(now(), std::memory_order_relaxed);
	forget_history();
	/* A checkpoint thread of the session before may still be formatting the
	 * network slot from the old baseline: write the new one between two
	 * writers. This runs in no signal handler, so it may wait for the slot. */
	while (network_slot_busy.test_and_set(std::memory_order_acquire))
		sched_yield();
	network_baseline = {};
	if (const auto reader = network_reader.load(std::memory_order_acquire);
	    reader == nullptr || !reader(network_baseline))
		network_baseline = {};
	network_slot_busy.clear(std::memory_order_release);
	/* A denial under way when the session begins is one the session saw. */
	notice_times.store(
		notice_now.load(std::memory_order_relaxed) != NetworkNotice::none ?
			1 : 0,
		std::memory_order_relaxed);
	write_memory_slot(descriptor, true);
	write_blank_stall_slot(descriptor);
	write_network_slot(descriptor);
	/* The storage slot of the session before is blank before any settle of
	 * this one is noted. */
	while (storage_slot_busy.test_and_set(std::memory_order_acquire))
		sched_yield();
	storage_settles = 0;
	storage_unsettled = 0;
	storage_has_last = false;
	write_storage_slot(descriptor);
	storage_slot_busy.clear(std::memory_order_release);
	/* The camera slot likewise: no failure of the session before is named. */
	while (camera_slot_busy.test_and_set(std::memory_order_acquire))
		sched_yield();
	camera_failures = 0;
	std::memset(camera_presented_has, 0, sizeof(camera_presented_has));
	camera_fronts = {};
	write_camera_slot(descriptor);
	camera_slot_busy.clear(std::memory_order_release);
	/* The same for a checkpoint of the state, which also reads the values the
	 * record takes out of what it quotes. */
	while (state_checkpointing.test_and_set(std::memory_order_seq_cst))
		sched_yield();
	learn_known_values(kernel_arguments);
	/* The file is as long as the record is, once, before the session's first
	 * state: that state covers all of the state's area (write_state), and no
	 * write after it changes the length. */
	(void)ftruncate(descriptor,
		static_cast<off_t>(session_state_offset + session_state_max_bytes));
	checkpointed_output_end.store(output_end.load(std::memory_order_acquire),
		std::memory_order_relaxed);
	checkpointed_completed.store(false, std::memory_order_relaxed);
	terminal_size.store(0, std::memory_order_seq_cst);
	session_open.store(true, std::memory_order_seq_cst);
	write_record_unguarded(descriptor, SessionEnd::running, 0, {}, {});
	state_checkpointing.clear(std::memory_order_seq_cst);
	start_checkpoints();
	return true;
}

bool session_record_open() noexcept
{
	return session_open.load(std::memory_order_acquire) &&
		record_descriptor.load(std::memory_order_acquire) >= 0;
}

void write_session_stall_slot(std::string_view slot) noexcept
{
	if (slot.size() != session_stall_slot_bytes ||
	    !session_open.load(std::memory_order_acquire))
		return;
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	ssize_t written;
	do {
		written = pwrite(descriptor, slot.data(), slot.size(),
			stall_slot_offset);
	} while (written < 0 && errno == EINTR);
}

void set_memory_terms_reader(MemoryTermsReader reader) noexcept
{
	terms_reader.store(reader, std::memory_order_release);
}

void checkpoint_session_memory() noexcept
{
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor >= 0) {
		write_memory_slot(descriptor, false);
		write_network_slot(descriptor);
		checkpoint_state(descriptor);
	}
}

void set_run_figures_reader(RunFiguresReader reader) noexcept
{
	figures_reader.store(reader, std::memory_order_release);
}

void set_network_counts_reader(SessionNetworkReader reader) noexcept
{
	network_reader.store(reader, std::memory_order_release);
}

void note_guest_settle(SettleKind kind, GuestStorage storage,
	SettleSuspension suspension) noexcept
{
	if (!session_open.load(std::memory_order_acquire))
		return;
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	while (storage_slot_busy.test_and_set(std::memory_order_acquire))
		sched_yield();
	++storage_settles;
	if (!guest_storage_settled(storage))
		++storage_unsettled;
	storage_has_last = true;
	storage_last_kind = kind;
	storage_last_storage = storage;
	storage_last_suspension = suspension;
	storage_last_at = now();
	write_storage_slot(descriptor);
	storage_slot_busy.clear(std::memory_order_release);
}

void note_camera_still_failure(
	const uml::platform::CameraStillFailure &failure) noexcept
{
	if (!session_open.load(std::memory_order_acquire))
		return;
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	while (camera_slot_busy.test_and_set(std::memory_order_acquire))
		sched_yield();
	camera_recent[camera_failures % session_camera_failures_kept] = {
		.at = now(), .failure = failure};
	++camera_failures;
	write_camera_slot(descriptor);
	camera_slot_busy.clear(std::memory_order_release);
}

void note_camera_presentation(
	const uml::platform::CameraPresentation &presentation) noexcept
{
	if (presentation.camera >= session_cameras_kept)
		return;
	if (!session_open.load(std::memory_order_acquire))
		return;
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	while (camera_slot_busy.test_and_set(std::memory_order_acquire))
		sched_yield();
	camera_presented[presentation.camera] = presentation;
	camera_presented_has[presentation.camera] = true;
	camera_fronts = {presentation.frontDevices, presentation.squareFormats,
		presentation.dynamicRatios, presentation.frontFormatsDropped};
	write_camera_slot(descriptor);
	camera_slot_busy.clear(std::memory_order_release);
}

void set_session_boot_facts(bool console_reports_completion,
	unsigned restarts) noexcept
{
	boot_reported.store(console_reports_completion,
		std::memory_order_relaxed);
	boot_restarts.store(restarts, std::memory_order_relaxed);
	boot_ramdisk.store(BootRamdisk::unreported, std::memory_order_relaxed);
}

void set_session_boot_ramdisk(BootRamdisk ramdisk) noexcept
{
	boot_ramdisk.store(ramdisk, std::memory_order_relaxed);
}

void set_network_notice(NetworkNotice notice) noexcept
{
	const auto before =
		notice_now.exchange(notice, std::memory_order_relaxed);
	if (notice != NetworkNotice::none && notice != before)
		notice_times.fetch_add(1, std::memory_order_relaxed);
}

void end_session_record(SessionEnd end, int status,
	std::string_view message) noexcept
{
	/* An end the guest or the person chose quotes no output, unless the boot
	 * of a guest that reports it had not finished: that is where it stuck. */
	const bool unfinished_boot = boot_reported.load(std::memory_order_relaxed) &&
		!boot_signal.completed();
	end_session(end, status, message, unfinished_boot ||
		(end != SessionEnd::powered_off && end != SessionEnd::restarted &&
			end != SessionEnd::stopped));
}

void record_kernel_crash() noexcept
{
	/* A second vCPU that crashes while the first records leaves the first
	 * one's record and name alone. */
	if (crash_recorded.exchange(true, std::memory_order_acq_rel))
		return;
	const auto tail = output_tail();
	const auto message = panic_message(tail);
	if (!message.empty())
		name_thread(message);
	if (!session_open.exchange(false, std::memory_order_seq_cst))
		return;
	write_record(message.empty() ? SessionEnd::kernel_abort :
		SessionEnd::guest_panic, SIGABRT, message, tail);
}

ssize_t kernel_write(int descriptor, const void *buffer, std::size_t length)
	noexcept
{
	if (length != 0 && buffer != nullptr && kept(descriptor)) {
		keep_output(buffer, length);
		boot_signal.feed(buffer, length);
		/* The fault ends the process as soon as the handler returns. */
		if (descriptor == STDERR_FILENO &&
		    std::string_view{static_cast<const char *>(buffer), length}
			    .starts_with(host_fault_line))
			record_host_fault();
	}
	const ::uml::platform::ScopedHostWait wait{
		::uml::platform::HostWait::send};
	return write(descriptor, buffer, length);
}

void *kernel_output_import(const char *name) noexcept
{
	if (name != nullptr && name[0] == '_')
		++name;
	return name != nullptr && std::strcmp(name, "write") == 0 ?
		reinterpret_cast<void *>(&kernel_write) : nullptr;
}

bool console_boot_completed() noexcept
{
	return boot_signal.completed();
}

bool console_init_observed() noexcept
{
	return boot_signal.init_observed();
}

std::size_t kernel_output_tail(std::span<char> buffer) noexcept
{
	const std::uint64_t end = output_end.load(std::memory_order_acquire);
	const std::uint64_t begun = output_start.load(std::memory_order_acquire);
	const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
		{end > begun ? end - begun : 0,
			static_cast<std::uint64_t>(output_size),
			static_cast<std::uint64_t>(buffer.size())}));
	for (std::size_t index = 0; index < count; ++index)
		buffer[index] = output[(end - count + index) % output_size].load(
			std::memory_order_relaxed);
	return count;
}

std::string_view panic_message(std::string_view output_text) noexcept
{
	const auto start = output_text.rfind(panic_prefix);
	if (start == std::string_view::npos)
		return {};
	auto message = output_text.substr(start + panic_prefix.size());
	if (const auto end = message.find('\n'); end != message.npos)
		message = message.substr(0, end);
	return message;
}

std::string_view host_fault_message(std::string_view output_text) noexcept
{
	const auto start = output_text.rfind(host_fault_registers);
	if (start == std::string_view::npos)
		return {};
	auto message = output_text.substr(start + host_fault_prefix.size());
	if (const auto end = message.find('\n'); end != message.npos)
		message = message.substr(0, end);
	return message;
}

void reset_session_record() noexcept
{
	session_open.store(false, std::memory_order_release);
	/* The number stays the record's (begin_session_record), on no file. */
	if (const int descriptor = record_descriptor.load(); descriptor >= 0) {
		if (const int empty = open("/dev/null", O_WRONLY | O_CLOEXEC);
		    empty >= 0) {
			(void)dup2(empty, descriptor);
			(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
			(void)close(empty);
		}
	}
	for (auto &console : console_descriptors)
		console.store(-1, std::memory_order_relaxed);
	output_end.store(0, std::memory_order_release);
	output_start.store(0, std::memory_order_release);
	output_clipped.store(false, std::memory_order_release);
	boot_signal.reset();
	boot_reported.store(false, std::memory_order_relaxed);
	boot_restarts.store(0, std::memory_order_relaxed);
	boot_ramdisk.store(BootRamdisk::unreported, std::memory_order_relaxed);
	crash_recorded.store(false, std::memory_order_release);
	while (state_checkpointing.test_and_set(std::memory_order_seq_cst))
		sched_yield();
	learn_known_values({});
	state_checkpointing.clear(std::memory_order_seq_cst);
}

} // namespace uml::runtime
