// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "kernarium.gamepad"

#include "Relay.h"

#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <log/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace andarium::gamepad {
using android::base::unique_fd;

namespace {
constexpr const char* uhid_path = "/dev/uhid";
// A device that has events to read is served this many before the relay looks at
// the others again, so one chatty device cannot hold the host's frames back.
constexpr int events_per_turn = 32;
// A device whose input reports fail to write logs the first failure and then one
// in this many.
constexpr uint32_t input_failure_log_interval = 256;
constexpr std::size_t get_report_reply_header = offsetof(uhid_event, u.get_report_reply.data);
constexpr std::size_t set_report_reply_size = offsetof(uhid_event, u.set_report_reply) + sizeof(uhid_set_report_reply_req);
constexpr std::size_t create2_header = offsetof(uhid_event, u.create2.rd_data);
constexpr std::size_t input2_header = offsetof(uhid_event, u.input2.data);

// The wire's sizes and numbers against the kernel's own header, so that a change
// of either is a compile error here and not a device that never appears.
static_assert(maximum_descriptor == HID_MAX_DESCRIPTOR_SIZE);
static_assert(maximum_report == UHID_DATA_MAX);
static_assert(name_size == sizeof(uhid_create2_req::name));
static_assert(uniq_size == sizeof(uhid_create2_req::uniq));
static_assert(UHID_STOP == UHID_START + 1 && UHID_OPEN == UHID_START + 2 && UHID_CLOSE == UHID_START + 3);
static_assert(offsetof(uhid_event, u) == sizeof(uint32_t));
static_assert(sizeof(uhid_get_report_reply_req::err) == sizeof(uint16_t));
static_assert(sizeof(uhid_output_req::size) == sizeof(uint16_t));
}  // namespace

bool Relay::listen_on_vsock() {
    listener_.reset(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    sockaddr_vm address{};
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_ANY;
    address.svm_port = port;
    if (listener_ < 0 || bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        listen(listener_.get(), 2)) {
        ALOGE("cannot listen on vsock port %u: %s", port, strerror(errno));
        return false;
    }
    return true;
}

int Relay::run() {
    if (!listen_on_vsock()) return 1;
    ALOGI("listening on vsock port %u", port);
    for (;;) {
        // A frame that could not be sent (the HELLO of a new connection, say) has
        // ended the connection already.
        if (broken_) drop_host();
        // The service starts at boot whether or not a host ever connects, so the
        // wait has no timeout: with no host and no device there is one
        // descriptor, the listener, and nothing wakes the relay.
        pollfd descriptors[2 + slot_count];
        descriptors[0] = {listener_.get(), POLLIN, 0};
        descriptors[1] = {host_.get(), POLLIN, 0};
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
            descriptors[2 + slot] = {slots_[slot].uhid.get(), POLLIN, 0};
        }
        const auto ready = poll(descriptors, 2 + slot_count, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            ALOGE("poll failed: %s", strerror(errno));
            return 1;
        }
        // A listener that reports an error or a hang-up and has nothing to
        // accept would make poll return at once from now on: leave, and init
        // starts the relay again with a new listener.
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(descriptors[0].revents & POLLIN)) {
            ALOGE("the listener on vsock port %u failed (poll events 0x%x)", port,
                  static_cast<unsigned>(descriptors[0].revents));
            return 1;
        }
        if (descriptors[0].revents & POLLIN) {
            if (!accept_host()) return 1;
            // Every device was replaced; what the other descriptors reported is stale.
            continue;
        }
        if (descriptors[1].revents != 0) {
            read_host();
            if (broken_) continue;
        }
        for (std::size_t slot = 0; slot < slot_count && !broken_; ++slot) {
            // A frame handled above may have replaced or destroyed this device.
            if (descriptors[2 + slot].revents != 0 && descriptors[2 + slot].fd == slots_[slot].uhid.get()) {
                service_slot(slot);
            }
        }
    }
}

bool Relay::accept_host() {
    sockaddr_vm peer{};
    socklen_t length = sizeof(peer);
    unique_fd accepted(accept4(listener_.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                               SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (accepted < 0) {
        if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED) return true;
        ALOGE("accept failed: %s", strerror(errno));
        return false;
    }
    // Only the private host channel may create a controller.
    if (peer.svm_cid != VMADDR_CID_HOST) {
        ALOGW("refused a connection from CID %u", peer.svm_cid);
        return true;
    }
    // A host that connects again replaces the one before it, which it may have
    // dropped without a close; the devices of the old connection go first.
    drop_host();
    host_ = std::move(accepted);
    counters_ = {};
    ALOGI("host connected");
    send_frame(encode_hello());
    return true;
}

void Relay::read_host() {
    uint8_t buffer[4096];
    const auto count = recv(host_.get(), buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (count <= 0) {
        if (count < 0) ALOGW("receive failed: %s", strerror(errno));
        broken_ = true;
        return;
    }
    const auto ok = reader_.feed(std::span<const uint8_t>(buffer, std::size_t(count)),
                                 [this](Type type, std::span<const uint8_t> payload) {
                                     return handle_frame(type, payload);
                                 });
    // A failed write has said why already.
    if (!ok && !broken_) {
        ALOGW("host sent a frame that breaks the protocol");
        broken_ = true;
    }
}

bool Relay::handle_frame(Type type, std::span<const uint8_t> payload) {
    // A frame after a failed write is not worth its work: the connection ends this turn.
    if (broken_) return false;
    ++counters_.frames;
    switch (type) {
        case Type::create: {
            const auto create = decode_create(payload);
            if (!create) return false;
            create_slot(*create);
            return true;
        }
        case Type::destroy: {
            const auto destroy = decode_destroy(payload);
            if (!destroy) return false;
            destroy_slot(destroy->slot);
            return true;
        }
        case Type::input: {
            const auto input = decode_input(payload);
            if (!input) return false;
            write_input(*input);
            return true;
        }
        case Type::get_report_reply: {
            const auto reply = decode_get_report_reply(payload);
            if (!reply) return false;
            if (slots_[reply->slot].uhid < 0) return true;
            out_.type = UHID_GET_REPORT_REPLY;
            out_.u.get_report_reply.id = reply->id;
            out_.u.get_report_reply.err = reply->error;
            out_.u.get_report_reply.size = uint16_t(reply->data.size());
            if (!reply->data.empty()) {
                std::memcpy(out_.u.get_report_reply.data, reply->data.data(), reply->data.size());
            }
            ++counters_.replies;
            write_reply(reply->slot, get_report_reply_header + reply->data.size());
            return true;
        }
        case Type::set_report_reply: {
            const auto reply = decode_set_report_reply(payload);
            if (!reply) return false;
            if (slots_[reply->slot].uhid < 0) return true;
            out_.type = UHID_SET_REPORT_REPLY;
            out_.u.set_report_reply.id = reply->id;
            out_.u.set_report_reply.err = reply->error;
            ++counters_.replies;
            write_reply(reply->slot, set_report_reply_size);
            return true;
        }
        default:
            // The reader refuses every type the host may not send.
            return false;
    }
}

void Relay::create_slot(const Create& create) {
    // A CREATE for a used slot replaces that slot's device.
    destroy_slot(create.slot);
    int result = 0;
    unique_fd uhid(open(uhid_path, O_RDWR | O_CLOEXEC | O_NONBLOCK));
    if (uhid < 0) {
        result = -errno;
        ALOGW("slot %u: cannot open %s: %s", unsigned(create.slot), uhid_path, strerror(errno));
    } else {
        // The kernel reads the event as far as the write goes and zero-fills the
        // rest, so only the descriptor's own length is written. The names are
        // ended here, whatever the host sent. The location of a device is its
        // slot's, so that Android's descriptor for it is the same in every session.
        std::memset(&out_, 0, create2_header);
        out_.type = UHID_CREATE2;
        auto& request = out_.u.create2;
        std::memcpy(request.name, create.name.data(), name_size);
        request.name[name_size - 1] = 0;
        std::memcpy(request.uniq, create.uniq.data(), uniq_size);
        request.uniq[uniq_size - 1] = 0;
        std::snprintf(reinterpret_cast<char*>(request.phys), sizeof(request.phys), "kernarium-gamepad-%u",
                      unsigned(create.slot));
        request.rd_size = uint16_t(create.descriptor.size());
        request.bus = create.bus;
        request.vendor = create.vendor;
        request.product = create.product;
        request.version = create.version;
        request.country = create.country;
        std::memcpy(request.rd_data, create.descriptor.data(), create.descriptor.size());
        const std::size_t size = create2_header + create.descriptor.size();
        const auto written = write(uhid.get(), &out_, size);
        if (written != ssize_t(size)) {
            result = written < 0 ? -errno : -EIO;
            ALOGW("slot %u: cannot create the device: %s", unsigned(create.slot), strerror(-result));
        }
    }
    if (result == 0) {
        auto& slot = slots_[create.slot];
        slot.uhid = std::move(uhid);
        slot.input_failures = 0;
        ++counters_.created;
        ALOGI("slot %u: created %04x:%04x on bus %u, a descriptor of %zu bytes", unsigned(create.slot),
              create.vendor, create.product, unsigned(create.bus), create.descriptor.size());
    } else {
        ++counters_.create_failed;
    }
    send_frame(encode_created(create.slot, result));
}

void Relay::destroy_slot(std::size_t slot) {
    auto& entry = slots_[slot];
    if (entry.uhid < 0) return;
    // Closing the descriptor destroys the device as well; the event makes the
    // kernel remove it before the relay moves on.
    const uint32_t type = UHID_DESTROY;
    (void)!write(entry.uhid.get(), &type, sizeof(type));
    entry.uhid.reset();
    ALOGI("slot %zu: destroyed", slot);
}

void Relay::write_input(const Input& input) {
    auto& slot = slots_[input.slot];
    if (slot.uhid < 0) {
        // The host and the guest disagree about which devices exist for as long
        // as one frame is in flight; the next CREATE settles it.
        ++counters_.input_dropped;
        return;
    }
    out_.type = UHID_INPUT2;
    out_.u.input2.size = uint16_t(input.report.size());
    std::memcpy(out_.u.input2.data, input.report.data(), input.report.size());
    const std::size_t size = input2_header + input.report.size();
    if (write(slot.uhid.get(), &out_, size) == ssize_t(size)) {
        ++counters_.input;
        return;
    }
    ++counters_.input_failed;
    if (slot.input_failures++ % input_failure_log_interval == 0) {
        ALOGW("slot %u: an input report was not accepted: %s (%u so far)", unsigned(input.slot), strerror(errno),
              slot.input_failures);
    }
}

void Relay::write_reply(std::size_t slot, std::size_t size) {
    const auto written = write(slots_[slot].uhid.get(), &out_, size);
    if (written != ssize_t(size)) {
        ALOGW("slot %zu: a reply was not accepted (%zd): %s", slot, written, strerror(errno));
    }
}

void Relay::service_slot(std::size_t slot) {
    auto& entry = slots_[slot];
    for (int served = 0; served < events_per_turn; ++served) {
        const auto count = read(entry.uhid.get(), &in_, sizeof(in_));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EAGAIN) return;
        if (count < static_cast<ssize_t>(sizeof(in_.type))) {
            // A device that cannot be read is gone, whatever the kernel says.
            ALOGW("slot %zu: reading the device failed (%zd): %s", slot, count, strerror(errno));
            destroy_slot(slot);
            return;
        }
        handle_uhid_event(slot, in_);
        if (broken_) return;
    }
}

void Relay::handle_uhid_event(std::size_t slot, const uhid_event& event) {
    const auto id = uint8_t(slot);
    switch (event.type) {
        case UHID_START:
        case UHID_STOP:
        case UHID_OPEN:
        case UHID_CLOSE:
            send_frame(encode_event(id, EventKind(event.type - UHID_START)));
            break;
        case UHID_OUTPUT: {
            const auto size = std::min<std::size_t>(event.u.output.size, maximum_report);
            if (const auto frame = encode_output(id, event.u.output.rtype,
                                                 std::span<const uint8_t>(event.u.output.data, size))) {
                ++counters_.output;
                send_frame(*frame);
            }
            break;
        }
        case UHID_GET_REPORT:
            ++counters_.requests;
            if (!send_frame(encode_get_report(id, event.u.get_report.rtype, event.u.get_report.rnum,
                                              event.u.get_report.id))) {
                refuse_request(slot, event);
            }
            break;
        case UHID_SET_REPORT: {
            ++counters_.requests;
            const auto size = std::min<std::size_t>(event.u.set_report.size, maximum_report);
            const auto frame = encode_set_report(id, event.u.set_report.rtype, event.u.set_report.rnum,
                                                 event.u.set_report.id,
                                                 std::span<const uint8_t>(event.u.set_report.data, size));
            if (!frame || !send_frame(*frame)) refuse_request(slot, event);
            break;
        }
        default:
            break;
    }
}

void Relay::refuse_request(std::size_t slot, const uhid_event& event) {
    std::memset(&out_, 0, get_report_reply_header);
    if (event.type == UHID_GET_REPORT) {
        out_.type = UHID_GET_REPORT_REPLY;
        out_.u.get_report_reply.id = event.u.get_report.id;
        out_.u.get_report_reply.err = EIO;
        write_reply(slot, get_report_reply_header);
    } else {
        out_.type = UHID_SET_REPORT_REPLY;
        out_.u.set_report_reply.id = event.u.set_report.id;
        out_.u.set_report_reply.err = EIO;
        write_reply(slot, set_report_reply_size);
    }
}

bool Relay::send_frame(std::span<const uint8_t> frame) {
    if (host_ < 0 || broken_) return false;
    const auto sent = send(host_.get(), frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(frame.size())) return true;
    // A frame that went out in part would leave the host reading garbage, and one
    // that did not go out would leave it with a stale picture: end the connection.
    // The host connects again and creates its controllers again.
    ALOGW("a frame of %zu bytes was not sent whole (%zd): %s", frame.size(), sent,
          sent < 0 ? strerror(errno) : "the socket took part of it");
    shutdown(host_.get(), SHUT_RDWR);
    broken_ = true;
    return false;
}

void Relay::drop_host() {
    if (host_ >= 0) {
        ALOGI("host disconnected: %u frames, %u devices created (%u refused), %u input reports written (%u dropped, "
              "%u failed), %u output reports, %u requests, %u replies",
              counters_.frames, counters_.created, counters_.create_failed, counters_.input,
              counters_.input_dropped, counters_.input_failed, counters_.output, counters_.requests,
              counters_.replies);
    }
    for (std::size_t slot = 0; slot < slot_count; ++slot) destroy_slot(slot);
    host_.reset();
    reader_.reset();
    broken_ = false;
}
}  // namespace andarium::gamepad
