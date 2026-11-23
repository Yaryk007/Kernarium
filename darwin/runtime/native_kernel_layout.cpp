// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_kernel_layout.hpp"

#include "host_environment.hpp"
#include "memory/domain_heap.hpp"
#include "memory/mmu/address_layout.hpp"
#include "memory/shared_backing.hpp"

#include <cstdio>
#include <mutex>

#include <sys/mman.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

/* What the start under way reserved for itself in the process's window, which
 * an undo gives back (undo_process_window). */
enum class WindowAttempt : std::uint8_t {
	none,
	/* The held window was grown at its end: `before_end` is where it ended. */
	grown,
	/* A new window was taken beside the held one: [before_begin, before_end)
	 * is the window held before, or empty where there was none. It stays
	 * mapped until the start commits. */
	replaced,
};

struct ProcessWindow final {
	std::mutex mutex;
	std::uintptr_t begin{};
	std::uintptr_t end{};
	WindowAttempt attempt{WindowAttempt::none};
	std::uintptr_t before_begin{};
	std::uintptr_t before_end{};
};

/* Never destroyed: a guest thread can still run when the process ends. */
[[clang::no_destroy]] constinit ProcessWindow process_window;

/* The attempt is kept: nothing is given back. A window held before, which the
 * attempt replaced, is released, unless it holds signed kernel text a load
 * kept: no mapping can replace that text on iOS, so the window stays mapped,
 * inert and no longer the process's. The caller holds the mutex. */
void commit_locked() noexcept
{
	auto &window = process_window;
	if (window.attempt == WindowAttempt::replaced &&
	    window.before_end != window.before_begin &&
	    !::uml::memory::host_range_kept_within(
		    reinterpret_cast<const void *>(window.before_begin),
		    window.before_end - window.before_begin))
		munmap(reinterpret_cast<void *>(window.before_begin),
			window.before_end - window.before_begin);
	window.attempt = WindowAttempt::none;
	window.before_begin = 0;
	window.before_end = 0;
}

/* Gives back what the attempt reserved and nothing else, and the window is as
 * it was before it. The caller holds the mutex. */
void undo_locked() noexcept
{
	auto &window = process_window;
	if (window.attempt == WindowAttempt::grown) {
		munmap(reinterpret_cast<void *>(window.before_end),
			window.end - window.before_end);
		window.end = window.before_end;
	} else if (window.attempt == WindowAttempt::replaced) {
		munmap(reinterpret_cast<void *>(window.begin),
			window.end - window.begin);
		window.begin = window.before_begin;
		window.end = window.before_end;
	}
	window.attempt = WindowAttempt::none;
	window.before_begin = 0;
	window.before_end = 0;
}

[[nodiscard]] NativeKernelLayout reserved_layout(std::uintptr_t base,
	std::size_t length) noexcept
{
	return {
		.load_bias = base - DARWIN_UML_NATIVE_LINK_ADDRESS,
		.window_start = base,
		.window_end = base + length,
		.valid = true,
		.reserved = true,
	};
}

/* A window is a multiple of the guest page, the host maps whole host pages,
 * and the reservation is what the host mapped. */
[[nodiscard]] std::size_t host_reservation_length(std::size_t length) noexcept
{
	const auto page = static_cast<std::size_t>(getpagesize());
	return (length + page - 1U) & ~(page - 1U);
}

/* Reserves [end, new_end) where the process's window ends, if the host has it
 * free; nothing is changed otherwise. */
[[nodiscard]] bool extend_reservation(std::uintptr_t end,
	std::uintptr_t new_end) noexcept
{
	const std::size_t length = new_end - end;
	void *const tail = mmap(reinterpret_cast<void *>(end), length, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (tail == MAP_FAILED)
		return false;
	if (reinterpret_cast<std::uintptr_t>(tail) != end) {
		munmap(tail, length);
		return false;
	}
	return true;
}

/* The span at the start of the window that the mapping trial leaves alone: the
 * signed kernel image and the reserved physical prefix after it, which the
 * kernel maps by other calls. The image is below 25 MiB in every accepted
 * kernel; guest RAM is at least 512 MiB.
 *
 * The kernel's own suffix mapping begins earlier than this (at 32 MiB for the
 * Android kernel), so the trial does not try the span from there to this
 * bound: a signed text alias of an earlier guest may lie in the image's span,
 * the trial must not map over it, and the bound is a margin above every
 * accepted image rather than the kernel's own start. The trial promises that
 * the host maps the RAM beyond this bound in one read-write mapping; it does
 * not promise the span below it, the kernel's later metadata allocations, the
 * wiring of the suffix's tail, or the optional alias the kernel logs and
 * continues without. */
constexpr std::size_t guest_ram_trial_skip = 64U << 20;

/* Makes [begin, begin + length) the window's protectionless reservation again:
 * a mapping over it in place frees no address. */
[[nodiscard]] bool hand_back_to_reservation(std::uintptr_t begin,
	std::size_t length) noexcept
{
	void *const reserved = mmap(reinterpret_cast<void *>(begin), length,
		PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	return reserved != MAP_FAILED &&
		reinterpret_cast<std::uintptr_t>(reserved) == begin;
}

} // namespace

NativeKernelPlacement native_kernel_placement(
	std::size_t guest_memory) noexcept
{
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
	return {MacosNativeKernelGeometry::derive_at(
			guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS),
		WindowPlacement::exact};
#else
	/* The same iOS build runs on an Apple silicon Mac in Designed for iPad
	 * mode, where the process is a macOS process: the device window below
	 * the signed iOS envelope is already occupied there, and Darwin answers
	 * the loader's address hint with somewhere else entirely, which the
	 * loader must reject. Placement follows the address space the process
	 * actually has rather than the SDK it was built against.
	 */
	if (::uml::platform::running_ios_app_on_macos()) {
		return {MacosNativeKernelGeometry::derive_at(
				guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS),
			WindowPlacement::exact};
	}
	return {DeviceNativeKernelGeometry::derive(guest_memory),
		WindowPlacement::anywhere};
#endif
}

bool reserve_process_heap() noexcept
{
	if (::uml::memory::HeapRegion::instance().reserve())
		return true;
	std::fprintf(stderr,
		"UML: cannot reserve the %llu-byte host heap region\n",
		static_cast<unsigned long long>(
			::uml::memory::HeapRegion::reservation()));
	return false;
}

bool native_guest_memory_mappable(const NativeKernelLayout &layout,
	std::size_t guest_memory) noexcept
{
	if (!layout.valid || layout.window_end <= layout.window_start ||
	    guest_memory == 0)
		return false;
	const auto page = static_cast<std::uintptr_t>(getpagesize());
	const std::uintptr_t window = layout.window_end - layout.window_start;
	const std::uintptr_t ram =
		(guest_memory + DARWIN_UML_GUEST_PAGE_SIZE - 1U) &
		~static_cast<std::uintptr_t>(DARWIN_UML_GUEST_PAGE_SIZE - 1U);
	if (ram < guest_memory || ram > window)
		return false;
	if (ram <= guest_ram_trial_skip)
		return true;
	/* The host maps whole host pages: the span is the host pages the RAM
	 * covers beyond the skipped prefix. */
	const std::uintptr_t begin =
		(layout.window_start + guest_ram_trial_skip + page - 1U) & ~(page - 1U);
	const std::uintptr_t end = (layout.window_start + ram) & ~(page - 1U);
	if (end <= begin)
		return true;
	const std::size_t length = end - begin;
	void *const mapping = mmap(reinterpret_cast<void *>(begin), length,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	const int error = errno;
	/* Whatever the host answered, the range is the window's reservation
	 * again; a failed mapping may have left it empty. */
	const bool whole = hand_back_to_reservation(begin, length);
	if (mapping == MAP_FAILED || reinterpret_cast<std::uintptr_t>(mapping) != begin) {
		std::fprintf(stderr,
			"UML: the host cannot map %llu bytes of guest RAM at %p, errno=%d\n",
			static_cast<unsigned long long>(length),
			reinterpret_cast<void *>(begin), error);
		return false;
	}
	if (!whole) {
		std::fprintf(stderr,
			"UML: the guest RAM trial left %llu bytes of the window mapped\n",
			static_cast<unsigned long long>(length));
		return false;
	}
	return true;
}

NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
	const auto product = native_kernel_placement(guest_memory);
	if (!product.geometry.valid)
		return product.geometry;
	const auto layout = reserve_process_native_window(
		product.geometry, product.placement);
	if (!layout.valid) {
		std::fprintf(stderr,
			"UML: cannot reserve a %llu-byte native kernel window\n",
			static_cast<unsigned long long>(
				product.geometry.window_end -
				product.geometry.window_start));
		return {.reservation_failed = true};
	}
	return layout;
}

NativeKernelLayout reserve_process_native_window(
	const NativeKernelLayout &geometry, WindowPlacement placement) noexcept
{
	if (!geometry.valid || geometry.window_end <= geometry.window_start)
		return {};
	const std::size_t length = static_cast<std::size_t>(
		geometry.window_end - geometry.window_start);
	std::lock_guard lock{process_window.mutex};
	/* A start that asks again without having settled its attempt has kept it. */
	commit_locked();
	const bool held = process_window.end != 0;
	/* A window the host places is the window wherever it is; one at an exact
	 * address must be at that address. */
	const bool fits_place = held &&
		(placement == WindowPlacement::anywhere ||
		 process_window.begin == geometry.window_start);
	if (fits_place &&
	    process_window.end - process_window.begin >= length)
		return reserved_layout(process_window.begin, length);
	if (fits_place && placement == WindowPlacement::exact) {
		const std::uintptr_t old_end = process_window.end;
		const std::uintptr_t new_end = process_window.begin +
			host_reservation_length(length);
		if (!extend_reservation(old_end, new_end))
			return {};
		process_window.end = new_end;
		process_window.attempt = WindowAttempt::grown;
		process_window.before_end = old_end;
		return reserved_layout(process_window.begin, length);
	}
	const auto window = reserve_native_kernel_window(geometry, placement);
	if (!window.valid)
		return {};
	/* The new window is held beside the old, which stays mapped and recorded
	 * until the start commits: a start that is refused has the old window
	 * back, whole (undo_process_window). */
	process_window.attempt = WindowAttempt::replaced;
	process_window.before_begin = process_window.begin;
	process_window.before_end = process_window.end;
	process_window.begin = window.window_start;
	process_window.end = window.window_start + host_reservation_length(length);
	return window;
}

void commit_process_window() noexcept
{
	std::lock_guard lock{process_window.mutex};
	commit_locked();
}

void undo_process_window() noexcept
{
	std::lock_guard lock{process_window.mutex};
	undo_locked();
}

ProcessNativeWindow process_native_window() noexcept
{
	std::lock_guard lock{process_window.mutex};
	return {process_window.begin, process_window.end};
}

NativeKernelLayout reserve_native_kernel_window(
	const NativeKernelLayout &geometry, WindowPlacement placement) noexcept
{
	if (!geometry.valid || geometry.window_end <= geometry.window_start)
		return {};
	const std::size_t length = static_cast<std::size_t>(
		geometry.window_end - geometry.window_start);
	/* Reserve the window before the guest is brought up so nothing else can
	 * take part of it. The address the geometry names is tried first, so a
	 * host that still offers it keeps the established layout. iOS 27 does
	 * not: an app's usable virtual range moved, no hint inside the
	 * compile-time envelope is honoured any more, and the kernel image would
	 * be placed outside the window. Such a host is asked where a window of
	 * this size fits instead of being told. A macOS kernel is built for its
	 * address and gets no other.
	 *
	 * Nothing inside the window is released while it lives, not even for
	 * the kernel image: Darwin prefers a freshly freed range for its next
	 * unhinted allocation, and a host allocation that landed there would be
	 * replaced by the next MAP_FIXED guest mapping without either side being
	 * told. The loader and guest RAM map over the reservation in place.
	 */
	const std::size_t reservation = host_reservation_length(length);
	void *reserved = mmap(reinterpret_cast<void *>(geometry.window_start),
		reservation, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1,
		0);
	if (reserved != MAP_FAILED &&
	    reinterpret_cast<std::uintptr_t>(reserved) != geometry.window_start) {
		munmap(reserved, reservation);
		reserved = placement == WindowPlacement::exact ? MAP_FAILED :
			mmap(nullptr, reservation, PROT_NONE,
				MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	}
	if (reserved == MAP_FAILED)
		return {};
	const auto base = reinterpret_cast<std::uintptr_t>(reserved);
	if ((base & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)) != 0U ||
	    base < DARWIN_UML_NATIVE_LINK_ADDRESS) {
		munmap(reserved, reservation);
		return {};
	}
	return reserved_layout(base, length);
}

NativeKernelLayoutScope::NativeKernelLayoutScope(
	const NativeKernelLayout &layout) noexcept
{
	if (!layout.valid)
		return;
	entered_ = ::uml::memory::configure_native_address_window(
		layout.window_start, layout.window_end);
}

NativeKernelLayoutScope::~NativeKernelLayoutScope()
{
	if (entered_)
		::uml::memory::reset_native_address_window();
}

} // namespace uml::runtime
