// SPDX-License-Identifier: Apache-2.0
//
// Page protection for the writes the old-application layer makes into read-only memory (the JNI
// function tables, the relocation slots of libraries).
//
// A PageWindow opens pages for writing and gives every one of them back with the protection it
// had. Opening is all or nothing: when one page cannot be opened the pages already opened are
// restored at once, and a restore that fails is reported. maps_protection() reads the protection
// a page really has from the text of /proc/self/maps, and says "unknown" for an address no
// mapping covers, so nothing is ever assumed writable. Neither touches the system itself: the
// caller supplies the mprotect and the file reads, and the host tests supply fakes.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace oldapp {

constexpr int kProtRead = 1;
constexpr int kProtWrite = 2;
constexpr int kProtExec = 4;
constexpr uintptr_t kPageSize = 4096;

// `Protect` is a callable bool(uintptr_t page, int protection) that applies the protection to one page.
template <class Protect>
class PageWindow {
public:
    static constexpr unsigned kMaxPages = 24;

    explicit PageWindow(Protect protect) : protect_(protect) {}
    PageWindow(const PageWindow&) = delete;
    PageWindow& operator=(const PageWindow&) = delete;

    // Makes `page` writable (keeping the other bits of `original`); a page opened before is left as it is.
    bool open(uintptr_t page, int original) {
        for (unsigned i = 0; i < count_; ++i) {
            if (pages_[i] == page) {
                return true;
            }
        }
        if (count_ >= kMaxPages || !protect_(page, original | kProtWrite)) {
            rollback();
            return false;
        }
        pages_[count_] = page;
        original_[count_] = original;
        ++count_;
        return true;
    }

    // Gives every opened page its protection back; the window is empty afterwards.
    void rollback() { (void)close(); }

    // As rollback(), and whether every restore worked.
    bool close() {
        bool restored = true;
        for (unsigned i = count_; i > 0; --i) {
            restored = protect_(pages_[i - 1], original_[i - 1]) && restored;
        }
        count_ = 0;
        return restored;
    }

    unsigned opened() const { return count_; }

private:
    Protect protect_;
    uintptr_t pages_[kMaxPages];
    int original_[kMaxPages];
    unsigned count_ = 0;
};

// The protection (kProt bits) of the mapping that covers `address`, from the text of /proc/self/maps read in pieces
// by `read` (bool(char* buffer, size_t capacity, size_t* got); got == 0 is the end); -1 when no line covers it.
template <class Read>
int maps_protection(Read read, uintptr_t address) {
    char chunk[512];
    char line[160];
    size_t length = 0;
    auto parse = [&](const char* text, size_t size) -> int {
        // "start-end perms offset dev inode path": hexadecimal range, then four permission characters.
        size_t i = 0;
        uint64_t start = 0, end = 0;
        auto hex = [&](uint64_t* out) {
            size_t digits = 0;
            *out = 0;
            while (i < size) {
                const char c = text[i];
                unsigned v;
                if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
                else break;
                *out = (*out << 4) | v;
                ++i;
                ++digits;
            }
            return digits != 0;
        };
        if (!hex(&start) || i >= size || text[i++] != '-' || !hex(&end) || i >= size || text[i++] != ' ' || i + 4 > size) {
            return -2;  // not a mapping line
        }
        if (address < start || address >= end) {
            return -2;
        }
        return (text[i] == 'r' ? kProtRead : 0) | (text[i + 1] == 'w' ? kProtWrite : 0) | (text[i + 2] == 'x' ? kProtExec : 0);
    };
    for (;;) {
        size_t got = 0;
        if (!read(chunk, sizeof(chunk), &got) || got == 0) {
            break;
        }
        for (size_t k = 0; k < got; ++k) {
            if (chunk[k] == '\n') {
                const int verdict = parse(line, length);
                if (verdict >= 0) {
                    return verdict;
                }
                length = 0;
            } else if (length < sizeof(line)) {
                line[length++] = chunk[k];  // a long path is cut: the range and permissions come first
            }
        }
    }
    if (length != 0) {
        const int verdict = parse(line, length);
        if (verdict >= 0) {
            return verdict;
        }
    }
    return -1;
}

}  // namespace oldapp
