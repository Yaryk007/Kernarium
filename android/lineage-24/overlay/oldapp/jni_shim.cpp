// SPDX-License-Identifier: Apache-2.0

#include "jni_shim.hpp"

#include <stdarg.h>

#include "low_arena.hpp"

namespace oldapp {
namespace {

using namespace jni;

constexpr int kMaxParams = 255;      // the JVM's limit on parameter slots
constexpr uint32_t kMethodBuckets = 8192;  // the heads of the method registry's chains (the registry itself has no limit)
constexpr uint32_t kFieldSlots = 2048;
constexpr unsigned kProbeLimit = 32;
constexpr unsigned kMaxBlocks = 512;  // Dalvik's limit on local references; a thread has no more live references
constexpr unsigned kMaxFrames = 64;   // frame records of a thread (PushLocalFrame and native invocations)
constexpr unsigned kMaxThunks = 1024; // native functions that can get a thunk
constexpr unsigned kRegisterChunk = 32;  // RegisterNatives methods copied at a time
constexpr uint32_t kDalvikArrayClass = 0x5a324241u;
constexpr uint32_t kBlockLive = 0xb10c11feu;
constexpr uint32_t kBlockEnded = 0xe4ded10cu;  // the references ended, the array is not collected yet
constexpr uint32_t kBlockDead = 0xdeadb10cu;
constexpr unsigned kEndedBuckets = 4096;        // the ended index, by the array's identity hash
constexpr unsigned kMaxExisting = 16384;        // blocks that exist (live, pending, ended, pinned): each one ends with a weak
                                                // global reference, and the total stays well below ART's 51200
constexpr uint32_t kIdsRetryMs = 1000;          // between two looks for the classes of the calls below that failed
constexpr unsigned kSweepStep = 32;             // ended blocks tested at a creation after a collection
constexpr uint32_t kCollectIntervalMs = 10000;  // between two Runtime.gc() calls made for room
constexpr uint32_t kFullSweepIntervalMs = 1000;  // between two full sweeps that no collection asked for
constexpr unsigned kRecordSlots = 2048;         // the runtime's own pointers that are out (Get...Elements / Critical on a real reference)
constexpr size_t kBlockTail = 16;  // zeroes behind the data: raw readers overrun by a few bytes
constexpr uint32_t kChunk = 4096;  // the unit of the block's synchronization with its array
constexpr uint32_t kScanBlock = 256;   // the bytes the merges compare with one libc call before they look at words
constexpr uint32_t kPullPiece = 16384;  // the array's bytes are read into the scratch buffer, this many at a time

template <class T>
struct IsVoid {
    static constexpr bool value = false;
};
template <>
struct IsVoid<void> {
    static constexpr bool value = true;
};
template <class T>
struct IsObject {
    static constexpr bool value = false;
};
template <>
struct IsObject<jobject> {
    static constexpr bool value = true;
};

// The Dalvik (Android 1.5 to 3.2) layout of an array object: Object {clazz, lock}, length, and the
// contents on the next 8-byte boundary.
struct DalvikArray {
    uint32_t clazz;
    uint32_t lock;
    uint32_t length;
    uint32_t pad;
    uint8_t data[8];
};
static_assert(offsetof(DalvikArray, length) == 8, "Dalvik ArrayObject length offset");
static_assert(offsetof(DalvikArray, data) == 16, "Dalvik ArrayObject contents offset");

// The layer's own header in front of the Dalvik object.
//
// Which addresses are blocks, in which state, and how long each one is, is the block record's to say (below), and nothing in
// the region has a say: no path reads a magic, a length, a link or a content byte of arena memory to decide that an address is
// a block, what state it is in or how long it is (the header has no length: capacity_of reads it from the record). The memory of
// a block that has been reclaimed can be made another block, or be merged and cut again, while an application's stale reference
// to it is in use, and what lies at the old address then is whatever was written there (a chunk header, another block's header,
// Dalvik words, contents the application chose). A header is read only where its block cannot be reclaimed: by the thread whose
// entries hold it, by a wrapper that holds an access (a reference or a pin that keeps the array), or under the mutex after a
// check of the record. `magic` is a diagnostic marker: stored (relaxed) at the transitions, so that a header in a memory dump
// says what the block was, and read by nothing.
// Every other field is read and written under the mutex (global, weak, bucket_next, list_next, list_prev), by the thread whose
// entries hold the block (refs, next), or once at the publication (baseline, hash); `pins` is atomic.
struct BlockHeader {
    uint32_t arena_links[2];  // not fields: the arena's link space while the block is free (Arena::kFreeLinkBytes); nothing reads a freed header
    uint32_t magic;      // kBlockLive / kBlockEnded / kBlockDead: a marker, never read (see above)
    jobject global;      // a global reference to the real array (a live block; an ended block keeps it while it is pinned)
    BlockHeader* next;   // the queue of blocks that wait for an environment
    uint8_t* baseline;   // the bytes as of the last synchronization (outside the low arena)
    uint32_t refs;       // the references the application holds to this block
    uint32_t pins;       // pointers Get...Elements / Get...Critical handed out that have not been released (mode 0 or ABORT)
    jobject weak;        // an ended block: a weak global reference to the array (null while a pin keeps `global`)
    uint32_t hash;       // the identity hash of the array, for the index of the ended blocks
    BlockHeader* bucket_next;  // the ended index: the chain of the hash's bucket, the newest first
    BlockHeader* list_next;    // the ended list, in the order the blocks ended (the collection sweeps)
    BlockHeader* list_prev;
};
constexpr size_t kBlockHead = (sizeof(BlockHeader) + 15) & ~size_t{15};
static_assert(sizeof(BlockHeader::arena_links) == Arena::kFreeLinkBytes && offsetof(BlockHeader, arena_links) == 0,
              "the arena's links overwrite the first bytes of a freed block, and only the padding is there");
static_assert(kBlockHead == (sizeof(void*) == 8 ? 96u : 64u), "the header in front of the Dalvik object is as shipped");

// ---- the block record
//
// Where a block starts and what state it is in is kept here, outside the region, and nowhere else. An address is a block only
// when this record says that a block starts exactly there; no byte of the region decides it, so the memory of a block that was
// reclaimed, merged and cut again, or covered by another block's contents, never makes its old address a block.
//
// One 32-bit word for each 64-byte window of the region (kRecordWindow), written only by the layer:
//   bits 0-1  the state: 0 no block starts in the window, 1 live, 2 ended
//   bits 2-3  the 16-byte slot of the window the block starts at
//   bits 4-31 the block's capacity, its length (below 2^28: the region is below kMaxShadowBytes)
// A word is zero or names the one block that starts in its window. Two blocks that exist at once have disjoint arena chunks, and
// a block's chunk is at least kRecordWindow bytes (the assert below), so their starts are further apart than a window: each word
// has one writer at a time and is written by plain release stores, never a read-modify-write.
//   - make_shadow publishes a block with the last store to it (the word, state live, with the length).
//   - end_locked and take_ended turn it ended and live again, in the same hold of the mutex as the ended index changes.
//   - reclaim_ended clears it, under the mutex, before the memory goes back to the arena. A block that starts in the same window
//     later has a chunk that overlaps this one's, so it is allocated after the free, hence after the clear.
// An unlocked reader answers from one acquire load of the word, which gives the state and the length together (fact_of), and
// takes that one answer for the whole of what it does. A block whose array the collector has cleared is still an ended block of
// its length here until the layer reclaims it (a sweep); only the clear removes it. The layer keeps the memory of the record
// (Platform::map_record: zero-filled, never cleared by the layer): up to kMaxShadowBytes / 64 words, resident by page only where
// block starts have been recorded, and a page stays resident after its blocks are gone.
constexpr uint32_t kRecordWindow = 64;
enum BlockState : uint32_t { kStateNone = 0, kStateLive = 1, kStateEnded = 2 };
static_assert(kMaxShadowBytes < (size_t{1} << 28), "a capacity fits the 28 bits of a record word");
static_assert(kRecordWindow % 16 == 0 && kBlockHead % 16 == 0, "block references are 16-byte aligned from the region");
static_assert(kBlockHead + offsetof(DalvikArray, data) + kBlockTail + Arena::kChunkHeaderBytes >= kRecordWindow,
              "a block's chunk is at least a record window, so a window holds at most one block's start");

// What the layer knows of a method: the kinds of its parameters (Z B C S I J F D, or L for every reference), `nparams` bytes
// that follow the struct in the same allocation. A node is published whole, by the store of its bucket's head, and is never
// changed or freed, so a reader needs no lock. Nothing bounds the number of methods or of parameters (the JVM's 255 slots).
struct MethodInfo {
    MethodInfo* next;  // the next node of the chain
    void* id;
    uint8_t nparams;
    uint8_t returns_bytes;
};

// A frame of the thread's local references: the entries below `height` belong to outer frames.
enum FrameKind : uint8_t { kFramePush = 1, kFrameNative = 2 };
struct FrameRecord {
    uint16_t height;
    uint8_t kind;
    void* lr;  // kFrameNative: the return address into the runtime
    void* sp;  // kFrameNative: the stack pointer the native method was entered with
};

// The references the application holds to byte[] blocks, in the order it got them: one entry for each result, so a
// block the same array was returned as twice has two entries (`refs` counts them), as Dalvik had two local references.
struct ThreadState {
    BlockHeader* blocks[kMaxBlocks];
    unsigned count;
    uint32_t bytes;  // the capacity of the live blocks
    Env* env;        // this thread's environment, as of the last entry made (the ends of references need it)
    FrameRecord frames[kMaxFrames];
    unsigned frame_count;
    unsigned frame_overflow;  // PushLocalFrame calls past kMaxFrames: their pops end nothing
    unsigned critical_depth;  // the runtime's critical regions this thread has open (GetPrimitiveArrayCritical, GetStringCritical)
};

// A pointer the runtime itself handed out for a real reference (and a block's reference may release): what it is, and how
// many Gets of it are out.
enum RecordKind : uint32_t { kRecordFree = 0, kRecordElements = 1, kRecordCritical = 2 };
struct PointerRecord {
    const void* pointer;
    uint32_t kind;
    uint32_t count;
};

struct Globals {
    Platform platform;
    void* orig[kSlotCount];
    void* orig_vm[kVmSlotCount];
    bool installed;
    bool vm_installed;
    bool shadows_on;
    Arena shadow_arena;
    const uint8_t* shadow_lo;
    const uint8_t* shadow_hi;
    uint32_t* record;  // the block record's memory (Platform::map_record)
    uint32_t exposed;  // set once, before the first block is published, never cleared (atomic): a shadow may have reached the application
    BlockHeader* pending;  // blocks of ended threads, waiting for an environment
    uint32_t declined_logged;  // the first declined shadow of the process has been logged
    uint32_t peak_logged;      // the highest block count that has been logged, and when
    uint32_t peak_logged_ms;
    uint32_t region_logged;    // the longest layer-opened region that has been logged, and when
    uint32_t region_logged_ms;
    uint32_t lock;
    uint32_t thunk_lock;
    void* thunk_target[kMaxThunks];
    unsigned thunk_count;
    unsigned thunk_capacity;
    uint32_t thunk_none_logged;   // the first application native that got no thunk has been logged
    uint32_t ended_logged;        // the first block that ended at a return has been logged
    uint32_t ended_max_logged;    // the highest number of references one return has ended that has been logged, and when
    uint32_t ended_max_logged_ms;
    // The ended blocks (see "ended blocks" below); everything here is under the ended lock.
    uint32_t ended_lock;       // the spin lock, where the platform gives no mutex
    BlockHeader* ended_bucket[kEndedBuckets];
    BlockHeader ended_head;    // the anchor of the circular list of the ended blocks
    BlockHeader* sweep_cursor; // the next ended block a sweep tests
    bool sweeping;             // a collection was seen: the list is being tested, kSweepStep blocks at a creation
    jobject sentinel;          // a weak reference to an array nothing holds: a collection clears it
    unsigned ended_count;
    uint64_t ended_bytes;
    uint32_t ending;           // ends that are under way (atomic): a thread that finds none and no ended block has nothing to wait for
    uint32_t ids_state;        // 0 not looked up, 1 found, 2 not found (tried again after kIdsRetryMs), 3 being looked up
    uint32_t ids_failed_ms;    // when the look for them last failed
    uint32_t existing;         // the blocks that hold a reservation: one for each block from its creation to its free (atomic)
    jclass system_class;       // global references to the classes of the calls below
    jmethodID hash_method;     // System.identityHashCode(Object)
    jclass runtime_class;
    jmethodID runtime_get;     // Runtime.getRuntime()
    jmethodID runtime_gc;      // Runtime.gc()
    uint32_t full_sweep_ms;    // when the last full sweep ran
    bool full_swept;           // one has run
    uint32_t gc_seen;          // the collections the layer has noticed (the sentinel found cleared), by either sweep
    uint32_t gc_full_seen;     // gc_seen as of the last full sweep: a collection past it has not been swept in full
    uint32_t collect_ms;       // when Runtime.gc() was last asked for
    uint32_t collect_asked;    // it has been asked for
    uint32_t collect_logged;
    PointerRecord records[kRecordSlots];  // open addressing by pointer; under records_lock
    unsigned records_in_use;
    uint32_t records_lock;     // the table's own lock (a leaf: nothing is called and no other lock is taken under it)
    uint32_t records_full_logged;  // the first pointer that found the table full has been logged
    uint8_t scratch[kPullPiece];  // the bytes of the array that a pull or a merge reads; under the mutex (see "scratch")
    MethodInfo* method_bucket[kMethodBuckets];  // the heads of the registry's chains; a head is stored with release, under `lock`
    void* byte_fields[kFieldSlots];
    Counters counters;
};

Globals g;

void lock_registry() {
    while (__atomic_exchange_n(&g.lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
}
void unlock_registry() { __atomic_store_n(&g.lock, 0u, __ATOMIC_RELEASE); }

template <class F>
F original(int slot) {
    return reinterpret_cast<F>(g.orig[slot]);
}

bool from_app(const void* return_address) { return g.platform.is_app_caller(return_address); }

inline bool is_shadow(const void* object) {
    const uint8_t* p = static_cast<const uint8_t*>(object);
    return p >= g.shadow_lo && p < g.shadow_hi;
}

inline uint32_t live_shadows() { return __atomic_load_n(&g.counters.shadows_live, __ATOMIC_RELAXED); }

// Whether the address of a block may be in the application's hands. The state is set once, by the first make_shadow that
// publishes a block, before the block is published, and never cleared. It is not the number of blocks that exist: that is zero
// while a block is being revived (counted live after its contents are synchronized) or built, and after the last one has been
// reclaimed, and an application keeps the address of a block long after the block is gone. Until it is set no address of the
// region has been handed out, so no call, release or recorded pointer has a shadow to find. A thread has an address only from
// the thread that published the block, through a chain of synchronization that includes the store, so a relaxed load sees it.
inline bool shadows_exposed() { return __atomic_load_n(&g.exposed, __ATOMIC_RELAXED) != 0; }
inline void expose_shadows() {
    if (!shadows_exposed()) {
        __atomic_store_n(&g.exposed, 1u, __ATOMIC_RELEASE);
    }
}

ThreadState* thread() { return static_cast<ThreadState*>(g.platform.thread_state(sizeof(ThreadState))); }

// ---- shadow blocks

BlockHeader* header_of(const void* block) {
    return reinterpret_cast<BlockHeader*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(block)) - kBlockHead);
}

// What the record says of the address `object`.
struct BlockFact {
    uint32_t state;     // BlockState; kStateNone: no block starts exactly there
    uint32_t capacity;  // the block's length (0 for none)
};
// One acquire load of one word answers state and length together; no byte of the region is read. The range and the alignment
// tests come first, so a reference of the runtime's costs two compares and never reaches the record.
inline BlockFact fact_of(const void* object) {
    const uint8_t* p = static_cast<const uint8_t*>(object);
    if (p < g.shadow_lo || p >= g.shadow_hi) {
        return {kStateNone, 0};
    }
    const size_t offset = static_cast<size_t>(p - g.shadow_lo);
    if ((offset & 15u) != 0) {
        return {kStateNone, 0};
    }
    const uint32_t word = __atomic_load_n(&g.record[offset / kRecordWindow], __ATOMIC_ACQUIRE);  // read once
    if ((word & 3u) == 0 || ((word >> 2) & 3u) != ((offset >> 4) & 3u)) {
        return {kStateNone, 0};
    }
    return {word & 3u, word >> 4};
}
inline uint32_t state_of(const void* object) { return fact_of(object).state; }

// The writers, under the rules of the record above. The word of a header is the one of its Dalvik object's reference.
inline uint32_t* record_word(const BlockHeader* header, uint32_t* slot) {
    const size_t offset = static_cast<size_t>(reinterpret_cast<const uint8_t*>(header) + kBlockHead - g.shadow_lo);
    *slot = static_cast<uint32_t>((offset >> 4) & 3u);
    return &g.record[offset / kRecordWindow];
}
// A new block, the last store to it.
inline void record_publish(const BlockHeader* header, uint32_t capacity) {
    uint32_t slot;
    uint32_t* word = record_word(header, &slot);
    __atomic_store_n(word, (capacity << 4) | (slot << 2) | kStateLive, __ATOMIC_RELEASE);
}
// The block ends or lives again (under the mutex, by the one writer of the word): the length and the slot stay.
inline void record_turn(const BlockHeader* header, uint32_t state) {
    uint32_t slot;
    uint32_t* word = record_word(header, &slot);
    __atomic_store_n(word, (__atomic_load_n(word, __ATOMIC_RELAXED) & ~3u) | state, __ATOMIC_RELEASE);
}
inline void record_clear(const BlockHeader* header) {
    uint32_t slot;
    __atomic_store_n(record_word(header, &slot), 0u, __ATOMIC_RELEASE);
}
// The diagnostic marker of a header (see BlockHeader): never read.
inline void mark(BlockHeader* header, uint32_t magic) { __atomic_store_n(&header->magic, magic, __ATOMIC_RELAXED); }

// The length of a block the caller holds (its entry, a reference or pin that keeps its array, the mutex after a check of the
// record): decoded from the record, the one source of a length. 0 where no block starts there.
inline uint32_t capacity_of(const BlockHeader* header) {
    return fact_of(reinterpret_cast<const uint8_t*>(header) + kBlockHead).capacity;
}

// Under the mutex, or by a holder of the block: whether a block starts at `header`'s place.
inline bool block_exists(const BlockHeader* header) {
    return state_of(reinterpret_cast<const uint8_t*>(header) + kBlockHead) != kStateNone;
}

// The header of a block that is live, otherwise null (an ended block, or an address that is not a block).
BlockHeader* live_header(const void* object) {
    return state_of(object) == kStateLive ? header_of(object) : nullptr;
}

DalvikArray* array_of(BlockHeader* header) {
    return reinterpret_cast<DalvikArray*>(reinterpret_cast<uint8_t*>(header) + kBlockHead);
}

// What the runtime is handed in place of a reference of the layer's: see Conv and Access below.

// A block with no reference left is not freed: see end_block (ended blocks, below).
void end_block(Env* env, BlockHeader* header);
void drain_pending(Env* env);

// Whether a JNI call may follow: not with an exception pending (almost no function may run then).
bool jni_usable(Env* env) { return !original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env); }

// Blocks that wait for an environment without a pending exception: a thread that ended, or an end with an exception
// pending.
void queue_pending(BlockHeader* header) {
    BlockHeader* head = __atomic_load_n(&g.pending, __ATOMIC_RELAXED);
    do {
        header->next = head;
    } while (!__atomic_compare_exchange_n(&g.pending, &head, header, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

// Whether `header` is one of this thread's own entries (a live block this thread holds a reference to).
bool has_entry(const ThreadState* state, const BlockHeader* header) {
    for (unsigned i = 0; i < state->count; ++i) {
        if (state->blocks[i] == header) {
            return true;
        }
    }
    return false;
}

// A reference of the thread ended: the block ends with its last reference. A thread that has made entries has an
// environment; without one (it cannot be), the block waits for the next wrapped call that has one.
void release_entry(Env* env, ThreadState* state, BlockHeader* header) {
    if (--header->refs > 0) {
        return;
    }
    state->bytes -= capacity_of(header);
    if (env != nullptr) {
        end_block(env, header);
        return;
    }
    queue_pending(header);
}

// Ends the entries above `height`, the newest first; returns how many.
unsigned end_entries(ThreadState* state, unsigned height) {
    unsigned ended = 0;
    while (state->count > height) {
        release_entry(state->env, state, state->blocks[--state->count]);
        ++ended;
    }
    return ended;
}

// Every wrapped function that has an environment starts here.
inline bool enter(Env* env, const void* return_address) {
    if (__atomic_load_n(&g.pending, __ATOMIC_RELAXED) != nullptr) {
        drain_pending(env);
    }
    return from_app(return_address);
}
inline void enter_any(Env* env) {
    if (__atomic_load_n(&g.pending, __ATOMIC_RELAXED) != nullptr) {
        drain_pending(env);
    }
}

// ---- the block and its array are one memory

// On Dalvik the application's pointer and Java saw the same bytes. Here a block and its array are two copies, kept
// alike by synchronizing them in chunks of kChunk bytes. Besides the block each one keeps a baseline: the bytes
// as of the last synchronization, in memory outside the low arena (it needs no low address). A chunk whose block
// differs from its baseline was written through the raw pointer since then, and that comparison is exact. At a
// synchronization point such a chunk is merged with the array's byte by byte: a byte the application wrote (it
// differs from its baseline) goes to the array, every other byte of the chunk takes the array's if Java changed it since
// the baseline; where both wrote the same byte, the application's wins. Any other chunk takes the bytes Java changed.
// The block never takes a byte Java did not change, so a raw write by another thread to such a byte, racing the
// synchronization, survives it and goes to the array at the next one; the stores are byte stores, so not even the
// bytes next to a changed one are stored to. A chunk neither side touched costs two
// comparisons and a read, and writes nothing to the array or the block.
// A synchronization of a range with a chunk the application wrote merges in the array's own memory, through the critical
// pair of the real array (sync_in_place): the stores are made into the live array, where a Java thread's write to any other
// byte is never touched, and no region copy is made. A range with no such chunk takes Java's bytes through region reads, in
// pieces of kPullPiece bytes, and makes no critical get. The one merge through region writes is a deferred end
// (drain_pending), which cannot wait for a critical get: one region write for each maximal run of the bytes the application
// wrote (no byte between two runs is written).
// The synchronization points: before a call that is passed the block, as an argument or as the receiver (the
// application's chunks go to the array), after that call, at the start of Get...Elements, Get...Critical and
// GetByteArrayRegion (the chunks of the range), before and after SetByteArrayRegion, and at a Release. A
// committing release (mode 0 or COMMIT) applies the same rule; JNI_ABORT discards the working copy: the block takes
// the array's bytes. A call that left an exception pending is not followed by a synchronization: almost no JNI
// function may run then. Only the record's length is trusted (capacity_of), never the length word of the Dalvik object,
// which the application can overwrite.
inline unsigned chunk_count(size_t bytes) { return static_cast<unsigned>((bytes + kChunk - 1) / kChunk); }

inline uint8_t* data_of(BlockHeader* header) { return array_of(header)->data; }
inline uint32_t chunk_offset(unsigned chunk) { return chunk * kChunk; }
inline uint32_t chunk_end(uint32_t capacity, unsigned chunk) {
    const uint64_t end = static_cast<uint64_t>(chunk + 1) * kChunk;
    return end < capacity ? static_cast<uint32_t>(end) : capacity;
}

// Whether the application wrote the chunk through the raw pointer since the last synchronization.
bool chunk_dirty(BlockHeader* header, uint32_t capacity, unsigned chunk) {
    const uint32_t offset = chunk_offset(chunk);
    return __builtin_memcmp(data_of(header) + offset, header->baseline + offset, chunk_end(capacity, chunk) - offset) != 0;
}

// Whether the application wrote any chunk that overlaps [start, end) through the raw pointer since the last synchronization.
bool range_dirty(BlockHeader* header, uint32_t start, uint32_t end) {
    const uint32_t capacity = capacity_of(header);
    if (end <= start || capacity == 0) {
        return false;
    }
    for (unsigned chunk = start / kChunk, last = (end - 1) / kChunk; chunk <= last; ++chunk) {
        if (chunk_dirty(header, capacity, chunk)) {
            return true;
        }
    }
    return false;
}

// The array's bytes (already in the baseline over [from, to)) reach the block where they differ from it.
void adopt_baseline(BlockHeader* header, uint32_t from, uint32_t to) {
    for (uint32_t at = from; at < to; at += kChunk) {
        const uint32_t length = to - at < kChunk ? to - at : kChunk;
        if (__builtin_memcmp(data_of(header) + at, header->baseline + at, length) != 0) {
            __builtin_memcpy(data_of(header) + at, header->baseline + at, length);
        }
    }
}

// The bytes are compared by words and stored one at a time: a store to the block (which other threads write through the
// raw pointer) or to the live array (which Java threads write) touches only a byte the rule has chosen, never its
// neighbours. The stores are relaxed atomics so that the compiler cannot merge neighbouring ones into a wider store.
//
// A byte of the live array (which Java threads write at any moment) or of the block (which other application threads write
// at any moment) that a decision rests on is loaded exactly once, into a local value, through a relaxed atomic load, which
// the compiler can neither repeat nor split; that one value is what is compared, stored and written to the baseline, so the
// copies cannot differ for a byte the other side never wrote (a second load that saw a write in between would leave block
// != baseline, and the next synchronization would take it for an application write and put the old value back into the
// array; or leave the baseline different from what the array was given). A word is four such loads. A pass that compares a
// span with memcmp to skip it (take_java, merge_bytes) decides only to look closer: the bytes it skips are equal at that
// moment, and what changes afterwards is taken at the next synchronization point. The window between the load of a byte
// and the store it leads to is the stated limit for a byte both sides write.
inline void store_byte(uint8_t* at, uint8_t value) { __atomic_store_n(at, value, __ATOMIC_RELAXED); }
inline uint8_t load_byte(const uint8_t* at) { return __atomic_load_n(at, __ATOMIC_RELAXED); }
// Four bytes at `at`, each loaded once into `bytes`, and the word they make up.
inline uint32_t load_word(const uint8_t* at, uint8_t (&bytes)[4]) {
    for (uint32_t m = 0; m < 4; ++m) {
        bytes[m] = load_byte(at + m);
    }
    uint32_t word;
    __builtin_memcpy(&word, bytes, 4);
    return word;
}
// Whether no byte of `a` equals the byte of `b` at its position (x has no zero byte).
inline bool all_bytes_differ(uint32_t a, uint32_t b) {
    const uint32_t x = a ^ b;
    return ((x - 0x01010101u) & ~x & 0x80808080u) == 0;
}

// The array's bytes `java` become the baseline, and the block takes the ones Java changed since the baseline, unless the
// application has written that byte meanwhile (block != baseline): a raw write by another thread to a byte Java did not
// write survives a synchronization that races it, and the next one pushes it; where both sides wrote, the application's
// byte stays (the baseline takes Java's, so the block still differs from it). Words that Java left alone are skipped.
inline void take_java_byte(uint8_t* block, uint8_t* baseline, uint8_t java, uint32_t at) {
    const uint8_t old = baseline[at];
    if (java == old) {
        return;
    }
    baseline[at] = java;
    if (load_byte(block + at) == old) {
        store_byte(block + at, java);
    }
}
void take_java_words(uint8_t* block, uint8_t* baseline, const uint8_t* java, uint32_t n) {
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint8_t seen[4];  // Java's four bytes, each loaded once: what the decision rests on is what is taken
        const uint32_t j = load_word(java + i, seen);
        uint32_t k;
        __builtin_memcpy(&k, baseline + i, 4);
        if (j == k) {
            continue;
        }
        uint8_t mine[4];
        const uint32_t b = load_word(block + i, mine);
        if (b == k && all_bytes_differ(j, k)) {  // Java changed all four and the application none: four byte stores
            for (uint32_t m = 0; m < 4; ++m) {
                store_byte(block + i + m, seen[m]);
                baseline[i + m] = seen[m];
            }
            continue;
        }
        for (uint32_t m = 0; m < 4; ++m) {
            take_java_byte(block, baseline, seen[m], i + m);
        }
    }
    for (; i < n; ++i) {
        take_java_byte(block, baseline, load_byte(java + i), i);
    }
}
// (libc's memcmp skips what Java left alone, kScanBlock bytes at a time: most of a run is that)
void take_java(uint8_t* block, uint8_t* baseline, const uint8_t* java, uint32_t n) {
    uint32_t i = 0;
    for (; i + kScanBlock <= n; i += kScanBlock) {
        if (__builtin_memcmp(java + i, baseline + i, kScanBlock) != 0) {
            take_java_words(block + i, baseline + i, java + i, kScanBlock);
        }
    }
    if (i < n) {
        take_java_words(block + i, baseline + i, java + i, n - i);
    }
}

// The byte-by-byte rule over n bytes: a byte the application wrote (block != baseline) replaces the array's `java`
// byte, unless that is the same already. Every byte that changes is reported to `sink` as changed(offset, count), the
// offset counted from `base`, in ascending order; a word whose four bytes all change is one report. Words are compared at
// a time and bytes stored one at a time: where the application wrote all four bytes of a word and Java none, the four are
// stored; any other word is decided byte by byte, so a byte the rule leaves alone is never stored to.
//
// A byte decided on as the application's becomes the baseline with the value the decision used (when the sink says the
// stores reach the array: Sink::kAdvance), so that the pull that follows (take_java) cannot read the merge's own store as a
// Java change: were the application to write the old value back in between, the block would hold it, differ from the
// baseline and push it at the next point, instead of taking the merge's value and losing the application's last write. A
// sink whose stores are made later (RunWriter) advances the baseline itself, once the write was accepted.
template <class Sink>
void merge_words(uint8_t* java, const uint8_t* block, uint8_t* baseline, uint32_t n, uint32_t base, Sink& sink) {
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint8_t mine[4];  // the application's four bytes, each loaded once
        const uint32_t b = load_word(block + i, mine);
        uint32_t k;
        __builtin_memcpy(&k, baseline + i, 4);
        if (b == k) {
            continue;  // the application wrote none of the four: the array's bytes stay
        }
        uint8_t seen[4];
        const uint32_t j = load_word(java + i, seen);
        if (j == k && all_bytes_differ(b, k)) {
            for (uint32_t m = 0; m < 4; ++m) {
                store_byte(java + i + m, mine[m]);  // the application wrote all four and Java none
                if (Sink::kAdvance) {
                    baseline[i + m] = mine[m];
                }
            }
            sink.changed(base + i, 4);
            continue;
        }
        for (uint32_t m = 0; m < 4; ++m) {
            const uint8_t written = mine[m];
            if (written != baseline[i + m]) {
                if (load_byte(java + i + m) != written) {
                    store_byte(java + i + m, written);
                    sink.changed(base + i + m, 1);
                }
                if (Sink::kAdvance) {
                    baseline[i + m] = written;
                }
            }
        }
    }
    for (; i < n; ++i) {
        const uint8_t written = load_byte(block + i);
        if (written != baseline[i]) {
            if (load_byte(java + i) != written) {
                store_byte(java + i, written);
                sink.changed(base + i, 1);
            }
            if (Sink::kAdvance) {
                baseline[i] = written;
            }
        }
    }
}
// (libc's memcmp skips what the application did not write, kScanBlock bytes at a time: most of a run is that)
template <class Sink>
void merge_bytes(uint8_t* java, const uint8_t* block, uint8_t* baseline, uint32_t n, Sink& sink) {
    uint32_t i = 0;
    for (; i + kScanBlock <= n; i += kScanBlock) {
        if (__builtin_memcmp(block + i, baseline + i, kScanBlock) != 0) {
            merge_words(java + i, block + i, baseline + i, kScanBlock, i, sink);
        }
    }
    if (i < n) {
        merge_words(java + i, block + i, baseline + i, n - i, i, sink);
    }
}

// The sink of the merge in the live array: whether any byte changed. The stores are made into the array's own memory, so the
// baseline advances at each decision.
struct AnyChange {
    static constexpr bool kAdvance = true;
    bool any = false;
    void changed(uint32_t, uint32_t) { any = true; }
};

// The sink of the merge in a buffer: the array gets exactly the bytes that changed, one region write for each maximal run
// of adjacent changed bytes, and never a byte between two runs. (A Java thread's write to a byte the application did not
// write, landing between the read of the run and these writes, is not undone.) `merged` holds the new bytes. The baseline
// of a run advances to those bytes only once the runtime has accepted the write: a refused write (it leaves an exception,
// which is taken out) leaves the baseline as it was, so that the bytes are still the application's at the next point.
struct RunWriter {
    static constexpr bool kAdvance = false;
    Env* env;
    jobject array;
    uint32_t from;  // the array offset of the first byte of the run
    const uint8_t* merged;
    uint8_t* baseline;  // the baseline over the same bytes
    uint32_t start;
    uint32_t end;
    bool open;
    RunWriter(Env* e, jobject a, uint32_t f, const uint8_t* m, uint8_t* b)
        : env(e), array(a), from(f), merged(m), baseline(b), start(0), end(0), open(false) {}
    void changed(uint32_t at, uint32_t count) {
        if (open && at == end) {
            end += count;
            return;
        }
        flush();
        start = at;
        end = at + count;
        open = true;
    }
    void flush() {
        if (!open) {
            return;
        }
        original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
            env, array, static_cast<jsize>(from + start), static_cast<jsize>(end - start),
            reinterpret_cast<const jbyte*>(merged + start));
        open = false;
        if (original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env) != 0) {
            original<void (*)(Env*)>(slot::ExceptionClear)(env);  // refused: the baseline stays, the bytes go at the next point
            return;
        }
        __builtin_memcpy(baseline + start, merged + start, end - start);
    }
};

// The chunks first..last (all written by the application): the array's bytes of the run are read; what Java changed in
// them is taken first (take_java, on the snapshot, before the application's bytes are decided); then a byte the application
// wrote (block != baseline) replaces the array's, and the bytes that changed go back to the array, each maximal run of them
// in one region write, after which the baseline of that run takes them. The array is not written where the application's
// bytes are the array's already, and never at a byte between two such runs. A run longer than the scratch buffer reads into
// memory the platform gives; where there is none, the run is merged chunk by chunk.
void merge_run(Env* env, BlockHeader* header, jobject array, unsigned first, unsigned last) {
    const uint32_t from = chunk_offset(first), to = chunk_end(capacity_of(header), last), n = to - from;
    uint8_t* java = g.scratch;
    bool heap = false;
    if (n > kPullPiece) {
        java = static_cast<uint8_t*>(g.platform.alloc_baseline(n));
        heap = java != nullptr;
        if (!heap) {
            for (unsigned c = first; c <= last; ++c) {
                merge_run(env, header, array, c, c);
            }
            return;
        }
    }
    original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
        env, array, static_cast<jsize>(from), static_cast<jsize>(n), reinterpret_cast<jbyte*>(java));
    uint8_t* block = data_of(header) + from;
    uint8_t* baseline = header->baseline + from;
    take_java(block, baseline, java, n);
    RunWriter writer(env, array, from, java, baseline);
    merge_bytes(java, block, baseline, n, writer);
    writer.flush();
    if (heap) {
        g.platform.free_baseline(java);
    }
}

// The array's bytes over [from, to) reach the baseline and, where Java changed them, the block (take_java), a piece of
// kPullPiece bytes at a time.
//
// A region copy is a snapshot: Java threads write the array, not the copy, so what is decided on the copy is what is
// taken from it, and a write after the copy is taken at the next synchronization point.
//
// The scratch: the bytes read from the array (a pull's pieces, a merge of up to kPullPiece bytes) are read into one static
// buffer, not onto the stack, so that a thread with a small stack can call any wrapper. Every synchronization runs under
// the mutex, and none runs inside another, so the buffer is the running one's alone.
void pull_range(Env* env, BlockHeader* header, jobject array, uint32_t from, uint32_t to) {
    uint8_t* piece = g.scratch;
    for (uint32_t at = from; at < to; at += kPullPiece) {
        const uint32_t n = to - at < kPullPiece ? to - at : kPullPiece;
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            env, array, static_cast<jsize>(at), static_cast<jsize>(n), reinterpret_cast<jbyte*>(piece));
        take_java(data_of(header) + at, header->baseline + at, piece, n);
    }
}

// The chunks the application wrote, merged into the array through region calls (merge_run: one write for each run of the
// bytes it wrote). This is the push of the one path that cannot go through the critical pair (drain_pending, below); every
// other synchronization uses sync_range. Nothing is read back.
void push_dirty_regions(Env* env, BlockHeader* header, jobject array) {
    const uint32_t capacity = capacity_of(header);
    if (capacity == 0 || array == nullptr) {
        return;
    }
    const unsigned last = chunk_count(capacity) - 1;
    for (unsigned chunk = 0; chunk <= last; ++chunk) {
        if (!chunk_dirty(header, capacity, chunk)) {
            continue;
        }
        unsigned run_end = chunk;
        while (run_end < last && chunk_dirty(header, capacity, run_end + 1)) {
            ++run_end;
        }
        merge_run(env, header, array, chunk, run_end);
        chunk = run_end;
    }
}

// A JNI_ABORT: the block is given the array's bytes back, whatever the application wrote.
void abort_block(Env* env, BlockHeader* header, jobject array) {
    const uint32_t capacity = capacity_of(header);
    if (capacity == 0 || array == nullptr) {
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
        env, array, 0, static_cast<jsize>(capacity), reinterpret_cast<jbyte*>(header->baseline));
    adopt_baseline(header, 0, capacity);
}

// The same rules through the real array's critical pair, the only JNI calls that CheckJNI allows inside a critical
// region (the application may be in one for another array when it asks for this block's critical pointer). The pair
// is released with mode 0 only when a chunk went to the array.
//
// The runtime's critical get waits while a collection's thread flip is pending unless the thread's own flip count is
// raised already, and the flip waits for every open region, so a thread must never make a critical get while it holds the
// mutex: another thread inside a region may be waiting for the mutex. That count rises only for a movable array or an
// uncompressed movable string, so a thread inside the application's region on a large array, a non-moving one or a
// compressed string has a count of zero, and a get the layer would call "nested" is the runtime's outermost. The caller
// therefore makes every get (first_critical, no mutex held, on a reference that is valid outside it) and passes the
// pointer; the hold that uses it revalidates the block.
//
// The longest time a region the layer opened stayed open is kept (Counters::longest_region_us): from the get's return to
// the release's, which includes the wait for the mutex behind the merges of other threads.
struct Pair {
    void* pointer;    // the array's bytes, null: no region is open
    uint64_t opened;  // when the get returned, nanoseconds on the monotonic clock
};
uint64_t now_ns() { return g.platform.now_ns != nullptr ? g.platform.now_ns() : 0; }
Pair first_critical(Env* env, const BlockHeader* header, jobject array) {
    if (capacity_of(header) == 0 || array == nullptr) {
        return Pair{nullptr, 0};
    }
    void* pointer = original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(env, array, nullptr);
    return Pair{pointer, pointer != nullptr ? now_ns() : 0};
}
void release_critical_pair(Env* env, jobject array, const Pair& pair, jint mode) {
    original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(env, array, pair.pointer, mode);
    if (g.platform.now_ns == nullptr) {
        return;
    }
    const uint32_t us = static_cast<uint32_t>((now_ns() - pair.opened) / 1000);
    uint32_t longest = __atomic_load_n(&g.counters.longest_region_us, __ATOMIC_RELAXED);
    while (us > longest && !__atomic_compare_exchange_n(&g.counters.longest_region_us, &longest, us, true, __ATOMIC_RELAXED,
                                                        __ATOMIC_RELAXED)) {
    }
}
bool block_is_valid(const BlockHeader* header) { return block_exists(header); }
// [start, end) of the block synchronized with its array in place, through the critical pair: a chunk the application wrote
// is merged into the array's own bytes, byte stores of exactly the bytes it wrote (a Java write to any other byte is never
// touched, and no region copy is made); every other chunk in the range takes the array's bytes when `pull` is set. The
// caller holds the mutex and has made the get (`pair`, from first_critical) before the hold, so that this makes no
// critical get. Between the get and the release the layer makes no runtime call: the release is the only one.
void sync_in_place(Env* env, BlockHeader* header, jobject array, const Pair& pair, uint32_t start, uint32_t end, bool pull) {
    const uint32_t capacity = capacity_of(header);
    if (capacity == 0 || array == nullptr || end <= start) {
        return;
    }
    if (pair.pointer == nullptr) {
        return;
    }
    void* pinned = pair.pointer;
    if (!block_is_valid(header)) {  // revalidated under this hold of the mutex: nothing to merge into
        release_critical_pair(env, array, pair, kReleaseAbort);
        return;
    }
    bool wrote = false;
    for (unsigned c = start / kChunk, last = (end - 1) / kChunk; c <= last; ++c) {
        const uint32_t offset = chunk_offset(c), length = chunk_end(capacity, c) - offset;
        if (chunk_dirty(header, capacity, c)) {
            uint8_t* java = static_cast<uint8_t*>(pinned) + offset;  // the array's bytes: the application's replace them
            const uint8_t* block = data_of(header) + offset;
            uint8_t* baseline = header->baseline + offset;
            AnyChange change;
            merge_bytes(java, block, baseline, length, change);
            wrote = change.any || wrote;
            take_java(data_of(header) + offset, header->baseline + offset, java, length);
        } else if (pull) {
            const uint32_t from = offset < start ? start : offset;
            const uint32_t to = offset + length > end ? end : offset + length;
            take_java(data_of(header) + from, header->baseline + from, static_cast<const uint8_t*>(pinned) + from, to - from);
        }
    }
    release_critical_pair(env, array, pair, wrote ? 0 : kReleaseAbort);
}
void abort_critical(Env* env, BlockHeader* header, jobject array, const Pair& pair) {
    const uint32_t capacity = capacity_of(header);
    if (capacity == 0 || array == nullptr) {
        return;
    }
    if (pair.pointer == nullptr) {
        return;
    }
    if (block_is_valid(header)) {
        // One read of the array, into the baseline; the block takes the baseline, not a second read of the array.
        __builtin_memcpy(header->baseline, pair.pointer, capacity);
        adopt_baseline(header, 0, capacity);
    }
    release_critical_pair(env, array, pair, kReleaseAbort);
}

// ---- ended blocks
//
// A block whose references have all ended (a native method returned, a frame was popped, DeleteLocalRef) is not freed.
// Its array may be held by Java still, and the application may keep the pointer: on Dalvik the object stayed where it
// was for as long as it was reachable. So the block stays at its address with its bytes and its baseline until the array
// is collected. At the end, what the application wrote through the pointer goes to the array, and the block's strong
// reference gives way to a weak one. The ended blocks are in one list, in the order they ended, and in an index by the
// array's identity hash. The same array obtained again gets its block back, wherever the pointer was kept, so Java's
// later writes and the application's later raw writes meet in it (point 3: the merge of the synchronization).
//
// How the layer learns of a collection: IsSameObject(weak, NULL), the JNI specification's test for a cleared
// reference. A sentinel, a weak reference to an array nothing holds, says when to look: every collection clears it, since
// it is younger than the last one; the first creation of a block after that starts testing the ended blocks, kSweepStep
// at each creation. The tests run only at the creation of a block, never at a native return, so a call that makes no
// block pays nothing. Room is made on demand: a full sweep, then a Runtime.gc() (at most every kCollectIntervalMs), then
// a full sweep again. The sentinel is a hint: that full sweep never reads it.
//
// The index, the list, the cursor and the frees are under one process-wide lock, the mutex, whose rule is below (what may
// run under the mutex). Runtime.gc() and the sentinel's array are made outside it. Nothing here runs with an exception
// pending, and the critical wrappers never start it, so no call here is made inside a critical region.

void lock_ended() {
    if (g.platform.mutex_lock != nullptr) {
        g.platform.mutex_lock();
        return;
    }
    while (__atomic_exchange_n(&g.ended_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
}
void unlock_ended() {
    if (g.platform.mutex_unlock != nullptr) {
        g.platform.mutex_unlock();
        return;
    }
    __atomic_store_n(&g.ended_lock, 0u, __ATOMIC_RELEASE);
}
struct EndedLock {
    EndedLock() { lock_ended(); }
    ~EndedLock() { unlock_ended(); }
    EndedLock(const EndedLock&) = delete;
    EndedLock& operator=(const EndedLock&) = delete;
};

// Synchronizes [start, end) of the block with its array, for the calling thread, which must not hold the mutex; `array` is a
// reference of the caller's own to the real array. `pull` also takes Java's bytes into the chunks the application did not
// write (the part a call needs before it runs is the push alone).
//   - First hold: if no chunk in the range was written through the pointer, the range is read through region calls (with
//     `pull`) in this hold, and that is all. A clean block's call never makes a critical get.
//   - Otherwise the first critical get of the array is made with no lock held (see first_critical), and the second hold
//     merges the chunks in place (sync_in_place) and releases the pair. A chunk the first hold saw dirty that another
//     thread has synchronized meanwhile is clean in the second, and costs a pair and nothing more.
// Where the get returns nothing, the chunks stay as they are in the block for the next synchronization point.
void sync_range(Env* env, BlockHeader* header, jobject array, uint32_t start, uint32_t end, bool pull) {
    if (end <= start || capacity_of(header) == 0 || array == nullptr) {
        return;
    }
    {
        EndedLock first_hold;
        if (!range_dirty(header, start, end)) {
            if (pull) {
                pull_range(env, header, array, start, end);
            }
            return;
        }
    }
    const Pair pair = first_critical(env, header, array);
    if (pair.pointer == nullptr) {
        return;
    }
    EndedLock second_hold;
    sync_in_place(env, header, array, pair, start, end, pull);
}

void publish_ended() {
    __atomic_store_n(&g.counters.shadows_ended, g.ended_count, __ATOMIC_RELAXED);
    __atomic_store_n(&g.counters.ended_bytes, static_cast<uint32_t>(g.ended_bytes), __ATOMIC_RELAXED);
}

// Under the lock.
void ended_add(BlockHeader* block) {
    block->list_prev = g.ended_head.list_prev;
    block->list_next = &g.ended_head;
    g.ended_head.list_prev->list_next = block;
    g.ended_head.list_prev = block;
    BlockHeader** chain = &g.ended_bucket[block->hash % kEndedBuckets];
    block->bucket_next = *chain;  // the newest first
    *chain = block;
    ++g.ended_count;
    g.ended_bytes += capacity_of(block);
    publish_ended();
}
void ended_remove(BlockHeader* block) {
    if (g.sweep_cursor == block) {
        g.sweep_cursor = block->list_next;
    }
    block->list_prev->list_next = block->list_next;
    block->list_next->list_prev = block->list_prev;
    BlockHeader** chain = &g.ended_bucket[block->hash % kEndedBuckets];
    while (*chain != block) {
        chain = &(*chain)->bucket_next;
    }
    *chain = block->bucket_next;
    --g.ended_count;
    g.ended_bytes -= capacity_of(block);
    publish_ended();
}

// Every block holds one reservation against kMaxExisting from its creation to its free.
void release_reservation() { __atomic_fetch_sub(&g.existing, 1u, __ATOMIC_ACQ_REL); }

// The array of an ended block is gone: the block, its baseline and its weak reference go.
void reclaim_ended(Env* env, BlockHeader* block) {
    ended_remove(block);
    record_clear(block);  // before the memory goes back to the arena: from here no address is this block
    mark(block, kBlockDead);
    original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, block->weak);
    if (block->baseline != nullptr) {
        g.platform.free_baseline(block->baseline);
    }
    g.shadow_arena.release(block);
    release_reservation();
    __atomic_fetch_add(&g.counters.blocks_collected, 1u, __ATOMIC_RELAXED);
}

bool cleared(Env* env, jobject weak) {
    return original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(env, weak, nullptr) != 0;
}

// Under the lock: an ended block that a pin kept (its strong reference) with no pin left gives way to a weak reference,
// and then waits for a collection like any other. Where no weak reference can be had it stays as it is.
void weaken(Env* env, BlockHeader* block) {
    jobject weak = original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(env, block->global);
    if (weak == nullptr) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
        return;
    }
    original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, block->global);
    block->global = nullptr;
    block->weak = weak;
}

// Tests the ended blocks from the cursor, `limit` of them (0: all, from the first); reclaims the ones whose arrays are
// gone. The sweep ends where the list does.
void sweep(Env* env, unsigned limit) {
    BlockHeader* node = limit == 0 ? g.ended_head.list_next : g.sweep_cursor;
    unsigned tested = 0;
    while (node != &g.ended_head && (limit == 0 || tested < limit)) {
        BlockHeader* next = node->list_next;
        ++tested;
        if (node->global != nullptr) {
            // Kept by a pin: its array cannot be collected. Once the last pin is gone it gives way to a weak reference
            // (the release that took it could not make one: a critical release, or an exception pending).
            if (__atomic_load_n(&node->pins, __ATOMIC_RELAXED) == 0) {
                weaken(env, node);
            }
        } else if (cleared(env, node->weak)) {
            reclaim_ended(env, node);
        }
        node = next;
    }
    g.sweep_cursor = node;
    if (node == &g.ended_head) {
        g.sweeping = false;
        g.sweep_cursor = g.ended_head.list_next;
    }
}

// The array of a new sentinel: an empty byte[] nothing holds but this local reference. Its allocation can wait for a
// collection, so it is made outside the mutex (make), by the thread that is about to take it, and the renewal under the
// mutex only makes the weak reference from it. The reference goes with the Spare: a renewal that used it has deleted it.
struct Spare {
    Env* env;
    jobject array;
    bool failed;  // the allocation failed once: this thread's renewals go without
    explicit Spare(Env* e) : env(e), array(nullptr), failed(false) {}
    ~Spare() {
        if (array != nullptr) {
            original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, array);
        }
    }
    Spare(const Spare&) = delete;
    Spare& operator=(const Spare&) = delete;
    // Not under the mutex; with no exception pending (every caller has checked).
    void make() {
        if (array != nullptr || failed) {
            return;
        }
        array = original<jobject (*)(Env*, jsize)>(slot::NewByteArray)(env, 0);
        if (array == nullptr) {
            original<void (*)(Env*)>(slot::ExceptionClear)(env);
            failed = true;
        }
    }
    // Whether a renewal that is due can go on: it has an array, or none can be had.
    bool ready() const { return array != nullptr || failed; }
};

// A collection has been seen (the sentinel was found cleared): counted once, by whichever sweep found it, and the
// sentinel renewed. A full sweep is due while a collection is past the last full sweep.
void renew_sentinel(Env* env, Spare& spare);
void note_collection(Env* env, Spare& spare) {
    ++g.gc_seen;
    renew_sentinel(env, spare);
}
// Under the mutex. Without an array in the spare (it could not be made) the sentinel stays unset: every creation then sees
// a collection, as it did when the allocation failed under the mutex.
void renew_sentinel(Env* env, Spare& spare) {
    if (g.sentinel != nullptr) {
        original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, g.sentinel);
        __atomic_store_n(&g.sentinel, static_cast<jobject>(nullptr), __ATOMIC_RELAXED);  // read without the mutex: sentinel_missing
    }
    if (spare.array == nullptr) {
        return;
    }
    __atomic_store_n(&g.sentinel, original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(env, spare.array), __ATOMIC_RELAXED);
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, spare.array);
    spare.array = nullptr;
}

// At the creation of a block, under the lock: whether a collection ran since the last look, and then the next
// kSweepStep ended blocks. True: a collection was seen and the sentinel needs an array that the spare does not have yet;
// nothing was changed, and the caller makes the array outside the mutex and comes back.
bool collection_step(Env* env, Spare& spare) {
    if (g.ended_count == 0) {
        return false;
    }
    if (!g.sweeping) {
        if (g.sentinel != nullptr && !cleared(env, g.sentinel)) {
            return false;
        }
        if (!spare.ready()) {
            return true;
        }
        note_collection(env, spare);
        g.sweeping = true;
        g.sweep_cursor = g.ended_head.list_next;
    }
    sweep(env, kSweepStep);
    return false;
}

// A full sweep is made for a creation that found no room. Under the lock. It is worth making when a collection ran that
// no full sweep has seen (the step sweep, which tests a few blocks at a time, counts the collection it finds but does not
// finish the list), or when a second has passed since the last one: the fallback that does not depend on the sentinel.
// Declined results in between do not repeat a sweep that found nothing.
bool full_sweep_due(Env* env, Spare& spare) {
    if (!g.full_swept || g.platform.now_ms == nullptr) {
        return true;
    }
    if (g.sentinel != nullptr && cleared(env, g.sentinel)) {
        note_collection(env, spare);
    }
    if (g.gc_seen != g.gc_full_seen) {
        return true;
    }
    return g.platform.now_ms() - g.full_sweep_ms >= kFullSweepIntervalMs;
}
// The sentinel is renewed before the list is tested: a collection that runs during the sweep clears the new one, and the
// next creation sweeps again.
void full_sweep(Env* env, Spare& spare) {
    if (g.sentinel != nullptr && cleared(env, g.sentinel)) {
        ++g.gc_seen;
    }
    g.gc_full_seen = g.gc_seen;
    renew_sentinel(env, spare);
    sweep(env, 0);
    g.sweeping = false;
    g.full_swept = true;
    g.full_sweep_ms = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
}

// System.identityHashCode and Runtime.gc(), looked up once (through the runtime's own table).
bool find_ids(Env* env) {
    if (g.system_class != nullptr) {  // a retry: the classes of the failed look go
        original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, g.system_class);
        g.system_class = nullptr;
    }
    if (g.runtime_class != nullptr) {
        original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, g.runtime_class);
        g.runtime_class = nullptr;
    }
    const auto find_class = original<jclass (*)(Env*, const char*)>(slot::FindClass);
    const auto global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef);
    const auto delete_local = original<void (*)(Env*, jobject)>(slot::DeleteLocalRef);
    const auto static_method = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticMethodID);
    const auto method = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetMethodID);
    jclass system = find_class(env, "java/lang/System");
    jclass runtime = system != nullptr ? find_class(env, "java/lang/Runtime") : nullptr;
    if (system != nullptr && runtime != nullptr) {
        g.system_class = static_cast<jclass>(global(env, system));
        g.runtime_class = static_cast<jclass>(global(env, runtime));
        g.hash_method = static_method(env, g.system_class, "identityHashCode", "(Ljava/lang/Object;)I");
        g.runtime_get = static_method(env, g.runtime_class, "getRuntime", "()Ljava/lang/Runtime;");
        g.runtime_gc = method(env, g.runtime_class, "gc", "()V");
    }
    if (system != nullptr) delete_local(env, system);
    if (runtime != nullptr) delete_local(env, runtime);
    const bool ok = g.system_class != nullptr && g.runtime_class != nullptr && g.hash_method != nullptr &&
                    g.runtime_get != nullptr && g.runtime_gc != nullptr;
    if (!ok) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
    }
    return ok;
}
// The classes and methods are looked up once, outside every lock of the layer (FindClass can run Java, and that code can
// enter the application's natives and this layer again): at the installation, before the first block can exist, and again
// by the first thread that needs them after a failed look, at most once in kIdsRetryMs. A thread that finds the lookup
// under way, or failed, has no identity hash to give: the result it asks a block for stays the real reference. Never a
// block without a hash, since a block without one could not be found again and the array would get a second.
bool ensure_ids(Env* env) {
    uint32_t state = __atomic_load_n(&g.ids_state, __ATOMIC_ACQUIRE);
    if (state == 1) {
        return true;
    }
    if (state == 3) {
        return false;
    }
    if (state == 2 && g.platform.now_ms != nullptr &&
        g.platform.now_ms() - __atomic_load_n(&g.ids_failed_ms, __ATOMIC_RELAXED) < kIdsRetryMs) {
        return false;
    }
    if (!__atomic_compare_exchange_n(&g.ids_state, &state, 3u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;  // another thread took the lookup
    }
    const bool found = find_ids(env);
    if (!found) {
        __atomic_store_n(&g.ids_failed_ms, g.platform.now_ms != nullptr ? g.platform.now_ms() : 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g.ids_state, found ? 1u : 2u, __ATOMIC_RELEASE);
    return found;
}

// The identity hash of the array `local` (a real reference). False where it cannot be had: the result then stays the real
// reference (no block is made that could not be found again).
bool identity_hash(Env* env, jobject local, uint32_t* hash) {
    if (!ensure_ids(env)) {
        return false;
    }
    jvalue argument;
    argument.j = 0;
    argument.l = local;
    const jint value = original<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(static_call_slot(kInt, kArray))(
        env, g.system_class, g.hash_method, &argument);
    if (!jni_usable(env)) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
        return false;
    }
    *hash = static_cast<uint32_t>(value);
    return true;
}

// The end of a block's last reference, under the lock: the strong reference gives way to a weak one (unless a pin keeps
// it) and the block waits in the index, so that no other thread can make a second block for the array in between. What
// the application wrote through the pointer has gone to the array (sync_in_place) in the same hold of the mutex.
void end_locked(Env* env, BlockHeader* header, Spare& spare) {
    if (__atomic_load_n(&header->pins, __ATOMIC_RELAXED) == 0) {
        jobject weak = original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(env, header->global);
        if (weak == nullptr) {
            // No weak reference to give: the block stays as it is, strong, and a later sweep tries again. It is not freed:
            // the application may hold its pointer, and the array may be reachable only through it.
            original<void (*)(Env*)>(slot::ExceptionClear)(env);
        } else {
            original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, header->global);
            header->global = nullptr;
            header->weak = weak;
        }
    }  // else a pointer from Get...Elements / Get...Critical was never released: the array stays reachable, the block valid
    mark(header, kBlockEnded);
    record_turn(header, kStateEnded);  // after the references, and in the same hold of the mutex as the index
    ended_add(header);
    if (g.sentinel == nullptr) {
        renew_sentinel(env, spare);
    }
    __atomic_fetch_sub(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g.counters.blocks_ended, 1u, __ATOMIC_RELAXED);
}

// With an exception pending none of that may run (NewWeakGlobalRef and SetByteArrayRegion are outside the functions
// CheckJNI allows then): the block waits for the next call without one, still strong.
// An end in progress, counted before its lock is taken: a thread that is about to look for the ended block of an array
// must not skip the index because the block has not been added yet.
struct Ending {
    Ending() { __atomic_fetch_add(&g.ending, 1u, __ATOMIC_ACQ_REL); }
    ~Ending() { __atomic_fetch_sub(&g.ending, 1u, __ATOMIC_ACQ_REL); }
    Ending(const Ending&) = delete;
    Ending& operator=(const Ending&) = delete;
};

// Whether the sentinel is missing, as a hint for the thread that is about to end a block: it makes the sentinel's array
// before it takes the mutex. (A racy read of a pointer that is never passed anywhere; a wrong answer costs one spare.)
bool sentinel_missing() { return __atomic_load_n(&g.sentinel, __ATOMIC_RELAXED) == nullptr; }

void end_block(Env* env, BlockHeader* header) {
    if (!jni_usable(env)) {
        queue_pending(header);
        return;
    }
    Spare spare(env);
    if (sentinel_missing()) {
        spare.make();
    }
    Ending ending;
    // A block the application did not write ends in one hold. One it wrote is pushed in place first: the first hold makes a
    // reference of its own, the first critical get is made with no lock held, and the second hold pushes, closes the pair and
    // ends the block (the end's weak reference and index entry come in one step with the push).
    jobject own = nullptr;
    uint32_t capacity = 0;
    {
        EndedLock first_hold;
        capacity = capacity_of(header);
        if (!range_dirty(header, 0, capacity)) {
            end_locked(env, header, spare);
            return;
        }
        own = original<jobject (*)(Env*, jobject)>(slot::NewLocalRef)(env, header->global);
    }
    const Pair pair = own != nullptr ? first_critical(env, header, own) : Pair{nullptr, 0};
    {
        EndedLock second_hold;
        if (pair.pointer != nullptr) {
            sync_in_place(env, header, own, pair, 0, capacity, false);
        }  // else the chunks stay in the block, which ends as it is (the next synchronization of the array pushes them)
        end_locked(env, header, spare);
    }
    if (own != nullptr) {
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, own);
    }
}

// The blocks that wait for an environment: with one, and no exception pending, their ends are finished. The exchange and
// the ends are one step for the other threads (a block taken from the list and not yet in the index would let a second
// thread make a second block for its array), so the pushes of the application's writes cannot wait for a critical get
// made before the hold: this is the one path that merges through region calls, with the exact per-run writes.
void drain_pending(Env* env) {
    if (!jni_usable(env)) {
        return;  // the next call without an exception finishes them
    }
    Spare spare(env);
    if (sentinel_missing()) {
        spare.make();
    }
    Ending ending;
    EndedLock lock;
    BlockHeader* list = __atomic_exchange_n(&g.pending, static_cast<BlockHeader*>(nullptr), __ATOMIC_ACQ_REL);
    while (list != nullptr) {
        BlockHeader* next = list->next;
        push_dirty_regions(env, list, list->global);
        end_locked(env, list, spare);
        __atomic_fetch_add(&g.counters.shadows_released, 1u, __ATOMIC_RELAXED);
        list = next;
    }
}

// The ended block for the array `local` (identity hash `hash`), turned live again with a new global reference for the thread of
// `env`, which holds its one reference; null if there is none. Two ended blocks can exist for one array (it was live on two
// threads): the newest is found first. `no_global` is set when the global reference could not be had. The record says live only
// when the block's references and its count are set.
BlockHeader* take_ended(Env* env, jobject local, uint32_t hash, size_t bytes, bool* no_global) {
    for (BlockHeader* block = g.ended_bucket[hash % kEndedBuckets]; block != nullptr; block = block->bucket_next) {
        if (block->hash != hash || capacity_of(block) != bytes ||
            original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(
                env, local, block->global != nullptr ? block->global : block->weak) == 0) {
            continue;
        }
        if (block->global != nullptr) {  // kept by a pin: the strong reference is its own again
            ended_remove(block);
            block->refs = 1;
            mark(block, kBlockLive);
            record_turn(block, kStateLive);
            return block;
        }
        jobject global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(env, local);
        if (global == nullptr) {
            *no_global = true;
            return nullptr;
        }
        ended_remove(block);
        original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, block->weak);
        block->weak = nullptr;
        block->global = global;
        block->refs = 1;
        mark(block, kBlockLive);
        record_turn(block, kStateLive);
        return block;
    }
    return nullptr;
}

// Asks the runtime to collect, at most every kCollectIntervalMs: true if it did. The ended blocks only wait for a
// collection, and a pause once in a while is better than a result that stays the real reference.
bool ask_for_collection(Env* env) {
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (g.platform.now_ms != nullptr && __atomic_load_n(&g.collect_asked, __ATOMIC_RELAXED) != 0 &&
        now - __atomic_load_n(&g.collect_ms, __ATOMIC_RELAXED) < kCollectIntervalMs) {
        return false;
    }
    if (!ensure_ids(env)) {
        return false;
    }
    __atomic_store_n(&g.collect_ms, now, __ATOMIC_RELAXED);
    __atomic_store_n(&g.collect_asked, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g.counters.collections_asked, 1u, __ATOMIC_RELAXED);
    if (g.platform.log != nullptr && __atomic_exchange_n(&g.collect_logged, 1u, __ATOMIC_RELAXED) == 0) {
        g.platform.log("a byte[] block did not fit: the runtime was asked to collect; ended blocks and bytes waiting:",
                       static_cast<long>(__atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED)),
                       static_cast<long>(__atomic_load_n(&g.counters.ended_bytes, __ATOMIC_RELAXED)));
    }
    jobject runtime = original<jobject (*)(Env*, jclass, jmethodID, const jvalue*)>(static_call_slot(kObject, kArray))(
        env, g.runtime_class, g.runtime_get, nullptr);
    if (runtime != nullptr) {
        original<void (*)(Env*, jobject, jmethodID, const jvalue*)>(call_slot(kVoid, kArray))(env, runtime, g.runtime_gc,
                                                                                               nullptr);
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, runtime);
    }
    if (!jni_usable(env)) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
    }
    return true;
}

// The diagnosis lines of the log (the sessions of real games read them): the first result that stayed the real
// reference, with the blocks and bytes the thread held, and the most blocks any thread has held, at most every
// kPeakLogMs while it grows.
constexpr uint32_t kPeakLogMs = 30000;

void note_declined(const ThreadState* state, const char* why) {
    __atomic_fetch_add(&g.counters.shadows_declined, 1u, __ATOMIC_RELAXED);
    if (g.platform.log != nullptr && __atomic_exchange_n(&g.declined_logged, 1u, __ATOMIC_RELAXED) == 0) {
        g.platform.log(why, state != nullptr ? static_cast<long>(state->count) : 0,
                       state != nullptr ? static_cast<long>(state->bytes) : 0);
        // The ended blocks that still wait for a collection are a reason a result may not fit.
        g.platform.log("ended byte[] blocks waiting for their arrays' collection when the first result was not made a "
                       "block; blocks and bytes:",
                       static_cast<long>(__atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED)),
                       static_cast<long>(__atomic_load_n(&g.counters.ended_bytes, __ATOMIC_RELAXED)));
    }
}

// The longest a critical region the layer opened has stayed open, logged when it has grown, at most every kPeakLogMs (the
// next creation after that logs the one that was kept, as for the peak).
void note_longest_region() {
    const uint32_t longest = __atomic_load_n(&g.counters.longest_region_us, __ATOMIC_RELAXED);
    uint32_t logged = __atomic_load_n(&g.region_logged, __ATOMIC_RELAXED);
    if (g.platform.log == nullptr || longest <= logged) {
        return;
    }
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (logged != 0 && g.platform.now_ms != nullptr &&
        now - __atomic_load_n(&g.region_logged_ms, __ATOMIC_RELAXED) < kPeakLogMs) {
        return;
    }
    if (__atomic_compare_exchange_n(&g.region_logged, &logged, longest, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g.region_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("the longest a critical region of a synchronization stayed open, microseconds:", longest, 0);
    }
}

void note_peak(uint32_t blocks) {
    note_longest_region();
    uint32_t peak = __atomic_load_n(&g.counters.peak_thread_blocks, __ATOMIC_RELAXED);
    while (blocks > peak && !__atomic_compare_exchange_n(&g.counters.peak_thread_blocks, &peak, blocks, true,
                                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (blocks > peak) {
        peak = blocks;
    }
    uint32_t logged = __atomic_load_n(&g.peak_logged, __ATOMIC_RELAXED);
    if (g.platform.log == nullptr || peak == logged) {
        return;
    }
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (logged != 0 && g.platform.now_ms != nullptr &&
        now - __atomic_load_n(&g.peak_logged_ms, __ATOMIC_RELAXED) < kPeakLogMs) {
        return;
    }
    if (__atomic_compare_exchange_n(&g.peak_logged, &logged, peak, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g.peak_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("most live byte[] blocks one thread has held; blocks live in the process:", peak,
                       static_cast<long>(live_shadows()));
    }
}

// The most recent blocks of a thread that are looked at for the same array before a new block is made.
constexpr unsigned kReuseWindow = 8;

// The memory of one block, and its reservation against kMaxExisting: the blocks that exist, in whatever state (live,
// waiting for an environment, ended, pinned), since every one of them ends with a weak global reference and an end can
// free nothing it has exposed. Null with a reason.
struct Room {
    uint8_t* raw;
    uint8_t* baseline;
    const char* why;
};
Room allocate_block(size_t need, size_t bytes) {
    Room room{nullptr, nullptr, nullptr};
    if (__atomic_add_fetch(&g.existing, 1u, __ATOMIC_ACQ_REL) > kMaxExisting) {
        release_reservation();
        room.why = "the first byte[] result stayed a real reference (too many blocks); blocks and bytes held:";
        return room;
    }
    room.raw = static_cast<uint8_t*>(g.shadow_arena.allocate(need));
    if (room.raw == nullptr) {
        release_reservation();
        room.why = "the first byte[] result stayed a real reference (no room in the region); blocks and bytes held:";
        return room;
    }
    if (bytes > 0) {
        room.baseline = static_cast<uint8_t*>(g.platform.alloc_baseline(bytes));
        if (room.baseline == nullptr) {
            g.shadow_arena.release(room.raw);
            release_reservation();
            room.raw = nullptr;
            room.why = "the first byte[] result stayed a real reference (no memory for the baseline); blocks and bytes held:";
        }
    }
    return room;
}

// A shadow for the real local reference `local`, which it replaces: the block holds a global reference to the
// array, the local reference is deleted. An array the thread got a block for among its most recent ones is the same
// block again (counted once more, one DeleteLocalRef for each); so is an array whose block has ended and is not
// collected: the application gets the block it may still hold the pointer of. Null when the thread already has the
// limit or the region is full: the caller keeps the real reference, as it would without the layer.
jobject make_shadow(Env* env, jobject local) {
    ThreadState* state = thread();
    if (state == nullptr) {
        note_declined(state, "the first byte[] result stayed a real reference (no thread state); blocks and bytes held:");
        return nullptr;
    }
    const jsize length = original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(env, local);
    if (length < 0) {
        return nullptr;
    }
    const size_t bytes = static_cast<size_t>(length);
    if (state->count >= kMaxBlocks) {
        note_declined(state, "the first byte[] result stayed a real reference (thread limit); blocks and bytes held:");
        return nullptr;
    }
    for (unsigned i = 0; i < kReuseWindow && i < state->count; ++i) {
        BlockHeader* known = state->blocks[state->count - 1 - i];
        if (capacity_of(known) == bytes &&
            original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(env, local, known->global) != 0) {
            ++known->refs;
            state->blocks[state->count++] = known;
            state->env = env;
            sync_range(env, known, local, 0, bytes, true);  // what Java wrote meanwhile shows, and what the application wrote reaches the array
            original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
            return array_of(known);
        }
    }
    if (__atomic_load_n(&g.pending, __ATOMIC_RELAXED) != nullptr) {
        drain_pending(env);  // blocks that waited for an environment: their ends come before this lookup
    }
    // The ended blocks: a sweep step if a collection ran, then the array's own block if it ended and is not collected.
    // Where the identity hash cannot be had the result stays the real reference: a block made without one could not be
    // found again, and the array would get a second block.
    uint32_t hash = 0;
    if (!identity_hash(env, local, &hash)) {
        note_declined(state, "the first byte[] result stayed a real reference (no identity hash); blocks and bytes held:");
        return nullptr;
    }
    if (__atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED) != 0 || __atomic_load_n(&g.ending, __ATOMIC_ACQUIRE) != 0) {
        BlockHeader* again = nullptr;
        bool no_global = false;
        Spare spare(env);
        for (bool want_array = true; want_array;) {
            {
                EndedLock lock;
                want_array = collection_step(env, spare);
                if (!want_array && g.ended_count != 0) {
                    again = take_ended(env, local, hash, bytes, &no_global);
                }
            }
            if (want_array) {
                spare.make();  // a collection was seen: the sentinel's new array is made outside the mutex
            }
        }
        if (no_global) {
            note_declined(state, "the first byte[] result stayed a real reference (no global reference); blocks and bytes held:");
            return nullptr;
        }
        if (again != nullptr) {
            // Java's writes since it ended and the application's raw writes since meet here. Any other thread that uses a
            // kept reference to the block synchronizes it on its own, so neither needs the other to have gone first.
            sync_range(env, again, local, 0, capacity_of(again), true);
            state->blocks[state->count++] = again;
            state->env = env;
            state->bytes += static_cast<uint32_t>(bytes);
            original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
            __atomic_fetch_add(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g.counters.blocks_revived, 1u, __ATOMIC_RELAXED);
            note_peak(state->count);
            return array_of(again);
        }
    }
    const size_t need = kBlockHead + offsetof(DalvikArray, data) + bytes + kBlockTail;
    Room room = allocate_block(need, bytes);
    if (room.raw == nullptr && __atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED) != 0) {
        // Ended blocks hold the room: those whose arrays are gone are freed; if that is not enough, the runtime is
        // asked to collect (at most every kCollectIntervalMs), outside the lock, and they are looked at again.
        Spare spare(env);
        spare.make();  // the sentinel's array, made outside the mutex
        {
            EndedLock lock;
            if (full_sweep_due(env, spare)) {
                full_sweep(env, spare);
            }
        }
        room = allocate_block(need, bytes);
        if (room.raw == nullptr && ask_for_collection(env)) {
            spare.make();
            {
                EndedLock lock;
                full_sweep(env, spare);  // a collection has just run
            }
            room = allocate_block(need, bytes);
        }
    }
    if (room.raw == nullptr) {
        note_declined(state, room.why);
        return nullptr;
    }
    jobject global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(env, local);
    if (global == nullptr) {
        if (room.baseline != nullptr) {
            g.platform.free_baseline(room.baseline);
        }
        g.shadow_arena.release(room.raw);
        release_reservation();
        note_declined(state, "the first byte[] result stayed a real reference (no global reference); blocks and bytes held:");
        return nullptr;
    }
    // The block is built while no record says it is there and is published last, by the store of its record word (see "the
    // block record"): every field, the Dalvik words, the contents, the baseline and the tail come first.
    BlockHeader* header = reinterpret_cast<BlockHeader*>(room.raw);
    header->magic = kBlockDead;
    header->global = global;
    header->next = nullptr;
    header->baseline = room.baseline;
    header->refs = 1;
    header->pins = 0;
    header->weak = nullptr;
    header->hash = hash;
    header->bucket_next = nullptr;
    header->list_next = nullptr;
    header->list_prev = nullptr;
    DalvikArray* block = array_of(header);
    block->clazz = kDalvikArrayClass;
    block->lock = 0;
    block->length = static_cast<uint32_t>(bytes);
    block->pad = 0;
    if (bytes > 0) {
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            env, local, 0, length, reinterpret_cast<jbyte*>(block->data));
        __builtin_memcpy(room.baseline, block->data, bytes);
    }
    __builtin_memset(block->data + bytes, 0, kBlockTail);
    expose_shadows();  // before the block can be found: from here a shadow may reach the application
    mark(header, kBlockLive);
    record_publish(header, static_cast<uint32_t>(bytes));  // the last store to the block
    state->blocks[state->count++] = header;
    state->env = env;
    state->bytes += static_cast<uint32_t>(bytes);
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
    __atomic_fetch_add(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g.counters.shadows_created, 1u, __ATOMIC_RELAXED);
    note_peak(state->count);
    return block;
}

// ---- the references the application keeps
//
// The reference to a block outlives the block's entry: an application keeps a byte[] it got in a native-library global
// (Dalvik's local reference was invalid once the native method returned, but its direct pointer to a reachable array
// was not), and may use it from another thread. There is no owner exemption in the rules for its use in a wrapper:
//   - every wrapper that gives the runtime an array for a block makes a local reference of its own under the mutex, from
//     the block's global or weak reference, uses only that, and deletes it when it returns. It never hands the runtime a
//     reference that belongs to the block: any thread's end, weakening or sweep can delete that, and so can the thread's
//     own, since Java code that runs during a call (a callback that calls a native method that deletes the last reference)
//     ends the block while the wrapper is still using it. The local reference keeps the array reachable meanwhile, so the
//     block is ended at most, never freed, and a synchronization after the call still has an array to reach;
//   - the use of a reference makes no entry and moves no block to the using thread: a block stays ended, or live in the
//     entries of the thread that holds it. Only an array that arrives as a result revives into an entry (make_shadow). A
//     thread with no native frame holds nothing afterwards, and a thread with 512 entries can still use a kept reference;
//   - a pointer from Get...Elements or Get...Critical on such a reference pins the block (an ended block that only a
//     weak reference held gets a global one for the pin) until the release; no entry is needed;
//   - where no reference can be made (an exception is pending, or a runtime critical region is open and this is the
//     critical pair) the wrapper takes a pin of its own on the block, under the mutex, and uses the block's strong
//     reference, which an end then leaves alone (end_locked); the pin is given back when the wrapper returns. A block
//     that only a weak reference holds has no strong reference to use, and the wrapper finds no array;
//   - DeleteLocalRef and GetArrayLength ask nothing of the array;
//   - an address is a block only when the layer's record says a block starts exactly there (the block record above); no
//     byte of the region decides it. The reference of a block whose array the collector has cleared (the weak reference
//     is cleared) is still an ended block of its length until the layer reclaims it (a sweep clears its record word); the
//     wrapper finds no array behind it and does what it does for a null array (no elements, no copy). The reference of a
//     block that has been reclaimed is no block: it converts to nothing, its length is 0, however the memory it points into
//     has been used since. Dalvik's behaviour there was undefined. A reference whose address a new block was made at exactly
//     reaches that block (as a dangling pointer could on Dalvik), whole.
// Every synchronization of a block with its array, whichever thread makes it, runs under the one process-wide mutex that
// also guards the ended index. The mutex is never held across a call into Java: a call is synchronized before and after,
// in two separate holds. It serializes the synchronizations of all blocks, not only of one: the layer has no per-block lock.
//
// What may run under the mutex. The collector's thread flip waits for every open runtime critical region, and sets its
// flag before it waits; a critical get waits while the flag is set unless the thread's own flip count is raised already
// (the runtime raises it only for a movable array or an uncompressed movable string, so a thread inside the application's
// region on a large array, a non-moving one or a compressed string has none); an allocation in the Java heap can wait for a
// collection. A thread inside a region may be waiting for the mutex, so the thread that holds it must never wait for the
// collector. Under the mutex there is therefore
//   - no critical get at all, nested in a region or not, and no allocation in the Java heap;
//   - no Java code, and no wait for another thread; and
//   - of the runtime only these calls, none of which waits for the collector to begin a flip:
//       GetByteArrayRegion, SetByteArrayRegion;
//       NewLocalRef, DeleteLocalRef, NewGlobalRef, DeleteGlobalRef, NewWeakGlobalRef, DeleteWeakGlobalRef, IsSameObject;
//       ExceptionCheck, ExceptionClear;
//       ReleasePrimitiveArrayCritical, ReleaseByteArrayElements (they close what a Get opened).
// A path that needs the critical get of an array (a block's critical pointer, its release, a dirty push) makes it before it
// takes the mutex, on a reference that is valid outside it (a local reference of its own made in an earlier hold, or, inside
// the application's region where none can be made, the block's pinned global), and revalidates the block in the next hold
// (first_critical, sync_in_place). Between a critical get and its release the layer makes no runtime call except that
// release: CheckJNI allows nothing else inside a region, and the merge is stores into the pinned memory. The sentinel's
// array is made the same way (Spare). The table of the runtime's own pointers has a lock of its own, so that the thread
// that has just opened a region records it without the mutex. A thread inside a region may take the mutex where a block
// must be synchronized: that is safe exactly because of the list above. The unit tests' fake runtime fails any other call
// that arrives with the mutex held, and models which gets raise a thread's flip count.

// How a wrapper reaches the array of a block.
struct Access {
    BlockHeader* block;  // null: the block is freed or its array is gone, and there is nothing to reach
    jobject array;       // what the runtime is handed: a local reference of the wrapper's own, or a pinned block's global
    bool own_ref;        // `array` is a local reference of the wrapper's own: it is deleted at the end
    bool pinned;         // the wrapper holds a pin of its own on the block: it is given back at the end
};

// The block a wrapper is handed the reference `object` of: live or ended, otherwise null (no block starts there). Only a
// candidate: the block can be reclaimed, and another made at the same place, before the wrapper takes the mutex, where
// open_header asks the record again; what it finds then is the block that is there.
BlockHeader* block_of(const void* object) {
    return state_of(object) != kStateNone ? header_of(object) : nullptr;
}

// A pin: see pin_block.
void count_pin(BlockHeader* header) { __atomic_fetch_add(&header->pins, 1u, __ATOMIC_RELAXED); }
bool unpin(BlockHeader* header) {
    uint32_t pins = __atomic_load_n(&header->pins, __ATOMIC_RELAXED);
    while (pins > 0 && !__atomic_compare_exchange_n(&header->pins, &pins, pins - 1, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return pins == 1;  // the last one
}
Access open_header(Env* env, BlockHeader* header, bool may_reference = true) {
    Access access{header, nullptr, false, false};
    EndedLock lock;
    if (!block_exists(header)) {  // the record decides, under the mutex: only a publication can change it during this hold
        access.block = nullptr;
        return access;
    }
    if (may_reference && jni_usable(env)) {
        jobject local = original<jobject (*)(Env*, jobject)>(slot::NewLocalRef)(env, header->global != nullptr ? header->global
                                                                                                                : header->weak);
        if (local == nullptr) {
            access.block = nullptr;  // the array was collected: no array to reach (the block is reclaimed by a later sweep)
            return access;
        }
        access.array = local;
        access.own_ref = true;
    } else if (header->global != nullptr) {
        // No reference can be made: the block's strong reference, kept by a pin for as long as the wrapper uses it.
        count_pin(header);
        access.array = header->global;
        access.pinned = true;
    }
    return access;
}
Access open_block(Env* env, const void* object) {
    BlockHeader* header = block_of(object);
    return header != nullptr ? open_header(env, header) : Access{nullptr, nullptr, false, false};
}
void close_access(Env* env, Access& access) {
    if (access.own_ref && access.array != nullptr) {
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, access.array);
    }
    if (access.pinned) {
        unpin(access.block);  // an ended block that this was the last pin of gives way to a weak reference at a later sweep
    }
    access.array = nullptr;
    access.own_ref = false;
    access.pinned = false;
}

// A synchronization of a block, whichever thread's it is, runs under the mutex.
struct SyncScope {
    SyncScope() { lock_ended(); }
    ~SyncScope() { unlock_ended(); }
    SyncScope(const SyncScope&) = delete;
    SyncScope& operator=(const SyncScope&) = delete;
};

// A pointer from Get...Elements or Get...Critical is a pin: the array stays reachable, and the block valid, until the
// pointer is released with mode 0 or JNI_ABORT (JNI_COMMIT keeps it). Dalvik pinned the array the same way. The pin is
// taken under the mutex, so an end cannot swap the block's strong reference for a weak one in between, and an ended block
// that only a weak reference holds is given a global reference for the pin first.
// False: the pin could not be had (the array is gone, or no global reference).
bool pin_block(Env* env, const Access& access) {
    if (access.block == nullptr) {
        return false;
    }
    EndedLock lock;
    BlockHeader* header = access.block;
    if (!block_exists(header)) {
        return false;
    }
    if (header->global == nullptr) {
        if (access.array == nullptr) {
            return false;
        }
        jobject global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(env, access.array);
        if (global == nullptr) {
            original<void (*)(Env*)>(slot::ExceptionClear)(env);
            return false;
        }
        original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, header->weak);
        header->weak = nullptr;
        header->global = global;
    }
    count_pin(header);
    return true;
}

// What the runtime is handed in place of one reference of a wrapper: the real array of a block, held for the wrapper's
// length (null for a block that is gone), anything else unchanged.
class Conv {
public:
    Conv(Env* env, jobject object) : env_(env), object_(object), shadow_(is_shadow(object)), access_{nullptr, nullptr, false, false} {
        if (shadow_) {
            access_ = open_block(env, object);
            __atomic_fetch_add(&g.counters.conversions, 1u, __ATOMIC_RELAXED);
        }
    }
    ~Conv() {
        if (shadow_) {
            close_access(env_, access_);
        }
    }
    Conv(const Conv&) = delete;
    Conv& operator=(const Conv&) = delete;
    jobject get() const { return shadow_ ? access_.array : object_; }

private:
    Env* env_;
    jobject object_;
    bool shadow_;
    Access access_;
};

// The blocks a call is passed, held for its length, each once.
struct HoldSet {
    Env* env;
    Access* items;
    unsigned capacity;
    unsigned count;
    const HoldSet* outer;  // another set whose blocks this one reuses
    HoldSet(Env* e, Access* storage, unsigned slots, const HoldSet* other = nullptr)
        : env(e), items(storage), capacity(slots), count(0), outer(other) {}
    ~HoldSet() {
        for (unsigned i = 0; i < count; ++i) {
            close_access(env, items[i]);
        }
    }
    HoldSet(const HoldSet&) = delete;
    HoldSet& operator=(const HoldSet&) = delete;
    // The array to hand the runtime for the shadow `object`; null for a block that is gone.
    jobject hold(const void* object) {
        BlockHeader* header = block_of(object);
        if (header == nullptr) {
            return nullptr;
        }
        for (const HoldSet* set = this; set != nullptr; set = set->outer) {
            for (unsigned i = 0; i < set->count; ++i) {
                if (set->items[i].block == header) {
                    return set->items[i].array;
                }
            }
        }
        if (count >= capacity) {
            return nullptr;
        }
        items[count] = open_header(env, header);
        if (items[count].block == nullptr) {
            return nullptr;
        }
        return items[count++].array;
    }
};

// The two sets of a call (the receiver's and the arguments') are synchronized around it: what the application wrote
// through the pointers reaches the arrays before, and what Java wrote reaches the blocks after. A call that left an
// exception pending is not followed by a synchronization.
template <class R, class F>
R call_synced(Env* env, const HoldSet& receiver, const HoldSet& arguments, F&& call) {
    const bool any = receiver.count + arguments.count != 0;
    const auto each_in = [&](const HoldSet& set, auto&& step) {
        for (unsigned i = 0; i < set.count; ++i) {
            const Access& access = set.items[i];
            if (access.array != nullptr && access.block != nullptr) {  // held: open_header found the block under the mutex
                step(access);  // sync_range takes the mutex itself
            }
        }
    };
    const auto each = [&](auto&& step) {
        each_in(receiver, step);
        each_in(arguments, step);
    };
    if (any && jni_usable(env)) {
        each([&](const Access& access) { sync_range(env, access.block, access.array, 0, capacity_of(access.block), false); });
    }
    const auto after = [&] {
        if (any && jni_usable(env)) {
            each([&](const Access& access) { sync_range(env, access.block, access.array, 0, capacity_of(access.block), true); });
        }
    };
    if constexpr (IsVoid<R>::value) {
        call();
        after();
    } else {
        R result = call();
        after();
        return result;
    }
}

// What an application gets for a reference-returning call: a shadow when the declared type is byte[].
jobject post_result(Env* env, bool bytes, jobject reference) {
    if (reference == nullptr || !bytes || !g.shadows_on) {
        return reference;
    }
    jobject shadow = make_shadow(env, reference);
    return shadow != nullptr ? shadow : reference;
}

// ---- signatures

unsigned hash_of(const void* id, unsigned modulus) {
    return static_cast<unsigned>((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id) >> 2) * 2654435761u) %
                                 modulus);
}

char kind_at(const MethodInfo& info, unsigned index) { return reinterpret_cast<const char*>(&info + 1)[index]; }

// The parameter kinds of a method signature into `kinds` (kMaxParams + 1 entries); false when it is malformed.
bool parse_signature(const char* sig, char* kinds, unsigned* count, bool* returns_bytes) {
    if (sig == nullptr || sig[0] != '(') {
        return false;
    }
    const char* p = sig + 1;
    unsigned n = 0;
    while (*p != '\0' && *p != ')') {
        char kind;
        switch (*p) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D':
                kind = *p++;
                break;
            case 'L':
                while (*p != '\0' && *p != ';') {
                    ++p;
                }
                if (*p == '\0') {
                    return false;
                }
                ++p;
                kind = 'L';
                break;
            case '[':
                while (*p == '[') {
                    ++p;
                }
                if (*p == 'L') {
                    while (*p != '\0' && *p != ';') {
                        ++p;
                    }
                }
                if (*p == '\0') {
                    return false;
                }
                ++p;
                kind = 'L';
                break;
            default:
                return false;
        }
        if (n >= kMaxParams) {
            return false;
        }
        kinds[n++] = kind;
    }
    if (*p != ')') {
        return false;
    }
    ++p;
    *count = n;
    *returns_bytes = p[0] == '[' && p[1] == 'B' && p[2] == '\0';
    return true;
}

const MethodInfo* find_method(const void* id);

// Makes `id` known with its parameter kinds. False only when the node cannot be allocated; nothing else can refuse it, and a
// method that is known already is left as it was.
bool store_method(void* id, const char* kinds, unsigned count, bool returns_bytes) {
    if (find_method(id) != nullptr) {
        return true;
    }
    MethodInfo* node = static_cast<MethodInfo*>(g.platform.alloc_baseline(sizeof(MethodInfo) + count));
    if (node == nullptr) {
        return false;
    }
    node->next = nullptr;
    node->id = id;
    node->nparams = static_cast<uint8_t>(count);
    node->returns_bytes = returns_bytes;
    char* stored = reinterpret_cast<char*>(node + 1);
    for (unsigned i = 0; i < count; ++i) {
        stored[i] = kinds[i];
    }
    MethodInfo** head = &g.method_bucket[hash_of(id, kMethodBuckets)];
    bool added = true;
    lock_registry();
    for (const MethodInfo* known = *head; known != nullptr; known = known->next) {
        if (known->id == id) {
            added = false;  // another thread stored it meanwhile
            break;
        }
    }
    if (added) {
        node->next = *head;
        __atomic_store_n(head, node, __ATOMIC_RELEASE);
    }
    unlock_registry();
    if (!added) {
        g.platform.free_baseline(node);
    }
    return true;
}

void register_method(void* id, const char* sig) {
    if (id == nullptr) {
        return;
    }
    char kinds[kMaxParams + 1];
    unsigned count = 0;
    bool returns_bytes = false;
    if (parse_signature(sig, kinds, &count, &returns_bytes)) {
        store_method(id, kinds, count, returns_bytes);  // where it cannot be stored the first call learns it again
    }
}

const MethodInfo* find_method(const void* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const MethodInfo* node = __atomic_load_n(&g.method_bucket[hash_of(id, kMethodBuckets)], __ATOMIC_ACQUIRE); node != nullptr;
         node = node->next) {
        if (node->id == id) {
            return node;
        }
    }
    return nullptr;
}

// A method ID the layer did not see being looked up (FromReflectedMethod, or one whose node could not be allocated then):
// asks the platform what its parameters are, from the ID alone (no receiver and no class). Null when they cannot be had now
// (nothing is remembered about that: the next call asks again). A call made with an exception pending, which ART does not refuse,
// is learned with the exception taken out for the time and put back.
const MethodInfo* learn_method(Env* env, void* id, bool is_static) {
    if (id == nullptr || g.platform.describe_method == nullptr) {
        return nullptr;
    }
    jobject pending = nullptr;
    if (original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env) != 0) {
        pending = original<jobject (*)(Env*)>(slot::ExceptionOccurred)(env);
        if (pending == nullptr) {
            return nullptr;
        }
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
    }
    const MethodInfo* learned = nullptr;
    char kinds[kMaxParams + 1];
    unsigned count = 0;
    // From the ID alone: no receiver and no class are needed, so a static call with a null class (which the runtime accepts: it
    // ignores the class of a CallStatic*) is learned like any other.
    if (g.platform.describe_method(env, g.orig, id, is_static, kinds, kMaxParams, &count) && store_method(id, kinds, count, false)) {
        learned = find_method(id);
    }
    if (pending != nullptr) {
        original<jint (*)(Env*, jobject)>(slot::Throw)(env, pending);
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, pending);
    }
    return learned;
}

// An application's call whose parameters the layer cannot learn (no method ID, an ID it cannot describe, no reflection to
// describe it with, memory for its node gone): it cannot say which of the call's values are references, and a block's address
// that reached the runtime would be taken for one. So the call does not reach the runtime: the process stops, with one line in
// the log. A platform caller's call is not stopped: it can hold a block's address only through an application's type error,
// which fails on stock too.
[[noreturn]] void stop_for_unknown_parameters(const void* method) {
    if (g.platform.log != nullptr) {
        g.platform.log("a call's parameters cannot be learned and a block's address must not reach the runtime: the process stops; method:",
                       static_cast<long>(reinterpret_cast<intptr_t>(method)), 0);
    }
    if (g.platform.abort_process != nullptr) {
        g.platform.abort_process();
    }
    __builtin_trap();
}

void register_byte_field(void* id, const char* sig) {
    if (id == nullptr || sig == nullptr || !(sig[0] == '[' && sig[1] == 'B' && sig[2] == '\0')) {
        return;
    }
    lock_registry();
    unsigned index = hash_of(id, kFieldSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kFieldSlots) {
        if (g.byte_fields[index] == id) {
            break;
        }
        if (g.byte_fields[index] == nullptr) {
            __atomic_store_n(&g.byte_fields[index], id, __ATOMIC_RELEASE);
            break;
        }
    }
    unlock_registry();
}

bool is_byte_field(const void* id) {
    if (id == nullptr) {
        return false;
    }
    unsigned index = hash_of(id, kFieldSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kFieldSlots) {
        void* seen = __atomic_load_n(&g.byte_fields[index], __ATOMIC_ACQUIRE);
        if (seen == id) {
            return true;
        }
        if (seen == nullptr) {
            return false;
        }
    }
    return false;
}

// ---- the Call families

enum class Kind { kInstance, kNonvirtual, kStatic, kConstructor };

template <class R, Kind K>
R call_original_v(int slot, Env* e, jobject l0, jobject l1, jmethodID m, va_list ap) {
    if constexpr (K == Kind::kNonvirtual) {
        return reinterpret_cast<R (*)(Env*, jobject, jobject, jmethodID, va_list)>(g.orig[slot])(e, l0, l1, m, ap);
    } else {
        return reinterpret_cast<R (*)(Env*, jobject, jmethodID, va_list)>(g.orig[slot])(e, l0, m, ap);
    }
}

template <class R, Kind K>
R call_original_a(int slot, Env* e, jobject l0, jobject l1, jmethodID m, const jvalue* a) {
    if constexpr (K == Kind::kNonvirtual) {
        return reinterpret_cast<R (*)(Env*, jobject, jobject, jmethodID, const jvalue*)>(g.orig[slot])(e, l0, l1, m, a);
    } else {
        return reinterpret_cast<R (*)(Env*, jobject, jmethodID, const jvalue*)>(g.orig[slot])(e, l0, m, a);
    }
}

template <class R>
struct Finisher {
    static R run(Env*, const MethodInfo*, R result, bool) { return result; }
};
template <>
struct Finisher<jobject> {
    static jobject run(Env* e, const MethodInfo* info, jobject result, bool app) {
        return app ? post_result(e, info != nullptr && info->returns_bytes, result) : result;
    }
};

template <class R, class F>
R complete(Env* e, const MethodInfo* info, bool app, F&& call) {
    if constexpr (IsVoid<R>::value) {
        call();
    } else {
        return Finisher<R>::run(e, info, call(), app);
    }
}

// Whether any argument is a shadow; `pack` receives the arguments with every shadow replaced by the array held for it.
bool collect_va(const MethodInfo& info, va_list ap, jvalue* pack, HoldSet& holds) {
    va_list copy;
    va_copy(copy, ap);
    bool changed = false;
    for (unsigned i = 0; i < info.nparams; ++i) {
        pack[i].j = 0;
        switch (kind_at(info, i)) {
            case 'Z': pack[i].z = static_cast<jboolean>(va_arg(copy, int)); break;
            case 'B': pack[i].b = static_cast<jbyte>(va_arg(copy, int)); break;
            case 'C': pack[i].c = static_cast<jchar>(va_arg(copy, int)); break;
            case 'S': pack[i].s = static_cast<jshort>(va_arg(copy, int)); break;
            case 'I': pack[i].i = va_arg(copy, jint); break;
            case 'J': pack[i].j = va_arg(copy, jlong); break;
            case 'F': pack[i].f = static_cast<jfloat>(va_arg(copy, double)); break;
            case 'D': pack[i].d = va_arg(copy, double); break;
            default: {
                jobject object = va_arg(copy, jobject);
                if (is_shadow(object)) {
                    changed = true;
                    pack[i].l = holds.hold(object);
                } else {
                    pack[i].l = object;
                }
            }
        }
    }
    va_end(copy);
    return changed;
}

bool collect_array(const MethodInfo& info, const jvalue* in, jvalue* pack, HoldSet& holds) {
    bool changed = false;
    for (unsigned i = 0; i < info.nparams; ++i) {
        pack[i] = in[i];
        if (kind_at(info, i) == 'L' && is_shadow(in[i].l)) {
            pack[i].l = holds.hold(in[i].l);
            changed = true;
        }
    }
    return changed;
}

// A call is passed blocks as its arguments and, for an instance or nonvirtual call, as its receiver (clone(), or any
// method without parameters, reads the array through the receiver). The blocks are held for the call and synchronized
// around it. Every call converts once a shadow may have reached the application (shadows_exposed), whatever blocks exist at
// the moment: a reference that is a block becomes a reference the runtime holds, any other address of the region (a block
// that is gone, one being built or revived) becomes null, and no address of the region reaches the runtime.
template <class R, Kind K, int SlotV, int SlotA>
R run_va(Env* e, jobject l0, jobject l1, jmethodID m, va_list ap, bool app) {
    const bool convert = shadows_exposed();
    const MethodInfo* info = ((IsObject<R>::value && app) || convert) ? find_method(m) : nullptr;
    if (convert) {
        constexpr bool kHasReceiver = K == Kind::kInstance || K == Kind::kNonvirtual;
        Access receiver_item[1];
        HoldSet receiver_set(e, receiver_item, 1);
        jobject receiver = l0;
        if (kHasReceiver && is_shadow(l0)) {
            receiver = receiver_set.hold(l0);
        }
        if (info == nullptr) {
            info = learn_method(e, m, K == Kind::kStatic);
            if (info == nullptr) {
                if (app) {
                    stop_for_unknown_parameters(m);
                }
                // A platform caller: its arguments go as they are (the kinds cannot be had), its receiver converted.
                HoldSet no_arguments(e, nullptr, 0, &receiver_set);
                return complete<R>(e, nullptr, app, [&] {
                    return call_synced<R>(e, receiver_set, no_arguments,
                                          [&] { return call_original_v<R, K>(SlotV, e, receiver, l1, m, ap); });
                });
            }
        }
        const unsigned params = info->nparams;
        Access* items = static_cast<Access*>(__builtin_alloca((params + 1) * sizeof(Access)));
        HoldSet argument_set(e, items, params, &receiver_set);
        if (params > 0) {
            jvalue* pack = static_cast<jvalue*>(__builtin_alloca(params * sizeof(jvalue)));
            if (collect_va(*info, ap, pack, argument_set)) {
                return complete<R>(e, info, app, [&] {
                    return call_synced<R>(e, receiver_set, argument_set,
                                          [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
                });
            }
        }
        if (receiver_set.count + argument_set.count != 0) {
            return complete<R>(e, info, app, [&] {
                return call_synced<R>(e, receiver_set, argument_set,
                                      [&] { return call_original_v<R, K>(SlotV, e, receiver, l1, m, ap); });
            });
        }
        return complete<R>(e, info, app, [&] { return call_original_v<R, K>(SlotV, e, receiver, l1, m, ap); });
    }
    return complete<R>(e, info, app, [&] { return call_original_v<R, K>(SlotV, e, l0, l1, m, ap); });
}

template <class R, Kind K, int SlotV, int SlotA>
R run_array(Env* e, jobject l0, jobject l1, jmethodID m, const jvalue* args, bool app) {
    const bool convert = shadows_exposed();
    const MethodInfo* info = ((IsObject<R>::value && app) || convert) ? find_method(m) : nullptr;
    if (convert) {
        constexpr bool kHasReceiver = K == Kind::kInstance || K == Kind::kNonvirtual;
        Access receiver_item[1];
        HoldSet receiver_set(e, receiver_item, 1);
        jobject receiver = l0;
        if (kHasReceiver && is_shadow(l0)) {
            receiver = receiver_set.hold(l0);
        }
        if (info == nullptr) {
            info = learn_method(e, m, K == Kind::kStatic);
            if (info == nullptr) {
                if (app) {
                    stop_for_unknown_parameters(m);
                }
                // A platform caller: its arguments go as they are (the kinds cannot be had), its receiver converted.
                HoldSet no_arguments(e, nullptr, 0, &receiver_set);
                return complete<R>(e, nullptr, app, [&] {
                    return call_synced<R>(e, receiver_set, no_arguments,
                                          [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, args); });
                });
            }
        }
        const unsigned params = info->nparams;
        Access* items = static_cast<Access*>(__builtin_alloca((params + 1) * sizeof(Access)));
        HoldSet argument_set(e, items, params, &receiver_set);
        if (params > 0) {
            jvalue* pack = static_cast<jvalue*>(__builtin_alloca(params * sizeof(jvalue)));
            if (collect_array(*info, args, pack, argument_set)) {
                return complete<R>(e, info, app, [&] {
                    return call_synced<R>(e, receiver_set, argument_set,
                                          [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
                });
            }
        }
        if (receiver_set.count + argument_set.count != 0) {
            return complete<R>(e, info, app, [&] {
                return call_synced<R>(e, receiver_set, argument_set,
                                      [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, args); });
            });
        }
        return complete<R>(e, info, app, [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, args); });
    }
    return complete<R>(e, info, app, [&] { return call_original_a<R, K>(SlotA, e, l0, l1, m, args); });
}

template <class R, int T>
struct Instance {
    static constexpr int kSlotV = call_slot(static_cast<Type>(T), kVa);
    static constexpr int kSlotA = call_slot(static_cast<Type>(T), kArray);
    static R va(Env* e, jobject o, jmethodID m, va_list ap) {
        return run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, enter(e, __builtin_return_address(0)));
    }
    static R array(Env* e, jobject o, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, a, enter(e, __builtin_return_address(0)));
    }
    static R variadic(Env* e, jobject o, jmethodID m, ...) {
        const bool app = enter(e, __builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, app);
            va_end(ap);
            return result;
        }
    }
};

template <class R, int T>
struct Nonvirtual {
    static constexpr int kSlotV = nonvirtual_slot(static_cast<Type>(T), kVa);
    static constexpr int kSlotA = nonvirtual_slot(static_cast<Type>(T), kArray);
    static R va(Env* e, jobject o, jclass c, jmethodID m, va_list ap) {
        return run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, enter(e, __builtin_return_address(0)));
    }
    static R array(Env* e, jobject o, jclass c, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, a, enter(e, __builtin_return_address(0)));
    }
    static R variadic(Env* e, jobject o, jclass c, jmethodID m, ...) {
        const bool app = enter(e, __builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, app);
            va_end(ap);
            return result;
        }
    }
};

// Static calls and constructors share the shape (jclass, jmethodID, arguments); `SlotVarargs` is the first slot
// of the family (Call...Method).
template <class R, int SlotVarargs, Kind K = Kind::kStatic>
struct Static {
    static constexpr int kSlotV = SlotVarargs + 1;
    static constexpr int kSlotA = SlotVarargs + 2;
    static R va(Env* e, jclass c, jmethodID m, va_list ap) {
        return run_va<R, K, kSlotV, kSlotA>(e, c, nullptr, m, ap, enter(e, __builtin_return_address(0)));
    }
    static R array(Env* e, jclass c, jmethodID m, const jvalue* a) {
        return run_array<R, K, kSlotV, kSlotA>(e, c, nullptr, m, a, enter(e, __builtin_return_address(0)));
    }
    static R variadic(Env* e, jclass c, jmethodID m, ...) {
        const bool app = enter(e, __builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, K, kSlotV, kSlotA>(e, c, nullptr, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, K, kSlotV, kSlotA>(e, c, nullptr, m, ap, app);
            va_end(ap);
            return result;
        }
    }
};

template <class R, int T>
void install_family(void** table) {
    using I = Instance<R, T>;
    using N = Nonvirtual<R, T>;
    using S = Static<R, static_call_slot(static_cast<Type>(T), kVarargs)>;
    table[call_slot(static_cast<Type>(T), kVarargs)] = reinterpret_cast<void*>(&I::variadic);
    table[call_slot(static_cast<Type>(T), kVa)] = reinterpret_cast<void*>(&I::va);
    table[call_slot(static_cast<Type>(T), kArray)] = reinterpret_cast<void*>(&I::array);
    table[nonvirtual_slot(static_cast<Type>(T), kVarargs)] = reinterpret_cast<void*>(&N::variadic);
    table[nonvirtual_slot(static_cast<Type>(T), kVa)] = reinterpret_cast<void*>(&N::va);
    table[nonvirtual_slot(static_cast<Type>(T), kArray)] = reinterpret_cast<void*>(&N::array);
    table[static_call_slot(static_cast<Type>(T), kVarargs)] = reinterpret_cast<void*>(&S::variadic);
    table[static_call_slot(static_cast<Type>(T), kVa)] = reinterpret_cast<void*>(&S::va);
    table[static_call_slot(static_cast<Type>(T), kArray)] = reinterpret_cast<void*>(&S::array);
}

// ---- registration and fields

jmethodID w_GetMethodID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jmethodID id = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetMethodID)(e, c, name, sig);
    register_method(id, sig);
    return id;
}
jmethodID w_GetStaticMethodID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jmethodID id =
        original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticMethodID)(e, c, name, sig);
    register_method(id, sig);
    return id;
}
jfieldID w_GetFieldID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jfieldID id = original<jfieldID (*)(Env*, jclass, const char*, const char*)>(slot::GetFieldID)(e, c, name, sig);
    register_byte_field(id, sig);
    return id;
}
jfieldID w_GetStaticFieldID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jfieldID id =
        original<jfieldID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticFieldID)(e, c, name, sig);
    register_byte_field(id, sig);
    return id;
}

jobject w_GetObjectField(Env* e, jobject o, jfieldID f) {
    const bool app = enter(e, __builtin_return_address(0));
    jobject result = original<jobject (*)(Env*, jobject, jfieldID)>(get_field_slot(kObject))(e, Conv(e, o).get(), f);
    return app ? post_result(e, is_byte_field(f), result) : result;
}
jobject w_GetStaticObjectField(Env* e, jclass c, jfieldID f) {
    const bool app = enter(e, __builtin_return_address(0));
    jobject result = original<jobject (*)(Env*, jclass, jfieldID)>(get_static_field_slot(kObject))(e, c, f);
    return app ? post_result(e, is_byte_field(f), result) : result;
}
void w_SetObjectField(Env* e, jobject o, jfieldID f, jobject value) {
    enter_any(e);
    original<void (*)(Env*, jobject, jfieldID, jobject)>(set_field_slot(kObject))(e, Conv(e, o).get(), f, Conv(e, value).get());
}
void w_SetStaticObjectField(Env* e, jclass c, jfieldID f, jobject value) {
    enter_any(e);
    original<void (*)(Env*, jclass, jfieldID, jobject)>(set_static_field_slot(kObject))(e, c, f, Conv(e, value).get());
}
void w_SetObjectArrayElement(Env* e, jobject array, jsize index, jobject value) {
    enter_any(e);
    original<void (*)(Env*, jobject, jsize, jobject)>(slot::SetObjectArrayElement)(e, array, index, Conv(e, value).get());
}
jobject w_NewObjectArray(Env* e, jsize length, jclass c, jobject initial) {
    enter_any(e);
    return original<jobject (*)(Env*, jsize, jclass, jobject)>(slot::NewObjectArray)(e, length, c, Conv(e, initial).get());
}

// ---- references and arrays that may be handed a shadow

jobject w_GetObjectClass(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::GetObjectClass)(e, Conv(e, o).get());
}
jboolean w_IsInstanceOf(Env* e, jobject o, jclass c) {
    enter_any(e);
    return original<jboolean (*)(Env*, jobject, jclass)>(slot::IsInstanceOf)(e, Conv(e, o).get(), c);
}
jobject w_NewGlobalRef(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(e, Conv(e, o).get());
}
jobject w_NewWeakGlobalRef(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(e, Conv(e, o).get());
}
jobject w_NewLocalRef(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewLocalRef)(e, Conv(e, o).get());
}
jboolean w_IsSameObject(Env* e, jobject a, jobject b) {
    enter_any(e);
    return original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(e, Conv(e, a).get(), Conv(e, b).get());
}
jint w_GetObjectRefType(Env* e, jobject o) {
    enter_any(e);
    // One look at the record answers: a block is a local reference (an ended block's reference is the one the application kept:
    // it stays a local); an address of the region that no block starts at is the null reference to the runtime (invalid), and is
    // not looked up again, since a block made there meanwhile is not what this reference was.
    if (is_shadow(o)) {
        return state_of(o) != kStateNone ? kRefTypeLocal
                                         : original<jint (*)(Env*, jobject)>(slot::GetObjectRefType)(e, nullptr);
    }
    return original<jint (*)(Env*, jobject)>(slot::GetObjectRefType)(e, o);
}
// A block whose array is gone has no monitor to take: the runtime aborts on a null object, so the call is refused (JNI_ERR).
jint w_MonitorEnter(Env* e, jobject o) {
    enter_any(e);
    Conv array(e, o);
    if (is_shadow(o) && array.get() == nullptr) {
        return JNI_ERR;
    }
    return original<jint (*)(Env*, jobject)>(slot::MonitorEnter)(e, array.get());
}
// MonitorExit is one of the functions JNI allows with an exception pending (a scoped monitor's unlock after a Java call
// threw), and a block's array cannot be reached with one pending: the exception is taken out, the array converted, the
// monitor released, and the exception put back unless MonitorExit left one of its own. The saved reference is deleted on
// every path.
jint w_MonitorExit(Env* e, jobject o) {
    enter_any(e);
    jobject pending = nullptr;
    if (is_shadow(o) && !jni_usable(e)) {
        pending = original<jobject (*)(Env*)>(slot::ExceptionOccurred)(e);
        original<void (*)(Env*)>(slot::ExceptionClear)(e);
    }
    jint result = JNI_ERR;
    {
        Conv array(e, o);
        if (!is_shadow(o) || array.get() != nullptr) {
            result = original<jint (*)(Env*, jobject)>(slot::MonitorExit)(e, array.get());
        }
    }
    if (pending != nullptr) {
        if (jni_usable(e)) {
            original<jint (*)(Env*, jobject)>(slot::Throw)(e, pending);
        }
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(e, pending);
    }
    return result;
}
// The entry of the block that is the newest in the innermost frame: DeleteLocalRef takes one reference of the block. Both
// runtimes search only the current frame (the segment a native method or PushLocalFrame opened) and leave a reference of
// an outer frame alone (ART's LocalReferenceTable::Remove and Dalvik's IndirectRefTable::remove: "wrong segment", nothing
// removed), so a nested native method that deletes a block's reference its caller's frame holds ends nothing here either.
// With no frame record (a native method that got no thunk) every entry of the thread is the frame's.
void w_DeleteLocalRef(Env* e, jobject o) {
    enter_any(e);
    if (is_shadow(o)) {
        BlockHeader* header = live_header(o);
        ThreadState* state = thread();
        if (header == nullptr || state == nullptr) {
            return;
        }
        const unsigned floor = state->frame_count > 0 ? state->frames[state->frame_count - 1].height : 0;
        for (unsigned i = state->count; i > floor; --i) {
            if (state->blocks[i - 1] == header) {
                const unsigned index = i - 1;
                for (unsigned k = index; k + 1 < state->count; ++k) {
                    state->blocks[k] = state->blocks[k + 1];
                }
                --state->count;  // (no frame's height is above the entry: the innermost frame's is the floor)
                release_entry(e, state, header);  // the block goes with its last reference
                return;
            }
        }
        return;  // another thread's block, or a reference of an outer frame: it goes with its own
    }
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(e, o);
}

// PushLocalFrame opens a frame of the thread's references: PopLocalFrame ends the entries made in it.
jint w_PushLocalFrame(Env* e, jint capacity) {
    enter_any(e);
    const jint result = original<jint (*)(Env*, jint)>(slot::PushLocalFrame)(e, capacity);
    if (result != JNI_OK) {
        return result;
    }
    ThreadState* state = thread();
    if (state == nullptr) {
        return result;
    }
    if (state->frame_count < kMaxFrames) {
        state->frames[state->frame_count++] = FrameRecord{static_cast<uint16_t>(state->count), kFramePush, nullptr, nullptr};
    } else {
        ++state->frame_overflow;
    }
    return result;
}
// A shadow lives in no reference table, so popping the frame cannot take it: the original pop runs with no result
// and the same block is returned, which gets an entry in the outer frame as the real result would. The entries made
// in the frame end with it.
jobject w_PopLocalFrame(Env* e, jobject result) {
    enter_any(e);
    const bool shadow_result = is_shadow(result);
    BlockHeader* known = shadow_result ? block_of(result) : nullptr;
    ThreadState* state = thread();
    // Only a block that is among this thread's own entries moves to the outer frame; any other reference keeps its meaning.
    // The entries are the thread's alone and each holds its block (refs >= 1), so the search is exact: a block that another
    // thread made at the address of a reclaimed one is not among them.
    BlockHeader* keep = known != nullptr && state != nullptr && has_entry(state, known) ? known : nullptr;
    jobject returned = original<jobject (*)(Env*, jobject)>(slot::PopLocalFrame)(e, shadow_result ? nullptr : result);
    if (state != nullptr) {
        if (state->frame_overflow > 0) {
            --state->frame_overflow;  // the pop of a frame no record was made for
        } else if (state->frame_count > 0 && state->frames[state->frame_count - 1].kind == kFramePush) {
            const unsigned height = state->frames[--state->frame_count].height;
            const bool outer_entry = keep != nullptr && height < kMaxBlocks;
            if (outer_entry) {
                ++keep->refs;  // taken before the frame's entries end, so the block survives them
            }
            const unsigned ended = end_entries(state, height);
            if (ended != 0) {
                __atomic_fetch_add(&g.counters.ended_by_pop, ended, __ATOMIC_RELAXED);
            }
            if (outer_entry) {
                state->blocks[state->count++] = keep;
                state->env = e;
            }
        }
    }
    if (shadow_result) {
        return known != nullptr ? result : nullptr;
    }
    return returned;
}

// RegisterNatives: the functions of the application's libraries are registered through their thunks.
jint w_RegisterNatives(Env* e, jclass c, const NativeMethod* methods, jint count) {
    enter_any(e);
    auto call = original<jint (*)(Env*, jclass, const NativeMethod*, jint)>(slot::RegisterNatives);
    if (methods == nullptr || count <= 0 || g.platform.thunk_address == nullptr) {
        return call(e, c, methods, count);
    }
    NativeMethod chunk[kRegisterChunk];
    for (jint done = 0; done < count;) {
        const jint n = count - done < static_cast<jint>(kRegisterChunk) ? count - done : static_cast<jint>(kRegisterChunk);
        for (jint i = 0; i < n; ++i) {
            chunk[i] = methods[done + i];
            chunk[i].function = thunk_for(chunk[i].function);
        }
        const jint result = call(e, c, chunk, n);
        if (result != JNI_OK) {
            return result;
        }
        done += n;
    }
    return JNI_OK;
}

// The block whose data starts at `elements`, when the pointer is one the layer handed out: live or ended (a freed block's
// pointer is nothing). The record says whether a block starts 16 bytes before it; a pointer that is not at the start of a
// block's data is nobody's and is not read as a header.
BlockHeader* block_at(const void* elements) {
    const uint8_t* p = static_cast<const uint8_t*>(elements);
    if (p == nullptr || p < g.shadow_lo + offsetof(DalvikArray, data)) {
        return nullptr;
    }
    return block_of(p - offsetof(DalvikArray, data));
}

jsize w_GetArrayLength(Env* e, jarray a) {
    enter_any(e);
    if (is_shadow(a)) {
        // The record knows a block's length: live or ended (an ended block whose array the collector has cleared is still an
        // ended block, of its length, until a sweep reclaims it), the block that starts at this address has it exactly; any
        // other address in the region (a block that was reclaimed, whatever lies in its memory now) has none, and gets 0. No
        // mutex and no byte of the region: one acquire load of the record's word.
        return static_cast<jsize>(fact_of(a).capacity);
    }
    return original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(e, a);
}
// ---- the runtime's own pointers
//
// An application may take a pointer from the runtime for a real reference to an array (a global it made of a block with
// NewGlobalRef, say) and release it through another reference to the same array, a block's among them. JNI accepts any
// reference to the array, so such a Release has to reach the runtime, which owns the copy or the critical region the
// pointer stands for. The layer therefore records the pointers the runtime hands out from Get...Elements and
// Get...Critical on real references, once the process has had a block, and a Release of a recorded pointer through a
// block's reference goes to the runtime's own Release with the block's array. Any other pointer that comes with a block's
// reference is the application's own buffer (a copy it made of the block), and is copied back as a write into the block.
// The record is removed at a release with mode 0 or JNI_ABORT; JNI_COMMIT keeps it, as the pointer stays valid.
//
// The table has a lock of its own, a spin lock: the pointers are recorded and looked up by a thread that has a runtime
// critical region open (its Get just returned, or its Release is about to), and such a thread must not wait for the
// layer's mutex on this account. Nothing is called and no other lock is taken under it, and it is held for a few
// instructions. A waiter yields its processor after a short spin (the platform's yield, which calls nothing of the runtime),
// so that a holder the scheduler took off its processor can run.
struct TableLock {
    static constexpr unsigned kSpinsBeforeYield = 64;
    TableLock() {
        unsigned spins = 0;
        while (__atomic_exchange_n(&g.records_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
            if (++spins >= kSpinsBeforeYield && g.platform.yield_thread != nullptr) {
                g.platform.yield_thread();
                spins = 0;
            }
        }
    }
    ~TableLock() { __atomic_store_n(&g.records_lock, 0u, __ATOMIC_RELEASE); }
    TableLock(const TableLock&) = delete;
    TableLock& operator=(const TableLock&) = delete;
};
unsigned record_slot(const void* pointer) {
    return static_cast<unsigned>((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer) >> 2) * 2654435761u) % kRecordSlots);
}
PointerRecord* find_record(const void* pointer, uint32_t kind) {
    for (unsigned i = record_slot(pointer), n = 0; n < kRecordSlots; i = (i + 1) % kRecordSlots, ++n) {
        PointerRecord* record = &g.records[i];
        if (record->kind == kRecordFree) {
            return nullptr;
        }
        if (record->pointer == pointer && record->kind == kind) {
            return record;
        }
    }
    return nullptr;
}
// Removes the record in slot i and closes the gap, so that no later lookup stops short of an entry (linear probing).
void erase_record(unsigned i) {
    unsigned j = i;
    for (;;) {
        j = (j + 1) % kRecordSlots;
        if (g.records[j].kind == kRecordFree) {
            break;
        }
        const unsigned home = record_slot(g.records[j].pointer);
        const bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
        if (!stays) {
            g.records[i] = g.records[j];
            i = j;
        }
    }
    g.records[i] = PointerRecord{nullptr, kRecordFree, 0};
    __atomic_fetch_sub(&g.records_in_use, 1u, __ATOMIC_RELAXED);
}
void record_runtime_pointer(const void* pointer, uint32_t kind) {
    if (pointer == nullptr) {
        return;
    }
    {
        TableLock lock;
        if (PointerRecord* record = find_record(pointer, kind)) {
            ++record->count;
            return;
        }
        if (__atomic_load_n(&g.records_in_use, __ATOMIC_RELAXED) < kRecordSlots / 2) {
            unsigned i = record_slot(pointer);
            while (g.records[i].kind != kRecordFree) {
                i = (i + 1) % kRecordSlots;
            }
            g.records[i] = PointerRecord{pointer, kind, 1};
            __atomic_fetch_add(&g.records_in_use, 1u, __ATOMIC_RELAXED);
            return;
        }
    }
    // Not recorded: a Release of it through a block's reference is treated as a buffer of the application's. One line says
    // so, once (after the table's lock is given up: the log is a call into the platform).
    if (g.platform.log != nullptr && __atomic_exchange_n(&g.records_full_logged, 1u, __ATOMIC_RELAXED) == 0) {
        g.platform.log("the table of the runtime's own pointers is full; later pointers are not recorded; pointers and slots:",
                       static_cast<long>(kRecordSlots / 2), static_cast<long>(kRecordSlots));
    }
}
// The count of the records is changed under the table's lock and read, atomically (relaxed) everywhere, also without it: a count of
// zero answers "not recorded" for a pointer without the lock, and that answer is exact. A caller asks about a pointer it holds. It
// holds it because the runtime handed it out to this thread, which recorded it (a store under the lock) before it used the pointer,
// or because another thread passed it on through the application's own synchronization, which orders that store before this load.
// A load ordered after a store reads it or a later value of the count, and the count is lower than the store made it only by the
// release of a record, which is then gone. So zero means that no record of the pointer exists; any other count takes the lock.
bool is_recorded_pointer(const void* pointer, uint32_t kind) {
    if (pointer == nullptr || __atomic_load_n(&g.records_in_use, __ATOMIC_RELAXED) == 0) {
        return false;
    }
    TableLock lock;
    return find_record(pointer, kind) != nullptr;
}
// A release of the pointer (any reference): the record goes with the last Get unless the mode is JNI_COMMIT.
void forget_runtime_pointer(const void* pointer, uint32_t kind, jint mode) {
    if (mode == kReleaseCommit || pointer == nullptr || __atomic_load_n(&g.records_in_use, __ATOMIC_RELAXED) == 0) {
        return;  // (no record exists: see is_recorded_pointer)
    }
    TableLock lock;
    PointerRecord* record = find_record(pointer, kind);
    if (record == nullptr) {
        return;
    }
    if (record->count > 1) {
        --record->count;
        return;
    }
    erase_record(static_cast<unsigned>(record - g.records));
}

// The pointer of a shadow is a copy, and says so (JNI_TRUE): the block is separate storage that is kept alike at the
// synchronization points, so code that commits before it lets Java read the array through another reference (a global
// reference, a pinned pointer) must be told to. A real reference keeps the runtime's own flag.
jbyte* w_GetByteArrayElements(Env* e, jarray a, jboolean* is_copy) {
    enter_any(e);
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 1;
        }
        Access access = open_block(e, a);
        if (access.block == nullptr || access.array == nullptr) {
            return nullptr;  // the array is gone, or nothing holds the block now (see w_GetByteArrayRegion): no pin to take
        }
        sync_range(e, access.block, access.array, 0, capacity_of(access.block), true);
        jbyte* elements = pin_block(e, access) ? reinterpret_cast<jbyte*>(array_of(access.block)->data) : nullptr;
        close_access(e, access);
        return elements;
    }
    jbyte* elements = original<jbyte* (*)(Env*, jarray, jboolean*)>(slot::GetByteArrayElements)(e, a, is_copy);
    record_runtime_pointer(elements, kRecordElements);
    return elements;
}
// The block is the working copy. A committing release (mode 0 or COMMIT) synchronizes like any other point: the
// chunks the application wrote are merged into the array, byte by byte; JNI_ABORT discards the working copy (the block
// takes the array's bytes). A buffer other than the block (an application that copied it) is the application's write
// into the block first; a pointer into the block's own memory but not at its start is nothing to copy from (the old
// runtime ignored the pointer; it only counts as the release of the block's pin). The release has a local reference of its
// own for the array. The critical pair may run inside another array's critical region, where none can be made: it uses
// the block's own strong reference, which the pin being released keeps, and a pin of its own besides (open_header). A
// release with an exception pending synchronizes nothing (no region call may follow), and neither does one that finds no
// array: the working copy keeps its writes for the next point.
void release_block(Env* e, BlockHeader* header, const void* elements, jint mode, bool critical) {
    ThreadState* state = thread();
    const bool in_region = critical && state != nullptr && state->critical_depth != 0;
    Access access = open_header(e, header, !in_region);
    const jobject array = access.array;
    if (access.block != nullptr && jni_usable(e) && array != nullptr) {
        const uint32_t capacity = capacity_of(header);  // the block is held: its length, from the record
        // A buffer other than the block (a copy the application made of it) is its write into the block first.
        const bool foreign = mode != kReleaseAbort && elements != nullptr && elements != data_of(header) && !is_shadow(elements) &&
                             capacity > 0;
        if (critical) {
            // The critical pair, as the release of a pointer that the application may hold inside a region of its own: the
            // get is made before the mutex is taken, on a reference that is valid outside it (inside a region, the block's
            // pinned global), the rest in one hold.
            const Pair pair = first_critical(e, header, array);
            SyncScope scope;
            if (mode == kReleaseAbort) {
                abort_critical(e, header, array, pair);
            } else {
                if (foreign) {
                    __builtin_memcpy(data_of(header), elements, capacity);
                }
                sync_in_place(e, header, array, pair, 0, capacity, true);
            }
        } else if (mode == kReleaseAbort) {
            SyncScope scope;
            abort_block(e, header, array);
        } else {
            if (foreign) {
                SyncScope scope;
                __builtin_memcpy(data_of(header), elements, capacity);
            }
            sync_range(e, header, array, 0, capacity, true);
        }
    }
    // A pin is given back only by a release that holds the block (a reference of its own, or a pin of its own): a block that
    // nothing holds is freed by a sweep, and its memory is another block's by the time of the write. A release that finds
    // no pin to give (a second release, a release without a Get) leaves the count as it is.
    if (mode != kReleaseCommit && array != nullptr && unpin(header) && !critical && jni_usable(e)) {
        EndedLock lock;
        if (state_of(array_of(header)) == kStateEnded && header->global != nullptr && __atomic_load_n(&header->pins, __ATOMIC_RELAXED) == 0) {
            weaken(e, header);  // the last pin of an ended block: it waits for a collection from now on
        }
    }
    close_access(e, access);
}

void close_critical_region();

// The runtime's pointer released through a block's reference: the runtime's own Release runs, with the block's array. The
// layer unpins nothing (the pointer was never one of its own) and closes the critical region it counted. The reference is a
// local one of this release's, or, where none can be made (an exception pending, or the region this very pointer opened),
// the block's strong or weak one, with the mutex held: the array is alive, since the real reference the pointer came from
// keeps it.
void release_runtime_pointer(Env* e, BlockHeader* header, void* elements, jint mode, bool critical) {
    Access access = open_header(e, header, !critical);
    {
        SyncScope scope;
        jobject array = access.array;
        if (array == nullptr && block_exists(header)) {
            array = header->global != nullptr ? header->global : header->weak;
        }
        if (array != nullptr) {
            if (critical) {
                original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(e, array, elements, mode);
            } else {
                original<void (*)(Env*, jarray, jbyte*, jint)>(slot::ReleaseByteArrayElements)(e, array, static_cast<jbyte*>(elements),
                                                                                                mode);
            }
        }
    }
    close_access(e, access);
    if (critical) {
        close_critical_region();
    }
    forget_runtime_pointer(elements, critical ? kRecordCritical : kRecordElements, mode);
}

// A Release goes to the block its pointer lies in, whichever reference it arrives through (another reference to the array,
// a global, or the block's own); the block, not the runtime, owns that memory. A pointer the runtime handed out and the
// layer recorded goes to the runtime instead, whichever reference comes with it. True when it was handled: a pointer or a
// reference of the layer's is never the runtime's to release. A pointer inside a block but not at its start, arriving with
// a real reference, finds no block and is dropped.
bool route_release(Env* e, jarray a, const void* elements, jint mode, bool critical) {
    BlockHeader* header = block_at(elements);
    const bool shadow_ref = is_shadow(a);
    if (header == nullptr && shadow_ref) {
        header = block_of(a);
        if (header != nullptr && is_recorded_pointer(elements, critical ? kRecordCritical : kRecordElements)) {
            release_runtime_pointer(e, header, const_cast<void*>(elements), mode, critical);
            return true;
        }
    }
    if (header != nullptr) {
        release_block(e, header, elements, mode, critical);
        return true;
    }
    return shadow_ref || is_shadow(elements);
}
void w_ReleaseByteArrayElements(Env* e, jarray a, jbyte* elements, jint mode) {
    enter_any(e);
    if (route_release(e, a, elements, mode, false)) {
        return;
    }
    original<void (*)(Env*, jarray, jbyte*, jint)>(slot::ReleaseByteArrayElements)(e, a, elements, mode);
    forget_runtime_pointer(elements, kRecordElements, mode);
}
// The critical entries make no other JNI call than the real array's critical pair, and do not drain the blocks of
// ended threads (that deletes global references, which a critical region forbids). The runtime's critical regions that the
// thread has open are counted (the string pair too): with none open and no exception pending a reference of a block that
// ended weakly gets a local reference of the wrapper's own and a pin like any other; inside a region, or with an exception
// pending, no call that makes a reference may be made, and the block is used through its own strong reference when it has
// one (a live block, a pinned one) and the call returns NULL otherwise.
void* w_GetPrimitiveArrayCritical(Env* e, jarray a, jboolean* is_copy) {
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 1;  // a copy, as for Get...Elements
        }
        BlockHeader* header = block_of(a);
        if (header == nullptr) {
            return nullptr;
        }
        ThreadState* state = thread();
        // With a runtime critical region open, or an exception pending, no reference can be made: the block's strong
        // reference is used, kept by the pin that the pointer is.
        const bool restricted = !jni_usable(e) || (state != nullptr && state->critical_depth != 0);
        Access access = open_header(e, header, !restricted);
        if (access.block == nullptr || access.array == nullptr) {
            close_access(e, access);
            return nullptr;
        }
        // The get is made before the mutex is taken, whether or not the thread has a region open (see first_critical).
        const Pair pair = first_critical(e, header, access.array);
        {
            SyncScope scope;
            sync_in_place(e, header, access.array, pair, 0, capacity_of(header), true);
        }
        if (access.pinned) {
            access.pinned = false;  // the wrapper's pin becomes the pointer's
        } else if (!pin_block(e, access)) {
            close_access(e, access);
            return nullptr;
        }
        close_access(e, access);
        return data_of(header);
    }
    void* elements = original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(e, a, is_copy);
    if (elements != nullptr) {
        ThreadState* state = thread();
        if (state != nullptr) {
            ++state->critical_depth;
        }
        record_runtime_pointer(elements, kRecordCritical);
    }
    return elements;
}
void close_critical_region() {
    ThreadState* state = thread();
    if (state != nullptr && state->critical_depth > 0) {
        --state->critical_depth;
    }
}
void w_ReleasePrimitiveArrayCritical(Env* e, jarray a, void* elements, jint mode) {
    if (route_release(e, a, elements, mode, true)) {
        return;
    }
    original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(e, a, elements, mode);
    close_critical_region();
    forget_runtime_pointer(elements, kRecordCritical, mode);
}
const jchar* w_GetStringCritical(Env* e, jstring string, jboolean* is_copy) {
    const jchar* chars = original<const jchar* (*)(Env*, jstring, jboolean*)>(slot::GetStringCritical)(e, string, is_copy);
    if (chars != nullptr) {
        ThreadState* state = thread();
        if (state != nullptr) {
            ++state->critical_depth;
        }
    }
    return chars;
}
void w_ReleaseStringCritical(Env* e, jstring string, const jchar* chars) {
    original<void (*)(Env*, jstring, const jchar*)>(slot::ReleaseStringCritical)(e, string, chars);
    close_critical_region();
}
void w_GetByteArrayRegion(Env* e, jarray a, jsize start, jsize length, jbyte* buffer) {
    enter_any(e);
    if (is_shadow(a)) {
        Access access = open_block(e, a);
        if (access.block == nullptr || access.array == nullptr) {
            // The block is gone, or nothing holds it: only a weak reference does, and none can be made now (an exception is
            // pending). No array, no copy, as for a null array: the block's bytes are not the array's, and a sweep may free
            // a block that nothing holds while it is read.
            return;
        }
        BlockHeader* header = access.block;
        if (start >= 0 && length >= 0 && static_cast<uint64_t>(start) + static_cast<uint64_t>(length) <= capacity_of(header)) {
            sync_range(e, header, access.array, static_cast<uint32_t>(start),
                       static_cast<uint32_t>(start) + static_cast<uint32_t>(length), true);
            __builtin_memcpy(buffer, data_of(header) + start, static_cast<size_t>(length));
        } else {
            // A range the block does not have: the runtime raises its exception for the real array.
            original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(e, access.array, start, length,
                                                                                             buffer);
        }
        close_access(e, access);
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(e, a, start, length, buffer);
}
void w_SetByteArrayRegion(Env* e, jarray a, jsize start, jsize length, const jbyte* buffer) {
    enter_any(e);
    if (is_shadow(a)) {
        Access access = open_block(e, a);
        if (access.block == nullptr || access.array == nullptr) {
            return;  // no array to reach, and no read of a block nothing holds (see w_GetByteArrayRegion)
        }
        BlockHeader* header = access.block;
        if (start >= 0 && length >= 0 && static_cast<uint64_t>(start) + static_cast<uint64_t>(length) <= capacity_of(header)) {
            if (length > 0) {
                // What the application wrote through the pointer in these chunks reaches the array first; the range
                // itself is about to be replaced, so nothing is read back.
                sync_range(e, header, access.array, static_cast<uint32_t>(start),
                           static_cast<uint32_t>(start) + static_cast<uint32_t>(length), false);
                SyncScope scope;
                __builtin_memcpy(data_of(header) + start, buffer, static_cast<size_t>(length));
                original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
                    e, access.array, start, length, buffer);
                __builtin_memcpy(header->baseline + start, buffer, static_cast<size_t>(length));
            }
        } else {
            // A range the block does not have: the runtime raises its exception for the real array.
            original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(e, access.array, start,
                                                                                                   length, buffer);
        }
        close_access(e, access);
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(e, a, start, length, buffer);
}

// ---- the invoke interface: only a "a library may have loaded" notification

void vm_note(const void* return_address) { (void)from_app(return_address); }

jint vm_GetEnv(Vm* vm, void** env, jint version) {
    vm_note(__builtin_return_address(0));
    return reinterpret_cast<jint (*)(Vm*, void**, jint)>(g.orig_vm[vmslot::GetEnv])(vm, env, version);
}
jint vm_AttachCurrentThread(Vm* vm, void** env, void* args) {
    vm_note(__builtin_return_address(0));
    return reinterpret_cast<jint (*)(Vm*, void**, void*)>(g.orig_vm[vmslot::AttachCurrentThread])(vm, env, args);
}
jint vm_AttachCurrentThreadAsDaemon(Vm* vm, void** env, void* args) {
    vm_note(__builtin_return_address(0));
    return reinterpret_cast<jint (*)(Vm*, void**, void*)>(g.orig_vm[vmslot::AttachCurrentThreadAsDaemon])(vm, env, args);
}
// The thread's references end with its attachment (its environment is gone after the call).
jint vm_DetachCurrentThread(Vm* vm) {
    ThreadState* state = g.shadows_on ? thread() : nullptr;
    if (state != nullptr) {
        const unsigned ended = end_entries(state, 0);
        if (ended != 0) {
            __atomic_fetch_add(&g.counters.ended_by_detach, ended, __ATOMIC_RELAXED);
        }
        state->frame_count = 0;
        state->frame_overflow = 0;
        state->env = nullptr;
    }
    return reinterpret_cast<jint (*)(Vm*)>(g.orig_vm[vmslot::DetachCurrentThread])(vm);
}

}  // namespace

bool is_shadow_block(const void* object) { return state_of(object) == kStateLive; }

bool is_ended_block(const void* object) { return state_of(object) == kStateEnded; }

void release_thread_state(void* state_pointer) {
    ThreadState* state = static_cast<ThreadState*>(state_pointer);
    if (state == nullptr || !g.shadows_on) {
        return;
    }
    for (unsigned i = 0; i < state->count; ++i) {
        BlockHeader* header = state->blocks[i];
        if (--header->refs > 0) {
            continue;  // the block has more entries: it is queued with its last
        }
        queue_pending(header);
    }
    state->count = 0;
    state->bytes = 0;
    state->frame_count = 0;
    state->frame_overflow = 0;
    state->env = nullptr;
}

// A copy of the counters, each member one relaxed load by name: other threads keep counting, so the copy is a sample of each
// counter and not one instant of all of them. A member added to Counters is added here too.
Counters counters() {
    const auto load = [](const auto& member) { return __atomic_load_n(&member, __ATOMIC_RELAXED); };
    Counters copy{};
    const Counters& now = g.counters;
    copy.shadows_created = load(now.shadows_created);
    copy.shadows_live = load(now.shadows_live);
    copy.shadows_declined = load(now.shadows_declined);
    copy.conversions = load(now.conversions);
    copy.shadows_released = load(now.shadows_released);
    copy.peak_thread_blocks = load(now.peak_thread_blocks);
    copy.ended_by_return = load(now.ended_by_return);
    copy.max_ended_by_return = load(now.max_ended_by_return);
    copy.ended_by_pop = load(now.ended_by_pop);
    copy.ended_by_detach = load(now.ended_by_detach);
    copy.natives_wrapped = load(now.natives_wrapped);
    copy.natives_unwrapped = load(now.natives_unwrapped);
    copy.shadows_ended = load(now.shadows_ended);
    copy.ended_bytes = load(now.ended_bytes);
    copy.blocks_ended = load(now.blocks_ended);
    copy.blocks_collected = load(now.blocks_collected);
    copy.blocks_revived = load(now.blocks_revived);
    copy.collections_asked = load(now.collections_asked);
    copy.longest_region_us = load(now.longest_region_us);
    return copy;
}

// ---- the end of a native method

// The thunk of a native function, or the function itself.
//
// Only a function of the application's own native libraries gets one, decided by the address of the function
// being registered or found (a platform or another package's library, such as WebView, keeps the runtime's
// binding, whoever the caller is). The thunks are a fixed set (the platform's thunk_capacity, 1024): one for
// each distinct function, kept for the life of the process. When they run out, the method is bound with no thunk,
// as when the layer has none: it works as before, and its blocks end by DeleteLocalRef, PopLocalFrame and
// DetachCurrentThread, or count toward the thread's 512 limit (a result beyond it stays the real reference, with its
// log line). One log line says so, once.
void* thunk_for(void* function) {
    if (function == nullptr || !g.installed || !g.shadows_on || g.platform.thunk_address == nullptr ||
        g.thunk_capacity == 0) {
        return function;
    }
    if (g.platform.is_own_library != nullptr ? !g.platform.is_own_library(function) : !from_app(function)) {
        return function;
    }
    while (__atomic_exchange_n(&g.thunk_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
    unsigned index = 0;
    while (index < g.thunk_count && g.thunk_target[index] != function) {
        ++index;
    }
    bool exhausted = false;
    if (index == g.thunk_count) {
        if (g.thunk_count >= g.thunk_capacity) {
            exhausted = true;
        } else {
            __atomic_store_n(&g.thunk_target[index], function, __ATOMIC_RELEASE);
            ++g.thunk_count;
            __atomic_fetch_add(&g.counters.natives_wrapped, 1u, __ATOMIC_RELAXED);
        }
    }
    const unsigned wrapped = g.thunk_count;
    __atomic_store_n(&g.thunk_lock, 0u, __ATOMIC_RELEASE);
    if (exhausted) {
        __atomic_fetch_add(&g.counters.natives_unwrapped, 1u, __ATOMIC_RELAXED);
        if (g.platform.log != nullptr && __atomic_exchange_n(&g.thunk_none_logged, 1u, __ATOMIC_RELAXED) == 0) {
            g.platform.log("an application native method got no thunk (all in use): its blocks end only with their references; "
                           "thunks in use:", static_cast<long>(wrapped), 0);
        }
        return function;
    }
    return g.platform.thunk_address(index);
}

// The diagnosis lines of the thunks: the first return that ended references, and the most one return has ended (a
// native method that makes many blocks in one call is what can reach the thread's limit), at most every kPeakLogMs.
static void note_ended_at_return(unsigned ended) {
    if (g.platform.log == nullptr) {
        return;
    }
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (__atomic_exchange_n(&g.ended_logged, 1u, __ATOMIC_RELAXED) == 0) {
        __atomic_store_n(&g.ended_max_logged, ended, __ATOMIC_RELAXED);
        __atomic_store_n(&g.ended_max_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("the first byte[] blocks ended at the return of a native method; references ended, native "
                       "methods with a thunk:", static_cast<long>(ended), static_cast<long>(g.thunk_count));
        return;
    }
    uint32_t most = __atomic_load_n(&g.counters.max_ended_by_return, __ATOMIC_RELAXED);
    const uint32_t logged = __atomic_load_n(&g.ended_max_logged, __ATOMIC_RELAXED);
    if (most <= logged ||
        (g.platform.now_ms != nullptr && now - __atomic_load_n(&g.ended_max_logged_ms, __ATOMIC_RELAXED) < kPeakLogMs)) {
        return;
    }
    uint32_t expected = logged;
    if (__atomic_compare_exchange_n(&g.ended_max_logged, &expected, most, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g.ended_max_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("the most references one native method call has ended at its return; native methods with a thunk:",
                       static_cast<long>(most), static_cast<long>(g.thunk_count));
    }
}

ThunkEntry thunk_enter(unsigned index, void* return_address, void* sp) {
    void* target = __atomic_load_n(&g.thunk_target[index < kMaxThunks ? index : 0], __ATOMIC_ACQUIRE);
    ThreadState* state = thread();
    if (state == nullptr || state->frame_count >= kMaxFrames) {
        return ThunkEntry{target, 0};  // no record: the method returns to the runtime as it is, its blocks end by themselves
    }
    state->frames[state->frame_count++] =
        FrameRecord{static_cast<uint16_t>(state->count), kFrameNative, return_address, sp};
    return ThunkEntry{target, 1};
}

void* thunk_leave(void* sp) {
    ThreadState* state = thread();
    if (state == nullptr) {
        __builtin_trap();  // the entry made the state: there is no return address to go back to
    }
    // The record of this invocation, the newest one entered with this stack pointer. The records above it are the
    // method's own unmatched PushLocalFrame calls, or invocations that never returned (a longjmp over them).
    unsigned k = state->frame_count;
    while (k > 0 && !(state->frames[k - 1].kind == kFrameNative && state->frames[k - 1].sp == sp)) {
        --k;
    }
    if (k == 0) {
        __builtin_trap();
    }
    const FrameRecord record = state->frames[k - 1];
    state->frame_count = k - 1;
    state->frame_overflow = 0;
    const unsigned ended = end_entries(state, record.height);
    if (ended != 0) {
        __atomic_fetch_add(&g.counters.ended_by_return, ended, __ATOMIC_RELAXED);
        uint32_t most = __atomic_load_n(&g.counters.max_ended_by_return, __ATOMIC_RELAXED);
        while (ended > most && !__atomic_compare_exchange_n(&g.counters.max_ended_by_return, &most, ended, true,
                                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
        note_ended_at_return(ended);
    }
    return record.lr;
}

void reset_for_test() {
    if (g.platform.free_baseline != nullptr) {
        for (MethodInfo* head : g.method_bucket) {
            while (head != nullptr) {
                MethodInfo* next = head->next;
                g.platform.free_baseline(head);
                head = next;
            }
        }
    }
    __builtin_memset(&g, 0, sizeof(g));
}

bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size, Env* env) {
    if (g.installed) {
        return false;
    }
    g.platform = platform;
    for (int i = 0; i < kSlotCount; ++i) {
        g.orig[i] = table[i];
    }
    // The record covers a region of at most kMaxShadowBytes whose base is 16-byte aligned (the references of blocks are aligned
    // from it): any other region gets no shadows, and neither does one whose record memory cannot be had. The memory is
    // zero-filled when the platform gives it, and the layer does not clear it.
    g.shadows_on = shadow_region != nullptr && shadow_size >= 4096 && shadow_size <= kMaxShadowBytes &&
                   (reinterpret_cast<uintptr_t>(shadow_region) & 15u) == 0 && platform.alloc_baseline != nullptr &&
                   platform.free_baseline != nullptr && platform.map_record != nullptr &&
                   g.shadow_arena.init(shadow_region, shadow_size);
    if (g.shadows_on) {
        g.record = static_cast<uint32_t*>(platform.map_record(shadow_record_bytes(shadow_size)));
        g.shadows_on = g.record != nullptr;
    }
    if (!g.shadows_on) {
        // Nothing to intercept. The query bounds of an earlier attempt that was refused later (the table could not be written)
        // are cleared: the queries range-test an address first, and an address of the region must not reach a record that is not
        // there.
        g.shadow_lo = nullptr;
        g.shadow_hi = nullptr;
        return true;
    }
    g.ended_head.list_next = &g.ended_head;
    g.ended_head.list_prev = &g.ended_head;
    g.sweep_cursor = &g.ended_head;
    g.shadow_lo = static_cast<const uint8_t*>(shadow_region) + kBlockHead;
    g.shadow_hi = static_cast<const uint8_t*>(shadow_region) + shadow_size;
    if (!platform.make_writable(table, kSlotCount * sizeof(void*))) {
        g.shadows_on = false;
        g.shadow_lo = nullptr;
        g.shadow_hi = nullptr;
        return false;
    }
    g.installed = true;
    if (env != nullptr) {
        ensure_ids(env);  // single-threaded here, and before the first wrapper runs: no block is made without a hash
    }
    g.thunk_capacity = platform.thunk_address != nullptr
                           ? (platform.thunk_capacity < kMaxThunks ? platform.thunk_capacity : kMaxThunks)
                           : 0;
    table[slot::GetMethodID] = reinterpret_cast<void*>(&w_GetMethodID);
    table[slot::GetStaticMethodID] = reinterpret_cast<void*>(&w_GetStaticMethodID);
    table[slot::GetFieldID] = reinterpret_cast<void*>(&w_GetFieldID);
    table[slot::GetStaticFieldID] = reinterpret_cast<void*>(&w_GetStaticFieldID);
    install_family<jobject, kObject>(table);
    install_family<jboolean, kBoolean>(table);
    install_family<jbyte, kByte>(table);
    install_family<jchar, kChar>(table);
    install_family<jshort, kShort>(table);
    install_family<jint, kInt>(table);
    install_family<jlong, kLong>(table);
    install_family<jfloat, kFloat>(table);
    install_family<jdouble, kDouble>(table);
    install_family<void, kVoid>(table);
    using ObjectConstructor = Static<jobject, slot::NewObject, Kind::kConstructor>;
    table[slot::NewObject] = reinterpret_cast<void*>(&ObjectConstructor::variadic);
    table[slot::NewObjectV] = reinterpret_cast<void*>(&ObjectConstructor::va);
    table[slot::NewObjectA] = reinterpret_cast<void*>(&ObjectConstructor::array);
    table[get_field_slot(kObject)] = reinterpret_cast<void*>(&w_GetObjectField);
    table[get_static_field_slot(kObject)] = reinterpret_cast<void*>(&w_GetStaticObjectField);
    table[set_field_slot(kObject)] = reinterpret_cast<void*>(&w_SetObjectField);
    table[set_static_field_slot(kObject)] = reinterpret_cast<void*>(&w_SetStaticObjectField);
    table[slot::SetObjectArrayElement] = reinterpret_cast<void*>(&w_SetObjectArrayElement);
    table[slot::NewObjectArray] = reinterpret_cast<void*>(&w_NewObjectArray);
    table[slot::GetObjectClass] = reinterpret_cast<void*>(&w_GetObjectClass);
    table[slot::IsInstanceOf] = reinterpret_cast<void*>(&w_IsInstanceOf);
    table[slot::NewGlobalRef] = reinterpret_cast<void*>(&w_NewGlobalRef);
    table[slot::NewWeakGlobalRef] = reinterpret_cast<void*>(&w_NewWeakGlobalRef);
    table[slot::NewLocalRef] = reinterpret_cast<void*>(&w_NewLocalRef);
    table[slot::IsSameObject] = reinterpret_cast<void*>(&w_IsSameObject);
    table[slot::GetObjectRefType] = reinterpret_cast<void*>(&w_GetObjectRefType);
    table[slot::MonitorEnter] = reinterpret_cast<void*>(&w_MonitorEnter);
    table[slot::MonitorExit] = reinterpret_cast<void*>(&w_MonitorExit);
    table[slot::DeleteLocalRef] = reinterpret_cast<void*>(&w_DeleteLocalRef);
    table[slot::PushLocalFrame] = reinterpret_cast<void*>(&w_PushLocalFrame);
    table[slot::PopLocalFrame] = reinterpret_cast<void*>(&w_PopLocalFrame);
    table[slot::RegisterNatives] = reinterpret_cast<void*>(&w_RegisterNatives);
    table[slot::GetArrayLength] = reinterpret_cast<void*>(&w_GetArrayLength);
    table[slot::GetByteArrayElements] = reinterpret_cast<void*>(&w_GetByteArrayElements);
    table[slot::ReleaseByteArrayElements] = reinterpret_cast<void*>(&w_ReleaseByteArrayElements);
    table[slot::GetPrimitiveArrayCritical] = reinterpret_cast<void*>(&w_GetPrimitiveArrayCritical);
    table[slot::GetStringCritical] = reinterpret_cast<void*>(&w_GetStringCritical);
    table[slot::ReleaseStringCritical] = reinterpret_cast<void*>(&w_ReleaseStringCritical);
    table[slot::ReleasePrimitiveArrayCritical] = reinterpret_cast<void*>(&w_ReleasePrimitiveArrayCritical);
    table[slot::GetByteArrayRegion] = reinterpret_cast<void*>(&w_GetByteArrayRegion);
    table[slot::SetByteArrayRegion] = reinterpret_cast<void*>(&w_SetByteArrayRegion);
    if (!platform.make_read_only(table, kSlotCount * sizeof(void*)) && platform.log != nullptr) {
        platform.log("a page of the JNI function table could not be made read-only again", 0, 0);
    }
    return true;
}

bool install_vm_wrappers(void** vm_table, const Platform& platform) {
    if (g.vm_installed) {
        return false;
    }
    g.platform = platform;
    for (int i = 0; i < kVmSlotCount; ++i) {
        g.orig_vm[i] = vm_table[i];
    }
    if (!platform.make_writable(vm_table, kVmSlotCount * sizeof(void*))) {
        return false;
    }
    g.vm_installed = true;
    vm_table[vmslot::GetEnv] = reinterpret_cast<void*>(&vm_GetEnv);
    vm_table[vmslot::AttachCurrentThread] = reinterpret_cast<void*>(&vm_AttachCurrentThread);
    vm_table[vmslot::AttachCurrentThreadAsDaemon] = reinterpret_cast<void*>(&vm_AttachCurrentThreadAsDaemon);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&vm_DetachCurrentThread);
    if (!platform.make_read_only(vm_table, kVmSlotCount * sizeof(void*)) && platform.log != nullptr) {
        platform.log("a page of the JavaVM function table could not be made read-only again", 0, 0);
    }
    return true;
}

}  // namespace oldapp

extern "C" uint64_t oldapp_thunk_enter(unsigned index, void* return_address, void* sp) {
    const oldapp::ThunkEntry entry = oldapp::thunk_enter(index, return_address, sp);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.target)) | (static_cast<uint64_t>(entry.pushed) << 32);
}

extern "C" void* oldapp_thunk_leave(void* sp) { return oldapp::thunk_leave(sp); }
