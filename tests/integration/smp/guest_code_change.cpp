// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding arm64/Linux guest workload: changes the code of a process
 * every way the guest has, and runs it again after each; no translation of
 * the old instructions may run.
 *
 * Each case keeps a small function in an anonymous mapping, "movz w0, #v;
 * ret", called from a loop on the same or another page so the engine has
 * translated, cached and linked it. Then something changes under the cache,
 * and the writer cleans the data cache and invalidates the instruction cache
 * to the point of unification, as the architecture asks:
 *
 *	smc		the process stores a new immediate, from outside the
 *			function and from inside the block that runs it;
 *	protect		mprotect takes execute or write away and gives it back,
 *			and the store lands only afterwards;
 *	fork		the parent or the child writes its page after the
 *			fork write-protected both, the page being copied or,
 *			once the other side has gone, reused in place;
 *	remap		MAP_FIXED, munmap and mmap, and mremap put other
 *			instructions at the address;
 *	shared		a process rewrites and synchronizes code on a page it
 *			shares with another process, which only runs it and
 *			has translated it since the fork, the executable view
 *			at the null page in a second run, which the guest's
 *			root can map;
 *	lazy-remote	another process writes the page with process_vm_writev(),
 *			into a mapping its owner has not touched since the fork,
 *			or into one its owner replaces with another frame
 *			after the kernel pinned the first;
 *	vcpu		threads on other vCPUs ran the function, and the caller
 *			that links to it, before the store and run them after;
 *	flood		another vCPU publishes many code changes while this one
 *			waits in the kernel, and the function is then patched.
 *
 * The vcpu case is the one that reaches translations several executors of
 * one address space share: every worker runs the same caller and callee, so a
 * link one of them made must not outlive a change another saw.
 *
 * A case that sees a value its code no longer holds is a stale translation.
 * Prints one line per failed check and, last, "code-change: abi=a64
 * checks=N failed=0 verdict=ok" when every case saw the new code. */

namespace {

using u32 = unsigned int;
using u64 = unsigned long;

constexpr long nr_read = 63, nr_write = 64, nr_pipe2 = 59, nr_exit_group = 94,
	nr_ftruncate = 46, nr_futex = 98, nr_munmap = 215, nr_mremap = 216,
	nr_clone = 220, nr_mmap = 222, nr_mprotect = 226, nr_wait4 = 260,
	nr_process_vm_writev = 271, nr_memfd_create = 279, nr_ioctl = 29,
	nr_userfaultfd = 282;
constexpr long prot_none = 0, prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr long map_shared = 0x01, map_private = 0x02, map_fixed = 0x10,
	map_anonymous = 0x20;
constexpr long mremap_maymove = 1, mremap_fixed = 2;
constexpr long futex_wait_private = 128, futex_wake_private = 129;
constexpr long sigchld = 17;
/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
 * CLONE_SYSVSEM */
constexpr u64 thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000;
/* A multiple of the 4 KiB guest page and of the 16 KiB host page: the far
 * callee is on another guest page and on another host page. */
constexpr u64 page = 16384;
constexpr u64 stack_size = 64 * 1024;
constexpr u32 calls = 200;
constexpr int workers = 3;

long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
	long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	__asm__ volatile("svc #0"
		: "+r"(x0)
		: "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		: "memory", "cc");
	return x0;
}

void print(const char *text)
{
	long length = 0;
	while (text[length] != 0)
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

void print(long value)
{
	char digits[24];
	int position = sizeof(digits);
	digits[--position] = 0;
	unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value) :
		static_cast<unsigned long>(value);
	do {
		digits[--position] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude != 0);
	if (value < 0)
		digits[--position] = '-';
	print(digits + position);
}

int failures;
int checks;

/* A case is checked once per observation: it prints only what failed. */
void expect(const char *name, long expected, long got)
{
	__atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED);
	if (expected == got)
		return;
	__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
	print("code-change: FAIL "); print(name);
	print(" expected="); print(expected);
	print(" got="); print(got); print("\n");
}

[[noreturn]] void leave(int status)
{
	raw_syscall(nr_exit_group, status);
	__builtin_trap();
}

/* Clean and invalidate to the point of unification what a store changed,
 * then synchronize this PE's fetch with it. */
void publish(const void *address, u64 bytes)
{
	const auto first = reinterpret_cast<u64>(address) & ~u64{3};
	const auto end = reinterpret_cast<u64>(address) + bytes;
	for (auto line = first & ~u64{63}; line < end; line += 64)
		__asm__ volatile("dc cvau, %0" : : "r"(line) : "memory");
	__asm__ volatile("dsb ish" : : : "memory");
	for (auto line = first & ~u64{63}; line < end; line += 64)
		__asm__ volatile("ic ivau, %0" : : "r"(line) : "memory");
	__asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

constexpr u32 ret = 0xd65f03c0U;

[[nodiscard]] constexpr u32 movz_w(unsigned reg, u32 value) noexcept
{
	return 0x52800000U | ((value & 0xffffU) << 5U) | reg;
}

[[nodiscard]] constexpr u32 branch_link(u64 from, u64 to) noexcept
{
	return 0x94000000U |
		(static_cast<u32>((static_cast<long>(to) - static_cast<long>(from)) >> 2) &
		 0x03ffffffU);
}

void put(unsigned char *at, u32 instruction)
{
	*reinterpret_cast<volatile u32 *>(at) = instruction;
}

/* The code under test. At 0, a caller that calls `callee` `calls` times; the
 * callee, "movz w0, #value; ret", is at callee_offset, on the same page or
 * another.
 *
 *	0:	stp x29, x30, [sp, #-32]!
 *	4:	str x19, [sp, #16]
 *	8:	movz w19, #calls
 *	12:	bl callee
 *	16:	subs w19, w19, #1
 *	20:	b.ne 12
 *	24:	ldr x19, [sp, #16]
 *	28:	ldp x29, x30, [sp], #32
 *	32:	ret
 */
constexpr u64 callee_near = 0x100;
constexpr u64 callee_far = page;

void emit_callee(unsigned char *callee, u32 value)
{
	put(callee, movz_w(0, value));
	put(callee + 4, ret);
	publish(callee, 8);
}

void emit_caller(unsigned char *caller, u64 callee_offset)
{
	put(caller + 0, 0xa9be7bfdU);
	put(caller + 4, 0xf9000bf3U);
	put(caller + 8, movz_w(19, calls));
	put(caller + 12, branch_link(12, callee_offset));
	put(caller + 16, 0x71000673U);
	put(caller + 20, 0x54ffffc1U);
	put(caller + 24, 0xf9400bf3U);
	put(caller + 28, 0xa8c27bfdU);
	put(caller + 32, ret);
	publish(caller, 36);
}

/* A store of a new immediate into the function, as a patcher or a JIT does
 * it: one instruction word, then the maintenance. */
void patch(unsigned char *callee, u32 value)
{
	put(callee, movz_w(0, value));
	publish(callee, 4);
}

long call_caller(unsigned char *caller)
{
	return reinterpret_cast<long (*)()>(caller)();
}

unsigned char *map_region(u64 pages, long protection = prot_read | prot_write | prot_exec,
	long flags = map_private | map_anonymous)
{
	const long result = raw_syscall(nr_mmap, 0, static_cast<long>(pages * page),
		protection, flags, -1, 0);
	if (result < 0 && result > -4096) {
		print("code-change: setup failed\n");
		leave(4);
	}
	return reinterpret_cast<unsigned char *>(result);
}

/* A fresh region holding the caller and, near or far, the callee. */
unsigned char *new_code(u32 value, u64 callee_offset, u64 pages = 2)
{
	auto *region = map_region(pages);
	emit_caller(region, callee_offset);
	emit_callee(region + callee_offset, value);
	return region;
}

void mprotect_page(unsigned char *region, long protection)
{
	if (raw_syscall(nr_mprotect, reinterpret_cast<long>(region),
		    static_cast<long>(page), protection) != 0) {
		print("code-change: mprotect failed\n");
		leave(4);
	}
}

/* Stores from outside the function. */
void case_smc(const char *name, u64 callee_offset)
{
	auto *region = new_code(1, callee_offset);
	auto *callee = region + callee_offset;
	expect(name, 1, call_caller(region));
	for (u32 value = 2; value <= 40; ++value) {
		const u32 next = value * 0x01010101U % 65521U;
		patch(callee, next);
		expect(name, next, call_caller(region));
	}
}

/* The function patches the immediate it is about to execute, in the block
 * that runs it: after the maintenance and ISB it runs the word it stored.
 *
 *	0:	adr x2, 36
 *	4:	movz w3, #0x5280, lsl #16
 *	8:	orr w3, w3, w1, lsl #5
 *	12:	str w3, [x2]
 *	16:	dc cvau, x2
 *	20:	dsb ish
 *	24:	ic ivau, x2
 *	28:	dsb ish
 *	32:	isb
 *	36:	movz w0, #0		(immediate patched)
 *	40:	ret
 */
void case_smc_running_block()
{
	auto *region = map_region(1);
	const u32 code[] = {
		0x10000122U, 0x52aa5003U, 0x2a011463U, 0xb9000043U, 0xd50b7b22U,
		0xd5033b9fU, 0xd50b7522U, 0xd5033b9fU, 0xd5033fdfU, movz_w(0, 0),
		ret,
	};
	for (u64 index = 0; index < sizeof(code) / sizeof(code[0]); ++index)
		put(region + 4 * index, code[index]);
	publish(region, sizeof(code));
	for (u32 value = 1; value <= 120; ++value) {
		register long x0 __asm__("x0");
		register long x1 __asm__("x1") = value;
		__asm__ volatile("blr %2"
			: "=r"(x0), "+r"(x1)
			: "r"(region)
			: "x2", "x3", "x30", "memory", "cc");
		expect("smc-running-block", value, x0);
	}
}

/* Execute and write taken away and given back, the store landing only once
 * the page is writable again. */
void case_protect()
{
	auto *region = new_code(1, callee_near);
	auto *callee = region + callee_near;
	expect("protect-warm", 1, call_caller(region));

	mprotect_page(region, prot_read | prot_exec);
	expect("protect-readonly", 1, call_caller(region));
	mprotect_page(region, prot_read | prot_write | prot_exec);
	patch(callee, 2);
	expect("protect-write-after-readonly", 2, call_caller(region));

	for (u32 value = 3; value < 12; ++value) {
		mprotect_page(region, prot_read | prot_exec);
		expect("protect-readonly-loop", value - 1, call_caller(region));
		mprotect_page(region, prot_read | prot_write | prot_exec);
		patch(callee, value);
		expect("protect-write-after-readonly-loop", value, call_caller(region));
	}

	/* No access at all, and the instruction changes while nothing can
	 * execute the page: here, after it is writable but not executable. */
	mprotect_page(region, prot_none);
	mprotect_page(region, prot_read | prot_write);
	patch(callee, 20);
	mprotect_page(region, prot_read | prot_write | prot_exec);
	expect("protect-none-then-write", 20, call_caller(region));

	/* A JIT's write window, then execute again. */
	for (u32 value = 21; value < 30; ++value) {
		mprotect_page(region, prot_read | prot_write);
		patch(callee, value);
		mprotect_page(region, prot_read | prot_exec);
		expect("protect-write-window", value, call_caller(region));
	}
}

/* fork write-protects every page of the parent. `parent_writes_first` says
 * which side writes first and `child_exits_first` whether the parent waits
 * for the child to exit before it writes, which lets the kernel reuse the
 * page in place. */
void case_fork(const char *name, bool parent_writes_first, bool child_exits_first)
{
	auto *region = new_code(1, callee_near);
	auto *callee = region + callee_near;
	expect(name, 1, call_caller(region));
	int pipe_to_child[2];
	int pipe_to_parent[2];
	raw_syscall(nr_pipe2, reinterpret_cast<long>(pipe_to_child), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(pipe_to_parent), 0);
	const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (child < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	char token = 0;
	if (child == 0) {
		int bad = 0;
		if (child_exits_first)
			leave(0);
		if (parent_writes_first) {
			/* The parent writes its page; the child's still holds the
			 * code from before the fork. */
			raw_syscall(nr_read, pipe_to_child[0], reinterpret_cast<long>(&token), 1);
			bad += call_caller(region) != 1;
			patch(callee, 7);
			bad += call_caller(region) != 7;
		} else {
			patch(callee, 5);
			bad += call_caller(region) != 5;
			raw_syscall(nr_write, pipe_to_parent[1], reinterpret_cast<long>(&token), 1);
			raw_syscall(nr_read, pipe_to_child[0], reinterpret_cast<long>(&token), 1);
			bad += call_caller(region) != 5;
		}
		leave(bad);
	}
	int status = 0;
	if (child_exits_first) {
		raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
		expect(name, 0, status);
		expect(name, 1, call_caller(region));
		patch(callee, 9);
		expect(name, 9, call_caller(region));
		patch(callee, 11);
		expect(name, 11, call_caller(region));
		return;
	}
	if (parent_writes_first) {
		patch(callee, 3);
		expect(name, 3, call_caller(region));
		raw_syscall(nr_write, pipe_to_child[1], reinterpret_cast<long>(&token), 1);
	} else {
		raw_syscall(nr_read, pipe_to_parent[0], reinterpret_cast<long>(&token), 1);
		/* The child has written its copy; ours is still the old code. */
		expect(name, 1, call_caller(region));
		patch(callee, 8);
		expect(name, 8, call_caller(region));
		raw_syscall(nr_write, pipe_to_child[1], reinterpret_cast<long>(&token), 1);
	}
	raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	expect(name, 0, status);
}

/* Other instructions at the same address by remapping. */
void case_remap()
{
	auto *region = new_code(1, callee_near, 1);
	expect("remap-warm", 1, call_caller(region));

	const long fixed = raw_syscall(nr_mmap, reinterpret_cast<long>(region),
		static_cast<long>(page), prot_read | prot_write | prot_exec,
		map_private | map_anonymous | map_fixed, -1, 0);
	if (fixed != reinterpret_cast<long>(region)) {
		print("code-change: MAP_FIXED failed\n");
		leave(4);
	}
	emit_caller(region, callee_near);
	emit_callee(region + callee_near, 2);
	expect("remap-fixed", 2, call_caller(region));

	raw_syscall(nr_munmap, reinterpret_cast<long>(region), static_cast<long>(page));
	const long again = raw_syscall(nr_mmap, reinterpret_cast<long>(region),
		static_cast<long>(page), prot_read | prot_write | prot_exec,
		map_private | map_anonymous | map_fixed, -1, 0);
	if (again != reinterpret_cast<long>(region)) {
		print("code-change: mmap failed\n");
		leave(4);
	}
	emit_caller(region, callee_near);
	emit_callee(region + callee_near, 3);
	expect("remap-munmap-mmap", 3, call_caller(region));

	/* The page moves away and the old address gets other code. */
	auto *other = map_region(1);
	const long moved = raw_syscall(nr_mremap, reinterpret_cast<long>(region),
		static_cast<long>(page), static_cast<long>(page),
		mremap_maymove | mremap_fixed, reinterpret_cast<long>(other));
	if (moved != reinterpret_cast<long>(other)) {
		print("code-change: mremap failed\n");
		leave(4);
	}
	expect("remap-moved-code", 3, call_caller(other));
	const long fresh = raw_syscall(nr_mmap, reinterpret_cast<long>(region),
		static_cast<long>(page), prot_read | prot_write | prot_exec,
		map_private | map_anonymous | map_fixed, -1, 0);
	if (fresh != reinterpret_cast<long>(region)) {
		print("code-change: mmap failed\n");
		leave(4);
	}
	emit_caller(region, callee_near);
	emit_callee(region + callee_near, 4);
	expect("remap-old-address", 4, call_caller(region));
	patch(other + callee_near, 5);
	expect("remap-moved-patched", 5, call_caller(other));
	expect("remap-old-address-unchanged", 4, call_caller(region));
}

/* Code two processes share. A page of a memfd is mapped writable and
 * executable, MAP_SHARED, and the process forks. The parent only runs the
 * function; the child rewrites it through the writable view and synchronizes
 * it for the executable one, as __clear_cache does, and tells the parent. The
 * parent warms after the fork, which flushes its own translations, so the
 * translation it holds is the one the child's change must reach. A process
 * with the privilege can map the executable view at the null page, which asks
 * the same of the synchronization there; mapping it must succeed. */
void case_shared_code(bool at_null_page)
{
	constexpr u32 rounds = 8;
	constexpr u32 warm_calls = 200;
	const long fd = raw_syscall(nr_memfd_create,
		reinterpret_cast<long>("code-change"), 0);
	if (fd < 0 || raw_syscall(nr_ftruncate, fd, static_cast<long>(page)) != 0) {
		print("code-change: memfd failed\n");
		leave(4);
	}
	const char *const check_name =
		at_null_page ? "shared-null-page" : "shared-memfd";
	const long writable = raw_syscall(nr_mmap, 0, static_cast<long>(page),
		prot_read | prot_write, map_shared, fd, 0);
	/* The null page is a page like another to the guest's kernel, which lets
	 * a privileged process map code there. */
	const long executable = raw_syscall(nr_mmap, 0, static_cast<long>(page),
		prot_read | prot_exec, map_shared | (at_null_page ? map_fixed : 0),
		fd, 0);
	if ((writable < 0 && writable > -4096) ||
	    (executable < 0 && executable > -4096)) {
		print("code-change: shared mmap failed\n");
		leave(4);
	}
	auto *rw = reinterpret_cast<volatile u32 *>(writable);
	const auto write_function = [&](u32 value) {
		rw[0] = movz_w(0, value);
		rw[1] = ret;
		__asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %1\n\t"
			"dsb ish\n\tisb"
			:
			: "r"(rw), "r"(executable)
			: "memory");
	};
	const auto run = [&]() -> long {
		register long x0 __asm__("x0");
		__asm__ volatile("isb\n\tblr %1"
			: "=r"(x0)
			: "r"(executable)
			: "x1", "x2", "x3", "x30", "memory", "cc");
		return x0;
	};
	write_function(1);

	int to_child[2];
	int to_parent[2];
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_child), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_parent), 0);
	const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (child < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	char token = 0;
	if (child == 0) {
		for (u32 round = 1; round <= rounds; ++round) {
			if (raw_syscall(nr_read, to_child[0],
				    reinterpret_cast<long>(&token), 1) != 1)
				leave(3);
			write_function(10 + round);
			raw_syscall(nr_write, to_parent[1],
				reinterpret_cast<long>(&token), 1);
		}
		leave(0);
	}
	for (u32 round = 1; round <= rounds; ++round) {
		for (u32 call = 0; call < warm_calls; ++call)
			(void)run();
		raw_syscall(nr_write, to_child[1], reinterpret_cast<long>(&token), 1);
		if (raw_syscall(nr_read, to_parent[0],
			    reinterpret_cast<long>(&token), 1) != 1) {
			print("code-change: shared read failed\n");
			leave(4);
		}
		expect(check_name, 10 + round, run());
	}
	int status = 0;
	raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	expect(at_null_page ? "shared-null-page-child" : "shared-memfd-child", 0,
		status);
}

/* A page of a memfd, mapped writable and executable and shared, is run by
 * the parent; the child forks with it and does not touch it. The parent then
 * writes the page through the child's mapping with process_vm_writev(), as a
 * debugger or a runtime patching another process does. The kernel writes
 * through the frame, for a child whose software page table has no entry for
 * the page yet, and the parent runs the new instruction all the same. After
 * the child has touched the page (so has an entry), the same holds. It runs
 * first: after the other cases the frame has held code before, which hides a
 * missed publication. */
void case_lazy_remote_target()
{
	constexpr u32 warm_calls = 200;
	const long fd = raw_syscall(nr_memfd_create,
		reinterpret_cast<long>("code-change-lazy"), 0);
	if (fd < 0 || raw_syscall(nr_ftruncate, fd, static_cast<long>(page)) != 0) {
		print("code-change: lazy memfd failed\n");
		leave(4);
	}
	const long executable = raw_syscall(nr_mmap, 0, static_cast<long>(page),
		prot_read | prot_write | prot_exec, map_shared, fd, 0);
	if (executable < 0 && executable > -4096) {
		print("code-change: lazy mmap failed\n");
		leave(4);
	}
	auto *rw = reinterpret_cast<volatile u32 *>(executable);
	rw[0] = movz_w(0, 1);
	rw[1] = ret;
	publish(reinterpret_cast<void *>(executable), 8);
	const auto run = [&]() -> long {
		register long x0 __asm__("x0");
		__asm__ volatile("isb\n\tblr %1"
			: "=r"(x0)
			: "r"(executable)
			: "x1", "x2", "x3", "x30", "memory", "cc");
		return x0;
	};
	int to_child[2];
	int to_parent[2];
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_child), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_parent), 0);
	const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (child < 0) {
		print("code-change: lazy fork failed\n");
		leave(4);
	}
	char token = 0;
	if (child == 0) {
		if (raw_syscall(nr_read, to_child[0],
			    reinterpret_cast<long>(&token), 1) != 1)
			leave(4);
		/* A read of the page gives the child its mapping. */
		const u32 touched = rw[0];
		(void)touched;
		raw_syscall(nr_write, to_parent[1], reinterpret_cast<long>(&token), 1);
		if (raw_syscall(nr_read, to_child[0],
			    reinterpret_cast<long>(&token), 1) != 1)
			leave(4);
		leave(0);
	}
	for (u32 call = 0; call < warm_calls; ++call)
		(void)run();
	struct IoVec {
		void *base;
		u64 length;
	};
	u32 instruction = movz_w(0, 11);
	IoVec local{&instruction, 4};
	IoVec remote{reinterpret_cast<void *>(executable), 4};
	const auto copy = [&]() {
		return raw_syscall(nr_process_vm_writev, child,
			reinterpret_cast<long>(&local), 1,
			reinterpret_cast<long>(&remote), 1, 0);
	};
	expect("lazy-remote-copy", 4, copy());
	expect("lazy-remote-untouched", 11, run());
	raw_syscall(nr_write, to_child[1], reinterpret_cast<long>(&token), 1);
	if (raw_syscall(nr_read, to_parent[0],
		    reinterpret_cast<long>(&token), 1) != 1) {
		print("code-change: lazy read failed\n");
		leave(4);
	}
	instruction = movz_w(0, 12);
	expect("lazy-remote-copy-again", 4, copy());
	expect("lazy-remote-touched", 12, run());
	raw_syscall(nr_write, to_child[1], reinterpret_cast<long>(&token), 1);
	int status = 0;
	raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	expect("lazy-remote-child", 0, status);
}

/* userfaultfd(2) on the writer's source buffer: the kernel's read of it
 * waits for a monitor, with the target's pages already pinned and its
 * memory map unlocked. */
struct UffdApi {
	u64 api;
	u64 features;
	u64 ioctls;
};

struct UffdRange {
	u64 start;
	u64 length;
};

struct UffdRegister {
	UffdRange range;
	u64 mode;
	u64 ioctls;
};

struct UffdCopy {
	u64 destination;
	u64 source;
	u64 length;
	u64 mode;
	long copied;
};

struct UffdMessage {
	unsigned char event;
	unsigned char reserved1;
	unsigned short reserved2;
	u32 reserved3;
	u64 flags;
	u64 address;
	u64 reserved4;
};

static_assert(sizeof(UffdMessage) == 32);

constexpr u64 uffdio_api = 0xc018aa3fUL, uffdio_register = 0xc020aa00UL,
	uffdio_copy = 0xc028aa03UL;
constexpr u64 uffd_api = 0xaa, uffd_register_missing = 1;
constexpr unsigned char uffd_event_pagefault = 0x12;
/* The guest's page, which is smaller than the host's: the unit of
 * UFFDIO_COPY. */
constexpr u64 uffd_page = 4096;

/* A page of a memfd holding "movz w0, #value; ret", mapped writable,
 * executable and shared at `at`. */
long code_file(const char *label, u32 value, long &at)
{
	const long fd = raw_syscall(nr_memfd_create, reinterpret_cast<long>(label), 0);
	if (fd < 0 || raw_syscall(nr_ftruncate, fd, static_cast<long>(page)) != 0) {
		print("code-change: remapped memfd failed\n");
		leave(4);
	}
	at = raw_syscall(nr_mmap, 0, static_cast<long>(page),
		prot_read | prot_write | prot_exec, map_shared, fd, 0);
	if (at < 0 && at > -4096) {
		print("code-change: remapped mmap failed\n");
		leave(4);
	}
	auto *words = reinterpret_cast<volatile u32 *>(at);
	words[0] = movz_w(0, value);
	words[1] = ret;
	publish(reinterpret_cast<void *>(at), 8);
	return fd;
}

/* A process writes another's page with process_vm_writev() while that
 * process, between the kernel's pin of the page and its store, puts another
 * frame at the same address. The writer's source buffer is registered with
 * userfaultfd, so the copy from it waits for the target, which is the
 * monitor, after the pin. The store lands in the frame the kernel pinned,
 * which this process runs as code; the frame mapped at the address now has
 * no code. The kernel names the frame it wrote, so the code is new when it
 * is called again, the mapping in the target still shows the other frame,
 * and the first frame holds the new instruction.
 *
 *	R	runs the frame and checks
 *	W	writes: process_vm_writev(T, S) with S registered
 *	T	holds the frame, then remaps its address to another frame when
 *		the fault on S arrives, and resolves the fault
 *
 * It runs early too: the first frame has not held other code. */
void case_lazy_remote_remapped()
{
	constexpr u32 warm_calls = 200;
	const char *name = "lazy-remote-remapped";
	long executable = 0;
	long other = 0;
	code_file("code-change-remap-first", 1, executable);
	const long second = code_file("code-change-remap-second", 3, other);
	raw_syscall(nr_munmap, other, static_cast<long>(page));
	const auto run = [&]() -> long {
		register long x0 __asm__("x0");
		__asm__ volatile("isb\n\tblr %1"
			: "=r"(x0)
			: "r"(executable)
			: "x1", "x2", "x3", "x30", "memory", "cc");
		return x0;
	};
	int to_writer_view[2];
	int from_writer[2];
	int to_writer[2];
	int from_target[2];
	int to_target[2];
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_writer_view), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(from_writer), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_writer), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(from_target), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_target), 0);
	const long writer = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (writer < 0) {
		print("code-change: remapped fork failed\n");
		leave(4);
	}
	if (writer == 0) {
		const long source = raw_syscall(nr_mmap, 0, static_cast<long>(page),
			prot_read | prot_write, map_private | map_anonymous, -1, 0);
		if (source < 0 && source > -4096) {
			print("code-change: remapped source failed\n");
			leave(4);
		}
		const long monitor = raw_syscall(nr_userfaultfd, 0);
		UffdApi api{uffd_api, 0, 0};
		UffdRegister registration{{static_cast<u64>(source), page},
			uffd_register_missing, 0};
		if (monitor < 0 ||
		    raw_syscall(nr_ioctl, monitor, static_cast<long>(uffdio_api),
			    reinterpret_cast<long>(&api)) != 0 ||
		    raw_syscall(nr_ioctl, monitor, static_cast<long>(uffdio_register),
			    reinterpret_cast<long>(&registration)) != 0) {
			print("code-change: userfaultfd failed\n");
			leave(4);
		}
		const long target = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
		if (target < 0) {
			print("code-change: remapped fork failed\n");
			leave(4);
		}
		if (target == 0) {
			auto *view = reinterpret_cast<volatile u32 *>(executable);
			/* A store of its own puts the page in this process's
			 * address space. */
			view[1] = view[1];
			raw_syscall(nr_write, to_writer_view[1],
				reinterpret_cast<long>(&executable), sizeof(executable));
			/* The copy from the source buffer faults: the page is
			 * pinned, and nothing is stored yet. */
			UffdMessage message{};
			if (raw_syscall(nr_read, monitor, reinterpret_cast<long>(&message),
				    sizeof(message)) != static_cast<long>(sizeof(message)) ||
			    message.event != uffd_event_pagefault) {
				print("code-change: userfaultfd message failed\n");
				leave(4);
			}
			/* Another frame at the address, and its page in this
			 * process's address space. */
			const long remapped = raw_syscall(nr_mmap, executable,
				static_cast<long>(page), prot_read | prot_write | prot_exec,
				map_shared | map_fixed, second, 0);
			if (remapped != executable) {
				print("code-change: remap failed\n");
				leave(4);
			}
			view[1] = view[1];
			char token = 0;
			raw_syscall(nr_write, from_target[1], reinterpret_cast<long>(&token), 1);
			raw_syscall(nr_read, to_target[0], reinterpret_cast<long>(&token), 1);
			/* The writer's buffer gets the new instruction, and the
			 * copy goes on. */
			const long carrier = raw_syscall(nr_mmap, 0, static_cast<long>(page),
				prot_read | prot_write, map_private | map_anonymous, -1, 0);
			if (carrier < 0 && carrier > -4096) {
				print("code-change: remapped carrier failed\n");
				leave(4);
			}
			*reinterpret_cast<volatile u32 *>(carrier) = movz_w(0, 2);
			UffdCopy fill{static_cast<u64>(source), static_cast<u64>(carrier),
				uffd_page, 0, 0};
			if (raw_syscall(nr_ioctl, monitor, static_cast<long>(uffdio_copy),
				    reinterpret_cast<long>(&fill)) != 0 ||
			    fill.copied != static_cast<long>(uffd_page)) {
				print("code-change: userfaultfd copy failed\n");
				leave(4);
			}
			raw_syscall(nr_read, to_target[0], reinterpret_cast<long>(&token), 1);
			const u32 seen = view[0];
			raw_syscall(nr_write, from_target[1], reinterpret_cast<long>(&seen),
				sizeof(seen));
			leave(0);
		}
		long view = 0;
		raw_syscall(nr_read, to_writer_view[0], reinterpret_cast<long>(&view),
			sizeof(view));
		char token = 0;
		raw_syscall(nr_write, from_writer[1], reinterpret_cast<long>(&token), 1);
		raw_syscall(nr_read, to_writer[0], reinterpret_cast<long>(&token), 1);
		struct IoVec {
			void *base;
			u64 length;
		};
		IoVec local{reinterpret_cast<void *>(source), 4};
		IoVec remote{reinterpret_cast<void *>(view), 4};
		const long done = raw_syscall(nr_process_vm_writev, target,
			reinterpret_cast<long>(&local), 1,
			reinterpret_cast<long>(&remote), 1, 0);
		char reply = done == 4 ? 1 : 0;
		raw_syscall(nr_write, from_writer[1], reinterpret_cast<long>(&reply), 1);
		int status = 0;
		raw_syscall(nr_wait4, target, reinterpret_cast<long>(&status), 0, 0);
		leave(status == 0 ? 0 : 5);
	}
	char token = 0;
	raw_syscall(nr_read, from_writer[0], reinterpret_cast<long>(&token), 1);
	/* The forks flushed this process's translations: run the code again so
	 * that the write meets a warm one. */
	for (u32 call = 0; call < warm_calls; ++call)
		(void)run();
	expect(name, 1, run());
	raw_syscall(nr_write, to_writer[1], reinterpret_cast<long>(&token), 1);
	/* The target has put the other frame at the address. */
	raw_syscall(nr_read, from_target[0], reinterpret_cast<long>(&token), 1);
	for (u32 call = 0; call < warm_calls; ++call)
		(void)run();
	expect(name, 1, run());
	raw_syscall(nr_write, to_target[1], reinterpret_cast<long>(&token), 1);
	char reply = 0;
	raw_syscall(nr_read, from_writer[0], reinterpret_cast<long>(&reply), 1);
	expect("lazy-remote-remapped-copy", 1, reply);
	expect(name, 2, run());
	/* The store was made in the frame that was pinned, not the one that
	 * is mapped now. */
	expect("lazy-remote-remapped-frame", movz_w(0, 2),
		*reinterpret_cast<volatile u32 *>(executable));
	raw_syscall(nr_write, to_target[1], reinterpret_cast<long>(&token), 1);
	u32 seen = 0;
	raw_syscall(nr_read, from_target[0], reinterpret_cast<long>(&seen), sizeof(seen));
	expect("lazy-remote-remapped-target", movz_w(0, 3), seen);
	int status = 0;
	raw_syscall(nr_wait4, writer, reinterpret_cast<long>(&status), 0, 0);
	expect("lazy-remote-remapped-exit", 0, status);
}

void futex_wait(volatile int *address, int expected)
{
	raw_syscall(nr_futex, reinterpret_cast<long>(address), futex_wait_private, expected, 0);
}

void futex_wake_all(volatile int *address)
{
	raw_syscall(nr_futex, reinterpret_cast<long>(address), futex_wake_private, 0x7fffffff);
}

} // namespace

extern "C" long guest_spawn_thread(u64 flags, void *stack_top,
	void (*entry)(void *), void *argument);

namespace {

/* Workers on other vCPUs run the function when asked, after the store, and
 * run it without pause between requests, so their executors hold it linked
 * while the main thread patches it. */
volatile int request;
volatile int replies;
volatile long worker_results[workers];
volatile int worker_stop;
volatile u32 worker_floor;
unsigned char *worker_region;
int worker_round_failures;

void worker(void *argument)
{
	const auto index = reinterpret_cast<u64>(argument);
	int seen = 0;
	for (;;) {
		int current;
		while ((current = __atomic_load_n(&request, __ATOMIC_ACQUIRE)) == seen) {
			/* Between rounds: run the function while it changes
			 * under this vCPU. It must hold the value published
			 * before this vCPU looked, or a later one. */
			const u32 floor = __atomic_load_n(&worker_floor, __ATOMIC_ACQUIRE);
			__asm__ volatile("isb" : : : "memory");
			const auto value = static_cast<u32>(call_caller(worker_region));
			if (value < floor)
				__atomic_fetch_add(&worker_round_failures, 1, __ATOMIC_RELAXED);
		}
		seen = current;
		if (worker_stop)
			break;
		/* The patch and its maintenance happened before the request;
		 * this PE's fetch must be synchronized with them. */
		__asm__ volatile("isb" : : : "memory");
		worker_results[index] = call_caller(worker_region);
		__atomic_fetch_add(&replies, 1, __ATOMIC_RELEASE);
		futex_wake_all(&replies);
	}
	__atomic_fetch_add(&replies, 1, __ATOMIC_RELEASE);
	futex_wake_all(&replies);
}

void ask_workers(int round, int &answered_before)
{
	__atomic_store_n(&request, round, __ATOMIC_RELEASE);
	futex_wake_all(&request);
	const int goal = answered_before + workers;
	int answered;
	while ((answered = __atomic_load_n(&replies, __ATOMIC_ACQUIRE)) < goal)
		futex_wait(&replies, answered);
	answered_before = goal;
}

void case_vcpu(bool write_protect_first)
{
	const char *name = write_protect_first ? "vcpu-after-protect" : "vcpu";
	worker_region = new_code(1, callee_near, 2);
	auto *callee = worker_region + callee_near;
	request = 0;
	replies = 0;
	worker_stop = 0;
	worker_floor = 1;
	for (int index = 0; index < workers; ++index) {
		auto *stack = map_region(stack_size / page, prot_read | prot_write,
			map_private | map_anonymous);
		if (guest_spawn_thread(thread_flags, stack + stack_size, worker,
			    reinterpret_cast<void *>(static_cast<u64>(index))) < 0) {
			print("code-change: thread failed\n");
			leave(4);
		}
	}
	int round = 0;
	int answered = 0;
	ask_workers(++round, answered);
	for (int index = 0; index < workers; ++index)
		expect(name, 1, worker_results[index]);
	for (u32 value = 2; value < 60; ++value) {
		if (write_protect_first) {
			mprotect_page(worker_region, prot_read | prot_exec);
			mprotect_page(worker_region, prot_read | prot_write | prot_exec);
		}
		patch(callee, value);
		__atomic_store_n(&worker_floor, value, __ATOMIC_RELEASE);
		/* Every vCPU runs the new code: this one and the workers'. */
		expect(name, value, call_caller(worker_region));
		ask_workers(++round, answered);
		for (int index = 0; index < workers; ++index)
			expect(name, value, worker_results[index]);
	}
	worker_stop = 1;
	ask_workers(++round, answered);
	expect(name, 0, __atomic_exchange_n(&worker_round_failures, 0, __ATOMIC_RELAXED));
}

/* Another vCPU maps, writes, publishes and unmaps executable pages without
 * pause while this one waits in the kernel inside the function, so the
 * address space's code changes many times; the function is then patched and
 * must be seen.
 *
 *	0:	stp x29, x30, [sp, #-16]!
 *	4:	ldr x9, 48
 *	8:	ldr w2, [x9]
 *	12:	ldr x0, 56
 *	16:	movz w1, #FUTEX_WAIT_PRIVATE
 *	20:	movz x3, #0
 *	24:	movz x8, #futex
 *	28:	svc #0
 *	32:	bl callee
 *	36:	ldp x29, x30, [sp], #16
 *	40:	ret
 *	48:	.quad &churn_expect
 *	56:	.quad &churn_done
 */
volatile int churn_request;
volatile int churn_wanted;
volatile int churn_done;
volatile int churn_expect;
int churn_rounds_asked;
unsigned char *flood_region;
constexpr int churn_pairs = 1500;

void churn(void *)
{
	int seen = 0;
	for (;;) {
		int current;
		while ((current = __atomic_load_n(&churn_request, __ATOMIC_ACQUIRE)) == seen)
			futex_wait(&churn_request, seen);
		seen = current;
		if (current < 0)
			break;
		for (int pair = 0; pair < churn_wanted; ++pair) {
			const long scratch = raw_syscall(nr_mmap, 0, static_cast<long>(page),
				prot_read | prot_write | prot_exec,
				map_private | map_anonymous, -1, 0);
			if (scratch < 0 && scratch > -4096)
				break;
			auto *code = reinterpret_cast<unsigned char *>(scratch);
			put(code, ret);
			publish(code, 4);
			raw_syscall(nr_munmap, scratch, static_cast<long>(page));
		}
		__atomic_store_n(&churn_done, current, __ATOMIC_RELEASE);
		futex_wake_all(&churn_done);
	}
}

void emit_waiting_function(unsigned char *at, u64 callee_offset)
{
	put(at + 0, 0xa9bf7bfdU);
	put(at + 4, 0x58000000U | ((44U / 4U) << 5U) | 9U);
	put(at + 8, 0xb9400122U);
	put(at + 12, 0x58000000U | ((44U / 4U) << 5U) | 0U);
	put(at + 16, movz_w(1, futex_wait_private));
	put(at + 20, 0xd2800003U);
	put(at + 24, 0xd2800000U | (static_cast<u32>(nr_futex) << 5U) | 8U);
	put(at + 28, 0xd4000001U);
	put(at + 32, branch_link(32, callee_offset));
	put(at + 36, 0xa8c17bfdU);
	put(at + 40, ret);
	*reinterpret_cast<volatile u64 *>(at + 48) =
		reinterpret_cast<u64>(&churn_expect);
	*reinterpret_cast<volatile u64 *>(at + 56) =
		reinterpret_cast<u64>(&churn_done);
	publish(at, 64);
}

/* Runs the function; `block` makes its wait last until the other vCPU says
 * it is done, and otherwise the wait finds the word already different. */
long flood_call(int done, bool block)
{
	churn_expect = block ? done : done + 1000;
	return call_caller(flood_region);
}

long flood_round(int pairs)
{
	const int done = __atomic_load_n(&churn_done, __ATOMIC_ACQUIRE);
	churn_wanted = pairs;
	__atomic_store_n(&churn_request, ++churn_rounds_asked, __ATOMIC_RELEASE);
	futex_wake_all(&churn_request);
	return flood_call(done, true);
}

void case_flood()
{
	flood_region = map_region(2);
	auto *callee = flood_region + callee_near;
	emit_waiting_function(flood_region, callee_near);
	emit_callee(callee, 1);
	auto *stack = map_region(stack_size / page, prot_read | prot_write,
		map_private | map_anonymous);
	churn_request = 0;
	churn_done = 0;
	churn_rounds_asked = 0;
	if (guest_spawn_thread(thread_flags, stack + stack_size, churn, nullptr) < 0) {
		print("code-change: thread failed\n");
		leave(4);
	}
	expect("flood-warm", 1, flood_call(0, false));
	for (u32 round = 0; round < 4; ++round) {
		const u32 first = 20 + round * 10;
		flood_round(0);
		flood_round(0);
		patch(callee, first);
		expect("flood-before", first, flood_call(0, false));
		expect("flood-after", first, flood_round(churn_pairs));
		patch(callee, first + 1);
		expect("flood-patched", first + 1, flood_call(0, false));
		patch(callee, first + 2);
		expect("flood-patched-again", first + 2, flood_call(0, false));
	}
	__atomic_store_n(&churn_request, -1, __ATOMIC_RELEASE);
	futex_wake_all(&churn_request);
}

} // namespace

extern "C" {

/* clone() returns twice on two stacks, which no compiled frame survives. */
__asm__(".text\n"
	".global guest_spawn_thread\n"
	".type guest_spawn_thread,%function\n"
	"guest_spawn_thread:\n"
	"	mov x9, x2\n"
	"	mov x10, x3\n"
	"	mov x2, #0\n"
	"	mov x3, #0\n"
	"	mov x4, #0\n"
	"	mov x8, #220\n"
	"	svc #0\n"
	"	cbnz x0, 1f\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	mov x0, x10\n"
	"	blr x9\n"
	"	mov x0, #0\n"
	"	mov x8, #93\n"
	"	svc #0\n"
	"1:	ret\n"
	".size guest_spawn_thread,.-guest_spawn_thread\n"
	".global _start\n"
	".type _start,%function\n"
	"_start:\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	bl guest_main\n"
	"	mov x8, #94\n"
	"	svc #0\n"
	".size _start,.-_start\n");

void *memset(void *destination, int value, u64 length)
{
	auto *bytes = static_cast<volatile unsigned char *>(destination);
	for (u64 index = 0; index < length; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

void *memcpy(void *destination, const void *source, u64 length)
{
	auto *to = static_cast<volatile unsigned char *>(destination);
	const auto *from = static_cast<const unsigned char *>(source);
	for (u64 index = 0; index < length; ++index)
		to[index] = from[index];
	return destination;
}

int guest_main()
{
	case_lazy_remote_target();
	case_lazy_remote_remapped();
	case_smc("smc-same-page", callee_near);
	case_smc("smc-other-page", callee_far);
	case_smc_running_block();
	case_protect();
	case_fork("fork-parent-first", true, false);
	case_fork("fork-child-first", false, false);
	case_fork("fork-child-gone", true, true);
	case_remap();
	case_shared_code(false);
	case_shared_code(true);
	case_vcpu(false);
	case_vcpu(true);
	case_flood();
	print("code-change: abi=a64 checks="); print(checks);
	print(" failed="); print(failures);
	print(" verdict="); print(failures == 0 ? "ok" : "fail"); print("\n");
	return failures == 0 ? 0 : 2;
}

}
