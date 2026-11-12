// SPDX-License-Identifier: GPL-2.0-or-later
#include "executable_memory.hpp"

#include "granule_set.hpp"

#include <sys/mman.h>

#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <os/lock.h>
#include <pthread.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>

/* Which route this object takes to JIT memory.
 *
 * UML_JIT_MEMORY_REGION_ROUTE selects the debugger-prepared region of an iOS
 * device. The build defines it for the iOS device product and for the unit
 * test variant that exercises the region without a debugger
 * (UML_JIT_MEMORY_REGION_TEST); it never defines it for an App Store runtime,
 * which does not link this object at all, and the check below refuses the
 * combination.
 */
#ifndef UML_JIT_MEMORY_REGION_ROUTE
#define UML_JIT_MEMORY_REGION_ROUTE 0
#endif
#ifndef UML_JIT_MEMORY_REGION_TEST
#define UML_JIT_MEMORY_REGION_TEST 0
#endif
#if UML_JIT_MEMORY_REGION_ROUTE && defined(UML_APP_STORE_RUNTIME)
#error "an App Store runtime has no debugger-prepared JIT memory"
#endif
#if UML_JIT_MEMORY_REGION_ROUTE && !UML_JIT_MEMORY_REGION_TEST && \
	(!TARGET_OS_IOS || TARGET_OS_SIMULATOR)
#error "debugger-prepared JIT memory exists on an iOS device only"
#endif

#if UML_JIT_MEMORY_REGION_TEST
#include "executable_memory_testing.hpp"
#endif

#if UML_JIT_MEMORY_REGION_ROUTE
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#if !UML_JIT_MEMORY_REGION_TEST
/* The code-signing status of a process: the only way to read CS_KILL. */
extern "C" int csops(pid_t pid, unsigned int operation, void *buffer,
	std::size_t size);
#endif
#endif

namespace uml::cpu::jit_memory {
namespace {

/* Publication route for MAP_JIT memory.
 *
 * macOS declares pthread_jit_write_protect_np and calls it directly. An iOS
 * device cannot obtain the entitlement, so without the debugger-prepared
 * region (UML_JIT_MEMORY_REGION_ROUTE) its JIT memory reports itself
 * unavailable rather than mapping memory it could never publish into.
 *
 * The Simulator is deliberately held to the same answer even though it runs on
 * macOS and could reach the toggle dynamically. Letting it translate through a
 * path production devices cannot use would make the Simulator unrepresentative
 * of the product it exists to stand in for.
 */
#if UML_JIT_MEMORY_REGION_ROUTE
/* The region's pages are written through their alias: no write window. */
constexpr void jit_write_protect(int) {}
#elif defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT) && \
	!defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
void jit_write_protect(int enable)
{
	pthread_jit_write_protect_np(enable);
}

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

#if !UML_JIT_MEMORY_REGION_ROUTE

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
		const auto first = used_.find(count, granules_);
		if (first != Set::none)
			used_.mark(first, count, true);
		os_unfair_lock_unlock(&lock_);
		if (first == Set::none)
			return nullptr;
		void *mapping = base_ + first * granule;
		/* A released range is reusable: the kernel may discard its pages
		 * until it is claimed back. */
		(void)madvise(mapping, count * granule, MADV_FREE_REUSE);
		return mapping;
	}

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
		used_.mark(first, count, false);
		os_unfair_lock_unlock(&lock_);
		return true;
	}

private:
	/* The host page on every Apple arm64 product. */
	static constexpr std::size_t granule = 16U * 1024U;
	static constexpr std::size_t step_bytes = 64U * 1024U * 1024U;
	static constexpr std::size_t max_bytes = 24U * step_bytes;
	static constexpr std::uintptr_t prediction_block = std::uintptr_t{1} << 29U;
	static constexpr std::uintptr_t near_block = std::uintptr_t{1} << 32U;
	using Set = GranuleSet<max_bytes / granule>;

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

	os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	bool claimed_{};
	std::byte *base_{};
	std::size_t granules_{};
	Set used_{};
};

constinit Window window;

#endif // !UML_JIT_MEMORY_REGION_ROUTE

#if UML_JIT_MEMORY_REGION_ROUTE

/* JIT memory on an iOS device, which no entitlement lets the process map.
 *
 * A debugger the user attached to the process (StikDebug's script, answering a
 * breakpoint) can make pages executable that the system would otherwise kill
 * the process for running. The protocol, in the order the process walks it:
 *
 *   1. map one region read-execute, so the pages exist executable before the
 *      debugger touches them;
 *   2. map the same pages again, writable, before they are blessed, so a page
 *      the JIT uses costs one page of memory and not two;
 *   3. execute `brk #0xf00d` with the command in x16 (1 prepares a region, 0
 *      detaches), the region's address in x0 and its length in x1. The
 *      debugger writes one byte into every 16 KiB page, which makes the
 *      kernel accept code there, and resumes after the breakpoint;
 *   4. send the detach command: while attached, every fault of the process
 *      stops in the debugger, which would erase the JIT's gain;
 *   5. wire the pages through the writable mapping, because whether a blessing
 *      survives the kernel reclaiming or compressing a page is not known;
 *   6. write a stub through the alias and call it through the executable
 *      mapping, so a region that does not run fails here and not in a guest.
 *
 * The debugger reads x0 and x1 as 40 bits, so the region lies below 1 TiB.
 * The process is released from its CS_KILL flag while a debugger is attached
 * and stays released after it leaves, so a bad page raises SIGBUS here and
 * the check can catch it. A debugger that stops the process stops every
 * thread; the bound on the request guards a debugger that stops one.
 */
constexpr std::size_t region_granule = 16U * 1024U;
/* The most one region holds. The JIT's budget is a few hundred MiB. */
constexpr std::size_t region_limit = 1024U * 1024U * 1024U;
constexpr std::uintptr_t region_address_limit = std::uintptr_t{1} << 40U;
constexpr std::uintptr_t region_prediction_block = std::uintptr_t{1} << 29U;
constexpr auto debugger_bound = std::chrono::minutes{10};

/* `brk #0xf00d`: the word a request executes. */
constexpr std::uint32_t request_word = 0xd43e01a0U;

#if UML_JIT_MEMORY_REGION_TEST
/* What the host test steers; see executable_memory_testing.hpp. */
std::atomic<bool> test_sends_requests{false};
std::atomic<bool (*)() noexcept> test_traced_probe{nullptr};
#endif

/* How a request to the debugger came back. */
enum class Reply : std::uint8_t {
	/* The debugger resumed the process past the breakpoint. */
	answered,
	/* No debugger was there: none was attached when the request was to be
	 * sent, or the breakpoint was not resumed and none is attached now. */
	no_debugger,
	/* The breakpoint was not resumed within the bound. The thread stays
	 * stopped wherever the debugger left it, and a debugger that answers
	 * late may still act. */
	timed_out,
	/* The breakpoint was not resumed, and a debugger is still attached. */
	unanswered,
};

/* One request to the debugger. The breakpoint instruction is the protocol's
 * (`brk #0xf00d`). A process nothing services it in is ended by SIGTRAP, so
 * callers send it only to a traced process and from a thread the trap guard
 * below covers. */
[[gnu::noinline]] void debugger_request(std::uint64_t command,
	std::uint64_t address, std::uint64_t length) noexcept
{
	register std::uint64_t x0 __asm__("x0") = address;
	register std::uint64_t x1 __asm__("x1") = length;
	register std::uint64_t x16 __asm__("x16") = command;
	__asm__ volatile("brk #0xf00d" : "+r"(x0), "+r"(x1) : "r"(x16) : "memory");
}

[[nodiscard]] bool traced() noexcept
{
#if UML_JIT_MEMORY_REGION_TEST
	const auto probe = test_traced_probe.load();
	return probe == nullptr || probe();
#else
	struct kinfo_proc information {};
	std::size_t length = sizeof information;
	int name[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
	if (sysctl(name, 4, &information, &length, nullptr, 0) != 0)
		return false;
	return (information.kp_proc.p_flag & P_TRACED) != 0;
#endif
}

/* The trap guard.
 *
 * A debugger can vanish while a request waits for it: StikDebug, which the
 * system ends when it spins in the background, leaves its debugserver holding
 * the process traced until it notices the closed connection. The breakpoint
 * then goes unanswered, and when the debugserver finally lets go the system
 * turns it into SIGTRAP, which ends the process. Reading P_TRACED before the
 * request cannot see that, so the process answers the signal itself: the
 * handler steps over a `brk #0xf00d` and counts it, and the thread that sent
 * the request reads the count to learn it was never resumed.
 *
 * The handler stays for the life of the process, and any SIGTRAP that is not
 * this one goes on to the disposition that was there before it: a crash
 * report still sees a genuine trap. SIGTRAP is a synchronous signal, so it is
 * delivered to the thread that executed the instruction and the thread must
 * not block it (a blocked fault signal makes its thread re-execute the
 * instruction forever); the request thread unblocks it. The count is shared
 * by all requests: a trap taken by another stopped request makes this one read
 * unanswered, never the reverse.
 */
std::atomic<unsigned> unanswered_requests{};
struct sigaction previous_trap {};

void on_request_trap(int signal_number, siginfo_t *information, void *context)
{
	auto *state = static_cast<ucontext_t *>(context);
	const auto pc = static_cast<std::uintptr_t>(
		arm_thread_state64_get_pc(state->uc_mcontext->__ss));
	if (pc != 0 && *reinterpret_cast<const std::uint32_t *>(pc) == request_word) {
		unanswered_requests.fetch_add(1, std::memory_order_acq_rel);
		arm_thread_state64_set_pc_fptr(state->uc_mcontext->__ss,
			reinterpret_cast<void (*)()>(pc + sizeof(std::uint32_t)));
		return;
	}
	if ((previous_trap.sa_flags & SA_SIGINFO) != 0 &&
	    previous_trap.sa_sigaction != nullptr) {
		previous_trap.sa_sigaction(signal_number, information, context);
	} else if ((previous_trap.sa_flags & SA_SIGINFO) == 0 &&
		   previous_trap.sa_handler != SIG_DFL &&
		   previous_trap.sa_handler != SIG_IGN) {
		previous_trap.sa_handler(signal_number);
	} else {
		/* The instruction runs again under the disposition it had. */
		(void)sigaction(SIGTRAP, &previous_trap, nullptr);
	}
}

/* Makes the guard the process's SIGTRAP disposition, whatever else set one
 * since the last request, and says whether it is. */
[[nodiscard]] bool arm_trap_guard() noexcept
{
	struct sigaction current {};
	if (sigaction(SIGTRAP, nullptr, &current) != 0)
		return false;
	if ((current.sa_flags & SA_SIGINFO) != 0 &&
	    current.sa_sigaction == on_request_trap)
		return true;
	struct sigaction guard {};
	guard.sa_sigaction = on_request_trap;
	guard.sa_flags = SA_SIGINFO;
	sigemptyset(&guard.sa_mask);
	previous_trap = current;
	return sigaction(SIGTRAP, &guard, nullptr) == 0;
}

struct Answer final {
	std::mutex mutex;
	std::condition_variable condition;
	bool done{};
	Reply reply{Reply::answered};
};

/* The request's body, on the thread that waits at the breakpoint. */
[[nodiscard]] Reply send_request(std::uint64_t command, std::uint64_t address,
	std::uint64_t length) noexcept
{
	sigset_t trap;
	sigemptyset(&trap);
	sigaddset(&trap, SIGTRAP);
	(void)pthread_sigmask(SIG_UNBLOCK, &trap, nullptr);
	/* The last look before the breakpoint: it is only for a process a
	 * debugger holds. */
	if (!traced())
		return Reply::no_debugger;
	const auto before = unanswered_requests.load(std::memory_order_acquire);
	debugger_request(command, address, length);
	if (unanswered_requests.load(std::memory_order_acquire) == before)
		return Reply::answered;
	return traced() ? Reply::unanswered : Reply::no_debugger;
}

/* Sends a request from a thread of its own and waits for the debugger, up to
 * the bound. The thread outlives a timeout, stopped wherever the debugger left
 * it. */
[[nodiscard]] Reply ask_debugger(std::uint64_t command, std::uint64_t address,
	std::uint64_t length) noexcept
{
#if UML_JIT_MEMORY_REGION_TEST
	if (!test_sends_requests.load())
		return Reply::answered;
#endif
	if (!traced())
		return Reply::no_debugger;
	if (!arm_trap_guard())
		return Reply::unanswered;
	try {
		auto answer = std::make_shared<Answer>();
		std::thread{[answer, command, address, length] {
			const auto reply = send_request(command, address, length);
			{
				std::lock_guard lock{answer->mutex};
				answer->reply = reply;
				answer->done = true;
			}
			answer->condition.notify_all();
		}}.detach();
		std::unique_lock lock{answer->mutex};
		if (!answer->condition.wait_for(
			    lock, debugger_bound, [&] { return answer->done; }))
			return Reply::timed_out;
		return answer->reply;
	} catch (...) {
		return Reply::unanswered;
	}
}

/* The process may run a bad page into SIGBUS and not into SIGKILL. */
[[nodiscard]] bool kill_cleared() noexcept
{
#if UML_JIT_MEMORY_REGION_TEST
	return true;
#else
	constexpr unsigned status_operation = 0;
	constexpr std::uint32_t kill_flag = 0x00000200U;
	std::uint32_t flags = 0;
	if (csops(getpid(), status_operation, &flags, sizeof flags) != 0)
		return false;
	return (flags & kill_flag) == 0;
#endif
}

sigjmp_buf fault_jump;

void on_fault(int, siginfo_t *, void *)
{
	siglongjmp(fault_jump, 1);
}

/* Runs `stub` (a function of no arguments that returns a 32-bit value) and
 * reports whether it returned `expected`, answering a fault in it with false.
 * Callers hold the preparation lock, which is what makes the one jump buffer
 * safe. */
[[nodiscard]] bool runs(void *stub, std::uint32_t expected) noexcept
{
	struct sigaction action {};
	action.sa_sigaction = on_fault;
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	struct sigaction previous_bus {};
	struct sigaction previous_segv {};
	struct sigaction previous_ill {};
	(void)sigaction(SIGBUS, &action, &previous_bus);
	(void)sigaction(SIGSEGV, &action, &previous_segv);
	(void)sigaction(SIGILL, &action, &previous_ill);
	volatile bool ok = false;
	if (sigsetjmp(fault_jump, 1) == 0)
		ok = reinterpret_cast<std::uint32_t (*)()>(stub)() == expected;
	(void)sigaction(SIGBUS, &previous_bus, nullptr);
	(void)sigaction(SIGSEGV, &previous_segv, nullptr);
	(void)sigaction(SIGILL, &previous_ill, nullptr);
	return ok;
}

/* One line on stderr for each step of the preparation that fails: the caller
 * sees only that the memory is not ready, and a device offers no other way to
 * tell which step it was. */
void note(const char *step, bool with_errno = false) noexcept
{
	if (with_errno)
		std::fprintf(stderr, "Kernarium: JIT memory: %s (errno %d)\n",
			step, errno);
	else
		std::fprintf(stderr, "Kernarium: JIT memory: %s\n", step);
}

class Region final {
public:
	[[nodiscard]] bool ready() const noexcept
	{
		return ready_.load(std::memory_order_acquire);
	}

	[[nodiscard]] std::size_t bytes() const noexcept
	{
		return ready() ? granules_ * region_granule : 0;
	}

	[[nodiscard]] RegionStatus prepare(std::size_t wanted) noexcept
	{
		std::lock_guard lock{prepare_mutex_};
		const auto granules = granules_for(wanted);
		if (wanted == 0 || granules * region_granule > region_limit)
			return RegionStatus::failed;
		if (ready())
			return granules <= granules_ ? RegionStatus::ready
						      : RegionStatus::smaller;
		if (spent_)
			return RegionStatus::failed;
		if (!traced())
			return RegionStatus::no_debugger;
		const auto size = granules * region_granule;
		std::byte *executable = nullptr;
		std::byte *writable = nullptr;
		if (!map(size, executable, writable)) {
			note("the region could not be mapped");
			return RegionStatus::failed;
		}
		const auto prepared = ask_debugger(
			1, reinterpret_cast<std::uintptr_t>(executable), size);
		if (prepared == Reply::no_debugger) {
			/* The debugger was gone before it served the request, so
			 * nothing can still write into the region, and this process
			 * has not involved one: it can try again. */
			note("the debugger left before it prepared the region");
			give_back(executable, writable, size);
			return RegionStatus::no_debugger;
		}
		/* From a request the debugger is involved: whatever happens, this
		 * process does not try again. */
		spent_ = true;
		if (prepared != Reply::answered) {
			/* A debugger that answers late still writes into the
			 * region, so it stays mapped. */
			note("the debugger did not prepare the region");
			return RegionStatus::failed;
		}
		/* Detach next: the guest must not run under the debugger. A
		 * debugger that does not answer this either is left as it is; one
		 * that is gone has detached, and the pages it blessed before it
		 * went are proved below. */
		const auto detached = ask_debugger(0, 0, 0);
		if (detached != Reply::answered && detached != Reply::no_debugger) {
			note("the debugger did not detach");
			return RegionStatus::failed;
		}
		if (mlock(writable, size) != 0) {
			note("the region could not be wired", true);
			give_back(executable, writable, size);
			return RegionStatus::failed;
		}
		if (!kill_cleared()) {
			note("the process still dies on a bad page");
			give_back(executable, writable, size);
			return RegionStatus::failed;
		}
		if (!check(executable, writable, size)) {
			note("the prepared pages did not run code");
			give_back(executable, writable, size);
			return RegionStatus::failed;
		}
		base_ = executable;
		delta_ = writable - executable;
		granules_ = granules;
		ready_.store(true, std::memory_order_release);
		return RegionStatus::ready;
	}

	[[nodiscard]] void *allocate(std::size_t size) noexcept
	{
		if (!ready() || size == 0 || size > granules_ * region_granule)
			return nullptr;
		const auto count = granules_for(size);
		os_unfair_lock_lock(&lock_);
		const auto first = used_.find(count, granules_);
		if (first != Set::none)
			used_.mark(first, count, true);
		os_unfair_lock_unlock(&lock_);
		return first == Set::none ? nullptr : base_ + first * region_granule;
	}

	/* Whether mapping came from the region; if so its granules return to it.
	 * The pages stay mapped, blessed and wired for the life of the process. */
	[[nodiscard]] bool release(void *mapping, std::size_t size) noexcept
	{
		if (!contains(mapping))
			return false;
		const auto first = static_cast<std::size_t>(
			static_cast<std::byte *>(mapping) - base_) / region_granule;
		const auto count = granules_for(size);
		if (count > granules_ - first)
			return true;
		os_unfair_lock_lock(&lock_);
		used_.mark(first, count, false);
		os_unfair_lock_unlock(&lock_);
		return true;
	}

	[[nodiscard]] void *alias(void *executable) const noexcept
	{
		return contains(executable) ?
			static_cast<std::byte *>(executable) + delta_ : executable;
	}

private:
	using Set = GranuleSet<region_limit / region_granule>;

	[[nodiscard]] static constexpr std::size_t granules_for(
		std::size_t size) noexcept
	{
		return (size + region_granule - 1U) / region_granule;
	}

	[[nodiscard]] bool contains(const void *address) const noexcept
	{
		if (!ready())
			return false;
		const auto *bytes = static_cast<const std::byte *>(address);
		return bytes >= base_ &&
			bytes < base_ + granules_ * region_granule;
	}

	/* Maps `size` bytes read-execute, as near this image as the kernel allows
	 * and below 1 TiB, and the same pages writable. */
	[[nodiscard]] static bool map(std::size_t size, std::byte *&executable,
		std::byte *&writable) noexcept
	{
		/* Any function of this image locates it. */
		const auto image = reinterpret_cast<std::uintptr_t>(
			&::uml::cpu::jit_memory::allocate);
		const std::array<std::uintptr_t, 2> hints{
			image & ~(region_prediction_block - 1U), 0};
		vm_address_t address = 0;
		bool mapped = false;
		for (const auto hint : hints) {
			address = static_cast<vm_address_t>(hint);
			if (vm_allocate(mach_task_self(), &address, size,
				    VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
				continue;
			if (address + size <= region_address_limit) {
				mapped = true;
				break;
			}
			(void)vm_deallocate(mach_task_self(), address, size);
		}
		if (!mapped)
			return false;
		if (vm_protect(mach_task_self(), address, size, FALSE,
			    VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
			(void)vm_deallocate(mach_task_self(), address, size);
			return false;
		}
		vm_address_t alias_address = 0;
		vm_prot_t current = 0;
		vm_prot_t maximum = 0;
		if (vm_remap(mach_task_self(), &alias_address, size, 0,
			    VM_FLAGS_ANYWHERE, mach_task_self(), address, FALSE,
			    &current, &maximum, VM_INHERIT_NONE) != KERN_SUCCESS) {
			(void)vm_deallocate(mach_task_self(), address, size);
			return false;
		}
		if (vm_protect(mach_task_self(), alias_address, size, FALSE,
			    VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
			(void)vm_deallocate(mach_task_self(), alias_address, size);
			(void)vm_deallocate(mach_task_self(), address, size);
			return false;
		}
		executable = reinterpret_cast<std::byte *>(address);
		writable = reinterpret_cast<std::byte *>(alias_address);
		return true;
	}

	/* Writes `mov w0, #value; ret` through the alias at the first and the last
	 * granule and runs both through the executable mapping. */
	[[nodiscard]] static bool check(std::byte *executable,
		std::byte *writable, std::size_t size) noexcept
	{
		const std::array<std::size_t, 2> offsets{0, size - region_granule};
		std::uint32_t value = 0x5a5a;
		for (const auto offset : offsets) {
			auto *words = reinterpret_cast<std::uint32_t *>(
				writable + offset);
			words[0] = 0x52800000U | (value << 5U);
			words[1] = 0xd65f03c0U;
			sys_icache_invalidate(executable + offset, 8);
			if (!runs(executable + offset, value))
				return false;
			++value;
		}
		return true;
	}

	/* A region the debugger blessed and the process cannot use. */
	static void give_back(std::byte *executable, std::byte *writable,
		std::size_t size) noexcept
	{
		(void)vm_deallocate(mach_task_self(),
			reinterpret_cast<vm_address_t>(writable), size);
		(void)vm_deallocate(mach_task_self(),
			reinterpret_cast<vm_address_t>(executable), size);
	}

	std::mutex prepare_mutex_;
	os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	std::atomic<bool> ready_{};
	bool spent_{};
	std::byte *base_{};
	std::ptrdiff_t delta_{};
	std::size_t granules_{};
	Set used_{};
};

[[clang::no_destroy]] constinit Region region;

#endif // UML_JIT_MEMORY_REGION_ROUTE

} // namespace

#if UML_JIT_MEMORY_REGION_TEST
namespace testing {

void send_real_requests(bool send) noexcept
{
	test_sends_requests.store(send);
}

void set_traced_probe(bool (*probe)() noexcept) noexcept
{
	test_traced_probe.store(probe);
}

int request(std::uint64_t command) noexcept
{
	return static_cast<int>(ask_debugger(command, 0, 0));
}

unsigned unanswered_requests_seen() noexcept
{
	return unanswered_requests.load();
}

} // namespace testing
#endif

bool available() noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	return region.ready();
#else
	static const bool supported = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
		/* A successful MAP_JIT mapping is necessary but not sufficient:
		 * without a W^X toggle the first store into it faults. Requiring
		 * the publication route keeps this from promising an engine that
		 * would abort on its first translated block.
		 */
		if (!jit_publication_supported())
			return false;
		void *mapping = mmap(nullptr, 16U * 1024U,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (mapping == MAP_FAILED)
			return false;
		(void)munmap(mapping, 16U * 1024U);
		window.claim();
		return true;
#else
		return false;
#endif
	}();
	return supported;
#endif
}

void *allocate(std::size_t size) noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	return region.allocate(size);
#else
	if constexpr (jit_publication_supported()) {
		if (void *mapping = window.allocate(size); mapping != nullptr)
			return mapping;
	}
	void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	return mapping == MAP_FAILED ? nullptr : mapping;
#endif
}

void release(void *mapping, std::size_t size)
{
#if UML_JIT_MEMORY_REGION_ROUTE
	(void)region.release(mapping, size);
#else
	if constexpr (jit_publication_supported()) {
		if (window.release(mapping, size))
			return;
	}
	(void)munmap(mapping, size);
#endif
}

void set_writable(bool writable)
{
	jit_write_protect(writable ? 0 : 1);
}

void *writable_alias(void *executable) noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	return region.alias(executable);
#else
	return executable;
#endif
}

void invalidate_instruction_cache(void *code, std::size_t size)
{
	sys_icache_invalidate(code, size);
}

bool debugger_route() noexcept
{
	return UML_JIT_MEMORY_REGION_ROUTE != 0;
}

bool debugger_attached() noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	return traced();
#else
	return false;
#endif
}

RegionStatus prepare_debugger_region(std::size_t bytes) noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	return region.prepare(bytes);
#else
	(void)bytes;
	return RegionStatus::unsupported;
#endif
}

void release_debugger() noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	if (traced())
		(void)ask_debugger(0, 0, 0);
#endif
}

std::size_t prepared_region_bytes() noexcept
{
#if UML_JIT_MEMORY_REGION_ROUTE
	return region.bytes();
#else
	return 0;
#endif
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
