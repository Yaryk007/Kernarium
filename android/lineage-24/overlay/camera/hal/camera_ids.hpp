// SPDX-License-Identifier: Apache-2.0
//
// The camera IDs Android sees. They are stable across devices so that an
// overlay can name them: "0" is the back zoom camera (or the primary back lens
// of a host that has no zoom camera), "1" the front camera, and each other back
// lens has an ID of its own, "2" the wide, "3" the ultra wide and "4" the
// telephoto. A host without a lens has no ID for it, and a host with no back
// camera numbers its front camera "0", so that the IDs of the cameras that exist
// stay consecutive from zero for Android's legacy interfaces.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "camera_abi.hpp"

namespace andarium::camera {

// What the assignment needs of a camera.
struct CameraKey {
    std::uint32_t index = 0; // the host's number for it (virtio_video_camera_desc::index)
    std::uint32_t facing = 0;
    std::uint32_t lens = 0;
};

struct IdAssignment {
    std::uint32_t id = 0;
    std::uint32_t index = 0;
};

constexpr std::uint32_t wide_camera_id = 2;
constexpr std::uint32_t ultra_wide_camera_id = 3;
constexpr std::uint32_t telephoto_camera_id = 4;

// The assignment, by ascending ID. External cameras and a second camera of one
// lens are left out.
inline std::vector<IdAssignment> assign_ids(std::span<const CameraKey> cameras)
{
    std::vector<IdAssignment> result;
    const CameraKey* primary = nullptr;
    for (const auto& camera : cameras)
        if (camera.facing == abi::facing::back && camera.lens == abi::lens::zoom) {
            primary = &camera;
            break;
        }
    if (primary == nullptr)
        for (const auto& camera : cameras)
            if (camera.facing == abi::facing::back && camera.lens != abi::lens::external) {
                primary = &camera;
                break;
            }
    if (primary != nullptr) result.push_back({0, primary->index});

    for (const auto& camera : cameras) {
        if (camera.facing != abi::facing::front) continue;
        result.push_back({primary != nullptr ? 1u : 0u, camera.index});
        break;
    }

    for (const auto& camera : cameras) {
        if (camera.facing != abi::facing::back || &camera == primary) continue;
        std::uint32_t id = 0;
        switch (camera.lens) {
        case abi::lens::wide: id = wide_camera_id; break;
        case abi::lens::ultra_wide: id = ultra_wide_camera_id; break;
        case abi::lens::telephoto: id = telephoto_camera_id; break;
        default: break;
        }
        if (id == 0) continue;
        if (std::any_of(result.begin(), result.end(), [id](const IdAssignment& taken) { return taken.id == id; })) continue;
        result.push_back({id, camera.index});
    }
    std::sort(result.begin(), result.end(), [](const IdAssignment& a, const IdAssignment& b) { return a.id < b.id; });
    return result;
}

// What the concurrency rules need of a camera: the ID Android gave it and the
// masks of its description, which name other cameras by the host's number.
struct Concurrency {
    std::uint32_t id = 0;
    std::uint32_t index = 0;
    std::uint32_t concurrent_mask = 0;
    std::uint32_t conflict_mask = 0;
};

inline bool names(std::uint32_t mask, std::uint32_t index) noexcept
{
    return index < 32 && (mask & (1u << index)) != 0;
}

// The largest sets of two or more cameras that can stream at the same time: in
// a set, every camera's concurrent mask names every other camera. Each set is
// ascending by ID, and the sets are ordered by their first IDs.
inline std::vector<std::vector<std::uint32_t>> concurrent_sets(std::span<const Concurrency> cameras)
{
    const std::size_t count = std::min<std::size_t>(cameras.size(), abi::max_cameras);
    std::vector<std::uint32_t> valid; // bit i of an entry: cameras[i] is in the set
    for (std::uint32_t subset = 1; subset < (1u << count); ++subset) {
        if (std::popcount(subset) < 2) continue;
        bool ok = true;
        for (std::size_t i = 0; ok && i < count; ++i) {
            if (!(subset & (1u << i))) continue;
            for (std::size_t j = 0; ok && j < count; ++j)
                if (i != j && (subset & (1u << j)) && !names(cameras[i].concurrent_mask, cameras[j].index)) ok = false;
        }
        if (ok) valid.push_back(subset);
    }
    std::vector<std::vector<std::uint32_t>> sets;
    for (const auto subset : valid) {
        const bool maximal = std::none_of(valid.begin(), valid.end(), [subset](std::uint32_t other) {
            return other != subset && (other & subset) == subset;
        });
        if (!maximal) continue;
        std::vector<std::uint32_t> ids;
        for (std::size_t i = 0; i < count; ++i)
            if (subset & (1u << i)) ids.push_back(cameras[i].id);
        std::sort(ids.begin(), ids.end());
        sets.push_back(std::move(ids));
    }
    std::sort(sets.begin(), sets.end());
    return sets;
}

// The sets the provider reports to Android: each host set reduced to the cameras
// that meet the guaranteed concurrent stream sizes (`meets(id)`, camera_plan.hpp),
// when at least two remain.
template <typename Meets>
std::vector<std::vector<std::uint32_t>> reported_concurrent_sets(std::span<const Concurrency> cameras, Meets&& meets)
{
    std::vector<std::vector<std::uint32_t>> reported;
    for (auto set : concurrent_sets(cameras)) {
        set.erase(std::remove_if(set.begin(), set.end(), [&](std::uint32_t id) { return !meets(id); }), set.end());
        if (set.size() >= 2 && std::find(reported.begin(), reported.end(), set) == reported.end())
            reported.push_back(std::move(set));
    }
    return reported;
}

// The `concurrent` feature of a camera layout, defined once for the layout
// generator's documentation, the HAL and the application: the cameras "0" and "1"
// are in one set that the host streams together, and both meet the guaranteed
// concurrent stream sizes. It is what Android's CameraManager requires of the
// feature (a reported combination holding the main back and front cameras).
template <typename Meets>
bool concurrent_feature(std::span<const Concurrency> cameras, Meets&& meets)
{
    for (const auto& set : reported_concurrent_sets(cameras, meets))
        if (std::find(set.begin(), set.end(), 0u) != set.end() && std::find(set.begin(), set.end(), 1u) != set.end())
            return true;
    return false;
}

// The IDs of the cameras that cannot be open while this one is: the cameras its
// conflict mask names, or that name it.
inline std::vector<std::uint32_t> conflicting_ids(std::span<const Concurrency> cameras, std::uint32_t id)
{
    std::vector<std::uint32_t> result;
    const Concurrency* self = nullptr;
    for (const auto& camera : cameras)
        if (camera.id == id) self = &camera;
    if (self == nullptr) return result;
    for (const auto& other : cameras) {
        if (other.id == id) continue;
        if (names(self->conflict_mask, other.index) || names(other.conflict_mask, self->index)) result.push_back(other.id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace andarium::camera
