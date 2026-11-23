// SPDX-License-Identifier: Apache-2.0
//
// android.hardware.vibrator.IVibrator (AIDL version 4) on the host's haptics.
// The class only converts between the AIDL types and the player, which owns
// every decision (haptics_player.hpp), so it keeps no state of its own.
#pragma once

#include <aidl/android/hardware/vibrator/BnVibrator.h>

#include "haptics_player.hpp"

namespace aidl::android::hardware::vibrator {
class Vibrator : public BnVibrator {
public:
    explicit Vibrator(andarium::haptics::Player& player) : player_(player) {}

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs, const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* _aidl_return) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive, int32_t* _aidl_return) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;

    // Everything the HAL does not offer: always-on effects, the resonant
    // frequency and Q factor, frequency control, the PWLE envelopes and vendor
    // effects. Each answers EX_UNSUPPORTED_OPERATION, and none is advertised in
    // getCapabilities.
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* _aidl_return) override;
    ndk::ScopedAStatus getQFactor(float* _aidl_return) override;
    ndk::ScopedAStatus getFrequencyResolution(float* _aidl_return) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* _aidl_return) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* _aidl_return) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus performVendorEffect(const VendorEffect& vendorEffect,
                                           const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getFrequencyToOutputAccelerationMap(
            std::vector<FrequencyAccelerationMapEntry>* _aidl_return) override;
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMaxMillis(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getPwleV2CompositionSizeMax(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMinMillis(int32_t* _aidl_return) override;
    ndk::ScopedAStatus composePwleV2(const CompositePwleV2& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) override;

    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

private:
    andarium::haptics::Player& player_;
};
}  // namespace aidl::android::hardware::vibrator
