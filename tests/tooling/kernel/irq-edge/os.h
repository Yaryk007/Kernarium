// SPDX-License-Identifier: GPL-2.0-or-later
void panic(const char *, ...) __attribute__((noreturn));
void os_set_thread_name(const char *name);
#define UM_NSEC_PER_SEC 1000000000ULL
long long os_nsecs(void);
