// SPDX-License-Identifier: Apache-2.0
//
// A boundary-tag allocator over one caller-supplied region.
//
// The old-application layer maps a region below 2 GiB and serves the allocator
// calls of an old library from it: such a library tests its heap pointers as
// signed 32-bit numbers. The same allocator serves the layer's own byte[]
// shadow blocks. It includes nothing of Android, so the host tests run it over
// an ordinary buffer.
//
// Layout. Every chunk starts with an 8-byte header, size_flags (the chunk's
// size, a multiple of 16, with kUsed and kPrevUsed in the low bits) and
// prev_size (valid only while the chunk before is free). A payload starts 8
// bytes into its chunk, and a chunk header is 8 modulo 16, so payloads are
// 16-byte aligned. A free chunk keeps its list links, as offsets from the
// region's first chunk, in its payload. A sentinel header ends the region.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace oldapp {

class Arena {
public:
    // Takes [base, base + size). False when the region is too small (under 64 bytes).
    bool init(void* base, size_t size);

    // 16-byte aligned, nullptr when the region is exhausted.
    void* allocate(size_t size);
    // Ignores a pointer that is not a live block of this arena (and returns false).
    bool release(void* pointer);
    // Null `pointer` allocates. nullptr (the old block untouched) when exhausted.
    void* reallocate(void* pointer, size_t size);

    // A free chunk keeps its list links in the first bytes of its payload. The arena promises nothing about what the address
    // of a freed block holds: a user that must tell whether an address is a block keeps its own record, and reads no byte of
    // the arena for it.
    static constexpr size_t kFreeLinkBytes = 2 * sizeof(uint32_t);
    // The bytes a chunk adds to its payload: a chunk of a request of n bytes is n + kChunkHeaderBytes rounded up to 16.
    static constexpr uint32_t kChunkHeaderBytes = 8;

    bool contains(const void* pointer) const {
        return pointer >= static_cast<const void*>(first_) && pointer < static_cast<const void*>(end_);
    }
    // The payload size of a live block: at least what was asked.
    size_t usable_size(const void* pointer) const;

    size_t bytes_in_use() const { return in_use_; }
    size_t peak_bytes_in_use() const { return peak_; }
    size_t block_count() const { return blocks_; }

    // Walks every chunk and every list; true when the structure is consistent. For tests.
    bool verify() const;

private:
    struct Chunk {
        uint32_t size_flags;
        uint32_t prev_size;
    };
    static constexpr uint32_t kUsed = 1;
    static constexpr uint32_t kPrevUsed = 2;
    static constexpr uint32_t kSizeMask = ~15u;
    static constexpr uint32_t kHeader = kChunkHeaderBytes;
    static constexpr uint32_t kMinChunk = 16;
    static constexpr uint32_t kNone = 0xffffffffu;
    static constexpr unsigned kBins = 108;

    Chunk* at(uint32_t offset) const { return reinterpret_cast<Chunk*>(first_ + offset); }
    uint32_t offset_of(const Chunk* chunk) const {
        return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(chunk) - first_);
    }
    static uint32_t size_of(const Chunk* chunk) { return chunk->size_flags & kSizeMask; }
    static uint32_t* links(Chunk* chunk) {  // [next, prev] in the payload of a free chunk
        return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(chunk) + kHeader);
    }
    static const uint32_t* links(const Chunk* chunk) {
        return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(chunk) + kHeader);
    }
    static unsigned bin_for(uint32_t size);
    static uint32_t chunk_size_for(size_t request);

    void insert(Chunk* chunk);
    void unlink(Chunk* chunk);
    void* allocate_locked(size_t size);
    void release_chunk(Chunk* chunk);
    bool live_block(const void* pointer, Chunk** chunk) const;
    void account(long delta_bytes, long delta_blocks);

    void lock();
    void unlock();

    uint8_t* first_ = nullptr;
    uint8_t* end_ = nullptr;  // the sentinel header
    uint32_t bins_[kBins];
    uint32_t lock_ = 0;
    size_t in_use_ = 0;
    size_t peak_ = 0;
    size_t blocks_ = 0;
};

inline unsigned Arena::bin_for(uint32_t size) {
    if (size < 1024) {
        return size / 16;
    }
    const unsigned log2 = 31u - static_cast<unsigned>(__builtin_clz(size));
    const unsigned half = (size >> (log2 - 1)) & 1u;
    const unsigned bin = 64 + (log2 - 10) * 2 + half;
    return bin < kBins ? bin : kBins - 1;
}

inline uint32_t Arena::chunk_size_for(size_t request) {
    if (request > 0x7fffff00u) {
        return 0;
    }
    uint32_t size = static_cast<uint32_t>(request) + kHeader;
    size = (size + 15u) & kSizeMask;
    return size < kMinChunk ? kMinChunk : size;
}

inline void Arena::lock() {
    while (__atomic_exchange_n(&lock_, 1u, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&lock_, __ATOMIC_RELAXED) != 0) {
#if defined(__arm__) || defined(__aarch64__)
            __asm__ __volatile__("yield");
#endif
        }
    }
}

inline void Arena::unlock() { __atomic_store_n(&lock_, 0u, __ATOMIC_RELEASE); }

inline bool Arena::init(void* base, size_t size) {
    if (size < 64 || size > 0xfffff000u) {
        return false;
    }
    uintptr_t start = reinterpret_cast<uintptr_t>(base);
    start = ((start + kHeader + 15u) & ~uintptr_t{15}) - kHeader;
    const uintptr_t limit = reinterpret_cast<uintptr_t>(base) + size;
    if (limit < start + kHeader + 32) {
        return false;
    }
    uint32_t total = static_cast<uint32_t>((limit - start - kHeader) & ~uintptr_t{15});
    first_ = reinterpret_cast<uint8_t*>(start);
    end_ = first_ + total;
    for (unsigned i = 0; i < kBins; ++i) {
        bins_[i] = kNone;
    }
    Chunk* sentinel = at(total);
    sentinel->size_flags = kUsed;
    sentinel->prev_size = total;
    Chunk* chunk = at(0);
    chunk->size_flags = total | kPrevUsed;
    chunk->prev_size = 0;
    insert(chunk);
    in_use_ = peak_ = blocks_ = 0;
    lock_ = 0;
    return true;
}

inline void Arena::insert(Chunk* chunk) {
    const unsigned bin = bin_for(size_of(chunk));
    const uint32_t self = offset_of(chunk);
    uint32_t* l = links(chunk);
    l[0] = bins_[bin];
    l[1] = kNone;
    if (bins_[bin] != kNone) {
        links(at(bins_[bin]))[1] = self;
    }
    bins_[bin] = self;
}

inline void Arena::unlink(Chunk* chunk) {
    const unsigned bin = bin_for(size_of(chunk));
    const uint32_t* l = links(chunk);
    const uint32_t next = l[0];
    const uint32_t prev = l[1];
    if (prev != kNone) {
        links(at(prev))[0] = next;
    } else {
        bins_[bin] = next;
    }
    if (next != kNone) {
        links(at(next))[1] = prev;
    }
}

inline void Arena::account(long delta_bytes, long delta_blocks) {
    in_use_ = static_cast<size_t>(static_cast<long>(in_use_) + delta_bytes);
    blocks_ = static_cast<size_t>(static_cast<long>(blocks_) + delta_blocks);
    if (in_use_ > peak_) {
        peak_ = in_use_;
    }
}

inline void* Arena::allocate_locked(size_t request) {
    const uint32_t need = chunk_size_for(request);
    if (need == 0) {
        return nullptr;
    }
    for (unsigned bin = bin_for(need); bin < kBins; ++bin) {
        for (uint32_t at_offset = bins_[bin]; at_offset != kNone; at_offset = links(at(at_offset))[0]) {
            Chunk* chunk = at(at_offset);
            const uint32_t size = size_of(chunk);
            if (size < need) {
                continue;  // only a large bin mixes sizes
            }
            unlink(chunk);
            const uint32_t prev_flag = chunk->size_flags & kPrevUsed;
            if (size - need >= kMinChunk) {
                Chunk* rest = at(at_offset + need);
                rest->size_flags = (size - need) | kPrevUsed;
                at(at_offset + size)->prev_size = size - need;
                insert(rest);
                chunk->size_flags = need | kUsed | prev_flag;
                account(need, 1);
            } else {
                Chunk* next = at(at_offset + size);
                next->size_flags |= kPrevUsed;
                chunk->size_flags = size | kUsed | prev_flag;
                account(size, 1);
            }
            return reinterpret_cast<uint8_t*>(chunk) + kHeader;
        }
    }
    return nullptr;
}

inline void* Arena::allocate(size_t request) {
    lock();
    void* result = allocate_locked(request);
    unlock();
    return result;
}

inline bool Arena::live_block(const void* pointer, Chunk** chunk) const {
    if (!contains(pointer) || (reinterpret_cast<uintptr_t>(pointer) & 15u) != 0) {
        return false;
    }
    Chunk* candidate = reinterpret_cast<Chunk*>(
        const_cast<uint8_t*>(static_cast<const uint8_t*>(pointer)) - kHeader);
    if ((candidate->size_flags & kUsed) == 0) {
        return false;
    }
    // A pointer into the middle of a block reads arbitrary bytes as a header: accept only a size that
    // fits the region and ends at a header that agrees this chunk is in use.
    const uint64_t offset = static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(candidate) - first_);
    const uint64_t size = size_of(candidate);
    if (size < kMinChunk || offset + size > static_cast<uint64_t>(end_ - first_) ||
        (at(static_cast<uint32_t>(offset + size))->size_flags & kPrevUsed) == 0) {
        return false;
    }
    *chunk = candidate;
    return true;
}

// `chunk` is a used chunk (or a tail the caller declares used): free it and merge it with free neighbours.
inline void Arena::release_chunk(Chunk* chunk) {
    uint32_t size = size_of(chunk);
    uint32_t prev_flag = chunk->size_flags & kPrevUsed;
    if (prev_flag == 0) {
        Chunk* before = at(offset_of(chunk) - chunk->prev_size);
        unlink(before);
        size += size_of(before);
        prev_flag = before->size_flags & kPrevUsed;
        chunk = before;
    }
    Chunk* after = at(offset_of(chunk) + size);
    if ((after->size_flags & kUsed) == 0) {
        unlink(after);
        size += size_of(after);
        after = at(offset_of(chunk) + size);
    }
    chunk->size_flags = size | prev_flag;
    after->prev_size = size;
    after->size_flags &= ~kPrevUsed;
    insert(chunk);
}

inline bool Arena::release(void* pointer) {
    lock();
    Chunk* chunk = nullptr;
    const bool live = live_block(pointer, &chunk);
    if (live) {
        account(-static_cast<long>(size_of(chunk)), -1);
        release_chunk(chunk);
    }
    unlock();
    return live;
}

inline size_t Arena::usable_size(const void* pointer) const {
    Chunk* chunk = nullptr;
    return live_block(pointer, &chunk) ? size_of(chunk) - kHeader : 0;
}

inline void* Arena::reallocate(void* pointer, size_t request) {
    if (pointer == nullptr) {
        return allocate(request);
    }
    const uint32_t need = chunk_size_for(request);
    if (need == 0) {
        return nullptr;
    }
    lock();
    Chunk* chunk = nullptr;
    if (!live_block(pointer, &chunk)) {
        unlock();
        return nullptr;
    }
    const uint32_t old_size = size_of(chunk);
    const uint32_t offset = offset_of(chunk);
    if (need <= old_size) {
        if (old_size - need >= kMinChunk) {  // give the tail back
            const uint32_t prev_flag = chunk->size_flags & kPrevUsed;
            chunk->size_flags = need | kUsed | prev_flag;
            Chunk* tail = at(offset + need);
            tail->size_flags = (old_size - need) | kUsed | kPrevUsed;
            account(-static_cast<long>(old_size - need), 0);
            release_chunk(tail);
        }
        unlock();
        return pointer;
    }
    Chunk* next = at(offset + old_size);
    if ((next->size_flags & kUsed) == 0 && old_size + size_of(next) >= need) {  // grow in place
        const uint32_t merged = old_size + size_of(next);
        unlink(next);
        const uint32_t prev_flag = chunk->size_flags & kPrevUsed;
        if (merged - need >= kMinChunk) {
            Chunk* rest = at(offset + need);
            rest->size_flags = (merged - need) | kPrevUsed;
            at(offset + merged)->prev_size = merged - need;
            insert(rest);
            chunk->size_flags = need | kUsed | prev_flag;
            account(static_cast<long>(need) - old_size, 0);
        } else {
            at(offset + merged)->size_flags |= kPrevUsed;
            chunk->size_flags = merged | kUsed | prev_flag;
            account(static_cast<long>(merged) - old_size, 0);
        }
        unlock();
        return pointer;
    }
    void* moved = allocate_locked(request);
    if (moved != nullptr) {
        __builtin_memcpy(moved, pointer, old_size - kHeader);
        account(-static_cast<long>(old_size), -1);
        release_chunk(chunk);
    }
    unlock();
    return moved;
}

inline bool Arena::verify() const {
    uint32_t free_chunks = 0;
    uint32_t offset = 0;
    bool prev_used = true;
    uint32_t prev_size = 0;
    const uint32_t total = static_cast<uint32_t>(end_ - first_);
    uint64_t used_bytes = 0;
    size_t used_blocks = 0;
    while (offset < total) {
        const Chunk* chunk = at(offset);
        const uint32_t size = size_of(chunk);
        if (size < kMinChunk || offset + size > total || (size & 15u) != 0) {
            return false;
        }
        if (((chunk->size_flags & kPrevUsed) != 0) != prev_used) {
            return false;
        }
        if (!prev_used && chunk->prev_size != prev_size) {
            return false;
        }
        const bool used = (chunk->size_flags & kUsed) != 0;
        if (!used) {
            if (!prev_used) {
                return false;  // two free chunks in a row
            }
            ++free_chunks;
        } else {
            used_bytes += size;
            ++used_blocks;
        }
        prev_used = used;
        prev_size = size;
        offset += size;
    }
    if (offset != total) {
        return false;
    }
    const Chunk* sentinel = at(total);
    if ((sentinel->size_flags & kUsed) == 0 || ((sentinel->size_flags & kPrevUsed) != 0) != prev_used) {
        return false;
    }
    if (!prev_used && sentinel->prev_size != prev_size) {
        return false;
    }
    uint32_t listed = 0;
    for (unsigned bin = 0; bin < kBins; ++bin) {
        uint32_t before = kNone;
        for (uint32_t at_offset = bins_[bin]; at_offset != kNone; at_offset = links(at(at_offset))[0]) {
            const Chunk* chunk = at(at_offset);
            if (at_offset >= total || (chunk->size_flags & kUsed) != 0 || bin_for(size_of(chunk)) != bin ||
                links(chunk)[1] != before) {
                return false;
            }
            before = at_offset;
            if (++listed > free_chunks) {
                return false;
            }
        }
    }
    return listed == free_chunks && used_bytes == in_use_ && used_blocks == blocks_;
}

}  // namespace oldapp
