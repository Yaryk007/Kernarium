// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "andarium.vibrator"

#include "Vibrator.hpp"

#include <log/log.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "haptics_effects.hpp"

namespace aidl::android::hardware::vibrator {
namespace {
using andarium::haptics::Outcome;

ndk::ScopedAStatus status_of(Outcome outcome) {
    switch (outcome) {
        case Outcome::ok:
            return ndk::ScopedAStatus::ok();
        case Outcome::illegal_argument:
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        case Outcome::unsupported:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

ndk::ScopedAStatus unsupported() {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

// The framework's callback as a function the player runs once the vibration is over.
andarium::haptics::Player::Done completion(const std::shared_ptr<IVibratorCallback>& callback) {
    if (!callback) return {};
    return [callback] {
        if (!callback->onComplete().isOk()) ALOGW("the completion callback failed");
    };
}
}  // namespace

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    // Core Haptics renders a tone at a given level, the primitives of a
    // composition and the predefined effects. It has no frequency response to
    // report, an audio-coupled mode or always-on effects.
    // Each constant is a member of its own anonymous enum, so each is converted
    // before the masks combine.
    *_aidl_return = int32_t(IVibrator::CAP_ON_CALLBACK) | int32_t(IVibrator::CAP_PERFORM_CALLBACK) |
                    int32_t(IVibrator::CAP_AMPLITUDE_CONTROL) | int32_t(IVibrator::CAP_COMPOSE_EFFECTS);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    player_.off();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs, const std::shared_ptr<IVibratorCallback>& callback) {
    return status_of(player_.on(timeoutMs, completion(callback)));
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    int32_t length = 0;
    const auto outcome = player_.perform(static_cast<int32_t>(effect), static_cast<int32_t>(strength),
                                         completion(callback), &length);
    if (outcome == Outcome::ok) *_aidl_return = length;
    return status_of(outcome);
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    _aidl_return->clear();
    for (const auto& entry : andarium::haptics::effects) {
        _aidl_return->push_back(static_cast<Effect>(entry.code));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    return status_of(player_.set_amplitude(amplitude));
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool) {
    return unsupported();
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* _aidl_return) {
    *_aidl_return = andarium::haptics::composition_delay_max_ms;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* _aidl_return) {
    *_aidl_return = int32_t(andarium::haptics::composition_size_max);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* _aidl_return) {
    _aidl_return->clear();
    for (const auto& entry : andarium::haptics::primitives) {
        _aidl_return->push_back(static_cast<CompositePrimitive>(entry.code));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive, int32_t* _aidl_return) {
    const auto duration = andarium::haptics::primitive_duration_ms(static_cast<int32_t>(primitive));
    if (!duration) return unsupported();
    *_aidl_return = int32_t(*duration);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    // The framework's limits are checked before anything is converted, so an
    // oversized request costs no allocation.
    if (composite.size() > andarium::haptics::composition_size_max) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::vector<andarium::haptics::CompositionStep> steps;
    steps.reserve(composite.size());
    for (const auto& element : composite) {
        steps.push_back({element.delayMs, static_cast<int32_t>(element.primitive), element.scale});
    }
    return status_of(player_.compose(steps, completion(callback)));
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t, Effect, EffectStrength) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getResonantFrequency(float*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getQFactor(float*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getFrequencyResolution(float*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>&,
                                         const std::shared_ptr<IVibratorCallback>&) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::performVendorEffect(const VendorEffect&,
                                                 const std::shared_ptr<IVibratorCallback>&) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getFrequencyToOutputAccelerationMap(
        std::vector<FrequencyAccelerationMapEntry>*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMaxMillis(int32_t*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getPwleV2CompositionSizeMax(int32_t*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMinMillis(int32_t*) {
    return unsupported();
}
ndk::ScopedAStatus Vibrator::composePwleV2(const CompositePwleV2&,
                                           const std::shared_ptr<IVibratorCallback>&) {
    return unsupported();
}

binder_status_t Vibrator::dump(int fd, const char**, uint32_t) {
    const auto text = player_.dump();
    // The descriptor belongs to the caller. A short write only shortens a
    // diagnostic, so the result is not an error.
    if (write(fd, text.data(), text.size()) < 0) ALOGW("dump not written");
    return STATUS_OK;
}
}  // namespace aidl::android::hardware::vibrator
