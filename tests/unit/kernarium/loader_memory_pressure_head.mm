// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The first part of the generated source of kernarium-loader-memory-pressure:
 * it registers the handler that forwards a report after static destruction
 * before the loader's own statics exist, so the handler runs after their
 * destructors (see loader_memory_pressure_tests.mm).
 */
#include <cstdlib>

extern "C" void loaderTestAfterStaticDestruction(void);

namespace {

struct RegisterFirst final {
    RegisterFirst() { std::atexit(&loaderTestAfterStaticDestruction); }
} registerFirst;

} // namespace

