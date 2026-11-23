// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera.hpp"

#include <cstdint>
#include <memory>

namespace uml::camera {

/* Where a launcher gets the platform camera that VirtioCameraDevice serves.
 * The camera device itself links no backend; this library owns the two a
 * launcher can choose between, so that a launcher reaches them through the
 * device layer. */

/* A platform camera, ready for makeInProcessCameraService(). */
using CameraSourceHandle = std::shared_ptr<uml::platform::Camera>;

/* The synthetic camera takes commands from a named pipe whose path this variable names (`interrupt`, `interrupt
 * background|multitasking|pressure`, `resume`, a line each): the only way a test reaches the camera of a guest that runs in
 * a process of its own, and the only way to interrupt a Mac session. Unset, the camera is deaf. */
inline constexpr const char* syntheticControlVariable = "UML_SYNTHETIC_CAMERA_CONTROL";

/* The cameras the synthetic camera presents. `phoneTurnedFront` is the phone whose front camera is mounted at 90 and
 * turned half around by the host, as an iPhone's is (SyntheticCameraOptions::frontPresentedAt90). */
enum class SyntheticProfile : std::uint8_t { phone, tablet, laptop, phoneTurnedFront };

/* Whether the system's camera permission is already granted to this process.
 * It only reads the answer: nothing here shows a prompt. */
[[nodiscard]] bool hostCameraPermissionGranted();

/* The synthetic camera. */
[[nodiscard]] CameraSourceHandle makeSyntheticCameraSource(SyntheticProfile profile);

/* The host's own cameras, discovered and not opened. It does not read the
 * permission; hostCameraPermissionGranted() is the caller's to check first. */
[[nodiscard]] CameraSourceHandle makeHostCameraSource();

} // namespace uml::camera
