// SPDX-License-Identifier: Apache-2.0
//
// The NMEA stream of the host's virtual receiver (/dev/gnss0; the contract is
// darwin/device/gnss/README.md, "The epoch"): bytes in, epochs out. An epoch is
// the six sentences the host writes each period, GGA, RMC, GSA, GST, VTG and
// the proprietary $PKRNF, ended by CR LF.
//
// The parser trusts nothing it reads. A sentence needs its '$', a checksum that
// matches and fields that parse, or it is dropped; an epoch whose position does
// not parse reports no fix. Where GGA and $PKRNF disagree about what kind of
// fix this is, the less precise reading wins: a position is precise only when
// both say so. The header has no Android dependency, so a host test compiles it
// against the host writer's output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace andarium::gnss::nmea {

// The most sentences an epoch in progress may hold. The host writes six; a
// stream that never ends an epoch (no GGA after the first, no $PKRNF) is not
// allowed to grow the epoch without bound: past this the epoch is dropped.
inline constexpr std::size_t kMaxEpochSentences = 16;

enum class FixClass { none, precise, approximate };

// What one epoch says about where the receiver is. Every optional field is
// absent when the host did not know it; none is ever zero for "unknown".
struct Position {
    FixClass fix_class = FixClass::none;
    // Degrees, WGS 84. Meaningful only when `fix_class` is not `none`.
    double latitude = 0;
    double longitude = 0;
    // The fix's own time, in milliseconds since 1970-01-01 UTC.
    int64_t utc_ms = 0;
    // Meters above sea level (GGA), and the geoid separation: the ellipsoidal
    // altitude Android calls an altitude is the sum of the two.
    std::optional<double> altitude;
    std::optional<double> geoid_separation;
    // Meters per second, and degrees clockwise from true north (RMC).
    std::optional<double> speed;
    std::optional<double> course;
    // As the host reported them in $PKRNF: meters, meters, meters per second
    // and degrees.
    std::optional<double> horizontal_accuracy;
    std::optional<double> vertical_accuracy;
    std::optional<double> speed_accuracy;
    std::optional<double> course_accuracy;
};

struct Epoch {
    Position position;
    // The sentences as they arrived, each ended by CR LF, in order.
    std::vector<std::string> sentences;
    // The time the sentences carry (RMC), also when there is no fix; 0 when
    // the epoch has no readable RMC.
    int64_t utc_ms = 0;
};

namespace detail {

inline int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

inline bool all_digits(std::string_view text) {
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

inline int digits_value(std::string_view text) {
    int value = 0;
    for (const char character : text) value = value * 10 + (character - '0');
    return value;
}

// Days from 1970-01-01 to the given day of the proleptic Gregorian calendar,
// after Howard Hinnant's days_from_civil.
inline int64_t days_from_civil(int64_t year, int month, int day) {
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

inline std::vector<std::string_view> split(std::string_view body) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = body.find(',', start);
        if (comma == std::string_view::npos) {
            fields.push_back(body.substr(start));
            return fields;
        }
        fields.push_back(body.substr(start, comma - start));
        start = comma + 1;
    }
}

inline std::string_view field(const std::vector<std::string_view>& fields, std::size_t index) {
    return index < fields.size() ? fields[index] : std::string_view();
}

}  // namespace detail

// The XOR of the characters of `body`, which sits between '$' and '*'.
inline uint8_t checksum(std::string_view body) {
    uint8_t value = 0;
    for (const char character : body) value ^= static_cast<uint8_t>(character);
    return value;
}

// "$" body "*HH\r\n".
inline std::string sentence_text(std::string_view body) {
    static constexpr char hexadecimal[] = "0123456789ABCDEF";
    const uint8_t sum = checksum(body);
    std::string text;
    text.reserve(body.size() + 7);
    text.push_back('$');
    text.append(body);
    text.push_back('*');
    text.push_back(hexadecimal[sum >> 4]);
    text.push_back(hexadecimal[sum & 0x0F]);
    text += "\r\n";
    return text;
}

// The body of a well formed sentence ('$' ... '*HH' and an optional CR LF) whose
// checksum matches, or nothing.
inline std::optional<std::string_view> sentence_body(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (line.size() < 4 || line.front() != '$') return std::nullopt;
    const std::size_t star = line.size() - 3;
    if (line[star] != '*') return std::nullopt;
    const int high = detail::hex_value(line[star + 1]);
    const int low = detail::hex_value(line[star + 2]);
    if (high < 0 || low < 0) return std::nullopt;
    const std::string_view body = line.substr(1, star - 1);
    if (checksum(body) != static_cast<uint8_t>(high * 16 + low)) return std::nullopt;
    return body;
}

// A number in fixed notation with an optional sign, such as "-12.50". Anything
// else, an exponent included, is not one.
inline std::optional<double> number(std::string_view text) {
    std::size_t at = 0;
    bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        at = 1;
    }
    int64_t mantissa = 0;
    int digits = 0;
    int fraction_digits = -1;
    for (; at < text.size(); ++at) {
        const char character = text[at];
        if (character == '.') {
            if (fraction_digits >= 0) return std::nullopt;
            fraction_digits = 0;
            continue;
        }
        if (character < '0' || character > '9' || digits >= 17) return std::nullopt;
        mantissa = mantissa * 10 + (character - '0');
        ++digits;
        if (fraction_digits >= 0) ++fraction_digits;
    }
    if (digits == 0) return std::nullopt;
    double value = static_cast<double>(mantissa);
    for (int shift = 0; shift < fraction_digits; ++shift) value /= 10.0;
    return negative ? -value : value;
}

// "ddmm.mmmm" (latitude) or "dddmm.mmmm" (longitude) with its hemisphere
// letter, as signed degrees.
inline std::optional<double> coordinate(std::string_view text, std::string_view hemisphere,
                                        double limit, char positive, char negative) {
    const auto packed = number(text);
    if (!packed || *packed < 0 || hemisphere.size() != 1 ||
        (hemisphere[0] != positive && hemisphere[0] != negative)) {
        return std::nullopt;
    }
    const double degrees = static_cast<double>(static_cast<int64_t>(*packed / 100.0));
    const double minutes = *packed - degrees * 100.0;
    if (minutes < 0 || minutes >= 60.0) return std::nullopt;
    const double value = degrees + minutes / 60.0;
    if (value > limit) return std::nullopt;
    return hemisphere[0] == negative ? -value : value;
}

// "hhmmss" or "hhmmss.ss" as milliseconds since midnight.
inline std::optional<int64_t> time_of_day_ms(std::string_view text) {
    if (text.size() < 6 || !detail::all_digits(text.substr(0, 6))) return std::nullopt;
    const int hour = detail::digits_value(text.substr(0, 2));
    const int minute = detail::digits_value(text.substr(2, 2));
    const int second = detail::digits_value(text.substr(4, 2));
    if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
    int64_t millis = 0;
    if (text.size() > 6) {
        if (text[6] != '.' || text.size() == 7 || !detail::all_digits(text.substr(7))) {
            return std::nullopt;
        }
        int64_t scale = 100;
        for (std::size_t at = 7; at < text.size() && scale > 0; ++at, scale /= 10) {
            millis += (text[at] - '0') * scale;
        }
    }
    return ((hour * 60 + minute) * 60 + second) * 1000 + millis;
}

// "ddmmyy" (the century is the 2000s) as days since 1970-01-01.
inline std::optional<int64_t> date_days(std::string_view text) {
    if (text.size() != 6 || !detail::all_digits(text)) return std::nullopt;
    const int day = detail::digits_value(text.substr(0, 2));
    const int month = detail::digits_value(text.substr(2, 2));
    const int year = 2000 + detail::digits_value(text.substr(4, 2));
    if (day < 1 || day > 31 || month < 1 || month > 12) return std::nullopt;
    return detail::days_from_civil(year, month, day);
}

// Splits the receiver's byte stream into sentences and groups them into
// epochs. An epoch ends at $PKRNF, or at the next GGA when the receiver sends
// no $PKRNF.
class Parser {
public:
    // The longest sentence kept. NMEA 0183 allows 82 characters; the longer
    // line is garbage and is skipped to the next '$'.
    static constexpr std::size_t maximum_sentence = 120;

    // Appends bytes the receiver wrote, in any chunking, and returns the epochs
    // they completed.
    std::vector<Epoch> feed(std::string_view bytes) {
        std::vector<Epoch> epochs;
        for (const char character : bytes) {
            if (character == '$') {
                line_.assign(1, '$');
                in_sentence_ = true;
            } else if (in_sentence_) {
                line_.push_back(character);
                if (character == '\n') {
                    in_sentence_ = false;
                    accept(line_, epochs);
                } else if (line_.size() > maximum_sentence) {
                    in_sentence_ = false;
                    ++rejected_;
                }
            }
        }
        return epochs;
    }

    // Forgets the sentence and the epoch in progress.
    void reset() {
        line_.clear();
        in_sentence_ = false;
        pending_ = Pending();
    }

    // The sentences dropped for a bad checksum, shape or length.
    uint64_t rejected() const { return rejected_; }

    // The epochs dropped for holding more than kMaxEpochSentences.
    uint64_t overflowed() const { return overflowed_; }

private:
    struct Pending {
        std::vector<std::string> sentences;
        std::vector<std::string> gga;  // the fields of each sentence kind, once seen
        std::vector<std::string> rmc;
        std::vector<std::string> pkrnf;
        bool has_gga = false;
        bool has_rmc = false;
        bool has_pkrnf = false;
    };

    static std::vector<std::string> own(const std::vector<std::string_view>& fields) {
        return std::vector<std::string>(fields.begin(), fields.end());
    }

    void accept(const std::string& line, std::vector<Epoch>& epochs) {
        const auto body = sentence_body(line);
        if (!body) {
            ++rejected_;
            return;
        }
        std::string text(line);
        // The checksum was verified on the line without its terminator; keep the
        // sentence exactly as received, and give it its CR LF when it had a bare LF.
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        text += "\r\n";
        const auto fields = detail::split(*body);
        const std::string_view id = fields.empty() ? std::string_view() : fields[0];
        const bool proprietary = id == "PKRNF";
        const std::string_view type = id.size() == 5 && id[0] != 'P' ? id.substr(2) : std::string_view();
        if (type == "GGA" && pending_.has_gga) finish(epochs);
        if (pending_.sentences.size() >= kMaxEpochSentences) {
            pending_ = Pending();
            ++overflowed_;
        }
        pending_.sentences.push_back(std::move(text));
        if (type == "GGA") {
            pending_.gga = own(fields);
            pending_.has_gga = true;
        } else if (type == "RMC") {
            pending_.rmc = own(fields);
            pending_.has_rmc = true;
        } else if (proprietary) {
            pending_.pkrnf = own(fields);
            pending_.has_pkrnf = true;
            finish(epochs);
        }
    }

    // Turns the sentences gathered so far into an epoch, or drops them when
    // they hold no GGA (the stream began in the middle of one).
    void finish(std::vector<Epoch>& epochs) {
        Pending done = std::move(pending_);
        pending_ = Pending();
        if (!done.has_gga) return;
        Epoch epoch;
        epoch.sentences = std::move(done.sentences);
        const auto view = [](const std::vector<std::string>& fields) {
            return std::vector<std::string_view>(fields.begin(), fields.end());
        };
        const auto gga = view(done.gga);
        const auto rmc = view(done.rmc);
        const auto pkrnf = view(done.pkrnf);

        std::optional<int64_t> clock;
        std::optional<int64_t> day;
        if (done.has_rmc) {
            clock = time_of_day_ms(detail::field(rmc, 1));
            day = date_days(detail::field(rmc, 9));
            if (clock && day) epoch.utc_ms = *day * 86'400'000 + *clock;
        }

        // What GGA says: 1 a satellite fix, 6 an estimate, anything else none.
        FixClass from_gga = FixClass::none;
        const std::string_view quality = detail::field(gga, 6);
        if (quality == "1") from_gga = FixClass::precise;
        else if (quality == "6") from_gga = FixClass::approximate;
        // What $PKRNF says; a missing or unreadable one has no opinion.
        std::optional<FixClass> from_proprietary;
        if (done.has_pkrnf) {
            const std::string_view letter = detail::field(pkrnf, 1);
            if (letter == "P") from_proprietary = FixClass::precise;
            else if (letter == "A") from_proprietary = FixClass::approximate;
            else if (letter == "N") from_proprietary = FixClass::none;
        }

        Position position;
        FixClass merged = from_gga;
        if (from_gga != FixClass::none && from_proprietary) {
            if (*from_proprietary == FixClass::none) merged = FixClass::none;
            else if (*from_proprietary == FixClass::approximate) merged = FixClass::approximate;
        }
        const auto latitude = coordinate(detail::field(gga, 2), detail::field(gga, 3), 90.0, 'N', 'S');
        const auto longitude = coordinate(detail::field(gga, 4), detail::field(gga, 5), 180.0, 'E', 'W');
        const bool rmc_valid = done.has_rmc && detail::field(rmc, 2) == "A";
        if (merged == FixClass::none || !latitude || !longitude || !rmc_valid || !clock || !day) {
            epoch.position = position;  // no fix
            epochs.push_back(std::move(epoch));
            return;
        }
        position.fix_class = merged;
        position.latitude = *latitude;
        position.longitude = *longitude;
        position.utc_ms = epoch.utc_ms;
        if (detail::field(gga, 10) == "M") position.altitude = number(detail::field(gga, 9));
        if (detail::field(gga, 12) == "M") position.geoid_separation = number(detail::field(gga, 11));
        if (const auto knots = number(detail::field(rmc, 7))) position.speed = *knots * (1852.0 / 3600.0);
        position.course = number(detail::field(rmc, 8));
        if (done.has_pkrnf) {
            position.horizontal_accuracy = number(detail::field(pkrnf, 2));
            position.vertical_accuracy = number(detail::field(pkrnf, 3));
            position.speed_accuracy = number(detail::field(pkrnf, 4));
            position.course_accuracy = number(detail::field(pkrnf, 5));
        }
        epoch.position = position;
        epochs.push_back(std::move(epoch));
    }

    std::string line_;
    bool in_sentence_ = false;
    Pending pending_;
    uint64_t rejected_ = 0;
    uint64_t overflowed_ = 0;
};

}  // namespace andarium::gnss::nmea
