// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A guest program that exercises the Android image's libz.so where the
 * ahead-of-time rewrite is observable: faults taken inside rewritten code, and
 * rewritten text that the program itself changes.
 *
 * It is built for Android arm64 against the image's own libc.so and libz.so,
 * with no headers and no runtime of its own, and loaded into a stock process:
 * LD_PRELOAD=<this library> toybox true. Every line it prints is a function of
 * the guest's architectural behaviour alone, so the same program run with the
 * ahead-of-time component on and off must print the same text (the runner
 * compares the two, after masking the registers that differ between two runs
 * of the same engine, such as stack addresses under ASLR).
 *
 * Fault cases: a read and a write fault inside libz.so (adler32, crc32,
 * inflate's fast path), each taken at the first byte of a guard page and in
 * the middle of an access sequence, with the handler that prints the fault
 * address, the program counter and the registers and then either leaves with
 * siglongjmp or repairs the page and returns so that the instruction runs
 * again; and the same read through a pointer with a tag in its top byte.
 *
 * Patch cases: a covered libz function is overwritten at run time (mprotect,
 * store, cache maintenance as the architecture requires) and called, other
 * functions on the same page are called again, and the original bytes are put
 * back.
 */
#include <stddef.h>
#include <stdint.h>

extern "C" {
// Only what the program uses of libc.so; the image's headers are not needed.
struct SigInfo {
    int signo;
    int err;
    int code;
    int pad;
    uint64_t addr;
};
// Bionic's LP64 struct sigaction.
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
int sigaction(int, const SigAction *, SigAction *);
int sigsetjmp(long *, int) __attribute__((returns_twice));
void siglongjmp(long *, int) __attribute__((noreturn));
void *mmap(void *, size_t, int, int, int, long);
int mprotect(void *, size_t, int);
long write(int, const void *, size_t);
int dprintf(int, const char *, ...);
int dladdr(const void *, DlInfo *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
char *strrchr(const char *, int);

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
unsigned long crc32_z(unsigned long, const uint8_t *, size_t);
const char *zlibVersion();
int compress2(uint8_t *, unsigned long *, const uint8_t *, unsigned long, int);
unsigned long compressBound(unsigned long);
int inflateInit_(ZStream *, const char *, int);
int inflate(ZStream *, int);
int inflateEnd(ZStream *);
}

namespace {

constexpr size_t kPage = 4096;
constexpr int kRead = 1;
constexpr int kWrite = 2;
constexpr int kExec = 4;

// The pages of the test region.
constexpr size_t kSource = 0;     // pages 0-2: source data; page 3 is staging below the guard
constexpr size_t kSourceBytes = 3 * kPage;
constexpr size_t kGuard = 4;      // PROT_NONE
constexpr size_t kReadOnly = 5;   // PROT_READ
constexpr size_t kScratch = 6;    // pages 6-9: output buffers
constexpr size_t kGuardEnd = 10;  // PROT_NONE
constexpr size_t kPages = 12;

uint8_t *region;
long jump[64];
bool repair;  // a fault repairs its page and returns instead of leaving
int faults;
int handled;

struct Range {
    const char *name;
    uint64_t low;
    uint64_t high;
};
Range ranges[4];
int range_count;

const char *base_name(const char *path) {
    const char *slash = path != nullptr ? strrchr(path, '/') : nullptr;
    return slash != nullptr ? slash + 1 : (path != nullptr ? path : "?");
}

void print_value(const char *label, uint64_t value) {
    for (int i = 0; i < range_count; ++i) {
        if (value >= ranges[i].low && value < ranges[i].high) {
            dprintf(1, " %s=%s+%#llx", label, ranges[i].name,
                    static_cast<unsigned long long>(value - ranges[i].low));
            return;
        }
    }
    dprintf(1, " %s=%#llx", label, static_cast<unsigned long long>(value));
}

void print_location(const char *label, uint64_t pc) {
    DlInfo info{};
    if (dladdr(reinterpret_cast<void *>(pc), &info) != 0 && info.fbase != nullptr) {
        dprintf(1, " %s=%s+%#llx", label, base_name(info.fname),
                static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(info.fbase)));
        return;
    }
    dprintf(1, " %s=%#llx", label, static_cast<unsigned long long>(pc));
}

// The kernel's arm64 ucontext: flags, link, stack (24), signal mask, padding
// to 1024 bits, then the 16-byte aligned machine context.
constexpr size_t kContext = 176;

void on_signal(int signo, SigInfo *info, void *context) {
    auto *bytes = static_cast<uint8_t *>(context);
    auto *words = reinterpret_cast<uint64_t *>(bytes + kContext);
    // words[0] fault address, words[1..31] x0-x30, words[32] sp, words[33] pc,
    // words[34] pstate.
    ranges[0] = {"R", reinterpret_cast<uint64_t>(region), reinterpret_cast<uint64_t>(region) + kPages * kPage};
    ranges[1] = {"S", words[32] - 0x10000, words[32] + 0x10000};
    range_count = 2;
    ++faults;
    dprintf(1, "  fault signo=%d code=%d", signo, info->code);
    print_value("addr", info->addr);
    print_location("pc", words[33]);
    print_value("sp", words[32]);
    dprintf(1, " pstate=%#llx\n", static_cast<unsigned long long>(words[34] & 0xf0000000ULL));
    dprintf(1, "  regs");
    char label[8];
    for (int i = 0; i < 31; ++i) {
        label[0] = 'x';
        label[1] = static_cast<char>('0' + i / 10);
        label[2] = static_cast<char>('0' + i % 10);
        label[3] = 0;
        print_value(label, words[1 + i]);
    }
    dprintf(1, "\n");
    if (repair) {
        const uint64_t page = (info->addr & 0x00ffffffffffffffULL) & ~(kPage - 1);
        ++handled;
        mprotect(reinterpret_cast<void *>(page), kPage, kRead | kWrite);
        return;
    }
    siglongjmp(jump, 1);
}

void install_handlers() {
    SigAction action{};
    action.handler = on_signal;
    action.flags = 4 | 0x40000000;  // SA_SIGINFO | SA_NODEFER
    sigaction(11, &action, nullptr);
    sigaction(7, &action, nullptr);
}

uint32_t random_state = 0x2545f491U;
uint32_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

void fill_source() {
    // Compressible, not trivially: words from a small alphabet.
    for (size_t i = 0; i < kSourceBytes; ++i) {
        const uint32_t r = next_random();
        region[kSource * kPage + i] = (r & 7U) == 0U ? static_cast<uint8_t>(r >> 8) : "etaoin shrdlu"[(r >> 4) % 13U];
    }
}

void arm_guards() {
    mprotect(region + kGuard * kPage, kPage, 0);
    mprotect(region + kReadOnly * kPage, kPage, kRead);
    mprotect(region + kGuardEnd * kPage, kPage, 0);
}

void repair_guards() {
    mprotect(region + kGuard * kPage, kPage, 0);
    mprotect(region + kReadOnly * kPage, kPage, kRead);
    mprotect(region + kGuardEnd * kPage, kPage, 0);
}

// A pool for libz's own allocations, so that its registers hold no heap
// pointers (their addresses would differ between two runs).
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

void begin(const char *name) {
    dprintf(1, "case %s\n", name);
    pool_used = 0;
}

// Runs one call; a fault that the handler left through siglongjmp is reported.
#define GUARDED(statement)                    \
    do {                                      \
        if (sigsetjmp(jump, 1) == 0) {        \
            statement;                        \
        } else {                              \
            dprintf(1, "  left by signal\n"); \
        }                                     \
    } while (0)

// Many calls of covered functions, so that a run's counters show the rewritten
// code was entered; the sum is the same with the component on or off.
void many_calls() {
    begin("many-calls");
    unsigned long sum = 0;
    for (unsigned i = 0; i < 3000; ++i) {
        const size_t at = (i * 37U) % (kSourceBytes - 1024);
        sum += crc32(sum, region + kSource * kPage + at, 1024) ^ adler32(i, region + kSource * kPage + at, 509);
    }
    dprintf(1, "  sum=%#lx\n", sum);
}

void read_faults() {
    unsigned long result = 0;
    // A read that ends in the guard page: the first access to it is in the
    // middle of the function's loop.
    static const unsigned lead_values[] = {1U, 7U, 64U, 100U, 4093U};
    for (const unsigned lead : lead_values) {
        begin("adler32-guard");
        dprintf(1, " lead=%u\n", lead);
        result = 0;
        GUARDED(result = adler32(1, region + kGuard * kPage - lead, lead + 33));
        dprintf(1, "  result=%#lx\n", result);
    }
    static const unsigned crc_leads[] = {1U, 5U, 63U, 129U, 2048U};
    for (const unsigned lead : crc_leads) {
        begin("crc32-guard");
        dprintf(1, " lead=%u\n", lead);
        result = 0;
        GUARDED(result = crc32(0, region + kGuard * kPage - lead, lead + 40));
        dprintf(1, "  result=%#lx\n", result);
    }
    begin("crc32_z-wholly-unmapped");
    result = 0;
    GUARDED(result = crc32_z(0, region + kGuard * kPage, 100));
    dprintf(1, "  result=%#lx\n", result);
    begin("adler32-null");
    result = 0;
    GUARDED(result = adler32(1, reinterpret_cast<const uint8_t *>(8), 100));
    dprintf(1, "  result=%#lx\n", result);
}

// A tag in the top byte, as Scudo gives heap pointers.
void tagged() {
    const uint64_t tag = 0xb4ULL << 56;
    begin("crc32-tagged-good");
    const unsigned long plain = crc32(0, region + kSource * kPage, 3 * kPage + 17);
    const unsigned long marked = crc32(0, reinterpret_cast<const uint8_t *>(reinterpret_cast<uint64_t>(region + kSource * kPage) | tag), 3 * kPage + 17);
    dprintf(1, "  plain=%#lx tagged=%#lx %s\n", plain, marked, plain == marked ? "same" : "DIFFER");
    begin("crc32-tagged-guard");
    unsigned long result = 0;
    GUARDED(result = crc32(0, reinterpret_cast<const uint8_t *>(reinterpret_cast<uint64_t>(region + kGuard * kPage - 3) | tag), 40));
    dprintf(1, "  result=%#lx\n", result);
}

// The program's own stream: libz compresses the source, and the result is
// the input of the inflate cases.
uint8_t compressed[32768];
unsigned long compressed_size;

void make_stream() {
    begin("compress2");
    compressed_size = sizeof(compressed);
    const int status = compress2(compressed, &compressed_size, region + kSource * kPage, kSourceBytes, 6);
    dprintf(1, "  status=%d size=%lu crc=%#lx\n", status, compressed_size, crc32(0, compressed, static_cast<unsigned>(compressed_size)));
}

int inflate_into(uint8_t *out, uint32_t out_size, const uint8_t *in, uint32_t in_size) {
    ZStream stream{};
    stream.zalloc = reinterpret_cast<void *>(pool_alloc);
    stream.zfree = reinterpret_cast<void *>(pool_free);
    int status = inflateInit_(&stream, zlibVersion(), sizeof(ZStream));
    if (status != 0) {
        return status;
    }
    stream.next_in = in;
    stream.avail_in = in_size;
    stream.next_out = out;
    stream.avail_out = out_size;
    status = inflate(&stream, 4);
    dprintf(1, "  inflate status=%d in=%llu out=%llu adler=%#llx\n", status, static_cast<unsigned long long>(stream.total_in),
            static_cast<unsigned long long>(stream.total_out), static_cast<unsigned long long>(stream.adler));
    inflateEnd(&stream);
    return status;
}

void inflate_faults() {
    // Output that runs into the guard page: inflate's fast path stores
    // byte by byte.
    static const unsigned room_values[] = {1U, 2U, 300U, 1024U};
    for (const unsigned room : room_values) {
        begin("inflate-output-guard");
        dprintf(1, " room=%u\n", room);
        // The output buffer ends at the guard page; avail_out claims more.
        GUARDED(inflate_into(region + kGuardEnd * kPage - room, static_cast<uint32_t>(room + 900), compressed, static_cast<uint32_t>(compressed_size)));
    }
    // Input that runs into the guard page.
    static const unsigned tail_values[] = {2U, 9U, 400U};
    for (const unsigned tail : tail_values) {
        begin("inflate-input-guard");
        dprintf(1, " tail=%u\n", tail);
        uint8_t *at = region + kGuard * kPage - tail;
        // The stream's last bytes lie below the guard; the claim is longer.
        // Copy first (the pages below the guard are writable).
        if (compressed_size >= tail) {
            memcpy(at, compressed, tail);
        }
        GUARDED(inflate_into(region + kScratch * kPage, 4 * kPage, at, static_cast<uint32_t>(tail + 64)));
    }
    // Output into a read-only page.
    begin("inflate-output-readonly");
    GUARDED(inflate_into(region + kReadOnly * kPage, kPage, compressed, static_cast<uint32_t>(compressed_size)));
    // The whole stream, no fault: the rewritten code must still produce the
    // exact result.
    begin("inflate-whole");
    inflate_into(region + kScratch * kPage, 4 * kPage, compressed, static_cast<uint32_t>(compressed_size));
    dprintf(1, "  output %s\n", memcmp(region + kScratch * kPage, region + kSource * kPage, kSourceBytes) == 0 ? "matches" : "DIFFERS");
}

// A handler that repairs the page and returns: the faulting instruction runs
// again inside the rewritten function, which must finish with the exact result.
void repaired_faults() {
    repair = true;
    static const unsigned lead_values[] = {3U, 100U};
    for (const unsigned lead : lead_values) {
        begin("adler32-repaired");
        dprintf(1, " lead=%u\n", lead);
        // The page below the guard is made readable again by the handler.
        mprotect(region + kGuard * kPage, kPage, 0);
        const unsigned long result = adler32(1, region + kGuard * kPage - lead, lead + 200);
        dprintf(1, "  result=%#lx handled=%d\n", result, handled);
    }
    begin("crc32-repaired");
    mprotect(region + kGuard * kPage, kPage, 0);
    const unsigned long result = crc32(0, region + kGuard * kPage - 17, 400);
    dprintf(1, "  result=%#lx handled=%d\n", result, handled);
    begin("inflate-output-repaired");
    mprotect(region + kGuardEnd * kPage, kPage, 0);
    inflate_into(region + kGuardEnd * kPage - 100, 5000, compressed, static_cast<uint32_t>(compressed_size));
    dprintf(1, "  handled=%d\n", handled);
    repair = false;
}

// Covered text changed by the program.
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

void patched_text() {
    // Patch compressBound: it is a small covered function.
    auto *function = reinterpret_cast<uint32_t *>(reinterpret_cast<uint64_t>(&compressBound));
    auto *page = reinterpret_cast<void *>(reinterpret_cast<uint64_t>(function) & ~(kPage - 1));
    begin("patch-before");
    const unsigned long before = compressBound(1000);
    dprintf(1, "  compressBound=%lu\n", before);
    uint32_t saved[2] = {function[0], function[1]};
    const int status = mprotect(page, kPage, kRead | kWrite | kExec);
    dprintf(1, "  mprotect=%d\n", status);
    if (status != 0) {
        dprintf(1, "  patch cases skipped\n");
        return;
    }
    begin("patch-same-bytes");
    function[0] = saved[0];
    function[1] = saved[1];
    flush(function, 8);
    dprintf(1, "  compressBound=%lu adler=%#lx crc=%#lx\n", compressBound(1000), adler32(1, region, 777), crc32(0, region, 777));
    begin("patch-change");
    function[0] = 0x52800540;  // mov w0, #42
    function[1] = 0xd65f03c0;  // ret
    flush(function, 8);
    dprintf(1, "  compressBound=%lu\n", compressBound(1000));
    // Other covered functions on the same page, after the change.
    dprintf(1, "  adler=%#lx crc=%#lx\n", adler32(1, region, 777), crc32(0, region, 777));
    {
        uint8_t again[8192];
        unsigned long size = sizeof(again);
        const int status2 = compress2(again, &size, region + kSource * kPage, 2 * kPage, 6);
        dprintf(1, "  compress2=%d size=%lu\n", status2, size);
    }
    begin("patch-restore");
    function[0] = saved[0];
    function[1] = saved[1];
    flush(function, 8);
    dprintf(1, "  compressBound=%lu\n", compressBound(1000));
    mprotect(page, kPage, kRead | kExec);
    begin("patch-after");
    inflate_into(region + kScratch * kPage, 4 * kPage, compressed, static_cast<uint32_t>(compressed_size));
    dprintf(1, "  output %s\n", memcmp(region + kScratch * kPage, region + kSource * kPage, kSourceBytes) == 0 ? "matches" : "DIFFERS");
}

__attribute__((constructor)) void run() {
    region = static_cast<uint8_t *>(mmap(nullptr, kPages * kPage, kRead | kWrite, 0x22, -1, 0));
    if (region == reinterpret_cast<uint8_t *>(-1)) {
        dprintf(1, "mmap failed\n");
        return;
    }
    install_handlers();
    fill_source();
    make_stream();
    many_calls();
    arm_guards();
    read_faults();
    tagged();
    inflate_faults();
    repair_guards();
    repaired_faults();
    repair_guards();
    patched_text();
    dprintf(1, "done faults=%d\n", faults);
}

}  // namespace
