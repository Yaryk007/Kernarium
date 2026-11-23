// SPDX-License-Identifier: Apache-2.0
//
// The state behind the AIDL IGnss of the host's virtual receiver: the engine of
// the framework's current callback, and the position mode and toggles that
// outlive one callback. Gnss.cpp is only the AIDL adapter over this class, so
// the entry points the framework's binder threads call, and the locks they
// take, live in a header a host test compiles with fakes.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "gnss_engine.hpp"
#include "gnss_reporter.hpp"

namespace andarium::gnss {

// The framework's binder threads call these entry points, any of them at any
// time, and a framework thread may hold a lock of its own while it does. No
// entry point calls the framework or waits for a delivery to it: every call is
// posted to the one Reporter of the HAL (gnss_reporter.hpp), which makes them
// from a thread of its own. So the locks here are held only across work that
// does not depend on the framework: joining the receiver's reader thread (which
// only posts), the wakelock, and opening the receiver.
//  - `ops_` serializes start, stop, close and setCallback, so that what they
//    post is in the order they ran, across callbacks.
//  - `mutex_` guards the engine pointer and the settings, and is held only for
//    moves, copies and atomic stores. It is never held across a call that takes
//    `ops_` or an engine's control lock.
// The order is `ops_`, then `mutex_`. The settings entry points take only
// `mutex_`, and the engine functions they call under it are atomic stores.
class Hal {
public:
    enum class StartResult { started, no_callback, no_receiver };

    explicit Hal(Env env) : env_(std::move(env)), reporter_(std::make_shared<Reporter>()) {}

    ~Hal() { close(); }

    // The framework's setCallback: a new engine for the new callback replaces
    // the old one, which ends silently (what it posted is still delivered, to
    // its callback, before anything of the new one). The engine tells the
    // callback what the receiver is (`sink.ready`) before anything else.
    void set_callback(Sink sink) {
        std::lock_guard ops(ops_);
        const std::shared_ptr<Engine> old = current();
        if (old) old->close();
        auto fresh = std::make_shared<Engine>(env_, std::move(sink), reporter_);
        std::lock_guard lock(mutex_);
        fresh->set_min_interval_ms(interval_ms_);
        fresh->set_nmea(nmea_);
        fresh->set_sv_status(sv_status_);
        engine_ = std::move(fresh);
    }

    StartResult start() {
        std::lock_guard ops(ops_);
        const auto engine = current();
        if (!engine) return StartResult::no_callback;
        return engine->start() ? StartResult::started : StartResult::no_receiver;
    }

    void stop() {
        std::lock_guard ops(ops_);
        if (const auto engine = current()) engine->stop();
    }

    // The framework's close(): the session ends silently and the callback is
    // dropped, with the locations still queued for it.
    void close() {
        std::lock_guard ops(ops_);
        std::shared_ptr<Engine> old;
        {
            std::lock_guard lock(mutex_);
            old = std::move(engine_);
        }
        if (old) old->close();
    }

    // False when the interval is not positive.
    bool set_position_mode(int interval_ms) {
        if (interval_ms <= 0) return false;
        std::lock_guard lock(mutex_);
        interval_ms_ = interval_ms;
        if (engine_) engine_->set_min_interval_ms(interval_ms);
        return true;
    }

    void set_sv_status(bool on) {
        std::lock_guard lock(mutex_);
        sv_status_ = on;
        if (engine_) engine_->set_sv_status(on);
    }

    void set_nmea(bool on) {
        std::lock_guard lock(mutex_);
        nmea_ = on;
        if (engine_) engine_->set_nmea(on);
    }

    std::optional<DebugFix> last_fix() {
        const auto engine = current();
        if (!engine) return std::nullopt;
        return engine->last_fix();
    }

private:
    std::shared_ptr<Engine> current() {
        std::lock_guard lock(mutex_);
        return engine_;
    }

    const Env env_;
    const std::shared_ptr<Reporter> reporter_;
    std::mutex ops_;
    // Guards the members below, and nothing else.
    std::mutex mutex_;
    std::shared_ptr<Engine> engine_;
    int interval_ms_ = Engine::kDefaultIntervalMs;
    bool nmea_ = false;
    bool sv_status_ = false;
};

}  // namespace andarium::gnss
