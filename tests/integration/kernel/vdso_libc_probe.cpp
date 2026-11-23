// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe: the guest's C library finds the vDSO and calls through it.
//
// vdso_probe.cpp checks the vDSO itself with no library in the way. This
// probe is a program of the guest's own C library (glibc in a Linux guest,
// bionic in an Android guest), which looks the entries up through
// AT_SYSINFO_EHDR as every program of that library does, and checks:
//
// - clock_gettime, gettimeofday and time return plausible values that agree
//   with each other and with the raw system call, and move forward, and an
//   invalid clock fails as the library's own contract says (-1);
// - glibc's time() reads the clock through an internal call to the vDSO's
//   clock_gettime(CLOCK_REALTIME_COARSE) and ignores its result, so without a
//   vDSO it returns what the stack held; gmtime(time()) is then NULL and a
//   caller that passes it on faults. The coarse clocks, gmtime and mktime of
//   the time are checked as well;
// - a traced child that makes those calls makes each system call from the
//   [vdso] mapping, which shows that the library found the entries and that
//   they reach the kernel as plain system calls. Without a vDSO the library
//   makes the call from its own code, or, as the glibc 2.39 of a guest
//   application's root file system does, fails every clock.
//
// The probe includes no header, so the same source builds against either
// library: in a Linux guest with c++ (as run_guest_probe.py does), and for
// Android on the host with clang against a stand-in of libc.so
// (run_android_vdso.py). It prints one line per case and exits with the number
// of failures.

#include <stddef.h>
#include <stdint.h>

extern "C" {
int clock_gettime(int clock, void *timespec);
int gettimeofday(void *timeval, void *zone);
long time(long *seconds);
int fork(void);
int waitpid(int pid, int *status, int options);
long ptrace(long request, long pid, long address, long data);
int kill(int pid, int signal);
int getpid(void);
long write(int descriptor, const void *buffer, size_t size);
long syscall(long number, ...);
void *gmtime(const long *seconds);
long mktime(void *broken_down);
long read(int descriptor, void *buffer, size_t size);
int open(const char *path, int flags);
int close(int descriptor);
[[noreturn]] void _exit(int status);
}

namespace {

struct Timespec {
	long seconds;
	long nanoseconds;
};

struct Timeval {
	long seconds;
	long microseconds;
};

struct Tm {
	int seconds, minutes, hours, day, month, year, weekday, yearday, daylight;
	long offset;
	const char *zone;
};

struct IoVec {
	void *base;
	size_t length;
};

struct Registers {
	uint64_t x[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
};

constexpr long ptrace_traceme = 0;
constexpr long ptrace_syscall = 24;
constexpr long ptrace_getregset = 0x4204;
constexpr long nt_prstatus = 1;
constexpr int sigstop = 19;
constexpr int sigkill = 9;
constexpr long nr_clock_gettime = 113;
constexpr long nr_gettimeofday = 169;
constexpr long nr_clock_gettime_raw = 113;

int failures;

size_t length(const char *text)
{
	size_t size = 0;
	while (text[size])
		size++;
	return size;
}

void say(const char *text)
{
	write(1, text, length(text));
}

void say_number(long value)
{
	char digits[24];
	int count = 0;
	unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value) : static_cast<unsigned long>(value);

	do {
		digits[count++] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude);
	if (value < 0)
		write(1, "-", 1);
	while (count)
		write(1, &digits[--count], 1);
}

void report(bool ok, const char *what, long a = 0, long b = 0)
{
	say(ok ? "ok   " : "FAIL ");
	say(what);
	if (a || b) {
		say(" (");
		say_number(a);
		say(", ");
		say_number(b);
		say(")");
	}
	say("\n");
	if (!ok)
		failures++;
}

bool not_after(const Timespec &a, const Timespec &b)
{
	return b.seconds > a.seconds || (b.seconds == a.seconds && b.nanoseconds >= a.nanoseconds);
}

long raw_seconds(int clock)
{
	Timespec raw = {};

	if (syscall(nr_clock_gettime_raw, clock, &raw) != 0)
		return -1;
	return raw.seconds;
}

bool within(long a, long b, long limit)
{
	return a >= b - limit && a <= b + limit;
}

// time() and what is built on it, against the system call's own reading.
void check_time_calls()
{
	long before = raw_seconds(0);
	long now = time(nullptr);
	long after = raw_seconds(0);

	report(now >= before - 1 && now <= after + 1, "time() is the system call's CLOCK_REALTIME (within a second)", now,
	       before);
	const Tm *broken_down = static_cast<const Tm *>(gmtime(&now));
	report(broken_down != nullptr && broken_down->year + 1900 >= 2024 && broken_down->year + 1900 < 2200 &&
	       broken_down->month >= 0 && broken_down->month < 12,
	       "gmtime(time()) is a date, not NULL", broken_down ? broken_down->year + 1900 : 0);
	// Wine's time-zone code passes it to mktime(); a time zone's offset is under a day.
	long local = broken_down ? mktime(const_cast<Tm *>(broken_down)) : -1;
	report(local != -1 && within(local, now, 86400 + 3600), "mktime(gmtime(time())) is the time within a time zone", local,
	       now);

	Timespec coarse = {}, coarse_monotonic = {};
	int a = clock_gettime(5, &coarse);
	int b = clock_gettime(6, &coarse_monotonic);

	report(a == 0 && b == 0 && within(coarse.seconds, raw_seconds(5), 2) &&
	       within(coarse_monotonic.seconds, raw_seconds(6), 2),
	       "CLOCK_REALTIME_COARSE and CLOCK_MONOTONIC_COARSE agree with the system call", coarse.seconds,
	       coarse_monotonic.seconds);
}

void check_values()
{
	Timespec monotonic_1 = {}, monotonic_2 = {}, realtime = {}, boottime = {}, ignored = {};
	Timeval day = {};
	long seconds = time(nullptr);
	int a = clock_gettime(1, &monotonic_1);
	int b = clock_gettime(0, &realtime);
	int c = clock_gettime(7, &boottime);
	int d = gettimeofday(&day, nullptr);
	int e = clock_gettime(1, &monotonic_2);

	report(a == 0 && b == 0 && c == 0 && d == 0 && e == 0, "clock_gettime and gettimeofday succeed", a, b);
	report(not_after(monotonic_1, monotonic_2) && monotonic_1.nanoseconds < 1000000000 &&
	       monotonic_2.nanoseconds < 1000000000,
	       "CLOCK_MONOTONIC moves forward", monotonic_1.seconds, monotonic_2.seconds);
	report(not_after(monotonic_1, boottime), "CLOCK_BOOTTIME is not behind CLOCK_MONOTONIC", boottime.seconds,
	       monotonic_1.seconds);
	report(realtime.seconds > 1700000000 && day.seconds >= realtime.seconds - 2 && day.seconds <= realtime.seconds + 2 &&
	       seconds >= realtime.seconds - 2 && seconds <= realtime.seconds + 2,
	       "time(), gettimeofday and CLOCK_REALTIME agree", seconds, realtime.seconds);
	report(clock_gettime(12, &ignored) == -1, "an invalid clock fails with -1 as the library's contract says");
	// This pins the plain-system-call contract: the call faults in the kernel and returns -1. An entry
	// with a counter fast path would store through the pointer in user mode and kill the probe with
	// SIGSEGV instead of failing this check, so such a change revisits it.
	report(clock_gettime(1, reinterpret_cast<void *>(1)) == -1, "a bad pointer fails with -1");
}

unsigned long parse_hex(const char *&p)
{
	unsigned long value = 0;

	for (;; p++) {
		char c = *p;
		if (c >= '0' && c <= '9')
			value = value * 16 + static_cast<unsigned>(c - '0');
		else if (c >= 'a' && c <= 'f')
			value = value * 16 + static_cast<unsigned>(c - 'a' + 10);
		else
			return value;
	}
}

// The [vdso] range of a process, or false.
bool vdso_range(int pid, unsigned long &start, unsigned long &end)
{
	char path[32] = "/proc/";
	size_t at = 6;
	char digits[12];
	int count = 0;

	for (int value = pid; value; value /= 10)
		digits[count++] = static_cast<char>('0' + value % 10);
	while (count)
		path[at++] = digits[--count];
	for (const char *tail = "/maps"; *tail; tail++)
		path[at++] = *tail;
	path[at] = 0;

	static char text[96 * 1024];
	int descriptor = open(path, 0);
	size_t size = 0;
	if (descriptor < 0)
		return false;
	for (;;) {
		long count_read = read(descriptor, text + size, sizeof(text) - 1 - size);
		if (count_read <= 0)
			break;
		size += static_cast<size_t>(count_read);
	}
	close(descriptor);
	text[size] = 0;
	for (const char *line = text; *line;) {
		const char *eol = line;
		while (*eol && *eol != '\n')
			eol++;
		if (eol - line >= 6 && eol[-1] == ']' && eol[-2] == 'o' && eol[-3] == 's' && eol[-4] == 'd' &&
		    eol[-5] == 'v' && eol[-6] == '[') {
			const char *p = line;
			start = parse_hex(p);
			p++;
			end = parse_hex(p);
			return true;
		}
		line = *eol ? eol + 1 : eol;
	}
	return false;
}

// The child that makes the calls the tracer watches.
[[noreturn]] void traced_child()
{
	Timespec ts = {};
	Timeval tv = {};

	if (ptrace(ptrace_traceme, 0, 0, 0) != 0)
		_exit(70);
	kill(getpid(), sigstop);
	int a = clock_gettime(1, &ts);
	int b = gettimeofday(&tv, nullptr);
	long c = time(nullptr);
	int d = clock_gettime(0, &ts);
	_exit(a == 0 && b == 0 && c > 0 && d == 0 ? 0 : 71);
}

void check_traced()
{
	int child = fork();

	if (child == 0)
		traced_child();
	int status = 0;
	if (child < 0 || waitpid(child, &status, 0) != child) {
		report(false, "the traced child starts");
		return;
	}
	if ((status & 0xff) != 0x7f) {
		// The child ended: _exit(70) is a refused PTRACE_TRACEME.
		report(false, "the traced child stops itself", status);
		return;
	}
	unsigned long start = 0, end = 0;
	bool have_vdso = vdso_range(child, start, end);
	report(have_vdso && start < end, "the traced child has a [vdso] mapping");

	int calls = 0, inside_vdso = 0, outside = 0;
	for (int stops = 0; stops < 20000; stops++) {
		if (ptrace(ptrace_syscall, child, 0, 0) != 0)
			break;
		if (waitpid(child, &status, 0) != child)
			break;
		if ((status & 0xff) != 0x7f)
			break;
		Registers registers = {};
		IoVec vector = { &registers, sizeof(registers) };
		if (ptrace(ptrace_getregset, child, nt_prstatus, reinterpret_cast<long>(&vector)) != 0)
			continue;
		if (registers.x[8] == nr_clock_gettime || registers.x[8] == nr_gettimeofday) {
			calls++;
			if (have_vdso && registers.pc >= start && registers.pc < end)
				inside_vdso++;
			else
				outside++;
		}
	}
	report(calls >= 8, "the child makes the clock system calls (each is a stop on entry and on exit)", calls);
	report(outside == 0 && inside_vdso == calls && calls > 0,
	       "every one is made from the [vdso] mapping, so the library calls the vDSO", inside_vdso, outside);
	if ((status & 0x7f) != 0 || ((status >> 8) & 0xff) != 0) {
		report(false, "the traced child's calls returned valid values", status);
		kill(child, sigkill);
		waitpid(child, &status, 0);
	} else {
		report(true, "the traced child's calls returned valid values");
	}
}

int run()
{
	check_values();
	check_time_calls();
	check_traced();
	return failures;
}

}

#ifdef VDSO_PROBE_OWN_START
// A program the host builds against a stand-in of bionic's libc.so has none of
// the C runtime's start-up files. bionic's linker has already run the library's
// own initialisation, so the entry only needs the argument vector, a call and
// the exit.
extern "C" [[noreturn]] void probe_entry(long *stack)
{
	(void)stack;
	_exit(run());
}

asm(".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "	mov	x29, xzr\n"
    "	mov	x30, xzr\n"
    "	mov	x0, sp\n"
    "	bl	probe_entry\n");
#else
int main()
{
	return run();
}
#endif
