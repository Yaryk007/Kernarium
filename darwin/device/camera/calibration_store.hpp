// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace uml::camera {

/* The colour calibration and optics learned from a Bayer still, kept per host
 * model and lens. They are the camera's characteristics, so they live outside
 * the guest's disks and survive a guest's update or erase. The store reads and
 * writes one small file per lens in its directory; an empty directory keeps
 * them in memory only. A file that fails its check is ignored, and a save
 * replaces the file whole. */
class CalibrationStore final {
public:
    explicit CalibrationStore(std::filesystem::path directory = {});

    [[nodiscard]] std::optional<uml::platform::CameraColorData> load(std::string_view model,
                                                                     std::string_view lens) const;
    /* False when the file could not be written; the value is kept in memory
     * regardless. */
    bool save(std::string_view model, std::string_view lens, const uml::platform::CameraColorData& data);

    /* The key a lens is stored under. */
    [[nodiscard]] static std::string lensKey(const uml::platform::CameraInfo& info);

private:
    [[nodiscard]] std::filesystem::path pathOf(std::string_view model, std::string_view lens) const;

    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, uml::platform::CameraColorData> memory_;
};

} // namespace uml::camera
