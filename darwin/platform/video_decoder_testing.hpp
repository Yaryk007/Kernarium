// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_decoder.hpp"

#if UML_VIDEO_DECODER_TESTING
namespace uml::platform::testing {

// Available only from the separately compiled unit-test variant of the Apple
// decoder. The next `count` pictures `decoder` decodes find the VideoToolbox
// session invalid, as the system leaves it when an app returns from the
// background, so that the recovery path runs on hardware that does not fail
// on demand. `decoder` must come from `makeVideoDecoder()` of that variant.
void loseSession(VideoDecoder& decoder, unsigned count);

// The next picture's output reports `status`, an error of the stream and not
// of the session, as the hardware does for data it cannot decode.
void failOutput(VideoDecoder& decoder, std::int32_t status);

} // namespace uml::platform::testing
#endif
