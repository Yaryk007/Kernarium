// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Why a still of a platform camera did not end in a picture, as numbers and
// fixed words. The platform camera fills what it knows (camera.hpp,
// CameraStillResult::failure), the camera device adds the camera and what it
// saw of the file and tells its owner (CameraDeviceOptions::onStillFailure),
// and the runtime's session record keeps the last few (runtime/session_record.hpp).
// Nothing in it is free text: a name, a path or a message of the framework
// never enters it.

#include <cstdint>
#include <string_view>

namespace uml::platform {

enum class CameraPosition : std::uint8_t { back, front, external };

// Where a still stopped.
enum class CameraStillStage : std::uint8_t {
    // The capture did not fail: the value of a result that is a picture.
    none,
    // The platform gave no reason.
    unknown,
    // The device refused the capture before it reached the platform (a
    // concurrency or budget limit, no session, or the platform's refusal).
    refused,
    // The platform accepted the capture and could not start it, or its retry.
    notStarted,
    // The platform's framework reported an error for the capture.
    frameworkError,
    // The capture finished without an error and without a file.
    missingData,
    // The photo resolved to a size other than the one asked for, after the
    // retry.
    resolvedSize,
    // The file's frame header states a size other than the one asked for, or
    // none.
    encodedSize,
    // The file is larger than the host takes.
    fileSize,
    // The device could not put the file into the guest's buffer.
    guestBuffer,
    // The capture was not ended in time: the platform did not end it, or the
    // host's budget kept a whole request waiting until its time ran out.
    timeout,
};

// The domain of the framework's error, as one of a few words: an unknown
// domain is `other`, whatever it is called.
enum class CameraErrorDomain : std::uint8_t {
    none,
    avFoundation,
    osStatus,
    cocoa,
    posix,
    mach,
    other,
};

enum class CameraFact : std::uint8_t { unknown, no, yes };

// When a value of the platform's was read, relative to the commit of the capture session's configuration that made it
// valid: a photo output's capabilities are only known to hold once the output is attached and the configuration is
// committed.
enum class CameraReading : std::uint8_t { unknown, beforeCommit, afterCommit };

[[nodiscard]] constexpr std::string_view readingName(CameraReading reading) noexcept
{
    switch (reading) {
    case CameraReading::unknown: break;
    case CameraReading::beforeCommit: return "before-commit";
    case CameraReading::afterCommit: return "after-commit";
    }
    return "-";
}

struct CameraStillFailure {
    CameraStillStage stage {CameraStillStage::none};
    // The camera's index as the guest sees it, and where it is mounted: filled
    // by the device.
    std::uint32_t camera {};
    CameraPosition position {CameraPosition::back};
    // The JPEG size asked for (zero: a capture with no JPEG), the size the
    // photo resolved to and the size its frame header states; zero for what is
    // not known.
    std::uint32_t askedWidth {};
    std::uint32_t askedHeight {};
    std::uint32_t resolvedWidth {};
    std::uint32_t resolvedHeight {};
    std::uint32_t encodedWidth {};
    std::uint32_t encodedHeight {};
    // Whether the photo output turns its stills to an earlier model's sensor
    // orientation (compensation), and whether that was on for the capture.
    CameraFact compensationSupported {};
    CameraFact compensationEnabled {};
    // When those two were read: after the commit of the configuration for a capture the platform refused or finished.
    CameraReading compensationReading {};
    // The Exif orientation (1 to 8) of the photo the host's output gave for the
    // capture; zero where it gave none or none was read.
    std::uint8_t hostExifOrientation {};
    // The framework's error, when it reported one.
    CameraErrorDomain errorDomain {CameraErrorDomain::none};
    std::int32_t errorCode {};
    friend bool operator==(const CameraStillFailure&, const CameraStillFailure&) = default;
};

[[nodiscard]] constexpr std::string_view stillStageName(CameraStillStage stage) noexcept
{
    switch (stage) {
    case CameraStillStage::none: return "none";
    case CameraStillStage::unknown: break;
    case CameraStillStage::refused: return "refused";
    case CameraStillStage::notStarted: return "not-started";
    case CameraStillStage::frameworkError: return "framework-error";
    case CameraStillStage::missingData: return "missing-data";
    case CameraStillStage::resolvedSize: return "resolved-size";
    case CameraStillStage::encodedSize: return "encoded-size";
    case CameraStillStage::fileSize: return "file-size";
    case CameraStillStage::guestBuffer: return "guest-buffer";
    case CameraStillStage::timeout: return "timeout";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view errorDomainName(CameraErrorDomain domain) noexcept
{
    switch (domain) {
    case CameraErrorDomain::none: return "none";
    case CameraErrorDomain::avFoundation: return "avfoundation";
    case CameraErrorDomain::osStatus: return "osstatus";
    case CameraErrorDomain::cocoa: return "cocoa";
    case CameraErrorDomain::posix: return "posix";
    case CameraErrorDomain::mach: return "mach";
    case CameraErrorDomain::other: break;
    }
    return "other";
}

[[nodiscard]] constexpr std::string_view factName(CameraFact fact) noexcept
{
    switch (fact) {
    case CameraFact::unknown: break;
    case CameraFact::no: return "no";
    case CameraFact::yes: return "yes";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view cameraPositionName(CameraPosition position) noexcept
{
    switch (position) {
    case CameraPosition::back: return "back";
    case CameraPosition::front: return "front";
    case CameraPosition::external: break;
    }
    return "external";
}

// The word for the domain a framework names (NSError's domain string): the few
// it knows, `other` for the rest, so no framework text is kept.
[[nodiscard]] constexpr CameraErrorDomain errorDomainOf(std::string_view domain) noexcept
{
    if (domain == "AVFoundationErrorDomain") return CameraErrorDomain::avFoundation;
    if (domain == "NSOSStatusErrorDomain") return CameraErrorDomain::osStatus;
    if (domain == "NSCocoaErrorDomain") return CameraErrorDomain::cocoa;
    if (domain == "NSPOSIXErrorDomain") return CameraErrorDomain::posix;
    if (domain == "NSMachErrorDomain") return CameraErrorDomain::mach;
    return CameraErrorDomain::other;
}

} // namespace uml::platform
