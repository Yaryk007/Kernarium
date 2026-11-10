// SPDX-License-Identifier: GPL-2.0-or-later
#include "executable_memory.hpp"

#include <sys/mman.h>

#include <libkern/OSCacheControl.h>
#include <os/lock.h>
#include <pthread.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
#include <mach/mach.h>
#include <mach/vm_map.h>

/* csops and its flags are XNU interfaces present on iOS but omitted from the
 * public device SDK headers. This sideload-only build uses status query 0 and
 * CS_DEBUGGED from xnu/bsd/sys/codesign.h; it does not mutate code-signing
 * state.
 */
extern "C" int csops(pid_t, unsigned int, void *, std::size_t);
constexpr unsigned int jit_cs_ops_status = 0;
constexpr std::uint32_t jit_cs_debugged = 0x10000000U;
#endif

namespace uml::cpu::jit_memory {
namespace {

/* Publication route for MAP_JIT memory.
 *
 * macOS declares pthread_jit_write_protect_np and calls it directly. Physical
 * devices normally cannot publish MAP_JIT memory, so JIT reports itself
 * unavailable there. UML_ENABLE_IOS_STIKDEBUG_JIT instead uses an RX mapping
 * prepared by StikDebug and writes it through a separate RW alias.
 *
 * The Simulator is deliberately held to the same answer even though it runs on
 * macOS and could reach the toggle dynamically. Letting it translate through a
 * path production devices cannot use would make the Simulator unrepresentative
 * of the product it exists to stand in for.
 */
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT) && \
	!defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
void jit_write_protect(int enable)
{
	pthread_jit_write_protect_np(enable);
}

constexpr bool jit_publication_supported() noexcept
{
	return true;
}
#elif defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
constexpr void jit_write_protect(int) {}

constexpr bool jit_publication_supported() noexcept
{
	return true;
}
#else
constexpr void jit_write_protect(int) {}

constexpr bool jit_publication_supported() noexcept
{
	return false;
}
#endif

#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
/* StikDebug universal.js protocol. x0 and x1 carry the normal function
 * arguments; x16 selects prepare-region (1) or detach (0). Calling either
 * breakpoint without universal.js attached is a configuration error and may
 * stop the process, so available() first requires CS_DEBUGGED.
 */
extern "C" __attribute__((noinline, optnone, naked))
void *uml_stikdebug_prepare_region(void *, std::size_t)
{
	__asm__("mov x16, #1\n"
	        "brk #0xf00d\n"
	        "ret\n");
}

extern "C" __attribute__((noinline, optnone, naked))
void uml_stikdebug_detach()
{
	__asm__("mov x16, #0\n"
	        "brk #0xf00d\n"
	        "ret\n");
}

[[nodiscard]] bool debugger_attached() noexcept
{
	std::uint32_t status = 0;
	return csops(getpid(), jit_cs_ops_status, &status, sizeof(status)) == 0 &&
	       (status & jit_cs_debugged) != 0;
}
#endif

/* Where JIT code lives.
 *
 * An Apple core predicts an indirect branch's target cheaply only when the
 * target lies near the branch. On an M1 Ultra a call through a register and
 * its return cost about 0.7 ns when caller and callee share an aligned
 * 512 MiB block, 1.9 ns when they share an aligned 4 GiB block, and 5 ns or
 * more when they do not. Generated code calls into this image -- FEXCore
 * reaches its x87 softfloat fallbacks that way for every x87 operation -- and
 * jumps between its own allocations, so where a mapping lands decides how fast
 * the code in it runs.
 *
 * The kernel places a mapping at the lowest free range. The 2 GiB between this
 * image and the shared cache fill with the JIT's own data, so a code buffer
 * mapped later in a run landed some 400 GiB away, and the guest process it
 * served ran x87 code a third slower than its neighbours.
 *
 * So the process claims one window, the largest, up to 1.5 GiB in 64 MiB
 * steps, whose lowest free range at or above this image's 512 MiB block lies
 * in its 4 GiB block, and serves every allocation from it, lowest address
 * first. It claims the window when it first asks whether it can run JIT
 * code, which it does when it selects a JIT engine, before its kernel boots:
 * by the first allocation the range is already scattered with small
 * mappings, and the JIT's data reservations claim the rest of it as soon as
 * the first contexts exist. FEXCore keeps a 16 MiB code buffer for each of
 * the eight address spaces each vCPU caches, so ten vCPUs keep 80 of them.
 *
 * MAP_JIT cannot map over an existing range, and a hole in a live window
 * would be the kernel's first choice for an unrelated mapping, so a release
 * keeps the pages mapped and marks them reusable; memory served again may
 * still hold what it held before. When the window is full, or cannot be
 * placed, a mapping goes wherever the kernel puts it.
 */
class Window final {
public:
	void claim() noexcept
	{
		os_unfair_lock_lock(&lock_);
		claim_locked();
		os_unfair_lock_unlock(&lock_);
	}

	[[nodiscard]] void *allocate(std::size_t size) noexcept
	{
		if (size == 0 || size > max_bytes)
			return nullptr;
		const auto count = granules_for(size);
		os_unfair_lock_lock(&lock_);
		claim_locked();
		const auto first = find(count);
		if (first != none)
			mark(first, count, true);
		os_unfair_lock_unlock(&lock_);
		if (first == none)
			return nullptr;
		void *mapping = base_ + first * granule;
		/* A released range is reusable: the kernel may discard its pages
		 * until it is claimed back. */
		(void)madvise(mapping, count * granule, MADV_FREE_REUSE);
		return mapping;
	}

#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
	/* iOS 26's trusted-execution monitor does not make a caller-created
	 * mapping executable. Ask universal.js/debugserver to create the RX
	 * mapping, then make a second view of the same VM object for writes.
	 */
	[[nodiscard]] bool prepare_ios() noexcept
	{
		os_unfair_lock_lock(&lock_);
		if (claimed_) {
			const bool ready = base_ != nullptr && writable_base_ != nullptr;
			os_unfair_lock_unlock(&lock_);
			return ready;
		}
		claimed_ = true;
		os_unfair_lock_unlock(&lock_);

		void *const executable =
			uml_stikdebug_prepare_region(nullptr, ios_window_bytes);
		if (executable == nullptr)
			return false;
		vm_address_t writable = 0;
		vm_prot_t current = VM_PROT_NONE;
		vm_prot_t maximum = VM_PROT_NONE;
		const auto executable_address = reinterpret_cast<vm_address_t>(
			executable);
		kern_return_t result = vm_remap(mach_task_self(), &writable,
			ios_window_bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
			executable_address, FALSE, &current, &maximum,
			VM_INHERIT_NONE);
		if (result == KERN_SUCCESS)
			result = vm_protect(mach_task_self(), writable,
				ios_window_bytes, FALSE, VM_PROT_READ | VM_PROT_WRITE);
		if (result != KERN_SUCCESS) {
			if (writable != 0)
				(void)vm_deallocate(
					mach_task_self(), writable, ios_window_bytes);
			(void)vm_deallocate(mach_task_self(), executable_address,
				ios_window_bytes);
			return false;
		}

		os_unfair_lock_lock(&lock_);
		base_ = static_cast<std::byte *>(executable);
		writable_base_ = reinterpret_cast<std::byte *>(writable);
		granules_ = ios_window_bytes / granule;
		os_unfair_lock_unlock(&lock_);
		return true;
	}

	[[nodiscard]] void *writable_alias(void *executable) const noexcept
	{
		auto *const address = static_cast<std::byte *>(executable);
		if (base_ == nullptr || writable_base_ == nullptr || address < base_ ||
		    address >= base_ + granules_ * granule)
			return nullptr;
		return writable_base_ + (address - base_);
	}
#endif

	/* Whether mapping came from the window; if so its pages return to it. */
	[[nodiscard]] bool release(void *mapping, std::size_t size) noexcept
	{
		os_unfair_lock_lock(&lock_);
		std::byte *const base = base_;
		const std::size_t granules = granules_;
		os_unfair_lock_unlock(&lock_);
		auto *const bytes = static_cast<std::byte *>(mapping);
		if (base == nullptr || bytes < base ||
		    bytes >= base + granules * granule)
			return false;
		const auto first =
			static_cast<std::size_t>(bytes - base) / granule;
		const auto count = granules_for(size);
		if (count > granules - first)
			return true;
		/* Marked reusable before it is free, so no later claim can
		 * race the advice. */
		(void)madvise(mapping, count * granule, MADV_FREE_REUSABLE);
		os_unfair_lock_lock(&lock_);
		mark(first, count, false);
		os_unfair_lock_unlock(&lock_);
		return true;
	}

	[[nodiscard]] void *mapping() const noexcept
	{
		return base_;
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		return granules_ * granule;
	}

private:
	/* The host page on every Apple arm64 product. */
	static constexpr std::size_t granule = 16U * 1024U;
	static constexpr std::size_t step_bytes = 64U * 1024U * 1024U;
	static constexpr std::size_t max_bytes = 24U * step_bytes;
#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
	/* Four production-size executor caches plus their auxiliary arenas fit in
	 * 768 MiB. Keeping the prepared region bounded also keeps universal.js's
	 * page preparation time practical on a phone.
	 */
	static constexpr std::size_t ios_window_bytes = 12U * step_bytes;
#endif
	static constexpr std::uintptr_t prediction_block = std::uintptr_t{1} << 29U;
	static constexpr std::uintptr_t near_block = std::uintptr_t{1} << 32U;
	static constexpr std::size_t none = ~std::size_t{0};

	[[nodiscard]] static constexpr std::size_t granules_for(
		std::size_t size) noexcept
	{
		return (size + granule - 1U) / granule;
	}

	void claim_locked() noexcept
	{
		if (claimed_)
			return;
		claimed_ = true;
		if (static_cast<std::size_t>(getpagesize()) > granule)
			return;
		/* Any function of this image locates it. */
		const auto image = reinterpret_cast<std::uintptr_t>(
			&::uml::cpu::jit_memory::allocate);
		const auto hint = image & ~(prediction_block - 1U);
		const auto block = image & ~(near_block - 1U);
		for (auto bytes = max_bytes; bytes != 0; bytes -= step_bytes) {
			void *mapping = mmap(reinterpret_cast<void *>(hint), bytes,
				PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
			if (mapping == MAP_FAILED)
				continue;
			const auto start = reinterpret_cast<std::uintptr_t>(mapping);
			if (start >= block && start - block <= near_block - bytes) {
				base_ = static_cast<std::byte *>(mapping);
				granules_ = bytes / granule;
				return;
			}
			(void)munmap(mapping, bytes);
		}
	}

	[[nodiscard]] std::size_t find(std::size_t count) const noexcept
	{
		std::size_t run = 0;
		for (std::size_t i = 0; i < granules_;) {
			const auto word = used_[i / 64U];
			if (i % 64U == 0 && word == ~std::uint64_t{0}) {
				run = 0;
				i += 64U;
				continue;
			}
			if (((word >> (i % 64U)) & 1U) != 0)
				run = 0;
			else if (++run == count)
				return i + 1U - count;
			++i;
		}
		return none;
	}

	void mark(std::size_t first, std::size_t count, bool used) noexcept
	{
		for (auto i = first; i < first + count; ++i) {
			const auto bit = std::uint64_t{1} << (i % 64U);
			if (used)
				used_[i / 64U] |= bit;
			else
				used_[i / 64U] &= ~bit;
		}
	}

	os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	bool claimed_{};
	std::byte *base_{};
#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
	std::byte *writable_base_{};
#endif
	std::size_t granules_{};
	std::array<std::uint64_t, max_bytes / granule / 64U> used_{};
};

constinit Window window;

} // namespace

bool available() noexcept
{
	static const bool supported = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
		/* A successful MAP_JIT mapping is necessary but not sufficient:
		 * without a W^X toggle the first store into it faults. Requiring
		 * the publication route keeps this from promising an engine that
		 * would abort on its first translated block.
		 */
		if (!jit_publication_supported())
			return false;
#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
		/* universal.js/debugserver creates the executable view. A remapped
		 * writable view of the same VM object receives all generated bytes.
		 * The window is the only iOS executable allocation, so the debugger
		 * can detach once both views exist.
		 */
		if (!debugger_attached())
			return false;
		const bool prepared = window.prepare_ios();
		uml_stikdebug_detach();
		return prepared;
#else
		void *mapping = mmap(nullptr, 16U * 1024U,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (mapping == MAP_FAILED)
			return false;
		(void)munmap(mapping, 16U * 1024U);
		window.claim();
		return true;
#endif
#else
		return false;
#endif
	}();
	return supported;
}

void *allocate(std::size_t size) noexcept
{
	if constexpr (jit_publication_supported()) {
		if (void *mapping = window.allocate(size); mapping != nullptr)
			return mapping;
	}
#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
	/* universal.js has detached, so a later mapping cannot be prepared. */
	return nullptr;
#else
	void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	return mapping == MAP_FAILED ? nullptr : mapping;
#endif
}

void *writable_alias(void *executable_address) noexcept
{
#if defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
	return window.writable_alias(executable_address);
#else
	return executable_address;
#endif
}

void release(void *mapping, std::size_t size)
{
	if constexpr (jit_publication_supported()) {
		if (window.release(mapping, size))
			return;
	}
#if !defined(UML_ENABLE_IOS_STIKDEBUG_JIT)
	(void)munmap(mapping, size);
#else
	(void)mapping;
	(void)size;
#endif
}

void set_writable(bool writable)
{
	jit_write_protect(writable ? 0 : 1);
}

void invalidate_instruction_cache(void *code, std::size_t size)
{
	sys_icache_invalidate(code, size);
}

void *reserve_guarded(std::size_t size) noexcept
{
	const auto guard = static_cast<std::size_t>(getpagesize());
	if (size == 0 || size % guard != 0 || size > SIZE_MAX - 2 * guard)
		return nullptr;
	void *mapping = mmap(nullptr, size + 2 * guard, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapping == MAP_FAILED)
		return nullptr;
	auto *data = static_cast<std::byte *>(mapping) + guard;
	if (mprotect(data, size, PROT_READ | PROT_WRITE) != 0) {
		(void)munmap(mapping, size + 2 * guard);
		return nullptr;
	}
	return data;
}

void release_guarded(void *data, std::size_t size)
{
	const auto guard = static_cast<std::size_t>(getpagesize());
	(void)munmap(static_cast<std::byte *>(data) - guard, size + 2 * guard);
}

} // namespace uml::cpu::jit_memory
