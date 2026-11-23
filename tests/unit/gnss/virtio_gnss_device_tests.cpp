// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"
#include "named_tests.hpp"
#include "nmea_writer.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virtio_gnss_device.hpp"
#include "watchdog.hpp"
#include "wire_bytes.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>

namespace {

namespace protocol = uml::gnss::protocol;
using namespace std::chrono_literals;
using uml::gnss::Endpoint;
using uml::gnss::Fix;
using uml::gnss::VirtioGnssDevice;
using uml::testing::bytes;
using uml::testing::require;
using uml::virtio::VhostUserQueueAction;

constexpr std::int64_t second = 1'000'000'000;
constexpr std::int64_t start = 1'794'318'330 * second;

Fix fixAt(std::int64_t utcNs, double latitude = 35.6895)
{
    Fix fix;
    fix.utcNs = utcNs;
    fix.latitude = latitude;
    fix.longitude = 139.6917;
    fix.altitude = 40.0;
    fix.ellipsoidalAltitude = 80.5;
    fix.horizontalAccuracy = 8.0;
    return fix;
}

bool readable(int descriptor, std::chrono::milliseconds timeout)
{
    pollfd entry {.fd = descriptor, .events = POLLIN, .revents = 0};
    return ::poll(&entry, 1, static_cast<int>(timeout.count())) == 1;
}

/* The device behind a test clock and a power log, with a guest that plays
 * the driver's two queues. Everything runs on the test's thread, as the
 * transport's worker would. */
struct Guest final {
    explicit Guest(
        std::unique_ptr<uml::gnss::FixSource> source = {},
        std::chrono::milliseconds period = 20ms)
        : endpoint(std::make_shared<Endpoint>(std::move(source)))
        , device(
              endpoint,
              VirtioGnssDevice::Options {
                  .period = period,
                  .clock = [this] { return now; },
              })
    {
        endpoint->setPowerObserver([this](bool on) { powerLog.push_back(on); });
        endpoint->setWakeHook([this] { ++wakes; });
    }

    void power(bool on)
    {
        const protocol::Control message {
            .version = protocol::version,
            .power = on ? 1U : 0U,
        };
        const auto result = device.processQueue(
            protocol::controlQueue, 1, bytes(message), 0);
        require(result.action == VhostUserQueueAction::complete);
        require(result.response.empty());
    }

    uml::virtio::VhostUserQueueResult receive(std::size_t capacity)
    {
        return device.processQueue(protocol::receiveQueue, 2, {}, capacity);
    }

    /* Fills buffers of `capacity` until the device has nothing more. */
    std::string takeAll(std::size_t capacity = 100)
    {
        std::string text;
        for (;;) {
            const auto result = receive(capacity);
            if (result.action == VhostUserQueueAction::retry) {
                require(result.response.empty());
                return text;
            }
            require(result.action == VhostUserQueueAction::complete);
            require(!result.response.empty() && result.response.size() <= capacity);
            text.append(result.response.begin(), result.response.end());
        }
    }

    /* Waits for an epoch the guest has not yet taken, running what the
     * transport would on the way. The device changes only inside these calls,
     * so the clock a test sets before one is the clock its epoch is made at. */
    std::string takeNext(std::chrono::milliseconds timeout = 2000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            auto text = takeAll();
            if (!text.empty())
                return text;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            require(left > 0ms, "an epoch arrives");
            serve(left);
        }
    }

    /* Waits for the transport to be told something, and runs what it does. */
    void serve(std::chrono::milliseconds timeout = 2000ms)
    {
        require(readable(device.eventDescriptor(), timeout), "the device is told");
        device.drainEventSource();
    }

    /* Runs what is pending, without waiting. */
    void settle()
    {
        while (readable(device.eventDescriptor(), 0ms))
            device.drainEventSource();
    }

    /* What the observers write to is declared before the device, so that it
     * outlives the device: destroying the device powers the receiver off, and
     * that reaches the observers. */
    std::int64_t now {start};
    std::vector<bool> powerLog;
    int wakes {};
    std::shared_ptr<Endpoint> endpoint;
    VirtioGnssDevice device;
};

void describesTheDevice()
{
    Guest guest;
    require(guest.device.features() == 0);
    require(guest.device.queueCount() == 2);
    require(guest.device.configuration().empty());
    require(guest.device.keepsRequestsOnDisabledRing());
    require(guest.device.eventDescriptor() >= 0);
    require(protocol::deviceId == 65534);
}

void sendsNothingWhileTheReceiverIsOff()
{
    Guest guest;
    guest.endpoint->report(fixAt(start));
    guest.settle();
    require(guest.takeAll().empty());
    require(!guest.endpoint->powered() && guest.powerLog.empty());
    // Nor does the timer run.
    require(!readable(guest.device.eventDescriptor(), 100ms));
}

void deliversTheFirstEpochAtPowerOn()
{
    Guest guest;
    guest.endpoint->report(fixAt(start));
    guest.settle();
    guest.power(true);
    require(guest.endpoint->powered());
    require(guest.powerLog == std::vector<bool> {true});
    require(guest.takeAll() == uml::gnss::nmeaEpoch(fixAt(start), start));
    // The poke that has the transport try the receive queue again runs
    // nothing else: the epoch was taken, and no new one is due yet.
    guest.settle();
    require(guest.takeAll().empty());
}

void splitsAnEpochAcrossTheGuestsBuffers()
{
    Guest guest;
    guest.endpoint->report(fixAt(start));
    guest.power(true);
    const auto expected = uml::gnss::nmeaEpoch(fixAt(start), start);
    for (std::size_t offset = 0; offset < expected.size(); offset += 100) {
        const auto result = guest.receive(100);
        require(result.action == VhostUserQueueAction::complete);
        const auto size = std::min<std::size_t>(100, expected.size() - offset);
        require(result.response.size() == size);
        require(std::string(result.response.begin(), result.response.end()) ==
                expected.substr(offset, size));
    }
    require(guest.receive(100).action == VhostUserQueueAction::retry);
}

void repeatsTheFixEachPeriodAndThenReportsNoFix()
{
    Guest guest;
    guest.endpoint->report(fixAt(start));
    guest.power(true);
    require(!guest.takeAll().empty());

    guest.now = start + 2 * second;
    // The fix is repeated with its own time.
    require(guest.takeNext() == uml::gnss::nmeaEpoch(fixAt(start), start));

    // A precise fix is stale after ten seconds.
    guest.now = start + 11 * second;
    require(guest.takeNext() == uml::gnss::nmeaEpoch(std::nullopt, guest.now));
}

void holdsAnApproximateFixForTwentyMinutes()
{
    Fix fix = fixAt(start);
    fix.approximate = true;
    Guest guest;
    guest.endpoint->report(fix);
    guest.power(true);
    require(!guest.takeAll().empty());
    guest.now = start + 19 * 60 * second;
    require(guest.takeNext() == uml::gnss::nmeaEpoch(fix, start));
    guest.now = start + 21 * 60 * second;
    require(guest.takeNext() == uml::gnss::nmeaEpoch(std::nullopt, guest.now));
}

void reportsANewFixAtOnce()
{
    // A period far longer than the test, so only the report can tell.
    Guest guest(nullptr, 60s);
    guest.endpoint->report(fixAt(start));
    guest.power(true);
    require(!guest.takeAll().empty());
    guest.settle();

    guest.now = start + second;
    guest.endpoint->report(fixAt(start + second, 36.0));
    guest.serve(1000ms);
    require(guest.takeAll() == uml::gnss::nmeaEpoch(fixAt(start + second, 36.0), start));
    require(guest.wakes == 0);
}

void clearingDropsTheHeldFix()
{
    Guest guest(nullptr, 60s);
    guest.endpoint->report(fixAt(start));
    guest.power(true);
    require(!guest.takeAll().empty());
    guest.settle();
    guest.endpoint->clear();
    guest.serve(1000ms);
    require(guest.takeAll() == uml::gnss::nmeaEpoch(std::nullopt, start));
}

void stopsAtPowerOffAndStartsFreshAtTheNextPowerOn()
{
    Guest guest;
    guest.endpoint->report(fixAt(start));
    guest.power(true);
    require(!guest.takeAll().empty());
    guest.settle();

    guest.power(false);
    require(!guest.endpoint->powered());
    require(guest.takeAll().empty());
    guest.settle();
    // The timer is gone: nothing is told to the transport.
    require(!readable(guest.device.eventDescriptor(), 150ms));

    guest.endpoint->report(fixAt(start + second, 36.0));
    guest.settle();
    guest.now = start + second;
    guest.power(true);
    require(guest.takeAll() == uml::gnss::nmeaEpoch(fixAt(start + second, 36.0), start));
    require((guest.powerLog == std::vector<bool> {true, false, true}));
}

void ignoresAPowerMessageItDoesNotUnderstand()
{
    Guest guest;
    const auto ignored = [&guest](std::vector<std::uint8_t> request) {
        const auto result = guest.device.processQueue(
            protocol::controlQueue, 1, request, 0);
        require(result.action == VhostUserQueueAction::complete);
        require(!guest.endpoint->powered());
    };
    ignored({});
    ignored({1, 0, 0, 0});
    ignored(bytes(protocol::Control {.version = 2, .power = 1}));
    ignored(bytes(protocol::Control {.version = 0, .power = 1}));
    ignored(bytes(protocol::Control {.version = 1, .power = 1}, std::uint8_t {0}));
    uml::testing::requireThrows(
        [&guest] { (void)guest.device.processQueue(2, 1, {}, 0); });
}

void returnsABufferItCannotFill()
{
    Guest guest;
    guest.power(true);
    const std::vector<std::uint8_t> payload {1, 2, 3};
    auto result = guest.device.processQueue(protocol::receiveQueue, 1, payload, 64);
    require(result.action == VhostUserQueueAction::complete && result.response.empty());
    result = guest.device.processQueue(protocol::receiveQueue, 1, {}, 0);
    require(result.action == VhostUserQueueAction::complete && result.response.empty());
    // The epoch is still there.
    require(!guest.takeAll().empty());
}

void aResetPowersTheReceiverOff()
{
    Guest guest;
    guest.power(true);
    guest.device.reset();
    require(!guest.endpoint->powered());
    require(guest.takeAll().empty());
    guest.device.reset();
    require((guest.powerLog == std::vector<bool> {true, false}));
}

void toldAnObserverSetLaterWhileTheReceiverIsOn()
{
    Guest guest;
    guest.power(true);
    std::vector<bool> later;
    guest.endpoint->setPowerObserver([&later](bool on) { later.push_back(on); });
    require(later == std::vector<bool> {true});
    guest.power(false);
    require((later == std::vector<bool> {true, false}));
    guest.endpoint->setPowerObserver({});
    guest.power(true);
    require(later.size() == 2);
}

void wakesAGuestThatTakesNothingOncePerPeriod()
{
    Guest guest;
    guest.power(true);
    // The guest is suspended: it takes nothing, so a period later it is woken.
    while (guest.wakes < 1)
        guest.serve();
    require(guest.wakes == 1);
    // The epoch was made again, not queued behind the one nobody took.
    const auto one = uml::gnss::nmeaEpoch(std::nullopt, start);
    require(guest.takeAll() == one);

    // The guest took it, so the next period needs no wake.
    require(guest.takeNext() == one);
    require(guest.wakes == 1);
    // And a guest that takes nothing again is woken again.
    while (guest.wakes < 2)
        guest.serve();
    require(guest.wakes == 2);
    require(guest.takeAll() == one);
}

void finishesAnEpochTheGuestHasBegun()
{
    Guest guest;
    guest.power(true);
    const auto expected = uml::gnss::nmeaEpoch(std::nullopt, start);
    const auto first = guest.receive(100);
    require(first.response.size() == 100);

    // The first drain is the one the power-on asked for; the second is a tick.
    guest.serve();
    guest.serve();
    // A guest that is reading is not woken and not given a new epoch inside
    // the old one.
    require(guest.wakes == 0);
    const auto rest = guest.takeAll();
    require(std::string(first.response.begin(), first.response.end()) + rest == expected);
}

void aSourceReplacesTheHostsReports()
{
    struct Source final : uml::gnss::FixSource {
        std::optional<Fix> latest(std::int64_t nowUtcNs) override
        {
            return fixAt(nowUtcNs, 12.5);
        }
        void powerChanged(bool on, std::int64_t) override { log.push_back(on); }
        std::vector<bool> log;
    };
    auto source = std::make_unique<Source>();
    const auto* observed = source.get();
    Guest guest(std::move(source));
    guest.endpoint->report(fixAt(start, 1.0));
    guest.power(true);
    require(guest.takeAll() == uml::gnss::nmeaEpoch(fixAt(start, 12.5), start));
    guest.power(false);
    require((observed->log == std::vector<bool> {true, false}));
}

void holdsFixesForTheirClassOnly()
{
    Fix precise = fixAt(start);
    require(uml::gnss::heldFix(precise, start + 10 * second).has_value());
    require(!uml::gnss::heldFix(precise, start + 10 * second + 1).has_value());
    // A clock that moved back does not make the fix stale.
    require(uml::gnss::heldFix(precise, start - 100 * second).has_value());
    Fix approximate = precise;
    approximate.approximate = true;
    require(uml::gnss::heldFix(approximate, start + 20 * 60 * second).has_value());
    require(!uml::gnss::heldFix(approximate, start + 20 * 60 * second + 1).has_value());
    require(!uml::gnss::heldFix(std::nullopt, start).has_value());
}

void theServiceStartsAndStops()
{
    auto started = uml::gnss::makeInProcessService();
    require(started.service != nullptr && started.endpoint != nullptr);
    require(!started.service->takeConnectionName().empty());
    started.endpoint->report(fixAt(start));
    started.service->stop();
    started.service->stop();
    require(!started.endpoint->powered());
}

} // namespace

int main()
{
    uml::testing::Watchdog watchdog("the GNSS device tests", 120s);
    static constexpr uml::testing::NamedTest tests[] = {
        {"describes-the-device", describesTheDevice},
        {"sends-nothing-while-the-receiver-is-off", sendsNothingWhileTheReceiverIsOff},
        {"delivers-the-first-epoch-at-power-on", deliversTheFirstEpochAtPowerOn},
        {"splits-an-epoch-across-the-guests-buffers", splitsAnEpochAcrossTheGuestsBuffers},
        {"repeats-the-fix-each-period-and-then-reports-no-fix",
         repeatsTheFixEachPeriodAndThenReportsNoFix},
        {"holds-an-approximate-fix-for-twenty-minutes", holdsAnApproximateFixForTwentyMinutes},
        {"reports-a-new-fix-at-once", reportsANewFixAtOnce},
        {"clearing-drops-the-held-fix", clearingDropsTheHeldFix},
        {"stops-at-power-off-and-starts-fresh", stopsAtPowerOffAndStartsFreshAtTheNextPowerOn},
        {"ignores-a-power-message-it-does-not-understand", ignoresAPowerMessageItDoesNotUnderstand},
        {"returns-a-buffer-it-cannot-fill", returnsABufferItCannotFill},
        {"a-reset-powers-the-receiver-off", aResetPowersTheReceiverOff},
        {"tells-an-observer-set-later", toldAnObserverSetLaterWhileTheReceiverIsOn},
        {"wakes-a-guest-that-takes-nothing", wakesAGuestThatTakesNothingOncePerPeriod},
        {"finishes-an-epoch-the-guest-has-begun", finishesAnEpochTheGuestHasBegun},
        {"a-source-replaces-the-hosts-reports", aSourceReplacesTheHostsReports},
        {"holds-fixes-for-their-class-only", holdsFixesForTheirClassOnly},
        {"the-service-starts-and-stops", theServiceStartsAndStops},
    };
    return uml::testing::runNamedTests(tests);
}
