// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::audio::session {

/* The AVAudioSession category and options the process's one session needs
 * (audio_session.hpp). The choice is plain data, so that a test can check it
 * without an audio session: audio_session.mm hands it to the system, and
 * checks at compile time that each option bit here is the SDK's. */

enum class Category : std::uint8_t {
    playback,
    playAndRecord,
};

/* AVAudioSessionCategoryOptions bits. */
namespace option {
inline constexpr std::uint32_t mixWithOthers = 1U << 0;
inline constexpr std::uint32_t allowBluetoothHandsFree = 1U << 2;
inline constexpr std::uint32_t defaultToSpeaker = 1U << 3;
inline constexpr std::uint32_t allowBluetoothA2DP = 1U << 5;
inline constexpr std::uint32_t bluetoothHighQualityRecording = 1U << 19;
} // namespace option

struct CategoryPlan final {
    Category category {};
    std::uint32_t options {};

    [[nodiscard]] constexpr bool operator==(const CategoryPlan&) const noexcept =
        default;
};

/* Playback alone is the playback category with no options: Bluetooth stays on
 * A2DP, the profile for music, whatever headset is connected.
 *
 * While the guest records with the Microphone setting on, the category is
 * play-and-record, and `bluetoothInput` lets a connected Bluetooth headset be
 * the input: the hands-free profile, which carries the headset's microphone
 * and also moves the headset's output to that profile, at telephone quality,
 * for as long as the guest records. The high-quality recording option keeps
 * both directions at full bandwidth on headsets that support it (the hands-free
 * profile is its fallback elsewhere). Without `bluetoothInput` the recording
 * category keeps the built-in microphone and A2DP, which is also what a session
 * that refused the Bluetooth options falls back to. */
[[nodiscard]] constexpr CategoryPlan categoryPlan(
    bool recording, bool bluetoothInput = true) noexcept
{
    if (!recording)
        return {Category::playback, 0};
    std::uint32_t options = option::mixWithOthers | option::defaultToSpeaker |
        option::allowBluetoothA2DP;
    if (bluetoothInput) {
        options |= option::allowBluetoothHandsFree |
            option::bluetoothHighQualityRecording;
    }
    return {Category::playAndRecord, options};
}

} // namespace uml::audio::session
