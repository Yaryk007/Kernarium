// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

/* The only first-party owner of executable memory. It maps JIT memory, opens
 * and closes the calling thread's write window over it, and publishes the
 * instructions written there. JIT engines reach executable memory only
 * through these calls; the gadget engines and shared CPU code never link
 * them. allocate, release and set_writable have the shapes of FEXCore's
 * executable-memory hooks.
 *
 * There are two routes to JIT memory. On macOS a mapping made for JIT is written
 * inside the calling thread's write window. On an iOS device that is not an
 * App Store build, a debugger prepares one region for the process once, and
 * the memory is written through a second, writable mapping of the same pages
 * (writable_alias) with no write window. The second route does not exist in
 * the App Store build, which does not compile or link it.
 */
namespace uml::cpu::jit_memory {

/* Whether this process can run JIT code. On macOS: a publication route exists
 * and a JIT mapping succeeds; the answer is probed once per process, and a
 * process that can claims the window allocate serves from then, before other
 * mappings fill the range near this image. On an iOS device: the region
 * prepare_debugger_region made is ready.
 */
[[nodiscard]] bool available() noexcept;

/* Returns size bytes of JIT memory that executes, or nullptr. On macOS it is
 * also readable and writable, and a thread writes it only inside its write
 * window; the memory comes from one window near this image, because Apple
 * cores predict an indirect branch's target cheaply only near the branch. On
 * an iOS device it comes from the prepared region, in whole 16 KiB granules,
 * and is written through writable_alias. Memory released and served again
 * may still hold its old bytes.
 */
[[nodiscard]] void *allocate(std::size_t size) noexcept;

/* The calls below end in the C library, which the compiler cannot prove
 * nothrow. They are not noexcept, so none compiles to a frame with its own
 * terminate handler.
 */

/* Returns memory allocate served; size is the size it was asked for. */
void release(void *mapping, std::size_t size);

/* Opens (true) or closes (false) the calling thread's write window over every
 * JIT mapping. While the window is open, the thread cannot execute JIT code.
 * On an iOS device there is no window: the call does nothing.
 */
void set_writable(bool writable);

/* The address to write the instructions of the JIT memory at `executable`
 * through, which allocate served. On macOS it is `executable`, written inside
 * the write window. On an iOS device it is the same bytes in the region's
 * writable mapping, an address that never executes. Instructions are still
 * made visible at `executable` (invalidate_instruction_cache).
 */
[[nodiscard]] void *writable_alias(void *executable) noexcept;

/* Makes size bytes of instructions written at code visible to instruction
 * fetch. Call it after closing the write window.
 */
void invalidate_instruction_cache(void *code, std::size_t size);

/* Reserves size bytes of ordinary read-write data between two inaccessible
 * guard regions, each one host page long, or returns nullptr. A JIT keeps
 * data its generated code addresses unchecked there, so an engine fault that
 * runs off either end stops at a guard instead of corrupting the heap. size
 * must be a multiple of the host page size.
 */
[[nodiscard]] void *reserve_guarded(std::size_t size) noexcept;

/* Releases a reservation reserve_guarded made of size bytes. */
void release_guarded(void *data, std::size_t size);

/* How an iOS device gets its JIT memory. The App Store build has none of it. */
enum class RegionStatus : std::uint8_t {
	/* The region is mapped, a debugger blessed it, the process detached from
	 * it and code written through the alias ran from the executable mapping.
	 * It stays so for the life of the process. */
	ready,
	/* This product has no such route: macOS, the Simulator, an App Store build.
	 */
	unsupported,
	/* No debugger is attached to the process (P_TRACED is clear), so no
	 * request is made. */
	no_debugger,
	/* The region could not be mapped, the debugger did not bless it in time,
	 * the pages could not be wired, or code did not run from them. */
	failed,
	/* An earlier call prepared a region smaller than this one, and the
	 * debugger is gone. */
	smaller,
};

/* Whether this build has the route (RegionStatus::unsupported otherwise). */
[[nodiscard]] bool debugger_route() noexcept;

/* Whether a debugger is attached to this process (P_TRACED). */
[[nodiscard]] bool debugger_attached() noexcept;

/* Prepares, once per process, one region of at least `bytes` for JIT memory.
 * It maps the region read-execute near this image and below 1 TiB, makes its
 * writable alias, asks the debugger to bless each of its pages, tells the
 * debugger to detach, wires the pages, and checks that code written through
 * the alias runs. A debugger that does not answer within a bound fails the
 * call. A later call with no more bytes than the region holds returns ready
 * at once; one with more returns smaller. It blocks for the seconds the
 * debugger takes, so call it from a worker thread, and never from a thread
 * that executes JIT code.
 */
[[nodiscard]] RegionStatus prepare_debugger_region(std::size_t bytes) noexcept;

/* Tells an attached debugger that services this protocol to detach, so a
 * process that will not use JIT memory does not stay under it. Does nothing
 * when no debugger is attached or the route does not exist. */
void release_debugger() noexcept;

/* The bytes of the prepared region, zero before it is ready. */
[[nodiscard]] std::size_t prepared_region_bytes() noexcept;

} // namespace uml::cpu::jit_memory
