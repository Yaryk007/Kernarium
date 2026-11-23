// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's JNI interposer (android/lineage-24/overlay/oldapp/jni_shim.cpp)
// against a fake JNI function table that accepts only live references: the slot constants against
// the specification's order (argv[1], tests/unit/andarium/fixtures/jni-slots.tsv), byte[] shadows
// with Dalvik's layout and everything an application can then do with one, the identity and writes
// of the array a shadow stands for, what the runtime is handed in place of a shadow (for any caller,
// whatever the method), the limits (nothing in use is recycled), a thread that ends, PopLocalFrame,
// and the calls of the platform's own libraries, which must see no shadow.
#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/arm/thread_status.h>
#include <sys/ucontext.h>
#endif

#include "jni_abi.hpp"
#include "jni_shim.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "watchdog.hpp"

using namespace oldapp;
using namespace oldapp::jni;
using uml::testing::require;
using uml::testing::waitUntil;

namespace {

// ---- the fake runtime

struct Obj {
    std::vector<uint8_t> bytes;  // byte[] contents; strings keep their text here too
    bool is_array = false;
    bool held = false;           // Java holds it: a collection does not take it
    bool is_class = false;       // what FindClass gave
    int hash = 0;                // its identity hash (given at the first ask)
    bool is_string = false;      // a string: bytes hold its characters (one byte each)
    bool compressed = false;     // a compressed string: its critical is a copy
    bool non_moving = false;     // in the non-moving space
};
// The runtime raises a thread's flip count at a critical get only for a movable array or an uncompressed movable string
// (art jni_internal.cc): an array of 12 KiB or more is in the large-object space and does not move, nor does one in the
// non-moving space, and a compressed string's critical is a copy.
bool raises_the_flip_count(const Obj* o) {
    if (o->non_moving) return false;
    if (o->is_string) return !o->compressed;
    return o->bytes.size() < 12288;
}
struct Ref {
    Obj* object;                 // null: a weak reference whose object was collected
    bool global;
    bool weak = false;
};

struct Method {
    std::string sig;
    std::string name;
};

struct World {
    std::set<Ref*> locals;
    std::set<Ref*> globals;
    std::set<Ref*> weaks;                   // weak global references
    std::set<Ref*> class_globals;           // the layer's global references to the classes it calls (kept for good)
    int gc_calls = 0;                       // Runtime.gc() calls
    int hash_calls = 0;                     // System.identityHashCode calls
    int next_hash = 100;
    bool same_hash = false;                 // every object has the identity hash 4242
    int weak_new_calls = 0;
    int same_calls = 0;                     // IsSameObject calls that reached the runtime
    bool find_fails = false;                // FindClass fails (an exception)
    bool weak_fails = false;                // NewWeakGlobalRef gives nothing
    bool hash_fails = false;                // System.identityHashCode leaves an exception
    std::vector<std::vector<Ref*>> frames;  // PushLocalFrame / PopLocalFrame
    std::map<void*, Method> methods;
    std::map<void*, std::string> fields;
    std::vector<jvalue> last_args;
    std::vector<Obj*> last_array_args;  // the object behind every reference argument, in order
    std::string last_call;
    bool exception_pending = false;
    int app_checks = 0;
    int traps = 0;
    int describe_calls = 0;
    bool describe_fails = false;
    int monitors = 0;
    jobject next_result = nullptr;
    int java_fill = -1;              // >= 0: every array argument of a call is filled with this byte during the call
    bool throw_after_call = false;   // a call leaves an exception pending
    int critical = 0;                // open critical regions of the fake runtime
    int set_calls = 0;               // SetByteArrayRegion calls that reached the runtime
    std::vector<std::vector<uint8_t>> seen_arrays;  // the bytes of every array argument when the call ran
    std::vector<uint8_t> seen_receiver;             // the bytes of an array receiver when the call ran
    int new_array_calls = 0;                        // NewByteArray calls that reached the runtime
    int critical_gets = 0;                          // GetPrimitiveArrayCritical calls that reached the runtime
    bool new_array_fails = false;                   // NewByteArray fails (an exception)
    int get_calls = 0;                              // GetByteArrayRegion calls that reached the runtime
    int release_calls = 0;                          // Release...Elements / ReleasePrimitiveArrayCritical calls that reached the runtime
    std::vector<const void*> released;              // the pointers they were handed
    struct LogLine {
        std::string message;
        long a, b;
    };
    std::vector<LogLine> logs;  // what the layer logged
    uint32_t clock_ms = 1000;   // the layer's clock
    Obj* pending_object = nullptr;      // the exception that is pending (ExceptionOccurred hands out a reference to it)
    Obj* monitor_exit_throws = nullptr; // MonitorExit raises this exception
    std::set<Ref*> revoked;     // globals that stopped being valid (what a re-entrant end of a block would make them)
    bool revoke_next = false;   // the next array function revokes the globals of every array first
    int set_fails = 0;          // the next SetByteArrayRegion calls are refused (an exception, nothing written)
    int receiver_calls = 0;     // instance and nonvirtual calls that reached the runtime
    Obj* last_receiver = nullptr;  // the object behind the receiver of the latest of them (null: the receiver was null)
};
World world;

// The fake runtime's lock. Several cases run the layer on more than one thread at once (a hook starts a thread, a thread waits
// for the layer's mutex while the owner holds it), and the layer calls the runtime from each of them, so the state above is
// guarded by this one leaf mutex. It is held only while fake-runtime code reads or writes that state, never across a test hook,
// a wait for another thread or a call into the layer, and the only lock taken under it is the flip model's, briefly and never
// waited on. So it cannot deadlock a case, and it hides no ordering a case is about: the layer's own mutex and the order of the
// layer's calls are untouched. A helper that guards itself may be called from one that holds the lock already (t_world_depth
// counts the thread's holds). A test body reads `world` without the lock only where no other thread touches it: before a thread
// starts, after it has been joined, or while the others wait on the body.
std::mutex g_world_mutex;
thread_local int t_world_depth = 0;
class WorldGuard {
public:
    WorldGuard() {
        if (t_world_depth++ == 0) g_world_mutex.lock();
    }
    ~WorldGuard() {
        if (--t_world_depth == 0) g_world_mutex.unlock();
    }
    WorldGuard(const WorldGuard&) = delete;
    WorldGuard& operator=(const WorldGuard&) = delete;
};
// For its scope the thread holds none of the lock, whatever depth it was at: a test hook, or a wait for another thread, runs
// inside it. The lock is taken again, at the same depth, when the scope ends.
class WorldUnlocked {
public:
    WorldUnlocked() : depth_(t_world_depth) {
        if (depth_ != 0) {
            t_world_depth = 0;
            g_world_mutex.unlock();
        }
    }
    ~WorldUnlocked() {
        if (depth_ != 0) {
            g_world_mutex.lock();
            t_world_depth = depth_;
        }
    }
    WorldUnlocked(const WorldUnlocked&) = delete;
    WorldUnlocked& operator=(const WorldUnlocked&) = delete;

private:
    int depth_;
};

// A test hook: what a case has the fake run at a chosen point, so that another thread (or the case) acts meanwhile. The slot is
// guarded like the rest of the fake's state; the fake copies or takes the hook out under the lock and runs it with the lock
// released (WorldUnlocked), so that whatever the hook does, on this thread or on one it starts, meets no lock of the fake.
template <class... Args>
class Hook {
public:
    Hook& operator=(std::nullptr_t) {
        WorldGuard guard;
        fn_ = nullptr;
        return *this;
    }
    template <class F>
    Hook& operator=(F&& f) {
        WorldGuard guard;
        fn_ = std::forward<F>(f);
        return *this;
    }
    // A copy of the hook; it stays in the slot (a hook that clears itself, or that stays for the rest of a case).
    std::function<void(Args...)> peek() const {
        WorldGuard guard;
        return fn_;
    }
    // The hook, out of its slot: it runs once, in the call that finds it.
    std::function<void(Args...)> take() {
        WorldGuard guard;
        std::function<void(Args...)> hook = std::move(fn_);
        fn_ = nullptr;
        return hook;
    }

private:
    std::function<void(Args...)> fn_;
};

thread_local int t_ended_depth = 0;  // how deep this thread is in the ended lock (a wrapper is never entered under it)
thread_local int t_regions = 0;      // the critical regions this thread has open (CheckJNI's rule: nothing else runs inside one)
thread_local int t_flips = 0;        // the thread's flip count: raised only by a get that raises_the_flip_count (the nesting the flip rule sees)
thread_local int t_app_regions = 0;  // the ones the application holds: the count after each of its own critical calls returned

// CheckJNI's rules: nothing but the critical pair runs inside a critical region (a thread's own), and almost nothing with
// an exception pending.
void jni_allowed(const char* what) {
    WorldGuard guard;
    require(t_regions == 0, what);
    require(!world.exception_pending, what);
}

// The layer's mutex rule (the comment "What may run under the mutex" in jni_shim.cpp): under the mutex the layer calls the
// runtime's region and reference functions and the releases, and no critical get at all (a get it would call nested can be
// the runtime's outermost, when the application's region is on an array that does not raise the flip count). A thread that
// holds the mutex must never wait for the collector, because a thread inside a runtime critical region may be waiting for
// the mutex while the collector's flip waits for that region. Every other fake function calls this first, so a call that is
// not on the list fails in every row, whichever thread makes it.
void not_under_mutex(const char* function) {
    require(t_ended_depth == 0, function);
    require(t_regions == t_app_regions, "a runtime call inside a region the layer opened (only its release is allowed)");
}
// A call on the list of the mutex rule (the region and reference functions, the exception checks): not inside a region the
// layer itself opened. The layer opens one only to merge in place, and between the get and the release it makes no runtime
// call but the release (CheckJNI aborts on any other JNI call inside a critical region).
void on_the_list(const char*) {
    require(t_regions == t_app_regions, "a runtime call inside a region the layer opened (only its release is allowed)");
}

// The runtime's thread-flip rule (art/runtime/gc/heap.cc): ThreadFlipBegin sets the flag, then waits until no thread has a
// flip count; a critical get that raises the count from zero waits while the flag is set; one that raises it further does
// not. A wait that exceeds two seconds is counted (the real runtime waits for good) and the get goes on.
std::mutex g_flip_mutex;
std::condition_variable g_flip_cv;
bool g_flip_running = false;      // a flip is pending or running
int g_open_regions = 0;           // threads with a region open
int g_gets_that_waited = 0;       // critical gets that found the flip pending and waited for it
std::atomic<int> g_flip_timeouts{0};
Hook<> g_before_first_critical;  // runs on the first thread that makes a critical get, before the get waits
void flip_enter_region() {
    std::unique_lock<std::mutex> l(g_flip_mutex);
    if (g_flip_running) {
        ++g_gets_that_waited;
        g_flip_cv.notify_all();
    }
    if (!g_flip_cv.wait_for(l, std::chrono::seconds(2), [] { return !g_flip_running; })) ++g_flip_timeouts;
    ++g_open_regions;
}
void flip_leave_region() {
    std::lock_guard<std::mutex> l(g_flip_mutex);
    --g_open_regions;
    g_flip_cv.notify_all();
}
// The collector: sets the flag and waits for the open regions (false: the wait timed out, for good in the real runtime),
// then the flip runs and ends.
bool flip_begin_waits_for_regions() {
    std::unique_lock<std::mutex> l(g_flip_mutex);
    g_flip_running = true;
    g_flip_cv.notify_all();
    return g_flip_cv.wait_for(l, std::chrono::seconds(3), [] { return g_open_regions == 0; });
}
void flip_end() {
    std::lock_guard<std::mutex> l(g_flip_mutex);
    g_flip_running = false;
    g_flip_cv.notify_all();
}

bool live(jobject ref) {
    WorldGuard guard;
    Ref* r = static_cast<Ref*>(ref);
    if (world.revoked.count(r) != 0) return false;
    return world.locals.count(r) != 0 || world.globals.count(r) != 0 || world.weaks.count(r) != 0 ||
           world.class_globals.count(r) != 0;
}
Ref* new_local(Obj* object) {
    WorldGuard guard;
    Ref* ref = new Ref{object, false};
    world.locals.insert(ref);
    if (!world.frames.empty()) world.frames.back().push_back(ref);
    return ref;
}
Obj* object_of(jobject ref) {
    WorldGuard guard;
    require(live(ref), "the runtime is handed a live reference, never a shadow");
    return static_cast<Ref*>(ref)->object;
}
// Called first by the fake's functions that take an array: when a row asks for it, every global reference to an array
// stops being valid at that point, as the end of a block by Java code that runs during the call would make it. A wrapper
// that hands the runtime the block's own reference then fails the liveness requirement; one that made its own does not.
void revoke_if_asked() {
    WorldGuard guard;
    if (!world.revoke_next) return;
    world.revoke_next = false;
    for (Ref* ref : world.globals) {
        if (ref->object != nullptr && ref->object->is_array) world.revoked.insert(ref);
    }
}

jobject byte_array(const std::vector<uint8_t>& bytes) {
    WorldGuard guard;
    Obj* object = new Obj{bytes, true};
    return new_local(object);
}

bool g_app_caller = true;
uint64_t g_epoch = 1;
bool g_no_thread_state = false;                  // the platform has no thread state to give
// Whether a native function is in the application's own libraries (the thunks' domain); a predicate decides by the
// function's address. Independent of the caller (g_app_caller).
bool g_own_default = true;
bool (*g_own_predicate)(const void*) = nullptr;
bool g_without_own_check = false;  // the next install gives the platform no is_own_library (not reset by reset_world)
bool g_without_describe = false;   // the next install gives the platform no describe_method (not reset by reset_world)

bool fake_is_app(const void*) {
    require(t_ended_depth == 0, "a wrapper was entered while the ended lock is held");
    WorldGuard guard;
    ++world.app_checks;
    return g_app_caller;
}
bool fake_is_own(const void* function) { return g_own_predicate != nullptr ? g_own_predicate(function) : g_own_default; }
thread_local void* t_block = nullptr;
thread_local uint64_t t_epoch = 0;
void* fake_thread_state(size_t size) {
    require(t_ended_depth == 0, "the thread state was asked for under the ended lock");
    if (g_no_thread_state) {
        return nullptr;
    }
    if (t_epoch != g_epoch) {
        std::free(t_block);
        t_block = std::calloc(1, size);
        t_epoch = g_epoch;
    }
    return t_block;
}
void fake_log(const char* message, long a, long b) {
    WorldGuard guard;
    world.logs.push_back({message, a, b});
}
uint32_t fake_now() {
    WorldGuard guard;
    return world.clock_ms;
}
// Platform::abort_process: the layer stops the process for a call whose parameters it cannot learn. A row that expects it comes
// back here (the_process_stops); anywhere else it fails the row.
std::jmp_buf g_stop_jump;
bool g_stop_expected = false;
int g_stops = 0;
void fake_abort() {
    require(g_stop_expected, "the layer stops the process only where a row expects it");
    g_stop_expected = false;
    ++g_stops;
    std::longjmp(g_stop_jump, 1);
}
bool g_baseline_fails = false;
void* fake_alloc_baseline(size_t bytes) { return g_baseline_fails ? nullptr : std::malloc(bytes); }
Hook<> g_in_free_baseline;  // runs inside the free of a baseline, the last step before a reclaimed block's memory goes back to the arena
void fake_free_baseline(void* baseline) {
    if (auto hook = g_in_free_baseline.take()) hook();
    std::free(baseline);
}
bool g_writable_ok = true;
bool g_read_only_ok = true;
bool fake_make_writable(void*, size_t) { return g_writable_ok; }
bool fake_make_read_only(void*, size_t) { return g_read_only_ok; }

// The kinds of a method's parameters from its signature, for an ID the layer never saw being looked up.
bool fake_describe(Env*, void* const*, void* id, bool, char* kinds, unsigned capacity, unsigned* count) {
    WorldGuard guard;
    ++world.describe_calls;
    const auto found = world.methods.find(id);
    if (world.describe_fails || found == world.methods.end()) return false;
    const std::string& sig = found->second.sig;
    unsigned n = 0;
    for (size_t i = 1; sig[i] != ')'; ++i) {
        require(n < capacity, "the fake has room");
        switch (sig[i]) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D': kinds[n++] = sig[i]; break;
            case 'L': while (sig[i] != ';') ++i; kinds[n++] = 'L'; break;
            case '[':
                while (sig[i] == '[') ++i;
                if (sig[i] == 'L') while (sig[i] != ';') ++i;
                kinds[n++] = 'L';
                break;
            default: require(false, "a signature the fake knows");
        }
    }
    *count = n;
    return true;
}
// The thunks of the fake: thunk i is the address kThunkBase + 8 * i, as the assembly lays them out.
constexpr uintptr_t kThunkBase = 0x70000000u;
void* fake_thunk_address(unsigned index) { return reinterpret_cast<void*>(kThunkBase + 8u * index); }
unsigned thunk_index_of(void* thunk) { return unsigned((reinterpret_cast<uintptr_t>(thunk) - kThunkBase) / 8u); }
bool is_thunk(void* address) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    return a >= kThunkBase && a < kThunkBase + 8u * 4096u;
}
unsigned g_thunk_capacity = 1024;
// The ended blocks' lock: a mutex that no thread takes twice, and that a wrapper is never entered under (the layer
// makes JNI calls under it, and they go to the runtime's own functions).
std::mutex g_ended_mutex;
int g_ended_locks = 0;
// A pause before a thread's next take of the ended lock: the thread sets t_pause_at_lock, and the hook runs on it, with no lock of the
// fake or of the layer held, just before the lock is taken (another thread acts meanwhile).
thread_local bool t_pause_at_lock = false;
Hook<> g_before_lock;
void fake_mutex_lock() {
    require(t_ended_depth == 0, "the ended lock is taken twice by one thread");
    if (t_pause_at_lock) {
        t_pause_at_lock = false;
        if (auto hook = g_before_lock.take()) hook();
    }
    g_ended_mutex.lock();
    ++t_ended_depth;
    ++g_ended_locks;
}
void fake_mutex_unlock() {
    require(t_ended_depth == 1, "the ended lock is released by its holder");
    --t_ended_depth;
    g_ended_mutex.unlock();
}
uint64_t fake_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
void fake_yield() { std::this_thread::yield(); }
// The memory of the block record: a buffer the cases share, zero-filled again at each installation by this test code (the layer
// never clears the record; production maps a fresh anonymous mapping). g_record_memory is replaced by a probe or a case that
// needs its own, and g_record_fails makes the platform unable to give it.
alignas(64) uint32_t g_record_storage[(8u << 20) / 64];
uint32_t* g_record_memory = g_record_storage;
size_t g_record_memory_bytes = sizeof(g_record_storage);
bool g_record_fails = false;
bool g_record_reset = true;
size_t g_record_expected_bytes = 0;  // what the installation must ask for: shadow_record_bytes of its region
void* fake_map_record(size_t bytes) {
    if (g_record_fails) {
        return nullptr;
    }
    require(bytes == g_record_expected_bytes && bytes <= g_record_memory_bytes,
            "the layer asks for the record of its region, and the buffer covers it");
    if (g_record_reset) {
        std::memset(g_record_memory, 0, bytes);
    }
    return g_record_memory;
}
const Platform kPlatform{fake_is_app, fake_thread_state, fake_log, fake_make_writable, fake_make_read_only,
                         fake_describe, fake_now, fake_alloc_baseline, fake_free_baseline, fake_thunk_address, 1024,
                         fake_is_own, fake_mutex_lock, fake_mutex_unlock, fake_now_ns, fake_yield, fake_map_record, fake_abort};

jsize f_GetArrayLength(Env*, jarray a) {
    WorldGuard guard;
    not_under_mutex("GetArrayLength is not on the list of calls under the mutex");
    return static_cast<jsize>(object_of(a)->bytes.size());
}
jobject f_NewByteArray(Env*, jsize n) {
    WorldGuard guard;
    not_under_mutex("NewByteArray (an allocation in the Java heap can wait for the collector) under the mutex");
    ++world.new_array_calls;
    if (world.new_array_fails) {
        world.exception_pending = true;
        return nullptr;
    }
    return byte_array(std::vector<uint8_t>(static_cast<size_t>(n), 0));
}
Hook<> g_after_set;  // runs inside a SetByteArrayRegion, after it copied
void f_SetByteArrayRegion(Env*, jarray a, jsize start, jsize len, const jbyte* buf) {
    WorldGuard guard;
    on_the_list("SetByteArrayRegion");
    revoke_if_asked();
    jni_allowed("SetByteArrayRegion where JNI is not allowed (a critical region, or an exception pending)");
    ++world.set_calls;
    auto& bytes = object_of(a)->bytes;
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > bytes.size() || world.set_fails > 0) {
        if (world.set_fails > 0) --world.set_fails;
        world.exception_pending = true;
        return;
    }
    std::memcpy(bytes.data() + start, buf, size_t(len));
    if (auto hook = g_after_set.take()) {
        WorldUnlocked unlocked;
        hook();
    }
}
Hook<> g_after_get;  // runs inside a GetByteArrayRegion, after it copied: another thread acts meanwhile
std::atomic<const void*> g_last_get_buffer{nullptr};  // where the latest GetByteArrayRegion that was not refused copied to
void f_GetByteArrayRegion(Env*, jarray a, jsize start, jsize len, jbyte* buf) {
    WorldGuard guard;
    on_the_list("GetByteArrayRegion");
    revoke_if_asked();
    jni_allowed("GetByteArrayRegion where JNI is not allowed (a critical region, or an exception pending)");
    ++world.get_calls;
    auto& bytes = object_of(a)->bytes;
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > bytes.size()) {
        world.exception_pending = true;
        return;
    }
    std::memcpy(buf, bytes.data() + start, size_t(len));
    g_last_get_buffer.store(buf, std::memory_order_release);
    if (auto hook = g_after_get.take()) {
        WorldUnlocked unlocked;
        hook();
    }
}
jbyte* f_GetByteArrayElements(Env*, jarray a, jboolean* is_copy) {
    WorldGuard guard;
    not_under_mutex("GetByteArrayElements under the mutex");
    revoke_if_asked();
    jni_allowed("GetByteArrayElements where JNI is not allowed");
    if (is_copy) *is_copy = 0;
    return reinterpret_cast<jbyte*>(object_of(a)->bytes.data());
}
void f_ReleaseByteArrayElements(Env*, jarray a, jbyte* p, jint) {
    WorldGuard guard;
    on_the_list("ReleaseByteArrayElements");
    revoke_if_asked();
    (void)object_of(a);
    ++world.release_calls;
    world.released.push_back(p);
}
// A critical get of the runtime. Under the layer's mutex none is allowed, whether or not the thread has a region open: the
// flip count decides whether a get waits, and the layer cannot know it. A get that raises the thread's count from zero waits
// for a pending flip; the hook runs just before that wait.
void begin_region(const Obj* object) {
    WorldGuard guard;
    if (raises_the_flip_count(object)) {
        if (t_flips == 0) {
            WorldUnlocked unlocked;  // the hook, and the wait for a pending flip, run without the fake's lock
            if (auto hook = g_before_first_critical.take()) hook();
            flip_enter_region();
        }
        ++t_flips;
    }
    ++t_regions;
    ++world.critical;
}
void end_region(const Obj* object) {
    WorldGuard guard;
    require(world.critical > 0 && t_regions > 0, "a critical region was open");
    --world.critical;
    --t_regions;
    if (raises_the_flip_count(object)) {
        require(t_flips > 0, "the thread's flip count was raised by the get");
        if (--t_flips == 0) flip_leave_region();
    }
}
void* f_GetPrimitiveArrayCritical(Env*, jarray a, jboolean* is_copy) {
    WorldGuard guard;
    on_the_list("GetPrimitiveArrayCritical");
    ++world.critical_gets;
    not_under_mutex("a critical get under the mutex");
    Obj* object = object_of(a);
    begin_region(object);
    revoke_if_asked();
    if (is_copy) *is_copy = 0;
    return object->bytes.data();
}
Hook<> g_in_release_critical;  // runs inside a ReleasePrimitiveArrayCritical, after it closed the region
void f_ReleasePrimitiveArrayCritical(Env*, jarray a, void* p, jint) {
    WorldGuard guard;
    revoke_if_asked();
    Obj* object = object_of(a);
    ++world.release_calls;
    world.released.push_back(p);
    end_region(object);
    if (auto hook = g_in_release_critical.take()) {
        WorldUnlocked unlocked;
        hook();
    }
}
// A string's critical: a compressed string's is a copy (no flip count), an uncompressed movable string's raises the count.
const jchar* f_GetStringCritical(Env*, jstring s, jboolean* is_copy) {
    WorldGuard guard;
    on_the_list("GetStringCritical");
    not_under_mutex("GetStringCritical under the mutex");
    Obj* object = object_of(s);
    begin_region(object);
    if (is_copy) *is_copy = object->compressed ? 1 : 0;
    return reinterpret_cast<const jchar*>(object->bytes.data());
}
void f_ReleaseStringCritical(Env*, jstring s, const jchar*) { end_region(object_of(s)); }
void f_DeleteLocalRef(Env*, jobject o) {
    WorldGuard guard;
    on_the_list("DeleteLocalRef");
    Ref* ref = static_cast<Ref*>(o);
    require(world.locals.erase(ref) == 1, "a local reference is deleted once, and only while it is live");
    for (auto& frame : world.frames) {
        for (auto at = frame.begin(); at != frame.end();) at = (*at == ref) ? frame.erase(at) : at + 1;
    }
    delete ref;
}
jobject f_NewGlobalRef(Env*, jobject o) {
    WorldGuard guard;
    on_the_list("NewGlobalRef");
    jni_allowed("NewGlobalRef where JNI is not allowed");
    require(live(o), "NewGlobalRef is handed a live reference, never a shadow");
    if (static_cast<Ref*>(o)->object == nullptr) return nullptr;  // a weak reference whose object was collected
    Ref* ref = new Ref{object_of(o), true};
    (ref->object->is_class ? world.class_globals : world.globals).insert(ref);
    return ref;
}
jobject f_NewWeakGlobalRef(Env*, jobject o) {
    WorldGuard guard;
    on_the_list("NewWeakGlobalRef");
    jni_allowed("NewWeakGlobalRef where JNI is not allowed");
    ++world.weak_new_calls;
    if (world.weak_fails) return nullptr;
    Ref* ref = new Ref{object_of(o), true, true};
    world.weaks.insert(ref);
    return ref;
}
void f_DeleteWeakGlobalRef(Env*, jobject o) {
    WorldGuard guard;
    on_the_list("DeleteWeakGlobalRef");
    require(t_regions == 0, "DeleteWeakGlobalRef inside a critical region");
    Ref* ref = static_cast<Ref*>(o);
    require(world.weaks.erase(ref) == 1, "a weak reference is deleted once, and only while it is live");
    delete ref;
}
void f_ExceptionClear(Env*) {
    WorldGuard guard;
    on_the_list("ExceptionClear");
    world.exception_pending = false;
}
jobject f_ExceptionOccurred(Env*) {
    WorldGuard guard;
    not_under_mutex("ExceptionOccurred is not on the list of calls under the mutex");
    if (!world.exception_pending) return nullptr;
    if (world.pending_object == nullptr) world.pending_object = new Obj{};
    return new_local(world.pending_object);
}
jint f_Throw(Env*, jobject throwable) {
    WorldGuard guard;
    not_under_mutex("Throw under the mutex");
    require(!world.exception_pending, "Throw with an exception pending");
    Obj* object = object_of(throwable);
    world.exception_pending = true;
    world.pending_object = object;
    return 0;
}
// A collection: a weak reference to an object nothing holds is cleared. Held: a strong reference, or Java.
// The weak references the ended blocks hold (the layer's sentinel, once a block has ended, is one more).
size_t block_weaks() {
    WorldGuard guard;
    return world.weaks.size() - (counters().blocks_ended != 0 ? 1 : 0);
}
bool g_collect_ignores_locals = false;  // a row that models a collector that clears a weak reference under a local one
void collect() {
    WorldGuard guard;
    std::set<Obj*> reachable;
    if (!g_collect_ignores_locals) {
        for (Ref* r : world.locals) reachable.insert(r->object);
    }
    for (Ref* r : world.globals) reachable.insert(r->object);
    for (Ref* r : world.weaks) {
        if (r->object != nullptr && !r->object->held && reachable.count(r->object) == 0) r->object = nullptr;
    }
}
Hook<Obj*> g_on_global_deleted;  // runs inside DeleteGlobalRef, before the reference goes
Hook<> g_on_find_class;          // runs inside FindClass: Java code that enters the layer again
bool g_ids_at_install = true;                    // the install gives the layer its environment, which resolves the ids
void f_DeleteGlobalRef(Env*, jobject o) {
    WorldGuard guard;
    on_the_list("DeleteGlobalRef");
    require(t_regions == 0, "DeleteGlobalRef inside a critical region");  // allowed with an exception pending
    Ref* ref = static_cast<Ref*>(o);
    if (auto hook = g_on_global_deleted.peek()) {
        Obj* object = ref->object;
        WorldUnlocked unlocked;
        hook(object);
    }
    require(world.globals.erase(ref) == 1, "a global reference is deleted once, and only while it is live");
    world.revoked.erase(ref);
    delete ref;
}
jobject f_NewLocalRef(Env*, jobject o) {
    WorldGuard guard;
    on_the_list("NewLocalRef");
    jni_allowed("NewLocalRef where JNI is not allowed");
    require(live(o), "NewLocalRef is handed a live reference, never a shadow");
    if (static_cast<Ref*>(o)->object == nullptr) return nullptr;  // a weak reference whose object was collected
    return new_local(object_of(o));
}
jboolean f_IsSameObject(Env*, jobject a, jobject b) {
    WorldGuard guard;
    on_the_list("IsSameObject");
    revoke_if_asked();
    require(t_regions == 0, "IsSameObject inside a critical region");
    ++world.same_calls;
    Obj* x = a == nullptr ? nullptr : object_of(a);
    Obj* y = b == nullptr ? nullptr : object_of(b);
    return x == y;
}
jobject f_GetObjectClass(Env*, jobject o) {
    WorldGuard guard;
    not_under_mutex("GetObjectClass under the mutex");
    (void)object_of(o);
    return new_local(new Obj{});
}
jboolean f_IsInstanceOf(Env*, jobject o, jclass) {
    WorldGuard guard;
    not_under_mutex("IsInstanceOf under the mutex");
    (void)object_of(o);
    return 1;
}
jint f_MonitorEnter(Env*, jobject o) {
    WorldGuard guard;
    not_under_mutex("MonitorEnter (it can wait for another thread) under the mutex");
    revoke_if_asked();
    (void)object_of(o);
    ++world.monitors;
    return 0;
}
jint f_MonitorExit(Env*, jobject o) {
    WorldGuard guard;
    not_under_mutex("MonitorExit under the mutex");
    revoke_if_asked();
    require(o != nullptr, "MonitorExit is handed an object");
    (void)object_of(o);
    --world.monitors;
    if (world.monitor_exit_throws != nullptr) {
        world.exception_pending = true;
        world.pending_object = world.monitor_exit_throws;
    }
    return 0;
}
std::atomic<int> g_ref_type_asked_with_a_reference{0};  // GetObjectRefType calls that reached the runtime with a reference
jint f_GetObjectRefType(Env*, jobject o) {
    WorldGuard guard;
    if (o != nullptr) ++g_ref_type_asked_with_a_reference;
    return 0;  // JNIInvalidRefType: the fake knows no reference of the kinds a row asks about
}
jint f_PushLocalFrame(Env*, jint) {
    WorldGuard guard;
    not_under_mutex("PushLocalFrame under the mutex");
    world.frames.emplace_back();
    return 0;
}
Hook<> g_in_pop;  // runs inside a PopLocalFrame of the runtime, before it pops: another thread acts meanwhile
jobject f_PopLocalFrame(Env*, jobject result) {
    WorldGuard guard;
    not_under_mutex("PopLocalFrame under the mutex");
    require(!world.frames.empty(), "a frame was pushed");
    if (auto hook = g_in_pop.take()) {
        WorldUnlocked unlocked;
        hook();
    }
    Obj* keep = result == nullptr ? nullptr : object_of(result);
    for (Ref* r : world.frames.back()) {
        if (world.locals.erase(r) == 1) delete r;
    }
    world.frames.pop_back();
    return keep == nullptr ? nullptr : new_local(keep);
}
int g_register_fail_at = -1;                            // the call (counted from 0) that fails
std::vector<std::vector<NativeMethod>> g_registered;    // what each call of the runtime's RegisterNatives received
jint f_RegisterNatives(Env*, jclass, const NativeMethod* methods, jint count) {
    WorldGuard guard;
    not_under_mutex("RegisterNatives under the mutex");
    require(count > 0 && methods != nullptr, "the runtime is handed methods");
    g_registered.emplace_back(methods, methods + count);
    return int(g_registered.size()) - 1 == g_register_fail_at ? JNI_ERR : JNI_OK;
}
jboolean f_ExceptionCheck(Env*) {
    WorldGuard guard;
    on_the_list("ExceptionCheck");
    return world.exception_pending;
}
jobject f_FindClass(Env*, const char*) {
    WorldGuard guard;
    jni_allowed("FindClass where JNI is not allowed");
    require(t_ended_depth == 0, "FindClass runs Java: not under the ended lock");
    if (auto hook = g_on_find_class.take()) {
        WorldUnlocked unlocked;
        hook();
    }
    if (world.find_fails) {
        world.exception_pending = true;
        return nullptr;
    }
    Obj* object = new Obj{};
    object->is_class = true;
    return new_local(object);
}
jmethodID f_GetMethodID(Env*, jclass, const char* name, const char* sig) {
    WorldGuard guard;
    jni_allowed("GetMethodID where JNI is not allowed");
    require(t_ended_depth == 0, "a method lookup can run Java: not under the ended lock");
    void* id = new int(0);
    world.methods[id] = Method{sig, name};
    return id;
}
jfieldID f_GetFieldID(Env*, jclass, const char*, const char* sig) {
    WorldGuard guard;
    not_under_mutex("GetFieldID under the mutex");
    void* id = new int(0);
    world.fields[id] = sig;
    return id;
}
jobject f_GetObjectField(Env*, jobject o, jfieldID f) {
    WorldGuard guard;
    not_under_mutex("GetObjectField under the mutex");
    (void)object_of(o);
    require(world.fields.count(f) == 1, "a known field");
    return world.next_result;
}
jobject f_GetStaticObjectField(Env*, jclass, jfieldID f) {
    WorldGuard guard;
    not_under_mutex("GetStaticObjectField under the mutex");
    require(world.fields.count(f) == 1, "a known field");
    return world.next_result;
}

// A call arrives as V (decoded here by the method's signature), A (a jvalue array) or variadic.
std::vector<jvalue> decode_va(const std::string& sig, va_list ap) {
    std::vector<jvalue> args;
    for (size_t i = 1; sig[i] != ')'; ++i) {
        jvalue v;
        v.j = 0;
        switch (sig[i]) {
            case 'Z': v.z = jboolean(va_arg(ap, int)); break;
            case 'B': v.b = jbyte(va_arg(ap, int)); break;
            case 'C': v.c = jchar(va_arg(ap, int)); break;
            case 'S': v.s = jshort(va_arg(ap, int)); break;
            case 'I': v.i = va_arg(ap, jint); break;
            case 'J': v.j = va_arg(ap, jlong); break;
            case 'F': v.f = jfloat(va_arg(ap, double)); break;
            case 'D': v.d = va_arg(ap, double); break;
            case 'L':
                v.l = va_arg(ap, jobject);
                while (sig[i] != ';') ++i;
                break;
            case '[':
                v.l = va_arg(ap, jobject);
                while (sig[i] == '[') ++i;
                if (sig[i] == 'L') while (sig[i] != ';') ++i;
                break;
            default: require(false, "a signature the fake knows");
        }
        args.push_back(v);
    }
    return args;
}
std::vector<jvalue> decode_array(const std::string& sig, const jvalue* in) {
    std::vector<jvalue> args;
    size_t n = 0;
    for (size_t i = 1; sig[i] != ')'; ++i) {
        switch (sig[i]) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D': break;
            case 'L': while (sig[i] != ';') ++i; break;
            case '[':
                while (sig[i] == '[') ++i;
                if (sig[i] == 'L') while (sig[i] != ';') ++i;
                break;
            default: require(false, "a signature the fake knows");
        }
        args.push_back(in[n++]);
    }
    return args;
}

// What the runtime saw: references must be real (a live reference of the fake), never a shadow.
void record(const std::string& call, void* method, const std::vector<jvalue>& args) {
    WorldGuard guard;
    not_under_mutex("a call into Java under the mutex");
    const Method& m = world.methods.at(method);
    world.last_call = call;
    world.last_args = args;
    world.last_array_args.clear();
    world.seen_arrays.clear();
    size_t n = 0;
    for (size_t i = 1; m.sig[i] != ')'; ++i, ++n) {
        if (m.sig[i] == 'L' || m.sig[i] == '[') {
            if (m.sig[i] == '[') {
                while (m.sig[i] == '[') ++i;
                if (m.sig[i] == 'L') while (m.sig[i] != ';') ++i;
            } else {
                while (m.sig[i] != ';') ++i;
            }
            if (args[n].l == nullptr) {
                world.last_array_args.push_back(nullptr);
                continue;
            }
            Obj* object = object_of(args[n].l);
            world.seen_arrays.push_back(object->bytes);  // what Java reads when the call begins
            if (world.java_fill >= 0) std::fill(object->bytes.begin(), object->bytes.end(), uint8_t(world.java_fill));
            world.last_array_args.push_back(object);
        }
    }
    if (world.throw_after_call) world.exception_pending = true;
}
jobject result_of(void* method) {
    WorldGuard guard;
    const Method& m = world.methods.at(method);
    char kind = m.sig[m.sig.find(')') + 1];
    if (kind == 'V' || kind == 'I') return nullptr;
    return world.next_result;
}

jobject f_CallStaticObjectMethodV(Env*, jclass, jmethodID m, va_list ap) {
    WorldGuard guard;
    record("static-object-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_CallStaticObjectMethodA(Env*, jclass, jmethodID m, const jvalue* a) {
    WorldGuard guard;
    if (world.methods.at(m).name == "getRuntime") {  // the layer's own call
        not_under_mutex("Runtime.getRuntime (Java code) under the mutex");
        jni_allowed("Runtime.getRuntime where JNI is not allowed");
        return new_local(new Obj{});
    }
    record("static-object-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallStaticObjectMethod(Env*, jclass, jmethodID, ...) {
    WorldGuard guard;
    require(false, "the variadic entry of the original is not used by the wrappers");
    return nullptr;
}
Hook<> g_in_call;  // runs at the start of a static int call: another thread acts while it is under way
jint f_CallStaticIntMethodV(Env*, jclass, jmethodID m, va_list ap) {
    WorldGuard guard;
    revoke_if_asked();
    if (auto hook = g_in_call.peek()) {
        WorldUnlocked unlocked;
        hook();
    }
    record("static-int-V", m, decode_va(world.methods.at(m).sig, ap));
    return 7;
}
jint f_CallStaticIntMethodA(Env*, jclass, jmethodID m, const jvalue* a) {
    WorldGuard guard;
    if (world.methods.at(m).name != "identityHashCode") revoke_if_asked();
    if (world.methods.at(m).name != "identityHashCode") {
        if (auto hook = g_in_call.peek()) {
            WorldUnlocked unlocked;
            hook();
        }
    }
    if (world.methods.at(m).name == "identityHashCode") {  // the layer's own call
        not_under_mutex("System.identityHashCode (Java code) under the mutex");
        jni_allowed("System.identityHashCode where JNI is not allowed");
        ++world.hash_calls;
        if (world.hash_fails) {
            world.exception_pending = true;
            return 0;
        }
        Obj* object = object_of(a[0].l);
        if (world.same_hash) return 4242;
        if (object->hash == 0) object->hash = world.next_hash++;
        return object->hash;
    }
    record("static-int-A", m, decode_array(world.methods.at(m).sig, a));
    return 7;
}
void f_CallVoidMethodA(Env*, jobject o, jmethodID m, const jvalue*) {
    WorldGuard guard;
    require(world.methods.at(m).name == "gc", "the layer calls Runtime.gc and no other instance method of its own");
    not_under_mutex("Runtime.gc (a collection) under the mutex");
    jni_allowed("Runtime.gc where JNI is not allowed");
    (void)object_of(o);
    ++world.gc_calls;
    collect();
}
void f_CallStaticVoidMethodV(Env*, jclass, jmethodID m, va_list ap) {
    WorldGuard guard;
    record("static-void-V", m, decode_va(world.methods.at(m).sig, ap));
}
void f_CallStaticVoidMethodA(Env*, jclass, jmethodID m, const jvalue* a) {
    WorldGuard guard;
    record("static-void-A", m, decode_array(world.methods.at(m).sig, a));
}
jobject f_NewObjectA(Env*, jclass, jmethodID m, const jvalue* a) {
    WorldGuard guard;
    record("new-A", m, decode_array(world.methods.at(m).sig, a));
    return new_local(new Obj{});
}
// What Java reads and writes through the receiver of an instance call.
bool g_null_receiver_ok = false;  // a row that has the layer replace a receiver by null: the fake notes it and goes on
void touch_receiver(jobject o) {
    WorldGuard guard;
    revoke_if_asked();
    ++world.receiver_calls;
    if (o == nullptr && g_null_receiver_ok) {
        world.last_receiver = nullptr;
        return;
    }
    Obj* object = object_of(o);
    world.last_receiver = object;
    world.seen_receiver.clear();
    if (object->is_array) {
        world.seen_receiver = object->bytes;
        if (world.java_fill >= 0) std::fill(object->bytes.begin(), object->bytes.end(), uint8_t(world.java_fill));
    }
}
jobject f_CallObjectMethodA(Env*, jobject o, jmethodID m, const jvalue* a) {
    WorldGuard guard;
    touch_receiver(o);
    record("object-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallObjectMethodV(Env*, jobject o, jmethodID m, va_list ap) {
    WorldGuard guard;
    touch_receiver(o);
    record("object-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_CallNonvirtualObjectMethodA(Env*, jobject o, jclass, jmethodID m, const jvalue* a) {
    WorldGuard guard;
    touch_receiver(o);
    record("nonvirtual-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallNonvirtualObjectMethodV(Env*, jobject o, jclass, jmethodID m, va_list ap) {
    WorldGuard guard;
    touch_receiver(o);
    record("nonvirtual-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_NewObjectV(Env*, jclass, jmethodID m, va_list ap) {
    WorldGuard guard;
    record("new-V", m, decode_va(world.methods.at(m).sig, ap));
    return new_local(new Obj{});
}
void f_SetObjectField(Env*, jobject o, jfieldID, jobject value) {
    WorldGuard guard;
    not_under_mutex("SetObjectField under the mutex");
    (void)object_of(o);
    if (value != nullptr) (void)object_of(value);
}
void f_SetObjectArrayElement(Env*, jobject, jsize, jobject value) {
    WorldGuard guard;
    not_under_mutex("SetObjectArrayElement under the mutex");
    if (value != nullptr) (void)object_of(value);
}
jobject f_NewObjectArray(Env*, jsize, jclass, jobject initial) {
    WorldGuard guard;
    not_under_mutex("NewObjectArray (an allocation) under the mutex");
    if (initial != nullptr) (void)object_of(initial);
    return new_local(new Obj{});
}

void trap(...) {
    WorldGuard guard;
    ++world.traps;
    require(false, "an original function the test did not provide was called");
}

struct Table {
    void* slots[kSlotCount];
    Env env;
    Table() {
        for (void*& s : slots) s = reinterpret_cast<void*>(&trap);
        slots[slot::GetArrayLength] = reinterpret_cast<void*>(&f_GetArrayLength);
        slots[slot::NewByteArray] = reinterpret_cast<void*>(&f_NewByteArray);
        slots[slot::SetByteArrayRegion] = reinterpret_cast<void*>(&f_SetByteArrayRegion);
        slots[slot::GetByteArrayRegion] = reinterpret_cast<void*>(&f_GetByteArrayRegion);
        slots[slot::GetByteArrayElements] = reinterpret_cast<void*>(&f_GetByteArrayElements);
        slots[slot::ReleaseByteArrayElements] = reinterpret_cast<void*>(&f_ReleaseByteArrayElements);
        slots[slot::GetPrimitiveArrayCritical] = reinterpret_cast<void*>(&f_GetPrimitiveArrayCritical);
        slots[slot::ReleasePrimitiveArrayCritical] = reinterpret_cast<void*>(&f_ReleasePrimitiveArrayCritical);
        slots[slot::GetStringCritical] = reinterpret_cast<void*>(&f_GetStringCritical);
        slots[slot::ReleaseStringCritical] = reinterpret_cast<void*>(&f_ReleaseStringCritical);
        slots[slot::DeleteLocalRef] = reinterpret_cast<void*>(&f_DeleteLocalRef);
        slots[slot::NewGlobalRef] = reinterpret_cast<void*>(&f_NewGlobalRef);
        slots[slot::NewWeakGlobalRef] = reinterpret_cast<void*>(&f_NewWeakGlobalRef);
        slots[slot::DeleteWeakGlobalRef] = reinterpret_cast<void*>(&f_DeleteWeakGlobalRef);
        slots[slot::ExceptionClear] = reinterpret_cast<void*>(&f_ExceptionClear);
        slots[slot::ExceptionOccurred] = reinterpret_cast<void*>(&f_ExceptionOccurred);
        slots[slot::Throw] = reinterpret_cast<void*>(&f_Throw);
        slots[call_slot(kVoid, kArray)] = reinterpret_cast<void*>(&f_CallVoidMethodA);
        slots[slot::DeleteGlobalRef] = reinterpret_cast<void*>(&f_DeleteGlobalRef);
        slots[slot::NewLocalRef] = reinterpret_cast<void*>(&f_NewLocalRef);
        slots[slot::IsSameObject] = reinterpret_cast<void*>(&f_IsSameObject);
        slots[slot::GetObjectClass] = reinterpret_cast<void*>(&f_GetObjectClass);
        slots[slot::IsInstanceOf] = reinterpret_cast<void*>(&f_IsInstanceOf);
        slots[slot::MonitorEnter] = reinterpret_cast<void*>(&f_MonitorEnter);
        slots[slot::MonitorExit] = reinterpret_cast<void*>(&f_MonitorExit);
        slots[slot::GetObjectRefType] = reinterpret_cast<void*>(&f_GetObjectRefType);
        slots[slot::PushLocalFrame] = reinterpret_cast<void*>(&f_PushLocalFrame);
        slots[slot::PopLocalFrame] = reinterpret_cast<void*>(&f_PopLocalFrame);
        slots[slot::RegisterNatives] = reinterpret_cast<void*>(&f_RegisterNatives);
        slots[slot::ExceptionCheck] = reinterpret_cast<void*>(&f_ExceptionCheck);
        slots[slot::FindClass] = reinterpret_cast<void*>(&f_FindClass);
        slots[slot::GetMethodID] = reinterpret_cast<void*>(&f_GetMethodID);
        slots[slot::GetStaticMethodID] = reinterpret_cast<void*>(&f_GetMethodID);
        slots[slot::GetFieldID] = reinterpret_cast<void*>(&f_GetFieldID);
        slots[slot::GetStaticFieldID] = reinterpret_cast<void*>(&f_GetFieldID);
        slots[get_field_slot(kObject)] = reinterpret_cast<void*>(&f_GetObjectField);
        slots[get_static_field_slot(kObject)] = reinterpret_cast<void*>(&f_GetStaticObjectField);
        slots[set_field_slot(kObject)] = reinterpret_cast<void*>(&f_SetObjectField);
        slots[slot::SetObjectArrayElement] = reinterpret_cast<void*>(&f_SetObjectArrayElement);
        slots[slot::NewObjectArray] = reinterpret_cast<void*>(&f_NewObjectArray);
        slots[static_call_slot(kObject, kVa)] = reinterpret_cast<void*>(&f_CallStaticObjectMethodV);
        slots[static_call_slot(kObject, kArray)] = reinterpret_cast<void*>(&f_CallStaticObjectMethodA);
        slots[static_call_slot(kObject, kVarargs)] = reinterpret_cast<void*>(&f_CallStaticObjectMethod);
        slots[static_call_slot(kInt, kVa)] = reinterpret_cast<void*>(&f_CallStaticIntMethodV);
        slots[static_call_slot(kInt, kArray)] = reinterpret_cast<void*>(&f_CallStaticIntMethodA);
        slots[static_call_slot(kVoid, kVa)] = reinterpret_cast<void*>(&f_CallStaticVoidMethodV);
        slots[static_call_slot(kVoid, kArray)] = reinterpret_cast<void*>(&f_CallStaticVoidMethodA);
        slots[call_slot(kObject, kArray)] = reinterpret_cast<void*>(&f_CallObjectMethodA);
        slots[slot::NewObjectA] = reinterpret_cast<void*>(&f_NewObjectA);
        slots[call_slot(kObject, kVa)] = reinterpret_cast<void*>(&f_CallObjectMethodV);
        slots[nonvirtual_slot(kObject, kArray)] = reinterpret_cast<void*>(&f_CallNonvirtualObjectMethodA);
        slots[nonvirtual_slot(kObject, kVa)] = reinterpret_cast<void*>(&f_CallNonvirtualObjectMethodV);
        slots[slot::NewObjectV] = reinterpret_cast<void*>(&f_NewObjectV);
        env.functions = slots;
    }
};

alignas(16) uint8_t g_shadow_region[2u << 20];
alignas(16) uint8_t g_big_region[8u << 20];
uint8_t* g_region = g_shadow_region;  // the memory of the blocks the next install uses
size_t g_region_size = sizeof(g_shadow_region);

template <class F>
F fn(const Table& table, int slot) {
    return reinterpret_cast<F>(table.slots[slot]);
}

void reset_world() {
    g_in_free_baseline = nullptr;
    reset_for_test();
    for (Ref* r : world.locals) delete r;
    for (Ref* r : world.globals) delete r;
    for (Ref* r : world.weaks) delete r;
    for (Ref* r : world.class_globals) delete r;
    world = World{};
    ++g_epoch;
    g_app_caller = true;
    g_writable_ok = true;
    g_read_only_ok = true;
    g_baseline_fails = false;
    g_own_predicate = nullptr;
    g_own_default = true;
    g_no_thread_state = false;
    g_ended_locks = 0;
    g_on_global_deleted = nullptr;
    g_on_find_class = nullptr;
    g_in_call = nullptr;
    g_after_get = nullptr;
    g_after_set = nullptr;
    g_in_pop = nullptr;
    g_before_lock = nullptr;
    g_null_receiver_ok = false;
    g_stop_expected = false;
    g_stops = 0;
    g_ref_type_asked_with_a_reference = 0;
    g_before_first_critical = nullptr;
    g_in_release_critical = nullptr;
    g_collect_ignores_locals = false;
    {
        std::lock_guard<std::mutex> l(g_flip_mutex);
        g_flip_running = false;
        g_open_regions = 0;
        g_gets_that_waited = 0;
    }
    t_regions = 0;
    t_flips = 0;
    t_app_regions = 0;
    g_flip_timeouts = 0;
    g_thunk_capacity = 1024;
    g_register_fail_at = -1;
    g_registered.clear();
}

void* (*g_wrapped_get_critical)(Env*, jarray, jboolean*) = nullptr;
void (*g_wrapped_release_critical)(Env*, jarray, void*, jint) = nullptr;
void* app_get_critical(Env* env, jarray a, jboolean* is_copy) {
    void* pointer = g_wrapped_get_critical(env, a, is_copy);
    t_app_regions = t_regions;
    return pointer;
}
void app_release_critical(Env* env, jarray a, void* p, jint mode) {
    g_wrapped_release_critical(env, a, p, mode);
    t_app_regions = t_regions;
}
const jchar* (*g_wrapped_get_string_critical)(Env*, jstring, jboolean*) = nullptr;
void (*g_wrapped_release_string_critical)(Env*, jstring, const jchar*) = nullptr;
const jchar* app_get_string_critical(Env* env, jstring s, jboolean* is_copy) {
    const jchar* chars = g_wrapped_get_string_critical(env, s, is_copy);
    t_app_regions = t_regions;
    return chars;
}
void app_release_string_critical(Env* env, jstring s, const jchar* chars) {
    g_wrapped_release_string_critical(env, s, chars);
    t_app_regions = t_regions;
}

// Installs over a fresh fake table.
Table* install(bool shadows = true, unsigned thunks = 1024) {
    reset_world();
    Table* table = new Table;
    Platform platform = kPlatform;
    platform.thunk_capacity = thunks;
    if (g_without_own_check) {
        platform.is_own_library = nullptr;
    }
    if (g_without_describe) {
        platform.describe_method = nullptr;
    }
    g_record_expected_bytes = shadow_record_bytes(g_region_size);
    require(install_jni_wrappers(table->slots, platform, shadows ? g_region : nullptr, shadows ? g_region_size : 0,
                                 g_ids_at_install ? &table->env : nullptr),
            "the wrappers install");
    // The application's critical calls go through trampolines that count the regions it holds afterwards: whatever else is
    // open while the layer runs is the layer's own.
    g_wrapped_get_critical = reinterpret_cast<void* (*)(Env*, jarray, jboolean*)>(table->slots[slot::GetPrimitiveArrayCritical]);
    g_wrapped_release_critical = reinterpret_cast<void (*)(Env*, jarray, void*, jint)>(table->slots[slot::ReleasePrimitiveArrayCritical]);
    table->slots[slot::GetPrimitiveArrayCritical] = reinterpret_cast<void*>(&app_get_critical);
    table->slots[slot::ReleasePrimitiveArrayCritical] = reinterpret_cast<void*>(&app_release_critical);
    g_wrapped_get_string_critical = reinterpret_cast<const jchar* (*)(Env*, jstring, jboolean*)>(table->slots[slot::GetStringCritical]);
    g_wrapped_release_string_critical = reinterpret_cast<void (*)(Env*, jstring, const jchar*)>(table->slots[slot::ReleaseStringCritical]);
    table->slots[slot::GetStringCritical] = reinterpret_cast<void*>(&app_get_string_critical);
    table->slots[slot::ReleaseStringCritical] = reinterpret_cast<void*>(&app_release_string_critical);
    return table;
}

void check_slot_constants(const char* fixture_path) {
    std::ifstream file(fixture_path);
    require(file.is_open(), "the slot fixture opens");
    std::map<std::string, int> env_slots;
    std::map<std::string, int> vm_slots;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string table, name;
        int index;
        words >> table >> index >> name;
        (table == "env" ? env_slots : vm_slots)[name] = index;
    }
    require(env_slots.size() == size_t(kSlotCount), "the fixture lists every slot of the table");
    auto expect = [&](const std::string& name, int index) {
        const auto found = env_slots.find(name);
        require(found != env_slots.end() && found->second == index, name.c_str());
    };
    const char* names[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
    const char* variants[] = {"", "V", "A"};
    for (int t = 0; t < 10; ++t) {
        for (int v = 0; v < 3; ++v) {
            expect(std::string("Call") + names[t] + "Method" + variants[v], call_slot(Type(t), Variant(v)));
            expect(std::string("CallNonvirtual") + names[t] + "Method" + variants[v], nonvirtual_slot(Type(t), Variant(v)));
            expect(std::string("CallStatic") + names[t] + "Method" + variants[v], static_call_slot(Type(t), Variant(v)));
        }
    }
    for (int t = 0; t < 9; ++t) {
        expect(std::string("Get") + names[t] + "Field", get_field_slot(Type(t)));
        expect(std::string("Set") + names[t] + "Field", set_field_slot(Type(t)));
        expect(std::string("GetStatic") + names[t] + "Field", get_static_field_slot(Type(t)));
        expect(std::string("SetStatic") + names[t] + "Field", set_static_field_slot(Type(t)));
    }
    for (int t = 1; t < 9; ++t) {
        expect(std::string("New") + names[t] + "Array", new_array_slot(Type(t)));
        expect(std::string("Get") + names[t] + "ArrayElements", get_array_elements_slot(Type(t)));
        expect(std::string("Release") + names[t] + "ArrayElements", release_array_elements_slot(Type(t)));
        expect(std::string("Get") + names[t] + "ArrayRegion", get_array_region_slot(Type(t)));
        expect(std::string("Set") + names[t] + "ArrayRegion", set_array_region_slot(Type(t)));
    }
    expect("GetVersion", slot::GetVersion);
    expect("FindClass", slot::FindClass);
    expect("GetSuperclass", slot::GetSuperclass);
    expect("Throw", slot::Throw);
    expect("ThrowNew", slot::ThrowNew);
    expect("DeleteWeakGlobalRef", slot::DeleteWeakGlobalRef);
    expect("ExceptionClear", slot::ExceptionClear);
    expect("ExceptionOccurred", slot::ExceptionOccurred);
    expect("PushLocalFrame", slot::PushLocalFrame);
    expect("PopLocalFrame", slot::PopLocalFrame);
    expect("RegisterNatives", slot::RegisterNatives);
    expect("NewGlobalRef", slot::NewGlobalRef);
    expect("DeleteGlobalRef", slot::DeleteGlobalRef);
    expect("DeleteLocalRef", slot::DeleteLocalRef);
    expect("IsSameObject", slot::IsSameObject);
    expect("NewLocalRef", slot::NewLocalRef);
    expect("EnsureLocalCapacity", slot::EnsureLocalCapacity);
    expect("NewObject", slot::NewObject);
    expect("NewObjectV", slot::NewObjectV);
    expect("NewObjectA", slot::NewObjectA);
    expect("GetObjectClass", slot::GetObjectClass);
    expect("IsInstanceOf", slot::IsInstanceOf);
    expect("GetMethodID", slot::GetMethodID);
    expect("GetFieldID", slot::GetFieldID);
    expect("GetStaticMethodID", slot::GetStaticMethodID);
    expect("GetStaticFieldID", slot::GetStaticFieldID);
    expect("NewString", slot::NewString);
    expect("NewStringUTF", slot::NewStringUTF);
    expect("GetArrayLength", slot::GetArrayLength);
    expect("NewObjectArray", slot::NewObjectArray);
    expect("GetObjectArrayElement", slot::GetObjectArrayElement);
    expect("SetObjectArrayElement", slot::SetObjectArrayElement);
    expect("GetPrimitiveArrayCritical", slot::GetPrimitiveArrayCritical);
    expect("ReleasePrimitiveArrayCritical", slot::ReleasePrimitiveArrayCritical);
    expect("NewWeakGlobalRef", slot::NewWeakGlobalRef);
    expect("ExceptionCheck", slot::ExceptionCheck);
    expect("GetObjectRefType", slot::GetObjectRefType);
    expect("MonitorEnter", slot::MonitorEnter);
    expect("MonitorExit", slot::MonitorExit);
    expect("ToReflectedMethod", slot::ToReflectedMethod);
    expect("GetStringUTFChars", slot::GetStringUTFChars);
    expect("ReleaseStringUTFChars", slot::ReleaseStringUTFChars);
    require(vm_slots.at("GetEnv") == vmslot::GetEnv && vm_slots.at("AttachCurrentThread") == vmslot::AttachCurrentThread &&
                vm_slots.at("AttachCurrentThreadAsDaemon") == vmslot::AttachCurrentThreadAsDaemon &&
                vm_slots.at("DestroyJavaVM") == vmslot::DestroyJavaVM &&
                vm_slots.at("DetachCurrentThread") == vmslot::DetachCurrentThread,
            "the invoke interface slots");
}


// ---- helpers for the scenarios

struct Fixture {
    Table* table;
    Env* env;
    jclass cls;
    jmethodID get_bytes;        // ()[B  (static)
    jmethodID consume;          // ([B)I (static)
    jmethodID mixed;            // (IJLjava/lang/String;[BDF)I (static)
    jmethodID consume_void;     // (Z[B)V
    jmethodID instance_bytes;   // ()[B (instance)
    jmethodID construct;        // ([B)V
    jmethodID not_bytes;        // ()Ljava/lang/Object;
    jmethodID many;             // twenty ints and a byte[]: more parameters than a fixed array of sixteen
    jfieldID bytes_field;       // [B
    jfieldID other_field;       // Ljava/lang/String;
};

Fixture make(bool shadows = true, unsigned thunks = 1024) {
    Fixture f;
    f.table = install(shadows, thunks);
    f.env = &f.table->env;
    f.cls = new_local(new Obj{});
    auto get_static = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetStaticMethodID);
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    auto get_field = fn<jfieldID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetFieldID);
    f.get_bytes = get_static(f.env, f.cls, "bytes", "()[B");
    f.consume = get_static(f.env, f.cls, "consume", "([B)I");
    f.mixed = get_static(f.env, f.cls, "mixed", "(IJLjava/lang/String;[BDF)I");
    f.consume_void = get_static(f.env, f.cls, "consumeVoid", "(Z[B)V");
    f.instance_bytes = get_method(f.env, f.cls, "ibytes", "()[B");
    f.construct = get_method(f.env, f.cls, "<init>", "([B)V");
    f.not_bytes = get_static(f.env, f.cls, "other", "()Ljava/lang/Object;");
    f.many = get_static(f.env, f.cls, "many", "(IIIIIIIIIIIIIIIIIIII[B)V");
    f.bytes_field = get_field(f.env, f.cls, "data", "[B");
    f.other_field = get_field(f.env, f.cls, "name", "Ljava/lang/String;");
    return f;
}

std::vector<uint8_t> sample(size_t n) {
    std::vector<uint8_t> bytes(n);
    for (size_t i = 0; i < n; ++i) bytes[i] = uint8_t(0x30 + i % 40);
    return bytes;
}

// The wrappers' variadic entry: the original CallStaticObjectMethod must not be needed by them.
jobject call_static_object_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jobject r = fn<jobject (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kObject, kVa))(f.env, f.cls, m, ap);
    va_end(ap);
    return r;
}
jint call_static_int_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jint r = fn<jint (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kInt, kVa))(f.env, f.cls, m, ap);
    va_end(ap);
    return r;
}
jobject call_object_v(Fixture& f, jobject receiver, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jobject r = fn<jobject (*)(Env*, jobject, jmethodID, va_list)>(*f.table, call_slot(kObject, kVa))(f.env, receiver, m, ap);
    va_end(ap);
    return r;
}
void call_static_void_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    fn<void (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kVoid, kVa))(f.env, f.cls, m, ap);
    va_end(ap);
}

// A byte[] result for application code: the shadow, and the object behind it.
struct Made {
    jobject shadow;
    Obj* object;
};
Made make_shadow_of(Fixture& f, const std::vector<uint8_t>& payload) {
    jobject array = byte_array(payload);
    Obj* object = object_of(array);
    world.next_result = array;
    return Made{call_static_object_v(f, f.get_bytes), object};
}

uint8_t* raw_of(const Made& made) { return const_cast<uint8_t*>(static_cast<const uint8_t*>(made.shadow)) + 16; }
jobject string_object() { return new_local(new Obj{}); }

// ---- scenarios

void check_shadow_layout_and_use() {
    Fixture f = make();
    const auto payload = sample(100);
    jobject array = byte_array(payload);
    Obj* object = object_of(array);
    world.next_result = array;
    const size_t locals_before = world.locals.size();
    jobject shadow = call_static_object_v(f, f.get_bytes);
    require(shadow != nullptr && is_shadow_block(shadow), "a byte[] result for application code is a shadow");
    require(world.locals.size() == locals_before - 1, "the real local reference was released");
    require(world.globals.size() == 1, "the block keeps a global reference to the array");
    require(counters().shadows_created == 1 && counters().shadows_live == 1, "one shadow");
    const uint8_t* raw = static_cast<const uint8_t*>(shadow);
    uint32_t length;
    std::memcpy(&length, raw + 8, 4);
    require(length == 100, "length at +8, as in Dalvik's ArrayObject");
    require(std::memcmp(raw + 0x10, payload.data(), 100) == 0, "the bytes start at +0x10");
    for (int i = 0; i < 16; ++i) require(raw[0x10 + 100 + i] == 0, "zeroes behind the data");
    require((reinterpret_cast<uintptr_t>(shadow) & 15u) == 0, "16-byte aligned");

    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 100, "GetArrayLength");
    jboolean copy = 1;
    jbyte* elements = fn<jbyte* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetByteArrayElements)(f.env, shadow, &copy);
    require(elements == reinterpret_cast<const jbyte*>(raw + 0x10) && copy == 1,
            "the elements are the block, which is separate storage, and the flag says so");
    auto release = fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements);
    elements[0] = 99;
    release(f.env, shadow, elements, kReleaseAbort);
    require(object->bytes[0] == payload[0] && raw[0x10] == payload[0],
            "JNI_ABORT leaves the array as it was, and the block shows it again");
    elements[0] = 99;
    release(f.env, shadow, elements, kReleaseCommit);
    require(object->bytes[0] == 99, "a commit copies the block back");
    elements[1] = 98;
    release(f.env, shadow, elements, 0);
    require(object->bytes[1] == 98 && object->bytes[2] == payload[2], "a release copies the block back");

    jbyte buffer[10];
    fn<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 5, 10, buffer);
    require(std::memcmp(buffer, payload.data() + 5, 10) == 0, "GetByteArrayRegion reads the block");
    jbyte patch[3] = {1, 2, 3};
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, shadow, 0, 3, patch);
    require(raw[0x10] == 1 && raw[0x12] == 3, "SetByteArrayRegion writes the block");
    require(object->bytes[0] == 1 && object->bytes[2] == 3, "and the array");
    require(!world.exception_pending, "in range, no exception");
    const size_t locals = world.locals.size();
    fn<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 95, 10, buffer);
    require(world.exception_pending, "out of range, the runtime raises it for the real array");
    world.exception_pending = false;
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, shadow, 95, 10, patch);
    require(world.exception_pending && raw[0x10 + 95] == payload[95], "and for a write; the block is not changed");
    world.exception_pending = false;
    require(world.locals.size() == locals, "no temporary reference exists after either");

    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::GetObjectRefType)(f.env, shadow) == kRefTypeLocal, "a local reference");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, shadow) == 1, "same");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, f.cls) == 0, "different");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, nullptr) == 0, "not null");

    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow);
    require(counters().shadows_live == 0 && world.globals.empty(), "DeleteLocalRef ended the block and its global reference");
    require(!is_shadow_block(shadow), "an ended block is no live shadow");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 100,
            "an ended block still knows its length (its reference may be kept)");
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow);  // a second delete is ignored
    require(counters().shadows_live == 0, "and a second delete changes nothing");
}

// The array a shadow stands for keeps its identity, and the ordinary JNI writes reach it.
void check_identity_and_writes() {
    Fixture f = make();
    const auto payload = sample(8);
    Made made = make_shadow_of(f, payload);
    require(is_shadow_block(made.shadow), "a shadow");
    jobject java_side = new_local(made.object);  // the reference the Java side keeps to the same array
    auto same = fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject);
    require(same(f.env, made.shadow, java_side) == 1, "IsSameObject(shadow, the array) is true");
    require(same(f.env, java_side, made.shadow) == 1, "in either order");
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow);
    require(world.globals.count(static_cast<Ref*>(global)) && object_of(global) == made.object,
            "NewGlobalRef(shadow) refers to the same object, not a copy");
    require(same(f.env, global, java_side) == 1 && same(f.env, made.shadow, global) == 1, "and compares equal to both");
    jobject weak = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewWeakGlobalRef)(f.env, made.shadow);
    require(object_of(weak) == made.object, "NewWeakGlobalRef(shadow) refers to the same object");
    jobject local = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewLocalRef)(f.env, made.shadow);
    require(!is_shadow_block(local) && object_of(local) == made.object, "NewLocalRef(shadow) is a real reference to the same object");
    jobject cls = fn<jobject (*)(Env*, jobject)>(*f.table, slot::GetObjectClass)(f.env, made.shadow);
    require(world.locals.count(static_cast<Ref*>(cls)), "GetObjectClass(shadow)");
    require(fn<jboolean (*)(Env*, jobject, jclass)>(*f.table, slot::IsInstanceOf)(f.env, made.shadow, f.cls) == 1, "IsInstanceOf(shadow)");
    require(counters().conversions >= 7, "each of those was a conversion");

    jbyte one = 77;
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, made.shadow, 3, 1, &one);
    require(made.object->bytes[3] == 77, "SetByteArrayRegion reaches the array");
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    auto* bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    require(bytes == reinterpret_cast<const jbyte*>(static_cast<const uint8_t*>(made.shadow) + 16), "the critical region is the block");
    bytes[0] = 88;
    release_critical(f.env, made.shadow, bytes, kReleaseAbort);
    require(made.object->bytes[0] == payload[0], "an aborted critical region leaves the array");
    bytes[0] = 88;
    release_critical(f.env, made.shadow, bytes, 0);
    require(made.object->bytes[0] == 88, "a released critical region reaches the array");
    // A buffer that is not the block (the application copied the elements) is what Release writes back.
    jbyte mine[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, made.shadow, mine, 0);
    require(made.object->bytes[7] == 9 && static_cast<const uint8_t*>(made.shadow)[16 + 7] == 9,
            "the application's own buffer is written back");

    // The block's global reference goes with the block, and only then.
    const size_t globals = world.globals.size();
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, made.shadow);
    require(world.globals.size() == globals - 1, "deleting the shadow deletes its own global reference");
    require(world.globals.count(static_cast<Ref*>(global)), "and not the one made from it");
    require(object_of(global) == made.object, "which still refers to the array");
}

void check_platform_caller_sees_nothing() {
    Fixture f = make();
    g_app_caller = false;
    const auto payload = sample(40);
    world.next_result = byte_array(payload);
    jobject result = call_static_object_v(f, f.get_bytes);
    require(!is_shadow_block(result) && world.locals.count(static_cast<Ref*>(result)), "the platform gets the real reference");
    require(counters().shadows_created == 0 && world.globals.empty(), "no shadow made");
}

void check_shadow_passed_back() {
    Fixture f = make();
    const auto payload = sample(64);
    Made made = make_shadow_of(f, payload);
    require(is_shadow_block(made.shadow), "a shadow");
    const size_t locals = world.locals.size();
    const size_t globals = world.globals.size();

    require(call_static_int_v(f, f.consume, made.shadow) == 7, "consume(V)");
    require(world.last_call == "static-int-A" && world.last_array_args.size() == 1 && world.last_array_args[0] == made.object,
            "the runtime was handed the array the shadow stands for, through the array form");
    require(world.locals.size() == locals && world.globals.size() == globals, "no temporary reference was made");

    jvalue args[1];
    args[0].l = made.shadow;
    require(fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, f.consume, args) == 7,
            "consume(A)");
    require(world.last_call == "static-int-A" && world.last_array_args[0] == made.object, "the same through the array form");
    require(args[0].l == made.shadow, "the caller's own array is not modified");

    jobject text = string_object();
    require(call_static_int_v(f, f.mixed, jint(-5), jlong(0x123456789abcdefLL), text, made.shadow, 2.5, 1.5) == 7, "mixed");
    require(world.last_args.size() == 6, "six arguments");
    require(world.last_args[0].i == -5 && world.last_args[1].j == 0x123456789abcdefLL && world.last_args[2].l == text &&
                world.last_args[4].d == 2.5 && world.last_args[5].f == 1.5f,
            "the other arguments arrive unchanged, in order and width");
    require(world.last_array_args.size() == 2 && world.last_array_args[1] == made.object, "the shadow became its array");

    jobject plain = byte_array(sample(8));
    require(call_static_int_v(f, f.consume, plain) == 7, "a real array passes through");
    require(world.last_args[0].l == plain, "unchanged");

    auto void_v = fn<void (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kVoid, kVa));
    auto call_void = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        void_v(f.env, f.cls, m, ap);
        va_end(ap);
    };
    call_void(f.consume_void, 1, made.shadow);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object, "void call");
    auto new_object = fn<jobject (*)(Env*, jclass, jmethodID, va_list)>(*f.table, slot::NewObjectV);
    auto construct = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = new_object(f.env, f.cls, m, ap);
        va_end(ap);
        return r;
    };
    jobject built = construct(f.construct, made.shadow);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object && world.locals.count(static_cast<Ref*>(built)),
            "a constructor given a shadow");

    fn<void (*)(Env*, jobject, jfieldID, jobject)>(*f.table, set_field_slot(kObject))(f.env, f.cls, f.other_field, made.shadow);
    fn<void (*)(Env*, jobject, jsize, jobject)>(*f.table, slot::SetObjectArrayElement)(f.env, f.cls, 0, made.shadow);
    fn<jobject (*)(Env*, jsize, jclass, jobject)>(*f.table, slot::NewObjectArray)(f.env, 4, f.cls, made.shadow);
    fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, made.shadow, f.other_field);
}

// Whoever calls: a platform helper that is handed a shadow turns it back too.
void check_conversion_for_any_caller() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    g_app_caller = false;
    require(call_static_int_v(f, f.consume, made.shadow) == 7, "consume as a platform caller");
    require(world.last_array_args[0] == made.object, "the platform's call got the real array");
    require(object_of(fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow)) == made.object,
            "NewGlobalRef as a platform caller");
    Made again = make_shadow_of(f, sample(4));
    require(!is_shadow_block(again.shadow), "but the platform is not given a shadow");
    require(counters().shadows_created == 1, "none was made for it");
}

void check_monitor() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorEnter)(f.env, made.shadow) == 0 && world.monitors == 1,
            "MonitorEnter reaches the runtime with the real array");
    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorExit)(f.env, made.shadow) == 0 && world.monitors == 0, "MonitorExit");
}

// A method ID the layer never saw looked up, and a method with more parameters than a fixed array holds.
void check_unknown_and_long_signatures() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    void* unknown = new int(0);
    world.methods[unknown] = Method{"(I[B)V", "unknown"};
    call_static_void_v(f, unknown, 5, made.shadow);
    require(world.describe_calls == 1, "the signature of an unknown ID is asked for once");
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object, "and the shadow still became its array");
    jvalue args[2];
    args[0].i = 1;
    args[1].l = made.shadow;
    fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, unknown, args);
    require(world.describe_calls == 1 && world.last_array_args[0] == made.object, "the answer is kept (array form)");

    // Twenty ints and a byte[] (more than sixteen parameters), the shadow last.
    call_static_void_v(f, f.many, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, made.shadow);
    require(world.last_args.size() == 21 && world.last_args[19].i == 20 && world.last_array_args.size() == 1 &&
                world.last_array_args[0] == made.object,
            "twenty-one parameters: the shadow is converted (V)");
    jvalue many[21];
    for (int i = 0; i < 20; ++i) many[i].i = i + 1;
    many[20].l = made.shadow;
    fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, f.many, many);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object && many[20].l == made.shadow,
            "twenty-one parameters: the shadow is converted (A), the caller's array is untouched");
}

void check_fields_and_other_results() {
    Fixture f = make();
    const auto payload = sample(30);
    world.next_result = byte_array(payload);
    jobject receiver = byte_array(sample(3));
    auto instance_v = fn<jobject (*)(Env*, jobject, jmethodID, va_list)>(*f.table, call_slot(kObject, kVa));
    auto call_instance = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = instance_v(f.env, receiver, m, ap);
        va_end(ap);
        return r;
    };
    jobject shadow = call_instance(f.instance_bytes);
    require(is_shadow_block(shadow), "an instance method that returns byte[]");
    world.next_result = byte_array(payload);
    jobject field = fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, receiver, f.bytes_field);
    require(is_shadow_block(field), "a byte[] field read");
    world.next_result = byte_array(payload);
    jobject static_field =
        fn<jobject (*)(Env*, jclass, jfieldID)>(*f.table, get_static_field_slot(kObject))(f.env, f.cls, f.bytes_field);
    require(is_shadow_block(static_field), "a static byte[] field read");
    world.next_result = byte_array(payload);
    jobject other = fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, receiver, f.other_field);
    require(!is_shadow_block(other), "a String field is the real reference");
    world.next_result = byte_array(payload);
    jobject obj = call_static_object_v(f, f.not_bytes);
    require(!is_shadow_block(obj), "the declared type decides");
    // NewByteArray is not a source of shadows: the application made that array itself.
    jobject fresh = fn<jobject (*)(Env*, jsize)>(*f.table, slot::NewByteArray)(f.env, 4);
    require(!is_shadow_block(fresh) && world.locals.count(static_cast<Ref*>(fresh)), "NewByteArray gives the real reference");
}

// Java writes into the array a block stands for: the block follows it where the application asks for the bytes, and
// after a call that was passed the block.
using GetRegion = void (*)(Env*, jarray, jsize, jsize, jbyte*);
using GetElems = jbyte* (*)(Env*, jarray, jboolean*);
using RelElems = void (*)(Env*, jarray, jbyte*, jint);

// The call that was passed the block fills the array: the raw pointer shows it at once.
void check_block_follows_a_call() {
    Fixture f = make();
    Made passed = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint8_t* raw = static_cast<const uint8_t*>(passed.shadow) + 16;
    world.java_fill = 0x41;
    require(call_static_int_v(f, f.consume, passed.shadow) == 7, "consume");
    require(raw[0] == 0x41 && raw[7] == 0x41, "(variadic form) the block shows what Java wrote during the call");
    std::fill(passed.object->bytes.begin(), passed.object->bytes.end(), uint8_t(0));
    jvalue args[1];
    args[0].l = passed.shadow;
    world.java_fill = 0x42;
    require(fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, f.consume, args) == 7,
            "consume(A)");
    require(raw[0] == 0x42 && raw[7] == 0x42, "(array form) the same");
    world.java_fill = 0x43;
    jobject built = fn<jobject (*)(Env*, jclass, jmethodID, ...)>(*f.table, slot::NewObject)(f.env, f.cls, f.construct, passed.shadow);
    require(built != nullptr && raw[0] == 0x43, "(NewObject) the same");
    world.java_fill = -1;
}

// Java writes later (another thread, a callback): every way to ask for the bytes shows it, and a release of a
// one-byte change keeps what Java wrote.
void check_block_follows_later_writes() {
    Fixture f = make();
    Made later = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint8_t* later_raw = static_cast<const uint8_t*>(later.shadow) + 16;
    std::fill(later.object->bytes.begin(), later.object->bytes.end(), uint8_t(0x41));
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, later.shadow, 0, 8, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0x41 && static_cast<uint8_t>(buffer[7]) == 0x41 && later_raw[0] == 0x41,
            "GetByteArrayRegion gives Java's bytes, and the block takes them");
    std::fill(later.object->bytes.begin(), later.object->bytes.end(), uint8_t(0x44));
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, later.shadow, nullptr);
    require(static_cast<uint8_t>(elements[0]) == 0x44 && static_cast<uint8_t>(elements[7]) == 0x44,
            "GetByteArrayElements gives Java's bytes");
    elements[0] = 0x01;
    fn<RelElems>(*f.table, slot::ReleaseByteArrayElements)(f.env, later.shadow, elements, 0);
    require(later.object->bytes[0] == 0x01 && later.object->bytes[5] == 0x44, "Release(0) keeps the bytes Java wrote");
}

// JNI_ABORT: the change is gone from the array and from the block.
void check_abort_gives_the_arrays_bytes_back() {
    Fixture f = make();
    Made aborted = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, aborted.shadow, nullptr);
    elements[0] = 0x77;
    fn<RelElems>(*f.table, slot::ReleaseByteArrayElements)(f.env, aborted.shadow, elements, kReleaseAbort);
    jbyte one = 0;
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, aborted.shadow, 0, 1, &one);
    require(static_cast<uint8_t>(one) == 0x10 && aborted.object->bytes[0] == 0x10, "after JNI_ABORT the bytes are Java's");
}

// A call that raised an exception leaves the block as it was (no JNI call runs with an exception pending); the next
// request for the bytes shows what Java wrote.
void check_exception_leaves_the_block() {
    Fixture f = make();
    Made thrown = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint8_t* thrown_raw = static_cast<const uint8_t*>(thrown.shadow) + 16;
    world.java_fill = 0x55;
    world.throw_after_call = true;
    call_static_int_v(f, f.consume, thrown.shadow);
    world.java_fill = -1;
    world.throw_after_call = false;
    require(world.exception_pending && thrown_raw[0] == 0, "an exception pending: nothing was called to refresh");
    world.exception_pending = false;
    jbyte one = 0;
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, thrown.shadow, 0, 1, &one);
    require(static_cast<uint8_t>(one) == 0x55 && thrown_raw[0] == 0x55, "and the next request shows Java's bytes");
}

// The critical pair runs inside another array's critical region: only the real array's critical pair is called.
void check_critical_pair_inside_a_region() {
    Fixture f = make();
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    std::fill(made.object->bytes.begin(), made.object->bytes.end(), uint8_t(0x42));  // what Java wrote
    jobject plain = byte_array(sample(4));
    void* pinned = critical(f.env, plain, nullptr);
    require(world.critical == 1, "a critical region of a real array is open");
    auto* bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    require(static_cast<uint8_t>(bytes[0]) == 0x42, "the critical pointer shows what Java wrote");
    release_critical(f.env, made.shadow, bytes, kReleaseAbort);
    raw_of(made)[5] = 0x99;  // written through the pointer, never released
    bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    require(static_cast<uint8_t>(bytes[5]) == 0x99 && made.object->bytes[5] == 0x99,
            "a raw write reaches the array through the critical pair");
    bytes[1] = 0x66;
    release_critical(f.env, made.shadow, bytes, 0);
    require(made.object->bytes[1] == 0x66 && made.object->bytes[2] == 0x42, "and its release reaches the array");
    bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    bytes[3] = 0x11;
    release_critical(f.env, made.shadow, bytes, kReleaseAbort);
    require(made.object->bytes[3] == 0x42 && static_cast<const uint8_t*>(made.shadow)[16 + 3] == 0x42,
            "an aborted release gives the block the array's bytes back");
    release_critical(f.env, plain, pinned, 0);
    require(world.critical == 0, "the real region closed");
}

// The blocks of ended threads wait for a wrapped call outside any critical region.
void check_critical_entries_do_not_drain() {
    Fixture f = make();
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    std::thread other([&] {
        for (int i = 0; i < 2; ++i) require(is_shadow_block(make_shadow_of(f, sample(8)).shadow), "a shadow");
        release_thread_state(t_block);
    });
    other.join();
    jobject plain = byte_array(sample(4));
    void* pinned = critical(f.env, plain, nullptr);
    const size_t globals = world.globals.size();
    auto* bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    release_critical(f.env, made.shadow, bytes, 0);
    require(world.critical == 1 && world.globals.size() == globals && counters().shadows_released == 0,
            "inside the region no global reference was deleted, and nothing was drained");
    release_critical(f.env, plain, pinned, 0);
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_released == 2, "the next wrapped call outside a region drained them");
}

// On Dalvik the application's pointer and Java saw one memory. A block and its array are two copies kept alike by
// synchronizing them: bytes the application wrote through the raw pointer and never released reach the array before a
// call that is passed it, and are still there afterwards.

void check_raw_write_reaches_a_call() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;
    raw[1] = 0xAB;
    world.set_calls = 0;
    world.critical_gets = 0;
    require(call_static_int_v(f, f.consume, made.shadow) == 7, "consume");
    require(world.seen_arrays.size() == 1 && world.seen_arrays[0][0] == 0xAA && world.seen_arrays[0][1] == 0xAB,
            "Java read what the application wrote through the pointer");
    require(raw[0] == 0xAA && raw[1] == 0xAB && made.object->bytes[0] == 0xAA, "and the bytes are still in both copies");
    require(world.critical_gets == 1 && world.set_calls == 0, "one chunk went to the array, in place: one critical pair, no region write");
    call_static_int_v(f, f.consume, made.shadow);
    require(world.critical_gets == 1, "a second call with nothing written makes no critical get");
}

// Java writes during the call and the application reads the pointer afterwards (the case MR1 fixed), while an earlier
// raw write stays.
void check_raw_write_and_java_write_in_one_call() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;
    world.java_fill = 0x41;
    call_static_int_v(f, f.consume, made.shadow);
    world.java_fill = -1;
    require(world.seen_arrays[0][0] == 0xAA, "Java saw the raw write when the call began");
    require(raw[0] == 0x41 && raw[7] == 0x41 && made.object->bytes[0] == 0x41, "and what Java wrote is in the block after it");
}

// Both sides changed the same chunk since the last synchronization: each side's bytes survive in it, and where both
// wrote the same byte the application's wins (the merge is byte by byte, in every synchronization).
void check_both_sides_changed_one_chunk_both_survive() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;                // the application, through the pointer
    raw[2] = 0xAD;                // the application, the byte Java also wrote
    made.object->bytes[1] = 0xBB;  // Java, in the same chunk
    made.object->bytes[2] = 0xBD;
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 8, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0xAA && made.object->bytes[0] == 0xAA, "the application's byte reached the array");
    require(static_cast<uint8_t>(buffer[1]) == 0xBB && made.object->bytes[1] == 0xBB && raw[1] == 0xBB,
            "Java's byte in that chunk survived, in the block too");
    require(static_cast<uint8_t>(buffer[2]) == 0xAD && made.object->bytes[2] == 0xAD, "where both wrote one byte, the application's wins");
    require(raw[3] == 0x10 && made.object->bytes[3] == 0x10, "the others are unchanged");
    // The same through a call: the application's chunk is pushed before it, merged with what Java holds.
    raw[4] = 0xC4;
    made.object->bytes[5] = 0xC5;
    call_static_int_v(f, f.consume, made.shadow);
    require(world.seen_arrays.back()[4] == 0xC4 && world.seen_arrays.back()[5] == 0xC5, "Java saw both writes when the call began");
    require(raw[4] == 0xC4 && raw[5] == 0xC5, "and the block holds both");
}

// The merge decides four bytes at once where it can; every word shape gives the byte-by-byte result: the application wrote
// the whole word, Java did, both did (different bytes, and the same byte), and the last bytes that fill no word.
void check_the_merge_by_words_is_byte_exact() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(4099, 0x10));
    uint8_t* raw = raw_of(made);
    for (unsigned i = 0; i < 4; ++i) {
        raw[0 + i] = static_cast<uint8_t>(0xA0 + i);  // word 0: only the application wrote
        made.object->bytes[4 + i] = static_cast<uint8_t>(0xB0 + i);  // word 1: only Java wrote
    }
    raw[8] = 0xC0;  // word 2: the application wrote the first and third bytes, Java the second and the third
    raw[10] = 0xC2;
    made.object->bytes[9] = 0xD1;
    made.object->bytes[10] = 0xD2;
    raw[12] = 0xE0;  // word 3: both wrote the first byte
    made.object->bytes[12] = 0xE1;
    raw[4096] = 0xF0;  // the three bytes after the last whole word of the block
    raw[4098] = 0xF2;
    made.object->bytes[4097] = 0xF1;
    made.object->bytes[4098] = 0xF3;
    jbyte buffer[16] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 16, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0xA0 && static_cast<uint8_t>(buffer[3]) == 0xA3 && made.object->bytes[3] == 0xA3,
            "the application's whole word reached the array");
    require(static_cast<uint8_t>(buffer[4]) == 0xB0 && static_cast<uint8_t>(buffer[7]) == 0xB3 && raw[7] == 0xB3,
            "Java's whole word reached the block");
    require(static_cast<uint8_t>(buffer[8]) == 0xC0 && static_cast<uint8_t>(buffer[9]) == 0xD1 && static_cast<uint8_t>(buffer[10]) == 0xC2 &&
                buffer[11] == 0x10,
            "a word both wrote keeps each side's own bytes and the application's where it is the same byte");
    require(static_cast<uint8_t>(buffer[12]) == 0xE0 && made.object->bytes[12] == 0xE0 && raw[12] == 0xE0, "the application wins the same byte");
    jbyte tail[3] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 4096, 3, tail);
    require(static_cast<uint8_t>(tail[0]) == 0xF0 && static_cast<uint8_t>(tail[1]) == 0xF1 && static_cast<uint8_t>(tail[2]) == 0xF2 &&
                made.object->bytes[4098] == 0xF2 && raw[4097] == 0xF1,
            "the bytes after the last whole word merge the same way");
}

// The unit is a chunk of 4096 bytes: a write in one chunk does not hold back what Java wrote in another.
void check_synchronization_is_per_chunk() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(10000, 0));
    uint8_t* raw = raw_of(made);
    raw[9000] = 0x77;                // chunk 2, the application
    made.object->bytes[10] = 0x55;   // chunk 0, Java
    world.set_calls = 0;
    world.critical_gets = 0;
    jbyte buffer[20] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 20, buffer);
    require(static_cast<uint8_t>(buffer[10]) == 0x55 && raw[10] == 0x55, "a read of chunk 0 shows Java's write");
    require(world.set_calls == 0 && world.critical_gets == 0 && made.object->bytes[9000] == 0, "and does not touch chunk 2");
    fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, nullptr);
    require(made.object->bytes[9000] == 0x77 && world.critical_gets == 1 && world.set_calls == 0,
            "the whole array's turn pushes chunk 2, and only it, in place");
}

// A block that neither side touched makes no write to the array at any synchronization point.
void check_a_clean_block_makes_no_array_write() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(9000));
    world.set_calls = 0;
    call_static_int_v(f, f.consume, made.shadow);
    jbyte buffer[100];
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 4000, 100, buffer);
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, nullptr);
    fn<RelElems>(*f.table, slot::ReleaseByteArrayElements)(f.env, made.shadow, elements, kReleaseAbort);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    void* pinned = critical(f.env, made.shadow, nullptr);
    release_critical(f.env, made.shadow, pinned, kReleaseAbort);
    require(world.set_calls == 0, "no write to the array");
    require(std::memcmp(raw_of(made), sample(9000).data(), 9000) == 0, "and the block is unchanged");
}

// A SetByteArrayRegion keeps what the application wrote elsewhere in the same chunks.
void check_set_region_keeps_other_raw_writes() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(200, 0));
    raw_of(made)[100] = 0x31;
    jbyte patch[4] = {1, 2, 3, 4};
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, made.shadow, 0, 4, patch);
    require(made.object->bytes[100] == 0x31 && made.object->bytes[3] == 4 && raw_of(made)[100] == 0x31 && raw_of(made)[3] == 4,
            "both the region and the earlier raw write are in both copies");
    jbyte buffer[1] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 100, 1, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0x31, "and a later read still has it");
}

// An exception pending: no JNI call runs, so nothing is pushed or pulled around the call.
void check_no_synchronization_with_an_exception_pending() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    raw_of(made)[0] = 0xAA;
    world.exception_pending = true;
    call_static_int_v(f, f.consume, made.shadow);
    require(world.seen_arrays[0][0] == 0, "the raw write was not pushed");
    world.exception_pending = false;
    jbyte one = 0;
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 1, &one);
    require(static_cast<uint8_t>(one) == 0xAA && made.object->bytes[0] == 0xAA, "it is pushed at the next synchronization point");
}

// A thread holds at most 512 live shadows; past that, and when the region is full, the result is the real
// reference. Nothing a caller may still use is ever recycled.
void check_limits_without_recycling() {
    Fixture f = make();
    std::vector<Made> held;
    for (int i = 0; i < 512; ++i) {
        std::vector<uint8_t> payload = sample(4);
        payload[0] = uint8_t(i);
        held.push_back(make_shadow_of(f, payload));
        require(is_shadow_block(held.back().shadow), "a shadow");
    }
    require(counters().shadows_live == 512 && counters().shadows_declined == 0, "512 live shadows");
    Made refused = make_shadow_of(f, sample(4));
    require(!is_shadow_block(refused.shadow) && world.locals.count(static_cast<Ref*>(refused.shadow)),
            "the 513th result is the real reference");
    require(counters().shadows_declined == 1 && counters().shadows_live == 512, "and it is counted as declined");
    for (int i = 0; i < 512; ++i) {
        const uint8_t* raw = static_cast<const uint8_t*>(held[size_t(i)].shadow);
        require(is_shadow_block(held[size_t(i)].shadow) && raw[16] == uint8_t(i), "every earlier shadow is intact");
        require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, held[size_t(i)].shadow) == 4, "with its length");
    }
    // A shadow the application deletes makes room.
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, held[100].shadow);
    Made again = make_shadow_of(f, sample(4));
    require(is_shadow_block(again.shadow), "after a delete the next result is a shadow again");
    // Another thread has its own 512.
    std::thread other([&] {
        Made mine = make_shadow_of(f, sample(10));
        require(is_shadow_block(mine.shadow), "another thread makes its own");
    });
    other.join();
    // The region is the other limit.
    Fixture g = make();
    Made big = make_shadow_of(g, std::vector<uint8_t>(1500000, 7));
    require(is_shadow_block(big.shadow), "a large array fits the region once");
    Made too_big = make_shadow_of(g, std::vector<uint8_t>(1500000, 8));
    require(!is_shadow_block(too_big.shadow) && counters().shadows_declined == 1, "the next one does not: the real reference");
    require(static_cast<const uint8_t*>(big.shadow)[16] == 7, "and the first is untouched");
}

// The diagnosis lines: the first declined shadow (with what the thread holds) and the most blocks a thread has held,
// at most once every 30 s while it grows.
size_t log_count(const char* part) {
    size_t n = 0;
    for (const auto& line : world.logs) n += line.message.find(part) != std::string::npos;
    return n;
}
void check_diagnosis_lines() {
    Fixture f = make();
    require(log_count("byte[] blocks") == 0 && log_count("stayed a real reference") == 0, "nothing logged before a shadow");
    std::vector<Made> held;
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 1 && world.logs.back().a == 1, "the first shadow logs the peak: 1");
    for (int i = 0; i < 20; ++i) held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 1, "growing within 30 s logs nothing more");
    world.clock_ms += 29999;
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 1, "29.999 s later still nothing");
    world.clock_ms += 1;
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 2 && world.logs.back().a == 23 && world.logs.back().b == 23,
            "after 30 s the peak is logged: 23 blocks, 23 live in the process");
    world.clock_ms += 60000;
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, held[3].shadow);
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 2, "a thread's count that does not exceed the peak is not a change");
    const size_t before_filling = log_count("most live byte[] blocks");
    while (counters().shadows_live < 512) held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("stayed a real reference") == 0, "512 blocks are all shadows");
    require(log_count("most live byte[] blocks") == before_filling + 1, "the first one above the logged peak logged it, the rest are within 30 s");
    Made refused = make_shadow_of(f, sample(100));
    require(!is_shadow_block(refused.shadow) && log_count("stayed a real reference") == 1, "the first declined shadow logs once");
    for (const auto& line : world.logs) {
        if (line.message.find("stayed a real reference") != std::string::npos) {
            require(line.message.find("thread limit") != std::string::npos && line.a == 512 && line.b == 512 * 100,
                    "with the reason and the thread's block count and bytes");
        }
    }
    make_shadow_of(f, sample(100));
    require(log_count("stayed a real reference") == 1, "a second declined shadow logs nothing");
    world.clock_ms += 30000;
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, held[4].shadow);
    make_shadow_of(f, sample(100));
    require(log_count("most live byte[] blocks") == before_filling + 2 && world.logs.back().a == 512,
            "and the peak of 512 is logged after 30 s");
    // The region is the other reason, and a new process logs again.
    Fixture g = make();
    world.clock_ms = 5;
    Made big = make_shadow_of(g, std::vector<uint8_t>(1500000, 7));
    Made too_big = make_shadow_of(g, std::vector<uint8_t>(1500000, 8));
    require(is_shadow_block(big.shadow) && !is_shadow_block(too_big.shadow), "the region holds one");
    require(log_count("no room in the region") == 1, "the region's refusal is named");
    for (const auto& line : world.logs) {
        if (line.message.find("no room in the region") != std::string::npos) {
            require(line.a == 1 && line.b == 1500000, "with the thread's one block of 1500000 bytes");
        }
    }
}


// The comparison with the baseline is exact: two words that a 32-bit digest would have called unchanged are a write.
void check_an_exact_comparison_keeps_every_write() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint32_t first = 0x00000001u, second = 0x4f69ba21u;  // the pair the reviewer found with equal digests
    std::memcpy(raw_of(made), &first, 4);
    std::memcpy(raw_of(made) + 4, &second, 4);
    std::vector<uint8_t> written(raw_of(made), raw_of(made) + 8);
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 8, buffer);
    require(std::memcmp(buffer, written.data(), 8) == 0 && made.object->bytes == written, "the write reached the array and the reader");
}

// A block the application wrote goes to the array in place, through one critical pair and no region call; a clean block's
// call reads the array through region calls in pieces of 16 KiB and makes no critical get; and a clean chunk is never written.
void check_a_dirty_block_is_pushed_in_place_and_a_clean_one_read_through_regions() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(6 * 4096));
    raw_of(made)[2 * 4096] ^= 0x55;
    raw_of(made)[3 * 4096] ^= 0x55;
    raw_of(made)[4 * 4096 + 5] ^= 0x55;
    world.set_calls = 0;
    world.get_calls = 0;
    world.critical_gets = 0;
    call_static_int_v(f, f.consume, made.shadow);
    require(world.critical_gets == 1 && world.set_calls == 0, "the bytes the application wrote went to the array in one pair, in place");
    require(world.get_calls == 2, "and the block, clean after the push, came back through regions in two pieces");
    require(world.seen_arrays[0][2 * 4096] == raw_of(made)[2 * 4096] && world.seen_arrays[0][4 * 4096 + 5] == raw_of(made)[4 * 4096 + 5],
            "Java saw the writes");
    Made big = make_shadow_of(f, sample(256 * 4096));
    world.set_calls = 0;
    world.get_calls = 0;
    world.critical_gets = 0;
    call_static_int_v(f, f.consume, big.shadow);
    require(world.set_calls == 0 && world.get_calls == 64 && world.critical_gets == 0,
            "a clean 1 MiB block: 64 region reads of 16 KiB, no write, no critical get");
    // The range of a small read is one chunk's turn, and only the bytes asked for come over.
    world.get_calls = 0;
    jbyte sixteen[16];
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, big.shadow, 100000, 16, sixteen);
    require(world.get_calls == 1 && world.set_calls == 0 && world.critical_gets == 0, "16 bytes: one small call");
}

// A committing release applies the chunk rule: the chunk the application wrote goes to the array, Java's other chunks stay.
void check_committing_release_keeps_java_bytes_in_clean_chunks() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8192, 0));
    auto elements = fn<jbyte* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, nullptr);
    auto release = fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements);
    elements[0] = 0x77;
    made.object->bytes[4096] = 0x41;  // Java, in the other chunk
    release(f.env, made.shadow, elements, 0);
    require(made.object->bytes[0] == 0x77 && made.object->bytes[4096] == 0x41, "mode 0: the application's chunk goes, Java's stays");
    require(raw_of(made)[4096] == 0x41, "and the block follows the array");
    elements[1] = 0x78;
    made.object->bytes[5000] = 0x42;
    release(f.env, made.shadow, elements, kReleaseCommit);
    require(made.object->bytes[1] == 0x78 && made.object->bytes[5000] == 0x42, "COMMIT the same");
    elements[2] = 0x79;
    release(f.env, made.shadow, elements, kReleaseAbort);
    require(made.object->bytes[2] == 0 && raw_of(made)[2] == 0, "ABORT discards the working copy");
    require(raw_of(made)[0] == 0x77 && raw_of(made)[1] == 0x78, "and keeps what had been committed");
}

// The reviewer's nested case: the array is open through its own critical pointer and through the block's.
void check_nested_critical_release_keeps_the_other_pointers_bytes() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8192, 0));
    Ref* real = new_local(made.object);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    uint8_t* original_pointer = static_cast<uint8_t*>(critical(f.env, real, nullptr));
    uint8_t* shadow_pointer = static_cast<uint8_t*>(critical(f.env, made.shadow, nullptr));
    original_pointer[4096] = 0x41;
    shadow_pointer[0] = 0x77;
    release(f.env, made.shadow, shadow_pointer, 0);
    release(f.env, real, original_pointer, 0);
    require(made.object->bytes[0] == 0x77, "the block's write reached the array");
    require(made.object->bytes[4096] == 0x41, "the other pointer's write in the other chunk survived");
    require(raw_of(made)[4096] == 0x41, "and the block shows it");
    // ABORT through the critical pair discards the block's writes.
    shadow_pointer = static_cast<uint8_t*>(critical(f.env, made.shadow, nullptr));
    shadow_pointer[8] = 0x55;
    release(f.env, made.shadow, shadow_pointer, kReleaseAbort);
    require(made.object->bytes[8] == 0 && raw_of(made)[8] == 0, "critical ABORT: the working copy is discarded");
}

// A shadow used as the receiver is synchronized like a parameter, in every call form, also for a method with no parameters.
void check_a_receiver_is_synchronized() {
    Fixture f = make();
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    jmethodID clone = get_method(f.env, f.cls, "clone", "()Ljava/lang/Object;");
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const jvalue none[1] = {};
    for (int form = 0; form < 5; ++form) {
        raw_of(made)[0] = uint8_t(0xA0 + form);
        world.seen_receiver.clear();
        switch (form) {
            case 0: fn<jobject (*)(Env*, jobject, jmethodID, ...)>(*f.table, call_slot(kObject, kVarargs))(f.env, made.shadow, clone); break;
            case 1: call_object_v(f, made.shadow, clone); break;
            case 2: fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, made.shadow, clone, none); break;
            case 3: fn<jobject (*)(Env*, jobject, jclass, jmethodID, const jvalue*)>(*f.table, nonvirtual_slot(kObject, kArray))(f.env, made.shadow, f.cls, clone, none); break;
            default: fn<jobject (*)(Env*, jobject, jclass, jmethodID, ...)>(*f.table, nonvirtual_slot(kObject, kVarargs))(f.env, made.shadow, f.cls, clone); break;
        }
        require(world.seen_receiver.size() == 8 && world.seen_receiver[0] == uint8_t(0xA0 + form), "Java read the receiver's bytes");
    }
    // What Java writes through the receiver shows in the block afterwards.
    world.java_fill = 0x31;
    fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, made.shadow, clone, none);
    world.java_fill = -1;
    require(raw_of(made)[0] == 0x31 && raw_of(made)[7] == 0x31, "and Java's writes through it come back");
}

// The same array returned twice is one block, counted twice.
void check_one_block_per_array() {
    Fixture f = make();
    jobject array = byte_array(std::vector<uint8_t>(8, 0));
    Obj* object = object_of(array);
    world.next_result = array;
    jobject first = call_static_object_v(f, f.get_bytes);
    world.next_result = new_local(object);
    jobject second = call_static_object_v(f, f.get_bytes);
    require(is_shadow_block(first) && second == first, "the same array is the same block");
    require(counters().shadows_live == 1 && world.globals.size() == 1, "one block, one global reference");
    static_cast<uint8_t*>(first)[16] = 0x66;
    call_static_int_v(f, f.consume, second);
    require(world.seen_arrays[0][0] == 0x66, "a write through the first is seen by Java when the second is passed");
    world.next_result = byte_array(std::vector<uint8_t>(8, 1));
    jobject other = call_static_object_v(f, f.get_bytes);
    require(other != first && counters().shadows_live == 2, "another array is another block");
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    del(f.env, first);
    require(is_shadow_block(second) && counters().shadows_live == 2, "the block outlives one of its two references");
    del(f.env, second);
    require(counters().shadows_live == 1 && world.globals.size() == 1, "and goes with the last");
    // An array of another size, or one that is not among the recent blocks, is not mistaken for it.
    world.next_result = new_local(object);
    require(!is_shadow_block(first) && is_shadow_block(call_static_object_v(f, f.get_bytes)), "it is a new block once the old one is gone");
}

// A real reference to the same array (a global reference, or a pinned pointer) is a second way to reach the bytes, so the
// getters of a shadow must say that their pointer is a copy: conformant code commits when it is told so.
void check_the_flag_of_the_elements_is_truthful() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    Ref* alias = new_local(made.object);  // a real reference to the same array
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    jmethodID clone = get_method(f.env, f.cls, "clone", "()Ljava/lang/Object;");
    jboolean copy = 0;
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, &copy);
    elements[0] = 0x77;
    if (copy != 0) {
        fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, made.shadow, elements, kReleaseCommit);
    }
    const jvalue none[1] = {};
    fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, alias, clone, none);
    require(world.seen_receiver.size() == 8 && world.seen_receiver[0] == 0x77,
            "Java, through the real reference, sees the byte of a code that commits when told to");
    require(copy == 1, "the flag of a shadow's elements is true");
    // A real reference keeps the runtime's own flag.
    jboolean real_copy = 1;
    fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, alias, &real_copy);
    require(real_copy == 0, "a real reference gets the runtime's flag");
}
void check_the_flag_of_the_critical_pointer_is_truthful() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8192, 0));
    Ref* alias = new_local(made.object);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    uint8_t* real = static_cast<uint8_t*>(critical(f.env, alias, nullptr));  // the real array, pinned
    jboolean copy = 0;
    uint8_t* block = static_cast<uint8_t*>(critical(f.env, made.shadow, &copy));
    block[0] = 0x77;
    if (copy != 0) release(f.env, made.shadow, block, 0);
    require(real[0] == 0x77, "the real pinned pointer sees the byte of a code that commits when told to");
    require(copy == 1, "the flag of a shadow's critical pointer is true");
    if (copy == 0) release(f.env, made.shadow, block, 0);
    release(f.env, alias, real, 0);
    jboolean real_copy = 1;
    void* again = critical(f.env, alias, &real_copy);
    require(real_copy == 0, "a real reference gets the runtime's flag");
    release(f.env, alias, again, 0);
}

// The length word of the Dalvik object is the application's to write: the layer trusts only its own capacity.
void check_a_stray_length_write_is_harmless() {
    Fixture f = make();
    std::vector<uint8_t> bytes;
    bytes.reserve(8 + 64);
    bytes.assign(8, 0x10);
    std::memset(bytes.data() + 8, 0xEE, 64);  // behind the array: what an overrun would hit
    jobject array = new_local(new Obj{std::move(bytes), true});
    Obj* object = object_of(array);
    world.next_result = array;
    jobject shadow = call_static_object_v(f, f.get_bytes);
    require(is_shadow_block(shadow), "a shadow");
    const uint32_t stray = 100000;
    std::memcpy(static_cast<uint8_t*>(shadow) + 8, &stray, 4);
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 8, "the length is the capacity");
    auto elements = fn<jbyte* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetByteArrayElements)(f.env, shadow, nullptr);
    auto release = fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements);
    elements[0] = 0x21;
    release(f.env, shadow, elements, 0);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    uint8_t* pinned = static_cast<uint8_t*>(critical(f.env, shadow, nullptr));
    pinned[1] = 0x22;
    release_critical(f.env, shadow, pinned, 0);
    release_critical(f.env, shadow, critical(f.env, shadow, nullptr), kReleaseAbort);
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 0, 8, buffer);
    call_static_int_v(f, f.consume, shadow);
    require(object->bytes[0] == 0x21 && object->bytes[1] == 0x22, "the writes reached the array");
    for (int i = 0; i < 64; ++i) require(object->bytes.data()[8 + i] == 0xEE, "and nothing behind it was written");
    jbyte wide[16] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 0, 16, wide);
    require(world.exception_pending, "a range beyond the capacity is the runtime's exception, whatever the length word says");
    world.exception_pending = false;
}


// ---- the end of a native method (the thunks, RegisterNatives, PushLocalFrame/PopLocalFrame, DetachCurrentThread)

// A function of an application library and its thunk, as the assembly would run it: the thunk is entered with the
// runtime's return address and a stack pointer; the stub that follows the method's return gives that stack pointer back.
void* app_function(unsigned n) { return reinterpret_cast<void*>(uintptr_t{0x20000000u} + 32u * n); }
struct Native {
    void* thunk;
    unsigned index;
};
Native thunk_of(void* function) {
    void* thunk = thunk_for(function);
    require(is_thunk(thunk), "an application function gets a thunk");
    return Native{thunk, thunk_index_of(thunk)};
}
bool enter_native(const Native& native, void* function, uintptr_t lr, uintptr_t sp) {
    const ThunkEntry entry = thunk_enter(native.index, reinterpret_cast<void*>(lr), reinterpret_cast<void*>(sp));
    require(entry.target == function, "the thunk jumps to its function");
    return entry.pushed == 1;
}
void* leave_native(uintptr_t sp) { return thunk_leave(reinterpret_cast<void*>(sp)); }

// The blocks a native method made end when it returns, whether it deleted any reference or not.
void check_a_native_invocation_ends_its_blocks() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(thunk_for(function) == native.thunk, "one thunk for a function, however often it is registered");
    require(counters().natives_wrapped == 1, "counted once");
    require(enter_native(native, function, 0x1234, 0x8000), "a record is pushed");
    Made a = make_shadow_of(f, sample(10));
    Made b = make_shadow_of(f, sample(20));
    Made c = make_shadow_of(f, sample(30));
    require(counters().shadows_live == 3 && world.globals.size() == 3, "three blocks during the invocation");
    require(is_shadow_block(a.shadow) && is_shadow_block(b.shadow) && is_shadow_block(c.shadow), "all usable");
    require(leave_native(0x8000) == reinterpret_cast<void*>(0x1234), "the return stub goes back to the runtime");
    require(counters().shadows_live == 0 && world.globals.empty() && counters().ended_by_return == 3,
            "the blocks ended: no block, no global reference");
    require(!is_shadow_block(a.shadow) && !is_shadow_block(c.shadow), "a block that ended is no block");
    // A reference the method deleted itself is not ended twice.
    require(enter_native(native, function, 0x1234, 0x8000), "again");
    Made d = make_shadow_of(f, sample(10));
    Made e = make_shadow_of(f, sample(10));
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, d.shadow);
    require(counters().shadows_live == 1, "the deleted one is gone");
    leave_native(0x8000);
    require(counters().shadows_live == 0 && world.globals.empty() && counters().ended_by_return == 4 &&
                !is_shadow_block(e.shadow),
            "the other ended at the return");
    // The result of a call that is not byte[] is no block, and nothing else is touched.
    const size_t locals = world.locals.size();
    require(enter_native(native, function, 0x1234, 0x8000), "once more");
    leave_native(0x8000);
    require(world.locals.size() == locals, "an invocation that made no block ends nothing");
}

// A native method that calls Java, which calls native again: each return ends its own blocks and no more.
void check_nested_invocations_end_their_own_blocks() {
    Fixture f = make();
    void* outer_function = app_function(1);
    void* inner_function = app_function(2);
    const Native outer = thunk_of(outer_function);
    const Native inner = thunk_of(inner_function);
    require(enter_native(outer, outer_function, 0x100, 0x9000), "outer");
    Made a = make_shadow_of(f, sample(4));
    require(enter_native(inner, inner_function, 0x200, 0x8000), "the outer method called Java, which called inner");
    Made b = make_shadow_of(f, sample(5));
    Made c = make_shadow_of(f, sample(6));
    require(leave_native(0x8000) == reinterpret_cast<void*>(0x200), "inner returns to the runtime");
    require(counters().shadows_live == 1 && is_shadow_block(a.shadow) && !is_shadow_block(b.shadow) &&
                !is_shadow_block(c.shadow),
            "inner's blocks ended, outer's did not");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, a.shadow) == 4, "outer's block still works");
    Made d = make_shadow_of(f, sample(7));
    require(leave_native(0x9000) == reinterpret_cast<void*>(0x100), "outer returns");
    require(counters().shadows_live == 0 && world.globals.empty() && !is_shadow_block(a.shadow) && !is_shadow_block(d.shadow),
            "and its blocks, the ones made before and after the inner call, ended");
    // A recursion twenty levels deep: every level keeps its own block until its own return.
    std::vector<Made> held;
    for (unsigned depth = 0; depth < 20; ++depth) {
        require(enter_native(outer, outer_function, 0x1000 + depth, 0x20000 - 0x100 * depth), "a level");
        std::vector<uint8_t> payload = sample(8);
        payload[0] = uint8_t(depth);
        held.push_back(make_shadow_of(f, payload));
    }
    for (unsigned depth = 20; depth > 0; --depth) {
        const unsigned level = depth - 1;
        require(is_shadow_block(held[level].shadow) && raw_of(held[level])[0] == uint8_t(level), "the level's block is intact");
        require(leave_native(0x20000 - 0x100 * level) == reinterpret_cast<void*>(uintptr_t{0x1000} + level), "the level returns");
        require(!is_shadow_block(held[level].shadow) && counters().shadows_live == level, "and only its block ended");
    }
}

// An exception pending at the return (the method threw, or a Java call it made did): the ends wait, with the blocks
// still strong, because the weak reference and the array write they need are not allowed then; the exception stays, and
// the next call without one finishes them.
void check_a_native_method_that_returns_with_an_exception_pending() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    Made a = make_shadow_of(f, sample(10));
    Made b = make_shadow_of(f, sample(11));
    world.exception_pending = true;
    require(leave_native(0x8000) == reinterpret_cast<void*>(0x1234), "the stub goes back");
    require(world.exception_pending && world.weaks.empty() && counters().shadows_ended == 0,
            "no weak reference was made, the exception is still pending");
    require(world.globals.size() == 2 && counters().shadows_live == 2, "the blocks wait, strong");
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(world.exception_pending && world.globals.size() == 2, "a call made with the exception still pending ends nothing");
    world.exception_pending = false;
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_live == 0 && counters().shadows_ended == 2 && world.globals.empty() && block_weaks() == 2,
            "the next call without one finished the ends: ended, weak");
    require(is_ended_block(a.shadow) && is_ended_block(b.shadow) && !is_shadow_block(a.shadow), "two ended blocks");
}

// The 513th result of a thread is no longer the end of the game's shadows: a method per frame, a result per method.
void check_six_hundred_invocations_decline_nothing() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    for (unsigned i = 0; i < 600; ++i) {
        require(enter_native(native, function, 0x1234, 0x8000), "enter");
        std::vector<uint8_t> payload = sample(100);
        payload[0] = uint8_t(i);
        Made made = make_shadow_of(f, payload);
        require(is_shadow_block(made.shadow) && raw_of(made)[0] == uint8_t(i), "a shadow, the 600th too");
        leave_native(0x8000);
    }
    require(counters().shadows_declined == 0 && counters().shadows_live == 0 && counters().peak_thread_blocks == 1,
            "none declined, none live, never more than one at a time");
    require(counters().ended_by_return == 600 && world.globals.empty(), "every one ended at its return");
}

// Frame records: at most 64. A method entered beyond them has no record and returns as it is; its blocks end with
// the nearest recorded invocation.
void check_more_than_sixty_four_nested_invocations() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    std::vector<Made> held;
    for (unsigned i = 0; i < 70; ++i) {
        const bool pushed = enter_native(native, function, 0x1000 + i, 0x40000 - 0x100 * i);
        require(pushed == (i < 64), "a record for each of the first 64 nested invocations, none after");
        held.push_back(make_shadow_of(f, sample(4)));
    }
    require(counters().shadows_live == 70, "70 blocks");
    // The methods past the 64th return without the stub; the 64th one's return ends all of theirs.
    require(leave_native(0x40000 - 0x100 * 63) == reinterpret_cast<void*>(uintptr_t{0x1000} + 63), "the 64th returns");
    require(counters().shadows_live == 63, "its own and the six after it ended");
    for (int i = 62; i >= 0; --i) {
        require(leave_native(0x40000 - 0x100 * unsigned(i)) == reinterpret_cast<void*>(uintptr_t{0x1000} + unsigned(i)), "return");
    }
    require(counters().shadows_live == 0 && world.globals.empty(), "all ended");
}

// A thread the platform has no state for: no record, the method returns as it is.
void check_no_thread_state_no_record() {
    make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    g_no_thread_state = true;
    require(!enter_native(native, function, 0x1234, 0x8000), "no record");
    g_no_thread_state = false;
}

// A method that never returned (a longjmp over it): the outer return ends its blocks, and the records are trimmed.
void check_an_abandoned_invocation_is_trimmed() {
    Fixture f = make();
    void* outer_function = app_function(1);
    void* inner_function = app_function(2);
    const Native outer = thunk_of(outer_function);
    const Native inner = thunk_of(inner_function);
    require(enter_native(outer, outer_function, 0x100, 0x9000), "outer");
    make_shadow_of(f, sample(4));
    require(enter_native(inner, inner_function, 0x200, 0x8000), "inner");
    make_shadow_of(f, sample(5));
    require(leave_native(0x9000) == reinterpret_cast<void*>(0x100), "outer returns over the inner invocation");
    require(counters().shadows_live == 0 && world.globals.empty(), "both blocks ended");
    // The thread goes on with clean records.
    require(enter_native(outer, outer_function, 0x300, 0x9000), "outer again");
    make_shadow_of(f, sample(4));
    require(leave_native(0x9000) == reinterpret_cast<void*>(0x300) && counters().shadows_live == 0, "and returns");
}

// A PushLocalFrame the method never popped is part of its invocation: the return ends its entries, and ART's own
// segment goes with the invocation.
void check_an_unmatched_push_local_frame_ends_with_the_invocation() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    make_shadow_of(f, sample(4));
    require(push(f.env, 16) == 0, "push");
    make_shadow_of(f, sample(4));
    require(counters().shadows_live == 2, "two blocks");
    leave_native(0x8000);
    require(counters().shadows_live == 0 && world.globals.empty(), "the return ended the entries in both");
    // A later method with a pop of its own works on its own frame.
    require(enter_native(native, function, 0x1234, 0x8000), "enter again");
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "its own push");
    make_shadow_of(f, sample(4));
    pop(f.env, nullptr);
    require(counters().shadows_live == 0, "its pop ended its own entry");
    leave_native(0x8000);
}

// The distinct natives that can have a thunk: 1024; past them a function is registered as it is and the block it
// makes ends with its references.
void check_the_thunks_run_out() {
    make();
    for (unsigned i = 0; i < 1030; ++i) {
        void* function = app_function(i);
        void* thunk = thunk_for(function);
        if (i < 1024) {
            require(is_thunk(thunk) && thunk_index_of(thunk) == i, "the first 1024 functions get a thunk each");
        } else {
            require(thunk == function, "then the function itself");
        }
    }
    require(counters().natives_wrapped == 1024 && counters().natives_unwrapped == 6, "counted");
    require(log_count("got no thunk") == 1, "and the first one is logged, once");
    require(thunk_for(app_function(5)) == fake_thunk_address(5), "a function that has a thunk still gets it");
    require(counters().natives_unwrapped == 6, "which is not a new miss");
    make(true, 4);
    for (unsigned i = 0; i < 4; ++i) require(is_thunk(thunk_for(app_function(i))), "the platform's capacity is the limit");
    require(thunk_for(app_function(4)) == app_function(4), "past it, none");
}

// A native method that got no thunk (the thunks ran out) is bound as it is, with one log line: the runtime enters and
// leaves it with no record, so its blocks end only by the other exact ends (DeleteLocalRef, PopLocalFrame,
// DetachCurrentThread) or count toward the thread's 512, the result beyond it staying the real reference.
void check_a_method_without_a_thunk_ends_by_the_other_ends() {
    Fixture f = make(true, 2);
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    require(is_thunk(thunk_for(app_function(0))) && is_thunk(thunk_for(app_function(1))), "two thunks, both in use");
    void* without = app_function(2);
    require(thunk_for(without) == without && thunk_for(app_function(3)) == app_function(3),
            "the next functions are bound as they are");
    require(counters().natives_wrapped == 2 && counters().natives_unwrapped == 2 && log_count("got no thunk") == 1,
            "counted, and logged once");
    // Called by the runtime directly: no record, no return stub. Its blocks stay until something ends them.
    require(push(f.env, 16) == 0, "a frame");
    for (int i = 0; i < 3; ++i) make_shadow_of(f, sample(4 + size_t(i)));
    require(counters().shadows_live == 3, "the blocks of the method stay with their references");
    pop(f.env, nullptr);
    require(counters().shadows_live == 0 && counters().ended_by_pop == 3 && counters().ended_by_return == 0,
            "PopLocalFrame ends them");
    std::vector<Made> held;
    for (int i = 0; i < 512; ++i) {
        std::vector<uint8_t> payload = sample(4);
        payload[0] = uint8_t(i);
        held.push_back(make_shadow_of(f, payload));
    }
    require(counters().shadows_live == 512 && counters().shadows_declined == 0, "512 count toward the limit");
    Made refused = make_shadow_of(f, sample(4));
    require(!is_shadow_block(refused.shadow) && counters().shadows_declined == 1, "the 513th is the real reference");
    del(f.env, held[7].shadow);
    require(is_shadow_block(make_shadow_of(f, sample(4)).shadow), "DeleteLocalRef makes room");
    struct Fns {
        static jint detach(Vm*) { return 0; }
        static jint other(Vm*, void**, jint) { return 0; }
    };
    void* vm_table[kVmSlotCount];
    for (void*& entry : vm_table) entry = reinterpret_cast<void*>(&Fns::other);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&Fns::detach);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    Vm vm{vm_table};
    reinterpret_cast<jint (*)(Vm*)>(vm_table[vmslot::DetachCurrentThread])(&vm);
    require(counters().shadows_live == 0 && counters().ended_by_detach == 512 && world.globals.empty(),
            "DetachCurrentThread ends the rest");
}

// Which functions get a thunk: those of the application's own libraries, with shadows on and thunks provided. The
// function's address decides; who is calling does not.
void check_which_functions_get_a_thunk() {
    make();
    g_own_default = false;
    require(thunk_for(app_function(1)) == app_function(1) && counters().natives_wrapped == 0,
            "a function outside the application's own libraries (another package's, a platform library): none");
    g_app_caller = false;
    g_own_default = true;
    require(is_thunk(thunk_for(app_function(1))), "an own function gets its thunk whoever is calling");
    g_app_caller = true;
    g_own_default = false;
    require(thunk_for(app_function(2)) == app_function(2), "and a foreign one gets none from an application caller");
    g_own_default = true;
    require(thunk_for(nullptr) == nullptr, "no function, none");
    make(/*shadows=*/false);
    require(thunk_for(app_function(1)) == app_function(1), "with no shadows the function stays as it is");
    make(true, 0);
    require(thunk_for(app_function(1)) == app_function(1), "and with no thunks");
    reset_world();
    require(thunk_for(app_function(1)) == app_function(1), "and before the wrappers are installed");
    // A platform that cannot tell the libraries apart: the application's code is the domain.
    g_without_own_check = true;
    make();
    g_without_own_check = false;
    require(is_thunk(thunk_for(app_function(1))), "with no own-library test, application code");
    g_app_caller = false;
    require(thunk_for(app_function(2)) == app_function(2), "and a platform function: none");
}

bool own_even(const void* address) { return ((reinterpret_cast<uintptr_t>(address) - 0x20000000u) / 32u) % 2 == 0; }

// RegisterNatives: the application's functions are registered through their thunks, in chunks, with the rest of
// the call unchanged and the runtime's first failure ending it.
void check_register_natives_registers_thunks() {
    Fixture f = make();
    g_own_predicate = own_even;
    std::vector<NativeMethod> methods;
    for (unsigned i = 0; i < 70; ++i) methods.push_back(NativeMethod{"name", "()V", app_function(i)});
    auto register_natives = fn<jint (*)(Env*, jclass, const NativeMethod*, jint)>(*f.table, slot::RegisterNatives);
    require(register_natives(f.env, f.cls, methods.data(), 70) == JNI_OK, "registered");
    std::vector<NativeMethod> got;
    for (const auto& call : g_registered) got.insert(got.end(), call.begin(), call.end());
    require(g_registered.size() == 3 && got.size() == 70, "three calls of the runtime, 32 + 32 + 6 methods");
    for (unsigned i = 0; i < 70; ++i) {
        require(got[i].name == methods[i].name && got[i].signature == methods[i].signature, "names and signatures unchanged");
        require(methods[i].function == app_function(i), "the caller's array is unchanged");
        if (i % 2 == 0) {
            require(is_thunk(got[i].function), "an application function is registered through its thunk");
            require(thunk_for(app_function(i)) == got[i].function, "which is its thunk");
        } else {
            require(got[i].function == app_function(i), "a function outside the application's libraries as it is");
        }
    }
    // The function's address decides, not the caller's: a platform library registering an own function gets its
    // thunk, an application library registering a foreign one gets none.
    g_own_predicate = nullptr;
    g_registered.clear();
    methods.assign({NativeMethod{"a", "()V", app_function(100)}});
    g_app_caller = false;
    require(register_natives(f.env, f.cls, methods.data(), 1) == JNI_OK && is_thunk(g_registered.back()[0].function),
            "a platform caller registering an own function: its thunk");
    g_app_caller = true;
    g_own_default = false;
    methods.assign({NativeMethod{"b", "()V", app_function(101)}});
    require(register_natives(f.env, f.cls, methods.data(), 1) == JNI_OK && g_registered.back()[0].function == app_function(101),
            "an application caller registering a foreign function: as it is");
    g_own_default = true;
    g_registered.clear();
    methods.assign(70, NativeMethod{"name", "()V", app_function(0)});
    g_register_fail_at = 1;
    require(register_natives(f.env, f.cls, methods.data(), 70) == JNI_ERR && g_registered.size() == 2,
            "the runtime's error ends the registration and is the result");
}

// The attachment of a thread ends its references.
void check_detach_ends_the_references_of_a_thread() {
    Fixture f = make();
    struct Fns {
        static jint detach(Vm*) { return 0; }
        static jint other(Vm*, void**, jint) { return 0; }
    };
    void* vm_table[kVmSlotCount];
    for (void*& s : vm_table) s = reinterpret_cast<void*>(&Fns::other);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&Fns::detach);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    Vm vm{vm_table};
    std::thread attached([&] {
        for (int i = 0; i < 5; ++i) require(is_shadow_block(make_shadow_of(f, sample(8)).shadow), "a shadow");
        require(counters().shadows_live == 5, "five references");
        const jint rc = reinterpret_cast<jint (*)(Vm*)>(vm_table[vmslot::DetachCurrentThread])(&vm);
        require(rc == 0, "the runtime's detach is the result");
        require(counters().shadows_live == 0 && world.globals.empty() && counters().ended_by_detach == 5,
                "the five ended with the attachment");
        // Attached again, it makes more; its end (no detach) leaves them to the next call with an environment.
        for (int i = 0; i < 2; ++i) make_shadow_of(f, sample(8));
        require(counters().shadows_live == 2, "the new attachment has its own");
        release_thread_state(t_block);
    });
    attached.join();
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_live == 0 && world.globals.empty(), "and the thread's end freed those");
}

// PushLocalFrame and PopLocalFrame: the entries made in a frame end with it; the shadow a pop is handed stays, with an
// entry in the outer frame.
void check_push_and_pop_local_frame_end_their_entries() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    Made base = make_shadow_of(f, sample(4));
    require(push(f.env, 16) == 0, "push");
    make_shadow_of(f, sample(5));
    make_shadow_of(f, sample(6));
    require(counters().shadows_live == 3, "three blocks, two of them in the frame");
    pop(f.env, nullptr);
    require(counters().shadows_live == 1 && is_shadow_block(base.shadow) && counters().ended_by_pop == 2,
            "the pop ended the frame's two, not the one below");
    // Nested frames.
    require(push(f.env, 16) == 0, "outer frame");
    Made x = make_shadow_of(f, sample(7));
    require(push(f.env, 16) == 0, "inner frame");
    Made y = make_shadow_of(f, sample(8));
    pop(f.env, nullptr);
    require(is_shadow_block(x.shadow) && !is_shadow_block(y.shadow), "the inner pop ended the inner frame's block");
    pop(f.env, nullptr);
    require(!is_shadow_block(x.shadow) && is_shadow_block(base.shadow), "and the outer one the outer frame's");
    // A shadow handed to the pop: it comes out into the outer frame and stays until it is deleted there.
    require(push(f.env, 16) == 0, "a frame");
    Made result = make_shadow_of(f, sample(9));
    make_shadow_of(f, sample(9));
    jobject out = pop(f.env, result.shadow);
    require(out == result.shadow && is_shadow_block(result.shadow) && counters().shadows_live == 2,
            "the result stayed, the other block of the frame ended");
    del(f.env, result.shadow);
    require(!is_shadow_block(result.shadow) && counters().shadows_live == 1, "the outer frame's reference is deleted once");
    del(f.env, base.shadow);
    require(counters().shadows_live == 0 && world.globals.empty(), "nothing is left");
}

// More PushLocalFrame calls than there are records: the pops of the unrecorded ones end nothing, the entries made
// in them end with the last recorded frame.
void check_frames_beyond_the_records() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    for (unsigned i = 0; i < 70; ++i) {
        require(push(f.env, 4) == 0, "push");
        make_shadow_of(f, sample(4));
    }
    require(counters().shadows_live == 70, "70 blocks");
    for (unsigned i = 0; i < 6; ++i) pop(f.env, nullptr);
    require(counters().shadows_live == 70, "the six pops of unrecorded frames ended nothing");
    pop(f.env, nullptr);
    require(counters().shadows_live == 63, "the 64th frame's pop ended its entries and those of the frames after it");
    for (unsigned i = 0; i < 63; ++i) pop(f.env, nullptr);
    require(counters().shadows_live == 0 && world.globals.empty(), "and the rest");
}

// DeleteLocalRef takes the newest entry of the block; the frames above it stay right.
void check_delete_local_ref_keeps_the_frames_right() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    Made a = make_shadow_of(f, sample(4));
    Made b = make_shadow_of(f, sample(5));
    Made c = make_shadow_of(f, sample(6));
    require(push(f.env, 16) == 0, "push");
    Made d = make_shadow_of(f, sample(7));
    del(f.env, b.shadow);  // an entry below the frame: both runtimes leave a reference of an outer frame alone
    require(is_shadow_block(b.shadow) && counters().shadows_live == 4, "the outer reference was not deleted from the inner frame");
    pop(f.env, nullptr);
    require(!is_shadow_block(d.shadow), "the frame's block ended");
    require(is_shadow_block(a.shadow) && is_shadow_block(b.shadow) && is_shadow_block(c.shadow) && counters().shadows_live == 3,
            "the pop did not end the entries below the frame");
    del(f.env, b.shadow);
    require(!is_shadow_block(b.shadow) && counters().shadows_live == 2, "and its own frame can delete it");
    // The same array twice is one block with two entries: the first delete leaves it, the second ends it.
    jobject array = byte_array(sample(12));
    Obj* object = object_of(array);
    world.next_result = array;
    jobject first = call_static_object_v(f, f.get_bytes);
    world.next_result = new_local(object);
    jobject second = call_static_object_v(f, f.get_bytes);
    require(first == second && counters().shadows_live == 3, "one block");
    del(f.env, first);
    require(is_shadow_block(first) && counters().shadows_live == 3, "one reference left");
    del(f.env, second);
    require(!is_shadow_block(first) && counters().shadows_live == 2, "and none");
    // An entry made in a native invocation for an array the invocation's caller already holds goes with the invocation only.
    void* function = app_function(1);
    const Native native = thunk_of(function);
    jobject again = byte_array(sample(12));
    Obj* again_object = object_of(again);
    world.next_result = again;
    jobject held = call_static_object_v(f, f.get_bytes);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    world.next_result = new_local(again_object);
    jobject same = call_static_object_v(f, f.get_bytes);
    require(same == held, "the same array is the same block");
    leave_native(0x8000);
    require(is_shadow_block(held), "the invocation's reference ended, the block of the caller's stays");
    del(f.env, held);
    require(!is_shadow_block(held), "and goes with its own");
}

// The diagnosis lines of the thunks: the first block that ended at a return, and the first function that got none.
void check_the_thunk_diagnosis_lines() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(log_count("ended at the return of a native method") == 0, "nothing yet");
    for (int i = 0; i < 3; ++i) {
        enter_native(native, function, 0x1234, 0x8000);
        make_shadow_of(f, sample(4));
        make_shadow_of(f, sample(4));
        leave_native(0x8000);
    }
    require(log_count("ended at the return of a native method") == 1, "once");
    for (const auto& line : world.logs) {
        if (line.message.find("ended at the return") != std::string::npos) {
            require(line.a == 2 && line.b == 1, "with the references it ended and the natives with a thunk");
        }
    }
    // The most one return has ended: logged when it grows, at most every 30 s, and a plateau reached inside the interval
    // is logged by the next return after it.
    const auto invocation = [&](int blocks) {
        enter_native(native, function, 0x1234, 0x8000);
        for (int i = 0; i < blocks; ++i) make_shadow_of(f, sample(4));
        leave_native(0x8000);
    };
    require(log_count("the most references one native method call") == 0, "no line for the first return's count");
    world.clock_ms += 31000;
    invocation(5);
    require(log_count("the most references one native method call") == 1 && world.logs.back().a == 5 && world.logs.back().b == 1,
            "a larger count is logged after 30 s");
    invocation(7);
    require(log_count("the most references one native method call") == 1, "not again within 30 s");
    world.clock_ms += 31000;
    invocation(1);
    require(log_count("the most references one native method call") == 2 && world.logs.back().a == 7,
            "and the count reached in between, by the next return after 30 s");
    require(counters().max_ended_by_return == 7, "the counter keeps the most");
}

// A thread ends: its blocks wait for an environment, then go with their global references.
void check_thread_end() {
    Fixture f = make();
    void* ended_state = nullptr;
    std::thread other([&] {
        for (int i = 0; i < 3; ++i) require(is_shadow_block(make_shadow_of(f, sample(8)).shadow), "a shadow");
        ended_state = t_block;
        release_thread_state(ended_state);  // what the thread's exit does: no JNI call
    });
    other.join();
    require(counters().shadows_live == 3 && world.globals.size() == 3, "the blocks wait: no environment on the exiting thread");
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_live == 0 && world.globals.empty() && counters().shadows_released == 3,
            "the next wrapped call with an environment freed them and deleted their global references");
}

// PopLocalFrame(shadow): the frame's references go, the shadow (in no table) stays and is what the caller gets.
void check_pop_local_frame() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    const size_t locals = world.locals.size();
    require(push(f.env, 32) == 0, "push");
    Made made = make_shadow_of(f, sample(4));
    jobject inside = fn<jobject (*)(Env*, const char*)>(*f.table, slot::FindClass)(f.env, "x");
    require(is_shadow_block(made.shadow) && world.locals.count(static_cast<Ref*>(inside)), "a shadow and another local in the frame");
    jobject result = pop(f.env, made.shadow);
    require(result == made.shadow && is_shadow_block(result), "the same block comes out of the frame");
    require(world.locals.size() == locals, "the frame's other references are gone");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, result) == 4, "and it is usable");
    require(!world.exception_pending, "no stale reference was deleted");
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, result);
    // A real reference and a null result take the original path.
    push(f.env, 32);
    jobject real = byte_array(sample(4));
    jobject out = pop(f.env, real);
    require(world.locals.count(static_cast<Ref*>(out)) && !is_shadow_block(out), "a real result is the original's");
    push(f.env, 32);
    require(pop(f.env, nullptr) == nullptr, "no result");
}

// ---- ended blocks: a block whose references ended stays at its address until its array is collected

void end_ref(Fixture& f, jobject shadow) { fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow); }

// A block of an array Java holds (a collection does not take it).
Made make_held(Fixture& f, const std::vector<uint8_t>& payload) {
    Made made = make_shadow_of(f, payload);
    made.object->held = true;
    return made;
}

// The same array obtained again from a call: a new local reference to the same object.
Made obtain_again(Fixture& f, Obj* object) {
    world.next_result = new_local(object);
    return Made{call_static_object_v(f, f.get_bytes), object};
}

// A kept pointer reads its bytes after a thousand later blocks of the same size, and no address is reused.
void check_a_kept_pointer_keeps_its_bytes() {
    Fixture f = make();
    std::vector<uint8_t> payload = sample(100);
    Made kept = make_held(f, payload);
    const uint8_t* p = static_cast<const uint8_t*>(kept.shadow);
    end_ref(f, kept.shadow);
    require(is_ended_block(p) && !is_shadow_block(p), "the block ended, and is no live block");
    require(counters().shadows_live == 0 && counters().shadows_ended == 1 && counters().ended_bytes == 100 &&
                world.globals.empty() && block_weaks() == 1,
            "no strong reference is left, a weak one is, and the block is counted as ended");
    std::set<const void*> addresses;
    for (int i = 0; i < 1000; ++i) {
        std::vector<uint8_t> other = sample(100);
        other[0] = uint8_t(i);
        Made temporary = make_shadow_of(f, other);
        require(is_shadow_block(temporary.shadow) && temporary.shadow != kept.shadow, "a new block, not the kept one's address");
        require(addresses.insert(temporary.shadow).second, "and no address is reused while nothing was collected");
        end_ref(f, temporary.shadow);
    }
    require(std::memcmp(p + 16, payload.data(), 100) == 0, "the kept pointer reads the bytes it had");
    require(counters().shadows_ended == 1001 && counters().shadows_declined == 0, "1001 ended blocks, none declined");
    require(g_ended_locks > 0, "the index is under the ended lock");
}

// A collection: the next creations free the ended blocks whose arrays are gone, and the next block of the size takes
// the address.
void check_a_collection_frees_ended_blocks() {
    Fixture f = make();
    Made gone = make_shadow_of(f, sample(100));
    Made kept = make_held(f, sample(100));
    const void* gone_at = gone.shadow;
    end_ref(f, gone.shadow);
    end_ref(f, kept.shadow);
    require(counters().shadows_ended == 2, "two ended blocks");
    Made before = make_shadow_of(f, sample(50));  // no collection yet: nothing is tested
    require(counters().blocks_collected == 0 && world.same_calls <= 1 + 8, "no collection, no sweep");
    collect();
    Made after = make_shadow_of(f, sample(100));
    require(counters().blocks_collected == 1 && counters().shadows_ended == 1, "the block of the gone array was freed");
    require(after.shadow == gone_at, "and the next block of that size takes its address");
    require(is_ended_block(kept.shadow), "the block of the array Java holds stays");
    require(block_weaks() == 1, "one weak reference is left");
    (void)before;
}

// A sweep tests 32 ended blocks at a creation, one test of the sentinel when nothing was collected.
void check_a_sweep_takes_thirty_two_blocks_at_a_creation() {
    Fixture f = make();
    for (int i = 0; i < 101; ++i) {
        std::vector<uint8_t> payload = sample(20);
        payload[0] = uint8_t(i);
        end_ref(f, make_shadow_of(f, payload).shadow);
    }
    world.same_calls = 0;
    end_ref(f, make_shadow_of(f, sample(30)).shadow);
    require(world.same_calls == 1 && counters().blocks_collected == 0, "nothing was collected: one test, of the sentinel");
    collect();
    const uint32_t expected[4] = {32, 64, 96, 102};  // the blocks that ended after the collection are tested, not freed
    for (int step = 0; step < 4; ++step) {
        world.same_calls = 0;
        std::vector<uint8_t> payload = sample(40);
        payload[0] = uint8_t(step);
        end_ref(f, make_shadow_of(f, payload).shadow);
        require(world.same_calls <= 1 + 32 + 1, "at most the sentinel and 32 blocks are tested at a creation");
        require(counters().blocks_collected == expected[step], "32 blocks at a creation, until the end of the list");
    }
    world.same_calls = 0;
    end_ref(f, make_shadow_of(f, sample(50)).shadow);
    require(world.same_calls == 1, "and then one test again, of the new sentinel");
}

// The end is a synchronization point: a raw write followed by a frame end reaches the array.
void check_the_end_is_a_synchronization_point() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    Made made = make_held(f, sample(9000));
    raw_of(made)[7] = 0x71;
    raw_of(made)[5000] = 0x72;
    require(made.object->bytes[7] != 0x71, "Java has not seen the writes");
    pop(f.env, nullptr);
    require(is_ended_block(made.shadow), "the frame's end ended the block");
    require(made.object->bytes[7] == 0x71 && made.object->bytes[5000] == 0x72, "the writes reached the array at the end");
    // The same at a native method's return.
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    Made second = make_held(f, sample(100));
    raw_of(second)[3] = 0x73;
    leave_native(0x8000);
    require(is_ended_block(second.shadow) && second.object->bytes[3] == 0x73, "and at a return");
}

// The same array again: the same address, and Java's write to one chunk and the raw write to another both survive.
void check_the_same_array_again_gets_its_block() {
    Fixture f = make();
    Made first = make_held(f, sample(9000));
    const void* p = first.shadow;
    uint8_t* raw = raw_of(first);
    end_ref(f, first.shadow);
    first.object->bytes[3] = 0xaa;  // Java writes to chunk 0 meanwhile
    raw[16 + 5000 - 16] = 0;        // (no write: the block is only read here)
    raw[5000] = 0xbb;               // the application writes through its kept pointer to chunk 1
    Made again = obtain_again(f, first.object);
    require(again.shadow == p && is_shadow_block(again.shadow), "the same array gets its block back: the pointer it kept");
    require(counters().blocks_revived == 1 && counters().shadows_created == 1 && counters().shadows_ended == 0 &&
                counters().shadows_live == 1 && world.globals.size() == 1 && block_weaks() == 0,
            "a revival: no new block, a global reference again, the weak one gone");
    require(raw_of(again)[3] == 0xaa, "Java's write to chunk 0 reached the block");
    require(first.object->bytes[5000] == 0xbb, "the raw write to chunk 1 reached the array");
    require(raw_of(again)[5000] == 0xbb && first.object->bytes[3] == 0xaa, "and both survive on both sides");
    end_ref(f, again.shadow);
    require(is_ended_block(p), "it ends again");
}

// An exception pending at a frame end: the end is deferred, no call the runtime forbids is made.
void check_an_exception_pending_defers_a_frame_end() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    Made made = make_held(f, sample(100));
    raw_of(made)[1] = 0x44;
    world.exception_pending = true;
    pop(f.env, nullptr);
    require(world.exception_pending && is_shadow_block(made.shadow) && world.globals.size() == 1 && block_weaks() == 0 &&
                made.object->bytes[1] != 0x44,
            "the block waits, strong, with the raw write not yet pushed; the fake enforced the rules of CheckJNI");
    world.exception_pending = false;
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(is_ended_block(made.shadow) && made.object->bytes[1] == 0x44 && world.globals.empty() && block_weaks() == 1,
            "the next call without an exception finished the end");
}

// A thread that ends, and DetachCurrentThread: the blocks end, they are not freed.
void check_thread_end_and_detach_end_blocks() {
    Fixture f = make();
    struct Fns {
        static jint detach(Vm*) { return 0; }
        static jint other(Vm*, void**, jint) { return 0; }
    };
    void* vm_table[kVmSlotCount];
    for (void*& entry : vm_table) entry = reinterpret_cast<void*>(&Fns::other);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&Fns::detach);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    Vm vm{vm_table};
    const void* attached_block = nullptr;
    const void* ended_thread_block = nullptr;
    std::thread attached([&] {
        Made made = make_held(f, sample(64));
        attached_block = made.shadow;
        raw_of(made)[0] = 0x66;
        reinterpret_cast<jint (*)(Vm*)>(vm_table[vmslot::DetachCurrentThread])(&vm);
        require(is_ended_block(attached_block) && made.object->bytes[0] == 0x66, "detach ended the block and pushed the write");
        Made other = make_held(f, sample(65));
        ended_thread_block = other.shadow;
        release_thread_state(t_block);  // the thread ends with no detach
    });
    attached.join();
    require(is_shadow_block(ended_thread_block), "the block of a thread that ended waits for an environment, strong");
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(is_ended_block(ended_thread_block) && is_ended_block(attached_block) && counters().shadows_ended == 2 &&
                counters().shadows_live == 0,
            "and the next call with an environment ended it: both are ended, not freed");
}

// The arena is full of ended blocks whose arrays Java holds: one Runtime.gc(), a decline with the line, no second gc
// within 10 s, and success once the arrays are gone.
void check_an_arena_full_of_ended_blocks() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 100; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(100000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;  // the region is full
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() >= 10 && kept.size() < 100, "the region held some blocks and then no more");
    const uint32_t ended = counters().shadows_ended;
    require(counters().shadows_declined == 1 && ended == kept.size(), "the first that did not fit was declined");
    require(world.gc_calls == 1 && counters().collections_asked == 1, "after one Runtime.gc()");
    require(log_count("did not fit: the runtime was asked to collect") == 1 && log_count("stayed a real reference (no room") == 1 &&
                log_count("waiting for their arrays' collection") == 1,
            "the lines: the collection, the decline, the ended blocks waiting");
    for (const auto& line : world.logs) {
        if (line.message.find("waiting for their arrays' collection") != std::string::npos) {
            require(line.a == long(ended) && line.b == long(ended) * 100000, "with the ended blocks and bytes");
        }
    }
    world.clock_ms += 9000;
    Made again = make_shadow_of(f, std::vector<uint8_t>(100000, 0x55));
    require(!is_shadow_block(again.shadow) && world.gc_calls == 1, "no second collection within 10 s");
    world.clock_ms += 1100;
    Made third = make_shadow_of(f, std::vector<uint8_t>(100000, 0x56));
    require(!is_shadow_block(third.shadow) && world.gc_calls == 2, "a second one after 10 s: the arrays are still held");
    for (const Made& made : kept) made.object->held = false;
    world.clock_ms += 11000;
    Made fourth = make_shadow_of(f, std::vector<uint8_t>(100000, 0x57));
    require(is_shadow_block(fourth.shadow) && world.gc_calls == 3, "and when the arrays are not held, a collection makes room");
    require(counters().shadows_ended < ended, "the blocks of the gone arrays were freed");
}

// The sweep that makes room needs no sentinel: arrays the runtime cleared without the sentinel having been cleared
// are found by the full sweep, with no call to the runtime.
void check_the_full_sweep_does_not_need_the_sentinel() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 100; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(100000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() >= 10 && kept.size() < 32 && world.gc_calls == 1, "the region is full of ended blocks");
    for (const Made& made : kept) {
        for (Ref* weak : world.weaks) {
            if (weak->object == made.object) weak->object = nullptr;  // the runtime cleared these and not the sentinel
        }
    }
    world.clock_ms += 1000;
    Made fresh = make_shadow_of(f, std::vector<uint8_t>(100000, 0x77));
    require(is_shadow_block(fresh.shadow), "room was made");
    require(world.gc_calls == 1 && counters().blocks_collected == kept.size(), "by the full sweep, with no call to the runtime");
}

// The cap on the blocks that exist: 16384; a full sweep, a collection, then the result stays the real reference.
void check_the_cap_of_ended_blocks() {
    g_region = g_big_region;  // 16384 blocks do not fit in the small region
    g_region_size = sizeof(g_big_region);
    Fixture f = make();
    g_region = g_shadow_region;
    g_region_size = sizeof(g_shadow_region);
    std::vector<Obj*> kept;
    for (unsigned i = 0; i < 16384; ++i) {
        Made made = make_held(f, std::vector<uint8_t>());
        require(is_shadow_block(made.shadow), "a block up to the cap");
        kept.push_back(made.object);
        end_ref(f, made.shadow);
    }
    require(counters().shadows_ended == 16384 && counters().shadows_declined == 0, "16384 ended blocks");
    Made over = make_shadow_of(f, sample(10));
    require(!is_shadow_block(over.shadow) && counters().shadows_declined == 1 && world.gc_calls == 1,
            "the next result stayed the real reference, after one collection");
    require(log_count("too many blocks") == 1, "and the line names the cap");
    for (Obj* object : kept) object->held = false;
    world.clock_ms += 11000;
    Made after = make_shadow_of(f, sample(10));
    require(is_shadow_block(after.shadow) && counters().shadows_ended < 16384, "with the arrays gone a block fits again");
}

// 64 arrays of one length that Java keeps, each obtained 1000 times in turn: 64 blocks, 64 addresses, never a second
// block, no decline.
void check_arrays_that_java_keeps_are_obtained_again_and_again() {
    Fixture f = make();
    std::vector<Obj*> objects;
    std::vector<const void*> addresses;
    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> payload = sample(200);
        payload[0] = uint8_t(i);
        Made made = make_held(f, payload);
        objects.push_back(made.object);
        addresses.push_back(made.shadow);
        end_ref(f, made.shadow);
    }
    require(std::set<const void*>(addresses.begin(), addresses.end()).size() == 64, "64 addresses");
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 64; ++i) {
            Made again = obtain_again(f, objects[size_t(i)]);
            require(again.shadow == addresses[size_t(i)], "each array always gets its own block");
            end_ref(f, again.shadow);
        }
    }
    require(counters().shadows_created == 64 && counters().shadows_declined == 0 && counters().blocks_revived == 64000 &&
                counters().shadows_ended == 64 && counters().shadows_live == 0,
            "64 blocks, 64000 revivals, no second block, no decline");
}

// Arrays with the same identity hash are told apart by identity.
void check_arrays_with_equal_hashes_get_their_own_blocks() {
    Fixture f = make();
    world.same_hash = true;
    Made a = make_held(f, sample(100));
    Made b = make_held(f, sample(100));
    const void* pa = a.shadow;
    const void* pb = b.shadow;
    require(pa != pb, "two blocks");
    end_ref(f, a.shadow);
    end_ref(f, b.shadow);
    Made b_again = obtain_again(f, b.object);
    Made a_again = obtain_again(f, a.object);
    require(b_again.shadow == pb && a_again.shadow == pa, "each gets its own block back, however the hashes compare");
    Made c = make_shadow_of(f, sample(100));
    require(c.shadow != pa && c.shadow != pb && counters().shadows_created == 3, "a third array gets a third block");
}

// Two ended blocks for one array (it was live on two threads): the newest is found.
void check_the_newest_ended_block_of_an_array_is_found() {
    Fixture f = make();
    Made first = make_held(f, sample(100));
    const void* first_at = first.shadow;
    const void* second_at = nullptr;
    std::thread other([&] {
        Made second = obtain_again(f, first.object);  // live on the first thread: this thread gets a block of its own
        second_at = second.shadow;
        end_ref(f, second.shadow);
    });
    other.join();
    require(second_at != nullptr && second_at != first_at, "a block of its own on the other thread");
    end_ref(f, first.shadow);  // the first block ended last: it is the newest
    Made again = obtain_again(f, first.object);
    require(again.shadow == first_at, "the newest ended block is the one found");
    end_ref(f, again.shadow);
}

// Where the identity hash cannot be had the result stays the real reference: a block made without one could not be found
// again, and the array would get a second block. A failed look for the ids is made again after a while.
void check_no_identity_hash_declines_the_result() {
    g_ids_at_install = false;  // the lookup is left to the first result
    Fixture f = make();
    g_ids_at_install = true;
    world.find_fails = true;
    Made first = make_shadow_of(f, sample(100));
    require(!is_shadow_block(first.shadow) && counters().shadows_created == 0 && counters().shadows_declined == 1,
            "the result stays the real reference");
    require(log_count("no identity hash") == 1, "and the line says why");
    world.clock_ms += 100;
    require(!is_shadow_block(make_shadow_of(f, sample(100)).shadow), "within the retry interval the lookup is not repeated");
    world.find_fails = false;
    world.clock_ms += 2000;
    Made later = make_shadow_of(f, sample(100));
    require(is_shadow_block(later.shadow), "after the interval the lookup is made again and the failure was not kept for good");
}

// A look for the hash that fails once does not make a second block for an array that has one.
void check_a_failed_hash_call_makes_no_second_block() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    const void* at = made.shadow;
    end_ref(f, made.shadow);
    world.hash_fails = true;
    Made declined = obtain_again(f, made.object);
    require(!is_shadow_block(declined.shadow) && counters().shadows_created == 1 && !world.exception_pending,
            "the result stays the real reference, no second block, no exception left");
    world.hash_fails = false;
    Made again = obtain_again(f, made.object);
    require(again.shadow == at && counters().shadows_created == 1, "and the next result is the block that was there");
}

// No weak reference to give at an end: the block is not freed (the application may hold its pointer, and the array may be
// reachable only through it). It stays as it is, strong, and a later sweep gives it the weak reference.
void check_no_weak_reference_keeps_the_block_strong() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    const std::vector<uint8_t> payload = sample(100);
    world.weak_fails = true;
    end_ref(f, made.shadow);
    require(is_ended_block(made.shadow) && counters().shadows_live == 0 && counters().shadows_ended == 1 &&
                world.globals.size() == 1 && std::memcmp(raw_of(made), payload.data(), 100) == 0,
            "the block is ended and strong, its pointer reads");
    world.weak_fails = false;
    collect();
    end_ref(f, make_shadow_of(f, sample(10)).shadow);  // a creation after a collection sweeps
    require(block_weaks() == 2 && world.globals.size() == 0 && is_ended_block(made.shadow), "the sweep made it weak");
    Made again = obtain_again(f, made.object);
    require(again.shadow == made.shadow && counters().shadows_created == 2, "and the array gets the same block back");
}

// Raw access to an ended block reads the bytes as of its end, and Java's later writes do not reach it.
void check_an_ended_block_does_not_see_javas_later_writes() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    const uint8_t first = raw_of(made)[10];
    end_ref(f, made.shadow);
    made.object->bytes[10] = uint8_t(first + 1);
    require(raw_of(made)[10] == first, "the pointer still reads the bytes as of the end");
    raw_of(made)[11] = 0x5e;  // a raw write with nothing to carry it: the stated limit
    require(made.object->bytes[11] != 0x5e, "and a raw write does not reach Java until the array is obtained again");
}


// ---- pins, the references of ended blocks, and the merge (the review round of the block ends)

using SetRegion = void (*)(Env*, jarray, jsize, jsize, const jbyte*);
jbyte* get_elements(Fixture& f, jobject a) { return fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, a, nullptr); }
void release_elements(Fixture& f, jobject a, const void* p, jint mode) {
    fn<void (*)(Env*, jarray, const void*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, a, p, mode);
}
void* get_critical(Fixture& f, jobject a) {
    return fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical)(f.env, a, nullptr);
}
void release_critical(Fixture& f, jobject a, void* p, jint mode) {
    fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical)(f.env, a, p, mode);
}
// The object address of the block whose data starts at `p`.
const uint8_t* object_at(const void* p) { return static_cast<const uint8_t*>(p) - 16; }
jsize length_of(Fixture& f, jobject a) { return fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, a); }

// A pointer from Get...Elements that is never released keeps the array reachable and the block valid, as Dalvik's pin
// did: no collection takes it and no later block takes its address.
void check_a_pointer_never_released_keeps_its_array() {
    Fixture f = make();
    const std::vector<uint8_t> payload = sample(4096);
    Made kept = make_shadow_of(f, payload);  // Java holds nothing of this array
    jbyte* p = get_elements(f, kept.shadow);
    require(reinterpret_cast<uint8_t*>(p) == raw_of(kept), "the pointer is the block's bytes");
    end_ref(f, kept.shadow);  // the native method returns; the pointer is never released
    require(is_ended_block(object_at(p)) && counters().shadows_ended == 1 && world.globals.size() == 1 && block_weaks() == 0,
            "the block ended, still held by a strong reference");
    collect();
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> other = sample(4096);
        other[0] = uint8_t(i + 1);
        Made temporary = make_shadow_of(f, other);
        require(temporary.shadow != kept.shadow, "no new block takes the kept pointer's address");
        end_ref(f, temporary.shadow);
        if (i % 10 == 9) collect();
    }
    require(is_ended_block(object_at(p)) && std::memcmp(p, payload.data(), payload.size()) == 0, "the kept pointer still reads its bytes");
    // The array obtained again is the block, with its pin; the release through it ends the pin, and then the array can go.
    Made again = obtain_again(f, kept.object);
    require(again.shadow == kept.shadow && is_shadow_block(again.shadow), "the same array gets its block back");
    release_elements(f, again.shadow, p, 0);
    end_ref(f, again.shadow);
    require(block_weaks() >= 1 && is_ended_block(object_at(p)), "unpinned, the block ended weakly like any other");
    const uint32_t freed = counters().blocks_collected;
    collect();
    for (int i = 0; i < 40 && counters().blocks_collected == freed + 0; ++i) {
        end_ref(f, make_shadow_of(f, sample(10)).shadow);  // a creation tests ended blocks
    }
    require(counters().blocks_collected > freed, "and a collection freed it");
}

// A Release goes to the block its pointer lies in, through whichever reference it arrives, and never to the runtime;
// COMMIT keeps the pin, mode 0 and ABORT end it; the last pin of an ended block through a critical release waits for a sweep.
void check_a_release_goes_to_the_block_its_pointer_lies_in() {
    Fixture f = make();
    Made m = make_held(f, sample(100));
    jbyte* p = get_elements(f, m.shadow);
    Ref* real = new_local(m.object);  // another reference to the same array
    p[0] = 0x44;
    world.released.clear();
    release_elements(f, real, p, kReleaseCommit);
    require(std::count(world.released.begin(), world.released.end(), static_cast<const void*>(p)) == 0,
            "a release by a real reference with the block's pointer is not the runtime's");
    require(m.object->bytes[0] == 0x44, "it synchronized the block");
    end_ref(f, m.shadow);
    require(is_ended_block(object_at(p)) && world.globals.size() == 1, "COMMIT did not end the pin: the ended block is held strongly");
    release_elements(f, real, p, 0);
    require(std::count(world.released.begin(), world.released.end(), static_cast<const void*>(p)) == 0 && world.globals.empty() &&
                block_weaks() == 1,
            "mode 0 through the real reference ended the pin of the ended block: it waits weakly now");
    // ABORT, a stale reference of the ended block, and a pointer that is a freed block's
    Made n = make_held(f, sample(100));
    jbyte* q = get_elements(f, n.shadow);
    end_ref(f, n.shadow);
    release_elements(f, n.shadow, q, kReleaseAbort);
    require(std::count(world.released.begin(), world.released.end(), static_cast<const void*>(q)) == 0 && block_weaks() == 2,
            "ABORT through the ended block's own reference ended the pin too");
    n.object->held = false;
    m.object->held = false;
    end_ref(f, real);  // the plain reference to the array goes too
    const uint32_t freed = counters().blocks_collected;
    collect();
    for (int i = 0; i < 40 && counters().blocks_collected < freed + 2; ++i) end_ref(f, make_shadow_of(f, sample(10)).shadow);
    require(counters().blocks_collected >= freed + 2, "the blocks were freed");
    Ref* plain = new_local(m.object);
    release_elements(f, plain, q, 0);
    require(std::count(world.released.begin(), world.released.end(), static_cast<const void*>(q)) == 0,
            "a freed block's pointer is not the runtime's either");
    // the critical pair: the last pin of an ended block waits for a sweep
    Made c = make_shadow_of(f, sample(100));
    void* r = get_critical(f, c.shadow);
    end_ref(f, c.shadow);
    world.released.clear();
    release_critical(f, new_local(c.object), r, 0);
    require(world.critical == 0 && is_ended_block(object_at(r)), "a critical release by a real reference: the block's");
    for (const void* released : world.released) {
        require(released != r, "the runtime was never handed the block's pointer");
    }
    const size_t strong = world.globals.size();
    collect();
    end_ref(f, make_shadow_of(f, sample(10)).shadow);  // a creation after a collection sweeps, and the unpinned block weakens
    require(world.globals.size() < strong, "the sweep gave the unpinned block's strong reference up");
}

// The reference of an ended block works in every wrapper (an application keeps a byte[] in a C global across native
// calls). The use makes no entry and moves no block: the block stays ended, and the wrapper hands the runtime a local
// reference of its own that it deletes when it returns. A release, a delete and the length ask nothing of the array. The
// reference of a block whose array was collected converts to nothing.
void check_the_reference_of_an_ended_block_keeps_working() {
    for (int with_live = 0; with_live < 2; ++with_live) {
        Fixture f = make();
        const std::vector<uint8_t> payload = sample(100);
        Made other = with_live ? make_held(f, sample(50)) : Made{nullptr, nullptr};
        Made kept = make_held(f, payload);
        end_ref(f, kept.shadow);
        require(is_ended_block(kept.shadow) && !is_shadow_block(kept.shadow), "the block ended");
        const uint32_t revived = counters().blocks_revived;
        const size_t locals = world.locals.size();
        const auto unchanged = [&](const char* what) {
            require(is_ended_block(kept.shadow) && counters().blocks_revived == revived && counters().shadows_live == (with_live ? 1u : 0u) &&
                        world.locals.size() == locals,
                    what);
        };

        require(length_of(f, kept.shadow) == 100, "the length");
        unchanged("the length asks nothing");
        jbyte* p = get_elements(f, kept.shadow);
        require(p == reinterpret_cast<jbyte*>(raw_of(kept)) && std::memcmp(p, payload.data(), 100) == 0, "the elements are the block's");
        unchanged("the elements made no entry and left no local reference");
        require(world.globals.size() == size_t(1 + with_live), "the pointer pinned the block with a strong reference of its own");
        release_elements(f, kept.shadow, p, 0);
        unchanged("the release too");
        require(block_weaks() == 1 && world.globals.size() == size_t(with_live), "the last release gave the strong reference up");

        jbyte buffer[10] = {};
        fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, kept.shadow, 5, 10, buffer);
        require(std::memcmp(buffer, payload.data() + 5, 10) == 0, "a region read has the bytes");
        const jbyte patch[3] = {1, 2, 3};
        fn<SetRegion>(*f.table, slot::SetByteArrayRegion)(f.env, kept.shadow, 0, 3, patch);
        require(kept.object->bytes[0] == 1 && kept.object->bytes[2] == 3 && raw_of(kept)[1] == 2, "a region write reaches the array");
        unchanged("the regions");

        require(call_static_int_v(f, f.consume, kept.shadow) == 7 && world.last_array_args.size() == 1 &&
                    world.last_array_args[0] == kept.object,
                "a call is handed the real array");
        unchanged("the call");
        world.next_result = nullptr;  // the call returns nothing
        call_object_v(f, kept.shadow, f.instance_bytes);
        require(world.seen_receiver.size() == 100, "and so is the receiver of an instance call");
        unchanged("the instance call");

        require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, kept.shadow, kept.shadow) == 1,
                "IsSameObject");
        jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, kept.shadow);
        require(global != nullptr && object_of(global) == kept.object, "NewGlobalRef is a real reference to the array");
        fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteGlobalRef)(f.env, global);
        world.next_result = nullptr;
        fn<void (*)(Env*, jobject, jfieldID, jobject)>(*f.table, set_field_slot(kObject))(f.env, f.cls, f.bytes_field, kept.shadow);
        unchanged("the identity and the field");

        // A delete, a release of a buffer the layer did not hand out, the critical pair.
        world.release_calls = 0;
        end_ref(f, kept.shadow);
        release_elements(f, kept.shadow, payload.data(), 0);
        void* critical = get_critical(f, kept.shadow);
        require(critical == raw_of(kept), "the critical entry gives the block's memory");
        release_critical(f, kept.shadow, critical, 0);
        bool handed = false;
        for (const void* released : world.released) handed = handed || released == critical || released == payload.data();
        require(!handed && is_ended_block(kept.shadow) && counters().blocks_revived == revived,
                "none of them handed the runtime the block's pointer or revived the block");
        (void)other;
    }

    // A block whose array was collected: the reference converts to nothing, and nothing in the layer crashes.
    Fixture f = make();
    Made gone = make_shadow_of(f, sample(100));
    end_ref(f, gone.shadow);
    collect();
    require(length_of(f, gone.shadow) == 100, "the length is the block's");
    require(get_elements(f, gone.shadow) == nullptr, "no elements: the array is gone");
    jbyte buffer[4] = {1, 1, 1, 1};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, gone.shadow, 0, 4, buffer);
    require(buffer[0] == 1, "a region read copies nothing");
    require(call_static_int_v(f, f.consume, gone.shadow) == 7 && world.last_array_args.size() == 1 &&
                world.last_array_args[0] == nullptr,
            "a call is handed null");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, gone.shadow, nullptr) == 1,
            "and the reference is the null reference");
}

// The merge is byte by byte in every synchronization, the end and the revival included: a byte Java wrote in a chunk the
// application also wrote survives.
void check_a_merge_is_byte_exact_across_an_end() {
    Fixture f = make();
    Made made = make_held(f, std::vector<uint8_t>(8192, 0x10));
    raw_of(made)[20] = 0xB2;        // the application, through the pointer
    made.object->bytes[10] = 0xA1;  // Java, in the same chunk
    end_ref(f, made.shadow);
    require(made.object->bytes[10] == 0xA1 && made.object->bytes[20] == 0xB2, "the end merged both into the array");
    Made again = obtain_again(f, made.object);
    require(again.shadow == made.shadow && raw_of(again)[10] == 0xA1 && raw_of(again)[20] == 0xB2, "and the revived block has both");
    raw_of(again)[30] = 0xC3;
    made.object->bytes[31] = 0xC4;
    void* pinned = get_critical(f, again.shadow);
    require(made.object->bytes[30] == 0xC3 && made.object->bytes[31] == 0xC4 && raw_of(again)[31] == 0xC4,
            "the critical pair merges the same way");
    release_critical(f, again.shadow, pinned, kReleaseAbort);
    // A copy of the elements the application wrote into and released: only the bytes it changed.
    jbyte* elements = get_elements(f, again.shadow);
    made.object->bytes[40] = 0xD0;  // Java, after the elements were taken
    std::vector<uint8_t> copy(elements, elements + 8192);
    copy[50] = 0xD1;
    release_elements(f, again.shadow, copy.data(), 0);
    require(made.object->bytes[50] == 0xD1 && made.object->bytes[40] == 0xD0, "a release of a copy: its changed byte, and Java's");
}

// Declined results do not sweep again and again: a full sweep needs a collection since the last one, or a second.
void check_declined_results_do_not_repeat_a_full_sweep() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 100; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(100000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() >= 10 && world.gc_calls == 1 && counters().shadows_declined == 1, "the region is full of ended blocks");
    int before = world.same_calls;
    for (int i = 0; i < 20; ++i) {
        require(!is_shadow_block(make_shadow_of(f, std::vector<uint8_t>(100000, uint8_t(i))).shadow), "declined");
    }
    require(world.same_calls - before <= 3 * 20, "no full sweep: no more than the tests of the sentinel for each");
    world.clock_ms += 1000;  // the fallback that does not depend on the sentinel
    before = world.same_calls;
    make_shadow_of(f, std::vector<uint8_t>(100000, 1));
    require(world.same_calls - before >= int(kept.size()), "after a second, one full sweep");
    before = world.same_calls;
    make_shadow_of(f, std::vector<uint8_t>(100000, 2));
    require(world.same_calls - before <= 4, "and only one");
    collect();
    before = world.same_calls;
    make_shadow_of(f, std::vector<uint8_t>(100000, 3));
    require(world.same_calls - before >= int(kept.size()), "a collection since the last one makes another at once");
}

// A second thread that obtains the array while an end runs gets the block the end makes, not a block of its own: the weak
// reference and the index entry come in one step under the lock.
void check_an_end_and_a_second_thread_make_one_block() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    Obj* object = made.object;
    std::atomic<bool> done{false};
    const void* other_at = nullptr;
    std::thread other;
    g_on_global_deleted = [&](Obj* gone) {  // inside the end: the strong reference goes, the block is not in the index yet
        if (gone != object || other.joinable()) return;
        world.next_result = new_local(object);
        other = std::thread([&] {
            other_at = call_static_object_v(f, f.get_bytes);
            done = true;
        });
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    end_ref(f, made.shadow);
    g_on_global_deleted = nullptr;
    other.join();
    require(other_at == made.shadow && counters().shadows_created == 1, "the other thread got the block back");
    end_ref(f, const_cast<void*>(other_at));
}

// The same for the ends that waited for an environment (a thread that ended, an exception): the exchange and the ends are
// one step.
void check_draining_the_ends_and_a_second_thread_make_one_block() {
    Fixture f = make();
    Obj* object = nullptr;
    const void* first_at = nullptr;
    std::thread owner([&] {
        Made made = make_held(f, sample(100));
        object = made.object;
        first_at = made.shadow;
        release_thread_state(t_block);  // the thread ends with no detach: its block waits for an environment
    });
    owner.join();
    std::atomic<bool> done{false};
    const void* other_at = nullptr;
    std::thread other;
    g_on_global_deleted = [&](Obj* gone) {
        if (gone != object || other.joinable()) return;
        world.next_result = new_local(object);
        other = std::thread([&] {
            other_at = call_static_object_v(f, f.get_bytes);
            done = true;
        });
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    jobject plain = byte_array(sample(4));
    length_of(f, plain);  // the next call with an environment drains
    g_on_global_deleted = nullptr;
    if (other.joinable()) other.join();
    require(other_at == first_at && counters().shadows_created == 1, "the other thread got the block back");
}

// The lookup of the classes runs Java and may enter the layer again, on the same thread: it holds no lock of the layer.
void check_the_class_lookup_may_enter_the_layer() {
    g_ids_at_install = false;  // the lookup is left to the first result
    Fixture f = make();
    g_ids_at_install = true;
    bool inner_block = false;
    g_on_find_class = [&] {
        Made inner = make_shadow_of(f, sample(10));
        inner_block = is_shadow_block(inner.shadow);
        end_ref(f, inner.shadow);
    };
    Made outer = make_held(f, sample(20));
    require(!inner_block && is_shadow_block(outer.shadow),
            "the result made during the lookup stays the real reference (no hash yet); the outer one is a block");
    end_ref(f, outer.shadow);
    Made again = obtain_again(f, outer.object);
    require(again.shadow == outer.shadow, "and the lookup finished: the array is found again");
}


// ---- the use of a kept reference by other threads, and the accounting of the blocks that exist

// Another thread: an environment of its own over the same table, as every thread of the runtime has.
struct Other {
    Env env;
    Fixture f;
    explicit Other(const Fixture& base) : env(*base.env), f(base) { f.env = &env; }
    Other(const Other&) = delete;
    Other& operator=(const Other&) = delete;
};

// A hand-off between the threads of a case: waits for the counter another thread of the case advances (`turn`, `stage`) to
// reach `value`. A hand-off that never comes fails the case within the limit, with a line that names the case, the waiter and the
// counter's value; the unbounded wait this replaces ended only when the test's own timeout killed the binary.
constexpr std::chrono::seconds kHandOffLimit{30};
void wait_for_counter(const char* the_case, const char* counter_name, const std::atomic<int>& counter, int value,
                      const char* waiter) {
    if (waitUntil([&] { return counter.load() == value; }, kHandOffLimit, std::chrono::microseconds(100))) return;
    uml::testing::fail(std::string(the_case) + ": " + waiter + " waited " + std::to_string(kHandOffLimit.count()) + " s for " +
                       counter_name + " " + std::to_string(value) + ", and it is " + std::to_string(counter.load()));
}

// A kept reference used on a second thread while the owner's native method returns: the call of the second thread is
// handed a local reference of its own, never the global reference that the end deletes. The block stays where it was (it
// ended), and the second thread, with no native frame, holds nothing afterwards.
void check_a_kept_reference_on_a_second_thread_survives_the_owners_end() {
    Fixture f = make();
    Made kept = make_held(f, sample(100));  // live on this thread
    Other other(f);
    std::atomic<int> stage{0};
    g_in_call = [&] {
        g_in_call = nullptr;
        stage = 1;
        wait_for_counter("check_a_kept_reference_on_a_second_thread_survives_the_owners_end", "stage", stage, 2,
                         "the second thread's call (the owner's end is under way)");
    };
    jint result = 0;
    std::thread second([&] {
        result = call_static_int_v(other.f, f.consume, kept.shadow);
        stage = 3;
    });
    wait_for_counter("check_a_kept_reference_on_a_second_thread_survives_the_owners_end", "stage", stage, 1,
                     "the owner (the second thread's call is under way)");
    end_ref(f, kept.shadow);  // the owner's native method returns while the call is under way
    stage = 2;
    second.join();
    require(result == 7 && world.last_array_args.size() == 1 && world.last_array_args[0] == kept.object,
            "the call was handed the array, a live reference");
    require(is_ended_block(kept.shadow) && counters().shadows_live == 0 && counters().blocks_revived == 0 &&
                world.globals.empty() && block_weaks() == 1,
            "the block ended, as the owner's return made it; the second thread made no entry");
    Made again = obtain_again(f, kept.object);
    require(again.shadow == kept.shadow && counters().shadows_created == 1, "and the array gets the same block back");
}

// A kept reference used on a thread with no native frame (an attached audio thread) makes no entry: nothing on that thread
// would end it, and the owner would get a second block for the array.
void check_a_kept_reference_on_a_thread_without_frames_makes_no_entry() {
    Fixture f = make();
    Made kept = make_held(f, sample(100));
    end_ref(f, kept.shadow);
    Other other(f);
    std::thread second([&] {
        require(length_of(other.f, kept.shadow) == 100, "the length");
        jbyte* p = get_elements(other.f, kept.shadow);
        require(p == reinterpret_cast<jbyte*>(raw_of(kept)), "the elements are the block's");
        release_elements(other.f, kept.shadow, p, 0);
        require(call_static_int_v(other.f, f.consume, kept.shadow) == 7, "a call");
    });
    second.join();
    require(is_ended_block(kept.shadow) && counters().shadows_live == 0 && counters().blocks_revived == 0,
            "the block stayed ended and no thread holds an entry of it");
    Made again = obtain_again(f, kept.object);
    require(again.shadow == kept.shadow && counters().shadows_created == 1, "the owner gets the same block");
}

// A thread that holds 512 entries can still use a kept reference: the use needs no entry.
void check_a_thread_at_its_limit_can_use_a_kept_reference() {
    Fixture f = make();
    Made kept = make_held(f, sample(100));
    end_ref(f, kept.shadow);
    for (int i = 0; i < 512; ++i) {
        require(is_shadow_block(make_shadow_of(f, sample(10)).shadow), "an entry up to the limit");
    }
    jbyte* p = get_elements(f, kept.shadow);
    require(p == reinterpret_cast<jbyte*>(raw_of(kept)), "the elements");
    release_elements(f, kept.shadow, p, 0);
    require(call_static_int_v(f, f.consume, kept.shadow) == 7 && world.last_array_args[0] == kept.object, "and a call");
}

// ---- no owner exemption: every wrapper holds a reference of its own, every synchronization runs under the mutex

// Every wrapper that gives the runtime an array for a block hands it a reference of the wrapper's own, also on the
// block's owner thread: the block's global stops being valid at the wrapper's first runtime call, as the end of the block by
// a callback would make it, and each wrapper still works. (Fails on c3b1ffc7: the owner's wrappers used the global.)
void check_no_wrapper_hands_the_runtime_the_blocks_own_reference() {
    Fixture f = make();
    Made made = make_held(f, sample(64));
    Made other = make_held(f, sample(64));
    jbyte buffer[64] = {};
    const auto revoked = [&](auto&& use) {
        raw_of(made)[3] = 0x77;  // something for the synchronization to do
        world.revoke_next = true;
        use();
        require(!world.revoke_next, "the wrapper called the runtime");
        world.revoke_next = false;
        world.revoked.clear();
    };
    revoked([&] { fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 64, buffer); });
    require(static_cast<uint8_t>(buffer[3]) == 0x77, "the region read has the raw write");
    revoked([&] {
        const jbyte one = 0x12;
        fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, made.shadow, 5, 1, &one);
    });
    jbyte* elements = nullptr;
    revoked([&] { elements = get_elements(f, made.shadow); });
    revoked([&] { release_elements(f, made.shadow, elements, 0); });
    void* critical = nullptr;
    revoked([&] { critical = get_critical(f, made.shadow); });
    revoked([&] { release_critical(f, made.shadow, critical, 0); });
    revoked([&] { require(call_static_int_v(f, f.consume, made.shadow) == 7, "a call with the block as its argument"); });
    revoked([&] {
        auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
        call_object_v(f, made.shadow, get_method(f.env, f.cls, "clone", "()Ljava/lang/Object;"));
    });
    revoked([&] {
        require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, made.shadow, other.shadow) == 0,
                "identity");
    });
    revoked([&] { fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorEnter)(f.env, made.shadow); });
    revoked([&] { fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorExit)(f.env, made.shadow); });
    require(length_of(f, made.shadow) == 64, "the length asks nothing of the array");
    require(made.object->bytes[3] == 0x77 && made.object->bytes[5] == 0x12, "and the writes reached the array");
}

// Java code that runs during a call ends the block (a native method it calls deletes the last reference): the call's
// synchronization after it goes on with the wrapper's own reference, and Java's writes during the call reach the block
// that has ended. (Fails on c3b1ffc7: the after-synchronization used the deleted global.)
void check_a_callback_that_ends_the_block_leaves_the_call_a_live_reference() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(32, 0x11));
    raw_of(a)[0] = 0x22;
    g_in_call = [&] {
        g_in_call = nullptr;
        end_ref(f, a.shadow);
        require(is_ended_block(a.shadow), "the callback ended the block");
    };
    world.java_fill = 0x44;
    require(call_static_int_v(f, f.consume, a.shadow) == 7, "the call completed");
    world.java_fill = -1;
    require(is_ended_block(a.shadow), "the block ended");
    require(raw_of(a)[0] == 0x44 && raw_of(a)[31] == 0x44, "Java's writes during the call reached the ended block");
    require(world.globals.empty() && block_weaks() == 1, "the block holds a weak reference now, and no global");
}

// The owner's synchronization is serialized with another thread's access to the block: a write that completes while the
// owner is merging is not undone by it. The hook runs in the runtime's ReleasePrimitiveArrayCritical that closes the
// merge's pair, which the layer makes inside its second hold (the other thread's SetByteArrayRegion starts then and
// waits for the mutex). (Fails on c3b1ffc7: the owner's merge wrote its stale snapshot over block and baseline;
// Java=31,42 and block=00,00 afterwards.)
void check_the_owners_merge_is_not_undone_by_another_threads_write() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(32, 0));
    Other other(f);
    raw_of(a)[0] = 0x31;
    std::thread worker;
    std::atomic<bool> finished{false};
    g_in_release_critical = [&] {
        worker = std::thread([&] {
            const jbyte value = 0x42;
            fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(other.f.env, a.shadow, 1, 1, &value);
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        while (!finished && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(!finished, "the other thread's write waits for the owner's merge: the mutex is held");
    };
    jbyte result[32] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 32, result);
    worker.join();
    require(a.object->bytes[0] == 0x31 && a.object->bytes[1] == 0x42, "Java has both writes");
    require(raw_of(a)[0] == 0x31 && raw_of(a)[1] == 0x42, "and so does the block");
    jbyte again[32] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 32, again);
    require(static_cast<uint8_t>(again[0]) == 0x31 && static_cast<uint8_t>(again[1]) == 0x42, "and the next read agrees");
}

// The reverse order: the owner pulls what Java wrote (the baseline takes it first), and another thread's synchronization of
// the same block, through a kept reference, must not read that half-way state as an application write and push the old byte
// back over Java's. (Fails on c3b1ffc7: Java's 0xA1 was lost.)
void check_another_threads_synchronization_does_not_undo_the_owners_pull() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(64, 0x3a));
    Other other(f);
    a.object->bytes[10] = 0xA1;  // Java wrote it; nobody wrote through the pointer
    std::thread worker;
    std::atomic<bool> finished{false};
    g_after_get = [&] {
        worker = std::thread([&] {
            jbyte* p = get_elements(other.f, a.shadow);
            release_elements(other.f, a.shadow, p, 0);
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        while (!finished && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    };
    jbyte result[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 64, result);
    worker.join();
    require(a.object->bytes[10] == 0xA1 && raw_of(a)[10] == 0xA1 && static_cast<uint8_t>(result[10]) == 0xA1,
            "Java's write is in the array, the block and the owner's read");
}

// The end of a block is a synchronization point too, and runs under the mutex: a second thread's access to the kept
// reference meets a synchronized block. The hook runs in the Release that closes the end's merge pair, which the layer
// makes inside its second hold, where the second thread starts its own SetByteArrayRegion. (Fails on c3b1ffc7: the end
// pushed before it took the mutex.)
void check_an_end_is_serialized_with_a_second_threads_access() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(32, 0));
    Other other(f);
    raw_of(a)[0] = 0x31;
    std::thread worker;
    std::atomic<bool> finished{false};
    g_in_release_critical = [&] {  // (the end's push is a merge of the dirty chunk, in place)
        worker = std::thread([&] {
            const jbyte value = 0x42;
            fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(other.f.env, a.shadow, 1, 1, &value);
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        while (!finished && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(!finished, "the other thread's access waits for the end: the mutex is held");
    };
    end_ref(f, a.shadow);
    worker.join();
    require(is_ended_block(a.shadow), "the block ended");
    require(a.object->bytes[0] == 0x31 && a.object->bytes[1] == 0x42, "Java has the application's write and the other thread's");
    require(raw_of(a)[0] == 0x31 && raw_of(a)[1] == 0x42, "and so does the block");
}

// ---- the runtime's own pointers released through a block's reference

// A pointer from GetPrimitiveArrayCritical on a real reference (a global the application made of a block), released
// through the block's own reference: the runtime's Release runs, so its critical region closes and the layer's count
// goes down with it. (Fails on 27123b5b: the Release was swallowed; the region stayed open and the kept reference
// of an ended block could no longer be given a critical pointer.)
void check_a_critical_pointer_of_the_runtime_released_through_a_block_reaches_the_runtime() {
    Fixture f = make();
    Made made = make_held(f, sample(64));
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow);
    require(!is_shadow_block(global), "a real global");
    void* pointer = get_critical(f, global);
    require(pointer != nullptr && pointer == made.object->bytes.data() && world.critical == 1, "the runtime's region is open");
    const int before = world.release_calls;
    release_critical(f, made.shadow, pointer, 0);
    require(world.critical == 0 && world.release_calls == before + 1 && world.released.back() == pointer,
            "the runtime's own Release ran with its own pointer");
    end_ref(f, made.shadow);  // the block ends: a kept reference with only a weak reference behind it
    void* mine = get_critical(f, made.shadow);
    require(mine == static_cast<void*>(raw_of(made)), "the layer's count is zero: the kept reference is given its pointer");
    release_critical(f, made.shadow, mine, 0);
    // The same through an ended block, whose array only a weak reference holds, inside the region the pointer opened.
    void* again = get_critical(f, global);
    const int second = world.release_calls;
    release_critical(f, made.shadow, again, 0);
    require(world.critical == 0 && world.release_calls == second + 1, "also through an ended block");
}

// The elements twin: the runtime's copy is released (not leaked), and the layer unpins nothing: a pin of its own on the
// block stays, so the ended block keeps its strong reference until its own pointer is released.
void check_an_elements_pointer_of_the_runtime_released_through_a_block_reaches_the_runtime() {
    Fixture f = make();
    Made made = make_held(f, sample(64));
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow);
    jbyte* mine = get_elements(f, made.shadow);  // the layer's own pin
    jbyte* theirs = get_elements(f, global);
    require(theirs == reinterpret_cast<jbyte*>(made.object->bytes.data()) && mine == reinterpret_cast<jbyte*>(raw_of(made)),
            "two different pointers");
    const size_t released = world.released.size();
    release_elements(f, made.shadow, theirs, 0);
    require(world.released.size() == released + 1 && world.released.back() == theirs,
            "the runtime's Release ran with its own pointer");
    end_ref(f, made.shadow);
    require(is_ended_block(made.shadow) && world.globals.size() == 2, "the block's pin is still held: it stays strong");
    release_elements(f, made.shadow, mine, 0);
    require(block_weaks() == 1 && world.globals.size() == 1, "its own release gives the strong reference up");
}

// A pointer the runtime handed out before the process had a block is still the runtime's when a block made later for the same array
// releases it: the layer records the runtime's pointers from the first one on, not from the first block. (Fails on a layer that
// records them only once a block has been made: the release is taken for the application's buffer and never reaches the runtime.)
void check_a_runtime_elements_pointer_got_before_the_first_block_is_released_by_the_runtime() {
    for (const jint mode : {jint(0), jint(kReleaseCommit), jint(kReleaseAbort)}) {
        Fixture f = make();
        jobject array = byte_array(sample(64));
        Obj* object = object_of(array);
        object->held = true;
        require(counters().shadows_created == 0, "no block has been made yet");
        jbyte* theirs = get_elements(f, array);  // the runtime's pointer, before any block
        Made made = obtain_again(f, object);      // the first block, of the same array
        require(is_shadow_block(made.shadow) && counters().shadows_created == 1, "the first block");
        const size_t before = world.released.size();
        release_elements(f, made.shadow, theirs, mode);
        require(world.released.size() == before + 1 && world.released.back() == theirs,
                "the runtime's own Release ran with its own pointer, whatever the mode");
        if (mode == kReleaseCommit) {  // the pointer stays valid after a commit: the record stays, and the final release finds it
            release_elements(f, made.shadow, theirs, 0);
            require(world.released.size() == before + 2, "the release that ends a committed pointer reached the runtime too");
        }
        release_elements(f, made.shadow, theirs, 0);
        require(world.released.size() == before + (mode == kReleaseCommit ? 2 : 1), "and the record is gone after the last release");
    }
}

// The same for a critical pointer, which another thread holds when the first block is made: its region closes at the release through
// the block's reference, and the layer's count of that thread's regions goes down with it.
void check_a_runtime_critical_pointer_got_before_the_first_block_closes_its_region_at_the_release() {
    constexpr const char* kCase = "check_a_runtime_critical_pointer_got_before_the_first_block_closes_its_region_at_the_release";
    Fixture f = make();
    Other holder(f);
    jobject array = byte_array(sample(64));
    Obj* object = object_of(array);
    object->held = true;
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, array);
    require(counters().shadows_created == 0, "no block has been made yet");
    std::atomic<int> stage{0};
    std::atomic<jobject> shadow{nullptr};
    int released_by_runtime = 0;
    std::thread thread([&] {
        void* pointer = get_critical(holder.f, global);  // the runtime's region opens on this thread, before any block
        stage = 1;
        wait_for_counter(kCase, "stage", stage, 2, "the thread that holds the pointer (the first block is being made)");
        const int before = world.release_calls;
        release_critical(holder.f, shadow.load(), pointer, 0);
        released_by_runtime = world.release_calls - before;
        stage = 3;
    });
    wait_for_counter(kCase, "stage", stage, 1, "the first thread (the other holds a critical pointer)");
    Made made = obtain_again(f, object);
    require(is_shadow_block(made.shadow) && counters().shadows_created == 1, "the first block, of the same array");
    shadow = made.shadow;
    stage = 2;
    thread.join();
    require(released_by_runtime == 1 && world.critical == 0, "the runtime's own Release ran and its critical region closed");
}

// JNI_COMMIT keeps the record (the pointer stays valid); mode 0 removes it, and then the address is nobody's again; a
// release through the real reference removes it too.
void check_the_record_of_a_runtime_pointer_ends_with_its_release() {
    Fixture f = make();
    Made made = make_held(f, sample(64));
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow);
    const size_t start = world.released.size();  // (the layer's own synchronizations release too: count from here)
    jbyte* theirs = get_elements(f, global);
    release_elements(f, made.shadow, theirs, 1 /* JNI_COMMIT */);
    release_elements(f, made.shadow, theirs, 0);
    require(world.released.size() == start + 2, "the commit kept the pointer's record; the release after it reached the runtime too");
    release_elements(f, made.shadow, theirs, 0);
    require(world.released.size() == start + 2, "the record is gone: the address is a buffer of the application's now");
    jbyte* second = get_elements(f, global);
    release_elements(f, global, second, 0);
    require(world.released.size() == start + 3, "a release through the real reference");
    release_elements(f, made.shadow, second, 0);
    require(world.released.size() == start + 3, "and it removed the record");
    // Two Gets of the same address (a runtime that gives the array's own memory): one record each.
    jbyte* one = get_elements(f, global);
    jbyte* two = get_elements(f, global);
    require(one == two, "the same address");
    release_elements(f, made.shadow, one, 0);
    release_elements(f, made.shadow, two, 0);
    require(world.released.size() == start + 5, "each release reached the runtime");
}

// Two threads get and release the runtime's pointers at the same time: the count of the records, which the unlocked fast path
// reads, is changed by each Get and Release under the table's lock, so that every access to it must be atomic. (The row's
// weight is under ThreadSanitizer, which reports a plain write against the unlocked read; in a plain build it checks that the
// pointers of both threads come back to the runtime and that nothing is left recorded.)
void check_two_threads_getting_and_releasing_pointers_race_on_no_count() {
    constexpr const char* kCase = "check_two_threads_getting_and_releasing_pointers_race_on_no_count";
    Fixture f = make();
    Other a(f);
    Other b(f);
    Made made = make_held(f, sample(100));
    jobject real_a = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(a.f.env, made.shadow);
    jobject real_b = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(b.f.env, byte_array(sample(90)));
    const size_t start = world.released.size();
    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    constexpr int kRounds = 4000;
    const auto work = [&](Fixture& local, jobject real) {
        ++ready;
        wait_for_counter(kCase, "go", go, 1, "a worker (the other worker is starting)");
        for (int n = 0; n < kRounds; ++n) {
            jbyte* pointer = get_elements(local, real);
            release_elements(local, real, pointer, 0);
        }
    };
    std::thread one([&] { work(a.f, real_a); });
    std::thread two([&] { work(b.f, real_b); });
    wait_for_counter(kCase, "ready", ready, 2, "the case (the workers start)");
    go = 1;
    one.join();
    two.join();
    require(world.released.size() == start + 2u * kRounds, "every release of both threads reached the runtime");
    const size_t after = world.released.size();
    jbyte* last = get_elements(f, real_a);
    release_elements(f, made.shadow, last, 0);
    release_elements(f, made.shadow, last, 0);
    require(world.released.size() == after + 1, "and nothing stayed recorded: a second release is the application's own");
}

// ---- DeleteLocalRef only reaches the innermost frame

// A nested native method (called from Java by the outer one, which holds the block's entry) deletes a reference of the
// block that it did not make: both runtimes leave a reference of an outer frame alone, so the block stays live in the
// outer frame, the outer call's synchronization goes on, and the outer return ends it. (Fails on 437d5edc: the nested
// delete took the outer frame's entry.)
void check_a_nested_native_method_cannot_delete_its_callers_reference() {
    Fixture f = make();
    void* outer_function = app_function(1);
    void* inner_function = app_function(2);
    const Native outer = thunk_of(outer_function);
    const Native inner = thunk_of(inner_function);
    require(enter_native(outer, outer_function, 0x100, 0x9000), "outer");
    Made a = make_shadow_of(f, sample(32));
    raw_of(a)[0] = 0x55;
    g_in_call = [&] {
        g_in_call = nullptr;
        require(enter_native(inner, inner_function, 0x200, 0x8000), "inner, called from Java");
        end_ref(f, a.shadow);  // the nested method frees the reference it was handed ("free the previous buffer")
        require(is_shadow_block(a.shadow) && counters().shadows_live == 1, "the outer frame's entry is left alone");
        Made own = make_shadow_of(f, sample(8));  // its own entry is the frame's: this one it can delete
        end_ref(f, own.shadow);
        require(!is_shadow_block(own.shadow) && counters().shadows_live == 1, "and an entry of its own ends");
        leave_native(0x8000);
    };
    world.java_fill = 0x44;
    require(call_static_int_v(f, f.consume, a.shadow) == 7, "the outer call completed");
    world.java_fill = -1;
    require(is_shadow_block(a.shadow) && raw_of(a)[0] == 0x44, "the block is live in the outer frame, with Java's writes");
    leave_native(0x9000);
    require(!is_shadow_block(a.shadow) && counters().shadows_live == 0, "and the outer return ends it");
}

// ---- MonitorExit with an exception pending

// MonitorExit of a block's reference with an exception pending (a scoped monitor's unlock after a Java call threw): the
// runtime is handed the array, the monitor is released, and the same exception is still pending afterwards. For the three
// kinds of block: one that ended (only a weak reference behind it), one live on another thread, one of this thread's.
// (Fails on 24093fc5: the runtime was handed NULL, and the monitor stayed held.)
void check_monitor_exit_with_an_exception_pending() {
    for (int kind = 0; kind < 3; ++kind) {
        Fixture f = make();
        Made made = make_held(f, sample(16));
        std::unique_ptr<Other> other;
        std::thread holder;
        Fixture* user = &f;
        if (kind == 0) {
            end_ref(f, made.shadow);
        } else if (kind == 1) {
            other.reset(new Other(f));
            user = &other->f;
        }
        auto enter = fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorEnter);
        auto leave = fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorExit);
        require(enter(user->env, made.shadow) == 0 && world.monitors == 1, "the monitor is taken");
        const size_t locals = world.locals.size();
        Obj exception;
        world.pending_object = &exception;
        world.exception_pending = true;  // a Java call threw
        require(leave(user->env, made.shadow) == 0 && world.monitors == 0, "the monitor is released with an exception pending");
        require(world.exception_pending && world.pending_object == &exception, "and the same exception is still pending");
        world.exception_pending = false;
        world.pending_object = nullptr;
        // MonitorExit that raises an exception of its own keeps that one.
        require(enter(user->env, made.shadow) == 0, "the monitor is taken again");
        Obj first, second;
        world.pending_object = &first;
        world.exception_pending = true;
        world.monitor_exit_throws = &second;
        leave(user->env, made.shadow);
        require(world.exception_pending && world.pending_object == &second, "the exception MonitorExit raised stays");
        world.exception_pending = false;
        world.monitor_exit_throws = nullptr;
        require(world.locals.size() == locals, "no local reference is left behind (the saved exception's is deleted)");
        // A block whose array is gone has no monitor: refused, not handed to the runtime as NULL.
        if (kind == 0) {
            made.object->held = false;
            collect();
            require(enter(user->env, made.shadow) != 0 && leave(user->env, made.shadow) != 0, "a collected array is refused");
        }
    }
}

// ---- stores that land where a test wants another thread's write to land

// A store that lands exactly where a test wants another thread's write to land. The page is made read-only; the first
// store into it faults; the handler writes the other thread's byte, opens the page and lets the store go on. That is the
// interleaving a few instructions wide between the layer's comparison and its store, which no call into the fake runtime
// falls into: a word store there would carry the old value of the neighbouring byte over the other thread's.
struct Watch {
    uint8_t* page = nullptr;
    size_t size = 0;
    volatile uint8_t* racing = nullptr;  // where the other thread's write goes
    uint8_t value = 0;
    uint8_t* const* more = nullptr;      // and more of them, all written at the same fault
    size_t more_count = 0;
    volatile int hits = 0;
};
Watch g_watch;
struct sigaction g_watch_old_bus, g_watch_old_segv;
void watch_handler(int signal_number, siginfo_t* info, void*) {
    uint8_t* at = static_cast<uint8_t*>(info->si_addr);
    if (g_watch.page != nullptr && at >= g_watch.page && at < g_watch.page + g_watch.size) {
        mprotect(g_watch.page, g_watch.size, PROT_READ | PROT_WRITE);
        g_watch.hits = g_watch.hits + 1;
        *g_watch.racing = g_watch.value;
        for (size_t i = 0; i < g_watch.more_count; ++i) *g_watch.more[i] = g_watch.value;
        return;
    }
    struct sigaction default_action;
    std::memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigaction(signal_number, &default_action, nullptr);  // a fault of anything else: the default, on its return
}
// The first whole page inside [memory, memory + length).
uint8_t* page_inside(uint8_t* memory, size_t length) {
    const size_t page = size_t(getpagesize());
    uint8_t* first = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(memory) + page - 1) & ~(page - 1));
    require(first + page <= memory + length, "a whole page lies inside the memory");
    return first;
}
void arm_watch(uint8_t* page, uint8_t* racing, uint8_t value, uint8_t* const* more = nullptr, size_t more_count = 0) {
    g_watch.page = page;
    g_watch.size = size_t(getpagesize());
    g_watch.racing = racing;
    g_watch.value = value;
    g_watch.more = more;
    g_watch.more_count = more_count;
    g_watch.hits = 0;
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = watch_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGBUS, &action, &g_watch_old_bus);
    sigaction(SIGSEGV, &action, &g_watch_old_segv);
    require(mprotect(page, g_watch.size, PROT_READ) == 0, "the page is made read-only");
}
int disarm_watch() {
    const int hits = g_watch.hits;
    mprotect(g_watch.page, g_watch.size, PROT_READ | PROT_WRITE);
    sigaction(SIGBUS, &g_watch_old_bus, nullptr);
    sigaction(SIGSEGV, &g_watch_old_segv, nullptr);
    g_watch.page = nullptr;
    return hits;
}

// ---- a synchronization writes only the bytes Java changed into the block

// A raw write to a byte of a clean chunk, made while the synchronization is pulling Java's bytes in (the hook runs in the
// runtime's region read, after it copied), is not overwritten by the pull: Java wrote a neighbouring byte, the application
// wrote this one, and both are in the block afterwards; the next synchronization pushes the application's byte. (Fails on
// ecb411ce: the pull copied whole chunks into the block.)
void check_a_pull_does_not_overwrite_a_raw_write_to_another_byte() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(64, 0x3a));
    a.object->bytes[10] = 0xA1;  // Java wrote it
    g_after_get = [&] { raw_of(a)[20] = 0x99; };  // the application wrote another byte of the chunk meanwhile
    jbyte result[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 64, result);
    require(raw_of(a)[10] == 0xA1 && raw_of(a)[20] == 0x99, "the block has Java's byte and the application's");
    require(a.object->bytes[20] == 0x3a, "the array does not have the raw write yet");
    jbyte again[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 64, again);
    require(a.object->bytes[20] == 0x99 && a.object->bytes[10] == 0xA1 && static_cast<uint8_t>(again[20]) == 0x99,
            "and the next synchronization pushes it");
}

// The same for a chunk the application had written: the merge stores into the block only the byte Java changed, so a raw
// write to a third byte that races the merge survives it. The write is placed at the layer's store of Java's byte into the
// block (the watch below, defined further down). (Fails on ecb411ce: the merged run was copied over the block.)
void check_a_merge_does_not_overwrite_a_raw_write_to_a_third_byte() {
    Fixture f = make();
    const size_t size = 65536;
    Made a = make_held(f, std::vector<uint8_t>(size, 0x3a));
    uint8_t* data = raw_of(a);
    uint8_t* page = page_inside(data, size);
    const size_t at = size_t(page - data);
    a.object->bytes[at + 10] = 0xA1;  // Java wrote one byte
    data[at + 0] = 0x31;              // the application wrote another: the chunk is dirty
    arm_watch(page, page + 40, 0x99);  // and a third, at the merge's store of Java's byte into the block
    jbyte result[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, jsize(at), 64, result);
    const int hits = disarm_watch();
    require(hits == 1, "the store of Java's byte was where the raw write landed");
    require(a.object->bytes[at + 0] == 0x31 && a.object->bytes[at + 10] == 0xA1, "Java has both of the earlier writes");
    require(data[at + 0] == 0x31 && data[at + 10] == 0xA1 && data[at + 40] == 0x99, "the block has all three");
    require(a.object->bytes[at + 40] == 0x3a, "the raw write is not in the array yet");
    jbyte again[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, jsize(at), 64, again);
    require(a.object->bytes[at + 40] == 0x99, "and the next synchronization pushes it");
}
// Where both sides wrote the same byte at the pull, the application's byte stays and goes to the array next time.
void check_a_byte_both_sides_wrote_keeps_the_applications_at_a_racing_pull() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(64, 0x3a));
    a.object->bytes[10] = 0xA1;
    g_after_get = [&] { raw_of(a)[10] = 0x55; };
    jbyte result[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 64, result);
    require(raw_of(a)[10] == 0x55, "the application's byte stays in the block");
    jbyte again[64] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 64, again);
    require(a.object->bytes[10] == 0x55 && static_cast<uint8_t>(again[10]) == 0x55, "and reaches the array at the next synchronization");
}

// Random writes from both sides, over sizes around the scan block, the chunk and the pull piece: after one read of the
// whole block both copies agree with the per-byte rule (a byte the application wrote is the application's, else a byte Java
// wrote is Java's, else it is as it was), and a second read changes nothing.
void check_the_synchronization_is_byte_exact_for_random_writes() {
    uint32_t seed = 12345;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const unsigned sizes[] = {1, 3, 4, 5, 255, 256, 257, 511, 512, 513, 4095, 4096, 4097, 8191, 16383, 16384, 16385, 20000, 40000};
    for (unsigned trial = 0; trial < 190; ++trial) {
        const unsigned n = sizes[trial % (sizeof(sizes) / sizeof(sizes[0]))];
        Fixture f = make();
        std::vector<uint8_t> original(n);
        for (unsigned i = 0; i < n; ++i) original[i] = uint8_t(next());
        Made a = make_held(f, original);
        std::vector<uint8_t> expected = original;
        const unsigned density = 1 + next() % 200;  // one in `density` bytes
        for (unsigned i = 0; i < n; ++i) {
            const bool java = next() % density == 0, app = next() % density == 0;
            if (java) {
                a.object->bytes[i] = uint8_t(original[i] ^ 0xA5);
                expected[i] = a.object->bytes[i];
            }
            if (app) {
                raw_of(a)[i] = uint8_t(original[i] ^ 0x5A);
                expected[i] = raw_of(a)[i];
            }
        }
        std::vector<jbyte> result(n);
        fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, jsize(n), result.data());
        require(std::memcmp(result.data(), expected.data(), n) == 0, "the read has the rule's bytes");
        require(a.object->bytes == expected && std::memcmp(raw_of(a), expected.data(), n) == 0, "so have the array and the block");
        fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, jsize(n), result.data());
        require(a.object->bytes == expected && std::memcmp(raw_of(a), expected.data(), n) == 0, "and a second read changes nothing");
        reset_world();
    }
}

// A full sweep follows every collection that no full sweep has seen, also one that the step sweep (a few blocks at a
// creation) found first: a result that fits after one full pass is not declined.
void check_a_collection_the_step_sweep_saw_is_swept_in_full() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 200; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(20000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() > 40 && counters().shadows_declined == 1, "the region is full of ended blocks");
    for (Made& made : kept) made.object->held = false;
    collect();
    Made big = make_shadow_of(f, std::vector<uint8_t>(1200 * 1024, 7));
    require(is_shadow_block(big.shadow), "the result fits: the collection was swept in full, not only 32 blocks of it");
}

// The critical pair on a kept reference: with no critical region open the block is held and pinned like any other (an
// ended block that only a weak reference held gets a global one for the pin); inside a region, where no call that makes a
// reference may be made, a block with a strong reference of its own works and one without gives nothing.
void check_a_kept_reference_gives_a_critical_pointer() {
    Fixture f = make();
    const std::vector<uint8_t> payload = sample(100);
    Made kept = make_held(f, payload);
    end_ref(f, kept.shadow);
    Made live = make_held(f, sample(60));
    jobject real = byte_array(sample(8));
    void* region = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical)(f.env, real, nullptr);
    require(region != nullptr, "a critical region of the runtime is open");
    require(get_critical(f, kept.shadow) == nullptr, "inside it a block that only a weak reference holds gives nothing");
    void* inside = get_critical(f, live.shadow);
    require(inside == raw_of(live), "and a live block works");
    release_critical(f, live.shadow, inside, 0);
    fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical)(f.env, real, region, 0);
    void* p = get_critical(f, kept.shadow);
    require(p == raw_of(kept) && std::memcmp(p, payload.data(), 100) == 0, "outside any region the same reference works");
    require(is_ended_block(kept.shadow) && counters().blocks_revived == 0 && counters().shadows_live == 1, "the block stayed ended");
    require(world.globals.size() == 2, "and it holds a strong reference while the pointer is out");
    release_critical(f, kept.shadow, p, 0);
    collect();
    end_ref(f, make_shadow_of(f, sample(10)).shadow);  // a sweep: the last pin is gone
    require(block_weaks() == 2, "after the release the block waits for a collection again");
}

// A release of a pointer that lies inside a block but not at its start is nothing to copy from: the old runtime ignored
// the pointer. Through the block's reference it counts as the release of the pin; through a real reference it is dropped.
void check_a_release_of_a_pointer_inside_a_block_is_harmless() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(4096));
    jbyte* p = get_elements(f, made.shadow);
    const std::vector<uint8_t> before(raw_of(made), raw_of(made) + 4096);
    world.release_calls = 0;
    release_elements(f, made.shadow, p + 100, 0);
    require(std::memcmp(raw_of(made), before.data(), 4096) == 0 && made.object->bytes == before, "nothing was copied over the block");
    end_ref(f, made.shadow);
    require(block_weaks() == 1 && world.globals.empty(), "the pin was counted off: the block ended with a weak reference");
    jbyte* q = get_elements(f, made.shadow);
    jobject real = byte_array(before);
    release_elements(f, real, q + 32, 0);  // aligned, inside the block, through a real reference
    release_elements(f, real, q + 33, 0);
    require(world.release_calls == 0, "dropped: the runtime is not handed a pointer it never gave");
    require(std::memcmp(raw_of(made), before.data(), 4096) == 0, "and the block is as it was");
}

// A block that only a weak reference holds (ended, no pin) has no array to reach while no reference can be made, as with an
// exception pending: its reference converts to nothing, as for a null array (README: no elements, no copy). A region read then
// copies nothing: the block's bytes are not the array's once Java has gone on with it, and nothing holds the block, so a sweep
// may free it while it is read. The row has Java change the array after the block ended and, in its second case, lets the
// array be collected (the layer cannot tell with an exception pending). Fails on a layer that copies the block's bytes.
void check_a_block_nothing_holds_gives_no_bytes_while_no_reference_can_be_made() {
    for (int collected = 0; collected < 2; ++collected) {
        Fixture f = make();
        const std::vector<uint8_t> payload = sample(100);
        Made kept = make_held(f, payload);
        end_ref(f, kept.shadow);
        require(is_ended_block(kept.shadow) && block_weaks() == 1 && world.globals.empty(),
                "an ended block that only a weak reference holds");
        for (uint8_t& byte : kept.object->bytes) byte = uint8_t(~byte);  // Java went on with the array
        if (collected != 0) {
            kept.object->held = false;
            collect();
        }
        world.exception_pending = true;
        world.get_calls = 0;
        world.set_calls = 0;
        jbyte buffer[100];
        std::memset(buffer, 0x77, sizeof(buffer));
        fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, kept.shadow, 0, 100, buffer);
        bool untouched = true;
        for (jbyte byte : buffer) untouched = untouched && byte == jbyte(0x77);
        require(untouched, "a region read of a block nothing holds copies nothing");
        require(get_elements(f, kept.shadow) == nullptr, "and gives no elements");
        const jbyte patch[3] = {1, 2, 3};
        fn<SetRegion>(*f.table, slot::SetByteArrayRegion)(f.env, kept.shadow, 0, 3, patch);
        require(world.get_calls == 0 && world.set_calls == 0, "and a region write reaches nothing: the layer called the runtime for none of them");
        world.exception_pending = false;
        require(kept.object->bytes[0] == uint8_t(~payload[0]) && std::memcmp(raw_of(kept), payload.data(), 100) == 0,
                "neither the array nor the block changed");
        require(is_ended_block(kept.shadow), "the block is as it was");
    }
}

// ---- the block record: an address is a block only where the layer's record says a block starts exactly there

constexpr uint32_t kLiveMagic = 0xb10c11feu, kEndedMagic = 0xe4ded10cu;  // the layer's marker words of a header (never read by the layer)
constexpr size_t kHeadBytes = sizeof(void*) == 8 ? 96 : 64;              // a block's header, in front of its Dalvik object

// Writes what a header holds (a block magic, then a length: at +0 and at +8 of the header, so that either layout is forged)
// into `contents`, at the place where the header of the block whose reference is `forged` lies, when `contents` are the bytes
// of the block whose reference is `container`.
void forge_header(std::vector<uint8_t>& contents, const void* container, const void* forged, uint32_t magic, uint32_t length) {
    const ptrdiff_t at = (static_cast<const uint8_t*>(forged) - kHeadBytes) - (static_cast<const uint8_t*>(container) + 16);
    require(at >= 0 && size_t(at) + 16 <= contents.size(), "the forged header lies in the contents of the block");
    for (const size_t layout : {size_t(0), size_t(8)}) {
        std::memcpy(contents.data() + at + layout, &magic, sizeof(magic));
        std::memcpy(contents.data() + at + layout + 4, &length, sizeof(length));
    }
}

// Every use of the reference `stale`, which is no block, finds none: it is no block of either kind, its length is 0, the runtime
// is handed null for it, nothing is read from it or written to it, and no pointer of it is released. `other` is a block that is
// live, and stays so.
void require_no_block_at(Fixture& f, jobject stale, const Made& other, const char* what) {
    const std::string at = std::string(what) + ": ";
    require(!is_shadow_block(stale) && !is_ended_block(stale), (at + "it is no block of either kind").c_str());
    require(length_of(f, stale) == 0, (at + "its length is 0").c_str());
    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::GetObjectRefType)(f.env, stale) != kRefTypeLocal,
            (at + "it is no local reference of the layer's").c_str());
    require(call_static_int_v(f, f.consume, stale) == 7 && world.last_array_args.size() == 1 && world.last_array_args[0] == nullptr,
            (at + "a call is handed null").c_str());
    world.get_calls = 0;
    world.set_calls = 0;
    world.release_calls = 0;
    jbyte buffer[8];
    std::memset(buffer, 0x77, sizeof(buffer));
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, stale, 0, 8, buffer);
    require(buffer[0] == 0x77 && buffer[7] == 0x77, (at + "a region read copies nothing").c_str());
    const jbyte patch[4] = {1, 2, 3, 4};
    fn<SetRegion>(*f.table, slot::SetByteArrayRegion)(f.env, stale, 0, 4, patch);
    require(get_elements(f, stale) == nullptr && get_critical(f, stale) == nullptr, (at + "it gives no elements").c_str());
    release_elements(f, stale, static_cast<const uint8_t*>(stale) + 16, 0);
    release_elements(f, stale, static_cast<const uint8_t*>(stale) + 16, kReleaseAbort);
    require(world.get_calls == 0 && world.set_calls == 0 && world.release_calls == 0,
            (at + "no region, no write and no release reached the runtime").c_str());
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, stale, nullptr) == 1,
            (at + "and it is the null reference").c_str());
    const uint32_t live = counters().shadows_live, ended = counters().shadows_ended;
    end_ref(f, stale);  // DeleteLocalRef of it ends nothing
    require(counters().shadows_live == live && counters().shadows_ended == ended && is_shadow_block(other.shadow),
            (at + "DeleteLocalRef of it ends nothing").c_str());
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    push(f.env, 16);
    require(pop(f.env, stale) == nullptr && counters().shadows_live == live && counters().shadows_ended == ended &&
                is_shadow_block(other.shadow),
            (at + "PopLocalFrame returns null for it and keeps nothing").c_str());
}

// A reference whose block was reclaimed, and whose memory another block's contents now cover, is no block: the layer's own
// record of where a block starts says so, not the bytes found in the arena. Two 100-byte blocks end, their arrays are collected and
// the next block made reclaims them; the block
// of a 600-byte array takes both chunks, and its contents hold what a header holds (a block magic and a length) at the place of
// the second block's header. The old reference must read as no block, whichever magic the contents hold, and every use of it must
// find none. (Fails on a layer that decides by the bytes: the old reference reads as a block of length 1234.)
void check_a_collected_reference_covered_by_another_blocks_contents_is_no_block() {
    for (const uint32_t magic : {kLiveMagic, kEndedMagic}) {
        Fixture f = make();
        Made left = make_shadow_of(f, std::vector<uint8_t>(100, 0));
        Made old = make_shadow_of(f, std::vector<uint8_t>(100, 0));
        std::vector<uint8_t> payload(600, 0x5a);
        forge_header(payload, left.shadow, old.shadow, magic, 1234);
        end_ref(f, left.shadow);
        end_ref(f, old.shadow);
        collect();
        Made replacement = make_shadow_of(f, payload);
        require(replacement.shadow == left.shadow, "the larger block takes the memory of the two reclaimed ones");
        require(is_shadow_block(replacement.shadow) && length_of(f, replacement.shadow) == 600, "the block that is there is whole");
        require_no_block_at(f, old.shadow, replacement, "the reclaimed block's reference, over forged contents");
        require(replacement.object->bytes == payload && std::memcmp(raw_of(replacement), payload.data(), 600) == 0,
                "the block that covers it is unchanged by all of them");
        require(counters().shadows_live == 1 && length_of(f, replacement.shadow) == 600, "and still live and whole");
    }
}

// The same inside a live block: a reference into the contents of a live block, 16-byte aligned and so a possible block start,
// at a place where the contents hold a header, is no block either. The pin of the block that holds it is not given back by a
// release of a pointer there.
void check_a_forged_header_inside_a_live_blocks_contents_is_no_block() {
    for (const uint32_t magic : {kLiveMagic, kEndedMagic}) {
        Fixture f = make();
        std::vector<uint8_t> payload(2000, 0x5a);
        Made host = make_held(f, std::vector<uint8_t>(payload));
        const uint8_t* forged = static_cast<const uint8_t*>(host.shadow) + 16 + 160 + kHeadBytes;  // a place inside the contents
        forge_header(payload, host.shadow, forged, magic, 777);
        fn<SetRegion>(*f.table, slot::SetByteArrayRegion)(f.env, host.shadow, 0, 2000, reinterpret_cast<const jbyte*>(payload.data()));
        require(std::memcmp(raw_of(host), payload.data(), 2000) == 0 && is_shadow_block(host.shadow), "the host block holds the forged header");
        require(forged + 16 + 16 <= raw_of(host) + 2000, "and the forged reference's own data lies in the contents as well");
        jbyte* pointer = get_elements(f, host.shadow);  // the host is pinned
        require(pointer == reinterpret_cast<jbyte*>(raw_of(host)), "the host's pin");
        require_no_block_at(f, const_cast<uint8_t*>(forged), host, "the interior reference, over a forged header");
        // A release of a pointer there, through a reference that is not the host's, reaches nothing and unpins nothing.
        jobject real = byte_array(sample(4));
        world.release_calls = 0;
        release_elements(f, real, forged + 16, 0);
        require(world.release_calls == 0 && is_shadow_block(host.shadow), "a release of the interior pointer reaches nothing");
        end_ref(f, host.shadow);
        require(is_ended_block(host.shadow) && world.globals.size() == 1, "the host's pin is still there: the ended block keeps its strong reference");
        release_elements(f, host.shadow, pointer, 0);
        require(world.globals.empty() && is_ended_block(host.shadow), "and its own release gives it back");
    }
}

// Contents that look like headers everywhere: each aligned 16 bytes hold a block magic and a length (a length no block of these rows
// has), at +0 and +8 of the unit, so that a layer that read a header from the bytes of the region would find a block at every
// 16-byte place of them.
constexpr uint32_t kForgedLength = 0x12345;
std::vector<uint8_t> forged_contents(size_t n, uint32_t magic) {
    std::vector<uint8_t> bytes(n, 0);
    for (size_t at = 0; at + 16 <= n; at += 16) {
        for (const size_t layout : {size_t(0), size_t(8)}) {
            std::memcpy(bytes.data() + at + layout, &magic, sizeof(magic));
            std::memcpy(bytes.data() + at + layout + 4, &kForgedLength, sizeof(kForgedLength));
        }
    }
    return bytes;
}

// Every 16-byte place in [from, to) is checked against `starts` (the blocks that exist, live, by their reference and length): a block
// where one starts, with its length, and none anywhere else, whatever the bytes there say.
void require_record_matches(Fixture& f, const uint8_t* from, const uint8_t* to, const std::map<const void*, size_t>& starts,
                            const char* what) {
    for (const uint8_t* at = from; at < to; at += 16) {
        const auto found = starts.find(at);
        const bool live = is_shadow_block(at);
        const jsize length = length_of(f, const_cast<uint8_t*>(at));
        if (found != starts.end()) {
            require(live && !is_ended_block(at) && length == jsize(found->second), what);
        } else {
            require(!live && !is_ended_block(at) && length == 0, what);
        }
    }
}

// Blocks that are reclaimed, and whose chunks the arena merges and cuts again at other boundaries, leave their addresses to
// whatever is made there: chunk headers, links, the headers and the contents of other blocks. The contents here are made of
// forged headers. After each new block, every 16-byte place of the span where the old blocks lay is a block exactly where a new
// block starts, with that block's own length (the first of them starts at an old block's address, with another length when the
// size differs), and none anywhere else. The first new block is up to six 16-byte steps shorter or longer than an old one, so
// that the later boundaries fall at every 16-byte step against the old ones, and the row asserts, as a count it requires, the
// case the record's slot compare is for: a new block that starts in the same 64-byte window as an old block's start, at another
// 16-byte slot of it (the old reference is no block, the new one is). (Fails on a layer that decides by the bytes, and on one that
// compares the window and not the slot.)
void check_after_a_merge_and_a_new_cut_a_place_is_a_block_only_where_one_starts() {
    size_t same_window_other_slot = 0;
    for (const uint32_t magic : {kLiveMagic, kEndedMagic}) {
        for (int delta = -6; delta <= 6; ++delta) {  // the first new block is 16 * delta bytes longer than an old one
            Fixture f = make();
            Made old[4] = {};
            for (Made& block : old) block = make_shadow_of(f, forged_contents(100, magic));
            const uint8_t* const from = static_cast<const uint8_t*>(old[0].shadow) - 16;  // the first place of the region a block can start at, or later
            require(from >= g_region + kHeadBytes, "the span starts inside the region the layer owns");
            const uint8_t* const to = static_cast<const uint8_t*>(old[3].shadow) + 16 + 100 + 64;
            for (Made& block : old) end_ref(f, block.shadow);
            collect();
            std::map<const void*, size_t> starts;
            std::vector<Made> fresh;
            const uint8_t* const region_start = g_region + kHeadBytes;
            const auto window_of = [&](const void* reference) { return size_t(static_cast<const uint8_t*>(reference) - region_start) >> 6; };
            const auto slot_of = [&](const void* reference) { return (size_t(static_cast<const uint8_t*>(reference) - region_start) >> 4) & 3u; };
            for (const size_t size : {size_t(int(100) + 16 * delta), size_t(100), size_t(300), size_t(37), size_t(250)}) {
                fresh.push_back(make_shadow_of(f, forged_contents(size, magic)));
                const Made& block = fresh.back();
                starts[block.shadow] = size;
                require(fresh.size() != 1 || block.shadow == old[0].shadow, "the first new block starts where the first old one did");
                require(is_shadow_block(block.shadow), "the new block is a block");
                require_record_matches(f, from, to, starts, "a place is a block only where a new block starts");
                for (const Made& earlier : old) {
                    if (window_of(earlier.shadow) == window_of(block.shadow) && slot_of(earlier.shadow) != slot_of(block.shadow)) {
                        ++same_window_other_slot;
                        require(length_of(f, block.shadow) == jsize(size) && !is_shadow_block(earlier.shadow) &&
                                    !is_ended_block(earlier.shadow) && length_of(f, earlier.shadow) == 0,
                                "an old start in the window of a new block, at another slot of it, is no block, and the new block is");
                    }
                }
            }
            require(length_of(f, old[0].shadow) == jsize(int(100) + 16 * delta), "the old reference that a new block starts at is that block, with its own length");
            for (const Made& block : fresh) {
                require(std::memcmp(raw_of(block), forged_contents(starts[block.shadow], magic).data(), starts[block.shadow]) == 0,
                        "and no new block was changed by what was asked of the old addresses");
            }
        }
    }
    require(same_window_other_slot > 0, "the row met a new block in an old start's window at another slot");
}

// PopLocalFrame keeps a block the thread holds, and nothing else. The reference of a block that ended, with the pop under way on its
// thread, is collected, and a block is made at its address by another thread (which reclaims it): the pop keeps nothing and takes no reference of
// the new block, which that thread's single end still ends. The same for a live block of another thread, the stale reference
// being that thread's own. The control: the thread's own block comes out of its frame and is its entry again.
void check_a_block_made_at_a_stale_address_during_PopLocalFrame_is_left_alone() {
    constexpr const char* kCase = "check_a_block_made_at_a_stale_address_during_PopLocalFrame_is_left_alone";
    {
        Fixture f = make();
        auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
        auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
        push(f.env, 16);
        Made mine = make_shadow_of(f, sample(100));
        jobject out = pop(f.env, mine.shadow);
        require(out == mine.shadow && is_shadow_block(out) && counters().shadows_live == 1, "the control: the thread's own block comes out of the frame");
        end_ref(f, out);
        require(is_ended_block(out), "as one entry, which one DeleteLocalRef ends");
    }
    for (int variant = 0; variant < 2; ++variant) {
        Fixture f = make();
        Other other(f);
        auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
        auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
        Made stale{};
        if (variant == 0) {  // the reference of a block that ended; its array will be collected under the pop
            stale = make_shadow_of(f, sample(100));
            end_ref(f, stale.shadow);
            require(is_ended_block(stale.shadow), "a block that ended and waits for its collection");
        }
        std::atomic<int> stage{0};
        Made theirs{};
        std::thread second([&] {
            if (variant == 1) {  // the second thread's live block is what the first pops
                theirs = make_shadow_of(other.f, sample(100));
                stage = 1;
            } else {
                wait_for_counter(kCase, "stage", stage, 1, "the second thread (the pop is under way)");
                collect();
                theirs = make_shadow_of(other.f, sample(100));  // the reclaimed block's address
                stage = 2;
            }
            wait_for_counter(kCase, "stage", stage, 3, "the second thread (the pop has returned)");
            end_ref(other.f, theirs.shadow);
            stage = 4;
        });
        if (variant == 1) {
            wait_for_counter(kCase, "stage", stage, 1, "the first thread (the second thread makes its block)");
            stale = theirs;
        } else {
            g_in_pop = [&] {
                stage = 1;
                wait_for_counter(kCase, "stage", stage, 2, "the pop (the second thread makes its block)");
            };
        }
        push(f.env, 16);
        jobject out = pop(f.env, stale.shadow);
        stage = 3;
        wait_for_counter(kCase, "stage", stage, 4, "the first thread (the second thread ends its block)");
        second.join();
        require(theirs.shadow == stale.shadow, "the second thread's block is at the address popped");
        require(out == stale.shadow, "the reference comes back as it was");
        require(is_ended_block(theirs.shadow) && counters().shadows_live == 0,
                "the second thread's one end ended its block: the pop took no reference of it, and moved nothing");
        require(counters().blocks_ended == (variant == 0 ? 2u : 1u), "each block ended once");
    }
}

// A wrapper that took the reference of a block that ended, finds the block gone, or another block, when it takes the mutex:
// the answer under the mutex decides. Another thread collects the block and makes (a) a block at the same address, which the
// region read then reads within its own length; or (b) a larger block from an earlier address, whose contents hold a header
// where the old one lay, which the read finds no block in and copies nothing.
void check_a_block_made_at_a_collected_address_before_the_mutex_is_taken_is_acted_on_whole() {
    constexpr const char* kCase = "check_a_block_made_at_a_collected_address_before_the_mutex_is_taken_is_acted_on_whole";
    for (int variant = 0; variant < 2; ++variant) {
        Fixture f = make();
        Other other(f);
        Made left{};
        if (variant == 1) left = make_shadow_of(f, std::vector<uint8_t>(100, 0x11));
        Made stale = make_shadow_of(f, std::vector<uint8_t>(100, 0x11));
        if (variant == 1) end_ref(f, left.shadow);
        end_ref(f, stale.shadow);
        require(is_ended_block(stale.shadow), "a block that ended and waits for its collection");
        std::vector<uint8_t> replacement_bytes = variant == 0 ? std::vector<uint8_t>(64, 0x22) : std::vector<uint8_t>(600, 0x5a);
        if (variant == 1) forge_header(replacement_bytes, left.shadow, stale.shadow, kLiveMagic, 1234);
        std::atomic<int> stage{0};
        Made replacement{};
        std::thread second([&] {
            wait_for_counter(kCase, "stage", stage, 1, "the second thread (the first is about to take the mutex)");
            collect();
            replacement = make_shadow_of(other.f, replacement_bytes);
            stage = 2;
        });
        g_before_lock = [&] {
            stage = 1;
            wait_for_counter(kCase, "stage", stage, 2, "the first thread (the second makes its block)");
        };
        t_pause_at_lock = true;
        jbyte buffer[40];
        std::memset(buffer, 0x77, sizeof(buffer));
        fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, stale.shadow, 0, 40, buffer);
        t_pause_at_lock = false;
        second.join();
        require(variant == 1 ? replacement.shadow == left.shadow : replacement.shadow == stale.shadow,
                "the second thread's block has the memory it was meant to cover");
        bool copied_new = true, untouched = true;
        for (jbyte byte : buffer) {
            copied_new = copied_new && byte == jbyte(0x22);
            untouched = untouched && byte == jbyte(0x77);
        }
        if (variant == 0) {
            require(copied_new && !world.exception_pending, "(a) the read acted on the block that is there, within its own length");
        } else {
            require(untouched && !world.exception_pending, "(b) the read found no block and copied nothing");
        }
        require(is_shadow_block(replacement.shadow) && length_of(f, replacement.shadow) == jsize(replacement_bytes.size()) &&
                    std::memcmp(raw_of(replacement), replacement_bytes.data(), replacement_bytes.size()) == 0,
                "the block that is there is whole and unchanged");
    }
}

// The record follows every transition. A run of 12000 steps (a fixed seed) of makes of 0 to 8192 bytes (their contents forged
// headers), ends, collections and revivals; after each, the answers are the model's: at each block, its state and length; at
// every 16-byte place inside every block the step touched and at samples of the span, no block; at each address of a block that
// was freed, none (or the block made there since). The marker word of each header agrees with the state the model has.
void check_the_record_follows_makes_ends_collections_and_revivals() {
    constexpr int kSteps = 12000;
    struct Block {
        Made made;
        size_t length;
        bool held;
        bool ended;
    };
    Fixture f = make();
    std::mt19937 rng(0x5eed0001u);
    std::vector<Block> blocks;                  // the blocks that exist as far as the model knows
    std::vector<const uint8_t*> stale;          // addresses of blocks that were freed (the newest last)
    std::set<const uint8_t*> ever;              // every address a block started at
    const auto pick = [&](size_t n) { return size_t(rng() % n); };
    const auto ref_of = [](const Block& block) { return static_cast<const uint8_t*>(block.made.shadow); };
    const auto marker = [&](const Block& block) {
        uint32_t magic;
        std::memcpy(&magic, ref_of(block) - kHeadBytes + 8, sizeof(magic));
        return magic;
    };
    const auto live_count = [&] { size_t n = 0; for (const Block& b : blocks) n += b.ended ? 0 : 1; return n; };
    const auto held_count = [&] { size_t n = 0; for (const Block& b : blocks) n += b.held ? 1 : 0; return n; };
    const uint8_t* low = nullptr;
    const uint8_t* high = nullptr;
    for (int step = 0; step < kSteps; ++step) {
        const uint8_t* touched_ref = nullptr;  // the block the step made, ended or revived, by value (the vector moves)
        size_t touched_length = 0;
        const unsigned action = unsigned(rng() % 100);
        if (action < 45 || blocks.empty()) {  // make
            const size_t size = rng() % 4 == 0 ? pick(8193) : pick(300);
            const bool held = held_count() < 40 && rng() % 3 == 0;
            const uint32_t magic = rng() % 2 != 0 ? kLiveMagic : kEndedMagic;
            Made made = make_shadow_of(f, forged_contents(size, magic));
            require(is_shadow_block(made.shadow), "the new block is live");
            made.object->held = held;
            // Whatever the layer freed on the way is gone: a droppable ended block that is no longer ended.
            const uint8_t* const at = static_cast<const uint8_t*>(made.shadow);
            for (size_t i = 0; i < blocks.size();) {
                if (blocks[i].ended && !blocks[i].held && (ref_of(blocks[i]) == at || !is_ended_block(ref_of(blocks[i])))) {
                    stale.push_back(ref_of(blocks[i]));
                    blocks.erase(blocks.begin() + std::ptrdiff_t(i));
                } else {
                    ++i;
                }
            }
            stale.erase(std::remove(stale.begin(), stale.end(), at), stale.end());
            blocks.push_back(Block{made, size, held, false});
            ever.insert(at);
            touched_ref = at;
            touched_length = size;
            if (low == nullptr || at < low) low = at;
            if (high == nullptr || at > high) high = at;
        } else if (action < 75 || live_count() > 40) {  // end a live block
            const size_t start = pick(blocks.size());
            for (size_t k = 0; k < blocks.size(); ++k) {
                Block& block = blocks[(start + k) % blocks.size()];
                if (!block.ended) {
                    end_ref(f, block.made.shadow);
                    block.ended = true;
                    touched_ref = ref_of(block);
                    touched_length = block.length;
                    break;
                }
            }
        } else if (action < 88) {  // a collection
            collect();
        } else {  // revive an ended block whose array is held
            const size_t start = pick(blocks.size());
            for (size_t k = 0; k < blocks.size(); ++k) {
                Block& block = blocks[(start + k) % blocks.size()];
                if (block.ended && block.held) {
                    Made again = obtain_again(f, block.made.object);
                    require(again.shadow == block.made.shadow, "an ended block is the block again");
                    block.ended = false;
                    touched_ref = ref_of(block);
                    touched_length = block.length;
                    break;
                }
            }
        }
        // The model's blocks: state, length and marker; a droppable ended block may have been freed by a sweep.
        size_t live = 0, ended = 0;
        for (size_t i = 0; i < blocks.size();) {
            Block& block = blocks[i];
            const uint8_t* ref = ref_of(block);
            if (block.ended && !block.held && !is_ended_block(ref)) {
                require(!is_shadow_block(ref) && length_of(f, block.made.shadow) == 0, "a freed block is no block, with no length");
                stale.push_back(ref);
                blocks.erase(blocks.begin() + std::ptrdiff_t(i));
                continue;
            }
            if (block.ended) {
                require(is_ended_block(ref) && !is_shadow_block(ref) && length_of(f, block.made.shadow) == jsize(block.length),
                        "an ended block: ended, with its length");
                require(marker(block) == kEndedMagic, "and its marker says ended");
                ++ended;
            } else {
                require(is_shadow_block(ref) && !is_ended_block(ref) && length_of(f, block.made.shadow) == jsize(block.length),
                        "a live block: live, with its length");
                require(marker(block) == kLiveMagic, "and its marker says live");
                ++live;
            }
            ++i;
        }
        require(counters().shadows_live == live && counters().shadows_ended == ended, "the counters count the model's blocks");
        // Every place inside a block the step touched, and inside one other, is no block; the freed addresses are none.
        const auto interior_is_none = [&](const uint8_t* ref, size_t length) {
            for (const uint8_t* at = ref - kHeadBytes - 16; at < ref + 16 + length + 32; at += 16) {
                if (at == ref) continue;
                require(!is_shadow_block(at) && !is_ended_block(at), "no place inside a block is a block");
            }
        };
        if (touched_ref != nullptr) interior_is_none(touched_ref, touched_length);
        if (!blocks.empty()) {
            const Block& other = blocks[pick(blocks.size())];
            interior_is_none(ref_of(other), other.length);
        }
        for (size_t i = stale.size() > 32 ? stale.size() - 32 : 0; i < stale.size(); ++i) {
            require(!is_shadow_block(stale[i]) && !is_ended_block(stale[i]) && length_of(f, const_cast<uint8_t*>(stale[i])) == 0,
                    "the address of a freed block is no block");
        }
        if (low != nullptr) {
            const uint8_t* const limit = std::min<const uint8_t*>(high + 1024, g_region + g_region_size - 64);  // inside the region the layer owns
            for (int sample_index = 0; sample_index < 8; ++sample_index) {
                const uint8_t* at = low + 16 * pick(size_t(limit - low) / 16);
                bool is_start = false;
                for (const Block& block : blocks) is_start = is_start || ref_of(block) == at;
                if (!is_start) {
                    require(!is_shadow_block(at) && !is_ended_block(at) && length_of(f, const_cast<uint8_t*>(at)) == 0,
                            "a place that no block starts at is no block");
                }
            }
        }
    }
    require(ever.size() > 50, "the run used many addresses");
}

// Readers that use stale and interior references while blocks are made, ended and collected at the same places read no block, or a
// length that a block published at exactly that address had. Two readers hammer GetArrayLength, GetObjectRefType and the
// state queries on a fixed set of addresses (the starts of 24 reclaimed blocks, places inside the blocks made after, and a place that
// is not 16-byte aligned) while the maker churns blocks of mixed sizes whose contents are forged headers; the maker logs the length of
// each block it makes at an address, and the readers' observations are checked against the log after both have been joined. The
// row cannot pass without the readers having answered: before the churn each reader must have looked at the block that stays live and
// at the places inside it, and every 50 steps the maker makes a block and holds it live until each reader has acknowledged a live
// answer of exactly its length, then ends it and holds it ended until each has acknowledged an ended answer of that length; it goes
// on only then, so each answer is checked against the one state that holds at that moment. (Under ThreadSanitizer this is the proof
// that no reader reads a byte of the region.)
void check_stale_and_interior_readers_during_churn_see_no_block_or_a_published_length() {
    constexpr const char* kCase = "check_stale_and_interior_readers_during_churn_see_no_block_or_a_published_length";
    constexpr int kSteps = 4000;
    constexpr int kReaders = 2;
    Fixture f = make();
    std::mt19937 rng(0x5eed0002u);
    std::mutex log_mutex;
    std::map<const void*, std::set<size_t>> published;  // address -> the lengths of the blocks made there
    const auto make_logged = [&](size_t size) {
        Made made = make_shadow_of(f, forged_contents(size, rng() % 2 != 0 ? kLiveMagic : kEndedMagic));
        std::lock_guard<std::mutex> lock(log_mutex);
        if (is_shadow_block(made.shadow)) published[made.shadow].insert(size);
        return made;
    };
    std::vector<const void*> watched;
    // A block that stays live through the churn, read all the while: the readers must see it whole every time, and no block
    // at the places inside it (a layer that answered "no block" for everything would pass the rest of the row).
    constexpr size_t kAnchorBytes = 120;
    Made anchor = make_logged(kAnchorBytes);
    require(is_shadow_block(anchor.shadow) && length_of(f, anchor.shadow) == jsize(kAnchorBytes), "the anchor is a live block");
    const uint8_t* const anchor_start = static_cast<const uint8_t*>(anchor.shadow);
    const std::set<const void*> anchor_inside = {anchor_start + 16, anchor_start + 48, anchor_start + 64};
    watched.push_back(anchor.shadow);
    watched.insert(watched.end(), anchor_inside.begin(), anchor_inside.end());
    {
        std::vector<Made> first;
        for (int i = 0; i < 24; ++i) {
            first.push_back(make_logged(16 + rng() % 400));
            watched.push_back(first.back().shadow);
        }
        for (const Made& block : first) {
            for (const size_t offset : {size_t(1), size_t(16), size_t(48), size_t(160), size_t(256)}) {
                watched.push_back(static_cast<const uint8_t*>(block.shadow) + offset);  // inside the block; the first is not aligned
            }
        }
        for (const Made& block : first) end_ref(f, block.shadow);
        collect();
    }
    struct Seen {
        const void* address;
        bool live, ended;
        jsize length;
        jint type;
    };
    std::vector<Seen> seen[kReaders];
    std::atomic<size_t> anchor_seen[kReaders] = {};
    std::atomic<size_t> inside_seen[kReaders] = {};
    // The maker's hold: held_phase is 2n - 1 while the n-th block is held live and 2n while it is held ended; the maker changes it
    // only after both readers wrote that phase to their acknowledgment, and a reader acknowledges after it checked its answers.
    std::atomic<uint32_t> held_phase{0};
    std::atomic<const void*> held_address{nullptr};
    std::atomic<jsize> held_length{0};
    std::atomic<uint32_t> acknowledged[kReaders] = {};
    std::atomic<int> started{0};
    std::atomic<bool> stop{false};
    std::vector<std::unique_ptr<Other>> readers;
    for (int i = 0; i < kReaders; ++i) readers.emplace_back(new Other(f));
    std::vector<std::thread> threads;
    for (int i = 0; i < kReaders; ++i) {
        threads.emplace_back([&, i] {
            Other& reader = *readers[size_t(i)];
            ++started;
            size_t rounds = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const uint32_t phase = held_phase.load(std::memory_order_acquire);
                if (phase != 0 && acknowledged[i].load(std::memory_order_relaxed) != phase) {
                    const void* const address = held_address.load(std::memory_order_relaxed);
                    const jsize length = held_length.load(std::memory_order_relaxed);
                    const bool is_live = is_shadow_block(address);
                    const bool is_ended = is_ended_block(address);
                    const jsize got = length_of(reader.f, const_cast<void*>(address));
                    if ((phase & 1u) != 0) {
                        require(is_live && !is_ended && got == length &&
                                    fn<jint (*)(Env*, jobject)>(*reader.f.table, slot::GetObjectRefType)(
                                        reader.f.env, const_cast<void*>(address)) == kRefTypeLocal,
                                "a block held live is a live block of its length");
                    } else {
                        require(!is_live && is_ended && got == length, "a block held ended is an ended block of its length");
                    }
                    acknowledged[i].store(phase, std::memory_order_release);
                }
                for (const void* address : watched) {
                    Seen now{address, is_shadow_block(address), is_ended_block(address), length_of(reader.f, const_cast<void*>(address)),
                             fn<jint (*)(Env*, jobject)>(*reader.f.table, slot::GetObjectRefType)(reader.f.env, const_cast<void*>(address))};
                    if (address == anchor.shadow) {
                        require(now.live && !now.ended && now.length == jsize(kAnchorBytes) && now.type == kRefTypeLocal,
                                "the block that stays live is a live block of its length, whatever the churn does");
                        ++anchor_seen[i];
                        continue;
                    }
                    if (anchor_inside.count(address) != 0) {
                        require(!now.live && !now.ended && now.length == 0 && now.type != kRefTypeLocal,
                                "a place inside the live block is no block");
                        ++inside_seen[i];
                        continue;
                    }
                    if ((now.live || now.ended || now.length != 0 || now.type == kRefTypeLocal) && seen[i].size() < 400000) {
                        seen[i].push_back(now);
                    }
                }
                if ((++rounds & 7u) == 0) std::this_thread::yield();
            }
        });
    }
    wait_for_counter(kCase, "started", started, kReaders, "the maker (the readers start)");
    const auto wait_for_readers = [&](const char* what, const std::function<bool(int)>& done) {
        for (int i = 0; i < kReaders; ++i) {
            if (!waitUntil([&] { return done(i); }, kHandOffLimit, std::chrono::microseconds(100))) {
                uml::testing::fail(std::string(kCase) + ": the maker waited " + std::to_string(kHandOffLimit.count()) + " s for reader " +
                                   std::to_string(i) + " to " + what);
            }
        }
    };
    wait_for_readers("look at the block that stays live and at the places inside it",
                     [&](int i) { return anchor_seen[i].load() > 0 && inside_seen[i].load() > 0; });
    uint32_t held_count = 0;
    for (int step = 0; step < kSteps; ++step) {
        if (step % 50 == 0) {
            Made held = make_logged(1 + rng() % 300);
            require(is_shadow_block(held.shadow), "the block to hold is made");
            held_address.store(held.shadow, std::memory_order_relaxed);
            held_length.store(length_of(f, held.shadow), std::memory_order_relaxed);
            const uint32_t live_phase = 2 * ++held_count - 1;
            held_phase.store(live_phase, std::memory_order_release);
            wait_for_readers("acknowledge the live block", [&](int i) { return acknowledged[i].load() == live_phase; });
            end_ref(f, held.shadow);
            held_phase.store(live_phase + 1, std::memory_order_release);
            wait_for_readers("acknowledge the ended block", [&](int i) { return acknowledged[i].load() == live_phase + 1; });
        }
        const size_t size = rng() % 3 == 0 ? rng() % 4096 : rng() % 200;
        Made made = make_logged(size);
        end_ref(f, made.shadow);
        if (step % 2 == 1) collect();
        if (step % 64 == 0) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& thread : threads) thread.join();
    require(g_ref_type_asked_with_a_reference == 0,
            "GetObjectRefType of an address of the region that no block started at reached the runtime as the null reference: it was not looked up a second time");
    for (int i = 0; i < kReaders; ++i) {
        require(acknowledged[i].load() == 2 * held_count, "each reader acknowledged every block held live and every one held ended");
    }
    require(is_shadow_block(anchor.shadow) && length_of(f, anchor.shadow) == jsize(kAnchorBytes), "and it is live and whole after the churn");
    for (int i = 0; i < kReaders; ++i) {
        for (const Seen& now : seen[i]) {
            const auto found = published.find(now.address);
            const bool started_there = found != published.end();
            require(now.length != jsize(kForgedLength), "a length read is never a forged header's");
            require(now.length == 0 || (started_there && found->second.count(size_t(now.length)) != 0),
                    "a length read is 0, or the length of a block made at exactly that address");
            require(!(now.live || now.ended || now.type == kRefTypeLocal) || started_there,
                    "an address is a block (of either kind, or a local reference) only where a block started");
        }
    }
}

// ---- GetObjectRefType looks at the record once, and a block made at that moment is not what the reference was

// The answer for an address of the region that no block starts at is the null reference to the runtime, however a block comes to
// stand there before the call returns. A layer that looked the address up a second time (a conversion of the reference) would
// hand the runtime the new block's array, and nothing a fake runtime can pause on lies between the two loads of the record. So
// the row pauses the load itself: the record is the platform's memory, supplied by the fake map_record, and its first page is
// protected; the first acquire load of that page faults, the handler answers the load with the zero word that was there, and
// holds the thread while another thread publishes a block at exactly the queried address, then lets the load's thread go on.
// It exists in the test target only, on an arm64 macOS host (the handler reads the AArch64 instruction and the Mach thread
// state), and not under a sanitizer (whose runtime makes the load). The handler accepts only a 32-bit LDAR or LDAPR and stops
// the process with a message at any other fault; the hold is bounded; and the page, the handlers and the record are restored
// on every way out of the row.
#if defined(__APPLE__) && defined(__aarch64__) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define UML_OLDAPP_NO_LOAD_PAUSE 1
#endif
#endif
#else
#define UML_OLDAPP_NO_LOAD_PAUSE 1
#endif

#ifndef UML_OLDAPP_NO_LOAD_PAUSE
struct LoadPause {
    void* page = nullptr;
    size_t size = 0;
    std::atomic<int> phase{0};  // 1: the load was caught and the page is open again; 2: the other thread has published
    std::atomic<unsigned> faults{0};
};
LoadPause g_load_pause;
[[noreturn]] void load_pause_stops(const char* message, int code) {
    const ssize_t written = write(2, message, strlen(message));
    (void)written;
    _Exit(code);
}
uint64_t monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000u + uint64_t(now.tv_nsec);
}
void load_pause_handler(int, siginfo_t* info, void* context) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t start = reinterpret_cast<uintptr_t>(g_load_pause.page);
    if (at < start || at >= start + g_load_pause.size) {
        load_pause_stops("oldapp_jni_tests: a fault outside the protected page of the record\n", 91);
    }
    auto& state = static_cast<ucontext_t*>(context)->uc_mcontext->__ss;
    const uintptr_t pc = arm_thread_state64_get_pc(state);
    const uint32_t instruction = *reinterpret_cast<const uint32_t*>(pc);
    // LDAR Wt, [Xn] and LDAPR Wt, [Xn]: what an acquire load of a 32-bit word compiles to, with or without the RCpc extension.
    const uint32_t shape = instruction & 0xfffffc00u;
    const uint32_t target = instruction & 31u;
    if ((shape != 0x88dffc00u && shape != 0xb8bfc000u) || target >= 29u) {
        char message[] = "oldapp_jni_tests: the faulting instruction is not an acquire load of a word: 00000000\n";
        for (unsigned i = 0; i < 8; ++i) message[sizeof(message) - 10 + i] = "0123456789abcdef"[(instruction >> (28 - 4 * i)) & 15u];
        load_pause_stops(message, 92);
    }
    state.__x[target] = 0;  // the word that was there before the other thread publishes
    arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void (*)()>(pc + 4));
    if (mprotect(g_load_pause.page, g_load_pause.size, PROT_READ | PROT_WRITE) != 0) {
        load_pause_stops("oldapp_jni_tests: the record's page cannot be opened again\n", 93);
    }
    ++g_load_pause.faults;
    g_load_pause.phase.store(1, std::memory_order_release);
    const uint64_t deadline = monotonic_ns() + 10u * 1000000000u;
    while (g_load_pause.phase.load(std::memory_order_acquire) != 2) {
        if (monotonic_ns() > deadline) {
            load_pause_stops("oldapp_jni_tests: the other thread did not publish within 10 s\n", 94);
        }
    }
}
// The record's memory for one row, and everything the row changes, put back by the destructor.
class ProtectedRecord {
public:
    explicit ProtectedRecord(size_t bytes) : bytes_(bytes) {
        mapping_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        require(mapping_ != MAP_FAILED, "the record's memory is mapped");
        g_record_memory = static_cast<uint32_t*>(mapping_);
        g_record_memory_bytes = bytes_;
    }
    ProtectedRecord(const ProtectedRecord&) = delete;
    ProtectedRecord& operator=(const ProtectedRecord&) = delete;
    ~ProtectedRecord() {
        disarm();
        g_record_memory = g_record_storage;
        g_record_memory_bytes = sizeof(g_record_storage);
        munmap(mapping_, bytes_);
    }
    // The first page of the record is protected, and the handler is in place.
    void arm() {
        g_load_pause.page = mapping_;
        g_load_pause.size = size_t(getpagesize());
        g_load_pause.phase = 0;
        g_load_pause.faults = 0;
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_sigaction = load_pause_handler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        require(sigaction(SIGBUS, &action, &old_bus_) == 0 && sigaction(SIGSEGV, &action, &old_segv_) == 0, "the fault handlers");
        armed_ = true;
        require(mprotect(mapping_, g_load_pause.size, PROT_NONE) == 0, "the first page of the record is protected");
    }
    void disarm() {
        if (!armed_) return;
        armed_ = false;
        mprotect(mapping_, g_load_pause.size, PROT_READ | PROT_WRITE);
        sigaction(SIGBUS, &old_bus_, nullptr);
        sigaction(SIGSEGV, &old_segv_, nullptr);
        g_load_pause.page = nullptr;
    }

private:
    void* mapping_ = nullptr;
    size_t bytes_;
    bool armed_ = false;
    struct sigaction old_bus_;
    struct sigaction old_segv_;
};
#endif

void check_get_object_ref_type_looks_at_the_record_once() {
    constexpr const char* kCase = "check_get_object_ref_type_looks_at_the_record_once";
#ifdef UML_OLDAPP_NO_LOAD_PAUSE
    std::printf("%s: skipped: it intercepts an AArch64 acquire load in the signal context, so it needs an arm64 macOS host and no sanitizer\n",
                kCase);
#else
    ProtectedRecord record(shadow_record_bytes(sizeof(g_shadow_region)));
    Fixture f = make();
    Other writer(f);
    void* const candidate = g_region + kHeadBytes + 16;
    require(!is_shadow_block(candidate), "the address starts empty");
    Made published{};
    std::thread maker([&] {
        wait_for_counter(kCase, "the pause", g_load_pause.phase, 1, "the writer (the query's load is caught)");
        published = make_shadow_of(writer.f, sample(100));
        require(published.shadow == candidate, "the writer publishes a block at exactly the queried address");
        g_load_pause.phase.store(2, std::memory_order_release);
    });
    record.arm();
    const jint type = fn<jint (*)(Env*, jobject)>(*f.table, slot::GetObjectRefType)(f.env, candidate);
    maker.join();
    record.disarm();
    require(g_load_pause.faults.load() == 1 && g_load_pause.phase.load() == 2, "the one load of the record was caught and let go on");
    require(is_shadow_block(candidate) && length_of(f, candidate) == 100, "the block was published before the query went on");
    require(type == 0, "the answer follows the one load: no block (invalid)");
    require(g_ref_type_asked_with_a_reference == 0, "and the runtime was handed the null reference, not the block that appeared");
#endif
}

// The block record has its own memory, which the platform gives and the layer never clears; a region it cannot cover gets no
// shadows. The layer asks for the record of its region and for nothing else; it writes nothing to the memory it is given at the
// installation (the platform's memory is zero-filled); and no shadow is made where the record cannot be had, the region's base is
// not 16-byte aligned or the region is larger than the record can describe.
void check_shadows_need_a_record_the_region_can_be_described_by() {
    const auto installs_nothing = [&](void* region, size_t size, const char* what, const Platform& platform = kPlatform) {
        reset_world();
        Table* table = new Table;
        g_record_expected_bytes = shadow_record_bytes(size);
        require(install_jni_wrappers(table->slots, platform, region, size, &table->env), what);
        require(table->slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength) &&
                    table->slots[slot::GetByteArrayRegion] == reinterpret_cast<void*>(&f_GetByteArrayRegion),
                what);
        require(!is_shadow_block(region) && counters().shadows_created == 0, what);
    };
    g_record_fails = true;
    installs_nothing(g_shadow_region, sizeof(g_shadow_region), "the platform cannot give the record: nothing is intercepted");
    g_record_fails = false;
    Platform without_record = kPlatform;
    without_record.map_record = nullptr;
    installs_nothing(g_shadow_region, sizeof(g_shadow_region), "a platform with no map_record: nothing is intercepted", without_record);
    installs_nothing(g_shadow_region + 8, sizeof(g_shadow_region) - 16, "a region whose base is not 16-byte aligned: nothing is intercepted");
    installs_nothing(g_shadow_region, kMaxShadowBytes + 16, "a region the record cannot describe: nothing is intercepted");
    // The record is asked for by the size of the region, and what the platform gives is not written at the installation.
    g_record_reset = false;
    std::memset(g_record_storage, 0x5a, shadow_record_bytes(sizeof(g_shadow_region)));
    {
        reset_world();
        Table* table = new Table;
        g_record_expected_bytes = shadow_record_bytes(sizeof(g_shadow_region));
        require(install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), &table->env), "installs");
        require(table->slots[slot::GetArrayLength] != reinterpret_cast<void*>(&f_GetArrayLength), "and intercepts");
        bool untouched = true;
        for (size_t i = 0; i < shadow_record_bytes(sizeof(g_shadow_region)) / sizeof(uint32_t); ++i) untouched = untouched && g_record_storage[i] == 0x5a5a5a5au;
        require(untouched, "the installation wrote nothing to the memory of the record");
    }
    g_record_reset = true;
}

// Only the layer's reclaim removes a block from the record. A block whose array the collector has cleared is still an ended block
// of its length (GetArrayLength says it; the wrappers find no array behind it and act as for null); the next sweep, a creation
// after the collection, reclaims it, and its reference is then no block, or the block that is made at exactly that address.
void check_a_collected_block_is_an_ended_block_of_its_length_until_a_sweep_reclaims_it() {
    Fixture f = make();
    Made left = make_shadow_of(f, sample(100));  // live: it keeps the freed memory from starting before the block
    Made gone = make_shadow_of(f, sample(100));
    end_ref(f, gone.shadow);
    collect();
    require(is_ended_block(gone.shadow) && !is_shadow_block(gone.shadow) && length_of(f, gone.shadow) == 100 &&
                counters().shadows_ended == 1 && counters().blocks_collected == 0,
            "collected and not reclaimed: still an ended block, of its length");
    require(get_elements(f, gone.shadow) == nullptr, "and the wrappers find no array behind it");
    require(call_static_int_v(f, f.consume, gone.shadow) == 7 && world.last_array_args.size() == 1 && world.last_array_args[0] == nullptr,
            "a call is handed null");
    Made next = make_shadow_of(f, sample(300));  // a creation after the collection sweeps it
    require(counters().blocks_collected == 1 && counters().shadows_ended == 0, "the sweep reclaimed it");
    require(next.shadow == gone.shadow && !is_ended_block(gone.shadow) && is_shadow_block(gone.shadow) && length_of(f, gone.shadow) == 300,
            "its address is now the block made there, with that block's length");
    require(is_shadow_block(left.shadow) && length_of(f, left.shadow) == 100, "the other block is as it was");
}

// The record is cleared before the memory of a reclaimed block goes back to the arena: by the time the block's baseline is freed (the
// last step before the release) the reclaimed block's reference is already no block, of either kind, with no length, while the block
// beside it is as it was. (Fails on a layer that clears the record after the release: another block could be made at the address
// and published, and the clear would then take that block out of the record.)
void check_the_record_is_cleared_before_the_memory_goes_back_to_the_arena() {
    Fixture f = make();
    Made left = make_shadow_of(f, sample(100));  // live beside it
    Made gone = make_shadow_of(f, sample(100));
    end_ref(f, gone.shadow);
    collect();
    int frees = 0;
    g_in_free_baseline = [&] {
        ++frees;
        require(!is_shadow_block(gone.shadow) && !is_ended_block(gone.shadow) && length_of(f, gone.shadow) == 0,
                "the reclaimed block's reference is no block by the time its baseline is freed");
        require(is_shadow_block(left.shadow) && length_of(f, left.shadow) == 100, "the block beside it is as it was");
    };
    Made next = make_shadow_of(f, sample(300));  // a creation after the collection sweeps the ended block
    require(frees == 1 && counters().blocks_collected == 1, "the sweep reclaimed it, and the hook ran once");
    require(is_shadow_block(next.shadow), "and the creation went on");
}

// ---- every call form converts once a shadow may have reached the application: the current counts decide nothing
//
// A call hands the runtime what the layer made of each reference in it: the array for a block, null for any other address of the
// region. The numbers of live and ended blocks say nothing about that: both are zero while a block is revived (counted live after
// its contents are synchronized) or built, and after the last block was reclaimed or never published, while an application
// keeps the address of a block it once had.

// The methods the rows below call. Each form hands the runtime one reference that the row chooses, as the byte[] argument of a
// method or as the receiver of one, so what the fake runtime receives says what the layer made of it.
struct CallKit {
    jmethodID take;   // ([B)Ljava/lang/Object;  (instance)
    jmethodID touch;  // ()Ljava/lang/Object;    (instance)
    jobject plain;    // a real object: the receiver of the argument forms
};
CallKit make_call_kit(Fixture& f) {
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    return CallKit{get_method(f.env, f.cls, "take", "([B)Ljava/lang/Object;"), get_method(f.env, f.cls, "touch", "()Ljava/lang/Object;"),
                   string_object()};
}

jobject call_nonvirtual_object_v(Fixture& f, jobject receiver, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jobject r = fn<jobject (*)(Env*, jobject, jclass, jmethodID, va_list)>(*f.table, nonvirtual_slot(kObject, kVa))(f.env, receiver, f.cls, m, ap);
    va_end(ap);
    return r;
}
jobject construct_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jobject r = fn<jobject (*)(Env*, jclass, jmethodID, va_list)>(*f.table, slot::NewObjectV)(f.env, f.cls, m, ap);
    va_end(ap);
    return r;
}

struct CallForm {
    const char* name;
    bool receiver;  // the reference is the receiver of the call; otherwise it is the byte[] argument
    void (*issue)(Fixture&, const CallKit&, jobject);
};
using ObjectA = jobject (*)(Env*, jobject, jmethodID, const jvalue*);
using NonvirtualA = jobject (*)(Env*, jobject, jclass, jmethodID, const jvalue*);
const CallForm kCallForms[] = {
    {"CallStaticIntMethodV", false, [](Fixture& f, const CallKit&, jobject o) { (void)call_static_int_v(f, f.consume, o); }},
    {"CallStaticIntMethodA", false,
     [](Fixture& f, const CallKit&, jobject o) {
         jvalue a[1] = {};
         a[0].l = o;
         (void)fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, f.consume, a);
     }},
    {"CallStaticIntMethod", false,
     [](Fixture& f, const CallKit&, jobject o) {
         (void)fn<jint (*)(Env*, jclass, jmethodID, ...)>(*f.table, static_call_slot(kInt, kVarargs))(f.env, f.cls, f.consume, o);
     }},
    {"CallStaticVoidMethodV", false, [](Fixture& f, const CallKit&, jobject o) { call_static_void_v(f, f.consume_void, jint(1), o); }},
    {"CallStaticVoidMethodA", false,
     [](Fixture& f, const CallKit&, jobject o) {
         jvalue a[2] = {};
         a[0].z = 1;
         a[1].l = o;
         fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, f.consume_void, a);
     }},
    {"CallStaticVoidMethod", false,
     [](Fixture& f, const CallKit&, jobject o) {
         fn<void (*)(Env*, jclass, jmethodID, ...)>(*f.table, static_call_slot(kVoid, kVarargs))(f.env, f.cls, f.consume_void, jint(1), o);
     }},
    {"CallObjectMethodV", false, [](Fixture& f, const CallKit& k, jobject o) { (void)call_object_v(f, k.plain, k.take, o); }},
    {"CallObjectMethodA", false,
     [](Fixture& f, const CallKit& k, jobject o) {
         jvalue a[1] = {};
         a[0].l = o;
         (void)fn<ObjectA>(*f.table, call_slot(kObject, kArray))(f.env, k.plain, k.take, a);
     }},
    {"CallObjectMethod", false,
     [](Fixture& f, const CallKit& k, jobject o) {
         (void)fn<jobject (*)(Env*, jobject, jmethodID, ...)>(*f.table, call_slot(kObject, kVarargs))(f.env, k.plain, k.take, o);
     }},
    {"CallNonvirtualObjectMethodV", false, [](Fixture& f, const CallKit& k, jobject o) { (void)call_nonvirtual_object_v(f, k.plain, k.take, o); }},
    {"CallNonvirtualObjectMethodA", false,
     [](Fixture& f, const CallKit& k, jobject o) {
         jvalue a[1] = {};
         a[0].l = o;
         (void)fn<NonvirtualA>(*f.table, nonvirtual_slot(kObject, kArray))(f.env, k.plain, f.cls, k.take, a);
     }},
    {"CallNonvirtualObjectMethod", false,
     [](Fixture& f, const CallKit& k, jobject o) {
         (void)fn<jobject (*)(Env*, jobject, jclass, jmethodID, ...)>(*f.table, nonvirtual_slot(kObject, kVarargs))(f.env, k.plain, f.cls, k.take, o);
     }},
    {"NewObjectV", false, [](Fixture& f, const CallKit&, jobject o) { (void)construct_v(f, f.construct, o); }},
    {"NewObjectA", false,
     [](Fixture& f, const CallKit&, jobject o) {
         jvalue a[1] = {};
         a[0].l = o;
         (void)fn<jobject (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, slot::NewObjectA)(f.env, f.cls, f.construct, a);
     }},
    {"NewObject", false,
     [](Fixture& f, const CallKit&, jobject o) {
         (void)fn<jobject (*)(Env*, jclass, jmethodID, ...)>(*f.table, slot::NewObject)(f.env, f.cls, f.construct, o);
     }},
    {"CallObjectMethodV (receiver)", true, [](Fixture& f, const CallKit& k, jobject o) { (void)call_object_v(f, o, k.touch); }},
    {"CallObjectMethodA (receiver)", true,
     [](Fixture& f, const CallKit& k, jobject o) {
         const jvalue none[1] = {};
         (void)fn<ObjectA>(*f.table, call_slot(kObject, kArray))(f.env, o, k.touch, none);
     }},
    {"CallObjectMethod (receiver)", true,
     [](Fixture& f, const CallKit& k, jobject o) {
         (void)fn<jobject (*)(Env*, jobject, jmethodID, ...)>(*f.table, call_slot(kObject, kVarargs))(f.env, o, k.touch);
     }},
    {"CallNonvirtualObjectMethodV (receiver)", true, [](Fixture& f, const CallKit& k, jobject o) { (void)call_nonvirtual_object_v(f, o, k.touch); }},
    {"CallNonvirtualObjectMethodA (receiver)", true,
     [](Fixture& f, const CallKit& k, jobject o) {
         const jvalue none[1] = {};
         (void)fn<NonvirtualA>(*f.table, nonvirtual_slot(kObject, kArray))(f.env, o, f.cls, k.touch, none);
     }},
    {"CallNonvirtualObjectMethod (receiver)", true,
     [](Fixture& f, const CallKit& k, jobject o) {
         (void)fn<jobject (*)(Env*, jobject, jclass, jmethodID, ...)>(*f.table, nonvirtual_slot(kObject, kVarargs))(f.env, o, f.cls, k.touch);
     }},
};

Obj g_not_reached;  // what the fake notes before a call: it is never handed out, so a call that did not reach the runtime shows
void forget_the_last_call() {
    WorldGuard guard;
    world.last_call.clear();
    world.last_array_args.assign(1, &g_not_reached);
    world.receiver_calls = 0;
    world.last_receiver = &g_not_reached;
}
// The object behind the reference the runtime was handed by the call since forget_the_last_call, or null when it was handed null.
Obj* runtime_was_handed(const CallForm& form) {
    WorldGuard guard;
    const std::string at = std::string(form.name) + ": ";
    if (form.receiver) {
        require(world.receiver_calls == 1 && world.last_receiver != &g_not_reached, (at + "the call reached the runtime once").c_str());
        return world.last_receiver;
    }
    require(!world.last_call.empty() && world.last_array_args.size() == 1 && world.last_array_args[0] != &g_not_reached,
            (at + "the call reached the runtime with its one reference").c_str());
    return world.last_array_args[0];
}

// A block is revived while another thread calls with its old pointer: the array is obtained again, the block is live in the record
// and neither counted live nor ended (the live count follows the synchronization), and the pointer is a kept reference of a
// reachable array, so every call form must give the runtime a reference of its own to the array. (Fails on a layer that skips the
// conversion while the counts are zero: the runtime is handed the address of the block.)
void check_a_call_during_a_revival_is_handed_the_array_by_a_reference_of_the_runtime() {
    constexpr const char* kCase = "check_a_call_during_a_revival_is_handed_the_array_by_a_reference_of_the_runtime";
    for (const CallForm& form : kCallForms) {
        Fixture f = make();
        const CallKit kit = make_call_kit(f);
        Other caller_env(f);
        Made x = make_held(f, std::vector<uint8_t>(100, 0x11));
        end_ref(f, x.shadow);
        require(is_ended_block(x.shadow) && counters().shadows_ended == 1, "the block ended, and its array is reachable");
        std::atomic<bool> tried_the_lock{false}, finished{false};
        std::thread caller;
        int pauses = 0;
        forget_the_last_call();
        g_before_lock = [&] { tried_the_lock = true; };
        g_after_get = [&] {  // the revival copies the array into the block, under the mutex
            ++pauses;
            require(counters().shadows_live == 0 && counters().shadows_ended == 0 && counters().blocks_revived == 0,
                    "the revival has left the ended count and not yet joined the live count");
            require(is_shadow_block(x.shadow) && length_of(f, x.shadow) == 100, "and the record says the block is live and whole");
            caller = std::thread([&] {
                t_pause_at_lock = true;
                form.issue(caller_env.f, kit, x.shadow);
                t_pause_at_lock = false;
                finished = true;
            });
            // A conversion waits for the mutex the revival holds; a call that skips it reaches the runtime at once.
            require(waitUntil([&] { return tried_the_lock.load() || finished.load(); }, kHandOffLimit),
                    (std::string(kCase) + ": the call neither waited for the mutex nor finished").c_str());
        };
        Made again = obtain_again(f, x.object);
        require(pauses == 1 && caller.joinable(), "the revival paused once, and the call was made meanwhile");
        caller.join();
        require(again.shadow == x.shadow && counters().shadows_live == 1 && counters().blocks_revived == 1, "the array has its block back");
        require(runtime_was_handed(form) == x.object, (std::string(form.name) + ": the runtime was handed the array").c_str());
    }
}

// The address of a block that is gone is no block, whichever numbers are zero: after a failed creation reclaimed the last block, and
// while a block is being built at that very address (nothing is published, nothing is counted), every call form gives the runtime
// null for it, and no address of the region. (Fails on a layer that skips the conversion while the counts are zero.)
void check_a_call_with_the_address_of_a_block_that_is_gone_is_handed_null() {
    for (const CallForm& form : kCallForms) {
        Fixture f = make();
        const CallKit kit = make_call_kit(f);
        Other reader(f);
        Made x = make_shadow_of(f, std::vector<uint8_t>(100, 0x11));
        end_ref(f, x.shadow);
        collect();
        g_null_receiver_ok = true;
        // (a) a replacement is being built at x's address: its contents are copied in, nothing is published or counted
        int pauses = 0;
        g_after_get = [&] {
            ++pauses;
            require(counters().shadows_live == 0 && counters().shadows_ended == 0, "no block is counted while the replacement is built");
            require(!is_shadow_block(x.shadow) && !is_ended_block(x.shadow) && length_of(f, x.shadow) == 0, "and none is published yet");
            require(static_cast<const uint8_t*>(g_last_get_buffer.load()) - 16 == x.shadow, "the replacement is being built at x's address");
            std::thread([&] {
                forget_the_last_call();
                form.issue(reader.f, kit, x.shadow);
                require(runtime_was_handed(form) == nullptr, (std::string(form.name) + ": (a) the runtime was handed null").c_str());
            }).join();
        };
        Made y = make_shadow_of(f, std::vector<uint8_t>(100, 0x66));
        require(pauses == 1 && y.shadow == x.shadow, "the replacement took x's address");
        // (b) the replacement ended and was collected; the next creation fails after it has reclaimed it: nothing is left
        end_ref(f, y.shadow);
        collect();
        g_baseline_fails = true;
        Made declined = make_shadow_of(f, std::vector<uint8_t>(100, 0x77));
        g_baseline_fails = false;
        require(!is_shadow_block(declined.shadow) && counters().shadows_live == 0 && counters().shadows_ended == 0 &&
                    counters().blocks_collected == 2,
                "every block was reclaimed, and the failed creation published none");
        require(!is_shadow_block(x.shadow) && !is_ended_block(x.shadow) && length_of(f, x.shadow) == 0, "x's address is no block");
        forget_the_last_call();
        form.issue(f, kit, x.shadow);
        require(runtime_was_handed(form) == nullptr, (std::string(form.name) + ": (b) the runtime was handed null").c_str());
    }
}

// A setup that is refused leaves no query bounds behind: the queries range-test an address before they read the record, and a
// refused attempt after an attempt that was refused later (the table could not be written, the record was already mapped) must not
// leave the range of the first for an address to match with no record to read. A fresh refusal and the retry sequence both answer
// "no block" for an address inside the region. (Fails with a null dereference on a layer that keeps the bounds.)
void check_a_refused_install_leaves_no_query_bounds() {
    const void* const inside = g_shadow_region + kHeadBytes + 16;
    const auto require_no_block = [&](const char* what) {
        require(!is_shadow_block(inside) && !is_ended_block(inside), what);
    };
    {  // a fresh install whose record cannot be had
        reset_world();
        Table* table = new Table;
        g_record_expected_bytes = shadow_record_bytes(sizeof(g_shadow_region));
        g_record_fails = true;
        require(install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), &table->env), "the refused install returns");
        g_record_fails = false;
        require_no_block("a fresh install without a record: an address of the region is no block");
    }
    {  // the table cannot be written, and the retry cannot have its record
        reset_world();
        Table* table = new Table;
        g_record_expected_bytes = shadow_record_bytes(sizeof(g_shadow_region));
        g_writable_ok = false;
        require(!install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), &table->env),
                "the first attempt is refused: the table cannot be written");
        require_no_block("after the first refusal: an address of the region is no block");
        g_writable_ok = true;
        g_record_fails = true;
        require(install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), &table->env),
                "the retry returns, with shadows off");
        g_record_fails = false;
        require(table->slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength), "and leaves the table as it was");
        require_no_block("after the retry without a record: an address of the region is no block");
    }
    {  // a retry that works after a refusal makes blocks
        reset_world();
        Table* table = new Table;
        g_record_expected_bytes = shadow_record_bytes(sizeof(g_shadow_region));
        g_writable_ok = false;
        require(!install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), &table->env), "refused");
        g_writable_ok = true;
        require(install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), &table->env), "and then accepted");
        require(table->slots[slot::GetArrayLength] != reinterpret_cast<void*>(&f_GetArrayLength), "wrapped");
    }
    g_record_fails = false;
    g_writable_ok = true;
}

// ---- a call never reaches the runtime unconverted: the parameters of a method are known, or the process stops
//
// Which of a call's values are references is known from the method: its signature at the lookup, else the platform's reflection
// at the first call. Nothing bounds the number of methods or of their parameters, and a failure to learn is not remembered. Where
// the parameters cannot be had at the moment of a call, the layer cannot tell a block's address from an integer, and the runtime
// would take it for a reference: the application's call does not reach it, and the process stops after one line in the log (a
// platform caller's call is forwarded, receiver converted, arguments as they are).

// Runs `call`, which must not return: the layer stops the process (Platform::abort_process) and the fake's stop comes back here.
template <class F>
bool the_process_stops(F&& call) {
    const int before = g_stops;
    g_stop_expected = true;
    if (setjmp(g_stop_jump) == 0) {
        call();
        g_stop_expected = false;
        return false;
    }
    return g_stops == before + 1;
}
size_t stop_lines() {
    WorldGuard guard;
    size_t n = 0;
    for (const auto& line : world.logs) n += line.message.find("cannot be learned") != std::string::npos ? 1 : 0;
    return n;
}

jmethodID unseen_method(const char* signature) {
    WorldGuard guard;
    void* id = new int(0);  // an ID the layer never saw being looked up (FromReflectedMethod makes such)
    world.methods[id] = Method{signature, "unseen"};
    return static_cast<jmethodID>(id);
}

// More methods than the registry had slots (and than any probe could place), and more parameters than its pool held: every one is
// known from its lookup, and a shadow passed to it is converted. (Fails on a registry of fixed size: the call is forwarded
// unchanged and the fake runtime is handed the block.)
void check_every_method_the_application_looks_up_is_known() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));  // a shadow may be in the application's hands from here on
    auto get_static = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetStaticMethodID);
    std::vector<jmethodID> ids;
    for (int i = 0; i < 20000; ++i) ids.push_back(get_static(f.env, f.cls, "m", "(I[B)I"));
    for (size_t i = 0; i < ids.size(); i += (i < 100 || i + 100 >= ids.size()) ? 1 : 197) {
        forget_the_last_call();
        require(call_static_int_v(f, ids[i], 3, made.shadow) == 7 && world.last_array_args.size() == 1 &&
                    world.last_array_args[0] == made.object,
                "a method looked up among twenty thousand: its shadow is converted");
    }
    const std::string wide_signature = "(" + std::string(254, 'I') + "[B)V";
    std::vector<jmethodID> wide;
    for (int i = 0; i < 300; ++i) wide.push_back(get_static(f.env, f.cls, "w", wide_signature.c_str()));
    jvalue args[255] = {};
    args[254].l = made.shadow;
    for (jmethodID m : wide) {
        forget_the_last_call();
        fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, m, args);
        require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object,
                "a method of 255 parameters among three hundred such: its shadow is converted");
    }
    require(world.describe_calls == 0 && g_stops == 0, "every method was known from its lookup: none was asked about, none stopped");
}

// An ID the layer must describe at its call, and that the platform cannot describe, is not remembered as undescribable: the call
// does not reach the runtime, the process is stopped (one line each time), and the next call asks the platform again and goes
// through once it can. (Fails on a layer that remembers the failure: no second question, and the call is forwarded unchanged.)
void check_a_method_that_cannot_be_described_stops_the_call_and_is_asked_about_again() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    jmethodID unseen = unseen_method("(I[B)I");
    world.describe_fails = true;
    forget_the_last_call();
    require(the_process_stops([&] { (void)call_static_int_v(f, unseen, 5, made.shadow); }), "the call in the V form stops the process");
    require(world.last_call.empty() && world.describe_calls == 1 && stop_lines() == 1, "it did not reach the runtime; one question, one line");
    jvalue args[2] = {};
    args[0].i = 5;
    args[1].l = made.shadow;
    require(the_process_stops([&] {
                (void)fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, unseen, args);
            }),
            "and in the A form");
    require(world.last_call.empty() && world.describe_calls == 2 && stop_lines() == 2, "the failure was not remembered: it was asked about again");
    world.describe_fails = false;
    require(call_static_int_v(f, unseen, 5, made.shadow) == 7 && world.last_array_args.size() == 1 && world.last_array_args[0] == made.object &&
                world.describe_calls == 3,
            "once the platform can say, the call goes through converted");
    require(call_static_int_v(f, unseen, 6, made.shadow) == 7 && world.describe_calls == 3, "and the answer is kept");
}

// Every other way the parameters can be unobtainable at a call stops it too, and none lets the runtime see the block's address:
// nothing to describe from (a platform without reflection), no method ID (the runtime aborts on that itself), and no memory for the
// method's node (the lookup then stored nothing, and the first call learns it again once memory is back). A method is described
// from its ID alone, so a null receiver or class is no cause: an instance call without a receiver is forwarded for the runtime to
// refuse (it aborts on a null receiver), and a static call with a null class runs (see the row for it).
void check_the_other_causes_of_unknown_parameters_stop_the_call() {
    {
        g_without_describe = true;
        Fixture f = make();
        g_without_describe = false;
        Made made = make_shadow_of(f, sample(16));
        jmethodID unseen = unseen_method("([B)I");
        forget_the_last_call();
        require(the_process_stops([&] { (void)call_static_int_v(f, unseen, made.shadow); }) && world.last_call.empty() && stop_lines() == 1,
                "a platform that cannot describe: the call stops");
        require(the_process_stops([&] { (void)call_static_int_v(f, nullptr, made.shadow); }) && world.last_call.empty() && stop_lines() == 2,
                "no method ID: the call stops");
    }
    {
        Fixture f = make();
        const CallKit kit = make_call_kit(f);
        Made made = make_shadow_of(f, sample(16));
        jmethodID unseen = unseen_method("([B)Ljava/lang/Object;");
        forget_the_last_call();
        g_null_receiver_ok = true;  // the fake notes a null receiver and goes on; the real runtime aborts on it
        (void)call_object_v(f, nullptr, unseen, made.shadow);
        g_null_receiver_ok = false;
        require(g_stops == 0 && world.describe_calls == 1 && !world.last_call.empty() && world.last_array_args.size() == 1 &&
                    world.last_array_args[0] == made.object,
                "an instance call without a receiver is learned from the ID and forwarded converted: no stop of the layer's");
        (void)kit;
    }
    {
        Fixture f = make();
        Made made = make_shadow_of(f, sample(16));
        auto get_static = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetStaticMethodID);
        g_baseline_fails = true;  // no memory for the node of a method
        jmethodID late = get_static(f.env, f.cls, "late", "(I[B)I");
        forget_the_last_call();
        require(the_process_stops([&] { (void)call_static_int_v(f, late, 1, made.shadow); }) && world.last_call.empty() && world.describe_calls == 1,
                "a lookup that stored nothing, learned again at the call that still has no memory: the call stops");
        g_baseline_fails = false;
        require(call_static_int_v(f, late, 1, made.shadow) == 7 && world.last_array_args.size() == 1 && world.last_array_args[0] == made.object &&
                    world.describe_calls == 2,
                "with memory back the call learns the method and is converted");
    }
}

// The runtime ignores the class argument of a CallStatic* (only the method ID is checked), so a static call with a null class runs,
// and so it does here whether or not the ID was seen at a lookup: an ID the layer must describe is described from the ID alone.
// (Fails on a layer that describes a method by a receiver or class: the call stops the process, and nothing reaches the runtime.)
void check_a_static_call_with_a_null_class_runs_converted() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    auto variadic = fn<jint (*)(Env*, jclass, jmethodID, ...)>(*f.table, static_call_slot(kInt, kVarargs));
    auto array = fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray));
    auto va_form = [&](jclass cls, jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        const jint r = fn<jint (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kInt, kVa))(f.env, cls, m, ap);
        va_end(ap);
        return r;
    };
    const auto converted = [&](const char* form) {
        require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object && g_stops == 0, form);
        forget_the_last_call();
    };
    forget_the_last_call();
    jmethodID unseen_variadic = unseen_method("(I[B)I");
    require(variadic(f.env, nullptr, unseen_variadic, 5, made.shadow) == 7, "the variadic form runs");
    converted("a static call with a null class, variadic, an unseen ID: the block is converted");
    jmethodID unseen_v = unseen_method("(I[B)I");
    require(va_form(nullptr, unseen_v, 5, made.shadow) == 7, "the V form runs");
    converted("and in the V form");
    jmethodID unseen_a = unseen_method("(I[B)I");
    jvalue args[2] = {};
    args[0].i = 5;
    args[1].l = made.shadow;
    require(array(f.env, nullptr, unseen_a, args) == 7, "the A form runs");
    converted("and in the A form");
    require(world.describe_calls == 3, "each unseen ID was described once, from the ID alone");
    require(variadic(f.env, nullptr, unseen_variadic, 6, made.shadow) == 7 && world.describe_calls == 3, "and the answer is kept");
    converted("a second call with the same ID");
    jmethodID looked_up = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetStaticMethodID)(f.env, f.cls, "m", "(I[B)I");
    require(variadic(f.env, nullptr, looked_up, 7, made.shadow) == 7 && world.describe_calls == 3, "an ID that was looked up runs with a null class as well");
    converted("and its block is converted");
}

// The stop is for the application's calls. A platform caller whose call's parameters cannot be had (a framework thread that meets a
// failing reflection at its first call of a method ID cached before the layer) is forwarded: its receiver is converted, its
// arguments are as it passed them, and nothing is remembered. It can hold a block's address only through an application's type
// error, which fails on stock too. The same state still stops an application's call.
// (Fails on a layer that stops for every caller: the process stops and nothing reaches the runtime.)
void check_a_platform_call_whose_parameters_cannot_be_had_is_forwarded() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    jmethodID unseen_static = unseen_method("(J)I");
    jmethodID unseen_instance = unseen_method("(J)Ljava/lang/Object;");
    const jlong value = static_cast<jlong>(reinterpret_cast<intptr_t>(made.shadow));  // an integer that is a block's address
    world.describe_fails = true;
    g_app_caller = false;
    const auto forwarded = [&](const char* what, bool with_receiver) {
        require(g_stops == 0 && !world.last_call.empty() && world.last_args.size() == 1 && world.last_args[0].j == value, what);
        require(!with_receiver || world.last_receiver == made.object, "and its receiver, a block, was converted");
        forget_the_last_call();
    };
    forget_the_last_call();
    require(call_static_int_v(f, unseen_static, value) == 7, "a static call by a platform caller runs");
    forwarded("the V form: forwarded with its argument as it was passed", false);
    jvalue args[1] = {};
    args[0].j = value;
    require(fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, unseen_static, args) == 7,
            "the A form runs");
    forwarded("the A form: forwarded with its argument as it was passed", false);
    (void)call_object_v(f, made.shadow, unseen_instance, value);
    forwarded("an instance call, V form: forwarded with its argument as it was passed", true);
    (void)fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, made.shadow, unseen_instance, args);
    forwarded("an instance call, A form: forwarded with its argument as it was passed", true);
    require(world.describe_calls == 4 && stop_lines() == 0, "every call asked again, none stopped, none logged a stop");
    g_app_caller = true;
    forget_the_last_call();
    require(the_process_stops([&] { (void)call_static_int_v(f, unseen_static, value); }) && world.last_call.empty() && stop_lines() == 1,
            "the same state, from the application: the call stops");
    world.describe_fails = false;
}

// A call made with an exception pending (ART does not refuse it) is no cause: the parameters of an ID the layer has to describe
// are had with the exception taken out for the time, and the same exception is pending again after the call.
void check_an_unseen_method_is_learned_with_an_exception_pending() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    jmethodID unseen = unseen_method("(I[B)I");
    {
        WorldGuard guard;
        world.exception_pending = true;
        world.pending_object = new Obj{};
    }
    Obj* const pending = world.pending_object;
    forget_the_last_call();
    require(call_static_int_v(f, unseen, 5, made.shadow) == 7 && world.last_array_args.size() == 1 && world.last_array_args[0] == made.object,
            "the call went through converted");
    require(world.describe_calls == 1 && g_stops == 0, "the method was described once");
    require(world.exception_pending && world.pending_object == pending, "and the same exception is pending after it");
}

// The ids of the layer's own calls are resolved at the installation, before the first block can be made.
void check_the_ids_are_resolved_before_the_first_block() {
    Fixture f = make();
    require(world.class_globals.size() == 2 && world.hash_calls == 0, "the classes were looked up at the installation");
    g_on_find_class = [&] { require(false, "no lookup is left for a result"); };
    Made made = make_shadow_of(f, sample(10));
    require(is_shadow_block(made.shadow), "the first result is a block");
}

// Every block that exists holds a reservation against the cap, live ones included: live blocks on several threads cannot
// end past it (each end makes a weak global reference, and the runtime's table is limited).
void check_live_blocks_of_many_threads_count_against_the_cap() {
    g_region = g_big_region;
    g_region_size = sizeof(g_big_region);
    Fixture f = make();
    g_region = g_shadow_region;
    g_region_size = sizeof(g_shadow_region);
    std::vector<Obj*> kept;
    for (unsigned i = 0; i < 16000; ++i) {
        Made made = make_held(f, std::vector<uint8_t>());
        kept.push_back(made.object);
        end_ref(f, made.shadow);
    }
    require(counters().shadows_ended == 16000, "16000 ended blocks");
    constexpr int kThreads = 3;
    std::vector<std::unique_ptr<Other>> others;
    std::vector<std::vector<jobject>> shadows(kThreads);
    for (int t = 0; t < kThreads; ++t) others.emplace_back(new Other(f));
    // One thread at a time, in turns (each counts the blocks it made and ended in order, and `kept` is shared). The turn is one
    // counter that every phase moves forward, and no value belongs to two phases: worker t makes its blocks at turn t; the
    // value kThreads is the main thread's (all have made theirs and hold them), and no worker waits for it; the main thread
    // then sets kEnds, and worker t ends its blocks at turn kEnds + t.
    constexpr int kChecks = kThreads;
    constexpr int kEnds = kThreads + 1;
    constexpr const char* kCase = "check_live_blocks_of_many_threads_count_against_the_cap";
    std::atomic<int> turn{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            wait_for_counter(kCase, "turn", turn, t, "a thread making its blocks");
            for (int i = 0; i < 512; ++i) {
                Made made = make_held(others[size_t(t)]->f, std::vector<uint8_t>());
                if (is_shadow_block(made.shadow)) shadows[size_t(t)].push_back(made.shadow);
                kept.push_back(made.object);
            }
            ++turn;
            wait_for_counter(kCase, "turn", turn, kEnds + t, "a thread ending its blocks");
            for (jobject shadow : shadows[size_t(t)]) end_ref(others[size_t(t)]->f, shadow);
            ++turn;
        });
    }
    wait_for_counter(kCase, "turn", turn, kChecks, "the main thread (all threads hold their blocks)");
    require(counters().shadows_live + counters().shadows_ended <= 16384, "live and ended blocks together stay under the cap");
    require(counters().shadows_declined > 0, "the results past it stayed real references");
    turn = kEnds;
    for (auto& thread : threads) thread.join();
    require(counters().shadows_ended <= 16384 && counters().shadows_live == 0, "and so do the ended ones after every end");
    require(block_weaks() <= 16384, "the weak references the layer holds are bounded by the cap");
}

// A block is published whole. A thread that reads a block's address without the mutex (is_shadow_block, is_ended_block, and the
// length of a reference, which the wrapper answers from the layer's record) and finds the block live finds the block that was made
// there, with its fields, Dalvik words, contents and tail, never what the address held before; and a block that is still being
// made is no block yet.
// Two parts. The proof of the order is deterministic: the maker pauses inside the fake's GetByteArrayRegion, which the layer
// calls for the contents after the header's fields and before the publication, the reader looks at the block under
// construction while it is paused, and finds no block (neither live nor ended, length 0). A layer that publishes the record of the
// block before it has built it fails that look every time. The stress part is the stale reader: one thread makes a block, waits
// until the other has looked at it whole, ends it and has the runtime collect its array, so that the next block is made at the
// address of the one before; the other thread reads that address all the while, in the stage the first publishes: while a block
// is being made, a live block at the address can only be the one being made. (Fails by its checks when make_shadow publishes the
// record before the header's other fields and the contents.)
void check_a_header_is_published_whole_to_readers_without_the_mutex() {
    constexpr const char* kCase = "check_a_header_is_published_whole_to_readers_without_the_mutex";
    constexpr int kGenerations = 1500;
    // Both sizes take the same chunk of the arena, so that the freed block's address is the next block's. They are long: the layer
    // copies the contents again for the baseline after it has built the block, and a layer that published the block before that
    // copy is seen by the stale reader almost every time it makes a block.
    constexpr size_t kShort = 32769, kLong = 32770;
    const auto size_of = [](int generation) -> size_t { return generation % 2 != 0 ? kShort : kLong; };
    const auto fill_of = [](int generation) -> uint8_t { return generation % 2 != 0 ? 0xA1 : 0x5E; };
    Fixture f = make();
    Other reader(f);
    const auto length_of_block = [&](const void* block) {
        return fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(reader.f.env, const_cast<void*>(block));
    };
    std::atomic<int> stage{0};  // 2g - 1: block g is being made; 2g: it is made and the reader is to look at it; then the maker ends it
    std::atomic<int> acked{0};  // the last block the reader looked at whole
    std::atomic<const void*> first{nullptr};    // the address of the first block: the reader's stale reference
    std::atomic<const void*> current{nullptr};  // the block of the latest generation made
    std::atomic<bool> stop{false};
    // The pause inside the construction of generation g: the maker names the block under construction and waits for the reader.
    std::atomic<int> paused{0};                      // the generation whose block is under construction and waits for the look
    std::atomic<int> looked{0};                      // the generation the reader has looked at while it was paused
    std::atomic<const void*> candidate{nullptr};     // the block under construction
    std::atomic<bool> candidate_live{false}, candidate_ended{false};
    std::atomic<jsize> candidate_length{-1};
    std::thread looker([&] {
        // The block as it was made for the generation: the length word, the contents and the zeroes behind them.
        const auto whole = [&](const void* block, int generation) {
            const uint8_t* raw = static_cast<const uint8_t*>(block);
            uint32_t length_word;
            std::memcpy(&length_word, raw + 8, sizeof(length_word));
            require(length_word == size_of(generation), "a live block's length word is its own");
            for (size_t i = 0; i < size_of(generation); ++i) {
                require(raw[16 + i] == fill_of(generation), "a live block's contents are its own");
            }
            for (size_t i = 0; i < 16; ++i) require(raw[16 + size_of(generation) + i] == 0, "a live block's tail is zeroes");
        };
        int done = 0;
        unsigned looks = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const int pause = paused.load(std::memory_order_acquire);
            if (pause > looked.load(std::memory_order_relaxed)) {
                // The maker is inside the construction of a block and waits: look at it, without the mutex.
                const void* block = candidate.load(std::memory_order_acquire);
                candidate_live.store(is_shadow_block(block), std::memory_order_relaxed);
                candidate_ended.store(is_ended_block(block), std::memory_order_relaxed);
                candidate_length.store(length_of_block(block), std::memory_order_relaxed);
                looked.store(pause, std::memory_order_release);
                continue;
            }
            const int at = stage.load(std::memory_order_acquire);
            const void* address = first.load(std::memory_order_acquire);
            if (address == nullptr) {
                std::this_thread::yield();
                continue;
            }
            const bool live = is_shadow_block(address);
            (void)is_ended_block(address);
            const jint type = fn<jint (*)(Env*, jobject)>(*f.table, slot::GetObjectRefType)(reader.f.env, const_cast<void*>(address));
            require(type == 0 || type == kRefTypeLocal, "the kind of a reference to the address is a block's (local) or the null reference's (invalid)");
            const jsize length = length_of_block(address);
            require(length == 0 || length == jsize(kShort) || length == jsize(kLong),
                    "a length read without the mutex is the length of a block that was or is at the address, never a link");
            if (at % 2 != 0 && live) {
                // Block g is being made and the one before it has ended: the live block is g, and nothing of it is old.
                const int generation = (at + 1) / 2;
                require(length == jsize(size_of(generation)), "a live block's length is its own");
                whole(address, generation);
            } else if (at != 0 && at % 2 == 0 && at / 2 > done) {
                const int generation = at / 2;
                const void* block = current.load(std::memory_order_acquire);
                require(is_shadow_block(block) && length_of_block(block) == jsize(size_of(generation)), "the block made is live");
                whole(block, generation);
                done = generation;
                acked.store(generation, std::memory_order_release);
            }
            if ((++looks & 15u) == 0) std::this_thread::yield();
        }
    });
    int reused = 0;
    for (int generation = 1; generation <= kGenerations; ++generation) {
        stage.store(2 * generation - 1, std::memory_order_release);
        g_after_get = [&, generation] {  // the layer has copied the contents into the block under construction and has not published it
            const uint8_t* contents = static_cast<const uint8_t*>(g_last_get_buffer.load(std::memory_order_acquire));
            candidate.store(contents - 16, std::memory_order_release);  // the object starts 16 bytes before its contents
            paused.store(generation, std::memory_order_release);
            wait_for_counter(kCase, "looked", looked, generation, "the maker (paused inside the construction of a block; the reader looks at it)");
            require(!candidate_live.load() && !candidate_ended.load() && candidate_length.load() == 0,
                    "a block that is still being made is no block yet: neither live nor ended, and no length");
        };
        Made made = make_shadow_of(f, std::vector<uint8_t>(size_of(generation), fill_of(generation)));
        require(made.shadow == candidate.load(std::memory_order_acquire), "the block that was paused is the block that was made");
        require(is_shadow_block(made.shadow), "a block");
        if (generation == 1) {
            first.store(made.shadow, std::memory_order_release);
        } else if (made.shadow == first.load(std::memory_order_acquire)) {
            ++reused;
        }
        current.store(made.shadow, std::memory_order_release);
        stage.store(2 * generation, std::memory_order_release);
        wait_for_counter(kCase, "acked", acked, generation, "the maker (the reader looks at each block)");
        end_ref(f, made.shadow);
        collect();  // nothing holds the array: the next creation frees the ended block
    }
    stop.store(true, std::memory_order_release);
    looker.join();
    require(reused >= kGenerations * 9 / 10, "the freed block's address was the next block's: the reader's reference was stale");
    require(g_ref_type_asked_with_a_reference == 0,
            "GetObjectRefType of an address that no block started at went to the runtime as the null reference: it was not looked up again");
}


// ---- the mutex holder never waits for the collector

// A bounded wait for a condition another thread brings about (two seconds).
template <class P>
bool wait_until(P&& condition) {
    for (int i = 0; i < 2000; ++i) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}
bool region_is_open() {
    std::lock_guard<std::mutex> l(g_flip_mutex);
    return g_open_regions == 1;
}
bool flip_is_pending() {
    std::lock_guard<std::mutex> l(g_flip_mutex);
    return g_flip_running;
}
bool a_get_waits_for_the_flip() {
    std::lock_guard<std::mutex> l(g_flip_mutex);
    return g_gets_that_waited != 0;
}

// A thread makes the critical get (or the release) of a block's pointer while another thread has a runtime region open,
// waiting for the layer's mutex or not, and the collector begins a flip. In the runtime's rule the flip waits for the
// region, and the first thread's get waits for the flip; if that thread held the mutex, the other, which takes it inside its
// region, would never close the region. The model times the waits out, and the row counts the timeouts.
void run_the_flip_cycle(bool a_releases, bool b_synchronizes_a_block) {
    Fixture f = make();
    Made k = make_held(f, sample(100));   // the block thread A uses
    Made k2 = make_held(f, sample(100));  // the block thread B synchronizes inside its region
    Other b(f);
    jobject real = byte_array(sample(64));  // thread B's real array
    std::atomic<bool> collector_waited_out{false};
    std::thread b_thread, collector;
    const auto inside_the_runtime_get = [&] {  // thread A is in the runtime's first critical get: it holds no mutex
        b_thread = std::thread([&] {
            void* p = get_critical(b.f, real);  // B's region opens (the layer records the pointer)
            if (b_synchronizes_a_block) {
                void* q = get_critical(b.f, k2.shadow);  // and B synchronizes a block inside it: the mutex
                release_critical(b.f, k2.shadow, q, 0);
            }
            wait_until(a_get_waits_for_the_flip);  // B stays in its region until thread A's get waits for the flip
            release_critical(b.f, real, p, 0);
        });
        require(wait_until(region_is_open), "thread B opened its region");
        collector = std::thread([&] {
            collector_waited_out = !flip_begin_waits_for_regions();
            flip_end();
        });
        require(wait_until(flip_is_pending), "the collector's flip is pending");
    };
    void* p = nullptr;
    if (a_releases) {
        p = get_critical(f, k.shadow);
        raw_of(k)[3] = 0x5A;  // the application writes through the pointer: the release merges it into the array
        g_before_first_critical = inside_the_runtime_get;
        release_critical(f, k.shadow, p, 0);
    } else {
        g_before_first_critical = inside_the_runtime_get;
        p = get_critical(f, k.shadow);
    }
    if (b_thread.joinable()) b_thread.join();
    if (collector.joinable()) collector.join();
    require(g_flip_timeouts == 0 && !collector_waited_out,
            "neither the critical get nor the collector's flip waited for good (the mutex holder waited for the collector)");
    if (a_releases) {
        require(k.object->bytes[3] == 0x5A, "the release reached the array");
    } else {
        require(p == static_cast<void*>(raw_of(k)), "the pointer is the block's");
        release_critical(f, k.shadow, p, 0);
    }
    require(world.critical == 0 && t_regions == 0, "every region closed");
}
void check_a_block_critical_get_does_not_hold_the_mutex_while_it_waits_for_the_collector() {
    run_the_flip_cycle(false, false);
    run_the_flip_cycle(false, true);
}
void check_a_block_critical_release_does_not_hold_the_mutex_while_it_waits_for_the_collector() {
    run_the_flip_cycle(true, false);
    run_the_flip_cycle(true, true);
}

// The mutex is free while the thread's first critical get is in the runtime: another thread uses a block meanwhile.
void check_the_mutex_is_free_while_the_first_critical_get_is_in_the_runtime() {
    Fixture f = make();
    Made k = make_held(f, sample(100));
    Made other_block = make_held(f, sample(100));
    Other c(f);
    std::atomic<bool> c_done{false};
    std::thread c_thread;
    g_before_first_critical = [&] {
        c_thread = std::thread([&] {
            jbyte* e = get_elements(c.f, other_block.shadow);  // takes the mutex
            release_elements(c.f, other_block.shadow, e, 0);
            c_done = true;
        });
        require(wait_until([&] { return c_done.load(); }), "another thread used a block while the get was in the runtime");
    };
    void* p = get_critical(f, k.shadow);
    c_thread.join();
    require(p == static_cast<void*>(raw_of(k)), "and the get went on");
    release_critical(f, k.shadow, p, 0);
}

// ---- a critical get is never made under the mutex, wherever the thread's region is

// Opens, as the application, a region of the runtime on an object of the kind, and returns what closes it. The kinds differ
// in whether the runtime raises the thread's flip count: 0 a small array (it does), 1 an array of 16 KiB (large-object
// space: it does not), 2 a small array in the non-moving space (no), 3 a compressed string (its critical is a copy: no),
// 4 an uncompressed string (yes).
constexpr int kRegionKinds = 5;
std::function<void()> open_region(Fixture& f, int kind) {
    if (kind <= 2) {
        Obj* object = new Obj{kind == 1 ? std::vector<uint8_t>(16384, 1) : sample(64), true};
        object->non_moving = kind == 2;
        jobject array = new_local(object);
        void* pointer = get_critical(f, array);
        require(pointer != nullptr, "the application's region is open");
        return [&f, array, pointer] { release_critical(f, array, pointer, 0); };
    }
    Obj* object = new Obj{std::vector<uint8_t>{'a', 'b', 'c', 'd'}, false};
    object->is_string = true;
    object->compressed = kind == 3;
    jobject string = new_local(object);
    const jchar* chars = fn<const jchar* (*)(Env*, jstring, jboolean*)>(*f.table, slot::GetStringCritical)(f.env, string, nullptr);
    require(chars != nullptr, "the application's string region is open");
    return [&f, string, chars] { fn<void (*)(Env*, jstring, const jchar*)>(*f.table, slot::ReleaseStringCritical)(f.env, string, chars); };
}

// A block's critical pointer, and its commit and abort releases, inside a region of the application's own on each kind of
// object: the layer's gets are made before the mutex is taken whatever the runtime's count is (the fake fails any get
// under it), the application's bytes reach the array at the commit, and the abort gives the block the array's bytes.
void check_a_block_critical_pointer_inside_any_region_makes_its_gets_outside_the_mutex() {
    for (int kind = 0; kind < kRegionKinds; ++kind) {
        Fixture f = make();
        Made k = make_held(f, sample(100));
        raw_of(k)[3] ^= 0x5A;  // the application wrote through the pointer: the chunk is dirty
        const uint8_t written = raw_of(k)[3];
        auto close = open_region(f, kind);
        world.critical_gets = 0;
        void* p = get_critical(f, k.shadow);
        require(p == static_cast<void*>(raw_of(k)), "the block's pointer inside the region");
        require(world.critical_gets == 1 && k.object->bytes[3] == written, "one get, and the application's byte reached the array");
        raw_of(k)[4] ^= 0x33;
        release_critical(f, k.shadow, p, 0);  // a commit: dirty, merged in place
        require(k.object->bytes[4] == raw_of(k)[4], "the commit's merge reached the array");
        void* q = get_critical(f, k.shadow);
        const uint8_t before = raw_of(k)[5];
        raw_of(k)[5] ^= 0x77;
        release_critical(f, k.shadow, q, kReleaseAbort);  // the abort path
        require(raw_of(k)[5] == before && k.object->bytes[5] == before, "an abort gives the block the array's bytes");
        require(world.critical == 1, "only the application's region is open");
        close();
        require(world.critical == 0 && t_regions == 0 && t_flips == 0, "every region closed");
    }
}

// The cycle the runtime's rule makes with a get under the mutex. Thread H holds a region that does not raise the runtime's
// flip count and gets (or releases) a block's pointer, which the runtime treats as its outermost get; meanwhile thread X, in
// a region of its own that does raise the count, takes the mutex for a block's pair of its own, and the collector's flip
// waits for X. A mutex held by H across its wait for the flip would hold X, X's region, the flip and H in a cycle. The
// model times the waits out and the row counts the timeouts.
void run_the_foreign_region_cycle(int kind, bool h_releases) {
    Fixture f = make();
    Made k = make_held(f, sample(100));
    Made k2 = make_held(f, sample(50));
    Other x(f);
    jobject small = byte_array(sample(64));
    std::atomic<int> x_stage{0};
    std::atomic<bool> go{false};
    std::atomic<bool> flip_ok{false};
    std::thread x_thread, collector;
    raw_of(k)[3] ^= 0x5A;
    void* p = h_releases ? get_critical(f, k.shadow) : nullptr;
    raw_of(k)[4] ^= 0x33;
    auto close = open_region(f, kind);
    g_before_first_critical = [&] {  // H's get of the block's array, the runtime's outermost: no mutex may be held
        x_thread = std::thread([&] {
            void* region = get_critical(x.f, small);  // X's region (the flip count rises)
            x_stage = 1;
            wait_until([&] { return go.load(); });
            void* pair = get_critical(x.f, k2.shadow);  // a block's pair inside it: the mutex
            x_stage = 2;
            release_critical(x.f, k2.shadow, pair, 0);
            release_critical(x.f, small, region, 0);
            x_stage = 3;
        });
        require(wait_until([&] { return x_stage.load() >= 1; }), "X's region is open");
        collector = std::thread([&] {
            flip_ok = flip_begin_waits_for_regions();
            flip_end();
        });
        require(wait_until(flip_is_pending), "the collector's flip is pending");
        go = true;
    };
    if (h_releases) {
        release_critical(f, k.shadow, p, 0);
    } else {
        p = get_critical(f, k.shadow);
    }
    x_thread.join();
    collector.join();
    require(x_stage == 3 && flip_ok && g_flip_timeouts == 0,
            "X's pair, the flip and H's get all completed: H held no mutex while it waited for the flip");
    require(k.object->bytes[3] == raw_of(k)[3], "H's merge reached the array");
    if (!h_releases) {
        release_critical(f, k.shadow, p, 0);
    }
    close();
    require(world.critical == 0 && t_regions == 0 && t_flips == 0, "every region closed");
}
void check_a_block_get_in_a_region_that_does_not_raise_the_flip_count_does_not_hold_the_mutex() {
    for (int kind : {1, 2, 3}) run_the_foreign_region_cycle(kind, false);
}
void check_a_block_release_in_a_region_that_does_not_raise_the_flip_count_does_not_hold_the_mutex() {
    for (int kind : {1, 2, 3}) run_the_foreign_region_cycle(kind, true);
}

// The longest a critical region the layer opened stayed open is kept, from the get's return to the release's, and
// includes the wait for the mutex: another thread holds it for 30 ms between the two holds.
void check_the_longest_layer_opened_region_is_counted() {
    Fixture f = make();
    Made a = make_held(f, sample(100));
    require(counters().longest_region_us == 0, "none yet");
    raw_of(a)[3] ^= 0x5A;
    std::thread holder;
    std::atomic<bool> holds{false};
    g_before_first_critical = [&] {  // in the runtime's get, with no mutex held: another thread takes it for a while
        holder = std::thread([&] {
            g_ended_mutex.lock();
            holds = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            g_ended_mutex.unlock();
        });
        require(wait_until([&] { return holds.load(); }), "the other thread holds the layer's mutex");
    };
    jbyte* e = get_elements(f, a.shadow);  // a dirty synchronization: the pair waits for the mutex
    holder.join();
    release_elements(f, a.shadow, e, 0);
    require(counters().longest_region_us >= 20000 && counters().longest_region_us < 2000000,
            "the region stayed open while the pair waited for the mutex");
}

// The table of the runtime's own pointers has a lock of its own: a thread that has just opened a region records the
// pointer without the mutex, whoever holds it.
void check_the_table_of_the_runtimes_pointers_does_not_wait_for_the_mutex() {
    Fixture f = make();
    (void)make_held(f, sample(100));  // a block has existed: the pointers are recorded
    Other o(f);
    jobject real = byte_array(sample(64));
    g_ended_mutex.lock();  // another thread holds the layer's mutex
    std::atomic<bool> done{false};
    std::thread t([&] {
        void* p = get_critical(o.f, real);
        release_critical(o.f, real, p, 0);
        jbyte* e = get_elements(o.f, real);
        release_elements(o.f, real, e, 0);
        done = true;
    });
    const bool finished = wait_until([&] { return done.load(); });
    g_ended_mutex.unlock();
    t.join();
    require(finished, "a real array's pointers were recorded and forgotten without the layer's mutex");
}

// The sentinel's array is allocated outside the mutex (the fake fails an allocation under it): at the first end, and when
// a collection is seen at a creation. Where the allocation fails the end goes on, no exception is left, and a later
// creation makes the sentinel.
void check_the_sentinels_array_is_made_outside_the_mutex() {
    Fixture f = make();
    Made a = make_shadow_of(f, sample(100));
    require(world.new_array_calls == 0, "no sentinel before a block ends");
    end_ref(f, a.shadow);
    require(world.new_array_calls == 1 && is_ended_block(a.shadow), "the first end made the sentinel's array");
    collect();  // the sentinel (a weak reference to an array nothing holds) is cleared
    Made b = make_shadow_of(f, sample(100));
    require(world.new_array_calls == 2 && counters().blocks_collected == 1,
            "a collection seen at a creation renewed the sentinel (one array) and swept the ended block");
    end_ref(f, b.shadow);
    require(world.new_array_calls == 2, "a sentinel in place needs no array");
}
void check_a_failed_sentinel_allocation_is_survived() {
    Fixture f = make();
    world.new_array_fails = true;
    Made a = make_shadow_of(f, sample(100));
    end_ref(f, a.shadow);
    require(!world.exception_pending && is_ended_block(a.shadow) && world.new_array_calls == 1,
            "the end went on, and the allocation's exception is not left pending");
    world.new_array_fails = false;
    collect();
    (void)make_shadow_of(f, sample(100));  // no sentinel: the creation sees a collection, makes the array outside the mutex
    require(counters().blocks_collected == 1 && world.new_array_calls == 2 && !world.exception_pending,
            "the next creation made the sentinel and swept");
}

// ---- which calls make a critical get, and which write through regions

// A clean block's call never makes a critical get and never writes the array: it reads Java's bytes through region calls,
// in a call to Java, the elements and the regions, and at the end of the block.
void check_a_clean_blocks_calls_make_no_critical_get() {
    Fixture f = make();
    Made a = make_held(f, sample(3 * 4096));
    a.object->bytes[100] = 0x77;  // Java wrote a byte; the application wrote none
    world.critical_gets = 0;
    world.set_calls = 0;
    world.get_calls = 0;
    call_static_int_v(f, f.consume, a.shadow);
    require(raw_of(a)[100] == 0x77, "the call gave the block Java's byte");
    require(world.critical_gets == 0 && world.set_calls == 0 && world.get_calls > 0, "by region reads: no critical get, no write");
    a.object->bytes[200] = 0x78;
    jbyte* elements = get_elements(f, a.shadow);
    release_elements(f, a.shadow, elements, 0);
    require(raw_of(a)[200] == 0x78 && world.critical_gets == 0 && world.set_calls == 0, "the elements: no critical get");
    a.object->bytes[300] = 0x79;
    jbyte buffer[400] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 400, buffer);
    const jbyte value = 0x42;
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, a.shadow, 5000, 1, &value);
    require(static_cast<uint8_t>(buffer[300]) == 0x79 && a.object->bytes[5000] == 0x42, "the regions work");
    require(world.critical_gets == 0, "and make no critical get either");
    end_ref(f, a.shadow);
    require(is_ended_block(a.shadow) && world.critical_gets == 0, "the end of a clean block makes no critical get");
}

// An exception pending synchronizes nothing: a native call that returns with one is not followed by a pull, an ended block
// waits with the application's writes where they are and no runtime call is made for it; the next call without an
// exception makes the end through the one path that writes through regions (exact runs of the bytes the application
// wrote, never a gap byte), and makes no critical get for it.
void check_an_exception_pending_synchronizes_nothing_and_a_deferred_end_writes_exact_runs() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    const size_t size = 3 * 4096;
    Made a = make_held(f, sample(size));
    const std::vector<uint8_t> before = a.object->bytes;
    std::vector<std::pair<size_t, size_t>> runs;  // start, length
    for (size_t at = 3; at < size; at += 97) {
        if (at + 1 >= 250 && at < 262) continue;  // the run across 256 is below
        if (at + 1 >= 4090 && at < 4102) continue;
        runs.push_back({at, 1});
    }
    runs.push_back({250, 12});
    runs.push_back({4090, 12});
    std::sort(runs.begin(), runs.end());
    for (const auto& run : runs) {
        for (size_t i = run.first; i < run.first + run.second; ++i) raw_of(a)[i] ^= 0x55;
    }
    std::vector<size_t> gaps;  // the first byte of every gap between two runs
    for (size_t k = 0; k + 1 < runs.size(); ++k) {
        const size_t end = runs[k].first + runs[k].second;
        require(end < runs[k + 1].first, "the runs are apart");
        gaps.push_back(end);
    }
    world.exception_pending = true;
    world.critical_gets = 0;
    world.set_calls = 0;
    world.get_calls = 0;
    call_static_int_v(f, f.consume, a.shadow);
    require(world.critical_gets == 0 && world.set_calls == 0 && world.get_calls == 0, "a call with an exception pending synchronizes nothing");
    pop(f.env, nullptr);
    require(is_shadow_block(a.shadow) && world.critical_gets == 0 && world.set_calls == 0 && world.get_calls == 0 &&
                a.object->bytes == before,
            "the end waits: no runtime call, the array untouched");
    world.exception_pending = false;
    g_after_get = [&] {  // Java writes the first byte of every gap while the deferred end reads the dirty run
        for (size_t gap : gaps) a.object->bytes[gap] = 0xB2;
    };
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(is_ended_block(a.shadow), "the next call without an exception finished the end");
    require(world.critical_gets == 0, "the deferred end made no critical get");
    require(world.set_calls == int(runs.size()), "one region write for each run of the application's bytes");
    for (const auto& run : runs) {
        for (size_t i = run.first; i < run.first + run.second; ++i) {
            require(a.object->bytes[i] == uint8_t(before[i] ^ 0x55), "the application's byte is in the array");
        }
    }
    for (size_t gap : gaps) {
        require(a.object->bytes[gap] == 0xB2, "Java's write into the gap is in the array");
    }
    size_t others = 0;
    for (size_t i = 0; i < size; ++i) {
        bool in_run = false;
        for (const auto& run : runs) in_run = in_run || (i >= run.first && i < run.first + run.second);
        if (!in_run && std::find(gaps.begin(), gaps.end(), i) == gaps.end()) {
            require(a.object->bytes[i] == before[i], "no other byte of the array changed");
            ++others;
        }
    }
    require(others > 0, "the rows' bytes include untouched ones");
}

// The array of a block is collected between the two holds of a dirty synchronization: the first hold decided "dirty", the
// first critical get runs with no lock held (the hook acts there), and the block is freed before the second hold. The
// second hold revalidates: it writes nothing into a freed block's array, closes the pair, and the layer's state is whole.
// (In the runtime a local reference keeps the array alive, so the row has the fake's collector clear the weak reference
// under it: the layer must not depend on that.)
void check_a_block_freed_between_the_two_holds_is_not_merged() {
    Fixture f = make();
    Made gone = make_shadow_of(f, sample(100));
    end_ref(f, gone.shadow);  // ended: the weak reference is all that holds the block's array
    require(is_ended_block(gone.shadow), "the block ended");
    raw_of(gone)[3] ^= 0x55;  // the application writes through the pointer it kept: the chunk is dirty
    const std::vector<uint8_t> array_before = gone.object->bytes;
    Other other(f);
    g_before_first_critical = [&] {  // in the runtime's first critical get: no lock held
        g_collect_ignores_locals = true;
        collect();
        g_collect_ignores_locals = false;
        std::thread([&] { (void)make_shadow_of(other.f, sample(10)); }).join();  // a creation sweeps: the block is freed
    };
    world.critical_gets = 0;
    jbyte buffer[16] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, gone.shadow, 0, 16, buffer);
    require(world.critical_gets == 1 && counters().blocks_collected == 1, "the get was made, and the block was freed meanwhile");
    require(gone.object->bytes == array_before, "nothing was written into the array of the freed block");
    require(world.critical == 0 && t_regions == 0 && !world.exception_pending, "the pair was closed");
}

// ---- a merge writes back exactly the bytes the application wrote

// A Java thread's write to a byte the application did not write, landing while the merge stores the application's byte into
// the live array, is neither undone in the array nor lost to the block: the merge stores that one byte and no other. The
// write is placed by the page-protection handler at the layer's first store into the array.
void check_a_java_write_during_a_merge_is_kept() {
    Fixture f = make();
    const size_t size = 65536;
    Made a = make_held(f, sample(size));
    uint8_t* array = a.object->bytes.data();
    uint8_t* page = page_inside(array, size);
    const size_t at = size_t(page + 20 - array);
    raw_of(a)[at] = uint8_t(array[at] ^ 0x55);  // the application wrote this byte: the chunk is dirty
    const uint8_t written = raw_of(a)[at];
    arm_watch(page, page + 21, 0xB2);           // Java writes the next byte meanwhile
    jbyte* elements = get_elements(f, a.shadow);  // a synchronization point: the dirty chunk is merged
    const int hits = disarm_watch();
    require(hits == 1, "the merge stored into the array where Java's write landed");
    require(array[at] == written, "the application's byte reached the array");
    require(array[at + 1] == 0xB2, "and Java's byte, which the merge did not read as the application's, is still there");
    release_elements(f, a.shadow, elements, 0);
    require(array[at + 1] == 0xB2 && raw_of(a)[at + 1] == 0xB2 && raw_of(a)[at] == written,
            "the next point gave the block Java's byte, and the application's stayed");
}

// Scattered bytes of the application, in runs of one byte and across the edges of a scan block (256) and of a chunk, and a
// Java write into the first byte of every gap, all placed by the page-protection handler at the merge's first store into
// the array: one critical pair and no region write, every gap byte keeps Java's value, and the block has them at the next
// point.
void check_scattered_application_bytes_keep_a_java_write_in_every_gap() {
    Fixture f = make();
    const size_t size = 65536;
    Made a = make_held(f, sample(size));
    uint8_t* array = a.object->bytes.data();
    const std::vector<uint8_t> before = a.object->bytes;
    std::vector<std::pair<size_t, size_t>> runs;  // start, length
    for (size_t at = 3; at < size; at += 97) {
        if (at + 1 >= 250 && at < 262) continue;  // the run across 256 is below
        if (at + 1 >= 4090 && at < 4102) continue;
        runs.push_back({at, 1});
    }
    runs.push_back({250, 12});
    runs.push_back({4090, 12});
    std::sort(runs.begin(), runs.end());
    for (const auto& run : runs) {
        for (size_t i = run.first; i < run.first + run.second; ++i) raw_of(a)[i] ^= 0x55;
    }
    std::vector<size_t> gaps;  // the first byte of every gap between two runs
    for (size_t k = 0; k + 1 < runs.size(); ++k) {
        const size_t end = runs[k].first + runs[k].second;
        require(end < runs[k + 1].first, "the runs are apart");
        gaps.push_back(end);
    }
    std::vector<uint8_t*> targets;
    for (size_t gap : gaps) targets.push_back(array + gap);
    uint8_t* page = page_inside(array, size);
    arm_watch(page, targets[0], 0xB2, targets.data() + 1, targets.size() - 1);
    world.set_calls = 0;
    world.critical_gets = 0;
    call_static_int_v(f, f.consume, a.shadow);
    const int hits = disarm_watch();
    require(hits == 1, "Java wrote the gaps at the merge's first store into the array");
    require(world.critical_gets == 1 && world.set_calls == 0, "one critical pair and no region write");
    for (const auto& run : runs) {
        for (size_t i = run.first; i < run.first + run.second; ++i) {
            require(a.object->bytes[i] == uint8_t(before[i] ^ 0x55), "the application's byte is in the array");
        }
    }
    for (size_t gap : gaps) {
        require(a.object->bytes[gap] == 0xB2, "Java's write into the gap is in the array");
    }
    size_t others = 0;
    for (size_t i = 0; i < size; ++i) {
        bool in_run = false;
        for (const auto& run : runs) in_run = in_run || (i >= run.first && i < run.first + run.second);
        if (!in_run && std::find(gaps.begin(), gaps.end(), i) == gaps.end()) {
            require(a.object->bytes[i] == before[i], "no other byte of the array changed");
            ++others;
        }
    }
    require(others > 0, "the rows' bytes include untouched ones");
    std::vector<jbyte> whole(size);
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, jsize(size), whole.data());  // a point: the block takes Java's bytes
    for (size_t gap : gaps) {
        require(raw_of(a)[gap] == 0xB2, "the block has Java's byte of the gap");
    }
    for (const auto& run : runs) {
        require(raw_of(a)[run.first] == uint8_t(before[run.first] ^ 0x55), "and kept the application's");
    }
}

// ---- the table of the runtime's own pointers, when it is full

// The table records 1024 pointers; the next one is not recorded (a Release of it through a block's reference is then taken
// for a buffer of the application's, as before), and one line says so, once.
void check_a_full_table_of_the_runtimes_pointers_logs_one_line() {
    Fixture f = make();
    (void)make_held(f, sample(8));  // a block has existed: the pointers are recorded
    std::vector<jobject> arrays;
    std::vector<void*> pointers;
    for (int i = 0; i < 1024; ++i) {
        arrays.push_back(byte_array(sample(8)));
        pointers.push_back(get_critical(f, arrays.back()));
    }
    require(log_count("table of the runtime's own pointers") == 0, "nothing is logged while the table has room");
    jobject one_more = byte_array(sample(8));
    void* not_recorded = get_critical(f, one_more);
    require(not_recorded != nullptr && log_count("table of the runtime's own pointers") == 1, "the first pointer past the table is logged");
    jobject another = byte_array(sample(8));
    void* also_not = get_critical(f, another);
    require(also_not != nullptr && log_count("table of the runtime's own pointers") == 1, "and the second is not");
    release_critical(f, another, also_not, 0);
    release_critical(f, one_more, not_recorded, 0);
    for (int i = 0; i < 1024; ++i) release_critical(f, arrays[size_t(i)], pointers[size_t(i)], 0);
    require(world.critical == 0 && t_regions == 0, "every region closed");
}

// ---- little stack

// Any wrapper can be called on a thread with a small stack (an audio thread, a thread of a library): a pull and a merge read
// the array into one static buffer, not onto the stack. The thread has exactly 16 KiB (a stack of our own, with a guard
// page below), the size of the old pull's buffer alone.
void check_the_synchronizations_need_little_stack() {
    Fixture f = make();
    const size_t size = 256 * 4096;
    Made a = make_held(f, sample(size));
    raw_of(a)[5 * 4096 + 1] ^= 0x55;  // a dirty chunk: a merge of one chunk
    for (size_t i = 40 * 4096; i < 48 * 4096; ++i) raw_of(a)[i] ^= 0x55;  // eight dirty chunks: a merge longer than the buffer
    a.object->bytes[100000] ^= 0x33;  // Java's change, taken by a pull
    Other o(f);
    struct Job {
        Other* thread;
        Made* made;
        std::vector<jbyte> copy;
        size_t size;
    } job{&o, &a, std::vector<jbyte>(size), size};
    const size_t page = size_t(getpagesize());
    require(page == 16384, "the host page is 16 KiB");
    uint8_t* stack = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    require(stack != MAP_FAILED && mprotect(stack, page, PROT_NONE) == 0, "a stack and its guard page");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    require(pthread_attr_setstack(&attributes, stack + page, page) == 0, "a thread with a 16 KiB stack");
    pthread_t thread;
    require(pthread_create(&thread, &attributes, [](void* argument) -> void* {
        Job* work = static_cast<Job*>(argument);
        fn<GetRegion>(*work->thread->f.table, slot::GetByteArrayRegion)(work->thread->f.env, work->made->shadow, 0,
                                                                         jsize(work->size), work->copy.data());
        return nullptr;
    }, &job) == 0, "the thread starts");
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
    munmap(stack, 2 * page);
    require(a.object->bytes[5 * 4096 + 1] == uint8_t(sample(size)[5 * 4096 + 1] ^ 0x55) &&
                a.object->bytes[47 * 4096 + 7] == uint8_t(sample(size)[47 * 4096 + 7] ^ 0x55),
            "the merges reached the array");
    require(raw_of(a)[100000] == uint8_t(sample(size)[100000] ^ 0x33) && uint8_t(job.copy[100000]) == raw_of(a)[100000],
            "the pull gave the block and the reader Java's byte");
}

// ---- stores that touch only the bytes the rule chose

// ---- one load of every Java byte a merge takes

// A Java thread's write that lands while the layer takes Java's bytes into the block must not leave the block different
// from the baseline for a byte the application never wrote: the next synchronization would take that for the application's
// write and put the old value back into the array. The page-protection handler places the write at the layer's first
// store into the block's page, between the layer's load of Java's byte and any second load of it (a second load is what
// the fast path of take_java_words made; the product is built with optimization, as these tests are).
struct WordRace {
    Made a;
    uint8_t* block;
    uint8_t* array;
    uint8_t* page;
    size_t at;  // the offset of a word in the page, on the word grid and 20 bytes or more into its chunk
};
WordRace word_race(Fixture& f, size_t size = 65536) {
    WordRace race{make_held(f, std::vector<uint8_t>(size, 0)), nullptr, nullptr, nullptr, 0};
    race.block = raw_of(race.a);
    race.array = race.a.object->bytes.data();
    race.page = page_inside(race.block, size);
    race.at = size_t(race.page - race.block) + 20;
    while (race.at % 4096 < 20 || race.at % 4 != 0) race.at += (race.at % 4 != 0) ? 1 : 4;
    require(race.block + race.at + 4 <= race.page + size_t(getpagesize()), "the word lies in the watched page");
    return race;
}
// The array and the block after a read-only release (the application never wrote the byte at `at + which`): Java's racing
// write is in the array and, a point later, in the block.
void require_java_write_kept(Fixture& f, WordRace& race, size_t which, uint8_t java_value, const char* what) {
    jbyte* p = get_elements(f, race.a.shadow);
    release_elements(f, race.a.shadow, p, 0);
    require(race.array[race.at + which] == java_value, what);
    require(race.block[race.at + which] == java_value, "and the block took it at the next point");
    p = get_elements(f, race.a.shadow);
    release_elements(f, race.a.shadow, p, 0);
    require(race.array[race.at + which] == java_value && race.block[race.at + which] == java_value, "and they stay alike");
}

// The fast path: Java changed all four bytes of a word, the application none, and a Java write to the first of them lands
// at the store of that byte into the block. The merge of the dirty chunk goes through the live array's critical pair.
void check_a_java_write_during_the_word_path_of_a_merge_is_not_reverted() {
    Fixture f = make();
    WordRace r = word_race(f);
    r.block[r.at - 16] = 0x41;  // the application wrote a byte of the chunk: it is merged in the live array
    for (size_t i = 0; i < 4; ++i) r.array[r.at + i] = 0x52;
    arm_watch(r.page, r.array + r.at, 0x63);
    jbyte* p = get_elements(f, r.a.shadow);
    const int hits = disarm_watch();
    require(hits == 1, "Java's write landed at the layer's store of the word's first byte");
    require(r.array[r.at] == 0x63 && r.array[r.at - 16] == 0x41, "the point left Java's write, and pushed the application's");
    release_elements(f, r.a.shadow, p, 0);
    require(r.array[r.at] == 0x63, "a read-only release cannot revert a Java-only write");
    require_java_write_kept(f, r, 0, 0x63, "and neither can a later point");
    require(r.array[r.at + 1] == 0x52 && r.block[r.at + 1] == 0x52, "the word's other bytes are Java's");
}

// The byte path: Java changed three bytes of a word, so each is decided on its own; a Java write to the second lands at the
// store of the first.
void check_a_java_write_during_the_byte_path_of_a_merge_is_not_reverted() {
    Fixture f = make();
    WordRace r = word_race(f);
    r.block[r.at - 16] = 0x41;
    for (size_t i = 0; i < 3; ++i) r.array[r.at + i] = 0x52;
    arm_watch(r.page, r.array + r.at + 1, 0x63);
    jbyte* p = get_elements(f, r.a.shadow);
    const int hits = disarm_watch();
    require(hits == 1, "Java's write landed at the layer's store of the word's first byte");
    release_elements(f, r.a.shadow, p, 0);
    require(r.array[r.at + 1] == 0x63, "a read-only release cannot revert a Java-only write");
    require_java_write_kept(f, r, 1, 0x63, "and neither can a later point");
    require(r.block[r.at] == 0x52 && r.block[r.at + 2] == 0x52 && r.array[r.at + 3] == 0, "the other bytes are as Java left them");
}

// The critical merge (a block's critical pointer, then its release), the same race.
void check_a_java_write_during_a_critical_merge_is_not_reverted() {
    Fixture f = make();
    WordRace r = word_race(f);
    r.block[r.at - 16] = 0x41;
    for (size_t i = 0; i < 4; ++i) r.array[r.at + i] = 0x52;
    arm_watch(r.page, r.array + r.at, 0x63);
    void* p = get_critical(f, r.a.shadow);
    const int hits = disarm_watch();
    require(hits == 1, "Java's write landed at the layer's store of the word's first byte");
    release_critical(f, r.a.shadow, p, 0);  // the application wrote nothing at `at`
    require(r.array[r.at] == 0x63, "a release cannot revert a Java-only write");
    require_java_write_kept(f, r, 0, 0x63, "and neither can a later point");
}

// The pull of a clean chunk inside the same critical pair (the block has a dirty chunk elsewhere, so the pair is made), the
// same race.
void check_a_java_write_during_the_pull_in_place_is_not_reverted() {
    Fixture f = make();
    WordRace r = word_race(f);
    const size_t far_chunk = (r.at / 4096 == 0) ? 3 * 4096 : 0;
    r.block[far_chunk + 1] = 0x41;  // a dirty chunk elsewhere: the pair is made; the chunk of the word is clean
    for (size_t i = 0; i < 4; ++i) r.array[r.at + i] = 0x52;
    world.critical_gets = 0;
    arm_watch(r.page, r.array + r.at, 0x63);
    jbyte* p = get_elements(f, r.a.shadow);
    const int hits = disarm_watch();
    require(world.critical_gets == 1 && hits == 1, "the pair pulled the clean chunk, and Java's write landed at its store");
    release_elements(f, r.a.shadow, p, 0);
    require(r.array[r.at] == 0x63, "a read-only release cannot revert a Java-only write");
    require_java_write_kept(f, r, 0, 0x63, "and neither can a later point");
}

// A pull through a region copy of a clean block: the copy is a snapshot, block and baseline take the copy's byte, and Java's
// write after the copy is taken at the next point.
void check_a_java_write_during_a_region_pull_is_taken_at_the_next_point() {
    Fixture f = make();
    WordRace r = word_race(f);
    for (size_t i = 0; i < 4; ++i) r.array[r.at + i] = 0x52;
    world.critical_gets = 0;
    arm_watch(r.page, r.array + r.at, 0x63);
    std::vector<jbyte> copy(65536);
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, r.a.shadow, 0, 65536, copy.data());
    const int hits = disarm_watch();
    require(hits == 1 && world.critical_gets == 0, "a region pull, and Java's write landed at its store");
    require(r.array[r.at] == 0x63, "the array keeps Java's write");
    require_java_write_kept(f, r, 0, 0x63, "and no later point reverts it");
}

// The deferred end of a block (the one merge through region calls): its copy is a snapshot too.
void check_a_java_write_during_a_deferred_ends_merge_is_kept() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    WordRace r = word_race(f);
    r.block[r.at - 16] = 0x41;
    for (size_t i = 0; i < 4; ++i) r.array[r.at + i] = 0x52;
    world.exception_pending = true;
    pop(f.env, nullptr);  // the end waits for a call without an exception
    world.exception_pending = false;
    arm_watch(r.page, r.array + r.at, 0x63);
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);  // the end is finished here
    const int hits = disarm_watch();
    require(hits == 1 && is_ended_block(r.a.shadow), "the end took Java's word, and Java's write landed at its store");
    require(r.array[r.at] == 0x63 && r.array[r.at - 16] == 0x41, "Java's write is kept, and the application's byte is in the array");
    jbyte* p = get_elements(f, r.a.shadow);  // the kept reference of the ended block: a point
    release_elements(f, r.a.shadow, p, 0);
    require(r.array[r.at] == 0x63 && r.block[r.at] == 0x63, "and the next point gave the block Java's byte");
}

// A span that a pull found equal to the baseline is skipped; what Java writes afterwards is taken at the next point.
void check_a_java_write_after_a_skipped_span_is_taken_at_the_next_point() {
    Fixture f = make();
    Made a = make_held(f, std::vector<uint8_t>(4096, 0x11));
    g_after_get = [&] { a.object->bytes[1000] = 0x77; };  // after the copy, which was equal to the baseline
    jbyte buffer[4096];
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 4096, buffer);
    require(raw_of(a)[1000] == 0x11 && a.object->bytes[1000] == 0x77, "the point did not see the write");
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, 4096, buffer);
    require(raw_of(a)[1000] == 0x77 && static_cast<uint8_t>(buffer[1000]) == 0x77, "the next point took it");
}

// ---- the application's last write is never lost to the merge's own store

// The merge stores the application's byte W into the array and the pull that follows reads the array; were it to take W for a
// Java change against the old baseline K, an application thread that wrote K back in between would have W stored over its
// last write, in its own memory and in the array, on a byte Java never wrote. The merge makes W the baseline at its decision,
// so the block (K) differs from it and K goes to the array at the next point. The page-protection handler places the
// application's write of K at the merge's store of W into the live array, between the load of the block byte (the one load
// every decision rests on) and the baseline write: a second load of the block byte there would see K and write it to the
// baseline instead.
void check_the_applications_write_back_to_the_old_value_during_a_critical_merge_is_kept() {
    for (size_t bytes : {size_t(1), size_t(4)}) {  // the byte path, and the word path
        Fixture f = make();
        const size_t size = 65536;
        Made a = make_held(f, std::vector<uint8_t>(size, 0));
        uint8_t* block = raw_of(a);
        uint8_t* array = a.object->bytes.data();
        uint8_t* apage = page_inside(array, size);
        size_t at = size_t(apage - array) + 100;
        at -= at % 4;
        for (size_t i = 0; i < bytes; ++i) block[at + i] = 0x41;  // the application's first write; the last point's value is 0
        arm_watch(apage, block + at, 0x00);  // at the merge's store into the array the application writes 0 back
        jbyte* p = get_elements(f, a.shadow);
        const int hits = disarm_watch();
        require(hits == 1, "the application's write landed at the merge's first store into the array");
        require(array[at] == 0x41 && block[at] == 0x00, "the array has the merge's byte, the block the application's last write");
        release_elements(f, a.shadow, p, 0);  // a point: block 0 != baseline 0x41, so 0 goes to the array
        require(block[at] == 0x00 && array[at] == 0x00, "the application's last write reached the array");
        p = get_elements(f, a.shadow);
        release_elements(f, a.shadow, p, 0);
        require(block[at] == 0x00 && array[at] == 0x00, "and the next point keeps it");
        for (size_t i = 1; i < bytes; ++i) require(block[at + i] == 0x41 && array[at + i] == 0x41, "the other bytes are the application's");
    }
}

// The same through the one region merge (a deferred end): the second write lands after the run's region write.
void check_the_applications_write_back_to_the_old_value_during_a_deferred_ends_merge_is_kept() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    Made a = make_held(f, std::vector<uint8_t>(65536, 0));
    uint8_t* block = raw_of(a);
    const size_t at = 5000;
    block[at] = 0x41;
    world.exception_pending = true;
    pop(f.env, nullptr);  // the end waits for a call without an exception
    world.exception_pending = false;
    g_after_set = [&] { block[at] = 0x00; };  // after the run's region write: the application writes 0 back
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);  // the end is finished here
    require(is_ended_block(a.shadow) && a.object->bytes[at] == 0x41 && block[at] == 0x00, "the end pushed the byte, and the block holds the last write");
    jbyte* p = get_elements(f, a.shadow);
    release_elements(f, a.shadow, p, 0);
    require(block[at] == 0x00 && a.object->bytes[at] == 0x00, "the next point pushed the application's last write");
}

// A region write the runtime refuses leaves the baseline as it was: the bytes are still the application's at the next point.
// Two runs; the first write is refused, the second accepted.
void check_a_refused_region_write_of_a_deferred_end_leaves_the_bytes_to_the_next_point() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    Made a = make_held(f, std::vector<uint8_t>(65536, 0));
    uint8_t* block = raw_of(a);
    a.object->bytes[2000] = 0x66;  // Java wrote a byte of the same chunk: it is taken at the end
    block[1000] = 0x41;            // run 1
    block[3000] = 0x42;            // run 2
    world.exception_pending = true;
    pop(f.env, nullptr);
    world.exception_pending = false;
    world.set_fails = 1;
    world.set_calls = 0;
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(is_ended_block(a.shadow) && world.set_calls == 2 && !world.exception_pending, "both writes were tried, no exception is left");
    require(a.object->bytes[1000] == 0 && a.object->bytes[3000] == 0x42, "the refused run is not in the array, the other is");
    require(block[2000] == 0x66, "Java's byte was taken into the block");
    world.set_calls = 0;
    jbyte* p = get_elements(f, a.shadow);  // the next point
    release_elements(f, a.shadow, p, 0);
    require(a.object->bytes[1000] == 0x41 && a.object->bytes[3000] == 0x42 && a.object->bytes[2000] == 0x66,
            "the next point pushed the refused bytes, and nothing else is lost");
    require(block[1000] == 0x41 && block[3000] == 0x42 && world.set_calls == 0, "the pair, not a region write, did it");
}

// A pull stores the byte Java changed and no other byte of its word: another thread's raw write to the next byte, landing
// at that store, survives, and reaches the array at the next point.
void check_a_pull_stores_only_the_byte_java_changed() {
    Fixture f = make();
    const size_t size = 65536;
    Made a = make_held(f, std::vector<uint8_t>(size, 0x11));
    uint8_t* data = raw_of(a);
    uint8_t* page = page_inside(data, size);
    uint8_t* changed = page + 33;  // Java's byte, in the middle of a word whose other bytes nobody changed
    uint8_t* neighbour = page + 34;
    a.object->bytes[size_t(changed - data)] = 0xA1;
    arm_watch(page, neighbour, 0x77);
    std::vector<jbyte> copy(size);
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, jsize(size), copy.data());
    const int hits = disarm_watch();
    require(hits == 1, "the store of Java's byte was where the other thread's write landed");
    require(*changed == 0xA1 && *neighbour == 0x77, "the block has Java's byte and the other thread's write");
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, a.shadow, 0, jsize(size), copy.data());
    require(a.object->bytes[size_t(neighbour - data)] == 0x77 && a.object->bytes[size_t(changed - data)] == 0xA1,
            "and the next point took the write to the array");
}

// The merge in the live array of a critical get stores the byte the application wrote and no other byte of its word: a Java
// thread's write to the next byte, landing at that store, survives.
void check_the_critical_merge_stores_only_the_byte_the_application_wrote() {
    Fixture f = make();
    const size_t size = 524288;
    Made a = make_held(f, std::vector<uint8_t>(size, 0x11));
    uint8_t* data = raw_of(a);
    uint8_t* array = a.object->bytes.data();
    uint8_t* page = page_inside(array, size);
    const size_t at = size_t(page + 33 - array);
    data[at] = 0x5A;  // the application writes through the pointer
    arm_watch(page, page + 34, 0x99);  // and Java writes the next byte of the array, as the merge stores
    void* p = get_critical(f, a.shadow);
    const int hits = disarm_watch();
    require(hits == 1, "the merge stored into the array where the other write landed");
    require(array[at] == 0x5A && array[at + 1] == 0x99, "the array has the application's byte and Java's write");
    release_critical(f, a.shadow, p, 0);
    require(data[at + 1] == 0x99, "and the block has Java's write after the release");
}

void check_install_rules() {
    reset_world();
    Table table;
    g_writable_ok = false;
    require(!install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)),
            "a table that cannot be written is left alone");
    require(table.slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength), "unchanged");
    g_writable_ok = true;
    g_read_only_ok = false;
    require(install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)),
            "a table that cannot be made read-only again is still installed (and reported)");
    g_read_only_ok = true;
    require(!install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)), "installing twice is refused");
    require(table.slots[slot::GetArrayLength] != reinterpret_cast<void*>(&f_GetArrayLength), "wrapped");
    require(table.slots[slot::MonitorEnter] != reinterpret_cast<void*>(&f_MonitorEnter) &&
                table.slots[slot::MonitorExit] != reinterpret_cast<void*>(&f_MonitorExit),
            "MonitorEnter and MonitorExit are wrapped");
    require(table.slots[slot::NewStringUTF] == reinterpret_cast<void*>(&trap) && table.slots[slot::FindClass] == reinterpret_cast<void*>(&f_FindClass) &&
                table.slots[slot::NewByteArray] == reinterpret_cast<void*>(&f_NewByteArray),
            "the functions that only made references the layer tracked are no longer wrapped");
    // VM wrappers ask the platform who is calling: that is the "a library may have loaded" notification.
    void* vm_table[kVmSlotCount] = {};
    struct VmFns {
        static jint get_env(Vm*, void**, jint) { return 0; }
    };
    for (void*& s : vm_table) s = reinterpret_cast<void*>(&VmFns::get_env);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    const int before = world.app_checks;
    Vm vm{vm_table};
    void* out;
    require(reinterpret_cast<jint (*)(Vm*, void**, jint)>(vm_table[vmslot::GetEnv])(&vm, &out, 0x10006) == 0, "GetEnv passes through");
    require(world.app_checks == before + 1, "and asks who the caller is");
}

void check_no_shadows_when_disabled() {
    Fixture f = make(/*shadows=*/false);
    const bool nothing = f.table->slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength);
    require(nothing, "with no region the table is left as it was");
}

}  // namespace

int main(int argc, char** argv) {
    // The whole binary runs in a few seconds; a thread of a case that never returns (a join, a lock) fails here, with a line,
    // long before the test's own timeout.
    uml::testing::Watchdog watchdog("the old-application JNI test", std::chrono::minutes(5));
    require(argc == 2, "usage: oldapp_jni_tests <jni-slots.tsv>");
    check_slot_constants(argv[1]);
    check_shadow_layout_and_use();
    check_identity_and_writes();
    check_platform_caller_sees_nothing();
    check_shadow_passed_back();
    check_conversion_for_any_caller();
    check_monitor();
    check_unknown_and_long_signatures();
    check_fields_and_other_results();
    check_block_follows_a_call();
    check_block_follows_later_writes();
    check_abort_gives_the_arrays_bytes_back();
    check_exception_leaves_the_block();
    check_raw_write_reaches_a_call();
    check_raw_write_and_java_write_in_one_call();
    check_both_sides_changed_one_chunk_both_survive();
    check_the_merge_by_words_is_byte_exact();
    check_synchronization_is_per_chunk();
    check_a_clean_block_makes_no_array_write();
    check_set_region_keeps_other_raw_writes();
    check_an_exact_comparison_keeps_every_write();
    check_a_dirty_block_is_pushed_in_place_and_a_clean_one_read_through_regions();
    check_committing_release_keeps_java_bytes_in_clean_chunks();
    check_nested_critical_release_keeps_the_other_pointers_bytes();
    check_a_receiver_is_synchronized();
    check_one_block_per_array();
    check_a_stray_length_write_is_harmless();
    check_the_flag_of_the_elements_is_truthful();
    check_the_flag_of_the_critical_pointer_is_truthful();
    check_no_synchronization_with_an_exception_pending();
    check_critical_pair_inside_a_region();
    check_critical_entries_do_not_drain();
    check_limits_without_recycling();
    check_diagnosis_lines();
    check_thread_end();
    check_pop_local_frame();
    check_a_native_invocation_ends_its_blocks();
    check_nested_invocations_end_their_own_blocks();
    check_a_native_method_that_returns_with_an_exception_pending();
    check_six_hundred_invocations_decline_nothing();
    check_more_than_sixty_four_nested_invocations();
    check_no_thread_state_no_record();
    check_an_abandoned_invocation_is_trimmed();
    check_an_unmatched_push_local_frame_ends_with_the_invocation();
    check_the_thunks_run_out();
    check_a_method_without_a_thunk_ends_by_the_other_ends();
    check_which_functions_get_a_thunk();
    check_register_natives_registers_thunks();
    check_detach_ends_the_references_of_a_thread();
    check_push_and_pop_local_frame_end_their_entries();
    check_frames_beyond_the_records();
    check_delete_local_ref_keeps_the_frames_right();
    check_the_thunk_diagnosis_lines();
    check_a_kept_pointer_keeps_its_bytes();
    check_a_collection_frees_ended_blocks();
    check_a_sweep_takes_thirty_two_blocks_at_a_creation();
    check_the_end_is_a_synchronization_point();
    check_the_same_array_again_gets_its_block();
    check_an_exception_pending_defers_a_frame_end();
    check_thread_end_and_detach_end_blocks();
    check_an_arena_full_of_ended_blocks();
    check_the_full_sweep_does_not_need_the_sentinel();
    check_the_cap_of_ended_blocks();
    check_arrays_that_java_keeps_are_obtained_again_and_again();
    check_arrays_with_equal_hashes_get_their_own_blocks();
    check_the_newest_ended_block_of_an_array_is_found();
    check_no_identity_hash_declines_the_result();
    check_a_failed_hash_call_makes_no_second_block();
    check_no_weak_reference_keeps_the_block_strong();
    check_an_ended_block_does_not_see_javas_later_writes();
    check_a_pointer_never_released_keeps_its_array();
    check_a_release_goes_to_the_block_its_pointer_lies_in();
    check_the_reference_of_an_ended_block_keeps_working();
    check_a_merge_is_byte_exact_across_an_end();
    check_declined_results_do_not_repeat_a_full_sweep();
    check_an_end_and_a_second_thread_make_one_block();
    check_draining_the_ends_and_a_second_thread_make_one_block();
    check_the_class_lookup_may_enter_the_layer();
    check_a_kept_reference_on_a_second_thread_survives_the_owners_end();
    check_a_kept_reference_on_a_thread_without_frames_makes_no_entry();
    check_a_thread_at_its_limit_can_use_a_kept_reference();
    check_no_wrapper_hands_the_runtime_the_blocks_own_reference();
    check_a_callback_that_ends_the_block_leaves_the_call_a_live_reference();
    check_the_owners_merge_is_not_undone_by_another_threads_write();
    check_another_threads_synchronization_does_not_undo_the_owners_pull();
    check_an_end_is_serialized_with_a_second_threads_access();
    check_a_critical_pointer_of_the_runtime_released_through_a_block_reaches_the_runtime();
    check_an_elements_pointer_of_the_runtime_released_through_a_block_reaches_the_runtime();
    check_a_runtime_elements_pointer_got_before_the_first_block_is_released_by_the_runtime();
    check_a_runtime_critical_pointer_got_before_the_first_block_closes_its_region_at_the_release();
    check_the_record_of_a_runtime_pointer_ends_with_its_release();
    check_two_threads_getting_and_releasing_pointers_race_on_no_count();
    check_a_nested_native_method_cannot_delete_its_callers_reference();
    check_monitor_exit_with_an_exception_pending();
    check_a_pull_does_not_overwrite_a_raw_write_to_another_byte();
    check_a_merge_does_not_overwrite_a_raw_write_to_a_third_byte();
    check_a_byte_both_sides_wrote_keeps_the_applications_at_a_racing_pull();
    check_the_synchronization_is_byte_exact_for_random_writes();
    check_a_collection_the_step_sweep_saw_is_swept_in_full();
    check_a_kept_reference_gives_a_critical_pointer();
    check_a_release_of_a_pointer_inside_a_block_is_harmless();
    check_a_block_nothing_holds_gives_no_bytes_while_no_reference_can_be_made();
    check_a_collected_reference_covered_by_another_blocks_contents_is_no_block();
    check_a_forged_header_inside_a_live_blocks_contents_is_no_block();
    check_shadows_need_a_record_the_region_can_be_described_by();
    check_a_refused_install_leaves_no_query_bounds();
    check_a_collected_block_is_an_ended_block_of_its_length_until_a_sweep_reclaims_it();
    check_the_record_is_cleared_before_the_memory_goes_back_to_the_arena();
    check_a_call_during_a_revival_is_handed_the_array_by_a_reference_of_the_runtime();
    check_a_call_with_the_address_of_a_block_that_is_gone_is_handed_null();
    check_every_method_the_application_looks_up_is_known();
    check_a_method_that_cannot_be_described_stops_the_call_and_is_asked_about_again();
    check_the_other_causes_of_unknown_parameters_stop_the_call();
    check_a_static_call_with_a_null_class_runs_converted();
    check_a_platform_call_whose_parameters_cannot_be_had_is_forwarded();
    check_an_unseen_method_is_learned_with_an_exception_pending();
    check_after_a_merge_and_a_new_cut_a_place_is_a_block_only_where_one_starts();
    check_a_block_made_at_a_stale_address_during_PopLocalFrame_is_left_alone();
    check_a_block_made_at_a_collected_address_before_the_mutex_is_taken_is_acted_on_whole();
    check_the_record_follows_makes_ends_collections_and_revivals();
    check_stale_and_interior_readers_during_churn_see_no_block_or_a_published_length();
    check_get_object_ref_type_looks_at_the_record_once();
    check_the_ids_are_resolved_before_the_first_block();
    check_live_blocks_of_many_threads_count_against_the_cap();
    check_a_header_is_published_whole_to_readers_without_the_mutex();
    check_a_block_critical_get_does_not_hold_the_mutex_while_it_waits_for_the_collector();
    check_a_block_critical_release_does_not_hold_the_mutex_while_it_waits_for_the_collector();
    check_the_mutex_is_free_while_the_first_critical_get_is_in_the_runtime();
    check_a_block_critical_pointer_inside_any_region_makes_its_gets_outside_the_mutex();
    check_a_block_get_in_a_region_that_does_not_raise_the_flip_count_does_not_hold_the_mutex();
    check_a_block_release_in_a_region_that_does_not_raise_the_flip_count_does_not_hold_the_mutex();
    check_the_longest_layer_opened_region_is_counted();
    check_the_table_of_the_runtimes_pointers_does_not_wait_for_the_mutex();
    check_the_sentinels_array_is_made_outside_the_mutex();
    check_a_failed_sentinel_allocation_is_survived();
    check_a_full_table_of_the_runtimes_pointers_logs_one_line();
    check_the_synchronizations_need_little_stack();
    check_a_clean_blocks_calls_make_no_critical_get();
    check_an_exception_pending_synchronizes_nothing_and_a_deferred_end_writes_exact_runs();
    check_a_block_freed_between_the_two_holds_is_not_merged();
    check_a_java_write_during_a_merge_is_kept();
    check_scattered_application_bytes_keep_a_java_write_in_every_gap();
    check_a_java_write_during_the_word_path_of_a_merge_is_not_reverted();
    check_a_java_write_during_the_byte_path_of_a_merge_is_not_reverted();
    check_a_java_write_during_a_critical_merge_is_not_reverted();
    check_a_java_write_during_the_pull_in_place_is_not_reverted();
    check_a_java_write_during_a_region_pull_is_taken_at_the_next_point();
    check_a_java_write_during_a_deferred_ends_merge_is_kept();
    check_a_java_write_after_a_skipped_span_is_taken_at_the_next_point();
    check_the_applications_write_back_to_the_old_value_during_a_critical_merge_is_kept();
    check_the_applications_write_back_to_the_old_value_during_a_deferred_ends_merge_is_kept();
    check_a_refused_region_write_of_a_deferred_end_leaves_the_bytes_to_the_next_point();
    check_a_pull_stores_only_the_byte_java_changed();
    check_the_critical_merge_stores_only_the_byte_the_application_wrote();
    check_install_rules();
    check_no_shadows_when_disabled();
    std::puts("old-application JNI: ok");
    return 0;
}
