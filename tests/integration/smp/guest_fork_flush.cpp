// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding Linux guest probe, arm64 and x86-64: what a fork leaves of the
 * parent's address space.
 *
 * The kernel's fork flush leaves a parent's mappings alone where fork changed
 * none of its page-table entries and invalidates the rest, the writable
 * private ones, which fork write-protects. The default mode forks over every
 * kind of memory a process has and requires each side to see only its own
 * bytes afterwards:
 *
 *	anonymous	private anonymous pages, the stack and .data of the probe
 *			and a writable private mapping of a file (one page only
 *			read before the fork, one written), parent first and
 *			child first, and the file itself must stay what it was;
 *	shared		a MAP_SHARED page, which both sides must keep sharing;
 *	protect		a page mprotect() made read-only before the fork must
 *			still refuse the parent's store, and a store the
 *			handler then permits must stay out of the child's copy;
 *	sibling		three more threads of the parent increment counters while
 *			the main thread forks: on a private anonymous page, a
 *			private file page, the original main stack (the
 *			growable stack VMA the forking thread alone wrote
 *			before), pages of a mapping mprotect() made writable
 *			just before the loop (one never written, one written
 *			and made read-only first) and a page of a writable and
 *			executable mapping whose first page the main thread
 *			runs after every fork; a child must find its copies
 *			frozen from the moment fork returned, whatever the
 *			threads do next, and its own marks must stay out of the
 *			parent, and no increment may be lost.
 *
 * The other modes drive the x86-64 accounting row: "text N" forks N times and
 * calls 32 code pages of the probe's own text after each fork, "rwx N" does the
 * same with one writable and executable private page, and "bare N" only forks.
 * The runner compares the emulator's holder registrations, which grow with each
 * fork only for the page that fork write-protected.
 *
 * Prints one line per failed check and, last, "fork-flush: abi=... checks=N
 * failed=M verdict=ok" (the row mode) or "fork-flush: mode=... forks=N". */

using word = unsigned long;

extern "C" {
long spawn_thread(word flags, void *stack_top, void (*entry)(void *),
	void *argument);
void restore_context();
int probe_main(long *stack);
#if defined(__x86_64__)
extern unsigned char text_pages[];
#endif
}

/* clone() returns twice on two stacks, which no compiled frame survives. */
#if defined(__aarch64__)
__asm__(".text\n"
	".global spawn_thread\n"
	"spawn_thread:\n"
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
	".global restore_context\n"
	"restore_context:\n"
	"	mov x8, #139\n"
	"	svc #0\n"
	".global _start\n"
	"_start:\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	mov x0, sp\n"
	"	bl probe_main\n"
	"	mov x8, #94\n"
	"	svc #0\n");
#else
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
	".global restore_context\n"
	"restore_context:\n"
	"	mov $15, %eax\n"
	"	syscall\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	".balign 4096\n"
	".global text_pages\n"
	"text_pages:\n"
	".rept 32\n"
	"	ret\n"
	"	.balign 4096\n"
	".endr\n");
#endif

namespace {

#if defined(__aarch64__)
constexpr const char *abi = "a64";
constexpr long nr_read = 63, nr_write = 64, nr_close = 57, nr_pipe2 = 59,
	nr_pread64 = 67, nr_pwrite64 = 68, nr_ftruncate = 46,
	nr_exit_group = 94, nr_nanosleep = 101,
	nr_clock_gettime = 113, nr_rt_sigaction = 134, nr_clone = 220,
	nr_mmap = 222, nr_munmap = 215, nr_mprotect = 226, nr_wait4 = 260,
	nr_memfd_create = 279;

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
		: "=r"(x0)
		: "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		: "memory", "cc");
	return x0;
}
#elif defined(__x86_64__)
constexpr const char *abi = "x86-64";
constexpr long nr_read = 0, nr_write = 1, nr_close = 3, nr_mmap = 9,
	nr_mprotect = 10, nr_munmap = 11, nr_rt_sigaction = 13, nr_pread64 = 17,
	nr_pwrite64 = 18, nr_nanosleep = 35, nr_clone = 56, nr_wait4 = 61,
	nr_ftruncate = 77, nr_sched_setaffinity = 203, nr_clock_gettime = 228,
	nr_exit_group = 231, nr_pipe2 = 293, nr_memfd_create = 319;

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
#error "an arm64 or x86-64 guest"
#endif

constexpr long prot_read = 1, prot_write = 2;
[[maybe_unused]] constexpr long prot_exec = 4;
constexpr long map_shared = 0x01, map_private = 0x02, map_anonymous = 0x20;
constexpr long sigchld = 17, sigsegv = 11;

/* A function that returns 42, and the writes and cache maintenance that
 * make it run: the code the rows keep running in a writable and executable
 * private page. */
void write_stub(unsigned char *where)
{
#if defined(__aarch64__)
	auto *words = reinterpret_cast<unsigned int *>(where);
	words[0] = 0x52800540u; /* mov w0, #42 */
	words[1] = 0xd65f03c0u; /* ret */
	__asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb"
		: : "r"(where) : "memory");
#else
	static const unsigned char bytes[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
	for (int index = 0; index < 6; ++index)
		where[index] = bytes[index];
#endif
}
constexpr long stub_result = 42;
/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
 * CLONE_SYSVSEM */
constexpr word thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000;
constexpr word sa_restorer = 0x04000000;
constexpr word page = 4096;
constexpr word stack_size = 64 * 1024;
constexpr unsigned char before = 0x11, parent_mark = 0xa0, child_mark = 0xc0;

struct Timespec {
	long seconds;
	long nanoseconds;
};

long long now()
{
	Timespec time{};
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(&time));
	return time.seconds * 1000000000LL + time.nanoseconds;
}

void sleep_milliseconds(long milliseconds)
{
	Timespec time{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
	raw_syscall(nr_nanosleep, reinterpret_cast<long>(&time), 0);
}

[[noreturn]] void leave(long code)
{
	for (;;)
		raw_syscall(nr_exit_group, code);
}

word length(const char *text)
{
	word count = 0;
	while (text[count] != 0)
		++count;
	return count;
}

void print(const char *text)
{
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), static_cast<long>(length(text)));
}

void print_number(long value)
{
	char digits[24];
	int count = 0;
	unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value)
					     : static_cast<unsigned long>(value);
	do {
		digits[count++] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude != 0);
	if (value < 0)
		raw_syscall(nr_write, 1, reinterpret_cast<long>("-"), 1);
	while (count > 0)
		raw_syscall(nr_write, 1, reinterpret_cast<long>(&digits[--count]), 1);
}

int checks;
int failed;

void expect(const char *name, long expected, long actual)
{
	++checks;
	if (expected == actual)
		return;
	++failed;
	print("fork-flush: FAIL ");
	print(name);
	print(" expected ");
	print_number(expected);
	print(" got ");
	print_number(actual);
	print("\n");
}

long map(word bytes, long protection, long flags, long descriptor = -1)
{
	const long result = raw_syscall(nr_mmap, 0, static_cast<long>(bytes),
		protection, flags, descriptor, 0);
	if (result < 0 && result > -4096) {
		print("fork-flush: mmap failed\n");
		leave(4);
	}
	return result;
}

/* Memory the rows fork over: `pages` pages from `bytes`. */
struct Region {
	const char *name;
	volatile unsigned char *bytes;
	word pages;
	/* Pages the fork leaves read-only in the page table (read, not
	 * written, before it): the first `read_only` pages. */
	word read_only;
};

alignas(4096) volatile unsigned char data_area[2 * page] = {1};
Region regions[5];
word region_count;
int file_descriptor = -1;
volatile unsigned char *shared_page;

void fill_before_fork()
{
	for (word index = 0; index < region_count; ++index) {
		const auto &region = regions[index];
		for (word number = region.read_only; number < region.pages; ++number) {
			for (word offset = 0; offset < page; offset += 512)
				region.bytes[number * page + offset] = before;
		}
	}
}

/* How many bytes the rows check hold something else than they should: the
 * first byte of every page holds `first` once this side wrote it, and every
 * byte holds what the page held before the fork otherwise. */
long count_bad(unsigned char first, bool written_by_me)
{
	long bad = 0;
	for (word index = 0; index < region_count; ++index) {
		const auto &region = regions[index];
		for (word number = 0; number < region.pages; ++number) {
			/* A page nobody wrote before the fork holds the file's bytes. */
			const unsigned char base = number < region.read_only ? 'F' : before;
			const auto *at = region.bytes + number * page;
			if (at[0] != (written_by_me ? first : base))
				++bad;
			for (word offset = 512; offset < page; offset += 512)
				if (at[offset] != base)
					++bad;
		}
	}
	return bad;
}

void write_first_bytes(unsigned char mark)
{
	for (word index = 0; index < region_count; ++index) {
		const auto &region = regions[index];
		for (word number = 0; number < region.pages; ++number)
			region.bytes[number * page] = mark;
	}
}

long wait_for(long child)
{
	int status = 0;
	raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	return status;
}

/* One fork over every region; `parent_first` says which side writes first.
 * Each row maps its anonymous and file regions afresh. */
void row_private(const char *name, bool parent_first)
{
	auto *anonymous = reinterpret_cast<volatile unsigned char *>(
		map(3 * page, prot_read | prot_write, map_private | map_anonymous));
	auto *file_area = reinterpret_cast<volatile unsigned char *>(
		map(2 * page, prot_read | prot_write, map_private, file_descriptor));
	/* The file's first page is only read, so fork finds a read-only entry
	 * in a writable private mapping; its second is written first. */
	expect("file-read", 'F', file_area[0]);
	regions[0] = {"anonymous", anonymous, 3, 0};
	regions[1] = {"file", file_area, 2, 1};
	fill_before_fork();
	int to_child[2];
	int to_parent[2];
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_child), 0);
	raw_syscall(nr_pipe2, reinterpret_cast<long>(to_parent), 0);
	const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (child < 0) {
		print("fork-flush: fork failed\n");
		leave(4);
	}
	char token = 0;
	if (child == 0) {
		long bad = 0;
		if (parent_first) {
			raw_syscall(nr_read, to_child[0], reinterpret_cast<long>(&token), 1);
			bad += count_bad(0, false);
			write_first_bytes(child_mark);
			bad += count_bad(child_mark, true);
			raw_syscall(nr_write, to_parent[1], reinterpret_cast<long>(&token), 1);
		} else {
			write_first_bytes(child_mark);
			bad += count_bad(child_mark, true);
			raw_syscall(nr_write, to_parent[1], reinterpret_cast<long>(&token), 1);
			raw_syscall(nr_read, to_child[0], reinterpret_cast<long>(&token), 1);
			bad += count_bad(child_mark, true);
		}
		leave(bad > 100 ? 100 : bad);
	}
	if (parent_first) {
		write_first_bytes(parent_mark);
		expect(name, 0, count_bad(parent_mark, true));
		raw_syscall(nr_write, to_child[1], reinterpret_cast<long>(&token), 1);
		raw_syscall(nr_read, to_parent[0], reinterpret_cast<long>(&token), 1);
		expect(name, 0, count_bad(parent_mark, true));
	} else {
		raw_syscall(nr_read, to_parent[0], reinterpret_cast<long>(&token), 1);
		expect(name, 0, count_bad(0, false));
		write_first_bytes(parent_mark);
		expect(name, 0, count_bad(parent_mark, true));
		raw_syscall(nr_write, to_child[1], reinterpret_cast<long>(&token), 1);
	}
	expect(name, 0, wait_for(child));
	raw_syscall(nr_close, to_child[0]);
	raw_syscall(nr_close, to_child[1]);
	raw_syscall(nr_close, to_parent[0]);
	raw_syscall(nr_close, to_parent[1]);
	raw_syscall(nr_munmap, reinterpret_cast<long>(anonymous), 3 * page);
	raw_syscall(nr_munmap, reinterpret_cast<long>(file_area), 2 * page);
}

/* The file under the private mapping holds what it held. */
void check_file_untouched(const char *name)
{
	for (word number = 0; number < 2; ++number) {
		unsigned char bytes[2] = {0, 0};
		raw_syscall(nr_pread64, file_descriptor, reinterpret_cast<long>(bytes), 2,
			static_cast<long>(number * page));
		expect(name, 'F', bytes[0]);
		expect(name, 'F', bytes[1]);
	}
}

void row_shared()
{
	shared_page[0] = 1;
	const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (child < 0) {
		print("fork-flush: fork failed\n");
		leave(4);
	}
	if (child == 0) {
		long bad = shared_page[0] != 1;
		shared_page[0] = 2;
		while (shared_page[1] != 3)
			;
		bad += shared_page[0] != 2;
		leave(bad);
	}
	while (shared_page[0] != 2)
		;
	shared_page[1] = 3;
	expect("shared", 0, wait_for(child));
}

/* The handler a refused store reaches: it permits the store and counts. */
volatile word protected_page;
volatile int refusals;

void on_fault(int, void *, void *)
{
	refusals = refusals + 1;
	raw_syscall(nr_mprotect, static_cast<long>(protected_page), page,
		prot_read | prot_write);
}

struct KernelSigaction {
	void *handler;
	word flags;
	void *restorer;
	word mask;
};

void row_protect()
{
	KernelSigaction action{reinterpret_cast<void *>(on_fault), sa_restorer | 4,
		reinterpret_cast<void *>(restore_context), 0};
	raw_syscall(nr_rt_sigaction, sigsegv, reinterpret_cast<long>(&action), 0, 8);
	auto *area = reinterpret_cast<volatile unsigned char *>(
		map(page, prot_read | prot_write, map_private | map_anonymous));
	area[0] = before;
	raw_syscall(nr_mprotect, reinterpret_cast<long>(area), page, prot_read);
	protected_page = reinterpret_cast<word>(area);
	const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
	if (child < 0) {
		print("fork-flush: fork failed\n");
		leave(4);
	}
	if (child == 0) {
		/* The child's copy is read-only too: its store is refused as well,
		 * and the handler permits it. */
		area[0] = child_mark;
		leave(refusals == 1 && area[0] == child_mark ? 0 : 1);
	}
	expect("protect-refused", 0, refusals);
	area[0] = parent_mark;
	expect("protect-handler", 1, refusals);
	expect("protect-store", parent_mark, area[0]);
	expect("protect-child", 0, wait_for(child));
	expect("protect-own", parent_mark, area[0]);
	raw_syscall(nr_munmap, reinterpret_cast<long>(area), page);
}

/* Counters of the sibling threads: each thread owns a slot in each of
 * these places, and increments them all together. */
constexpr int siblings = 3;

struct alignas(64) Counters {
	volatile word first;
	volatile word second;
	volatile word mark;
};

/* What each sibling counted, on pages of their own: a store of the parent's
 * that fork's copy-on-write lost shows as a counter below its tally. */
struct alignas(64) Tally {
	volatile word count;
};

constexpr int places = 6;
const char *const place_names[places] = {
	"sibling-lost-anonymous", "sibling-lost-file", "sibling-lost-main-stack",
	"sibling-lost-upgraded-never-written", "sibling-lost-upgraded-written",
	"sibling-lost-writable-executable"};

alignas(4096) Tally tallies[siblings];
volatile int stop;
volatile int go;
volatile int finished;
/* Where each place's counters are, and what each held before the loop. */
Counters *counters[places];
word initial[places];

void sibling(void *argument)
{
	const auto index = reinterpret_cast<word>(argument);
	while (!go) {
	}
	while (!stop) {
		for (int round = 0; round < 64; ++round) {
			for (int place = 0; place < places; ++place)
				counters[place][index].first = counters[place][index].first + 1;
			tallies[index].count = tallies[index].count + 1;
		}
	}
	__atomic_fetch_add(&finished, 1, __ATOMIC_SEQ_CST);
}

void row_sibling()
{
	counters[0] = reinterpret_cast<Counters *>(
		map(page, prot_read | prot_write, map_private | map_anonymous));
	/* The file page the counters share is written first, so the siblings'
	 * stores find an anonymous copy that fork write-protects. */
	auto *file_page = reinterpret_cast<unsigned char *>(
		map(page, prot_read | prot_write, map_private, file_descriptor));
	counters[1] = reinterpret_cast<Counters *>(file_page + 2048);
	/* The original main stack: this frame lives in the VMA the kernel built
	 * at exec, which only the forking thread has written so far. The
	 * counters have a page of their own there, so the forking thread's own
	 * frames never break its copy-on-write for them. */
	alignas(4096) Counters on_stack[siblings];
	counters[2] = on_stack;
	/* Two pages made writable by mprotect() just before the loop: the first
	 * was never written (its read mapped the zero page), the second was
	 * written, made read-only and read again. */
	auto *upgraded = reinterpret_cast<unsigned char *>(
		map(2 * page, prot_read | prot_write, map_private | map_anonymous));
	counters[3] = reinterpret_cast<Counters *>(upgraded);
	counters[4] = reinterpret_cast<Counters *>(upgraded + page);
	/* A writable and executable mapping: its first page runs code after
	 * every fork, the counters are on its second. */
	auto *executable = reinterpret_cast<unsigned char *>(
		map(2 * page, prot_read | prot_write | prot_exec, map_private | map_anonymous));
	write_stub(executable);
	counters[5] = reinterpret_cast<Counters *>(executable + page);
	expect("sibling-stub", stub_result, reinterpret_cast<long (*)()>(executable)());

	for (int place = 0; place < places; ++place)
		initial[place] = place == 3 ? 0 : 1;
	for (int index = 0; index < siblings; ++index) {
		on_stack[index].first = 0;
		on_stack[index].second = 0;
		on_stack[index].mark = 0;
		for (int place = 0; place < places; ++place) {
			if (place != 3)
				counters[place][index].first = 1;
		}
	}
	raw_syscall(nr_mprotect, reinterpret_cast<long>(upgraded), static_cast<long>(2 * page), prot_read);
	[[maybe_unused]] word sink = counters[3][0].first + counters[4][0].first;
	for (int index = 0; index < siblings; ++index) {
		const long stack = map(stack_size, prot_read | prot_write, map_private | map_anonymous);
		if (spawn_thread(thread_flags, reinterpret_cast<void *>(stack + stack_size),
			    sibling, reinterpret_cast<void *>(index)) < 0) {
			print("fork-flush: thread failed\n");
			leave(4);
		}
	}
	/* The upgrade comes right before the loop; the siblings wait for it. */
	raw_syscall(nr_mprotect, reinterpret_cast<long>(upgraded), static_cast<long>(2 * page),
		prot_read | prot_write);
	go = 1;
	long long forks = 0;
	long long broken = 0;
	const long long started = now();
	while (now() - started < 3000000000LL) {
		const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
		if (child < 0) {
			print("fork-flush: fork failed\n");
			leave(4);
		}
		if (child == 0) {
			/* Fork returned: whatever the siblings store now belongs to the
			 * parent's copy, and these hold still. */
			word before_count[places][siblings];
			for (int place = 0; place < places; ++place) {
				for (int index = 0; index < siblings; ++index)
					before_count[place][index] = counters[place][index].first;
			}
			counters[0][0].mark = child_mark;
			for (volatile int spin = 0; spin < 3000; spin = spin + 1) {
			}
			long bad = counters[0][0].mark != child_mark;
			for (int place = 0; place < places; ++place) {
				for (int index = 0; index < siblings; ++index)
					bad += counters[place][index].first != before_count[place][index];
			}
			leave(bad > 100 ? 100 : bad);
		}
		++forks;
		expect("sibling-child", 0, wait_for(child));
		/* The child's mark stays in the child's copy, and the code page
		 * still runs. */
		if (counters[0][0].mark != 0)
			++broken;
		if (reinterpret_cast<long (*)()>(executable)() != stub_result)
			++broken;
	}
	expect("sibling-mark", 0, broken);
	expect("sibling-forks", 1, forks > 5);
	stop = 1;
	const long long stopping = now();
	while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) != siblings) {
		if (now() - stopping >= 10000000000LL) {
			print("fork-flush: stuck\n");
			leave(3);
		}
		sleep_milliseconds(5);
	}
	/* Every counter started at its initial value and its sibling added what
	 * it tallied, each store in the parent's own pages. */
	for (int place = 0; place < places; ++place) {
		for (int index = 0; index < siblings; ++index) {
			expect(place_names[place], static_cast<long>(tallies[index].count + initial[place]),
				static_cast<long>(counters[place][index].first));
		}
	}
	expect("sibling-counters", 1, tallies[0].count > 1 && tallies[siblings - 1].count > 1);
}

bool same_mode(const char *left, const char *right)
{
	while (*left != 0 && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

#if defined(__x86_64__)
void fork_loop(const char *mode, long forks, unsigned char *rwx)
{
	const bool walk_text = same_mode(mode, "text");
	for (long index = 0; index < forks; ++index) {
		const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
		if (child < 0) {
			print("fork-flush: fork failed\n");
			leave(4);
		}
		if (child == 0)
			leave(0);
		wait_for(child);
		if (rwx != nullptr)
			reinterpret_cast<void (*)()>(rwx)();
		else if (walk_text)
			for (word number = 0; number < 32; ++number)
				reinterpret_cast<void (*)()>(text_pages + number * page)();
	}
	print("fork-flush: mode=");
	print(mode);
	print(" forks=");
	print_number(forks);
	print("\n");
}

long parse_number(const char *text)
{
	long value = 0;
	for (; *text >= '0' && *text <= '9'; ++text)
		value = value * 10 + (*text - '0');
	return value;
}
#endif

} // namespace

extern "C" {

void *memset(void *destination, int value, word count)
{
	auto *bytes = static_cast<unsigned char *>(destination);
	for (word index = 0; index < count; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

int probe_main(long *stack)
{
	const long argc = stack[0];
	const char *mode = argc > 1 ? reinterpret_cast<const char *>(stack[2]) : "rows";
#if defined(__x86_64__)
	if (!same_mode(mode, "rows")) {
		/* One vCPU, so the code pages register once and not once per
		 * executor. */
		word mask = 1;
		raw_syscall(nr_sched_setaffinity, 0, sizeof(mask), reinterpret_cast<long>(&mask));
		const long forks = argc > 2 ? parse_number(reinterpret_cast<const char *>(stack[3])) : 0;
		unsigned char *rwx = nullptr;
		if (same_mode(mode, "rwx")) {
			rwx = reinterpret_cast<unsigned char *>(
				map(page, prot_read | prot_write | prot_exec, map_private | map_anonymous));
			rwx[0] = 0xc3;
			rwx[1] = 0x90;
			reinterpret_cast<void (*)()>(rwx)();
		}
		fork_loop(mode, forks, rwx);
		return 0;
	}
#else
	(void)same_mode;
	(void)mode;
#endif

	file_descriptor = static_cast<int>(raw_syscall(nr_memfd_create,
		reinterpret_cast<long>("fork-flush"), 0));
	if (file_descriptor < 0) {
		print("fork-flush: memfd_create failed\n");
		return 4;
	}
	raw_syscall(nr_ftruncate, file_descriptor, 2 * page);
	for (word number = 0; number < 2; ++number) {
		unsigned char bytes[page];
		for (word index = 0; index < page; ++index)
			bytes[index] = 'F';
		raw_syscall(nr_pwrite64, file_descriptor, reinterpret_cast<long>(bytes),
			page, static_cast<long>(number * page));
	}
	shared_page = reinterpret_cast<volatile unsigned char *>(
		map(page, prot_read | prot_write, map_shared | map_anonymous));
	volatile unsigned char stack_area[2 * page];
	region_count = 4;
	regions[2] = {"data", data_area, 2, 0};
	regions[3] = {"stack", stack_area, 2, 0};

	row_private("private-parent-first", true);
	check_file_untouched("file-parent-first");
	row_private("private-child-first", false);
	check_file_untouched("file-child-first");
	row_shared();
	row_protect();
	row_sibling();
	check_file_untouched("file-after-sibling");

	print("fork-flush: abi=");
	print(abi);
	print(" checks=");
	print_number(checks);
	print(" failed=");
	print_number(failed);
	print(failed == 0 ? " verdict=ok\n" : " verdict=fail\n");
	return failed == 0 ? 0 : 2;
}

} // extern "C"
