// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace uml::platform {

enum class CameraPermission : std::uint8_t {
    // The user has not been asked.
    undetermined,
    denied,
    granted,
};

/* The system's permission to capture from a camera.
 *
 * permission() only reads the current answer and never shows a prompt.
 * request() may show the system prompt, and it does so only when the answer is
 * undetermined; the caller invokes it as the direct result of a user's act, such
 * as turning a Camera setting on, and never at launch. The completion runs
 * once, on an arbitrary thread, with the answer. */
class CameraAuthorization {
public:
    virtual ~CameraAuthorization() = default;

    [[nodiscard]] virtual CameraPermission permission() const noexcept = 0;
    virtual void request(std::function<void(CameraPermission)> completion) = 0;
};

[[nodiscard]] std::shared_ptr<CameraAuthorization> makeAppleCameraAuthorization();

} // namespace uml::platform
