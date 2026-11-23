// SPDX-License-Identifier: GPL-2.0-or-later
/* Changes the code of an x86 process every way the guest has, and runs it
 * again after each: no translation of the old bytes may run.
 *
 * Each case keeps a small function in an anonymous mapping, "mov $v, %eax;
 * ret", called from a loop on the same or another page so the engine has
 * translated, cached and linked it. Then something changes under the cache:
 *
 *	smc		the process stores a new immediate, from outside the
 *			function and from inside the unit that runs it;
 *	protect		mprotect takes execute or write away and gives it back,
 *			and the store lands only afterwards;
 *	fork		the parent or the child writes its page after the
 *			fork write-protected both, the page being copied or,
 *			once the other side has gone, reused in place;
 *	remap		MAP_FIXED, munmap and mmap, and mremap put other bytes
 *			at the address;
 *	vcpu		another thread, on another vCPU, ran the function
 *			before the store and runs it after;
 *	flood		another vCPU publishes more code changes than the
 *			address space remembers while this one is away, and
 *			the function is then translated again and patched;
 *	alias		the code is a shared mapping of a memfd or tmpfs file and
 *			the store goes through another mapping of the same
 *			frame, in this process, from another vCPU, from another
 *			process (forked before or after the first call) or from
 *			the kernel (read(2), or process_vm_writev(2) into
 *			another process that maps the frame as data only, or
 *			that maps another frame at the address after the
 *			kernel pinned the first), with the executable mapping
 *			protected, unmapped and mapped again between the call
 *			and the store, and into the very unit that runs;
 *	page-cache	the code is a file or memfd mapped executable and nothing
 *			else maps it; the kernel's own copy into the page cache
 *			(pwrite(2), splice(2), a truncate into the page) rewrites
 *			it, by this process or by a process forked before the
 *			code first ran, and a call after the write must run the
 *			new bytes, as x86 hardware does;
 *	two-patch	one block of one thread patches two instructions of a
 *			function with no link between the stores while three
 *			other vCPUs run the function, and after the thread
 *			flags that it is done every one of them must run both
 *			patches.
 *
 * A case that sees a value its code no longer holds is a stale translation.
 * Prints one line per case and, last, "code-change: abi=... verdict=ok" when
 * every case saw the new code. */
namespace {

#if defined(__x86_64__)
using word = unsigned long;
constexpr long nr_read = 0, nr_write = 1, nr_mmap = 9, nr_mprotect = 10,
	nr_munmap = 11, nr_mremap = 25, nr_pipe = 22, nr_fork = 57, nr_wait4 = 61,
	nr_futex = 202, nr_exit_group = 231, nr_getppid = 110, nr_clock_gettime = 228,
	nr_open = 2, nr_close = 3, nr_unlink = 87, nr_ftruncate = 77,
	nr_memfd_create = 319, nr_process_vm_writev = 311, nr_ioctl = 16,
	nr_userfaultfd = 323, nr_pwrite = 18, nr_splice = 275;
constexpr const char *abi = "x86-64";
long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
	long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long r10 __asm__("r10") = a3;
	register long r8 __asm__("r8") = a4;
	register long r9 __asm__("r9") = a5;
	long result;
	__asm__ volatile("syscall"
		: "=a"(result)
		: "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory", "cc");
	return result;
}
#else
using word = unsigned int;
constexpr long nr_read = 3, nr_write = 4, nr_mmap = 192, nr_mprotect = 125,
	nr_munmap = 91, nr_mremap = 163, nr_pipe = 42, nr_fork = 2, nr_wait4 = 114,
	nr_futex = 240, nr_exit_group = 252, nr_getppid = 64, nr_clock_gettime = 265,
	nr_open = 5, nr_close = 6, nr_unlink = 10, nr_ftruncate = 93,
	nr_memfd_create = 356, nr_process_vm_writev = 348, nr_ioctl = 54,
	nr_userfaultfd = 374, nr_pwrite = 181, nr_splice = 313;
constexpr const char *abi = "i386";
long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
	long a3 = 0, long a4 = 0, long a5 = 0)
{
	long result;
	/* The sixth argument goes in EBP, which an operand cannot name: pushed
	 * first, from its slot while ESP is still the compiler's, then loaded
	 * from the stack after EBP is saved. */
	__asm__ volatile("push %7\n\tpush %%ebp\n\tmov 4(%%esp), %%ebp\n\tint $0x80\n\t"
		"pop %%ebp\n\tadd $4, %%esp"
		: "=a"(result)
		: "a"(number), "b"(a0), "c"(a1), "d"(a2), "S"(a3), "D"(a4), "m"(a5)
		: "memory", "cc");
	return result;
}
#endif

constexpr long prot_none = 0, prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr long map_shared = 0x01, map_private = 0x02, map_fixed = 0x10,
	map_anonymous = 0x20;
constexpr long o_rdwr = 2, o_creat = 0100, o_excl = 0200;
constexpr long mremap_maymove = 1, mremap_fixed = 2;
constexpr long futex_wait_private = 128, futex_wake_private = 129;
/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
 * CLONE_SYSVSEM */
constexpr word thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000;
constexpr word page = 4096;
constexpr word stack_size = 64 * 1024;
constexpr int calls = 200;

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
	++checks;
	if (expected == got)
		return;
	++failures;
	print("code-change: FAIL "); print(name);
	print(" expected="); print(expected);
	print(" got="); print(got); print("\n");
}

[[noreturn]] void leave(int status)
{
	raw_syscall(nr_exit_group, status);
	__builtin_trap();
}

/* The code under test. At 0, a caller that calls `callee` calls times; the
 * callee, "mov $value, %eax; ret", is at callee_offset, on the same page or
 * another. The bytes mean the same in 32-bit and 64-bit mode.
 *
 *	0:	b9 c8 00 00 00		mov $calls, %ecx
 *	5:	e8 rel32		call callee
 *	10:	ff c9			dec %ecx
 *	12:	75 f7			jnz 5
 *	14:	c3			ret
 */
constexpr word callee_near = 0x100;
constexpr word callee_far = page;

void emit_callee(unsigned char *callee, unsigned value)
{
	callee[0] = 0xb8;
	callee[1] = static_cast<unsigned char>(value);
	callee[2] = static_cast<unsigned char>(value >> 8);
	callee[3] = static_cast<unsigned char>(value >> 16);
	callee[4] = static_cast<unsigned char>(value >> 24);
	callee[5] = 0xc3;
}

void emit_caller(unsigned char *caller, word callee_offset)
{
	const unsigned rel = static_cast<unsigned>(callee_offset - 10);
	caller[0] = 0xb9;
	caller[1] = static_cast<unsigned char>(calls);
	caller[2] = static_cast<unsigned char>(calls >> 8);
	caller[3] = 0;
	caller[4] = 0;
	caller[5] = 0xe8;
	caller[6] = static_cast<unsigned char>(rel);
	caller[7] = static_cast<unsigned char>(rel >> 8);
	caller[8] = static_cast<unsigned char>(rel >> 16);
	caller[9] = static_cast<unsigned char>(rel >> 24);
	caller[10] = 0xff;
	caller[11] = 0xc9;
	caller[12] = 0x75;
	caller[13] = 0xf7;
	caller[14] = 0xc3;
}

/* A store of a new immediate into the function, as a patcher or a JIT does
 * it: four byte stores, the way a multi-byte patch is not atomic. */
void patch(unsigned char *callee, unsigned value)
{
	volatile unsigned char *bytes = callee;
	bytes[1] = static_cast<unsigned char>(value);
	bytes[2] = static_cast<unsigned char>(value >> 8);
	bytes[3] = static_cast<unsigned char>(value >> 16);
	bytes[4] = static_cast<unsigned char>(value >> 24);
}

long call_caller(unsigned char *caller)
{
	return reinterpret_cast<long (*)()>(caller)();
}

unsigned char *map_region(word pages, long protection = prot_read | prot_write | prot_exec,
	long flags = map_private | map_anonymous)
{
	const long result = raw_syscall(nr_mmap, 0, pages * page, protection, flags, -1, 0);
	if (result < 0 && result > -4096) {
		print("code-change: setup failed\n");
		leave(4);
	}
	return reinterpret_cast<unsigned char *>(result);
}

/* A fresh region holding the caller and, near or far, the callee. */
unsigned char *new_code(unsigned value, word callee_offset, word pages = 2)
{
	auto *region = map_region(pages);
	emit_caller(region, callee_offset);
	emit_callee(region + callee_offset, value);
	return region;
}

/* Stores of every kind, from outside the function. */
void case_smc(const char *name, word callee_offset)
{
	auto *region = new_code(1, callee_offset);
	auto *callee = region + callee_offset;
	expect(name, 1, call_caller(region));
	for (unsigned value = 2; value <= 40; ++value) {
		patch(callee, value * 0x01010101U % 100000U);
		expect(name, value * 0x01010101U % 100000U, call_caller(region));
	}
}

/* The function patches the immediate it is about to execute, in the unit
 * that runs it: x86 runs the bytes it just stored.
 *
 *	0:	e8 00 00 00 00		call 5
 *	5:	58			pop %eax
 *	6:	88 48 05		mov %cl, 5(%eax)
 *	9:	b8 00 00 00 00		mov $0, %eax	(low byte patched)
 *	14:	c3			ret
 */
void case_smc_running_unit()
{
	auto *region = map_region(1);
	const unsigned char code[] = {
		0xe8, 0x00, 0x00, 0x00, 0x00, 0x58, 0x88, 0x48, 0x05,
		0xb8, 0x00, 0x00, 0x00, 0x00, 0xc3,
	};
	for (word index = 0; index < sizeof(code); ++index)
		region[index] = code[index];
	for (unsigned value = 1; value <= 120; ++value) {
		long result;
#if defined(__x86_64__)
		__asm__ volatile("call *%1"
			: "=a"(result) : "r"(region), "c"(value)
			: "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
#else
		__asm__ volatile("call *%1"
			: "=a"(result) : "r"(region), "c"(value)
			: "edx", "memory", "cc");
#endif
		expect("smc-running-unit", value, result);
	}
}

/* Execute and write taken away and given back, the store landing only once
 * the page is writable again. */
void case_protect()
{
	auto *region = new_code(1, callee_near);
	auto *callee = region + callee_near;
	expect("protect-warm", 1, call_caller(region));

	/* Write permission off and on: the page stays executable throughout,
	 * so the translation may stay, and the store after it must still
	 * retire it. */
	raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page, prot_read | prot_exec);
	expect("protect-readonly", 1, call_caller(region));
	raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page,
		prot_read | prot_write | prot_exec);
	patch(callee, 2);
	expect("protect-write-after-readonly", 2, call_caller(region));

	/* The same, and the function called while the page is read-only. */
	for (unsigned value = 3; value < 12; ++value) {
		raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page, prot_read | prot_exec);
		expect("protect-readonly-loop", value - 1, call_caller(region));
		raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page,
			prot_read | prot_write | prot_exec);
		patch(callee, value);
		expect("protect-write-after-readonly-loop", value, call_caller(region));
	}

	/* No access at all, and the bytes change while nothing can reach the
	 * page, as a loader's relocation or a JIT's patch does through another
	 * mapping of it: here, after the page is writable but not executable. */
	raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page, prot_none);
	raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page, prot_read | prot_write);
	patch(callee, 20);
	raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page,
		prot_read | prot_write | prot_exec);
	expect("protect-none-then-write", 20, call_caller(region));

	/* Writable and not executable (where the kernel lets it): a JIT's
	 * write window, then execute again. */
	for (unsigned value = 21; value < 30; ++value) {
		raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page, prot_read | prot_write);
		patch(callee, value);
		raw_syscall(nr_mprotect, reinterpret_cast<long>(region), page,
			prot_read | prot_exec);
		expect("protect-write-window", value, call_caller(region));
	}
}

/* fork write-protects every page of the parent. `parent_first` says which
 * side writes first and `child_waits` whether the parent waits for the child
 * to exit before it writes, which lets the kernel reuse the page in place. */
void case_fork(const char *name, bool parent_writes_first, bool child_exits_first)
{
	auto *region = new_code(1, callee_near);
	auto *callee = region + callee_near;
	expect(name, 1, call_caller(region));
	int pipe_to_child[2];
	int pipe_to_parent[2];
	raw_syscall(nr_pipe, reinterpret_cast<long>(pipe_to_child));
	raw_syscall(nr_pipe, reinterpret_cast<long>(pipe_to_parent));
	const long child = raw_syscall(nr_fork);
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

/* Other bytes at the same address by remapping. */
void case_remap()
{
	auto *region = new_code(1, callee_near, 1);
	expect("remap-warm", 1, call_caller(region));

	/* MAP_FIXED over the page: a zero page, then new code. */
	const long fixed = raw_syscall(nr_mmap, reinterpret_cast<long>(region), page,
		prot_read | prot_write | prot_exec, map_private | map_anonymous | map_fixed, -1, 0);
	if (fixed != reinterpret_cast<long>(region)) {
		print("code-change: MAP_FIXED failed\n");
		leave(4);
	}
	emit_caller(region, callee_near);
	emit_callee(region + callee_near, 2);
	expect("remap-fixed", 2, call_caller(region));

	/* munmap, then mmap of the same address again. */
	raw_syscall(nr_munmap, reinterpret_cast<long>(region), page);
	const long again = raw_syscall(nr_mmap, reinterpret_cast<long>(region), page,
		prot_read | prot_write | prot_exec, map_private | map_anonymous | map_fixed, -1, 0);
	if (again != reinterpret_cast<long>(region)) {
		print("code-change: mmap failed\n");
		leave(4);
	}
	emit_caller(region, callee_near);
	emit_callee(region + callee_near, 3);
	expect("remap-munmap-mmap", 3, call_caller(region));

	/* The page moves away and the old address gets other code; then the
	 * page comes back. */
	auto *other = map_region(1);
	const long moved = raw_syscall(nr_mremap, reinterpret_cast<long>(region), page, page,
		mremap_maymove | mremap_fixed, reinterpret_cast<long>(other));
	if (moved != reinterpret_cast<long>(other)) {
		print("code-change: mremap failed\n");
		leave(4);
	}
	expect("remap-moved-code", 3, call_caller(other));
	const long fresh = raw_syscall(nr_mmap, reinterpret_cast<long>(region), page,
		prot_read | prot_write | prot_exec, map_private | map_anonymous | map_fixed, -1, 0);
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

/* A second vCPU ran the function before the store and runs it after. */
volatile int request;
volatile int reply;
volatile long worker_result;
volatile int worker_stop;
unsigned char *worker_region;

void futex_wait(volatile int *address, int expected)
{
	raw_syscall(nr_futex, reinterpret_cast<long>(address), futex_wait_private, expected, 0);
}

void futex_wake(volatile int *address)
{
	raw_syscall(nr_futex, reinterpret_cast<long>(address), futex_wake_private, 1);
}

void worker(void *)
{
	int seen = 0;
	for (;;) {
		int current;
		while ((current = __atomic_load_n(&request, __ATOMIC_ACQUIRE)) == seen)
			futex_wait(&request, seen);
		seen = current;
		if (worker_stop)
			break;
		worker_result = call_caller(worker_region);
		__atomic_store_n(&reply, seen, __ATOMIC_RELEASE);
		futex_wake(&reply);
	}
	__atomic_store_n(&reply, -1, __ATOMIC_RELEASE);
	futex_wake(&reply);
}

long ask_worker(int round)
{
	__atomic_store_n(&request, round, __ATOMIC_RELEASE);
	futex_wake(&request);
	int answered;
	while ((answered = __atomic_load_n(&reply, __ATOMIC_ACQUIRE)) != round)
		futex_wait(&reply, answered);
	return worker_result;
}

extern "C" long spawn_thread(word flags, void *stack_top, void (*entry)(void *),
	void *argument);

void case_vcpu(bool write_protect_first)
{
	const char *name = write_protect_first ? "vcpu-after-protect" : "vcpu";
	worker_region = new_code(1, callee_near, 2);
	auto *callee = worker_region + callee_near;
	auto *stack = map_region(stack_size / page, prot_read | prot_write,
		map_private | map_anonymous);
	request = 0;
	reply = 0;
	worker_stop = 0;
	if (spawn_thread(thread_flags, stack + stack_size, worker, nullptr) < 0) {
		print("code-change: thread failed\n");
		leave(4);
	}
	int round = 0;
	expect(name, 1, ask_worker(++round));
	for (unsigned value = 2; value < 30; ++value) {
		if (write_protect_first) {
			raw_syscall(nr_mprotect, reinterpret_cast<long>(worker_region), page,
				prot_read | prot_exec);
			raw_syscall(nr_mprotect, reinterpret_cast<long>(worker_region), page,
				prot_read | prot_write | prot_exec);
		}
		patch(callee, value);
		/* Both vCPUs run the new code: this one and the worker's. */
		expect(name, value, call_caller(worker_region));
		expect(name, value, ask_worker(++round));
	}
	worker_stop = 1;
	__atomic_store_n(&request, ++round, __ATOMIC_RELEASE);
	futex_wake(&request);
	int answered;
	while ((answered = __atomic_load_n(&reply, __ATOMIC_ACQUIRE)) != -1)
		futex_wait(&reply, answered);
}

/* Another vCPU unmaps more executable pages than the address space keeps
 * records of while this one waits in the kernel, so this vCPU can only be
 * told that everything changed: its engine drops all its translations and
 * starts over. The function is then translated again and a store into it
 * must still be seen.
 *
 * The function waits for the other vCPU itself, so the first code this vCPU
 * translates after it starts over is on the page of the function, the page
 * it translated last: an engine whose decoder remembered that page from
 * before would not ask for it again, and no store to it would be published.
 *
 *	wait:	futex(churn_done, FUTEX_WAIT_PRIVATE, churn_expect)
 *		call callee
 *		ret
 */
volatile int churn_request;
volatile int churn_wanted;
volatile int churn_done;
volatile int churn_expect;
int churn_rounds_asked;
unsigned char *flood_region;
constexpr unsigned churn_pairs = 1500;

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
			const long scratch = raw_syscall(nr_mmap, 0, page,
				prot_read | prot_write | prot_exec,
				map_private | map_anonymous, -1, 0);
			if (scratch < 0 && scratch > -4096)
				break;
			*reinterpret_cast<volatile unsigned char *>(scratch) = 0xc3;
			raw_syscall(nr_munmap, scratch, page);
		}
		__atomic_store_n(&churn_done, current, __ATOMIC_RELEASE);
		futex_wake(&churn_done);
	}
}

void emit_waiting_function(unsigned char *at, word callee_offset)
{
	unsigned char *out = at;
	const auto put = [&](unsigned char byte) { *out++ = byte; };
	const auto put32 = [&](unsigned long value) {
		for (int shift = 0; shift < 32; shift += 8)
			put(static_cast<unsigned char>(value >> shift));
	};
#if defined(__x86_64__)
	const auto put64 = [&](unsigned long value) {
		for (int shift = 0; shift < 64; shift += 8)
			put(static_cast<unsigned char>(value >> shift));
	};
	put(0x48); put(0xb8); put64(reinterpret_cast<unsigned long>(&churn_expect));
	put(0x8b); put(0x10);				/* mov (%rax), %edx */
	put(0xb8); put32(nr_futex);			/* mov $futex, %eax */
	put(0x48); put(0xbf); put64(reinterpret_cast<unsigned long>(&churn_done));
	put(0xbe); put32(futex_wait_private);		/* mov $op, %esi */
	put(0x45); put(0x31); put(0xd2);		/* xor %r10d, %r10d */
	put(0x0f); put(0x05);				/* syscall */
#else
	put(0x53); put(0x56);				/* push %ebx; push %esi */
	put(0xb8); put32(reinterpret_cast<unsigned>(&churn_expect));
	put(0x8b); put(0x10);				/* mov (%eax), %edx */
	put(0xb8); put32(nr_futex);
	put(0xbb); put32(reinterpret_cast<unsigned>(&churn_done));
	put(0xb9); put32(futex_wait_private);		/* mov $op, %ecx */
	put(0x31); put(0xf6);				/* xor %esi, %esi */
	put(0xcd); put(0x80);				/* int $0x80 */
#endif
	put(0xe8);					/* call callee */
	put32(callee_offset - static_cast<word>(out + 4 - at));
#if !defined(__x86_64__)
	put(0x5e); put(0x5b);				/* pop %esi; pop %ebx */
#endif
	put(0xc3);
}

/* Runs the function; `block` makes its wait last until the other vCPU says
 * it is done, and otherwise the wait finds the word already different. */
long flood_call(int done, bool block)
{
	churn_expect = block ? done : done + 1000;
	return call_caller(flood_region);
}

/* Asks the other vCPU for `pairs` map and unmap pairs, and runs the function
 * through them. */
long flood_round(int pairs)
{
	const int done = __atomic_load_n(&churn_done, __ATOMIC_ACQUIRE);
	churn_wanted = pairs;
	__atomic_store_n(&churn_request, ++churn_rounds_asked, __ATOMIC_RELEASE);
	futex_wake(&churn_request);
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
	if (spawn_thread(thread_flags, stack + stack_size, churn, nullptr) < 0) {
		print("code-change: thread failed\n");
		leave(4);
	}
	expect("flood-warm", 1, flood_call(0, false));
	for (unsigned round = 0; round < 4; ++round) {
		const unsigned first = 20 + round * 10;
		/* Everything this vCPU runs around the function is translated
		 * before the flood, so that the function's page is the last one the
		 * decoder read. */
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
	futex_wake(&churn_request);
}

/* One block of one thread patches two instructions of a function, each with
 * one byte store, and no link or system call separates the stores, while
 * three other vCPUs run the function in a loop that enters the engine often.
 * The thread then flags that it is done through memory that holds no code,
 * and every vCPU that saw the flag crosses a barrier -- a system call, or one
 * of the serializing instructions user mode has, CPUID and IRET, which the
 * cross-modifying code protocol makes a barrier -- and must run the function
 * with both patches.
 *
 *	function:	xor %eax, %eax
 *			mov $a, %al		patched first
 *			xor %edx, %edx
 *			mov $d, %dl		patched second
 *			ret
 *
 * The writer is straight-line code, "mov %sil, 3(%rdi); fld1; fsin * delay;
 * fstp %st(0); mov %dl, 7(%rdi); ret" in 64-bit mode and the same with its
 * arguments from the stack in 32-bit mode. The delay is what lets a reader
 * that noticed the first store read the function before the second lands. */
#ifndef TWO_PATCH_ROUNDS
#define TWO_PATCH_ROUNDS 2000
#endif
#ifndef TWO_PATCH_DELAY
#define TWO_PATCH_DELAY 40
#endif
/* What a reader does between seeing the flag and running the function. */
constexpr int barrier_system_call = 0;
constexpr int barrier_cpuid = 1;
constexpr int barrier_iret = 2;
constexpr int patch_readers = 3;
constexpr unsigned patch_rounds = TWO_PATCH_ROUNDS;
constexpr unsigned patch_delay = TWO_PATCH_DELAY;
constexpr word patch_first = 3;
constexpr word patch_second = 7;

volatile int patch_go;
volatile int patch_flag;
volatile int patch_stop;
volatile int patch_barrier;
volatile int patch_armed[patch_readers];
volatile int patch_finished[patch_readers];
volatile unsigned patch_seen[patch_readers];
unsigned char *patch_function;

void emit_patch_function(unsigned char *at, unsigned first, unsigned second)
{
	const unsigned char code[] = {
		0x31, 0xc0, 0xb0, static_cast<unsigned char>(first),
		0x31, 0xd2, 0xb2, static_cast<unsigned char>(second), 0xc3,
	};
	for (word index = 0; index < sizeof(code); ++index)
		at[index] = code[index];
}

/* void writer(unsigned char *function, unsigned first, unsigned second) */
void emit_patch_writer(unsigned char *at, unsigned delay)
{
	unsigned char *out = at;
	const auto put = [&](unsigned char byte) { *out++ = byte; };
#if defined(__x86_64__)
	put(0x40); put(0x88); put(0x77); put(patch_first);	/* mov %sil, 3(%rdi) */
#else
	put(0x8b); put(0x4c); put(0x24); put(0x04);		/* mov 4(%esp), %ecx */
	put(0x8b); put(0x44); put(0x24); put(0x08);		/* mov 8(%esp), %eax */
	put(0x8b); put(0x54); put(0x24); put(0x0c);		/* mov 12(%esp), %edx */
	put(0x88); put(0x41); put(patch_first);		/* mov %al, 3(%ecx) */
#endif
	put(0xd9); put(0xe8);					/* fld1 */
	for (unsigned count = 0; count < delay; ++count) {
		put(0xd9); put(0xfe);				/* fsin */
	}
	put(0xdd); put(0xd8);					/* fstp %st(0) */
#if defined(__x86_64__)
	put(0x88); put(0x57); put(patch_second);		/* mov %dl, 7(%rdi) */
#else
	put(0x88); put(0x51); put(patch_second);		/* mov %dl, 7(%ecx) */
#endif
	put(0xc3);
}

/* The function's two results, al and dl, as one number. */
unsigned call_patch_function(unsigned char *function)
{
	word low, high;
#if defined(__x86_64__)
	__asm__ volatile("call *%2"
		: "=a"(low), "=d"(high) : "r"(function)
		: "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
#else
	__asm__ volatile("call *%2"
		: "=a"(low), "=d"(high) : "r"(function)
		: "ecx", "memory", "cc");
#endif
	return (static_cast<unsigned>(low) & 0xffU) |
		((static_cast<unsigned>(high) & 0xffU) << 8);
}

/* The serializing instructions user mode has: the cross-modifying code
 * protocol lets a processor that saw the flag run the new code after any of
 * them. */
void serialize_with_cpuid()
{
	unsigned a = 0, b, c = 0, d;
	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) : : "memory");
}

/* An IRET to the next instruction at the same privilege level, as the
 * kernel's sync_core() does it. */
void serialize_with_iret()
{
#if defined(__x86_64__)
	unsigned long scratch;
	__asm__ volatile(
		"mov %%ss, %0\n\t"
		"pushq %q0\n\t"
		"pushq %%rsp\n\t"
		"addq $8, (%%rsp)\n\t"
		"pushfq\n\t"
		"mov %%cs, %0\n\t"
		"pushq %q0\n\t"
		"pushq $1f\n\t"
		"iretq\n\t"
		"1:"
		: "=&r"(scratch) : : "cc", "memory");
#else
	__asm__ volatile(
		"pushfl\n\t"
		"pushl %%cs\n\t"
		"pushl $1f\n\t"
		"iret\n\t"
		"1:"
		: : : "cc", "memory");
#endif
}

void futex_wake_all(volatile int *address)
{
	raw_syscall(nr_futex, reinterpret_cast<long>(address), futex_wake_private, 0x7fffffff);
}

void patch_reader(void *argument)
{
	const int index = static_cast<int>(reinterpret_cast<word>(argument));
	int joined = 0;
	for (;;) {
		int round;
		while ((round = __atomic_load_n(&patch_go, __ATOMIC_ACQUIRE)) == joined)
			futex_wait(&patch_go, joined);
		joined = round;
		if (patch_stop)
			break;
		/* Translated, linked and run with the bytes of the last round. */
		(void)call_patch_function(patch_function);
		__atomic_store_n(&patch_armed[index], round, __ATOMIC_RELEASE);
		while (__atomic_load_n(&patch_flag, __ATOMIC_ACQUIRE) != round) {
			(void)call_patch_function(patch_function);
			raw_syscall(nr_getppid);
		}
		if (patch_barrier == barrier_system_call)
			raw_syscall(nr_getppid);
		else if (patch_barrier == barrier_cpuid)
			serialize_with_cpuid();
		else if (patch_barrier == barrier_iret)
			serialize_with_iret();
		patch_seen[index] = call_patch_function(patch_function);
		__atomic_store_n(&patch_finished[index], round, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&patch_finished[index], -1, __ATOMIC_RELEASE);
}

void wait_for_readers(volatile int *states, int round)
{
	for (int index = 0; index < patch_readers; ++index)
		while (__atomic_load_n(&states[index], __ATOMIC_ACQUIRE) != round)
			raw_syscall(nr_getppid);
}

long now_microseconds()
{
	long stamp[2];
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(stamp));
	return stamp[0] * 1000000L + stamp[1] / 1000L;
}

void case_two_patch(int barrier)
{
	patch_barrier = barrier;
	patch_stop = 0;
	patch_go = 0;
	patch_flag = 0;
	for (int index = 0; index < patch_readers; ++index) {
		patch_armed[index] = 0;
		patch_finished[index] = 0;
		patch_seen[index] = 0;
	}
	auto *region = map_region(2);
	patch_function = region + callee_near;
	emit_patch_function(patch_function, 0, 0);
	auto *writer_code = region + page + 0x40;
	emit_patch_writer(writer_code, patch_delay);
	const auto writer = reinterpret_cast<void (*)(unsigned char *, unsigned, unsigned)>(
		writer_code);
	auto *stacks = map_region(patch_readers * stack_size / page,
		prot_read | prot_write, map_private | map_anonymous);
	for (int index = 0; index < patch_readers; ++index)
		if (spawn_thread(thread_flags, stacks + (index + 1) * stack_size,
			    patch_reader, reinterpret_cast<void *>(static_cast<word>(index))) < 0) {
			print("code-change: thread failed\n");
			leave(4);
		}
	/* What a reader that missed a patch ran: the function with the first
	 * patch only, with neither patch, or some other mixture. */
	unsigned first_only = 0, neither = 0, other = 0;
	unsigned reported = 0;
	const long started = now_microseconds();
	unsigned previous = 0;
	for (unsigned round = 1; round <= patch_rounds; ++round) {
		const unsigned first = 1 + round % 200;
		const unsigned second = 101 + round * 7 % 151;
		const unsigned wanted = first | (second << 8);
		__atomic_store_n(&patch_go, static_cast<int>(round), __ATOMIC_RELEASE);
		futex_wake_all(&patch_go);
		wait_for_readers(patch_armed, static_cast<int>(round));
		writer(patch_function, first, second);
		__atomic_store_n(&patch_flag, static_cast<int>(round), __ATOMIC_RELEASE);
		wait_for_readers(patch_finished, static_cast<int>(round));
		for (int index = 0; index < patch_readers; ++index) {
			const unsigned got = patch_seen[index];
			if (got == wanted)
				continue;
			if (got == ((previous & 0xff00U) | first))
				++first_only;
			else if (got == previous)
				++neither;
			else
				++other;
			if (reported++ < 6) {
				print("code-change: two-patch round "); print(round);
				print(" reader "); print(index);
				print(" wanted "); print(first); print(","); print(second);
				print(" got "); print(got & 0xffU); print(",");
				print(got >> 8); print("\n");
			}
		}
		expect("two-patch-writer", wanted, call_patch_function(patch_function));
		previous = wanted;
	}
	const long elapsed = now_microseconds() - started;
	patch_stop = 1;
	__atomic_store_n(&patch_go, static_cast<int>(patch_rounds + 1), __ATOMIC_RELEASE);
	futex_wake_all(&patch_go);
	wait_for_readers(patch_finished, -1);
	print("code-change: two-patch rounds="); print(patch_rounds);
	print(" delay="); print(patch_delay);
	print(" microseconds="); print(elapsed);
	print(" barrier=");
	print(barrier == barrier_system_call ? "system-call" :
		barrier == barrier_cpuid ? "cpuid" : "iret");
	print(" first-patch-only="); print(first_only);
	print(" neither-patch="); print(neither);
	print(" other="); print(other); print("\n");
	expect("two-patch", 0, first_only + neither + other);
}


/* Code in a file that two mappings of one process show, or that two
 * processes share: the function is stored through the writable mapping (W)
 * and called through the executable one (X). The store reaches the frame
 * whatever its address. */
struct Alias final {
	unsigned char *x;
	unsigned char *w;
	long fd;
	word pages;
};

long open_file(bool tmpfs)
{
	if (!tmpfs) {
		const long fd = raw_syscall(nr_memfd_create,
			reinterpret_cast<long>("code-change-alias"), 0);
		if (fd < 0) {
			print("code-change: memfd_create failed\n");
			leave(4);
		}
		return fd;
	}
	const char *path = "/tmp/.code-change-alias";
	raw_syscall(nr_unlink, reinterpret_cast<long>(path));
	const long fd = raw_syscall(nr_open, reinterpret_cast<long>(path),
		o_rdwr | o_creat | o_excl, 0700);
	if (fd < 0) {
		print("code-change: open failed\n");
		leave(4);
	}
	raw_syscall(nr_unlink, reinterpret_cast<long>(path));
	return fd;
}

unsigned char *map_file(long fd, word pages, long protection, unsigned char *at = nullptr)
{
	const long result = raw_syscall(nr_mmap, reinterpret_cast<long>(at), pages * page,
		protection, map_shared | (at != nullptr ? map_fixed : 0), fd, 0);
	if (result < 0 && result > -4096) {
		print("code-change: file mapping failed\n");
		leave(4);
	}
	return reinterpret_cast<unsigned char *>(result);
}

/* A file of `pages` pages holding the caller and the callee, mapped
 * writable once and executable once. `touch_w` says whether the code goes
 * in through W; without it a mapping of its own fills the file and goes
 * away, so that W is never touched. */
Alias new_alias(bool tmpfs, unsigned value, word callee_offset, word pages = 2,
	bool touch_w = true)
{
	Alias alias{};
	alias.fd = open_file(tmpfs);
	alias.pages = pages;
	raw_syscall(nr_ftruncate, alias.fd, pages * page);
	if (touch_w) {
		alias.w = map_file(alias.fd, pages, prot_read | prot_write);
		emit_caller(alias.w, callee_offset);
		emit_callee(alias.w + callee_offset, value);
	} else {
		auto *filler = map_file(alias.fd, pages, prot_read | prot_write);
		emit_caller(filler, callee_offset);
		emit_callee(filler + callee_offset, value);
		raw_syscall(nr_munmap, reinterpret_cast<long>(filler), pages * page);
		alias.w = map_file(alias.fd, pages, prot_read | prot_write);
	}
	alias.x = map_file(alias.fd, pages, prot_read | prot_exec);
	return alias;
}

/* The function called through X, stored through W. */
void case_alias(const char *name, bool tmpfs, word callee_offset)
{
	auto alias = new_alias(tmpfs, 1, callee_offset);
	auto *callee = alias.w + callee_offset;
	expect(name, 1, call_caller(alias.x));
	for (unsigned value = 2; value <= 40; ++value) {
		patch(callee, value * 0x01010101U % 100000U);
		expect(name, value * 0x01010101U % 100000U, call_caller(alias.x));
	}
}

/* Mapped after the stores, then stored to again, and a fresh address each
 * round, which no translation has seen. */
void case_alias_late_mapping()
{
	const char *name = "alias-store-before-exec-mapping";
	auto alias = new_alias(false, 1, callee_near, 2);
	auto *callee = alias.w + callee_near;
	raw_syscall(nr_munmap, reinterpret_cast<long>(alias.x), alias.pages * page);
	patch(callee, 5);
	auto *x = map_file(alias.fd, alias.pages, prot_read | prot_exec);
	expect(name, 5, call_caller(x));
	patch(callee, 6);
	expect(name, 6, call_caller(x));
	for (unsigned value = 7; value < 14; ++value) {
		patch(callee, value);
		auto *fresh = map_file(alias.fd, alias.pages, prot_read | prot_exec);
		expect("alias-fresh-address", value, call_caller(fresh));
	}
}

/* A second process stores through its own mapping, forked before the first
 * call (so each side has its own translation) or after it. */
void case_alias_other_process(const char *name, bool fork_before_warm)
{
	auto alias = new_alias(false, 1, callee_near);
	auto *callee = alias.w + callee_near;
	int pipe_to_child[2];
	int pipe_to_parent[2];
	raw_syscall(nr_pipe, reinterpret_cast<long>(pipe_to_child));
	raw_syscall(nr_pipe, reinterpret_cast<long>(pipe_to_parent));
	if (!fork_before_warm)
		expect(name, 1, call_caller(alias.x));
	const long child = raw_syscall(nr_fork);
	if (child < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	char token = 0;
	if (child == 0) {
		raw_syscall(nr_read, pipe_to_child[0], reinterpret_cast<long>(&token), 1);
		patch(callee, 7);
		raw_syscall(nr_write, pipe_to_parent[1], reinterpret_cast<long>(&token), 1);
		/* The parent stores next; the child runs what it stored. */
		raw_syscall(nr_read, pipe_to_child[0], reinterpret_cast<long>(&token), 1);
		const long seen = call_caller(alias.x);
		leave(seen == 9 ? 0 : 1);
	}
	if (fork_before_warm)
		expect(name, 1, call_caller(alias.x));
	raw_syscall(nr_write, pipe_to_child[1], reinterpret_cast<long>(&token), 1);
	raw_syscall(nr_read, pipe_to_parent[0], reinterpret_cast<long>(&token), 1);
	expect(name, 7, call_caller(alias.x));
	patch(callee, 9);
	expect(name, 9, call_caller(alias.x));
	raw_syscall(nr_write, pipe_to_child[1], reinterpret_cast<long>(&token), 1);
	int status = 0;
	raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	expect(name, 0, status);
}

/* The parent warms X, forks, the child exits, the parent calls X again and
 * then stores through W. */
void case_alias_fork_rewarm()
{
	const char *name = "alias-fork-parent-rewarm-then-store";
	auto alias = new_alias(false, 1, callee_near);
	auto *callee = alias.w + callee_near;
	expect(name, 1, call_caller(alias.x));
	const long child = raw_syscall(nr_fork);
	if (child < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	if (child == 0)
		leave(0);
	int status = 0;
	raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	expect(name, 0, status);
	expect(name, 1, call_caller(alias.x));
	patch(callee, 4);
	expect(name, 4, call_caller(alias.x));
	patch(callee, 8);
	expect(name, 8, call_caller(alias.x));
}

/* X made writable and executable-less again around an edit elsewhere on the
 * page, then called, then stored to through W. */
void case_alias_protect_cycle()
{
	const char *name = "alias-mprotect-cycle-rewarm-then-store";
	auto alias = new_alias(false, 1, callee_near);
	auto *callee = alias.w + callee_near;
	expect(name, 1, call_caller(alias.x));
	raw_syscall(nr_mprotect, reinterpret_cast<long>(alias.x), alias.pages * page,
		prot_read | prot_write);
	alias.x[0x800] = 0x90;
	raw_syscall(nr_mprotect, reinterpret_cast<long>(alias.x), alias.pages * page,
		prot_read | prot_exec);
	expect(name, 1, call_caller(alias.x));
	patch(callee, 3);
	expect(name, 3, call_caller(alias.x));
	patch(callee, 6);
	expect(name, 6, call_caller(alias.x));
}

/* X unmapped and mapped again at the same address, called before the store. */
void case_alias_unmap_remap()
{
	const char *name = "alias-unmap-remap-x";
	auto alias = new_alias(false, 1, callee_near);
	auto *callee = alias.w + callee_near;
	expect(name, 1, call_caller(alias.x));
	raw_syscall(nr_munmap, reinterpret_cast<long>(alias.x), alias.pages * page);
	alias.x = map_file(alias.fd, alias.pages, prot_read | prot_exec, alias.x);
	expect(name, 1, call_caller(alias.x));
	patch(callee, 4);
	expect(name, 4, call_caller(alias.x));
	patch(callee, 8);
	expect(name, 8, call_caller(alias.x));
}

/* The kernel stores: read(2) into W after X ran. With `untouched_w` the
 * process never touched W before the read. Both rows fail, on both engines,
 * when the kernel's write is not published at the frame its alias names. They
 * do not tell that publication from the fallback a write takes when the host
 * does not resolve the alias, which publishes everywhere; the memory layer's
 * kernel-write rows count those writes. */
void case_alias_kernel_write(bool untouched_w)
{
	const char *name = untouched_w ? "alias-kernel-write-untouched-w" : "alias-kernel-write";
	auto alias = new_alias(false, 1, callee_near, 2, !untouched_w);
	auto *callee = alias.w + callee_near;
	expect(name, 1, call_caller(alias.x));
	for (unsigned value = 2; value < 8; ++value) {
		int channel[2];
		raw_syscall(nr_pipe, reinterpret_cast<long>(channel));
		unsigned char immediate[4] = {static_cast<unsigned char>(value), 0, 0, 0};
		raw_syscall(nr_write, channel[1], reinterpret_cast<long>(immediate), 4);
		raw_syscall(nr_read, channel[0], reinterpret_cast<long>(callee + 1), 4);
		raw_syscall(nr_close, channel[0]);
		raw_syscall(nr_close, channel[1]);
		expect(name, value, call_caller(alias.x));
	}
}

/* process_vm_writev(2): the kernel stores into another process's memory,
 * so no translated store sees it. A second process maps the file writable
 * and not as code (the first maps it executable and runs it), and a third
 * writes the second's page. The second has touched the page when `touched`,
 * so its address space holds an entry for it; otherwise the kernel fills the
 * entry at a fault the host never sees. The write names the frame it wrote
 * either way, and it is published at that frame's holders. */
struct IoVec final {
	word base;
	word length;
};

void case_alias_remote_write(bool touched)
{
	const char *name = touched ? "alias-remote-write" :
		"alias-remote-write-untouched";
	auto alias = new_alias(false, 1, callee_near);
	expect(name, 1, call_caller(alias.x));
	int to_target[2];
	int from_target[2];
	int to_writer[2];
	int from_writer[2];
	raw_syscall(nr_pipe, reinterpret_cast<long>(to_target));
	raw_syscall(nr_pipe, reinterpret_cast<long>(from_target));
	raw_syscall(nr_pipe, reinterpret_cast<long>(to_writer));
	raw_syscall(nr_pipe, reinterpret_cast<long>(from_writer));
	const long target = raw_syscall(nr_fork);
	if (target < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	if (target == 0) {
		/* Only a data view of the frame: the executable one goes. */
		raw_syscall(nr_munmap, reinterpret_cast<long>(alias.x),
			alias.pages * page);
		auto *mapped = map_file(alias.fd, alias.pages, prot_read | prot_write);
		if (touched) {
			/* Its own store puts the page in this process's address
			 * space, as the process that a debugger writes into has
			 * touched it. */
			volatile unsigned char *touch = mapped + callee_near + 8;
			*touch = *touch;
		}
		const word view = reinterpret_cast<word>(mapped);
		raw_syscall(nr_write, from_target[1], reinterpret_cast<long>(&view),
			sizeof(view));
		char token = 0;
		raw_syscall(nr_read, to_target[0], reinterpret_cast<long>(&token), 1);
		leave(0);
	}
	word view = 0;
	raw_syscall(nr_read, from_target[0], reinterpret_cast<long>(&view),
		sizeof(view));
	const long writer = raw_syscall(nr_fork);
	if (writer < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	if (writer == 0) {
		for (;;) {
			unsigned char value = 0;
			raw_syscall(nr_read, to_writer[0], reinterpret_cast<long>(&value), 1);
			if (value == 0)
				leave(0);
			unsigned char immediate[4] = {value, 0, 0, 0};
			IoVec local{reinterpret_cast<word>(immediate), 4};
			IoVec remote{view + callee_near + 1, 4};
			const long done = raw_syscall(nr_process_vm_writev, target,
				reinterpret_cast<long>(&local), 1,
				reinterpret_cast<long>(&remote), 1, 0);
			char reply = done == 4 ? 1 : 0;
			raw_syscall(nr_write, from_writer[1], reinterpret_cast<long>(&reply), 1);
		}
	}
	/* The forks flushed this process's translations: run it again so that
	 * each write below meets a warm one. */
	expect(name, 1, call_caller(alias.x));
	for (unsigned value = 2; value < 8; ++value) {
		unsigned char command = static_cast<unsigned char>(value);
		raw_syscall(nr_write, to_writer[1], reinterpret_cast<long>(&command), 1);
		char reply = 0;
		raw_syscall(nr_read, from_writer[0], reinterpret_cast<long>(&reply), 1);
		expect(touched ? "alias-remote-write-done" :
			"alias-remote-write-untouched-done", 1, reply);
		expect(name, value, call_caller(alias.x));
	}
	unsigned char leave_command = 0;
	raw_syscall(nr_write, to_writer[1], reinterpret_cast<long>(&leave_command), 1);
	char token = 0;
	raw_syscall(nr_write, to_target[1], reinterpret_cast<long>(&token), 1);
	const long children[2] = {writer, target};
	for (const long child : children) {
		int status = 0;
		raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
		expect(touched ? "alias-remote-write-exit" :
			"alias-remote-write-untouched-exit", 0, status);
	}
}

/* userfaultfd(2) on the writer's source buffer: the kernel's read of it
 * waits for a monitor, with the target's pages already pinned and its
 * memory map unlocked. */
struct UffdApi final {
	unsigned long long api;
	unsigned long long features;
	unsigned long long ioctls;
};

struct UffdRange final {
	unsigned long long start;
	unsigned long long length;
};

struct UffdRegister final {
	UffdRange range;
	unsigned long long mode;
	unsigned long long ioctls;
};

struct UffdCopy final {
	unsigned long long destination;
	unsigned long long source;
	unsigned long long length;
	unsigned long long mode;
	long long copied;
};

struct UffdMessage final {
	unsigned char event;
	unsigned char reserved1;
	unsigned short reserved2;
	unsigned reserved3;
	unsigned long long flags;
	unsigned long long address;
	unsigned long long reserved4;
};

static_assert(sizeof(UffdMessage) == 32);

constexpr unsigned long uffdio_api = 0xc018aa3fUL, uffdio_register = 0xc020aa00UL,
	uffdio_copy = 0xc028aa03UL;
constexpr unsigned long long uffd_api = 0xaa, uffd_register_missing = 1;
constexpr unsigned char uffd_event_pagefault = 0x12;
/* The guest's page, whatever the host's: the unit of UFFDIO_COPY. */
constexpr unsigned long long uffd_page = 4096;

/* A process writes another's page with process_vm_writev(2) while that
 * process, between the kernel's pin of the page and its store, puts another
 * frame at the same address. The writer's source buffer is registered with
 * userfaultfd, so the copy from it waits for the target, which is the
 * monitor, after the pin. The store lands in the frame the kernel pinned,
 * which this process runs as code; the frame mapped at the address now has
 * no code. The kernel names the frame it wrote, so the code is new when it
 * is called again, the data mapping in the target still shows the other
 * frame, and the first frame holds the new bytes.
 *
 *	R	runs the frame (X) and checks
 *	W	writes: process_vm_writev(T, S) with S registered
 *	T	holds the frame (A), then remaps A to another frame when the
 *		fault on S arrives, and resolves the fault */
void case_alias_remote_write_remapped()
{
	const char *name = "alias-remote-write-remapped";
	auto alias = new_alias(false, 1, callee_near);
	auto other = new_alias(false, 3, callee_near);
	expect(name, 1, call_caller(alias.x));
	int to_writer_view[2];
	int from_writer[2];
	int to_writer[2];
	int from_target[2];
	int to_target[2];
	raw_syscall(nr_pipe, reinterpret_cast<long>(to_writer_view));
	raw_syscall(nr_pipe, reinterpret_cast<long>(from_writer));
	raw_syscall(nr_pipe, reinterpret_cast<long>(to_writer));
	raw_syscall(nr_pipe, reinterpret_cast<long>(from_target));
	raw_syscall(nr_pipe, reinterpret_cast<long>(to_target));
	const long writer = raw_syscall(nr_fork);
	if (writer < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	if (writer == 0) {
		auto *source = map_region(1, prot_read | prot_write);
		const long monitor = raw_syscall(nr_userfaultfd, 0);
		UffdApi api{uffd_api, 0, 0};
		UffdRegister registration{
			{reinterpret_cast<word>(source), page}, uffd_register_missing, 0};
		if (monitor < 0 ||
		    raw_syscall(nr_ioctl, monitor, static_cast<long>(uffdio_api),
			    reinterpret_cast<long>(&api)) != 0 ||
		    raw_syscall(nr_ioctl, monitor, static_cast<long>(uffdio_register),
			    reinterpret_cast<long>(&registration)) != 0) {
			print("code-change: userfaultfd failed\n");
			leave(4);
		}
		const long target = raw_syscall(nr_fork);
		if (target < 0) {
			print("code-change: fork failed\n");
			leave(4);
		}
		if (target == 0) {
			/* Only a data view of the frame: the others go. */
			raw_syscall(nr_munmap, reinterpret_cast<long>(alias.x),
				alias.pages * page);
			raw_syscall(nr_munmap, reinterpret_cast<long>(alias.w),
				alias.pages * page);
			raw_syscall(nr_munmap, reinterpret_cast<long>(other.x),
				other.pages * page);
			raw_syscall(nr_munmap, reinterpret_cast<long>(other.w),
				other.pages * page);
			auto *mapped = map_file(alias.fd, alias.pages, prot_read | prot_write);
			/* Its own store puts the page in this process's address
			 * space. */
			volatile unsigned char *touch = mapped + callee_near + 8;
			*touch = *touch;
			const word view = reinterpret_cast<word>(mapped);
			raw_syscall(nr_write, to_writer_view[1],
				reinterpret_cast<long>(&view), sizeof(view));
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
			map_file(other.fd, other.pages, prot_read | prot_write, mapped);
			*touch = *touch;
			char token = 0;
			raw_syscall(nr_write, from_target[1], reinterpret_cast<long>(&token), 1);
			raw_syscall(nr_read, to_target[0], reinterpret_cast<long>(&token), 1);
			/* The writer's buffer gets the new immediate, and the
			 * copy goes on. */
			auto *carrier = map_region(1, prot_read | prot_write);
			carrier[0] = 2;
			UffdCopy fill{reinterpret_cast<word>(source),
				reinterpret_cast<word>(carrier), uffd_page, 0, 0};
			if (raw_syscall(nr_ioctl, monitor, static_cast<long>(uffdio_copy),
				    reinterpret_cast<long>(&fill)) != 0 ||
			    fill.copied != static_cast<long long>(uffd_page)) {
				print("code-change: userfaultfd copy failed\n");
				leave(4);
			}
			raw_syscall(nr_read, to_target[0], reinterpret_cast<long>(&token), 1);
			const volatile unsigned char *bytes = mapped + callee_near + 1;
			const unsigned seen = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
				(static_cast<unsigned>(bytes[3]) << 24);
			raw_syscall(nr_write, from_target[1], reinterpret_cast<long>(&seen),
				sizeof(seen));
			leave(0);
		}
		word view = 0;
		raw_syscall(nr_read, to_writer_view[0], reinterpret_cast<long>(&view),
			sizeof(view));
		char token = 0;
		raw_syscall(nr_write, from_writer[1], reinterpret_cast<long>(&token), 1);
		raw_syscall(nr_read, to_writer[0], reinterpret_cast<long>(&token), 1);
		IoVec local{reinterpret_cast<word>(source), 4};
		IoVec remote{view + callee_near + 1, 4};
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
	/* The forks flushed this process's translations: run it again so that
	 * the write meets a warm one. */
	expect(name, 1, call_caller(alias.x));
	raw_syscall(nr_write, to_writer[1], reinterpret_cast<long>(&token), 1);
	/* The target has put the other frame at the address. */
	raw_syscall(nr_read, from_target[0], reinterpret_cast<long>(&token), 1);
	expect(name, 1, call_caller(alias.x));
	raw_syscall(nr_write, to_target[1], reinterpret_cast<long>(&token), 1);
	char reply = 0;
	raw_syscall(nr_read, from_writer[0], reinterpret_cast<long>(&reply), 1);
	expect("alias-remote-write-remapped-done", 1, reply);
	expect(name, 2, call_caller(alias.x));
	/* The store was made in the frame that was pinned, not the one that
	 * is mapped now. */
	const volatile unsigned char *frame = alias.w + callee_near + 1;
	expect("alias-remote-write-remapped-frame", 2, frame[0]);
	raw_syscall(nr_write, to_target[1], reinterpret_cast<long>(&token), 1);
	unsigned seen = 0;
	raw_syscall(nr_read, from_target[0], reinterpret_cast<long>(&seen), sizeof(seen));
	expect("alias-remote-write-remapped-target", 3, seen);
	int status = 0;
	raw_syscall(nr_wait4, writer, reinterpret_cast<long>(&status), 0, 0);
	expect("alias-remote-write-remapped-exit", 0, status);
}

/* The kernel's own copy into the page cache: nothing maps the file writable,
 * the code went into the file before any mapping existed, and the kernel
 * rewrites the frame under the mapping that runs it (x86 hardware fetches
 * the new bytes, so the call after the write must return them). The writer
 * is this process, or a process forked before the code first ran, which
 * serves the writes over a pipe: a fork between the run and the write
 * publishes a code change of its own and would hide a missed publication. */
enum class PageWrite { pwrite, splice };

long do_pwrite(long fd, const unsigned char *bytes, word length, word offset)
{
#if defined(__x86_64__)
	return raw_syscall(nr_pwrite, fd, reinterpret_cast<long>(bytes),
		static_cast<long>(length), static_cast<long>(offset));
#else
	return raw_syscall(nr_pwrite, fd, reinterpret_cast<long>(bytes),
		static_cast<long>(length), static_cast<long>(offset), 0);
#endif
}

bool page_write(PageWrite how, long fd, word offset, const unsigned char *bytes,
	word length)
{
	if (how == PageWrite::pwrite)
		return do_pwrite(fd, bytes, length, offset) == static_cast<long>(length);
	int channel[2];
	if (raw_syscall(nr_pipe, reinterpret_cast<long>(channel)) != 0)
		return false;
	long long at = offset;
	raw_syscall(nr_write, channel[1], reinterpret_cast<long>(bytes),
		static_cast<long>(length));
	const long moved = raw_syscall(nr_splice, channel[0], 0, fd,
		reinterpret_cast<long>(&at), static_cast<long>(length), 0);
	raw_syscall(nr_close, channel[0]);
	raw_syscall(nr_close, channel[1]);
	return moved == static_cast<long>(length);
}

struct PageWriteRequest final {
	int how;
	word offset;
	word length;
	unsigned char bytes[8];
};

struct PageWriteServer final {
	long child;
	int to[2];
	int from[2];
};

PageWriteServer start_page_write_server(long fd)
{
	PageWriteServer server{};
	if (raw_syscall(nr_pipe, reinterpret_cast<long>(server.to)) != 0 ||
	    raw_syscall(nr_pipe, reinterpret_cast<long>(server.from)) != 0) {
		print("code-change: pipe failed\n");
		leave(4);
	}
	server.child = raw_syscall(nr_fork);
	if (server.child < 0) {
		print("code-change: fork failed\n");
		leave(4);
	}
	if (server.child == 0) {
		PageWriteRequest request{};
		while (raw_syscall(nr_read, server.to[0], reinterpret_cast<long>(&request),
			       sizeof(request)) == static_cast<long>(sizeof(request)) &&
		       request.how >= 0) {
			const unsigned char ok = page_write(static_cast<PageWrite>(request.how),
				fd, request.offset, request.bytes, request.length) ? 1 : 0;
			raw_syscall(nr_write, server.from[1], reinterpret_cast<long>(&ok), 1);
		}
		leave(0);
	}
	return server;
}

bool serve_page_write(const PageWriteServer &server, PageWrite how, word offset,
	const unsigned char *bytes, word length)
{
	PageWriteRequest request{};
	request.how = static_cast<int>(how);
	request.offset = offset;
	request.length = length;
	for (word index = 0; index < length && index < sizeof(request.bytes); ++index)
		request.bytes[index] = bytes[index];
	unsigned char ok = 0;
	raw_syscall(nr_write, server.to[1], reinterpret_cast<long>(&request),
		sizeof(request));
	return raw_syscall(nr_read, server.from[0], reinterpret_cast<long>(&ok), 1) == 1 &&
		ok == 1;
}

void stop_page_write_server(const PageWriteServer &server)
{
	PageWriteRequest request{};
	request.how = -1;
	raw_syscall(nr_write, server.to[1], reinterpret_cast<long>(&request),
		sizeof(request));
	int status = -1;
	raw_syscall(nr_wait4, server.child, reinterpret_cast<long>(&status), 0, 0);
	const int ends[] = {server.to[0], server.to[1], server.from[0], server.from[1]};
	for (const int end : ends)
		raw_syscall(nr_close, end);
}

unsigned char page_cache_image[2 * page];

void case_page_cache_write(const char *name, bool tmpfs, bool shared, bool early,
	PageWrite how)
{
	const long fd = open_file(tmpfs);
	emit_caller(page_cache_image, callee_near);
	emit_callee(page_cache_image + callee_near, 1);
	if (do_pwrite(fd, page_cache_image, 2 * page, 0) != static_cast<long>(2 * page)) {
		print("code-change: page-cache setup failed\n");
		leave(4);
	}
	PageWriteServer server{};
	if (early)
		server = start_page_write_server(fd);
	const long mapped = raw_syscall(nr_mmap, 0, 2 * page, prot_read | prot_exec,
		shared ? map_shared : map_private, fd, 0);
	if (mapped < 0 && mapped > -4096) {
		print("code-change: page-cache mapping failed\n");
		leave(4);
	}
	auto *x = reinterpret_cast<unsigned char *>(mapped);
	expect(name, 1, call_caller(x));
	for (unsigned value = 2; value < 5; ++value) {
		const unsigned char immediate[4] = {static_cast<unsigned char>(value), 0, 0, 0};
		const bool done = early ?
			serve_page_write(server, how, callee_near + 1, immediate, 4) :
			page_write(how, fd, callee_near + 1, immediate, 4);
		expect("page-cache-write-done", 1, done ? 1 : 0);
		expect(name, value, call_caller(x));
	}
	if (early)
		stop_page_write_server(server);
	raw_syscall(nr_munmap, mapped, 2 * page);
	raw_syscall(nr_close, fd);
}

/* A truncate into the page zeroes the page's tail in place and keeps it
 * mapped. The callee ends at the page's last byte, "mov $imm32, %eax" at
 * 4091, and its "ret" is the first byte of the anonymous page mapped right
 * after the file's page, so zeroing the immediate's top bytes leaves a
 * function that returns the low ones. */
void case_page_cache_truncate_tail(const char *name, bool tmpfs)
{
	const long fd = open_file(tmpfs);
	constexpr word callee = page - 5;
	emit_caller(page_cache_image, callee);
	page_cache_image[callee] = 0xb8;
	page_cache_image[callee + 1] = 0x01;
	page_cache_image[callee + 2] = 0x02;
	page_cache_image[callee + 3] = 0x03;
	page_cache_image[callee + 4] = 0x04;
	if (do_pwrite(fd, page_cache_image, page, 0) != static_cast<long>(page)) {
		print("code-change: page-cache setup failed\n");
		leave(4);
	}
	auto *region = map_region(2);
	region[page] = 0xc3;
	/* Read-only before anything forks, which would write-protect it and so
	 * change a page the callee's translation covers. */
	raw_syscall(nr_mprotect, reinterpret_cast<long>(region + page), page,
		prot_read | prot_exec);
	const long mapped = raw_syscall(nr_mmap, reinterpret_cast<long>(region), page,
		prot_read | prot_exec, map_shared | map_fixed, fd, 0);
	if (mapped < 0 && mapped > -4096) {
		print("code-change: page-cache mapping failed\n");
		leave(4);
	}
	expect(name, 0x04030201, call_caller(region));
	/* Cut at the immediate's byte 2, put bytes 2 and 3 back, cut at byte 1. */
	expect("page-cache-truncate-done", 0, raw_syscall(nr_ftruncate, fd, page - 2));
	expect(name, 0x0201, call_caller(region));
	const unsigned char top[2] = {0x03, 0x04};
	expect("page-cache-write-done", 2, do_pwrite(fd, top, 2, page - 2));
	expect(name, 0x04030201, call_caller(region));
	expect("page-cache-truncate-done", 0, raw_syscall(nr_ftruncate, fd, page - 3));
	expect(name, 0x01, call_caller(region));
	raw_syscall(nr_munmap, reinterpret_cast<long>(region), 2 * page);
	raw_syscall(nr_close, fd);
}

/* The unit that runs stores into its own next instruction through W:
 *
 *	0:	88 4a 04		mov %cl, 4(%rdx)	(W)
 *	3:	b8 00 00 00 00		mov $0, %eax		(low byte patched)
 *	8:	c3			ret
 */
void case_alias_running_block()
{
	const char *name = "alias-running-block";
	auto alias = new_alias(false, 1, callee_near);
	const unsigned char code[] = {
		0x88, 0x4a, 0x04, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xc3,
	};
	for (word index = 0; index < sizeof(code); ++index)
		alias.w[index] = code[index];
	for (unsigned value = 1; value <= 60; ++value) {
		long result;
#if defined(__x86_64__)
		__asm__ volatile("call *%1"
			: "=a"(result) : "r"(alias.x), "c"(value), "d"(alias.w)
			: "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
#else
		__asm__ volatile("call *%1"
			: "=a"(result) : "r"(alias.x), "c"(value), "d"(alias.w)
			: "memory", "cc");
#endif
		expect(name, value, result);
	}
}

/* Two executable mappings of the frame, and a writable one. */
void case_alias_two_exec_mappings()
{
	const char *name = "alias-two-exec-mappings";
	auto alias = new_alias(false, 1, callee_near);
	auto *callee = alias.w + callee_near;
	auto *second = map_file(alias.fd, alias.pages, prot_read | prot_exec);
	expect(name, 1, call_caller(alias.x));
	expect(name, 1, call_caller(second));
	for (unsigned value = 2; value < 12; ++value) {
		patch(callee, value);
		expect(name, value, call_caller(alias.x));
		expect(name, value, call_caller(second));
	}
}

/* A mapping that is both writable and executable beside one that only
 * executes: the store through the first reaches the second. */
void case_alias_rwx_plus_rx()
{
	const char *name = "alias-rwx-plus-rx";
	auto alias = new_alias(false, 1, callee_near);
	auto *rwx = map_file(alias.fd, alias.pages, prot_read | prot_write | prot_exec);
	expect(name, 1, call_caller(rwx));
	expect(name, 1, call_caller(alias.x));
	for (unsigned value = 2; value < 12; ++value) {
		patch(rwx + callee_near, value);
		expect(name, value, call_caller(rwx));
		expect(name, value, call_caller(alias.x));
	}
}

/* Another vCPU runs the function through X while this one stores through W. */
void case_alias_vcpu()
{
	const char *name = "alias-vcpu";
	auto alias = new_alias(false, 1, callee_near);
	auto *callee = alias.w + callee_near;
	worker_region = alias.x;
	auto *stack = map_region(stack_size / page, prot_read | prot_write,
		map_private | map_anonymous);
	request = 0;
	reply = 0;
	worker_stop = 0;
	if (spawn_thread(thread_flags, stack + stack_size, worker, nullptr) < 0) {
		print("code-change: thread failed\n");
		leave(4);
	}
	int round = 0;
	expect(name, 1, ask_worker(++round));
	for (unsigned value = 2; value < 30; ++value) {
		patch(callee, value);
		expect(name, value, call_caller(alias.x));
		expect(name, value, ask_worker(++round));
	}
	worker_stop = 1;
	__atomic_store_n(&request, ++round, __ATOMIC_RELEASE);
	futex_wake(&request);
	int answered;
	while ((answered = __atomic_load_n(&reply, __ATOMIC_ACQUIRE)) != -1)
		futex_wait(&reply, answered);
}

} // namespace

extern "C" {
#if defined(__x86_64__)
__asm__(".text\n"
	".global spawn_thread\n"
	"spawn_thread:\n"
	"	mov %rdx, %r8\n"
	"	mov %rcx, %r9\n"
	"	xor %edx, %edx\n"
	"	xor %r10d, %r10d\n"
	"	mov $56, %eax\n"
	"	syscall\n"
	"	test %rax, %rax\n"
	"	jnz 1f\n"
	"	xor %ebp, %ebp\n"
	"	mov %r9, %rdi\n"
	"	and $-16, %rsp\n"
	"	call *%r8\n"
	"	xor %edi, %edi\n"
	"	mov $60, %eax\n"
	"	syscall\n"
	"1:	ret\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n");
#else
__asm__(".text\n"
	".global spawn_thread\n"
	"spawn_thread:\n"
	"	push %ebx\n"
	"	push %esi\n"
	"	push %edi\n"
	"	mov 16(%esp), %ebx\n"
	"	mov 20(%esp), %ecx\n"
	"	mov 24(%esp), %esi\n"
	"	mov 28(%esp), %edi\n"
	"	sub $16, %ecx\n"
	"	mov %esi, (%ecx)\n"
	"	mov %edi, 4(%ecx)\n"
	"	xor %edx, %edx\n"
	"	xor %esi, %esi\n"
	"	xor %edi, %edi\n"
	"	mov $120, %eax\n"
	"	int $0x80\n"
	"	test %eax, %eax\n"
	"	jnz 1f\n"
	"	xor %ebp, %ebp\n"
	"	mov (%esp), %eax\n"
	"	mov 4(%esp), %edx\n"
	"	sub $12, %esp\n"
	"	push %edx\n"
	"	call *%eax\n"
	"	xor %ebx, %ebx\n"
	"	mov $1, %eax\n"
	"	int $0x80\n"
	"1:	pop %edi\n"
	"	pop %esi\n"
	"	pop %ebx\n"
	"	ret\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	and $-16, %esp\n"
	"	call probe_main\n"
	"	mov %eax, %ebx\n"
	"	mov $252, %eax\n"
	"	int $0x80\n");
#endif

int probe_main()
{
	case_smc("smc-same-page", callee_near);
	case_smc("smc-other-page", callee_far);
	case_smc_running_unit();
	case_protect();
	case_fork("fork-parent-first", true, false);
	case_fork("fork-child-first", false, false);
	case_fork("fork-child-gone", true, true);
	case_remap();
	case_vcpu(false);
	case_vcpu(true);
	case_flood();
	case_alias("alias-memfd", false, callee_near);
	case_alias("alias-memfd-far", false, callee_far);
	case_alias("alias-tmpfs", true, callee_near);
	case_alias_late_mapping();
	case_alias_other_process("alias-other-process-store", false);
	case_alias_other_process("alias-other-process-early-store", true);
	case_alias_fork_rewarm();
	case_alias_protect_cycle();
	case_alias_unmap_remap();
	case_alias_kernel_write(false);
	case_alias_kernel_write(true);
	case_alias_remote_write(true);
	case_alias_remote_write(false);
	case_alias_remote_write_remapped();
	case_page_cache_write("page-cache-memfd-shared-self-pwrite", false, true, false,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-memfd-private-self-pwrite", false, false, false,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-memfd-shared-early-pwrite", false, true, true,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-memfd-private-early-pwrite", false, false, true,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-tmpfs-shared-self-pwrite", true, true, false,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-tmpfs-private-self-pwrite", true, false, false,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-tmpfs-shared-early-pwrite", true, true, true,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-tmpfs-private-early-pwrite", true, false, true,
		PageWrite::pwrite);
	case_page_cache_write("page-cache-memfd-shared-self-splice", false, true, false,
		PageWrite::splice);
	case_page_cache_write("page-cache-tmpfs-shared-early-splice", true, true, true,
		PageWrite::splice);
	case_page_cache_truncate_tail("page-cache-memfd-truncate-tail", false);
	case_page_cache_truncate_tail("page-cache-tmpfs-truncate-tail", true);
	case_alias_running_block();
	case_alias_two_exec_mappings();
	case_alias_rwx_plus_rx();
	case_alias_vcpu();
	case_two_patch(barrier_system_call);
	case_two_patch(barrier_cpuid);
	case_two_patch(barrier_iret);
	print("code-change: abi="); print(abi);
	print(" checks="); print(checks);
	print(" failed="); print(failures);
	print(" verdict="); print(failures == 0 ? "ok" : "fail"); print("\n");
	return failures == 0 ? 0 : 2;
}

}
