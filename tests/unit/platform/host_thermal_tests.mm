// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Apple host thermal source (platform/host_thermal.hpp) says when the
 * system posts a change of the thermal state or of Low Power Mode: a notice is
 * posted to the default center, as the system does, and the caller's function
 * is called once for each; none is called once the caller has withdrawn, nor
 * after the provider ends.
 */
#import <Foundation/Foundation.h>

#include "host_thermal.hpp"
#include "require.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

using uml::testing::require;

void counted(void *context)
{
	static_cast<std::atomic<int> *>(context)->fetch_add(1);
}

/* Posts the notice from a thread of the test's own, which a block registered
 * with no queue runs on. */
void post(NSNotificationName name)
{
	std::thread poster{[name] {
		@autoreleasepool {
			[NSNotificationCenter.defaultCenter postNotificationName:name
				object:nil];
		}
	}};
	poster.join();
}

void a_notice_reaches_the_caller_until_it_withdraws()
{
	std::atomic<int> calls{0};
	std::unique_ptr<uml::platform::HostThermalProvider> provider =
		uml::platform::makeAppleHostThermalProvider();
	require(provider != nullptr);
	const uml::platform::HostThermalState reading = provider->state();
	require(static_cast<unsigned>(reading.level) <= 3, "a thermal level");

	/* Nothing is called before the caller asks. */
	post(NSProcessInfoThermalStateDidChangeNotification);
	require(calls.load() == 0, "a notice reached a caller that had not asked");

	provider->observe(&counted, &calls);
	post(NSProcessInfoThermalStateDidChangeNotification);
	require(calls.load() == 1, "a thermal notice did not reach the caller");
	post(NSProcessInfoPowerStateDidChangeNotification);
	require(calls.load() == 2, "a Low Power Mode notice did not reach the caller");

	/* Withdrawn: none, and asking again starts them again. */
	provider->observe(nullptr, nullptr);
	post(NSProcessInfoThermalStateDidChangeNotification);
	require(calls.load() == 2, "a notice reached a caller that had withdrawn");
	provider->observe(&counted, &calls);
	post(NSProcessInfoThermalStateDidChangeNotification);
	require(calls.load() == 3, "a notice did not reach a caller that asked again");

	/* The provider ends while the caller is still asking: no call after. */
	provider.reset();
	post(NSProcessInfoThermalStateDidChangeNotification);
	post(NSProcessInfoPowerStateDidChangeNotification);
	require(calls.load() == 3, "a notice reached the caller after the provider ended");
}

} // namespace

int main()
{
	a_notice_reaches_the_caller_until_it_withdraws();
	std::puts("host thermal tests passed");
	return 0;
}
