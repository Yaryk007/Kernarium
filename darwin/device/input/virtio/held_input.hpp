// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>

namespace uml::input::virtio {

/* What a guest's input device has been told is held, from the events it was
 * given and without knowing which device sends them: an EV_KEY code (a key, a
 * button, BTN_TOUCH, BTN_TOOL_*) is held from an event with a nonzero value
 * until one with value 0, and a multitouch slot, which ABS_MT_SLOT selects,
 * holds a contact from a tracking ID other than -1 until -1. A host that
 * drops events the guest was meant to get keeps a key down or a contact in
 * the guest that nobody lifts; release() ends everything held in one report.
 *
 * Key codes at or above KEY_CNT and slots at or above `slotCount` are not
 * tracked, since a guest does not deliver the first and the touchscreen has
 * ten of the second. Not thread-safe. */
class HeldInput final {
public:
    static constexpr std::size_t keyCount = 0x300;
    static constexpr std::size_t slotCount = 32;

    /* Follows events the guest is given. */
    void observe(std::span<const protocol::InputEvent> events) noexcept;

    /* Whether the guest holds a key or a contact. */
    [[nodiscard]] bool any() const noexcept;

    /* The number of events appendRelease() appends. */
    [[nodiscard]] std::size_t releaseSize() const noexcept;

    /* Appends one report that ends everything held, nothing when nothing is
     * held, and records that the guest now holds nothing:
     *  - each held key, in code order, with value 0;
     *  - each held slot, in slot order, selected and given tracking ID -1
     *    (BTN_TOUCH comes first, which Linux's legacy touch emulation
     *    needs, as the touchscreen's own reports have it);
     *  - the slot the guest had selected, selected again, so the report
     *    that follows reads as it was written;
     *  - SYN_REPORT. */
    void appendRelease(std::deque<protocol::InputEvent>& events) noexcept;

    /* The guest holds nothing and has the first slot selected, as after a
     * reset. */
    void reset() noexcept;

private:
    /* The highest held slot, which a release selects last; slotCount when
     * none is held. */
    [[nodiscard]] std::size_t lastHeldSlot() const noexcept;
    /* Whether a release must select the guest's slot again. */
    [[nodiscard]] bool restoresSlot() const noexcept;

    std::bitset<keyCount> keys_;
    std::bitset<slotCount> contacts_;
    std::int32_t slot_ {};
};

} // namespace uml::input::virtio
