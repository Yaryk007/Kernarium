// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "block/android_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace kernarium::identity {

/* The installation identity an Android guest is given (runtime API 42): the
 * serial number and the eth0 address that tell one installation from another.
 * darwin/device/block/README.md owns the identity file and its rules; this is
 * how a boot request states the values it must have, and how the runtime
 * resolves the rest. */

/* What a request states before the two values were added to it. */
inline constexpr std::size_t bootSizeBeforeSerial =
    offsetof(kernarium_runtime_boot, android_serial_number);
inline constexpr std::size_t bootSizeBeforeEthernetMac =
    offsetof(kernarium_runtime_boot, android_ethernet_mac);

/* The values a request states, and whether it stated a value that is not
 * one. */
struct Request {
    uml::android::InstallationIdentity stated;
    bool invalid {};
};

/* A text the application states, at most `limit` characters; false for one
 * longer, so a string that is not terminated is never read past its limit. */
[[nodiscard]] inline bool readText(
    const char* text, std::size_t limit, std::string& value)
{
    const std::size_t length = ::strnlen(text, limit + 1U);
    if (length > limit)
        return false;
    value.assign(text, length);
    return true;
}

/* The identity fields of `boot`, read only as far as its struct_size covers
 * them. A value that is not valid sets `invalid`: the run fails over it,
 * because a caller that states a value wants exactly that one. */
[[nodiscard]] inline Request readRequest(const kernarium_runtime_boot& boot)
{
    Request request;
    std::string text;
    if (boot.struct_size >= bootSizeBeforeSerial + sizeof(boot.android_serial_number) &&
        boot.android_serial_number != nullptr) {
        if (readText(boot.android_serial_number, 20, text) &&
            uml::android::validSerial(text))
            request.stated.serial = text;
        else
            request.invalid = true;
    }
    if (boot.struct_size >=
            bootSizeBeforeEthernetMac + sizeof(boot.android_ethernet_mac) &&
        boot.android_ethernet_mac != nullptr) {
        std::optional<uml::android::MacAddress> mac;
        if (readText(boot.android_ethernet_mac, 17, text))
            mac = uml::android::parseMac(text);
        if (mac && uml::android::validEthernetMac(*mac))
            request.stated.ethernetMac = mac;
        else
            request.invalid = true;
    }
    return request;
}

/* What the runtime asks of the installation (uml::android::prepareInstallation,
 * which locks it, prepares the data disk and resolves the identity): the
 * request's stated values, or else the file beside `dataDisk`, which a new data
 * disk replaces. The host's own messages never contain the values: the outcome
 * is what the caller may say. */
[[nodiscard]] inline uml::android::IdentityRequest identityRequest(
    const Request& request, const std::filesystem::path& dataDisk)
{
    return {
        .file = uml::android::identityFile(dataDisk),
        .stated = request.stated,
    };
}

} // namespace kernarium::identity
