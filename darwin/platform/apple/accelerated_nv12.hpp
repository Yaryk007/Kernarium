// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The Apple host's crop, rotate and scale of an NV12 picture: Accelerate's
// vImage scaling and quarter-turn rotation with the scratch memory kept between
// frames. It does what the portable transformNv12() does (nv12_transform.hpp)
// within the resampling filter's tolerance; a rotation and a copy are exact.

#include "nv12_transform.hpp"

#include <memory>

namespace uml::platform {

[[nodiscard]] std::unique_ptr<Nv12Transformer> makeAcceleratedNv12Transformer();

} // namespace uml::platform
