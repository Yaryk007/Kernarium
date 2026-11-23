// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The session record's state against a checkpoint that is delayed between its
 * last look at the session and its write. The test binary defines pwrite, which
 * the record's object resolves to, and holds the state write of the thread that
 * asks for it; every other write goes to the real one. An end is the last state
 * on disk once the checkpoint's repair has run, whether it waited for the
 * checkpoint (an ordinary end waits a bounded time) or did not (a crash). The
 * race is deterministic here: the checkpoint is stuck in its write while the
 * end runs. The instant between that write and its repair is accepted and not
 * tested.
 *
 * The same hooks count the writes of the state area and the file's truncations:
 * a state is one write of the whole area, the file is cut to its size once at a
 * session's begin, and a reader that races the writers never sees one state
 * mixed with another.
 */
#include "scratch_directory.hpp"
#include "session_record.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <exception>
#include <mutex>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::SessionEnd;
using uml::testing::require;
using namespace std::chrono_literals;

/* The thread that asks for its next state write to be held, and for how long. */
thread_local bool hold_state_write = false;
std::atomic<int> hold_ms{0};
std::atomic<bool> state_write_held{false};
std::atomic<bool> state_write_landed{false};

/* Counted since the last reset: the writes to the state's offset, those of
 * a size other than the area's, and the truncations of any file. */
std::atomic<unsigned> state_writes{0};
std::atomic<unsigned> state_writes_not_whole{0};
std::atomic<unsigned> truncations{0};

void reset_counts()
{
	state_writes.store(0);
	state_writes_not_whole.store(0);
	truncations.store(0);
}

std::string read_file(const std::string &path)
{
	std::string text;
	if (std::FILE *file = std::fopen(path.c_str(), "r"); file != nullptr) {
		char buffer[4096];
		std::size_t count;
		while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
			text.append(buffer, count);
		std::fclose(file);
	}
	return text;
}

/* The file as the application reads it for a bug report: one bounded read of
 * the whole file (`BoundedFile.read`), not a loop of reads. */
std::string read_file_once(const std::string &path)
{
	const int descriptor = open(path.c_str(), O_RDONLY);
	if (descriptor < 0)
		return {};
	std::string text(uml::runtime::session_record_max_bytes + 1, '\0');
	const ssize_t count = read(descriptor, text.data(), text.size());
	close(descriptor);
	text.resize(count > 0 ? static_cast<std::size_t>(count) : 0);
	return text;
}

bool has(const std::string &text, const char *part)
{
	return text.find(part) != std::string::npos;
}

/* The record's own descriptors, on a file that takes everything written. */
class Console final {
public:
	Console() : descriptor_{open("/dev/null", O_WRONLY)}
	{
		require(descriptor_ >= 0, "no console");
	}
	Console(const Console &) = delete;
	Console &operator=(const Console &) = delete;
	~Console() { close(descriptor_); }

	std::vector<std::string> arguments() const
	{
		return {"ssl0=fd:" + std::to_string(descriptor_)};
	}

	void write(const std::string &text) const
	{
		require(uml::runtime::kernel_write(descriptor_, text.data(),
			text.size()) == static_cast<ssize_t>(text.size()));
	}

private:
	int descriptor_;
};

/* Begins a session of a guest whose boot is unfinished, and starts a
 * checkpoint on a thread whose state write is held for `ms`. Returns once the
 * thread is inside the held write. */
std::thread checkpoint_held_for(int ms, const Console &console)
{
	state_write_held.store(false);
	state_write_landed.store(false);
	hold_ms.store(ms);
	std::thread checkpoint{[&console] {
		hold_state_write = true;
		/* The record's own checkpoint thread may hold the writer's flag
		 * now: ask again, with more output, until this thread's write
		 * is the held one. */
		for (int attempt = 0; attempt < 200 &&
			     !state_write_held.load(); ++attempt) {
			console.write("init: line " + std::to_string(attempt) + "\n");
			uml::runtime::checkpoint_session_memory();
		}
	}};
	for (int wait = 0; wait < 500 && !state_write_held.load(); ++wait)
		std::this_thread::sleep_for(10ms);
	require(state_write_held.load(), "the checkpoint did not reach its write");
	return checkpoint;
}

/* An ordinary end waits a bounded time for the checkpoint, which is held longer,
 * then writes: the checkpoint's late write is followed by the end's record. */
void an_end_outlasts_a_delayed_checkpoint(bool crash)
{
	const Console console;
	const uml::testing::ScratchSubdirectory directory{
		"uml-session-delayed.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("init: init first stage started!\n");
	std::thread checkpoint = checkpoint_held_for(300, console);
	if (crash)
		uml::runtime::record_kernel_crash();
	else
		uml::runtime::end_session_record(SessionEnd::stopped, 0);
	const std::string at_end = read_file(path);
	const bool end_came_first = !state_write_landed.load();
	checkpoint.join();
	const std::string after = read_file(path);
	const char *name = crash ? "state ended\nend kernel-abort\n" :
		"state ended\nend stopped\n";
	require(has(after, name) && !has(after, "state running\n"),
		std::string{"the delayed checkpoint overwrote the end: "} + after);
	require(state_write_landed.load(), "the held write never landed");
	/* The wait is bounded: the end did not wait out the delay. */
	require(end_came_first && has(at_end, name),
		"the end waited for a checkpoint that was held: " + at_end);
	require(after.size() == at_end.size(),
		"the record changed size after the end");
	uml::runtime::reset_session_record();
}

/* A session begun after an end is not repaired with the end before it. */
void the_next_session_begins_clean()
{
	const Console console;
	const uml::testing::ScratchSubdirectory directory{
		"uml-session-delayed.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	uml::runtime::end_session_record(SessionEnd::restarted, 0);
	uml::runtime::set_session_boot_facts(true, 1);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("init: init first stage started!\n");
	uml::runtime::checkpoint_session_memory();
	const std::string state = read_file(path);
	require(has(state, "state running\n") && !has(state, "state ended\n"),
		"the next session's checkpoint wrote the end before it: " + state);
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	require(has(read_file(path), "state ended\nend stopped\n"));
	uml::runtime::reset_session_record();
}

/* Whether a snapshot of the file is one state: the file is the record's whole
 * size, the area holds a single state followed by newlines only, a running
 * state of a completed boot quotes no output, and the quoted lines are consecutive ones of the
 * writer (each state's tail is a run of "init: line N" and perhaps the line of
 * the boot's end, so a state that is one's beginning and another's end breaks
 * the count). */
std::string state_problem(const std::string &file)
{
	if (file.size() != uml::runtime::session_record_max_bytes)
		return "the file is " + std::to_string(file.size()) + " bytes";
	std::string text = file.substr(uml::runtime::session_state_offset);
	while (text.ends_with("\n\n"))
		text.pop_back();
	if (!text.starts_with("state running\n") &&
	    !text.starts_with("state ended\n"))
		return "the state does not begin with its first line";
	if (text.find("\n\n") != std::string::npos)
		return "a blank line in the middle";
	if (text.find("\nstate ") != std::string::npos)
		return "two state lines";
	const std::size_t quoted = text.find("kernel-output\n");
	if (text.starts_with("state running\n") &&
	    has(text, "\nboot completed\n") && quoted != std::string::npos)
		return "a running state of a completed boot quotes output";
	if (quoted == std::string::npos)
		return {};
	long previous = -1;
	std::size_t at = quoted + 14;
	while (at < text.size()) {
		const std::size_t end = text.find('\n', at);
		const std::string line = text.substr(at, end == std::string::npos ?
			std::string::npos : end - at);
		if (line.starts_with("init: line ")) {
			const long number = std::strtol(line.c_str() + 11, nullptr, 10);
			if (previous >= 0 && number != previous + 1)
				return "output lines " + std::to_string(previous) +
					" then " + std::to_string(number);
			previous = number;
		} else if (!line.starts_with("init: processing action")) {
			return "an odd line in the output: " + line;
		}
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return {};
}

/* Every state is one write of the whole area, shorter and longer ones alike,
 * and nothing cuts the file after the session began. */
void every_state_is_written_whole()
{
	const Console console;
	const uml::testing::ScratchSubdirectory directory{
		"uml-session-whole.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	reset_counts();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	require(truncations.load() == 1 && state_writes.load() == 1,
		"a session's begin is one truncation and one state");
	require(read_file(path).size() == uml::runtime::session_record_max_bytes,
		"the file is not its whole size once the session began");
	reset_counts();
	std::string output;
	for (int line = 0; line < 400; ++line)
		output += "init: line " + std::to_string(line) + "\n";
	console.write(output);
	uml::runtime::checkpoint_session_memory();
	const std::string long_state = read_file(path);
	require(has(long_state, "kernel-output\ninit: line ") &&
		state_problem(long_state).empty(),
		"a long state is not whole: " + state_problem(long_state));
	console.write("init: processing action (sys.boot_completed=1) from (x)\n");
	uml::runtime::checkpoint_session_memory();
	const std::string short_state = read_file(path);
	require(has(short_state, "\nboot completed\n") &&
		!has(short_state, "kernel-output") &&
		short_state.size() == long_state.size() &&
		state_problem(short_state).empty(),
		"a shorter state left the longer one's end: " +
			state_problem(short_state));
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	const std::string ended = read_file(path);
	require(has(ended, "state ended\nend stopped\n") &&
		ended.size() == long_state.size() && state_problem(ended).empty(),
		"an end is not whole: " + state_problem(ended));
	require(state_writes.load() >= 3 && state_writes_not_whole.load() == 0,
		"a state was written in a part of its area: " +
			std::to_string(state_writes_not_whole.load()));
	require(truncations.load() == 0,
		"the file was cut after the session began");
	uml::runtime::reset_session_record();
}

/* A reader that reads the file as fast as it can, while a session begins, grows
 * its output, completes its boot and ends, again and again, sees one state
 * every time. The old writer shrank the file after each shorter state, and a
 * read between the write and the cut saw the new state's start followed by the
 * old state's end. The reader reads as the application does, with one bounded
 * read of the whole file: a reader that reads it in several parts (an `fread`
 * loop) can take the start of one state and the end of the next when a write
 * lands between its reads, however whole each write is, so it cannot tell a
 * mixed state of the writer from its own. The state is a long one (200 lines
 * of output). */
void a_reader_never_sees_two_states()
{
	const Console console;
	const uml::testing::ScratchSubdirectory directory{
		"uml-session-reader.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	std::atomic<bool> stop{false};
	std::atomic<unsigned> reads{0};
	std::mutex mutex;
	std::string problem;
	std::thread reader{[&] {
		while (!stop.load(std::memory_order_acquire)) {
			const std::string problem_now =
				state_problem(read_file_once(path));
			reads.fetch_add(1);
			if (!problem_now.empty()) {
				const std::lock_guard<std::mutex> lock{mutex};
				if (problem.empty())
					problem = problem_now;
			}
		}
	}};
	for (int round = 0; round < 60; ++round) {
		uml::runtime::reset_session_record();
		uml::runtime::set_session_boot_facts(true, round);
		require(uml::runtime::begin_session_record(path,
			console.arguments()));
		std::string output;
		for (int line = 0; line < 200; ++line)
			output += "init: line " + std::to_string(line) + "\n";
		console.write(output);
		uml::runtime::checkpoint_session_memory();
		console.write(
			"init: processing action (sys.boot_completed=1) from (x)\n");
		uml::runtime::checkpoint_session_memory();
		uml::runtime::end_session_record(round % 2 == 0 ?
			SessionEnd::stopped : SessionEnd::failed, round % 2);
	}
	stop.store(true, std::memory_order_release);
	reader.join();
	{
		const std::lock_guard<std::mutex> lock{mutex};
		require(problem.empty(), "a reader saw " + problem);
	}
	require(reads.load() > 100, "the reader did not run");
	uml::runtime::reset_session_record();
}

} // namespace

/* The record's object resolves pwrite and ftruncate here. */
extern "C" int ftruncate(int descriptor, off_t length)
{
	using Ftruncate = int (*)(int, off_t);
	static const Ftruncate real =
		reinterpret_cast<Ftruncate>(dlsym(RTLD_NEXT, "ftruncate"));
	truncations.fetch_add(1);
	return real(descriptor, length);
}

extern "C" ssize_t pwrite(int descriptor, const void *bytes, size_t size,
	off_t offset)
{
	using Pwrite = ssize_t (*)(int, const void *, size_t, off_t);
	static const Pwrite real =
		reinterpret_cast<Pwrite>(dlsym(RTLD_NEXT, "pwrite"));
	const bool state = offset ==
		static_cast<off_t>(uml::runtime::session_state_offset);
	if (state) {
		state_writes.fetch_add(1);
		if (size != uml::runtime::session_state_max_bytes)
			state_writes_not_whole.fetch_add(1);
	}
	if (!hold_state_write || !state)
		return real(descriptor, bytes, size, offset);
	hold_state_write = false;
	state_write_held.store(true);
	std::this_thread::sleep_for(std::chrono::milliseconds{hold_ms.load()});
	const ssize_t written = real(descriptor, bytes, size, offset);
	state_write_landed.store(true);
	return written;
}

int main()
{
	try {
		an_end_outlasts_a_delayed_checkpoint(false);
		an_end_outlasts_a_delayed_checkpoint(true);
		the_next_session_begins_clean();
		every_state_is_written_whole();
		a_reader_never_sees_two_states();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "delayed checkpoint test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("session record delayed checkpoint tests passed");
	return EXIT_SUCCESS;
}
