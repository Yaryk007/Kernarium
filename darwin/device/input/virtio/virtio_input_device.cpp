// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_input_device.hpp"

#include "hid_keyboard.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace uml::input::virtio {
namespace {

constexpr std::size_t maximumQueuedEvents = 4096;

void setBit(
    std::array<std::uint8_t, 128>& bitmap,
    std::uint16_t bit) noexcept
{
    bitmap[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

std::uint8_t bitmapSize(
    const std::array<std::uint8_t, 128>& bitmap) noexcept
{
    for (std::size_t index = bitmap.size(); index != 0; --index) {
        if (bitmap[index - 1U] != 0)
            return static_cast<std::uint8_t>(index);
    }
    return 0;
}

/* What names a kind of device to the guest. The names, serials and product
 * numbers are permanent once shipped: Android keeps a keyboard's layout and
 * its settings against them. */
struct KindInfo final {
    const char* label;
    const char* name;
    const char* serial;
    std::uint16_t product;
};

constexpr KindInfo kindInfo(DeviceKind kind) noexcept
{
    switch (kind) {
    case DeviceKind::touchscreen:
        return {"Virtio touchscreen", "Kernarium Touchscreen",
                "kernarium-touch-0", 1};
    case DeviceKind::keyboard:
        return {"Virtio keyboard", "Kernarium Keyboard",
                "kernarium-keyboard-0", 2};
    case DeviceKind::hardwareKeyboard:
        return {"Virtio hardware keyboard", "Kernarium Hardware Keyboard",
                "kernarium-hardware-keyboard-0", 3};
    case DeviceKind::mouse:
        return {"Virtio mouse", "Kernarium Mouse", "kernarium-mouse-0", 4};
    case DeviceKind::pointer:
        return {"Virtio pointer", "Kernarium Pointer", "kernarium-pointer-0",
                5};
    }
    return {"Virtio input", "Kernarium Input", "kernarium-input-0", 0};
}

template<typename Value>
void copyValue(protocol::InputConfig& config, const Value& value)
{
    static_assert(sizeof(value) <= sizeof(config.value));
    std::memcpy(&config.value, &value, sizeof(value));
    config.size = static_cast<std::uint8_t>(sizeof(value));
}

void copyString(protocol::InputConfig& config, const char* string)
{
    const auto length = std::min<std::size_t>(
        std::strlen(string), config.value.string.size());
    std::memcpy(config.value.string.data(), string, length);
    config.size = static_cast<std::uint8_t>(length);
}

bool endsReport(const protocol::InputEvent& event) noexcept
{
    return event.type == protocol::eventSynchronize &&
        event.code == protocol::synchronizeReport;
}

enum class Motion : std::uint8_t {
    none,
    absolute,
    relative,
};

/* Whether a report only moves the absolute pointer, or only a relative mouse,
 * and does nothing else. */
template<typename Iterator>
Motion motionOf(Iterator first, Iterator last) noexcept
{
    bool absolute = false;
    bool relative = false;
    for (; first != last; ++first) {
        const protocol::InputEvent& event = *first;
        if (endsReport(event))
            continue;
        if (event.type == protocol::eventAbsolute &&
            (event.code == protocol::absoluteMtSlot ||
             event.code == protocol::absoluteMtPositionX ||
             event.code == protocol::absoluteMtPositionY)) {
            absolute = true;
        } else if (
            event.type == protocol::eventRelative &&
            (event.code == protocol::relativeX ||
             event.code == protocol::relativeY)) {
            relative = true;
        } else {
            return Motion::none;
        }
    }
    if (absolute == relative)
        return Motion::none;
    return absolute ? Motion::absolute : Motion::relative;
}

} // namespace

EventQueue::EventQueue(Attachment attachment)
    : wake_("virtio-input event")
    , pluggable_(attachment == Attachment::pluggable)
    , connected_(!pluggable_)
    , probed_(!pluggable_)
    , registered_(!pluggable_)
{
}

void EventQueue::wake() noexcept
{
    if (!wake_.notify()) {
        std::fprintf(
            stderr,
            "Kernarium: virtio-input wake failed: %s\n",
            std::strerror(errno));
    }
}

void EventQueue::forgetGuestLocked() noexcept
{
    guestHolds_.reset();
    dropped_ = false;
    for (const auto& event : events_)
        guestHolds_.observe({&event, 1});
}

void EventQueue::queueLocked(
    std::span<const protocol::InputEvent> events) noexcept
{
    events_.insert(events_.end(), events.begin(), events.end());
    guestHolds_.observe(events);
}

void EventQueue::queueSwitchLocked() noexcept
{
    if (!switch_ || pluggable_)
        return;
    const protocol::InputEvent report[] = {
        {protocol::eventSwitch, switch_->code, switch_->on ? 1 : 0},
        {protocol::eventSynchronize, protocol::synchronizeReport, 0},
    };
    queueLocked(report);
}

bool EventQueue::holdLocked(
    std::span<const protocol::InputEvent> events) noexcept
{
    if (heldLost_)
        return true;
    while (!events.empty()) {
        std::size_t length = 0;
        while (length < events.size() && !endsReport(events[length]))
            ++length;
        if (length < events.size())
            ++length;
        const auto report = events.first(length);
        events = events.subspan(length);

        const auto motion = endsReport(report.back()) ?
            motionOf(report.begin(), report.end()) : Motion::none;
        if (motion == Motion::relative) {
            /* A relative mouse has no position to keep. */
            continue;
        }
        if (motion == Motion::absolute && !held_.empty() &&
            endsReport(held_.back())) {
            /* The pointer's latest position replaces the one that was held
             * last. */
            auto first = held_.end() - 1;
            while (first != held_.begin() && !endsReport(*(first - 1)))
                --first;
            if (motionOf(first, held_.end()) == Motion::absolute)
                held_.erase(first, held_.end());
        }
        if (report.size() > heldEventLimit - held_.size()) {
            /* A prefix of the stream would leave a key or a contact down that
             * the rest of it releases, so all of it goes. */
            held_.clear();
            heldLost_ = true;
            if (!reportedHoldOverflow_) {
                std::fputs(
                    "Kernarium: dropping input because the guest has not "
                    "registered the plugged-in virtio device\n",
                    stderr);
                reportedHoldOverflow_ = true;
            }
            return true;
        }
        held_.insert(held_.end(), report.begin(), report.end());
    }
    return false;
}

bool EventQueue::enqueue(
    std::span<const protocol::InputEvent> events) noexcept
{
    if (events.empty())
        return false;
    bool wasEmpty = false;
    {
        std::lock_guard lock(mutex_);
        if (!connected_)
            return false;
        if (!registered_)
            return holdLocked(events);
        /* After a drop, the guest first gets the release of what it holds,
         * so the room needed is the release's and the report's. */
        const auto release = dropped_ ? guestHolds_.releaseSize() : 0;
        const auto room = maximumQueuedEvents - events_.size();
        if (release > room || events.size() > room - release) {
            dropped_ = true;
            if (!reportedOverflow_) {
                std::fputs(
                    "Kernarium: dropping input because the virtio queue is full\n",
                    stderr);
                reportedOverflow_ = true;
            }
            return false;
        }
        wasEmpty = events_.empty();
        if (dropped_) {
            guestHolds_.appendRelease(events_);
            dropped_ = false;
        }
        queueLocked(events);
    }
    if (wasEmpty)
        wake();
    return false;
}

void EventQueue::setSwitch(Switch state) noexcept
{
    if (pluggable_)
        return;
    {
        std::lock_guard lock(mutex_);
        switch_ = state;
        events_.clear();
        guestHolds_.reset();
        dropped_ = false;
        queueSwitchLocked();
    }
    wake();
}

std::optional<EventQueue::Switch> EventQueue::switchState() const noexcept
{
    std::lock_guard lock(mutex_);
    return switch_;
}

bool EventQueue::connected() const noexcept
{
    std::lock_guard lock(mutex_);
    return connected_;
}

bool EventQueue::advertised() const noexcept
{
    std::lock_guard lock(mutex_);
    return connected_ && probed_;
}

bool EventQueue::registered() const noexcept
{
    std::lock_guard lock(mutex_);
    return registered_;
}

bool EventQueue::setConnectedLocked(bool connected) noexcept
{
    if (!pluggable_ || connected_ == connected)
        return false;
    connected_ = connected;
    registered_ = false;
    held_.clear();
    heldLost_ = false;
    reportedHoldOverflow_ = false;
    forgetGuestLocked();
    /* A driver that has not probed has nothing to be told: it reads the
     * state when it does, and the first buffer it posts finds it. */
    if (probed_)
        ++configGeneration_;
    return true;
}

bool EventQueue::setConnected(bool connected) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!setConnectedLocked(connected))
            return false;
    }
    wake();
    return true;
}

void EventQueue::disconnect(
    std::span<const protocol::InputEvent> finalEvents) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!pluggable_ || !connected_)
            return;
        /* What waits for a registration the guest never saw has nothing to
         * release. */
        if (registered_ &&
            finalEvents.size() <= maximumQueuedEvents - events_.size()) {
            queueLocked(finalEvents);
        }
        (void)setConnectedLocked(false);
    }
    wake();
}

void EventQueue::setStatusSink(StatusSink sink)
{
    std::lock_guard lock(sinkMutex_);
    sink_ = std::move(sink);
}

bool EventQueue::take(protocol::InputEvent& event) noexcept
{
    std::lock_guard lock(mutex_);
    if (registered_ && events_.empty() && dropped_) {
        /* The guest has read everything and the host may have nothing more
         * to send: a lift that was dropped must not wait for a later report.
         * The queue is empty, so the release fits. appendRelease forgets
         * what it ends, so the report that comes next, and the lock that
         * orders it with this call, owe no second one. A device that is
         * unplugged or whose input device is not registered injects
         * nothing. */
        guestHolds_.appendRelease(events_);
        dropped_ = false;
    }
    if (events_.empty())
        return false;
    event = events_.front();
    events_.pop_front();
    return true;
}

int EventQueue::descriptor() const noexcept
{
    return wake_.descriptor();
}

void EventQueue::drain()
{
    switch (wake_.drain()) {
    case uml::platform::WakeSocket::Drained::empty:
        return;
    case uml::platform::WakeSocket::Drained::closed:
        throw std::runtime_error("closed virtio-input event source");
    case uml::platform::WakeSocket::Drained::failed:
        break;
    }
    throw std::runtime_error(
        "reading virtio-input event source failed: " +
        std::string(std::strerror(errno)));
}

void EventQueue::eventBufferPosted() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (probed_)
            return;
        probed_ = true;
        if (!connected_)
            return;
        ++configGeneration_;
    }
    wake();
}

void EventQueue::guestStatus(const protocol::InputEvent& event) noexcept
{
    StatusEvent status;
    bool flushed = false;
    if (event.type == protocol::eventLed) {
        status = {
            .kind = StatusEvent::Kind::led,
            .led = event.code,
            .on = event.value != 0,
        };
    } else if (event.type == protocol::statusNotice &&
               event.code == protocol::noticeRegistration &&
               (event.value == 0 || event.value == 1)) {
        const bool on = event.value == 1;
        std::lock_guard lock(mutex_);
        if (!pluggable_)
            return;
        if (connected_) {
            if (registered_ != on)
                forgetGuestLocked();
            registered_ = on;
            if (on) {
                flushed = !held_.empty();
                heldLost_ = false;
                if (flushed) {
                    events_.insert(events_.end(), held_.begin(), held_.end());
                    for (const auto& held : held_)
                        guestHolds_.observe({&held, 1});
                    held_.clear();
                }
            }
        } else if (on) {
            /* A registration that answers a plug the host has withdrawn. An
             * unregistration is the answer to an unplug and is reported. */
            return;
        }
        status = {
            .kind = StatusEvent::Kind::registration,
            .on = on,
        };
    } else {
        return;
    }
    if (flushed)
        wake();
    std::lock_guard lock(sinkMutex_);
    if (sink_)
        sink_(status);
}

bool EventQueue::configChangePending() const noexcept
{
    std::lock_guard lock(mutex_);
    return configGeneration_ != configDelivered_;
}

bool EventQueue::claimConfigChange() noexcept
{
    std::lock_guard lock(mutex_);
    configClaimed_ = configGeneration_;
    return configGeneration_ != configDelivered_;
}

void EventQueue::clearConfigChange() noexcept
{
    std::lock_guard lock(mutex_);
    configDelivered_ = configClaimed_;
}

void EventQueue::reset() noexcept
{
    std::lock_guard lock(mutex_);
    events_.clear();
    held_.clear();
    guestHolds_.reset();
    dropped_ = false;
    heldLost_ = false;
    reportedHoldOverflow_ = false;
    probed_ = !pluggable_;
    registered_ = !pluggable_;
    queueSwitchLocked();
}

void EventQueue::clear() noexcept
{
    std::lock_guard lock(mutex_);
    events_.clear();
    guestHolds_.reset();
    dropped_ = false;
}

VirtioInputDevice::VirtioInputDevice(
    DeviceKind kind,
    std::shared_ptr<EventQueue> events)
    : kind_(kind)
    , events_(std::move(events))
{
    if (!events_)
        throw std::invalid_argument("invalid virtio-input device options");
}

const char* VirtioInputDevice::name() const noexcept
{
    return kindInfo(kind_).label;
}

std::uint64_t VirtioInputDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioInputDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioInputDevice::configuration() const
{
    return uml::virtio::toBytes(selectedConfiguration());
}

void VirtioInputDevice::writeConfiguration(
    std::uint32_t offset,
    std::uint32_t flags,
    std::span<const std::uint8_t> bytes)
{
    constexpr std::uint32_t writableFlag = 0;
    constexpr std::uint32_t migrationFlag = 1;
    if ((flags != writableFlag && flags != migrationFlag) || bytes.empty() ||
        offset > 2U || bytes.size() > 2U - offset) {
        throw std::runtime_error("invalid virtio-input config write");
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (offset + index == 0)
            select_ = bytes[index];
        else
            subselect_ = bytes[index];
    }
}

uml::virtio::VhostUserQueueResult VirtioInputDevice::processQueue(
    std::uint32_t index,
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    (void)requestId;
    if (index == protocol::eventQueue) {
        if (!request.empty())
            throw std::runtime_error("virtio-input event buffer is readable");
        events_->eventBufferPosted();
        protocol::InputEvent event {};
        if (!events_->take(event))
            return {};
        if (responseCapacity < sizeof(event))
            throw std::runtime_error("short virtio-input event buffer");
        if (!reportedDelivery_) {
            std::fprintf(stdout, "%s event queue active\n", name());
            reportedDelivery_ = true;
        }
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = uml::virtio::toBytes(event),
        };
    }
    if (index != protocol::statusQueue)
        throw std::runtime_error("invalid virtio-input queue");
    if (request.size() != sizeof(protocol::InputEvent) ||
        responseCapacity != 0) {
        throw std::runtime_error("invalid virtio-input status buffer");
    }
    events_->guestStatus(
        uml::virtio::fromBytes<protocol::InputEvent>(request));
    return {
        .action = uml::virtio::VhostUserQueueAction::complete,
        .response = {},
    };
}

void VirtioInputDevice::reset() noexcept
{
    select_ = protocol::configUnset;
    subselect_ = 0;
    reportedDelivery_ = false;
    events_->reset();
}

int VirtioInputDevice::eventDescriptor() const noexcept
{
    return events_->descriptor();
}

void VirtioInputDevice::drainEventSource()
{
    events_->drain();
}

bool VirtioInputDevice::configChangePending() const noexcept
{
    return events_->configChangePending();
}

bool VirtioInputDevice::claimConfigChange() noexcept
{
    return events_->claimConfigChange();
}

void VirtioInputDevice::clearConfigChange() noexcept
{
    events_->clearConfigChange();
}

namespace {

void setRange(
    protocol::InputConfig& config,
    std::uint32_t maximum)
{
    const protocol::InputAbsoluteInfo info {
        .minimum = 0,
        .maximum = maximum,
        .fuzz = 0,
        .flat = 0,
        .resolution = 0,
    };
    copyValue(config, info);
}

void touchscreenEventBits(
    protocol::InputConfig& config,
    std::uint8_t subselect)
{
    if (subselect == protocol::eventKey) {
        setBit(config.value.bitmap, protocol::buttonTouch);
    } else if (subselect == protocol::eventAbsolute) {
        setBit(config.value.bitmap, protocol::absoluteX);
        setBit(config.value.bitmap, protocol::absoluteY);
        setBit(config.value.bitmap, protocol::absoluteMtSlot);
        setBit(config.value.bitmap, protocol::absoluteMtPositionX);
        setBit(config.value.bitmap, protocol::absoluteMtPositionY);
        setBit(config.value.bitmap, protocol::absoluteMtToolType);
        setBit(config.value.bitmap, protocol::absoluteMtTrackingId);
    }
}

void sideKeyEventBits(
    protocol::InputConfig& config,
    std::uint8_t subselect,
    std::optional<EventQueue::Switch> state)
{
    if (subselect == protocol::eventKey) {
        for (std::uint16_t code = 1; code <= protocol::keyboardKeyLast;
             ++code) {
            if (protocol::keyboardAdvertisesKey(code))
                setBit(config.value.bitmap, code);
        }
    } else if (subselect == protocol::eventLed) {
        setBit(config.value.bitmap, protocol::ledNumLock);
        setBit(config.value.bitmap, protocol::ledCapsLock);
        setBit(config.value.bitmap, protocol::ledScrollLock);
    } else if (subselect == protocol::eventSwitch && state) {
        setBit(config.value.bitmap, state->code);
    }
}

/* Exactly the keys the HID table can produce, and the three lock LEDs
 * Android lights. */
void hardwareKeyboardEventBits(
    protocol::InputConfig& config,
    std::uint8_t subselect)
{
    if (subselect == protocol::eventKey) {
        const auto& image = hid::keyImage();
        std::copy(image.begin(), image.end(), config.value.bitmap.begin());
    } else if (subselect == protocol::eventLed) {
        setBit(config.value.bitmap, protocol::ledNumLock);
        setBit(config.value.bitmap, protocol::ledCapsLock);
        setBit(config.value.bitmap, protocol::ledScrollLock);
    }
}

void setMouseButtons(protocol::InputConfig& config)
{
    for (const auto button : {protocol::buttonLeft, protocol::buttonRight,
                              protocol::buttonMiddle, protocol::buttonSide,
                              protocol::buttonExtra})
        setBit(config.value.bitmap, button);
}

void mouseEventBits(protocol::InputConfig& config, std::uint8_t subselect)
{
    if (subselect == protocol::eventKey) {
        setMouseButtons(config);
    } else if (subselect == protocol::eventRelative) {
        for (const auto axis :
             {protocol::relativeX, protocol::relativeY,
              protocol::relativeHorizontalWheel, protocol::relativeWheel,
              protocol::relativeWheelHighResolution,
              protocol::relativeHorizontalWheelHighResolution})
            setBit(config.value.bitmap, axis);
    } else if (subselect == protocol::eventLed) {
        setBit(config.value.bitmap, protocol::ledMisc);
    }
}

/* An absolute mouse: a contact the host holds in the first slot while the
 * pointer is over the guest's display, with no input property, and the
 * buttons. */
void pointerEventBits(protocol::InputConfig& config, std::uint8_t subselect)
{
    if (subselect == protocol::eventKey) {
        setBit(config.value.bitmap, protocol::buttonTouch);
        setBit(config.value.bitmap, protocol::buttonToolMouse);
        setMouseButtons(config);
    } else if (subselect == protocol::eventAbsolute) {
        setBit(config.value.bitmap, protocol::absoluteMtSlot);
        setBit(config.value.bitmap, protocol::absoluteMtPositionX);
        setBit(config.value.bitmap, protocol::absoluteMtPositionY);
        setBit(config.value.bitmap, protocol::absoluteMtTrackingId);
    }
}

} // namespace

protocol::InputConfig VirtioInputDevice::selectedConfiguration() const
{
    protocol::InputConfig config {};
    config.select = select_;
    config.subselect = subselect_;
    const auto info = kindInfo(kind_);
    /* An unplugged device keeps its identity and reports nothing it can do,
     * and so does a plugged one until its driver has probed (EventQueue): a
     * guest without hot-plug support reads an input device that has no
     * events, and a guest with it registers none. */
    const bool plugged = events_->advertised();

    switch (select_) {
    case protocol::configIdName:
        copyString(config, info.name);
        break;
    case protocol::configIdSerial:
        copyString(config, info.serial);
        break;
    case protocol::configIdDeviceIds: {
        const protocol::InputDeviceIds ids {
            .bus = protocol::busVirtual,
            .vendor = 0x1af4,
            .product = info.product,
            .version = 1,
        };
        copyValue(config, ids);
        break;
    }
    case protocol::configPropertyBits:
        if (plugged && kind_ == DeviceKind::touchscreen && subselect_ == 0) {
            setBit(config.value.bitmap, protocol::propertyDirect);
            config.size = bitmapSize(config.value.bitmap);
        }
        break;
    case protocol::configEventBits:
        if (!plugged)
            break;
        switch (kind_) {
        case DeviceKind::touchscreen:
            touchscreenEventBits(config, subselect_);
            break;
        case DeviceKind::keyboard:
            sideKeyEventBits(config, subselect_, events_->switchState());
            break;
        case DeviceKind::hardwareKeyboard:
            hardwareKeyboardEventBits(config, subselect_);
            break;
        case DeviceKind::mouse:
            mouseEventBits(config, subselect_);
            break;
        case DeviceKind::pointer:
            pointerEventBits(config, subselect_);
            break;
        }
        config.size = bitmapSize(config.value.bitmap);
        break;
    case protocol::configAbsoluteInfo:
        if (!plugged)
            break;
        if (kind_ == DeviceKind::touchscreen &&
            (subselect_ == protocol::absoluteX ||
             subselect_ == protocol::absoluteY ||
             subselect_ == protocol::absoluteMtPositionX ||
             subselect_ == protocol::absoluteMtPositionY ||
             subselect_ == protocol::absoluteMtSlot ||
             subselect_ == protocol::absoluteMtToolType ||
             subselect_ == protocol::absoluteMtTrackingId)) {
            setRange(
                config,
                subselect_ == protocol::absoluteMtSlot ?
                    protocol::touchscreenContactCount - 1 :
                subselect_ == protocol::absoluteMtToolType ?
                    static_cast<std::uint32_t>(protocol::toolTypePalm) :
                subselect_ == protocol::absoluteMtTrackingId ?
                    static_cast<std::uint32_t>(
                        protocol::touchscreenTrackingIdMaximum) :
                    protocol::touchscreenAbsoluteMaximum);
        } else if (kind_ == DeviceKind::pointer &&
                   (subselect_ == protocol::absoluteMtPositionX ||
                    subselect_ == protocol::absoluteMtPositionY ||
                    subselect_ == protocol::absoluteMtSlot ||
                    subselect_ == protocol::absoluteMtTrackingId)) {
            setRange(
                config,
                subselect_ == protocol::absoluteMtSlot ?
                    protocol::pointerSlotCount - 1 :
                subselect_ == protocol::absoluteMtTrackingId ?
                    static_cast<std::uint32_t>(
                        protocol::touchscreenTrackingIdMaximum) :
                    protocol::pointerAbsoluteMaximum);
        }
        break;
    default:
        break;
    }
    return config;
}

} // namespace uml::input::virtio
