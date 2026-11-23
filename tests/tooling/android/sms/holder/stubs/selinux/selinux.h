// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for selinux/selinux.h (see prelude.h): holder_test.cpp answers for the peer of a connection.
#pragma once
extern "C" int getpeercon(int fd, char** context);
extern "C" void freecon(char* context);
