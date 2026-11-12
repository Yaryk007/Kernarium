// SPDX-License-Identifier: GPL-2.0-or-later

#include "reclaim_wait.hpp"

#include <algorithm>
#include <utility>

namespace uml::gpu {

namespace {

/* The wait of the allocation the calling thread runs. */
struct ThreadWait {
    const ReclaimWait* owner {};
    bool active {};
    std::uint64_t bytes {};
    ReclaimWait::Clock::time_point started {};
    ReclaimWait::Clock::time_point deadline {};
    ReclaimWait::Clock::time_point lastRetry {};
    std::uint32_t retries {};
    std::vector<std::uint32_t> watched;
    std::uint32_t expected {};
    /* The watched process that went, or 0. */
    std::uint32_t gone {};
};

thread_local ThreadWait threadWait;

} // namespace

void ReclaimWait::Host::finished(bool, Clock::duration, std::uint64_t) noexcept
{
}

std::optional<ReclaimWait::Plan> ReclaimWait::plan(Clock::time_point now,
    std::uint64_t shortfall, std::uint32_t requester,
    const std::vector<Holder>& holders)
{
    std::lock_guard lock(mutex_);
    const auto decline = [this](Declined reason) -> std::optional<Plan> {
        lastDeclined_ = reason;
        declined_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    };
    if (lastActivity_ && now - *lastActivity_ >= episodeGap) {
        kills_ = 0;
        goneThisEpisode_.clear();
    }
    std::erase_if(excluded_,
        [now](const auto& entry) { return entry.second <= now; });
    if (now < pausedUntil_)
        return decline(Declined::paused);
    if (kills_ >= killsPerEpisode)
        return decline(Declined::episodeSpent);
    /* The next process the reclaimer ends goes `killSpacing` after the last,
     * too late for a wait that begins sooner than this. */
    if (lastKill_ && now - *lastKill_ < killSpacing - limit)
        return decline(Declined::killTooRecent);

    Plan result;
    const Holder* largest = nullptr;
    for (const Holder& holder : holders) {
        if (holder.pid == 0 || holder.pid == requester ||
            excluded_.contains(holder.pid)) {
            continue;
        }
        result.watched.push_back(holder.pid);
        if (largest == nullptr || holder.bytes > largest->bytes)
            largest = &holder;
    }
    if (largest == nullptr)
        return decline(Declined::noHolder);
    if (largest->bytes < shortfall)
        return decline(Declined::holderTooSmall);
    result.expected = largest->pid;
    lastActivity_ = now;
    return result;
}

void ReclaimWait::decline(Declined reason) noexcept
{
    std::lock_guard lock(mutex_);
    lastDeclined_ = reason;
    declined_.fetch_add(1, std::memory_order_relaxed);
}

void ReclaimWait::holderGone(Clock::time_point now, std::uint32_t pid)
{
    std::lock_guard lock(mutex_);
    if (goneThisEpisode_.insert(pid).second)
        ++kills_;
    lastKill_ = now;
    lastActivity_ = now;
}

void ReclaimWait::ended(Clock::time_point now, bool fruitless,
    std::uint32_t expected)
{
    std::lock_guard lock(mutex_);
    lastActivity_ = now;
    if (!fruitless)
        return;
    pausedUntil_ = now + fruitlessPause;
    if (expected != 0)
        excluded_[expected] = now + holderExcluded;
}

bool ReclaimWait::wait(Host& host, std::uint32_t contextId,
    std::uint64_t bytes, std::uint32_t attempt)
{
    ThreadWait& state = threadWait;
    if (attempt == 0) {
        state = {};
        if (!host.reclaimerArmed()) {
            decline(Declined::notArmed);
            return false;
        }
        const auto lacking = host.shortfall(bytes);
        if (!lacking) {
            decline(Declined::noPool);
            return false;
        }
        std::vector<Holder> holders;
        try {
            holders = host.candidates();
        } catch (...) {
            decline(Declined::noHolder);
            return false;
        }
        const Clock::time_point now = host.now();
        /* Room that showed since the refusal still needs a holder to wait
         * for, in case the next attempt is refused again. */
        auto chosen = plan(now, std::max<std::uint64_t>(*lacking, 1),
            host.processOf(contextId), holders);
        if (!chosen)
            return false;
        state.owner = this;
        state.active = true;
        state.bytes = bytes;
        state.started = now;
        state.deadline = now + limit;
        state.watched = std::move(chosen->watched);
        state.expected = chosen->expected;
        waits_.fetch_add(1, std::memory_order_relaxed);
        host.announce();
    } else if (state.owner != this || !state.active) {
        return false;
    }

    for (;;) {
        const Clock::time_point now = host.now();
        const auto lacking = host.shortfall(bytes);
        if (!lacking)
            break;
        if (state.gone == 0) {
            for (const std::uint32_t pid : state.watched) {
                if (!host.holds(pid)) {
                    state.gone = pid;
                    holderGone(now, pid);
                    break;
                }
            }
        }
        /* An attempt runs the pool's own reclaim, which frees what the gone
         * holder left in the drivers' caches: try again while it may help,
         * but not more often than `retryInterval`. */
        const bool spaced =
            state.retries == 0 || now - state.lastRetry >= retryInterval;
        if (spaced && (*lacking == 0 || state.gone != 0) &&
            now < state.deadline) {
            state.lastRetry = now;
            ++state.retries;
            return true;
        }
        if (now >= state.deadline)
            break;
        host.sleep(poll);
    }
    finish(host, false);
    return false;
}

void ReclaimWait::finish(Host& host, bool admitted) noexcept
{
    ThreadWait& state = threadWait;
    state.active = false;
    const Clock::time_point now = host.now();
    ended(now, !admitted && state.gone == 0, state.expected);
    (admitted ? saves_ : failures_).fetch_add(1, std::memory_order_relaxed);
    host.finished(admitted, now - state.started, state.bytes);
}

void ReclaimWait::waited(Host& host, bool admitted) noexcept
{
    const ThreadWait& state = threadWait;
    if (state.owner == this && state.active)
        finish(host, admitted);
}

ReclaimWait::Counts ReclaimWait::counts() const noexcept
{
    return {
        .waits = waits_.load(std::memory_order_relaxed),
        .saves = saves_.load(std::memory_order_relaxed),
        .failures = failures_.load(std::memory_order_relaxed),
        .declined = declined_.load(std::memory_order_relaxed),
    };
}

std::optional<ReclaimWait::Declined> ReclaimWait::lastDeclined()
    const noexcept
{
    std::lock_guard lock(mutex_);
    return lastDeclined_;
}

} // namespace uml::gpu
