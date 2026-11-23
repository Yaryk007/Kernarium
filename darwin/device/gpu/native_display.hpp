// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace uml::gpu {

struct NativeDisplayStateCore;

class NativeDisplay final : public Display {
public:
    NativeDisplay(
        std::uint32_t width,
        std::uint32_t height,
        uml::input::vnc::VncInputSink inputSink = {},
        void* nativeView = nullptr,
        uml::input::vnc::PointerMode pointerMode =
            uml::input::vnc::PointerMode::mouse,
        std::uint32_t maximumDimension =
            protocol::maximumDisplayDimension,
        bool autoResizeGuest = true,
        std::uint32_t refreshRate = 60);
    /* A display over a state the caller made, such as the windowless one
     * (offscreen_display.hpp). */
    explicit NativeDisplay(std::shared_ptr<NativeDisplayStateCore> state);
    ~NativeDisplay() override;

    /* The refresh rate a run is granted, decided once before the guest boots:
     * 120 when `requested` is 120 and every one of these holds, else 60. The
     * screen of `nativeView`'s own window scene (never another scene) shows at
     * least 100 frames a second (an iOS screen with a variable refresh rate;
     * 61 to 99 would give a 120 Hz mode that the display link cannot pace);
     * the host is an iPhone or iPad, not an iPad app on a Mac or on a Vision
     * Pro; and a mode of `width` x `height` can state 120 Hz in its EDID
     * (edid_timing.hpp). The caller states the answer to everything that
     * depends on it: the EDID and the display's rate, the host-feature bit and
     * the guest's boot property, the display link's pacing, and the
     * application's run state. */
    [[nodiscard]] static std::uint32_t grantedRefreshRate(
        std::uint32_t requested,
        void* nativeView,
        std::uint32_t width,
        std::uint32_t height) noexcept;

    NativeDisplay(const NativeDisplay&) = delete;
    NativeDisplay& operator=(const NativeDisplay&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] std::uint32_t refreshRate() const noexcept override;
    void noteModeRefreshRate(std::uint32_t rate) noexcept override;
    [[nodiscard]] int resizeEventDescriptor() const noexcept override;
    [[nodiscard]] bool drainResizeEvent() override;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override;
    void resetGuestSession() noexcept override;
    [[nodiscard]] bool claimResizeNotification() noexcept override;
    void releaseResizeNotification() noexcept override;
    void clear() noexcept override;
    void update(
        std::span<const std::uint8_t> source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::size_t sourceRowBytes,
        protocol::GpuFormat sourceFormat,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t width,
        std::uint32_t height) override;
    void fillSelfTestPattern() override;
    [[nodiscard]] PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept override;
    [[nodiscard]] bool needsFullCpuFallback() const noexcept override
    {
        return true;
    }
    [[nodiscard]] DisplayStatistics statistics() const noexcept override;
    void recordCpuFallbackFrame() noexcept override;

private:
    std::shared_ptr<NativeDisplayStateCore> state_;
};

} // namespace uml::gpu
