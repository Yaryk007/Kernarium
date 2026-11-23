// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The records the guest debug program (aot_guest_debug.cpp) prints, and the
 * way it names where a value points. Header only and freestanding (no libc
 * call): the program is built with no headers of its own, and the host
 * harness of the tooling rows (tests/tooling/arm64/aot_records_harness.cpp)
 * builds the same code. records.py is the one reader.
 *
 * A record is a kind and its fields, in the order its schema (records.py)
 * lists them. Every field is a line of its own:
 *
 *   AOT <TAB> kind <TAB> field <TAB> type <TAB> value...
 *
 * with the types
 *
 *   n  an unsigned number: lowercase hexadecimal, no leading zeros
 *   i  a signed number: decimal
 *   t  text: every byte outside [A-Za-z0-9._+/:[]-] is written %xx
 *      (lowercase hexadecimal), so that a space, '=', a tab, a line end, '%'
 *      and every non-ASCII byte are escaped, and nothing in a value can be
 *      taken for the separator or the end of the line
 *   m  a place in a named mapping: its name as text, <TAB> the offset in hex
 *   u  a place in an unnamed mapping: its size in hex, <TAB> its protection
 *      (three characters), <TAB> its place among the unnamed mappings of that
 *      size and protection (lowest address first) in decimal, <TAB> how many
 *      there are in decimal, <TAB> the offset in hex
 *
 * The place and the count are found by counting, so no number of mappings of
 * any shape can exhaust the program.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aot_records {

// ---- the text of /proc/<pid>/maps ------------------------------------------

inline uint64_t parse_hex(const char *&at)
{
    uint64_t value = 0;
    for (;; ++at) {
        const char c = *at;
        if (c >= '0' && c <= '9') {
            value = value * 16 + static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = value * 16 + static_cast<uint64_t>(c - 'a' + 10);
        } else {
            return value;
        }
    }
}

struct Line {
    uint64_t low;
    uint64_t high;
    uint64_t offset;
    const char *protection;
    const char *name;
    const char *name_end;
};

// One line of the maps text, or false at its end.
inline bool read_line(const char *&at, Line &line)
{
    if (*at == 0) {
        return false;
    }
    line.low = parse_hex(at);
    if (*at == '-') {
        ++at;
    }
    line.high = parse_hex(at);
    while (*at == ' ') {
        ++at;
    }
    line.protection = at;
    while (*at != ' ' && *at != '\n' && *at != 0) {
        ++at;
    }
    if (*at == ' ') {
        ++at;
    }
    line.offset = parse_hex(at);
    for (int field = 0; field < 2 && *at != 0; ++field) {  // device, inode
        while (*at == ' ') {
            ++at;
        }
        while (*at != ' ' && *at != '\n' && *at != 0) {
            ++at;
        }
    }
    while (*at == ' ') {
        ++at;
    }
    line.name = at;
    while (*at != '\n' && *at != 0) {
        ++at;
    }
    line.name_end = at;
    if (*at == '\n') {
        ++at;
    }
    return true;
}

inline bool same_protection(const char *a, const char *b)
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == 0) {
            return true;
        }
    }
    return true;
}

// ---- a value ---------------------------------------------------------------

// A number, a place in a named mapping (a file by its base name), or a place
// in an unnamed one.
struct Value {
    enum Kind { kNumber, kNamed, kUnnamed };
    Kind kind = kNumber;
    uint64_t number = 0;  // the number; the offset of a place
    const char *name = nullptr;  // kNamed: not NUL-terminated
    size_t name_length = 0;
    uint64_t size = 0;  // kUnnamed: the shape, the place and the count
    char protection[3] = {'-', '-', '-'};
    uint64_t ordinal = 0;
    uint64_t count = 0;
};

inline Value number_value(uint64_t number)
{
    Value value;
    value.number = number;
    return value;
}

inline Value named_value(const char *name, size_t length, uint64_t offset)
{
    Value value;
    value.kind = Value::kNamed;
    value.number = offset;
    value.name = name;
    value.name_length = length;
    return value;
}

// Where `value` points in the maps text, if into a mapping: a file by its base
// name and the offset in the file, a named mapping by its name and the offset
// in it, an unnamed mapping by its shape, its place among that shape, how many
// there are, and the offset in it.
inline bool resolve(const char *maps, uint64_t value, Value &out)
{
    const char *at = maps;
    Line line{};
    bool found = false;
    while (read_line(at, line)) {
        if (value >= line.low && value < line.high) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    if (line.name != line.name_end && *line.name == '/') {
        const char *base = line.name;
        for (const char *c = line.name; c < line.name_end; ++c) {
            if (*c == '/') {
                base = c + 1;
            }
        }
        out = named_value(base, static_cast<size_t>(line.name_end - base), line.offset + value - line.low);
    } else if (line.name != line.name_end) {
        out = named_value(line.name, static_cast<size_t>(line.name_end - line.name), value - line.low);
    } else {
        uint64_t ordinal = 0;
        uint64_t count = 0;
        at = maps;
        Line other{};
        while (read_line(at, other)) {
            if (other.name != other.name_end || other.high - other.low != line.high - line.low ||
                !same_protection(other.protection, line.protection)) {
                continue;
            }
            if (other.low == line.low) {
                ordinal = count;
            }
            ++count;
        }
        out = Value{};
        out.kind = Value::kUnnamed;
        out.number = value - line.low;
        out.size = line.high - line.low;
        for (int i = 0; i < 3; ++i) {
            out.protection[i] = line.protection[i];
        }
        out.ordinal = ordinal;
        out.count = count;
    }
    return true;
}

// ---- the lines -------------------------------------------------------------

struct Sink {
    void (*write)(void *context, const char *data, size_t length);
    void *context;
};

// One line, buffered, ended and written by end().
class LineWriter {
public:
    explicit LineWriter(const Sink &sink) : sink_(sink) {}

    void raw(char c)
    {
        if (length_ == sizeof(buffer_)) {
            flush();
        }
        buffer_[length_++] = c;
    }

    void raw(const char *text)
    {
        for (; *text != 0; ++text) {
            raw(*text);
        }
    }

    void tab() { raw('\t'); }

    // Text: every byte outside the safe set is %xx.
    void escaped(const char *text, size_t length)
    {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '.' || c == '_' || c == '+' || c == '-' || c == '/' || c == ':' || c == '[' ||
                              c == ']';
            if (safe) {
                raw(static_cast<char>(c));
            } else {
                raw('%');
                raw(digits[c >> 4]);
                raw(digits[c & 15]);
            }
        }
    }

    void hex(uint64_t value)
    {
        char reversed[16];
        int count = 0;
        do {
            const unsigned digit = static_cast<unsigned>(value & 15);
            reversed[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            value >>= 4;
        } while (value != 0);
        while (count > 0) {
            raw(reversed[--count]);
        }
    }

    void decimal(uint64_t value)
    {
        char reversed[20];
        int count = 0;
        do {
            reversed[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            raw(reversed[--count]);
        }
    }

    void end()
    {
        raw('\n');
        flush();
    }

private:
    void flush()
    {
        if (length_ != 0) {
            sink_.write(sink_.context, buffer_, length_);
            length_ = 0;
        }
    }

    Sink sink_;
    char buffer_[256];
    size_t length_ = 0;
};

inline size_t length_of(const char *text)
{
    size_t length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

// The fields of one record, a line each. The kind and the field names are
// identifiers of the schema in records.py.
class Record {
public:
    Record(const Sink &sink, const char *kind) : sink_(sink), kind_(kind) {}

    void number(const char *field, uint64_t value)
    {
        LineWriter line = begin(field, 'n');
        line.hex(value);
        line.end();
    }

    void integer(const char *field, int64_t value)
    {
        LineWriter line = begin(field, 'i');
        if (value < 0) {
            line.raw('-');
            line.decimal(0 - static_cast<uint64_t>(value));
        } else {
            line.decimal(static_cast<uint64_t>(value));
        }
        line.end();
    }

    void text(const char *field, const char *text, size_t length)
    {
        LineWriter line = begin(field, 't');
        line.escaped(text, length);
        line.end();
    }

    void text(const char *field, const char *text) { this->text(field, text, length_of(text)); }

    void value(const char *field, const Value &value)
    {
        switch (value.kind) {
        case Value::kNumber: {
            number(field, value.number);
            break;
        }
        case Value::kNamed: {
            LineWriter line = begin(field, 'm');
            line.escaped(value.name, value.name_length);
            line.tab();
            line.hex(value.number);
            line.end();
            break;
        }
        case Value::kUnnamed: {
            LineWriter line = begin(field, 'u');
            line.hex(value.size);
            line.tab();
            for (int i = 0; i < 3; ++i) {
                line.raw(value.protection[i]);
            }
            line.tab();
            line.decimal(value.ordinal);
            line.tab();
            line.decimal(value.count);
            line.tab();
            line.hex(value.number);
            line.end();
            break;
        }
        }
    }

private:
    LineWriter begin(const char *field, char type)
    {
        LineWriter line(sink_);
        line.raw("AOT");
        line.tab();
        line.raw(kind_);
        line.tab();
        line.raw(field);
        line.tab();
        line.raw(type);
        line.tab();
        return line;
    }

    Sink sink_;
    const char *kind_;
};

}  // namespace aot_records
