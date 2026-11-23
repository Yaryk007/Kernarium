// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_thermal.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace uml::platform {
namespace {

/* What a notification calls. It outlives the provider that made it, so that
 * a notification in flight when the provider ends finds a lock and no call,
 * not freed memory. */
struct Notified final {
	std::mutex lock;
	void (*changed)(void *){};
	void *context{};

	void deliver() noexcept
	{
		std::lock_guard guard{lock};
		if (changed != nullptr)
			changed(context);
	}
};

class AppleHostThermalProvider final : public HostThermalProvider {
public:
	~AppleHostThermalProvider() override
	{
		observe(nullptr, nullptr);
		NSNotificationCenter *const center = NSNotificationCenter.defaultCenter;
		for (id token : tokens_)
			[center removeObserver:token];
	}

	void observe(void (*changed)(void *), void *context) noexcept override
	{
		{
			std::lock_guard guard{notified_->lock};
			notified_->changed = changed;
			notified_->context = context;
		}
		if (changed == nullptr || !tokens_.empty())
			return;
		/* On the posting thread: a block that does no more than ask the
		 * caller's function to hand the reading on. */
		const std::shared_ptr<Notified> notified = notified_;
		NSNotificationCenter *const center = NSNotificationCenter.defaultCenter;
		for (NSNotificationName name :
		     {NSProcessInfoThermalStateDidChangeNotification,
		      NSProcessInfoPowerStateDidChangeNotification}) {
			tokens_.push_back([center addObserverForName:name object:nil
				queue:nil usingBlock:^(NSNotification *) {
					notified->deliver();
				}]);
		}
	}

	HostThermalState state() noexcept override
	{
		@autoreleasepool {
			NSProcessInfo *info = NSProcessInfo.processInfo;
			HostThermalState result;
			switch (info.thermalState) {
			case NSProcessInfoThermalStateNominal:
				result.level = HostThermalLevel::nominal;
				break;
			case NSProcessInfoThermalStateFair:
				result.level = HostThermalLevel::fair;
				break;
			case NSProcessInfoThermalStateSerious:
				result.level = HostThermalLevel::serious;
				break;
			case NSProcessInfoThermalStateCritical:
				result.level = HostThermalLevel::critical;
				break;
			}
			result.lowPowerMode = info.lowPowerModeEnabled ? true : false;
			result.activeProcessors = static_cast<std::uint8_t>(std::min<NSUInteger>(
				info.activeProcessorCount, 255));
			return result;
		}
	}

private:
	std::shared_ptr<Notified> notified_{std::make_shared<Notified>()};
	std::vector<id> tokens_;
};

} // namespace

std::unique_ptr<HostThermalProvider> makeAppleHostThermalProvider()
{
	return std::make_unique<AppleHostThermalProvider>();
}

} // namespace uml::platform
