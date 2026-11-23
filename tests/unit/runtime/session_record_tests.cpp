// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The session record (runtime/session_record): the console descriptors the
 * kernel arguments name, the kernel output it keeps, the panic message it
 * finds, and the record file through a session's begin, its first end, a
 * kernel crash and an exit() of the process, which runs in a child.
 */
#include "session_record.hpp"
#include "require.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::SessionEnd;
using uml::testing::require;

constexpr std::size_t slot_bytes = uml::runtime::session_memory_slot_bytes;
constexpr std::size_t state_offset = uml::runtime::session_state_offset;

std::string read_file(const std::string &path)
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

std::string temporary_record()
{
	char path[] = "/tmp/uml-session-record-XXXXXX";
	const int descriptor = mkstemp(path);
	require(descriptor >= 0, "no temporary record");
	close(descriptor);
	return path;
}

std::string tail()
{
	std::vector<char> buffer(8192);
	return {buffer.data(), uml::runtime::kernel_output_tail(buffer)};
}

void test_names()
{
	require(uml::runtime::session_end_name(SessionEnd::running) == "running");
	require(uml::runtime::session_end_name(SessionEnd::guest_panic) ==
		"guest-panic");
	require(uml::runtime::session_end_name(
		SessionEnd::hibernation_restore_failed) ==
		"hibernation-restore-failed");
	require(uml::runtime::session_end_name(SessionEnd::process_exit) ==
		"process-exit");
	require(uml::runtime::session_end_name(SessionEnd::host_fault) ==
		"host-fault");
}

void test_console_descriptors()
{
	const std::vector<std::string> arguments{"console=ttyS0",
		"ssl0=fd:7,fd:7", "con=fd:0,fd:1", "con1=fd:3", "ssl=pts",
		"conx=fd:4", "ssl1=fd:-1", "ssl2=fd:9x", "mem=512M"};
	int found[4] = {-1, -1, -1, -1};
	require(uml::runtime::console_output_descriptors(arguments, found) == 3,
		"the console descriptors were miscounted");
	require(found[0] == 7 && found[1] == 1 && found[2] == 3,
		"a console's output descriptor was misread");
	int one[1] = {-1};
	require(uml::runtime::console_output_descriptors(arguments, one) == 1 &&
		one[0] == 7, "a short list was overrun");
}

void test_panic_message()
{
	using uml::runtime::panic_message;
	require(panic_message("no panic here\n").empty());
	require(panic_message("x\nDarwin UML panic: first\nDarwin UML panic: "
		"Attempted to kill init! exitcode=0x0000000b\nmore\n") ==
		"Attempted to kill init! exitcode=0x0000000b",
		"the last panic line was not the message");
	require(panic_message("Darwin UML panic: unterminated") ==
		"unterminated");
}

void test_kept_output()
{
	uml::runtime::reset_session_record();
	int console[2];
	require(pipe(console) == 0);
	int other[2];
	require(pipe(other) == 0);
	const std::vector<std::string> arguments{"ssl0=fd:" +
		std::to_string(console[1])};
	require(uml::runtime::begin_session_record({}, arguments),
		"a session without a record did not begin");
	require(uml::runtime::kernel_write(console[1], "console line\n", 13) ==
		13, "the console write did not reach its descriptor");
	require(uml::runtime::kernel_write(other[1], "elsewhere\n", 10) == 10);
	std::string kept = tail();
	require(kept == "console line\n", "the kept output is wrong: " + kept);
	/* More than the record keeps: the oldest bytes go. */
	std::string long_output(5000, 'a');
	long_output += "the end\n";
	for (std::size_t written = 0; written < long_output.size();) {
		const auto count = uml::runtime::kernel_write(console[1],
			long_output.data() + written,
			std::min<std::size_t>(512, long_output.size() - written));
		require(count > 0);
		written += static_cast<std::size_t>(count);
		char drain[512];
		(void)read(console[0], drain, sizeof(drain));
	}
	kept = tail();
	require(kept.size() == 4096 && kept.ends_with("aaaathe end\n"),
		"the kept output did not keep the newest bytes");
	for (const int descriptor : {console[0], console[1], other[0], other[1]})
		close(descriptor);
	uml::runtime::reset_session_record();
}

/* The console says an Android guest's boot finished: kernel_write reads it as
 * it passes, from a console and not from another descriptor, and a new session
 * forgets it. */
void test_the_console_says_when_the_boot_finished()
{
	uml::runtime::reset_session_record();
	int console[2];
	require(pipe(console) == 0);
	int other[2];
	require(pipe(other) == 0);
	const std::vector<std::string> arguments{"ssl0=fd:" +
		std::to_string(console[1])};
	require(uml::runtime::begin_session_record({}, arguments));
	require(!uml::runtime::console_boot_completed() &&
		!uml::runtime::console_init_observed(),
		"a new session was a finished boot or had seen init");
	const std::string line =
		"init: processing action (sys.boot_completed=1) from (x)\n";
	require(uml::runtime::kernel_write(other[1], line.data(), line.size()) ==
		static_cast<ssize_t>(line.size()));
	require(!uml::runtime::console_boot_completed(),
		"output that is no console's finished the boot");
	/* A write that splits the line. */
	const std::size_t cut = line.find("boot_");
	require(uml::runtime::kernel_write(console[1], line.data(), cut) ==
		static_cast<ssize_t>(cut));
	require(!uml::runtime::console_boot_completed(),
		"half a line finished the boot");
	require(uml::runtime::kernel_write(console[1], line.data() + cut,
		line.size() - cut) == static_cast<ssize_t>(line.size() - cut));
	require(uml::runtime::console_boot_completed() &&
		uml::runtime::console_init_observed(),
		"the console's line did not finish the boot");
	/* The next session begins from nothing. */
	require(uml::runtime::begin_session_record({}, arguments));
	require(!uml::runtime::console_boot_completed() &&
		!uml::runtime::console_init_observed(),
		"a finished boot outlived its session");
	uml::runtime::reset_session_record();
	for (const int descriptor : {console[0], console[1], other[0], other[1]})
		close(descriptor);
}

/* A console of the test: a descriptor the kernel arguments name, on a file
 * that takes everything written. */
class TestConsole final {
public:
	TestConsole() : descriptor_{open("/dev/null", O_WRONLY)}
	{
		require(descriptor_ >= 0, "no console");
	}
	TestConsole(const TestConsole &) = delete;
	TestConsole &operator=(const TestConsole &) = delete;
	~TestConsole() { close(descriptor_); }

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

/* Replaces the first bytes of the state, which no rewrite of it leaves. */
void scratch_the_state(const std::string &path)
{
	const int descriptor = open(path.c_str(), O_WRONLY);
	require(descriptor >= 0);
	require(pwrite(descriptor, "SCRATCH", 7, state_offset) == 7);
	close(descriptor);
}

bool state_was_rewritten(const std::string &path)
{
	return !read_file(path).substr(state_offset).starts_with("SCRATCH");
}

std::size_t file_size(const std::string &path)
{
	return read_file(path).size();
}

/* The state as the reader sees it: the writer pads the area to its full
 * size with newlines, so the text ends where they begin. */
std::string state_of(const std::string &path)
{
	std::string state = read_file(path).substr(state_offset);
	while (state.ends_with("\n\n"))
		state.pop_back();
	return state;
}

void test_boot_state()
{
	const std::string stuck = "init: init first stage started!\n"
		"init: starting service 'netd'\n";
	const std::string done =
		"init: processing action (sys.boot_completed=1) from (x)\n";
	const TestConsole console;

	/* A guest that reports its boot: the state says where the boot is, the
	 * checkpoints keep the output, and a completed boot drops it. Until
	 * init's output is seen the boot is unobserved, not unfinished. */
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 3);
	std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	std::string state = state_of(path);
	require(state.starts_with("state running\nend running\nstatus 0\npid ") &&
		has(state, "\nboot unobserved\nrestarts 3\n") &&
		!has(state, "kernel-output"),
		"a begun boot was recorded wrong: " + state);
	uml::runtime::checkpoint_session_memory();
	scratch_the_state(path);
	uml::runtime::checkpoint_session_memory();
	require(!state_was_rewritten(path), "a silent boot was written again");
	console.write(stuck);
	uml::runtime::checkpoint_session_memory();
	state = state_of(path);
	require(state.starts_with("state running\n") &&
		has(state, "\nboot unfinished\nrestarts 3\n") &&
		state.ends_with("kernel-output\n" + stuck),
		"a checkpoint did not keep the output: " + state);
	scratch_the_state(path);
	uml::runtime::checkpoint_session_memory();
	require(!state_was_rewritten(path),
		"output that did not grow was written again");
	console.write(done);
	uml::runtime::checkpoint_session_memory();
	state = state_of(path);
	require(state.starts_with("state running\n") &&
		has(state, "\nboot completed\nrestarts 3\n") &&
		!has(state, "kernel-output") && state.ends_with("\n") &&
		file_size(path) == uml::runtime::session_record_max_bytes,
		"a completed boot was recorded wrong: " + state);
	scratch_the_state(path);
	console.write(stuck);
	uml::runtime::checkpoint_session_memory();
	require(!state_was_rewritten(path), "a completed boot was written again");
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	state = state_of(path);
	require(has(state, "state ended\nend stopped\n") &&
		has(state, "\nboot completed\nrestarts 3\n") &&
		!has(state, "kernel-output"),
		"a stop after the boot was recorded wrong: " + state);
	unlink(path.c_str());

	/* The ends that a person or the guest chooses quote the output while the
	 * boot is unfinished, and only then. */
	for (const SessionEnd end : {SessionEnd::stopped, SessionEnd::restarted,
		     SessionEnd::powered_off}) {
		path = temporary_record();
		uml::runtime::reset_session_record();
		uml::runtime::set_session_boot_facts(true, 2);
		require(uml::runtime::begin_session_record(path,
			console.arguments()));
		console.write(stuck);
		uml::runtime::end_session_record(end, 0);
		state = state_of(path);
		require(has(state, "state ended\nend ") &&
			has(state, "\nboot unfinished\nrestarts 2\n") &&
			state.ends_with("kernel-output\n" + stuck),
			"an end during the boot was recorded wrong: " + state);
		unlink(path.c_str());
	}

	/* A guest that does not report its boot has neither line and no
	 * checkpoint of the state. */
	path = temporary_record();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(false, 4);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	state = state_of(path);
	require(!has(state, "boot ") && !has(state, "restarts "),
		"a Linux guest's record named a boot: " + state);
	scratch_the_state(path);
	console.write(stuck);
	uml::runtime::checkpoint_session_memory();
	require(!state_was_rewritten(path),
		"the state of a guest that reports no boot was checkpointed");
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	require(!has(read_file(path), "kernel-output"),
		"a stop quoted the output of a guest that reports no boot");
	unlink(path.c_str());
	uml::runtime::reset_session_record();
}

/* A console that shows nothing of init, such as a kernel's lines alone, is
 * not an unfinished boot: the record says so, and still keeps the output. */
void test_boot_unobserved()
{
	const std::string kernel = "Linux version 6.18\n";
	const std::string init = "init: init first stage started!\n";
	const TestConsole console;
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write(kernel);
	uml::runtime::checkpoint_session_memory();
	std::string state = state_of(path);
	require(has(state, "\nboot unobserved\nrestarts 0\n") &&
		state.ends_with("kernel-output\n" + kernel),
		"output without init's was recorded wrong: " + state);
	console.write(init);
	uml::runtime::checkpoint_session_memory();
	state = state_of(path);
	require(has(state, "\nboot unfinished\nrestarts 0\n") &&
		state.ends_with("kernel-output\n" + kernel + init),
		"init's first line did not make the boot unfinished: " + state);
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write(kernel);
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	state = state_of(path);
	require(has(state, "state ended\nend stopped\n") &&
		has(state, "\nboot unobserved\n") &&
		state.ends_with("kernel-output\n" + kernel),
		"a stop of an unobserved boot was recorded wrong: " + state);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The state names the ramdisk the boot used, so that an application reads from
 * the record alone whether a boot that did not finish was the root ramdisk's.
 * It is the boot's own: the facts of the next boot forget it. */
void test_the_record_names_the_boots_ramdisk()
{
	const TestConsole console;
	const std::string stuck = "init: init first stage started!\n";
	for (const auto &[ramdisk, name] :
	     {std::pair{uml::runtime::BootRamdisk::stock, "stock"},
		     std::pair{uml::runtime::BootRamdisk::root, "root"},
		     std::pair{uml::runtime::BootRamdisk::recovery, "recovery"}}) {
		uml::runtime::reset_session_record();
		uml::runtime::set_session_boot_facts(true, 1);
		uml::runtime::set_session_boot_ramdisk(ramdisk);
		const std::string path = temporary_record();
		require(uml::runtime::begin_session_record(path,
			console.arguments()));
		std::string state = state_of(path);
		const std::string line = std::string("\nboot unobserved\nrestarts 1\nramdisk ") +
			name + "\n";
		require(has(state, line),
			std::string("a begun boot did not name its ramdisk ") + name +
			": " + state);
		/* The end keeps it, with and without the output. */
		console.write(stuck);
		uml::runtime::end_session_record(SessionEnd::stopped, 0);
		state = state_of(path);
		require(has(state, std::string("\nramdisk ") + name + "\n"),
			std::string("an ended boot lost its ramdisk ") + name + ": " + state);
		unlink(path.c_str());
	}

	/* A boot whose ramdisk was never chosen, and the next boot after one that
	 * was, say none; a guest that reports no boot says none whatever is set. */
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	uml::runtime::set_session_boot_ramdisk(uml::runtime::BootRamdisk::root);
	uml::runtime::set_session_boot_facts(true, 1);
	std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	require(!has(state_of(path), "ramdisk "),
		"a boot inherited the ramdisk of the one before");
	unlink(path.c_str());
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(false, 0);
	uml::runtime::set_session_boot_ramdisk(uml::runtime::BootRamdisk::root);
	path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	require(!has(state_of(path), "ramdisk "),
		"a guest that reports no boot named a ramdisk");
	unlink(path.c_str());
}

/* The restart count and the boot belong to their session: the next one
 * starts unobserved again, whatever the one before did. */
void test_boot_state_per_session()
{
	const std::string done =
		"init: processing action (sys.boot_completed=1) from (x)\n";
	const TestConsole console;
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write(done);
	uml::runtime::checkpoint_session_memory();
	require(has(read_file(path), "\nboot completed\nrestarts 0\n"));
	uml::runtime::end_session_record(SessionEnd::restarted, 0);
	uml::runtime::set_session_boot_facts(true, 1);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	const std::string state = state_of(path);
	require(state.starts_with("state running\n") &&
		has(state, "\nboot unobserved\nrestarts 1\n") &&
		!has(state, "kernel-output"),
		"a restarted boot began finished: " + state);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* A boot's record quotes the output of its own boot. The console of a boot
 * that completed is left out of its orderly end; the unfinished record of the
 * boot after it, which is checkpointed and ended with the output so far, must
 * not bring it back. */
void test_a_boot_quotes_only_its_own_output()
{
	const TestConsole console;
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("init: processing action (sys.boot_completed=1) from (x)\n"
		"PREVIOUS_BOOT_CANARY\n");
	uml::runtime::checkpoint_session_memory();
	uml::runtime::end_session_record(SessionEnd::restarted, 0);
	require(!has(read_file(path), "PREVIOUS_BOOT_CANARY"),
		"a completed boot's end quoted its console");
	uml::runtime::set_session_boot_facts(true, 1);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	char tail[uml::runtime::session_kernel_output_bytes];
	require(uml::runtime::kernel_output_tail(tail) == 0,
		"a new boot held the boot before's output");
	console.write("init: init first stage started!\n");
	uml::runtime::checkpoint_session_memory();
	std::string state = state_of(path);
	require(has(state, "\nboot unfinished\n") &&
		has(state, "init first stage started") &&
		!has(state, "PREVIOUS_BOOT_CANARY"),
		"the unfinished boot quoted the boot before: " + state);
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	state = state_of(path);
	require(has(state, "state ended\nend stopped\n") &&
		has(state, "init first stage started") &&
		!has(state, "PREVIOUS_BOOT_CANARY"),
		"the unfinished boot's end quoted the boot before: " + state);
	/* A boot whose first line is a crash does not take the one before into
	 * its tail either. */
	uml::runtime::set_session_boot_facts(true, 2);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("Darwin UML panic: short\n");
	uml::runtime::end_session_record(SessionEnd::failed, 1, "x");
	state = state_of(path);
	require(has(state, "Darwin UML panic: short") &&
		!has(state, "init first stage started"),
		"a later boot's end quoted an earlier boot: " + state);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The record starts a quoted tail at a whole line only where the ring cut it
 * (the kernel echoes a serial number in its command line, which the masking of
 * the tail cannot find in the middle of a line). A boot that follows one that
 * overflowed the ring begins at its own first line, and keeps it. */
void test_a_short_boot_after_a_long_one_keeps_its_first_line()
{
	const TestConsole console;
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	std::string long_boot;
	while (long_boot.size() < 3 * uml::runtime::session_kernel_output_bytes)
		long_boot += "init: a line of the long boot\n";
	console.write(long_boot);
	uml::runtime::end_session_record(SessionEnd::restarted, 0);
	uml::runtime::set_session_boot_facts(true, 1);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("FIRST_LINE_OF_THE_SHORT_BOOT\n"
		"init: init first stage started!\n");
	uml::runtime::checkpoint_session_memory();
	std::string state = state_of(path);
	require(has(state, "kernel-output\nFIRST_LINE_OF_THE_SHORT_BOOT\n") &&
		!has(state, "a line of the long boot"),
		"the short boot's first line was dropped or the long boot's quoted: " + state);
	/* Its own output that overflows the ring is cut: the record starts at
	 * the first whole line the ring kept, and never in the middle of one. */
	console.write(long_boot);
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	state = state_of(path);
	require(has(state, "kernel-output\ninit: a line of the long boot\n") &&
		!has(state, "FIRST_LINE_OF_THE_SHORT_BOOT"),
		"a boot that overflowed the ring did not start at a whole line: " + state);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The state of an unfinished boot is bounded like the end's, whatever the
 * console writes. */
void test_boot_state_size()
{
	const TestConsole console;
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 4294967295U);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, console.arguments()));
	/* Lines, in one write longer than the ring: the record quotes the lines
	 * the ring kept, from the first whole one. */
	std::string output;
	while (output.size() < 3 * uml::runtime::session_kernel_output_bytes)
		output += std::string(63, '\x01') + '\n';
	console.write(output);
	uml::runtime::checkpoint_session_memory();
	require(file_size(path) <= uml::runtime::session_record_max_bytes &&
		file_size(path) > state_offset + 4096,
		"an unfinished boot's state is not bounded: " +
			std::to_string(file_size(path)));
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	require(file_size(path) <= uml::runtime::session_record_max_bytes);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* An end is the last word on the state: a checkpoint that is running while
 * the session ends, or that comes after, leaves the end alone. */
void test_the_end_outlasts_state_checkpoints()
{
	const TestConsole console;
	for (int round = 0; round < 300; ++round) {
		uml::runtime::reset_session_record();
		uml::runtime::set_session_boot_facts(true, 0);
		const std::string path = temporary_record();
		require(uml::runtime::begin_session_record(path,
			console.arguments()));
		console.write("init: init first stage started!\n");
		std::atomic<bool> stop{false};
		std::thread checkpoints{[&] {
			std::uint64_t counter = 0;
			while (!stop.load(std::memory_order_acquire)) {
				console.write("init: line " + std::to_string(++counter) +
					"\n");
				uml::runtime::checkpoint_session_memory();
			}
		}};
		for (int spin = 0; spin < round % 50; ++spin)
			std::this_thread::yield();
		uml::runtime::end_session_record(SessionEnd::stopped, 0);
		const std::string just_after = state_of(path);
		stop.store(true, std::memory_order_release);
		checkpoints.join();
		const std::string state = state_of(path);
		require(has(just_after, "state ended\nend stopped\n") &&
			has(state, "state ended\nend stopped\n") &&
			has(state, "\nboot unfinished\n"),
			"a checkpoint overwrote the end in round " +
				std::to_string(round) + ": " + state);
		/* The file holds one record, whatever order the writes came in. */
		require(file_size(path) == uml::runtime::session_record_max_bytes &&
			state.find('\0') == std::string::npos,
			"the state is not one record in round " +
				std::to_string(round));
		uml::runtime::reset_session_record();
		unlink(path.c_str());
	}
}

void test_record_lifecycle()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	require(record.starts_with("uml-session 8\nmemory at ") &&
		record.size() > state_offset && record[slot_bytes - 1] == '\n' &&
		record.substr(state_offset).starts_with(
			"state running\nend running\n"),
		"a begun session is not running: " + record);
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	record = read_file(path);
	require(has(record, "state ended\nend powered-off\nstatus 0\n") &&
		has(record, "\nended ") && !has(record, "kernel-output"),
		"a power-off was recorded wrong: " + record);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143);
	require(read_file(path).substr(state_offset) == record.substr(state_offset),
		"a second end replaced the first");

	/* A restart begins the next session in the same file. */
	require(uml::runtime::begin_session_record(path, {}));
	require(has(read_file(path), "state running\n"));
	const char line[] = "Darwin: the kernel says goodbye\n";
	(void)uml::runtime::kernel_write(STDERR_FILENO, line, sizeof(line) - 1);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143,
		"exit\nwith\x01" "control");
	record = read_file(path);
	require(has(record, "end kernel-exit\nstatus 143\n") &&
		has(record, "message exit?with?control\n") &&
		has(record, "kernel-output\n") &&
		has(record, "the kernel says goodbye\n"),
		"a kernel exit was recorded wrong: " + record);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The kernel echoes its command line, which holds the Android guest's serial
 * number; the record keeps the output but not that value, and neither does
 * what a bug report archive copies from it. The values are synthetic. */
void test_the_record_keeps_no_serial_number()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const char echo[] =
		"Kernel command line: quiet androidboot.serialno=TESTSERIAL0123 "
		"androidboot.hardware=cutf_cvm\n"
		"init: ro.boot.serialno=TESTSERIAL0123\n"
		"two on a line androidboot.serialno=\"FIRSTVALUE\" "
		"androidboot.serialno=SECONDVALUE\n"
		"the last: androidboot.serialno=LASTVALUE";
	(void)uml::runtime::kernel_write(STDERR_FILENO, echo, sizeof(echo) - 1);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143, "exit");
	const std::string record = read_file(path);
	require(has(record, "kernel-output\n") &&
		has(record, "Kernel command line: quiet androidboot.serialno=<masked> "
			    "androidboot.hardware=cutf_cvm\n") &&
		has(record, "init: ro.boot.serialno=<masked>\n") &&
		has(record, "the last: androidboot.serialno=<masked>"),
		"the tail was not kept with the serial masked: " + record);
	for (const char *value : {"TESTSERIAL0123", "FIRSTVALUE", "SECONDVALUE",
				  "LASTVALUE"})
		require(!has(record, value),
			std::string{"the record keeps a serial number: "} + value);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The ring keeps the last bytes of the output, so after a wrap its first line
 * is cut at any byte: in the middle of `androidboot.serialno=` or of its value.
 * The record starts at the first complete line, with the output as one write
 * longer than the ring (which keeps its end) and as many small ones. */
void test_a_wrapped_ring_keeps_no_serial_number()
{
	constexpr std::size_t ring = uml::runtime::session_kernel_output_bytes;
	const std::string key = "androidboot.serialno=";
	const std::string serial = "TESTSERIAL0123456";
	const std::string echo =
		"Kernel command line: quiet " + key + serial + " next\n";
	const std::size_t at = echo.find(key);
	const std::string marker = "\nring end marker\n";
	for (const bool one_write : {true, false}) {
		/* The ring starts `cut` bytes into the echo: before the key,
		 * inside it, at its last byte, at the value, inside the value
		 * and just after it. */
		for (const std::size_t cut :
		     {std::size_t{0}, std::size_t{3}, at - 1, at, at + 1,
		      at + 12, at + 13, at + 20, at + key.size(),
		      at + key.size() + 4, at + key.size() + serial.size() - 1,
		      at + key.size() + serial.size()}) {
			std::string input = echo;
			const std::size_t padding =
				ring + cut - echo.size() - marker.size();
			for (std::size_t line = 0; line < padding / 32; ++line)
				input += std::string(31, 'y') + '\n';
			input += std::string(padding % 32, 'y');
			input += marker;
			require(input.size() == ring + cut, "a wrong test input");
			uml::runtime::reset_session_record();
			const std::string path = temporary_record();
			require(uml::runtime::begin_session_record(path, {}));
			if (one_write) {
				(void)uml::runtime::kernel_write(
					STDERR_FILENO, input.data(), input.size());
			} else {
				for (std::size_t offset = 0; offset < input.size();
				     offset += 64)
					(void)uml::runtime::kernel_write(
						STDERR_FILENO, input.data() + offset,
						std::min<std::size_t>(64, input.size() - offset));
			}
			uml::runtime::end_session_record(SessionEnd::kernel_exit, 143,
				"exit");
			const std::string record = read_file(path);
			const std::string where = (one_write ? " (one write, cut " :
							       " (small writes, cut ") +
				std::to_string(cut) + "): " + record;
			for (const char *part : {"TESTSERIAL", "SERIAL0123", "0123456"})
				require(!has(record, part),
					std::string{"the record keeps part of the serial number "} +
						part + where);
			require(has(record, "ring end marker\n"),
				"the record lost the end of the output" + where);
			/* A first line that is cut is dropped whole, never kept
			 * as the part that is left of it. */
			require(!has(record, "kernel-output\nrmand") &&
				!has(record, "kernel-output\n" + serial),
				"the record starts inside a line" + where);
			uml::runtime::reset_session_record();
			unlink(path.c_str());
		}
	}
	/* The ring that dropped nothing keeps its first line, even when the output
	 * fills it exactly. */
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string full = echo;
	full.resize(ring, 'y');
	(void)uml::runtime::kernel_write(STDERR_FILENO, full.data(), full.size());
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143, "exit");
	const std::string record = read_file(path);
	require(has(record, "kernel-output\nKernel command line: quiet " + key +
			    "<masked> next\n") && !has(record, serial),
		"a ring that dropped nothing lost its first line: " + record);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* Made-up values: nothing here belongs to an installation. */
const std::string made_up_key =
	"ZmFrZS1rZXktZm9yLXRlc3RzLW9ubHkrL0FCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFla0123456789==";
const std::string made_up_serial = "TESTSERIAL0123";
const std::string made_up_zone = "Test/Nowhere";
const std::string made_up_address = "02:ab:cd:00:11:22";

/* The kernel arguments through which the runtime states them. */
std::vector<std::string> identity_arguments()
{
	return {"quiet", "androidboot.serialno=" + made_up_serial,
		"androidboot.kernarium.adb_key=" + made_up_key,
		"androidboot.kernarium.timezone=" + made_up_zone,
		"androidboot.hardware=test"};
}

/* No eight characters of `value` in a row: the record keeps none of it. */
void require_no_part_of(const std::string &record, const std::string &value,
	const std::string &where)
{
	for (std::size_t at = 0; at + 8 <= value.size(); ++at)
		require(!has(record, value.substr(at, 8)),
			"the record keeps part of a value (" + value.substr(at, 8) +
				")" + where);
}

void require_no_identity(const std::string &record, const std::string &where)
{
	require_no_part_of(record, made_up_key, where);
	require_no_part_of(record, made_up_serial, where);
	require_no_part_of(record, made_up_zone, where);
	require_no_part_of(record, made_up_address, where);
}

/* The kernel echoes its command line, which carries the guest's serial
 * number, the host's adb key and the time zone it was given, and init prints
 * a property it cannot set with the value: the record keeps the output and
 * none of the three values, wherever they stand, and no hardware address,
 * and it puts masking words in their place. */
void test_the_record_keeps_no_identity_values()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, identity_arguments()));
	const std::string lines[] = {
		"Kernel command line: quiet androidboot.serialno=" + made_up_serial +
			" androidboot.kernarium.adb_key=" + made_up_key +
			" androidboot.kernarium.timezone=" + made_up_zone +
			" androidboot.hardware=test\n",
		"init: Init cannot set 'ro.boot.kernarium.adb_key' to '" +
			made_up_key + "': too long\n",
		"[ro.serialno]: [" + made_up_serial + "]\n",
		"[persist.sys.timezone]: [" + made_up_zone + "]\n",
		"eth0: link up, address " + made_up_address + "\n",
		"unrelated: 05:04:50 dead:beef::1 12:34:56 serial OTHERSERIAL99 "
		"zone Other/Elsewhere\n",
		"the last: " + made_up_key,
	};
	for (const std::string &line : lines)
		(void)uml::runtime::kernel_write(STDERR_FILENO, line.data(),
			line.size());
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143, "exit");
	const std::string record = read_file(path);
	require_no_identity(record, ": " + record);
	require(has(record, "Kernel command line: quiet androidboot.serialno="
			    "<masked> androidboot.kernarium.adb_key=<masked> "
			    "androidboot.kernarium.timezone=<time zone> "
			    "androidboot.hardware=test\n") &&
		has(record, "init: Init cannot set 'ro.boot.kernarium.adb_key' to "
			    "'<masked>': too long\n") &&
		has(record, "[ro.serialno]: [<masked>]\n") &&
		has(record, "[persist.sys.timezone]: [<time zone>]\n") &&
		has(record, "eth0: link up, address <address>\n") &&
		has(record, "the last: <masked>"),
		"the output was not kept with its values masked: " + record);
	/* What is not an identity value stays: a time of day, an IPv6 address,
	 * another serial number and another zone. */
	require(has(record, "unrelated: 05:04:50 dead:beef::1 12:34:56 serial "
			    "OTHERSERIAL99 zone Other/Elsewhere\n"),
		"the record masked what is no identity value: " + record);
	uml::runtime::reset_session_record();
	unlink(path.c_str());

	/* A message, which an end states from what the guest printed. */
	require(uml::runtime::begin_session_record(path, identity_arguments()));
	uml::runtime::end_session_record(SessionEnd::failed, 1,
		"gave up on " + made_up_serial + " with " + made_up_key);
	const std::string message = read_file(path);
	require(has(message, "message gave up on <masked> with <masked>\n"),
		"the message was not kept with its values masked: " + message);
	require_no_identity(message, ": " + message);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The text the record quotes of the kernel's output, which ends the file's
 * state: without the newlines that pad it. */
std::string quoted_output(const std::string &record)
{
	const std::string mark = "kernel-output\n";
	const auto at = record.find(mark);
	require(at != std::string::npos, "the record quotes no output: " + record);
	std::string text = record.substr(at + mark.size());
	while (!text.empty() && text.back() == '\n')
		text.pop_back();
	return text;
}

/* The message the record states, up to the end of its line. */
std::string message_of(const std::string &record)
{
	const std::string mark = "\nmessage ";
	const auto at = record.find(mark);
	require(at != std::string::npos, "the record has no message: " + record);
	const auto from = at + mark.size();
	return record.substr(from, record.find('\n', from) - from);
}

/* A session states its own values, and none of an earlier one stays, each of
 * the three on its own: a launch that states two of them keeps the third as the
 * kernel printed it, apart from what the names and the hardware address's
 * shape take out. */
void test_a_session_masks_only_its_own_values()
{
	const std::string zone_line = "[tz]: [" + made_up_zone + "]\n";
	const std::string key_line = "[key]: [" + made_up_key + "]\n";
	const std::string serial_line = "[serial]: [" + made_up_serial + "]\n";
	const std::string lines = key_line + serial_line + zone_line;
	struct Case {
		const char *left_out;
		std::vector<std::string> arguments;
		std::string expected;
	};
	const std::vector<Case> cases{
		{"the key",
			{"androidboot.serialno=" + made_up_serial,
				"androidboot.kernarium.timezone=" + made_up_zone},
			"[key]: [" + made_up_key + "]\n[serial]: [<masked>]\n"
			"[tz]: [<time zone>]"},
		{"the serial number",
			{"androidboot.kernarium.adb_key=" + made_up_key,
				"androidboot.kernarium.timezone=" + made_up_zone},
			"[key]: [<masked>]\n[serial]: [" + made_up_serial + "]\n"
			"[tz]: [<time zone>]"},
		{"the time zone",
			{"androidboot.kernarium.adb_key=" + made_up_key,
				"androidboot.serialno=" + made_up_serial},
			"[key]: [<masked>]\n[serial]: [<masked>]\n"
			"[tz]: [" + made_up_zone + "]"},
		{"all three", {"quiet"},
			"[key]: [" + made_up_key + "]\n[serial]: [" + made_up_serial +
				"]\n[tz]: [" + made_up_zone + "]"},
	};
	for (const Case &one : cases) {
		uml::runtime::reset_session_record();
		const std::string path = temporary_record();
		/* The session before states all three. */
		require(uml::runtime::begin_session_record(path, identity_arguments()));
		uml::runtime::end_session_record(SessionEnd::kernel_exit, 0, "exit");
		require(uml::runtime::begin_session_record(path, one.arguments));
		(void)uml::runtime::kernel_write(STDERR_FILENO, lines.data(),
			lines.size());
		uml::runtime::end_session_record(SessionEnd::kernel_exit, 0, "exit");
		const std::string record = read_file(path);
		require(quoted_output(record) == one.expected,
			std::string{"a session that leaves out "} + one.left_out +
				" masked by an earlier session's value: " + record);
		uml::runtime::reset_session_record();
		unlink(path.c_str());
	}
}

/* The ring keeps the last bytes of the output, so after a wrap its first line
 * is cut at any byte, in a value as well as in a name. The record starts at the
 * first complete line, with the output as one write longer than the ring and as
 * many small ones, whichever byte the ring begins at. */
void test_a_wrapped_ring_keeps_no_identity_values()
{
	constexpr std::size_t ring = uml::runtime::session_kernel_output_bytes;
	const std::string echo =
		"init: Init cannot set 'ro.boot.kernarium.adb_key' to '" + made_up_key +
		"'\n[ro.serialno]: [" + made_up_serial + "]\n[tz]: [" + made_up_zone +
		"]\nup on " + made_up_address + " ok\n";
	const std::string marker = "\nring end marker\n";
	for (const bool one_write : {true, false}) {
		for (std::size_t cut = 0; cut < echo.size(); ++cut) {
			std::string input = echo;
			const std::size_t padding =
				ring + cut - echo.size() - marker.size();
			for (std::size_t line = 0; line < padding / 32; ++line)
				input += std::string(31, 'y') + '\n';
			input += std::string(padding % 32, 'y');
			input += marker;
			require(input.size() == ring + cut, "a wrong test input");
			uml::runtime::reset_session_record();
			const std::string path = temporary_record();
			require(uml::runtime::begin_session_record(path,
				identity_arguments()));
			if (one_write) {
				(void)uml::runtime::kernel_write(
					STDERR_FILENO, input.data(), input.size());
			} else {
				for (std::size_t offset = 0; offset < input.size();
				     offset += 61)
					(void)uml::runtime::kernel_write(
						STDERR_FILENO, input.data() + offset,
						std::min<std::size_t>(61, input.size() - offset));
			}
			uml::runtime::end_session_record(SessionEnd::kernel_exit, 143,
				"exit");
			const std::string record = read_file(path);
			const std::string where = (one_write ? " (one write, cut " :
							       " (small writes, cut ") +
				std::to_string(cut) + ")";
			require_no_identity(record, where);
			require(has(record, "ring end marker\n"),
				"the record lost the end of the output" + where);
			uml::runtime::reset_session_record();
			unlink(path.c_str());
		}
	}
}

/* A value that arrives in two writes: a record written between them, as a
 * checkpoint of a boot that is not finished does and a process that is ended
 * from outside leaves, holds the part that is there masked from the length at
 * which it can be told from an ordinary word (the shortest prefix is raw, which
 * a process that dies at that moment keeps), and the whole output is masked
 * once the rest has come. The text is exact at every cut. */
void test_a_value_in_two_writes_is_masked_in_both_states()
{
	struct Split {
		std::string before;
		std::string value;
		std::string after;
		std::string mask;
		std::size_t shortest;
	};
	const std::vector<Split> splits{
		{"init: Init cannot set 'ro.boot.kernarium.adb_key' to '", made_up_key,
			"'\n", "<masked>", 8},
		{"[ro.serialno]: [", made_up_serial, "]\n", "<masked>", 4},
		{"[persist.sys.timezone]: [", made_up_zone, "]\n", "<time zone>", 4},
	};
	for (const Split &split : splits) {
		for (std::size_t cut = 1; cut < split.value.size(); ++cut) {
			uml::runtime::reset_session_record();
			uml::runtime::set_session_boot_facts(true, 0);
			const std::string path = temporary_record();
			require(uml::runtime::begin_session_record(path,
				identity_arguments()));
			const std::string first = split.before + split.value.substr(0, cut);
			(void)uml::runtime::kernel_write(STDERR_FILENO, first.data(),
				first.size());
			uml::runtime::checkpoint_session_memory();
			const std::string where = " (split at " + std::to_string(cut) +
				" of " + split.value + ")";
			const std::string between = read_file(path);
			require(quoted_output(between) ==
					split.before + (cut >= split.shortest ? split.mask :
						split.value.substr(0, cut)),
				"the part before the second write is not as it is to be" +
					where + ": " + between);
			const std::string rest =
				split.value.substr(cut) + split.after;
			(void)uml::runtime::kernel_write(STDERR_FILENO, rest.data(),
				rest.size());
			uml::runtime::end_session_record(SessionEnd::kernel_exit, 143,
				"exit");
			const std::string record = read_file(path);
			require(quoted_output(record) ==
					split.before + split.mask +
						split.after.substr(0, split.after.size() - 1),
				"the output was not kept masked" + where + ": " + record);
			uml::runtime::reset_session_record();
			unlink(path.c_str());
		}
	}
}

/* A value that stands against more of a word is no value: the serial number
 * and the adb key are bounded by word characters (the key by the base64
 * alphabet too), the time zone by the characters of a zone's path. Ordinary
 * text that holds a value stays as it is, and the value alone is masked. */
void test_a_value_in_a_longer_word_is_not_masked()
{
	const std::string word = "madeupword";
	const std::vector<std::string> arguments{
		"androidboot.serialno=" + word,
		"androidboot.kernarium.adb_key=" + made_up_key,
		"androidboot.kernarium.timezone=" + made_up_zone};
	const std::string &key = made_up_key;
	const std::string zone = made_up_zone;
	/* One line each; every `<` marks what is masked. */
	const std::vector<std::pair<std::string, std::string>> lines{
		{word + "_to_mount", word + "_to_mount"},
		{"_" + word + "_", "_" + word + "_"},
		{"un" + word + " " + word + "s", "un" + word + " " + word + "s"},
		{"x" + word + "x", "x" + word + "x"},
		{"1" + word, "1" + word},
		{word + " (" + word + ") [" + word + "]",
			"<masked> (<masked>) [<masked>]"},
		{word + "-" + word + "." + word, "<masked>-<masked>.<masked>"},
		{"x" + key + "x", "x" + key + "x"},
		{"+" + key + "+", "+" + key + "+"},
		{"/" + key + "/", "/" + key + "/"},
		{"_" + key, "_" + key},
		{key + "=", key + "="},
		{"[" + key + "]", "[<masked>]"},
		{"name=" + key, "name=<masked>"},
		{"'" + key + "' " + key, "'<masked>' <masked>"},
		{"/usr/share/" + zone, "/usr/share/" + zone},
		{zone + "/more", zone + "/more"},
		{zone + "-1", zone + "-1"},
		{"[" + zone + "] (" + zone + ")", "[<time zone>] (<time zone>)"},
	};
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	for (const auto &[input, expected] : lines) {
		require(uml::runtime::begin_session_record(path, arguments));
		const std::string line = input + "\nend\n";
		(void)uml::runtime::kernel_write(STDERR_FILENO, line.data(),
			line.size());
		uml::runtime::end_session_record(SessionEnd::kernel_exit, 0, "exit");
		const std::string record = read_file(path);
		require(quoted_output(record) == expected + "\nend",
			"a value in a longer word was not kept as it is, or a lone one was "
			"not masked, for `" + input + "`: " + record);
	}
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* Output that ends where a value may have begun (the guest has printed the
 * first bytes of it and the rest is not written yet) is masked from those
 * bytes on, so that a checkpoint between two writes shows none of the value.
 * The bytes only count when they begin a word: the end of a longer word that
 * merely starts as a value does, stays as it is. */
void test_the_start_of_a_value_at_the_end_needs_a_boundary()
{
	const std::string word = "madeupword";
	const std::vector<std::string> arguments{
		"androidboot.serialno=" + word,
		"androidboot.kernarium.adb_key=" + made_up_key,
		"androidboot.kernarium.timezone=" + made_up_zone};
	const std::string key = made_up_key.substr(0, 12);
	const std::string zone = made_up_zone.substr(0, 6);
	/* The output ends with the input: no newline follows it. */
	const std::vector<std::pair<std::string, std::string>> endings{
		{" made", " <masked>"},
		{"(madeupwor", "(<masked>"},
		{"xmade", "xmade"},
		{"_madeup", "_madeup"},
		{"7madeupwor", "7madeupwor"},
		{" " + key, " <masked>"},
		{"'" + key, "'<masked>"},
		{"x" + key, "x" + key},
		{"+" + key, "+" + key},
		{"/" + key, "/" + key},
		{"_" + key, "_" + key},
		{" " + zone, " <time zone>"},
		{"(" + zone, "(<time zone>"},
		{"x" + zone, "x" + zone},
		{"/" + zone, "/" + zone},
		{"-" + zone, "-" + zone},
	};
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	for (const auto &[input, expected] : endings) {
		require(uml::runtime::begin_session_record(path, arguments));
		(void)uml::runtime::kernel_write(STDERR_FILENO, input.data(),
			input.size());
		uml::runtime::end_session_record(SessionEnd::kernel_exit, 0, "exit");
		const std::string record = read_file(path);
		require(quoted_output(record) == expected,
			"the start of a value at the end was not masked, or a word's end "
			"was, for `" + input + "`: " + record);
	}
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The record stores the output without its carriage returns and with every
 * other byte that is not printable as `?`, so a value that the guest printed
 * with such a byte inside is the value in the record when the byte is dropped
 * and is not when it becomes `?`. The mask is matched on what is stored: a
 * checkpoint, the output the end quotes and the end's message all keep no
 * value, whichever byte stands inside it and wherever. */
void test_a_byte_inside_a_value_does_not_hide_it()
{
	const std::vector<std::pair<std::string, std::string>> values{
		{made_up_serial, "<masked>"},
		{made_up_zone, "<time zone>"},
		{made_up_key, "<masked>"},
	};
	for (const auto &[value, mask] : values) {
		/* Every position in the serial number and the zone, and a spread in
		 * the key, which is long. */
		const std::size_t step = value.size() > 40 ? 7 : 1;
		for (std::size_t at = 1; at < value.size(); at += step) {
			for (const char byte : {'\r', '\x01', '\x7f', '\xff'}) {
				std::string broken = value;
				broken.insert(at, 1, byte);
				/* A carriage return is dropped, so the value is whole in
				 * the record; any other byte is stored as `?`. */
				const std::string stored = byte == '\r' ?
					mask : [&] {
						std::string text = value;
						text.insert(at, 1, '?');
						return text;
					}();
				const std::string where = " (byte " +
					std::to_string(static_cast<int>(
						static_cast<unsigned char>(byte))) +
					" at " + std::to_string(at) + " of " + value + ")";
				uml::runtime::reset_session_record();
				uml::runtime::set_session_boot_facts(true, 0);
				const std::string path = temporary_record();
				/* A checkpoint of the unfinished boot. */
				require(uml::runtime::begin_session_record(path,
					identity_arguments()));
				const std::string line = "[" + broken + "]\n";
				(void)uml::runtime::kernel_write(STDERR_FILENO, line.data(),
					line.size());
				uml::runtime::checkpoint_session_memory();
				const std::string between = read_file(path);
				require(quoted_output(between) == "[" + stored + "]",
					"a checkpoint did not keep the value as it is to be" +
						where + ": " + between);
				/* The output the end quotes. */
				uml::runtime::end_session_record(SessionEnd::kernel_exit, 0,
					"exit");
				const std::string record = read_file(path);
				require(quoted_output(record) == "[" + stored + "]",
					"the end did not keep the value as it is to be" + where +
						": " + record);
				uml::runtime::reset_session_record();
				unlink(path.c_str());
				/* The end's message. */
				require(uml::runtime::begin_session_record(path,
					identity_arguments()));
				uml::runtime::end_session_record(SessionEnd::failed, 1,
					"gave up on [" + broken + "]");
				const std::string message = read_file(path);
				require(message_of(message) == "gave up on [" + stored + "]",
					"the message did not keep the value as it is to be" +
						where + ": " + message);
				uml::runtime::reset_session_record();
				unlink(path.c_str());
			}
		}
	}
}

void test_memory_checkpoints()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const std::string begun = read_file(path);
	const std::string slot = begun.substr(0, slot_bytes);
	require(has(slot, " footprint ") && has(slot, " peak "),
		"the memory slot has no footprint: " + slot);
	/* A checkpoint rewrites the slot and nothing after it. */
	constexpr int checkpoints = 500;
	const auto start = std::chrono::steady_clock::now();
	for (int index = 0; index < checkpoints; ++index)
		uml::runtime::checkpoint_session_memory();
	const auto each = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - start).count() / checkpoints;
	std::printf("a memory checkpoint takes %.1f us\n", each);
	require(each < 5000.0, "a memory checkpoint is far too slow");
	const std::string checked = read_file(path);
	require(checked.size() == begun.size() &&
		checked.substr(state_offset) == begun.substr(state_offset),
		"a checkpoint changed the session's state");
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	uml::runtime::checkpoint_session_memory();
	const std::string ended = read_file(path);
	require(ended.substr(state_offset).starts_with("state ended\nend powered-off\n"),
		"a checkpoint after the end replaced it: " + ended);
	/* The next session reuses the record's descriptor number. */
	const std::string next = temporary_record();
	require(uml::runtime::begin_session_record(next, {}));
	require(has(read_file(next), "\nstate running\n") &&
		has(read_file(path), "\nend powered-off\n"),
		"the next session's record was written to the wrong file");
	uml::runtime::reset_session_record();
	unlink(path.c_str());
	unlink(next.c_str());
}

std::atomic<unsigned> malloc_readings{0};

uml::runtime::MemoryTerms fixed_terms(bool read_malloc) noexcept
{
	constexpr std::uint64_t mib = std::uint64_t{1} << 20;
	if (read_malloc)
		malloc_readings.fetch_add(1, std::memory_order_relaxed);
	return {
		.guest_held = 4000 * mib,
		.gpu_budget = 1251 * mib,
		.gpu_held = 1240 * mib,
		.gpu_age_ms = 3500,
		.graphics = 700 * mib,
		.media = 5 * mib,
		.cache = 240 * mib,
		.heaps = 250 * mib,
		.malloc_allocated = read_malloc ? 900 * mib : 0,
		.malloc_in_use = read_malloc ? 800 * mib : 0,
		.internal = 5000 * mib,
		.compressed = 12 * mib,
	};
}

/* When a checkpoint reads malloc's term. */
namespace due {
using uml::runtime::memory_malloc_reading_due;
constexpr std::uint64_t mib = std::uint64_t{1} << 20;
static_assert(memory_malloc_reading_due(false, 0, 0),
	"the first checkpoint reads it");
static_assert(memory_malloc_reading_due(true, 1064 * mib, 1000 * mib) &&
		!memory_malloc_reading_due(true, 1064 * mib - 1, 1000 * mib) &&
		!memory_malloc_reading_due(true, 900 * mib, 1000 * mib),
	"then only a footprint 64 MiB above the highest reading's");
} // namespace due

std::size_t count_of(const std::string &text, const char *part)
{
	std::size_t count = 0;
	for (auto at = text.find(part); at != std::string::npos;
	     at = text.find(part, at + 1))
		++count;
	return count;
}

std::vector<std::string> words(const std::string &line)
{
	std::vector<std::string> found;
	std::istringstream stream{line};
	for (std::string word; stream >> word;)
		found.push_back(word);
	return found;
}

/* The history rows of a memory slot, oldest first, as words. */
std::vector<std::vector<std::string>> history_rows(const std::string &slot)
{
	std::vector<std::vector<std::string>> rows;
	for (auto at = slot.find("\nh "); at != std::string::npos;
	     at = slot.find("\nh ", at + 1))
		rows.push_back(words(slot.substr(at + 1, slot.find('\n', at + 1) -
			at - 1)));
	return rows;
}

/* A row's figures from the span on: span frames gap thermal pressure
 * compressed internal reliefs shed renewals largest cpu vcpu perf mhz
 * energy. */
constexpr std::size_t span_word = 11;
constexpr std::size_t row_words = span_word + 16;

std::vector<std::string> run_words(const std::vector<std::string> &row)
{
	require(row.size() == row_words, "a row has " +
		std::to_string(row.size()) + " words");
	return {row.begin() + span_word + 1, row.end()};
}

std::string slot_of(const std::string &record)
{
	return record.substr(0, slot_bytes);
}

void test_memory_terms()
{
	uml::runtime::reset_session_record();
	uml::runtime::set_memory_terms_reader(&fixed_terms);
	malloc_readings.store(0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const std::string begun = read_file(path);
	const std::string first = begun.substr(0, slot_bytes);
	require(has(first, "\nterms guest 4000 gpu 1240/1251 age 3 graphics 700 "
			"media 5 cache 240 heaps 250 malloc 900/800 age 0 other 0 "
			"internal 5000 compressed 12\n") &&
		has(first, "\npeak-terms at ") &&
		has(first, "\nhistory seconds footprint available guest gpu "
			"graphics cache heaps malloc other span frames gap thermal "
			"pressure compressed internal reliefs shed renewals largest "
			"cpu vcpu perf mhz energy\n") &&
		count_of(first, "\nh ") == 1,
		"the first checkpoint has no terms: " + first);
	/* Enough checkpoints to fill both histories, which then stay at their
	 * sizes, oldest first. */
	constexpr unsigned checkpoints = uml::runtime::memory_history_recent +
		uml::runtime::memory_history_older_stride *
			(uml::runtime::memory_history_older + 2);
	for (unsigned index = 0; index < checkpoints; ++index)
		uml::runtime::checkpoint_session_memory();
	const std::string checked = read_file(path);
	const std::string slot = checked.substr(0, slot_bytes);
	require(count_of(slot, "\nh ") == uml::runtime::memory_history_recent +
			uml::runtime::memory_history_older &&
		slot[slot_bytes - 1] == '\n' &&
		slot.find_last_not_of(" \n") < slot_bytes - 2,
		"the history did not keep its sizes: " + slot);
	long previous = -1;
	for (auto at = slot.find("\nh "); at != std::string::npos;
	     at = slot.find("\nh ", at + 1)) {
		const long seconds = std::strtol(slot.c_str() + at + 3, nullptr, 10);
		require(seconds >= previous, "the history is out of order: " + slot);
		previous = seconds;
	}
	require(checked.substr(state_offset) == begun.substr(state_offset),
		"a checkpoint changed the session's state");
	/* Checkpoints at one footprint read malloc's term once, and repeat that
	 * reading. */
	require(malloc_readings.load() == 1 &&
			has(slot, "\nterms guest 4000 gpu 1240/1251 age 3 graphics "
				"700 media 5 cache 240 heaps 250 malloc 900/800 age "),
		"malloc's term was read again or not repeated: " + slot);

	/* What the product reads, and what that costs. */
	uml::runtime::set_memory_terms_reader(&uml::runtime::memory_terms);
	constexpr int timed = 200;
	const auto start = std::chrono::steady_clock::now();
	for (int index = 0; index < timed; ++index)
		uml::runtime::checkpoint_session_memory();
	const auto each = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - start).count() / timed;
	const auto walk_start = std::chrono::steady_clock::now();
	for (int index = 0; index < timed; ++index)
		(void)uml::runtime::memory_terms(true);
	const auto walk = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - walk_start).count() / timed;
	std::printf("a memory checkpoint with its terms takes %.1f us; reading "
		"malloc's term as well takes %.1f us\n", each, walk);
	require(each < 5000.0, "a memory checkpoint is far too slow");
	require(has(read_file(path).substr(0, slot_bytes), " malloc "),
		"the product's reader wrote no terms");

	/* And with the run figures the product reads: what the checkpoint costs
	 * with them, and what reading them costs alone. */
	uml::runtime::set_run_figures_reader(&uml::runtime::run_figures);
	const auto figures_start = std::chrono::steady_clock::now();
	for (int index = 0; index < timed; ++index)
		uml::runtime::checkpoint_session_memory();
	const auto with_figures = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - figures_start).count() / timed;
	const auto read_start = std::chrono::steady_clock::now();
	for (int index = 0; index < timed; ++index)
		(void)uml::runtime::run_figures(false);
	const auto read = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - read_start).count() / timed;
	std::printf("a memory checkpoint with its terms and the run figures takes "
		"%.1f us; reading the run figures takes %.1f us\n", with_figures, read);
	require(with_figures < 5000.0, "a memory checkpoint is far too slow");
	const auto product_rows = history_rows(slot_of(read_file(path)));
	require(!product_rows.empty() && product_rows.back().size() == row_words,
		"the product's rows are not whole");
	std::string newest;
	for (const auto &word : product_rows.back())
		newest += (newest.empty() ? "" : " ") + word;
	std::printf("a row: %s\n", newest.c_str());
	uml::runtime::set_run_figures_reader(nullptr);
	uml::runtime::set_memory_terms_reader(nullptr);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* ---- The figures of the run in the history rows. ---- */

/* The run figures a test's reader reports: set by the test, one reading at a
 * time, under a lock because the checkpoint thread may read too. */
struct Script final {
	std::mutex lock;
	uml::runtime::RunFigures figures;
	unsigned begins{};
	std::uint64_t step{};
};

Script script;

uml::runtime::RunFigures scripted_figures(bool session_begins) noexcept
{
	const std::lock_guard guard{script.lock};
	if (session_begins)
		++script.begins;
	return script.figures;
}

void script_set(const uml::runtime::RunFigures &figures)
{
	const std::lock_guard guard{script.lock};
	script.figures = figures;
}

/* A reader for a long run: the figures are a function of the step the test
 * has reached, so a checkpoint the background thread adds changes none. Each
 * step flushes 10 frames, the longest gap is 20 ms but at the step
 * `spike_step`, uses 100 ms of CPU on the process and 80 ms on the carriers,
 * 60 ms of it on performance cores, 300 million cycles and 200 million nJ. */
constexpr std::uint64_t spike_step = 25;

uml::runtime::RunFigures stepped_figures(bool) noexcept
{
	const std::lock_guard guard{script.lock};
	const std::uint64_t step = script.step;
	uml::runtime::RunFigures figures;
	figures.sources = uml::runtime::RunFigures::frames |
		uml::runtime::RunFigures::host | uml::runtime::RunFigures::cache |
		uml::runtime::RunFigures::largest | uml::runtime::RunFigures::cpu |
		uml::runtime::RunFigures::carriers | uml::runtime::RunFigures::usage |
		uml::runtime::RunFigures::cycle_count;
	figures.flushes = 1000 + 10 * step;
	figures.longest_gap_ns = (step == spike_step ? 900 : 20) * 1'000'000ULL;
	figures.thermal = static_cast<std::uint8_t>(step % 4);
	figures.pressure = static_cast<std::uint8_t>(step % 3);
	figures.reliefs = step / 10;
	figures.shed_accounts = step / 5;
	figures.renewals = step / 2;
	figures.largest_open = 150ULL << 20;
	figures.cpu_ns = 100'000'000ULL * step;
	figures.carrier_ns = 80'000'000ULL * step;
	figures.performance_ns = 60'000'000ULL * step;
	figures.cycles = 300'000'000ULL * step;
	figures.energy_nj = 200'000'000ULL * step;
	return figures;
}

void set_step(std::uint64_t step)
{
	const std::lock_guard guard{script.lock};
	script.step = step;
}

void finish_session(const std::string &path)
{
	uml::runtime::set_run_figures_reader(nullptr);
	uml::runtime::set_memory_terms_reader(nullptr);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* What a row says of the interval since the checkpoint before it: the
 * difference of the counters, the longest gap, the states at the checkpoint
 * and "-" for a source with nothing to say. */
void test_run_figures_in_a_row()
{
	using F = uml::runtime::RunFigures;
	constexpr std::uint64_t mib = std::uint64_t{1} << 20;
	uml::runtime::reset_session_record();
	uml::runtime::set_memory_terms_reader(&fixed_terms);
	uml::runtime::set_run_figures_reader(&scripted_figures);
	F first;
	first.sources = F::frames | F::host | F::cache | F::largest | F::cpu |
		F::carriers | F::usage | F::cycle_count;
	first.flushes = 1000;
	first.thermal = 1;
	first.pressure = 0;
	first.reliefs = 3;
	first.shed_accounts = 5;
	first.renewals = 2;
	first.largest_open = 150 * mib;
	first.cpu_ns = 10'000'000'000ULL;
	first.carrier_ns = 8'000'000'000ULL;
	first.performance_ns = 6'000'000'000ULL;
	first.cycles = 30'000'000'000ULL;
	first.energy_nj = 20'000'000'000ULL;
	script_set(first);
	{
		const std::lock_guard guard{script.lock};
		script.begins = 0;
	}
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	/* The session's first reading is told it opens the session, and the rest
	 * are not; the background thread may add readings, never a begin. */
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	{
		const std::lock_guard guard{script.lock};
		require(script.begins == 1, "the session's first reading was not "
			"told it begins the session");
	}
	auto rows = history_rows(slot_of(read_file(path)));
	require(rows.size() == 1, "the begin wrote no row");
	require((run_words(rows[0]) == std::vector<std::string>{"-", "-", "1",
			"0", "12", "5000", "-", "-", "-", "150", "-", "-", "-", "-",
			"-"}),
		"the first row of a session has no interval to speak of");

	/* The next checkpoint: 120 more frames, the longest gap 45 ms, a state a
	 * step hotter, and the counters' differences. */
	F second = first;
	second.flushes = 1120;
	second.longest_gap_ns = 45'000'000ULL;
	second.thermal = 2;
	second.pressure = 1;
	second.reliefs = 4;
	second.shed_accounts = 7;
	second.renewals = 5;
	second.largest_open = 158 * mib;
	second.cpu_ns = 15'900'000'000ULL;
	second.carrier_ns = 13'000'000'000ULL;
	second.performance_ns = 10'400'000'000ULL;
	second.cycles = 47'700'000'000ULL;
	second.energy_nj = 31'800'000'000ULL;
	script_set(second);
	std::this_thread::sleep_for(std::chrono::milliseconds{20});
	uml::runtime::checkpoint_session_memory();
	rows = history_rows(slot_of(read_file(path)));
	require(rows.size() == 2, "a checkpoint added no row");
	const auto &row = rows[1];
	const auto span = std::stoull(row[span_word]);
	require(span >= 20 && span < 60000,
		"the row's span is not the time since the checkpoint before: " +
		row[span_word]);
	/* 4.4 of 5.9 s on performance cores, 17.7 million cycles per 5.9 ms of
	 * CPU time is 3000 MHz, and 11.8 J. */
	require((run_words(row) == std::vector<std::string>{"120", "45", "2",
			"1", "12", "5000", "1", "2", "3", "158", "5900", "5000", "74",
			"3000", "11800"}),
		"a row did not say what the run did in its interval");

	/* A source that has nothing to say is "-": here the host's state, the
	 * cache and the usage, while frames, CPU time and the carriers answer. */
	F third = second;
	third.sources = F::frames | F::cpu | F::carriers;
	third.flushes = 1180;
	third.longest_gap_ns = 16'000'000ULL;
	third.cpu_ns = second.cpu_ns + 2'000'000'000ULL;
	third.carrier_ns = second.carrier_ns + 1'000'000'000ULL;
	script_set(third);
	uml::runtime::checkpoint_session_memory();
	rows = history_rows(slot_of(read_file(path)));
	require(rows.size() == 3 &&
		(run_words(rows[2]) == std::vector<std::string>{"60", "16", "-",
			"-", "12", "5000", "-", "-", "-", "-", "2000", "1000", "-",
			"-", "-"}),
		"absent sources were not left out of a row");

	/* A counter that fell, as the carriers' time does when one ends, counts
	 * nothing for that interval, and the next is measured from the new
	 * reading. */
	F fourth = third;
	fourth.carrier_ns = 400'000'000ULL;
	fourth.flushes = 1180;
	script_set(fourth);
	uml::runtime::checkpoint_session_memory();
	F fifth = fourth;
	fifth.carrier_ns = 900'000'000ULL;
	script_set(fifth);
	uml::runtime::checkpoint_session_memory();
	rows = history_rows(slot_of(read_file(path)));
	require(rows.size() == 5 &&
		run_words(rows[3])[0] == "0" && run_words(rows[3])[10] == "0" &&
		run_words(rows[3])[11] == "0" && run_words(rows[4])[11] == "500",
		"a counter that fell was not counted as nothing");

	/* A platform with no source for the cycles (iOS) answers the rest of the
	 * usage and leaves the cycles per CPU second out. */
	F sixth = second;
	sixth.sources = second.sources & ~F::cycle_count;
	script_set(sixth);
	uml::runtime::checkpoint_session_memory();
	F seventh = sixth;
	seventh.flushes += 60;
	seventh.cpu_ns += 2'000'000'000ULL;
	seventh.performance_ns += 1'500'000'000ULL;
	seventh.energy_nj += 3'000'000'000ULL;
	script_set(seventh);
	uml::runtime::checkpoint_session_memory();
	rows = history_rows(slot_of(read_file(path)));
	require(rows.size() == 7 &&
		run_words(rows[6])[0] == "60" && run_words(rows[6])[10] == "2000" &&
		run_words(rows[6])[12] == "75" && run_words(rows[6])[13] == "-" &&
		run_words(rows[6])[14] == "3000",
		"a platform without cycles did not leave only them out");
	finish_session(path);
}

/* A row of the older history stands for the checkpoints between it and the
 * older row before, and says what the run did in all of them. */
void test_older_rows_sum_their_window()
{
	using uml::runtime::memory_history_older;
	using uml::runtime::memory_history_older_stride;
	using uml::runtime::memory_history_recent;
	uml::runtime::reset_session_record();
	uml::runtime::set_memory_terms_reader(&fixed_terms);
	uml::runtime::set_run_figures_reader(&stepped_figures);
	set_step(0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	constexpr std::uint64_t last = 400;
	for (std::uint64_t step = 1; step <= last; ++step) {
		set_step(step);
		uml::runtime::checkpoint_session_memory();
	}
	const std::string slot = slot_of(read_file(path));
	const auto rows = history_rows(slot);
	require(rows.size() == memory_history_recent + memory_history_older,
		"the history did not keep its sizes");
	/* The recent rows are each one checkpoint: 10 frames, 100 ms of CPU
	 * time, 60 of it on performance cores. */
	for (std::size_t n = memory_history_older; n < rows.size(); ++n) {
		const auto run = run_words(rows[n]);
		require(run[0] == "10" && run[1] == "20" && run[10] == "100" &&
			run[11] == "80" && run[12] == "60" && run[13] == "3000" &&
			run[14] == "200", "a recent row is not one interval");
	}
	/* The older rows are the checkpoints at multiples of the stride that
	 * have left the recent history, newest last. */
	const std::uint64_t highest = ((last - memory_history_recent) /
		memory_history_older_stride) * memory_history_older_stride;
	const std::uint64_t oldest = highest -
		(memory_history_older - 1) * memory_history_older_stride;
	require(spike_step + memory_history_older_stride > oldest &&
			spike_step <= oldest,
		"the test's spike is not in the oldest older row's window");
	for (std::size_t n = 0; n < memory_history_older; ++n) {
		const std::uint64_t sampled = oldest + n * memory_history_older_stride;
		const auto run = run_words(rows[n]);
		/* frames over the stride's checkpoints, the longest gap among them,
		 * the CPU times summed and the states at the sampled one. */
		const bool spike = n == 0;
		const auto per = [&](std::uint64_t divisor) {
			return std::to_string(sampled / divisor -
				(sampled - memory_history_older_stride) / divisor);
		};
		require(run[0] == std::to_string(10 * memory_history_older_stride) &&
			run[1] == (spike ? "900" : "20") &&
			run[2] == std::to_string(sampled % 4) &&
			run[3] == std::to_string(sampled % 3) &&
			run[6] == per(10) && run[7] == per(5) && run[8] == per(2) &&
			run[9] == "150" &&
			run[10] == std::to_string(100 * memory_history_older_stride) &&
			run[11] == std::to_string(80 * memory_history_older_stride) &&
			run[12] == "60" && run[13] == "3000" &&
			run[14] == std::to_string(200 * memory_history_older_stride),
			"an older row did not sum its window: " + std::to_string(n) +
			" " + slot);
	}
	/* The seconds still rise through both histories. */
	long previous = -1;
	for (const auto &row : rows) {
		const long seconds = std::strtol(row[1].c_str(), nullptr, 10);
		require(seconds >= previous, "the history is out of order");
		previous = seconds;
	}
	finish_session(path);
}

/* The terms a test's reader reports when everything is huge. */
uml::runtime::MemoryTerms huge_terms(bool) noexcept
{
	constexpr std::uint64_t huge = std::uint64_t{900000000} << 20;
	return {
		.guest_held = huge, .gpu_budget = huge, .gpu_held = huge,
		.gpu_age_ms = 3500, .graphics = huge, .media = huge, .cache = huge,
		.heaps = huge, .malloc_allocated = huge, .malloc_in_use = huge,
		.internal = huge, .compressed = huge,
	};
}

uml::runtime::RunFigures huge_figures(bool) noexcept
{
	const std::lock_guard guard{script.lock};
	const std::uint64_t huge = std::uint64_t{1} << 60;
	uml::runtime::RunFigures figures;
	figures.sources = 0xff;
	figures.flushes = huge / 1000 * (script.step + 1);
	figures.longest_gap_ns = huge;
	figures.thermal = 3;
	figures.pressure = 2;
	figures.reliefs = huge / 1000 * (script.step + 1);
	figures.shed_accounts = figures.reliefs;
	figures.renewals = figures.reliefs;
	figures.largest_open = huge;
	figures.cpu_ns = huge / 1000 * (script.step + 1);
	figures.carrier_ns = figures.cpu_ns;
	figures.performance_ns = figures.cpu_ns;
	figures.cycles = figures.cpu_ns;
	figures.energy_nj = figures.cpu_ns;
	return figures;
}

/* A row has a size whatever its figures are, and a history that does not fit
 * the slot loses its oldest rows, never the newest. */
void test_rows_never_overflow_the_slot()
{
	uml::runtime::reset_session_record();
	uml::runtime::set_memory_terms_reader(&huge_terms);
	uml::runtime::set_run_figures_reader(&huge_figures);
	set_step(0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	for (std::uint64_t step = 1; step <= 400; ++step) {
		set_step(step);
		uml::runtime::checkpoint_session_memory();
	}
	const std::string record = read_file(path);
	const std::string slot = slot_of(record);
	const auto rows = history_rows(slot);
	require(!rows.empty() && slot[slot_bytes - 1] == '\n',
		"the slot lost its end");
	const auto &newest = rows.back();
	require(newest.size() == row_words, "the newest row was cut");
	/* The newest checkpoint is in the slot, which the seconds of a session
	 * that ran nothing cannot tell from the one before it: the row count
	 * shows rows were dropped from the old end. */
	require(rows.size() < uml::runtime::memory_history_recent +
			uml::runtime::memory_history_older &&
		rows.size() >= uml::runtime::memory_history_recent,
		"the oldest rows were not the ones dropped: " +
		std::to_string(rows.size()));
	for (const auto &row : rows)
		require(row.size() == row_words, "a row was cut");
	require(record.substr(state_offset).starts_with("state ended\n"),
		"the slot touched the state");
	finish_session(path);
}

void *crash_on_a_thread(void *)
{
	uml::runtime::record_kernel_crash();
	static char name[64];
	pthread_getname_np(pthread_self(), name, sizeof(name));
	return name;
}

void test_kernel_crash()
{
	for (const bool panicked : {true, false}) {
		uml::runtime::reset_session_record();
		const std::string path = temporary_record();
		require(uml::runtime::begin_session_record(path, {}));
		const char panic[] = "[ 1.0] Kernel panic - not syncing: sysrq "
			"triggered crash\nDarwin UML panic: sysrq triggered crash\n";
		const char other[] = "[ 1.0] an ordinary line\n";
		if (panicked)
			(void)uml::runtime::kernel_write(STDERR_FILENO, panic,
				sizeof(panic) - 1);
		else
			(void)uml::runtime::kernel_write(STDERR_FILENO, other,
				sizeof(other) - 1);
		pthread_t thread;
		require(pthread_create(&thread, nullptr, crash_on_a_thread,
			nullptr) == 0);
		void *result = nullptr;
		require(pthread_join(thread, &result) == 0);
		const std::string name{static_cast<const char *>(result)};
		const std::string record = read_file(path);
		if (panicked) {
			require(name == "panic: sysrq triggered crash",
				"the panicking thread was named " + name);
			require(has(record, "end guest-panic\nstatus 6\n") &&
				has(record, "message sysrq triggered crash\n") &&
				has(record, "Kernel panic - not syncing"),
				"a panic was recorded wrong: " + record);
		} else {
			require(name.empty(), "a crash without a panic named its "
				"thread " + name);
			require(has(record, "end kernel-abort\n") &&
				!has(record, "message "),
				"a kernel abort was recorded wrong: " + record);
		}
		uml::runtime::reset_session_record();
		unlink(path.c_str());
	}
}

void test_host_fault()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const char registers[] = "Darwin host fault: signal=11 esr=0x92000046 "
		"far=0 pc=0x1a2b3c sp=0x16f000 lr=0x1a2b00\n";
	const char line[] = "Darwin host fault: taken on a host thread which "
		"owns no virtual CPU; the guest is not the faulting agent\n";
	(void)uml::runtime::kernel_write(STDERR_FILENO, registers,
		sizeof(registers) - 1);
	require(has(read_file(path), "state running\n"),
		"the registers alone ended the session");
	(void)uml::runtime::kernel_write(STDERR_FILENO, line, sizeof(line) - 1);
	const std::string record = read_file(path);
	require(has(record, "end host-fault\nstatus 11\n") &&
		has(record, "message signal=11 esr=0x92000046 far=0 pc=0x1a2b3c") &&
		has(record, "owns no virtual CPU"),
		"a host fault was recorded wrong: " + record);
	uml::runtime::record_kernel_crash();
	require(read_file(path).substr(state_offset) == record.substr(state_offset),
		"a later crash replaced the fault");
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_process_exit_in_a_child()
{
	const std::string path = temporary_record();
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		uml::runtime::reset_session_record();
		if (!uml::runtime::begin_session_record(path, {}))
			_exit(2);
		std::exit(0);
	}
	int status = 0;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0, "the child did not exit");
	const std::string record = read_file(path);
	require(has(record, "end process-exit\n") &&
		has(record, "message exit() was called while the guest ran\n"),
		"an exit() while the guest ran was recorded wrong: " + record);
	unlink(path.c_str());
}

/* The file is a fixed layout of a size it never exceeds, whatever the session
 * writes into it, and its parts do not overwrite each other. */
void test_size_bound_and_stall_slot()
{
	using uml::runtime::session_record_max_bytes;
	using uml::runtime::session_stall_slot_bytes;
	static_assert(session_record_max_bytes == 19200,
		"the record's largest size is stated in the documentation");
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	const int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
	require(sink >= 0, "no sink");
	const std::string console = "con0=fd:" + std::to_string(sink) + ",fd:" +
		std::to_string(sink);
	const std::vector<std::string> arguments{console};
	require(uml::runtime::begin_session_record(path, arguments));
	const std::string begun = read_file(path);
	require(begun.substr(slot_bytes, 14) == "stalls 1 base " &&
		begun[state_offset - 1] == '\n' &&
		begun.substr(slot_bytes).find("total 0 shown 0") != std::string::npos,
		"a session starts with an empty stall slot");
	/* The file has its whole size from the start, and keeps it. */
	require(begun.size() == session_record_max_bytes);

	/* A slot of the wrong size changes nothing; the right one lands in its
	 * place alone. */
	uml::runtime::write_session_stall_slot(std::string(100, 'x'));
	uml::runtime::write_session_stall_slot(
		std::string(session_stall_slot_bytes + 1, 'x'));
	require(read_file(path) == begun, "a wrong-sized slot was written");
	std::string slot(session_stall_slot_bytes, 'x');
	slot.back() = '\n';
	uml::runtime::write_session_stall_slot(slot);
	std::string changed = read_file(path);
	require(changed.size() == begun.size() &&
		changed.substr(slot_bytes, session_stall_slot_bytes) == slot &&
		changed.substr(0, slot_bytes) == begun.substr(0, slot_bytes) &&
		changed.substr(state_offset) == begun.substr(state_offset),
		"the stall slot touched another part");

	/* A long run of checkpoints, stall slots and kernel output, and an end
	 * with a long message: the file stays within its bound. */
	const std::string noise(60000, 'k');
	for (unsigned round = 0; round < 40; ++round) {
		(void)uml::runtime::kernel_write(sink, noise.data(), noise.size());
		uml::runtime::checkpoint_session_memory();
		uml::runtime::write_session_stall_slot(slot);
		require(read_file(path).size() == begun.size(),
			"the running record grew");
	}
	uml::runtime::end_session_record(SessionEnd::failed, 1,
		std::string(100000, 'm'));
	const std::string ended = read_file(path);
	require(ended.size() == session_record_max_bytes &&
		has(ended.substr(state_offset), "state ended\nend failed\n") &&
		has(ended.substr(state_offset), "\nmessage m"),
		"an end of a long message left a record of " +
		std::to_string(ended.size()) + " bytes");
	/* Nothing writes the slot once the session has ended. */
	uml::runtime::write_session_stall_slot(std::string(
		session_stall_slot_bytes, 'y'));
	require(read_file(path) == ended, "a slot was written after the end");

	/* The next session in the file starts empty, in the same size. */
	require(uml::runtime::begin_session_record(path, arguments));
	const std::string next = read_file(path);
	require(next.size() == ended.size() &&
		next.substr(state_offset, 14) == "state running\n" &&
		!has(next.substr(state_offset), "message m") &&
		next.substr(slot_bytes, 14) == "stalls 1 base ",
		"a session inherited the last one");
	uml::runtime::reset_session_record();
	(void)close(sink);
	unlink(path.c_str());
}

/* The counts a test's reader reports: a provider that has counted since the
 * test began, and a switch for a guest with no network. */
uml::runtime::SessionNetworkCounts reader_counts;
bool reader_present = true;

bool test_network_reader(uml::runtime::SessionNetworkCounts &counts) noexcept
{
	if (!reader_present)
		return false;
	counts = reader_counts;
	return true;
}

/* The network slot sits between the stall slot and the state: it holds this
 * session's share of the counts and the notice, rewritten at each checkpoint
 * and at the end, and it touches no other part. */
/* The network slot's lines up to the blanks that pad it. */
std::string network_slot_text(const std::string &slot)
{
	return slot.substr(0, slot.find_last_of('\n', slot.size() - 2) + 1);
}

/* The slot's text for fixed figures is the golden file AndariumCore parses
 * too (tests/unit/andarium/fixtures/network-slot-golden.txt). */
void the_network_slot_matches_the_golden_file(const char *golden_path)
{
	using uml::runtime::NetworkNotice;
	uml::runtime::reset_session_record();
	uml::runtime::set_network_notice(NetworkNotice::none);
	reader_counts = {};
	reader_present = true;
	uml::runtime::set_network_counts_reader(&test_network_reader);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	reader_counts = {.dns_answered = 5, .dns_policy_denied = 3,
			 .dns_timed_out = 2, .dns_failed = 1,
			 .dhcp_discovers = 1, .dhcp_requests = 2,
			 .dhcp_acks = 1, .tcp_attempts = 12,
			 .tcp_established = 4, .tcp_unreachable = 2,
			 .tcp_reset = 1};
	uml::runtime::set_network_notice(NetworkNotice::cellular_denied);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::set_network_notice(NetworkNotice::wifi_denied);
	uml::runtime::checkpoint_session_memory();
	const std::string text = network_slot_text(read_file(path).substr(
		uml::runtime::session_network_offset,
		uml::runtime::session_network_slot_bytes));
	const std::string golden = read_file(golden_path);
	require(!golden.empty(), "the golden network slot opens");
	if (golden != text) {
		std::fprintf(stderr, "golden:\n%s\nslot:\n%s\n", golden.c_str(),
			text.c_str());
		require(false, "the network slot differs from the golden file");
	}
	uml::runtime::set_network_counts_reader(nullptr);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_network_slot()
{
	using uml::runtime::NetworkNotice;
	using uml::runtime::session_network_offset;
	using uml::runtime::session_network_slot_bytes;
	static_assert(session_network_offset == 12288 &&
			session_network_slot_bytes == 512 &&
			uml::runtime::session_storage_offset ==
				session_network_offset + session_network_slot_bytes,
		"the network slot sits after the stall slot");
	const auto slot_of = [](const std::string &record) {
		return record.substr(session_network_offset,
			session_network_slot_bytes);
	};
	uml::runtime::reset_session_record();
	uml::runtime::set_network_notice(NetworkNotice::none);
	/* The provider counted before this session began. */
	reader_counts = {.dns_answered = 10, .dns_failed = 2, .tcp_attempts = 7,
			 .tcp_reset = 1};
	reader_present = true;
	uml::runtime::set_network_counts_reader(&test_network_reader);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	std::string slot = slot_of(record);
	require(slot.starts_with("network 1\nnet-dns answered 0 policy-denied 0 "
				 "timed-out 0 failed 0\n"
				 "net-dhcp discovers 0 requests 0 acks 0\n"
				 "net-tcp attempts 0 established 0 unreachable 0 "
				 "reset 0\nnet-notice denials 0 now none\n") &&
		slot.back() == '\n' && slot.size() == session_network_slot_bytes,
		"a session does not start from the provider's reading: " + slot);

	/* A checkpoint writes the share since the begin. */
	reader_counts.dns_answered = 15;
	reader_counts.dns_policy_denied = 3;
	reader_counts.dhcp_discovers = 1;
	reader_counts.dhcp_requests = 1;
	reader_counts.dhcp_acks = 1;
	reader_counts.tcp_attempts = 12;
	reader_counts.tcp_established = 4;
	reader_counts.tcp_unreachable = 2;
	uml::runtime::set_network_notice(NetworkNotice::cellular_denied);
	uml::runtime::checkpoint_session_memory();
	record = read_file(path);
	slot = slot_of(record);
	require(has(slot, "net-dns answered 5 policy-denied 3 timed-out 0 "
			  "failed 0\n") &&
		has(slot, "net-dhcp discovers 1 requests 1 acks 1\n") &&
		has(slot, "net-tcp attempts 5 established 4 unreachable 2 "
			  "reset 0\n") &&
		has(slot, "net-notice denials 1 now cellular\n"),
		"a checkpoint wrote the wrong network slot: " + slot);
	require(record.substr(state_offset).starts_with(
			"state running\nend running\n") &&
		record.substr(slot_bytes, 14) == "stalls 1 base ",
		"the network slot touched another part");

	/* The notice counts the denials that began, the one in force kept. */
	uml::runtime::set_network_notice(NetworkNotice::cellular_denied);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::set_network_notice(NetworkNotice::wifi_denied);
	/* The end writes the last counts, which no checkpoint follows. */
	reader_counts.dns_timed_out = 9;
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	record = read_file(path);
	slot = slot_of(record);
	require(has(slot, "timed-out 9 failed 0\n") &&
		has(slot, "net-notice denials 2 now wifi\n") &&
		has(record.substr(state_offset), "state ended\n"),
		"the end did not write the last network slot: " + slot);

	/* A denial under way when the next session begins is one it saw, and a
	 * provider made since is counted whole. */
	reader_counts = {.dns_answered = 3};
	require(uml::runtime::begin_session_record(path, {}));
	slot = slot_of(read_file(path));
	require(has(slot, "net-dns answered 0 ") &&
		has(slot, "net-notice denials 1 now wifi\n"),
		"the next session began wrong: " + slot);
	reader_counts = {.dns_answered = 1};
	uml::runtime::checkpoint_session_memory();
	slot = slot_of(read_file(path));
	require(has(slot, "net-dns answered 1 "),
		"a provider made since was not counted whole: " + slot);

	/* A guest with no network has the notice and no counts. */
	reader_present = false;
	uml::runtime::checkpoint_session_memory();
	slot = slot_of(read_file(path));
	require(slot.starts_with("network 1\nnet-notice denials 1 now wifi\n") &&
		!has(slot, "net-dns"),
		"a guest with no network was given counts: " + slot);

	uml::runtime::set_network_counts_reader(nullptr);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The storage slot sits between the network slot and the state: blank until
 * the guest's storage is first settled, then the counts of the settles and of
 * those that left it unsettled, and the last one. It touches no other part and
 * holds words and numbers only. */
std::string storage_slot_text(const std::string &slot)
{
	return slot.substr(0, slot.find_last_of('\n', slot.size() - 2) + 1);
}

/* The last line's time is the wall clock's: a golden file holds a fixed one. */
std::string without_the_time(std::string text)
{
	const std::size_t at = text.rfind(" at ");
	require(at != std::string::npos, "the storage slot has no time: " + text);
	const std::size_t end = text.find('\n', at);
	return text.replace(at + 4, end - at - 4, "1794114290");
}

/* The slot's text for fixed figures is the golden file AndariumCore parses
 * too (tests/unit/andarium/fixtures/storage-slot-golden.txt). */
void the_storage_slot_matches_the_golden_file(const char *golden_path)
{
	using uml::runtime::GuestStorage;
	using uml::runtime::SettleKind;
	using uml::runtime::SettleSuspension;
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	uml::runtime::note_guest_settle(SettleKind::sync,
		GuestStorage::unconfirmed, SettleSuspension::not_asked);
	uml::runtime::note_guest_settle(SettleKind::quiesce_resident,
		GuestStorage::confirmed, SettleSuspension::suspended);
	uml::runtime::note_guest_settle(SettleKind::quiesce,
		GuestStorage::refused, SettleSuspension::not_suspended);
	uml::runtime::note_guest_settle(SettleKind::quiesce,
		GuestStorage::confirmed, SettleSuspension::suspended);
	const std::string text = without_the_time(storage_slot_text(
		read_file(path).substr(uml::runtime::session_storage_offset,
			uml::runtime::session_storage_slot_bytes)));
	const std::string golden = read_file(golden_path);
	require(!golden.empty(), "the golden storage slot opens");
	if (golden != text) {
		std::fprintf(stderr, "golden:\n%s\nslot:\n%s\n", golden.c_str(),
			text.c_str());
		require(false, "the storage slot differs from the golden file");
	}
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_storage_slot()
{
	using uml::runtime::GuestStorage;
	using uml::runtime::SettleKind;
	using uml::runtime::SettleSuspension;
	using uml::runtime::session_storage_offset;
	using uml::runtime::session_storage_slot_bytes;
	static_assert(session_storage_offset == 12800 &&
			session_storage_slot_bytes == 256 &&
			uml::runtime::session_camera_offset ==
				session_storage_offset + session_storage_slot_bytes,
		"the storage slot sits between the network slot and the camera slot");
	const auto slot_of = [](const std::string &record) {
		return record.substr(session_storage_offset,
			session_storage_slot_bytes);
	};
	/* The outcomes the slot counts as settled are the ones the applications
	 * print "settled" for, and no others, and the zero value, which claims
	 * nothing, is none of them. */
	require(static_cast<int>(GuestStorage::not_reported) == 0,
		"the zero value of the storage outcome claims something");
	require(uml::runtime::guest_storage_settled(GuestStorage::confirmed) &&
		uml::runtime::guest_storage_settled(GuestStorage::no_guest) &&
		!uml::runtime::guest_storage_settled(GuestStorage::not_reported) &&
		!uml::runtime::guest_storage_settled(GuestStorage::unconfirmed) &&
		!uml::runtime::guest_storage_settled(GuestStorage::refused) &&
		!uml::runtime::guest_storage_settled(GuestStorage::unreachable),
		"an outcome was counted settled that is not");

	uml::runtime::reset_session_record();
	/* Without a session nothing is recorded, and nothing fails. */
	uml::runtime::note_guest_settle(SettleKind::sync,
		GuestStorage::confirmed, SettleSuspension::not_asked);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	std::string slot = slot_of(record);
	require(slot == std::string(session_storage_slot_bytes - 1, ' ') + "\n",
		"a session does not begin with a blank storage slot: " + slot);

	/* A settle writes the counts and the last one, in the slot alone. */
	uml::runtime::note_guest_settle(SettleKind::sync,
		GuestStorage::confirmed, SettleSuspension::not_asked);
	record = read_file(path);
	slot = slot_of(record);
	require(slot.starts_with("storage 1\nsettles 1 unsettled 0\n"
				 "last sync confirmed not-asked at ") &&
		slot.back() == '\n' && slot.size() == session_storage_slot_bytes,
		"a confirmed sync was recorded wrong: " + slot);
	require(record.substr(state_offset).starts_with(
			"state running\nend running\n") &&
		record.substr(slot_bytes, 14) == "stalls 1 base " &&
		record.substr(uml::runtime::session_network_offset, 9) ==
			"network 1",
		"the storage slot touched another part");

	/* An unreachable console, a refusal and a guest that did not answer
	 * count as unsettled; no guest does not. The last one is the last. */
	uml::runtime::note_guest_settle(SettleKind::quiesce,
		GuestStorage::unreachable, SettleSuspension::not_suspended);
	slot = slot_of(read_file(path));
	require(slot.starts_with("storage 1\nsettles 2 unsettled 1\n"
				 "last quiesce unreachable not-suspended at "),
		"an unreachable console was recorded wrong: " + slot);
	uml::runtime::note_guest_settle(SettleKind::sync, GuestStorage::no_guest,
		SettleSuspension::not_asked);
	uml::runtime::note_guest_settle(SettleKind::quiesce_resident,
		GuestStorage::refused, SettleSuspension::not_suspended);
	uml::runtime::note_guest_settle(SettleKind::sync,
		GuestStorage::unconfirmed, SettleSuspension::not_asked);
	slot = slot_of(read_file(path));
	require(slot.starts_with("storage 1\nsettles 5 unsettled 3\n"
				 "last sync unconfirmed not-asked at "),
		"the counts of settles were wrong: " + slot);
	/* Words, digits and blanks only: never a path, an address or a name. */
	for (const char c : storage_slot_text(slot))
		require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == ' ' || c == '-' || c == '\n',
			"the storage slot holds a character that is no word: " + slot);

	/* The end keeps what the session counted, and the file does not change
	 * once the session has ended. */
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	const std::string ended = read_file(path);
	require(slot_of(ended).starts_with("storage 1\nsettles 5 unsettled 3\n"),
		"the end lost the storage slot");
	uml::runtime::note_guest_settle(SettleKind::quiesce,
		GuestStorage::confirmed, SettleSuspension::suspended);
	require(read_file(path) == ended, "a settle was written after the end");

	/* The next session in the file starts blank and counts its own. */
	require(uml::runtime::begin_session_record(path, {}));
	slot = slot_of(read_file(path));
	require(slot == std::string(session_storage_slot_bytes - 1, ' ') + "\n",
		"a session inherited the last one's storage slot: " + slot);
	uml::runtime::note_guest_settle(SettleKind::quiesce,
		GuestStorage::confirmed, SettleSuspension::suspended);
	slot = slot_of(read_file(path));
	require(slot.starts_with("storage 1\nsettles 1 unsettled 0\n"
				 "last quiesce confirmed suspended at "),
		"the next session did not count its own: " + slot);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The camera slot sits between the storage slot and the state: blank until a
 * still of the guest's cameras fails, then the count of the failures and the
 * newest that fit, each with the camera, the stage and the numbers and words
 * that say why. It touches no other part and holds words and numbers only. */
std::string camera_slot_text(const std::string &slot)
{
	return slot.substr(0, slot.find_last_of('\n', slot.size() - 2) + 1);
}

/* Every failure line's time is the wall clock's: a golden file holds a fixed
 * one. */
std::string with_fixed_times(std::string text)
{
	for (std::size_t at = text.find("still at "); at != std::string::npos;
	     at = text.find("still at ", at + 1)) {
		const std::size_t begin = at + 9;
		const std::size_t end = text.find(' ', begin);
		require(end != std::string::npos,
			"the camera slot has no time: " + text);
		text.replace(begin, end - begin, "1794114290");
	}
	return text;
}

uml::platform::CameraStillFailure camera_failure(
	uml::platform::CameraStillStage stage, std::uint32_t camera,
	uml::platform::CameraPosition position)
{
	uml::platform::CameraStillFailure failure;
	failure.stage = stage;
	failure.camera = camera;
	failure.position = position;
	return failure;
}

/* The three failures the golden file holds, which AndariumCore parses too
 * (tests/unit/andarium/fixtures/camera-slot-golden.txt). */
void note_the_golden_failures()
{
	using namespace uml::platform;
	auto refused = camera_failure(CameraStillStage::refused, 0,
		CameraPosition::back);
	refused.askedWidth = 4032;
	refused.askedHeight = 3024;
	uml::runtime::note_camera_still_failure(refused);
	auto error = camera_failure(CameraStillStage::frameworkError, 1,
		CameraPosition::front);
	error.askedWidth = 2016;
	error.askedHeight = 1512;
	error.resolvedWidth = 2016;
	error.resolvedHeight = 1512;
	error.compensationSupported = CameraFact::yes;
	error.compensationEnabled = CameraFact::no;
	error.compensationReading = CameraReading::afterCommit;
	error.errorDomain = CameraErrorDomain::avFoundation;
	error.errorCode = -11800;
	uml::runtime::note_camera_still_failure(error);
	auto encoded = camera_failure(CameraStillStage::encodedSize, 1,
		CameraPosition::front);
	encoded.askedWidth = 2016;
	encoded.askedHeight = 1512;
	encoded.resolvedWidth = 2016;
	encoded.resolvedHeight = 1512;
	encoded.encodedWidth = 1512;
	encoded.encodedHeight = 2016;
	encoded.compensationSupported = CameraFact::yes;
	encoded.compensationEnabled = CameraFact::no;
	encoded.compensationReading = CameraReading::afterCommit;
	encoded.hostExifOrientation = 6;
	uml::runtime::note_camera_still_failure(encoded);
}

/* How a back camera and a front camera that imitates an earlier one were
 * presented: the notes the golden file holds (the numbers are an example's).
 * The front camera has a note of every kind. */
uml::platform::CameraPresentation camera_back_note()
{
	using namespace uml::platform;
	CameraPresentation note;
	note.camera = 0;
	note.position = CameraPosition::back;
	note.deviceType = CameraDeviceType::wide;
	note.coordinatorAngles = {90, 270, 0, 180};
	note.stated = 90;
	note.platformStated = 90;
	note.turn = 0;
	note.anchorWidth = 4032;
	note.anchorHeight = 3024;
	note.anchorFieldOfView = 63;
	note.videoKnown = true;
	note.videoDefaultAngle = 0;
	note.videoAngle = 0;
	note.videoAngleSupported = CameraFact::yes;
	note.videoDefaultMirrored = 0;
	note.videoMirrored = 0;
	note.videoAskedWidth = 1920;
	note.videoAskedHeight = 1440;
	note.videoFirstWidth = 1920;
	note.videoFirstHeight = 1440;
	return note;
}

uml::platform::CameraPresentation camera_front_note()
{
	using namespace uml::platform;
	CameraPresentation note;
	note.camera = 1;
	note.position = CameraPosition::front;
	note.deviceType = CameraDeviceType::wide;
	note.coordinatorAngles = {0, 180, 90, 270};
	note.stated = 90;
	note.platformStated = 90;
	note.turn = 270;
	note.earlier = true;
	note.anchorWidth = 1920;
	note.anchorHeight = 1440;
	note.anchorRatios = aspectRatioBit(CameraAspectRatio::r4x3) |
		aspectRatioBit(CameraAspectRatio::r3x4);
	note.anchorFieldOfView = 85;
	note.frontDevices = 2;
	note.squareFormats = true;
	note.dynamicRatios = true;
	note.frontFormatsDropped = 3;
	note.refusals = 1;
	note.lastRefusal = 4;
	note.ratioKnown = true;
	note.ratioPinned = CameraAspectRatio::r3x4;
	note.ratioNow = CameraAspectRatio::r3x4;
	note.ratioWidth = 1512;
	note.ratioHeight = 2016;
	note.ratioError = 0;
	note.videoKnown = true;
	note.videoDefaultAngle = 0;
	note.videoAngle = 270;
	note.videoAngleSupported = CameraFact::yes;
	note.videoDefaultMirrored = 0;
	note.videoMirrored = 0;
	note.videoAskedWidth = 1920;
	note.videoAskedHeight = 1440;
	note.videoFirstWidth = 1920;
	note.videoFirstHeight = 1440;
	note.videoStabilization = VideoStabilizationState::standard;
	note.photoKnown = true;
	note.compensationSupported = CameraFact::yes;
	note.compensationEnabled = CameraFact::yes;
	note.compensationReading = CameraReading::afterCommit;
	note.photoDefaultAngle = 90;
	note.photoAngle = 90;
	note.photoDefaultMirrored = 0;
	note.photoMirrored = 0;
	note.photoMaximumWidth = 2016;
	note.photoMaximumHeight = 1512;
	note.exifOrientation = 6;
	note.centerStageActive = CameraFact::no;
	note.distortionCorrected = CameraFact::yes;
	return note;
}

void note_the_golden_cameras()
{
	uml::runtime::note_camera_presentation(camera_back_note());
	uml::runtime::note_camera_presentation(camera_front_note());
}

void the_camera_slot_matches_the_golden_file(const char *golden_path)
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	note_the_golden_cameras();
	note_the_golden_failures();
	const std::string text = with_fixed_times(camera_slot_text(
		read_file(path).substr(uml::runtime::session_camera_offset,
			uml::runtime::session_camera_slot_bytes)));
	const std::string golden = read_file(golden_path);
	require(!golden.empty(), "the golden camera slot opens");
	if (golden != text) {
		std::fprintf(stderr, "golden:\n%s\nslot:\n%s\n", golden.c_str(),
			text.c_str());
		require(false, "the camera slot differs from the golden file");
	}
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_camera_slot()
{
	using namespace uml::platform;
	using uml::runtime::session_camera_offset;
	using uml::runtime::session_camera_slot_bytes;
	static_assert(session_camera_offset == 13056 &&
			session_camera_slot_bytes == 1024 &&
			state_offset == session_camera_offset +
				session_camera_slot_bytes,
		"the camera slot sits between the storage slot and the state");
	const auto slot_of = [](const std::string &record) {
		return record.substr(session_camera_offset,
			session_camera_slot_bytes);
	};
	uml::runtime::reset_session_record();
	/* Without a session nothing is recorded, and nothing fails. */
	uml::runtime::note_camera_still_failure(camera_failure(
		CameraStillStage::timeout, 0, CameraPosition::back));
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	std::string slot = slot_of(record);
	require(slot == std::string(session_camera_slot_bytes - 1, ' ') + "\n",
		"a session does not begin with a blank camera slot: " + slot);

	/* A failure writes the count and its line, in the slot alone: the
	 * resolved size of a photo that came back transposed. */
	auto transposed = camera_failure(CameraStillStage::resolvedSize, 1,
		CameraPosition::front);
	transposed.askedWidth = 2016;
	transposed.askedHeight = 1512;
	transposed.resolvedWidth = 1512;
	transposed.resolvedHeight = 2016;
	transposed.compensationSupported = CameraFact::yes;
	transposed.compensationEnabled = CameraFact::no;
	uml::runtime::note_camera_still_failure(transposed);
	record = read_file(path);
	slot = slot_of(record);
	require(slot.starts_with("camera 1\nstills failed 1\nstill at ") &&
		has(slot, " camera 1 front stage resolved-size asked 2016x1512 "
			  "resolved 1512x2016 encoded - compensation supported "
			  "yes enabled no read - exif - error none\n") &&
		slot.back() == '\n' && slot.size() == session_camera_slot_bytes,
		"a failure was recorded wrong: " + slot);
	require(record.substr(state_offset).starts_with(
			"state running\nend running\n") &&
		record.substr(slot_bytes, 14) == "stalls 1 base " &&
		record.substr(uml::runtime::session_network_offset, 9) ==
			"network 1" &&
		record.substr(uml::runtime::session_storage_offset,
			uml::runtime::session_storage_slot_bytes) ==
			std::string(uml::runtime::session_storage_slot_bytes - 1,
				' ') + "\n",
		"the camera slot touched another part");

	/* A framework error names its domain and code; a code is signed. */
	auto error = camera_failure(CameraStillStage::frameworkError, 1,
		CameraPosition::front);
	error.askedWidth = 2016;
	error.askedHeight = 1512;
	error.errorDomain = CameraErrorDomain::avFoundation;
	error.errorCode = -11800;
	uml::runtime::note_camera_still_failure(error);
	slot = slot_of(read_file(path));
	require(slot.starts_with("camera 1\nstills failed 2\n") &&
		has(slot, "stage framework-error asked 2016x1512 resolved - "
			  "encoded - compensation supported unknown enabled "
			  "unknown read - exif - error avfoundation -11800\n") &&
		has(slot, "stage resolved-size"),
		"a framework error was recorded wrong: " + slot);

	/* Every stage and position has its word, and nothing but words, digits,
	 * blanks and hyphens is written: never a path, an address or a name. */
	for (const auto stage : {CameraStillStage::unknown,
		     CameraStillStage::refused, CameraStillStage::notStarted,
		     CameraStillStage::missingData,
		     CameraStillStage::encodedSize, CameraStillStage::fileSize,
		     CameraStillStage::guestBuffer, CameraStillStage::timeout})
		uml::runtime::note_camera_still_failure(camera_failure(stage, 2,
			CameraPosition::external));
	slot = slot_of(read_file(path));
	for (const char c : camera_slot_text(slot))
		require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == ' ' || c == '-' || c == '\n',
			"the camera slot holds a character that is no word: " +
				slot);

	/* A long run of failures, with the largest numbers, leaves the file its
	 * size, the slot whole lines, and the newest of them. */
	CameraStillFailure largest = camera_failure(
		CameraStillStage::frameworkError, 4294967295U,
		CameraPosition::external);
	largest.askedWidth = largest.askedHeight = 4294967295U;
	largest.resolvedWidth = largest.resolvedHeight = 4294967295U;
	largest.encodedWidth = largest.encodedHeight = 4294967295U;
	largest.compensationSupported = CameraFact::unknown;
	largest.compensationEnabled = CameraFact::unknown;
	largest.errorDomain = CameraErrorDomain::other;
	largest.errorCode = INT32_MIN;
	for (unsigned round = 0; round < 40; ++round) {
		largest.camera = 4294967295U - round;
		uml::runtime::note_camera_still_failure(largest);
		record = read_file(path);
		require(record.size() == uml::runtime::session_record_max_bytes,
			"the running record changed size");
	}
	slot = slot_of(record);
	require(slot.size() == session_camera_slot_bytes &&
		slot.back() == '\n' && slot.starts_with("camera 1\n"
			"stills failed 50\nstill at ") &&
		has(slot, " camera " + std::to_string(4294967295U - 39U) + " "),
		"a run of large failures was recorded wrong: " + slot);
	std::size_t lines = 0;
	for (std::size_t at = slot.find("still at "); at != std::string::npos;
	     at = slot.find("still at ", at + 1)) {
		const std::size_t end = slot.find('\n', at);
		require(end != std::string::npos &&
				slot.find("error other -2147483648", at) < end,
			"a failure line was cut: " + slot);
		++lines;
	}
	require(lines >= 4 && lines <= uml::runtime::session_camera_failures_kept,
		"the slot holds " + std::to_string(lines) + " lines: " + slot);
	require(record.substr(state_offset).starts_with(
			"state running\nend running\n") &&
		record.substr(slot_bytes, 14) == "stalls 1 base ",
		"a run of failures touched another part");

	/* The end keeps what the session counted, and the file does not change
	 * once the session has ended. */
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	const std::string ended = read_file(path);
	require(slot_of(ended).starts_with("camera 1\nstills failed 50\n"),
		"the end lost the camera slot");
	uml::runtime::note_camera_still_failure(error);
	require(read_file(path) == ended, "a failure was written after the end");

	/* The next session in the file starts blank and counts its own. */
	require(uml::runtime::begin_session_record(path, {}));
	slot = slot_of(read_file(path));
	require(slot == std::string(session_camera_slot_bytes - 1, ' ') + "\n",
		"a session inherited the last one's camera slot: " + slot);
	uml::runtime::note_camera_still_failure(error);
	slot = slot_of(read_file(path));
	require(slot.starts_with("camera 1\nstills failed 1\nstill at "),
		"the next session did not count its own: " + slot);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* The camera slot also says how the platform presented each of the guest's
 * cameras: a mount line, and a ratio, video, photo and refused line where the
 * camera has something to say, the newest note of a camera replacing its older
 * one. The slot stays one KiB of whole lines whatever the notes hold, and the
 * lines that matter most (the front cameras', the two newest failures) are the
 * ones that stay. */
void test_camera_presentation()
{
	using namespace uml::platform;
	using uml::runtime::session_camera_offset;
	using uml::runtime::session_camera_slot_bytes;
	const auto slot_of = [](const std::string &record) {
		return record.substr(session_camera_offset,
			session_camera_slot_bytes);
	};
	const auto count = [](const std::string &text, const std::string &part) {
		std::size_t found = 0;
		for (std::size_t at = text.find(part); at != std::string::npos;
		     at = text.find(part, at + 1))
			++found;
		return found;
	};
	uml::runtime::reset_session_record();
	/* Without a session nothing is recorded, and nothing fails. */
	uml::runtime::note_camera_presentation(camera_front_note());
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	std::string slot = slot_of(record);
	require(slot == std::string(session_camera_slot_bytes - 1, ' ') + "\n",
		"a session does not begin with a blank camera slot: " + slot);

	/* A note writes the format line, the count (none failed) and the camera's
	 * lines, in the slot alone. */
	uml::runtime::note_camera_presentation(camera_front_note());
	record = read_file(path);
	slot = slot_of(record);
	require(slot.starts_with(
			"camera 1\nstills failed 0\n"
			"fronts 2 square yes dynamic yes dropped 3\n"
			"mount camera 1 front type 1 angles 0 180 90 270 stated 90 "
			"turn 270 earlier yes anchor 1920x1440 ratios 24 fov 85\n"
			"ratio camera 1 pinned 3x4 now 3x4 dims 1512x2016 error 0\n"
			"video camera 1 default 0 set 270 supported yes mirror 0 0 "
			"asked 1920x1440 first 1920x1440 stab standard\n"
			"photo camera 1 compensation supported yes enabled yes "
			"read after-commit default 90 set 90 mirror 0 0 max 2016x1512 "
			"exif 6 center no "
			"distortion yes\n"
			"refused camera 1 count 1 last 4\n") &&
		slot.back() == '\n' && slot.size() == session_camera_slot_bytes &&
		slot.find_first_not_of(' ', slot.find("last 4\n") + 7) ==
			slot.size() - 1,
		"a presentation was recorded wrong: " + slot);
	require(record.size() == uml::runtime::session_record_max_bytes &&
		record.substr(state_offset).starts_with(
			"state running\nend running\n") &&
		record.substr(slot_bytes, 14) == "stalls 1 base " &&
		record.substr(uml::runtime::session_network_offset, 9) ==
			"network 1" &&
		record.substr(uml::runtime::session_storage_offset,
			uml::runtime::session_storage_slot_bytes) ==
			std::string(uml::runtime::session_storage_slot_bytes - 1,
				' ') + "\n",
		"a presentation touched another part");

	/* The newest note of a camera replaces its older: the first frame of the
	 * stream arrives, a ratio change completes, and the lines say so. */
	auto front = camera_front_note();
	front.videoFirstWidth = front.videoFirstHeight = 0;
	front.ratioError = ratioPending;
	front.exifOrientation = 0;
	uml::runtime::note_camera_presentation(front);
	slot = slot_of(read_file(path));
	require(has(slot, " asked 1920x1440 first - stab standard\n") &&
		has(slot, " now 3x4 dims 1512x2016 error pending\n") &&
		has(slot, " max 2016x1512 exif - center ") &&
		count(slot, "video camera 1 ") == 1 &&
		count(slot, "mount camera 1 ") == 1,
		"a camera's earlier note was not replaced: " + slot);
	/* The half turn the host applies to a camera's pictures is named next to
	 * its connection's turn, and a camera with none says nothing. */
	require(!has(slot, "hostturn"),
		"a camera with no half turn names none: " + slot);
	auto turned = camera_front_note();
	turned.stated = turned.platformStated = 270;
	turned.presentationTurn = 180;
	uml::runtime::note_camera_presentation(turned);
	slot = slot_of(read_file(path));
	require(has(slot, " stated 270 turn 270 hostturn 180 earlier yes ") &&
		count(slot, "mount camera 1 ") == 1,
		"the half turn of a camera's pictures was not recorded: " + slot);
	uml::runtime::note_camera_presentation(camera_front_note());
	slot = slot_of(read_file(path));
	require(has(slot, " first 1920x1440 stab standard\n") &&
		has(slot, " error 0\n") && has(slot, " exif 6 "),
		"the later note was not written: " + slot);

	/* The video line says how the connection is stabilized: a format that
	 * supports no mode leaves the picture unstabilized and the line says so,
	 * and a connection nothing was written to has a hyphen. */
	auto unsupported = camera_front_note();
	unsupported.videoStabilization = VideoStabilizationState::unsupported;
	uml::runtime::note_camera_presentation(unsupported);
	slot = slot_of(read_file(path));
	require(has(slot, " first 1920x1440 stab unsupported\n"),
		"an unstabilized picture was not said: " + slot);
	auto unknown = camera_front_note();
	unknown.videoStabilization = VideoStabilizationState::unknown;
	uml::runtime::note_camera_presentation(unknown);
	slot = slot_of(read_file(path));
	require(has(slot, " first 1920x1440 stab -\n"),
		"a connection nothing was said of was not a hyphen: " + slot);
	uml::runtime::note_camera_presentation(camera_front_note());

	/* A camera that has nothing but its mounting has that line alone, and
	 * what the platform did not state is a hyphen; the cameras are written in
	 * the order the guest numbers them. */
	CameraPresentation bare;
	bare.camera = 0;
	bare.frontDevices = 2;
	bare.squareFormats = bare.dynamicRatios = true;
	bare.frontFormatsDropped = 3;
	uml::runtime::note_camera_presentation(bare);
	slot = slot_of(read_file(path));
	require(slot.starts_with("camera 1\nstills failed 0\n"
			"fronts 2 square yes dynamic yes dropped 3\n"
			"mount camera 0 back type 0 angles - - - - stated 0 turn 0 "
			"earlier no anchor - ratios 0 fov -\n"
			"mount camera 1 front ") &&
		count(slot, " camera 0 ") == 1,
		"a bare camera was recorded wrong: " + slot);

	/* A tablet that boots turned tells the guest another orientation than the platform's: the line names both (the
	 * measured values of a tablet: the guest is told 180 for the front camera and 0 for the back, the platform's own
	 * being 90 for both), and a phone, which states what the platform does, names one. */
	auto tablet_back = camera_back_note();
	tablet_back.camera = 2;
	tablet_back.stated = 0;
	tablet_back.platformStated = 90;
	uml::runtime::note_camera_presentation(tablet_back);
	auto tablet_front = camera_front_note();
	tablet_front.camera = 3;
	tablet_front.stated = 180;
	tablet_front.platformStated = 90;
	tablet_front.earlier = false;
	tablet_front.ratioKnown = tablet_front.videoKnown = tablet_front.photoKnown = false;
	tablet_front.refusals = 0;
	uml::runtime::note_camera_presentation(tablet_front);
	slot = slot_of(read_file(path));
	require(has(slot, "mount camera 2 back type 1 angles 90 270 0 180 stated 0 platform 90 turn 0 earlier no ") &&
		has(slot, "mount camera 3 front type 1 angles 0 180 90 270 stated 180 platform 90 turn 270 earlier no ") &&
		has(slot, "mount camera 1 front type 1 angles 0 180 90 270 stated 90 turn 270 earlier yes ") &&
		!has(slot, "stated 90 platform"),
		"a tablet's two orientations were recorded wrong: " + slot);

	/* A camera number the slot has no room for is not recorded. */
	CameraPresentation beyond = camera_front_note();
	beyond.camera = uml::runtime::session_cameras_kept;
	const std::string before = slot_of(read_file(path));
	uml::runtime::note_camera_presentation(beyond);
	require(slot_of(read_file(path)) == before,
		"a camera beyond the slot's room was recorded");

	/* Only words, digits, blanks and hyphens are written. */
	for (const char c : camera_slot_text(slot))
		require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == ' ' || c == '-' || c == '\n',
			"the camera slot holds a character that is no word: " + slot);

	/* The failures and the notes share the slot: both are written, the
	 * failures after the cameras, the older of them dropped first. */
	uml::runtime::reset_session_record();
	unlink(path.c_str());
	require(uml::runtime::begin_session_record(path, {}));
	uml::runtime::note_camera_presentation(camera_back_note());
	uml::runtime::note_camera_presentation(camera_front_note());
	note_the_golden_failures();
	slot = slot_of(read_file(path));
	const std::size_t last_mount = slot.rfind("refused camera 1 ");
	const std::size_t first_still = slot.find("still at ");
	require(slot.starts_with("camera 1\nstills failed 3\n"
			"fronts 2 square yes dynamic yes dropped 3\n"
			"mount camera 0 back ") &&
		last_mount != std::string::npos && first_still != std::string::npos &&
		last_mount < first_still &&
		count(slot, "still at ") == 2 && !has(slot, "stage refused") &&
		has(slot, "stage framework-error") && has(slot, "stage encoded-size"),
		"the notes and the failures did not share the slot: " + slot);

	/* Every camera with every line and the largest numbers, and a run of the
	 * largest failures: the file keeps its size, the slot whole lines, the front
	 * camera's lines and the newest failure stay, and the cameras that are not
	 * the front's give way first. */
	uml::runtime::reset_session_record();
	unlink(path.c_str());
	require(uml::runtime::begin_session_record(path, {}));
	for (std::uint32_t camera = 0; camera < uml::runtime::session_cameras_kept; ++camera) {
		auto note = camera_front_note();
		note.camera = camera;
		note.position = camera == 3 ? CameraPosition::front : CameraPosition::back;
		note.earlier = camera == 3;
		note.anchorWidth = note.anchorHeight = 4294967295U;
		note.ratioWidth = note.ratioHeight = 4294967295U;
		note.videoAskedWidth = note.videoAskedHeight = 4294967295U;
		note.videoFirstWidth = note.videoFirstHeight = 4294967295U;
		note.photoMaximumWidth = note.photoMaximumHeight = 4294967295U;
		note.refusals = 65535;
		note.coordinatorAngles = {359, 359, 359, 359};
		note.anchorFieldOfView = 359;
		note.stated = note.platformStated = note.turn = 359;
		note.ratioError = INT32_MIN + 1;
		uml::runtime::note_camera_presentation(note);
		record = read_file(path);
		require(record.size() == uml::runtime::session_record_max_bytes,
			"the running record changed size");
	}
	CameraStillFailure largest = camera_failure(
		CameraStillStage::frameworkError, 4294967295U, CameraPosition::external);
	largest.askedWidth = largest.askedHeight = 4294967295U;
	largest.resolvedWidth = largest.resolvedHeight = 4294967295U;
	largest.encodedWidth = largest.encodedHeight = 4294967295U;
	largest.hostExifOrientation = 8;
	largest.errorDomain = CameraErrorDomain::other;
	largest.errorCode = INT32_MIN;
	for (unsigned round = 0; round < 12; ++round) {
		largest.camera = 4294967295U - round;
		uml::runtime::note_camera_still_failure(largest);
	}
	record = read_file(path);
	slot = slot_of(record);
	require(record.size() == uml::runtime::session_record_max_bytes &&
		slot.size() == session_camera_slot_bytes && slot.back() == '\n' &&
		slot.starts_with("camera 1\nstills failed 12\n"),
		"the slot of every camera and failure was recorded wrong: " + slot);
	for (const char *kind : {"mount camera 3 front ", "ratio camera 3 ",
		     "video camera 3 ", "photo camera 3 ", "refused camera 3 "})
		require(has(slot, kind),
			std::string("the front camera lost its line ") + kind + ": " + slot);
	require(has(slot, " camera " + std::to_string(4294967295U - 11U) + " "),
		"the newest failure was not kept: " + slot);
	for (std::size_t at = 0; at < camera_slot_text(slot).size();) {
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		require(line.starts_with("camera 1") || line.starts_with("stills ") ||
				line.starts_with("fronts ") ||
				line.starts_with("mount camera ") ||
				line.starts_with("ratio camera ") ||
				line.starts_with("video camera ") ||
				line.starts_with("photo camera ") ||
				line.starts_with("refused camera ") ||
				(line.starts_with("still at ") &&
					line.find("error other -2147483648") != std::string::npos),
			"a line was cut or unknown: " + line);
		at = end + 1;
	}

	/* The largest of the front devices' facts keep the front camera's lines. */
	for (std::uint32_t camera = 0; camera < uml::runtime::session_cameras_kept; ++camera) {
		auto note = camera_front_note();
		note.camera = camera;
		note.position = camera == 3 ? CameraPosition::front : CameraPosition::back;
		note.earlier = camera == 3;
		note.frontDevices = 255;
		note.frontFormatsDropped = 65535;
		uml::runtime::note_camera_presentation(note);
	}
	slot = slot_of(read_file(path));
	require(slot.starts_with("camera 1\nstills failed 12\n"
			"fronts 255 square yes dynamic yes dropped 65535\n") &&
		has(slot, "photo camera 3 ") &&
		has(slot, " camera " + std::to_string(4294967295U - 11U) + " "),
		"the largest front facts cost the slot its lines: " + slot);

	/* The end keeps the notes, and the next session starts blank. */
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	const std::string ended = read_file(path);
	require(slot_of(ended).starts_with("camera 1\nstills failed 12\n"),
		"the end lost the camera slot");
	uml::runtime::note_camera_presentation(camera_front_note());
	require(read_file(path) == ended, "a note was written after the end");
	require(uml::runtime::begin_session_record(path, {}));
	slot = slot_of(read_file(path));
	require(slot == std::string(session_camera_slot_bytes - 1, ' ') + "\n",
		"a session inherited the last one's notes: " + slot);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

/* A front device whose formats were all dropped is no camera, and it leaves
 * no line of its own: what the platform found of the front devices is a line
 * of the slot, kept whichever camera the guest used, and the slot still holds
 * the five guest cameras and the two newest failures a phone's bug report
 * needs. */
void test_front_device_facts()
{
	using namespace uml::platform;
	using uml::runtime::session_camera_offset;
	using uml::runtime::session_camera_slot_bytes;
	const auto slot_of = [](const std::string &record) {
		return record.substr(session_camera_offset,
			session_camera_slot_bytes);
	};
	const auto count = [](const std::string &text, const std::string &part) {
		std::size_t found = 0;
		for (std::size_t at = text.find(part); at != std::string::npos;
		     at = text.find(part, at + 1))
			++found;
		return found;
	};
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));

	/* Only a back camera was used, and the one front device had all four of
	 * its formats dropped: the slot says so. */
	auto back = camera_back_note();
	back.frontDevices = 1;
	back.squareFormats = false;
	back.dynamicRatios = false;
	back.frontFormatsDropped = 4;
	uml::runtime::note_camera_presentation(back);
	std::string slot = slot_of(read_file(path));
	require(slot.starts_with("camera 1\nstills failed 0\n"
			"fronts 1 square no dynamic no dropped 4\n"
			"mount camera 0 back ") &&
		count(slot, "fronts ") == 1 && !has(slot, " front type "),
		"the front devices' facts were recorded wrong: " + slot);
	/* A failed still is counted after the line, and the line stays. */
	uml::runtime::note_camera_still_failure(camera_failure(
		CameraStillStage::timeout, 0, CameraPosition::back));
	slot = slot_of(read_file(path));
	require(slot.starts_with("camera 1\nstills failed 1\n"
			"fronts 1 square no dynamic no dropped 4\n") &&
		has(slot, "stage timeout"),
		"a failure lost the front devices' facts: " + slot);

	/* Five guest cameras of an ordinary phone (only the front one used, with
	 * a mounting, video and photo line) and the two newest failures: the
	 * line, the front camera's lines and both failures stay, the other
	 * cameras' mounting lines give way last, and every line is whole. */
	uml::runtime::reset_session_record();
	unlink(path.c_str());
	require(uml::runtime::begin_session_record(path, {}));
	for (std::uint32_t camera = 0; camera < 5; ++camera) {
		auto note = camera == 3 ? camera_front_note() : camera_back_note();
		note.camera = camera;
		note.frontDevices = 1;
		note.squareFormats = note.dynamicRatios = false;
		note.frontFormatsDropped = 2;
		if (camera == 3) {
			note.ratioKnown = false;
			note.refusals = 0;
		} else {
			note.videoKnown = false;
		}
		uml::runtime::note_camera_presentation(note);
	}
	note_the_golden_failures();
	slot = slot_of(read_file(path));
	const std::size_t used = camera_slot_text(slot).size();
	require(slot.starts_with("camera 1\nstills failed 3\n"
			"fronts 1 square no dynamic no dropped 2\n") &&
		count(slot, "mount camera ") >= 3 &&
		count(slot, "still at ") == 2 && used <= session_camera_slot_bytes - 1,
		"five cameras and two failures did not fit the slot: " + slot);
	for (const char *kind : {"mount camera 3 front ", "video camera 3 ",
		     "photo camera 3 "})
		require(has(slot, kind),
			std::string("the front camera lost its line ") + kind + ": " + slot);
	for (std::size_t at = 0; at < used;) {
		const std::size_t end = slot.find('\n', at);
		require(end != std::string::npos && end < session_camera_slot_bytes,
			"a line of the slot was cut: " + slot);
		at = end + 1;
	}
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_unopenable_record()
{
	uml::runtime::reset_session_record();
	require(!uml::runtime::begin_session_record(
		"/nonexistent-directory/session", {}),
		"a record that cannot be opened was reported open");
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
}

} // namespace

int main(int argc, char **argv)
{
	try {
		/* The child forks before this process has a thread of its own. */
		test_process_exit_in_a_child();
		test_names();
		test_console_descriptors();
		test_panic_message();
		test_kept_output();
		test_the_console_says_when_the_boot_finished();
		test_boot_state();
		test_the_record_names_the_boots_ramdisk();
		test_boot_unobserved();
		test_boot_state_per_session();
		test_a_boot_quotes_only_its_own_output();
		test_a_short_boot_after_a_long_one_keeps_its_first_line();
		test_boot_state_size();
		test_the_end_outlasts_state_checkpoints();
		test_record_lifecycle();
		test_the_record_keeps_no_serial_number();
		test_a_wrapped_ring_keeps_no_serial_number();
		test_the_record_keeps_no_identity_values();
		test_a_session_masks_only_its_own_values();
		test_a_wrapped_ring_keeps_no_identity_values();
		test_a_value_in_two_writes_is_masked_in_both_states();
		test_a_value_in_a_longer_word_is_not_masked();
		test_the_start_of_a_value_at_the_end_needs_a_boundary();
		test_a_byte_inside_a_value_does_not_hide_it();
		test_memory_checkpoints();
		test_network_slot();
		test_storage_slot();
		test_camera_slot();
		test_camera_presentation();
		test_front_device_facts();
		/* The registration passes the golden files; a run without them
		 * fails rather than skipping the check. */
		require(argc == 4, "the golden slot files are not given");
		the_network_slot_matches_the_golden_file(argv[1]);
		the_storage_slot_matches_the_golden_file(argv[2]);
		the_camera_slot_matches_the_golden_file(argv[3]);
		test_memory_terms();
		test_run_figures_in_a_row();
		test_older_rows_sum_their_window();
		test_rows_never_overflow_the_slot();
		test_kernel_crash();
		test_host_fault();
		test_size_bound_and_stall_slot();
		test_unopenable_record();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "session record test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("session record tests passed");
	return EXIT_SUCCESS;
}
