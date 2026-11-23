// SPDX-License-Identifier: Apache-2.0
//
// The old-application layer, bound to Android.
//
// ActivityThread loads this library into the process of an application the person switched it
// on for, a 32-bit application written for Dalvik (Android before 4.0): Dalvik gave its native
// code direct object pointers and a heap below 2 GiB. JNI_OnLoad installs the interposer on ART's
// shared function table (jni_shim.cpp) and keeps an allocator below 2 GiB (low_arena.hpp) for the
// libraries the application ships.
//
// The allocator is pointed at by rewriting relocation slots, never code. Every module of the
// process gets the calls that take a pointer (free, realloc, reallocarray, malloc_usable_size):
// they send a block of the arena to the arena and anything else to libc, so a platform module,
// libc's own getline, or the framework freeing a NativeActivity buffer can handle a block the
// application allocated. Only the application's libraries get the calls that return memory.
// All consumers are rewritten before any producer, so an arena block never exists where a
// consumer would refuse it.
//
// This file declares the few libc and linker functions it uses itself, so it includes no
// Android header.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "application_path.hpp"
#include "elf_scan.hpp"
#include "jni_shim.hpp"
#include "low_arena.hpp"
#include "protection.hpp"

extern "C" {
void* oldapp_mmap(void*, size_t, int, int, int, long) __asm__("mmap");
int oldapp_munmap(void*, size_t) __asm__("munmap");
int oldapp_mprotect(void*, size_t, int) __asm__("mprotect");
void* oldapp_malloc(size_t) __asm__("malloc");
void* oldapp_calloc(size_t, size_t) __asm__("calloc");
void* oldapp_realloc(void*, size_t) __asm__("realloc");
void oldapp_free(void*) __asm__("free");
void oldapp_abort() __asm__("abort");
size_t oldapp_usable_size(void*) __asm__("malloc_usable_size");
int* oldapp_errno() __asm__("__errno");
int oldapp_open(const char*, int, ...) __asm__("open");
long oldapp_read(int, void*, size_t) __asm__("read");
int oldapp_close(int) __asm__("close");
int oldapp_pthread_key_create(int*, void (*)(void*)) __asm__("pthread_key_create");
void* oldapp_pthread_getspecific(int) __asm__("pthread_getspecific");
int oldapp_pthread_setspecific(int, const void*) __asm__("pthread_setspecific");
int oldapp_pthread_mutex_lock(void*) __asm__("pthread_mutex_lock");
int oldapp_pthread_mutex_unlock(void*) __asm__("pthread_mutex_unlock");
int oldapp_log_print(int, const char*, const char*, ...) __asm__("__android_log_print");
int oldapp_clock_gettime(int, long*) __asm__("clock_gettime");  // struct timespec is two longs on bionic
int oldapp_sched_yield() __asm__("sched_yield");
void* oldapp_dlsym(void*, const char*) __asm__("dlsym");

// The native-method thunks (oldapp_thunks.S): thunk i is at oldapp_thunks + 8 * i.
void oldapp_thunks();
void oldapp_thunks_end();

// bionic's struct dl_phdr_info, 32-bit layout.
struct OldappPhdrInfo {
    uint32_t addr;
    const char* name;
    const oldapp::Elf32Phdr* phdr;
    uint16_t phnum;
    uint64_t adds;
    uint64_t subs;
    size_t tls_modid;
    void* tls_data;
};
int oldapp_dl_iterate_phdr(int (*)(OldappPhdrInfo*, size_t, void*), void*) __asm__("dl_iterate_phdr");
}

namespace {

using namespace oldapp;

constexpr size_t kArenaBytes = 64u << 20;
constexpr size_t kShadowBytes = 32u << 20;
static_assert(kShadowBytes <= kMaxShadowBytes, "the shadow region is within what the layer's block record covers");
constexpr uintptr_t kLowLimit = 0x7ff00000u;
constexpr unsigned kMaxModules = 512;
constexpr unsigned kMaxSlots = 160;
constexpr unsigned kNegativeCache = 64;
constexpr unsigned kCheckEvery = 256;  // JNI calls between two looks at the loader's counters
constexpr int kLogInfo = 4;
constexpr int kLogWarn = 5;
constexpr const char* kTag = "kernarium_oldapp";

Arena g_arena;
bool g_arena_ready = false;
int g_thread_key = 0;
bool g_have_thread_key = false;  // a key is any int: bionic sets its top bit
bool g_installed = false;
char g_package[256];  // the package this process runs (its process name up to ':'); empty: unknown

// The lock of the ended byte[] blocks (jni_shim.cpp): bionic's PTHREAD_MUTEX_INITIALIZER is all zeroes, and the
// buffer is larger than any version's pthread_mutex_t.
alignas(8) uint32_t g_ended_mutex[16];
void ended_lock() { oldapp_pthread_mutex_lock(g_ended_mutex); }
void ended_unlock() { oldapp_pthread_mutex_unlock(g_ended_mutex); }

void log_line(const char* message, long a, long b) { oldapp_log_print(kLogInfo, kTag, "%s %ld %ld", message, a, b); }

uint32_t now_ms() {
    long now[2] = {0, 0};
    oldapp_clock_gettime(1, now);  // CLOCK_MONOTONIC
    return static_cast<uint32_t>(now[0]) * 1000u + static_cast<uint32_t>(now[1] / 1000000);
}

uint64_t now_ns() {
    long now[2] = {0, 0};
    oldapp_clock_gettime(1, now);  // CLOCK_MONOTONIC
    return static_cast<uint64_t>(static_cast<uint32_t>(now[0])) * 1000000000ull + static_cast<uint64_t>(static_cast<uint32_t>(now[1]));
}
void yield_thread() { oldapp_sched_yield(); }

// The baselines of the byte[] blocks live in libc's heap: they need no low address.
void* alloc_baseline(size_t bytes) { return oldapp_malloc(bytes); }
void free_baseline(void* baseline) { oldapp_free(baseline); }

// The platform's stop (jni_shim.hpp, Platform::abort_process): the process aborts, with the line the layer logged before it.
void abort_process() { oldapp_abort(); }

// The memory of the shadow blocks' record (jni_shim.cpp): a mapping of its own, fresh and so zero-filled, that the layer never
// clears. It is resident by page only where block starts have been recorded, and a page stays resident after its blocks go.
// It needs no low address: only the layer's own code reads it.
void* map_record(size_t bytes) {
    constexpr int kProtReadWrite = 3;
    constexpr int kMapPrivateAnonymous = 0x02 | 0x20;
    void* mapped = oldapp_mmap(nullptr, bytes, kProtReadWrite, kMapPrivateAnonymous, -1, 0);
    return mapped == reinterpret_cast<void*>(-1) ? nullptr : mapped;
}

// The first allocation the arena could not serve: one log line, for the sessions of real games.
uint32_t g_fallback_logged = 0;
void note_fallback(size_t requested) {
    if (__atomic_exchange_n(&g_fallback_logged, 1u, __ATOMIC_RELAXED) == 0) {
        log_line("the low arena has no room: an allocation went to libc; bytes requested and in use:",
                 static_cast<long>(requested), static_cast<long>(g_arena.bytes_in_use()));
    }
}

// ---- the allocator the old libraries are pointed at

void* low_malloc(size_t size) {
    void* pointer = g_arena.allocate(size);
    if (pointer != nullptr) {
        return pointer;
    }
    note_fallback(size);
    return oldapp_malloc(size);
}
void low_free(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    if (g_arena.contains(pointer)) {
        g_arena.release(pointer);  // not a live block of the arena (a double free): ignored
    } else {
        oldapp_free(pointer);
    }
}
void* low_calloc(size_t count, size_t size) {
    size_t bytes;
    if (__builtin_mul_overflow(count, size, &bytes)) {
        *oldapp_errno() = 12;  // ENOMEM
        return nullptr;
    }
    void* pointer = g_arena.allocate(bytes);
    if (pointer == nullptr) {
        note_fallback(bytes);
        return oldapp_calloc(count, size);
    }
    __builtin_memset(pointer, 0, bytes);
    return pointer;
}
size_t low_usable_size(void* pointer) {
    if (pointer == nullptr) {
        return 0;
    }
    return g_arena.contains(pointer) ? g_arena.usable_size(pointer) : oldapp_usable_size(pointer);
}
// A block of the arena that outgrows it moves out to libc; the caller owns the new block as it owned the old.
void* grow_out_of_arena(void* pointer, size_t size) {
    note_fallback(size);
    void* outside = oldapp_malloc(size);
    if (outside != nullptr) {
        const size_t old = g_arena.usable_size(pointer);
        __builtin_memcpy(outside, pointer, old < size ? old : size);
        g_arena.release(pointer);
    }
    return outside;
}
// realloc as the application's libraries see it: a block from before the redirection moves into the arena.
void* low_realloc(void* pointer, size_t size) {
    if (pointer == nullptr) {
        return low_malloc(size);
    }
    if (g_arena.contains(pointer)) {
        void* moved = g_arena.reallocate(pointer, size);
        return moved != nullptr ? moved : grow_out_of_arena(pointer, size);
    }
    void* inside = g_arena.allocate(size);
    if (inside == nullptr) {
        note_fallback(size);
        return oldapp_realloc(pointer, size);
    }
    const size_t old = oldapp_usable_size(pointer);
    __builtin_memcpy(inside, pointer, old < size ? old : size);
    oldapp_free(pointer);
    return inside;
}
// realloc as a platform module sees it: its own blocks stay libc's, a block of the arena is resized there.
void* platform_realloc(void* pointer, size_t size) {
    if (pointer == nullptr || !g_arena.contains(pointer)) {
        return oldapp_realloc(pointer, size);
    }
    void* moved = g_arena.reallocate(pointer, size);
    return moved != nullptr ? moved : grow_out_of_arena(pointer, size);
}
void* low_reallocarray(void* pointer, size_t count, size_t size) {
    size_t bytes;
    if (__builtin_mul_overflow(count, size, &bytes)) {
        *oldapp_errno() = 12;
        return nullptr;
    }
    return low_realloc(pointer, bytes);
}
void* platform_reallocarray(void* pointer, size_t count, size_t size) {
    size_t bytes;
    if (__builtin_mul_overflow(count, size, &bytes)) {
        *oldapp_errno() = 12;
        return nullptr;
    }
    return platform_realloc(pointer, bytes);
}
char* low_strndup(const char* text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    char* copy = static_cast<char*>(low_malloc(length + 1));
    if (copy != nullptr) {
        __builtin_memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}
char* low_strdup(const char* text) { return low_strndup(text, 0xffffffffu); }
void* low_new(size_t size) {
    void* pointer = low_malloc(size != 0 ? size : 1);
    if (pointer == nullptr) {
        __builtin_trap();  // out of memory: operator new would have thrown std::bad_alloc
    }
    return pointer;
}
void* low_new_nothrow(size_t size, const void*) { return low_malloc(size != 0 ? size : 1); }
void low_delete(void* pointer) { low_free(pointer); }
void low_delete_nothrow(void* pointer, const void*) { low_free(pointer); }
void low_delete_sized(void* pointer, size_t) { low_free(pointer); }

void* layer_dlsym(void* handle, const char* name);

// `producing`: the producer pass of an application library, which may hand out memory of the arena. The consumer pass
// gives every module the realloc that never does, so a module whose free cannot be rewritten stays free of arena blocks.
void* replacement_for(Import what, bool producing) {
    switch (what) {
        case Import::kMalloc: return reinterpret_cast<void*>(&low_malloc);
        case Import::kFree: return reinterpret_cast<void*>(&low_free);
        case Import::kCalloc: return reinterpret_cast<void*>(&low_calloc);
        case Import::kRealloc:
            return producing ? reinterpret_cast<void*>(&low_realloc) : reinterpret_cast<void*>(&platform_realloc);
        case Import::kReallocarray:
            return producing ? reinterpret_cast<void*>(&low_reallocarray)
                             : reinterpret_cast<void*>(&platform_reallocarray);
        case Import::kUsableSize: return reinterpret_cast<void*>(&low_usable_size);
        case Import::kStrdup: return reinterpret_cast<void*>(&low_strdup);
        case Import::kStrndup: return reinterpret_cast<void*>(&low_strndup);
        case Import::kNew:
        case Import::kNewArray: return reinterpret_cast<void*>(&low_new);
        case Import::kNewNothrow:
        case Import::kNewArrayNothrow: return reinterpret_cast<void*>(&low_new_nothrow);
        case Import::kDelete:
        case Import::kDeleteArray: return reinterpret_cast<void*>(&low_delete);
        case Import::kDeleteNothrow:
        case Import::kDeleteArrayNothrow: return reinterpret_cast<void*>(&low_delete_nothrow);
        case Import::kDeleteSized:
        case Import::kDeleteArraySized: return reinterpret_cast<void*>(&low_delete_sized);
        case Import::kDlsym: return reinterpret_cast<void*>(&layer_dlsym);
        case Import::kCount: break;
    }
    return nullptr;
}

// ---- the modules of the process

struct Module {
    uintptr_t lo;
    uintptr_t hi;
    uintptr_t bias;
    const Elf32Phdr* phdr;
    unsigned phnum;
    uint32_t name_hash;
    uint8_t app;    // application code (classification)
    uint8_t libc;
    uint8_t skip;   // never touched: this library, the dynamic linker
    uint8_t consumers_done;
    uint8_t consumers_whole;  // every consumer slot of the module was rewritten (none left in an executable page)
    uint8_t producers_done;
    uintptr_t probe_address;  // a slot this module's rewrite wrote, to see whether it still holds our function
    void* probe_value;
};

Module g_modules[kMaxModules];    // published: sorted by lo, read under g_module_seq
unsigned g_module_count = 0;
uint32_t g_module_seq = 0;
Module g_work[kMaxModules];       // the refresh's working set, under g_refresh_lock
unsigned g_work_count = 0;
uint32_t g_refresh_lock = 0;
uint64_t g_stamp_adds = ~uint64_t{0};
uint64_t g_stamp_subs = ~uint64_t{0};
uint32_t g_epoch = 1;
uint32_t g_calls = 0;
uintptr_t g_negative_page[kNegativeCache];
uint32_t g_negative_epoch[kNegativeCache];
uint32_t g_negative_hits[kNegativeCache];

bool platform_path(const char* name) {
    static const char* const kPrefixes[] = {"/system/", "/system_ext/", "/vendor/", "/product/", "/odm/", "/apex/",
                                            "/linker", "/data/dalvik-cache/"};
    if (name == nullptr || name[0] != '/') {
        return true;  // the main program, the vdso, a name with no path
    }
    for (const char* prefix : kPrefixes) {
        size_t i = 0;
        while (prefix[i] != '\0' && name[i] == prefix[i]) {
            ++i;
        }
        if (prefix[i] == '\0') {
            return true;
        }
    }
    return false;
}

const char* base_name(const char* name) {
    const char* base = name;
    for (const char* p = name; p != nullptr && *p != '\0'; ++p) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base != nullptr ? base : "";
}

bool same_text(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool starts_with(const char* text, const char* prefix) {
    while (*prefix != '\0' && *text == *prefix) {
        ++text;
        ++prefix;
    }
    return *prefix == '\0';
}

uint32_t hash_text(const char* text) {
    uint32_t hash = 2166136261u;
    for (; text != nullptr && *text != '\0'; ++text) {
        hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
    }
    return hash;
}

// The refresh asks the dynamic linker for its list, and a thread that is inside dlopen (a constructor of the new
// library calling JNI) holds the linker's lock: waiting here for another thread's refresh could deadlock with it,
// so a thread that finds a refresh under way goes on with the list it has.
bool try_lock_refresh() { return __atomic_exchange_n(&g_refresh_lock, 1u, __ATOMIC_ACQUIRE) == 0; }
void unlock_refresh() { __atomic_store_n(&g_refresh_lock, 0u, __ATOMIC_RELEASE); }

bool protect_page(uintptr_t page, int protection) {
    return oldapp_mprotect(reinterpret_cast<void*>(page), kPageSize, protection) == 0;
}

struct Patched {
    unsigned written;   // slots rewritten
    bool whole;         // no slot was left alone: not in an executable page, and every page could be opened
};

// Points `mask` imports of one module at the layer's functions. All or nothing for the module: when a page cannot
// be opened, no slot is written. A slot in an executable page (a text relocation) is left alone: opening it would
// take away its execute bit under the threads that run there. It runs inside the loader's iteration of the modules,
// so the module cannot be closed under it.
Patched patch_module(const Module& module, uint32_t mask, bool include_defined, bool producing, Module* record) {
    ImportSlot slots[kMaxSlots];
    const size_t found = scan_imports(module.bias, module.phdr, module.phnum, slots, kMaxSlots, mask, include_defined);
    const size_t count = found < kMaxSlots ? found : kMaxSlots;
    bool whole = found <= kMaxSlots;
    if (found > kMaxSlots) {
        oldapp_log_print(kLogWarn, kTag, "%ld imports found, only %ld rewritten", static_cast<long>(found),
                         static_cast<long>(count));
    }
    struct Planned {
        uintptr_t address;
        void* replacement;
    };
    Planned planned[kMaxSlots];
    unsigned planned_count = 0;
    PageWindow<bool (*)(uintptr_t, int)> window(&protect_page);
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t address = module.bias + slots[i].vaddr;
        bool inside = false;
        for (unsigned k = 0; k < module.phnum; ++k) {
            if (module.phdr[k].p_type == kPtLoad && slots[i].vaddr >= module.phdr[k].p_vaddr &&
                slots[i].vaddr < module.phdr[k].p_vaddr + module.phdr[k].p_memsz) {
                inside = true;
            }
        }
        const int protection = original_protection(module.phdr, module.phnum, slots[i].vaddr);
        void* replacement = replacement_for(slots[i].what, producing);
        if (!inside || (protection & kProtExec) != 0) {
            whole = false;
            continue;
        }
        if (replacement == nullptr) {
            continue;
        }
        if (!window.open(address & ~(kPageSize - 1), protection)) {
            oldapp_log_print(kLogWarn, kTag, "a page of the module at %lx could not be opened: its imports are left alone",
                             static_cast<long>(module.lo));
            return Patched{0, false};
        }
        planned[planned_count++] = Planned{address, replacement};
    }
    for (unsigned i = 0; i < planned_count; ++i) {
        __atomic_store_n(reinterpret_cast<void**>(planned[i].address), planned[i].replacement, __ATOMIC_RELEASE);
    }
    if (!window.close()) {
        oldapp_log_print(kLogWarn, kTag, "a page of the module at %lx could not be made read-only again",
                         static_cast<long>(module.lo));
    }
    if (planned_count > 0 && record != nullptr && record->probe_address == 0) {
        record->probe_address = planned[0].address;
        record->probe_value = planned[0].replacement;
    }
    return Patched{planned_count, whole};
}

struct Collect {
    unsigned count;
    uint64_t adds;
    uint64_t subs;
    bool first;
    unsigned consumers;  // modules whose consumers this refresh rewrote
    unsigned producers;
};

// What an earlier refresh already did to this module, if it is still the same library at the same place.
void inherit_state(Module& module) {
    for (unsigned k = 0; k < g_module_count; ++k) {
        const Module& old = g_modules[k];
        if (old.lo == module.lo && old.hi == module.hi && old.bias == module.bias && old.name_hash == module.name_hash) {
            // The module is loaded now (the loader holds its list for the caller), so its pages can be read.
            const bool intact = old.probe_address == 0 ||
                                *reinterpret_cast<void* volatile*>(old.probe_address) == old.probe_value;
            if (intact) {
                module.consumers_done = old.consumers_done;
                module.consumers_whole = old.consumers_whole;
                module.producers_done = old.producers_done;
                module.probe_address = old.probe_address;
                module.probe_value = old.probe_value;
            }
            return;
        }
    }
}

int collect_module(OldappPhdrInfo* info, size_t size, void* user) {
    Collect* collect = static_cast<Collect*>(user);
    if (collect->first) {
        collect->first = false;
        collect->adds = size >= sizeof(OldappPhdrInfo) ? info->adds : g_calls;
        collect->subs = size >= sizeof(OldappPhdrInfo) ? info->subs : 0;
    }
    uintptr_t lo = ~uintptr_t{0};
    uintptr_t hi = 0;
    for (unsigned i = 0; i < info->phnum; ++i) {
        if (info->phdr[i].p_type == kPtLoad) {
            const uintptr_t start = info->addr + info->phdr[i].p_vaddr;
            const uintptr_t end = start + info->phdr[i].p_memsz;
            lo = start < lo ? start : lo;
            hi = end > hi ? end : hi;
        }
    }
    if (hi == 0 || collect->count >= kMaxModules) {
        return 0;
    }
    // This library's own code is never the application's, wherever it was installed from.
    const uintptr_t self = reinterpret_cast<uintptr_t>(&collect_module);
    const bool is_self = self >= lo && self < hi;
    const char* base = base_name(info->name);
    const bool is_linker = starts_with(base, "linker") || same_text(base, "ld-android.so");
    Module& module = g_work[collect->count++];
    module = Module{};
    module.lo = lo;
    module.hi = hi;
    module.bias = info->addr;
    module.phdr = info->phdr;
    module.phnum = info->phnum;
    module.name_hash = hash_text(info->name);
    module.app = !platform_path(info->name) && !is_self;
    module.libc = platform_path(info->name) && same_text(base, "libc.so");
    module.skip = is_self || is_linker || (info->name != nullptr && info->name[0] != '/');
    inherit_state(module);
    // First every consumer, in every module (this pass); the producers of the application's libraries follow in a
    // pass of their own, so no arena pointer exists before every module that was loaded accepts it.
    if (g_arena_ready && !module.skip && !module.consumers_done) {
        module.consumers_done = 1;
        const Patched done = patch_module(module, module.app ? kAppConsumers : kPlatformConsumers, module.libc != 0, false, &module);
        module.consumers_whole = done.whole;
        collect->consumers += done.written > 0;
    }
    return 0;
}

// Pass two: the producers, in the application's libraries whose consumers were all rewritten (a library that kept
// a consumer slot in an executable page would hand an arena block to libc's free).
int producer_module(OldappPhdrInfo* info, size_t, void* user) {
    Collect* collect = static_cast<Collect*>(user);
    const uint32_t name_hash = hash_text(info->name);
    for (unsigned i = 0; i < g_work_count; ++i) {
        Module& module = g_work[i];
        if (module.bias != info->addr || module.name_hash != name_hash || module.phdr != info->phdr) {
            continue;
        }
        if (module.skip || !module.app || module.producers_done) {
            return 0;
        }
        module.producers_done = 1;
        if (module.consumers_whole) {
            collect->producers += patch_module(module, kProducers, false, true, &module).written > 0;
        } else {
            oldapp_log_print(kLogWarn, kTag, "the module at %lx keeps an import of free that cannot be rewritten: its "
                             "allocations stay in libc", static_cast<long>(module.lo));
        }
        return 0;
    }
    return 0;
}

int stamp_module(OldappPhdrInfo* info, size_t size, void* user) {
    Collect* collect = static_cast<Collect*>(user);
    collect->adds = size >= sizeof(OldappPhdrInfo) ? info->adds : g_calls;
    collect->subs = size >= sizeof(OldappPhdrInfo) ? info->subs : 0;
    return 1;
}

void sort_modules(Module* modules, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {  // insertion sort: the list is short and nearly sorted
        Module key = modules[i];
        unsigned j = i;
        while (j > 0 && modules[j - 1].lo > key.lo) {
            modules[j] = modules[j - 1];
            --j;
        }
        modules[j] = key;
    }
}

// Rebuilds the module list when the loader's counters moved, rewrites the modules that are new (or whose slots no
// longer hold the layer's functions: a library closed and opened again at the same address), and publishes the list.
void refresh_modules(bool force) {
    Collect now{0, 0, 0, true, 0, 0};
    oldapp_dl_iterate_phdr(stamp_module, &now);
    if (!try_lock_refresh()) {
        return;
    }
    if (!force && now.adds == g_stamp_adds && now.subs == g_stamp_subs) {
        unlock_refresh();  // another thread was first
        return;
    }
    Collect collect{0, 0, 0, true, 0, 0};
    oldapp_dl_iterate_phdr(collect_module, &collect);
    g_work_count = collect.count;
    sort_modules(g_work, g_work_count);
    if (g_arena_ready) {
        oldapp_dl_iterate_phdr(producer_module, &collect);
    }
    const unsigned consumers = collect.consumers, producers = collect.producers;
    __atomic_store_n(&g_module_seq, g_module_seq + 1, __ATOMIC_RELEASE);  // odd: readers wait
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __builtin_memcpy(g_modules, g_work, sizeof(Module) * g_work_count);
    __atomic_store_n(&g_module_count, g_work_count, __ATOMIC_RELAXED);
    __atomic_store_n(&g_module_seq, g_module_seq + 1, __ATOMIC_RELEASE);
    g_stamp_adds = collect.adds;
    g_stamp_subs = collect.subs;
    ++g_epoch;
    unlock_refresh();
    if (consumers != 0 || producers != 0) {
        oldapp_log_print(kLogInfo, kTag, "modules %ld: consumers rewritten in %ld, producers in %ld",
                         static_cast<long>(g_work_count), static_cast<long>(consumers), static_cast<long>(producers));
    }
}

void refresh_if_changed() { refresh_modules(false); }

// 1: application code, 0: platform code, -1: no mapping known for the address.
int classify(uintptr_t address) {
    for (;;) {
        const uint32_t before = __atomic_load_n(&g_module_seq, __ATOMIC_ACQUIRE);
        if ((before & 1u) != 0) {
            continue;  // a refresh is publishing
        }
        int verdict = -1;
        unsigned low = 0;
        unsigned high = __atomic_load_n(&g_module_count, __ATOMIC_RELAXED);
        while (low < high) {
            const unsigned middle = (low + high) / 2;
            if (address < __atomic_load_n(&g_modules[middle].lo, __ATOMIC_RELAXED)) {
                high = middle;
            } else if (address >= __atomic_load_n(&g_modules[middle].hi, __ATOMIC_RELAXED)) {
                low = middle + 1;
            } else {
                verdict = __atomic_load_n(&g_modules[middle].app, __ATOMIC_RELAXED) != 0 ? 1 : 0;
                break;
            }
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_module_seq, __ATOMIC_RELAXED) == before) {
            return verdict;
        }
    }
}

bool is_app_caller(const void* return_address) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(return_address) & ~uintptr_t{1};  // the Thumb bit
    if ((__atomic_add_fetch(&g_calls, 1u, __ATOMIC_RELAXED) % kCheckEvery) == 0) {
        refresh_if_changed();  // a library may have been opened with no JNI call from it
    }
    int verdict = classify(address);
    if (verdict >= 0) {
        return verdict == 1;
    }
    // Code no mapping of the list covers: generated code, or a library loaded since the last refresh.
    const uintptr_t page = address >> 12;
    const unsigned slot = page % kNegativeCache;
    if (g_negative_page[slot] == page && g_negative_epoch[slot] == g_epoch &&
        (++g_negative_hits[slot] % kCheckEvery) != 0) {
        return false;
    }
    refresh_if_changed();
    verdict = classify(address);
    if (verdict >= 0) {
        return verdict == 1;
    }
    g_negative_page[slot] = page;
    g_negative_epoch[slot] = g_epoch;
    g_negative_hits[slot] = 0;
    return false;
}

// Whether the function is in one of the application's own libraries (the thunks' domain): asked once for each native
// method the runtime binds, never per call. The answer comes from the loader's list as it is at this moment, never from the
// classification cache: a library that was closed and another opened at its address would keep the old verdict there, and
// a refresh that another thread is making would leave none. Bindings are rare, and the loader's lock is held for the
// length of the question, so the library cannot be closed under it. The loader's list can still name a library that was
// closed (its entry outlives the unload) beside the one now mapped over its range: the later entry is the current one.
struct OwnQuery {
    uintptr_t address;
    int verdict;  // -1: no mapping covers the address
};
int own_module_at(OldappPhdrInfo* info, size_t, void* user) {
    OwnQuery* query = static_cast<OwnQuery*>(user);
    uintptr_t lo = ~uintptr_t{0};
    uintptr_t hi = 0;
    for (unsigned i = 0; i < info->phnum; ++i) {
        if (info->phdr[i].p_type == kPtLoad) {
            const uintptr_t start = info->addr + info->phdr[i].p_vaddr;
            const uintptr_t end = start + info->phdr[i].p_memsz;
            lo = start < lo ? start : lo;
            hi = end > hi ? end : hi;
        }
    }
    if (hi == 0 || query->address < lo || query->address >= hi) {
        return 0;
    }
    const uintptr_t self = reinterpret_cast<uintptr_t>(&own_module_at);
    const bool is_self = self >= lo && self < hi;
    query->verdict = !is_self && !platform_path(info->name) && in_application_directories(info->name, g_package) ? 1 : 0;
    return 0;  // the list goes on: a later entry over the same range replaces this verdict
}
bool is_own_library(const void* function) {
    OwnQuery query{reinterpret_cast<uintptr_t>(function) & ~uintptr_t{1}, -1};  // the Thumb bit
    oldapp_dl_iterate_phdr(own_module_at, &query);
    return query.verdict == 1;
}

// The package of this process, from its name (/proc/self/cmdline): left empty when it cannot be read, and every
// application library is then the application's own. A process named other than its package (android:process) shows
// no package there: package_from_runtime() below asks the runtime, which knows it, and the name is only the fallback.
void read_package() {
    constexpr int kReadOnly = 0;
    constexpr int kCloseOnExec = 0x80000;
    g_package[0] = '\0';
    const int fd = oldapp_open("/proc/self/cmdline", kReadOnly | kCloseOnExec);
    if (fd < 0) {
        return;
    }
    char name[sizeof(g_package)];
    const long n = oldapp_read(fd, name, sizeof(name) - 1);
    oldapp_close(fd);
    if (n <= 0) {
        return;
    }
    name[n] = '\0';  // the file is the NUL-separated argument list: its first string is the process name
    if (!package_of_process(name, g_package, sizeof(g_package))) {
        g_package[0] = '\0';
    }
}

// The package the runtime says this process runs: ActivityThread.currentPackageName(), looked up through JNI at the
// load of the layer (the framework loads it in handleBindApplication, after the application's binding is set). It
// replaces the name taken from the command line when it answers. False (nothing changed) when the class or the method is
// not there, the call fails, or the answer is empty.
bool package_from_runtime(jni::Env* env) {
    using namespace jni;
    auto push_frame = reinterpret_cast<jint (*)(Env*, jint)>(env->functions[slot::PushLocalFrame]);
    auto pop_frame = reinterpret_cast<jobject (*)(Env*, jobject)>(env->functions[slot::PopLocalFrame]);
    auto exception_check = reinterpret_cast<jboolean (*)(Env*)>(env->functions[slot::ExceptionCheck]);
    auto exception_clear = reinterpret_cast<void (*)(Env*)>(env->functions[slot::ExceptionClear]);
    if (push_frame(env, 8) != JNI_OK) {
        exception_clear(env);
        return false;
    }
    bool ok = false;
    do {
        auto find_class = reinterpret_cast<jobject (*)(Env*, const char*)>(env->functions[slot::FindClass]);
        auto get_static = reinterpret_cast<jmethodID (*)(Env*, jclass, const char*, const char*)>(
            env->functions[slot::GetStaticMethodID]);
        auto call_static = reinterpret_cast<jobject (*)(Env*, jclass, jmethodID, const jvalue*)>(
            env->functions[static_call_slot(kObject, kArray)]);
        auto utf_chars = reinterpret_cast<const char* (*)(Env*, jobject, jboolean*)>(env->functions[slot::GetStringUTFChars]);
        auto release_chars = reinterpret_cast<void (*)(Env*, jobject, const char*)>(env->functions[slot::ReleaseStringUTFChars]);
        jobject activity_thread = find_class(env, "android/app/ActivityThread");
        if (activity_thread == nullptr || exception_check(env)) {
            break;
        }
        jmethodID current = get_static(env, activity_thread, "currentPackageName", "()Ljava/lang/String;");
        if (current == nullptr || exception_check(env)) {
            break;
        }
        jobject name = call_static(env, activity_thread, current, nullptr);
        if (name == nullptr || exception_check(env)) {
            break;
        }
        const char* chars = utf_chars(env, name, nullptr);
        if (chars == nullptr) {
            break;
        }
        char text[sizeof(g_package)];
        if (package_of_process(chars, text, sizeof(text))) {
            for (size_t i = 0; i < sizeof(text); ++i) {
                g_package[i] = text[i];
                if (text[i] == '\0') {
                    break;
                }
            }
            ok = true;
        }
        release_chars(env, name, chars);
    } while (false);
    if (exception_check(env)) {
        exception_clear(env);
        ok = false;
    }
    pop_frame(env, nullptr);
    return ok;
}

// ---- the native methods of the application

constexpr unsigned kThunkBytes = 8;  // movw ip, #index; b entry

void* thunk_address(unsigned index) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&oldapp_thunks) + kThunkBytes * index);
}
unsigned thunk_count() {
    return static_cast<unsigned>((reinterpret_cast<uintptr_t>(&oldapp_thunks_end) - reinterpret_cast<uintptr_t>(&oldapp_thunks)) /
                                 kThunkBytes);
}

// ART's lookup of a symbol in the library it loaded (SharedLibrary::FindSymbolWithoutNativeBridge: dlsym with the
// library's handle) finds the application's JNI_OnLoad and the Java_ functions its native methods are bound to by
// name. The function of the application's own library comes back as its thunk; any other lookup is the loader's
// answer. Only libart's import of dlsym is pointed here.
__attribute__((noinline)) void* layer_dlsym_found(void* handle, const char* name) {
    void* function = oldapp_dlsym(handle, name);
    if (function == nullptr || !(starts_with(name, "Java_") || same_text(name, "JNI_OnLoad"))) {
        return function;
    }
    return thunk_for(function);
}
void* layer_dlsym(void* handle, const char* name) {
    // RTLD_DEFAULT and RTLD_NEXT look up from the caller's namespace: they go to the loader as libart's own call (a tail
    // call, so the caller is still libart). This library is 32-bit: Bionic's LP32 values are 0xffffffff and 0xfffffffe
    // (the LP64 ones, 0 and -1, are not those).
    constexpr uintptr_t kRtldDefault = 0xffffffffu;
    constexpr uintptr_t kRtldNext = 0xfffffffeu;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    if (handle == nullptr || value == kRtldDefault || value == kRtldNext) {
        return oldapp_dlsym(handle, name);
    }
    return layer_dlsym_found(handle, name);
}

int rewrite_art_module(OldappPhdrInfo* info, size_t, void* user) {
    if (!platform_path(info->name) || !same_text(base_name(info->name), "libart.so")) {
        return 0;
    }
    Module module{};
    module.lo = info->addr;
    module.bias = info->addr;
    module.phdr = info->phdr;
    module.phnum = info->phnum;
    *static_cast<unsigned*>(user) += patch_module(module, kDlsymImport, false, false, nullptr).written;
    return 1;
}

// Points libart's dlsym slot at layer_dlsym: the number of slots rewritten (0: the native methods get no thunk when they
// are bound by name).
unsigned rewrite_art_dlsym() {
    unsigned written = 0;
    oldapp_dl_iterate_phdr(rewrite_art_module, &written);
    return written;
}

// ---- protection of the interposed tables

bool read_maps(int fd, char* buffer, size_t capacity, size_t* got) {
    const long n = oldapp_read(fd, buffer, capacity);
    if (n < 0) {
        return false;
    }
    *got = static_cast<size_t>(n);
    return true;
}

int table_protection(uintptr_t address) {
    constexpr int kReadOnly = 0;
    constexpr int kCloseOnExec = 0x80000;
    const int fd = oldapp_open("/proc/self/maps", kReadOnly | kCloseOnExec);
    if (fd < 0) {
        return -1;
    }
    const int protection =
        maps_protection([fd](char* buffer, size_t capacity, size_t* got) { return read_maps(fd, buffer, capacity, got); },
                        address);
    oldapp_close(fd);
    return protection;
}

PageWindow<bool (*)(uintptr_t, int)> g_window(&protect_page);

bool make_writable(void* address, size_t length) {
    g_window.rollback();
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t first = start & ~(kPageSize - 1);
    const uintptr_t last = (start + length - 1) & ~(kPageSize - 1);
    for (uintptr_t page = first; page <= last; page += kPageSize) {
        const int protection = table_protection(page < start ? start : page);
        if (protection < 0) {
            g_window.rollback();
            return false;  // no mapping is known for it: never assumed writable
        }
        if (!g_window.open(page, protection)) {
            return false;
        }
    }
    return true;
}

bool make_read_only(void*, size_t) { return g_window.close(); }

// ---- per-thread state

void thread_exit(void* state) {
    release_thread_state(state);
    oldapp_free(state);
}

void* thread_state(size_t size) {
    if (!g_have_thread_key) {
        return nullptr;
    }
    void* state = oldapp_pthread_getspecific(g_thread_key);
    if (state == nullptr) {
        state = oldapp_calloc(1, size);
        if (state != nullptr) {
            oldapp_pthread_setspecific(g_thread_key, state);
        }
    }
    return state;
}

// ---- a method the layer did not see being looked up

char kind_of_class_name(const char* name) {
    static const struct {
        const char* name;
        char kind;
    } kPrimitives[] = {{"boolean", 'Z'}, {"byte", 'B'}, {"char", 'C'}, {"short", 'S'},
                       {"int", 'I'},     {"long", 'J'}, {"float", 'F'}, {"double", 'D'}};
    for (const auto& primitive : kPrimitives) {
        if (same_text(name, primitive.name)) {
            return primitive.kind;
        }
    }
    return 'L';
}

// Executable.getParameterTypes() of the method's reflected object, and the name of each class: every function is
// taken from the table as it was before the layer, and the locals of the lookup are gone when it returns.
bool describe_method(jni::Env* env, void* const* original, void* id, bool is_static, char* kinds, unsigned capacity,
                     unsigned* count) {
    using namespace jni;
    auto push_frame = reinterpret_cast<jint (*)(Env*, jint)>(original[slot::PushLocalFrame]);
    auto pop_frame = reinterpret_cast<jobject (*)(Env*, jobject)>(original[slot::PopLocalFrame]);
    auto exception_check = reinterpret_cast<jboolean (*)(Env*)>(original[slot::ExceptionCheck]);
    auto exception_clear = reinterpret_cast<void (*)(Env*)>(original[slot::ExceptionClear]);
    if (push_frame(env, 32) != JNI_OK) {
        exception_clear(env);
        return false;
    }
    bool ok = false;
    do {
        auto to_reflected = reinterpret_cast<jobject (*)(Env*, jclass, jmethodID, jboolean)>(original[slot::ToReflectedMethod]);
        auto find_class = reinterpret_cast<jobject (*)(Env*, const char*)>(original[slot::FindClass]);
        auto get_method = reinterpret_cast<jmethodID (*)(Env*, jclass, const char*, const char*)>(original[slot::GetMethodID]);
        auto call_object = reinterpret_cast<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(
            original[call_slot(kObject, kArray)]);
        auto array_length = reinterpret_cast<jsize (*)(Env*, jarray)>(original[slot::GetArrayLength]);
        auto element = reinterpret_cast<jobject (*)(Env*, jobject, jsize)>(original[slot::GetObjectArrayElement]);
        auto utf_chars = reinterpret_cast<const char* (*)(Env*, jobject, jboolean*)>(original[slot::GetStringUTFChars]);
        auto release_chars = reinterpret_cast<void (*)(Env*, jobject, const char*)>(original[slot::ReleaseStringUTFChars]);
        // The runtime's ToReflectedMethod decodes the method ID and ignores its class and static-flag arguments, so a method is
        // described from its ID alone: any class does for the call, and one is needed because CheckJNI refuses a null one.
        jobject executable = find_class(env, "java/lang/reflect/Executable");
        jobject class_class = find_class(env, "java/lang/Class");
        if (executable == nullptr || class_class == nullptr || exception_check(env)) {
            break;
        }
        jobject reflected = to_reflected(env, executable, id, is_static ? 1 : 0);
        if (reflected == nullptr || exception_check(env)) {
            break;
        }
        jmethodID get_types = get_method(env, executable, "getParameterTypes", "()[Ljava/lang/Class;");
        jmethodID get_name = get_method(env, class_class, "getName", "()Ljava/lang/String;");
        if (get_types == nullptr || get_name == nullptr || exception_check(env)) {
            break;
        }
        jobject types = call_object(env, reflected, get_types, nullptr);
        if (types == nullptr || exception_check(env)) {
            break;
        }
        const jsize n = array_length(env, types);
        if (n < 0 || static_cast<unsigned>(n) > capacity) {
            break;
        }
        bool all = true;
        for (jsize i = 0; i < n && all; ++i) {
            jobject type = element(env, types, i);
            jobject name = type != nullptr ? call_object(env, type, get_name, nullptr) : nullptr;
            const char* chars = name != nullptr && !exception_check(env) ? utf_chars(env, name, nullptr) : nullptr;
            if (chars == nullptr) {
                all = false;
            } else {
                kinds[i] = kind_of_class_name(chars);
                release_chars(env, name, chars);
            }
        }
        if (!all || exception_check(env)) {
            break;
        }
        *count = static_cast<unsigned>(n);
        ok = true;
    } while (false);
    if (exception_check(env)) {
        exception_clear(env);
        ok = false;
    }
    pop_frame(env, nullptr);
    return ok;
}

// ---- the region below 2 GiB

uint8_t* map_low_region(size_t bytes) {
    static const uintptr_t kHints[] = {0x30000000u, 0x40000000u, 0x50000000u, 0x58000000u, 0x20000000u,
                                       0x10000000u, 0x60000000u};
    constexpr int kProtReadWrite = 3;
    constexpr int kMapPrivateAnonymousNoReserve = 0x02 | 0x20 | 0x4000;
    constexpr int kMapFixedNoReplace = 0x100000;
    for (uintptr_t hint : kHints) {
        void* mapped = oldapp_mmap(reinterpret_cast<void*>(hint), bytes, kProtReadWrite,
                                   kMapPrivateAnonymousNoReserve | kMapFixedNoReplace, -1, 0);
        if (mapped == reinterpret_cast<void*>(-1)) {
            continue;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(mapped);
        if (start >= 0x10000u && start + bytes <= kLowLimit) {
            return static_cast<uint8_t*>(mapped);
        }
        oldapp_munmap(mapped, bytes);  // an older kernel ignores the flag and may place it anywhere
    }
    return nullptr;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int JNI_OnLoad(void* vm_pointer, void*) {
    constexpr int kJniVersion = 0x00010006;
    auto* vm = static_cast<jni::Vm*>(vm_pointer);
    if (g_installed) {
        return kJniVersion;
    }
    g_installed = true;
    jni::Env* env = nullptr;
    if (reinterpret_cast<int (*)(jni::Vm*, void**, int)>(vm->functions[jni::vmslot::GetEnv])(
            vm, reinterpret_cast<void**>(&env), kJniVersion) != 0 || env == nullptr) {
        oldapp_log_print(kLogWarn, kTag, "no JNIEnv on the loading thread: the layer is not installed");
        return kJniVersion;
    }
    g_have_thread_key = oldapp_pthread_key_create(&g_thread_key, thread_exit) == 0;
    uint8_t* region = map_low_region(kArenaBytes + kShadowBytes);
    if (region != nullptr) {
        g_arena_ready = g_arena.init(region, kArenaBytes);
        oldapp_log_print(kLogInfo, kTag, "region below 2 GiB at %p", static_cast<void*>(region));
    } else {
        oldapp_log_print(kLogWarn, kTag, "no region below 2 GiB: allocator imports are left alone");
    }
    read_package();
    const bool package_from_command_line = g_package[0] != '\0';
    if (!package_from_runtime(env) && !package_from_command_line) {
        oldapp_log_print(kLogWarn, kTag,
                         "the package of this process is unknown: every library but the platform's counts as the application's own");
    }
    const Platform platform{is_app_caller, thread_state, log_line, make_writable, make_read_only, describe_method, now_ms,
                            alloc_baseline, free_baseline, region != nullptr ? thunk_address : nullptr,
                            region != nullptr ? thunk_count() : 0, is_own_library, ended_lock, ended_unlock, now_ns,
                            yield_thread, map_record, abort_process};
    refresh_modules(true);  // every library the application loaded before this one
    void** table = const_cast<void**>(env->functions);
    const bool jni_ok = install_jni_wrappers(table, platform, region != nullptr ? region + kArenaBytes : nullptr,
                                             region != nullptr ? kShadowBytes : 0, env);
    install_vm_wrappers(const_cast<void**>(vm->functions), platform);
    oldapp_log_print(kLogInfo, kTag, "installed: jni=%ld arena=%ld", static_cast<long>(jni_ok),
                     static_cast<long>(g_arena_ready));
    const unsigned art_slots = jni_ok && region != nullptr ? rewrite_art_dlsym() : 0;
    oldapp_log_print(kLogInfo, kTag, "native-method thunks: ready %ld, libart's dlsym rewritten %ld",
                     static_cast<long>(jni_ok && region != nullptr ? thunk_count() : 0), static_cast<long>(art_slots));
    return kJniVersion;
}
