// SPDX-License-Identifier: GPL-2.0-or-later

#include "apple_microphone_tap.hpp"

#include <algorithm>
#include <span>

namespace uml::audio {

MicrophoneTap::MicrophoneTap(AVAudioConverter* converter, AVAudioPCMBuffer* converted, CaptureRing& ring,
                             std::uint8_t channels, std::uint32_t outputRate)
    : converter_(converter)
    , converted_(converted)
    , ring_(ring)
    , frameBytes_(channels * sizeof(std::int16_t))
    , outputRate_(outputRate)
{
    // The first reset of a process does work a later one does not (the call's own lookup): do it here, off the tap's
    // thread. A converter that has not run holds nothing, so this drops nothing.
    [converter_ reset];
}

void MicrophoneTap::receive(AVAudioPCMBuffer* buffer, AVAudioTime* when)
{
    // The recording's epoch and boundary as this buffer reaches the tap: a recording that begins while the buffer is
    // converted drops it at the write (CaptureRing::restart).
    const std::uint64_t epoch = ring_.epoch();
    if (!started_ || epoch != epoch_) {
        const std::uint64_t boundary = ring_.boundaryNanoseconds();
        std::uint64_t captured = 0;
        if (when != nil && when.hostTimeValid)
            captured = static_cast<std::uint64_t>([AVAudioTime secondsForHostTime:when.hostTime] * 1e9);
        // A converter that has not run holds nothing; one that has holds audio from before this recording.
        if (started_)
            [converter_ reset];
        started_ = true;
        epoch_ = epoch;
        leaveOut_ = framesBeforeBoundary(captured, boundary, outputRate_);
    }
    __block BOOL supplied = NO;
    converted_.frameLength = 0;
    NSError* error = nil;
    const auto status = [converter_ convertToBuffer:converted_
                                              error:&error
                                 withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount, AVAudioConverterInputStatus* inputStatus) {
                                     if (supplied) {
                                         *inputStatus = AVAudioConverterInputStatus_NoDataNow;
                                         return nil;
                                     }
                                     supplied = YES;
                                     *inputStatus = AVAudioConverterInputStatus_HaveData;
                                     return buffer;
                                 }];
    if (status == AVAudioConverterOutputStatus_Error)
        return;
    const auto* data = static_cast<const std::uint8_t*>(converted_.audioBufferList->mBuffers[0].mData);
    if (data == nullptr)
        return;
    const std::size_t frames = converted_.frameLength;
    const std::size_t skipped = std::min(frames, leaveOut_);
    leaveOut_ -= skipped;
    (void)ring_.write(std::span(data + skipped * frameBytes_, (frames - skipped) * frameBytes_), epoch);
}

} // namespace uml::audio
