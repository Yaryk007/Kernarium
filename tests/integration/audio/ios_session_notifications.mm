// SPDX-License-Identifier: GPL-2.0-or-later
//
// The audio-session notifications a system posts when another app takes the
// session and gives it back, for the Simulator tests of the Apple sink and
// the Apple microphone. The Simulator never interrupts a session, so the tests
// post what an interruption would. UML_AUDIO_LEGACY_SESSION selects what
// systems before iOS 27 post: AVAudioSessionInterruptionNotification.

#include "ios_session_notifications.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#ifdef UML_AUDIO_LEGACY_SESSION

// iOS 27 deprecates the notification, which this variant exists for.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void postDeactivation(DeactivationSource, bool interrupted)
{
    if (interrupted)
        postInterruptionBegan();
}

void postInterruptionBegan()
{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionInterruptionNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionInterruptionTypeKey :
                            @(AVAudioSessionInterruptionTypeBegan)
                    }];
}

void postInterruptionEnded(bool shouldResume)
{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionInterruptionNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionInterruptionTypeKey :
                            @(AVAudioSessionInterruptionTypeEnded),
                        AVAudioSessionInterruptionOptionKey :
                            @(shouldResume ?
                                    AVAudioSessionInterruptionOptionShouldResume :
                                    0)
                    }];
}

#pragma clang diagnostic pop

#else

// iOS creates the contexts its notifications carry and offers no initializer,
// so each is a subclass that answers the reads the sink and the
// microphone make.
@interface InterruptionContextDouble : AVAudioSessionInterruptionContext
@end

@implementation InterruptionContextDouble
- (AVAudioSessionInterruptionReason)reason
{
    return AVAudioSessionInterruptionReasonDefault;
}
@end

@interface DeactivationContextDouble : AVAudioSessionDeactivationContext
@property(nonatomic) AVAudioSessionDeactivationSource sourceDouble;
@property(nonatomic, strong) AVAudioSessionInterruptionContext* interruptionDouble;
@end

@implementation DeactivationContextDouble
- (AVAudioSessionDeactivationSource)source
{
    return self.sourceDouble;
}

- (AVAudioSessionInterruptionContext*)interruptionContext
{
    return self.interruptionDouble;
}
@end

@interface ResumptionContextDouble : AVAudioSessionResumptionContext
@property(nonatomic) AVAudioSessionResumptionRecommendation recommendationDouble;
@end

@implementation ResumptionContextDouble
- (AVAudioSessionResumptionRecommendation)recommendation
{
    return self.recommendationDouble;
}
@end

namespace {

template <class Context> Context* make()
{
    return static_cast<Context*>(class_createInstance([Context class], 0));
}

} // namespace

void postDeactivation(DeactivationSource source, bool interrupted)
{
    auto* context = make<DeactivationContextDouble>();
    context.sourceDouble = source == DeactivationSource::app ?
        AVAudioSessionDeactivationSourceApp :
        AVAudioSessionDeactivationSourceSystem;
    context.interruptionDouble =
        interrupted ? make<InterruptionContextDouble>() : nil;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionDidBecomeInactiveNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionDeactivationContextKey : context
                    }];
}

void postInterruptionBegan()
{
    postDeactivation(DeactivationSource::system, true);
}

void postInterruptionEnded(bool shouldResume)
{
    auto* context = make<ResumptionContextDouble>();
    context.recommendationDouble = shouldResume ?
        AVAudioSessionResumptionRecommendationShouldResume :
        AVAudioSessionResumptionRecommendationShouldNotResume;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionResumptionRecommendationNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionResumptionContextKey : context
                    }];
}

#endif // UML_AUDIO_LEGACY_SESSION
