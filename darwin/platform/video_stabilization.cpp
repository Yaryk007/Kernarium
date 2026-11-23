// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_stabilization.hpp"

namespace uml::platform {

VideoStabilization chooseVideoStabilization(bool wanted, VideoStabilizationSupport support) noexcept
{
    if (!wanted) return VideoStabilization::off;
    if (support.lowLatency) return VideoStabilization::lowLatency;
    if (support.standard) return VideoStabilization::standard;
    return VideoStabilization::off;
}

VideoStabilizationStep stabilizationStep(bool wanted, VideoStabilizationSupport support,
                                         const VideoStabilizationHeld& held) noexcept
{
    VideoStabilizationStep step;
    step.choice = chooseVideoStabilization(wanted, support);
    step.write = step.choice != held.mode;
    if (!wanted) step.state = VideoStabilizationState::notWanted;
    else if (step.choice == VideoStabilization::lowLatency) step.state = VideoStabilizationState::lowLatency;
    else if (step.choice == VideoStabilization::standard) step.state = VideoStabilizationState::standard;
    else step.state = VideoStabilizationState::unsupported;
    step.note = step.state != held.state;
    step.unsupported = step.note && step.state == VideoStabilizationState::unsupported;
    return step;
}

VideoStabilizationStep applyVideoStabilization(VideoStabilizationPort& port, bool wanted, VideoStabilizationHeld& held)
{
    const VideoStabilizationStep step = stabilizationStep(wanted, port.support(), held);
    if (step.write) port.write(step.choice);
    held = {step.choice, step.state};
    return step;
}

} // namespace uml::platform
