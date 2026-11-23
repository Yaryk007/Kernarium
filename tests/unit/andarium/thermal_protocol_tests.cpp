// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest's end of the thermal wire (vsock 5559) against the golden frames
// that Andarium's ThermalProtocol.swift checks too, and the reader that turns a
// byte stream into records. argv[1] is the fixture,
// tests/unit/andarium/fixtures/thermal-golden-frames.txt.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "require.hpp"
#include "thermal_protocol.hpp"

using namespace andarium::thermal;
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

// The host's rules for a record it may send (ThermalProtocol.swift), written
// out here independently of the decoder: the oracle for what the guest reads
// and for the fixture's `reject-` frames.
bool host_may_send(const Bytes& frame) {
    return frame.size() == 8 && frame[0] == 'A' && frame[1] == 'T' && frame[2] == 'H' && frame[3] == '1' &&
           frame[4] <= 4 && frame[5] == 0 && frame[6] == 0 && frame[7] == 0;
}

Bytes record_of(uint8_t severity) { return {'A', 'T', 'H', '1', severity, 0, 0, 0}; }

void check_constants() {
    require(port == 5559, "the thermal stream is vsock port 5559");
    require(record_size == 8, "a record is 8 bytes");
    require(highest_severity == 4, "CRITICAL is the highest severity");
    require(uint8_t(Severity::none) == 0 && uint8_t(Severity::light) == 1 && uint8_t(Severity::moderate) == 2 &&
                uint8_t(Severity::severe) == 3 && uint8_t(Severity::critical) == 4,
            "the severities are ThrottlingSeverity's numbers");
}

void check_fixture_frames(const std::map<std::string, Bytes>& fixture) {
    const auto frame = [&](const char* name) -> const Bytes& {
        const auto found = fixture.find(name);
        require(found != fixture.end(), name);
        return found->second;
    };
    // The bytes the design lists, one by one.
    require(frame("none") == Bytes({0x41, 0x54, 0x48, 0x31, 0, 0, 0, 0}), "none");
    require(frame("light") == Bytes({0x41, 0x54, 0x48, 0x31, 1, 0, 0, 0}), "light");
    require(frame("moderate") == Bytes({0x41, 0x54, 0x48, 0x31, 2, 0, 0, 0}), "moderate");
    require(frame("severe") == Bytes({0x41, 0x54, 0x48, 0x31, 3, 0, 0, 0}), "severe");
    require(frame("critical") == Bytes({0x41, 0x54, 0x48, 0x31, 4, 0, 0, 0}), "critical");
    require(frame("reject-emergency") == Bytes({0x41, 0x54, 0x48, 0x31, 5, 0, 0, 0}), "reject-emergency");
    require(frame("reject-shutdown") == Bytes({0x41, 0x54, 0x48, 0x31, 6, 0, 0, 0}), "reject-shutdown");
    require(frame("reject-magic") == Bytes({0x41, 0x54, 0x48, 0x32, 0, 0, 0, 0}), "reject-magic");
    require(frame("reject-reserved") == Bytes({0x41, 0x54, 0x48, 0x31, 1, 0, 1, 0}), "reject-reserved");
    require(fixture.size() == 9, "the fixture holds the five records and the four rejects");

    // The guest reads each record as the severity it names.
    require(decode_record(frame("none")) == Severity::none, "none decodes");
    require(decode_record(frame("light")) == Severity::light, "light decodes");
    require(decode_record(frame("moderate")) == Severity::moderate, "moderate decodes");
    require(decode_record(frame("severe")) == Severity::severe, "severe decodes");
    require(decode_record(frame("critical")) == Severity::critical, "critical decodes");

    // The host's rules accept every record and refuse every `reject-` frame,
    // so the oracle above, the decoder and the fixture agree.
    for (const auto& [name, bytes] : fixture) {
        if (name.starts_with("reject-")) {
            require(!host_may_send(bytes), "the host never sends " + name);
            require(!decode_record(bytes).has_value(), "the guest refuses " + name);
        } else {
            require(host_may_send(bytes), "the host may send " + name);
            require(decode_record(bytes).has_value(), "the guest reads " + name);
        }
    }
}

// The decoder and the reader against the oracle, over every value of every
// field: nothing the host may not send is read, and nothing it may send is
// refused.
void check_every_byte() {
    for (std::size_t position = 0; position < record_size; ++position) {
        for (unsigned value = 0; value < 256; ++value) {
            Bytes frame = record_of(0);
            frame[position] = uint8_t(value);
            const bool allowed = host_may_send(frame);
            require(decode_record(frame).has_value() == allowed, "decoder against the oracle");
            RecordReader reader;
            const auto output = reader.feed(frame);
            require(output.failed == !allowed, "reader against the oracle");
            require(output.records.size() == (allowed ? 1u : 0u), "reader records against the oracle");
        }
    }
    // Android's EMERGENCY and SHUTDOWN, and every value above, are no record.
    for (unsigned severity = 5; severity < 256; ++severity) {
        require(!decode_record(record_of(uint8_t(severity))).has_value(), "no severity above CRITICAL");
    }
    require(!decode_record(Bytes{}).has_value(), "an empty record");
    Bytes shorter = record_of(0);
    shorter.pop_back();
    require(!decode_record(shorter).has_value(), "a short record");
    Bytes longer = record_of(0);
    longer.push_back(0);
    require(!decode_record(longer).has_value(), "a long record");
}

void check_reader(const std::map<std::string, Bytes>& fixture) {
    Bytes all;
    for (const char* name : {"none", "light", "severe", "critical", "severe", "none"}) {
        all.insert(all.end(), fixture.at(name).begin(), fixture.at(name).end());
    }
    const std::vector<Severity> expected{Severity::none,     Severity::light, Severity::severe,
                                         Severity::critical, Severity::severe, Severity::none};

    RecordReader whole;
    const auto together = whole.feed(all);
    require(!together.failed && together.records == expected, "whole records");

    RecordReader single;
    std::vector<Severity> seen;
    for (const auto byte : all) {
        const auto output = single.feed(std::span(&byte, 1));
        require(!output.failed, "one byte at a time");
        seen.insert(seen.end(), output.records.begin(), output.records.end());
    }
    require(seen == expected, "one byte at a time");

    RecordReader pieces;
    seen.clear();
    for (std::size_t at = 0, size = 5; at < all.size(); at += size, size = size % 11 + 3) {
        const auto piece = std::span(all).subspan(at, std::min(size, all.size() - at));
        const auto output = pieces.feed(piece);
        require(!output.failed, "uneven pieces");
        seen.insert(seen.end(), output.records.begin(), output.records.end());
    }
    require(seen == expected, "uneven pieces");

    // A record that stops half way is held, and the rest completes it.
    RecordReader split;
    require(split.feed(std::span(fixture.at("severe")).first(3)).records.empty(), "half a record is held");
    const auto rest = split.feed(std::span(fixture.at("severe")).subspan(3));
    require(!rest.failed && rest.records == std::vector<Severity>{Severity::severe}, "the rest completes it");

    // A bad byte ends the stream's use, as soon as it arrives, and the records
    // before it count. The bytes after it are dropped.
    for (const char* name : {"reject-emergency", "reject-shutdown", "reject-magic", "reject-reserved"}) {
        Bytes stream = fixture.at("light");
        stream.insert(stream.end(), fixture.at(name).begin(), fixture.at(name).end());
        stream.insert(stream.end(), fixture.at("critical").begin(), fixture.at("critical").end());
        RecordReader reader;
        const auto output = reader.feed(stream);
        require(output.failed, std::string("a stream with ") + name + " fails");
        require(output.records == std::vector<Severity>{Severity::light},
                std::string("only the record before ") + name + " counts");
    }
    // The magic is checked as it arrives: three right bytes and a wrong one.
    RecordReader early;
    const auto cut = early.feed(Bytes{'A', 'T', 'H', '2'});
    require(cut.failed && cut.records.empty(), "a wrong magic byte fails at once");

    // After a failure the owner closes the connection and resets the reader,
    // and the next connection starts clean.
    RecordReader reused;
    require(reused.feed(fixture.at("reject-magic")).failed, "the bad stream fails");
    reused.reset();
    const auto fresh = reused.feed(fixture.at("critical"));
    require(!fresh.failed && fresh.records == std::vector<Severity>{Severity::critical}, "reset starts clean");
    // A half record is dropped by a reset too.
    RecordReader abandoned;
    require(abandoned.feed(std::span(fixture.at("severe")).first(6)).records.empty(), "a held half record");
    abandoned.reset();
    const auto after = abandoned.feed(fixture.at("none"));
    require(!after.failed && after.records == std::vector<Severity>{Severity::none}, "a reset drops a half record");
}
}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: thermal_protocol_tests <thermal-golden-frames.txt>");
    const auto fixture = read_fixture(argv[1]);
    check_constants();
    check_fixture_frames(fixture);
    check_every_byte();
    check_reader(fixture);
    std::puts("thermal protocol: ok");
    return 0;
}
