// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host side of the guest debug program's records: the same header
 * (tests/integration/aot/guest_records.hpp) builds here, and
 * test_aot_records.py reads what it prints with the one reader (records.py)
 * and compares it with what that reader would write for the same inputs.
 *
 *   resolve <maps file> <hex value>...   a record "probe" with one field
 *                                        "value" for each: where the value
 *                                        points in the maps text
 *   text <hex bytes>...                  a probe with the text of the bytes
 *   named <hex bytes> <hex offset>       a probe with a place in a named
 *                                        mapping
 *   number <hex>                         a probe with an unsigned number
 *   integer <decimal>                    a probe with a signed number
 *   regs <maps file> <hex> <hex>         a register record whose x06 and x07
 *                                        hold the two values, resolved in the
 *                                        maps text, and the rest zero
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "guest_records.hpp"

namespace {

void write_to_stdout(void *, const char *data, size_t length)
{
    std::fwrite(data, 1, length, stdout);
}

const aot_records::Sink sink{write_to_stdout, nullptr};

std::string bytes_of(const char *hex)
{
    std::string bytes;
    const size_t length = std::strlen(hex);
    for (size_t i = 0; i + 1 < length; i += 2) {
        bytes.push_back(static_cast<char>(std::strtoul(std::string(hex + i, 2).c_str(), nullptr, 16)));
    }
    return bytes;
}

bool read_maps(const char *path, std::string &maps)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    maps.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

aot_records::Value place_of(const std::string &maps, uint64_t value)
{
    aot_records::Value found;
    if (aot_records::resolve(maps.c_str(), value, found)) {
        return found;
    }
    return aot_records::number_value(value);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    const std::string command = argv[1];
    if (command == "resolve" && argc >= 3) {
        std::string maps;
        if (!read_maps(argv[2], maps)) {
            return 2;
        }
        for (int i = 3; i < argc; ++i) {
            aot_records::Record(sink, "probe").value("value", place_of(maps, std::strtoull(argv[i], nullptr, 16)));
        }
    } else if (command == "text") {
        for (int i = 2; i < argc; ++i) {
            const std::string bytes = bytes_of(argv[i]);
            aot_records::Record(sink, "probe").text("value", bytes.data(), bytes.size());
        }
    } else if (command == "named" && argc == 4) {
        const std::string bytes = bytes_of(argv[2]);
        aot_records::Record(sink, "probe").value(
            "value", aot_records::named_value(bytes.data(), bytes.size(), std::strtoull(argv[3], nullptr, 16)));
    } else if (command == "number" && argc == 3) {
        aot_records::Record(sink, "probe").number("value", std::strtoull(argv[2], nullptr, 16));
    } else if (command == "integer" && argc == 3) {
        aot_records::Record(sink, "probe").integer("value", std::strtoll(argv[2], nullptr, 10));
    } else if (command == "regs" && argc == 5) {
        std::string maps;
        if (!read_maps(argv[2], maps)) {
            return 2;
        }
        aot_records::Record record(sink, "regs");
        record.text("tag", "crc32");
        for (int i = 0; i < 31; ++i) {
            char label[4] = {'x', static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), 0};
            if (i == 6 || i == 7) {
                record.value(label, place_of(maps, std::strtoull(argv[i == 6 ? 3 : 4], nullptr, 16)));
            } else {
                record.number(label, 0);
            }
        }
        record.number("sp", 0);
        record.value("pc", aot_records::named_value("libz.so", 7, 0x12190));
        record.number("nzcv", 0);
    } else {
        return 2;
    }
    return 0;
}
