// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for cutils/sockets.h (see prelude.h): holder_test.cpp gives the socket init would.
#pragma once
extern "C" int android_get_control_socket(const char* name);
