// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_session_category.hpp"
#include "require.hpp"

#include <cstdint>

namespace {

using uml::audio::session::Category;
using uml::audio::session::categoryPlan;
using uml::testing::require;
namespace option = uml::audio::session::option;

[[nodiscard]] bool has(std::uint32_t options, std::uint32_t bit)
{
    return (options & bit) == bit;
}

// Playback alone keeps the playback category and its defaults, which route a
// Bluetooth headset over A2DP; the hands-free profile is never allowed there,
// whatever the caller asks.
void playbackOnlyKeepsA2dp()
{
    for (const bool bluetoothInput : {true, false}) {
        const auto plan = categoryPlan(false, bluetoothInput);
        require(plan.category == Category::playback,
            "playback alone uses the playback category");
        require(plan.options == 0,
            "playback alone sets no option: no hands-free, no recording");
    }
}

// While the guest records, a Bluetooth headset may be the input: the
// hands-free profile, and full bandwidth where the headset supports it.
void recordingAllowsTheHeadsetMicrophone()
{
    const auto plan = categoryPlan(true);
    require(plan.category == Category::playAndRecord,
        "recording uses play-and-record");
    require(has(plan.options, option::allowBluetoothHandsFree),
        "recording lets a headset be the input over the hands-free profile");
    require(has(plan.options, option::bluetoothHighQualityRecording),
        "recording asks for the high-quality route where one is supported");
    require(plan.options == categoryPlan(true, true).options,
        "the headset microphone is the default while recording");
}

// The recording category still mixes with other apps' audio, plays through the
// speaker when nothing else is connected, and keeps A2DP for output.
void recordingKeepsTheRestOfTheConfiguration()
{
    for (const bool bluetoothInput : {true, false}) {
        const auto plan = categoryPlan(true, bluetoothInput);
        require(has(plan.options, option::mixWithOthers),
            "recording mixes with other audio");
        require(has(plan.options, option::defaultToSpeaker),
            "recording defaults to the speaker");
        require(has(plan.options, option::allowBluetoothA2DP),
            "recording allows an A2DP output");
    }
}

// The fallback a session that refuses the Bluetooth options uses keeps the
// built-in microphone and nothing of the headset's input.
void recordingWithoutBluetoothInputKeepsTheBuiltInMicrophone()
{
    const auto plan = categoryPlan(true, false);
    require(plan.category == Category::playAndRecord,
        "the fallback still records");
    require(!has(plan.options, option::allowBluetoothHandsFree),
        "the fallback does not allow the hands-free profile");
    require(!has(plan.options, option::bluetoothHighQualityRecording),
        "the fallback does not ask for high-quality recording");
}

// The bits are the SDK's AVAudioSessionCategoryOptions values (audio_session.mm
// checks each at compile time), so the whole mask is what the system receives.
void masksAreTheSystemsValues()
{
    require(categoryPlan(true).options == 0x8002DU,
        "mix 0x1, hands-free 0x4, speaker 0x8, A2DP 0x20, high quality 0x80000");
    require(categoryPlan(true, false).options == 0x29U,
        "mix 0x1, speaker 0x8, A2DP 0x20");
}

} // namespace

int main()
{
    playbackOnlyKeepsA2dp();
    recordingAllowsTheHeadsetMicrophone();
    recordingKeepsTheRestOfTheConfiguration();
    recordingWithoutBluetoothInputKeepsTheBuiltInMicrophone();
    masksAreTheSystemsValues();
    return 0;
}
