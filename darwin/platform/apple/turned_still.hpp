// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A still of a camera whose pictures the host presents half around (CameraInfo::presentationTurn): the uncompressed
// photo turned and encoded as the JPEG the guest takes. The platform's own JPEG cannot be turned, because a JPEG is a
// coded picture; the pixels are therefore asked for uncompressed, turned while they are copied into the hardware
// encoder's own frame, and encoded at the guest's quality. The file carries no colour profile, as the platform's own
// files carried none (a reader takes sRGB), so a photo is delivered only while its colours are verified to be sRGB.

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace uml::platform {

// Why a photo could not be turned and encoded. Fixed words, as the rest of the still's failures: no framework message
// or value of the photo enters them.
enum class TurnedStillFailure : std::uint8_t {
    none,
    noPixels,           // the photo has no pixel buffer
    unsupportedFormat,  // the pixels are not two planes of full-range 4:2:0, or a side is odd
    notBt601,           // the pixels state a matrix a JPEG cannot carry (JFIF is BT.601)
    notSrgb,            // the pixels state primaries, a transfer or a colour space that is not sRGB: an untagged file would show wrong colours
    unknownColours,     // the pixels state no colours, and the capture is not known to be in sRGB
    noEncoder,          // the host has no hardware JPEG encoder that takes this picture
    timedOut,           // the encoder did not finish in time
    failed,             // the encoder reported an error, or its image was not a JPEG
};

[[nodiscard]] constexpr std::string_view turnedStillFailureName(TurnedStillFailure failure) noexcept
{
    switch (failure) {
    case TurnedStillFailure::none: return "none";
    case TurnedStillFailure::noPixels: return "the photo has no pixels";
    case TurnedStillFailure::unsupportedFormat: return "the photo is not full-range 4:2:0 of even size";
    case TurnedStillFailure::notBt601: return "the photo's YCbCr matrix is not BT.601";
    case TurnedStillFailure::notSrgb: return "the photo's colours are not sRGB";
    case TurnedStillFailure::unknownColours: return "the photo states no colours and its capture was not held in sRGB";
    case TurnedStillFailure::noEncoder: return "no hardware JPEG encoder takes the photo";
    case TurnedStillFailure::timedOut: return "the JPEG encoder did not finish in time";
    case TurnedStillFailure::failed: return "the JPEG encoder failed or its file passed the size bound";
    }
    return "unknown";
}

struct TurnedStill {
    // A JFIF image of the photo's size, turned half around, with no colour profile; empty on failure.
    std::vector<std::uint8_t> jpeg;
    TurnedStillFailure failure {TurnedStillFailure::none};
};

// Turns `photo` (full-range 4:2:0 in two planes, with BT.601 or no matrix stated) half around and encodes it at `jpegQuality`
// (1 to 100) in the hardware encoder. The file has no colour profile, so what a reader takes of it is sRGB, and the photo is
// delivered only when that is what it is: its primaries, transfer, colour space object and ICC profile, those it carries,
// are BT.709 or sRGB's (the primaries of both, the transfer of either), and a photo that carries none of a statement is
// taken as sRGB only if `captureIsSrgb` (the capture's colour space is the device's sRGB, selected and still in effect).
// The file may not pass `maximumBytes` (zero: no bound): the encoder fails a larger one before its bytes are copied out,
// so none of the host's copies of a file that is not passed on is ever made. The photo is only read. The encoder lives for
// this call.
[[nodiscard]] TurnedStill turnedStillOf(CVPixelBufferRef photo, std::uint32_t jpegQuality, std::uint64_t maximumBytes, bool captureIsSrgb);

} // namespace uml::platform
