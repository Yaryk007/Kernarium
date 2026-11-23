// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "display_geometry.hpp"
#include "present_pacer.hpp"
#include "scanout_framebuffer.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace uml::gpu {

struct MetalPresentationState;

/* What a draw left owed. `unavailable`: the newest frame is owed and the view
 * had no drawable or render pass to draw it into (a transient failure), so it
 * is neither shown nor retired, and with no later frame nothing asks for it
 * again; a caller that paces its draws draws again. `done`: nothing is owed,
 * or the frame is shown or in flight, or it was retired. */
enum class DrawResult : std::uint8_t {
    done,
    unavailable,
};

/* A draw's result, and for an `unavailable` one how long the request for the
 * drawable blocked the calling thread (a layer waits up to a second for a
 * drawable that the display holds): that time alone, not a wait for any lock
 * of the presentation and not the setting up of the render pass. A caller
 * that draws again counts it, because each retry pays it again. `frame` is
 * the frame the draw claimed (0 when it claimed none): the frame whose
 * budget the wait is counted against. */
struct DrawOutcome {
    DrawResult result {DrawResult::done};
    double drawableWaitSeconds {};
    std::uint64_t frame {};

    DrawOutcome() = default;
    DrawOutcome(DrawResult drawResult, double waitSeconds = 0.0, std::uint64_t claimed = 0) noexcept
        : result(drawResult)
        , drawableWaitSeconds(waitSeconds)
        , frame(claimed)
    {
    }
};

// Common newest-frame-wins Metal timeline used by the AppKit and UIKit shells.
// Objective-C and Metal types remain inside the implementation.
class MetalPresentation final {
public:
    MetalPresentation(std::uint32_t width, std::uint32_t height);
    ~MetalPresentation();

    MetalPresentation(const MetalPresentation&) = delete;
    MetalPresentation& operator=(const MetalPresentation&) = delete;

    void setRedrawCallback(std::function<void()> callback);
    [[nodiscard]] void* device() const noexcept;
    [[nodiscard]] ScanoutFramebuffer& framebuffer() noexcept;
    [[nodiscard]] const ScanoutFramebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        bool requestRedraw = true) noexcept;
    [[nodiscard]] DisplayRotation rotation() const noexcept;
    /* Rotates only host presentation. The guest scanout dimensions and
     * contents remain unchanged. */
    [[nodiscard]] bool setRotation(DisplayRotation rotation) noexcept;
    /* Re-present the latest guest content after the host drawable changes.
     * This is independent of the guest frame timeline and therefore must not
     * make a later guest frame look superseded. */
    void drawableSizeChanged() noexcept;
    void publishCpuFrame() noexcept;
    [[nodiscard]] PresentOutcome present(
        const MetalScanout& scanout) noexcept;
    /* `budget` is what the draw may still wait for a drawable: it asks the
     * view for one only while the frame it claims has budget left. */
    DrawOutcome draw(
        void* nativeView,
        DrawableBudget budget = DrawableBudget::unlimited()) noexcept;
    /* The same into the next drawable of a CAMetalLayer, which need belong to
     * no window and then has no display link to ask for a frame: whoever owns
     * the layer calls this from the redraw callback. */
    void drawLayer(void* metalLayer) noexcept;
    [[nodiscard]] DisplayStatistics statistics() const noexcept;
    void recordCpuFallbackFrame() noexcept;
    /* A CPU update of the guest's pixels was accepted: one offered frame
     * (DisplayStatistics::offeredFrames), counted before the frame is
     * published. A direct scanout counts itself in present(). */
    void recordGuestUpdate() noexcept;
    void shutdown() noexcept;

private:
    std::shared_ptr<MetalPresentationState> state_;
};

} // namespace uml::gpu
