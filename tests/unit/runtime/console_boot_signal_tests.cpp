// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What the console says of an Android guest's boot: init's processing of
 * sys.boot_completed=1, its start of recovery, or apexd's word that the system
 * booted end the boot, and init's first line shows that init's output is read
 * at all. The signal reads the console's bytes in whatever chunks the kernel
 * writes them, finds a marker that a write splits, takes near-misses for
 * nothing, and loses no chunk to a race.
 */
#include "console_boot_signal.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using uml::runtime::ConsoleBootSignal;
using uml::testing::require;

constexpr std::string_view line =
	"[    9.2] init: processing action (sys.boot_completed=1) from "
	"(/system/etc/init/hw/init.rc:1)\n";
constexpr std::string_view recovery = "init: starting service 'recovery'...\n";
constexpr std::string_view apex =
	"[   37.0][  T388] apexd: markBootCompleted() received by ApexService\n";
constexpr std::string_view first_stage =
	"[    0.4][    T1] init: init first stage started!\n";

void feed(ConsoleBootSignal &signal, std::string_view text)
{
	signal.feed(text.data(), text.size());
}

void test_a_line_in_one_write_completes()
{
	ConsoleBootSignal signal;
	require(!signal.completed(), "a new signal was complete");
	feed(signal, "init: starting service 'zygote'...\n");
	require(!signal.completed(), "an unrelated line completed");
	feed(signal, line);
	require(signal.completed(), "the boot's line did not complete");
}

void test_recovery_completes()
{
	ConsoleBootSignal signal;
	feed(signal, recovery);
	require(signal.completed(), "recovery's start did not complete");
}

void test_apexds_word_completes()
{
	ConsoleBootSignal signal;
	feed(signal, "apexd: markBootCompleted() received\n");
	feed(signal, "apexd: markBootCompleted() ApexService\n");
	require(!signal.completed(), "a near miss of apexd's line completed");
	feed(signal, apex);
	require(signal.completed() && signal.init_observed(),
		"apexd's line did not complete the boot");
}

/* Init's first line is the evidence that its output is read: it completes
 * nothing, and a boot that completed has shown it too. */
void test_init_observed_follows_init_output()
{
	ConsoleBootSignal signal;
	require(!signal.init_observed() && !signal.completed(), "a new signal saw init");
	feed(signal, "[    0.0] Linux version 6.18\n");
	feed(signal, "Freeing unused kernel memory\n");
	feed(signal, "init: init first stage started\n");
	feed(signal, "init: first stage started!\n");
	require(!signal.init_observed(), "the kernel's own lines or a near miss showed init");
	feed(signal, first_stage);
	require(signal.init_observed() && !signal.completed(),
		"init's first line did not show init, or completed the boot");
	/* Seen once, seen: later output changes nothing. */
	feed(signal, "init: starting service 'zygote'...\n");
	require(signal.init_observed() && !signal.completed(),
		"later output undid the sight of init");
	feed(signal, line);
	require(signal.init_observed() && signal.completed(),
		"a finished boot did not show init");
	signal.reset();
	require(!signal.init_observed() && !signal.completed(),
		"a reset kept the sight of init");

	/* A boot that completed without its first line was seen (a console
	 * turned on late), and split lines are found. */
	ConsoleBootSignal late;
	feed(late, line);
	require(late.init_observed(), "a finished boot with no first line was unseen");
	for (std::size_t cut = 0; cut <= first_stage.size(); ++cut) {
		ConsoleBootSignal split;
		feed(split, first_stage.substr(0, cut));
		feed(split, first_stage.substr(cut));
		require(split.init_observed(), "a split first line was missed");
	}
}

void test_a_marker_split_anywhere_is_found()
{
	for (const auto text : {line, recovery, apex}) {
		for (std::size_t cut = 0; cut <= text.size(); ++cut) {
			ConsoleBootSignal signal;
			feed(signal, text.substr(0, cut));
			feed(signal, text.substr(cut));
			require(signal.completed(), "a split marker was missed");
		}
		ConsoleBootSignal bytes;
		for (const char byte : text)
			bytes.feed(&byte, 1);
		require(bytes.completed(), "a marker written a byte at a time was missed");
	}
}

void test_near_misses_complete_nothing()
{
	ConsoleBootSignal signal;
	feed(signal, "init: processing action (sys.boot_completed=0) from x\n");
	feed(signal, "init: processing action (sys.boot_completed\n");
	feed(signal, "processing action (sys.boot_complete=1)\n");
	feed(signal, "starting service 'recovery'\n");
	feed(signal, "init: starting service 'recoveryd'\n");
	feed(signal, "sys.boot_completed=1\n");
	require(!signal.completed(), "a near miss completed");
	/* A prefix repeated before the real marker does not hide it. */
	feed(signal, "processing action (processing action (sys.boot_completed=1)");
	require(signal.completed(), "a marker after its own prefix was missed");
}

void test_noise_around_a_marker()
{
	std::string noise(100000, 'p');
	for (std::size_t index = 0; index < noise.size(); index += 7)
		noise[index] = "pro cessing action (sys.b"[index % 25];
	ConsoleBootSignal before;
	feed(before, noise);
	require(!before.completed(), "noise completed");
	feed(before, line);
	require(before.completed(), "a marker after noise was missed");
	ConsoleBootSignal inside;
	feed(inside, noise + std::string{line} + noise);
	require(inside.completed(), "a marker inside noise was missed");
}

void test_a_completed_signal_latches_until_reset()
{
	ConsoleBootSignal signal;
	feed(signal, line);
	feed(signal, "anything else\n");
	require(signal.completed(), "later output undid a completed boot");
	signal.reset();
	require(!signal.completed(), "a reset did not forget the boot");
	feed(signal, "init: processing action (sys.boot_comp");
	signal.reset();
	feed(signal, "leted=1)");
	require(!signal.completed(), "a reset kept half a marker");
	feed(signal, recovery);
	require(signal.completed(), "the boot after a reset did not complete");
}

void test_empty_and_null_input()
{
	ConsoleBootSignal signal;
	signal.feed(nullptr, 5);
	signal.feed(line.data(), 0);
	require(!signal.completed(), "nothing completed the boot");
}

/* Threads that feed at once lose no chunk: a chunk that holds a whole marker
 * completes the boot whatever the others write meanwhile. */
void test_concurrent_writers_lose_no_chunk()
{
	for (int round = 0; round != 50; ++round) {
		ConsoleBootSignal signal;
		std::atomic_bool go{};
		std::vector<std::thread> writers;
		for (int writer = 0; writer != 6; ++writer)
			writers.emplace_back([&, writer] {
				while (!go.load())
					std::this_thread::yield();
				const std::string noise(257 + writer, 'x');
				for (int index = 0; index != 400; ++index)
					feed(signal, noise);
				if (writer == 3)
					feed(signal, line);
				for (int index = 0; index != 400; ++index)
					feed(signal, noise);
			});
		go.store(true);
		for (auto &writer : writers)
			writer.join();
		require(signal.completed(), "a chunk was lost to a race");
	}
}

/* A feeder that scanned from an old state cannot publish over a newer one:
 * one writer's chunk is long and ends in half a marker, so that its result is
 * a partial match, and another's whole marker lands while it is still
 * scanning. The boot must read as completed, whichever finishes last. */
void test_a_slow_feeder_cannot_undo_a_completion()
{
	std::string slow(1U << 20, 'x');
	slow += "[    9.2] init: processing action (sys.boot_comp";
	std::string slow_init(1U << 20, 'x');
	slow_init += "init: init first stage";
	for (const std::string *chunk : {&slow, &slow_init}) {
		for (int round = 0; round != 100; ++round) {
			ConsoleBootSignal signal;
			std::atomic_bool go{};
			std::thread scanning{[&] {
				while (!go.load())
					std::this_thread::yield();
				feed(signal, *chunk);
			}};
			std::thread finishing{[&] {
				while (!go.load())
					std::this_thread::yield();
				/* After the other has read the state, well inside its scan. */
				std::this_thread::sleep_for(std::chrono::microseconds(300));
				feed(signal, line);
			}};
			go.store(true);
			scanning.join();
			finishing.join();
			require(signal.completed() && signal.init_observed(),
				"a slow feeder undid a completion it had not seen");
		}
	}
}

} // namespace

int main()
{
	test_a_line_in_one_write_completes();
	test_recovery_completes();
	test_apexds_word_completes();
	test_init_observed_follows_init_output();
	test_a_marker_split_anywhere_is_found();
	test_near_misses_complete_nothing();
	test_noise_around_a_marker();
	test_a_completed_signal_latches_until_reset();
	test_empty_and_null_input();
	test_concurrent_writers_lose_no_chunk();
	test_a_slow_feeder_cannot_undo_a_completion();
	std::puts("console boot signal tests passed");
	return EXIT_SUCCESS;
}
