// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>

namespace uml::platform {

/* The system's thermal state, as NSProcessInfo names it. */
enum class HostThermalLevel : std::uint8_t {
	nominal,
	fair,
	serious,
	critical,
};

struct HostThermalState final {
	HostThermalLevel level{HostThermalLevel::nominal};
	bool lowPowerMode{};
	/* The processor cores the system runs now; fewer than the device has
	 * while it holds some back. 0 where unknown. */
	std::uint8_t activeProcessors{};

	friend bool operator==(const HostThermalState &,
		const HostThermalState &) = default;
};

/* What the host says of its heat and power state. Reading it is cheap and
 * any thread may; nothing is cached, so a reading is the system's now. */
class HostThermalProvider {
public:
	virtual ~HostThermalProvider() = default;

	[[nodiscard]] virtual HostThermalState state() noexcept = 0;

	/* Calls `changed(context)` whenever the system says the thermal state or
	 * Low Power Mode changed, from a thread of the system's, never the
	 * caller's; the active core count has no such notice. A null `changed`
	 * ends it, and once the call returns no call is running or will start.
	 * A source with no notice never calls. */
	virtual void observe(void (*changed)(void *), void *context) noexcept
	{
		(void)changed;
		(void)context;
	}
};

[[nodiscard]] std::unique_ptr<HostThermalProvider>
makeAppleHostThermalProvider();

} // namespace uml::platform
