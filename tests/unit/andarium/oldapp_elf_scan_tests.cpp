// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's relocation scanner (android/lineage-24/overlay/oldapp/elf_scan.hpp)
// over a synthetic ELF32 ARM image: the allocator imports in the PLT and the GOT are found, other
// symbols, a library's own definitions (unless asked for, as for libc) and relative relocations are
// not, the consumer/producer masks pick what a platform module and an application library get, and
// a slot's original protection follows the program headers.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "application_path.hpp"
#include "elf_scan.hpp"
#include "require.hpp"

using namespace oldapp;
using uml::testing::require;

namespace {

constexpr uint32_t kStr = 0x040, kSym = 0x100, kRelDyn = 0x200, kJmpRel = 0x300, kDynamic = 0x380;
constexpr uint32_t kRArmAbs32 = 2, kRArmGlobDat = 21, kRArmJumpSlot = 22, kRArmRelative = 23;

struct Image {
    alignas(16) uint8_t bytes[0x1000] = {};
    Elf32Phdr phdrs[4] = {};

    template <class T>
    T* at(uint32_t offset) {
        return reinterpret_cast<T*>(bytes + offset);
    }
};

uint32_t name_at(Image& image, uint32_t& cursor, const char* text) {
    const uint32_t offset = cursor;
    std::strcpy(reinterpret_cast<char*>(image.bytes + kStr + cursor), text);
    cursor += static_cast<uint32_t>(std::strlen(text)) + 1;
    return offset;
}

void build(Image& image) {
    uint32_t cursor = 1;  // offset 0 is the empty name
    Elf32Sym* sym = image.at<Elf32Sym>(kSym);
    sym[1] = {name_at(image, cursor, "malloc"), 0, 0, 0x12, 0, 0};
    sym[2] = {name_at(image, cursor, "free"), 0, 0, 0x12, 0, 0};
    sym[3] = {name_at(image, cursor, "foo"), 0, 0, 0x12, 0, 0};
    sym[4] = {name_at(image, cursor, "_Znwj"), 0, 0, 0x12, 0, 0};
    sym[5] = {sym[2].st_name, 0x1234, 4, 0x12, 0, 7};  // "free" again, but defined by the library itself
    sym[6] = {name_at(image, cursor, "_ZdlPvRKSt9nothrow_t"), 0, 0, 0x12, 0, 0};
    sym[7] = {name_at(image, cursor, "strdup"), 0, 0, 0x12, 0, 0};
    sym[8] = {name_at(image, cursor, "dlsym"), 0, 0, 0x12, 0, 0};
    Elf32Rel* rel = image.at<Elf32Rel>(kRelDyn);
    rel[0] = {0x500, (1u << 8) | kRArmGlobDat};        // malloc
    rel[1] = {0x504, (3u << 8) | kRArmAbs32};          // foo: not an import
    rel[2] = {0x508, kRArmRelative};                   // relative, no symbol
    rel[3] = {0x50c, (7u << 8) | kRArmAbs32};          // strdup
    Elf32Rel* jmp = image.at<Elf32Rel>(kJmpRel);
    jmp[0] = {0x600, (2u << 8) | kRArmJumpSlot};       // free
    jmp[1] = {0x604, (4u << 8) | kRArmJumpSlot};       // operator new
    jmp[2] = {0x608, (5u << 8) | kRArmJumpSlot};       // the library's own free: left alone
    jmp[3] = {0x60c, (6u << 8) | kRArmJumpSlot};       // operator delete (nothrow)
    jmp[4] = {0x610, (8u << 8) | kRArmJumpSlot};       // dlsym: libart's symbol lookup
    Elf32Dyn* dyn = image.at<Elf32Dyn>(kDynamic);
    int i = 0;
    dyn[i++] = {6, kSym};
    dyn[i++] = {5, kStr};
    dyn[i++] = {10, cursor};
    dyn[i++] = {17, kRelDyn};
    dyn[i++] = {18, 4 * sizeof(Elf32Rel)};
    dyn[i++] = {23, kJmpRel};
    dyn[i++] = {2, 5 * sizeof(Elf32Rel)};
    dyn[i++] = {0, 0};
    image.phdrs[0] = {kPtLoad, 0, 0, 0, 0x1000, 0x1000, kPfR | kPfX, 0x1000};
    image.phdrs[1] = {kPtLoad, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, kPfR | kPfW, 0x1000};
    image.phdrs[2] = {kPtDynamic, kDynamic, kDynamic, kDynamic, 128, 128, kPfR | kPfW, 4};
    image.phdrs[3] = {kPtGnuRelro, 0x1000, 0x1000, 0x1000, 0x400, 0x400, kPfR, 1};
}

void check_scan() {
    Image image;
    build(image);
    ImportSlot slots[16];
    const size_t found = scan_imports(reinterpret_cast<uintptr_t>(image.bytes), image.phdrs, 4, slots, 16,
                                      kAppConsumers | kProducers, false);
    require(found == 5, "free, operator new and the nothrow delete in the PLT; malloc and strdup in the GOT");
    require(slots[0].vaddr == 0x600 && slots[0].what == Import::kFree, "PLT free");
    require(slots[1].vaddr == 0x604 && slots[1].what == Import::kNew, "PLT operator new");
    require(slots[2].vaddr == 0x60c && slots[2].what == Import::kDeleteNothrow, "PLT nothrow delete");
    require(slots[3].vaddr == 0x500 && slots[3].what == Import::kMalloc, "GOT malloc");
    require(slots[4].vaddr == 0x50c && slots[4].what == Import::kStrdup, "GOT strdup");
}

void check_capacity() {
    Image image;
    build(image);
    ImportSlot slots[2];
    const size_t found = scan_imports(reinterpret_cast<uintptr_t>(image.bytes), image.phdrs, 4, slots, 2,
                                      kAppConsumers | kProducers, false);
    require(found == 5, "the count is the number found, whatever the capacity");
    require(slots[0].vaddr == 0x600 && slots[1].vaddr == 0x604, "the first ones are written");
}

// A platform module gets only the calls that take a pointer; an application library gets those and the producers.
void check_masks() {
    Image image;
    build(image);
    ImportSlot slots[16];
    const uintptr_t bias = reinterpret_cast<uintptr_t>(image.bytes);
    size_t found = scan_imports(bias, image.phdrs, 4, slots, 16, kPlatformConsumers, false);
    require(found == 1 && slots[0].what == Import::kFree && slots[0].vaddr == 0x600, "a platform module: free only (not the delete operators)");
    found = scan_imports(bias, image.phdrs, 4, slots, 16, kAppConsumers, false);
    require(found == 2 && slots[0].what == Import::kFree && slots[1].what == Import::kDeleteNothrow, "an application: free and the delete operators");
    found = scan_imports(bias, image.phdrs, 4, slots, 16, kProducers, false);
    require(found == 3, "producers: operator new, malloc, strdup");
    for (size_t i = 0; i < found; ++i) {
        require(slots[i].what == Import::kNew || slots[i].what == Import::kMalloc || slots[i].what == Import::kStrdup, "a producer");
    }
}

// realloc and reallocarray are in both passes. The consumer pass gives every module the version that never puts a block
// in the arena; only the producer pass, for a module whose consumers were all rewritten, promotes an application library
// to the one that does. A module that kept a free it could not rewrite therefore never gets an arena block from realloc.
void check_realloc_is_in_both_passes() {
    const uint32_t both = bit(Import::kRealloc) | bit(Import::kReallocarray);
    require((kPlatformConsumers & both) == both && (kAppConsumers & both) == both, "every module gets the consumer versions");
    require((kProducers & both) == both, "the producer pass promotes them");
    require((kPlatformConsumers & kProducers) == both && (kAppConsumers & kProducers) == both,
            "and they are the only imports in both passes");
}

// libc.so calls its own exports through its PLT: its defined `free` slot is wanted there, and only there.
void check_defined_symbols() {
    Image image;
    build(image);
    ImportSlot slots[16];
    const uintptr_t bias = reinterpret_cast<uintptr_t>(image.bytes);
    require(scan_imports(bias, image.phdrs, 4, slots, 16, kPlatformConsumers, false) == 1, "the library's own free is left alone");
    const size_t found = scan_imports(bias, image.phdrs, 4, slots, 16, kPlatformConsumers, true);
    require(found == 2 && slots[0].vaddr == 0x600 && slots[1].vaddr == 0x608 && slots[1].what == Import::kFree,
            "libc's slots for its own free are found when asked for");
}

// libart's dlsym is the one slot outside the allocator masks: only its own mask finds it.
void check_dlsym_import() {
    Image image;
    build(image);
    ImportSlot slots[16];
    const uintptr_t bias = reinterpret_cast<uintptr_t>(image.bytes);
    const size_t found = scan_imports(bias, image.phdrs, 4, slots, 16, kDlsymImport, false);
    require(found == 1 && slots[0].vaddr == 0x610 && slots[0].what == Import::kDlsym, "the dlsym slot");
    require(scan_imports(bias, image.phdrs, 4, slots, 16, kAppConsumers | kProducers | kPlatformConsumers, false) == 5,
            "and no allocator mask picks it up");
}

void check_no_dynamic_section() {
    Image image;
    build(image);
    image.phdrs[2].p_type = 0x70000000;
    ImportSlot slots[4];
    require(scan_imports(reinterpret_cast<uintptr_t>(image.bytes), image.phdrs, 4, slots, 4, kAppConsumers | kProducers, false) == 0,
            "a library with no dynamic section has nothing to scan");
}

void check_corrupt_name() {
    Image image;
    build(image);
    image.at<Elf32Sym>(kSym)[2].st_name = 0x7fffffff;  // beyond DT_STRSZ
    ImportSlot slots[8];
    const size_t found = scan_imports(reinterpret_cast<uintptr_t>(image.bytes), image.phdrs, 4, slots, 8,
                                      kAppConsumers | kProducers, false);
    require(found == 4, "a symbol whose name lies outside the string table is skipped");
}

void check_symbol_names() {
    Import what;
    for (const char* name : {"malloc", "free", "calloc", "realloc", "strdup", "strndup", "reallocarray", "malloc_usable_size", "dlsym",
                             "_Znwj", "_Znaj",
                             "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t", "_ZdlPv", "_ZdaPv",
                             "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t", "_ZdlPvj", "_ZdaPvj"}) {
        require(import_for_symbol(name, &what), name);
    }
    for (const char* name : {"memalign", "posix_memalign", "mallinfo", "dlopen", "dlsymx", "Znwj", ""}) {
        require(!import_for_symbol(name, &what), name);
    }
}

void check_protection() {
    Image image;
    build(image);
    require(original_protection(image.phdrs, 4, 0x500) == (1 | 4), "a slot in the read-execute segment");
    require(original_protection(image.phdrs, 4, 0x1100) == 1, "a slot in RELRO goes back to read-only");
    require(original_protection(image.phdrs, 4, 0x1800) == (1 | 2), "a slot in plain data goes back to read-write");
}

}  // namespace

// The application's own libraries (the native-method thunks' domain): the install and data directories of the package
// of the process, and no other package's.
void check_application_paths() {
    char package[64];
    require(package_of_process("com.example.game", package, sizeof(package)) && std::strcmp(package, "com.example.game") == 0,
            "a process named for the package");
    require(package_of_process("com.example.game:remote", package, sizeof(package)) && std::strcmp(package, "com.example.game") == 0,
            "a private process of the package: up to the colon");
    require(!package_of_process("", package, sizeof(package)) && !package_of_process(":x", package, sizeof(package)) &&
                !package_of_process(nullptr, package, sizeof(package)),
            "no name, no package");
    require(!package_of_process("com.example.much.too.long.for.the.buffer", package, 16), "a name that does not fit");
    const char* const own[] = {
        "/data/app/~~Qw3e==/com.example.game-Zx9==/lib/arm/libgame.so",
        "/data/app/~~Qw3e==/com.example.game-Zx9==/base.apk!/lib/armeabi-v7a/libgame.so",
        "/data/app/~~Qw3e==/com.example.game-Zx9==/split_config.armeabi_v7a.apk!/lib/armeabi-v7a/libgame.so",
        "/data/app/com.example.game-1/lib/arm/libgame.so",
        "/data/data/com.example.game/files/libdownloaded.so",
        "/data/user/0/com.example.game/code_cache/libx.so",
        "/data/user_de/10/com.example.game/lib/libx.so",
        "/mnt/expand/0123-4567/app/~~Aa==/com.example.game-Bb==/lib/arm/libgame.so",
    };
    for (const char* path : own) {
        require(in_application_directories(path, "com.example.game"), "an own library");
    }
    const char* const other[] = {
        "/data/app/~~Qw3e==/com.google.android.webview-Zx9==/lib/arm/libwebviewchromium.so",
        "/data/app/~~Qw3e==/com.example.gamefoo-Zx9==/lib/arm/libgame.so",     // a longer package name
        "/data/app/~~Qw3e==/com.example.game.extra-Zx9==/lib/arm/libgame.so",  // a package that extends the name
        "/data/app/~~Qw3e==/xcom.example.game-Zx9==/lib/arm/libgame.so",
        "/data/data/com.example.other/files/libx.so",
        "/data/data/com.example.gamefoo/files/libx.so",
        "/data/user/0/com.example.gamefoo/files/libx.so",
        "/data/local/tmp/com.example.game/libx.so",  // not a directory the platform gives the package
        "/system/lib/libc.so",
        "libname.so",
        "",
    };
    for (const char* path : other) {
        require(!in_application_directories(path, "com.example.game"), "not an own library");
    }
    require(!in_application_directories(nullptr, "com.example.game"), "no path, none");
    require(in_application_directories("/data/app/x/lib/arm/libgame.so", "") && in_application_directories("/data/app/x/lib/arm/libgame.so", nullptr),
            "an unknown package: every application library is the application's own");
}

int main() {
    check_application_paths();
    check_scan();
    check_capacity();
    check_masks();
    check_dlsym_import();
    check_realloc_is_in_both_passes();
    check_defined_symbols();
    check_no_dynamic_section();
    check_corrupt_name();
    check_symbol_names();
    check_protection();
    std::puts("old-application ELF scan: ok");
    return 0;
}
