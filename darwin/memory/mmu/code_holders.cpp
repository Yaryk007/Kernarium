// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_holders.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <thread>
#include <utility>

namespace uml::memory::code_holders {
namespace {

/* DARWIN_UML_CODE_HOLDER_COUNTERS=<path> writes the counters to <path> once a
 * second for as long as the process runs: a diagnostic for what the holder
 * words cost a guest. The emulator leaves through _Exit(), so the file is up
 * to a second old at the end. */
void write_counters(const std::string &path)
{
	const auto now = counters();
	std::FILE *out = std::fopen(path.c_str(), "w");
	if (out == nullptr)
		return;
	std::fprintf(out,
		"registrations=%llu overflows=%llu exchanges=%llu "
		"publications=%llu all_publications=%llu "
		"generations_published=%llu leaves=%llu leaf_bytes=%llu "
		"unknown_backings=%llu overlap_widenings=%llu "
		"absent_kernel_pages=%llu kernel_exchanges=%llu\n",
		static_cast<unsigned long long>(now.registrations),
		static_cast<unsigned long long>(now.overflows),
		static_cast<unsigned long long>(now.exchanges),
		static_cast<unsigned long long>(now.publications),
		static_cast<unsigned long long>(now.all_publications),
		static_cast<unsigned long long>(now.generations_published),
		static_cast<unsigned long long>(now.leaves),
		static_cast<unsigned long long>(now.leaves * sizeof(Leaf)),
		static_cast<unsigned long long>(now.unknown_backings),
		static_cast<unsigned long long>(now.overlap_widenings),
		static_cast<unsigned long long>(now.absent_kernel_pages),
		static_cast<unsigned long long>(now.kernel_exchanges));
	std::fclose(out);
}

/* Gives the calling thread back the signal mask it had, on every way out of
 * the start. */
class MaskRestore final {
public:
	explicit MaskRestore(const sigset_t &previous) noexcept
		: previous_{previous}
	{
	}
	MaskRestore(const MaskRestore &) = delete;
	MaskRestore &operator=(const MaskRestore &) = delete;
	~MaskRestore() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

private:
	sigset_t previous_;
};

bool start_report() noexcept
{
	const char *path = std::getenv("DARWIN_UML_CODE_HOLDER_COUNTERS");
	if (path == nullptr || *path == '\0')
		return false;
	try {
		/* Everything that can throw but the thread itself comes before the
		 * block. */
		std::string file{path};
		/* Host signals are the guest's interrupts and must never be
		 * delivered to this thread; it inherits the blocked mask. */
		sigset_t all;
		sigset_t previous;
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &previous);
		const MaskRestore restore{previous};
		std::thread{[file = std::move(file)] {
			for (;;) {
				write_counters(file);
				std::this_thread::sleep_for(std::chrono::seconds{1});
			}
		}}.detach();
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace

void report_counters_once() noexcept
{
	static const bool started = start_report();
	(void)started;
}

} // namespace uml::memory::code_holders
