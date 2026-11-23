// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared vectors for the camera layouts' `concurrent` feature, which is defined once
// (camera_ids.hpp, concurrent_feature): the cameras "0" and "1" are in one set that
// the host streams together, and both meet the guaranteed concurrent stream sizes
// (camera_plan.hpp, meets_concurrent_streams). The HAL's tests hold the vectors
// and the application lane's tests use the same ones, so that a layout and the
// cameras Android lists never disagree. The formats are those of the synthetic
// camera (darwin/platform/synthetic_camera/synthetic_camera.cpp) as a node lists
// them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera_ids.hpp"
#include "camera_plan.hpp"

namespace andarium::camera::vectors {

struct CameraCase {
    std::uint32_t id = 0; // the ID Android gives it
    std::uint32_t index = 0; // the host's number for it
    Formats formats;
    std::uint32_t concurrent_mask = 0; // by host number
};

struct ConcurrentVector {
    std::string name;
    std::vector<CameraCase> cameras;
    bool concurrent = false; // what concurrent_feature must say
};

inline Formats formats_of(std::vector<VideoMode> video, std::vector<Size> jpeg)
{
    Formats formats;
    formats.video = std::move(video);
    formats.jpeg = std::move(jpeg);
    return formats;
}

// A phone lens and the phone's front camera (video up to 4K, stills up to 12 MP).
inline Formats phone_lens_formats()
{
    return formats_of({{640, 480, 1, 60}, {1280, 720, 1, 120}, {1920, 1080, 1, 60}, {3840, 2160, 1, 60}},
                      {{4032, 3024}, {2016, 1512}, {1008, 756}});
}

// A pad's front camera (video up to 1080p, stills up to 2016 x 1512).
inline Formats pad_front_formats()
{
    return formats_of({{640, 480, 1, 60}, {1280, 720, 1, 60}, {1920, 1080, 1, 60}}, {{2016, 1512}, {1008, 756}});
}

// A laptop's only camera.
inline Formats laptop_formats()
{
    return formats_of({{640, 480, 1, 30}, {1280, 720, 1, 30}, {1920, 1080, 1, 30}}, {{1920, 1080}});
}

inline std::vector<ConcurrentVector> concurrent_vectors()
{
    const std::uint32_t with0 = 1u << 0, with1 = 1u << 1, with2 = 1u << 2;
    std::vector<ConcurrentVector> vectors;
    vectors.push_back({"the phone's zoom and front cameras stream together",
                       {{0, 0, phone_lens_formats(), with1}, {1, 1, phone_lens_formats(), with0}},
                       true});
    vectors.push_back({"a phone of three lenses: the wide lens and the front camera stream together, the other lenses do not",
                       {{0, 0, phone_lens_formats(), with1},
                        {1, 1, phone_lens_formats(), with0},
                        {3, 2, phone_lens_formats(), 0},
                        {4, 3, phone_lens_formats(), 0}},
                       true});
    vectors.push_back({"a pad's front camera cannot make the guaranteed 1920x1440 JPEG (its video stops at 1080p)",
                       {{0, 0, phone_lens_formats(), with1}, {1, 1, pad_front_formats(), with0}},
                       false});
    vectors.push_back({"a laptop has one camera", {{0, 0, laptop_formats(), 0}}, false});
    vectors.push_back({"one side naming the other is not a set",
                       {{0, 0, phone_lens_formats(), with1}, {1, 1, phone_lens_formats(), 0}},
                       false});
    vectors.push_back({"a third camera that fails the sizes leaves the first two a set",
                       {{0, 0, phone_lens_formats(), with1 | with2},
                        {1, 1, phone_lens_formats(), with0 | with2},
                        {2, 2, pad_front_formats(), with0 | with1}},
                       true});
    vectors.push_back({"cameras 0 and 1 each stream only with a third one",
                       {{0, 0, phone_lens_formats(), with2},
                        {1, 1, phone_lens_formats(), with2},
                        {2, 2, phone_lens_formats(), with0 | with1}},
                       false});
    vectors.push_back({"a front camera that has no 720p stream",
                       {{0, 0, phone_lens_formats(), with1},
                        {1, 1, formats_of({{1024, 1024, 1, 30}}, {{1024, 1024}}), with0}},
                       false});
    vectors.push_back({"the IDs differ from the host's numbers",
                       {{0, 4, phone_lens_formats(), 1u << 5}, {1, 5, phone_lens_formats(), 1u << 4}},
                       true});
    return vectors;
}

// What concurrent_feature says of a vector's cameras.
inline bool evaluate(const ConcurrentVector& vector)
{
    std::vector<Concurrency> cameras;
    for (const auto& camera : vector.cameras) cameras.push_back({camera.id, camera.index, camera.concurrent_mask, 0});
    return concurrent_feature(std::span<const Concurrency>(cameras), [&](std::uint32_t id) {
        for (const auto& camera : vector.cameras)
            if (camera.id == id) return meets_concurrent_streams(derive_sizes(camera.formats));
        return false;
    });
}

} // namespace andarium::camera::vectors
