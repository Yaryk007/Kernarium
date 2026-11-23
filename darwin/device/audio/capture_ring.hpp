// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>

namespace uml::audio {

/* The frames at the start of a buffer, captured from `capturedNanoseconds`, that were captured before the boundary: a
 * first sample captured any time before it counts, so the count is rounded up. Zero where either time is not known
 * (zero). Times are on one clock. */
[[nodiscard]] constexpr std::size_t framesBeforeBoundary(
    std::uint64_t capturedNanoseconds, std::uint64_t boundaryNanoseconds, std::uint32_t rate) noexcept
{
    if (capturedNanoseconds == 0 || boundaryNanoseconds <= capturedNanoseconds)
        return 0;
    constexpr std::uint64_t second = 1'000'000'000ULL;
    return static_cast<std::size_t>(
        ((boundaryNanoseconds - capturedNanoseconds) * rate + second - 1) / second);
}

/* What a test pauses at inside restart(); production has none (the calls compile to nothing). */
struct NoRestartSeam {
    static void afterPublish() noexcept {}
    static void afterWait() noexcept {}
    static void afterCursor() noexcept {}
};

/* A bounded single-producer, single-consumer ring of bytes, for a capture
 * source whose audio arrives on a thread of its own. Neither side allocates
 * or locks. The producer calls write() only; the consumer calls read(),
 * discard(), clear() and restart() only. A full ring drops the bytes the
 * producer offers beyond its room, and the consumer discards the oldest bytes
 * it does not want, so a source that runs ahead of the guest bounds its
 * latency instead of growing it.
 *
 * A consumer that begins a recording restart()s the ring: the audio the
 * producer captured before then must not reach it, including a buffer the
 * producer holds, converting it, when the restart is made. The producer reads
 * epoch() and boundaryNanoseconds() as a buffer reaches it and offers the
 * buffer with that epoch (write(data, epoch)): a restart in between drops it,
 * and a buffer that began before the boundary has its leading frames trimmed
 * by the producer (framesBeforeBoundary). Audio the producer writes under the
 * new epoch while the restart is under way is kept: the producer notes where
 * its first write of an epoch begins, and the restart drops what is before
 * that note, or everything held where the note is not of the new epoch.
 * The producer never waits for the consumer. */
template <typename Seam>
class BasicCaptureRing final {
public:
    explicit BasicCaptureRing(std::size_t capacityBytes)
        : capacity_(std::bit_ceil(std::max<std::size_t>(capacityBytes, 2)))
        , bytes_(std::make_unique<std::uint8_t[]>(capacity_))
    {
    }

    /* Returns how many of the bytes it took; the rest did not fit. */
    std::size_t write(std::span<const std::uint8_t> data) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto count = std::min(data.size(), capacity_ - (head - tail));
        copyIn(head, data.first(count));
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    /* The same, for a buffer that reached the producer when the ring was at `epoch`: dropped (and reported as taken) if
     * a restart has been made since. */
    std::size_t write(std::span<const std::uint8_t> data, std::uint64_t epoch) noexcept
    {
        // Sequentially consistent: either this sees the restart's epoch, or the restart sees this write under way.
        writing_.store(true, std::memory_order_seq_cst);
        std::size_t taken = data.size();
        if (epoch_.load(std::memory_order_seq_cst) == epoch) {
            // The first write of an epoch notes where it begins, before it moves the write cursor.
            const std::uint64_t tag = tagOf(epoch);
            if (firstTag_.load(std::memory_order_relaxed) != tag) {
                firstPosition_.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
                firstTag_.store(tag, std::memory_order_release);
            }
            taken = write(data);
        }
        writing_.store(false, std::memory_order_release);
        return taken;
    }

    /* The epoch and the boundary as they stand, for a buffer that has just reached the producer. */
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t boundaryNanoseconds() const noexcept { return boundary_.load(std::memory_order_acquire); }

    /* Begins a recording at `boundaryNanoseconds` (the producer's clock): drops what is held and every buffer a producer
     * received before this call, whenever it writes it, and keeps what the producer writes under the new epoch. A write
     * that is under way is waited for, a copy of one buffer. */
    void restart(std::uint64_t boundaryNanoseconds) noexcept
    {
        boundary_.store(boundaryNanoseconds, std::memory_order_release);
        const std::uint64_t next = epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
        Seam::afterPublish();
        while (writing_.load(std::memory_order_seq_cst))
            std::this_thread::yield();
        Seam::afterWait();
        const auto head = head_.load(std::memory_order_acquire);
        Seam::afterCursor();
        // The note is read after the cursor: a first write of the new epoch that begins in between begins at that cursor.
        const bool noted = firstTag_.load(std::memory_order_acquire) == tagOf(next);
        tail_.store(noted ? firstPosition_.load(std::memory_order_relaxed) : head, std::memory_order_release);
    }

    /* Moves up to out.size() of the oldest bytes into `out`. */
    std::size_t read(std::span<std::uint8_t> out) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        const auto count = std::min(out.size(), head - tail);
        copyOut(tail, out.first(count));
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return head_.load(std::memory_order_acquire) -
            tail_.load(std::memory_order_acquire);
    }

    /* Drops the `count` oldest bytes, or all that are held. */
    void discard(std::size_t count) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        tail_.store(tail + std::min(count, head - tail),
                    std::memory_order_release);
    }

    void clear() noexcept { discard(capacity_); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    // An epoch as a note: never zero, which is no note. An epoch is 64 bits wide, so none repeats while a ring lives
    // and a note of an earlier recording never passes for the present one's.
    [[nodiscard]] static constexpr std::uint64_t tagOf(std::uint64_t epoch) noexcept { return epoch + 1; }

    void copyIn(std::size_t position, std::span<const std::uint8_t> data) noexcept
    {
        const auto start = position & (capacity_ - 1U);
        const auto first = std::min(data.size(), capacity_ - start);
        std::memcpy(bytes_.get() + start, data.data(), first);
        std::memcpy(bytes_.get(), data.data() + first, data.size() - first);
    }

    void copyOut(std::size_t position, std::span<std::uint8_t> out) const noexcept
    {
        const auto start = position & (capacity_ - 1U);
        const auto first = std::min(out.size(), capacity_ - start);
        std::memcpy(out.data(), bytes_.get() + start, first);
        std::memcpy(out.data() + first, bytes_.get(), out.size() - first);
    }

    const std::size_t capacity_;
    std::unique_ptr<std::uint8_t[]> bytes_;
    // Free-running positions; their difference is the fill.
    std::atomic<std::size_t> head_ {};
    std::atomic<std::size_t> tail_ {};
    std::atomic<std::uint64_t> epoch_ {};
    std::atomic<std::uint64_t> boundary_ {};
    std::atomic<bool> writing_ {};
    // The producer's note of where its first write of an epoch began (tagOf(epoch), zero for none) and the position.
    std::atomic<std::uint64_t> firstTag_ {};
    std::atomic<std::size_t> firstPosition_ {};
};

using CaptureRing = BasicCaptureRing<NoRestartSeam>;

} // namespace uml::audio
