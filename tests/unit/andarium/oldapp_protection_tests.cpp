// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's page protection (android/lineage-24/overlay/oldapp/protection.hpp):
// pages are opened for writing all or nothing, a failed open gives back the pages already opened, a
// restore that fails is reported, and the protection of an address is read from the text of
// /proc/self/maps, with "unknown" for an address no line covers.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "protection.hpp"
#include "require.hpp"

using namespace oldapp;
using uml::testing::require;

namespace {

struct Calls {
    std::map<uintptr_t, int> protection;  // the fake kernel's state
    std::vector<std::pair<uintptr_t, int>> log;
    int fail_on_call = 0;                 // 1-based: that mprotect fails
    std::map<uintptr_t, int> fail_restore_of;  // a page whose restore (to this protection) fails
};
Calls calls;

bool fake_protect(uintptr_t page, int protection) {
    calls.log.emplace_back(page, protection);
    if (calls.fail_on_call != 0 && static_cast<int>(calls.log.size()) == calls.fail_on_call) return false;
    const auto doomed = calls.fail_restore_of.find(page);
    if (doomed != calls.fail_restore_of.end() && doomed->second == protection) return false;
    calls.protection[page] = protection;
    return true;
}

void reset() { calls = Calls{}; }

void check_open_and_close() {
    reset();
    PageWindow<bool (*)(uintptr_t, int)> window(&fake_protect);
    calls.protection = {{0x1000, kProtRead}, {0x2000, kProtRead}, {0x3000, kProtRead | kProtExec}};
    require(window.open(0x1000, kProtRead) && window.open(0x2000, kProtRead), "two read-only pages open");
    require(calls.protection[0x1000] == (kProtRead | kProtWrite), "writable now");
    require(window.open(0x1000, kProtRead) && calls.log.size() == 2, "a page opened before is not opened again");
    require(window.close(), "closing restores");
    require(calls.protection[0x1000] == kProtRead && calls.protection[0x2000] == kProtRead, "every page has its protection back");
    require(window.opened() == 0, "and the window is empty");
}

// The second page cannot be opened: the first, already writable, is given back.
void check_rollback_when_a_page_cannot_be_opened() {
    reset();
    PageWindow<bool (*)(uintptr_t, int)> window(&fake_protect);
    calls.protection = {{0x1000, kProtRead}, {0x2000, kProtRead}};
    calls.fail_on_call = 2;
    require(window.open(0x1000, kProtRead), "the first page opens");
    require(!window.open(0x2000, kProtRead), "the second does not");
    require(calls.protection[0x1000] == kProtRead, "the first page is read-only again");
    require(window.opened() == 0, "nothing stays open");
}

void check_restore_failure_is_reported() {
    reset();
    PageWindow<bool (*)(uintptr_t, int)> window(&fake_protect);
    calls.protection = {{0x1000, kProtRead}, {0x2000, kProtRead}};
    require(window.open(0x1000, kProtRead) && window.open(0x2000, kProtRead), "open");
    calls.fail_restore_of[0x2000] = kProtRead;
    require(!window.close(), "a restore that fails is reported");
    require(calls.protection[0x1000] == kProtRead, "and the other page was still restored");
}

void check_page_limit() {
    reset();
    PageWindow<bool (*)(uintptr_t, int)> window(&fake_protect);
    for (unsigned i = 0; i < PageWindow<bool (*)(uintptr_t, int)>::kMaxPages; ++i) {
        calls.protection[0x1000 * (i + 1)] = kProtRead;
        require(window.open(0x1000 * (i + 1), kProtRead), "within the limit");
    }
    require(!window.open(0x100000, kProtRead), "beyond the limit nothing more opens, and the window rolls back");
    require(window.opened() == 0 && calls.protection[0x1000] == kProtRead, "everything given back");
}

// ---- /proc/self/maps

struct Text {
    std::string text;
    size_t at = 0;
    size_t piece = 7;  // a small read size, so lines straddle reads
    bool operator()(char* buffer, size_t capacity, size_t* got) {
        const size_t n = std::min({piece, capacity, text.size() - at});
        std::memcpy(buffer, text.data() + at, n);
        at += n;
        *got = n;
        return true;
    }
};

const char kMaps[] =
    "00010000-00012000 r-xp 00000000 fd:00 123 /system/bin/app_process32\n"
    "00012000-00013000 r--p 00002000 fd:00 123 /system/bin/app_process32\n"
    "70000000-70100000 rw-p 00000000 00:00 0 [anon:libc_malloc]\n"
    "a1b2c000-a1b2d000 r--p 00000000 fd:00 77 /apex/com.android.art/lib/libart.so with a very long name "
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
    "a1b2d000-a1b30000 rwxp 00001000 fd:00 77 /apex/com.android.art/lib/libart.so\n"
    "ffff0000-ffff1000 r-xp 00000000 00:00 0 [vectors]";

void check_maps_protection() {
    auto query = [](uintptr_t address) {
        Text text{kMaps};
        return maps_protection(text, address);
    };
    require(query(0x10000) == (kProtRead | kProtExec), "the first byte of a text mapping");
    require(query(0x11fff) == (kProtRead | kProtExec), "its last byte");
    require(query(0x12000) == kProtRead, "the next mapping starts where the last ended");
    require(query(0x70050000) == (kProtRead | kProtWrite), "an anonymous read-write mapping");
    require(query(0xa1b2c800) == kProtRead, "a line with a long path is still read");
    require(query(0xa1b2f000) == (kProtRead | kProtWrite | kProtExec), "a read-write-execute mapping is reported as it is");
    require(query(0xffff0800) == (kProtRead | kProtExec), "the last line has no newline");
    require(query(0x13000) == -1, "an address in a gap is unknown");
    require(query(0x0) == -1 && query(0xfffff000) == -1, "so are the ends");
    Text empty{""};
    require(maps_protection(empty, 0x10000) == -1, "an empty file knows nothing");
    struct Failing {
        bool operator()(char*, size_t, size_t*) { return false; }
    } failing;
    require(maps_protection(failing, 0x10000) == -1, "a read error is unknown, not writable");
}

}  // namespace

int main() {
    check_open_and_close();
    check_rollback_when_a_page_cannot_be_opened();
    check_restore_failure_is_reported();
    check_page_limit();
    check_maps_protection();
    std::puts("old-application page protection: ok");
    return 0;
}
