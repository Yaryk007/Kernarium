// SPDX-License-Identifier: Apache-2.0
//
// The native half of the old-application probe (oldapp_probe_apk.py).
//
// An application written for Android before 4.0 does four things that ART no longer
// supports. This library does them the way such an application does, as ordinary
// application code, and reports one "key=value" for each, as the text its native method
// returns (the receiver makes it the broadcast's result data):
//
//   shim_mapped  whether the old-application layer is mapped in this process
//   raw_read     reads a Java byte[] through the object pointer at the Dalvik offsets
//                (length at +8, bytes at +0x10)
//   passback     hands the same byte[] back to Java and to the array functions
//   elements     GetByteArrayElements on it returns the bytes
//   global       a global reference made from it is a usable array
//   thread_raw   the raw read from a second, attached thread
//   low_heap     64 blocks of 256 KiB from malloc/calloc/realloc/strdup all lie below 2 GiB
//   write_through  a write through SetByteArrayRegion, GetByteArrayElements or
//                GetPrimitiveArrayCritical reaches the Java array
//   identity     the array, a global reference made from it and the array a Java call returned
//                are one object (IsSameObject)
//   retained     a reference kept while 520 more results arrive still reads its own bytes
//   java_fill    Java fills an array the native side passed it (System.arraycopy), and the native side reads the
//                new bytes through GetByteArrayRegion and GetByteArrayElements
//   java_fill_raw  and through the object pointer at the Dalvik offsets
//   raw_write_java the native side fills an array through the object pointer, never releases it, and a Java
//                method (Arrays.hashCode) that is passed the array returns the checksum of those bytes ("skipped"
//                where the layer is not mapped: a reference is no pointer)
//   commit_chunks  the array is open through its own critical pointer and through the block's: a write through
//                each, in different 4 KiB chunks, both reach the array when both are released
//   clone        the array's clone() sees a write made through the pointer GetByteArrayElements returned
//                (released before the call when that pointer is a copy)
//   alias_elements  a write through the pointer GetByteArrayElements returned, committed when the pointer says it is a
//                copy, shows through a second (real) reference to the array
//   alias_critical  the same through GetPrimitiveArrayCritical, against the array's own pinned pointer
//   ends_lazy_thumb, ends_lazy_arm, ends_reg_thumb, ends_reg_arm  600 calls of a native method that each get a byte[]
//                result from Java and read it through the object pointer, never deleting it: the blocks end when the
//                method returns, as Dalvik's local references did (a Thumb or an ARM function, bound by name or by
//                RegisterNatives). Past 512 a result would stay the runtime's own reference and the raw read fail
//   ends_recursion  a native method calls Java, which calls the native method, to a depth of 70 (past the 64 frame
//                records of a thread): every level's block outlives the calls below it and ends with its own return
//   ends_onload  the blocks JNI_OnLoad made end when it returns
//   ends_outside  a library that is not the application's own (a copy of this library in another package's
//                directory, whose path the options carry) registers the same native method: it gets no thunk, so
//                the blocks of its calls are not ended by their returns and a result past the thread's 512 stays
//                the runtime's own reference (the row passes when a call fails after the first: the layer left
//                the method as it was)
//   ends_exception  a native method that returns with an exception pending: the process goes on, the exception arrives
//   abi_args, abi_returns  a native method gets its arguments (12, with jlong, jdouble and jfloat on the stack) and
//                returns its result (every type) as without the thunk
//   scene_complete, scene_data, scene_write  (the options "scene": a run of its own, in a fresh process) the render
//                thread of an application. A java.lang.Thread that Java starts (the probe class is its Runnable)
//                runs a Java loop that enters two lazily bound native methods of this library directly from Java:
//                an init method that gets 170 fresh byte[] results from a Java asset reader, and a frame method
//                (800 frames) that gets a few per frame and 100 to 150 in a burst every 100 frames (a map change),
//                5000 or more in all, of seeded lengths from 16 bytes to 320 KB, none repeated, none deleted, no
//                local frame. Each is read as the length and content Java gave it (through the JNI functions, and
//                through the object pointer where the layer is mapped); every eighth gets four bytes written
//                (through the pointer where the layer is mapped) and Java reads them back. The runner reads the
//                layer's log lines for the process: no declined result, no allocation that left the arena
//   scene_attached  the same loop, shorter (100 frames), on a native thread of the probe that attaches itself to the
//                runtime (AttachCurrentThread) and detaches at the end: the other way a thread enters the runtime
//   ended_keep   an array Java holds is obtained as a block and its pointer kept; the block's reference ends, a
//                thousand temporaries of 64 KiB come and go with Runtime.gc() between batches, and the kept pointer
//                still reads the array's bytes as of the end. Java then changes a byte, the application writes another
//                through the kept pointer (in another 4 KiB chunk), and the array obtained again is the block at the
//                same address, with both bytes visible on both sides
//   ended_arrays 64 arrays that Java holds, each obtained 100 times in turn: always its own block, at the same address
//   ended_pin    a pointer from GetByteArrayElements that is never released, on an array Java drops: the block's
//                reference ends, Runtime.gc() runs between 100 results of the same size, and the kept pointer still
//                reads what was written through it; a Release by a real reference to the array with the block's
//                pointer is the block's, not the runtime's (the process lives and the array is Java's)
//   ended_kept_ref  the reference of a block that ended, kept in a variable across 100 "frames": each frame asks it
//                the length, the elements, a Java call and a region, with Java and the application both writing the
//                array (bytes of one 4 KiB chunk), and both writes show on both sides
//   bind_swap    (the options "swap|<foreign library>|<own library>": a run of its own) a library of another package is
//                loaded at a reserved address, enters the layer and is closed; a library of the application's own is
//                loaded at the same address and its native method registered. It gets its thunk: 600 calls from one
//                native frame all return a block (a binding decided from the address's earlier owner would leave the
//                method without a thunk, and the 513th result would be the runtime's own reference)
//   ended_reuse, ended_limit, sentinel_pressure, sentinel_gc, create_ns  recorded: a temporary took the address of an
//                earlier one after a collection; a raw write after the block ended does not reach Java (the stated
//                limit); whether a weak reference to an array nothing holds is cleared by allocation pressure and by an
//                explicit collection (the layer's sentinel); nanoseconds for a byte[] result and its deletion
//   push_ns      recorded: nanoseconds for a Java call that is passed a 1 MiB block with every 4 KiB chunk written
//                through the pointer (the push of the dirty chunks, and the pull after), and for the same call with the
//                block clean
//   push_scatter4_us, push_scatter2_us  recorded: microseconds for a Java call that is passed a 1 MiB block of which every
//                4th (every other) byte is written through the pointer, new values each round: the worst case of the exact
//                merge, which stores each written byte into the live array (262144 and 524288 runs of bytes)
//   real_pair_ns, real_critical_ns  recorded: nanoseconds for a Get...Elements / Release pair (JNI_ABORT) and for a critical
//                pair on an array the application made, which stays the runtime's own (the layer records the runtime's
//                pointers once it has made a block, so that a block's reference can release them)
//   empty_us     microseconds for 10^6 calls of an empty native method from a Java loop (recorded, not judged)
//   monitor      MonitorEnter and MonitorExit accept it
//   pop          PopLocalFrame hands it out of the frame
//   getline      getline grows a buffer the application allocated with malloc
//   usable_size  malloc_usable_size accepts a block the application allocated
//   libc_block   the application reallocates and frees a block libc allocated
//   table_read_only  the JNI and invoke function tables are not writable once the layer is installed
//   done         the last entry: its absence means the process did not get this far
//
// A pass is "=pass", anything else "=fail" (or "=skipped"). Reads of an address that may
// not be mapped go through a pipe, whose write fails with EFAULT instead of faulting.
//
// No Android or bionic header is included: the library is built freestanding for armv7
// with a host compiler as well as with Soong.

#include <stddef.h>
#include <stdint.h>

#include "jni_abi.hpp"

extern "C" {
int probe_open(const char*, int) __asm__("open");
int probe_close(int) __asm__("close");
long probe_read(int, void*, size_t) __asm__("read");
long probe_write(int, const void*, size_t) __asm__("write");
int probe_pipe(int[2]) __asm__("pipe");
void* probe_malloc(size_t) __asm__("malloc");
void* probe_calloc(size_t, size_t) __asm__("calloc");
void* probe_realloc(void*, size_t) __asm__("realloc");
void probe_free(void*) __asm__("free");
char* probe_strdup(const char*) __asm__("strdup");
void* probe_fopen(const char*, const char*) __asm__("fopen");
char* probe_fgets(char*, int, void*) __asm__("fgets");
int probe_sscanf(const char*, const char*, ...) __asm__("sscanf");
int probe_fclose(void*) __asm__("fclose");
long probe_getline(char**, size_t*, void*) __asm__("getline");
size_t probe_usable_size(void*) __asm__("malloc_usable_size");
int probe_pthread_create(unsigned long*, const void*, void* (*)(void*), void*) __asm__("pthread_create");
int probe_pthread_join(unsigned long, void**) __asm__("pthread_join");
int probe_clock_gettime(int, long*) __asm__("clock_gettime");  // struct timespec is two longs on bionic
void* probe_dlopen(const char*, int) __asm__("dlopen");
struct DlExtInfo {  // android_dlextinfo
    uint64_t flags;
    void* reserved_address;
    size_t reserved_size;
    int relro_fd;
    int library_fd;
    int64_t library_fd_offset;
    void* library_namespace;
};
void* probe_dlopen_ext(const char*, int, const DlExtInfo*) __asm__("android_dlopen_ext");
int probe_dlclose(void*) __asm__("dlclose");
void* probe_mmap(void*, size_t, int, int, int, long) __asm__("mmap");
void* probe_dlsym(void*, const char*) __asm__("dlsym");
}

namespace {

using namespace oldapp::jni;

constexpr int kReadOnly = 0;
constexpr const char kPayload[] = "OLD-APP-PAYLOAD-0123456789abcdef";
constexpr jint kPayloadLength = sizeof(kPayload) - 1;
constexpr int kBlocks = 64;
constexpr size_t kBlockBytes = 256u << 10;
constexpr int kRetainedResults = 520;  // more than Dalvik's 512 local references

size_t length_of(const char* text) {
    size_t n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

bool same(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool contains(const char* haystack, size_t length, const char* needle) {
    const size_t n = length_of(needle);
    for (size_t i = 0; i + n <= length; ++i) {
        if (same(haystack + i, needle, n)) {
            return true;
        }
    }
    return false;
}

// Copies `count` bytes (at most one pipe buffer) from an address that may be unmapped.
bool safe_copy(void* destination, const void* source, size_t count) {
    int fds[2];
    if (probe_pipe(fds) != 0) {
        return false;
    }
    const long wrote = probe_write(fds[1], source, count);
    bool ok = wrote == static_cast<long>(count) && probe_read(fds[0], destination, count) == static_cast<long>(count);
    probe_close(fds[0]);
    probe_close(fds[1]);
    return ok;
}

template <class F>
F entry(Env* env, int slot) {
    return reinterpret_cast<F>(const_cast<void*>(env->functions[slot]));
}

struct Report {
    char text[3072];
    size_t length = 0;
    void append(const char* piece) {
        for (size_t i = 0; piece[i] != '\0' && length + 1 < sizeof(text); ++i) {
            text[length++] = piece[i];
        }
        text[length] = '\0';
    }
    void line(const char* key, const char* value) {
        append(key);
        append("=");
        append(value);
        append(";");
    }
    void verdict(const char* key, bool pass) { line(key, pass ? "pass" : "fail"); }
};

bool shim_mapped() {
    int fd = probe_open("/proc/self/maps", kReadOnly);
    if (fd < 0) {
        return false;
    }
    static char buffer[8192 + 64];  // the chunk and the tail of the one before it
    size_t kept = 0;
    bool found = false;
    for (;;) {
        const long got = probe_read(fd, buffer + kept, 8192);
        if (got <= 0) {
            break;
        }
        const size_t total = kept + static_cast<size_t>(got);
        if (contains(buffer, total, "libkernarium_oldapp")) {
            found = true;
            break;
        }
        kept = total < 48 ? total : 48;
        for (size_t i = 0; i < kept; ++i) {
            buffer[i] = buffer[total - kept + i];
        }
    }
    probe_close(fd);
    return found;
}

void hex(char* out, uint32_t value) {  // eight digits
    for (int i = 0; i < 8; ++i) {
        out[i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
    }
    out[8] = '\0';
}

// Whether the object pointer reads as a Dalvik byte[]; `report` gets what was seen, for a reader of the log.
bool raw_read(Report& report, jobject array) {
    const char* raw = static_cast<const char*>(array);
    char text[9];
    hex(text, reinterpret_cast<uint32_t>(array));
    report.line("raw_pointer", text);
    int32_t length = 0;
    char bytes[64];
    if (!safe_copy(&length, raw + 8, sizeof(length))) {
        report.line("raw_length", "unreadable");
        return false;
    }
    hex(text, static_cast<uint32_t>(length));
    report.line("raw_length", text);
    return length == kPayloadLength && safe_copy(bytes, raw + 0x10, kPayloadLength) &&
           same(bytes, kPayload, kPayloadLength);
}

bool low_heap() {
    void* blocks[kBlocks];
    bool ok = true;
    for (int i = 0; i < kBlocks; ++i) {
        // Each block is written only up to the size it was asked for.
        size_t size = (i % 3 == 0) ? kBlockBytes : kBlockBytes / 2;
        void* block = (i % 3 == 0) ? probe_calloc(1, kBlockBytes) : probe_malloc(kBlockBytes / 2);
        if (i % 3 == 1) {
            block = probe_realloc(block, kBlockBytes);
            size = kBlockBytes;
        }
        blocks[i] = block;
        if (block == nullptr || reinterpret_cast<uintptr_t>(block) + size > 0x80000000u) {
            ok = false;
            continue;
        }
        for (size_t at = 0; at < size; at += 4096) {
            static_cast<volatile char*>(block)[at] = static_cast<char>(i);
        }
    }
    char* copy = probe_strdup(kPayload);
    ok = ok && copy != nullptr && reinterpret_cast<uintptr_t>(copy) + sizeof(kPayload) <= 0x80000000u &&
         same(copy, kPayload, sizeof(kPayload));
    probe_free(copy);
    for (int i = 0; i < kBlocks; ++i) {
        probe_free(blocks[i]);
    }
    return ok;
}

// Copies `count` bytes to an address that may be unmapped: a pipe read into it fails with EFAULT.
bool safe_store(void* destination, const void* source, size_t count) {
    int fds[2];
    if (probe_pipe(fds) != 0) {
        return false;
    }
    const long wrote = probe_write(fds[1], source, count);
    bool ok = wrote == static_cast<long>(count) && probe_read(fds[0], destination, count) == static_cast<long>(count);
    probe_close(fds[0]);
    probe_close(fds[1]);
    return ok;
}

// Whether the mapping that holds `pointer` is not writable, from /proc/self/maps.
bool mapped_read_only(const void* pointer) {
    void* file = probe_fopen("/proc/self/maps", "r");
    if (file == nullptr) {
        return false;
    }
    char line[512];
    bool found = false, read_only = false;
    while (!found && probe_fgets(line, sizeof(line), file) != nullptr) {
        unsigned low = 0, high = 0;
        char perms[5] = {};
        if (probe_sscanf(line, "%x-%x %4s", &low, &high, perms) == 3 &&
            reinterpret_cast<uintptr_t>(pointer) >= low && reinterpret_cast<uintptr_t>(pointer) < high) {
            found = true;
            read_only = perms[1] != 'w';
        }
    }
    probe_fclose(file);
    return found && read_only;
}

// What an application does with a byte[] the way a Dalvik-era application does it, each reported as one verdict.
// The array comes back from Java as a call result: ByteBuffer.wrap(original).array(), which is `original` itself.
void array_cases(Report& report, Env* env) {
    const auto find_class = entry<jclass (*)(Env*, const char*)>(env, slot::FindClass);
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetMethodID);
    const auto call_static = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_instance = entry<jobject (*)(Env*, jobject, jmethodID, ...)>(env, call_slot(kObject, kVarargs));
    const auto new_array = entry<jobject (*)(Env*, jsize)>(env, slot::NewByteArray);
    const auto set_region = entry<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(env, slot::SetByteArrayRegion);
    const auto get_region = entry<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(env, slot::GetByteArrayRegion);
    const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);

    jclass buffer_class = find_class(env, "java/nio/ByteBuffer");
    jmethodID wrap = static_method(env, buffer_class, "wrap", "([B)Ljava/nio/ByteBuffer;");
    jmethodID array_method = method(env, buffer_class, "array", "()[B");
    if (buffer_class == nullptr || wrap == nullptr || array_method == nullptr) {
        report.line("write_through", "fail");
        report.line("identity", "fail");
        report.line("retained", "fail");
        report.line("monitor", "fail");
        report.line("pop", "fail");
        return;
    }
    const auto result_of = [&](jobject original) {
        jobject buffer = call_static(env, buffer_class, wrap, original);
        jobject result = call_instance(env, buffer, array_method);
        delete_local(env, buffer);
        return result;
    };
    const auto make_original = [&](jbyte first) {
        jobject original = new_array(env, 4);
        const jbyte initial[4] = {first, 18, 19, 20};
        set_region(env, original, 0, 4, initial);
        return original;
    };

    jobject original = make_original(17);
    jobject shadow = result_of(original);
    const auto original_byte = [&](jsize index) {
        jbyte value = 0;
        get_region(env, original, index, 1, &value);
        return value;
    };

    // Writes reach the Java array.
    jbyte patch = 77;
    set_region(env, shadow, 0, 1, &patch);
    bool writes = original_byte(0) == 77;
    const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
    const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
    jbyte* elements = get_elements(env, shadow, nullptr);
    if (elements != nullptr) {
        elements[1] = 55;
        release_elements(env, shadow, elements, 0);
    }
    writes = writes && elements != nullptr && original_byte(1) == 55;
    const auto critical = entry<void* (*)(Env*, jarray, jboolean*)>(env, slot::GetPrimitiveArrayCritical);
    const auto release_critical = entry<void (*)(Env*, jarray, void*, jint)>(env, slot::ReleasePrimitiveArrayCritical);
    jbyte* bytes = static_cast<jbyte*>(critical(env, shadow, nullptr));
    if (bytes != nullptr) {
        bytes[2] = 88;
        release_critical(env, shadow, bytes, 0);
    }
    writes = writes && bytes != nullptr && original_byte(2) == 88;
    report.verdict("write_through", writes && !exception_check(env));

    // One object.
    const auto same_object = entry<jboolean (*)(Env*, jobject, jobject)>(env, slot::IsSameObject);
    const auto new_global = entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef);
    jobject global = new_global(env, shadow);
    report.verdict("identity", global != nullptr && same_object(env, shadow, original) && same_object(env, global, original) &&
                                   same_object(env, shadow, global));
    if (global != nullptr) {
        entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, global);
    }

    // The monitor.
    const auto monitor_enter = entry<jint (*)(Env*, jobject)>(env, slot::MonitorEnter);
    const auto monitor_exit = entry<jint (*)(Env*, jobject)>(env, slot::MonitorExit);
    const bool entered = monitor_enter(env, shadow) == 0;
    report.verdict("monitor", entered && monitor_exit(env, shadow) == 0 && !exception_check(env));

    // The local reference table never recycles what the application still holds.
    jobject held[kRetainedResults];
    int kept = 0;
    bool retained = true;
    for (int i = 0; i < kRetainedResults; ++i) {
        jobject next = make_original(static_cast<jbyte>(i));
        held[kept] = result_of(next);
        retained = retained && held[kept] != nullptr;
        ++kept;
        delete_local(env, next);
    }
    jbyte first_byte = 0;
    get_region(env, shadow, 0, 1, &first_byte);
    retained = retained && first_byte == 77 && array_length(env, shadow) == 4;
    for (int i = 0; i < kept; ++i) {
        jbyte value = 0;
        get_region(env, held[i], 0, 1, &value);
        retained = retained && array_length(env, held[i]) == 4 && value == static_cast<jbyte>(i);
        delete_local(env, held[i]);
    }
    report.verdict("retained", retained && !exception_check(env));

    // Java writes into the array the native side passed it; the native side reads what Java wrote.
    {
        jclass system_class = find_class(env, "java/lang/System");
        jmethodID arraycopy = static_method(env, system_class, "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V");
        jobject source = new_array(env, 4);
        const jbyte filled[4] = {1, 2, 3, 4};
        set_region(env, source, 0, 4, filled);
        jobject target = result_of(make_original(9));
        bool region_ok = false, elements_ok = false, raw_ok = false;
        if (system_class != nullptr && arraycopy != nullptr && target != nullptr) {
            entry<void (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kVoid, kVarargs))(
                env, system_class, arraycopy, source, 0, target, 0, 4);
            jbyte seen[4] = {};
            get_region(env, target, 0, 4, seen);
            region_ok = same(reinterpret_cast<const char*>(seen), reinterpret_cast<const char*>(filled), 4);
            jbyte* elements = get_elements(env, target, nullptr);
            elements_ok = elements != nullptr && same(reinterpret_cast<const char*>(elements), reinterpret_cast<const char*>(filled), 4);
            if (elements != nullptr) {
                release_elements(env, target, elements, kReleaseAbort);
            }
            char raw_bytes[4] = {};
            raw_ok = safe_copy(raw_bytes, static_cast<const char*>(target) + 0x10, 4) &&
                     same(raw_bytes, reinterpret_cast<const char*>(filled), 4);
        }
        report.verdict("java_fill", region_ok && elements_ok && !exception_check(env));
        report.verdict("java_fill_raw", raw_ok);
        delete_local(env, target);
        delete_local(env, source);
    }

    // The native side writes through the object pointer and never releases; Java reads the array.
    {
        jobject target = result_of(make_original(9));
        if (!shim_mapped()) {
            report.line("raw_write_java", "skipped");
        } else {
            jclass arrays_class = find_class(env, "java/util/Arrays");
            jmethodID hash_code = static_method(env, arrays_class, "hashCode", "([B)I");
            const char fill[4] = {0x21, 0x22, 0x23, 0x24};
            uint32_t expected = 1;
            for (char byte : fill) {
                expected = 31u * expected + static_cast<uint32_t>(static_cast<int32_t>(byte));
            }
            const bool stored = target != nullptr && safe_store(static_cast<char*>(target) + 0x10, fill, 4);
            jint got = 0;
            if (stored && arrays_class != nullptr && hash_code != nullptr) {
                got = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs))(
                    env, arrays_class, hash_code, target);
            }
            char after[4] = {};
            const bool kept = stored && safe_copy(after, static_cast<const char*>(target) + 0x10, 4) && same(after, fill, 4);
            report.verdict("raw_write_java", stored && static_cast<uint32_t>(got) == expected && kept && !exception_check(env));
        }
        delete_local(env, target);
    }

    // Conformant JNI, the runtime's own way: the array open through its own critical pointer and through the
    // block's at once, a write through each in a different chunk, both released.
    {
        jobject big = new_array(env, 8192);
        jobject target = big != nullptr ? result_of(big) : nullptr;
        bool chunks = false;
        if (target != nullptr) {
            jbyte* real = static_cast<jbyte*>(critical(env, big, nullptr));
            jbyte* block = static_cast<jbyte*>(critical(env, target, nullptr));
            if (real != nullptr && block != nullptr) {
                real[4096] = 0x41;
                block[0] = 0x77;
                release_critical(env, target, block, 0);
                release_critical(env, big, real, 0);
                jbyte seen[2] = {};
                get_region(env, big, 0, 1, seen);
                get_region(env, big, 4096, 1, seen + 1);
                chunks = seen[0] == 0x77 && seen[1] == 0x41;
            }
        }
        report.verdict("commit_chunks", chunks && !exception_check(env));
        if (target != nullptr) {
            delete_local(env, target);
        }
        delete_local(env, big);
    }

    // clone() of the array, with a write made through the pointer Get...Elements returned.
    {
        jobject small = new_array(env, 8);
        jobject target = small != nullptr ? result_of(small) : nullptr;
        bool cloned = false;
        if (target != nullptr) {
            jclass array_class = entry<jclass (*)(Env*, jobject)>(env, slot::GetObjectClass)(env, small);
            jmethodID clone = array_class != nullptr ? method(env, array_class, "clone", "()Ljava/lang/Object;") : nullptr;
            jboolean copy = 1;
            jbyte* bytes = get_elements(env, target, &copy);
            if (bytes != nullptr && clone != nullptr) {
                bytes[0] = 0x77;
                if (copy != 0) {
                    release_elements(env, target, bytes, 0);
                }
                jobject copied = call_instance(env, target, clone);
                jbyte seen = 0;
                if (copied != nullptr) {
                    get_region(env, copied, 0, 1, &seen);
                    delete_local(env, copied);
                }
                cloned = seen == 0x77;
                if (copy == 0) {
                    release_elements(env, target, bytes, 0);
                }
            }
        }
        report.verdict("clone", cloned && !exception_check(env));
        if (target != nullptr) {
            delete_local(env, target);
        }
        delete_local(env, small);
    }

    // A second, real reference to the same array, and the flag the pointers carry: an application that commits when
    // the pointer says it is a copy has to see the write through the other reference.
    {
        jobject small = new_array(env, 8);
        jobject target = small != nullptr ? result_of(small) : nullptr;
        bool aliased = false;
        if (target != nullptr) {
            jboolean copy = 1;
            jbyte* bytes = get_elements(env, target, &copy);
            if (bytes != nullptr) {
                bytes[0] = 0x77;
                if (copy != 0) {
                    release_elements(env, target, bytes, 0);
                }
                jbyte seen = 0;
                get_region(env, small, 0, 1, &seen);
                aliased = seen == 0x77;
                if (copy == 0) {
                    release_elements(env, target, bytes, 0);
                }
            }
        }
        report.verdict("alias_elements", aliased && !exception_check(env));
        if (target != nullptr) {
            delete_local(env, target);
        }
        delete_local(env, small);
    }
    {
        jobject big = new_array(env, 8192);
        jobject target = big != nullptr ? result_of(big) : nullptr;
        bool aliased = false;
        if (target != nullptr) {
            jboolean pinned_copy = 0;
            jbyte* real = static_cast<jbyte*>(critical(env, big, &pinned_copy));
            jboolean copy = 1;
            jbyte* block = static_cast<jbyte*>(critical(env, target, &copy));
            if (real != nullptr && block != nullptr) {
                block[0] = 0x77;
                if (copy != 0) {
                    release_critical(env, target, block, 0);
                }
                // A pinned pointer that is itself a copy (a debuggable application) only shows the write after its release.
                const bool direct_sees = pinned_copy != 0 || real[0] == 0x77;
                release_critical(env, big, real, 2);  // JNI_ABORT: the pinned pointer made no write
                jbyte seen = 0;
                if (copy == 0) {
                    release_critical(env, target, block, 0);
                }
                get_region(env, big, 0, 1, &seen);
                aliased = direct_sees && seen == 0x77;
            }
        }
        report.verdict("alias_critical", aliased && !exception_check(env));
        if (target != nullptr) {
            delete_local(env, target);
        }
        delete_local(env, big);
    }

    // PopLocalFrame.
    entry<jint (*)(Env*, jint)>(env, slot::PushLocalFrame)(env, 32);
    jobject in_frame = result_of(original);
    jobject outside = entry<jobject (*)(Env*, jobject)>(env, slot::PopLocalFrame)(env, in_frame);
    report.verdict("pop", outside != nullptr && array_length(env, outside) == 4 && !exception_check(env));
    delete_local(env, outside);
    delete_local(env, shadow);
    delete_local(env, original);
}

// getline and malloc_usable_size on blocks the application allocated.
void allocator_cases(Report& report) {
    size_t size = 16;
    char* buffer = static_cast<char*>(probe_malloc(size));
    void* file = probe_fopen("/proc/self/maps", "r");
    const long got = file != nullptr ? probe_getline(&buffer, &size, file) : -1;
    report.verdict("getline", got > 16);
    if (file != nullptr) {
        probe_fclose(file);
    }
    probe_free(buffer);
    void* block = probe_malloc(32);
    report.verdict("usable_size", probe_usable_size(block) >= 32);
    probe_free(block);

    // The other direction: getline allocates the line, the application grows and frees it.
    char* line = nullptr;
    size_t line_size = 0;
    void* maps = probe_fopen("/proc/self/maps", "r");
    const long read = maps != nullptr ? probe_getline(&line, &line_size, maps) : -1;
    bool ok = read > 0 && line != nullptr;
    const char first = ok ? line[0] : 0;
    char* bigger = ok ? static_cast<char*>(probe_realloc(line, 4096)) : nullptr;
    ok = ok && bigger != nullptr && bigger[0] == first;
    probe_free(bigger != nullptr ? bigger : line);
    if (maps != nullptr) {
        probe_fclose(maps);
    }
    report.verdict("libc_block", ok);
}

struct ThreadWork {
    Vm* vm;
    jclass probe;  // a global reference
    jmethodID payload;
    bool ok;
};

void* thread_main(void* argument) {
    ThreadWork* work = static_cast<ThreadWork*>(argument);
    Env* env = nullptr;
    if (entry<jint (*)(Vm*, void**, void*)>(reinterpret_cast<Env*>(work->vm), vmslot::AttachCurrentThread)(
            work->vm, reinterpret_cast<void**>(&env), nullptr) != 0) {
        return nullptr;
    }
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    jobject array = call_object(env, work->probe, work->payload);
    if (array != nullptr) {
        Report scratch;
        work->ok = raw_read(scratch, array);
        delete_local(env, array);
    }
    entry<jint (*)(Vm*)>(reinterpret_cast<Env*>(work->vm), vmslot::DetachCurrentThread)(work->vm);
    return nullptr;
}

// The raw read from a thread of its own: the layer keeps its state for each thread.
bool thread_raw_read(Env* env, jclass probe, jmethodID payload) {
    Vm* vm = nullptr;
    if (entry<jint (*)(Env*, Vm**)>(env, 219)(env, &vm) != 0) {  // GetJavaVM
        return false;
    }
    ThreadWork work{vm, static_cast<jclass>(entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef)(env, probe)),
                    payload, false};
    unsigned long thread = 0;
    const bool started = probe_pthread_create(&thread, nullptr, thread_main, &work) == 0;
    if (started) {
        probe_pthread_join(thread, nullptr);
    }
    entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, work.probe);
    return started && work.ok;
}


// ---- the end of a native method

jmethodID g_payload_id = nullptr;  // Probe.payload()[B
jmethodID g_recurse_id = nullptr;  // Probe.recurse(I)I
jint g_retv = 0;                   // what retV(I)V stored
bool g_onload_made = false;        // JNI_OnLoad got its byte[] results
constexpr int kTicks = 600;        // past Dalvik's 512 local references
constexpr int kOnLoadBlocks = 400;
constexpr jint kAbi0 = 0x1234abc;

void decimal(char* out, uint32_t value) {
    char digits[11];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (int i = 0; i < n; ++i) {
        out[i] = digits[n - 1 - i];
    }
    out[n] = '\0';
}

uint64_t now_us() {
    long t[2] = {0, 0};
    probe_clock_gettime(1, t);  // CLOCK_MONOTONIC
    return static_cast<uint64_t>(t[0]) * 1000000u + static_cast<uint64_t>(t[1]) / 1000u;
}

// What a game's native method does: it gets a byte[] from Java, reads it at the Dalvik offsets and never deletes it
// (Dalvik freed it when the method returned).
jint tick_body(Env* env, jclass probe) {
    // The payload method is looked up here, not taken from a global: the row ends_outside runs this function in a
    // second copy of the library, whose globals nothing has set.
    const jmethodID payload =
        entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID)(env, probe, "payload", "()[B");
    if (payload == nullptr) {
        return 0;
    }
    jobject array = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs))(env, probe, payload);
    Report scratch;
    return array != nullptr && raw_read(scratch, array) ? 1 : 0;
}

// A level that calls Java, which calls the native method again: the block is read before and after the call below.
jint recurse_body(Env* env, jclass probe, jint depth) {
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);
    jobject array = call_object(env, probe, g_payload_id);
    Report scratch;
    bool ok = array != nullptr && raw_read(scratch, array);
    jint inner = depth > 0 ? call_int(env, probe, g_recurse_id, depth - 1) : 0;
    ok = ok && inner >= 0 && !exception_check(env) && array_length(env, array) == kPayloadLength && raw_read(scratch, array);
    return ok ? inner + 1 : -1;
}

jlong abi_sum(jint a, jlong b, jdouble c, jfloat d, jint e, jlong f, jdouble g, jfloat h, jint i, jint j, jint k, jint l) {
    // The conversions go through jint: a conversion to a 64-bit integer is a library call, and this library links none.
    return jlong{a} + 3 * b + jlong{static_cast<jint>(c * 8.0)} + jlong{static_cast<jint>(d * 16.0f)} + 5 * jlong{e} + 7 * f +
           jlong{static_cast<jint>(g * 32.0)} + jlong{static_cast<jint>(h * 64.0f)} + 11 * jlong{i} + 13 * jlong{j} +
           17 * jlong{k} + 19 * jlong{l};
}

jint tick_reg(Env* env, jclass probe) { return tick_body(env, probe); }
__attribute__((target("arm"))) jint tick_reg_arm(Env* env, jclass probe) { return tick_body(env, probe); }
__attribute__((target("arm"))) jlong abi12_arm(Env*, jclass, jint a, jlong b, jdouble c, jfloat d, jint e, jlong f, jdouble g,
                                               jfloat h, jint i, jint j, jint k, jint l) {
    return abi_sum(a, b, c, d, e, f, g, h, i, j, k, l);
}

bool abi_args(Env* env, jclass probe) {
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto call_long = entry<jlong (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kLong, kVarargs));
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);
    const char* const signature = "(IJDFIJDFIIII)J";
    jmethodID zero = static_method(env, probe, "abi0", "()I");
    jmethodID thumb = static_method(env, probe, "abi12", signature);
    jmethodID arm = static_method(env, probe, "abi12Arm", signature);
    if (zero == nullptr || thumb == nullptr || arm == nullptr) {
        return false;
    }
    const jlong expected = abi_sum(7, 0x100000001LL, 2.5, 3.25f, -9, -0x200000003LL, 6.75, 0.5f, 100, -200, 300, -400);
    bool ok = call_int(env, probe, zero) == kAbi0;
    jmethodID methods[2] = {thumb, arm};
    for (jmethodID method : methods) {
        ok = ok && call_long(env, probe, method, 7, 0x100000001LL, 2.5, 3.25f, -9, -0x200000003LL, 6.75, 0.5f, 100, -200,
                             300, -400) == expected;
    }
    return ok && !exception_check(env);
}

bool abi_returns(Env* env, jclass probe) {
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);
    const auto same_object = entry<jboolean (*)(Env*, jobject, jobject)>(env, slot::IsSameObject);
    const auto new_string = entry<jstring (*)(Env*, const char*)>(env, slot::NewStringUTF);
    jmethodID z = static_method(env, probe, "retZ", "(Z)Z"), b = static_method(env, probe, "retB", "(B)B"),
              c = static_method(env, probe, "retC", "(C)C"), s = static_method(env, probe, "retS", "(S)S"),
              i = static_method(env, probe, "retI", "(I)I"), j = static_method(env, probe, "retJ", "(J)J"),
              f = static_method(env, probe, "retF", "(F)F"), d = static_method(env, probe, "retD", "(D)D"),
              l = static_method(env, probe, "retL", "(Ljava/lang/String;)Ljava/lang/String;"),
              v = static_method(env, probe, "retV", "(I)V"), r = static_method(env, probe, "readV", "()I");
    if (!z || !b || !c || !s || !i || !j || !f || !d || !l || !v || !r) {
        return false;
    }
    bool ok = entry<jboolean (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kBoolean, kVarargs))(env, probe, z, 1) == 1;
    ok = ok && entry<jbyte (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kByte, kVarargs))(env, probe, b, -5) == -5;
    ok = ok && entry<jchar (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kChar, kVarargs))(env, probe, c, 0xabcd) == 0xabcd;
    ok = ok && entry<jshort (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kShort, kVarargs))(env, probe, s, -1234) == -1234;
    ok = ok && entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs))(env, probe, i, 0x12345678) == 0x12345678;
    ok = ok && entry<jlong (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kLong, kVarargs))(
                   env, probe, j, 0x123456789abcdefLL) == 0x123456789abcdefLL;
    ok = ok && entry<jfloat (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kFloat, kVarargs))(env, probe, f, 1.5f) == 1.5f;
    ok = ok && entry<jdouble (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kDouble, kVarargs))(env, probe, d, 2.25) == 2.25;
    jstring text = new_string(env, "thunk");
    jobject echoed = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs))(env, probe, l, text);
    ok = ok && echoed != nullptr && same_object(env, echoed, text);
    entry<void (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kVoid, kVarargs))(env, probe, v, 91);
    ok = ok && entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs))(env, probe, r) == 91;
    return ok && !exception_check(env);
}

// ends_outside: the native method tickReg is registered again with the tick function of another copy of this library
// (a package other than the application's own: the layer gives its methods no thunk). A call whose block is not ended
// at its return piles its blocks up in the frame of Probe.run, so the result past the thread's limit is the
// runtime's own reference and its raw read fails. The row passes when the calls work for a while and then stop: with a
// thunk they would all work, with a library that cannot run at all the first would fail.
void outside_case(Report& report, Env* env, jclass probe, const char* path) {
    if (path == nullptr || path[0] != '/') {
        report.line("ends_outside", "skipped");
        return;
    }
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto register_natives = entry<jint (*)(Env*, jclass, const NativeMethod*, jint)>(env, slot::RegisterNatives);
    const auto exception_clear = entry<void (*)(Env*)>(env, slot::ExceptionClear);
    void* handle = probe_dlopen(path, 2);  // RTLD_NOW
    void* function = handle != nullptr ? probe_dlsym(handle, "Java_org_example_andarium_oldappprobe_Probe_tick") : nullptr;
    const jmethodID tick = static_method(env, probe, "tickReg", "()I");
    int stopped_at = -1;
    bool registered = false;
    // The blocks the calls pile up in this frame end with a local frame of their own, so the cases after this one start
    // from a thread with no live references.
    const auto push_frame = entry<jint (*)(Env*, jint)>(env, slot::PushLocalFrame);
    const auto pop_frame = entry<jobject (*)(Env*, jobject)>(env, slot::PopLocalFrame);
    const bool framed = push_frame(env, 1024) == 0;
    if (function != nullptr && tick != nullptr) {
        const NativeMethod method{"tickReg", "()I", function};
        registered = register_natives(env, probe, &method, 1) == 0;
        for (int i = 0; registered && i < kTicks; ++i) {
            if (call_int(env, probe, tick) != 1) {
                stopped_at = i;
                break;
            }
        }
    }
    exception_clear(env);
    if (framed) {
        pop_frame(env, nullptr);
    }
    char text[12] = "none";
    if (stopped_at >= 0) {
        decimal(text, static_cast<uint32_t>(stopped_at));
    }
    report.line("outside_stopped_at", text);
    report.verdict("ends_outside", stopped_at > 0);
}

// bind_swap: the loader's list changes under an address. A library of another package is opened in a reserved range, makes
// a JNI call (the layer learns the range as another package's) and is closed; the application's own library is opened in
// the same range and its native method tickReg registered. The binding is decided from the loader's list as it is.
bool split_paths(const char* options, char* foreign, size_t capacity, char* own) {
    size_t n = 0;
    while (options[n] != '\0' && options[n] != '|' && n + 1 < capacity) {
        foreign[n] = options[n];
        ++n;
    }
    foreign[n] = '\0';
    if (options[n] != '|') {
        return false;
    }
    size_t k = 0;
    ++n;
    while (options[n] != '\0' && k + 1 < capacity) {
        own[k++] = options[n++];
    }
    own[k] = '\0';
    return foreign[0] == '/' && own[0] == '/';
}

void swap_case(Report& report, Env* env, jclass probe, const char* paths) {
    constexpr size_t kReserve = 1u << 20;
    constexpr int kCalls = 600;
    char foreign[512], own[512];
    if (!split_paths(paths, foreign, sizeof(foreign), own)) {
        report.line("bind_swap", "skipped");
        return;
    }
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto register_natives = entry<jint (*)(Env*, jclass, const NativeMethod*, jint)>(env, slot::RegisterNatives);
    const auto exception_clear = entry<void (*)(Env*)>(env, slot::ExceptionClear);
    const auto push_frame = entry<jint (*)(Env*, jint)>(env, slot::PushLocalFrame);
    const auto pop_frame = entry<jobject (*)(Env*, jobject)>(env, slot::PopLocalFrame);
    const char* stage = "reserve";
    int good = 0;
    bool same_place = false;
    void* reserved = probe_mmap(nullptr, kReserve, 0, 0x22, -1, 0);  // PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS
    const DlExtInfo ext{1, reserved, kReserve, -1, -1, 0, nullptr};  // ANDROID_DLEXT_RESERVED_ADDRESS
    if (reserved != nullptr && reserved != reinterpret_cast<void*>(-1)) {
        stage = "foreign";
        void* first = probe_dlopen_ext(foreign, 2, &ext);
        void* first_tick = first != nullptr ? probe_dlsym(first, "Java_org_example_andarium_oldappprobe_Probe_tick") : nullptr;
        if (first_tick != nullptr) {
            stage = "call";
            const jint called = reinterpret_cast<jint (*)(Env*, jclass)>(first_tick)(env, probe);
            exception_clear(env);
            if (called == 1) {
                stage = "close";
                if (probe_dlclose(first) == 0 &&
                    probe_mmap(reserved, kReserve, 0, 0x32, -1, 0) == reserved) {  // MAP_FIXED: the range is ours again
                    stage = "own";
                    void* second = probe_dlopen_ext(own, 2, &ext);
                    void* second_tick = second != nullptr ? probe_dlsym(second, "Java_org_example_andarium_oldappprobe_Probe_tick")
                                                          : nullptr;
                    same_place = second_tick == first_tick;
                    const jmethodID tick = static_method(env, probe, "tickReg", "()I");
                    if (second_tick != nullptr && same_place && tick != nullptr) {
                        stage = "register";
                        const NativeMethod method{"tickReg", "()I", second_tick};
                        const bool framed = push_frame(env, 1024) == 0;
                        if (register_natives(env, probe, &method, 1) == 0) {
                            stage = "calls";
                            for (; good < kCalls; ++good) {
                                if (call_int(env, probe, tick) != 1) {
                                    break;
                                }
                            }
                        }
                        exception_clear(env);
                        if (framed) {
                            pop_frame(env, nullptr);
                        }
                    }
                }
            }
        }
    }
    char text[12];
    decimal(text, static_cast<uint32_t>(good));
    report.line("bind_swap_good", text);
    report.line("bind_swap_same_address", same_place ? "yes" : "no");
    report.line("bind_swap_stage", stage);
    report.verdict("bind_swap", good == kCalls && same_place);
}

// ---- the application scene (the options "scene")

constexpr int kSceneFrames = 800;
constexpr int kSceneInitResults = 170;
constexpr int kSceneBurstEvery = 100;  // a burst at frame % 100 == 60
constexpr uint32_t kSceneMinimumResults = 5000;
constexpr uint32_t kSceneMinimumBursts = 5;

struct Scene {
    uint32_t rng = 0x2545F491u;
    jmethodID asset = nullptr;  // Probe.asset(II)[B
    jmethodID peek = nullptr;   // Probe.peek([BI)I
    bool layer = false;         // the layer is mapped: results are read through the object pointer too
    uint32_t results = 0;
    uint32_t bursts = 0;
    uint64_t bytes = 0;
    uint32_t failed_at = 0;  // the number of the first result that did not read right
    uint32_t failure = 0;    // which check
    uint32_t write_failed_at = 0;
    uint32_t largest = 0;
    int frames = 0;         // the frames of the loop that is running
    jint status = 0;        // what the loop returned (the Java thread reports it through sceneDone)
    bool finished = false;  // sceneDone was called
};
Scene g_scene;

uint32_t scene_random() {
    uint32_t x = g_scene.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_scene.rng = x;
    return x;
}

// The lengths of an asset reader: most are small, a few are large. 16 bytes to 320 KB.
uint32_t scene_length() {
    const uint32_t pick = scene_random() % 1000;
    const uint32_t spread = scene_random();
    if (pick < 700) return 16 + spread % 1985;
    if (pick < 950) return 2000 + spread % 38000;
    if (pick < 995) return 40000 + spread % 80000;
    return 120000 + spread % 200000;
}

bool all_equal(const uint8_t* bytes, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

bool scene_fail(uint32_t code) {
    if (g_scene.failed_at == 0) {
        g_scene.failed_at = g_scene.results;
        g_scene.failure = code;
    }
    return false;
}

// One result of the asset reader: a fresh array of a seeded length, never deleted.
bool scene_result(Env* env, jclass probe) {
    Scene& scene = g_scene;
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
    const auto get_region = entry<void (*)(Env*, jarray, jsize, jsize, void*)>(env, slot::GetByteArrayRegion);
    const auto set_region = entry<void (*)(Env*, jarray, jsize, jsize, const void*)>(env, slot::SetByteArrayRegion);
    const uint32_t n = scene_length();
    const uint8_t value = static_cast<uint8_t>(1 + scene_random() % 250);
    jobject array = call_object(env, probe, scene.asset, static_cast<jint>(n), static_cast<jint>(value));
    ++scene.results;
    scene.bytes += n;
    scene.largest = n > scene.largest ? n : scene.largest;
    if (array == nullptr) {
        return scene_fail(1);
    }
    if (static_cast<uint32_t>(array_length(env, array)) != n) {
        return scene_fail(2);
    }
    const uint32_t samples[3] = {0, n / 2 - 8, n - 16};
    uint8_t seen[16];
    for (uint32_t at : samples) {
        get_region(env, array, static_cast<jsize>(at), 16, seen);
        if (!all_equal(seen, 16, value)) {
            return scene_fail(3);
        }
    }
    const char* raw = static_cast<const char*>(array);
    if (scene.layer) {
        uint8_t head[32];
        int32_t length = 0;
        if (!safe_copy(head, raw, sizeof(head))) {
            return scene_fail(4);  // the result is no block: the application would read from a reference
        }
        __builtin_memcpy(&length, head + 8, sizeof(length));
        if (static_cast<uint32_t>(length) != n) {
            return scene_fail(5);
        }
        if (!all_equal(head + 16, 16, value)) {
            return scene_fail(6);
        }
        const uint32_t tails[2] = {n / 2 - 8, n - 16};
        for (uint32_t at : tails) {
            if (!safe_copy(seen, raw + 16 + at, 16) || !all_equal(seen, 16, value)) {
                return scene_fail(7);
            }
        }
    }
    if ((scene.results & 7) == 0) {
        const uint32_t at = n / 3;
        const uint8_t marks[4] = {static_cast<uint8_t>(value ^ 0x5a), static_cast<uint8_t>(value ^ 0xa5),
                                  static_cast<uint8_t>(value ^ 0x33), static_cast<uint8_t>(value ^ 0xcc)};
        if (scene.layer) {
            if (!safe_store(const_cast<char*>(raw) + 16 + at, marks, sizeof(marks))) {
                return scene_fail(8);
            }
        } else {
            set_region(env, array, static_cast<jsize>(at), 4, marks);
        }
        for (uint32_t k = 0; k < 4; ++k) {
            if (static_cast<uint8_t>(call_int(env, probe, scene.peek, array, static_cast<jint>(at + k))) != marks[k]) {
                if (scene.write_failed_at == 0) {
                    scene.write_failed_at = scene.results;
                }
            }
        }
    }
    return true;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_sceneFrames(Env*,
                                                                                                             jclass) {
    return g_scene.frames;
}

extern "C" __attribute__((visibility("default"))) void Java_org_example_andarium_oldappprobe_Probe_sceneDone(Env*, jclass,
                                                                                                           jint status) {
    g_scene.status = status;
    g_scene.finished = true;
}

extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_sceneInit(Env* env,
                                                                                                           jclass probe) {
    bool ok = g_scene.failed_at == 0;
    for (int i = 0; ok && i < kSceneInitResults; ++i) {
        ok = scene_result(env, probe);
    }
    return ok ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_sceneFrame(
    Env* env, jclass probe, jint frame) {
    if (g_scene.failed_at != 0) {
        return 0;
    }
    const bool burst = frame % kSceneBurstEvery == 60;
    const uint32_t count = burst ? 100 + scene_random() % 51 : 4 + scene_random() % 5;
    g_scene.bursts += burst ? 1 : 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = scene_result(env, probe);
    }
    return ok ? 1 : 0;
}

namespace {

struct SceneWork {
    Vm* vm;
    jclass probe;  // a global reference
    jmethodID scene;
    jint status;
};

// The thread of the attached variant: a native thread that attaches itself, runs the Java loop and detaches.
void* scene_thread_main(void* argument) {
    SceneWork* work = static_cast<SceneWork*>(argument);
    Env* env = nullptr;
    if (entry<jint (*)(Vm*, void**, void*)>(reinterpret_cast<Env*>(work->vm), vmslot::AttachCurrentThread)(
            work->vm, reinterpret_cast<void**>(&env), nullptr) != 0) {
        return nullptr;
    }
    work->status = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs))(
        env, work->probe, work->scene, g_scene.frames);
    entry<jint (*)(Vm*)>(reinterpret_cast<Env*>(work->vm), vmslot::DetachCurrentThread)(work->vm);
    return nullptr;
}

constexpr int kAttachedFrames = 100;
constexpr uint32_t kAttachedMinimumResults = 500;

// One run of the scene. The Java thread (`attached` false): Probe.render() starts a java.lang.Thread whose run loop is
// scene(sceneFrames()) and joins it; the thread reports its status through sceneDone. The attached variant: a native
// thread attaches itself and calls scene(frames).
struct SceneRun {
    bool started = false;
    jint status = 0;
    uint32_t elapsed_ms = 0;
};

SceneRun run_scene(Env* env, jclass probe, bool attached) {
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    g_scene = Scene{};
    g_scene.asset = static_method(env, probe, "asset", "(II)[B");
    g_scene.peek = static_method(env, probe, "peek", "([BI)I");
    g_scene.layer = shim_mapped();
    g_scene.frames = attached ? kAttachedFrames : kSceneFrames;
    SceneRun run;
    const uint64_t start = now_us();
    if (g_scene.asset != nullptr && g_scene.peek != nullptr) {
        if (attached) {
            SceneWork work{nullptr, nullptr, static_method(env, probe, "scene", "(I)I"), 0};
            if (work.scene != nullptr && entry<jint (*)(Env*, Vm**)>(env, 219)(env, &work.vm) == 0) {  // GetJavaVM
                work.probe = static_cast<jclass>(entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef)(env, probe));
                unsigned long thread = 0;
                run.started = probe_pthread_create(&thread, nullptr, scene_thread_main, &work) == 0;
                if (run.started) {
                    probe_pthread_join(thread, nullptr);
                }
                entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, work.probe);
                run.status = work.status;
            }
        } else {
            const jmethodID render = static_method(env, probe, "render", "()V");
            if (render != nullptr) {
                entry<void (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kVoid, kVarargs))(env, probe, render);
                run.started = g_scene.finished;
                run.status = g_scene.status;
            }
        }
    }
    run.elapsed_ms = static_cast<uint32_t>((now_us() - start) / 1000);
    return run;
}

// The scene on a thread of its own, as a render thread: the rows scene_complete, scene_data and scene_write (a thread
// Java started) and scene_attached (a thread the probe attached).
void scene_cases(Report& report, Env* env, jclass probe) {
    char text[12];
    const SceneRun run = run_scene(env, probe, false);
    decimal(text, run.elapsed_ms);
    report.line("scene_ms", text);
    decimal(text, g_scene.results);
    report.line("scene_results", text);
    decimal(text, g_scene.bursts);
    report.line("scene_bursts", text);
    decimal(text, static_cast<uint32_t>(g_scene.bytes >> 10));
    report.line("scene_kib", text);
    decimal(text, g_scene.largest);
    report.line("scene_largest", text);
    decimal(text, g_scene.failed_at);
    report.line("scene_failed_at", text);
    decimal(text, g_scene.failure);
    report.line("scene_failure", text);
    report.verdict("scene_complete", run.started && run.status == 1 && g_scene.results >= kSceneMinimumResults &&
                                         g_scene.bursts >= kSceneMinimumBursts);
    report.verdict("scene_data", run.started && g_scene.failed_at == 0);
    report.verdict("scene_write", run.started && g_scene.write_failed_at == 0);

    const SceneRun attached = run_scene(env, probe, true);
    decimal(text, attached.elapsed_ms);
    report.line("scene_attached_ms", text);
    decimal(text, g_scene.results);
    report.line("scene_attached_results", text);
    decimal(text, g_scene.failed_at);
    report.line("scene_attached_failed_at", text);
    report.verdict("scene_attached", attached.started && attached.status == 1 && g_scene.failed_at == 0 &&
                                         g_scene.write_failed_at == 0 && g_scene.results >= kAttachedMinimumResults &&
                                         g_scene.bursts >= 1);
}

// ---- ended blocks (the rows ended_*)

struct EndedIds {
    jmethodID asset, same, poke, peek, collect, payload;
};

// Whether the object pointer reads as a block of `n` bytes that all hold `value`: the length word, and samples.
bool block_holds(const void* block, uint32_t n, uint8_t value) {
    const char* raw = static_cast<const char*>(block);
    uint8_t head[32], seen[16];
    int32_t length = 0;
    if (!safe_copy(head, raw, sizeof(head))) {
        return false;
    }
    __builtin_memcpy(&length, head + 8, sizeof(length));
    if (static_cast<uint32_t>(length) != n || !all_equal(head + 16, 16, value)) {
        return false;
    }
    const uint32_t samples[2] = {n / 2 - 8, n - 16};
    for (uint32_t at : samples) {
        if (!safe_copy(seen, raw + 16 + at, 16) || !all_equal(seen, 16, value)) {
            return false;
        }
    }
    return true;
}

uint8_t byte_at(const void* block, uint32_t at) {
    uint8_t value = 0;
    safe_copy(&value, static_cast<const char*>(block) + 16 + at, 1);
    return value;
}

void ended_cases(Report& report, Env* env, jclass probe) {
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto call_void = entry<void (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kVoid, kVarargs));
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    const auto new_global = entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef);
    const auto delete_global = entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef);
    const auto exception_clear = entry<void (*)(Env*)>(env, slot::ExceptionClear);
    const EndedIds ids{static_method(env, probe, "asset", "(II)[B"), static_method(env, probe, "same", "([B)[B"),
                       static_method(env, probe, "poke", "([BII)V"), static_method(env, probe, "peek", "([BI)I"),
                       static_method(env, probe, "collect", "()V"), static_method(env, probe, "payload", "()[B")};
    if (!ids.asset || !ids.same || !ids.poke || !ids.peek || !ids.collect || !ids.payload) {
        report.line("ended_keep", "fail");
        report.line("ended_arrays", "fail");
        report.line("ended_pin", "fail");
        report.line("ended_kept_ref", "fail");
        return;
    }

    // An array Java holds, its pointer kept; a thousand temporaries; the pointer still reads the array.
    {
        constexpr uint32_t n = 65536;
        unsigned failed = 0;  // the first check that did not hold (recorded)
        const auto check = [&](unsigned code, bool condition) {
            if (!condition && failed == 0) {
                failed = code;
            }
        };
        jobject block = call_object(env, probe, ids.asset, static_cast<jint>(n), 0x11);
        const void* kept = block;
        check(1, block != nullptr && block_holds(block, n, 0x11));
        jobject held = block != nullptr ? new_global(env, block) : nullptr;  // the array, held: Java's reference
        if (block != nullptr) {
            delete_local(env, block);  // the block's last reference ends
        }
        const void* early[8] = {};
        bool reused = false;
        for (int i = 0; i < 1000; ++i) {
            jobject temporary = call_object(env, probe, ids.asset, static_cast<jint>(n), 0x20 + (i & 31));
            check(2, temporary != nullptr && temporary != kept);
            for (const void* before : early) {
                reused = reused || (before != nullptr && before == temporary);
            }
            if (i < 8) {
                early[i] = temporary;
            }
            if (temporary != nullptr) {
                delete_local(env, temporary);
            }
            if (i % 100 == 99) {
                call_void(env, probe, ids.collect);
            }
        }
        check(3, block_holds(kept, n, 0x11));  // the kept pointer still reads the array's bytes as of the end
        call_void(env, probe, ids.poke, held, 100, 0x22);  // Java changes a byte
        const uint8_t written = 0x33;
        check(4, safe_store(const_cast<char*>(static_cast<const char*>(kept)) + 16 + 5000, &written, 1));  // another 4 KiB chunk
        jobject again = call_object(env, probe, ids.same, held);  // the array obtained again
        check(5, again == kept);
        check(6, byte_at(again, 100) == 0x22 && byte_at(again, 5000) == 0x33);
        check(7, call_int(env, probe, ids.peek, held, 5000) == 0x33 && call_int(env, probe, ids.peek, held, 100) == 0x22);
        if (again != nullptr) {
            delete_local(env, again);
        }
        if (held != nullptr) {
            delete_global(env, held);
        }
        check(8, !entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck)(env));
        exception_clear(env);
        report.verdict("ended_keep", failed == 0);
        char code[12];
        decimal(code, failed);
        report.line("ended_keep_failed_check", code);
        report.line("ended_reuse", reused ? "yes" : "no");
    }

    // 64 arrays Java holds, each obtained again and again: its own block, always.
    {
        constexpr int kArrays = 64;
        constexpr int kRounds = 100;
        constexpr uint32_t n = 300;
        const void* at[kArrays] = {};
        jobject blocks[kArrays] = {};
        jobject held[kArrays] = {};
        bool ok = true;
        for (int i = 0; i < kArrays; ++i) {  // all live at once: 64 addresses
            blocks[i] = call_object(env, probe, ids.asset, static_cast<jint>(n), 1 + i);
            at[i] = blocks[i];
            ok = ok && blocks[i] != nullptr && block_holds(blocks[i], n, static_cast<uint8_t>(1 + i));
            held[i] = blocks[i] != nullptr ? new_global(env, blocks[i]) : nullptr;
        }
        for (int i = 0; i < kArrays; ++i) {
            for (int j = i + 1; j < kArrays; ++j) {
                ok = ok && at[i] != at[j];
            }
            if (blocks[i] != nullptr) {
                delete_local(env, blocks[i]);
            }
        }
        const uint64_t start = now_us();
        for (int round = 0; ok && round < kRounds; ++round) {
            for (int i = 0; ok && i < kArrays; ++i) {
                jobject again = call_object(env, probe, ids.same, held[i]);
                ok = again == at[i] && block_holds(again, n, static_cast<uint8_t>(1 + i));
                if (again != nullptr) {
                    delete_local(env, again);
                }
            }
        }
        char text[12];
        decimal(text, static_cast<uint32_t>((now_us() - start) * 1000 / (kRounds * kArrays)));
        report.line("arrays_ns", text);
        for (jobject reference : held) {
            if (reference != nullptr) {
                delete_global(env, reference);
            }
        }
        report.verdict("ended_arrays", ok && !entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck)(env));
        exception_clear(env);
    }

    // A pointer from Get...Elements that is never released: the array stays reachable and the pointer valid (Dalvik pinned
    // it). Java holds nothing of this array.
    {
        constexpr uint32_t n = 4096;
        unsigned failed = 0;
        const auto check = [&](unsigned code, bool condition) {
            if (!condition && failed == 0) {
                failed = code;
            }
        };
        const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
        const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
        jobject block = call_object(env, probe, ids.asset, static_cast<jint>(n), 0x61);
        check(1, block != nullptr && block_holds(block, n, 0x61));
        jbyte* pointer = block != nullptr ? get_elements(env, block, nullptr) : nullptr;
        uint8_t pattern[16];
        for (unsigned i = 0; i < sizeof(pattern); ++i) {
            pattern[i] = static_cast<uint8_t>(0x90 + i);
        }
        check(2, pointer != nullptr && safe_store(pointer + 100, pattern, sizeof(pattern)));
        if (block != nullptr) {
            delete_local(env, block);  // the native method returns; the pointer is never released
        }
        for (int i = 0; i < 100; ++i) {
            jobject temporary = call_object(env, probe, ids.asset, static_cast<jint>(n), 0x70 + (i & 15));
            check(3, temporary != nullptr && temporary != block);
            if (temporary != nullptr) {
                delete_local(env, temporary);
            }
            if (i % 10 == 9) {
                call_void(env, probe, ids.collect);
            }
        }
        uint8_t seen[128];
        check(4, pointer != nullptr && safe_copy(seen, pointer, sizeof(seen)) && all_equal(seen, 100, 0x61) &&
                     __builtin_memcmp(seen + 100, pattern, sizeof(pattern)) == 0);
        // The release goes to the block, however it arrives: the runtime would free a pointer it never handed out.
        jobject second = call_object(env, probe, ids.asset, 300, 0x62);
        jobject held = second != nullptr ? new_global(env, second) : nullptr;  // a real reference to the array
        jbyte* second_pointer = second != nullptr ? get_elements(env, second, nullptr) : nullptr;
        check(5, held != nullptr && second_pointer != nullptr);
        if (held != nullptr && second_pointer != nullptr) {
            const uint8_t written = 0x63;
            check(6, safe_store(second_pointer + 7, &written, 1));
            release_elements(env, held, second_pointer, 0);
            check(7, call_int(env, probe, ids.peek, held, 7) == 0x63);
        }
        if (second != nullptr) {
            delete_local(env, second);
        }
        if (held != nullptr) {
            delete_global(env, held);
        }
        check(8, !entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck)(env));
        exception_clear(env);
        report.verdict("ended_pin", failed == 0);
        char code[12];
        decimal(code, failed);
        report.line("ended_pin_failed_check", code);
    }

    // The reference of a block that ended, kept across "frames": every call accepts it, and Java's and the application's
    // writes (different bytes of one chunk) meet in the block.
    {
        constexpr uint32_t n = 2048;
        constexpr int kFrames = 100;
        unsigned failed = 0;
        const auto check = [&](unsigned code, bool condition) {
            if (!condition && failed == 0) {
                failed = code;
            }
        };
        const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
        const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
        const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
        const auto get_region = entry<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(env, slot::GetByteArrayRegion);
        jobject kept = call_object(env, probe, ids.asset, static_cast<jint>(n), 0x41);
        check(1, kept != nullptr && block_holds(kept, n, 0x41));
        jobject held = kept != nullptr ? new_global(env, kept) : nullptr;  // Java keeps the array
        if (kept != nullptr) {
            delete_local(env, kept);  // the first native method returned: the reference in the variable is an ended block's
        }
        for (int frame = 0; failed == 0 && frame < kFrames; ++frame) {
            call_void(env, probe, ids.poke, held, frame, 0x80 + (frame & 0x3f));  // Java writes
            check(2, array_length(env, kept) == static_cast<jsize>(n));
            jbyte* elements = get_elements(env, kept, nullptr);
            check(3, elements != nullptr && byte_at(kept, frame) == 0x80 + (frame & 0x3f));  // Java's write is in the block
            const uint8_t written = static_cast<uint8_t>(0x40 + (frame & 0x3f));
            check(4, elements != nullptr && safe_store(elements + 1000 + frame, &written, 1));  // the application writes
            if (elements != nullptr) {
                release_elements(env, kept, elements, 0);
            }
            check(5, call_int(env, probe, ids.peek, kept, 1000 + frame) == written);  // a call with the kept reference
            jbyte region[2] = {};
            get_region(env, kept, frame, 1, region);
            check(6, static_cast<uint8_t>(region[0]) == 0x80 + (frame & 0x3f));
            check(7, call_int(env, probe, ids.peek, held, 1000 + frame) == written &&
                         (call_int(env, probe, ids.peek, held, frame) & 0xff) == 0x80 + (frame & 0x3f));  // both reached the array
            delete_local(env, kept);  // the frame's end: the reference ends again
        }
        if (held != nullptr) {
            delete_global(env, held);
        }
        check(8, !entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck)(env));
        exception_clear(env);
        report.verdict("ended_kept_ref", failed == 0);
        char code[12];
        decimal(code, failed);
        report.line("ended_kept_ref_failed_check", code);
    }

    // What a dirty push costs (recorded): a Java call passed a 1 MiB block with every chunk written through the pointer,
    // against the same call with the block clean.
    {
        const jmethodID echo = static_method(env, probe, "echo", "([B)I");
        constexpr uint32_t n = 1u << 20;
        constexpr int kRounds = 20;
        jobject block = echo != nullptr ? call_object(env, probe, ids.asset, static_cast<jint>(n), 0x10) : nullptr;
        if (block != nullptr && block_holds(block, n, 0x10)) {
            uint64_t dirty_us = 0, clean_us = 0;
            for (int round = 0; round < kRounds; ++round) {
                for (uint32_t chunk = 0; chunk < n / 4096; ++chunk) {
                    const uint8_t written = static_cast<uint8_t>(round + chunk + 1);
                    safe_store(const_cast<char*>(static_cast<const char*>(block)) + 16 + chunk * 4096 + 1, &written, 1);
                }
                const uint64_t start = now_us();
                call_int(env, probe, echo, block);
                dirty_us += now_us() - start;
                const uint64_t clean_start = now_us();
                call_int(env, probe, echo, block);
                clean_us += now_us() - clean_start;
            }
            char text[12];
            decimal(text, static_cast<uint32_t>(dirty_us * 1000 / kRounds));
            report.line("push_dirty_ns", text);
            decimal(text, static_cast<uint32_t>(clean_us * 1000 / kRounds));
            report.line("push_clean_ns", text);
        }
        if (block != nullptr) {
            delete_local(env, block);
        }
        exception_clear(env);
    }

    // The worst cases of the exact merge (recorded): the same call with a block of which every 4th byte, and every other
    // byte, is written through the pointer. The merge stores each written byte into the live array on its own and no byte
    // between two runs, so these are the most stores a 1 MiB block can cost. The chunks are written 4 KiB at a time (the
    // other bytes of the pattern are the ones already there).
    for (int scatter = 0; scatter < 2; ++scatter) {
        const uint32_t step = scatter == 0 ? 4u : 2u;
        const jmethodID echo = static_method(env, probe, "echo", "([B)I");
        constexpr uint32_t n = 1u << 20;
        constexpr int kRounds = 3;
        jobject block = echo != nullptr ? call_object(env, probe, ids.asset, static_cast<jint>(n), 0x10) : nullptr;
        if (block != nullptr && block_holds(block, n, 0x10)) {
            uint64_t total_us = 0;
            bool stored = true;
            for (int round = 0; round < kRounds && stored; ++round) {
                uint8_t pattern[4096];
                for (uint32_t i = 0; i < sizeof(pattern); ++i) {
                    pattern[i] = i % step == 0 ? static_cast<uint8_t>(0x20 + round) : 0x10;
                }
                for (uint32_t chunk = 0; chunk < n / 4096 && stored; ++chunk) {
                    stored = safe_store(const_cast<char*>(static_cast<const char*>(block)) + 16 + chunk * 4096, pattern, sizeof(pattern));
                }
                const uint64_t start = now_us();
                call_int(env, probe, echo, block);
                total_us += now_us() - start;
            }
            if (stored) {
                char text[12];
                decimal(text, static_cast<uint32_t>(total_us / kRounds));
                report.line(step == 4 ? "push_scatter4_us" : "push_scatter2_us", text);
            }
        }
        if (block != nullptr) {
            delete_local(env, block);
        }
        exception_clear(env);
    }

    // What a Get and Release of a real array costs while the layer is active (recorded): the least of three rounds.
    {
        const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
        const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
        const auto get_critical = entry<void* (*)(Env*, jarray, jboolean*)>(env, slot::GetPrimitiveArrayCritical);
        const auto release_critical = entry<void (*)(Env*, jarray, void*, jint)>(env, slot::ReleasePrimitiveArrayCritical);
        const auto make_array = entry<jobject (*)(Env*, jsize)>(env, slot::NewByteArray);
        jobject real = make_array(env, 256);
        if (real != nullptr) {
            constexpr int kPairs = 20000;
            uint64_t best_elements = ~uint64_t{0}, best_critical = ~uint64_t{0};
            for (int round = 0; round < 3; ++round) {
                uint64_t start = now_us();
                for (int i = 0; i < kPairs; ++i) {
                    jbyte* pointer = get_elements(env, real, nullptr);
                    if (pointer != nullptr) {
                        release_elements(env, real, pointer, kReleaseAbort);
                    }
                }
                uint64_t elapsed = now_us() - start;
                best_elements = elapsed < best_elements ? elapsed : best_elements;
                start = now_us();
                for (int i = 0; i < kPairs; ++i) {
                    void* pointer = get_critical(env, real, nullptr);
                    if (pointer != nullptr) {
                        release_critical(env, real, pointer, kReleaseAbort);
                    }
                }
                elapsed = now_us() - start;
                best_critical = elapsed < best_critical ? elapsed : best_critical;
            }
            char text[12];
            decimal(text, static_cast<uint32_t>(best_elements * 1000 / kPairs));
            report.line("real_pair_ns", text);
            decimal(text, static_cast<uint32_t>(best_critical * 1000 / kPairs));
            report.line("real_critical_ns", text);
            delete_local(env, real);
        }
        exception_clear(env);
    }

    // The stated limit: a raw write after the block ended reaches Java only when the array is obtained again.
    {
        jobject block = call_object(env, probe, ids.asset, 100, 0x51);
        jobject held = block != nullptr ? new_global(env, block) : nullptr;
        const uint8_t written = 0x44;
        const bool wrote = block != nullptr && held != nullptr &&
                           (delete_local(env, block), safe_store(const_cast<char*>(static_cast<const char*>(block)) + 16 + 5, &written, 1));
        report.line("ended_limit", !wrote ? "skipped" : call_int(env, probe, ids.peek, held, 5) == 0x44 ? "seen" : "unseen");
        if (held != nullptr) {
            delete_global(env, held);
        }
        exception_clear(env);
    }

    // What the layer's sentinel relies on: a weak reference to an array nothing holds is cleared by a collection, also
    // one the runtime starts itself after allocation pressure.
    {
        const auto new_array = entry<jobject (*)(Env*, jsize)>(env, slot::NewByteArray);
        const auto new_weak = entry<jobject (*)(Env*, jobject)>(env, slot::NewWeakGlobalRef);
        const auto delete_weak = entry<void (*)(Env*, jobject)>(env, slot::DeleteWeakGlobalRef);
        const auto same_object = entry<jboolean (*)(Env*, jobject, jobject)>(env, slot::IsSameObject);
        jobject array = new_array(env, 0);
        jobject weak = array != nullptr ? new_weak(env, array) : nullptr;
        if (array != nullptr) {
            delete_local(env, array);
        }
        bool pressure = false;
        for (int i = 0; weak != nullptr && !pressure && i < 2000; ++i) {
            jobject garbage = new_array(env, 1 << 20);
            if (garbage != nullptr) {
                delete_local(env, garbage);
            }
            pressure = same_object(env, weak, nullptr) != 0;
        }
        report.line("sentinel_pressure", weak == nullptr ? "skipped" : pressure ? "cleared" : "kept");
        if (weak != nullptr) {
            delete_weak(env, weak);
        }
        array = new_array(env, 0);
        weak = array != nullptr ? new_weak(env, array) : nullptr;
        if (array != nullptr) {
            delete_local(env, array);
        }
        call_void(env, probe, ids.collect);
        report.line("sentinel_gc", weak == nullptr ? "skipped" : same_object(env, weak, nullptr) != 0 ? "cleared" : "kept");
        if (weak != nullptr) {
            delete_weak(env, weak);
        }
        // What one IsSameObject costs, for the numbers of the layer's tests (nanoseconds).
        jobject strong = new_array(env, 8);
        weak = strong != nullptr ? new_weak(env, strong) : nullptr;
        if (weak != nullptr) {
            const uint64_t start = now_us();
            for (int i = 0; i < 20000; ++i) {
                same_object(env, weak, nullptr);
            }
            char text[12];
            decimal(text, static_cast<uint32_t>((now_us() - start) * 1000 / 20000));
            report.line("is_same_ns", text);
            delete_weak(env, weak);
        }
        if (strong != nullptr) {
            delete_local(env, strong);
        }
        exception_clear(env);
    }

    // The cost of a byte[] result and its deletion, in nanoseconds: the least of three rounds of 5000.
    {
        uint64_t best = ~uint64_t{0};
        for (int round = 0; round < 3; ++round) {
            const uint64_t start = now_us();
            for (int i = 0; i < 5000; ++i) {
                jobject array = call_object(env, probe, ids.payload);
                if (array != nullptr) {
                    delete_local(env, array);
                }
            }
            const uint64_t elapsed = now_us() - start;
            best = elapsed < best ? elapsed : best;
            call_void(env, probe, ids.collect);
        }
        char text[12];
        decimal(text, static_cast<uint32_t>(best * 1000 / 5000));
        report.line("create_ns", text);
        // What System.identityHashCode costs from native code.
        const auto find_class = entry<jclass (*)(Env*, const char*)>(env, slot::FindClass);
        const auto new_array = entry<jobject (*)(Env*, jsize)>(env, slot::NewByteArray);
        jclass system = find_class(env, "java/lang/System");
        jmethodID hash = system != nullptr ? static_method(env, system, "identityHashCode", "(Ljava/lang/Object;)I") : nullptr;
        jobject subject = new_array(env, 8);
        if (hash != nullptr && subject != nullptr) {
            const uint64_t start = now_us();
            for (int i = 0; i < 5000; ++i) {
                call_int(env, system, hash, subject);
            }
            decimal(text, static_cast<uint32_t>((now_us() - start) * 1000 / 5000));
            report.line("hash_call_ns", text);
        }
        exception_clear(env);
    }
}

// The cases of the end of a native method (see the list at the top).
void ends_cases(Report& report, Env* env, jclass probe, const char* options) {
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_void = entry<void (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kVoid, kVarargs));
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);
    const auto exception_clear = entry<void (*)(Env*)>(env, slot::ExceptionClear);
    g_payload_id = static_method(env, probe, "payload", "()[B");
    g_recurse_id = static_method(env, probe, "recurse", "(I)I");
    const auto ticks = [&](const char* name) {
        jmethodID id = static_method(env, probe, name, "()I");
        bool ok = id != nullptr && g_payload_id != nullptr;
        for (int i = 0; ok && i < kTicks; ++i) {
            ok = call_int(env, probe, id) == 1;
        }
        ok = ok && !exception_check(env);
        exception_clear(env);
        return ok;
    };
    report.verdict("ends_lazy_thumb", ticks("tick"));
    report.verdict("ends_lazy_arm", ticks("tickArm"));
    report.verdict("ends_reg_thumb", ticks("tickReg"));
    report.verdict("ends_reg_arm", ticks("tickRegArm"));
    report.verdict("ends_recursion", g_recurse_id != nullptr && call_int(env, probe, g_recurse_id, 70) == 71 && !exception_check(env));
    exception_clear(env);

    // JNI_OnLoad made 400 results and returned: the 400 made here would be past the limit if those still counted.
    {
        bool ok = g_onload_made && g_payload_id != nullptr;
        jobject held[kOnLoadBlocks] = {};
        for (int i = 0; ok && i < kOnLoadBlocks; ++i) {
            Report scratch;
            held[i] = call_object(env, probe, g_payload_id);
            ok = held[i] != nullptr && raw_read(scratch, held[i]);
        }
        for (jobject reference : held) {
            if (reference != nullptr) {
                delete_local(env, reference);
            }
        }
        report.verdict("ends_onload", ok && !exception_check(env));
        exception_clear(env);
    }

    // A native method that returns with an exception pending.
    {
        jmethodID throwing = static_method(env, probe, "throwing", "()V");
        jmethodID zero = static_method(env, probe, "abi0", "()I");
        bool raised = false;
        if (throwing != nullptr) {
            call_void(env, probe, throwing);
            raised = exception_check(env) != 0;
            exception_clear(env);
        }
        report.verdict("ends_exception", raised && zero != nullptr && call_int(env, probe, zero) == kAbi0 && !exception_check(env));
        exception_clear(env);
    }
    report.verdict("abi_args", abi_args(env, probe));
    exception_clear(env);
    report.verdict("abi_returns", abi_returns(env, probe));
    exception_clear(env);
    outside_case(report, env, probe, options);

    // The cost of a native call: an empty native method called from a Java loop, the least of three rounds.
    {
        jmethodID spin = static_method(env, probe, "spin", "(I)I");
        uint64_t best = ~uint64_t{0};
        for (int round = 0; spin != nullptr && round < 3; ++round) {
            const uint64_t start = now_us();
            call_int(env, probe, spin, 1000000);
            const uint64_t elapsed = now_us() - start;
            best = elapsed < best ? elapsed : best;
        }
        char text[12] = "none";
        if (best != ~uint64_t{0}) {
            decimal(text, static_cast<uint32_t>(best));
        }
        report.line("empty_us", text);
        exception_clear(env);
    }
}

}  // namespace

// The native methods the cases call. tick and tickArm are bound by name (a Thumb and an ARM function); the others
// that are not listed in JNI_OnLoad are bound by name too.
extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_tick(Env* env, jclass probe) {
    return tick_body(env, probe);
}
extern "C" __attribute__((visibility("default"), target("arm"))) jint Java_org_example_andarium_oldappprobe_Probe_tickArm(
    Env* env, jclass probe) {
    return tick_body(env, probe);
}
extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_recurse(Env* env, jclass probe,
                                                                                                         jint depth) {
    return recurse_body(env, probe, depth);
}
// A byte[] result, a portable check of it, an exception, and the return: the layer ends the block with an exception pending.
extern "C" __attribute__((visibility("default"))) void Java_org_example_andarium_oldappprobe_Probe_throwing(Env* env,
                                                                                                            jclass probe) {
    jobject array = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs))(
        env, probe, g_payload_id);
    const jsize length = array != nullptr ? entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength)(env, array) : -1;
    jclass exception = entry<jclass (*)(Env*, const char*)>(env, slot::FindClass)(env, "java/lang/RuntimeException");
    if (exception != nullptr && length == kPayloadLength) {
        entry<jint (*)(Env*, jclass, const char*)>(env, slot::ThrowNew)(env, exception, "thunk probe");
    }
}
extern "C" __attribute__((visibility("default"))) void Java_org_example_andarium_oldappprobe_Probe_empty(Env*, jclass) {}
extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_abi0(Env*, jclass) {
    return kAbi0;
}
extern "C" __attribute__((visibility("default"))) jlong Java_org_example_andarium_oldappprobe_Probe_abi12(
    Env*, jclass, jint a, jlong b, jdouble c, jfloat d, jint e, jlong f, jdouble g, jfloat h, jint i, jint j, jint k, jint l) {
    return abi_sum(a, b, c, d, e, f, g, h, i, j, k, l);
}
extern "C" __attribute__((visibility("default"))) jboolean Java_org_example_andarium_oldappprobe_Probe_retZ(Env*, jclass, jboolean v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jbyte Java_org_example_andarium_oldappprobe_Probe_retB(Env*, jclass, jbyte v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jchar Java_org_example_andarium_oldappprobe_Probe_retC(Env*, jclass, jchar v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jshort Java_org_example_andarium_oldappprobe_Probe_retS(Env*, jclass, jshort v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_retI(Env*, jclass, jint v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jlong Java_org_example_andarium_oldappprobe_Probe_retJ(Env*, jclass, jlong v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jfloat Java_org_example_andarium_oldappprobe_Probe_retF(Env*, jclass, jfloat v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jdouble Java_org_example_andarium_oldappprobe_Probe_retD(Env*, jclass, jdouble v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) jstring Java_org_example_andarium_oldappprobe_Probe_retL(Env*, jclass, jstring v) {
    return v;
}
extern "C" __attribute__((visibility("default"))) void Java_org_example_andarium_oldappprobe_Probe_retV(Env*, jclass, jint v) {
    g_retv = v;
}
extern "C" __attribute__((visibility("default"))) jint Java_org_example_andarium_oldappprobe_Probe_readV(Env*, jclass) {
    return g_retv;
}

// Probe.run(String options): the native method of the Java receiver; the text it returns is the report.
extern "C" __attribute__((visibility("default"))) jstring Java_org_example_andarium_oldappprobe_Probe_run(
    Env* env, jclass probe, jstring options) {
    using GetUtf = const char* (*)(Env*, jstring, jboolean*);
    using ReleaseUtf = void (*)(Env*, jstring, const char*);
    const char* opts = options != nullptr ? entry<GetUtf>(env, 169)(env, options, nullptr) : "";  // GetStringUTFChars
    Report report;
    report.line("shim_mapped", shim_mapped() ? "yes" : "no");
    // The options "scene" and "peak" (the text includes the NUL): the scene alone, in a process of its own; and one more
    // result from this process, after the layer's 30 s between two diagnosis lines, so that the peaks it kept appear in
    // the log (a result and a return are what log them).
    const bool scene_only = same(opts, "scene", 6), peak_only = same(opts, "peak", 5);
    if (same(opts, "swap|", 5)) {
        swap_case(report, env, probe, opts + 5);
        report.line("done", "1");
        entry<ReleaseUtf>(env, 170)(env, options, opts);
        return entry<jstring (*)(Env*, const char*)>(env, slot::NewStringUTF)(env, report.text);
    }
    if (scene_only || peak_only) {
        if (scene_only) {
            scene_cases(report, env, probe);
        } else {
            const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
            const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
            const jmethodID once = static_method(env, probe, "payload", "()[B");
            report.verdict("peak_result", once != nullptr && call_object(env, probe, once) != nullptr);
        }
        report.line("done", "1");
        entry<ReleaseUtf>(env, 170)(env, options, opts);
        return entry<jstring (*)(Env*, const char*)>(env, slot::NewStringUTF)(env, report.text);
    }

    const auto method_id = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
    const auto get_region = entry<void (*)(Env*, jarray, jsize, jsize, void*)>(env, slot::GetByteArrayRegion);
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);
    const auto exception_clear = entry<void (*)(Env*)>(env, slot::ExceptionClear);

    jmethodID payload = method_id(env, probe, "payload", "()[B");
    jmethodID echo = method_id(env, probe, "echo", "([B)I");
    jobject array = payload != nullptr ? call_object(env, probe, payload) : nullptr;
    if (exception_check(env)) {
        exception_clear(env);
        array = nullptr;
    }
    if (array == nullptr || echo == nullptr) {
        report.line("raw_read", "fail");
        report.line("passback", "fail");
    } else {
        report.verdict("raw_read", raw_read(report, array));
        char bytes[64];
        const jint echoed = call_int(env, probe, echo, array);
        const jsize counted = array_length(env, array);
        get_region(env, array, 0, kPayloadLength, bytes);
        report.verdict("passback", !exception_check(env) && echoed == kPayloadLength && counted == kPayloadLength &&
                                       same(bytes, kPayload, kPayloadLength));
        exception_clear(env);

        const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
        const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
        jbyte* elements = get_elements(env, array, nullptr);
        bool elements_ok = elements != nullptr && same(reinterpret_cast<const char*>(elements), kPayload, kPayloadLength);
        if (elements != nullptr) {
            release_elements(env, array, elements, 2);  // JNI_ABORT
        }
        report.verdict("elements", elements_ok && !exception_check(env));

        jobject global = entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef)(env, array);
        bool global_ok = global != nullptr && array_length(env, global) == kPayloadLength &&
                         call_int(env, probe, echo, global) == kPayloadLength && !exception_check(env);
        if (global != nullptr) {
            entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, global);
        }
        exception_clear(env);
        report.verdict("global", global_ok);
        delete_local(env, array);
        report.verdict("thread_raw", thread_raw_read(env, probe, payload));
    }

    array_cases(report, env);
    allocator_cases(report);
    {
        Vm* vm = nullptr;
        entry<jint (*)(Env*, Vm**)>(env, 219)(env, &vm);  // GetJavaVM
        report.verdict("table_read_only", vm != nullptr && mapped_read_only(env->functions) && mapped_read_only(vm->functions));
    }
    report.verdict("low_heap", low_heap());
    ends_cases(report, env, probe, opts);
    ended_cases(report, env, probe);

    report.line("done", "1");
    if (options != nullptr) {
        entry<ReleaseUtf>(env, 170)(env, options, opts);  // ReleaseStringUTFChars
    }
    return entry<jstring (*)(Env*, const char*)>(env, slot::NewStringUTF)(env, report.text);
}

extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(Vm* vm, void*) {
    // The ordinary first call of a JNI library: it is also what makes the old-application
    // layer look at this library's allocator imports.
    void* env_pointer = nullptr;
    entry<jint (*)(Vm*, void**, jint)>(reinterpret_cast<Env*>(vm), vmslot::GetEnv)(vm, &env_pointer, 0x00010006);
    Env* env = static_cast<Env*>(env_pointer);
    if (env == nullptr) {
        return 0x00010006;
    }
    // Two natives are bound here (RegisterNatives): a Thumb and an ARM function. The results this call gets from
    // Java are never deleted: they must end with JNI_OnLoad.
    jclass probe = entry<jclass (*)(Env*, const char*)>(env, slot::FindClass)(env, "org/example/andarium/oldappprobe/Probe");
    if (probe != nullptr) {
        const NativeMethod methods[] = {
            {"tickReg", "()I", reinterpret_cast<void*>(&tick_reg)},
            {"tickRegArm", "()I", reinterpret_cast<void*>(&tick_reg_arm)},
            {"abi12Arm", "(IJDFIJDFIIII)J", reinterpret_cast<void*>(&abi12_arm)},
        };
        entry<jint (*)(Env*, jclass, const NativeMethod*, jint)>(env, slot::RegisterNatives)(env, probe, methods, 3);
        jmethodID payload = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID)(
            env, probe, "payload", "()[B");
        if (payload != nullptr) {
            const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
            for (int i = 0; i < kOnLoadBlocks; ++i) {
                g_onload_made = call_object(env, probe, payload) != nullptr;
            }
        }
        if (entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck)(env)) {
            entry<void (*)(Env*)>(env, slot::ExceptionClear)(env);
        }
    }
    return 0x00010006;
}
