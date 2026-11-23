// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * What the runtime's calls that write the guest's storage back
 * (kernarium_runtime_settle: sync, quiesce, quiesce_resident) do around the
 * emulator library's own call: the report's envelope, and a handle that a sync
 * can use without holding up the lifecycle calls. Header-only so that a test
 * drives the same code with a fake library.
 */

#include "KernariumRuntime.h"
#include "uml/emulator.h"

#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace kernarium::settle {

/* The runtime's report shares the library's values, so a report is copied word
 * for word and neither can drift from the other. */
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_NOT_REPORTED) ==
    static_cast<int>(UML_EMULATOR_GUEST_STORAGE_NOT_REPORTED));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_CONFIRMED) ==
    static_cast<int>(UML_EMULATOR_GUEST_STORAGE_CONFIRMED));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_NO_GUEST) ==
    static_cast<int>(UML_EMULATOR_GUEST_STORAGE_NO_GUEST));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_UNCONFIRMED) ==
    static_cast<int>(UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_REFUSED) ==
    static_cast<int>(UML_EMULATOR_GUEST_STORAGE_REFUSED));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_UNREACHABLE) ==
    static_cast<int>(UML_EMULATOR_GUEST_STORAGE_UNREACHABLE));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED) ==
    static_cast<int>(UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_SUSPENSION_SUSPENDED) ==
    static_cast<int>(UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED));
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_SUSPENDED) ==
    static_cast<int>(UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED));
/* The zero value claims nothing, so a report that was never written, as one
 * a failed call leaves, is no confirmation. */
static_assert(static_cast<int>(KERNARIUM_RUNTIME_GUEST_STORAGE_NOT_REPORTED) == 0);

/* Whether a caller's report can be filled: none was asked for, or it is large
 * enough for every field this runtime writes. */
[[nodiscard]] inline bool usable(const kernarium_runtime_settle* settle) noexcept
{
    return settle == nullptr || settle->struct_size >= sizeof(*settle);
}

/* No guest runs: nothing to flush and nothing to settle. */
inline void reportNoGuest(kernarium_runtime_settle* settle) noexcept
{
    if (settle == nullptr)
        return;
    settle->reserved = 0U;
    settle->guest_storage = KERNARIUM_RUNTIME_GUEST_STORAGE_NO_GUEST;
    settle->guest_suspension = KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED;
}

/* The library's report, which the call fills only when it succeeds. */
inline void report(kernarium_runtime_settle* settle, const uml_emulator_settle& library) noexcept
{
    if (settle == nullptr)
        return;
    settle->reserved = 0U;
    settle->guest_storage = static_cast<uint32_t>(library.guest_storage);
    settle->guest_suspension = static_cast<uint32_t>(library.guest_suspension);
}

/*
 * One settling call on `emulator`: `call(emulator, &library)` is the library's
 * own. With no emulator there is no guest. The runtime's report is written only
 * when the library returns 0 or more; a failure leaves the caller's report as
 * the caller set it. The library's report starts as "not reported", never as
 * a confirmation.
 */
template <class Call>
[[nodiscard]] int run(kernarium_runtime_settle* settle, uml_emulator* emulator, Call&& call)
{
    if (!usable(settle))
        return -EINVAL;
    if (emulator == nullptr) {
        reportNoGuest(settle);
        return 0;
    }
    uml_emulator_settle library {};
    library.struct_size = sizeof(library);
    const int result = call(emulator, &library);
    if (result >= 0)
        report(settle, library);
    return result;
}

/*
 * The emulator that runs, for a call that waits on the guest. A sync can take
 * seconds, and the library bounds how long it waits for a lifecycle call of its
 * own, so a sync must not queue behind the runtime's lifecycle mutex, which a
 * quiesce holds for as long as it takes: it pins the handle instead, and the
 * registration that ends the guest waits for every pin before the handle can be
 * destroyed.
 */
class Gate final {
public:
    class Pin final {
    public:
        Pin(Gate* gate, uml_emulator* emulator) noexcept
            : gate_(gate)
            , emulator_(emulator)
        {
        }

        Pin(Pin&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr))
            , emulator_(other.emulator_)
        {
        }

        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        Pin& operator=(Pin&&) = delete;

        ~Pin()
        {
            if (gate_ != nullptr)
                gate_->unpin();
        }

        /* Null when no emulator runs. */
        [[nodiscard]] uml_emulator* emulator() const noexcept { return emulator_; }

    private:
        Gate* gate_;
        uml_emulator* emulator_;
    };

    /* The emulator that now runs. */
    void open(uml_emulator* emulator)
    {
        std::lock_guard lock(mutex_);
        emulator_ = emulator;
    }

    /* The emulator is no longer named, and no call that pinned it is left. */
    void close()
    {
        std::unique_lock lock(mutex_);
        emulator_ = nullptr;
        idle_.wait(lock, [this] { return pins_ == 0U; });
    }

    [[nodiscard]] Pin pin()
    {
        std::lock_guard lock(mutex_);
        if (emulator_ == nullptr)
            return Pin(nullptr, nullptr);
        ++pins_;
        return Pin(this, emulator_);
    }

private:
    void unpin() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            --pins_;
        }
        idle_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable idle_;
    uml_emulator* emulator_ {};
    unsigned pins_ {};
};

/* The runtime's sync: a settling call on the pinned emulator. */
template <class Call>
[[nodiscard]] int sync(Gate& gate, kernarium_runtime_settle* settle, Call&& library)
{
    if (!usable(settle))
        return -EINVAL;
    const Gate::Pin pin = gate.pin();
    return run(settle, pin.emulator(), library);
}

} // namespace kernarium::settle
