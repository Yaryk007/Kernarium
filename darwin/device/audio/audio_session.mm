// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_session.hpp"

#include "audio_session_category.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <utility>

namespace uml::audio::session {

#if TARGET_OS_IPHONE

namespace {

std::mutex lock;
bool playbackNeeds {};
bool captureNeeds {};
// What the session holds from the last category apply() set: the plan, and the
// category, mode and options the session itself reported right after. A later
// apply() sets a category only when the plan or those reports differ, so that
// whatever else changes the session (a media-services reset, another part of
// the app) is set again.
std::optional<CategoryPlan> heldPlan;
NSString* __strong heldCategory;
NSString* __strong heldMode;
NSUInteger heldOptions {};
// The session refused the Bluetooth options since the microphone last took it.
bool bluetoothRefused {};
double playbackCycle {};
double captureCycle {};

/* The shortest IO cycle a client that needs the session asks for, or 0. */
double preferredCycle() noexcept
{
    double cycle = 0.0;
    for (const auto [needs, asked] :
         {std::pair {playbackNeeds, playbackCycle},
          std::pair {captureNeeds, captureCycle}}) {
        if (needs && asked > 0.0 && (cycle == 0.0 || asked < cycle))
            cycle = asked;
    }
    return cycle;
}

/* The plan's option bits are the SDK's. */
static_assert(option::mixWithOthers ==
    AVAudioSessionCategoryOptionMixWithOthers);
static_assert(option::allowBluetoothHandsFree ==
    AVAudioSessionCategoryOptionAllowBluetoothHFP);
static_assert(option::defaultToSpeaker ==
    AVAudioSessionCategoryOptionDefaultToSpeaker);
static_assert(option::allowBluetoothA2DP ==
    AVAudioSessionCategoryOptionAllowBluetoothA2DP);
static_assert(option::bluetoothHighQualityRecording ==
    AVAudioSessionCategoryOptionBluetoothHighQualityRecording);

/* The option bits whose loss changes where the audio goes. */
constexpr std::uint32_t routingOptions = option::allowBluetoothHandsFree |
    option::allowBluetoothA2DP | option::bluetoothHighQualityRecording;

bool setCategory(AVAudioSession* session, const CategoryPlan& plan,
    NSError** error) noexcept
{
    NSString* const category = plan.category == Category::playAndRecord
        ? AVAudioSessionCategoryPlayAndRecord
        : AVAudioSessionCategoryPlayback;
    if (![session
            setCategory:category
                   mode:AVAudioSessionModeDefault
                options:static_cast<AVAudioSessionCategoryOptions>(plan.options)
                  error:error])
        return false;
    heldPlan.reset();
    // The session lock does not cover the rest of the application, and the
    // picture-in-picture window sets the category too. A call of that kind
    // between the set above and the reads below leaves the session in its
    // state, and what is read then must not stand for the plan: it is
    // remembered only if it agrees with what was asked for, so that otherwise
    // the next apply() sets the category again.
    NSString* const reportedCategory = session.category;
    NSString* const reportedMode = session.mode;
    const NSUInteger reportedOptions = session.categoryOptions;
    if (![reportedCategory isEqualToString:category] ||
        ![reportedMode isEqualToString:AVAudioSessionModeDefault] ||
        (reportedOptions & plan.options & routingOptions) !=
            (plan.options & routingOptions)) {
        static bool reported = false;
        if (!std::exchange(reported, true)) {
            std::fprintf(stderr,
                "Audio session settings differ from the category just set "
                "(options %u asked for, %lu reported); setting it again with "
                "the next use\n",
                plan.options, static_cast<unsigned long>(reportedOptions));
        }
        return true;
    }
    heldPlan = plan;
    heldCategory = reportedCategory;
    heldMode = reportedMode;
    heldOptions = reportedOptions;
    return true;
}

/* Whether the session holds `plan` as setCategory() left it. Setting a
 * category the session already has is not free: on an iPhone a
 * play-and-record one takes hundreds of milliseconds and posts a
 * category-change route notification, on which the output restarts and takes
 * the session again (apply() below). Each such turn blocks the thread that
 * serves the guest's audio, and the guest's capture loses the periods it is
 * owed. */
bool holds(AVAudioSession* session, const CategoryPlan& plan) noexcept
{
    return heldPlan == plan &&
        [session.category isEqualToString:heldCategory] &&
        [session.mode isEqualToString:heldMode] &&
        session.categoryOptions == heldOptions;
}

/* Sets the category the current clients need, when the session does not hold
 * it, and activates the session. */
bool apply() noexcept
{
    @try {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;
        const bool recording = captureNeeds;
        const CategoryPlan wanted = categoryPlan(recording, !bluetoothRefused);
        if (!holds(session, wanted) && !setCategory(session, wanted, &error)) {
            // The microphone worked before the Bluetooth options, and a
            // session that refuses them must not cost the guest it.
            static bool reported = false;
            const CategoryPlan fallback = categoryPlan(recording, false);
            if (!recording || bluetoothRefused ||
                (!holds(session, fallback) &&
                    !setCategory(session, fallback, &error)))
                return false;
            bluetoothRefused = true;
            if (!std::exchange(reported, true)) {
                std::fprintf(stderr,
                    "Audio session refused the Bluetooth microphone options; "
                    "the guest records from the built-in microphone\n");
            }
        }
        // Only a preference: a session that keeps another cycle still plays.
        if (const double cycle = preferredCycle(); cycle > 0.0)
            (void)[session setPreferredIOBufferDuration:cycle error:&error];
        return [session setActive:YES error:&error];
    } @catch (NSException*) {
        return false;
    }
}

} // namespace

bool otherAudioPlaying() noexcept
{
    @try {
        return [AVAudioSession sharedInstance].secondaryAudioShouldBeSilencedHint;
    } @catch (NSException*) {
        return false;
    }
}

bool acquire(Client client) noexcept
{
    const std::lock_guard guard(lock);
    bool& needs = client == Client::capture ? captureNeeds : playbackNeeds;
    const bool before = needs;
    needs = true;
    // The microphone starting, or built again after a route change that may
    // allow them, asks for the Bluetooth options once more; the output taking
    // the session does not.
    if (client == Client::capture)
        bluetoothRefused = false;
    if (apply())
        return true;
    needs = before;
    return false;
}

void release(Client client) noexcept
{
    const std::lock_guard guard(lock);
    (client == Client::capture ? captureNeeds : playbackNeeds) = false;
    if (playbackNeeds || captureNeeds) {
        (void)apply();
        return;
    }
    @try {
        NSError* error = nil;
        [[AVAudioSession sharedInstance]
            setActive:NO
          withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                error:&error];
    } @catch (NSException*) {
    }
}

void preferIoCycle(Client client, double seconds) noexcept
{
    const std::lock_guard guard(lock);
    (client == Client::capture ? captureCycle : playbackCycle) = seconds;
}

#else

bool otherAudioPlaying() noexcept
{
    return false;
}

void preferIoCycle(Client, double) noexcept {}

bool acquire(Client) noexcept
{
    return true;
}

void release(Client) noexcept {}

#endif

} // namespace uml::audio::session
