// SPDX-License-Identifier: Apache-2.0
//
// Finds the relocation slots of an ELF32 ARM library that import the allocator.
//
// The old-application layer points these slots at its below-2 GiB allocator. The
// scanner reads a library the way the dynamic linker leaves it in memory: the
// program headers, the dynamic section (its pointers are virtual addresses) and
// the REL relocations of the PLT and the GOT. It returns slot addresses and
// never writes, so the host tests run it over a synthetic image.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace oldapp {

struct Elf32Phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
struct Elf32Dyn {
    int32_t d_tag;
    uint32_t d_val;
};
struct Elf32Rel {
    uint32_t r_offset, r_info;
};
struct Elf32Sym {
    uint32_t st_name, st_value, st_size;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
};

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtGnuRelro = 0x6474e552;
constexpr uint32_t kPfX = 1;
constexpr uint32_t kPfW = 2;
constexpr uint32_t kPfR = 4;

// The imports the layer replaces. A consumer takes a pointer that may be an arena block (free, realloc,
// reallocarray, malloc_usable_size, delete); a producer returns memory (malloc, calloc, strdup, new).
enum class Import : uint8_t {
    kMalloc, kFree, kCalloc, kRealloc, kStrdup, kStrndup, kReallocarray, kUsableSize,
    kNew, kNewArray, kNewNothrow, kNewArrayNothrow,
    kDelete, kDeleteArray, kDeleteNothrow, kDeleteArrayNothrow, kDeleteSized, kDeleteArraySized,
    kDlsym,  // not an allocator call: the runtime's symbol lookup, which the layer wraps for the natives' thunks
    kCount,
};

constexpr uint32_t bit(Import what) { return uint32_t{1} << static_cast<unsigned>(what); }

// What every module gets (the platform's too): the calls that take a pointer.
constexpr uint32_t kPlatformConsumers =
    bit(Import::kFree) | bit(Import::kRealloc) | bit(Import::kReallocarray) | bit(Import::kUsableSize);
// What an application library also gets: the delete operators, and every producer.
constexpr uint32_t kDeleteOperators = bit(Import::kDelete) | bit(Import::kDeleteArray) | bit(Import::kDeleteNothrow) |
                                      bit(Import::kDeleteArrayNothrow) | bit(Import::kDeleteSized) |
                                      bit(Import::kDeleteArraySized);
constexpr uint32_t kAppConsumers = kPlatformConsumers | kDeleteOperators;
// realloc and reallocarray are in both: a consumer pass gives every module the calls that never put a block in the
// arena, and the producer pass, for a module whose consumers are all rewritten, gives the application's libraries the
// ones that do (a block libc allocated moves into the arena when they reallocate it).
constexpr uint32_t kProducers = bit(Import::kMalloc) | bit(Import::kCalloc) | bit(Import::kStrdup) |
                                bit(Import::kStrndup) | bit(Import::kNew) | bit(Import::kNewArray) |
                                bit(Import::kNewNothrow) | bit(Import::kNewArrayNothrow) | bit(Import::kRealloc) |
                                bit(Import::kReallocarray);

// libart's symbol lookup: the one import the layer rewrites outside the allocator.
constexpr uint32_t kDlsymImport = bit(Import::kDlsym);

struct ImportSlot {
    uint32_t vaddr;  // relative to the library's load bias
    Import what;
};

inline bool import_for_symbol(const char* name, Import* out) {
    struct Entry {
        const char* name;
        Import what;
    };
    static const Entry kEntries[] = {
        {"malloc", Import::kMalloc},
        {"free", Import::kFree},
        {"calloc", Import::kCalloc},
        {"realloc", Import::kRealloc},
        {"strdup", Import::kStrdup},
        {"strndup", Import::kStrndup},
        {"reallocarray", Import::kReallocarray},
        {"malloc_usable_size", Import::kUsableSize},
        {"_Znwj", Import::kNew},
        {"_Znaj", Import::kNewArray},
        {"_ZnwjRKSt9nothrow_t", Import::kNewNothrow},
        {"_ZnajRKSt9nothrow_t", Import::kNewArrayNothrow},
        {"_ZdlPv", Import::kDelete},
        {"_ZdaPv", Import::kDeleteArray},
        {"_ZdlPvRKSt9nothrow_t", Import::kDeleteNothrow},
        {"_ZdaPvRKSt9nothrow_t", Import::kDeleteArrayNothrow},
        {"_ZdlPvj", Import::kDeleteSized},
        {"_ZdaPvj", Import::kDeleteArraySized},
        {"dlsym", Import::kDlsym},
    };
    for (const Entry& entry : kEntries) {
        if (entry.name[0] == name[0] && __builtin_strcmp(entry.name, name) == 0) {
            *out = entry.what;
            return true;
        }
    }
    return false;
}

// The slots of the library mapped at `load_bias` that import one of the symbols in `mask` (bits of Import). A
// symbol the library defines itself is skipped unless `include_defined`: that is for libc.so, whose own calls
// to its exports go through its PLT. At most `capacity` are written; the number found is returned (it may
// exceed `capacity`).
inline size_t scan_imports(uintptr_t load_bias, const Elf32Phdr* phdrs, unsigned phnum, ImportSlot* out,
                           size_t capacity, uint32_t mask, bool include_defined) {
    constexpr int32_t kDtNull = 0, kDtPltRelSz = 2, kDtStrTab = 5, kDtSymTab = 6, kDtStrSz = 10, kDtRel = 17,
                      kDtRelSz = 18, kDtJmpRel = 23;
    constexpr uint32_t kRArmAbs32 = 2, kRArmGlobDat = 21, kRArmJumpSlot = 22;
    const Elf32Dyn* dynamic = nullptr;
    for (unsigned i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type == kPtDynamic) {
            dynamic = reinterpret_cast<const Elf32Dyn*>(load_bias + phdrs[i].p_vaddr);
        }
    }
    if (dynamic == nullptr) {
        return 0;
    }
    uint32_t symtab = 0, strtab = 0, strsz = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    for (const Elf32Dyn* d = dynamic; d->d_tag != kDtNull; ++d) {
        switch (d->d_tag) {
            case kDtSymTab: symtab = d->d_val; break;
            case kDtStrTab: strtab = d->d_val; break;
            case kDtStrSz: strsz = d->d_val; break;
            case kDtRel: rel = d->d_val; break;
            case kDtRelSz: relsz = d->d_val; break;
            case kDtJmpRel: jmprel = d->d_val; break;
            case kDtPltRelSz: pltrelsz = d->d_val; break;
            default: break;
        }
    }
    if (symtab == 0 || strtab == 0) {
        return 0;
    }
    const Elf32Sym* symbols = reinterpret_cast<const Elf32Sym*>(load_bias + symtab);
    const char* strings = reinterpret_cast<const char*>(load_bias + strtab);
    size_t found = 0;
    const struct {
        uint32_t table, size;
    } tables[2] = {{jmprel, pltrelsz}, {rel, relsz}};
    for (const auto& table : tables) {
        if (table.table == 0) {
            continue;
        }
        const Elf32Rel* entries = reinterpret_cast<const Elf32Rel*>(load_bias + table.table);
        const size_t count = table.size / sizeof(Elf32Rel);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t type = entries[i].r_info & 0xffu;
            const uint32_t index = entries[i].r_info >> 8;
            if (index == 0 || (type != kRArmJumpSlot && type != kRArmGlobDat && type != kRArmAbs32)) {
                continue;
            }
            const Elf32Sym& symbol = symbols[index];
            if ((symbol.st_shndx != 0 && !include_defined) || (strsz != 0 && symbol.st_name >= strsz)) {
                continue;  // defined by the library itself, or a corrupt name
            }
            Import what;
            if (!import_for_symbol(strings + symbol.st_name, &what) || (mask & bit(what)) == 0) {
                continue;
            }
            if (found < capacity) {
                out[found] = ImportSlot{entries[i].r_offset, what};
            }
            ++found;
        }
    }
    return found;
}

// The protection a page of the library had before the layer wrote to it: the PT_LOAD flags, read-only
// inside PT_GNU_RELRO (the linker re-protects that range after relocating).
inline int original_protection(const Elf32Phdr* phdrs, unsigned phnum, uint32_t vaddr) {
    for (unsigned i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type == kPtGnuRelro && vaddr >= phdrs[i].p_vaddr &&
            vaddr < phdrs[i].p_vaddr + phdrs[i].p_memsz) {
            return 1;  // PROT_READ
        }
    }
    for (unsigned i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type == kPtLoad && vaddr >= phdrs[i].p_vaddr && vaddr < phdrs[i].p_vaddr + phdrs[i].p_memsz) {
            return ((phdrs[i].p_flags & kPfR) ? 1 : 0) | ((phdrs[i].p_flags & kPfW) ? 2 : 0) |
                   ((phdrs[i].p_flags & kPfX) ? 4 : 0);
        }
    }
    return 3;
}

}  // namespace oldapp
