/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_STATIC_MMU_H
#define UML_DARWIN_STATIC_MMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum darwin_uml_static_access {
	DARWIN_UML_STATIC_READ = 0,
	DARWIN_UML_STATIC_WRITE = 1,
	DARWIN_UML_STATIC_EXECUTE = 2,
};

enum darwin_uml_static_translation_result {
	DARWIN_UML_STATIC_TRANSLATED = 0,
	DARWIN_UML_STATIC_UNMAPPED = 1,
	DARWIN_UML_STATIC_PERMISSION = 2,
	DARWIN_UML_STATIC_HOST_ERROR = -1,
};

struct darwin_uml_static_mapping_change {
	uint64_t address;
	uint64_t length;
};

struct darwin_uml_static_code_change {
	uint64_t address;
	uint64_t length;
};

struct darwin_uml_static_sequences {
	uint64_t mapping_changes;
	uint64_t code_changes;
	uint64_t execution_interrupt;
};

/* Flags of a code-page stamp. */
enum {
	/* The mapping the stamp was asked through carries the kernel's immutable
	 * fact. Only darwin_uml_static_vm_code_page() knows it: a stamp read by
	 * handle leaves it clear, because a handle names the physical page, not
	 * one of its mappings. */
	DARWIN_UML_STATIC_STAMP_IMMUTABLE = 1,
	/* The page's record cannot count its executable mappings exactly any
	 * more, so no verdict about its bytes may be kept. */
	DARWIN_UML_STATIC_STAMP_NEVER_SHARE = 2,
};

/* What one physical page says about itself. `identity` is never reused.
 * `version` moves at every publication and at every departure of a mapping.
 * `epoch` moves at a publication that names the page's frame and when the last
 * executable mapping of the page departs, and, for every page at once, at a
 * kernel write that names no frame (a target page its address space does not
 * map, or a frame Linux cannot name); nowhere else. */
struct darwin_uml_static_code_page_stamp {
	uint64_t identity;
	uint64_t version;
	uint64_t epoch;
	uint64_t flags;
};

int darwin_uml_static_vm_map(void *address, int fd,
			     unsigned long long offset, unsigned long length,
			     int protection, int guest_code,
			     int guest_writable);
int darwin_uml_static_vm_map_generation(unsigned long generation,
					 void *address, int fd,
					 unsigned long long offset,
					 unsigned long length,
					 int protection, int guest_code,
					 int guest_writable);
int darwin_uml_static_vm_protect(void *address, unsigned long length,
				 int protection);
int darwin_uml_static_vm_unmap(void *address, unsigned long length);
int darwin_uml_static_vm_unmap_generation(unsigned long generation,
					    void *address,
					    unsigned long length);
/* Discards every cached mapping without retiring the generation. A following
 * synchronized Linux page-table walk may republish it immediately. */
int darwin_uml_static_vm_flush_generation(unsigned long generation);
int darwin_uml_static_vm_activate(unsigned long generation);
int darwin_uml_static_vm_destroy(unsigned long generation);
/* 1 while the generation has an address space, 0 once it was destroyed (a
 * generation is never reused), negative if the host cannot say. */
int darwin_uml_static_vm_generation_live(unsigned long generation);
/* Advances whenever a generation is destroyed, in any guest of the process,
 * so one load tells a holder of per-generation state whether any generation
 * may have died since it last asked darwin_uml_static_vm_generation_live(). */
unsigned long long darwin_uml_static_vm_generation_retirements(void);
/* Unmaps the page at `address` when its mapping names other backing than
 * `fd` at `offset`; an absent or equal mapping is left as it is. The caller's
 * backing is never installed: which of the two is stale is unknown, and the
 * next access faults so a page-table walker replays the current PTE.
 */
int darwin_uml_static_vm_discard_if_stale(unsigned long address, int fd,
					  unsigned long long offset);
int darwin_uml_static_vm_copy_from_buffer(unsigned long address,
					   const void *buffer,
					   unsigned long length);
/* The same copy for a kernel write, a uaccess the kernel made through its own
 * alias of the page. It publishes a code change for a guest-code page only
 * once darwin_uml_static_vm_require_coherent_kernel_code_writes() was called:
 * arm64 and AArch32 code see such a write only after their own instruction
 * cache maintenance, which publishes it. */
int darwin_uml_static_vm_copy_kernel_write(unsigned long address,
					   const void *buffer,
					   unsigned long length);
/* Called before code whose instruction fetch is coherent with every store,
 * x86 code, is first translated; from then on, for the life of the process,
 * every kernel write into a guest-code page publishes a code change. */
void darwin_uml_static_vm_require_coherent_kernel_code_writes(void);
int darwin_uml_static_vm_retry_fault(unsigned long address, int execute,
				     int write);
int darwin_uml_static_vm_batch_begin(unsigned long generation);
int darwin_uml_static_vm_batch_end(void);
int darwin_uml_static_vm_walker_lock(unsigned long generation);
int darwin_uml_static_vm_walker_unlock(unsigned long generation);

/* Returns a host data pointer valid through the end of the translated guest
 * page. The mapping is never executable. Each vCPU observes concurrent
 * map/protect/unmap operations at its next bounded guest-dispatch boundary.
 */
int darwin_uml_static_vm_translate(unsigned long generation,
				   unsigned long address,
				   enum darwin_uml_static_access access,
				   void **host_address);
/* darwin_uml_static_vm_translate() for an execute access that also names the
 * holder word of the frame it translated: an 8-byte atomic, 8-byte aligned,
 * in which a translator registers its generation before it reads the frame's
 * bytes, so that a code synchronization by another process reaches it
 * (memory/mmu/code_holders.hpp has the word's layout). It lives as long as the
 * mapping does. `*holder` is null when the frame's backing cannot make one;
 * every publication on such a backing then goes to every generation, and
 * there is nothing to register. */
int darwin_uml_static_vm_translate_execute(unsigned long generation,
					   unsigned long address,
					   void **host_address, void **holder);
/* darwin_uml_static_vm_translate() through an execution context of the same
 * generation: while the context is entered, a guest address is resolved from
 * the generation it holds; otherwise, or for a null context, this is
 * darwin_uml_static_vm_translate(). */
int darwin_uml_static_vm_execution_context_translate(
	void *context, unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address);
/* Returns an opaque, mapping-lifetime handle for an executable physical page.
 * Callers may retain it only while their cached mapping sequence is current.
 */
int darwin_uml_static_vm_code_page(
	unsigned long generation, unsigned long address, void **handle,
	struct darwin_uml_static_code_page_stamp *stamp);
int darwin_uml_static_vm_code_page_stamp(
	const void *handle, struct darwin_uml_static_code_page_stamp *stamp);
void darwin_uml_static_vm_invalidate_code_page(void *handle);
uint64_t darwin_uml_static_vm_changes(unsigned long generation);
/* Copies mapping ranges changed after `sequence` and returns their count.
 * A negative result means the caller must invalidate its complete TLB.  The
 * returned current_sequence is an atomic snapshot taken with the ranges.
 */
int darwin_uml_static_vm_mapping_changes(
	unsigned long generation, uint64_t sequence,
	struct darwin_uml_static_mapping_change *changes, size_t capacity,
	uint64_t *current_sequence);
/* A cached execution context keeps the generation alive and avoids resolving
 * thread-local address-space state at every translated-block boundary. One
 * thread begins and ends it, runs inside it nested with any other it enters,
 * and while it is entered the guest instructions the thread runs belong to its
 * generation: darwin_uml_static_vm_synchronize_code() and
 * darwin_uml_static_vm_invalidate_code() resolve a guest address there, not in
 * the generation the thread last activated. */
void *darwin_uml_static_vm_execution_context_create(unsigned long generation);
void darwin_uml_static_vm_execution_context_destroy(void *context);
int darwin_uml_static_vm_execution_context_begin(void *context,
						 unsigned long guest_pc);
int darwin_uml_static_vm_execution_context_end(void *context);
struct darwin_uml_static_sequences darwin_uml_static_vm_execution_context_sequences(
	void *context);
/* Where the context's code-change sequence lives: the word that
 * darwin_uml_static_vm_execution_context_sequences() reports as code_changes,
 * which only grows and stays valid while the context does. Translated code
 * reads it with one single-copy-atomic load at a context synchronization point
 * to tell whether guest code changed since its translations were validated. */
const uint64_t *darwin_uml_static_vm_execution_context_code_sequence(
	void *context);
/* Copies the guest ranges whose code changed after `sequence` and returns
 * their count.  A negative result means the caller must retire every
 * translation it holds for the address space; a range of length zero names
 * the whole address space, as a flush does.
 *
 * A mapping change publishes a range only for pages a translation can be
 * wrong about: a guest-code page that grants execute stops granting it,
 * goes away, or holds other bytes (another backing or another place in it).
 * A protection or writability change that keeps execute and the bytes, and
 * a mapping where none was, publish none; they appear only in the mapping
 * changes, which name the pages whose cached host addresses to retire.  A
 * published page also moves its code-page stamp, which no store can once the
 * page cannot be executed.
 */
int darwin_uml_static_vm_execution_context_code_change_ranges(
	void *context, uint64_t sequence,
	struct darwin_uml_static_code_change *changes, size_t capacity,
	uint64_t *current_sequence);
void darwin_uml_static_vm_invalidate_code(unsigned long address);
/* darwin_uml_static_vm_invalidate_code() for a guest's own code
 * synchronization (IC IVAU) that must say whether it found the frame: 1 when
 * the generation maps nothing at `address` (the entered execution context's
 * where the thread is inside one, else the active one), a guest page whose
 * page table entry the walker has not installed yet. Hardware faults such an
 * instruction like a load; the caller reports that fault so the kernel maps
 * the page, and runs the instruction again. Everything else publishes as
 * darwin_uml_static_vm_invalidate_code() does and answers 0. A host with no
 * address space for the thread answers -1, and the helper treats any non-zero
 * answer as an unmapped address and faults. */
int darwin_uml_static_vm_synchronize_code(unsigned long address);
int darwin_uml_static_vm_invalidate_code_generation(
	unsigned long generation, unsigned long address, unsigned long length);
/* darwin_uml_vm_invalidate_code_frame() for guest pages: Linux wrote
 * [address, address + length), within one guest page of the generation, through
 * its alias of the frame at `fd` and `offset`. The change is published at the
 * holders of that frame, whatever the generation maps at `address` now. A
 * negative `fd` names no frame. 0, or a negative value for a range or offset
 * that is invalid. */
int darwin_uml_static_vm_invalidate_code_frame(
	unsigned long generation, unsigned long address, unsigned long length,
	int fd, unsigned long long offset);
/* darwin_uml_vm_invalidate_code_frames(): Linux wrote the whole guest pages
 * [offset, offset + length) of the frame source `fd` through its own alias,
 * for no process. The change is published at the x86 holders of those frames
 * and returns at once while no x86 code may hold one; a negative `fd` names no
 * frame. 0, or a negative value for an offset or length that is not whole
 * pages. */
int darwin_uml_static_vm_invalidate_code_frames(
	int fd, unsigned long long offset, unsigned long long length);

#if defined(DARWIN_UML_X86_64_GUEST) && DARWIN_UML_X86_64_GUEST
/* The x86-64 guest's frame holders (darwin/memory/mmu/frame_accounting.hpp):
 * the locations at which a translator registered a physical page, so that a
 * store through any alias of it reaches them. Host-internal; not the kernel
 * bridge. */
struct darwin_uml_static_frame_location {
	uint64_t generation;
	uint64_t page;
};

struct darwin_uml_static_frame_hold {
	uint64_t record_identity;
	uint64_t debt_sequence;
	/* Out: how many locations the debt names (for CROWDED, how many the
	 * caller must make room for). */
	size_t owed;
	/* In: take the global fallback for a debt larger than the buffer
	 * instead of reporting CROWDED. */
	int global_ok;
	/* Out: the frame's record and its holder change count as the hold left
	 * them, for a caller that remembers a settled registration; null when
	 * no holder stands. */
	const void *record;
	uint64_t alias_changes;
};

enum darwin_uml_static_frame_hold_result {
	/* The holder stands and nothing is owed. */
	DARWIN_UML_STATIC_FRAME_SETTLED = 0,
	/* The holder stands; `owed` write locations are to be retired and the
	 * participants waited for before the page's bytes are read. */
	DARWIN_UML_STATIC_FRAME_OWED = 1,
	/* The page is not translatable here, or the generation is gone. */
	DARWIN_UML_STATIC_FRAME_REFUSED = 2,
	/* Nothing changed; the caller must not cache what it translates. */
	DARWIN_UML_STATIC_FRAME_FAILED = 3,
	/* Nothing changed; the debt is `owed` locations, more than the buffer
	 * holds. Ask again with room for them. */
	DARWIN_UML_STATIC_FRAME_CROWDED = 4,
	/* The holder stands, and the debt is every write translation of every
	 * participant: retire them all and wait for every participant. */
	DARWIN_UML_STATIC_FRAME_GLOBAL = 5,
};

/* Registers the page at `address` of `generation` as a holder of its frame,
 * before the caller reads a byte of it. At most `capacity` owed locations are
 * written to `owed`. */
int darwin_uml_static_vm_hold_code_frame(
	void *context, unsigned long generation, unsigned long address,
	struct darwin_uml_static_frame_location *owed, size_t capacity,
	struct darwin_uml_static_frame_hold *hold);
/* A completed retirement of the locations a hold reported covers its debt. */
void darwin_uml_static_vm_clear_code_debt(
	void *context, unsigned long generation, unsigned long address,
	uint64_t record_identity, uint64_t debt_sequence);

struct darwin_uml_static_frame_state {
	const void *backing;
	uint64_t offset;
	const void *record;
	uint64_t alias_changes;
	/* The frame has a holder other than the page itself. */
	int other_holder;
};

/* The write fill's read of the frame at `address`: 0, or -1 when the context
 * cannot say. */
int darwin_uml_static_vm_frame_state(
	void *context, unsigned long generation, unsigned long address,
	struct darwin_uml_static_frame_state *state);
/* The same for a caller with an entered execution context, which holds no
 * lock: through the page's code-page handle when it has one, by the page's
 * word when it has none (a data mapping of the frame). 0, or -1 when it cannot
 * say. */
int darwin_uml_static_vm_execution_context_frame_fill(
	void *context, const void *code_page, unsigned long address,
	struct darwin_uml_static_frame_state *state);
/* 1 while the words the state read have not moved. */
int darwin_uml_static_vm_frame_state_unchanged(
	const struct darwin_uml_static_frame_state *state);

struct darwin_uml_static_frame_publication {
	/* Holder addresses in the writer's generation, and the ring records the
	 * call added there. */
	size_t own_count;
	uint64_t own_records;
	int overflow;
};

/* Publishes the store of `length` bytes at `address`, after its bytes were
 * written, at every holder of the frame it lands in. Returns 1 if it did, 0
 * when the frame has no holder but the writer's own page (the caller publishes
 * as it always did), -1 when the context cannot say. */
int darwin_uml_static_vm_publish_frame_store(
	void *context, unsigned long generation, unsigned long address,
	unsigned long length, uint64_t *own, size_t capacity,
	struct darwin_uml_static_frame_publication *publication);
/* Before a store: the holder addresses of its frame in the writer's own
 * generation that darwin_uml_static_vm_publish_frame_store would publish at,
 * with nothing published. The same result and `publication` fields, bar the
 * record count. */
int darwin_uml_static_vm_frame_own_holders(
	void *context, unsigned long generation, unsigned long address,
	unsigned long length, uint64_t *own, size_t capacity,
	struct darwin_uml_static_frame_publication *publication);
/* The generation's flush serial; see FrameAccounting::walk. 0 for no
 * context. */
uint64_t darwin_uml_static_vm_execution_context_flush_serial(void *context);
#endif

#ifdef __cplusplus
}
#endif

#endif
