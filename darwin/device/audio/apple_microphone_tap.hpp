// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "capture_ring.hpp"

#import <AVFAudio/AVFAudio.h>

#include <cstddef>
#include <cstdint>

namespace uml::audio {

/* What the microphone's input tap does with each buffer it is given, apart
 * from the engine, so that a test runs the code the engine runs.
 *
 * It converts the buffer and writes the result to the ring under the epoch
 * the buffer reached it at, leaving out what the recording must not have: the
 * audio captured before the recording's boundary. The converter keeps samples
 * between calls, so when the ring begins a recording (a new epoch) the tap
 * resets it before it converts the first buffer of that epoch; the output
 * then begins with that buffer's first sample. The frames to leave out are
 * counted once, from that buffer's capture time, and taken from the front of
 * the epoch's output: a converter emits fewer frames than it was given, and
 * holds the rest for the next call, so a boundary late in a buffer reaches
 * into the output of the next.
 *
 * receive() runs on the tap's own thread. It adds no lock, wait or allocation
 * of its own: the converter's reset takes the framework lock a conversion
 * takes, and the constructor runs one reset so that the first one on the
 * tap's thread finds the call already made once. A buffer whose time is not
 * known (no host time) has nothing left out: the boundary cannot be placed
 * in it. */
class MicrophoneTap final {
public:
    /* `converted` is the converter's output buffer, in the ring's sample format and channel count. */
    MicrophoneTap(AVAudioConverter* converter, AVAudioPCMBuffer* converted, CaptureRing& ring,
                  std::uint8_t channels, std::uint32_t outputRate);

    /* `when` is the time of the buffer's first sample; the clock is the ring's boundary's. */
    void receive(AVAudioPCMBuffer* buffer, AVAudioTime* when);

private:
    AVAudioConverter* __strong converter_;
    AVAudioPCMBuffer* __strong converted_;
    CaptureRing& ring_;
    const std::uint32_t frameBytes_;
    const std::uint32_t outputRate_;
    // The epoch of the buffers being converted, and the frames of its output that remain to be left out.
    bool started_ {};
    std::uint64_t epoch_ {};
    std::size_t leaveOut_ {};
};

} // namespace uml::audio
