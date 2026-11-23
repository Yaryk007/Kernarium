// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "device.hpp"
#include "options.hpp"
#include "carrier_activity.hpp"
#include "context.hpp"
#include "guest_sync.hpp"
#include "guest_console.hpp"
#include "guest_hand_back.hpp"
#include "guest_hibernation.hpp"
#include "guest_statistics.hpp"
#include "kernel_signal_scope.hpp"
#include "memory_pressure_response.hpp"
#include "native_kernel_layout.hpp"
#include "session_record.hpp"
#include "stall_watch.hpp"
#include "start_refusal.hpp"
#include "thread_fence.hpp"
#include "virtual_jetsam.hpp"
#include "x86_runtime_support.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if UML_EMULATOR_HAS_KERNEL_RUNNER
extern "C" {
#include "elf_loader.h"
}
#endif

#include <cerrno>
#include <cstdio>
#include <unistd.h>

#if UML_EMULATOR_TESTING
#include <cstring>
#endif

namespace uml::emulator {

namespace {

/* A kernel without the console's "sync" command can only be asked for SysRq
 * 's', which reports that its writeback started, not that it finished. This
 * is the bound the host then waits before flushing what the guest handed it:
 * long enough for an ordinary writeback to reach the block layer, short enough
 * to fit inside the few seconds an application has when it is being put away. */
constexpr unsigned int guest_writeback_settle_ms = 1500U;

/* The waits for the guest's "sync" reply, which says the guest wrote its
 * filesystems back and so is the only confirmation. Each is one budget for the
 * whole request, the fallback of a kernel without the command and its settling
 * period included, and is chosen for the call that makes it, so that the calls
 * together stay inside what the SysRq request and its fixed settling period
 * took before (2 s and 1.5 s):
 *  - a sync alone waits at most guest_sync_wait_ms for the lifecycle lock and
 *    the reply together, the lock for at most guest_sync_lock_wait_ms of it;
 *  - the sync that opens a quiesce waits guest_quiesce_sync_wait_ms. It orders
 *    the writeback before the trim, and is the confirmation the quiesce
 *    reports, so a guest that needs longer costs the quiesce no more than that
 *    wait and leaves the storage unconfirmed;
 *  - the writeback after a suspend that failed waits guest_final_sync_wait_ms,
 *    since a guest that did not answer the first is not asked again. */
constexpr unsigned int guest_sync_wait_ms = 3500U;
constexpr unsigned int guest_sync_lock_wait_ms = 1000U;
constexpr unsigned int guest_quiesce_sync_wait_ms = 2000U;
constexpr unsigned int guest_final_sync_wait_ms = 1500U;

/* A guest about to be suspended hands its cold page cache and free memory to
 * the host first, since it keeps whatever it holds until it runs again. The
 * guest reclaims for at most a second and compacts for at most another, so
 * this also covers the release and the reply. A pass still running when it
 * expires finishes before the guest takes the suspend request queued behind
 * it. A quiesce counts it from its call. */
constexpr unsigned int guest_trim_wait_ms = 3000U;

/* A trim that must still be sent after its allowance went to the wait for the
 * mutex waits this long for the reply, which is the request and no more. */
constexpr unsigned int guest_trim_send_wait_ms = 1U;

using SteadyTime = std::chrono::steady_clock::time_point;

[[nodiscard]] SteadyTime steady_now() noexcept
{
	return std::chrono::steady_clock::now();
}

#if UML_EMULATOR_TESTING
/* The clock the lifecycle calls count their waits by; a test replaces it. */
std::atomic<SteadyTime (*)() noexcept> lifecycle_clock{&steady_now};
#endif

[[nodiscard]] SteadyTime lifecycle_now() noexcept
{
#if UML_EMULATOR_TESTING
	return lifecycle_clock.load()();
#else
	return steady_now();
#endif
}

/* What is left of the guest trim's reply-wait allowance for a call made at
 * `called` that took the lifecycle mutex at `locked`: the time the call spent
 * waiting for the mutex counts against it, and the writeback that now comes
 * first in a quiesce does not. It is not an end-to-end bound of three
 * seconds: a console user that holds the console mutex once the remainder is
 * computed delays the request after it, as it did before. */
[[nodiscard]] unsigned int trim_wait_left(SteadyTime called,
	SteadyTime locked) noexcept
{
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
		locked - called).count();
	if (waited >= static_cast<std::int64_t>(guest_trim_wait_ms))
		return 0U;
	return guest_trim_wait_ms -
		static_cast<unsigned int>(std::max<std::int64_t>(waited, 0));
}

/* The most a request for the guest's memory figures waits for its reply. The
 * guest answers from its console interrupt, so a reply that has not come by
 * now is not coming soon, and the caller is not held longer for it. */
constexpr std::chrono::milliseconds guest_memory_wait {500};

/* What a call that settles the guest's storage reports of the guest. */
struct SettleOutcome final {
	::uml::runtime::GuestStorage storage;
	::uml::runtime::SettleSuspension suspension;
};

/* What a writeback request that ended as `writeback` says of the guest's
 * storage. A console that cannot be reached means there is no guest to settle
 * when none runs, and a guest the host cannot ask when one does. */
[[nodiscard]] ::uml::runtime::GuestStorage storage_after(
	::uml::device::GuestWriteback writeback, bool guest_running) noexcept
{
	using ::uml::device::GuestWriteback;
	using ::uml::runtime::GuestStorage;
	switch (writeback) {
	case GuestWriteback::done:
		return GuestStorage::confirmed;
	case GuestWriteback::started:
	case GuestWriteback::busy:
	case GuestWriteback::unanswered:
		return GuestStorage::unconfirmed;
	case GuestWriteback::refused:
		return GuestStorage::refused;
	case GuestWriteback::unreachable:
		break;
	}
	return guest_running ? GuestStorage::unreachable : GuestStorage::no_guest;
}

/* The public report's values are the runtime's, so the record and the API say
 * the same word. The zero value claims nothing. */
static_assert(static_cast<int>(::uml::runtime::GuestStorage::not_reported) ==
	UML_EMULATOR_GUEST_STORAGE_NOT_REPORTED);
static_assert(UML_EMULATOR_GUEST_STORAGE_NOT_REPORTED == 0);
static_assert(static_cast<int>(::uml::runtime::GuestStorage::confirmed) ==
	UML_EMULATOR_GUEST_STORAGE_CONFIRMED);
static_assert(static_cast<int>(::uml::runtime::GuestStorage::no_guest) ==
	UML_EMULATOR_GUEST_STORAGE_NO_GUEST);
static_assert(static_cast<int>(::uml::runtime::GuestStorage::unconfirmed) ==
	UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED);
static_assert(static_cast<int>(::uml::runtime::GuestStorage::refused) ==
	UML_EMULATOR_GUEST_STORAGE_REFUSED);
static_assert(static_cast<int>(::uml::runtime::GuestStorage::unreachable) ==
	UML_EMULATOR_GUEST_STORAGE_UNREACHABLE);
static_assert(static_cast<int>(
	::uml::runtime::SettleSuspension::not_asked) ==
	UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED);
static_assert(static_cast<int>(::uml::runtime::SettleSuspension::suspended) ==
	UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED);
static_assert(static_cast<int>(
	::uml::runtime::SettleSuspension::not_suspended) ==
	UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED);

/* A line written with write(2) alone: it takes no lock and allocates
 * nothing, so it can be written while the guest's threads are held. */
void write_line(const char *before, unsigned number,
	const char *after) noexcept
{
	char line[256];
	std::size_t size = 0;
	const auto append = [&](const char *text) {
		while (*text != '\0' && size < sizeof line - 1)
			line[size++] = *text++;
	};
	append(before);
	char digits[12];
	std::size_t count = 0;
	do {
		digits[count++] = static_cast<char>('0' + number % 10U);
		number /= 10U;
	} while (number != 0U);
	while (count != 0 && size < sizeof line - 1)
		line[size++] = digits[--count];
	append(after);
	line[size++] = '\n';
	(void)!::write(STDERR_FILENO, line, size);
}

/* The fence's condition for the plain hold, with every thread stopped: none
 * of them owns the guest file registry, so the release and the restore of the
 * disk locks cannot wait on a stopped owner. */
[[nodiscard]] bool registry_idle(void *) noexcept
{
	return ::uml::runtime::guest_file_registry_idle();
}

[[nodiscard]] int guest_request_result(
	::uml::device::GuestRequest request) noexcept
{
	switch (request) {
	case ::uml::device::GuestRequest::done:
	case ::uml::device::GuestRequest::sent:
		return UML_EMULATOR_OK;
	case ::uml::device::GuestRequest::unanswered:
	case ::uml::device::GuestRequest::busy:
		return -UML_EMULATOR_RUNTIME_ERROR;
	case ::uml::device::GuestRequest::unreachable:
	case ::uml::device::GuestRequest::refused:
		break;
	}
	return -UML_EMULATOR_UNSUPPORTED;
}

/* An embedded kernel maps fixed host addresses, so a process admits one run
 * at a time. A run queued behind it stays cancellable by its handle's stop
 * request. */
std::mutex process_address_space_mutex;
std::condition_variable process_address_space_changed;
bool process_address_space_admitted;

class TranslationCacheRunScope final {
public:
	TranslationCacheRunScope() noexcept = default;

	[[nodiscard]] bool enter(bool required) noexcept
	{
		required_ = required;
		entered_ = !required ||
			::uml::runtime::acquire_translation_cache_run_session();
		return entered_;
	}

	~TranslationCacheRunScope()
	{
		if (required_ && entered_)
			::uml::runtime::release_translation_cache_run_session();
	}

	TranslationCacheRunScope(const TranslationCacheRunScope &) = delete;
	TranslationCacheRunScope &operator=(
		const TranslationCacheRunScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	bool required_{};
	bool entered_{};
};

#if UML_EMULATOR_TESTING
/* The region a test declares to be a linked kernel (src/testing.hpp). */
std::atomic<const std::byte *> test_linked_kernel_begin;
std::atomic<std::size_t> test_linked_kernel_size;
#endif

/* Whether `image` is a kernel image the process links into itself
 * (runtime/kernel_signal_scope.hpp): the one kernel image a handle keeps a
 * pointer to instead of a copy, since no caller can free it and a copy would
 * be dirty memory for the handle's life. Any other buffer is the caller's. */
[[nodiscard]] bool is_linked_kernel(std::span<const std::byte> image) noexcept
{
#if UML_EMULATOR_TESTING
	if (const auto begin =
		    test_linked_kernel_begin.load(std::memory_order_acquire);
	    begin != nullptr && !image.empty() && image.data() == begin &&
	    image.size() == test_linked_kernel_size.load(std::memory_order_relaxed))
		return true;
#endif
	return ::uml::runtime::is_embedded_kernel_image(
		image.data(), image.size());
}

#if UML_EMULATOR_TESTING
constexpr char test_kernel_magic[] = "uml-test-kernel";

[[nodiscard]] testing::KernelEntry test_kernel_entry(
	std::span<const std::byte> image) noexcept
{
	testing::KernelEntry entry{};
	if (image.size() != sizeof(test_kernel_magic) + sizeof(entry) ||
	    std::memcmp(image.data(), test_kernel_magic,
		    sizeof(test_kernel_magic)) != 0)
		return nullptr;
	std::memcpy(&entry, image.data() + sizeof(test_kernel_magic),
		sizeof(entry));
	return entry;
}

std::atomic<testing::AddressSpaceAdmissionHook> after_handle_admission_hook;
std::atomic<testing::AddressSpaceAdmissionHook> admission_wait_boundary_hook;
std::atomic<testing::AddressSpaceAdmissionHook> before_gate_release_hook;
std::atomic<void *> address_space_admission_hook_context;
std::atomic<testing::GuestHandBackHook> guest_hand_back_hook;
std::atomic<void *> guest_hand_back_hook_context;

void invoke_admission_hook(
	const std::atomic<testing::AddressSpaceAdmissionHook> &hook) noexcept
{
	if (const auto callback = hook.load(std::memory_order_acquire);
	    callback != nullptr)
			callback(address_space_admission_hook_context.load(
				std::memory_order_acquire));
}

/* A Darwin sanitizer holds the product's macOS window itself (AddressSanitizer
 * keeps its heap at 0x600000000000, ThreadSanitizer reserves everything
 * outside its application ranges), and the process reserves its window for
 * good. A sanitized test variant therefore places runs at the start of
 * ThreadSanitizer's middle application range, which AddressSanitizer leaves to
 * the application too, unless a test places them elsewhere. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define UML_EMULATOR_TEST_SANITIZED 1
#endif
#endif
#ifdef UML_EMULATOR_TEST_SANITIZED
constexpr std::uintptr_t default_test_native_window_start = 0x550000000000ULL;
#else
constexpr std::uintptr_t default_test_native_window_start = 0;
#endif
std::atomic<std::uintptr_t> test_native_kernel_window_start{
	default_test_native_window_start};
/* Where the pre-start RAM trial of a run is made, instead of in the run's
 * window; 0 for the window. */
std::atomic<std::uintptr_t> test_guest_ram_trial_start{0};
/* Whether every run's window is one the host places, as on an iOS device. */
std::atomic_bool test_native_kernel_window_by_the_host{false};
#endif

/* Where a run's native kernel window goes: the product's placement, or in the
 * test variant the same geometry wherever a test placed the window. The
 * process holds its window for as long as it lives, so a run takes the window
 * it holds or extends it. */
[[nodiscard]] ::uml::runtime::NativeKernelLayout run_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
#if UML_EMULATOR_TESTING
	const auto start = test_native_kernel_window_start.load(
		std::memory_order_acquire);
	auto product = ::uml::runtime::native_kernel_placement(guest_memory);
	if (test_native_kernel_window_by_the_host.load(
		    std::memory_order_acquire) &&
	    product.geometry.valid) {
		const auto layout = ::uml::runtime::reserve_process_native_window(
			product.geometry, ::uml::runtime::WindowPlacement::anywhere);
		return layout.valid ? layout :
			::uml::runtime::NativeKernelLayout{.reservation_failed = true};
	}
	if (start != 0 && product.geometry.valid &&
	    product.placement == ::uml::runtime::WindowPlacement::exact) {
		auto &geometry = product.geometry;
		const auto length = geometry.window_end - geometry.window_start;
		geometry.load_bias += start - geometry.window_start;
		geometry.window_start = start;
		geometry.window_end = start + length;
		const auto layout = ::uml::runtime::reserve_process_native_window(
			geometry, product.placement);
		return layout.valid ? layout :
			::uml::runtime::NativeKernelLayout{.reservation_failed = true};
	}
#endif
	return ::uml::runtime::prepare_native_kernel_layout(guest_memory);
}

/* The layout the pre-start RAM trial is made in: the run's own. A test
 * variant can make it elsewhere, where the host maps no RAM. */
[[nodiscard]] ::uml::runtime::NativeKernelLayout guest_ram_trial_layout(
	::uml::runtime::NativeKernelLayout layout) noexcept
{
#if UML_EMULATOR_TESTING
	if (const auto start =
		    test_guest_ram_trial_start.load(std::memory_order_acquire);
	    start != 0) {
		const auto length = layout.window_end - layout.window_start;
		layout.window_start = start;
		layout.window_end = start + length;
	}
#endif
	return layout;
}

/* The ahead-of-time choice a start takes, put back unless the run reaches its
 * guest: every way a start can fail after the choice and before the guest
 * runs leaves the session's choice as it was before the start. A run that
 * restarts keeps its session's, whichever way its next start ends. It is
 * held under the process admission, which the choice also belongs to. */
class AotChoiceScope final {
public:
	AotChoiceScope() noexcept : before_{::uml::runtime::aot_choice()} {}

	~AotChoiceScope()
	{
		if (!kept_)
			::uml::runtime::restore_aot_choice(before_);
	}

	/* The guest runs: the choice is the session's now. */
	void keep() noexcept { kept_ = true; }

	AotChoiceScope(const AotChoiceScope &) = delete;
	AotChoiceScope &operator=(const AotChoiceScope &) = delete;

private:
	int before_;
	bool kept_{};
};

#if UML_EMULATOR_HAS_KERNEL_RUNNER
/* What the loader calls as it enters the kernel (uml_elf_run). */
void keep_aot_choice(void *scope) noexcept
{
	static_cast<AotChoiceScope *>(scope)->keep();
}
#endif

class ProcessAddressSpaceAdmission final {
public:
	ProcessAddressSpaceAdmission() noexcept = default;

	[[nodiscard]] bool enter(::uml::runtime::Context &context)
	{
		std::unique_lock lock{process_address_space_mutex};
		while (process_address_space_admitted &&
		       !context.stop_requested()) {
#if UML_EMULATOR_TESTING
			invoke_admission_hook(admission_wait_boundary_hook);
#endif
			process_address_space_changed.wait(lock);
		}
		if (context.stop_requested())
			return false;
		process_address_space_admitted = true;
		entered_ = true;
		return true;
	}

	~ProcessAddressSpaceAdmission()
	{
		if (!entered_)
			return;
#if UML_EMULATOR_TESTING
		invoke_admission_hook(before_gate_release_hook);
#endif
		{
			std::lock_guard lock{process_address_space_mutex};
			process_address_space_admitted = false;
		}
		process_address_space_changed.notify_all();
	}

	ProcessAddressSpaceAdmission(const ProcessAddressSpaceAdmission &) = delete;
	ProcessAddressSpaceAdmission &operator=(
		const ProcessAddressSpaceAdmission &) = delete;

private:
	bool entered_{};
};
}

#if UML_EMULATOR_TESTING
namespace testing {

std::vector<std::byte> kernel_image(KernelEntry entry)
{
	std::vector<std::byte> image(sizeof(test_kernel_magic) + sizeof(entry));
	std::memcpy(image.data(), test_kernel_magic, sizeof(test_kernel_magic));
	std::memcpy(image.data() + sizeof(test_kernel_magic), &entry,
		sizeof(entry));
	return image;
}

void set_address_space_admission_hooks(
		AddressSpaceAdmissionHook after_handle_admission,
		AddressSpaceAdmissionHook at_wait_boundary,
		AddressSpaceAdmissionHook before_gate_release, void *context) noexcept
{
	address_space_admission_hook_context.store(context, std::memory_order_release);
	after_handle_admission_hook.store(
		after_handle_admission, std::memory_order_release);
	admission_wait_boundary_hook.store(
		at_wait_boundary, std::memory_order_release);
	before_gate_release_hook.store(
		before_gate_release, std::memory_order_release);
}

void set_linked_kernel(const void *start, std::size_t size) noexcept
{
	test_linked_kernel_size.store(size, std::memory_order_relaxed);
	test_linked_kernel_begin.store(static_cast<const std::byte *>(start),
		std::memory_order_release);
}

void place_native_kernel_window(std::uintptr_t start) noexcept
{
	test_native_kernel_window_start.store(start, std::memory_order_release);
}

void place_native_kernel_window_by_the_host(bool by_the_host) noexcept
{
	test_native_kernel_window_by_the_host.store(
		by_the_host, std::memory_order_release);
}

void place_guest_ram_trial(std::uintptr_t start) noexcept
{
	test_guest_ram_trial_start.store(start, std::memory_order_release);
}

void set_guest_hand_back_hook(GuestHandBackHook hook, void *context) noexcept
{
	guest_hand_back_hook_context.store(context, std::memory_order_release);
	guest_hand_back_hook.store(hook, std::memory_order_release);
}

void report_guest_shutdown(::uml::runtime::GuestShutdownKind kind,
	const char *command) noexcept
{
	if (auto *const context = ::uml::runtime::current_context();
	    context != nullptr)
		context->record_guest_shutdown(kind,
			command == nullptr ? std::string_view{} :
				std::string_view{command});
}

NativeKernelWindow native_kernel_window(std::size_t guest_memory) noexcept
{
	const auto layout = run_native_kernel_layout(guest_memory);
	if (!layout.valid)
		return {};
	return {.start = layout.window_start, .end = layout.window_end};
}

} // namespace testing
#endif

class Emulator final {
public:
	explicit Emulator(const uml_emulator_config &config)
		: rootfs_(config.rootfs_path == nullptr ? "" : config.rootfs_path),
		  runtime_{},
		  network_{network_provider(config)},
		  network_context_{network_}
	{
		if (config.kernel_image != nullptr && config.kernel_image_size != 0) {
			const auto image = std::span{
				static_cast<const std::byte *>(config.kernel_image),
				config.kernel_image_size};
			if (is_linked_kernel(image)) {
				/* The process's own kernel section is immutable and outlives
				 * every handle, and a copy of it would be dirty memory for
				 * the handle's life. Nothing a caller can free is borrowed. */
				kernel_view_ = image;
			} else {
				kernel_.assign(image.begin(), image.end());
				kernel_view_ = kernel_;
			}
		}
	}

	Emulator(const Emulator &) = delete;
	Emulator &operator=(const Emulator &) = delete;

	[[nodiscard]] int release_guest()
	{
		ProcessAddressSpaceAdmission process_admission;
		::uml::runtime::RunScope run_scope{runtime_};
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		if (!power_off_pending_)
			return -UML_EMULATOR_UNSUPPORTED;
		if (!process_admission.enter(runtime_))
			return -UML_EMULATOR_RUNTIME_ERROR;
		const int result = hand_back_guest();
		if (result == 0) {
			power_off_pending_ = false;
			/* The session is over and none of its executors exists: the
			 * next one chooses its ahead-of-time component afresh. */
			::uml::runtime::end_aot_session();
		}
		return result;
	}

	[[nodiscard]] int run(int argc, const char *const argv[],
			      const char *const envp[])
	{
		/* The mark of the last run of this handle is gone before anything of
		 * this one can fail, so that a result this run returns for another
		 * reason never reads as a refusal for memory. */
		start_refused_for_memory_.store(false, std::memory_order_release);
		if (argc < 0 || (argc != 0 && argv == nullptr))
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (kernel_view_.empty())
			return -UML_EMULATOR_UNSUPPORTED;
		ProcessAddressSpaceAdmission process_admission;
		::uml::runtime::RunScope run_scope{runtime_};
		/* Declared after RunScope so the session-owned executor pool drains
		 * first on scope exit. Thread states retain only lightweight JIT
		 * wrappers; Context::end_run() then retires those wrappers and their
		 * memory domain after no pooled cache can still refer to it.
		 */
		TranslationCacheRunScope translation_cache_run;
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		clear_run_end();
		/* A process runs one guest, and that guest may restart itself: this
		 * handle's guest, after a run that ended with a restart, goes back to
		 * the process once the run is admitted. Refuse any other later run
		 * before it names the console, maps a kernel window or reaches a
		 * kernel that would find no guest RAM left to claim. */
		if (!restartable_ && ::uml::runtime::process_guest_spent())
			return -UML_EMULATOR_UNSUPPORTED;
#if UML_EMULATOR_TESTING
		invoke_admission_hook(after_handle_admission_hook);
#endif
		auto prepared = prepare_emulator_options(argc, argv, rootfs_);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (runtime_.set_execution_engine(prepared.engine) != 0)
			return -UML_EMULATOR_UNSUPPORTED;
		runtime_.set_top_byte_ignore(prepared.top_byte_ignore);
		runtime_.set_x86_jit_tier(prepared.jit_tier_instructions);
		/* What this handle's own run keeps (the engine, the top-byte setting
		 * and the tier belong to its context) is set here; what the whole
		 * process keeps waits for the run to own the process. */
		auto &arguments = prepared.arguments;
		std::vector<char *> argument_pointers;
		argument_pointers.reserve(arguments.size() + 1);
		for (auto &argument : arguments)
			argument_pointers.push_back(argument.data());
		argument_pointers.push_back(nullptr);

		std::vector<std::string> environment_strings;
		if (envp != nullptr) {
			for (auto entry = envp; *entry != nullptr; ++entry)
				environment_strings.emplace_back(*entry);
		}
		std::vector<char *> environment_pointers;
		environment_pointers.reserve(environment_strings.size() + 1);
		for (auto &entry : environment_strings)
			environment_pointers.push_back(entry.data());
		environment_pointers.push_back(nullptr);
		if (!process_admission.enter(runtime_))
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Another run may have claimed the guest while this one waited for
		 * admission. */
		if (!restartable_ && ::uml::runtime::process_guest_spent())
			return -UML_EMULATOR_UNSUPPORTED;
		/* Before any executor reads it, so before any table of the
		 * component exists: the component runs only with the release it was
		 * made from, which the launcher states. A launch that asks for it
		 * where the build, the framework or the release does not have it
		 * does not start; a launch that states no release (a Linux guest)
		 * gets none. The choice belongs to the session of the run that owns
		 * the process: it is taken once the run is admitted, after the
		 * session before it has ended (release_guest ends it while it holds
		 * the admission), and a run the process refuses never reaches it.
		 * A start that fails after it and before its guest runs puts the
		 * session's choice back (AotChoiceScope). */
		AotChoiceScope choice_scope;
		if (!prepared.informational) {
			::uml::runtime::AotRelease release{
				.id = prepared.aot_release_id,
				.manifest_sha256 = prepared.aot_manifest_sha256,
				.system_sha256 = prepared.aot_system_sha256};
			if (!::uml::runtime::select_aot(prepared.aot,
				    prepared.aot_release_stated ? &release : nullptr))
				return -UML_EMULATOR_UNSUPPORTED;
		}
		/* The settings the whole process keeps, published only once the run owns
		 * the process and its start is valid: a start the process refuses (the
		 * guest spent, the component refused) leaves none of them behind, and a
		 * start that waits for admission cannot change what the run before it
		 * is using. */
		if (!prepared.informational)
			prepare_guest_hibernation(prepared.guest_hibernation);
		const bool jit_run = !prepared.informational &&
			runtime_.execution_engine() ==
				::uml::runtime::ExecutionEngine::jit;
		/* Before any JIT engine exists, so every executor derives its cache
		 * geometry from the same budget. Data-only engines neither allocate
		 * these resources nor claim the process-global configuration.
		 */
		if (jit_run &&
		    !::uml::runtime::set_translation_cache_budget(
				prepared.jit_cache_budget, prepared.guest_cpus,
				prepared.jit_executor_cache_budget))
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Before any gadget executor exists, for the same reason; an
		 * x86-64 address space starts on the gadget engine in a JIT run
		 * too. The limit the launch states for a host that sets none comes
		 * first, since the budget derives from it. */
		std::size_t gadget_cache_budget{};
		if (!prepared.informational) {
			::uml::runtime::set_process_memory_limit(
				prepared.memory_limit_bytes);
			gadget_cache_budget = ::uml::runtime::set_gadget_cache_budget(
				prepared.guest_memory_bytes,
				prepared.gadget_cache_budget, prepared.guest_cpus);
		}
		/* A JIT run activates the managed multi-vCPU executor pool. */
		if (!translation_cache_run.enter(jit_run))
			return -UML_EMULATOR_RUNTIME_ERROR;
		if (restartable_) {
			if (const int refused = hand_back_guest(); refused != 0)
				return refused;
		}
		/* Readies the guest console's location before the guest binds it,
		 * and removes it when the run returns, however the guest ended. */
		const ::uml::device::GuestConsoleRun console_run{
			!prepared.informational && prepared.host_console_directory};
		/* The geometry of the guest's size is checked, and the window it
		 * needs reserved, before anything else is: a size the product has no
		 * layout for reserves nothing. A host that has not the address space
		 * or the memory limit for the guest's size says so here, before the
		 * kernel starts, and the run fails with a result of its own: past
		 * this point the kernel answers a refused mapping by ending the
		 * process. */
		const auto native_layout = run_native_kernel_layout(
			prepared.guest_memory_bytes);
		if (!native_layout.valid)
			return native_layout.reservation_failed ?
				refused_for_memory() :
				-UML_EMULATOR_INVALID_ARGUMENT;
		/* The kernel's first block of RAM comes from the process's heap
		 * region, which the window may have left no room for. A start
		 * refused for it leaves the process's window as it was before the
		 * start (THE RULE, runtime/native_kernel_layout.hpp), so that the
		 * guest tried next has the address space it had; every other start
		 * keeps what it reserved. An informational run asks for no heap
		 * region and loads a kernel, whose kept text the window then holds:
		 * it keeps its window. */
		if (!prepared.informational &&
		    !::uml::runtime::reserve_process_heap()) {
			::uml::runtime::undo_process_window();
			return refused_for_memory();
		}
		::uml::runtime::commit_process_window();
		if (!prepared.informational &&
		    !::uml::runtime::native_guest_memory_mappable(
			    guest_ram_trial_layout(native_layout),
			    prepared.guest_memory_bytes))
			return refused_for_memory();
		const ::uml::runtime::NativeKernelLayoutScope layout_scope{
			native_layout};
		if (!layout_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Where the host sets no limit and the launch stated one, a Mac run
		 * says when its footprint goes past it: the line iOS would have
		 * ended the process at. It ends with the run. */
		std::unique_ptr<::uml::runtime::VirtualJetsamWatch> virtual_jetsam;
		if (!prepared.informational)
			virtual_jetsam = ::uml::runtime::start_virtual_jetsam_watch(
				gadget_cache_budget);
		/* One opaque route publishes CPU and network endpoints under one lock.
		 * Its intrusive storage and commit are allocation-free. */
		::uml::runtime::ActiveRunRouteScope route_scope{
			runtime_, &network_context_,
			::uml::runtime::ActiveRunRouteScope::DeferredPublication{}};
		route_scope.commit();

		/* An informational run prints and returns: it ran no guest, so it
		 * has no end to record. */
		std::optional<RunEndRecorder> run_end_recorder;
		/* Declared after the recorder, so that it ends first and records an
		 * event in progress before the session ends. */
		std::unique_ptr<::uml::runtime::StallWatch> stall_watch;
		/* Ends before the watch: its trim reaches this handle. A test variant
		 * never listens to the system, whose real events would reach the
		 * guest consoles the tests count. */
		std::unique_ptr<::uml::runtime::MemoryPressureWatch> memory_pressure;
#if UML_EMULATOR_TESTING
		if (const auto entry = test_kernel_entry(kernel_view_);
		    entry != nullptr) {
			if (!prepared.informational) {
				::uml::runtime::set_memory_terms_reader(
					&::uml::runtime::memory_terms);
				::uml::runtime::set_run_figures_reader(
					&::uml::runtime::run_figures);
				(void)::uml::runtime::begin_session_record(
					prepared.session_record_path, arguments);
				run_end_recorder.emplace(*this);
				stall_watch = ::uml::runtime::start_stall_watch();
			}
			/* The kernel's own process state is the run's to restore, as
			 * the loader restores it around a real kernel. */
			const ::uml::runtime::KernelSignalScope kernel_signals{0, 0};
			choice_scope.keep();
			return entry(static_cast<int>(arguments.size()),
				argument_pointers.data(), environment_pointers.data());
		}
#endif
#if UML_EMULATOR_HAS_KERNEL_RUNNER
		if (!prepared.informational) {
			::uml::runtime::set_memory_terms_reader(
				&::uml::runtime::memory_terms);
			::uml::runtime::set_run_figures_reader(
				&::uml::runtime::run_figures);
			(void)::uml::runtime::begin_session_record(
				prepared.session_record_path, arguments);
			run_end_recorder.emplace(*this);
			stall_watch = ::uml::runtime::start_stall_watch();
#if !UML_EMULATOR_TESTING
			memory_pressure = ::uml::runtime::start_memory_pressure_response(
				&Emulator::trim_for_memory_pressure_of, this);
#endif
		}
		/* The choice is the session's once the loader is about to enter the
		 * kernel: a kernel image it refuses leaves it as it was. */
		const auto result = uml_elf_run(kernel_view_.data(),
			kernel_view_.size(), native_layout.load_bias,
			native_layout.reserved ? native_layout.window_start : 0U,
			native_layout.reserved ? native_layout.window_end : 0U,
			static_cast<int>(arguments.size()), argument_pointers.data(),
			environment_pointers.data(), keep_aot_choice, &choice_scope);
		return result;
#else
		return -UML_EMULATOR_UNSUPPORTED;
#endif
	}

	[[nodiscard]] bool has_embedded_kernel() const noexcept
	{
		return !kernel_view_.empty();
	}

	void request_stop() noexcept
	{
		/* A stop reaches threads that run. */
		bring_back_guest_ram();
		/* Predicate mutation and wait enrollment share this mutex, so a
		 * notification cannot land between a false check and wait. */
		{
			std::lock_guard lock{process_address_space_mutex};
			runtime_.request_stop();
		}
		process_address_space_changed.notify_all();
	}

	/* Writes the guest's storage back and reports it, then flushes the host's
	 * files. The lifecycle lock is bounded: a lifecycle call that keeps it
	 * for longer leaves the storage unconfirmed, and the host files are
	 * flushed all the same. */
	[[nodiscard]] int sync(SettleOutcome &outcome) noexcept
	{
		const SteadyTime called = lifecycle_now();
		{
			BoundedLifecycleLock lock{*this,
				std::chrono::milliseconds{guest_sync_lock_wait_ms}};
			if (!lock.held()) {
				outcome = {::uml::runtime::GuestStorage::unconfirmed,
					::uml::runtime::SettleSuspension::not_asked};
			} else if (guest_held()) {
				/* Asking would end the suspend, and nothing writes while
				 * the host holds the guest. A guest it does not hold, as
				 * when its threads could not be held or its suspend was
				 * not confirmed, can have written since: it is asked. */
				outcome = quiesce_outcome_;
			} else {
				::uml::runtime::note_host_wake();
				const auto waited =
					std::chrono::duration_cast<std::chrono::milliseconds>(
						lifecycle_now() - called);
				const auto wait = std::max(
					std::chrono::milliseconds{guest_sync_wait_ms} - waited,
					std::chrono::milliseconds{1});
				outcome = {storage_after(
					::uml::device::request_guest_writeback(wait,
						guest_writeback_settle_ms), guest_running()),
					::uml::runtime::SettleSuspension::not_asked};
			}
		}
		::uml::runtime::note_guest_settle(
			::uml::runtime::SettleKind::sync, outcome.storage,
			outcome.suspension);
		const auto result = ::uml::runtime::sync_guest_files();
		return static_cast<int>(result.failed);
	}

	/* `resident` keeps the guest's RAM in the process: nothing is hibernated,
	 * and the threads are held by the handle's own fence. */
	[[nodiscard]] int quiesce(SettleOutcome &outcome,
		bool resident = false) noexcept
	{
		/* The trim's reply wait is counted from the call: the time spent
		 * behind a lifecycle call or a pressure trim is taken from it. The
		 * pressure trims sent so far are noted just before, to know whether
		 * one was sent for this guest while the call waited: one that ends
		 * between the two reads counts, as the guest was just trimmed. */
		const std::uint64_t pressure_trims_at_call =
			pressure_trims_sent_.load();
		const SteadyTime called = lifecycle_now();
		const LifecycleLock lock{*this};
		const SteadyTime locked = lifecycle_now();
		if (!guest_held()) {
			/* The guest runs, or is not held and may have run since: a
			 * saved outcome says nothing of what it wrote after it. */
			const auto earlier = guest_suspension_;
			/* A console request ends a suspend the guest took itself. */
			::uml::runtime::note_host_wake();
			const bool running = guest_running();
			/* The guest's data goes first: what it dirtied reaches the
			 * host, and becomes reclaimable for the trim, before the trim
			 * spends the time that the sync needs. Its reply is bounded,
			 * and the fixed settling period of a kernel that can only be
			 * asked for SysRq 's' is left to the writeback after a failed
			 * suspend. */
			const auto first = ::uml::device::request_guest_writeback(
				std::chrono::milliseconds{guest_quiesce_sync_wait_ms}, 0U);
			auto writeback = first;
			/* A guest that gave no reply in time is slow, or stuck on its
			 * I/O: its trim would reclaim little and could spend the whole
			 * of its reply wait, which the few seconds an application has
			 * cannot afford. */
			const bool slow =
				first == ::uml::device::GuestWriteback::unanswered ||
				first == ::uml::device::GuestWriteback::busy;
			/* The request is the host's promise (uml_emulator_quiesce
			 * trims the guest first). Only a pressure trim that reached
			 * the guest meanwhile, and used the allowance, stands for it:
			 * elapsed time alone does not show that anyone asked. */
			if (!slow) {
				const unsigned int wait = trim_wait_left(called, locked);
				const bool asked = pressure_trims_sent_.load() !=
					pressure_trims_at_call;
				if (wait != 0U)
					(void)::uml::device::trim_guest_page_cache(wait);
				else if (!asked)
					(void)::uml::device::trim_guest_page_cache(
						guest_trim_send_wait_ms);
			}
			auto suspend_sync = ::uml::device::GuestSuspendSync::unstated;
			guest_suspension_ = ::uml::device::suspend_guest(&suspend_sync);
			/* A suspension that was uncertain stays so for the wake at the
			 * resume, whatever this suspend says. */
			if (guest_suspension_ == ::uml::device::GuestSuspension::none &&
			    earlier == ::uml::device::GuestSuspension::unconfirmed)
				guest_suspension_ = earlier;
			if (guest_suspension_ == ::uml::device::GuestSuspension::system) {
				/* The suspend's reply says the guest reached the sleep
				 * boundary. It confirms the storage only when it also
				 * states that the suspend's own sync returned (a kernel
				 * without the statement, or one whose sync_on_suspend is
				 * off, does not): that sync followed the writeback above,
				 * so it stands whatever the writeback reported. Otherwise
				 * the storage is what the writeback reported. */
				const bool synced = suspend_sync ==
					::uml::device::GuestSuspendSync::synced;
				outcome = {synced ? ::uml::runtime::GuestStorage::confirmed :
					storage_after(first, running),
					::uml::runtime::SettleSuspension::suspended};
			} else {
				/* The guest runs, or may: what it holds dirty now is what
				 * a last writeback finds. */
				if (!slow && first !=
					::uml::device::GuestWriteback::unreachable)
					writeback = ::uml::device::request_guest_writeback(
						std::chrono::milliseconds{guest_final_sync_wait_ms},
						guest_writeback_settle_ms);
				outcome = {storage_after(writeback, running),
					::uml::runtime::SettleSuspension::not_suspended};
				if (outcome.storage == ::uml::runtime::GuestStorage::no_guest)
					outcome.suspension =
						::uml::runtime::SettleSuspension::not_asked;
			}
			quiesce_outcome_ = outcome;
		} else {
			outcome = quiesce_outcome_;
		}
		::uml::runtime::note_guest_settle(resident ?
			::uml::runtime::SettleKind::quiesce_resident :
			::uml::runtime::SettleKind::quiesce, outcome.storage,
			outcome.suspension);
		const auto result = ::uml::runtime::sync_guest_files();
		/* Last, with the guest asleep and its files on disk. */
		if (guest_suspension_ == ::uml::device::GuestSuspension::system)
			settle_suspended_guest(resident);
		else if (guest_suspension_ == ::uml::device::GuestSuspension::none) {
			/* The guest was left running (it could not suspend): what the
			 * memory pressure response asked of the trimmer while this call
			 * held the mutex was answered busy, and no resume follows to
			 * settle it. */
			(void)apply_trimming(false);
		}
		/* The files the host could not flush, as the API reports them. A
		 * lock it could not release is no file left unflushed, and is
		 * logged where it happens. */
		return static_cast<int>(result.failed);
	}

	void resume() noexcept
	{
		/* A hibernation still writing leaves what it has not reached in the
		 * process. */
		request_hibernation_abort();
		const LifecycleLock lock{*this};
		/* The disks, the RAM and the threads are back before the guest is
		 * woken. A restore that fails ends the process and does not
		 * return; where the end is replaced, nothing is woken either. */
		if (!bring_back_guest_locked())
			return;
		const auto suspension = std::exchange(guest_suspension_,
			::uml::device::GuestSuspension::none);
		::uml::runtime::note_host_wake();
		/* The host may have changed its thermal state or power mode while
		 * the process was suspended, and the system's notice with it: the
		 * stall watch reads them once. */
		::uml::runtime::request_host_state_reading();
		::uml::device::resume_guest(suspension);
		/* What was wanted while the guest slept, and a change that did not
		 * go through before: one that fails stays owed. */
		(void)apply_trimming(true);
	}

	/* The guest's periodic trimmer runs while the application wants it or
	 * the memory pressure response does: ending one request leaves the
	 * other's in place. */
	[[nodiscard]] int set_page_cache_trimming(bool enabled) noexcept
	{
		const LifecycleLock lock{*this};
		const bool before = std::exchange(trim_wanted_by_app_, enabled);
		/* Any console request ends a suspend, so a quiesced guest receives the
		 * change when it is resumed. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none) {
			trim_requested_by_app_ = true;
			return UML_EMULATOR_OK;
		}
		const int result = guest_request_result(apply_trimming(true, true));
		/* A request that was not sent, or that the guest refused, did not
		 * happen: the wish is the one before it. One that was sent and not
		 * answered is kept, as the guest may have taken it. */
		if (result == -UML_EMULATOR_UNSUPPORTED)
			trim_wanted_by_app_ = before;
		return result;
	}

	/* Brings the guest's periodic trimmer to what is wanted of it, when the
	 * guest does not run as wanted. `trim_applied_` is what the guest last took
	 * (the wishes come first, so that a request that does not go through
	 * leaves the difference for the next call, and for the resume, to settle).
	 * `wait` is the application's synchronous request, which `force` sends
	 * whether or not the guest runs as wanted; the memory pressure response
	 * does not wait for the guest. Under the lifecycle mutex, with the guest
	 * not quiesced. */
	[[nodiscard]] ::uml::device::GuestRequest apply_trimming(bool wait,
		bool force = false) noexcept
	{
		using ::uml::device::GuestRequest;
		const bool wanted = trim_wanted_by_app_ || trim_wanted_by_pressure_.load();
		if (wanted == trim_applied_ && !force && !trim_requested_by_app_)
			return GuestRequest::done;
		::uml::runtime::note_host_wake();
		const GuestRequest request = wait ?
			::uml::device::set_guest_page_cache_trim(wanted) :
			::uml::device::set_guest_page_cache_trim_nowait(wanted);
		if (request == GuestRequest::done || request == GuestRequest::sent ||
		    request == GuestRequest::unanswered) {
			trim_applied_ = wanted;
			trim_requested_by_app_ = false;
		}
		return request;
	}

	[[nodiscard]] int trim_page_cache() noexcept
	{
		const LifecycleLock lock{*this};
		/* Quiescing already trimmed it, and a request would wake it. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none)
			return UML_EMULATOR_OK;
		::uml::runtime::note_host_wake();
		return guest_request_result(
			::uml::device::trim_guest_page_cache(guest_trim_wait_ms));
	}

	/* The guest's page-cache requests for the memory pressure response,
	 * which runs when the system says it is short of memory. A lifecycle
	 * call in progress, or waiting for the mutex, is not waited for and goes
	 * first (the response asks again shortly: it would otherwise be waited
	 * for, or wait for this one), and a guest the host holds suspended is not
	 * woken for a trim: quiescing trimmed it. The suspension is read under
	 * the mutex on every try. The gradual trimmer's requests are sent and not
	 * waited for, so that holding the mutex is brief; a trim of all the cache
	 * is waited for, bounded. */
	[[nodiscard]] ::uml::runtime::TrimOutcome trim_for_memory_pressure(
		::uml::runtime::TrimKind kind) noexcept
	{
		using ::uml::runtime::TrimKind;
		using ::uml::runtime::TrimOutcome;
		/* What the response last wanted of the trimmer is recorded before the
		 * lock is tried, so that a request that finds the guest held by a
		 * lifecycle call for longer than its retries last still reaches the
		 * guest: the resume and any later call settle the difference. */
		if (kind != TrimKind::once)
			trim_wanted_by_pressure_.store(kind == TrimKind::gradual_on);
		std::unique_lock lock{guest_lifecycle_mutex_, std::try_to_lock};
		if (!lock.owns_lock())
			return TrimOutcome::busy;
		/* The mutex has no queue: a lifecycle call that announced itself
		 * and waits for it goes first, whatever the order the system wakes
		 * the threads in. The response asks again shortly. */
		if (lifecycle_calls_.load() != 0U)
			return TrimOutcome::busy;
		if (kind != TrimKind::once)
			return set_trimming_for_pressure();
		if (guest_suspension_ != ::uml::device::GuestSuspension::none)
			return TrimOutcome::quiesced;
		::uml::runtime::note_host_wake();
		const TrimOutcome outcome = trim_outcome_of(
			::uml::device::trim_guest_page_cache(guest_trim_wait_ms));
		/* Recorded under the mutex, after the request: a quiesce that waited
		 * meanwhile sees that its trim was sent. */
		if (outcome != TrimOutcome::unreachable)
			pressure_trims_sent_.fetch_add(1);
		return outcome;
	}

	/* The guest's periodic trimmer for the response: it stays on while the
	 * application wants it. The request is sent and not waited for, as the
	 * lifecycle mutex is held. Under the lifecycle mutex. */
	[[nodiscard]] ::uml::runtime::TrimOutcome set_trimming_for_pressure() noexcept
	{
		using ::uml::runtime::TrimOutcome;
		/* The wish was recorded by the caller, before it took the lock. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none)
			return TrimOutcome::quiesced;
		return trim_outcome_of(apply_trimming(false));
	}

	[[nodiscard]] static ::uml::runtime::TrimOutcome trim_outcome_of(
		::uml::device::GuestRequest request) noexcept
	{
		using ::uml::runtime::TrimOutcome;
		switch (request) {
		case ::uml::device::GuestRequest::done:
			return TrimOutcome::done;
		case ::uml::device::GuestRequest::sent:
			return TrimOutcome::sent;
		case ::uml::device::GuestRequest::unanswered:
			return TrimOutcome::unanswered;
		case ::uml::device::GuestRequest::refused:
			return TrimOutcome::refused;
		case ::uml::device::GuestRequest::busy:
			return TrimOutcome::busy;
		case ::uml::device::GuestRequest::unreachable:
			break;
		}
		return TrimOutcome::unreachable;
	}

	static ::uml::runtime::TrimOutcome trim_for_memory_pressure_of(
		void *emulator, ::uml::runtime::TrimKind kind) noexcept
	{
		return static_cast<Emulator *>(emulator)->trim_for_memory_pressure(
			kind);
	}

	/* The figures of this handle's running guest: its route is the one
	 * process-wide active run, which is committed once the run is admitted
	 * and withdrawn before it returns. Nothing here waits for the guest. */
	[[nodiscard]] int read_statistics(uml_emulator_statistics &statistics)
		const noexcept
	{
		if (::uml::runtime::current_context() != &runtime_)
			return -UML_EMULATOR_UNSUPPORTED;
		const auto cpu = ::uml::runtime::guest_cpu_time();
		if (!cpu.listed)
			return -UML_EMULATOR_RUNTIME_ERROR;
		const auto ram = ::uml::runtime::guest_ram_usage();
		statistics = {
			.struct_size = sizeof(statistics),
			.guest_cpu_count = cpu.carriers,
			.guest_cpu_time_ns = cpu.nanoseconds,
			.guest_ram_bytes = ram ? ram->ram_bytes : 0U,
			.guest_ram_held_bytes = ram ? ram->held_bytes : 0U,
		};
		return UML_EMULATOR_OK;
	}

	/* A console request wakes a suspended guest, so it is sent only under
	 * the lifecycle mutex, which says whether the host has quiesced the
	 * guest, and never while another lifecycle call holds it: that call
	 * would otherwise be waited for, or wait for this one. The wait for the
	 * reply is outside the mutex, so a quiesce that starts after the send
	 * does not wait for it. */
	[[nodiscard]] int read_guest_memory(
		uml_emulator_guest_memory &memory) noexcept
	{
		std::unique_lock lock{guest_lifecycle_mutex_, std::try_to_lock};
		if (!lock.owns_lock() ||
			guest_suspension_ != ::uml::device::GuestSuspension::none)
			return -UML_EMULATOR_UNSUPPORTED;
		auto query = ::uml::device::GuestMemoryQuery::start();
		lock.unlock();
		if (!query)
			return -UML_EMULATOR_UNSUPPORTED;
		::uml::device::GuestMemoryFigures figures;
		switch (query->await(guest_memory_wait, figures)) {
		case ::uml::device::GuestMemoryResult::done:
			memory = {
				.struct_size = sizeof(memory),
				.reserved = 0U,
				.total_bytes = figures.total_bytes,
				.available_bytes = figures.available_bytes,
			};
			return UML_EMULATOR_OK;
		case ::uml::device::GuestMemoryResult::unreachable:
		case ::uml::device::GuestMemoryResult::refused:
			return -UML_EMULATOR_UNSUPPORTED;
		case ::uml::device::GuestMemoryResult::unanswered:
		case ::uml::device::GuestMemoryResult::malformed:
			break;
		}
		return -UML_EMULATOR_RUNTIME_ERROR;
	}

	void wake() noexcept
	{
		const LifecycleLock lock{*this};
		/* A quiesced guest stays put until the host resumes it. */
		if (guest_suspension_ == ::uml::device::GuestSuspension::none) {
			/* The stall watch sleeps while every carrier is idle: it is
			 * told first that one is about to be woken, so that a carrier
			 * the system then does not schedule is seen. */
			::uml::runtime::note_host_wake();
			(void)::uml::device::wake_guest();
		}
	}

	/* Whether a guest runs on this handle: the process-wide active run is
	 * its own, committed once the run is admitted and withdrawn before it
	 * returns. */
	[[nodiscard]] bool guest_running() const noexcept
	{
		return ::uml::runtime::current_context() == &runtime_;
	}

	[[nodiscard]] ::uml::runtime::ExecutionEngine
	execution_engine() const noexcept
	{
		return runtime_.execution_engine();
	}

	[[nodiscard]] uml_emulator_run_end run_end() const noexcept
	{
		std::lock_guard lock{run_end_mutex_};
		return run_end_;
	}

	/* Whether this handle's last run was refused for want of memory before
	 * its kernel started (start_refusal.hpp). */
	[[nodiscard]] bool start_refused_for_memory() const noexcept
	{
		return start_refused_for_memory_.load(std::memory_order_acquire);
	}

#if UML_EMULATOR_TESTING
	/* The lifecycle calls that have announced themselves and not ended. */
	[[nodiscard]] unsigned int lifecycle_calls() const noexcept
	{
		return lifecycle_calls_.load();
	}

	/* What a hand-back does to the handle's state. */
	void reset_guest_state_for_testing() noexcept
	{
		reset_guest_state();
	}

	/* Whether the handle runs the linked kernel section and holds no copy. */
	[[nodiscard]] bool kernel_image_borrowed() const noexcept
	{
		return kernel_.empty() && !kernel_view_.empty();
	}

	void use_hibernation(
		std::unique_ptr<::uml::runtime::HibernationSteps> steps,
		::uml::runtime::GuestHibernation::FailureHandler on_failure)
	{
		const LifecycleLock lock{*this};
		publish_hibernation(nullptr);
		hibernation_ = std::make_unique<::uml::runtime::GuestHibernation>(
			std::move(steps), on_failure);
		publish_hibernation(hibernation_.get());
	}
#endif

private:
	/* Hands the process's guest back, for the run that boots it again after a
	 * restart and for an explicit release after a power-off. A refusal
	 * changes nothing the caller cannot repeat: the guest stays to be handed
	 * back by a later run or release, once the caller has stopped what holds
	 * it. */
	[[nodiscard]] int hand_back_guest() noexcept
	{
		switch (::uml::runtime::guest_hand_back_block()) {
		case ::uml::runtime::GuestHandBackBlock::none:
			break;
		case ::uml::runtime::GuestHandBackBlock::borrowed_ram:
			(void)::dprintf(STDERR_FILENO,
				"Darwin: the guest cannot be handed back: a host "
				"service still borrows its RAM; stop the device "
				"services first\n");
			return -UML_EMULATOR_RUNTIME_ERROR;
		case ::uml::runtime::GuestHandBackBlock::kernel_threads:
			(void)::dprintf(STDERR_FILENO,
				"Darwin: the guest cannot be handed back: a kernel "
				"thread is still running\n");
			return -UML_EMULATOR_RUNTIME_ERROR;
		}
		/* The handle's state describes this kernel and its RAM; the
		 * hibernation holds the commitment the hand-back retires. */
		reset_guest_state();
		::uml::runtime::GuestHandBack hand_back;
		bool handed_back;
		{
			/* The commitment of the RAM is deleted in here, and a reader of
			 * the statistics must not meet it half gone. */
			const ::uml::runtime::GuestRamRetirementScope retirement;
			handed_back = ::uml::runtime::hand_back_guest(hand_back);
		}
		if (!handed_back) {
			(void)::dprintf(STDERR_FILENO,
				"Darwin: the guest could not be handed back: "
				"errno=%d\n", errno);
			return -UML_EMULATOR_RUNTIME_ERROR;
		}
		const auto &released = hand_back.released;
		(void)::dprintf(STDERR_FILENO,
			"Darwin: guest handed back: descriptors=%zu keys=%zu "
			"allocations=%zu (%zu bytes) mappings=%zu (%zu bytes) "
			"sbrk=%zu guest_files=%zu heaps=%zu (%zu bytes) "
			"leases=%zu\n",
			released.descriptors, released.keys, released.allocations,
			released.allocation_bytes, released.mappings,
			released.mapping_bytes, released.sbrk_calls,
			hand_back.guest_files, hand_back.heaps,
			hand_back.heap_bytes, hand_back.heap_leases);
		restartable_ = false;
#if UML_EMULATOR_TESTING
		if (const auto hook = guest_hand_back_hook.load(
			    std::memory_order_acquire); hook != nullptr)
			hook(guest_hand_back_hook_context.load(
				std::memory_order_acquire),
				{.descriptors = released.descriptors,
				 .keys = released.keys,
				 .allocations = released.allocations,
				 .allocation_bytes = released.allocation_bytes,
				 .mappings = released.mappings,
				 .mapping_bytes = released.mapping_bytes,
				 .sbrk_calls = released.sbrk_calls,
				 .guest_files = hand_back.guest_files,
				 .heaps = hand_back.heaps,
				 .heap_bytes = hand_back.heap_bytes,
				 .heap_leases = hand_back.heap_leases});
#endif
		return 0;
	}

	/* What the handle remembers of its kernel: a suspension, a hibernation
	 * of its RAM, a page-cache request waiting for the resume. The next
	 * kernel starts without any of them. The hibernation image of the RAM is
	 * dropped with the hibernation, and the next boot makes its own. */
	void reset_guest_state() noexcept
	{
		request_hibernation_abort();
		const LifecycleLock lock{*this};
		/* A stop or a resume on another thread may be inside the abort
		 * request: withdrawing the pointer waits for it to return, and
		 * none starts after, so the object is destroyed unobserved. */
		publish_hibernation(nullptr);
		hibernation_.reset();
		if (guest_hold_ == GuestHold::fence)
			guest_fence_->release();
		guest_hold_ = GuestHold::none;
		guest_hold_failure_logged_ = false;
		guest_suspension_ = ::uml::device::GuestSuspension::none;
		guest_file_locks_released_ = false;
		/* The next kernel starts with its trimmer off, and nothing wants it
		 * on. */
		trim_wanted_by_app_ = false;
		trim_wanted_by_pressure_.store(false);
		trim_applied_ = false;
		trim_requested_by_app_ = false;
	}

	/* A run that begins on the handle starts from no end. */
	void clear_run_end() noexcept
	{
		std::lock_guard lock{run_end_mutex_};
		run_end_ = {
			.struct_size = sizeof(run_end_),
			.kind = UML_EMULATOR_RUN_END_NONE,
			.restart_command = {},
			.boot_completed = 0,
			.init_observed = 0,
		};
	}

	/* Says how the run's guest ended, once its kernel has returned. The host's
	 * own stop request wins over what the guest said on its way out, and a
	 * kernel that said nothing failed. */
	void record_run_end() noexcept
	{
		uml_emulator_run_end end{
			.struct_size = sizeof(end),
			.kind = UML_EMULATOR_RUN_END_FAILED,
			.restart_command = {},
			.boot_completed =
				::uml::runtime::console_boot_completed() ? 1U : 0U,
			.init_observed =
				::uml::runtime::console_init_observed() ? 1U : 0U,
		};
		const auto shutdown = runtime_.guest_shutdown();
		if (runtime_.stop_requested()) {
			end.kind = UML_EMULATOR_RUN_END_STOPPED;
		} else if (shutdown.kind ==
			   ::uml::runtime::GuestShutdownKind::restart) {
			end.kind = UML_EMULATOR_RUN_END_RESTART;
			static_assert(sizeof(end.restart_command) >
				      ::uml::runtime::guest_shutdown_command_limit);
			std::copy_n(shutdown.command,
				    ::uml::runtime::guest_shutdown_command_limit,
				    end.restart_command);
		} else if (shutdown.kind !=
			   ::uml::runtime::GuestShutdownKind::none) {
			end.kind = UML_EMULATOR_RUN_END_POWER_OFF;
		}
		restartable_ = end.kind == UML_EMULATOR_RUN_END_RESTART;
		power_off_pending_ = end.kind == UML_EMULATOR_RUN_END_POWER_OFF &&
			::uml::runtime::process_guest_spent();
		using ::uml::runtime::SessionEnd;
		::uml::runtime::end_session_record(
			end.kind == UML_EMULATOR_RUN_END_STOPPED ? SessionEnd::stopped :
			end.kind == UML_EMULATOR_RUN_END_RESTART ?
				SessionEnd::restarted :
			end.kind == UML_EMULATOR_RUN_END_POWER_OFF ?
				SessionEnd::powered_off : SessionEnd::failed, 0);
#if DARWIN_UML_X86_64_GUEST
		::uml::runtime::report_x86_run_counters();
#endif
		std::lock_guard lock{run_end_mutex_};
		run_end_ = end;
	}

	/* Records the end of the run that launched a kernel, on every way out of
	 * the run, an exception included. */
	class RunEndRecorder final {
	public:
		explicit RunEndRecorder(Emulator &emulator) noexcept
			: emulator_{emulator}
		{
		}
		~RunEndRecorder()
		{
			{
				const LifecycleLock lock{emulator_};
				(void)::uml::runtime::release_guest_file_locks(true);
				emulator_.guest_file_locks_released_ = false;
			}
			emulator_.record_run_end();
		}
		RunEndRecorder(const RunEndRecorder &) = delete;
		RunEndRecorder &operator=(const RunEndRecorder &) = delete;

	private:
		Emulator &emulator_;
	};

	/* Before the guest boots: what the options say about hibernating, and
	 * the images a process that ended in a hibernation left behind. */
	void prepare_guest_hibernation(::uml::runtime::HibernationMode mode)
	{
		const LifecycleLock lock{*this};
		hibernation_mode_ = mode;
		if (mode != ::uml::runtime::HibernationMode::off)
			(void)::uml::runtime::remove_stale_hibernation_images(
				::uml::runtime::hibernation_directory());
	}

	/* What a confirmed system suspension has the host do, last, with the guest
	 * asleep and its files on disk. The guest's disks are the host's to keep:
	 * the kernel's own wake sources (the RTC alarm, VNC input) end a suspend
	 * the host asked for, so once the host's locks on the disks are gone,
	 * only a hold on every thread that can run the guest keeps it from
	 * writing them before the resume has locked them again and found them
	 * unchanged. The locks therefore leave only with the threads held, and
	 * stay where they cannot be. */
	void settle_suspended_guest(bool resident) noexcept
	{
		if (!hold_guest_threads(resident)) {
			if (!std::exchange(guest_hold_failure_logged_, true))
				write_line("Darwin: the guest's disk locks stay held: "
					"its threads could not be held (reason ",
					static_cast<unsigned>(guest_hold_failure_), ")");
			return;
		}
		/* The locks leave first: a process holding one when the system
		 * suspends it is ended, and the write can take a while. */
		const unsigned failed = ::uml::runtime::release_guest_file_locks();
		guest_file_locks_released_ = true;
		if (failed != 0U)
			write_line("Darwin: ", failed, " guest disk lock(s) could not "
				"be released; the system may end the process while it "
				"is suspended");
		if (guest_hold_ == GuestHold::hibernation)
			hibernation_->write();
	}

	/* Whether the host holds the guest, suspended by the guest's own word and
	 * with every thread that can run it held: the only state in which a saved
	 * outcome still describes it. A guest whose suspension is unconfirmed, or
	 * whose threads could not be held, can run and write on its own. */
	[[nodiscard]] bool guest_held() const noexcept
	{
		return guest_suspension_ == ::uml::device::GuestSuspension::system &&
			guest_hold_ != GuestHold::none;
	}

	/* Holds every thread that can touch the guest's RAM or disks until
	 * `release_guest_hold`. The hibernation does it where the RAM is wired
	 * (or a mode forces it), and goes on to write the RAM out; elsewhere, or
	 * for a `resident` quiesce, a fence of the handle's own does, and nothing
	 * is written. False, with everything running, when the threads could not
	 * be held. */
	[[nodiscard]] bool hold_guest_threads(bool resident) noexcept
	{
		if (guest_hold_ != GuestHold::none)
			return true;
		if (!resident && hibernation_ == nullptr &&
		    hibernation_mode_ != ::uml::runtime::HibernationMode::off) {
			try {
				hibernation_ = ::uml::runtime::GuestHibernation::create(
					hibernation_mode_,
					::uml::runtime::hibernation_directory());
			} catch (...) {
				return false;
			}
			publish_hibernation(hibernation_.get());
		}
		if (!resident && hibernation_ != nullptr) {
			switch (hibernation_->hold(
				&::uml::runtime::guest_file_registry_idle)) {
			case ::uml::runtime::HibernationHold::held:
				guest_hold_ = GuestHold::hibernation;
				return true;
			case ::uml::runtime::HibernationHold::failed:
				guest_hold_failure_ = hibernation_->report().hold_failure;
				return false;
			case ::uml::runtime::HibernationHold::unavailable:
				break;
			}
		}
		try {
			if (!guest_fence_)
				guest_fence_ = std::make_unique<::uml::runtime::ThreadFence>();
		} catch (...) {
			return false;
		}
		if (!guest_fence_->hold(::uml::runtime::guest_hold_budget_ms,
			    &registry_idle)) {
			guest_hold_failure_ = guest_fence_->report().failure;
			return false;
		}
		guest_hold_ = GuestHold::fence;
		return true;
	}

	/* Lets the held threads run, after the hibernation has brought the RAM
	 * back. False when it could not: nothing runs then. */
	[[nodiscard]] bool release_guest_hold() noexcept
	{
		if (hibernation_ != nullptr && !hibernation_->restore())
			return false;
		if (guest_hold_ == GuestHold::fence)
			guest_fence_->release();
		guest_hold_ = GuestHold::none;
		guest_hold_failure_logged_ = false;
		return true;
	}

	/* Names the hibernation the abort request may reach, or none. The
	 * request runs without the lifecycle mutex, which a hibernation in
	 * progress holds, so the pointer has a mutex of its own: held across the
	 * request, which only sets a flag, and across the withdrawal that
	 * precedes the object's destruction. */
	void publish_hibernation(
		::uml::runtime::GuestHibernation *hibernation) noexcept
	{
		std::lock_guard lock{hibernation_abort_mutex_};
		hibernation_pointer_ = hibernation;
	}

	void request_hibernation_abort() noexcept
	{
		std::lock_guard lock{hibernation_abort_mutex_};
		if (hibernation_pointer_ != nullptr)
			hibernation_pointer_->request_abort();
	}

	void bring_back_guest_ram() noexcept
	{
		request_hibernation_abort();
		const LifecycleLock lock{*this};
		(void)bring_back_guest_locked();
	}

	/* The disks locked and verified again, then the RAM, then the threads:
	 * the guest runs on nothing the host has not checked. False when the RAM
	 * could not be brought back. */
	[[nodiscard]] bool bring_back_guest_locked() noexcept
	{
		if (guest_file_locks_released_) {
			::uml::runtime::restore_guest_file_locks();
			guest_file_locks_released_ = false;
		}
		return release_guest_hold();
	}

	[[nodiscard]] static uml::network::Provider network_provider(
		const uml_emulator_config &config) noexcept
	{
		if (config.network != nullptr)
			return {
				.context = config.network->context,
				.open = config.network->open,
				.irq_fd = config.network->irq_fd,
				.read = config.network->read,
				.write = config.network->write,
			};
		return {};
	}

	std::string rootfs_;
	/* The handle's own copy of the kernel image; empty when it borrows. */
	std::vector<std::byte> kernel_;
	/* The image the handle runs: `kernel_`, or the linked kernel section. */
	std::span<const std::byte> kernel_view_;
	::uml::runtime::Context runtime_;
	uml::network::Provider network_;
	uml::network::ProviderContext network_context_;
	std::timed_mutex guest_lifecycle_mutex_;
	/* The lifecycle calls that have announced themselves and not ended: they
	 * wait for the mutex or hold it. A pressure trim, which only tries the
	 * mutex, gives way while one is counted. */
	std::atomic<unsigned int> lifecycle_calls_{0};
	/* The pressure trims that reached the guest, sent or sent and answered;
	 * changed with the mutex held. */
	std::atomic<std::uint64_t> pressure_trims_sent_{0};

	/* The blocking take of the lifecycle mutex: the call is counted before
	 * it waits, and stays counted until it lets go, on every way out. */
	class LifecycleLock final {
	public:
		explicit LifecycleLock(Emulator &emulator) noexcept
			: emulator_{emulator}
		{
			emulator_.lifecycle_calls_.fetch_add(1);
			emulator_.guest_lifecycle_mutex_.lock();
		}

		~LifecycleLock()
		{
			emulator_.lifecycle_calls_.fetch_sub(1);
			emulator_.guest_lifecycle_mutex_.unlock();
		}

		LifecycleLock(const LifecycleLock &) = delete;
		LifecycleLock &operator=(const LifecycleLock &) = delete;

	private:
		Emulator &emulator_;
	};

	/* The take of the lifecycle mutex that gives up after `wait`: counted
	 * while it waits and while it holds the mutex, like LifecycleLock, and
	 * not at all once it has given up. */
	class BoundedLifecycleLock final {
	public:
		BoundedLifecycleLock(Emulator &emulator,
			std::chrono::milliseconds wait) noexcept
			: emulator_{emulator}
		{
			emulator_.lifecycle_calls_.fetch_add(1);
			held_ = emulator_.guest_lifecycle_mutex_.try_lock_for(wait);
			if (!held_)
				emulator_.lifecycle_calls_.fetch_sub(1);
		}

		~BoundedLifecycleLock()
		{
			if (!held_)
				return;
			emulator_.lifecycle_calls_.fetch_sub(1);
			emulator_.guest_lifecycle_mutex_.unlock();
		}

		BoundedLifecycleLock(const BoundedLifecycleLock &) = delete;
		BoundedLifecycleLock &operator=(const BoundedLifecycleLock &) = delete;

		[[nodiscard]] bool held() const noexcept { return held_; }

	private:
		Emulator &emulator_;
		bool held_{};
	};
	bool guest_file_locks_released_{};
	/* What holds the quiesced guest's threads: its hibernation, or a fence
	 * of the handle's own where the RAM is not hibernated. Guarded by the
	 * lifecycle mutex, like the fence. */
	enum class GuestHold { none, hibernation, fence };
	GuestHold guest_hold_{GuestHold::none};
	std::unique_ptr<::uml::runtime::ThreadFence> guest_fence_;
	int guest_hold_failure_{};
	bool guest_hold_failure_logged_{};
	::uml::device::GuestSuspension guest_suspension_ {
		::uml::device::GuestSuspension::none};
	/* What the quiesce that suspended, or tried to suspend, the guest
	 * reported; a call that finds the guest held (guest_held()) reports it
	 * again. Guarded by the lifecycle mutex. */
	SettleOutcome quiesce_outcome_ {::uml::runtime::GuestStorage::unconfirmed,
		::uml::runtime::SettleSuspension::not_suspended};
	/* Who wants the guest's periodic trimmer on: the application, through
	 * uml_emulator_set_page_cache_trimming, and the memory pressure response;
	 * and whether the guest last took a request to run it. They differ while a
	 * request is deferred (the guest is quiesced) or did not go through.
	 * Guarded by the lifecycle mutex. */
	bool trim_wanted_by_app_{};
	/* Atomic: the response records its wish before it tries the mutex. */
	std::atomic<bool> trim_wanted_by_pressure_{false};
	bool trim_applied_{};
	/* The application asked for a change while the guest was quiesced, and the
	 * guest has not been told since: the resume sends it even when it equals
	 * what the guest last took (the application's request is a statement, not
	 * only a difference). */
	bool trim_requested_by_app_{};
	::uml::runtime::HibernationMode hibernation_mode_ {
		::uml::runtime::HibernationMode::automatic};
	/* Made by the first quiesce that hibernates. Guarded by the lifecycle
	 * mutex; the pointer is also published, under its own mutex, for the
	 * abort request, which a stop or a resume sends before it waits for the
	 * lifecycle mutex. The mutex is a leaf: nothing is taken under it but the
	 * request itself. */
	std::unique_ptr<::uml::runtime::GuestHibernation> hibernation_;
	std::mutex hibernation_abort_mutex_;
	::uml::runtime::GuestHibernation *hibernation_pointer_{};
	/* The handle's last run ended with a restart, so its guest goes back to
	 * the process when the handle runs again. Only the handle's one run at a
	 * time reads or writes it. */
	bool restartable_{};
	/* Only the handle that owns a cleanly powered-off guest can release it.
	 * Clearing this on success prevents an old handle from releasing a later
	 * guest belonging to another handle. */
	bool power_off_pending_{};
	/* Set where a run is refused for want of memory before its kernel starts,
	 * and cleared when the next run of the handle begins. */
	std::atomic_bool start_refused_for_memory_{false};

	/* The result of a run refused for want of memory before its kernel
	 * started, with the mark that says so: the same number also comes from a
	 * failed allocation of the library's own. */
	[[nodiscard]] int refused_for_memory() noexcept
	{
		start_refused_for_memory_.store(true, std::memory_order_release);
		return -UML_EMULATOR_OUT_OF_MEMORY;
	}

	/* How the handle's most recent run ended, for uml_emulator_get_run_end. */
	mutable std::mutex run_end_mutex_;
	uml_emulator_run_end run_end_ {
		.struct_size = sizeof(uml_emulator_run_end),
		.kind = UML_EMULATOR_RUN_END_NONE,
		.restart_command = {},
		.boot_completed = 0,
		.init_observed = 0,
	};
};

} // namespace uml::emulator

struct uml_emulator {
	explicit uml_emulator(const uml_emulator_config &config) : implementation(config) {}

	uml::emulator::Emulator implementation;
};

namespace uml::emulator {

bool start_refused_for_memory(const uml_emulator *emulator) noexcept
{
	return emulator != nullptr &&
		emulator->implementation.start_refused_for_memory();
}

} // namespace uml::emulator

#if UML_EMULATOR_TESTING
namespace uml::emulator::testing {

void use_hibernation(uml_emulator *emulator,
	std::unique_ptr<::uml::runtime::HibernationSteps> steps,
	void (*on_failure)() noexcept)
{
	emulator->implementation.use_hibernation(std::move(steps), on_failure);
}

bool kernel_image_borrowed(const uml_emulator *emulator) noexcept
{
	return emulator->implementation.kernel_image_borrowed();
}

::uml::runtime::TrimOutcome trim_for_memory_pressure(
	uml_emulator *emulator) noexcept
{
	return emulator->implementation.trim_for_memory_pressure(
		::uml::runtime::TrimKind::once);
}

::uml::runtime::TrimOutcome trim_for_memory_pressure(
	uml_emulator *emulator, ::uml::runtime::TrimKind kind) noexcept
{
	return emulator->implementation.trim_for_memory_pressure(kind);
}

unsigned int lifecycle_calls(const uml_emulator *emulator) noexcept
{
	return emulator->implementation.lifecycle_calls();
}

void reset_guest_state(uml_emulator *emulator) noexcept
{
	emulator->implementation.reset_guest_state_for_testing();
}

void set_lifecycle_clock(std::chrono::steady_clock::time_point (*now)()
	noexcept) noexcept
{
	::uml::emulator::lifecycle_clock.store(
		now != nullptr ? now : &::uml::emulator::steady_now);
}

} // namespace uml::emulator::testing
#endif

extern "C" int uml_emulator_create(const uml_emulator_config *config,
				  uml_emulator **result)
{
	if (result == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*result = nullptr;
	if (config == nullptr ||
	    config->abi_version != UML_EMULATOR_ABI_VERSION ||
	    config->struct_size < sizeof(*config) ||
	    (config->kernel_image == nullptr && config->kernel_image_size != 0) ||
	    (config->network != nullptr &&
	     (config->network->struct_size < sizeof(*config->network) ||
	      config->network->open == nullptr ||
	      config->network->irq_fd == nullptr ||
	      config->network->read == nullptr ||
	      config->network->write == nullptr)))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	std::unique_ptr<uml_emulator> emulator;
	try {
		emulator.reset(new uml_emulator(*config));
	} catch (const std::bad_alloc &) {
		return -UML_EMULATOR_OUT_OF_MEMORY;
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
	*result = emulator.release();
	return UML_EMULATOR_OK;
}

extern "C" void uml_emulator_destroy(uml_emulator *emulator)
{
	std::unique_ptr<uml_emulator>{emulator};
}

extern "C" int uml_emulator_get_capabilities(
	const uml_emulator *emulator, uml_emulator_capabilities *capabilities)
{
	if (emulator == nullptr || capabilities == nullptr ||
	    capabilities->struct_size < sizeof(*capabilities))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	capabilities->abi_version = UML_EMULATOR_ABI_VERSION;
	capabilities->userspace_execution =
		static_cast<uml_emulator_execution_model>(
			emulator->implementation.execution_engine());
	const bool elf_kernel = UML_EMULATOR_HAS_KERNEL_RUNNER &&
		emulator->implementation.has_embedded_kernel();
	capabilities->can_run = elf_kernel;
	capabilities->sandbox_safe = !elf_kernel;
	capabilities->platform = UML_APPLE_PLATFORM_NAME;
	return UML_EMULATOR_OK;
}

extern "C" int uml_emulator_run(uml_emulator *emulator, int argc,
			       const char *const argv[], const char *const envp[])
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	try {
		return emulator->implementation.run(argc, argv, envp);
	} catch (const std::bad_alloc &) {
		return -UML_EMULATOR_OUT_OF_MEMORY;
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
}

extern "C" int uml_emulator_get_run_end(const uml_emulator *emulator,
					uml_emulator_run_end *end)
{
	if (emulator == nullptr || end == nullptr ||
	    end->struct_size < sizeof(*end))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*end = emulator->implementation.run_end();
	return UML_EMULATOR_OK;
}

extern "C" int uml_emulator_release_guest(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	try {
		return emulator->implementation.release_guest();
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
}

extern "C" void uml_emulator_request_stop(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.request_stop();
}

namespace {

/* A report is optional; one that is given must be one this build can fill. */
[[nodiscard]] bool settle_report_usable(const uml_emulator_settle *report)
	noexcept
{
	return report == nullptr || report->struct_size >= sizeof(*report);
}

void fill_settle_report(uml_emulator_settle *report,
	const ::uml::emulator::SettleOutcome &outcome) noexcept
{
	if (report == nullptr)
		return;
	report->reserved = 0U;
	report->guest_storage =
		static_cast<uml_emulator_guest_storage>(outcome.storage);
	report->guest_suspension =
		static_cast<uml_emulator_guest_suspension>(outcome.suspension);
}

} // namespace

extern "C" int uml_emulator_sync(uml_emulator *emulator,
	uml_emulator_settle *settle)
{
	if (emulator == nullptr || !settle_report_usable(settle))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	/* Ask the guest first. Only it can write back its own page cache, and
	 * flushing the host before it would push down a prefix of what the guest
	 * still holds. A guest without a management console simply does not
	 * answer, which leaves the host flush doing what it did before. */
	::uml::emulator::SettleOutcome outcome {};
	const int failed = emulator->implementation.sync(outcome);
	fill_settle_report(settle, outcome);
	return failed;
}

extern "C" int uml_emulator_quiesce(uml_emulator *emulator,
	uml_emulator_settle *settle)
{
	if (emulator == nullptr || !settle_report_usable(settle))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	::uml::emulator::SettleOutcome outcome {};
	const int failed = emulator->implementation.quiesce(outcome);
	fill_settle_report(settle, outcome);
	return failed;
}

extern "C" int uml_emulator_quiesce_resident(uml_emulator *emulator,
	uml_emulator_settle *settle)
{
	if (emulator == nullptr || !settle_report_usable(settle))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	::uml::emulator::SettleOutcome outcome {};
	const int failed = emulator->implementation.quiesce(outcome, true);
	fill_settle_report(settle, outcome);
	return failed;
}

extern "C" void uml_emulator_resume(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.resume();
}

extern "C" void uml_emulator_wake(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.wake();
}

extern "C" int uml_emulator_set_page_cache_trimming(uml_emulator *emulator,
	int enabled)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.set_page_cache_trimming(enabled != 0);
}

extern "C" int uml_emulator_trim_page_cache(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.trim_page_cache();
}

extern "C" int uml_emulator_get_statistics(const uml_emulator *emulator,
	uml_emulator_statistics *statistics)
{
	if (emulator == nullptr || statistics == nullptr ||
		statistics->struct_size < sizeof(*statistics))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.read_statistics(*statistics);
}

extern "C" int uml_emulator_get_guest_memory(uml_emulator *emulator,
	uml_emulator_guest_memory *memory)
{
	if (emulator == nullptr || memory == nullptr ||
		memory->struct_size < sizeof(*memory))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.read_guest_memory(*memory);
}
