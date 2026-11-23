// SPDX-License-Identifier: Apache-2.0
//
// The one thread that calls the framework. The framework's callbacks are
// synchronous binder calls, and the framework answers one of them while it
// holds locks of its own, some of which its threads also hold while they call
// into this HAL (it registers a status listener under its listener lock and
// asks the HAL to collect satellite status from there). A HAL thread that
// waits for the framework while it holds a lock one of its own entry points
// takes can therefore close a cycle with a framework thread that waits for
// that entry point. So no entry point, and no thread that holds a lock of the
// HAL, calls the framework: they post what the framework is to hear, and this
// thread, which holds no lock of the HAL while it calls, makes the calls in
// the order they were posted. The HAL has one reporter, shared by every
// callback it ever has.
//
// The delivery contract, with the reference being the HAL that made every call
// inside its entry point, where whatever an entry point had reported was heard
// before it returned:
//  - An essential item (the announcement of a callback, and every status) is
//    never dropped and never reordered. It is delivered, whole, to the
//    callback it was posted for, in the order posted across every callback,
//    even if the HAL is closed or the callback replaced before the thread got
//    to it. A start that would leave more than kMaxStatuses essential items
//    undelivered is refused instead (reserve), so the queue is bounded and
//    nothing is dropped to bound it.
//  - A report item (a location, the sentences of an epoch, the satellites in
//    view) is droppable. Each belongs to a session's token. Retiring the token
//    (the session restarted, or the HAL closed or its callback replaced)
//    drops the queued reports of the session, and a report being delivered
//    stops before its next call. Past kMaxReports the oldest are dropped.
//  - Posting never waits for a delivery, and nothing here waits for one: no
//    call is made that the framework could answer while a lock of the HAL is
//    held. A new callback's items follow every essential item of the old one
//    and any call of the old one already in flight.
//  - The thread ends when the reporter is destroyed and the essential items
//    are all delivered. Destroying a reporter never waits for a call in
//    flight: it detaches the thread, which owns its queue and every callback
//    in it, delivers what is left and ends. At the end of the process nothing
//    more could have been delivered anyway.
// The header has no Android dependency, so a host test compiles it with
// callbacks that block.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace andarium::gnss {

class Reporter {
public:
    // A report's session: set once the session's reports are no longer wanted.
    using Token = std::shared_ptr<std::atomic<bool>>;

    static Token make_token() { return std::make_shared<std::atomic<bool>>(false); }

    // The newest reports kept when the framework is slow.
    static constexpr std::size_t kMaxReports = 64;
    // The essential items that may be waiting: the four statuses of a session,
    // for 64 sessions.
    static constexpr std::size_t kSessionStatuses = 4;
    static constexpr std::size_t kMaxStatuses = 64 * kSessionStatuses;

    Reporter() : state_(std::make_shared<State>()) {
        thread_ = std::thread([state = state_] { run(state); });
    }

    // Never waits for a call in flight, and never drops an essential item.
    ~Reporter() {
        bool idle = false;
        {
            std::lock_guard lock(state_->mutex);
            state_->closing = true;
            drop_reports(*state_, nullptr);
            idle = !state_->delivering && state_->queue.empty();
        }
        state_->wake.notify_all();
        if (idle) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }

    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    // Room for `units` more essential items: they are counted as taken, and
    // posted later without counting again (post_essential with `reserved`), or
    // given back with release. False when they do not fit.
    bool reserve(std::size_t units) {
        std::lock_guard lock(state_->mutex);
        if (state_->committed + units > kMaxStatuses) return false;
        state_->committed += units;
        return true;
    }

    void release(std::size_t units) {
        std::lock_guard lock(state_->mutex);
        state_->committed = units > state_->committed ? 0 : state_->committed - units;
    }

    // Queues an essential item after everything posted before it. One that was
    // not reserved counts for one unit, and is never refused.
    void post_essential(std::function<void()> deliver, bool reserved) {
        {
            std::lock_guard lock(state_->mutex);
            if (!reserved) ++state_->committed;
            state_->queue.push_back(Item{Kind::essential, nullptr, std::move(deliver)});
        }
        state_->wake.notify_one();
    }

    // Queues a report of the session `token`. Ignored once the token is
    // retired.
    void post_report(const Token& token, std::function<void()> deliver) {
        {
            std::lock_guard lock(state_->mutex);
            if (token->load()) return;
            if (state_->reports >= kMaxReports) drop_oldest_report(*state_);
            ++state_->reports;
            state_->queue.push_back(Item{Kind::report, token, std::move(deliver)});
        }
        state_->wake.notify_one();
    }

    // The reports of the session are not wanted: the queued ones are dropped,
    // and the one being delivered checks the token before each of its calls.
    void retire(const Token& token) {
        std::lock_guard lock(state_->mutex);
        token->store(true);
        drop_reports(*state_, &token);
    }

private:
    enum class Kind { essential, report };

    struct Item {
        Kind kind;
        Token token;
        std::function<void()> deliver;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Item> queue;
        std::size_t reports = 0;
        // The essential items queued and the units reserved for ones to come.
        std::size_t committed = 0;
        bool closing = false;
        bool delivering = false;
    };

    static void drop_oldest_report(State& state) {
        for (auto it = state.queue.begin(); it != state.queue.end(); ++it) {
            if (it->kind == Kind::report) {
                state.queue.erase(it);
                --state.reports;
                return;
            }
        }
    }

    // Drops the queued reports of `token`, or every one when it is null.
    static void drop_reports(State& state, const Token* token) {
        for (auto it = state.queue.begin(); it != state.queue.end();) {
            if (it->kind == Kind::report && (token == nullptr || it->token == *token)) {
                it = state.queue.erase(it);
                --state.reports;
            } else {
                ++it;
            }
        }
    }

    // Holds nothing of the HAL, and `state->mutex` never while it calls out.
    static void run(const std::shared_ptr<State>& state) {
        std::unique_lock lock(state->mutex);
        for (;;) {
            state->wake.wait(lock, [&] { return state->closing || !state->queue.empty(); });
            if (state->queue.empty()) return;
            Item item = std::move(state->queue.front());
            state->queue.pop_front();
            if (item.kind == Kind::report) {
                --state->reports;
            } else if (state->committed > 0) {
                --state->committed;
            }
            state->delivering = true;
            lock.unlock();
            item.deliver();
            item.deliver = nullptr;
            item.token = nullptr;
            lock.lock();
            state->delivering = false;
        }
    }

    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace andarium::gnss
