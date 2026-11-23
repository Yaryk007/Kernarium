// SPDX-License-Identifier: GPL-2.0-or-later

/* A Venus allocation the pool refused waits for the guest's reclaimer only
 * where the reclaimer could make the room in time: it is armed, a quiet
 * holder other than the requester holds at least what the pool lacks, and the
 * reclaimer may end one now (its pace and its episode's limit, and a pause
 * after a wait that saw nothing go). A wait tells the guest at once, is tried
 * again once room shows or a holder has gone (and then at most every
 * retryInterval), and ends at its limit. The test plays the host with its own
 * clock, which a sleep advances. */

#include "reclaim_wait.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

using uml::gpu::ReclaimWait;
using uml::testing::require;
using Clock = ReclaimWait::Clock;
using Holder = ReclaimWait::Holder;
using Declined = ReclaimWait::Declined;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint64_t mebibyte = 1U << 20;
constexpr std::uint32_t requester = 900;
constexpr std::uint32_t requesterContext = 7;

class FakeHost final : public ReclaimWait::Host {
public:
    Clock::time_point clock = origin;
    bool armed = true;
    /* What the pool lacks, at a time; 100 MiB until told otherwise. */
    std::function<std::optional<std::uint64_t>(Clock::time_point)> lacking =
        [](Clock::time_point) -> std::optional<std::uint64_t> {
        return 100 * mebibyte;
    };
    std::vector<Holder> holders;
    /* When a holder's process goes. */
    std::map<std::uint32_t, Clock::time_point> goneAt;
    int announced = 0;
    std::vector<std::pair<bool, Clock::duration>> finishes;

    [[nodiscard]] Clock::time_point now() const noexcept override
    {
        return clock;
    }
    void sleep(Clock::duration duration) noexcept override
    {
        clock += duration;
    }
    [[nodiscard]] bool reclaimerArmed() const noexcept override
    {
        return armed;
    }
    [[nodiscard]] std::optional<std::uint64_t> shortfall(
        std::uint64_t) const noexcept override
    {
        return lacking(clock);
    }
    [[nodiscard]] std::uint32_t processOf(
        std::uint32_t contextId) const noexcept override
    {
        return contextId == requesterContext ? requester : 0;
    }
    [[nodiscard]] std::vector<Holder> candidates() const override
    {
        std::vector<Holder> present;
        for (const Holder& holder : holders) {
            if (holds(holder.pid))
                present.push_back(holder);
        }
        return present;
    }
    [[nodiscard]] bool holds(std::uint32_t pid) const noexcept override
    {
        const auto found = goneAt.find(pid);
        return found == goneAt.end() || clock < found->second;
    }
    void announce() noexcept override { ++announced; }
    void finished(bool admitted, Clock::duration waited,
        std::uint64_t) noexcept override
    {
        finishes.emplace_back(admitted, waited);
    }
};

/* One refused allocation of 64 MiB the way the renderer runs it: it is tried
 * again while the wait says so, and is made once `madeAt` has come (never, by
 * default). Returns whether it was made. */
bool allocate(ReclaimWait& wait, FakeHost& host,
    std::optional<Clock::time_point> madeAt = std::nullopt,
    std::uint32_t* attempts = nullptr)
{
    std::uint32_t attempt = 0;
    for (;; ++attempt) {
        if (attempt != 0 && madeAt && host.clock >= *madeAt) {
            wait.waited(host, true);
            break;
        }
        if (!wait.wait(host, requesterContext, 64 * mebibyte, attempt)) {
            if (attempt != 0)
                wait.waited(host, false);
            if (attempts)
                *attempts = attempt;
            return false;
        }
    }
    if (attempts)
        *attempts = attempt;
    return true;
}

void checkAnUnarmedGuestIsNotWaitedFor()
{
    ReclaimWait wait;
    FakeHost host;
    host.armed = false;
    host.holders = {{.pid = 100, .bytes = 500 * mebibyte}};
    require(!allocate(wait, host), "an unarmed guest made the allocation");
    require(host.clock == origin, "an unarmed guest was waited for");
    require(host.announced == 0, "an unarmed guest was told of a wait");
    require(wait.lastDeclined() == Declined::notArmed, "wrong reason");
    require(wait.counts().declined == 1 && wait.counts().waits == 0,
        "the refusal that did not wait was not counted as declined");
}

void checkAnImpossibleRequestFailsAtOnce()
{
    ReclaimWait wait;
    FakeHost host;
    require(!allocate(wait, host), "made with no holder");
    require(host.clock == origin, "waited with no holder");
    require(wait.lastDeclined() == Declined::noHolder, "wrong reason");

    /* The requester itself is no holder to wait for. */
    host.holders = {{.pid = requester, .bytes = 500 * mebibyte}};
    require(!allocate(wait, host), "made with only the requester holding");
    require(host.clock == origin, "waited for the requester itself");
    require(wait.lastDeclined() == Declined::noHolder, "wrong reason");

    /* Only one holder goes within the limit: the largest has to cover what
     * the pool lacks by itself. */
    host.holders = {{.pid = 100, .bytes = 99 * mebibyte},
        {.pid = 101, .bytes = 99 * mebibyte}};
    require(!allocate(wait, host), "made with holders too small");
    require(host.clock == origin, "waited for holders too small");
    require(wait.lastDeclined() == Declined::holderTooSmall, "wrong reason");
    require(host.announced == 0, "an impossible request was announced");
    require(wait.counts().waits == 0, "an impossible request counted a wait");

    /* No pool: the renderer is going. */
    host.holders = {{.pid = 100, .bytes = 500 * mebibyte}};
    host.lacking = [](Clock::time_point) -> std::optional<std::uint64_t> {
        return std::nullopt;
    };
    require(!allocate(wait, host), "made without a pool");
    require(wait.lastDeclined() == Declined::noPool, "wrong reason");
}

void checkAHolderThatGoesSavesTheAllocation()
{
    ReclaimWait wait;
    FakeHost host;
    host.holders = {{.pid = 100, .bytes = 200 * mebibyte},
        {.pid = requester, .bytes = 900 * mebibyte}};
    host.goneAt[100] = origin + 300ms;
    /* The pool shows the memory 400 ms after the holder went. */
    host.lacking = [](Clock::time_point now) -> std::optional<std::uint64_t> {
        return now < origin + 700ms ? 100 * mebibyte : 0;
    };
    std::uint32_t attempts = 0;
    require(allocate(wait, host, origin + 700ms, &attempts),
        "the allocation was not made once the holder went");
    require(host.announced == 1, "the wait was not announced once");
    /* Tried at the holder's going, then every retryInterval until made. */
    require(attempts == 5, "the attempts were not spaced by retryInterval");
    require(host.finishes.size() == 1 && host.finishes[0].first &&
            host.finishes[0].second == 700ms,
        "the save was not reported with its stall");
    const auto counts = wait.counts();
    require(counts.waits == 1 && counts.saves == 1 && counts.failures == 0,
        "the save was not counted");
}

void checkRoomThatShowsSavesTheAllocation()
{
    ReclaimWait wait;
    FakeHost host;
    host.holders = {{.pid = 100, .bytes = 200 * mebibyte}};
    host.lacking = [](Clock::time_point now) -> std::optional<std::uint64_t> {
        return now < origin + 50ms ? 100 * mebibyte : 0;
    };
    std::uint32_t attempts = 0;
    require(allocate(wait, host, origin + 50ms, &attempts),
        "room that showed did not make the allocation");
    require(attempts == 1, "room was not tried at once");
    require(wait.counts().saves == 1, "the save was not counted");
    /* No holder went, but the wait saved the allocation: nothing pauses. */
    host.clock = origin + 5s;
    host.lacking = [](Clock::time_point) -> std::optional<std::uint64_t> {
        return 100 * mebibyte;
    };
    host.goneAt[100] = origin + 5s + 100ms;
    require(allocate(wait, host, origin + 5s + 100ms),
        "a save without a kill paused the next wait");
}

void checkAFruitlessWaitEndsAtItsLimitAndPauses()
{
    ReclaimWait wait;
    FakeHost host;
    host.holders = {{.pid = 100, .bytes = 200 * mebibyte},
        {.pid = 101, .bytes = 150 * mebibyte}};
    require(!allocate(wait, host), "made with nothing freed");
    require(host.clock == origin + ReclaimWait::limit,
        "the wait did not end at its limit");
    require(host.finishes.size() == 1 && !host.finishes[0].first &&
            host.finishes[0].second == ReclaimWait::limit,
        "the failure was not reported with its stall");
    require(wait.counts().failures == 1, "the failure was not counted");

    /* The reclaimer found nothing it may end: it looks again after 10 s. */
    host.clock = origin + ReclaimWait::limit + 9s;
    require(!allocate(wait, host), "made while paused");
    require(host.clock == origin + ReclaimWait::limit + 9s,
        "waited while paused");
    require(wait.lastDeclined() == Declined::paused, "wrong reason");

    /* The holder it saw stay is not waited for again; the next one is. */
    host.clock = origin + 20s;
    host.goneAt[101] = origin + 20s + 200ms;
    host.lacking = [](Clock::time_point now) -> std::optional<std::uint64_t> {
        return now < origin + 20s + 200ms ? 100 * mebibyte : 0;
    };
    require(allocate(wait, host, origin + 20s + 200ms),
        "the next holder was not waited for");
    host.holders = {{.pid = 100, .bytes = 200 * mebibyte}};
    host.lacking = [](Clock::time_point) -> std::optional<std::uint64_t> {
        return 100 * mebibyte;
    };
    host.clock = origin + 40s;
    require(!allocate(wait, host), "made from an excluded holder");
    require(wait.lastDeclined() == Declined::noHolder,
        "a holder that stayed was waited for again");
    host.clock = origin + 90s;
    require(!allocate(wait, host), "made with nothing freed");
    require(host.clock == origin + 90s + ReclaimWait::limit,
        "the exclusion outlived holderExcluded");
}

void checkTheReclaimersPaceAndEpisode()
{
    ReclaimWait wait;
    FakeHost host;
    /* Make one holder go during each wait, as the reclaimer does. */
    const auto killDuring = [&host](std::uint32_t pid) {
        host.holders.push_back({.pid = pid, .bytes = 200 * mebibyte});
        host.goneAt[pid] = host.clock + 300ms;
        const auto freedAt = host.clock + 300ms;
        host.lacking = [freedAt](Clock::time_point now)
            -> std::optional<std::uint64_t> {
            return now < freedAt ? 100 * mebibyte : 0;
        };
        return freedAt;
    };
    const auto firstFreed = killDuring(100);
    require(allocate(wait, host, firstFreed), "the first kill did not save");

    /* The next kill comes 3 s after this one: too late for a wait now. */
    host.clock += 1s;
    host.holders = {{.pid = 101, .bytes = 200 * mebibyte}};
    host.lacking = [](Clock::time_point) -> std::optional<std::uint64_t> {
        return 100 * mebibyte;
    };
    const auto before = host.clock;
    require(!allocate(wait, host), "made right after a kill");
    require(host.clock == before, "waited right after a kill");
    require(wait.lastDeclined() == Declined::killTooRecent, "wrong reason");

    host.clock += 1s;
    const auto secondFreed = killDuring(102);
    require(allocate(wait, host, secondFreed), "the second kill did not save");
    host.clock += 2s;
    const auto thirdFreed = killDuring(103);
    require(allocate(wait, host, thirdFreed), "the third kill did not save");

    /* Three in one episode: the reclaimer ends no more. */
    host.clock += 2s;
    host.holders = {{.pid = 104, .bytes = 200 * mebibyte}};
    host.lacking = [](Clock::time_point) -> std::optional<std::uint64_t> {
        return 100 * mebibyte;
    };
    const auto spent = host.clock;
    require(!allocate(wait, host), "made past the episode's limit");
    require(host.clock == spent, "waited past the episode's limit");
    require(wait.lastDeclined() == Declined::episodeSpent, "wrong reason");

    /* The episode ends once nothing has waited for episodeGap. */
    host.clock = spent + ReclaimWait::episodeGap;
    const auto nextFreed = killDuring(105);
    require(allocate(wait, host, nextFreed), "a new episode did not wait");
    require(wait.counts().saves == 4, "the saves were not counted");
}

void checkARetryPastTheLimitIsRefused()
{
    ReclaimWait wait;
    FakeHost host;
    host.holders = {{.pid = 100, .bytes = 200 * mebibyte}};
    host.goneAt[100] = origin + 100ms;
    require(wait.wait(host, requesterContext, 64 * mebibyte, 0),
        "the holder's going was not tried");
    /* The attempt itself took past the limit. */
    host.clock = origin + ReclaimWait::limit + 1ms;
    require(!wait.wait(host, requesterContext, 64 * mebibyte, 1),
        "a retry past the limit was tried again");
    wait.waited(host, false);
    const auto counts = wait.counts();
    require(counts.waits == 1 && counts.failures == 1 && counts.saves == 0,
        "the failure was counted other than once");
    /* A retry the wait does not hold is refused. */
    require(!wait.wait(host, requesterContext, 64 * mebibyte, 1),
        "a retry with no wait was tried again");
}

} // namespace

int main()
{
    checkAnUnarmedGuestIsNotWaitedFor();
    checkAnImpossibleRequestFailsAtOnce();
    checkAHolderThatGoesSavesTheAllocation();
    checkRoomThatShowsSavesTheAllocation();
    checkAFruitlessWaitEndsAtItsLimitAndPauses();
    checkTheReclaimersPaceAndEpisode();
    checkARetryPastTheLimitIsRefused();
    return 0;
}
