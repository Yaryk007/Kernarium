// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 Linux guest probe: a kernel read that lands in a page
 * the process has pinned and runs.
 *
 * A read from /dev/ubdb opened with O_DIRECT is written by the kernel into the
 * caller's own pages through the kernel's alias of them, with no process and
 * no user address on the path, so the host's translation of the code that
 * page holds must hear of it from the kernel. So is a read into a registered
 * buffer that a file's read copies, as the generic copy-out of a buffer
 * vector does. One mode runs per process:
 *
 *	direct-anon	pread() into an anonymous page the process runs;
 *	direct-shared	pread() into a shared memfd page the process runs
 *			through another mapping of the memfd;
 *	fixed-anon	the same read as io_uring READ_FIXED into an anonymous
 *			page registered, which pins it, before it first ran;
 *	fixed-shared	READ_FIXED into the writable mapping of a memfd page
 *			that is registered before it first ran through its
 *			executable one;
 *	concurrent	pread() into a shared memfd page that another process
 *			runs without a pause: its translation of the page is
 *			dropped when the read pins the page and made again from
 *			the old bytes at once, so only a call that follows the
 *			write can drop it again;
 *	fixed-file	READ_FIXED of a buffered file (a memfd that holds the
 *			new function) into an anonymous page registered before
 *			it first ran;
 *	loop-fixed	the same read from a loop device, opened with O_DIRECT,
 *			that has the memfd as its backing file.
 *
 * Each page holds a function that returns 41 and is called many times. The
 * device holds one that returns 83. After the read, the data must be the new
 * function (the frame changed), the function called through the executable
 * mapping must return 83 as well, and a store through a user mapping, the
 * control, must still take effect (97).
 *
 * Prints "pinned-code-write: mode=... " and a verdict. */
namespace {

using word = unsigned long;
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

constexpr long nr_write = 1, nr_open = 2, nr_close = 3, nr_mmap = 9,
	nr_ioctl = 16, nr_pread64 = 17, nr_pwrite64 = 18, nr_nanosleep = 35, nr_fork = 57,
	nr_wait4 = 61, nr_kill = 62, nr_prctl = 157, nr_ftruncate = 77,
	nr_memfd_create = 319,
	nr_io_uring_setup = 425, nr_io_uring_enter = 426, nr_io_uring_register = 427;
constexpr long prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr long map_shared = 0x01, map_private = 0x02, map_anonymous = 0x20;
constexpr long o_rdwr = 2, o_direct = 040000;
constexpr long loop_set_fd = 0x4C00, loop_clr_fd = 0x4C01, loop_ctl_get_free = 0x4C82;
constexpr word page = 4096;
constexpr int calls = 128;
constexpr int old_value = 41, new_value = 83, control_value = 97;
constexpr u8 op_read_fixed = 4;
constexpr u32 enter_getevents = 1;
constexpr long register_buffers = 0;

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

long address_of(const void *pointer)
{
	return reinterpret_cast<long>(pointer);
}

void print(const char *text)
{
	long length = 0;
	while (text[length] != 0)
		++length;
	raw_syscall(nr_write, 1, address_of(text), length);
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

bool same(const char *left, const char *right)
{
	while (*left != 0 && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

const char *mode_name;
int failures;

void fail(const char *what, long detail = 0)
{
	++failures;
	print("pinned-code-write: FAIL mode="); print(mode_name);
	print(" "); print(what);
	print(" "); print(detail); print("\n");
}

u8 *map(long length, long protection, long flags, long fd = -1, long offset = 0)
{
	const long result = raw_syscall(nr_mmap, 0, length, protection, flags, fd, offset);
	if (result < 0 && result > -4096) {
		fail("mmap failed", result);
		return nullptr;
	}
	return reinterpret_cast<u8 *>(result);
}

/* The function "mov eax, value; ret", alone in its page. */
void fill_function(u8 *destination, int value)
{
	for (word at = 0; at < page; ++at)
		destination[at] = 0;
	destination[0] = 0xb8;
	destination[1] = static_cast<u8>(value);
	destination[5] = 0xc3;
}

/* What the function at @code returns, called often enough to be translated. */
int run(const u8 *code)
{
	auto *function = reinterpret_cast<int (*)()>(const_cast<u8 *>(code));
	int result = 0;
	for (int call = 0; call < calls; ++call)
		result = function();
	return result;
}

/* The value the page's bytes hold, read as data. */
int data_value(const u8 *code)
{
	return static_cast<const volatile u8 *>(code)[1];
}

struct Sqring final {
	u32 head, tail, mask, entries, flags, dropped, array, resv1;
	u64 user_addr;
};

struct Cqring final {
	u32 head, tail, mask, entries, overflow, cqes, flags, resv1;
	u64 user_addr;
};

struct Params final {
	u32 sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd, resv[3];
	Sqring sq;
	Cqring cq;
};

struct Cqe final {
	u64 user_data;
	int res;
	u32 flags;
};

struct Iovec final {
	void *base;
	word length;
};

/* One READ_FIXED of a page from @disk into @buffer, which is registered
 * first, and so pinned. @executable, a mapping of the same page, runs before
 * the read is submitted, with the buffer pinned, and @before is what it
 * returned. Returns the bytes read or a negative error. */
long read_fixed(long disk, u8 *buffer, const u8 *executable, int &before)
{
	Params params{};
	const long ring = raw_syscall(nr_io_uring_setup, 2, address_of(&params));
	if (ring < 0)
		return ring;
	long sq_size = params.sq.array + params.sq_entries * 4;
	long cq_size = params.cq.cqes + params.cq_entries * sizeof(Cqe);
	if (sq_size < static_cast<long>(page))
		sq_size = page;
	if (cq_size < static_cast<long>(page))
		cq_size = page;
	u8 *sq = map(sq_size, prot_read | prot_write, map_shared, ring, 0);
	u8 *cq = map(cq_size, prot_read | prot_write, map_shared, ring, 0x8000000);
	u8 *entries = map(params.sq_entries * 64, prot_read | prot_write, map_shared, ring, 0x10000000);
	if (sq == nullptr || cq == nullptr || entries == nullptr)
		return -9001;
	Iovec vector{buffer, page};
	const long registered = raw_syscall(nr_io_uring_register, ring, register_buffers,
		address_of(&vector), 1);
	if (registered < 0)
		return registered;
	before = run(executable);
	const u32 old = *reinterpret_cast<volatile u32 *>(cq + params.cq.tail);
	for (int at = 0; at < 64; ++at)
		entries[at] = 0;
	entries[0] = op_read_fixed;
	*reinterpret_cast<int *>(entries + 4) = static_cast<int>(disk);
	*reinterpret_cast<u64 *>(entries + 8) = 0;
	*reinterpret_cast<u64 *>(entries + 16) = static_cast<u64>(address_of(buffer));
	*reinterpret_cast<u32 *>(entries + 24) = page;
	*reinterpret_cast<u16 *>(entries + 40) = 0;
	*reinterpret_cast<u32 *>(sq + params.sq.array) = 0;
	__atomic_store_n(reinterpret_cast<u32 *>(sq + params.sq.tail), 1, __ATOMIC_RELEASE);
	const long entered = raw_syscall(nr_io_uring_enter, ring, 1, 1, enter_getevents, 0, 0);
	if (entered < 0)
		return entered;
	const u32 tail = __atomic_load_n(reinterpret_cast<u32 *>(cq + params.cq.tail), __ATOMIC_ACQUIRE);
	if (tail == old)
		return -9002;
	const long result = reinterpret_cast<Cqe *>(cq + params.cq.cqes)[0].res;
	raw_syscall(nr_close, ring);
	return result;
}

void sleep_milliseconds(long milliseconds)
{
	long spec[2] = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
	raw_syscall(nr_nanosleep, address_of(spec), 0);
}

/* The value a process that runs the page without a pause last saw, after a
 * pause that lets it see a change. */
int seen_by(const volatile int *flag)
{
	sleep_milliseconds(200);
	return *flag;
}

/* A shared memfd page that another process runs, and the disk read into
 * it. */
void exercise_concurrent()
{
	u8 first[page];
	fill_function(first, old_value);
	const long memfd = raw_syscall(nr_memfd_create, address_of("pinned"), 0);
	if (memfd < 0) {
		fail("memfd_create failed", memfd);
		return;
	}
	raw_syscall(nr_ftruncate, memfd, page);
	raw_syscall(nr_pwrite64, memfd, address_of(first), page, 0);
	u8 *writable = map(page, prot_read | prot_write, map_shared, memfd);
	u8 *executable = map(page, prot_read | prot_exec, map_shared, memfd);
	auto *flag = reinterpret_cast<volatile int *>(
		map(page, prot_read | prot_write, map_shared | map_anonymous));
	if (writable == nullptr || executable == nullptr || flag == nullptr)
		return;
	*flag = 0;
	const long disk = raw_syscall(nr_open, address_of("/dev/ubdb"), o_direct);
	if (disk < 0) {
		fail("/dev/ubdb cannot be opened with O_DIRECT", disk);
		return;
	}
	const long child = raw_syscall(nr_fork);
	if (child < 0) {
		fail("fork failed", child);
		return;
	}
	if (child == 0) {
		raw_syscall(nr_prctl, 1, 9); /* PR_SET_PDEATHSIG, SIGKILL */
		auto *function = reinterpret_cast<int (*)()>(executable);
		for (;;)
			*flag = function();
	}
	const int before = seen_by(flag);
	const long done = raw_syscall(nr_pread64, disk, address_of(writable), page, 0);
	const int after = seen_by(flag);
	const int data = data_value(writable);
	/* One byte, so that the page is a function whenever the other process
	 * runs it. */
	writable[1] = static_cast<u8>(control_value);
	const int control = seen_by(flag);
	raw_syscall(nr_kill, child, 9);
	raw_syscall(nr_wait4, child, 0, 0, 0);
	print("pinned-code-write: mode="); print(mode_name);
	print(" io="); print(done);
	print(" before="); print(before);
	print(" after="); print(after);
	print(" data="); print(data);
	print(" control="); print(control); print("\n");
	if (done != static_cast<long>(page))
		fail("the read did not return a page", done);
	if (before != old_value)
		fail("the other process did not run the old function first", before);
	if (data != new_value)
		fail("the read did not change the page", data);
	if (after != new_value)
		fail("the other process ran the old function after the read", after);
	if (control != control_value)
		fail("a store through a user mapping did not take effect", control);
	raw_syscall(nr_close, disk);
}

/* The decimal digits of @value, for a device name. */
void number_to_text(long value, char *out)
{
	char digits[16];
	int count = 0;
	do {
		digits[count++] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	int at = 0;
	while (count != 0)
		out[at++] = digits[--count];
	out[at] = 0;
}

/* A loop device with the file @backing, opened with O_DIRECT, or a negative
 * error. */
long open_loop(long backing)
{
	const long control = raw_syscall(nr_open, address_of("/dev/loop-control"), o_rdwr);
	if (control < 0)
		return control;
	const long index = raw_syscall(nr_ioctl, control, loop_ctl_get_free);
	raw_syscall(nr_close, control);
	if (index < 0)
		return index;
	char path[32] = "/dev/loop";
	number_to_text(index, path + 9);
	long device = -1;
	/* The node appears once the device does. */
	for (int tries = 0; tries < 50 && device < 0; ++tries) {
		device = raw_syscall(nr_open, address_of(path), o_rdwr | o_direct);
		if (device < 0)
			sleep_milliseconds(100);
	}
	if (device < 0)
		return device;
	const long bound = raw_syscall(nr_ioctl, device, loop_set_fd, backing);
	if (bound < 0) {
		raw_syscall(nr_close, device);
		return bound;
	}
	return device;
}

/* A page the process runs, registered before it first ran, and the
 * READ_FIXED of a buffered file into it: a memfd that holds the new function,
 * or a loop device over that memfd. The kernel copies the file's bytes into
 * the buffer with the generic copy-out of a buffer vector. */
void exercise_file(bool loop)
{
	u8 first[page], second[page];
	fill_function(first, old_value);
	fill_function(second, new_value);
	u8 *writable = map(page, prot_read | prot_write | prot_exec, map_private | map_anonymous);
	if (writable == nullptr)
		return;
	for (word at = 0; at < page; ++at)
		writable[at] = first[at];
	const long memfd = raw_syscall(nr_memfd_create, address_of("source"), 0);
	if (memfd < 0) {
		fail("memfd_create failed", memfd);
		return;
	}
	raw_syscall(nr_ftruncate, memfd, page);
	raw_syscall(nr_pwrite64, memfd, address_of(second), page, 0);
	long source = memfd;
	if (loop) {
		source = open_loop(memfd);
		if (source < 0) {
			fail("a loop device over the memfd cannot be set up", source);
			return;
		}
	}
	int before = 0;
	const long done = read_fixed(source, writable, writable, before);
	const int after = run(writable);
	const int data = data_value(writable);
	fill_function(writable, control_value);
	const int control = run(writable);
	print("pinned-code-write: mode="); print(mode_name);
	print(" io="); print(done);
	print(" before="); print(before);
	print(" after="); print(after);
	print(" data="); print(data);
	print(" control="); print(control); print("\n");
	if (done != static_cast<long>(page))
		fail("the read did not return a page", done);
	if (before != old_value)
		fail("the page did not run the old function first", before);
	if (data != new_value)
		fail("the read did not change the page", data);
	if (after != new_value)
		fail("the page ran the old function after the read", after);
	if (control != control_value)
		fail("a store through a user mapping did not take effect", control);
	if (loop) {
		raw_syscall(nr_ioctl, source, loop_clr_fd);
		raw_syscall(nr_close, source);
	}
}

/* One mode. */
void exercise(bool shared, bool fixed)
{
	u8 first[page];
	fill_function(first, old_value);
	u8 *writable = nullptr;
	u8 *executable = nullptr;
	if (shared) {
		const long memfd = raw_syscall(nr_memfd_create, address_of("pinned"), 0);
		if (memfd < 0) {
			fail("memfd_create failed", memfd);
			return;
		}
		raw_syscall(nr_ftruncate, memfd, page);
		raw_syscall(nr_pwrite64, memfd, address_of(first), page, 0);
		writable = map(page, prot_read | prot_write, map_shared, memfd);
		executable = map(page, prot_read | prot_exec, map_shared, memfd);
	} else {
		writable = map(page, prot_read | prot_write | prot_exec, map_private | map_anonymous);
		executable = writable;
		if (writable != nullptr)
			for (word at = 0; at < page; ++at)
				writable[at] = first[at];
	}
	if (writable == nullptr || executable == nullptr)
		return;
	const long disk = raw_syscall(nr_open, address_of("/dev/ubdb"), o_direct);
	if (disk < 0) {
		fail("/dev/ubdb cannot be opened with O_DIRECT", disk);
		return;
	}
	int before = 0;
	long done;
	if (fixed) {
		done = read_fixed(disk, writable, executable, before);
	} else {
		before = run(executable);
		done = raw_syscall(nr_pread64, disk, address_of(writable), page, 0);
	}
	const int after = run(executable);
	const int data = data_value(writable);
	fill_function(writable, control_value);
	const int control = run(executable);
	print("pinned-code-write: mode="); print(mode_name);
	print(" io="); print(done);
	print(" before="); print(before);
	print(" after="); print(after);
	print(" data="); print(data);
	print(" control="); print(control); print("\n");
	if (done != static_cast<long>(page))
		fail("the read did not return a page", done);
	if (before != old_value)
		fail("the page did not run the old function first", before);
	if (data != new_value)
		fail("the read did not change the page", data);
	if (after != new_value)
		fail("the page ran the old function after the read", after);
	if (control != control_value)
		fail("a store through a user mapping did not take effect", control);
	raw_syscall(nr_close, disk);
}

}

extern "C" {

void *memset(void *destination, int value, word count)
{
	auto *bytes = static_cast<unsigned char *>(destination);
	for (word index = 0; index < count; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

void *memcpy(void *destination, const void *source, word count)
{
	auto *to = static_cast<unsigned char *>(destination);
	const auto *from = static_cast<const unsigned char *>(source);
	for (word index = 0; index < count; ++index)
		to[index] = from[index];
	return destination;
}

}

extern "C" int probe_main(long *stack);

__asm__(".text\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n");

extern "C" int probe_main(long *stack)
{
	const long argc = stack[0];
	mode_name = argc > 1 ? reinterpret_cast<const char *>(stack[2]) : "";
	if (same(mode_name, "direct-anon"))
		exercise(false, false);
	else if (same(mode_name, "direct-shared"))
		exercise(true, false);
	else if (same(mode_name, "fixed-anon"))
		exercise(false, true);
	else if (same(mode_name, "fixed-shared"))
		exercise(true, true);
	else if (same(mode_name, "concurrent"))
		exercise_concurrent();
	else if (same(mode_name, "fixed-file"))
		exercise_file(false);
	else if (same(mode_name, "loop-fixed"))
		exercise_file(true);
	else {
		print("pinned-code-write: unknown mode\n");
		return 4;
	}
	print("pinned-code-write: mode="); print(mode_name);
	print(" failed="); print(failures);
	print(" verdict="); print(failures == 0 ? "ok" : "fail"); print("\n");
	return failures == 0 ? 0 : 2;
}
