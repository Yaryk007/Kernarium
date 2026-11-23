// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A guest program that runs the Android image's rewritten libz.so in many
 * processes, for the rows that exercise how the framework shares what it
 * verified between address spaces.
 *
 * It is built like aot_guest_debug.cpp (Android arm64, the image's own libc.so
 * and libz.so, no headers, no runtime of its own) and loaded into a stock
 * process: AOT_SHARE_MODE=<mode> LD_PRELOAD=<this library> toybox true. The
 * parent never runs libz itself, except where a mode says so: its children do,
 * so that the parent maps few of the library's pages and the page cache can
 * drop the rest. The mode names what the children run:
 *
 *   fork      the system libz.so, twelve children one after another and then
 *             four at once: the pages of a read-only file system, which the
 *             kernel declares immutable, are verified once for all of them.
 *   copy      a byte-identical copy of libz.so on the writable data file
 *             system, which binds by content like the original but is not
 *             immutable, so every child verifies its pages.
 *   cow       children that write a text page of the system libz.so (a
 *             write of the same bytes copies the page), one that patches it,
 *             and after them children that do not: the copied page is
 *             anonymous in its own address space only.
 *   cow-parent  this process copies the page first, and its children inherit
 *             the copy (an anonymous page under a private mapping of a
 *             read-only file system): that page is refused, the library's
 *             others are shared.
 *   mprotect  children that change the protection of a text page to writable
 *             and back with no store: the page stays the file system's.
 *   mprotect-write  children that make the page writable, write it and make
 *             it executable again: the page is copied, so it is refused.
 *   pressure  rounds of children, each followed by a child that takes more
 *             memory than is free, so that the kernel reclaims file pages (the
 *             library's among them) and reuses their frames; the next round
 *             runs the library again. The guest is not rooted, so there is no
 *             drop_caches or compaction to do it exactly; the faults that
 *             reading the library back takes say whether it lost pages.
 *   evict     the one way a frame is freed without pressure, after the last
 *             mapping leaves: posix_fadvise(DONTNEED) on a file nothing maps
 *             (a control, which must lose its pages) and on libz.so, which
 *             loses the pages no process has mapped at that moment (the
 *             system's own processes map the rest). Whatever it evicts is run
 *             again by children, which must verify it afresh.
 *   reclaim   the same eviction done for real on the study's own guest only:
 *             a kernel whose madvise(MADV_PAGEOUT) pages out file pages
 *             whoever maps them and whoever owns the file, and whose mincore
 *             tells the truth about them (a stock kernel does neither).
 *             libz.so's pages are evicted from every address space three
 *             times over, with children running it again after each.
 *
 * Every line it prints is a function of the guest's architectural behaviour
 * alone: the runner (run_aot_guest.py) requires the same lines with the
 * component on and off, and, in a diagnostics build, reads how often the
 * framework hashed a page and how often it did not.
 */
#include <stddef.h>
#include <stdint.h>

extern "C" {
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int mincore(void *, size_t, unsigned char *);
int open(const char *, int, ...);
long read(int, void *, size_t);
long write(int, const void *, size_t);
int close(int);
int pipe(int *);
int unlink(const char *);
int dprintf(int, const char *, ...);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int strcmp(const char *, const char *);
char *getenv(const char *);
void _exit(int) __attribute__((noreturn));
int fork();
int waitpid(int, int *, int);
unsigned alarm(unsigned);

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
long lseek(int, long, int);
int posix_fadvise(int, long, long, int);
int fsync(int);
int madvise(void *, size_t, int);
int getrusage(int, void *);
}

namespace {

constexpr size_t kPage = 4096;
constexpr int kRead = 1;
constexpr int kWrite = 2;
constexpr int kExec = 4;
constexpr int kMapPrivateAnonymous = 0x22;
constexpr int kMapShared = 1;
constexpr int kOpenWriteCreateTruncate = 1 | 0x40 | 0x200;
constexpr int kRtldNow = 2;

constexpr const char *kSystemZ = "/system/lib64/libz.so";
constexpr const char *kCopyZ = "/data/local/tmp/libz_share_copy.so";

void fail(const char *what) {
    dprintf(1, "FAIL %s\n", what);
}

// A pool for libz's own allocations, so that no allocator state reaches the
// output.
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

// The entry points of the libz.so a child runs.
struct Zlib {
    unsigned long (*adler)(unsigned long, const uint8_t *, unsigned);
    unsigned long (*crc)(unsigned long, const uint8_t *, unsigned);
    const char *(*version)();
    int (*deflate_init)(ZStream *, int, const char *, int);
    int (*deflate_run)(ZStream *, int);
    int (*deflate_end)(ZStream *);
};

Zlib system_zlib() {
    return {adler32, crc32, zlibVersion, deflateInit_, deflate, deflateEnd};
}

template <typename T>
bool symbol(void *handle, const char *name, T &out) {
    void *address = dlsym(handle, name);
    out = reinterpret_cast<T>(address);
    return address != nullptr;
}

bool copy_zlib(void *handle, Zlib &z) {
    return symbol(handle, "adler32", z.adler) && symbol(handle, "crc32", z.crc) &&
           symbol(handle, "zlibVersion", z.version) && symbol(handle, "deflateInit_", z.deflate_init) &&
           symbol(handle, "deflate", z.deflate_run) && symbol(handle, "deflateEnd", z.deflate_end);
}

uint8_t *data;  // deterministic content the work reads
constexpr size_t kDataPages = 4;
constexpr unsigned kWorkBytes = 3 * 1024 + 129;

void fill_data() {
    data = static_cast<uint8_t *>(mmap(nullptr, 8 * kPage, kRead | kWrite, kMapPrivateAnonymous, -1, 0));
    uint32_t state = 0x2545f491U;
    for (size_t i = 0; i < kDataPages * kPage; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = (state & 7U) == 0U ? static_cast<uint8_t>(state >> 8) : "etaoin shrdlu"[(state >> 4) % 13U];
    }
}

// The covered calls a child makes: the same for a round and a function of the
// data alone.
unsigned long work(const Zlib &z, unsigned round) {
    uint8_t out[4096];
    const size_t at = (round * 37U) % 512U;
    unsigned long sum = z.crc(round, data + at, kWorkBytes);
    sum ^= z.adler(round + 1, data + at, kWorkBytes);
    ZStream stream{};
    pool_used = 0;
    stream.zalloc = reinterpret_cast<void *>(pool_alloc);
    stream.zfree = reinterpret_cast<void *>(pool_free);
    if (z.deflate_init(&stream, 6, z.version(), sizeof(ZStream)) == 0) {
        stream.next_in = data + at;
        stream.avail_in = 700;
        stream.next_out = out;
        stream.avail_out = sizeof(out);
        z.deflate_run(&stream, 4);
        sum += z.crc(0, out, static_cast<unsigned>(stream.total_out));
        z.deflate_end(&stream);
    }
    return sum;
}

// What a child does to its own copy of the library before it runs.
enum class Prep {
    None,
    // A write of the byte already there: the page is copied and stays equal.
    CopyPage,
    // The same, then the entry of adler32 is replaced.
    PatchAdler,
    // The text page is made writable and back, with no store.
    Protect,
    // The text page is made writable, written, and made executable again.
    ProtectAndWrite,
};

// Make the stores to [start, start + length) visible to instruction fetch.
void flush_code(void *start, size_t length) {
    const uintptr_t first = reinterpret_cast<uintptr_t>(start) & ~uintptr_t{15};
    const uintptr_t last = reinterpret_cast<uintptr_t>(start) + length;
    for (uintptr_t a = first; a < last; a += 16) {
        asm volatile("dc cvau, %0" ::"r"(a) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (uintptr_t a = first; a < last; a += 16) {
        asm volatile("ic ivau, %0" ::"r"(a) : "memory");
    }
    asm volatile("dsb ish\n\tisb" ::: "memory");
}

uint8_t *page_of(const void *function) {
    return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(function) & ~(kPage - 1));
}

void prepare(Prep prep, const Zlib &z) {
    if (prep == Prep::None) {
        return;
    }
    uint8_t *entry = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(z.adler));
    uint8_t *page = page_of(entry);
    if (mprotect(page, kPage, kRead | kWrite | kExec) != 0) {
        fail("mprotect writable");
        return;
    }
    volatile uint8_t *byte = page;
    switch (prep) {
    case Prep::CopyPage:
    case Prep::ProtectAndWrite:
        *byte = *byte;
        break;
    case Prep::PatchAdler: {
        // mov x0, #7 ; ret
        const uint32_t patch[2] = {0xd28000e0U, 0xd65f03c0U};
        memcpy(entry, patch, sizeof(patch));
        flush_code(entry, sizeof(patch));
        break;
    }
    default:
        break;
    }
    if (mprotect(page, kPage, kRead | kExec) != 0) {
        fail("mprotect executable");
    }
}

// One child: it prepares, runs two rounds and sends the sum to the parent.
bool child_start(int index, const Zlib &z, Prep prep, int &read_end, int &pid) {
    int fds[2];
    if (pipe(fds) != 0) {
        fail("pipe");
        return false;
    }
    pid = fork();
    if (pid < 0) {
        fail("fork");
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        prepare(prep, z);
        unsigned long sum = work(z, static_cast<unsigned>(index)) + work(z, static_cast<unsigned>(index) + 100U);
        write(fds[1], &sum, sizeof(sum));
        _exit(0);
    }
    close(fds[1]);
    read_end = fds[0];
    return true;
}

bool child_finish(const char *label, int index, int read_end, int pid) {
    unsigned long sum = 0;
    const long got = read(read_end, &sum, sizeof(sum));
    close(read_end);
    int status = 0;
    waitpid(pid, &status, 0);
    if (got != static_cast<long>(sizeof(sum)) || status != 0) {
        dprintf(1, "FAIL %s child %d: read %ld status %d\n", label, index, got, status);
        return false;
    }
    dprintf(1, "%s child %d sum=%#lx\n", label, index, sum);
    return true;
}

bool children_in_turn(const char *label, int first, int count, const Zlib &z, Prep prep) {
    for (int i = first; i < first + count; ++i) {
        int read_end = -1;
        int pid = -1;
        if (!child_start(i, z, prep, read_end, pid) || !child_finish(label, i, read_end, pid)) {
            return false;
        }
    }
    return true;
}

bool children_together(const char *label, int first, int count, const Zlib &z) {
    int read_ends[8];
    int pids[8];
    for (int i = 0; i < count; ++i) {
        if (!child_start(first + i, z, Prep::None, read_ends[i], pids[i])) {
            return false;
        }
    }
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        ok = child_finish(label, first + i, read_ends[i], pids[i]) && ok;
    }
    return ok;
}

// ---- modes -----------------------------------------------------------------

void fork_mode() {
    const Zlib z = system_zlib();
    if (children_in_turn("sequential", 0, 12, z, Prep::None)) {
        children_together("together", 12, 4, z);
    }
}

bool copy_file(const char *from, const char *to) {
    const int in = open(from, 0);
    if (in < 0) {
        return false;
    }
    const int out = open(to, kOpenWriteCreateTruncate, 0755);
    if (out < 0) {
        close(in);
        return false;
    }
    static uint8_t buffer[1 << 16];
    bool ok = true;
    for (;;) {
        const long got = read(in, buffer, sizeof(buffer));
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (write(out, buffer, static_cast<size_t>(got)) != got) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    return ok;
}

void copy_mode() {
    if (!copy_file(kSystemZ, kCopyZ)) {
        fail("copy libz.so");
        return;
    }
    void *handle = dlopen(kCopyZ, kRtldNow);
    Zlib z{};
    if (handle == nullptr || !copy_zlib(handle, z)) {
        fail("dlopen the copy");
        unlink(kCopyZ);
        return;
    }
    if (children_in_turn("copy", 0, 12, z, Prep::None)) {
        children_together("copy together", 12, 4, z);
    }
    unlink(kCopyZ);
}

void cow_mode() {
    const Zlib z = system_zlib();
    // The children copy the page in turn; the one that patches it changes
    // only its own copy.
    children_in_turn("copy-page", 0, 2, z, Prep::CopyPage);
    children_in_turn("patched", 2, 1, z, Prep::PatchAdler);
    children_in_turn("after-patch", 3, 3, z, Prep::None);
    children_together("together", 6, 4, z);
}

void protect_mode() {
    const Zlib z = system_zlib();
    children_in_turn("protect", 0, 3, z, Prep::Protect);
    children_in_turn("plain", 3, 3, z, Prep::None);
}

void protect_write_mode() {
    const Zlib z = system_zlib();
    children_in_turn("protect-write", 0, 3, z, Prep::ProtectAndWrite);
    children_in_turn("plain", 3, 3, z, Prep::None);
}

// The copy of the page is made here, and the children inherit it.
void cow_parent_mode() {
    const Zlib z = system_zlib();
    prepare(Prep::CopyPage, z);
    children_in_turn("inherited", 0, 3, z, Prep::None);
    children_together("together", 3, 4, z);
}

// How many of the file's pages the page cache holds, and how many it has.
// mincore tells only about a file the caller owns or could write: for any other
// file, every file of the read-only system image among them, a stock kernel
// reports every page as resident (the study's kernel does not). A stock guest
// learns whether a system file lost pages from the faults that reading it back
// takes (libz_readbacks).
long cached_pages(const char *path, long *total) {
    const int fd = open(path, 0);
    if (fd < 0) {
        return -1;
    }
    const long size = lseek(fd, 0, 2);
    *total = (size + static_cast<long>(kPage) - 1) / static_cast<long>(kPage);
    void *map = size > 0 ? mmap(nullptr, static_cast<size_t>(size), kRead, kMapShared, fd, 0)
                         : reinterpret_cast<void *>(-1L);
    if (map == reinterpret_cast<void *>(-1L)) {
        close(fd);
        return -1;
    }
    static unsigned char vec[4096];
    long count = -1;
    if (*total <= static_cast<long>(sizeof(vec)) && mincore(map, static_cast<size_t>(size), vec) == 0) {
        count = 0;
        for (long i = 0; i < *total; ++i) {
            count += vec[i] & 1;
        }
    }
    munmap(map, static_cast<size_t>(size));
    close(fd);
    return count;
}

// A number from /proc/meminfo, in kB, or -1.
long meminfo_kb(const char *key) {
    const int fd = open("/proc/meminfo", 0);
    if (fd < 0) {
        return -1;
    }
    static char text[4096];
    const long got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0) {
        return -1;
    }
    text[got] = 0;
    for (long i = 0; i < got; ++i) {
        if (i != 0 && text[i - 1] != '\n') {
            continue;
        }
        long k = 0;
        while (key[k] != 0 && text[i + k] == key[k]) {
            ++k;
        }
        if (key[k] != 0 || text[i + k] != ':') {
            continue;
        }
        long value = 0;
        for (long j = i + k + 1; j < got && text[j] != '\n'; ++j) {
            if (text[j] >= '0' && text[j] <= '9') {
                value = value * 10 + (text[j] - '0');
            }
        }
        return value;
    }
    return -1;
}

// A child that takes memory from the page cache: it touches anonymous pages
// up to what is free and a share of what the page cache holds, so that the
// kernel reclaims file pages, mapped ones included, and later reuses their
// frames. The guest is not rooted (no drop_caches, no compaction), and a page
// that another process maps stays through fadvise, so reclaim under pressure
// is the way a frame of the library is freed.
void pressure_child(long extra_kb) {
    const long free_kb = meminfo_kb("MemFree");
    if (free_kb < 0) {
        _exit(1);
    }
    const size_t bytes = static_cast<size_t>(free_kb + extra_kb) * 1024UL;
    uint8_t *region = static_cast<uint8_t *>(mmap(nullptr, bytes, kRead | kWrite, kMapPrivateAnonymous, -1, 0));
    if (region == reinterpret_cast<uint8_t *>(-1L)) {
        _exit(2);
    }
    for (size_t at = 0; at < bytes; at += kPage) {
        region[at] = 1;
    }
    _exit(0);
}

// The major faults of this process so far: faults that found no page in the
// page cache and read one (getrusage's ru_majflt).
long major_faults() {
    long usage[18] = {};
    return getrusage(0, usage) == 0 ? usage[9] : -1;
}

// libz.so mapped privately and read-only.
struct LibzMap {
    void *map = nullptr;
    long size = 0;
};

LibzMap map_libz() {
    constexpr int kMapPrivate = 2;
    LibzMap m;
    const int fd = open(kSystemZ, 0);
    m.size = fd >= 0 ? lseek(fd, 0, 2) : -1;
    if (m.size > 0) {
        void *p = mmap(nullptr, static_cast<size_t>(m.size), kRead, kMapPrivate, fd, 0);
        if (p != reinterpret_cast<void *>(-1L)) {
            m.map = p;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    return m;
}

// Touches every page of the mapping, which leaves them all in the page cache,
// and returns the major faults it took: how many runs of pages the page cache
// did not hold (the file system reads a run at a time), 0 when it held all.
long touch_readbacks(const LibzMap &m) {
    const long before = major_faults();
    for (long offset = 0; offset < m.size; offset += static_cast<long>(kPage)) {
        (void)static_cast<const volatile uint8_t *>(m.map)[offset];
    }
    return major_faults() - before;
}

// The faults it takes to read libz.so's absent pages back; the pages stay in
// the page cache afterwards.
long libz_readbacks() {
    const LibzMap m = map_libz();
    if (m.map == nullptr) {
        return -1;
    }
    const long faults = touch_readbacks(m);
    munmap(m.map, static_cast<size_t>(m.size));
    return faults;
}

// The rounds are comments the runner prints and does not compare: how many
// pages of libz.so the page cache held before and after the pressure depends
// on the guest's memory, not on the engine.
void pressure_mode() {
    const Zlib z = system_zlib();
    for (int round = 0; round < 4; ++round) {
        char label[16];
        label[0] = 'r';
        label[1] = static_cast<char>('0' + round);
        label[2] = 0;
        if (!children_in_turn(label, round * 3, 3, z, Prep::None)) {
            return;
        }
        const int pid = fork();
        if (pid == 0) {
            pressure_child(static_cast<long>(meminfo_kb("Cached")) / 2);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        dprintf(1, "# round %d: %ld faults read libz.so's pages back; the pressure child exited %d\n", round,
                libz_readbacks(), status);
    }
    children_in_turn("last", 12, 3, z, Prep::None);
}

// Pages the page cache holds of a file after the kernel is asked to drop
// them. Returns the count before in `before`.
long evict(const char *path, long &before) {
    long total = 0;
    before = cached_pages(path, &total);
    const int fd = open(path, 0);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, 4);  // POSIX_FADV_DONTNEED
        close(fd);
    }
    return cached_pages(path, &total);
}

// The control proves the pair (fadvise, mincore) evicts a file nothing maps;
// libz.so is then given the same, and the pages it loses (if any: the
// system's own processes map every page of a covered library, and the kernel
// does not drop a mapped page) are run again by children, which print the
// same sums whatever happened. The counts are comments the runner reads.
void evict_mode() {
    constexpr const char *control = "/data/local/tmp/libz_evict_control.bin";
    const int fd = open(control, kOpenWriteCreateTruncate, 0644);
    static uint8_t block[kPage];
    for (int i = 0; i < 256 && fd >= 0; ++i) {
        block[0] = static_cast<uint8_t>(i);
        if (write(fd, block, sizeof(block)) != static_cast<long>(sizeof(block))) {
            break;
        }
    }
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    long control_before = 0;
    const long control_after = evict(control, control_before);
    unlink(control);
    dprintf(1, "# control: %ld of 256 pages cached before, %ld after\n", control_before, control_after);
    if (control_before < 128 || control_after != 0) {
        fail("the control file was not evicted: eviction is not observable here");
        return;
    }
    const Zlib z = system_zlib();
    if (!children_in_turn("before", 0, 3, z, Prep::None)) {
        return;
    }
    const long libz_before = libz_readbacks();
    const int libz_fd = open(kSystemZ, 0);
    if (libz_fd >= 0) {
        posix_fadvise(libz_fd, 0, 0, 4);  // POSIX_FADV_DONTNEED
        close(libz_fd);
    }
    dprintf(1, "# libz.so: %ld faults read pages back before the eviction, %ld after\n", libz_before,
            libz_readbacks());
    children_in_turn("after", 3, 3, z, Prep::None);
}

// The study kernel's eviction, three times over: map libz.so, touch every page
// so that this address space has them, and page them out. The kernel unmaps
// each folio from every address space that maps it and drops it from the page
// cache, which frees the frame; touching the pages reads them back into frames
// the allocator hands out again (a frame freed in one round is a frame the
// next round may give to the same page), and the children that follow run the
// library, which the framework must verify from its bytes again each time. The
// counts are comments the runner reads; on a kernel that spares shared file
// pages (the stock one) nothing is evicted and the runner fails the row.
//
// AOT_SHARE_ROUND=n makes one invocation of it round n only (the first one
// also runs the children that precede the first eviction), so that the runner
// can read the framework's counters around every round. The page cache and the
// framework's table outlive the process, so the rounds follow one another as
// they do inside one.
void reclaim_mode() {
    constexpr int kMadvPageout = 21;
    constexpr int kRounds = 3;
    const char *only = getenv("AOT_SHARE_ROUND");
    const int first_round = only == nullptr ? 0 : only[0] - '0';
    const int end_round = only == nullptr ? kRounds : first_round + 1;
    if (only != nullptr && (only[0] < '0' || only[0] >= '0' + kRounds || only[1] != 0)) {
        fail("AOT_SHARE_ROUND is not a round of the reclaim mode");
        return;
    }
    const Zlib z = system_zlib();
    if (first_round == 0 && !children_in_turn("before", 0, 3, z, Prep::None)) {
        return;
    }
    const LibzMap m = map_libz();
    if (m.map == nullptr) {
        fail("map libz.so");
        return;
    }
    for (int round = first_round; round < end_round; ++round) {
        touch_readbacks(m);
        long total = 0;
        const long before = cached_pages(kSystemZ, &total);
        if (madvise(m.map, static_cast<size_t>(m.size), kMadvPageout) != 0) {
            fail("madvise(MADV_PAGEOUT)");
        }
        const long after = cached_pages(kSystemZ, &total);
        const long readbacks = touch_readbacks(m);
        dprintf(1, "# reclaim: libz.so %ld pages cached before the eviction, %ld after; %ld faults read them back\n",
                before, after, readbacks);
        char label[16];
        label[0] = 'a';
        label[1] = static_cast<char>('0' + round);
        label[2] = 0;
        if (!children_in_turn(label, 3 + round * 3, 3, z, Prep::None)) {
            break;
        }
    }
    munmap(m.map, static_cast<size_t>(m.size));
}

__attribute__((constructor)) void run() {
    const char *mode = getenv("AOT_SHARE_MODE");
    if (mode == nullptr) {
        return;
    }
    alarm(280);
    fill_data();
    if (strcmp(mode, "fork") == 0) {
        fork_mode();
    } else if (strcmp(mode, "copy") == 0) {
        copy_mode();
    } else if (strcmp(mode, "cow") == 0) {
        cow_mode();
    } else if (strcmp(mode, "cow-parent") == 0) {
        cow_parent_mode();
    } else if (strcmp(mode, "mprotect") == 0) {
        protect_mode();
    } else if (strcmp(mode, "mprotect-write") == 0) {
        protect_write_mode();
    } else if (strcmp(mode, "pressure") == 0) {
        pressure_mode();
    } else if (strcmp(mode, "evict") == 0) {
        evict_mode();
    } else if (strcmp(mode, "reclaim") == 0) {
        reclaim_mode();
    } else {
        dprintf(1, "FAIL unknown mode\n");
    }
    dprintf(1, "done mode=%s\n", mode);
    _exit(0);
}

}  // namespace
