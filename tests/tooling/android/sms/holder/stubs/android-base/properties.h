// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for android-base/properties.h (see prelude.h): holder_test.cpp records the properties set.
#pragma once
#include <string>

namespace android {
namespace base {
bool SetProperty(const std::string& key, const std::string& value);
}  // namespace base
}  // namespace android
