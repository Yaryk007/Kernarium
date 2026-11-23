// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "memory/domain_heap.hpp"
#include "memory/shared_backing.hpp"
#include "native_window_test_support.hpp"
#include "require.hpp"
#include "runtime/guest_memory_budget.hpp"
#include "runtime/native_kernel_layout.hpp"
#include "start_refusal.hpp"
#include "testing.hpp"

#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>

/* A process with little address space (an iOS application without Extended
 * Virtual Addressing has six GiB) refuses a guest through the run's pre-start
 * checks, one at a time: its window, then the process's heap region. A refusal
 * leaves nothing of the run in the process, and what the run reserved is
 * where a host that offers the room for it put it: the heap region does not
 * take the address a window needs. Each case is a process of its own, so that
 * the heap region is still unreserved and the address space it is limited to
 * is the one these runs use. The limit is the system's own (RLIMIT_AS, which
 * counts every mapping of the process). */

namespace {

using uml::testing::require;

constexpr std::uint64_t mib = std::uint64_t{1} << 20;

unsigned int entries;

int counting_kernel(int, char *const[], char *const[])
{
	++entries;
	return 0;
}

/* The mapped size of this process: what RLIMIT_AS limits. */
std::uint64_t mapped_bytes()
{
	task_vm_info_data_t information{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	require(task_info(mach_task_self(), TASK_VM_INFO,
			reinterpret_cast<task_info_t>(&information), &count) ==
			KERN_SUCCESS,
		"could not read the size of the process's mappings");
	return information.virtual_size;
}

/* The window of a guest of `guest_mib`: the length the product derives, the
 * RAM, an allowance of the same size held between 1 and 4 GiB, and the fixed
 * span of the shared-memory aperture and the stub pages. Derived, not
 * reserved: the process holds a window it has taken for as long as it lives. */
std::uint64_t window_bytes(std::uint64_t guest_mib)
{
	return uml::runtime::guest_memory::native_window_bytes(guest_mib * mib);
}

int run_with_memory(uml_emulator *emulator, const char *memory)
{
	const char *arguments[]{"uml-test", "--execution-engine=gadgets", memory};
	return uml_emulator_run(emulator, 3, arguments, nullptr);
}

void test_a_refused_heap_or_window_leaves_the_room_a_smaller_guest_needs()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	require(uml::memory::HeapRegion::instance().base() == 0,
		"the process's heap region was reserved before the runs: the test "
		"would not meet its refusal");

	/* The process has room for its heap region and a window of a 1536 MiB
	 * guest and a little more, less than a window of 2176 MiB takes beside
	 * the heap region: the larger guest gets its window and is refused the
	 * heap region, and the window it took must not stay, for the smaller
	 * guest must still be started. */
	const std::uint64_t small_window = window_bytes(1536);
	const std::uint64_t large_window = window_bytes(2176);
	constexpr std::uint64_t margin = 512 * mib;
	require(large_window > small_window + margin,
		"the test's two guests do not differ by more than its margin");
	rlimit limit{};
	require(getrlimit(RLIMIT_AS, &limit) == 0,
		"could not read the address space limit");
	limit.rlim_cur = mapped_bytes() + uml::memory::HeapRegion::reservation() +
		small_window + margin;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not limit the address space");

	/* A refused start leaves the process's window as it was: here none. */
	auto expect_window_as_it_was = [](const uml::runtime::ProcessNativeWindow &was) {
		const auto now = uml::runtime::process_native_window();
		require(now.begin == was.begin && now.end == was.end,
			"a refused run left the window other than it was, in its address "
			"or its size");
	};
	const auto before_first = uml::runtime::process_native_window();
	require(before_first.begin == 0 && before_first.end == 0,
		"the process held a window before the runs");
	require(run_with_memory(emulator.get(), "mem=2176M") ==
			-UML_EMULATOR_OUT_OF_MEMORY,
		"a guest the process has no room for beside its heap region was not "
		"refused with the out-of-memory result");
	expect_window_as_it_was(before_first);
	require(uml::runtime::process_native_window().begin == 0,
		"a refused run kept the window it reserved");
	require(entries == 0, "a guest the process could not hold entered its "
		"kernel");
	require(uml::emulator::start_refused_for_memory(emulator.get()),
		"a run the host refused for want of memory carries no mark that says "
		"so");
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a refused run took the room a smaller guest needed");
	require(!uml::emulator::start_refused_for_memory(emulator.get()),
		"a run that started carries the mark of the refused run before it");
	const auto before_second = uml::runtime::process_native_window();
	require(before_second.begin != 0,
		"the guest that started left no window");
	require(run_with_memory(emulator.get(), "mem=2176M") ==
			-UML_EMULATOR_OUT_OF_MEMORY && entries == 1,
		"the larger guest was started once the smaller one had been");
	expect_window_as_it_was(before_second);
	require(uml::emulator::start_refused_for_memory(emulator.get()),
		"the second refusal carries no mark");
	/* A run that fails for another reason clears what the refusal before it
	 * left: the number a failed allocation of the library's own returns is the
	 * same, and must not read as a refusal of the guest's size. */
	require(uml_emulator_run(emulator.get(), -1, nullptr, nullptr) ==
			-UML_EMULATOR_INVALID_ARGUMENT,
		"a run with a negative argument count was not refused");
	require(!uml::emulator::start_refused_for_memory(emulator.get()),
		"a run that failed for another reason kept the mark of a refusal");
}

/* A process with no room for its heap region refuses every guest, and the
 * window the run reserved for it does not stay in the process. A refusal is
 * not for good: with the room, the same guest starts. */
void test_a_refused_heap_leaves_no_window_behind()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	require(uml::memory::HeapRegion::instance().base() == 0,
		"the process's heap region was reserved before the runs");
	rlimit limit{};
	require(getrlimit(RLIMIT_AS, &limit) == 0,
		"could not read the address space limit");
	limit.rlim_cur = mapped_bytes() + window_bytes(1536) + 512 * mib;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not limit the address space");
	require(run_with_memory(emulator.get(), "mem=1536M") ==
			-UML_EMULATOR_OUT_OF_MEMORY && entries == 0,
		"a process without room for its heap region started a guest");
	require(uml::emulator::start_refused_for_memory(emulator.get()),
		"a refused heap region carries no mark of a refusal for memory");
	require(uml::runtime::process_native_window().begin == 0,
		"a refused heap region left the guest's window in the process");
	require(uml::memory::HeapRegion::instance().base() == 0,
		"a refused heap region was reserved");
	limit.rlim_cur = mapped_bytes() + window_bytes(1536) +
		uml::memory::HeapRegion::reservation() + 512 * mib;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not raise the address space limit");
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a guest refused for want of room for its heap region was not started "
		"once the process had it");
	require(!uml::emulator::start_refused_for_memory(emulator.get()),
		"the run that started carries the mark of the refusal before it");
}

/* The pre-start trial of a guest's RAM is part of every run: a host that maps
 * no RAM where the kernel would ends the run before its kernel, and the next
 * run, tried where the host does map it, is started. The trial is made at an
 * address no process has, for a window the run reserved. */
void test_a_refused_ram_trial_ends_the_run_before_its_kernel()
{
	constexpr std::uintptr_t beyond_the_address_space = 1ULL << 48;
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	uml::emulator::testing::place_guest_ram_trial(beyond_the_address_space);
	require(run_with_memory(emulator.get(), "mem=1536M") ==
			-UML_EMULATOR_OUT_OF_MEMORY && entries == 0,
		"a host that maps no RAM for the guest started it");
	require(uml::emulator::start_refused_for_memory(emulator.get()),
		"a refused RAM trial carries no mark of a refusal for memory");
	uml::emulator::testing::place_guest_ram_trial(0);
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a refused trial spent the process's guest");
}

/* A size the product has no layout for reserves nothing: no heap region, no
 * window. The geometry is checked before anything is reserved. */
void test_a_size_without_a_layout_reserves_nothing()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	require(uml::memory::HeapRegion::instance().base() == 0,
		"the process's heap region was reserved before the runs");
	require(run_with_memory(emulator.get(), "mem=0M") ==
			-UML_EMULATOR_INVALID_ARGUMENT && entries == 0,
		"a guest of no memory was not refused as an invalid argument");
	require(uml::memory::HeapRegion::instance().base() == 0,
		"a run that had no layout reserved the heap region");
	require(uml::runtime::process_native_window().begin == 0,
		"a run that had no layout reserved a window");
	require(!uml::emulator::start_refused_for_memory(emulator.get()),
		"a run with no layout is marked as refused for want of memory");
}

/* Fills every run of free address space below `end` that holds `size` bytes
 * (the host places an anonymous mapping in the lowest one), so that the next
 * mapping of that size is placed at the start of what is free at `end` and
 * below. The blocks stay mapped. */
void fill_free_runs_below(std::uintptr_t end, std::size_t size)
{
	for (;;) {
		void *const block = mmap(nullptr, size, PROT_NONE,
			MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
		if (block == MAP_FAILED)
			return;
		if (reinterpret_cast<std::uintptr_t>(block) >= end) {
			munmap(block, size);
			return;
		}
	}
}

/* The heap region goes where the host places it, and it must not take the
 * address a window needs: where the only room for both is one stretch of
 * address space whose start the heap region would take, the window, which is
 * at an exact address on macOS, is reserved first and the heap region has the
 * rest. The host's placement is the lowest free run that holds the size, so
 * the process below the window's address is filled, and the stretch freed
 * is the window's address with a little below it and room for the heap region
 * after. */
void test_the_heap_region_does_not_take_the_windows_address()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	const std::uintptr_t window = uml::testing::native_test_window_start;
	const std::uint64_t window_length = window_bytes(1536);
	const std::uint64_t heap = uml::memory::HeapRegion::reservation();
	constexpr std::uint64_t gib = std::uint64_t{1} << 30;
	const std::uintptr_t begin = window - gib;
	const std::uintptr_t end = window + window_length + heap + gib;
	fill_free_runs_below(end, 512 * gib);
	fill_free_runs_below(end, 64 * gib);
	fill_free_runs_below(end, heap);
	require(munmap(reinterpret_cast<void *>(begin), end - begin) == 0,
		"could not free the stretch the test leaves");
	/* Where a heap region reserved now would go: into the stretch, at the
	 * address the window needs or below it. */
	void *const probe = mmap(nullptr, heap, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(probe != MAP_FAILED, "the stretch holds no heap region");
	const auto placed = reinterpret_cast<std::uintptr_t>(probe);
	munmap(probe, heap);
	require(placed >= begin && placed < window + window_length,
		"the test did not leave the host one place for a heap region, which "
		"covers the address of the window");
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a guest whose window the address space held was refused for the "
		"heap region that the host would place on it");
	const auto held = uml::runtime::process_native_window();
	require(held.begin == window,
		"the window was not reserved at its address");
	const auto heap_base = uml::memory::HeapRegion::instance().base();
	require(heap_base >= held.end || heap_base + heap <= held.begin,
		"the heap region overlaps the window");
}

/* The refusal belongs to the run of the handle that was refused: a run of
 * another handle, refused or not, changes nothing about it, in either order
 * of their runs. */
void test_the_refusal_belongs_to_the_run_of_its_handle()
{
	const auto first = uml::testing::make_emulator(counting_kernel);
	const auto second = uml::testing::make_emulator(counting_kernel);
	rlimit limit{};
	require(getrlimit(RLIMIT_AS, &limit) == 0,
		"could not read the address space limit");
	limit.rlim_cur = mapped_bytes() + window_bytes(2176) + 512 * mib;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not limit the address space");
	/* The first handle is refused, then the second fails for another reason:
	 * the first keeps what its run was. */
	require(run_with_memory(first.get(), "mem=2176M") ==
			-UML_EMULATOR_OUT_OF_MEMORY,
		"the first guest was not refused");
	require(uml_emulator_run(second.get(), -1, nullptr, nullptr) ==
			-UML_EMULATOR_INVALID_ARGUMENT,
		"a run with a negative argument count was not refused");
	require(uml::emulator::start_refused_for_memory(first.get()),
		"another handle's run took the refusal from the handle that had it");
	require(!uml::emulator::start_refused_for_memory(second.get()),
		"a run that failed for another reason is marked as refused for memory");
	/* The first handle's run fails for another reason, then the second is
	 * refused: the first does not take the refusal. */
	require(uml_emulator_run(first.get(), -1, nullptr, nullptr) ==
			-UML_EMULATOR_INVALID_ARGUMENT,
		"a run with a negative argument count was not refused");
	require(run_with_memory(second.get(), "mem=2176M") ==
			-UML_EMULATOR_OUT_OF_MEMORY,
		"the second guest was not refused");
	require(!uml::emulator::start_refused_for_memory(first.get()),
		"another handle's refusal was taken for a run that had none");
	require(uml::emulator::start_refused_for_memory(second.get()),
		"a refused run carries no mark");
}

constexpr std::uint8_t kept_text_marker = 0x7e;
std::uintptr_t kept_text_address;
std::size_t kept_text_length;

/* The kernel of an informational run that the loader ends with its own
 * failure: it has aliased the first of its signed text, which the host keeps
 * for the life of the process, and cannot place the second. The text is a page
 * of the window, mapped over its reservation as the loader maps it, and 126 is
 * what the loader answers. */
int kernel_that_keeps_text_and_fails(int, char *const[], char *const[])
{
	const auto window = uml::runtime::process_native_window();
	const auto page = static_cast<std::size_t>(getpagesize());
	const std::uintptr_t address = window.begin + 512 * page;
	if (window.begin == 0 || address + page > window.end)
		return 1;
	void *const text = mmap(reinterpret_cast<void *>(address), page,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	if (text != reinterpret_cast<void *>(address))
		return 2;
	*static_cast<std::uint8_t *>(text) = kept_text_marker;
	if (!uml::memory::keep_host_range(text, page))
		return 3;
	kept_text_address = address;
	kept_text_length = page;
	return 126;
}

bool kept_text_is_readable()
{
	std::uint8_t byte = 0;
	vm_size_t size = 0;
	return vm_read_overwrite(mach_task_self(), kept_text_address, 1,
		       reinterpret_cast<vm_address_t>(&byte), &size) ==
		KERN_SUCCESS && size == 1 && byte == kept_text_marker;
}

/* How far the mappings of the process may differ between two readings of an
 * unchanged process: the allocations of the test itself. */
constexpr std::uint64_t mapping_noise = 16 * mib;

bool near_in_size(std::uint64_t before, std::uint64_t after)
{
	return before > after ? before - after <= mapping_noise :
		after - before <= mapping_noise;
}

/* A process that holds the window of a 1536 MiB guest, with the signed text an
 * informational run's load kept in it, and has room for that guest's start
 * and little more (the limit is a margin above its heap region): the state
 * every row below starts from. */
struct WindowWithKeptText {
	uml::testing::Emulator loader{uml::testing::make_emulator(
		kernel_that_keeps_text_and_fails)};
	uml::testing::Emulator starter{uml::testing::make_emulator(counting_kernel)};
	uml::runtime::ProcessNativeWindow window{};
	std::uint64_t mapped{};

	explicit WindowWithKeptText(bool by_the_host)
	{
		require(uml::memory::HeapRegion::instance().base() == 0,
			"the process's heap region was reserved before the runs");
		uml::emulator::testing::place_native_kernel_window_by_the_host(
			by_the_host);
		const char *information[]{"uml-test", "--execution-engine=gadgets",
			"--version", "mem=1536M"};
		require(uml_emulator_run(loader.get(), 4, information, nullptr) == 126,
			"the informational run did not end with the loader's failure");
		require(kept_text_address != 0 && kept_text_is_readable(),
			"the informational run did not keep its text");
		window = uml::runtime::process_native_window();
		require(window.begin != 0 && kept_text_address >= window.begin &&
				kept_text_address < window.end,
			"the informational run's window does not hold its text");
		mapped = mapped_bytes();
		rlimit limit{};
		require(getrlimit(RLIMIT_AS, &limit) == 0,
			"could not read the address space limit");
		limit.rlim_cur = mapped + uml::memory::HeapRegion::reservation() +
			512 * mib;
		require(setrlimit(RLIMIT_AS, &limit) == 0,
			"could not limit the address space");
	}

	/* A start of `memory` that the process has no room for: refused, with the
	 * process's window and its mappings as they were, and the text kept. */
	void refused_larger_start_changes_nothing(const char *memory)
	{
		require(run_with_memory(starter.get(), memory) ==
				-UML_EMULATOR_OUT_OF_MEMORY && entries == 0,
			"a process without room for its heap region started a guest");
		require(uml::emulator::start_refused_for_memory(starter.get()),
			"a refused heap region carries no mark of a refusal for memory");
		const auto held = uml::runtime::process_native_window();
		require(held.begin == window.begin && held.end == window.end,
			"a refused start left the process's window other than it was");
		require(near_in_size(mapped, mapped_bytes()),
			"a refused start left a reservation in the process");
		require(uml::memory::host_range_kept(
				reinterpret_cast<const void *>(kept_text_address),
				kept_text_length),
			"the host forgot the text a load kept");
		require(kept_text_is_readable(),
			"a refused start took the signed text a load had kept out of the "
			"process, which the loader still records as in place");
	}

	/* The guest the process had room for before: started, at the limit as it
	 * was, with its text and its window. */
	void smaller_guest_starts_at_the_same_limit()
	{
		require(run_with_memory(starter.get(), "mem=1536M") == 0 &&
				entries == 1,
			"a guest the process had room for before a refused start was not "
			"started at the limit it had");
		require(!uml::emulator::start_refused_for_memory(starter.get()),
			"the run that started carries the mark of the refusal before it");
		const auto held = uml::runtime::process_native_window();
		require(held.begin == window.begin && held.end == window.end,
			"the guest did not start in the window the process held");
		require(kept_text_is_readable(),
			"the text a load kept was lost to the run that started");
	}
};

/* A start refused its heap region leaves the process's window as it was before
 * the start (THE RULE, runtime/native_kernel_layout.hpp), the signed text a
 * load kept in it included. An informational run is the one that loads a
 * kernel before the heap region is reserved, and the loader can fail it after
 * the first of its text is kept. A larger guest tried first grows the window,
 * or takes a new one where the host places it, and is refused; the guest the
 * process had room for before is then started at the same limit. */
void test_a_refused_larger_start_leaves_the_window_as_it_was(bool by_the_host)
{
	WindowWithKeptText process{by_the_host};
	process.refused_larger_start_changes_nothing("mem=2176M");
	process.smaller_guest_starts_at_the_same_limit();
}

/* Refused starts of ever larger guests, in a row, leave the reserved total
 * unchanged: none keeps what it grew the window by. */
void test_refused_larger_starts_in_a_row_leak_no_growth()
{
	WindowWithKeptText process{false};
	for (const char *memory :
		{"mem=2176M", "mem=2304M", "mem=2432M", "mem=2560M"})
		process.refused_larger_start_changes_nothing(memory);
	process.smaller_guest_starts_at_the_same_limit();
}

/* A window reserved for a larger guest, where the host places it, takes a new
 * place beside the one the process holds. The old window holds text a load
 * kept: it stays mapped when the start commits, and an undo restores it. */
void test_a_larger_window_keeps_the_text_a_load_kept()
{
	const auto loader = uml::testing::make_emulator(
		kernel_that_keeps_text_and_fails);
	const char *information[]{
		"uml-test", "--execution-engine=gadgets", "--version"};
	require(uml_emulator_run(loader.get(), 3, information, nullptr) == 126,
		"the informational run did not end with the loader's failure");
	require(kept_text_address != 0 && kept_text_is_readable(),
		"the informational run did not keep its text");
	const auto old_window = uml::runtime::process_native_window();
	const auto geometry =
		uml::runtime::native_kernel_placement(4096 * mib).geometry;
	require(geometry.valid, "the larger guest has no layout");
	require(geometry.window_end - geometry.window_start >
			old_window.end - old_window.begin,
		"the larger guest's window is not larger than the held one");
	/* The start is refused: the window held before is the process's again. */
	const auto refused = uml::runtime::reserve_process_native_window(
		geometry, uml::runtime::WindowPlacement::anywhere);
	require(refused.valid, "the larger window was not reserved");
	require(uml::runtime::process_native_window().begin != old_window.begin,
		"the larger guest did not take a window of its own");
	uml::runtime::undo_process_window();
	const auto restored = uml::runtime::process_native_window();
	require(restored.begin == old_window.begin &&
			restored.end == old_window.end && kept_text_is_readable(),
		"a refused start did not leave the window and the text it held");
	/* The start commits: the window the process has is the larger, and the old
	 * one, which holds the kept text, stays mapped. */
	require(uml::runtime::reserve_process_native_window(
			geometry, uml::runtime::WindowPlacement::anywhere).valid,
		"the larger window was not reserved again");
	uml::runtime::commit_process_window();
	require(uml::runtime::process_native_window().begin != old_window.begin &&
			kept_text_is_readable(),
		"a window replaced for a larger guest took the signed text a load had "
		"kept in it out of the process");
}

} // namespace

int main(int argc, char **argv)
{
#if defined(UML_NATIVE_TEST_WINDOW_UNDER_TSAN) || \
	defined(UML_NATIVE_TEST_WINDOW_UNDER_ASAN)
	/* A sanitizer holds shadow memory that the limit would count. */
	std::puts("skipped under a sanitizer");
	return EXIT_SUCCESS;
#else
	try {
		uml::emulator::testing::place_native_kernel_window(
			uml::testing::native_test_window_start);
		const std::string_view mode = argc > 1 ? argv[1] : "";
		if (mode == "--no-room-for-the-heap")
			test_a_refused_heap_leaves_no_window_behind();
		else if (mode == "--ram-trial")
			test_a_refused_ram_trial_ends_the_run_before_its_kernel();
		else if (mode == "--no-layout")
			test_a_size_without_a_layout_reserves_nothing();
		else if (mode == "--heap-and-window-addresses")
			test_the_heap_region_does_not_take_the_windows_address();
		else if (mode == "--two-handles")
			test_the_refusal_belongs_to_the_run_of_its_handle();
		else if (mode == "--kept-text")
			test_a_refused_larger_start_leaves_the_window_as_it_was(false);
		else if (mode == "--kept-text-by-the-host")
			test_a_refused_larger_start_leaves_the_window_as_it_was(true);
		else if (mode == "--no-growth-leak")
			test_refused_larger_starts_in_a_row_leak_no_growth();
		else if (mode == "--kept-text-window-replaced")
			test_a_larger_window_keeps_the_text_a_load_kept();
		else
			test_a_refused_heap_or_window_leaves_the_room_a_smaller_guest_needs();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "emulator address space test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
#endif
}
