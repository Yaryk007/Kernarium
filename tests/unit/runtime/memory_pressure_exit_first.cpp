// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The first static initializer of memory_pressure_exit_tests: it registers the
 * handler that raises a level, and the target links this file's object before
 * the response's, so the handler runs after the response's statics are
 * destroyed (handlers and destructors run in the reverse of the order they
 * registered in).
 */
#include <cstdlib>

namespace uml::testing {
/* Defined by memory_pressure_exit_tests.cpp. */
void afterStaticDestruction();
} // namespace uml::testing

namespace {

struct RegisterFirst final {
	RegisterFirst() { std::atexit(&uml::testing::afterStaticDestruction); }
} register_first;

} // namespace
