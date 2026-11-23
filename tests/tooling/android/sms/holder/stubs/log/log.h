// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for log/log.h (see prelude.h): holder_test.cpp keeps the lines for the test to read.
#pragma once
#ifndef LOG_TAG
#error "LOG_TAG is not defined"
#endif
extern "C" void holder_test_log(char level, const char* format, ...) __attribute__((format(printf, 2, 3)));
#define ALOGI(...) holder_test_log('I', __VA_ARGS__)
#define ALOGW(...) holder_test_log('W', __VA_ARGS__)
#define ALOGE(...) holder_test_log('E', __VA_ARGS__)
