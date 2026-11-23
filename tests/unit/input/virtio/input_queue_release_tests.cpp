// SPDX-License-Identifier: GPL-2.0-or-later

// What a full virtio-input queue owes the guest. A report the queue drops
// can carry the lift of a contact or the release of a key, and the guest
// would hold it until something else released it. The queue therefore
// releases what the guest was told is held before the next report it
// accepts. The same holds for the hardware keyboard, mouse and pointer, whose
// devices the host plugs and unplugs and whose queue owes nothing to a guest
// that has not registered an input device.

#include "hardware_test_support.hpp"
#include "held_input.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "touchscreen.hpp"
#include "virtio_input_device.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::HeldInput;
using uml::input::virtio::Touchscreen;
using uml::input::virtio::VirtioInputDevice;
using uml::input::virtio::testing::Rig;
namespace protocol = uml::input::virtio::protocol;

using uml::testing::require;

using Events = std::vector<protocol::InputEvent>;

// EventQueue's limit, which the tests fill exactly.
constexpr std::size_t queueCapacity = 4096;

constexpr std::uint16_t eventRelative = 0x02;
constexpr std::uint16_t relativeX = 0x00;
constexpr std::uint16_t buttonLeft = 0x110;
constexpr std::uint16_t buttonRight = 0x111;
constexpr std::uint16_t keyA = 30;
constexpr std::uint16_t keyB = 48;

constexpr protocol::InputEvent synchronize()
{
    return {protocol::eventSynchronize, protocol::synchronizeReport, 0};
}

constexpr protocol::InputEvent key(std::uint16_t code, std::int32_t value)
{
    return {protocol::eventKey, code, value};
}

constexpr protocol::InputEvent absolute(
    std::uint16_t code, std::int32_t value)
{
    return {protocol::eventAbsolute, code, value};
}

constexpr protocol::InputEvent slot(std::int32_t index)
{
    return absolute(protocol::absoluteMtSlot, index);
}

constexpr protocol::InputEvent lift()
{
    return absolute(protocol::absoluteMtTrackingId, -1);
}

Events concat(std::initializer_list<Events> parts)
{
    Events all;
    for (const auto& part : parts)
        all.insert(all.end(), part.begin(), part.end());
    return all;
}

Events touchEvents(const uml::input::virtio::TouchscreenReport& report)
{
    return Events(report.span().begin(), report.span().end());
}

std::string describe(const protocol::InputEvent& event)
{
    char text[64];
    std::snprintf(
        text, sizeof(text), "{type %u code %u value %d}",
        static_cast<unsigned>(event.type),
        static_cast<unsigned>(event.code),
        static_cast<int>(event.value));
    return text;
}

// Compares two event streams and names the first difference.
void requireStream(
    const Events& got,
    const Events& want,
    const std::source_location where = std::source_location::current())
{
    const auto common = std::min(got.size(), want.size());
    for (std::size_t index = 0; index < common; ++index) {
        if (std::memcmp(&got[index], &want[index], sizeof(got[index])) != 0) {
            uml::testing::fail(
                "event " + std::to_string(index) + " is " +
                    describe(got[index]) + ", wanted " +
                    describe(want[index]),
                where);
        }
    }
    if (got.size() != want.size()) {
        uml::testing::fail(
            "the stream has " + std::to_string(got.size()) +
                " events, wanted " + std::to_string(want.size()),
            where);
    }
}

// Whether a stream ends with `tail`.
bool sameStreamEnd(const Events& got, const Events& tail)
{
    if (got.size() < tail.size())
        return false;
    return std::memcmp(
               got.data() + (got.size() - tail.size()), tail.data(),
               tail.size() * sizeof(protocol::InputEvent)) == 0;
}

// What a guest's input core believes after the events it was given.
struct Guest final {
    std::bitset<0x300> keys;
    std::bitset<16> contacts;
    std::int32_t slot {};

    void apply(const protocol::InputEvent& event)
    {
        if (event.type == protocol::eventKey && event.code < keys.size()) {
            keys.set(event.code, event.value != 0);
        } else if (event.type == protocol::eventAbsolute &&
                   event.code == protocol::absoluteMtSlot) {
            slot = event.value;
        } else if (event.type == protocol::eventAbsolute &&
                   event.code == protocol::absoluteMtTrackingId &&
                   slot >= 0 &&
                   static_cast<std::size_t>(slot) < contacts.size()) {
            contacts.set(static_cast<std::size_t>(slot), event.value != -1);
        }
    }

    void apply(const Events& events)
    {
        for (const auto& event : events)
            apply(event);
    }

    [[nodiscard]] bool idle() const
    {
        return keys.none() && contacts.none();
    }
};

// An EventQueue and a count of what it holds, which lets a test fill it
// exactly: a report goes in whole or not at all.
class Harness final {
public:
    EventQueue queue;

    // A report the queue has room for.
    void send(const Events& events)
    {
        require(events.size() <= queueCapacity - queued_);
        queue.enqueue(events);
        queued_ += events.size();
    }

    // A report the queue has no room for: it is dropped.
    void reject(const Events& events)
    {
        require(events.size() > queueCapacity - queued_);
        queue.enqueue(events);
    }

    // Fills the queue with empty reports, up to its capacity.
    void fill()
    {
        while (queued_ < queueCapacity)
            send(Events {synchronize()});
    }

    Events take(std::size_t count)
    {
        Events taken;
        protocol::InputEvent event {};
        while (taken.size() < count && queue.take(event))
            taken.push_back(event);
        require(taken.size() == count);
        queued_ -= count;
        return taken;
    }

    Events drainAll()
    {
        Events taken;
        protocol::InputEvent event {};
        while (queue.take(event))
            taken.push_back(event);
        queued_ = 0;
        return taken;
    }

    void forget()
    {
        queued_ = 0;
    }

private:
    std::size_t queued_ {};
};

// Standard error, captured for the test's duration.
class CapturedStderr final {
public:
    CapturedStderr()
        : file_(std::tmpfile())
        , saved_(::dup(STDERR_FILENO))
    {
        require(file_ != nullptr && saved_ >= 0);
        std::fflush(stderr);
        require(::dup2(::fileno(file_), STDERR_FILENO) >= 0);
    }

    CapturedStderr(const CapturedStderr&) = delete;
    CapturedStderr& operator=(const CapturedStderr&) = delete;

    std::string finish()
    {
        std::fflush(stderr);
        require(::dup2(saved_, STDERR_FILENO) >= 0);
        ::close(saved_);
        std::string text;
        std::rewind(file_);
        std::array<char, 256> buffer {};
        std::size_t count = 0;
        while ((count = std::fread(buffer.data(), 1, buffer.size(), file_)) !=
               0) {
            text.append(buffer.data(), count);
        }
        std::fclose(file_);
        return text;
    }

private:
    std::FILE* file_;
    int saved_;
};

std::size_t occurrences(const std::string& text, const char* needle)
{
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

// The report that lifts a lone contact in slot 0 and nothing else.
Events liftOfSlotZero()
{
    return {key(protocol::buttonTouch, 0), slot(0), lift(), synchronize()};
}

// A guest that reads part of a full queue leaves room without draining it.
// Then the host sends `next`. The guest must go on to read what was queued
// before, then `release`, then `next`. Returns what was queued before.
Events readAround(Harness& h, const Events& release, const Events& next)
{
    constexpr std::size_t readBefore = 64;
    auto all = h.take(readBefore);
    h.send(next);
    const auto rest = h.drainAll();
    all.insert(all.end(), rest.begin(), rest.end());
    const auto tail = concat({release, next});
    require(all.size() == queueCapacity + tail.size());
    requireStream(Events(all.end() - tail.size(), all.end()), tail);
    return Events(all.begin(), all.end() - tail.size());
}

void aDroppedLiftIsReleasedBeforeTheNextReport()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    const auto next = touchEvents(screen.touch(0, 300, 400, true));
    const auto stale = readAround(h, liftOfSlotZero(), next);

    // Without the release the guest would still have the touch.
    Guest guest;
    guest.apply(stale);
    require(guest.contacts.test(0));
    require(guest.keys.test(protocol::buttonTouch));
    guest.apply(liftOfSlotZero());
    require(guest.idle());
    guest.apply(next);
    require(guest.contacts.test(0));
}

void aDroppedMoveStillReleasesTheContactItHeld()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(1, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(1, 150, 250, true)));

    // The contact stays down on the host. The guest is told it lifted
    // before the report that follows, which moves it.
    const auto move = touchEvents(screen.touch(1, 500, 600, true));
    const Events released {
        key(protocol::buttonTouch, 0), slot(1), lift(), synchronize()};
    Guest guest;
    guest.apply(readAround(h, released, move));
    require(guest.contacts.test(1));
    guest.apply(released);
    require(guest.idle());
    // The move names no tracking ID, so it does not start the contact
    // again; the report still sets BTN_TOUCH, which the contact's real lift
    // clears.
    guest.apply(move);
    require(guest.contacts.none());

    // The slot takes a new touch again, which is a new contact.
    (void)screen.touch(1, 500, 600, false);
    guest.apply(touchEvents(screen.touch(1, 10, 20, true)));
    require(guest.contacts.test(1));
}

void severalContactsAreAllReleasedInSlotOrder()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 100, true)));
    h.send(touchEvents(screen.touch(3, 200, 200, true)));
    h.send(touchEvents(screen.touch(7, 300, 300, true)));
    // The guest is told slot 3 lifts, and its current slot is then 3.
    h.send(touchEvents(screen.touch(3, 200, 200, false)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 100, false)));
    h.reject(touchEvents(screen.touch(7, 310, 310, true)));

    // BTN_TOUCH is held until the last contact lifts. The slot the guest
    // had selected is selected again, so the report that follows reads as
    // it was written.
    const Events releases {
        key(protocol::buttonTouch, 0),
        slot(0), lift(),
        slot(7), lift(),
        slot(3),
        synchronize(),
    };
    const auto next = touchEvents(screen.touch(3, 50, 60, true));
    Guest guest;
    guest.apply(readAround(h, releases, next));
    require(guest.contacts.test(0));
    require(!guest.contacts.test(3));
    require(guest.contacts.test(7));
    require(guest.slot == 3);

    guest.apply(releases);
    require(guest.idle());
    require(guest.slot == 3);
}

void aDroppedKeyReleaseReachesTheGuestThroughTheDevice()
{
    auto queue = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, queue);
    // Reads up to `limit` events, as many as the guest posts buffers for.
    const auto read = [&device](std::size_t limit) {
        Events events;
        while (events.size() < limit) {
            const auto result =
                device.processQueue(protocol::eventQueue, 1, {}, 8);
            if (result.action != uml::virtio::VhostUserQueueAction::complete)
                break;
            events.push_back(
                uml::testing::loadExact<protocol::InputEvent>(
                    result.response));
        }
        return events;
    };

    queue->enqueue(Events {key(keyA, 1), synchronize()});
    for (std::size_t queued = 2; queued < queueCapacity; ++queued)
        queue->enqueue(Events {synchronize()});
    queue->enqueue(Events {key(keyA, 0), synchronize()}); // dropped

    auto all = read(64);
    require(all.size() == 64);
    queue->enqueue(Events {key(keyB, 1), synchronize()});
    const auto rest = read(queueCapacity);
    all.insert(all.end(), rest.begin(), rest.end());
    const Events tail {
        key(keyA, 0), synchronize(), key(keyB, 1), synchronize()};
    require(all.size() == queueCapacity + tail.size());
    requireStream(Events(all.end() - tail.size(), all.end()), tail);

    Guest guest;
    guest.apply(Events(all.begin(), all.end() - tail.size()));
    require(guest.keys.test(keyA));
    guest.apply(Events(tail.begin(), tail.begin() + 2));
    require(guest.idle());
}

void aDroppedMouseButtonReleaseIsReleasedWithoutTouchKnowledge()
{
    // A mouse sends relative motion and buttons, and no contacts.
    Harness h;
    h.send({key(buttonLeft, 1), synchronize()});
    h.send({key(buttonRight, 1), synchronize()});
    h.send({key(buttonRight, 0), synchronize()});
    h.send({{eventRelative, relativeX, 5}, synchronize()});
    h.fill();
    h.reject({key(buttonLeft, 0), synchronize()});

    const Events motion {{eventRelative, relativeX, -3}, synchronize()};
    Guest guest;
    guest.apply(readAround(
        h, Events {key(buttonLeft, 0), synchronize()}, motion));
    require(guest.keys.test(buttonLeft));
    require(!guest.keys.test(buttonRight));
}

void everyHeldKeyIsReleasedInCodeOrder()
{
    Harness h;
    h.send({key(keyB, 1), synchronize()});
    h.send({key(keyA, 1), synchronize()});
    h.send({key(keyB, 2), synchronize()}); // autorepeat: still held
    h.send({key(buttonLeft, 1), synchronize()});
    h.send({key(keyA, 0), synchronize()});
    h.send({key(keyA, 1), synchronize()});
    h.fill();
    h.reject({key(keyB, 0), synchronize()});

    const Events next {key(keyA, 0), synchronize()};
    (void)readAround(
        h,
        Events {
            key(keyA, 0), key(keyB, 0), key(buttonLeft, 0), synchronize()},
        next);
}

void nothingIsSynthesizedWithoutAnOverflow()
{
    Harness h;
    Touchscreen screen;
    Events sent;
    const auto push = [&](const Events& events) {
        h.send(events);
        sent.insert(sent.end(), events.begin(), events.end());
    };
    const auto pushReport = [&](const auto& report) {
        push(touchEvents(report));
    };
    pushReport(screen.touch(0, 100, 200, true));
    pushReport(screen.touch(0, 110, 210, true));
    pushReport(screen.touch(1, 300, 400, true));
    push({key(keyA, 1), synchronize()});
    push({key(buttonLeft, 1), synchronize()});
    pushReport(screen.touch(0, 110, 210, false));
    push({key(keyA, 0), synchronize()});
    push({key(buttonLeft, 0), synchronize()});
    pushReport(screen.touch(1, 300, 400, false));
    pushReport(screen.touch(0, 5, 6, true));
    requireStream(h.drainAll(), sent);
}

void anOverflowWithNothingHeldSynthesizesNothing()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.send(touchEvents(screen.touch(0, 100, 200, false)));
    h.send({key(keyA, 1), synchronize()});
    h.send({key(keyA, 0), synchronize()});
    h.fill();
    h.reject({{eventRelative, relativeX, 1}, synchronize()});
    require(h.drainAll().size() == queueCapacity);

    const Events next {key(keyB, 1), synchronize()};
    h.send(next);
    requireStream(h.drainAll(), next);
}

void theReleaseWaitsForRoomForItAndTheReport()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The release is four events and the report two, so six must be free.
    // A queue that kept the report alone would take it at four.
    const Events report {absolute(protocol::absoluteX, 77), synchronize()};
    (void)h.take(4);
    h.queue.enqueue(report);
    (void)h.take(1);
    h.queue.enqueue(report);
    (void)h.take(1);
    h.queue.enqueue(report);

    const auto rest = h.drainAll();
    require(rest.size() == queueCapacity);
    requireStream(
        Events(rest.end() - 6, rest.end()),
        concat({liftOfSlotZero(), report}));
}

void clearForgetsAPendingRelease()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The guest reset: it holds nothing, whatever it was told before.
    h.queue.clear();
    h.forget();
    const auto next = touchEvents(screen.touch(0, 300, 400, true));
    h.send(next);
    requireStream(h.drainAll(), next);
}

void clearForgetsWhatTheGuestHeld()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.send({key(keyA, 1), synchronize()});
    h.queue.clear();
    h.forget();

    h.fill();
    h.reject({synchronize()});
    require(h.drainAll().size() == queueCapacity);
    const Events next {key(keyB, 1), synchronize()};
    h.send(next);
    requireStream(h.drainAll(), next);
}

void theOverflowIsReportedOnce()
{
    CapturedStderr captured;
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));
    h.reject({key(keyA, 1), synchronize()});
    (void)h.drainAll();

    // The queue releases the contact, then fills and overflows again.
    h.send(touchEvents(screen.touch(0, 300, 400, true)));
    (void)h.drainAll();
    h.fill();
    h.reject(touchEvents(screen.touch(0, 300, 400, false)));
    h.reject(touchEvents(screen.touch(1, 300, 400, true)));
    const auto text = captured.finish();
    require(occurrences(
                text,
                "dropping input because the virtio queue is full") == 1);
}

void aDroppedFinalLiftIsReleasedAtDrainWithoutAFurtherReport()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The person has lifted and sends nothing more. The guest reads
    // everything and then the release, so it holds nothing.
    const auto all = h.drainAll();
    const auto release = liftOfSlotZero();
    require(all.size() == queueCapacity + release.size());
    requireStream(Events(all.end() - release.size(), all.end()), release);
    Guest guest;
    guest.apply(Events(all.begin(), all.end() - release.size()));
    require(guest.contacts.test(0));
    guest.apply(release);
    require(guest.idle());

    protocol::InputEvent event {};
    require(!h.queue.take(event));
}

void everyDroppedFinalReleaseIsReleasedAtDrain()
{
    // Two contacts and a button: the guest holds all three when the queue
    // fills, and the lifts are dropped.
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(2, 100, 100, true)));
    h.send(touchEvents(screen.touch(5, 200, 200, true)));
    h.send({key(buttonLeft, 1), synchronize()});
    h.fill();
    h.reject(touchEvents(screen.touch(5, 200, 200, false)));
    h.reject(touchEvents(screen.touch(2, 100, 100, false)));
    h.reject({key(buttonLeft, 0), synchronize()});

    const auto all = h.drainAll();
    const Events release {
        key(buttonLeft, 0), key(protocol::buttonTouch, 0),
        slot(2), lift(),
        slot(5), lift(),
        synchronize(),
    };
    require(all.size() == queueCapacity + release.size());
    requireStream(Events(all.end() - release.size(), all.end()), release);
    Guest guest;
    guest.apply(all);
    require(guest.idle());
}

void aDrainWithNothingHeldEmitsNothing()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.send(touchEvents(screen.touch(0, 100, 200, false)));
    h.send({key(keyA, 1), synchronize()});
    h.send({key(keyA, 0), synchronize()});
    h.fill();
    h.reject({{eventRelative, relativeX, 1}, synchronize()});

    require(h.drainAll().size() == queueCapacity);
    protocol::InputEvent event {};
    require(!h.queue.take(event));

    // The drop is forgotten with nothing owed, so the next report goes
    // alone, as does a drain after it.
    const Events next {key(keyB, 1), synchronize()};
    h.send(next);
    requireStream(h.drainAll(), next);
    require(!h.queue.take(event));
}

void aReleaseAtDrainIsNotFollowedByASecondOne()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));
    require(h.drainAll().size() == queueCapacity + liftOfSlotZero().size());

    const auto next = touchEvents(screen.touch(0, 300, 400, true));
    h.send(next);
    requireStream(h.drainAll(), next);
}

void aReleaseWithTheNextReportIsNotFollowedByASecondOneAtDrain()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The release goes in with the report that follows, and the drain that
    // empties the queue adds nothing.
    const auto next = touchEvents(screen.touch(0, 300, 400, true));
    (void)readAround(h, liftOfSlotZero(), next);
    protocol::InputEvent event {};
    require(!h.queue.take(event));
}

void aReaderRacingTheHostEndsWithNothingHeld()
{
    // The host moves one or two contacts at full rate and lifts them; the
    // reader pauses now and then, so the queue overflows at times, whatever
    // report the last drop takes. When the host has lifted everything and the
    // reader has drained the queue, the guest holds nothing, with the release
    // from the reader's drain and from the host's next report taken in turn
    // under one lock.
    EventQueue queue;
    std::atomic<bool> finished {false};
    Guest guest;
    std::size_t read = 0;
    std::thread reader([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        protocol::InputEvent event {};
        for (;;) {
            if (queue.take(event)) {
                guest.apply(event);
                if (++read % 4096 == 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                }
                continue;
            }
            if (finished.load(std::memory_order_acquire))
                break;
            require(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        while (queue.take(event))
            guest.apply(event);
    });

    Touchscreen screen;
    const auto send = [&queue](
                          const uml::input::virtio::TouchscreenReport& r) {
        queue.enqueue(r.span());
    };
    for (unsigned cycle = 0; cycle < 400; ++cycle) {
        send(screen.touch(0, 100, 100, true));
        if (cycle % 2 == 0)
            send(screen.touch(1, 200, 200, true));
        for (std::uint32_t step = 1; step <= 300; ++step) {
            send(screen.touch(0, 100 + step, 100, true));
            if (cycle % 2 == 0)
                send(screen.touch(1, 200, 200 + step, true));
        }
        send(screen.touch(0, 400, 100, false));
        if (cycle % 2 == 0)
            send(screen.touch(1, 200, 500, false));
    }
    finished.store(true, std::memory_order_release);
    reader.join();

    require(guest.idle());
    protocol::InputEvent event {};
    require(!queue.take(event));
}

Events releaseOf(HeldInput& held)
{
    std::deque<protocol::InputEvent> events;
    held.appendRelease(events);
    return Events(events.begin(), events.end());
}

constexpr protocol::InputEvent track(std::int32_t id)
{
    return absolute(protocol::absoluteMtTrackingId, id);
}

void heldInputReleasesOnlyWhatWasToldHeld()
{
    HeldInput held;
    require(!held.any());
    require(held.releaseSize() == 0);
    require(releaseOf(held).empty());

    held.observe(Events {
        key(keyA, 1), synchronize(),
        key(keyB, 1), synchronize(),
        key(keyA, 0), synchronize(),
        slot(2), track(41), synchronize(),
        slot(5), track(42), synchronize(),
        slot(2), track(-1), synchronize(),
    });
    require(held.any());
    require(held.releaseSize() == 1 + 2 + 1 + 1);
    // The guest selected slot 2 last, and slot 5 is the one released last.
    requireStream(
        releaseOf(held),
        {key(keyB, 0), slot(5), lift(), slot(2), synchronize()});
    require(!held.any());
    require(releaseOf(held).empty());
}

void heldInputReleaseSizeIsTheEventsAppended()
{
    const std::array<Events, 7> histories {{
        {key(keyA, 1), synchronize()},
        {slot(0), track(1), synchronize()},
        {slot(3), track(1), synchronize()},
        {slot(1), track(1), slot(4), track(2), slot(1), synchronize()},
        {slot(9), track(1), slot(8), track(2), key(protocol::buttonTouch, 1),
         key(keyB, 1), key(buttonLeft, 1), synchronize()},
        // A slot the guest selects beyond the tracked range is restored, and
        // a contact in it is not tracked.
        {slot(2), track(1), slot(40), track(2), synchronize()},
        // A negative slot is no slot, and nothing is restored.
        {slot(2), track(1), slot(-1), track(2), synchronize()},
    }};
    for (const auto& history : histories) {
        HeldInput held;
        held.observe(history);
        const auto size = held.releaseSize();
        require(size != 0);
        require(releaseOf(held).size() == size);
    }
}

void heldInputIgnoresWhatAGuestDoesNotTake()
{
    HeldInput held;
    held.observe(Events {
        key(HeldInput::keyCount, 1),
        key(0xffff, 1),
        slot(static_cast<std::int32_t>(HeldInput::slotCount)), track(7),
        slot(-1), track(7),
        // A position or a tool type does not hold a contact.
        slot(0),
        absolute(protocol::absoluteMtPositionX, 5),
        absolute(protocol::absoluteMtToolType, protocol::toolTypePalm),
        {eventRelative, relativeX, 1},
        synchronize(),
    });
    require(!held.any());

    // The highest tracked key code and slot are held.
    held.observe(Events {
        key(HeldInput::keyCount - 1, 1),
        slot(static_cast<std::int32_t>(HeldInput::slotCount) - 1), track(1),
        synchronize(),
    });
    requireStream(
        releaseOf(held),
        {key(HeldInput::keyCount - 1, 0),
         slot(static_cast<std::int32_t>(HeldInput::slotCount) - 1), lift(),
         synchronize()});
}

void heldInputResetForgetsEverythingAndSelectsTheFirstSlot()
{
    HeldInput held;
    held.observe(Events {
        key(keyA, 1), slot(6), track(1), synchronize()});
    held.reset();
    require(!held.any());

    // Slot 0 is selected again, not slot 6 where the guest was.
    held.observe(Events {slot(1), track(1), slot(0), synchronize()});
    requireStream(
        releaseOf(held),
        {slot(1), lift(), slot(0), synchronize()});
    held.observe(Events {track(1), synchronize()});
    requireStream(releaseOf(held), {slot(0), lift(), synchronize()});
}


// ---- The devices the host plugs and unplugs.

constexpr std::uint32_t hidUsageA = 0x04;
constexpr std::uint32_t hidUsageB = 0x05;

constexpr protocol::InputEvent registration(std::int32_t registered)
{
    return {
        protocol::statusNotice, protocol::noticeRegistration, registered};
}

// Reads up to `limit` events from a device, as a guest that posts that many
// buffers does.
Events readEvents(
    VirtioInputDevice& device,
    std::size_t limit = std::numeric_limits<std::size_t>::max())
{
    Events events;
    while (events.size() < limit) {
        const auto result =
            device.processQueue(protocol::eventQueue, 1, {}, 8);
        if (result.action != uml::virtio::VhostUserQueueAction::complete)
            break;
        events.push_back(
            uml::testing::loadExact<protocol::InputEvent>(result.response));
    }
    return events;
}

// Fills a queue that holds `queued` events with empty reports up to its
// capacity.
void fillQueue(EventQueue& queue, std::size_t queued)
{
    for (; queued < queueCapacity; ++queued)
        queue.enqueue(Events {synchronize()});
}

// A guest that reads part of a full queue of a plugged device, after the host
// dropped a report: the host then sends one more through `send`. Returns the
// events the guest read after the 64 it read first, in the order it read
// them, which must end with `tail`.
Events readAroundDevice(
    VirtioInputDevice& device,
    const Events& tail,
    const std::function<void()>& send)
{
    auto all = readEvents(device, 64);
    require(all.size() == 64);
    send();
    const auto rest = readEvents(device);
    all.insert(all.end(), rest.begin(), rest.end());
    require(all.size() == queueCapacity + tail.size());
    requireStream(Events(all.end() - tail.size(), all.end()), tail);
    return Events(all.begin(), all.end() - tail.size());
}

void aDroppedKeyReleaseOnTheHardwareKeyboardIsReleased()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.keyUsage(hidUsageA, true);
    fillQueue(*rig.keyboardQueue, 2);
    // The release of A is dropped: the host believes the key is up.
    (void)rig.input.keyUsage(hidUsageA, false);

    const Events tail {
        key(keyA, 0), synchronize(), key(keyB, 1), synchronize()};
    const auto stale = readAroundDevice(rig.keyboardDevice, tail, [&] {
        (void)rig.input.keyUsage(hidUsageB, true);
    });
    Guest guest;
    guest.apply(stale);
    require(guest.keys.test(keyA));
    guest.apply(Events(tail.begin(), tail.begin() + 2));
    require(guest.idle());
}

void aDroppedButtonReleaseOnTheMouseIsReleased()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.mouse(0, 0, 0b11, 0, 0);
    fillQueue(*rig.mouseQueue, 3);
    (void)rig.input.mouse(0, 0, 0b10, 0, 0); // the left button, dropped

    // The guest holds both buttons, and the host believes only the right one
    // is down, so the release ends both ahead of the motion.
    const Events tail {
        key(buttonLeft, 0), key(buttonRight, 0), synchronize(),
        {eventRelative, relativeX, 4}, synchronize()};
    const auto stale = readAroundDevice(rig.mouseDevice, tail, [&] {
        (void)rig.input.mouse(4, 0, 0b10, 0, 0);
    });
    Guest guest;
    guest.apply(stale);
    require(guest.keys.test(buttonLeft) && guest.keys.test(buttonRight));
    guest.apply(Events(tail.begin(), tail.begin() + 3));
    require(guest.idle());
}

void aDroppedLeaveOnThePointerReleasesTheContactAndTheButton()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.pointer(100, 200, 1U, true, 1000, 2000);
    // The first report, eight events, is the tool, the contact and the left
    // button.
    fillQueue(*rig.pointerQueue, 8);
    (void)rig.input.pointer(0, 0, 0, false, 1000, 2000); // dropped

    const Events release {
        key(buttonLeft, 0),
        key(protocol::buttonToolMouse, 0),
        key(protocol::buttonTouch, 0),
        slot(0), lift(),
        synchronize(),
    };
    Events stale;
    {
        auto all = readEvents(rig.pointerDevice, 64);
        (void)rig.input.pointer(300, 400, 0, true, 1000, 2000);
        const auto rest = readEvents(rig.pointerDevice);
        all.insert(all.end(), rest.begin(), rest.end());
        require(all.size() > queueCapacity + release.size());
        // The release comes right after what was queued, before the enter.
        const auto at = all.begin() + queueCapacity;
        requireStream(Events(at, at + release.size()), release);
        stale = Events(all.begin(), at);
    }
    Guest guest;
    guest.apply(stale);
    require(guest.contacts.test(0));
    require(guest.keys.test(buttonLeft));
    guest.apply(release);
    require(guest.idle());
}

void nothingIsInjectedAfterAnUnplug()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.keyUsage(hidUsageA, true);
    fillQueue(*rig.keyboardQueue, 2);
    (void)rig.input.keyUsage(hidUsageA, false); // dropped

    // The unplug ends the guest's input device: it holds nothing, so the
    // reads that follow carry what was queued and no release.
    rig.input.setKeyboardConnected(false);
    const auto all = readEvents(rig.keyboardDevice);
    require(all.size() == queueCapacity);
    require(sameStreamEnd(all, Events {synchronize()}));

    // The device is plugged and registered again, and nothing is owed for
    // the key of the first session.
    rig.input.setKeyboardConnected(true);
    Rig::registers(rig.keyboardDevice);
    (void)rig.input.keyUsage(hidUsageB, true);
    requireStream(
        readEvents(rig.keyboardDevice), Events {key(keyB, 1), synchronize()});
}

void nothingIsInjectedWhileTheGuestHasNoInputDevice()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.keyUsage(hidUsageA, true);
    fillQueue(*rig.keyboardQueue, 2);
    (void)rig.input.keyUsage(hidUsageA, false); // dropped

    // The guest takes its input device down: what it reads is what was
    // queued, and the empty queue owes it nothing.
    Rig::status(rig.keyboardDevice, registration(0));
    const auto all = readEvents(rig.keyboardDevice);
    require(all.size() == queueCapacity);
    require(sameStreamEnd(all, Events {synchronize()}));
    require(readEvents(rig.keyboardDevice).empty());

    // A report for the device that is not there is held for the one that
    // comes, and the old device's release is not.
    (void)rig.input.keyUsage(hidUsageB, true);
    require(readEvents(rig.keyboardDevice).empty());
    Rig::registers(rig.keyboardDevice);
    requireStream(
        readEvents(rig.keyboardDevice), Events {key(keyB, 1), synchronize()});
}

void aReportHeldForTheRegistrationIsHeldByTheGuestAfterIt()
{
    Rig rig;
    rig.input.setKeyboardConnected(true); // plugged, not registered
    (void)rig.input.keyUsage(hidUsageA, true);
    require(readEvents(rig.keyboardDevice).empty());

    // The registration delivers what was held, and the queue follows it as it
    // follows every report: a later drop releases the key.
    Rig::registers(rig.keyboardDevice);
    fillQueue(*rig.keyboardQueue, 2);
    (void)rig.input.keyUsage(hidUsageA, false); // dropped
    const Events tail {
        key(keyA, 0), synchronize(), key(keyB, 1), synchronize()};
    const auto stale = readAroundDevice(rig.keyboardDevice, tail, [&] {
        (void)rig.input.keyUsage(hidUsageB, true);
    });
    Guest guest;
    guest.apply(stale);
    require(guest.keys.test(keyA));
}

void aResetOfTheDeviceForgetsWhatTheGuestHeld()
{
    // A fixed device stays registered through a reset, so the reset alone
    // decides what the queue still owes.
    EventQueue fixed;
    fixed.enqueue(Events {key(keyA, 1), synchronize()});
    fillQueue(fixed, 2);
    fixed.enqueue(Events {key(keyA, 0), synchronize()}); // dropped
    fixed.reset();
    const Events next {key(keyB, 1), synchronize()};
    fixed.enqueue(next);
    Events got;
    protocol::InputEvent event {};
    while (fixed.take(event))
        got.push_back(event);
    requireStream(got, next);

    // A plugged device is unregistered by a reset and registers again.
    EventQueue plugged(EventQueue::Attachment::pluggable);
    require(plugged.setConnected(true));
    plugged.guestStatus(registration(1));
    plugged.enqueue(Events {key(keyA, 1), synchronize()});
    fillQueue(plugged, 2);
    plugged.enqueue(Events {key(keyA, 0), synchronize()}); // dropped
    plugged.reset();
    plugged.guestStatus(registration(1));
    plugged.enqueue(next);
    got.clear();
    while (plugged.take(event))
        got.push_back(event);
    requireStream(got, next);
}

void anUnplugForgetsWhatTheGuestHeldButNotWhatIsStillQueued()
{
    EventQueue queue(EventQueue::Attachment::pluggable);
    require(queue.setConnected(true));
    queue.guestStatus(registration(1));
    queue.enqueue(Events {key(keyA, 1), synchronize()});
    protocol::InputEvent event {};
    while (queue.take(event)) {
    }
    // The guest read the key, then the device went away and came back. The
    // new input device holds nothing, however the host plugs it.
    require(queue.setConnected(false));
    require(queue.setConnected(true));
    queue.guestStatus(registration(1));
    fillQueue(queue, 0);
    queue.enqueue(Events {key(keyB, 1), synchronize()}); // dropped
    std::size_t count = 0;
    while (queue.take(event))
        ++count;
    require(count == queueCapacity);
    // Nothing was held, so nothing is released after the drop either.
    require(!queue.take(event));

    // What the new input device is still to read is held by it once read.
    queue.enqueue(Events {key(keyA, 1), synchronize()});
    require(queue.setConnected(false));
    require(queue.setConnected(true));
    queue.guestStatus(registration(1));
    fillQueue(queue, 2);
    queue.enqueue(Events {key(keyB, 1), synchronize()}); // dropped
    Events got;
    while (queue.take(event))
        got.push_back(event);
    require(got.size() == queueCapacity + 2);
    requireStream(
        Events(got.end() - 2, got.end()),
        Events {key(keyA, 0), synchronize()});
}

} // namespace

int main(int argc, char** argv)
{
    const std::array<uml::testing::NamedTest, 30> tests {{
        {"a-dropped-lift-is-released-before-the-next-report",
         aDroppedLiftIsReleasedBeforeTheNextReport},
        {"a-dropped-move-still-releases-the-contact-it-held",
         aDroppedMoveStillReleasesTheContactItHeld},
        {"several-contacts-are-all-released-in-slot-order",
         severalContactsAreAllReleasedInSlotOrder},
        {"a-dropped-key-release-reaches-the-guest-through-the-device",
         aDroppedKeyReleaseReachesTheGuestThroughTheDevice},
        {"a-dropped-mouse-button-release-is-released-without-touch-knowledge",
         aDroppedMouseButtonReleaseIsReleasedWithoutTouchKnowledge},
        {"every-held-key-is-released-in-code-order",
         everyHeldKeyIsReleasedInCodeOrder},
        {"nothing-is-synthesized-without-an-overflow",
         nothingIsSynthesizedWithoutAnOverflow},
        {"an-overflow-with-nothing-held-synthesizes-nothing",
         anOverflowWithNothingHeldSynthesizesNothing},
        {"the-release-waits-for-room-for-it-and-the-report",
         theReleaseWaitsForRoomForItAndTheReport},
        {"clear-forgets-a-pending-release", clearForgetsAPendingRelease},
        {"clear-forgets-what-the-guest-held", clearForgetsWhatTheGuestHeld},
        {"the-overflow-is-reported-once", theOverflowIsReportedOnce},
        {"a-dropped-final-lift-is-released-at-drain-without-a-further-report",
         aDroppedFinalLiftIsReleasedAtDrainWithoutAFurtherReport},
        {"every-dropped-final-release-is-released-at-drain",
         everyDroppedFinalReleaseIsReleasedAtDrain},
        {"a-drain-with-nothing-held-emits-nothing",
         aDrainWithNothingHeldEmitsNothing},
        {"a-release-at-drain-is-not-followed-by-a-second-one",
         aReleaseAtDrainIsNotFollowedByASecondOne},
        {"a-release-with-the-next-report-is-not-followed-by-a-second-one-at-drain",
         aReleaseWithTheNextReportIsNotFollowedByASecondOneAtDrain},
        {"a-reader-racing-the-host-ends-with-nothing-held",
         aReaderRacingTheHostEndsWithNothingHeld},
        {"held-input-releases-only-what-was-told-held",
         heldInputReleasesOnlyWhatWasToldHeld},
        {"held-input-release-size-is-the-events-appended",
         heldInputReleaseSizeIsTheEventsAppended},
        {"held-input-ignores-what-a-guest-does-not-take",
         heldInputIgnoresWhatAGuestDoesNotTake},
        {"held-input-reset-forgets-everything-and-selects-the-first-slot",
         heldInputResetForgetsEverythingAndSelectsTheFirstSlot},
        {"a-dropped-key-release-on-the-hardware-keyboard-is-released",
         aDroppedKeyReleaseOnTheHardwareKeyboardIsReleased},
        {"a-dropped-button-release-on-the-mouse-is-released",
         aDroppedButtonReleaseOnTheMouseIsReleased},
        {"a-dropped-leave-on-the-pointer-releases-the-contact-and-the-button",
         aDroppedLeaveOnThePointerReleasesTheContactAndTheButton},
        {"nothing-is-injected-after-an-unplug", nothingIsInjectedAfterAnUnplug},
        {"nothing-is-injected-while-the-guest-has-no-input-device",
         nothingIsInjectedWhileTheGuestHasNoInputDevice},
        {"a-report-held-for-the-registration-is-held-by-the-guest-after-it",
         aReportHeldForTheRegistrationIsHeldByTheGuestAfterIt},
        {"a-reset-of-the-device-forgets-what-the-guest-held",
         aResetOfTheDeviceForgetsWhatTheGuestHeld},
        {"an-unplug-forgets-what-the-guest-held-but-not-what-is-still-queued",
         anUnplugForgetsWhatTheGuestHeldButNotWhatIsStillQueued},
    }};
    // A name on the command line runs only that case.
    if (argc > 1) {
        for (const auto& test : tests) {
            if (std::strcmp(test.name, argv[1]) == 0)
                return uml::testing::runNamedTests(
                    std::span<const uml::testing::NamedTest>(&test, 1));
        }
        std::fprintf(stderr, "no such case: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return uml::testing::runNamedTests(tests);
}
