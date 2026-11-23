/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_MEMORY_ABI_H
#define UML_DARWIN_MEMORY_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/* guest_code is a bit set. Zero maps a page that is not guest code. Bit 0
 * marks guest code. Bit 1, meaningful only with bit 0 (the value 3), is the
 * kernel's fact that no store can change the page while it is mapped: an
 * up-to-date page-cache page of a read-only file system. The host shares what
 * it verified about such a page between address spaces and about no other. A
 * kernel that never sets bit 1 stays correct. */
int darwin_uml_vm_map(void *address, int fd, unsigned long long offset,
		      unsigned long length, int protection, int guest_code,
		      int guest_writable);
int darwin_uml_vm_map_generation(unsigned long generation, void *address,
				 int fd, unsigned long long offset,
				 unsigned long length, int protection,
				 int guest_code, int guest_writable);
int darwin_uml_vm_protect(void *address, unsigned long length,
			  int protection);
int darwin_uml_vm_unmap(void *address, unsigned long length);
int darwin_uml_vm_unmap_generation(unsigned long generation, void *address,
				   unsigned long length);
int darwin_uml_vm_drop(void *address, unsigned long length);
int darwin_uml_vm_activate(unsigned long generation);
int darwin_uml_vm_batch_begin(unsigned long generation);
/* Returns 0 once the batch is published, DARWIN_UML_VM_BATCH_REJECTED
 * (uml_tlb_sync_ranges.h) when an invalidation of the generation arrived
 * before it ended and the walker must walk it again, or a negative value on
 * failure. */
int darwin_uml_vm_batch_end(void);
int darwin_uml_vm_walker_lock(unsigned long generation);
int darwin_uml_vm_walker_unlock(unsigned long generation);
int darwin_uml_vm_destroy(unsigned long generation);
int darwin_uml_vm_flush_generation(unsigned long generation);
/* The generation's own code changed in [address, address + length) by a
 * flush by address, as an AArch32 cacheflush() makes it: the frames the
 * generation maps there now are published at their holders, and every
 * generation hears of a page the generation has no entry for. Returns 0, or
 * a negative value on failure. */
int darwin_uml_vm_invalidate_code(unsigned long generation,
				  unsigned long address, unsigned long length);
/* Linux wrote length bytes at address in the generation's process, within
 * one guest page, through its own alias of the frame at fd and offset (the
 * frame's offset in fd, which Linux still holds): a ptrace, /proc/pid/mem
 * or process_vm_writev() store. The change is published at every holder of
 * that frame, whatever the generation maps at address now, and in the
 * generation when its view of address is that frame and code, or it holds
 * the frame. A negative fd names no frame, and every generation hears of
 * the change. Returns 0, or a negative value on failure. */
int darwin_uml_vm_invalidate_code_frame(unsigned long generation,
					unsigned long address,
					unsigned long length, int fd,
					unsigned long long offset);
/* Linux wrote the bytes of the whole guest pages [offset, offset + length)
 * of the frame at fd through its own alias, for no process and at no
 * user address, into a frame some process maps and runs as code: a write(2),
 * splice(2) or truncate into a page-cache or memfd frame, or a block
 * device's read into a page a process pinned (O_DIRECT, io_uring), whether
 * that page is anonymous or a file's. Any frame of guest RAM is named the
 * same way, by the fd and offset of its backing. The change is published
 * at every x86 translation of those pages, whichever address spaces map
 * them; an arm64 guest follows the architecture's cache maintenance and is
 * not told. A negative fd names no frame, and every address space hears of
 * the change. Returns 0, or a negative value on failure. */
int darwin_uml_vm_invalidate_code_frames(int fd, unsigned long long offset,
					 unsigned long long length);
int darwin_uml_vm_sync_from_backing(unsigned long address);
int darwin_uml_vm_sync_from_buffer(unsigned long address,
				   const void *buffer, unsigned long length);
/* A uaccess pinned the page at fd and offset for address: discard the
 * software mapping of address when it names other backing, because either
 * sample may be the stale one. An absent or equal mapping is left as it is,
 * and nothing is installed. Returns 0, or a negative value on failure. */
int darwin_uml_vm_discard_if_stale(unsigned long address, int fd,
				   unsigned long long offset);
/* Returns 1 when the access can be retried, 0 when Linux must resolve the
 * fault, or a negative value on failure. */
int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write);

/* Guest RAM commitment. Ranges are byte offsets into the anonymous physical-
 * memory backing, aligned to the host page. Release hands a range the guest
 * no longer uses back to the host; commit makes a released range the guest's
 * again before the guest uses it. Both are idempotent per host page. */
int darwin_uml_physmem_release(unsigned long long offset,
			       unsigned long long length);
int darwin_uml_physmem_commit(unsigned long long offset,
			      unsigned long long length);

#ifdef __cplusplus
}
#endif

#endif
