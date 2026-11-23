// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The runtime's refusal to resume (runtime/guest_sync): a disk that changed
 * while the guest was away ends the process, and the session record says why
 * for the next launch. The process aborts, so the case runs in a child whose
 * SIGABRT handler exits, which keeps the system's crash reporter out.
 */
#include "guest_files.hpp"
#include "guest_sync.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "session_record.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using uml::testing::require;

constexpr int abortedExit = 77;

std::string readFile(const std::string &path)
{
	std::string text;
	if (FILE *file = std::fopen(path.c_str(), "r"); file != nullptr) {
		char buffer[4096];
		std::size_t count;
		while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
			text.append(buffer, count);
		std::fclose(file);
	}
	return text;
}

bool has(const std::string &text, const std::string &part)
{
	return text.find(part) != std::string::npos;
}

void onAbort(int) noexcept
{
	_exit(abortedExit);
}

/* The child: a guest disk locked, released, written while released, and
 * restored. Returns only by exiting. */
[[noreturn]] void resumeOnAChangedDisk(const std::string &record,
	const std::string &disk)
{
	(void)std::signal(SIGABRT, onAbort);
	/* The refusal is also written to stderr, which this case does not show. */
	if (const int quiet = ::open("/dev/null", O_WRONLY); quiet >= 0)
		(void)::dup2(quiet, STDERR_FILENO);
	uml::runtime::reset_session_record();
	if (!uml::runtime::begin_session_record(record, {}))
		_exit(2);
	const int descriptor = ::open(disk.c_str(), O_RDWR | O_CREAT, 0600);
	struct flock lock {};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	if (descriptor < 0 ||
	    uml::platform::set_guest_file_lock(descriptor, F_SETLK, lock) != 0)
		_exit(3);
	if (uml::runtime::release_guest_file_locks() != 0)
		_exit(4);
	if (::pwrite(descriptor, "changed", 7, 0) != 7)
		_exit(5);
	uml::runtime::restore_guest_file_locks();
	_exit(6);
}

void aRefusedResumeIsRecordedBeforeTheAbort()
{
	const uml::testing::ScratchSubdirectory directory {
		"uml-resume-refusal.XXXXXX"};
	const std::string record = (directory.path() / "session").string();
	const std::string disk = (directory.path() / "disk").string();
	const pid_t child = ::fork();
	require(child >= 0, "fork");
	if (child == 0)
		resumeOnAChangedDisk(record, disk);
	int status = 0;
	require(::waitpid(child, &status, 0) == child, "wait for the child");
	require(WIFEXITED(status) && WEXITSTATUS(status) == abortedExit,
		"a resume on a disk that changed must abort the process");
	const std::string text = readFile(record);
	require(has(text, "state ended\nend failed\n") &&
		has(text, "message a guest disk changed or cannot be relocked on "
			"resume (errno " + std::to_string(ESTALE) + ")\n"),
		"the refusal was not in the session record: " + text);
}

} // namespace

int main()
{
	aRefusedResumeIsRecordedBeforeTheAbort();
	std::puts("guest resume refusal tests passed");
	return 0;
}
