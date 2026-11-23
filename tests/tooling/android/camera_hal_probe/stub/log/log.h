// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stand-in for an Android header that the camera HAL's EmulatedSensorHost.cpp includes (see ../stub/EmulatedSensor.h).
#pragma once
#include <cstdio>
#define ALOGE(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define ALOGW(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
