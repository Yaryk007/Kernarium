// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace uml::gpu {

/* Whether a Venus allocation the device memory pool refused waits for the
 * guest's reclaimer to end a cached application, and the wait itself.
 *
 * The reclaimer (lmkd's GPU reclaim, which the memory report arms) hears of a
 * refusal at once, ends the largest quiet holder it may end, and the pool sees
 * the memory about a second later. A refused allocation that waits that long
 * and is tried again is made instead of failing its application. It waits
 * only where the reclaimer could make the room in time:
 *
 *   - the reclaimer is armed;
 *   - some quiet holder, other than the requesting process, holds at least
 *     the bytes the pool lacks, by the reclaimer's own terms (the ledger's
 *     per-process figures): only one process is ended within `limit`;
 *   - the reclaimer may end one now: fewer than `killsPerEpisode` holders
 *     have gone in its episode, the last went at least `killSpacing - limit`
 *     ago, and the last wait that saw none go was `fruitlessPause` ago or
 *     more. A holder a wait saw stay is not waited for again for
 *     `holderExcluded`, since the reclaimer ends only cached applications.
 *
 * Otherwise the allocation is refused at once. A wait ends when room shows,
 * or when a holder it watches has gone, at which point the allocation is tried
 * again (every `retryInterval` after that, so the pool's own reclaim frees the
 * driver caches the holder left), or at `limit`. Thread-safe: each thread that
 * runs an allocation keeps its own wait. */
class ReclaimWait {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr Clock::duration limit = std::chrono::milliseconds(1500);
    static constexpr Clock::duration poll = std::chrono::milliseconds(5);
    static constexpr Clock::duration retryInterval =
        std::chrono::milliseconds(100);
    /* The reclaimer ends at most one process in this time, and this many
     * while its episode lasts. */
    static constexpr Clock::duration killSpacing = std::chrono::seconds(3);
    static constexpr unsigned killsPerEpisode = 3;
    /* The reclaimer's episode ends at the first report that shows neither
     * pressure nor a refusal, which it asks for every 3 s: the host takes two
     * of those without a wait as its end. */
    static constexpr Clock::duration episodeGap = std::chrono::seconds(6);
    /* A reclaimer that found nothing to end looks again after 10 s. */
    static constexpr Clock::duration fruitlessPause = std::chrono::seconds(10);
    static constexpr Clock::duration holderExcluded = std::chrono::seconds(60);

    struct Holder {
        std::uint32_t pid {};
        std::uint64_t bytes {};
    };

    /* What a wait reads and does. */
    class Host {
    public:
        virtual ~Host() = default;
        [[nodiscard]] virtual Clock::time_point now() const noexcept = 0;
        virtual void sleep(Clock::duration duration) noexcept = 0;
        /* Whether a reclaimer acts on the memory reports. */
        [[nodiscard]] virtual bool reclaimerArmed() const noexcept = 0;
        /* The bytes the pool lacks to admit `bytes` more, 0 when it has the
         * room, or nothing when there is no pool (the renderer is going). */
        [[nodiscard]] virtual std::optional<std::uint64_t> shortfall(
            std::uint64_t bytes) const noexcept = 0;
        /* The guest process of a context, or 0. */
        [[nodiscard]] virtual std::uint32_t processOf(
            std::uint32_t contextId) const noexcept = 0;
        /* The processes the reclaimer could end by its own terms (quiet and
         * holding enough), with what each holds. */
        [[nodiscard]] virtual std::vector<Holder> candidates() const = 0;
        /* Whether the process still has a context. */
        [[nodiscard]] virtual bool holds(std::uint32_t pid) const noexcept = 0;
        /* An allocation waits: tell the reclaimer at once. */
        virtual void announce() noexcept = 0;
        /* A wait of `waited` for `bytes` ended, with the allocation made or
         * refused. */
        virtual void finished(bool admitted, Clock::duration waited,
            std::uint64_t bytes) noexcept;
    };

    /* Why an allocation did not wait. */
    enum class Declined : std::uint8_t {
        notArmed,
        noPool,
        noHolder,
        holderTooSmall,
        episodeSpent,
        killTooRecent,
        paused,
    };

    struct Counts {
        std::uint64_t waits {};
        /* Waits whose allocation was made in the end. */
        std::uint64_t saves {};
        std::uint64_t failures {};
        /* Refusals that did not wait. */
        std::uint64_t declined {};
    };

    ReclaimWait() = default;
    ReclaimWait(const ReclaimWait&) = delete;
    ReclaimWait& operator=(const ReclaimWait&) = delete;

    /* An allocation of `bytes` for context `contextId` was refused, `attempt`
     * times since its first refusal. Waits as the class describes, on the
     * calling thread, and returns whether to try it again. */
    [[nodiscard]] bool wait(Host& host, std::uint32_t contextId,
        std::uint64_t bytes, std::uint32_t attempt);
    /* An allocation wait() returned true for was made, or refused in the
     * end. Called on the thread that waited. */
    void waited(Host& host, bool admitted) noexcept;

    [[nodiscard]] Counts counts() const noexcept;
    /* Why the last allocation that did not wait did not. */
    [[nodiscard]] std::optional<Declined> lastDeclined() const noexcept;

private:
    struct Plan {
        std::vector<std::uint32_t> watched;
        std::uint32_t expected {};
    };

    [[nodiscard]] std::optional<Plan> plan(Clock::time_point now,
        std::uint64_t shortfall, std::uint32_t requester,
        const std::vector<Holder>& holders);
    void finish(Host& host, bool admitted) noexcept;
    void decline(Declined reason) noexcept;
    void holderGone(Clock::time_point now, std::uint32_t pid);
    void ended(Clock::time_point now, bool gone, std::uint32_t expected);

    mutable std::mutex mutex_;
    unsigned kills_ {};
    std::set<std::uint32_t> goneThisEpisode_;
    std::optional<Clock::time_point> lastKill_;
    std::optional<Clock::time_point> lastActivity_;
    Clock::time_point pausedUntil_ {};
    std::map<std::uint32_t, Clock::time_point> excluded_;
    std::optional<Declined> lastDeclined_;

    std::atomic<std::uint64_t> waits_ {};
    std::atomic<std::uint64_t> saves_ {};
    std::atomic<std::uint64_t> failures_ {};
    std::atomic<std::uint64_t> declined_ {};
};

} // namespace uml::gpu
