// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "input/virtio/hardware_input.hpp"
#include "input/vnc/vnc_input.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace uml::app {

/* --hardware-input: carries the display's keys and pointer to the guest's
 * hardware keyboard, mouse and pointer. The display reports them as it would
 * to the touchscreen, in its mouse form (uml::input::vnc): Linux key codes, an
 * absolute position over the guest's scanout in 0..32767 with the three
 * buttons, and wheel steps. A key plugs the keyboard and a pointer event plugs
 * the mouse, which the guest sees at once; when the last client of a VNC
 * display has gone both are unplugged. A native window has no clients, so
 * what it plugged stays plugged. Whatever the display sends goes to Android
 * as it arrives: the launcher reserves no key.
 *
 * It outlives the boots: each boot attaches the devices it made. */
class HardwareInputBridge final {
public:
	using Input = uml::input::virtio::HardwareInput;

	void attach(std::shared_ptr<Input> input)
	{
		std::lock_guard lock(mutex_);
		input_ = std::move(input);
		keyboard_plugged_ = false;
		mouse_plugged_ = false;
		reset_pointer();
	}

	/* The boot's devices are going away. */
	void detach() noexcept
	{
		std::shared_ptr<Input> input;
		{
			std::lock_guard lock(mutex_);
			input = std::move(input_);
		}
	}

	/* The size of the guest's scanout, which the pointer is relative to, or
	 * nothing while the guest has none; and what resumes a guest that
	 * suspended itself. Both may be called from any thread, and are called
	 * with the bridge's lock held, so they must not call it. */
	struct ScanoutMode {
		std::uint32_t width {};
		std::uint32_t height {};
	};
	using ScanoutModeFunction = std::function<std::optional<ScanoutMode>()>;
	void set_scanout_mode(ScanoutModeFunction function)
	{
		std::lock_guard lock(mutex_);
		scanout_mode_ = std::move(function);
	}
	void set_wake(std::function<void()> function)
	{
		std::lock_guard lock(mutex_);
		wake_ = std::move(function);
	}

	/* The display's input thread. */
	void deliver(std::span<const uml::input::vnc::VncInputEvent> events)
	{
		namespace vnc = uml::input::vnc;
		std::lock_guard lock(mutex_);
		if (!input_)
			return;
		bool wakes = false;
		bool pointer_changed = false;
		std::int32_t wheel = 0;
		std::int32_t horizontal_wheel = 0;
		const auto flush = [&] {
			if (pointer_changed && have_x_ && have_y_) {
				plug_mouse();
				const auto mode = scanout_mode_ ?
					scanout_mode_() : std::nullopt;
				wakes |= input_->pointer(
					x_, y_, buttons_, true,
					mode ? mode->width : 0U, mode ? mode->height : 0U);
			}
			if (wheel != 0 || horizontal_wheel != 0) {
				plug_mouse();
				wakes |= input_->mouse(
					0, 0, 0, wheel, horizontal_wheel);
			}
			pointer_changed = false;
			wheel = 0;
			horizontal_wheel = 0;
		};
		for (const auto &event : events) {
			switch (event.type) {
			case vnc::eventKey:
				if (const auto button = button_bit(event.code)) {
					const auto before = buttons_;
					buttons_ = event.value != 0 ?
						(buttons_ | button) : (buttons_ & ~button);
					pointer_changed |= buttons_ != before;
				} else if (event.code != vnc::buttonTouch) {
					plug_keyboard();
					wakes |= input_->key(event.code, event.value != 0);
				}
				break;
			case vnc::eventAbsolute:
				if (event.code == vnc::absoluteX) {
					x_ = clamp_position(event.value);
					have_x_ = true;
					pointer_changed = true;
				} else if (event.code == vnc::absoluteY) {
					y_ = clamp_position(event.value);
					have_y_ = true;
					pointer_changed = true;
				}
				break;
			case vnc::eventRelative:
				/* A detent, in the high-resolution units of the
				 * guest's wheels. */
				if (event.code == vnc::relativeWheel)
					wheel += event.value * wheel_detent;
				else if (event.code == vnc::relativeHorizontalWheel)
					horizontal_wheel += event.value * wheel_detent;
				break;
			case vnc::eventSync:
				flush();
				break;
			default:
				break;
			}
		}
		flush();
		if (wakes)
			wake();
	}

	/* A VNC display's number of clients changed. */
	void clients(std::uint32_t count) noexcept
	{
		if (count != 0)
			return;
		std::lock_guard lock(mutex_);
		if (!input_)
			return;
		if (keyboard_plugged_) {
			input_->setKeyboardConnected(false);
			keyboard_plugged_ = false;
			std::fputs("Hardware keyboard: unplugged\n", stderr);
		}
		if (mouse_plugged_) {
			input_->setMouseConnected(false);
			mouse_plugged_ = false;
			std::fputs("Hardware mouse: unplugged\n", stderr);
		}
		reset_pointer();
	}

	/* What the guest tells the host of the devices, on stderr, where the
	 * tests of the guest's side read it. */
	[[nodiscard]] static Input::StatusCallback report()
	{
		return [](uml::input::virtio::HardwareStatus status, bool on) {
			using Status = uml::input::virtio::HardwareStatus;
			const char *text = on ? "on" : "off";
			switch (status) {
			case Status::numLock:
				std::fprintf(stderr, "Hardware keyboard: Num Lock %s\n",
					     text);
				break;
			case Status::capsLock:
				std::fprintf(stderr, "Hardware keyboard: Caps Lock %s\n",
					     text);
				break;
			case Status::scrollLock:
				std::fprintf(stderr,
					     "Hardware keyboard: Scroll Lock %s\n", text);
				break;
			case Status::pointerCapture:
				std::fprintf(stderr, "Hardware mouse: pointer capture %s\n",
					     text);
				break;
			case Status::keyboardRegistered:
				std::fprintf(stderr, "Hardware keyboard: %s\n",
					     on ? "registered by the guest" :
						  "unregistered by the guest");
				break;
			case Status::mouseRegistered:
				std::fprintf(stderr, "Hardware mouse: %s\n",
					     on ? "registered by the guest" :
						  "unregistered by the guest");
				break;
			case Status::pointerRegistered:
				std::fprintf(stderr, "Hardware pointer: %s\n",
					     on ? "registered by the guest" :
						  "unregistered by the guest");
				break;
			}
		};
	}

private:
	/* The raw units of one wheel detent: a scroll step of the display. */
	static constexpr std::int32_t wheel_detent =
		uml::input::virtio::protocol::wheelDetent;

	/* The bit of a mouse button in the buttons the device library takes, or
	 * zero for a key. */
	static std::uint32_t button_bit(std::uint16_t code) noexcept
	{
		namespace vnc = uml::input::vnc;
		if (code == vnc::buttonLeft)
			return uml::input::virtio::buttonPrimary;
		if (code == vnc::buttonRight)
			return uml::input::virtio::buttonSecondary;
		if (code == vnc::buttonMiddle)
			return uml::input::virtio::buttonMiddle;
		return 0;
	}

	static std::uint32_t clamp_position(std::int32_t value) noexcept
	{
		return static_cast<std::uint32_t>(std::clamp(
			value, std::int32_t {0},
			static_cast<std::int32_t>(
				uml::input::vnc::absoluteMaximum)));
	}

	void reset_pointer() noexcept
	{
		buttons_ = 0;
		have_x_ = false;
		have_y_ = false;
	}

	void plug_keyboard()
	{
		if (keyboard_plugged_)
			return;
		keyboard_plugged_ = true;
		input_->setKeyboardConnected(true);
		std::fputs("Hardware keyboard: plugged\n", stderr);
	}

	void plug_mouse()
	{
		if (mouse_plugged_)
			return;
		mouse_plugged_ = true;
		input_->setMouseConnected(true);
		std::fputs("Hardware mouse: plugged\n", stderr);
	}

	void wake() noexcept
	{
		if (wake_)
			wake_();
	}

	std::mutex mutex_;
	ScanoutModeFunction scanout_mode_;
	std::function<void()> wake_;
	std::shared_ptr<Input> input_;
	bool keyboard_plugged_ {};
	bool mouse_plugged_ {};
	std::uint32_t buttons_ {};
	std::uint32_t x_ {};
	std::uint32_t y_ {};
	bool have_x_ {};
	bool have_y_ {};
};

} // namespace uml::app
