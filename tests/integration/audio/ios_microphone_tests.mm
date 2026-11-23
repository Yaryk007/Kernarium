// SPDX-License-Identifier: GPL-2.0-or-later
//
// Starts the Apple microphone in the iOS Simulator and posts the audio-session
// notifications that iOS 27 posts when another app takes the session and gives
// it back. An interruption must stop the microphone and release the session.
// The system's advice to resume must start it again; without that advice it
// must start again only if no other app plays, because starting takes the
// session and the microphone must never take it from an app that is playing
// or recording, such as a call. The app's own deactivations interrupt
// nothing. run_ios_interruption.py builds this file with the microphone and
// the session for the Simulator and runs it.
//
// Nothing here opens a real input or can show a permission prompt: the
// engine's start and stop, its input node's format and tap, and the session's
// category and activation are hooked, as the sink test hooks the session, and
// the hooked session reports the category, mode and options it was given, so
// that the tests count how often the category is set. The real microphone is
// checked by hand (see the audio README).

#include "audio_session.hpp"
#include "capture_ring.hpp"
#include "ios_session_notifications.hpp"
#include "microphone_capture_source.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool passed, const char* name, const std::string& detail = {})
{
    failures += passed ? 0 : 1;
    std::printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name,
        detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

// What the hooks observe and drive.
std::atomic<bool> otherAudioPlaying {};
std::atomic<int> engineStarts {};
std::atomic<int> engineStops {};
std::atomic<int> sessionActivations {};
std::atomic<int> sessionDeactivations {};
// How many of the next activations the session refuses, as it does while the
// system has not given the route back.
std::atomic<int> activationsToFail {};
// The last category the session was given, and whether the session refuses
// the Bluetooth options (a category call that carries them fails).
std::atomic<NSUInteger> categoryOptions {};
std::atomic<int> categoryCalls {};
std::atomic<bool> categoryIsPlayAndRecord {};
std::atomic<bool> refuseBluetoothOptions {};
// The category, mode and options the hooked session reports, as a real session
// does after the category calls it took. A reset leaves what a media-services
// reset does, and another part of the app can change them under the code that
// owns the session.
NSString* __strong sessionCategory = AVAudioSessionCategorySoloAmbient;
NSString* __strong sessionMode = AVAudioSessionModeDefault;
std::atomic<NSUInteger> sessionOptions {};
// What another part of the application does to the session right after a
// category call took effect, before the caller reads the session back.
void (*afterCategorySet)() = nullptr;
// The same, but while the caller reads the session back: right after it read
// the category (1), the mode (2) or the options (3), so that the readings come
// from two states. It applies to the readings that follow one category call.
int tornAfterRead = 0;
bool tornArmed = false;
void (*tornWrite)() = nullptr;

void tornPoint(int point)
{
    if (tornArmed && tornAfterRead == point) {
        tornArmed = false;
        tornWrite();
    }
}
// The engine the microphone started last, to post its configuration change.
AVAudioEngine* __weak startedEngine {};
// The callback the microphone gave the notification center for the engine's
// configuration change, as the center would call it. A route change that was
// already running when an interruption began calls it later; removing the
// observer does not stop that.
void (^routeChangeCallback)(NSNotification*) = nil;

void changeSession(NSString* category, NSString* mode, NSUInteger options)
{
    sessionCategory = category;
    sessionMode = mode;
    sessionOptions = options;
}

void resetSession()
{
    changeSession(AVAudioSessionCategorySoloAmbient, AVAudioSessionModeDefault, 0);
}

struct Counts final {
    int starts {};
    int stops {};
    int activations {};
    int deactivations {};

    [[nodiscard]] static Counts now()
    {
        return {engineStarts.load(), engineStops.load(),
            sessionActivations.load(), sessionDeactivations.load()};
    }

    [[nodiscard]] std::string text() const
    {
        return "starts " + std::to_string(starts) + ", stops " +
            std::to_string(stops) + ", activations " +
            std::to_string(activations) + ", deactivations " +
            std::to_string(deactivations);
    }

    bool operator==(const Counts&) const = default;
};

[[nodiscard]] Counts since(const Counts& before)
{
    const auto now = Counts::now();
    return {now.starts - before.starts, now.stops - before.stops,
        now.activations - before.activations,
        now.deactivations - before.deactivations};
}

void installHooks()
{
    Method observe = class_getInstanceMethod([NSNotificationCenter class],
        @selector(addObserverForName:object:queue:usingBlock:));
    using ObserveFunction = id (*)(id, SEL, NSNotificationName, id,
        NSOperationQueue*, void (^)(NSNotification*));
    const auto observeOriginal =
        reinterpret_cast<ObserveFunction>(method_getImplementation(observe));
    method_setImplementation(observe, imp_implementationWithBlock(
        ^id(NSNotificationCenter* center, NSNotificationName name, id object,
            NSOperationQueue* queue, void (^block)(NSNotification*)) {
          if ([name isEqualToString:AVAudioEngineConfigurationChangeNotification])
              routeChangeCallback = [block copy];
          return observeOriginal(center,
              @selector(addObserverForName:object:queue:usingBlock:), name,
              object, queue, block);
        }));

    Method hint = class_getInstanceMethod([AVAudioSession class],
        @selector(secondaryAudioShouldBeSilencedHint));
    method_setImplementation(hint, imp_implementationWithBlock(^BOOL(id) {
        return otherAudioPlaying.load();
    }));

    Method category = class_getInstanceMethod([AVAudioSession class],
        @selector(setCategory:mode:options:error:));
    method_setImplementation(category, imp_implementationWithBlock(
        ^BOOL(id, NSString* name, NSString* mode, NSUInteger options,
            NSError* __autoreleasing* error) {
          ++categoryCalls;
          categoryIsPlayAndRecord = [name isEqualToString:
              AVAudioSessionCategoryPlayAndRecord];
          categoryOptions = options;
          const NSUInteger bluetooth =
              AVAudioSessionCategoryOptionAllowBluetoothHFP |
              AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
          if (refuseBluetoothOptions.load() && (options & bluetooth) != 0) {
              if (error != nullptr) {
                  *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:-50
                                           userInfo:nil];
              }
              return NO;
          }
          changeSession(name, mode, options);
          if (afterCategorySet != nullptr)
              afterCategorySet();
          tornArmed = tornWrite != nullptr;
          return YES;
        }));
    Method reportedCategory =
        class_getInstanceMethod([AVAudioSession class], @selector(category));
    method_setImplementation(reportedCategory,
        imp_implementationWithBlock(^NSString*(id) {
          NSString* const reported = sessionCategory;
          tornPoint(1);
          return reported;
        }));
    Method reportedMode =
        class_getInstanceMethod([AVAudioSession class], @selector(mode));
    method_setImplementation(reportedMode,
        imp_implementationWithBlock(^NSString*(id) {
          NSString* const reported = sessionMode;
          tornPoint(2);
          return reported;
        }));
    Method reportedOptions = class_getInstanceMethod(
        [AVAudioSession class], @selector(categoryOptions));
    method_setImplementation(reportedOptions,
        imp_implementationWithBlock(^NSUInteger(id) {
          const NSUInteger reported = sessionOptions.load();
          tornPoint(3);
          return reported;
        }));

    Method activate = class_getInstanceMethod(
        [AVAudioSession class], @selector(setActive:error:));
    method_setImplementation(activate, imp_implementationWithBlock(
        ^BOOL(id, BOOL active, NSError* __autoreleasing* error) {
          if (active && activationsToFail.load() > 0) {
              --activationsToFail;
              if (error != nullptr) {
                  *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:-50
                                           userInfo:nil];
              }
              return NO;
          }
          if (active)
              ++sessionActivations;
          return YES;
        }));

    Method deactivate = class_getInstanceMethod(
        [AVAudioSession class], @selector(setActive:withOptions:error:));
    method_setImplementation(deactivate, imp_implementationWithBlock(
        ^BOOL(id, BOOL active, NSUInteger, NSError* __autoreleasing*) {
          ++(active ? sessionActivations : sessionDeactivations);
          return YES;
        }));

    Method start = class_getInstanceMethod(
        [AVAudioEngine class], @selector(startAndReturnError:));
    method_setImplementation(start, imp_implementationWithBlock(
        ^BOOL(id engine, NSError* __autoreleasing*) {
          ++engineStarts;
          startedEngine = engine;
          return YES;
        }));

    Method stop =
        class_getInstanceMethod([AVAudioEngine class], @selector(stop));
    method_setImplementation(stop, imp_implementationWithBlock(^(id) {
        ++engineStops;
    }));

    Method prepare =
        class_getInstanceMethod([AVAudioEngine class], @selector(prepare));
    method_setImplementation(prepare, imp_implementationWithBlock(^(id) {}));

    // The input node answers a format and takes a tap without any hardware.
    static AVAudioFormat* const format =
        [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000
                                                       channels:1];
    Method inputFormat = class_getInstanceMethod(
        [AVAudioInputNode class], @selector(outputFormatForBus:));
    method_setImplementation(inputFormat,
        imp_implementationWithBlock(^AVAudioFormat*(id, AVAudioNodeBus) {
          return format;
        }));
#ifdef UML_AUDIO_LEGACY_SESSION
    // Before iOS 27 the only tap call is the one that raises on failure.
    Method tap = class_getInstanceMethod([AVAudioInputNode class],
        @selector(installTapOnBus:bufferSize:format:block:));
    method_setImplementation(tap, imp_implementationWithBlock(
        ^(id, AVAudioNodeBus, AVAudioFrameCount, AVAudioFormat*,
            AVAudioNodeTapBlock) {}));
#else
    Method tap = class_getInstanceMethod([AVAudioInputNode class],
        @selector(installTapOnBus:bufferSize:format:error:block:));
    method_setImplementation(tap, imp_implementationWithBlock(
        ^BOOL(id, AVAudioNodeBus, AVAudioFrameCount, AVAudioFormat*,
            NSError* __autoreleasing*, AVAudioNodeTapBlock) { return YES; }));
#endif
    Method untap = class_getInstanceMethod(
        [AVAudioInputNode class], @selector(removeTapOnBus:));
    method_setImplementation(untap,
        imp_implementationWithBlock(^(id, AVAudioNodeBus) {}));
}

// A guest that records, as the capture source starts it.
struct Recording final {
    Recording()
        : input(uml::audio::makeAppleMicrophoneInput())
        , ring(1U << 16)
    {
    }

    [[nodiscard]] bool start() { return input->start(1, ring); }
    void stop() { input->stop(); }

    std::unique_ptr<uml::audio::MicrophoneInput> input;
    uml::audio::CaptureRing ring;
};

#ifndef UML_AUDIO_LEGACY_SESSION
// The resumption notification with no context, as a recommendation the
// system does not give. Systems before iOS 27 have no such notification.
void postRecommendationWithoutContext()
{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionResumptionRecommendationNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:nil];
}
#endif

void startsAndStops()
{
    otherAudioPlaying = false;
    Recording recording;
    auto before = Counts::now();
    check(recording.start(), "the microphone starts");
    auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "starting takes the session and starts the engine once", seen.text());
    before = Counts::now();
    recording.stop();
    seen = since(before);
    check(seen.stops == 1 && seen.deactivations == 1,
        "stopping stops the engine and releases the session", seen.text());
}

void deactivationsThatInterruptNothing()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the deactivations");
    const auto before = Counts::now();
    postDeactivation(DeactivationSource::app, false);
    postDeactivation(DeactivationSource::system, false);
    const auto seen = since(before);
    check(seen == Counts {},
        "the app's own deactivation and a system deactivation that "
        "interrupts nothing leave the microphone running",
        seen.text());
}

void interruptionStopsTheMicrophone()
{
    otherAudioPlaying = true;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    auto before = Counts::now();
    postInterruptionBegan();
    auto seen = since(before);
    check(seen.stops == 1 && seen.deactivations == 1 && seen.starts == 0 &&
            seen.activations == 0,
        "an interruption stops the engine and releases the session",
        seen.text());
    before = Counts::now();
    postInterruptionBegan();
    seen = since(before);
    check(seen == Counts {}, "a second interruption does nothing more",
        seen.text());
    otherAudioPlaying = false;
}

void adviceToResumeStartsIt()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    auto before = Counts::now();
    postInterruptionEnded(true);
    auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "a recommendation to resume starts the microphone again",
        seen.text());
    before = Counts::now();
    postInterruptionEnded(true);
    seen = since(before);
    check(seen == Counts {},
        "a recommendation while the microphone runs changes nothing",
        seen.text());

    // The system's advice settles it, whatever the hint says.
    postInterruptionBegan();
    otherAudioPlaying = true;
    before = Counts::now();
    postInterruptionEnded(true);
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "a recommendation to resume starts it while another app plays too",
        seen.text());
    otherAudioPlaying = false;
}

// The microphone never takes the session from an app that plays.
void noAdviceWhileAnotherAppPlays()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    otherAudioPlaying = true;
    auto before = Counts::now();
    postInterruptionEnded(false);
    auto seen = since(before);
    check(seen == Counts {},
        "a recommendation not to resume while another app plays starts "
        "nothing and takes no session",
        seen.text());
#ifndef UML_AUDIO_LEGACY_SESSION
    before = Counts::now();
    postRecommendationWithoutContext();
    seen = since(before);
    check(seen == Counts {},
        "so does a recommendation that carries no advice", seen.text());
#endif

    // It stays closed until the next recommendation to resume.
    otherAudioPlaying = false;
    before = Counts::now();
    postInterruptionEnded(true);
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "the next recommendation to resume starts it", seen.text());
}

void noAdviceWhileNoAppPlays()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    auto before = Counts::now();
    postInterruptionEnded(false);
    auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "a recommendation not to resume starts the microphone when no other "
        "app plays",
        seen.text());

#ifndef UML_AUDIO_LEGACY_SESSION
    postInterruptionBegan();
    before = Counts::now();
    postRecommendationWithoutContext();
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "so does a recommendation that carries no advice", seen.text());
#endif
}

void guestRestartsRecording()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    otherAudioPlaying = true;
    postInterruptionEnded(false);
    const auto before = Counts::now();
    check(recording.start(), "the guest's next recording starts it again");
    const auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "the engine starts once for it", seen.text());
    otherAudioPlaying = false;
}

void stoppedAndDestroyedMicrophones()
{
    otherAudioPlaying = false;
    {
        Recording recording;
        check(recording.start(), "the microphone starts before it is stopped");
        postInterruptionBegan();
        recording.stop();
        const auto before = Counts::now();
        postInterruptionEnded(true);
        postInterruptionBegan();
        postInterruptionEnded(false);
        const auto seen = since(before);
        check(seen == Counts {},
            "a microphone the guest stopped stays stopped", seen.text());
    }
    {
        Recording recording;
        check(recording.start(), "the microphone starts before it is destroyed");
        postInterruptionBegan();
        recording.input.reset();
        const auto before = Counts::now();
        postInterruptionEnded(true);
        postInterruptionBegan();
        postInterruptionEnded(false);
        const auto seen = since(before);
        check(seen == Counts {},
            "notifications for a destroyed microphone do nothing",
            seen.text());
    }
}

// While the guest records, a Bluetooth headset may be the input; the session
// is asked for it only then (the options themselves are checked by
// audio-session-category).
void sessionAllowsTheHeadsetMicrophone()
{
    otherAudioPlaying = false;
    refuseBluetoothOptions = false;
    resetSession();
    Recording recording;
    check(recording.start(), "the microphone starts for the category check");
    const NSUInteger wanted = AVAudioSessionCategoryOptionMixWithOthers |
        AVAudioSessionCategoryOptionDefaultToSpeaker |
        AVAudioSessionCategoryOptionAllowBluetoothA2DP |
        AVAudioSessionCategoryOptionAllowBluetoothHFP |
        AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    check(categoryIsPlayAndRecord.load() && categoryOptions.load() == wanted,
        "recording asks for play-and-record with the headset microphone",
        "options " + std::to_string(categoryOptions.load()));
}

// A session that refuses the Bluetooth options must not cost the guest its
// microphone: it records from the built-in one.
void refusedBluetoothOptionsFallBack()
{
    otherAudioPlaying = false;
    refuseBluetoothOptions = true;
    resetSession();
    Recording recording;
    const int callsBefore = categoryCalls.load();
    const bool started = recording.start();
    refuseBluetoothOptions = false;
    check(started, "the microphone starts although the session refuses the "
                   "Bluetooth options");
    const NSUInteger bluetooth =
        AVAudioSessionCategoryOptionAllowBluetoothHFP |
        AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    check(categoryCalls.load() == callsBefore + 2 &&
            categoryIsPlayAndRecord.load() &&
            (categoryOptions.load() & bluetooth) == 0 &&
            (categoryOptions.load() &
                AVAudioSessionCategoryOptionAllowBluetoothA2DP) != 0,
        "it asks again for play-and-record without them",
        "calls " + std::to_string(categoryCalls.load() - callsBefore) +
            ", options " + std::to_string(categoryOptions.load()));
}

// Setting the category a session already has is not free on an iPhone: a
// play-and-record category takes about 300 ms and posts a category-change route
// notification, on which the output restarts and takes the session again. So
// the category is set only when it changes: the output taking and releasing the
// session while the guest records sets nothing, and neither does a microphone
// that is built again.
void sessionCategoryIsSetOnlyWhenItChanges()
{
    using uml::audio::session::Client;
    otherAudioPlaying = false;
    refuseBluetoothOptions = false;
    resetSession();
    int before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before + 1,
        "the output takes the session with the playback category once",
        "calls " + std::to_string(categoryCalls.load() - before));
    before = categoryCalls.load();
    for (int restart = 0; restart < 3; ++restart)
        (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before,
        "the output taking it again sets nothing",
        "calls " + std::to_string(categoryCalls.load() - before));

    Recording recording;
    before = categoryCalls.load();
    check(recording.start(), "the microphone starts for the category count");
    check(categoryCalls.load() == before + 1 && categoryIsPlayAndRecord.load(),
        "recording sets play-and-record once",
        "calls " + std::to_string(categoryCalls.load() - before));
    before = categoryCalls.load();
    for (int restart = 0; restart < 3; ++restart)
        (void)uml::audio::session::acquire(Client::playback);
    uml::audio::session::release(Client::playback);
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before,
        "the output taking and releasing the session while the guest records "
        "sets no category",
        "calls " + std::to_string(categoryCalls.load() - before));

    before = categoryCalls.load();
    recording.stop();
    check(categoryCalls.load() == before + 1 && !categoryIsPlayAndRecord.load(),
        "the end of the recording returns the category to playback once",
        "calls " + std::to_string(categoryCalls.load() - before));
    before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    uml::audio::session::release(Client::playback);
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before,
        "and the output keeps the playback category it holds",
        "calls " + std::to_string(categoryCalls.load() - before));
    uml::audio::session::release(Client::playback);
}

// What changes the session behind the code that owns it, a media-services reset
// or another part of the app that sets its own category, is noticed and set
// right once, whichever of the category, mode and options it touched.
void aSessionChangedBehindItsBackIsSetAgain()
{
    using uml::audio::session::Client;
    otherAudioPlaying = false;
    refuseBluetoothOptions = false;
    resetSession();
    (void)uml::audio::session::acquire(Client::playback);

    const auto setsOnceAfter = [](const char* what, NSString* category,
                                   NSString* mode, NSUInteger options) {
        changeSession(category, mode, options);
        const int before = categoryCalls.load();
        (void)uml::audio::session::acquire(uml::audio::session::Client::playback);
        const int first = categoryCalls.load() - before;
        (void)uml::audio::session::acquire(uml::audio::session::Client::playback);
        check(first == 1 && categoryCalls.load() - before == 1, what,
            "calls " + std::to_string(first) + ", then " +
                std::to_string(categoryCalls.load() - before - first));
    };
    setsOnceAfter("a media-services reset is set again once",
        AVAudioSessionCategorySoloAmbient, AVAudioSessionModeDefault, 0);
    setsOnceAfter("another mode is set again once",
        AVAudioSessionCategoryPlayback, AVAudioSessionModeMoviePlayback, 0);
    setsOnceAfter("other options are set again once",
        AVAudioSessionCategoryPlayback, AVAudioSessionModeDefault,
        AVAudioSessionCategoryOptionMixWithOthers);

    // The same while the guest records: the headset's options are asked for
    // again.
    Recording recording;
    check(recording.start(), "the microphone starts before the session changes");
    setsOnceAfter("a recording category without the headset options is set "
                  "again once",
        AVAudioSessionCategoryPlayAndRecord, AVAudioSessionModeDefault,
        AVAudioSessionCategoryOptionMixWithOthers |
            AVAudioSessionCategoryOptionDefaultToSpeaker |
            AVAudioSessionCategoryOptionAllowBluetoothA2DP);
    check((categoryOptions.load() &
              AVAudioSessionCategoryOptionAllowBluetoothHFP) != 0,
        "with the headset options", "options " +
            std::to_string(categoryOptions.load()));
    recording.stop();
    uml::audio::session::release(Client::playback);
}

// A session that refused the Bluetooth options is not asked for them again by
// the output restarting, which happens all the time while the guest records,
// but it is asked again when the microphone is built again after a route
// change, as a headset that connects may now allow them.
void refusedBluetoothOptionsAreAskedAgainWhenTheMicrophoneIsBuiltAgain()
{
    using uml::audio::session::Client;
    otherAudioPlaying = false;
    refuseBluetoothOptions = true;
    resetSession();
    (void)uml::audio::session::acquire(Client::playback);
    Recording recording;
    check(recording.start(), "the microphone starts without the headset options");
    AVAudioEngine* const engine = startedEngine;
    int before = categoryCalls.load();
    for (int restart = 0; restart < 3; ++restart)
        (void)uml::audio::session::acquire(Client::playback);
    uml::audio::session::release(Client::playback);
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before,
        "the output taking the session sets and asks for nothing",
        "calls " + std::to_string(categoryCalls.load() - before));

    before = categoryCalls.load();
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:engine];
    check(categoryCalls.load() == before + 1 && startedEngine != engine,
        "a route change asks for the headset options again, once, and "
        "keeps the category it holds",
        "calls " + std::to_string(categoryCalls.load() - before));

    refuseBluetoothOptions = false;
    before = categoryCalls.load();
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:startedEngine];
    const NSUInteger bluetooth =
        AVAudioSessionCategoryOptionAllowBluetoothHFP |
        AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    check(categoryCalls.load() == before + 1 &&
            (categoryOptions.load() & bluetooth) == bluetooth,
        "and a headset that allows them gets them at the next one",
        "calls " + std::to_string(categoryCalls.load() - before));
    before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before, "which the output keeps",
        "calls " + std::to_string(categoryCalls.load() - before));
    recording.stop();
    uml::audio::session::release(Client::playback);
}

// Another part of the application sets the session's category too (the
// picture-in-picture window does, as the app leaves the foreground), and the
// session lock does not cover it. When its call lands between ours and our
// reading of the session, what we read is its state, not the plan we set, and
// that must not be remembered as ours: the next acquire sets the category again.
void aChangeBetweenTheSetAndTheReadingIsNotRemembered()
{
    using uml::audio::session::Client;
    otherAudioPlaying = false;
    refuseBluetoothOptions = false;
    resetSession();

    // The recording category without the headset options, as the window sets it.
    afterCategorySet = [] {
        changeSession(AVAudioSessionCategoryPlayAndRecord,
            AVAudioSessionModeDefault,
            AVAudioSessionCategoryOptionMixWithOthers |
                AVAudioSessionCategoryOptionDefaultToSpeaker |
                AVAudioSessionCategoryOptionAllowBluetoothA2DP);
    };
    Recording recording;
    check(recording.start(), "the microphone starts under the other caller");
    afterCategorySet = nullptr;
    int before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    const NSUInteger headset = AVAudioSessionCategoryOptionAllowBluetoothHFP |
        AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    check(categoryCalls.load() == before + 1 &&
            (categoryOptions.load() & headset) == headset &&
            (sessionOptions.load() & headset) == headset,
        "the next acquire asks for the headset options again",
        "calls " + std::to_string(categoryCalls.load() - before));
    before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before, "and once they are held sets nothing",
        "calls " + std::to_string(categoryCalls.load() - before));
    recording.stop();
    uml::audio::session::release(Client::playback);

    // The playback category in the movie mode, as the window sets it.
    resetSession();
    afterCategorySet = [] {
        changeSession(AVAudioSessionCategoryPlayback,
            AVAudioSessionModeMoviePlayback,
            AVAudioSessionCategoryOptionMixWithOthers);
    };
    (void)uml::audio::session::acquire(Client::playback);
    afterCategorySet = nullptr;
    before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before + 1 &&
            [sessionMode isEqualToString:AVAudioSessionModeDefault],
        "a mode set in between is set again by the next acquire",
        "calls " + std::to_string(categoryCalls.load() - before));
    before = categoryCalls.load();
    (void)uml::audio::session::acquire(Client::playback);
    check(categoryCalls.load() == before, "and once it is held sets nothing",
        "calls " + std::to_string(categoryCalls.load() - before));
    uml::audio::session::release(Client::playback);
}

// The two ways the picture-in-picture window sets the session.
void windowSetsTheRecordingCategory()
{
    changeSession(AVAudioSessionCategoryPlayAndRecord,
        AVAudioSessionModeDefault,
        AVAudioSessionCategoryOptionMixWithOthers |
            AVAudioSessionCategoryOptionDefaultToSpeaker |
            AVAudioSessionCategoryOptionAllowBluetoothA2DP);
}

void windowSetsThePlaybackMovieMode()
{
    changeSession(AVAudioSessionCategoryPlayback,
        AVAudioSessionModeMoviePlayback,
        AVAudioSessionCategoryOptionMixWithOthers);
}

// The other caller can also land between two of the reads, which then come from
// two states: the category and the mode it did not change, with the options it
// did, and the like. A reading like that can agree with what was asked for,
// the playback plan asks for no options, so it may be remembered, and the next
// use finds the session in a state it did not leave and sets the category
// again: once, and then nothing more.
void aChangeBetweenTwoReadingsIsCaught()
{
    using uml::audio::session::Client;
    otherAudioPlaying = false;
    const NSUInteger headset = AVAudioSessionCategoryOptionMixWithOthers |
        AVAudioSessionCategoryOptionDefaultToSpeaker |
        AVAudioSessionCategoryOptionAllowBluetoothA2DP |
        AVAudioSessionCategoryOptionAllowBluetoothHFP |
        AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    const NSUInteger builtIn = AVAudioSessionCategoryOptionMixWithOthers |
        AVAudioSessionCategoryOptionDefaultToSpeaker |
        AVAudioSessionCategoryOptionAllowBluetoothA2DP;
    struct Plan final {
        const char* name;
        NSString* category;
        NSUInteger options;
    };
    const Plan plans[] = {
        {"playback", AVAudioSessionCategoryPlayback, 0},
        {"recording", AVAudioSessionCategoryPlayAndRecord, headset},
        {"recording without the headset options",
            AVAudioSessionCategoryPlayAndRecord, builtIn},
    };
    struct Writer final {
        const char* name;
        void (*write)();
    };
    const Writer writers[] = {
        {"recording category", windowSetsTheRecordingCategory},
        {"movie mode", windowSetsThePlaybackMovieMode},
    };
    const char* const points[] = {"the category", "the mode", "the options"};

    for (int plan = 0; plan < 3; ++plan) {
        for (const Writer& writer : writers) {
            for (int point = 1; point <= 3; ++point) {
                refuseBluetoothOptions = plan == 2;
                resetSession();
                tornWrite = writer.write;
                tornAfterRead = point;
                tornArmed = false;
                std::optional<Recording> recording;
                if (plan == 0) {
                    (void)uml::audio::session::acquire(Client::playback);
                } else {
                    recording.emplace();
                    check(recording->start(), "the microphone starts under "
                                              "the other caller");
                }
                tornWrite = nullptr;
                tornArmed = false;

                const std::string label = std::string(plans[plan].name) +
                    ", the " + writer.name + " after " + points[point - 1];
                const auto intended = [&] {
                    return [sessionCategory isEqualToString:
                                plans[plan].category] &&
                        [sessionMode isEqualToString:AVAudioSessionModeDefault] &&
                        sessionOptions.load() == plans[plan].options;
                };
                // A writer whose state is the plan's own leaves nothing to set.
                const int expected = intended() ? 0 : 1;
                int before = categoryCalls.load();
                (void)uml::audio::session::acquire(Client::playback);
                check(intended() && categoryCalls.load() - before == expected,
                    (label + ": the next acquire restores the plan").c_str(),
                    "calls " + std::to_string(categoryCalls.load() - before) +
                        ", expected " + std::to_string(expected));
                before = categoryCalls.load();
                (void)uml::audio::session::acquire(Client::playback);
                check(intended() && categoryCalls.load() == before,
                    (label + ": a further acquire sets nothing").c_str(),
                    "calls " + std::to_string(categoryCalls.load() - before));
                if (recording)
                    recording->stop();
                uml::audio::session::release(Client::playback);
            }
        }
    }
    refuseBluetoothOptions = false;
}

// A headset that connects or leaves changes the route under the running
// engine, which the microphone builds again. The session stays taken, because
// releasing it would return the category to playback, which switches a
// headset from the hands-free profile to A2DP and back (a route change each).
void routeChangeRebuildsTheEngineOnTheSameSession()
{
    otherAudioPlaying = false;
    refuseBluetoothOptions = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the route changes");
    AVAudioEngine* const engine = startedEngine;
    check(engine != nil, "the microphone started an engine");
    const auto before = Counts::now();
    const int callsBefore = categoryCalls.load();
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:engine];
    const auto seen = since(before);
    check(seen.stops == 1 && seen.starts == 1 && seen.deactivations == 0,
        "a route change builds the engine again without releasing the session",
        seen.text());
    // Taking the session again sets nothing: its category has not changed.
    check(categoryIsPlayAndRecord.load() && categoryCalls.load() == callsBefore,
        "and the category stays play-and-record, not set again",
        "category calls " + std::to_string(categoryCalls.load() - callsBefore));
    check(startedEngine != engine, "the engine is a new one");

    // The new engine is observed in its turn, and the old one no longer.
    AVAudioEngine* const rebuilt = startedEngine;
    const auto second = Counts::now();
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:engine];
    check(since(second) == Counts {},
        "the engine that was replaced is no longer observed",
        since(second).text());
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:rebuilt];
    check(since(second).starts == 1 && since(second).deactivations == 0,
        "the new engine is observed", since(second).text());

    const auto stopping = Counts::now();
    recording.stop();
    const auto stopped = since(stopping);
    check(stopped.stops == 1 && stopped.deactivations == 1,
        "stopping releases the session", stopped.text());
    const auto idle = Counts::now();
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:rebuilt];
    check(since(idle) == Counts {},
        "a route change after the guest stopped recording starts nothing",
        since(idle).text());
}

// A route change whose rebuild cannot take the session back (the system has
// not given the route back yet) leaves the microphone wanted and without an
// engine. The capture source asks it to revive while the guest records, and it
// opens the engine again, at most once a second. An interruption leaves the
// engine to the system's advice instead, and a stopped microphone opens
// nothing.
/* A route change reaches the microphone through a callback that takes the
 * microphone's lock. One that was entered before another app took the session
 * runs after the interruption began, and must not open the microphone: it
 * would take the session from the app that has it. Nor may one that was
 * entered for an engine that has since been replaced rebuild the engine that
 * replaced it. */
void aRouteChangeThatBeganBeforeAnInterruptionStartsNothing()
{
    otherAudioPlaying = false;
    refuseBluetoothOptions = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the late route change");
    const auto entered = routeChangeCallback;
    check(entered != nil, "the microphone listens for its engine's route");

    otherAudioPlaying = true;
    postInterruptionBegan();
    auto before = Counts::now();
    entered(nil);
    auto seen = since(before);
    check(seen == Counts {},
        "a route change that began before the interruption leaves the "
        "microphone closed while another app plays",
        seen.text());

    // The interruption ends and the microphone opens a new engine; a route
    // change that was entered for the old one does not touch the new one.
    postInterruptionEnded(true);
    AVAudioEngine* const resumed = startedEngine;
    before = Counts::now();
    entered(nil);
    seen = since(before);
    check(seen == Counts {} && startedEngine == resumed,
        "and one entered for the engine that was replaced leaves the new "
        "engine alone",
        seen.text());
    recording.stop();
}

void aRebuildThatCannotTakeTheSessionIsTriedAgain()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the route changes");
    AVAudioEngine* const engine = startedEngine;
    auto before = Counts::now();
    activationsToFail = 1;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioEngineConfigurationChangeNotification
                      object:engine];
    auto seen = since(before);
    check(seen.stops == 1 && seen.starts == 0 && seen.activations == 0,
        "a rebuild that cannot take the session leaves no engine", seen.text());

    before = Counts::now();
    recording.input->revive();
    check(since(before) == Counts {},
        "it is not tried again within a second", since(before).text());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recording.input->revive();
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "it is tried again after a second and runs", seen.text());

    before = Counts::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recording.input->revive();
    check(since(before) == Counts {},
        "a microphone that runs is left alone", since(before).text());

    // Interrupted: the system's advice decides, not a retry.
    postInterruptionBegan();
    before = Counts::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recording.input->revive();
    check(since(before) == Counts {},
        "an interrupted microphone waits for the advice, not a retry",
        since(before).text());
    postInterruptionEnded(true);
    check(since(before).starts == 1, "the advice opens it again",
        since(before).text());

    recording.stop();
    before = Counts::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recording.input->revive();
    check(since(before) == Counts {}, "a stopped microphone opens nothing",
        since(before).text());
}

// The system's advice to resume arrives while the route is not back yet, so
// the reopen fails. The interruption is over all the same: the microphone
// opens again, at most once a second, without waiting for another advice.
void aRefusedReopenAfterTheAdviceIsTriedAgain()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    auto before = Counts::now();
    activationsToFail = 1;
    postInterruptionEnded(true);
    auto seen = since(before);
    check(seen.starts == 0 && seen.activations == 0,
        "a reopen the system refuses leaves no engine", seen.text());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recording.input->revive();
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "the microphone opens again once the system allows it", seen.text());
    recording.stop();
}

#ifndef UML_AUDIO_LEGACY_SESSION
// The notification iOS 27 deprecates is no longer observed.
void deprecatedNotificationIsIgnored()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the old notification");
    const auto before = Counts::now();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionInterruptionNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionInterruptionTypeKey :
                            @(AVAudioSessionInterruptionTypeBegan)
                    }];
#pragma clang diagnostic pop
    const auto seen = since(before);
    check(seen == Counts {},
        "the deprecated interruption notification does nothing", seen.text());
}
#endif

} // namespace

int main()
{
    @autoreleasepool {
        installHooks();
        startsAndStops();
        deactivationsThatInterruptNothing();
        interruptionStopsTheMicrophone();
        adviceToResumeStartsIt();
        noAdviceWhileAnotherAppPlays();
        noAdviceWhileNoAppPlays();
        guestRestartsRecording();
        stoppedAndDestroyedMicrophones();
        sessionAllowsTheHeadsetMicrophone();
        refusedBluetoothOptionsFallBack();
        routeChangeRebuildsTheEngineOnTheSameSession();
        aRouteChangeThatBeganBeforeAnInterruptionStartsNothing();
        aRebuildThatCannotTakeTheSessionIsTriedAgain();
        aRefusedReopenAfterTheAdviceIsTriedAgain();
        sessionCategoryIsSetOnlyWhenItChanges();
        aSessionChangedBehindItsBackIsSetAgain();
        refusedBluetoothOptionsAreAskedAgainWhenTheMicrophoneIsBuiltAgain();
        aChangeBetweenTheSetAndTheReadingIsNotRemembered();
        aChangeBetweenTwoReadingsIsCaught();
#ifndef UML_AUDIO_LEGACY_SESSION
        deprecatedNotificationIsIgnored();
#endif
    }
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
        failures);
    return failures == 0 ? 0 : 1;
}
