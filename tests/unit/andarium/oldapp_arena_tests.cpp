// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's allocator (android/lineage-24/overlay/oldapp/low_arena.hpp)
// over an ordinary buffer: alignment, splitting and coalescing, exhaustion, resizing,
// pointers that are not its own, and a long random run that checks the structure's
// invariants as it goes.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "low_arena.hpp"
#include "require.hpp"

using oldapp::Arena;
using uml::testing::require;

namespace {

alignas(16) uint8_t g_region[4u << 20];
alignas(16) uint8_t g_small_region[4096];

Arena fresh(size_t size = sizeof(g_region)) {
    Arena arena;
    require(arena.init(g_region, size), "the arena takes the region");
    return arena;
}

void check_alignment_and_basics() {
    Arena arena = fresh();
    require(arena.verify(), "a fresh arena is consistent");
    require(arena.bytes_in_use() == 0 && arena.block_count() == 0, "a fresh arena holds nothing");
    for (size_t size : {size_t{0}, size_t{1}, size_t{7}, size_t{8}, size_t{9}, size_t{24}, size_t{100}, size_t{4096}}) {
        void* pointer = arena.allocate(size);
        require(pointer != nullptr, "a small request succeeds");
        require((reinterpret_cast<uintptr_t>(pointer) & 15u) == 0, "payloads are 16-byte aligned");
        require(arena.contains(pointer), "the block is inside the arena");
        require(arena.usable_size(pointer) >= size, "usable size covers the request");
        std::memset(pointer, 0xa5, size);
        require(arena.verify(), "consistent after an allocation");
    }
    require(arena.block_count() == 8, "eight blocks");
}

// The old-application layer's block record relies on the least chunk of a block (its header, Dalvik words, tail and this header):
// kChunkHeaderBytes is the bytes a chunk adds to its payload, and a chunk is the request plus them rounded up to 16.
void check_chunk_header_bytes() {
    Arena arena = fresh();
    for (size_t size : {size_t{0}, size_t{1}, size_t{8}, size_t{9}, size_t{24}, size_t{100}, size_t{200}, size_t{1000}}) {
        void* first = arena.allocate(size);
        void* second = arena.allocate(size);
        require(first != nullptr && second != nullptr, "two small requests succeed");
        size_t chunk = (size + Arena::kChunkHeaderBytes + 15) & ~size_t{15};
        if (chunk < 16) chunk = 16;
        require(arena.usable_size(first) + Arena::kChunkHeaderBytes == chunk, "a chunk is its payload plus kChunkHeaderBytes");
        require(static_cast<uint8_t*>(second) - static_cast<uint8_t*>(first) == static_cast<ptrdiff_t>(chunk),
                "and the next payload lies one chunk further");
    }
}

void check_coalescing() {
    Arena arena = fresh(64 * 1024);
    void* a = arena.allocate(1000);
    void* b = arena.allocate(1000);
    void* c = arena.allocate(1000);
    void* d = arena.allocate(1000);
    require(a && b && c && d, "four blocks");
    require(arena.release(a) && arena.release(c), "free the outer two");
    require(arena.verify(), "consistent with two holes");
    require(arena.release(b), "free the middle one");
    require(arena.verify(), "the three merged");
    // The merged hole holds a block of the three sizes together.
    void* big = arena.allocate(3000);
    require(big != nullptr, "a block that needs the merged hole");
    require(big == a, "it reuses the merged hole from its start");
    require(arena.release(d) && arena.release(big), "free the rest");
    require(arena.verify() && arena.bytes_in_use() == 0, "everything is back");
    // After everything is freed one block can take nearly the whole region.
    void* whole = arena.allocate(60 * 1024);
    require(whole != nullptr, "one block takes almost the whole arena");
}

void check_exhaustion() {
    Arena arena = fresh(16 * 1024);
    std::vector<void*> blocks;
    while (void* pointer = arena.allocate(500)) {
        blocks.push_back(pointer);
    }
    require(blocks.size() > 20 && blocks.size() < 40, "the region holds a few dozen 500-byte blocks");
    require(arena.allocate(500) == nullptr, "an exhausted arena says so");
    require(arena.release(blocks[3]), "free one");
    void* again = arena.allocate(500);
    require(again == blocks[3], "the freed block comes back");
    require(arena.allocate(0x7fffffffu) == nullptr, "an impossible size fails");
    require(arena.verify(), "consistent after exhaustion");
}

void check_foreign_pointers() {
    Arena arena = fresh();
    uint8_t outside[64];
    void* a = arena.allocate(100);
    std::memset(a, 0xff, 100);  // an interior pointer reads these bytes as a header: not a plausible one
    require(!arena.release(outside), "a pointer outside is ignored");
    require(!arena.release(static_cast<uint8_t*>(a) + 16), "an interior pointer is ignored");
    require(arena.release(a), "a live block frees");
    require(!arena.release(a), "a second free is ignored");
    require(arena.usable_size(a) == 0, "a freed block has no usable size");
    require(arena.verify() && arena.bytes_in_use() == 0, "still consistent");
}

void check_realloc() {
    Arena arena = fresh();
    uint8_t* p = static_cast<uint8_t*>(arena.allocate(100));
    for (int i = 0; i < 100; ++i) p[i] = uint8_t(i);
    uint8_t* q = static_cast<uint8_t*>(arena.reallocate(p, 5000));
    require(q != nullptr, "grows");
    for (int i = 0; i < 100; ++i) require(q[i] == uint8_t(i), "contents survive growth");
    uint8_t* r = static_cast<uint8_t*>(arena.reallocate(q, 40));
    require(r == q, "shrinking keeps the block");
    for (int i = 0; i < 40; ++i) require(r[i] == uint8_t(i), "contents survive shrinking");
    require(arena.verify(), "consistent after resizing");
    // Growth in place into a free neighbour.
    uint8_t* a = static_cast<uint8_t*>(arena.allocate(64));
    uint8_t* b = static_cast<uint8_t*>(arena.allocate(64));
    arena.release(b);
    require(arena.reallocate(a, 100) == a, "grows into the free neighbour without moving");
    require(arena.verify(), "consistent after growing in place");
    // A failed growth leaves the old block alone.
    Arena small;
    require(small.init(g_small_region, sizeof(g_small_region)), "a small arena");
    uint8_t* s = static_cast<uint8_t*>(small.allocate(1000));
    std::memset(s, 0x5a, 1000);
    require(small.reallocate(s, 100000) == nullptr, "an impossible growth fails");
    require(s[0] == 0x5a && s[999] == 0x5a && small.usable_size(s) >= 1000, "the old block is untouched");
    require(arena.reallocate(nullptr, 32) != nullptr, "realloc of null allocates");
}

void check_random_run() {
    Arena arena = fresh();
    struct Live {
        uint8_t* pointer;
        size_t size;
        uint8_t fill;
    };
    std::vector<Live> live;
    uint32_t state = 12345;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int step = 0; step < 30000; ++step) {
        const uint32_t action = next() % 10;
        if (action < 5 || live.empty()) {
            const size_t size = (next() % 8 == 0) ? next() % 20000 : next() % 300;
            uint8_t* pointer = static_cast<uint8_t*>(arena.allocate(size));
            if (pointer != nullptr) {
                const uint8_t fill = uint8_t(next());
                std::memset(pointer, fill, size);
                live.push_back({pointer, size, fill});
            }
        } else if (action < 8) {
            const size_t index = next() % live.size();
            Live entry = live[index];
            for (size_t i = 0; i < entry.size; ++i) require(entry.pointer[i] == entry.fill, "no block was overwritten");
            require(arena.release(entry.pointer), "a live block frees");
            live[index] = live.back();
            live.pop_back();
        } else {
            const size_t index = next() % live.size();
            Live& entry = live[index];
            const size_t size = next() % 2000;
            uint8_t* moved = static_cast<uint8_t*>(arena.reallocate(entry.pointer, size));
            if (moved != nullptr) {
                const size_t kept = size < entry.size ? size : entry.size;
                for (size_t i = 0; i < kept; ++i) require(moved[i] == entry.fill, "resizing keeps the contents");
                std::memset(moved, entry.fill, size);
                entry.pointer = moved;
                entry.size = size;
            }
        }
        if (step % 1000 == 0) require(arena.verify(), "consistent during the run");
    }
    for (const Live& entry : live) {
        for (size_t i = 0; i < entry.size; ++i) require(entry.pointer[i] == entry.fill, "no block was overwritten");
        require(arena.release(entry.pointer), "free at the end");
    }
    require(arena.verify() && arena.bytes_in_use() == 0 && arena.block_count() == 0, "empty at the end");
    require(arena.peak_bytes_in_use() > 100000, "the run used the arena");
    require(arena.allocate(sizeof(g_region) - 4096) != nullptr, "the whole region is one block again");
}

}  // namespace

int main() {
    check_alignment_and_basics();
    check_chunk_header_bytes();
    check_coalescing();
    check_exhaustion();
    check_foreign_pointers();
    check_realloc();
    check_random_run();
    std::puts("old-application arena: ok");
    return 0;
}
