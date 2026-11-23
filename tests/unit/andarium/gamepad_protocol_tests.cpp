// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest's end of the gamepad wire (vsock 5558) against the golden frames
// that Andarium's GamepadProtocol.swift checks too, and the reader that turns a
// byte stream into frames. argv[1] is the fixture,
// tests/unit/andarium/fixtures/gamepad-golden-frames.txt. The fixture holds
// three frames and three rejects; everything else is written out here, byte by
// byte, independently of the header under test.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "gamepad_protocol.hpp"
#include "require.hpp"

using namespace andarium::gamepad;
using uml::testing::require;
using Bytes = std::vector<uint8_t>;

namespace {
Bytes parse_hex(const std::string& text) {
    Bytes bytes;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        require(token.size() == 2, "a fixture byte is two hex digits");
        bytes.push_back(uint8_t(std::stoul(token, nullptr, 16)));
    }
    return bytes;
}

// `name: <hex bytes>` per line; a line that starts with # is a comment.
std::map<std::string, Bytes> read_fixture(const char* path) {
    std::ifstream file(path);
    require(file.is_open(), "the golden-frame fixture opens");
    std::map<std::string, Bytes> frames;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto colon = line.find(':');
        require(colon != std::string::npos, "a fixture line is name: bytes");
        require(frames.emplace(line.substr(0, colon), parse_hex(line.substr(colon + 1))).second,
                "fixture names are unique");
    }
    return frames;
}

// ---- The oracle: the wire as the design table writes it, independent of the header.

void put16(Bytes& out, unsigned value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
}
void put32(Bytes& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}

Bytes frame_of(unsigned type, const Bytes& payload) {
    Bytes frame{'A', 'G', 'P', '1'};
    put16(frame, type);
    put16(frame, unsigned(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// What the host may send for a frame of `type` and a payload of `length` bytes,
// from the design's table: the ranges of each type, and the limit of 4400.
bool host_header_allowed(unsigned type, unsigned length) {
    if (length > 4400) return false;
    switch (type) {
        case 1: return length >= 215 && length <= 214 + 4096;  // CREATE: 214 bytes and a descriptor of 1..4096
        case 2: return length == 4;                            // DESTROY
        case 3: return length >= 5 && length <= 4 + 4096;      // INPUT: 4 bytes and a report of 1..4096
        case 4: return length >= 10 && length <= 10 + 4096;    // GET_REPORT_REPLY: 10 bytes and 0..4096
        case 5: return length == 8;                            // SET_REPORT_REPLY
        default: return false;
    }
}

Bytes header_of(unsigned type, unsigned length, Bytes magic = {'A', 'G', 'P', '1'}) {
    Bytes header = std::move(magic);
    put16(header, type);
    put16(header, length);
    return header;
}

Bytes sequence(std::size_t count, uint8_t first = 0) {
    Bytes bytes(count);
    for (std::size_t i = 0; i < count; ++i) bytes[i] = uint8_t(first + i * 7 + i / 251);
    return bytes;
}

Bytes create_payload(unsigned slot, unsigned flags, unsigned bus, uint32_t vendor, uint32_t product, uint32_t version,
                     uint32_t country, const std::string& name, const std::string& uniq,
                     const Bytes& descriptor, std::optional<unsigned> declared_length = std::nullopt) {
    Bytes payload{uint8_t(slot), uint8_t(flags)};
    put16(payload, bus);
    put32(payload, vendor);
    put32(payload, product);
    put32(payload, version);
    put32(payload, country);
    Bytes name_field(128, 0);
    std::copy(name.begin(), name.end(), name_field.begin());
    payload.insert(payload.end(), name_field.begin(), name_field.end());
    Bytes uniq_field(64, 0);
    std::copy(uniq.begin(), uniq.end(), uniq_field.begin());
    payload.insert(payload.end(), uniq_field.begin(), uniq_field.end());
    put16(payload, declared_length.value_or(unsigned(descriptor.size())));
    payload.insert(payload.end(), descriptor.begin(), descriptor.end());
    return payload;
}

Bytes input_payload(unsigned slot, const Bytes& report, std::optional<unsigned> declared_length = std::nullopt) {
    Bytes payload{uint8_t(slot), 0};
    put16(payload, declared_length.value_or(unsigned(report.size())));
    payload.insert(payload.end(), report.begin(), report.end());
    return payload;
}

Bytes get_report_reply_payload(unsigned slot, unsigned error, uint32_t id, const Bytes& data,
                               std::optional<unsigned> declared_length = std::nullopt) {
    Bytes payload{uint8_t(slot), 0};
    put16(payload, error);
    put32(payload, id);
    put16(payload, declared_length.value_or(unsigned(data.size())));
    payload.insert(payload.end(), data.begin(), data.end());
    return payload;
}

Bytes set_report_reply_payload(unsigned slot, unsigned error, uint32_t id) {
    Bytes payload{uint8_t(slot), 0};
    put16(payload, error);
    put32(payload, id);
    return payload;
}

std::span<const uint8_t> view(const Bytes& bytes) { return std::span<const uint8_t>(bytes); }
Bytes bytes_of(std::span<const uint8_t> view_of) { return Bytes(view_of.begin(), view_of.end()); }

// ---- The checks.

void check_constants() {
    require(port == 5558, "the gamepad stream is vsock port 5558");
    require(header_size == 8 && maximum_payload == 4400, "a header is 8 bytes and a payload at most 4400");
    require(slot_count == 8, "eight slots");
    require(maximum_descriptor == 4096 && maximum_report == 4096, "descriptors and reports are at most 4096 bytes");
    require(name_size == 128 && uniq_size == 64, "the name is 128 bytes and the uniq 64");
    require(protocol_version == 1, "protocol version 1");
    require(magic == std::array<uint8_t, 4>{0x41, 0x47, 0x50, 0x31}, "the magic is AGP1");
    require(uint16_t(Type::create) == 1 && uint16_t(Type::destroy) == 2 && uint16_t(Type::input) == 3 &&
                uint16_t(Type::get_report_reply) == 4 && uint16_t(Type::set_report_reply) == 5,
            "the host's types");
    require(uint16_t(Type::hello) == 16 && uint16_t(Type::created) == 17 && uint16_t(Type::event) == 18 &&
                uint16_t(Type::output) == 19 && uint16_t(Type::get_report) == 20 &&
                uint16_t(Type::set_report) == 21,
            "the guest's types");
    require(uint8_t(EventKind::start) == 0 && uint8_t(EventKind::stop) == 1 && uint8_t(EventKind::open) == 2 &&
                uint8_t(EventKind::close) == 3,
            "the event kinds are uhid's events in order");
    // A CREATE of the longest descriptor, a GET_REPORT_REPLY and an INPUT of the longest report all fit.
    require(create_fixed_size + maximum_descriptor <= maximum_payload, "the longest CREATE fits the limit");
    require(get_report_reply_fixed_size + maximum_report <= maximum_payload, "the longest reply fits the limit");
    require(set_report_fixed_size + maximum_report <= maximum_payload, "the longest SET_REPORT fits the limit");
}

void check_fixture_frames(const std::map<std::string, Bytes>& fixture) {
    const auto frame = [&](const char* name) -> const Bytes& {
        const auto found = fixture.find(name);
        require(found != fixture.end(), name);
        return found->second;
    };
    // The bytes the design lists, one by one.
    require(frame("destroy-slot-1") == Bytes({0x41, 0x47, 0x50, 0x31, 0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00}),
            "destroy-slot-1");
    require(frame("input-slot-0") ==
                Bytes({0x41, 0x47, 0x50, 0x31, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x80, 0x80}),
            "input-slot-0");
    require(frame("hello") == Bytes({0x41, 0x47, 0x50, 0x31, 0x10, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08, 0x00}), "hello");
    require(frame("reject-magic") == Bytes({0x41, 0x47, 0x50, 0x32, 0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00}),
            "reject-magic");
    require(frame("reject-type") == Bytes({0x41, 0x47, 0x50, 0x31, 0xff, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00}),
            "reject-type");
    require(frame("reject-length") == Bytes({0x41, 0x47, 0x50, 0x31, 0x03, 0x00, 0x31, 0x11}), "reject-length");
    require(fixture.size() == 6, "the fixture holds three frames and three rejects");

    // The guest reads the two frames of the host as the slot and the report they name.
    {
        const auto& destroy = frame("destroy-slot-1");
        const auto header = parse_host_header(view(destroy));
        require(header && header->type == Type::destroy && header->length == 4, "destroy-slot-1's header");
        const auto decoded = decode_destroy(std::span(destroy).subspan(header_size));
        require(decoded && decoded->slot == 1, "destroy-slot-1 names slot 1");
    }
    {
        const auto& input = frame("input-slot-0");
        const auto header = parse_host_header(view(input));
        require(header && header->type == Type::input && header->length == 7, "input-slot-0's header");
        const auto decoded = decode_input(std::span(input).subspan(header_size));
        require(decoded && decoded->slot == 0 && bytes_of(decoded->report) == Bytes({0x01, 0x80, 0x80}),
                "input-slot-0 carries the report 01 80 80 for slot 0");
    }
    // The guest sends hello as the golden bytes, and the host's rules refuse nothing of it: it is a guest frame,
    // so the oracle for the guest's direction is written here, apart from the encoder.
    require(encode_hello() == frame("hello"), "the guest encodes hello as the golden frame");
    require(host_header_allowed(2, 4) && host_header_allowed(3, 7), "the oracle accepts the host's frames");

    // Every reject is refused by the header alone, as a stream and as a header, and the oracle agrees.
    for (const char* name : {"reject-magic", "reject-type", "reject-length"}) {
        const auto& bytes = frame(name);
        require(!parse_host_header(view(bytes)), std::string("the header of ") + name + " is refused");
        FrameReader reader;
        require(!reader.feed(view(bytes), [](Type, std::span<const uint8_t>) { return true; }),
                std::string("the stream with ") + name + " fails");
    }
    require(!host_header_allowed(0xff, 4) && !host_header_allowed(3, 0x1131), "the oracle refuses the rejects");
}

// The header against the oracle, over every type with a spread of lengths and every length of every type.
void check_header_decisions() {
    const unsigned lengths[] = {0,   1,    3,    4,    5,    7,    8,    9,    10,   11,    213,   214,   215,   216,
                                4099, 4100, 4101, 4105, 4106, 4107, 4309, 4310, 4311, 4399, 4400, 4401, 65535};
    for (unsigned type = 0; type < 65536; ++type) {
        for (const auto length : lengths) {
            const auto header = parse_host_header(header_of(type, length));
            require(header.has_value() == host_header_allowed(type, length), "header against the oracle");
            if (header) require(uint16_t(header->type) == type && header->length == length, "the header's fields");
        }
    }
    for (const unsigned type : {1u, 2u, 3u, 4u, 5u, 16u, 17u, 18u, 19u, 20u, 21u, 255u}) {
        for (unsigned length = 0; length < 65536; ++length) {
            require(parse_host_header(header_of(type, length)).has_value() == host_header_allowed(type, length),
                    "every length against the oracle");
        }
    }
    // The magic, byte by byte.
    for (unsigned position = 0; position < 4; ++position) {
        for (unsigned value = 0; value < 256; ++value) {
            Bytes header = header_of(2, 4);
            header[position] = uint8_t(value);
            require(parse_host_header(header).has_value() == (value == magic[position]), "the magic");
        }
    }
    require(!parse_host_header(Bytes{}) && !parse_host_header(Bytes{'A', 'G', 'P', '1', 2, 0, 4}), "a short header");
    // Types only the guest sends.
    for (unsigned type = 16; type <= 21; ++type) {
        require(!parse_host_header(header_of(type, 4)), "the host never sends a guest type");
    }
}

void check_create() {
    const Bytes descriptor = sequence(300, 9);
    const auto payload = create_payload(5, 0, 3, 0x054c, 0x0ce6, 0x8100, 33, "DualSense Wireless Controller",
                                        "kernarium-gamepad-5", descriptor);
    require(payload.size() == 214 + 300, "the CREATE oracle's size");
    const auto create = decode_create(payload);
    require(create.has_value(), "a CREATE decodes");
    require(create->slot == 5 && create->bus == 3 && create->vendor == 0x054c && create->product == 0x0ce6 &&
                create->version == 0x8100 && create->country == 33,
            "the CREATE fields");
    require(create->name.size() == 128 && create->uniq.size() == 64, "the name and uniq spans");
    require(std::string(reinterpret_cast<const char*>(create->name.data())) == "DualSense Wireless Controller",
            "the name");
    require(std::string(reinterpret_cast<const char*>(create->uniq.data())) == "kernarium-gamepad-5", "the uniq");
    require(bytes_of(create->descriptor) == descriptor, "the descriptor");
    // The spans lie inside the payload, at the offsets of the design.
    require(create->name.data() == payload.data() + 20 && create->uniq.data() == payload.data() + 148 &&
                create->descriptor.data() == payload.data() + 214,
            "the offsets of name, uniq and descriptor");

    // Slots 0 through 7, and the flags and the bus and ids at their extremes.
    for (unsigned slot = 0; slot < 256; ++slot) {
        const auto decoded = decode_create(create_payload(slot, 0, 0, 0, 0, 0, 0, "", "", Bytes{0x05}));
        require(decoded.has_value() == (slot < 8), "slots 0 through 7 only");
    }
    const auto wide = decode_create(create_payload(7, 0xff, 0xffff, 0xffffffffu, 0xfffffffeu, 0x80000000u,
                                                   0xffffffffu, "n", "u", Bytes{0x01}));
    require(wide && wide->bus == 0xffff && wide->vendor == 0xffffffffu && wide->product == 0xfffffffeu &&
                wide->version == 0x80000000u && wide->country == 0xffffffffu,
            "the 16- and 32-bit fields keep every bit");
    // The flags are reserved: any value reads the same.
    require(decode_create(create_payload(0, 0xa5, 3, 1, 2, 3, 4, "n", "u", Bytes{0x05})).has_value(), "flags are ignored");

    // The descriptor's length: 1 and 4096 are the ends, 0 and 4097 are out, and it must fill the payload.
    require(decode_create(create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(1))).has_value(), "a descriptor of 1");
    const auto longest_payload = create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(4096));
    const auto longest = decode_create(longest_payload);
    require(longest && longest->descriptor.size() == 4096 && bytes_of(longest->descriptor) == sequence(4096),
            "a descriptor of 4096");
    require(!decode_create(create_payload(0, 0, 3, 1, 2, 3, 0, "", "", Bytes{})), "no descriptor");
    require(!decode_create(create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(4097))), "a descriptor of 4097");
    require(!decode_create(create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(10), 11)), "a length longer than the data");
    require(!decode_create(create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(10), 9)), "a length shorter than the data");
    require(!decode_create(create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(10), 0)), "a length of zero over data");
    Bytes truncated = create_payload(0, 0, 3, 1, 2, 3, 0, "", "", sequence(10));
    truncated.resize(213);
    require(!decode_create(truncated) && !decode_create(Bytes{}) && !decode_create(Bytes(214, 0)),
            "a payload with no room for a descriptor");
    // A name or a uniq with no end is the relay's to end: the decoder hands over all of the bytes.
    const auto full_payload =
        create_payload(0, 0, 3, 1, 2, 3, 0, std::string(128, 'x'), std::string(64, 'y'), Bytes{0x05});
    const auto full = decode_create(full_payload);
    require(full && bytes_of(full->name) == Bytes(128, 'x') && bytes_of(full->uniq) == Bytes(64, 'y'),
            "a name and a uniq that fill their fields");
}

void check_destroy_and_input() {
    for (unsigned slot = 0; slot < 256; ++slot) {
        require(decode_destroy(Bytes{uint8_t(slot), 0, 0, 0}).has_value() == (slot < 8), "DESTROY's slot");
        require(decode_input(input_payload(slot, Bytes{1, 2, 3})).has_value() == (slot < 8), "INPUT's slot");
    }
    // The reserved bytes of DESTROY are ignored.
    require(decode_destroy(Bytes{3, 0xff, 0xff, 0xff}) && decode_destroy(Bytes{3, 0xff, 0xff, 0xff})->slot == 3,
            "DESTROY's reserved bytes");
    require(!decode_destroy(Bytes{1, 0, 0}) && !decode_destroy(Bytes{1, 0, 0, 0, 0}) && !decode_destroy(Bytes{}),
            "DESTROY is exactly 4 bytes");

    const auto input_bytes = input_payload(6, Bytes{0x01, 0x80, 0x7f, 0x00});
    const auto input = decode_input(input_bytes);
    require(input && input->slot == 6 && bytes_of(input->report) == Bytes({0x01, 0x80, 0x7f, 0x00}), "an INPUT");
    require(decode_input(input_payload(0, sequence(1))).has_value(), "a report of 1");
    const auto longest_bytes = input_payload(0, sequence(4096));
    const auto longest = decode_input(longest_bytes);
    require(longest && longest->report.size() == 4096 && bytes_of(longest->report) == sequence(4096),
            "a report of 4096");
    require(!decode_input(input_payload(0, Bytes{})), "no report");
    require(!decode_input(input_payload(0, sequence(4097))), "a report of 4097");
    require(!decode_input(input_payload(0, sequence(3), 4)) && !decode_input(input_payload(0, sequence(3), 2)) &&
                !decode_input(input_payload(0, sequence(3), 0)),
            "a length that does not fill the payload");
    require(!decode_input(Bytes{0, 0, 3}) && !decode_input(Bytes{}), "a payload with no room for a report");
    // The reserved byte is ignored.
    require(decode_input(Bytes{2, 0xaa, 1, 0, 0x42}).has_value(), "INPUT's reserved byte");
}

void check_replies() {
    const auto reply_bytes = get_report_reply_payload(4, 0, 0xdeadbeefu, Bytes{0x05, 1, 2, 3});
    const auto reply = decode_get_report_reply(reply_bytes);
    require(reply && reply->slot == 4 && reply->error == 0 && reply->id == 0xdeadbeefu &&
                bytes_of(reply->data) == Bytes({0x05, 1, 2, 3}),
            "a GET_REPORT_REPLY");
    const auto failed_bytes = get_report_reply_payload(1, 5, 7, Bytes{});
    const auto failed = decode_get_report_reply(failed_bytes);
    require(failed && failed->error == 5 && failed->id == 7 && failed->data.empty(), "a refused GET_REPORT_REPLY has no data");
    const auto longest_bytes = get_report_reply_payload(0, 0, 1, sequence(4096));
    const auto longest = decode_get_report_reply(longest_bytes);
    require(longest && longest->data.size() == 4096 && bytes_of(longest->data) == sequence(4096), "a reply of 4096");
    require(!decode_get_report_reply(get_report_reply_payload(0, 0, 1, sequence(4097))), "a reply of 4097");
    require(!decode_get_report_reply(get_report_reply_payload(0, 0, 1, sequence(3), 4)) &&
                !decode_get_report_reply(get_report_reply_payload(0, 0, 1, sequence(3), 2)),
            "a length that does not fill the payload");
    require(!decode_get_report_reply(Bytes(9, 0)) && !decode_get_report_reply(Bytes{}), "a short GET_REPORT_REPLY");
    for (unsigned slot = 0; slot < 256; ++slot) {
        require(decode_get_report_reply(get_report_reply_payload(slot, 0, 1, Bytes{1})).has_value() == (slot < 8),
                "GET_REPORT_REPLY's slot");
        require(decode_set_report_reply(set_report_reply_payload(slot, 0, 1)).has_value() == (slot < 8),
                "SET_REPORT_REPLY's slot");
    }

    const auto set = decode_set_report_reply(set_report_reply_payload(2, 22, 0x01020304u));
    require(set && set->slot == 2 && set->error == 22 && set->id == 0x01020304u, "a SET_REPORT_REPLY");
    require(!decode_set_report_reply(Bytes(7, 0)) && !decode_set_report_reply(Bytes(9, 0)) &&
                !decode_set_report_reply(Bytes{}),
            "SET_REPORT_REPLY is exactly 8 bytes");
}

// The guest's frames, against hand-built bytes and a small reader written here.
struct GuestFrame {
    unsigned type;
    Bytes payload;
};

GuestFrame read_guest_frame(const Bytes& bytes) {
    require(bytes.size() >= 8 && std::equal(bytes.begin(), bytes.begin() + 4, magic.begin()), "the guest's magic");
    const unsigned type = bytes[4] | (bytes[5] << 8);
    const unsigned length = bytes[6] | (bytes[7] << 8);
    require(bytes.size() == 8 + length, "the guest's payload length is the rest of the frame");
    require(length <= 4400, "the guest's frames respect the limit too");
    return {type, Bytes(bytes.begin() + 8, bytes.end())};
}

void check_encoders() {
    // HELLO is the golden frame; the others are written out.
    require(encode_hello() == Bytes({0x41, 0x47, 0x50, 0x31, 0x10, 0x00, 0x04, 0x00, 0x01, 0x00, 0x08, 0x00}), "HELLO");

    require(encode_created(0, 0) == frame_of(17, Bytes{0, 0, 0, 0, 0, 0, 0, 0}), "CREATED slot 0, result 0");
    require(encode_created(7, -19) == frame_of(17, Bytes{7, 0, 0, 0, 0xed, 0xff, 0xff, 0xff}),
            "CREATED slot 7, result -ENODEV");
    require(encode_created(3, -2147483647 - 1) == frame_of(17, Bytes{3, 0, 0, 0, 0, 0, 0, 0x80}), "CREATED, the lowest result");
    require(encode_created(1, 0x7fffffff) == frame_of(17, Bytes{1, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f}),
            "CREATED, the highest result");

    require(encode_event(0, EventKind::start) == frame_of(18, Bytes{0, 0, 0, 0}), "EVENT start");
    require(encode_event(2, EventKind::stop) == frame_of(18, Bytes{2, 1, 0, 0}), "EVENT stop");
    require(encode_event(4, EventKind::open) == frame_of(18, Bytes{4, 2, 0, 0}), "EVENT open");
    require(encode_event(7, EventKind::close) == frame_of(18, Bytes{7, 3, 0, 0}), "EVENT close");

    const Bytes rumble{0x02, 0x03, 0x04, 0x00, 0xff, 0x80};
    const auto output = encode_output(6, 1, rumble);
    require(output && *output == frame_of(19, Bytes{6, 1, 6, 0, 0x02, 0x03, 0x04, 0x00, 0xff, 0x80}), "OUTPUT");
    const auto empty_output = encode_output(1, 1, Bytes{});
    require(empty_output && *empty_output == frame_of(19, Bytes{1, 1, 0, 0}), "an empty OUTPUT");
    const auto longest_output = encode_output(0, 0, sequence(4096));
    require(longest_output && read_guest_frame(*longest_output).payload.size() == 4100, "an OUTPUT of 4096");
    require(!encode_output(0, 0, sequence(4097)), "an OUTPUT of 4097 is not encoded");

    require(encode_get_report(2, 0, 0x09, 0x04030201u) == frame_of(20, Bytes{2, 0, 9, 0, 1, 2, 3, 4}), "GET_REPORT");
    require(encode_get_report(7, 2, 0xff, 0xffffffffu) == frame_of(20, Bytes{7, 2, 0xff, 0, 0xff, 0xff, 0xff, 0xff}),
            "GET_REPORT, the largest values");

    const Bytes feature{0x03, 0xaa, 0xbb};
    const auto set = encode_set_report(5, 2, 3, 0x0000ff01u, feature);
    require(set && *set == frame_of(21, Bytes{5, 2, 3, 0, 1, 0xff, 0, 0, 3, 0, 0x03, 0xaa, 0xbb}), "SET_REPORT");
    const auto empty_set = encode_set_report(0, 1, 0, 9, Bytes{});
    require(empty_set && *empty_set == frame_of(21, Bytes{0, 1, 0, 0, 9, 0, 0, 0, 0, 0}), "an empty SET_REPORT");
    const auto longest_set = encode_set_report(0, 1, 0, 9, sequence(4096));
    require(longest_set && read_guest_frame(*longest_set).payload.size() == 4106, "a SET_REPORT of 4096");
    require(!encode_set_report(0, 1, 0, 9, sequence(4097)), "a SET_REPORT of 4097 is not encoded");

    // Every frame the guest encodes is a frame of the table: a guest type and a payload within the limit.
    for (const auto& bytes : {encode_hello(), encode_created(1, 0), encode_event(1, EventKind::start),
                              encode_get_report(1, 0, 1, 1), *output, *set, *longest_output, *longest_set}) {
        const auto frame = read_guest_frame(bytes);
        require(frame.type >= 16 && frame.type <= 21, "the guest sends guest types");
    }
}

void check_reader(const std::map<std::string, Bytes>& fixture) {
    // Every kind of frame of the host in one stream.
    const Bytes descriptor = sequence(1500, 3);
    const Bytes stream_frames[] = {
        frame_of(1, create_payload(2, 0, 3, 0x054c, 0x0ce6, 0x8100, 0, "name", "uniq", descriptor)),
        fixture.at("input-slot-0"),
        frame_of(4, get_report_reply_payload(2, 0, 11, sequence(63, 1))),
        frame_of(5, set_report_reply_payload(2, 0, 12)),
        fixture.at("destroy-slot-1"),
        frame_of(1, create_payload(7, 0, 5, 0x045e, 0x0b13, 0x0903, 0, "pad", "", sequence(4096))),
        frame_of(3, input_payload(7, sequence(4096, 5))),
        fixture.at("input-slot-0"),
    };
    Bytes all;
    for (const auto& frame : stream_frames) all.insert(all.end(), frame.begin(), frame.end());

    struct Seen {
        Type type;
        Bytes payload;
    };
    const auto collect = [](FrameReader& reader, std::span<const uint8_t> bytes, std::vector<Seen>& seen) {
        return reader.feed(bytes, [&seen](Type type, std::span<const uint8_t> payload) {
            seen.push_back({type, bytes_of(payload)});
            return true;
        });
    };
    const auto check_seen = [&](const std::vector<Seen>& seen, const char* how) {
        require(seen.size() == std::size(stream_frames), std::string(how) + ": every frame");
        for (std::size_t i = 0; i < seen.size(); ++i) {
            const Bytes expected(stream_frames[i].begin() + 8, stream_frames[i].end());
            require(uint16_t(seen[i].type) == (stream_frames[i][4] | (stream_frames[i][5] << 8)) &&
                        seen[i].payload == expected,
                    std::string(how) + ": frame " + std::to_string(i));
        }
    };

    {
        FrameReader reader;
        std::vector<Seen> seen;
        require(collect(reader, all, seen), "whole frames");
        check_seen(seen, "whole frames");
    }
    {
        FrameReader reader;
        std::vector<Seen> seen;
        for (const auto byte : all) require(collect(reader, std::span(&byte, 1), seen), "one byte at a time");
        check_seen(seen, "one byte at a time");
    }
    for (const std::size_t first : {1u, 5u, 8u, 9u, 13u, 997u}) {
        FrameReader reader;
        std::vector<Seen> seen;
        std::size_t size = first;
        for (std::size_t at = 0; at < all.size(); at += size, size = size % 4099 + 3) {
            const auto piece = std::span(all).subspan(at, std::min(size, all.size() - at));
            require(collect(reader, piece, seen), "uneven pieces");
        }
        check_seen(seen, "uneven pieces");
    }

    // A frame that stops half way is held, in the header or in the payload, and the rest completes it.
    for (const std::size_t cut : {3u, 8u, 9u, 11u}) {
        FrameReader reader;
        std::vector<Seen> seen;
        const auto& input = fixture.at("input-slot-0");
        require(collect(reader, std::span(input).first(cut), seen) && seen.empty(), "half a frame is held");
        require(collect(reader, std::span(input).subspan(cut), seen) && seen.size() == 1 &&
                    seen[0].type == Type::input && seen[0].payload == Bytes({0, 0, 3, 0, 1, 0x80, 0x80}),
                "the rest completes it");
    }

    // A header that breaks the rules ends the stream's use as soon as it is whole; the frames before it count and
    // the bytes after it are dropped.
    for (const char* name : {"reject-magic", "reject-type", "reject-length"}) {
        Bytes stream = fixture.at("destroy-slot-1");
        const auto& bad = fixture.at(name);
        stream.insert(stream.end(), bad.begin(), bad.end());
        const auto& after = fixture.at("input-slot-0");
        stream.insert(stream.end(), after.begin(), after.end());
        FrameReader reader;
        std::vector<Seen> seen;
        require(!collect(reader, stream, seen), std::string("a stream with ") + name + " fails");
        require(seen.size() == 1 && seen[0].type == Type::destroy, std::string("only the frame before ") + name + " counts");
        // After a failure the reader takes nothing more, until it is reset.
        require(!collect(reader, fixture.at("input-slot-0"), seen) && seen.size() == 1, "a failed reader stays failed");
        reader.reset();
        require(collect(reader, fixture.at("input-slot-0"), seen) && seen.size() == 2 && seen[1].type == Type::input,
                "a reset reader starts clean");
    }
    // A bad header of a type the host never sends, and a length the type does not allow, in a stream.
    for (const Bytes& bad : {header_of(16, 4), header_of(1, 10), header_of(2, 5), header_of(3, 4), header_of(5, 9),
                             header_of(4, 9), header_of(3, 4401), header_of(0, 4)}) {
        FrameReader reader;
        std::vector<Seen> seen;
        require(!collect(reader, bad, seen) && seen.empty(), "a header the host may not send");
    }

    // A payload that the handler refuses ends the stream's use too: the frame is whole but wrong.
    {
        FrameReader reader;
        int calls = 0;
        const auto ok = reader.feed(all, [&calls](Type, std::span<const uint8_t>) { return ++calls < 3; });
        require(!ok && calls == 3, "the handler ends the stream by returning false");
        require(!reader.feed(fixture.at("input-slot-0"), [](Type, std::span<const uint8_t>) { return true; }),
                "and the reader stays failed");
    }

    // A half frame is dropped by a reset.
    {
        FrameReader reader;
        std::vector<Seen> seen;
        require(collect(reader, std::span(fixture.at("input-slot-0")).first(10), seen) && seen.empty(), "a held half frame");
        reader.reset();
        require(collect(reader, fixture.at("destroy-slot-1"), seen) && seen.size() == 1 && seen[0].type == Type::destroy,
                "a reset drops a half frame");
    }
    // An empty feed does nothing.
    {
        FrameReader reader;
        std::vector<Seen> seen;
        require(collect(reader, std::span<const uint8_t>(), seen) && seen.empty(), "an empty feed");
    }
}
}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: gamepad_protocol_tests <gamepad-golden-frames.txt>");
    const auto fixture = read_fixture(argv[1]);
    check_constants();
    check_fixture_frames(fixture);
    check_header_decisions();
    check_create();
    check_destroy_and_input();
    check_replies();
    check_encoders();
    check_reader(fixture);
    std::puts("gamepad protocol: ok");
    return 0;
}
