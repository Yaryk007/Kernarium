// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A guest program that debugs, and crashes inside, the Android image's
 * rewritten libz.so and libc.so: what a debugger or a crash reporter sees must
 * be the same with the ahead-of-time component on and off.
 *
 * It is built like aot_guest_probe.cpp (Android arm64, the image's own libc.so
 * and libz.so, no headers, no runtime of its own) and loaded into a stock
 * process: AOT_DEBUG_MODE=<mode> LD_PRELOAD=<this library> toybox true. The
 * mode names what it does:
 *
 *   crash-memcpy   libc's memcpy faults on a source that runs into an
 *                  unmapped page, called by libz (inflateSetDictionary), so
 *                  that the backtrace has two rewritten libraries above the
 *                  program; the access that faults is the first of its
 *                  sequence and crosses the page. The process dies of SIGSEGV
 *                  and the crash reporter writes its tombstone.
 *   crash-cross    libc's memcmp loads eight bytes across the end of a mapped
 *                  page: a plain load of a rewritten sequence whose first
 *                  access cannot be refilled (the page after it is unmapped).
 *   crash-abort    covered calls, then abort() (tgkill).
 *   crash-sibling  the same abort while another thread runs libz: the crash
 *                  reporter stops every thread of the process (a thread
 *                  running rewritten code included) to unwind it.
 *   ptrace         a tracer and its tracee: attach, registers, a breakpoint
 *                  in libz, single steps, steps from libz into libc and back,
 *                  a changed register and a changed pc, detach and the
 *                  rewritten code entered again.
 *   sibling        a thread running libz in a loop gets a breakpoint written
 *                  by a tracer that never stops it, and, in a second round,
 *                  a breakpoint at the pc where it was found.
 *
 * It runs with address-space randomization off (personality
 * ADDR_NO_RANDOMIZE, which needs no privilege and takes effect at exec: the
 * constructor sets it and executes the process again once), so the registers
 * and addresses of a crash are a function of the guest's behaviour alone and
 * the runner compares them. It prints "randomization: off" once that holds
 * and "FAIL" if the personality cannot be set.
 *
 * Every line it prints is a function of the guest's architectural behaviour
 * alone: the runner (run_aot_guest.py) compares the same mode run with the
 * component on and off, after masking the values that differ between two runs
 * of the same engine (process ids, times, the stack protector's random value
 * and what depends on it).
 */
#include <stddef.h>
#include <stdint.h>

#include "guest_records.hpp"

extern "C" {
struct SigInfo {
    int signo;
    int err;
    int code;
    int pad;
    uint64_t addr;
};
struct SigAction {
    int flags;
    void (*handler)(int, SigInfo *, void *);
    uint64_t mask;
    void (*restorer)(void);
};
struct DlInfo {
    const char *fname;
    void *fbase;
    const char *sname;
    void *saddr;
};
struct IoVec {
    void *base;
    size_t len;
};
int sigaction(int, const SigAction *, SigAction *);
void *mmap(void *, size_t, int, int, int, long);
int mprotect(void *, size_t, int);
long write(int, const void *, size_t);
int dprintf(int, const char *, ...);
int dladdr(const void *, DlInfo *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
char *strrchr(const char *, int);
int strcmp(const char *, const char *);
int memcmp(const void *, const void *, size_t);
char *getenv(const char *);
int setenv(const char *, const char *, int);
int personality(unsigned long);
int execv(const char *, char *const[]);
int open(const char *, int, ...);
long read(int, void *, size_t);
int close(int);
void abort(void) __attribute__((noreturn));
void _exit(int) __attribute__((noreturn));
int fork();
int raise(int);
int kill(int, int);
int *__errno();
long ptrace(int, ...);
int waitpid(int, int *, int);
unsigned alarm(unsigned);
int prctl(int, ...);
long syscall(long, ...);
int usleep(unsigned);
int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);

// libz.so (zlib 1.3.x); z_stream is the LP64 layout.
struct ZStream {
    const uint8_t *next_in;
    uint32_t avail_in;
    uint64_t total_in;
    uint8_t *next_out;
    uint32_t avail_out;
    uint64_t total_out;
    const char *msg;
    void *state;
    void *zalloc;
    void *zfree;
    void *opaque;
    int data_type;
    uint64_t adler;
    uint64_t reserved;
};
unsigned long adler32(unsigned long, const uint8_t *, unsigned);
unsigned long crc32(unsigned long, const uint8_t *, unsigned);
const char *zlibVersion();
int deflateInit_(ZStream *, int, const char *, int);
int deflate(ZStream *, int);
int deflateEnd(ZStream *);
int inflateInit2_(ZStream *, int, const char *, int);
int inflateSetDictionary(ZStream *, const uint8_t *, unsigned);
int inflateEnd(ZStream *);
}

namespace {

constexpr size_t kPage = 4096;
constexpr int kRead = 1;
constexpr int kWrite = 2;
constexpr int kExec = 4;

constexpr int kSigTrap = 5;
constexpr int kSigKill = 9;
constexpr int kSigStop = 19;
constexpr int kWAll = 0x40000000;

constexpr int kPtraceTraceMe = 0;
constexpr int kPtracePeekText = 1;
constexpr int kPtracePokeText = 4;
constexpr int kPtraceCont = 7;
constexpr int kPtraceSingleStep = 9;
constexpr int kPtraceAttach = 16;
constexpr int kPtraceDetach = 17;
constexpr int kPtraceGetRegSet = 0x4204;
constexpr int kPtraceSetRegSet = 0x4205;
constexpr long kNtPrStatus = 1;
constexpr long kSysGettid = 178;
constexpr uint32_t kBrk = 0xd4200000U;  // brk #0, the instruction debuggers write

struct Regs {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

const char *base_name(const char *path) {
    const char *slash = path != nullptr ? strrchr(path, '/') : nullptr;
    return slash != nullptr ? slash + 1 : (path != nullptr ? path : "?");
}

// The library and the offset in it of an address; the name alone when only
// the library matters.
const char *library_of(uint64_t pc) {
    DlInfo info{};
    if (dladdr(reinterpret_cast<void *>(pc), &info) != 0 && info.fbase != nullptr) {
        return base_name(info.fname);
    }
    return "?";
}

// The library and the offset in it of an address, as a record's value; the
// number itself when no loaded library holds it.
aot_records::Value location(uint64_t pc) {
    DlInfo info{};
    if (dladdr(reinterpret_cast<void *>(pc), &info) != 0 && info.fbase != nullptr) {
        const char *name = base_name(info.fname);
        return aot_records::named_value(name, aot_records::length_of(name),
                                        pc - reinterpret_cast<uint64_t>(info.fbase));
    }
    return aot_records::number_value(pc);
}

struct Range {
    const char *name;
    uint64_t low;
    uint64_t high;
};
Range ranges[3];
int range_count;

// What the program prints is records (guest_records.hpp, schema in
// records.py): a field is a line of its own, and what the runner reads is
// exactly that.
void write_to_output(void *, const char *data, size_t length) {
    write(1, data, length);
}

const aot_records::Sink output{write_to_output, nullptr};

aot_records::Record record(const char *kind) {
    return aot_records::Record(output, kind);
}

// A count or a sum, as the number it is.
uint64_t unsigned_of(long value) {
    return static_cast<uint64_t>(static_cast<int64_t>(value));
}

// The tracee's mappings (/proc/<pid>/maps), read before its registers are
// printed: a value that points into one is named by it, whatever address the
// mapping has in this run (guest_records.hpp says how). A text that does not
// fit the buffer is a failure of the row: a layout that is cut short names
// nothing reliably.
char maps_text[1 << 17];
long maps_size;

void load_maps(int pid) {
    char path[32] = "/proc/";
    int length = 6;
    char digits[12];
    int count = 0;
    for (unsigned number = static_cast<unsigned>(pid); number != 0; number /= 10) {
        digits[count++] = static_cast<char>('0' + number % 10);
    }
    while (count > 0) {
        path[length++] = digits[--count];
    }
    const char tail[] = "/maps";
    for (const char *c = tail; *c != 0; ++c) {
        path[length++] = *c;
    }
    path[length] = 0;
    maps_size = 0;
    const int fd = open(path, 0);
    if (fd < 0) {
        return;
    }
    for (;;) {
        const long got = read(fd, maps_text + maps_size, sizeof(maps_text) - 1 - maps_size);
        if (got <= 0) {
            break;
        }
        maps_size += got;
    }
    close(fd);
    maps_text[maps_size] = 0;
    if (maps_size >= static_cast<long>(sizeof(maps_text)) - 1) {
        dprintf(1, "\nFAIL the maps of process %d do not fit %zu bytes\n", pid, sizeof(maps_text));
    }
}

// Where a register's value points, as the run does not choose it: a range the
// program knows, the library that holds it, a mapping of the tracee (by its
// name, or the shape of an unnamed one and the place in it), or the number.
aot_records::Value resolve_value(uint64_t value) {
    for (int i = 0; i < range_count; ++i) {
        if (value >= ranges[i].low && value < ranges[i].high) {
            return aot_records::named_value(ranges[i].name, aot_records::length_of(ranges[i].name),
                                            value - ranges[i].low);
        }
    }
    // An address in a loaded library is named by the library, so that two
    // runs with different load addresses print the same.
    DlInfo info{};
    if (value > 0x10000 && value < (1ULL << 48) && dladdr(reinterpret_cast<void *>(value), &info) != 0 &&
        info.fbase != nullptr) {
        const char *name = base_name(info.fname);
        return aot_records::named_value(name, aot_records::length_of(name),
                                        value - reinterpret_cast<uint64_t>(info.fbase));
    }
    aot_records::Value mapped;
    if (value > 0x10000 && maps_size > 0 && aot_records::resolve(maps_text, value, mapped)) {
        return mapped;
    }
    return aot_records::number_value(value);
}

// An error is a line of its own that begins with FAIL, wherever the program
// was in a line when it came: the newline before it ends the line it
// interrupts (a record line that was cut short is no record), and the runner
// rejects an output that holds FAIL anywhere.
void fail(const char *what) {
    dprintf(1, "\nFAIL %s\n", what);
}

// A pool for libz's own allocations: the library's calls to malloc would leave
// the allocator's random values in registers, which would differ between two
// runs of one engine.
alignas(16) uint8_t pool[1 << 20];
size_t pool_used;
extern "C" void *pool_alloc(void *, unsigned items, unsigned size) {
    const size_t bytes = (static_cast<size_t>(items) * size + 63U) & ~size_t{63};
    if (pool_used + bytes > sizeof(pool)) {
        return nullptr;
    }
    void *p = pool + pool_used;
    pool_used += bytes;
    memset(p, 0, bytes);
    return p;
}
extern "C" void pool_free(void *, void *) {}

// The sibling's buffer is large so that nearly all of its time is inside
// libz, in the rewritten code, and almost none in this program's own loop.
constexpr unsigned kSiblingBytes = 1U << 20;
uint8_t *sibling_buffer;

uint8_t *data;  // the pages the work reads: deterministic content
constexpr size_t kDataPages = 4;
constexpr unsigned kWorkBytes = 3 * 1024 + 129;

void fill_data() {
    data = static_cast<uint8_t *>(mmap(nullptr, 8 * kPage, kRead | kWrite, 0x22, -1, 0));
    sibling_buffer = static_cast<uint8_t *>(mmap(nullptr, kSiblingBytes, kRead | kWrite, 0x22, -1, 0));
    uint32_t state = 0x2545f491U;
    for (size_t i = 0; i < kDataPages * kPage; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = (state & 7U) == 0U ? static_cast<uint8_t>(state >> 8) : "etaoin shrdlu"[(state >> 4) % 13U];
    }
}

// The covered calls the tracee makes: the same on every round, and a function
// of the data alone.
unsigned long work(unsigned round) {
    uint8_t out[4096];
    const size_t at = (round * 37U) % 512U;
    unsigned long sum = crc32(round, data + at, kWorkBytes);
    sum ^= adler32(round + 1, data + at, kWorkBytes);
    ZStream stream{};
    pool_used = 0;
    stream.zalloc = reinterpret_cast<void *>(pool_alloc);
    stream.zfree = reinterpret_cast<void *>(pool_free);
    if (deflateInit_(&stream, 6, zlibVersion(), sizeof(ZStream)) == 0) {
        stream.next_in = data + at;
        stream.avail_in = 700;
        stream.next_out = out;
        stream.avail_out = sizeof(out);
        deflate(&stream, 4);
        sum += crc32(0, out, static_cast<unsigned>(stream.total_out));
        deflateEnd(&stream);
    }
    return sum;
}

// ---- tombstones ------------------------------------------------------------

void crash_memcpy() {
    uint8_t *region = static_cast<uint8_t *>(mmap(nullptr, 3 * kPage, kRead | kWrite, 0x22, -1, 0));
    mprotect(region + 2 * kPage, kPage, 0);
    for (size_t i = 0; i < 2 * kPage; ++i) {
        region[i] = static_cast<uint8_t>(i * 7U);
    }
    // Warm the covered code before the fault.
    unsigned long sum = 0;
    for (unsigned i = 0; i < 400; ++i) {
        sum += crc32(sum, region + (i % 17U), 513);
    }
    record("warm").number("sum", sum);
    ZStream stream{};
    stream.zalloc = reinterpret_cast<void *>(pool_alloc);
    stream.zfree = reinterpret_cast<void *>(pool_free);
    if (inflateInit2_(&stream, -15, zlibVersion(), sizeof(ZStream)) != 0) {
        fail("inflateInit2_");
        return;
    }
    // The dictionary's last 24 bytes lie below the unmapped page and the rest
    // above it: libz copies it into the window with libc's memcpy, whose first
    // access crosses into the unmapped page.
    record("note").text("text", "dictionary crosses the page");
    inflateSetDictionary(&stream, region + 2 * kPage - 24, 64);
    fail("the copy did not fault");
}

// A covered libc routine whose first access crosses into an unmapped page: a
// small memcmp loads eight bytes from each side, the first of them across the
// end of the mapped page.
void crash_cross() {
    uint8_t *region = static_cast<uint8_t *>(mmap(nullptr, 3 * kPage, kRead | kWrite, 0x22, -1, 0));
    mprotect(region + 2 * kPage, kPage, 0);
    for (size_t i = 0; i < 2 * kPage; ++i) {
        region[i] = static_cast<uint8_t>(i * 7U);
    }
    int sum = 0;
    for (unsigned i = 0; i < 400; ++i) {
        sum += memcmp(region + (i % 17U), region + 1024 + (i % 13U), 12);
    }
    record("warm").number("sum", unsigned_of(sum));
    record("note").text("text", "memcmp crosses the page");
    sum = memcmp(region + 2 * kPage - 6, region + 64, 12);
    dprintf(1, "\nFAIL the compare did not fault (%d)\n", sum);
}

void crash_abort() {
    unsigned long sum = 0;
    for (unsigned i = 0; i < 400; ++i) {
        sum += crc32(sum, data + (i % 17U), 513);
    }
    record("warm").number("sum", sum);
    abort();
}

// ---- ptrace helpers --------------------------------------------------------

bool get_regs(int pid, Regs &regs) {
    IoVec vec{&regs, sizeof(regs)};
    return ptrace(kPtraceGetRegSet, pid, reinterpret_cast<void *>(kNtPrStatus), &vec) == 0;
}

bool set_regs(int pid, Regs &regs) {
    IoVec vec{&regs, sizeof(regs)};
    return ptrace(kPtraceSetRegSet, pid, reinterpret_cast<void *>(kNtPrStatus), &vec) == 0;
}

int g_tracee;

void print_regs(const char *tag, const Regs &regs) {
    load_maps(g_tracee);
    aot_records::Record r = record("regs");
    r.text("tag", tag);
    char label[4];
    for (int i = 0; i < 31; ++i) {
        label[0] = 'x';
        label[1] = static_cast<char>('0' + i / 10);
        label[2] = static_cast<char>('0' + i % 10);
        label[3] = 0;
        r.value(label, resolve_value(regs.x[i]));
    }
    r.value("sp", resolve_value(regs.sp));
    r.value("pc", location(regs.pc));
    r.number("nzcv", regs.pstate & 0xf0000000ULL);
}

// Waits for the next event of a tracee; returns the stop signal, or -1 when it
// ended.
int wait_stop(int pid) {
    int status = 0;
    const int r = waitpid(pid, &status, kWAll);
    if (r != pid) {
        return -1;
    }
    if ((status & 0xff) != 0x7f) {
        record("tracee_ended").number("status", static_cast<uint64_t>(status));
        return -1;
    }
    return (status >> 8) & 0xff;
}

struct Breakpoint {
    uint64_t address;
    uint32_t saved;
    bool set;
};

bool set_breakpoint(int pid, Breakpoint &bp, uint64_t address) {
    *__errno() = 0;
    const long word = ptrace(kPtracePeekText, pid, reinterpret_cast<void *>(address), nullptr);
    if (*__errno() != 0) {
        return false;
    }
    bp.address = address;
    bp.saved = static_cast<uint32_t>(word);
    const uint64_t patched = (static_cast<uint64_t>(word) & ~0xffffffffULL) | kBrk;
    if (ptrace(kPtracePokeText, pid, reinterpret_cast<void *>(address), reinterpret_cast<void *>(patched)) != 0) {
        return false;
    }
    bp.set = true;
    return true;
}

bool clear_breakpoint(int pid, Breakpoint &bp) {
    if (!bp.set) {
        return true;
    }
    *__errno() = 0;
    const long word = ptrace(kPtracePeekText, pid, reinterpret_cast<void *>(bp.address), nullptr);
    const uint64_t restored = (static_cast<uint64_t>(word) & ~0xffffffffULL) | bp.saved;
    bp.set = false;
    return ptrace(kPtracePokeText, pid, reinterpret_cast<void *>(bp.address), reinterpret_cast<void *>(restored)) == 0;
}

// Runs the tracee to its next stop; true when it stopped with `expected`.
bool resume_to(int pid, int request, int expected, Regs &regs) {
    if (ptrace(request, pid, nullptr, nullptr) != 0) {
        fail("resume");
        return false;
    }
    const int sig = wait_stop(pid);
    if (sig != expected) {
        dprintf(1, "\nFAIL stop signal %d, expected %d\n", sig, expected);
        return false;
    }
    return get_regs(pid, regs);
}

[[noreturn]] void tracee() {
    prctl(1, kSigKill);  // PR_SET_PDEATHSIG
    alarm(240);
    ptrace(kPtraceTraceMe, 0, nullptr, nullptr);
    raise(kSigStop);
    unsigned long sum = 0;
    for (unsigned round = 0; round < 300; ++round) {
        sum += work(round);
    }
    record("tracee_done").number("sum", sum);
    _exit(0);
}

bool in_library(uint64_t pc, const char *name) {
    return strcmp(library_of(pc), name) == 0;
}

// A call into the tracee's covered function: the hit, the arguments and the
// registers there.
bool run_to(int pid, Breakpoint &bp, uint64_t address, const char *what, Regs &regs) {
    if (!set_breakpoint(pid, bp, address)) {
        fail("set breakpoint");
        return false;
    }
    if (!resume_to(pid, kPtraceCont, kSigTrap, regs)) {
        return false;
    }
    aot_records::Record r = record("breakpoint");
    r.text("what", what);
    r.value("hit", location(regs.pc));
    r.text("at_breakpoint", regs.pc == address ? "yes" : "NO");
    if (regs.pc != address) {
        fail("not at the breakpoint");
        return false;
    }
    return clear_breakpoint(pid, bp);
}

// The next call of `address` that reads the data pages (the tracee also
// calls it on its own stack): the process can compute the answer itself.
bool run_to_data_call(int pid, Breakpoint &bp, uint64_t address, const char *what, Regs &regs) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (attempt > 0 && !resume_to(pid, kPtraceSingleStep, kSigTrap, regs)) {
            return false;  // off the instruction the last breakpoint was on
        }
        if (!run_to(pid, bp, address, what, regs)) {
            return false;
        }
        if (regs.x[1] >= ranges[0].low && regs.x[1] < ranges[0].high) {
            return true;
        }
        record("note").text("text", "not the data: buffer is not in D, going on");
    }
    fail("no call on the data");
    return false;
}

void ptrace_mode() {
    alarm(480);
    fill_data();
    ranges[0] = {"D", reinterpret_cast<uint64_t>(data), reinterpret_cast<uint64_t>(data) + 8 * kPage};
    range_count = 1;
    const int pid = fork();
    if (pid == 0) {
        tracee();
    }
    g_tracee = pid;
    if (wait_stop(pid) != kSigStop) {
        fail("the tracee did not stop at its raise");
        return;
    }
    Regs regs{};
    if (!get_regs(pid, regs)) {
        fail("GETREGSET");
        return;
    }
    record("attached").value("pc", location(regs.pc));

    // 1. A breakpoint on a covered libz function and the state there.
    const uint64_t crc_entry = reinterpret_cast<uint64_t>(&crc32);
    const uint64_t adler_entry = reinterpret_cast<uint64_t>(&adler32);
    const uint64_t compress_entry = reinterpret_cast<uint64_t>(&deflate);
    Breakpoint bp{};
    if (!run_to(pid, bp, crc_entry, "breakpoint crc32", regs)) {
        return;
    }
    range_count = 2;
    ranges[1] = {"S", regs.sp - 0x10000, regs.sp + 0x10000};
    print_regs("crc32", regs);
    const uint64_t return_address = regs.x[30];
    record("returns_to").value("location", location(return_address));

    // 2. Single steps through the function.
    for (int step = 0; step < 40; ++step) {
        if (!resume_to(pid, kPtraceSingleStep, kSigTrap, regs)) {
            return;
        }
        aot_records::Record r = record("step");
        r.integer("n", step);
        r.value("pc", location(regs.pc));
        if (step == 0 || step == 9 || step == 39) {
            print_regs("step", regs);
        }
    }

    // 3. Leave the function: run to the return address, with the result.
    Breakpoint back{};
    if (!set_breakpoint(pid, back, return_address) || !resume_to(pid, kPtraceCont, kSigTrap, regs)) {
        return;
    }
    clear_breakpoint(pid, back);
    aot_records::Record back_at = record("returned");
    back_at.value("pc", location(regs.pc));
    back_at.text("at_return", regs.pc == return_address ? "yes" : "NO");

    // 4. A changed register: a shorter length; the result is the library's
    // answer for it, which this process computes itself (from the call that
    // reads the data, whose bytes it has too).
    if (!run_to_data_call(pid, bp, crc_entry, "breakpoint crc32 on the data", regs)) {
        return;
    }
    const uint64_t seed = regs.x[0];
    const uint64_t buffer = regs.x[1];
    const uint64_t length = regs.x[2];
    const uint64_t back_address = regs.x[30];
    regs.x[2] = length - 9;
    if (!set_regs(pid, regs)) {
        fail("SETREGSET");
        return;
    }
    if (!set_breakpoint(pid, back, back_address) || !resume_to(pid, kPtraceCont, kSigTrap, regs)) {
        return;
    }
    clear_breakpoint(pid, back);
    const unsigned long expected =
        crc32(seed, reinterpret_cast<const uint8_t *>(buffer), static_cast<unsigned>(length - 9));
    aot_records::Record length_changed = record("changed");
    length_changed.text("what", "length");
    length_changed.text("verdict", regs.x[0] == expected ? "is" : "IS NOT");
    if (regs.x[0] != expected) {
        fail("the changed length");
        return;
    }

    // 5. A changed pc: the next crc32 call on the data becomes a call of
    // adler32 (same arguments, same return address).
    if (!run_to_data_call(pid, bp, crc_entry, "breakpoint crc32 on the data again", regs)) {
        return;
    }
    const uint64_t seed3 = regs.x[0];
    const uint64_t buffer3 = regs.x[1];
    const uint64_t length3 = regs.x[2];
    const uint64_t back3 = regs.x[30];
    regs.pc = adler_entry;
    if (!set_regs(pid, regs)) {
        fail("SETREGSET pc");
        return;
    }
    if (!set_breakpoint(pid, back, back3) || !resume_to(pid, kPtraceCont, kSigTrap, regs)) {
        return;
    }
    clear_breakpoint(pid, back);
    const unsigned long adler =
        adler32(seed3, reinterpret_cast<const uint8_t *>(buffer3), static_cast<unsigned>(length3));
    aot_records::Record pc_changed = record("changed");
    pc_changed.text("what", "pc");
    pc_changed.text("verdict", regs.x[0] == adler ? "is" : "IS NOT");
    if (regs.x[0] != adler) {
        fail("the changed pc");
        return;
    }

    // 6. From libz into libc and back, one instruction at a time: every
    // change of library is recorded with the step it happens at.
    if (!run_to(pid, bp, compress_entry, "breakpoint deflate", regs)) {
        return;
    }
    const char *current = library_of(regs.pc);
    int transitions = 0;
    int libc_steps = 0;
    int steps = 0;
    for (; steps < 6000 && transitions < 24; ++steps) {
        if (!resume_to(pid, kPtraceSingleStep, kSigTrap, regs)) {
            return;
        }
        const char *now = library_of(regs.pc);
        if (strcmp(now, "libc.so") == 0) {
            ++libc_steps;
        }
        if (strcmp(now, current) != 0) {
            ++transitions;
            aot_records::Record r = record("transition");
            r.integer("step", steps);
            r.text("from", current);
            r.text("to", now);
            r.value("pc", location(regs.pc));
            current = now;
        }
    }
    aot_records::Record summary = record("steps");
    summary.integer("steps", steps);
    summary.integer("changes", transitions);
    summary.integer("libc", libc_steps);

    // 7. Detach: the tracee runs its remaining rounds on its own, through the
    // rewritten code, and prints its sum.
    if (ptrace(kPtraceDetach, pid, nullptr, nullptr) != 0) {
        fail("detach");
        return;
    }
    int status = 0;
    if (waitpid(pid, &status, kWAll) != pid) {
        fail("waitpid after detach");
        return;
    }
    record("detached").number("status", static_cast<uint64_t>(status));
}

// The extent of the library that holds `address`, from its program headers
// in memory: the tracer, a forked child of a threaded process, must not call
// dladdr.
struct Extent {
    uint64_t low;
    uint64_t high;
};

Extent extent_of(uint64_t address) {
    DlInfo info{};
    Extent extent{0, 0};
    if (dladdr(reinterpret_cast<void *>(address), &info) == 0 || info.fbase == nullptr) {
        return extent;
    }
    const auto *base = static_cast<const uint8_t *>(info.fbase);
    const uint64_t phoff = *reinterpret_cast<const uint64_t *>(base + 32);
    const unsigned phnum = *reinterpret_cast<const uint16_t *>(base + 56);
    uint64_t high = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *header = base + phoff + 56 * i;
        if (*reinterpret_cast<const uint32_t *>(header) == 1) {  // PT_LOAD
            const uint64_t end = *reinterpret_cast<const uint64_t *>(header + 16) + *reinterpret_cast<const uint64_t *>(header + 40);
            if (end > high) {
                high = end;
            }
        }
    }
    extent.low = reinterpret_cast<uint64_t>(info.fbase);
    extent.high = extent.low + high;
    return extent;
}

// ---- a thread that is never stopped ---------------------------------------

// Shared with the tracer, which is another process: the breakpoint it writes
// and the word it replaced, set before the breakpoint is.
struct Shared {
    volatile uint64_t address;
    volatile uint32_t saved;
};
Shared *shared;

volatile uint64_t g_calls;
volatile int g_tid_b;
volatile int g_hits;
volatile uint64_t g_hit_pc;
volatile int g_hit_code;
volatile int g_hit_tid;
volatile int g_stop_b;

void flush(void *start, size_t size) {
    const uint64_t first = reinterpret_cast<uint64_t>(start) & ~uint64_t{63};
    const uint64_t last = reinterpret_cast<uint64_t>(start) + size;
    for (uint64_t at = first; at < last; at += 64) {
        asm volatile("dc cvau, %0" ::"r"(at) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (uint64_t at = first; at < last; at += 64) {
        asm volatile("ic ivau, %0" ::"r"(at) : "memory");
    }
    asm volatile("dsb ish\n\tisb" ::: "memory");
}

constexpr size_t kContext = 176;

// The breakpoint was hit by a thread that nobody stopped: the handler records
// where and puts the instruction back, so that the thread runs it again.
void on_trap(int, SigInfo *info, void *context) {
    auto *words = reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(context) + kContext);
    g_hit_pc = words[33];
    g_hit_code = info->code;
    g_hit_tid = static_cast<int>(syscall(kSysGettid));
    const uint64_t address = shared->address;
    auto *at = reinterpret_cast<uint32_t *>(address);
    void *page = reinterpret_cast<void *>(address & ~(kPage - 1));
    mprotect(page, kPage, kRead | kWrite | kExec);
    *at = shared->saved;
    flush(at, 4);
    mprotect(page, kPage, kRead | kExec);
    g_hits = g_hits + 1;
}

void *thread_b(void *) {
    g_tid_b = static_cast<int>(syscall(kSysGettid));
    while (g_stop_b == 0) {
        g_calls = g_calls + 1;
        crc32(0, sibling_buffer, kSiblingBytes);
    }
    return nullptr;
}

bool wait_for_calls(uint64_t more, int seconds) {
    const uint64_t first = g_calls;
    for (int i = 0; i < seconds * 1000; ++i) {
        if (g_calls >= first + more) {
            return true;
        }
        usleep(1000);
    }
    return false;
}

// The tracer, a process of its own: finds the address (the entry of crc32, or
// the pc where it catches the sibling `sampler` running in libz), stops the
// thread `target` (a thread that is not the sibling), writes a breakpoint
// there through it and lets it go. The sibling is never stopped while the
// breakpoint is written.
[[noreturn]] void tracer_writes(int target, bool find_pc, int sampler, Extent libz) {
    alarm(120);
    uint64_t address = reinterpret_cast<uint64_t>(&crc32);
    if (find_pc) {
        address = 0;
        for (int attempt = 0; attempt < 40 && address == 0; ++attempt) {
            if (ptrace(kPtraceAttach, sampler, nullptr, nullptr) != 0) {
                _exit(2);
            }
            if (wait_stop(sampler) < 0) {
                _exit(3);
            }
            Regs regs{};
            if (!get_regs(sampler, regs)) {
                _exit(4);
            }
            ptrace(kPtraceDetach, sampler, nullptr, nullptr);
            if (regs.pc >= libz.low && regs.pc < libz.high) {
                address = regs.pc;
            } else {
                usleep(2000);
            }
        }
        if (address == 0) {
            _exit(5);
        }
    }
    if (ptrace(kPtraceAttach, target, nullptr, nullptr) != 0) {
        _exit(6);
    }
    if (wait_stop(target) < 0) {
        _exit(7);
    }
    *__errno() = 0;
    const long word = ptrace(kPtracePeekText, target, reinterpret_cast<void *>(address), nullptr);
    if (*__errno() != 0) {
        _exit(8);
    }
    shared->address = address;
    shared->saved = static_cast<uint32_t>(word);
    asm volatile("dmb ish" ::: "memory");
    const uint64_t patched = (static_cast<uint64_t>(word) & ~0xffffffffULL) | kBrk;
    if (ptrace(kPtracePokeText, target, reinterpret_cast<void *>(address), reinterpret_cast<void *>(patched)) != 0) {
        _exit(9);
    }
    ptrace(kPtraceDetach, target, nullptr, nullptr);
    _exit(0);
}

void sibling_round(const char *name, bool in_loop, int main_tid) {
    const Extent libz = extent_of(reinterpret_cast<uint64_t>(&crc32));
    g_hits = 0;
    g_hit_pc = 0;
    if (!wait_for_calls(30, 30)) {
        fail("the thread does not run");
        return;
    }
    const int tracer = fork();
    if (tracer == 0) {
        tracer_writes(main_tid, in_loop, g_tid_b, libz);
    }
    int status = 0;
    if (waitpid(tracer, &status, 0) != tracer || status != 0) {
        dprintf(1, "\nFAIL tracer status=%#x\n", status);
        return;
    }
    for (int i = 0; i < 10000 && g_hits == 0; ++i) {
        usleep(1000);
    }
    aot_records::Record hit = record(in_loop ? "sibling_loop" : "sibling_entry");
    hit.text("round", name);
    hit.text("hit", g_hits == 1 ? "yes" : "NO");
    hit.text("by_sibling", g_hit_tid == g_tid_b ? "yes" : "NO");
    hit.text("pc_at_breakpoint", g_hit_pc == shared->address ? "yes" : "NO");
    hit.text("in_libz", in_library(g_hit_pc, "libz.so") ? "yes" : "NO");
    hit.integer("code", g_hit_code);
    if (!in_loop) {
        hit.value("pc", location(g_hit_pc));
    }
    if (g_hits != 1) {
        fail("no hit");
        return;
    }
    aot_records::Record goes_on = record("sibling_runs_on");
    goes_on.text("round", name);
    goes_on.text("yes", wait_for_calls(20, 30) ? "yes" : "NO");
}

void sibling_mode() {
    alarm(240);
    fill_data();
    shared = static_cast<Shared *>(mmap(nullptr, kPage, kRead | kWrite, 0x21, -1, 0));  // MAP_SHARED | MAP_ANONYMOUS
    SigAction action{};
    action.handler = on_trap;
    action.flags = 4 | 0x40000000;  // SA_SIGINFO | SA_NODEFER
    sigaction(kSigTrap, &action, nullptr);
    unsigned long thread = 0;
    if (pthread_create(&thread, nullptr, thread_b, nullptr) != 0) {
        fail("pthread_create");
        return;
    }
    const int main_tid = static_cast<int>(syscall(kSysGettid));
    for (int i = 0; i < 20000 && g_tid_b == 0; ++i) {
        usleep(1000);
    }
    sibling_round("entry", false, main_tid);
    sibling_round("in loop", true, main_tid);
    g_stop_b = 1;
}

// The process aborts while another thread runs libz: the crash reporter
// stops every thread of the process to unwind it, this one included.
void crash_with_sibling() {
    alarm(240);
    fill_data();
    unsigned long thread = 0;
    if (pthread_create(&thread, nullptr, thread_b, nullptr) != 0) {
        fail("pthread_create");
        return;
    }
    if (!wait_for_calls(20, 30)) {
        fail("the thread does not run");
        return;
    }
    record("note").text("text", "sibling is running");
    abort();
}

// Run the program again with address-space randomization off. The
// personality is inherited by exec and applies to the new image's layout.
constexpr unsigned long kQuery = 0xffffffffUL;
constexpr unsigned long kAddrNoRandomize = 0x0040000UL;

bool randomization_off() {
    const int current = personality(kQuery);
    return current >= 0 && (static_cast<unsigned long>(current) & kAddrNoRandomize) != 0;
}

// Returns only if the process cannot be executed again.
void execute_without_randomization() {
    static char arguments[4096];
    static char *vector[64];
    const int fd = open("/proc/self/cmdline", 0);
    if (fd < 0) {
        return;
    }
    long used = 0;
    for (;;) {
        const long got = read(fd, arguments + used, sizeof(arguments) - 1 - used);
        if (got <= 0) {
            break;
        }
        used += got;
    }
    close(fd);
    arguments[used] = 0;
    size_t count = 0;
    for (long at = 0; at < used && count < 63;) {
        vector[count++] = arguments + at;
        while (at < used && arguments[at] != 0) {
            ++at;
        }
        ++at;
    }
    vector[count] = nullptr;
    execv("/proc/self/exe", vector);
}

bool without_randomization() {
    if (randomization_off()) {
        return true;
    }
    if (getenv("AOT_DEBUG_REEXEC") != nullptr) {
        dprintf(1, "\nFAIL randomization is still on after the program ran again\n");
        return false;
    }
    const int current = personality(kQuery);
    if (current < 0 || personality(static_cast<unsigned long>(current) | kAddrNoRandomize) < 0) {
        dprintf(1, "\nFAIL the personality cannot be set\n");
        return false;
    }
    setenv("AOT_DEBUG_REEXEC", "1", 1);
    execute_without_randomization();
    dprintf(1, "\nFAIL the program cannot be run again\n");
    return false;
}

__attribute__((constructor)) void run() {
    const char *mode = getenv("AOT_DEBUG_MODE");
    if (mode == nullptr) {
        return;
    }
    if (!without_randomization()) {
        _exit(0);
    }
    record("randomization").text("state", "off");
    if (strcmp(mode, "crash-memcpy") == 0) {
        fill_data();
        crash_memcpy();
    } else if (strcmp(mode, "crash-cross") == 0) {
        crash_cross();
    } else if (strcmp(mode, "crash-abort") == 0) {
        fill_data();
        crash_abort();
    } else if (strcmp(mode, "crash-sibling") == 0) {
        crash_with_sibling();
    } else if (strcmp(mode, "ptrace") == 0) {
        ptrace_mode();
    } else if (strcmp(mode, "sibling") == 0) {
        sibling_mode();
    } else {
        dprintf(1, "\nFAIL unknown mode\n");
    }
    record("done").text("mode", mode);
    _exit(0);
}

}  // namespace
