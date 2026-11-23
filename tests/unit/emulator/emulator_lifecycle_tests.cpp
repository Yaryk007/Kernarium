// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The lifecycle calls reach the guest only through its management console, so
 * a datagram socket bound where the guest would bind its console sees exactly
 * what a guest would be asked, in order. The same socket orders the guest's
 * wake against the hibernation steps a test supplies: when the guest is woken,
 * its RAM and threads are already back.
 */
#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "guest_console.hpp"
#include "guest_console_test_support.hpp"
#include "guest_hibernation.hpp"
#include "kernel_threads.hpp"
#include "memory_pressure_response.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "thread_nap.hpp"
#include "uml_darwin_host.h"
#include "wait_until.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uml::testing::awaitRequest;
using uml::testing::bindServer;
using uml::testing::receiveRequest;
using uml::testing::require;
using uml::testing::sendReply;
using uml::testing::waitUntil;

/* Stands where the guest's console would be and answers every request the
 * way a guest that has the trim command would. */
class FakeGuestConsole final {
public:
	FakeGuestConsole()
	{
		const auto &location = ::uml::device::guest_console_location();
		require(location.has_value());
		path_ = location->console;
		descriptor_ = bindServer(path_);
	}

	FakeGuestConsole(const FakeGuestConsole &) = delete;
	FakeGuestConsole &operator=(const FakeGuestConsole &) = delete;

	~FakeGuestConsole()
	{
		(void)::close(descriptor_);
		(void)::unlink(path_.c_str());
	}

	/* Called on the console's thread with each request, before its reply. */
	std::function<void(const std::string &)> onRequest;
	/* A guest that answers this many "trim once" requests and then stops
	 * answering them, as one that sleeps through the pass does; negative
	 * answers them all. */
	int trimsAnsweredBeforeSilence {-1};
	/* A guest that cannot suspend: it refuses "suspend" for a reason that
	 * waiting does not cure (not busy), and keeps running. */
	bool refuseSuspend {false};
	/* A guest that takes "trim on" and "trim off" and never replies to them,
	 * as one that is busy for longer than a request waits. */
	bool silentToggles {false};
	/* A guest whose suspend is held, as by a freezer that cannot finish: it
	 * never replies to "suspend", answers the wake that aborts it, and then,
	 * for `lateFailure`, gives the suspend up on the request's own socket. */
	enum class Hold { none, silent, lateFailure };
	Hold holdSuspend {Hold::none};
	/* How the guest answers "sync": with the reply of a guest that has the
	 * command, as a kernel without it does ("Unknown command", so the host
	 * falls back to SysRq 's'), or not at all, as one whose sync waits for a
	 * disk the host holds. */
	enum class Sync { synced, unknown, silent };
	Sync sync {Sync::synced};
	/* What the guest's reply to "suspend" says of the suspend's own sync: a
	 * kernel before the statement says nothing, one with it says that the sync
	 * returned or that there was none (sync_on_suspend off). */
	enum class Suspend { unstated, synced, skipped };
	Suspend suspendStatement {Suspend::unstated};
	/* A guest that answers this many "sync" requests as `sync` says and then
	 * stops answering them; negative answers them all. */
	int syncsAnsweredBeforeSilence {-1};
	/* A kernel whose SysRq mask is off refuses SysRq 's'. */
	bool sysrqDisabled {false};
	/* How the guest answers "mem": with its figures, as a kernel that lacks
	 * the command does, with a line that is not the figures, not at all, or
	 * (deferred) with the figures just before it answers its next request,
	 * so that the next request is heard while "mem" still waits. */
	enum class Memory { figures, unknown, malformed, silent, deferred };
	Memory memory {Memory::figures};
	static constexpr std::string_view memoryFigures {
		"total=2147483648 available=1073741824 free=104857600"};

	/* Answers every request that arrives while `operation` runs, and returns
	 * them in order. */
	template<typename Operation>
	std::vector<std::string> serving(Operation &&operation)
	{
		std::vector<std::string> received;
		std::atomic<bool> finished {false};
		std::thread guest {[this, &received, &finished] {
			for (;;) {
				if (awaitRequest(descriptor_, 20)) {
					received.push_back(answer());
					continue;
				}
				if (finished.load()) {
					/* What the operation sent last. */
					if (!awaitRequest(descriptor_, 100))
						return;
				}
			}
		}};
		std::forward<Operation>(operation)();
		finished = true;
		guest.join();
		return received;
	}

	/* The console's path is gone, as the guest's is while it restarts: the
	 * requests that follow cannot be sent. */
	void unlinkConsole() const
	{
		(void)::unlink(path_.c_str());
	}

	/* The console is back, at the same path. The requests it was sent while it
	 * was gone are lost. */
	void relinkConsole()
	{
		(void)::close(descriptor_);
		descriptor_ = bindServer(path_);
	}

	/* Answers `count` requests in another thread while `operation` runs,
	 * and returns them in the order they arrived. */
	template<typename Operation>
	std::vector<std::string> requests(std::size_t count,
		Operation &&operation)
	{
		std::vector<std::string> received;
		std::thread guest {[this, count, &received] {
			for (std::size_t index = 0; index < count; ++index) {
				/* The operation sends at once; a missing request
				 * fails the comparison instead of waiting here. */
				if (!awaitRequest(descriptor_,
					    holdSuspend == Hold::none ? 10000 : 20000))
					return;
				received.push_back(answer());
			}
		}};
		std::forward<Operation>(operation)();
		guest.join();
		/* Nothing else arrives either. */
		require(!awaitRequest(descriptor_, 200));
		return received;
	}

private:
	std::string answer() const
	{
		sockaddr_un client {};
		socklen_t clientLength {};
		const auto command = receiveRequest(descriptor_, client, clientLength);
		if (onRequest)
			onRequest(command);
		if (command == "trim once" && trimsAnsweredBeforeSilence >= 0 &&
		    trimsHeard_++ >= trimsAnsweredBeforeSilence)
			return command;
		if (silentToggles && (command == "trim on" || command == "trim off"))
			return command;
		if (command == "suspend" && holdSuspend != Hold::none) {
			suspendClient_ = client;
			suspendClientLength_ = clientLength;
			return command;
		}
		std::string_view text = "running";
		std::uint32_t error = 0U;
		if (command == "sync" && syncsAnsweredBeforeSilence >= 0 &&
		    syncsHeard_++ >= syncsAnsweredBeforeSilence)
			return command;
		if (command == "sync") {
			switch (sync) {
			case Sync::synced:
				text = "synced in 1 ms";
				break;
			case Sync::unknown:
				text = "Unknown command";
				error = 1U;
				break;
			case Sync::silent:
				return command;
			}
		} else if (command == "mem") {
			switch (memory) {
			case Memory::figures:
				text = memoryFigures;
				break;
			case Memory::unknown:
				text = "Unknown command: mem";
				error = 1U;
				break;
			case Memory::malformed:
				text = "total=lots";
				break;
			case Memory::silent:
				return command;
			case Memory::deferred:
				memoryClient_ = client;
				memoryClientLength_ = clientLength;
				return command;
			}
		} else if (memoryClientLength_ != 0) {
			require(sendReply(descriptor_, memoryClient_,
				memoryClientLength_, 0U, memoryFigures),
				"the waiting memory request is still open");
			memoryClientLength_ = 0;
		}
		if (command == "suspend" && refuseSuspend) {
			text = "Suspend failed: refused by the test";
			error = 1U;
		} else if (command == "suspend") {
			switch (suspendStatement) {
			case Suspend::unstated:
				text = "suspended";
				break;
			case Suspend::synced:
				text = "suspended, synced";
				break;
			case Suspend::skipped:
				text = "suspended, not synced";
				break;
			}
		}
		if (command == "sysrq s") {
			if (sysrqDisabled) {
				text = "sysrq s disabled";
				error = 1U;
			} else {
				text = "sysrq s ok";
			}
		} else if (command == "trim once")
			text = "trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			       "released 0 KiB in 1 ms";
		else if (command.starts_with("trim "))
			text = std::string_view {command}.substr(5);
		/* A request that does not wait for its answer has closed its socket. */
		const bool replied =
			sendReply(descriptor_, client, clientLength, error, text);
		require(replied || command == "trim on" || command == "trim off",
			"the request's socket is still open");
		if (command == "wake" && holdSuspend == Hold::lateFailure)
			require(sendReply(descriptor_, suspendClient_,
				suspendClientLength_, 1U, "Suspend failed: -16"),
				"the aborted suspend's socket is still open");
		return command;
	}

	std::string path_;
	int descriptor_ {-1};
	mutable int trimsHeard_ {0};
	mutable int syncsHeard_ {0};
	mutable sockaddr_un suspendClient_ {};
	mutable socklen_t suspendClientLength_ {};
	mutable sockaddr_un memoryClient_ {};
	mutable socklen_t memoryClientLength_ {};
};

using Requests = std::vector<std::string>;

/* A handle with no kernel: the lifecycle calls reach only the console. */
uml::testing::Emulator makeEmulator()
{
	return uml::testing::create_emulator({
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
	});
}

/* A quiesced guest keeps what it holds, so it trims before it suspends. */
void quiesceTrimsBeforeItSuspends(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(3U, [emulator] {
		require(uml_emulator_quiesce(emulator, nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
}

/* Any request would wake a quiesced guest, so nothing reaches it until the
 * resume, which carries the trimming change after the wake. */
void aQuiescedGuestHearsOnlyTheResume(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(0U, [emulator] {
		require(uml_emulator_trim_page_cache(emulator) ==
			UML_EMULATOR_OK);
		require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
			UML_EMULATOR_OK);
		require(uml_emulator_quiesce(emulator, nullptr) == 0);
	}).empty());
	require(console.requests(2U, [emulator] {
		uml_emulator_resume(emulator);
	}) == Requests {"wake", "trim on"});
}

void aRunningGuestIsAskedAtOnce(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(1U, [emulator] {
		require(uml_emulator_set_page_cache_trimming(emulator, 0) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim off"});
	require(console.requests(1U, [emulator] {
		require(uml_emulator_trim_page_cache(emulator) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim once"});
	/* Resuming a running guest changes nothing it was told. */
	require(console.requests(0U, [emulator] {
		uml_emulator_resume(emulator);
	}).empty());
}

/* The memory pressure response's trim reaches a running guest as the
 * host's own does, and wakes nothing the host has put to sleep. */
void theMemoryPressureTrimAsksARunningGuest(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	using ::uml::runtime::TrimOutcome;
	require(console.requests(1U, [emulator] {
		require(uml::emulator::testing::trim_for_memory_pressure(emulator) ==
			TrimOutcome::done);
	}) == Requests {"trim once"});
}

/* The memory pressure response's gradual trimmer is one short message to a
 * running guest, sent when it changes what the guest runs, and it never ends a
 * trimmer the application asked for (nor the application's change the
 * response's). */
void theGradualPressureTrimmerIsOneMessageAndKeepsWhatTheApplicationAsked(
	uml_emulator *emulator, FakeGuestConsole &console)
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	/* A request that sends a message is `sent`, as it does not wait for the
	 * guest; one that finds the guest as asked sends nothing and is `done`. */
	const auto ask = [&](TrimKind kind, const Requests &expected) {
		require(console.requests(expected.size(), [&] {
			require(trim_for_memory_pressure(emulator, kind) ==
				(expected.empty() ? TrimOutcome::done : TrimOutcome::sent));
		}) == expected);
	};
	ask(TrimKind::gradual_on, {"trim on"});
	/* The guest runs as asked: asking again sends nothing. */
	ask(TrimKind::gradual_on, {});
	ask(TrimKind::gradual_off, {"trim off"});
	ask(TrimKind::gradual_off, {});

	/* The application's trimmer stays when the response ends its own. */
	require(console.requests(1U, [emulator] {
		require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim on"});
	ask(TrimKind::gradual_on, {});
	ask(TrimKind::gradual_off, {});
	/* And the response's stays when the application ends its own. */
	ask(TrimKind::gradual_on, {});
	require(console.requests(1U, [emulator] {
		require(uml_emulator_set_page_cache_trimming(emulator, 0) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim on"});
	ask(TrimKind::gradual_off, {"trim off"});
}

void aQuiescedGuestIsNotWokenForThePressureTrim(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	using ::uml::runtime::TrimOutcome;
	require(console.requests(3U, [emulator] {
		require(uml_emulator_quiesce(emulator, nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(console.requests(0U, [emulator] {
		require(uml::emulator::testing::trim_for_memory_pressure(emulator) ==
			TrimOutcome::quiesced);
	}).empty());
	require(console.requests(1U, [emulator] {
		uml_emulator_resume(emulator);
	}) == Requests {"wake"});
}

/* The gradual trimmer's request to a quiesced guest waits for the resume,
 * which carries the last of them after the wake. */
void aQuiescedGuestHearsTheGradualPressureTrimmerOnlyAtTheResume(
	uml_emulator *emulator, FakeGuestConsole &console)
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	require(console.requests(3U, [emulator] {
		require(uml_emulator_quiesce(emulator, nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(console.requests(0U, [emulator] {
		require(trim_for_memory_pressure(emulator, TrimKind::gradual_on) ==
			TrimOutcome::quiesced);
		require(trim_for_memory_pressure(emulator, TrimKind::gradual_off) ==
			TrimOutcome::quiesced);
		require(trim_for_memory_pressure(emulator, TrimKind::gradual_on) ==
			TrimOutcome::quiesced);
	}).empty());
	require(console.requests(2U, [emulator] {
		uml_emulator_resume(emulator);
	}) == Requests {"wake", "trim on"});
	/* Ended again, so the handle is as it was. */
	require(console.requests(1U, [emulator] {
		require(trim_for_memory_pressure(emulator, TrimKind::gradual_off) ==
			TrimOutcome::sent);
	}) == Requests {"trim off"});
}

/* A trimmer request the console did not take leaves what the guest runs and
 * what is wanted apart. The next call settles it, whichever call it is, and
 * compares what is wanted with what the guest took, not with the wish before. */
void aGradualRequestThatWasNotSentIsOwedUntilAnyCallSettlesIt()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto turnedOnThenLostOff = [](FakeGuestConsole &console,
		uml_emulator *emulator) {
		require(console.requests(1U, [&] {
			require(trim_for_memory_pressure(emulator, TrimKind::gradual_on) ==
				TrimOutcome::sent);
		}) == Requests {"trim on"});
		/* The console is gone when the response ends the trimmer. */
		console.unlinkConsole();
		require(trim_for_memory_pressure(emulator, TrimKind::gradual_off) ==
			TrimOutcome::unreachable);
		console.relinkConsole();
	};
	{
		/* The same request, asked again, is sent: it is not taken for done. */
		const auto emulator = makeEmulator();
		FakeGuestConsole console;
		turnedOnThenLostOff(console, emulator.get());
		require(console.requests(1U, [&] {
			require(trim_for_memory_pressure(emulator.get(),
				TrimKind::gradual_off) == TrimOutcome::sent);
		}) == Requests {"trim off"});
		require(console.requests(0U, [&] {
			require(trim_for_memory_pressure(emulator.get(),
				TrimKind::gradual_off) == TrimOutcome::done);
		}).empty());
	}
	{
		/* The application's request settles it. */
		const auto emulator = makeEmulator();
		FakeGuestConsole console;
		turnedOnThenLostOff(console, emulator.get());
		require(console.requests(1U, [&] {
			require(uml_emulator_set_page_cache_trimming(emulator.get(), 0) ==
				UML_EMULATOR_OK);
		}) == Requests {"trim off"});
		require(console.requests(0U, [&] {
			require(trim_for_memory_pressure(emulator.get(),
				TrimKind::gradual_off) == TrimOutcome::done);
		}).empty());
	}
	{
		/* A request for what the guest still runs sends nothing, whatever was
		 * wished in between. */
		const auto emulator = makeEmulator();
		FakeGuestConsole console;
		turnedOnThenLostOff(console, emulator.get());
		require(console.requests(0U, [&] {
			require(trim_for_memory_pressure(emulator.get(),
				TrimKind::gradual_on) == TrimOutcome::done);
		}).empty());
	}
}

/* A trimmer change the resume could not carry stays owed: the next request
 * sends it. */
void aResumeThatCouldNotReplayTheTrimmerLeavesItOwed()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_on) ==
			TrimOutcome::quiesced);
	}).empty());
	/* The guest hears the wake and is then gone for the replay. */
	console.onRequest = [&console](const std::string &command) {
		if (command == "wake")
			console.unlinkConsole();
	};
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	console.onRequest = nullptr;
	console.relinkConsole();
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_on) ==
			TrimOutcome::sent);
	}) == Requests {"trim on"});
}

/* The pressure response's trimmer request is not waited for, so a console that
 * never answers cannot hold the lifecycle mutex; the application's request
 * keeps its synchronous contract, and what it sent stands as asked. */
void aSilentConsoleHoldsNeitherThePressureTrimmerNorTheApplicationsRecord()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.silentToggles = true;
	const auto started = std::chrono::steady_clock::now();
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_on) ==
			TrimOutcome::sent);
	}) == Requests {"trim on"});
	/* Real time: the guest's answer window is seconds long. */
	require(std::chrono::steady_clock::now() - started <
		std::chrono::milliseconds {1000}, "the pressure trimmer waited");
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_off) ==
			TrimOutcome::sent);
	}) == Requests {"trim off"});

	/* The application's request waits for its answer, and gets none. */
	const auto asked = std::chrono::steady_clock::now();
	require(console.requests(1U, [&] {
		require(uml_emulator_set_page_cache_trimming(emulator.get(), 1) ==
			-UML_EMULATOR_RUNTIME_ERROR);
	}) == Requests {"trim on"});
	require(std::chrono::steady_clock::now() - asked >=
		std::chrono::milliseconds {1500}, "the application's request did not wait");
	/* Sent, the guest may have taken it: it is wanted and counted as applied,
	 * so the response neither repeats it nor ends it. */
	require(console.requests(0U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_on) ==
			TrimOutcome::done);
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_off) ==
			TrimOutcome::done);
	}).empty());
}

/* Whatever order the application's and the response's changes arrive in, what
 * the guest last took is what either of them wants. */
void theTrimmerEndsAsEitherOfThemLastWanted()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto askUntilHeard = [](uml_emulator *emulator, TrimKind kind) {
		/* A response that meets another call asks again. */
		const auto until = std::chrono::steady_clock::now() +
			std::chrono::seconds {20};
		TrimOutcome outcome = TrimOutcome::busy;
		while (outcome == TrimOutcome::busy &&
		       std::chrono::steady_clock::now() < until) {
			outcome = trim_for_memory_pressure(emulator, kind);
			if (outcome == TrimOutcome::busy)
				std::this_thread::yield();
		}
		return outcome;
	};
	/* The application's change, asked again while the console cannot take it:
	 * the response's requests are sent without waiting, and a console that has
	 * not read them yet has no room in its queue (Darwin refuses a datagram it
	 * has no room for instead of waiting for room). A change that was not sent
	 * did not happen. */
	const auto changeUntilSent = [](uml_emulator *emulator, bool on) {
		const auto until = std::chrono::steady_clock::now() +
			std::chrono::seconds {20};
		int result = -UML_EMULATOR_UNSUPPORTED;
		while (result == -UML_EMULATOR_UNSUPPORTED &&
		       std::chrono::steady_clock::now() < until) {
			result = uml_emulator_set_page_cache_trimming(emulator, on ? 1 : 0);
			if (result == -UML_EMULATOR_UNSUPPORTED)
				std::this_thread::yield();
		}
		return result;
	};
	for (const bool application : {false, true}) {
		for (const bool pressure : {false, true}) {
			const auto emulator = makeEmulator();
			FakeGuestConsole console;
			const auto heard = console.serving([&] {
				std::thread app {[&] {
					for (int index = 0; index < 60; ++index) {
						const int result = changeUntilSent(
							emulator.get(), index % 2 == 0);
						require(result == UML_EMULATOR_OK,
							"the application's change returned " +
							std::to_string(result));
					}
				}};
				std::thread response {[&] {
					for (int index = 0; index < 60; ++index)
						(void)askUntilHeard(emulator.get(), index % 2 == 0 ?
							TrimKind::gradual_on : TrimKind::gradual_off);
				}};
				app.join();
				response.join();
				const int result = changeUntilSent(emulator.get(), application);
				require(result == UML_EMULATOR_OK,
					"the application's last change returned " +
					std::to_string(result));
				const auto last = askUntilHeard(emulator.get(), pressure ?
					TrimKind::gradual_on : TrimKind::gradual_off);
				require(last == TrimOutcome::sent || last == TrimOutcome::done);
			});
			const bool wanted = application || pressure;
			std::string last = "trim off";
			for (const auto &command : heard)
				if (command == "trim on" || command == "trim off")
					last = command;
			require(last == (wanted ? "trim on" : "trim off"),
				"the guest was left as the earlier wish");
			/* And it is the state the handle holds: asking again sends
			 * nothing. */
			require(console.serving([&] {
				require(askUntilHeard(emulator.get(), pressure ?
					TrimKind::gradual_on : TrimKind::gradual_off) ==
					TrimOutcome::done);
			}).empty());
		}
	}
}

/* The next kernel starts with its trimmer off and nothing wanting it on: a
 * wish the application or the response had before the restart is not carried
 * over, and the response's request is sent again. */
void aRestartedGuestStartsWithTheTrimmerOffAndUnwanted()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	require(console.requests(1U, [&] {
		require(uml_emulator_set_page_cache_trimming(emulator.get(), 1) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim on"});
	uml::emulator::testing::reset_guest_state(emulator.get());
	/* The application's wish went with the kernel: the response's request
	 * is the only one now, and the guest is asked. */
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_on) ==
			TrimOutcome::sent);
	}) == Requests {"trim on"});
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(), TrimKind::gradual_off) ==
			TrimOutcome::sent);
	}) == Requests {"trim off"});
}

void withoutAConsoleNothingWaits(uml_emulator *emulator)
{
	require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
		-UML_EMULATOR_UNSUPPORTED);
	require(uml_emulator_trim_page_cache(emulator) ==
		-UML_EMULATOR_UNSUPPORTED);
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(uml_emulator_get_guest_memory(emulator, &memory) ==
		-UML_EMULATOR_UNSUPPORTED);
	require(uml_emulator_set_page_cache_trimming(nullptr, 1) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_trim_page_cache(nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml::emulator::testing::trim_for_memory_pressure(emulator) ==
		::uml::runtime::TrimOutcome::unreachable);
	/* A trimmer no one could turn on has nothing to turn off. */
	require(uml::emulator::testing::trim_for_memory_pressure(emulator,
		::uml::runtime::TrimKind::gradual_on) ==
		::uml::runtime::TrimOutcome::unreachable);
	require(uml::emulator::testing::trim_for_memory_pressure(emulator,
		::uml::runtime::TrimKind::gradual_off) ==
		::uml::runtime::TrimOutcome::done);
	require(uml_emulator_quiesce_resident(nullptr, nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
}

/* ---- What a settle call says of the guest. ---- */

using Storage = uml_emulator_guest_storage;
using Suspension = uml_emulator_guest_suspension;

/* A report that no call has filled. */
uml_emulator_settle blankSettle()
{
	return {.struct_size = sizeof(uml_emulator_settle),
		.reserved = 0xdeadU,
		.guest_storage = static_cast<Storage>(99),
		.guest_suspension = static_cast<Suspension>(99)};
}

/* What an application takes as settled: the guest confirmed its writeback, or
 * no guest ran. Nothing else may be called settled. */
bool storageIsSettled(const uml_emulator_settle &settle)
{
	return settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_CONFIRMED ||
		settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_NO_GUEST;
}

/* The report of `call` against `console`, and the requests it sent. */
struct Settled final {
	Requests heard;
	uml_emulator_settle settle {blankSettle()};
	std::chrono::milliseconds took {};
};

template<typename Call>
Settled settleAgainst(FakeGuestConsole &console, std::size_t requests,
	Call &&call)
{
	Settled result;
	const auto started = std::chrono::steady_clock::now();
	result.heard = console.requests(requests, [&] {
		require(std::forward<Call>(call)(result.settle) == 0,
			"the host's files flushed");
		result.took = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started);
	});
	return result;
}

void requireReport(const Settled &result, Storage storage,
	Suspension suspension, const char *what)
{
	require(result.settle.guest_storage == storage &&
		result.settle.guest_suspension == suspension &&
		result.settle.reserved == 0U, what);
}

/* The guest's own reply to "sync" is the confirmation, and nothing else is
 * sent. */
void aSyncIsConfirmedByTheGuestsReply()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	const auto result = settleAgainst(console, 1U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_sync(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED,
		"a guest that replied to its sync is confirmed");
	require(storageIsSettled(result.settle));
	require(result.took < std::chrono::milliseconds {1500},
		"a confirmed sync does not sleep a fixed time");
}

/* A guest that refuses writeback is not settled, whatever the host's flush
 * said: neither the command nor SysRq's 's' is available. */
void aRefusedSyncIsNotSettled()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::unknown;
	console.sysrqDisabled = true;
	const auto result = settleAgainst(console, 2U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_sync(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync", "sysrq s"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_REFUSED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED, "a refusal is reported");
	require(!storageIsSettled(result.settle));
}

/* An older accepted kernel has no "sync": SysRq's 's' only starts a
 * writeback, so the guest is asked and not confirmed, and no application may
 * call its storage settled. */
void anOlderKernelIsOnlyAskedAndStaysUnconfirmed()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::unknown;
	const auto result = settleAgainst(console, 2U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_sync(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync", "sysrq s"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED,
		"a started writeback is not a confirmed one");
	require(!storageIsSettled(result.settle));
	require(result.took >= std::chrono::milliseconds {1400},
		"the fallback gives the writeback its settling period");
}

/* A guest that gives no reply, as one whose sync waits for a disk the host
 * holds, is unconfirmed after the call's bound, and is not asked twice. */
void aSyncWithoutAReplyIsUnconfirmedAfterItsBound()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::silent;
	const auto result = settleAgainst(console, 1U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_sync(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync"}, "silence is not followed by SysRq");
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED, "silence is unconfirmed");
	require(!storageIsSettled(result.settle));
	require(result.took >= std::chrono::milliseconds {3400} &&
		result.took < std::chrono::milliseconds {5500},
		"a sync waits for its reply for 3.5 s");
}

/* No guest runs: there is nothing to settle, and the report says so without a
 * console. */
void withoutAGuestThereIsNothingToSettle()
{
	const auto emulator = makeEmulator();
	auto settle = blankSettle();
	require(uml_emulator_sync(emulator.get(), &settle) == 0);
	require(settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_NO_GUEST &&
		settle.guest_suspension == UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED);
	require(storageIsSettled(settle));
	settle = blankSettle();
	require(uml_emulator_quiesce(emulator.get(), &settle) == 0);
	require(settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_NO_GUEST &&
		settle.guest_suspension == UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED);
	settle = blankSettle();
	require(uml_emulator_quiesce_resident(emulator.get(), &settle) == 0);
	require(storageIsSettled(settle));
}

/* What a kernel that runs on the handle saw of the settle calls, with no
 * console where the guest would bind it. */
struct RunningGuest final {
	uml_emulator *emulator {};
	uml_emulator_settle sync {blankSettle()};
	uml_emulator_settle quiesce {blankSettle()};
	int syncResult {-1};
	int quiesceResult {-1};
};

RunningGuest runningGuest;

int kernelThatSettles(int, char *const[], char *const[])
{
	runningGuest.syncResult =
		uml_emulator_sync(runningGuest.emulator, &runningGuest.sync);
	runningGuest.quiesceResult =
		uml_emulator_quiesce(runningGuest.emulator, &runningGuest.quiesce);
	return 0;
}

/* A guest that runs but whose console cannot be reached is not settled: it
 * may hold dirty pages, and nothing was asked. */
void aRunningGuestWithoutAConsoleIsNotSettled()
{
	const auto emulator = uml::testing::make_emulator(kernelThatSettles);
	runningGuest = {.emulator = emulator.get()};
	const char *arguments[] {"uml-lifecycle"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 0);
	require(runningGuest.syncResult == 0 && runningGuest.quiesceResult == 0);
	for (const auto &settle : {runningGuest.sync, runningGuest.quiesce}) {
		require(settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_UNREACHABLE,
			"a running guest nobody could ask is unreachable");
		require(!storageIsSettled(settle));
	}
	require(runningGuest.sync.guest_suspension ==
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED &&
		runningGuest.quiesce.guest_suspension ==
		UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED);
}

/* The calls refuse what they cannot fill, and leave the report alone. */
void theReportIsChecked(uml_emulator *emulator)
{
	auto settle = blankSettle();
	require(uml_emulator_sync(nullptr, &settle) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_quiesce(nullptr, &settle) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	settle.struct_size = sizeof(settle) - 1U;
	require(uml_emulator_sync(emulator, &settle) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_quiesce(emulator, &settle) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_quiesce_resident(emulator, &settle) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(settle.reserved == 0xdeadU &&
		settle.guest_storage == static_cast<Storage>(99),
		"a refused call wrote its report");
}

/* A quiesce that suspends is confirmed by the guest's reply to its sync, which
 * goes first, and the trim after it: a suspend reply that states nothing of
 * the suspend's own sync confirms nothing of the storage. A sync that finds
 * the guest held asks nothing. */
void aSuspendedGuestIsConfirmed()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	const auto result = settleAgainst(console, 3U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync", "trim once", "suspend"},
		"the writeback comes before the trim");
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a guest that answered its sync and suspended is confirmed");
	require(storageIsSettled(result.settle));
	/* A sync that finds the guest quiesced asks nothing, which would end the
	 * suspend, and reports what the quiesce did. */
	auto settle = blankSettle();
	require(console.requests(0U, [&] {
		require(uml_emulator_sync(emulator.get(), &settle) == 0);
	}).empty());
	require(settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_CONFIRMED &&
		settle.guest_suspension == UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED);
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
}

/* A suspend that was refused leaves the guest running: it is written back
 * once more, and that decides whether its storage is confirmed. */
void aRefusedSuspendIsWrittenBackAndNotSuspended()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.refuseSuspend = true;
	const auto result = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync", "trim once", "suspend", "sync"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED,
		"the storage is confirmed and the guest runs");
}

/* An older kernel's SysRq 's' only starts the writeback, and its suspend
 * reply, which states nothing of its own sync (the guest's sync_on_suspend
 * decides whether it syncs), confirms nothing: the storage stays unconfirmed
 * whether the guest sleeps or not. */
void anOlderKernelIsNeverConfirmedBySuspend()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::unknown;
	const auto suspended = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(suspended.heard ==
		Requests {"sync", "sysrq s", "trim once", "suspend"},
		"the fixed settling period waits for the failed suspend only");
	require(suspended.took < std::chrono::milliseconds {1400});
	requireReport(suspended, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a suspend does not confirm a started writeback");
	require(!storageIsSettled(suspended.settle));
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
	console.refuseSuspend = true;
	const auto refused = settleAgainst(console, 6U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(refused.heard == Requests {"sync", "sysrq s", "trim once",
		"suspend", "sync", "sysrq s"});
	requireReport(refused, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED,
		"a started writeback is not a confirmed one");
	require(!storageIsSettled(refused.settle));
}

/* A guest that sleeps but refused its writeback is not settled either when
 * its suspend states nothing: the suspend is no confirmation then, and the
 * report says both. */
void aSuspendDoesNotSettleARefusedWriteback()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::unknown;
	console.sysrqDisabled = true;
	const auto result = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(result.heard ==
		Requests {"sync", "sysrq s", "trim once", "suspend"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_REFUSED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a refused writeback stays refused beside a suspension");
	require(!storageIsSettled(result.settle));
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

/* A kernel that states in its suspend reply that the suspend's own sync
 * returned has written the storage back, whatever its writeback request got:
 * the sync followed it, so the storage is confirmed, and the guest sleeps. A
 * statement that there was no sync, or none, adds nothing, and the storage
 * stays what the writeback request made it. */
void aSuspendThatStatesItSyncedConfirmsTheStorage()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::unknown;
	console.sysrqDisabled = true;
	console.suspendStatement = FakeGuestConsole::Suspend::synced;
	const auto refused = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(refused.heard ==
		Requests {"sync", "sysrq s", "trim once", "suspend"});
	requireReport(refused, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a suspend that synced confirms what the writeback was refused");
	require(storageIsSettled(refused.settle));
	/* The same report from a guest the host holds, with nothing asked. */
	auto again = blankSettle();
	require(console.requests(0U, [&] {
		require(uml_emulator_sync(emulator.get(), &again) == 0);
	}).empty());
	require(again.guest_storage == UML_EMULATOR_GUEST_STORAGE_CONFIRMED &&
		again.guest_suspension == UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED);
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
	/* The resident quiesce reads the reply the same way. */
	const auto resident = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce_resident(emulator.get(), &settle);
		});
	requireReport(resident, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"the resident quiesce takes the statement too");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

void aSuspendThatStatesItDidNotSyncConfirmsNothingMore()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.suspendStatement = FakeGuestConsole::Suspend::skipped;
	const auto answered = settleAgainst(console, 3U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	requireReport(answered, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"the sync's own reply still confirms");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
	console.sync = FakeGuestConsole::Sync::unknown;
	const auto started = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	requireReport(started, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a suspend that did not sync adds no confirmation");
	require(!storageIsSettled(started.settle));
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

/* A guest that refuses to write back and to suspend is reported refused. */
void aRefusingGuestIsNotSettledByAQuiesce()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::unknown;
	console.sysrqDisabled = true;
	console.refuseSuspend = true;
	const auto result = settleAgainst(console, 6U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync", "sysrq s", "trim once",
		"suspend", "sync", "sysrq s"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_REFUSED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED, "a refusal is reported");
}

/* A sync that gets no reply is a guest that is slow, or stuck on its I/O: the
 * trim is not sent after it, and a suspend that follows without a statement
 * confirms nothing, so the storage stays unconfirmed beside it; with the
 * suspend refused no second sync is tried either. */
void aQuiesceWhoseSyncGetsNoReplySkipsTheTrim()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::silent;
	const auto suspended = settleAgainst(console, 2U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(suspended.heard == Requests {"sync", "suspend"},
		"no trim is sent after a sync that got no reply");
	requireReport(suspended, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a suspend that states nothing does not confirm the storage the sync "
		"did not");
	require(!storageIsSettled(suspended.settle));
	require(suspended.took >= std::chrono::milliseconds {1900} &&
		suspended.took < std::chrono::milliseconds {4000},
		"the opening sync waits for 2 s");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
	console.refuseSuspend = true;
	const auto refused = settleAgainst(console, 2U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(refused.heard == Requests {"sync", "suspend"});
	requireReport(refused, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED, "no confirmation");
}

/* The same guest, when its suspend says its own sync returned, is confirmed:
 * that sync is the explicit statement the sync request did not get. One whose
 * suspend says there was no sync stays unconfirmed. */
void aSuspendThatStatesItSyncedConfirmsAfterASilentSync()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.sync = FakeGuestConsole::Sync::silent;
	console.suspendStatement = FakeGuestConsole::Suspend::synced;
	const auto synced = settleAgainst(console, 2U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(synced.heard == Requests {"sync", "suspend"},
		"no trim is sent after a sync that got no reply");
	requireReport(synced, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"the suspend's own sync confirms what the first did not");
	require(storageIsSettled(synced.settle));
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
	console.suspendStatement = FakeGuestConsole::Suspend::skipped;
	const auto skipped = settleAgainst(console, 2U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	requireReport(skipped, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED,
		"a suspend that did not sync confirms nothing");
	require(!storageIsSettled(skipped.settle));
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

/* The writeback after a refused suspend waits for 1.5 s, and a guest that
 * does not reply then is unconfirmed. */
void aFinalWritebackWithoutAReplyIsUnconfirmed()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.syncsAnsweredBeforeSilence = 1;
	console.refuseSuspend = true;
	const auto result = settleAgainst(console, 4U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(result.heard == Requests {"sync", "trim once", "suspend", "sync"});
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED, "the last sync got no reply");
	require(result.took >= std::chrono::milliseconds {1400});
}

/* A lifecycle call that keeps the mutex for longer than a sync will wait for
 * it leaves the storage unconfirmed: the sync asks nothing and flushes the
 * host's files, within its bound. */
void aSyncBesideALongLifecycleCallIsUnconfirmed()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	std::mutex mutex;
	std::condition_variable changed;
	bool trimming {false};
	bool released {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "trim once")
			return;
		std::unique_lock lock {mutex};
		trimming = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {5},
			[&] { return released; });
	};
	Settled result;
	const auto heard = console.requests(1U, [&] {
		std::thread trim {[&] {
			(void)uml_emulator_trim_page_cache(emulator.get());
		}};
		{
			std::unique_lock lock {mutex};
			require(changed.wait_for(lock, std::chrono::seconds {10},
				[&] { return trimming; }), "the trim never reached the guest");
		}
		const auto started = std::chrono::steady_clock::now();
		require(uml_emulator_sync(emulator.get(), &result.settle) == 0);
		result.took = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started);
		{
			const std::lock_guard lock {mutex};
			released = true;
		}
		changed.notify_all();
		trim.join();
	});
	require(heard == Requests {"trim once"}, "the sync asked the guest");
	requireReport(result, UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED,
		"a busy lifecycle call is not confirmation");
	require(result.took >= std::chrono::milliseconds {900} &&
		result.took < std::chrono::milliseconds {2500},
		"the lifecycle lock is waited for for 1 s");
}

/* ---- Hibernating the guest's RAM around a quiesce. ---- */

/* What happened, in order, on every thread: hibernation steps and the
 * guest's console requests share it. */
class Log final {
public:
	void add(std::string entry)
	{
		const std::lock_guard lock {mutex_};
		entries_.push_back(std::move(entry));
	}

	[[nodiscard]] std::vector<std::string> take()
	{
		const std::lock_guard lock {mutex_};
		return std::exchange(entries_, {});
	}

private:
	std::mutex mutex_;
	std::vector<std::string> entries_;
};

Log hibernationLog;
std::atomic<int> restoreFailures {0};

struct Steps final : uml::runtime::HibernationSteps {
	bool restoreSucceeds {true};
	bool holdSucceeds {true};
	/* Called on the quiescing thread as the steps run. */
	std::function<void()> atHold;
	std::function<void()> atWrite;
	/* The write waits for the abort request that resume sends. */
	bool writeWaitsForAbort {false};
	std::atomic<bool> aborted {false};
	std::atomic<bool> writing {false};

	bool prepare() noexcept override
	{
		hibernationLog.add("prepare");
		return true;
	}
	bool hold(uml::runtime::HibernationReport &,
		uml::runtime::HoldCondition) noexcept override
	{
		hibernationLog.add("hold");
		if (atHold)
			atHold();
		return holdSucceeds;
	}
	bool write(uml::runtime::HibernationReport &) noexcept override
	{
		if (atWrite)
			atWrite();
		writing = true;
		if (writeWaitsForAbort) {
			const bool abortedInTime = uml::testing::waitUntil(
				[this] { return aborted.load(); },
				std::chrono::seconds {10},
				std::chrono::milliseconds {2});
			hibernationLog.add(
				abortedInTime ? "write:aborted" : "write:timeout");
		} else {
			hibernationLog.add("write");
		}
		return true;
	}
	bool restore(uml::runtime::HibernationReport &) noexcept override
	{
		hibernationLog.add("restore");
		return restoreSucceeds;
	}
	void release() noexcept override { hibernationLog.add("release"); }
	void set_abort(bool abort) noexcept override
	{
		if (abort) {
			hibernationLog.add("abort");
			aborted = true;
		} else {
			aborted = false;
		}
	}
};

void countRestoreFailure() noexcept
{
	hibernationLog.add("failure");
	++restoreFailures;
}

/* Installs `steps` on `emulator`; they stay owned by it, and the test keeps a
 * pointer to flip switches. */
Steps *installSteps(uml_emulator *emulator)
{
	auto steps = std::make_unique<Steps>();
	Steps *const raw = steps.get();
	uml::emulator::testing::use_hibernation(emulator, std::move(steps),
		&countRestoreFailure);
	return raw;
}

using Entries = std::vector<std::string>;

/* The wake is the first thing the guest can act on, so by then the RAM must be
 * whole and every thread released. */
void aHibernatedGuestIsWholeBeforeItIsWoken()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(hibernationLog.take() == Entries {"console:sync",
		"console:trim once", "console:suspend", "prepare", "hold", "write"},
		"a quiesce hibernates after the guest is asleep");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "restore", "release",
		"console:wake"}, "the RAM is back and the threads run before the wake");
}

void aSecondQuiesceKeepsTheHibernation()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}).empty());
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"},
		"a guest that is already hibernated is not hibernated again");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

/* A host short of the space an image takes quiesces resident: the guest
 * sleeps and its threads are held, and not a byte of RAM is written. */
void aResidentQuiesceWritesNoRam()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce_resident(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"},
		"the guest is trimmed and suspended as by any quiesce");
	require(hibernationLog.take() == Entries {"console:sync",
		"console:trim once", "console:suspend"}, "the hibernation is not prepared, held or written");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "console:wake"},
		"nothing was taken out of the RAM, so nothing is restored");
	/* The next quiesce is the host's own choice again. */
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(hibernationLog.take() == Entries {"console:sync",
		"console:trim once", "console:suspend", "prepare", "hold", "write"},
		"a quiesce after a resident one hibernates");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

/* The first hold stands: a guest already hibernated is not made resident, and
 * one held resident is not hibernated behind its holder's back. */
void aQuiesceKeepsWhateverHoldsTheGuest()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce_resident(emulator.get(), nullptr) == 0);
	}).empty());
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"},
		"a hibernated guest stays hibernated");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce_resident(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}).empty());
	require(hibernationLog.take().empty(),
		"a guest held resident is not hibernated by a later quiesce");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

void aGuestThatCouldNotSuspendIsNotHibernated()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.refuseSuspend = true;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	const auto requests = console.requests(4U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	});
	require(requests == Requests {"sync", "trim once", "suspend", "sync"},
		"a refused suspend is written back and left running, never stopped");
	require(hibernationLog.take().empty(),
		"only a system suspension is hibernated");
	require(console.requests(0U, [&] { uml_emulator_resume(emulator.get()); })
			.empty(),
		"a guest that kept running has nothing to be woken from");
}

/* A suspend with no reply at its deadline is aborted with a wake, not taken
 * for a suspended guest, which would be hibernated while its freezer still
 * runs. The request's final refusal says the guest runs: its resume sends
 * nothing. */
void aSuspendThatTimedOutIsNotHibernated()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.holdSuspend = FakeGuestConsole::Hold::lateFailure;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(5U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend", "wake", "sync"},
		"a suspend that does not reply is aborted and written back");
	require(hibernationLog.take().empty(),
		"only a confirmed system suspension is hibernated");
	/* A guest known to run has no sleep to end, so it is asked. */
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
			UML_EMULATOR_OK);
	}) == Requests {"mem"}, "a guest known to run was not asked");
	require(console.requests(0U, [&] { uml_emulator_resume(emulator.get()); })
			.empty(),
		"a guest known to run has nothing to be woken from");
}

/* Without a final word the guest may be asleep, so the resume wakes it. */
void aSuspendThatNeverAnsweredIsWokenAtTheResume()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.holdSuspend = FakeGuestConsole::Hold::silent;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(5U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend", "wake", "sync"});
	require(hibernationLog.take().empty(),
		"an unconfirmed suspension is not hibernated");
	/* It may be asleep, and a request would end that sleep. */
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(console.requests(0U, [&] {
		require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
			-UML_EMULATOR_UNSUPPORTED);
	}).empty(), "a guest that may be asleep was asked for its memory");
	/* What the quiesce answered is not repeated while the guest may have run:
	 * its wake ended the suspend, and it can have written since. A sync and a
	 * quiesce each ask it. */
	auto settle = blankSettle();
	require(console.requests(1U, [&] {
		require(uml_emulator_sync(emulator.get(), &settle) == 0);
	}) == Requests {"sync"}, "an unconfirmed guest was not asked again");
	require(settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_CONFIRMED &&
		settle.guest_suspension == UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED);
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
			== Requests {"wake"},
		"an unconfirmed guest is woken at the resume");
}

void aGuestWithoutAConsoleIsNotHibernated()
{
	const auto emulator = makeEmulator();
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	require(hibernationLog.take().empty(), "nothing suspended, nothing to do");
	uml_emulator_resume(emulator.get());
}

/* A failed restore must not wake anything: the guest would run on missing
 * memory. The failure handler ends the process in production. */
void aFailedRestoreWakesNothing()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	Steps *const steps = installSteps(emulator.get());
	steps->restoreSucceeds = false;
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	(void)hibernationLog.take();
	require(console.requests(0U, [&] {
		uml_emulator_resume(emulator.get());
	}).empty(), "a guest whose RAM is not back is not woken");
	require(hibernationLog.take() == Entries {"abort", "restore", "failure"},
		"the failure is reported and nothing is released");
	require(restoreFailures.load() == 1, "once");
	/* The guest stays quiesced; a later resume tries again. */
	steps->restoreSucceeds = true;
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "restore", "release",
		"console:wake"}, "and wakes it once the RAM is back");
}

/* A resume that arrives while the image is still being written does not wait
 * for the whole write: it asks the write to stop, then restores. */
void aResumeDuringTheWriteStopsTheWrite()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	Steps *const steps = installSteps(emulator.get());
	steps->writeWaitsForAbort = true;
	(void)hibernationLog.take();
	/* The guest answers the quiesce's three requests and the resume's wake. */
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(4U, [&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		});
	}};
	require(uml::testing::waitUntil([&] { return steps->writing.load(); },
		std::chrono::seconds {5}, std::chrono::milliseconds {2}),
		"the write must start");
	uml_emulator_resume(emulator.get());
	quiescing.join();
	require(heard == Requests {"sync", "trim once", "suspend", "wake"});
	const auto entries = hibernationLog.take();
	require(entries == Entries {"prepare", "hold", "abort", "write:aborted",
		"restore", "release"} || entries == Entries {"prepare", "hold",
		"abort", "write:aborted", "abort", "restore", "release"},
		"the resume stops the write, then restores");
}

/* Threads that are held cannot see a stop, so a stop restores first. */
void aStopBringsTheRamBack()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	(void)hibernationLog.take();
	uml_emulator_request_stop(emulator.get());
	require(hibernationLog.take() == Entries {"abort", "restore", "release"},
		"a stop runs the threads that were held");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

/* ---- The guest's disks while their locks are released. ---- */

/* A disk the way the kernel has one: opened and locked through the host
 * bridge, as os_lock_file locks it. */
class GuestDisk final {
public:
	GuestDisk()
	{
		const char *const directory = std::getenv("TMPDIR");
		require(directory != nullptr, "the test has a private directory");
		path_ = std::string {directory} + "/guest-disk.img";
		descriptor_ = darwin_uml_open(path_.c_str(),
			O_RDWR | O_CREAT | O_TRUNC, 0600);
		require(descriptor_ >= 0, "open the guest disk");
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		require(darwin_uml_fcntl(descriptor_, F_SETLK,
			reinterpret_cast<long>(&lock)) == 0, "lock the guest disk");
	}

	GuestDisk(const GuestDisk &) = delete;
	GuestDisk &operator=(const GuestDisk &) = delete;

	~GuestDisk()
	{
		(void)::close(descriptor_);
		(void)::unlink(path_.c_str());
	}

	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

	/* Whether another process cannot lock the disk, as a second host would
	 * find it. A forked child asks, through the descriptor it inherits: the
	 * kernel's lock belongs to the open file description, which the child
	 * shares, and conflicts with a lock query from any other process. */
	[[nodiscard]] bool lockedByAnotherProcess() const
	{
		int result[2];
		require(::pipe(result) == 0);
		const pid_t child = ::fork();
		require(child >= 0);
		if (child == 0) {
			(void)::close(result[0]);
			struct flock query {};
			query.l_type = F_WRLCK;
			query.l_whence = SEEK_SET;
			const char answer = ::fcntl(descriptor_, F_GETLK, &query) == 0 ?
				(query.l_type == F_UNLCK ? 'n' : 'y') : 'e';
			::_exit(::write(result[1], &answer, 1) == 1 ? 0 : 1);
		}
		(void)::close(result[1]);
		char answer = 'e';
		require(::read(result[0], &answer, 1) == 1);
		(void)::close(result[0]);
		int status = 0;
		require(::waitpid(child, &status, 0) == child);
		require(answer != 'e', "query the disk's lock");
		return answer == 'y';
	}

private:
	std::string path_;
	int descriptor_ {-1};
};

/* A thread of the kernel that writes the guest's disk whenever it runs, as a
 * guest does that its own wake source has woken. */
class WakingGuest final {
public:
	explicit WakingGuest(int disk) : disk_{disk}
	{
		require(::uml::runtime::kernel_pthread_create(&thread_, nullptr,
			&writeUntilStopped, this) == 0, "create the guest's thread");
		require(runsAgain(), "the guest's thread must run");
	}

	WakingGuest(const WakingGuest &) = delete;
	WakingGuest &operator=(const WakingGuest &) = delete;

	~WakingGuest()
	{
		stop_.store(true);
		require(::pthread_join(thread_, nullptr) == 0);
	}

	/* The guest writes again within a few seconds. */
	[[nodiscard]] bool runsAgain() const
	{
		const auto before = writes_.load();
		return uml::testing::waitUntil(
			[&] { return writes_.load() > before + 2U; },
			std::chrono::seconds {5}, std::chrono::milliseconds {2});
	}

	/* The guest wrote nothing for a while, which no thread that runs can
	 * promise: it writes every few hundred microseconds. */
	[[nodiscard]] bool stoodStill() const
	{
		const auto before = writes_.load();
		std::this_thread::sleep_for(std::chrono::milliseconds {100});
		return writes_.load() == before;
	}

private:
	static void *writeUntilStopped(void *argument)
	{
		auto *const guest = static_cast<WakingGuest *>(argument);
		while (!guest->stop_.load(std::memory_order_acquire)) {
			if (::pwrite(guest->disk_, "w", 1, 0) == 1)
				guest->writes_.fetch_add(1, std::memory_order_relaxed);
			uml::testing::nap(300);
		}
		return nullptr;
	}

	int disk_;
	pthread_t thread_ {};
	std::atomic<std::uint64_t> writes_ {0};
	std::atomic<bool> stop_ {false};
};

/* A confirmed system suspension ends at the guest's own wake sources (its RTC
 * alarm, VNC input) as well as at the host's wake, so the guest can run, and
 * write its disks, while the host holds no lock on them. Every thread that
 * can run it is held from before the locks leave until the host has them back
 * and verified the disks, whether or not the guest's RAM is hibernated. */
void aGuestThatWakesItselfCannotWriteDisksItDoesNotLock()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	WakingGuest guest {disk.descriptor()};
	require(disk.lockedByAnotherProcess(), "a running guest owns its disk");
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(!disk.lockedByAnotherProcess(),
		"a suspended guest leaves its disk unlocked for the system");
	require(guest.stoodStill(),
		"the guest wrote its disk while the host held no lock on it");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess(), "the resume locks the disk again");
	require(guest.runsAgain(), "the guest runs once it is woken");
}

/* A thread the fence cannot hold, here one inside the C library for as long
 * as the test says, leaves the guest able to run. The locks then stay, as an
 * unconfirmed suspension keeps them, and the resume has nothing to verify. */
std::atomic<bool> serviceBlocked {true};

int blockInComparator(const void *, const void *)
{
	while (serviceBlocked.load(std::memory_order_acquire))
		uml::testing::nap(500);
	return 0;
}

void aGuestThatCannotBeHeldKeepsItsLocks()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	WakingGuest guest {disk.descriptor()};
	serviceBlocked.store(true);
	std::atomic<bool> started {false};
	std::thread service {[&] {
		(void)::pthread_setname_np("uml-test-service");
		int values[2] = {2, 1};
		started.store(true);
		(void)::heapsort(values, 2, sizeof values[0], &blockInComparator);
	}};
	require(uml::testing::waitUntil([&] { return started.load(); },
		std::chrono::seconds {5}));
	uml::testing::nap(50000);
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(disk.lockedByAnotherProcess(),
		"locks may not leave while the guest's threads can run");
	serviceBlocked.store(false);
	service.join();
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess());
	require(guest.runsAgain(), "the guest runs once it is woken");
}

/* A thread of the kernel that asks the host bridge for its disk's lock without
 * pause, as os_lock_file does on every open. Each request spends most of its
 * time inside the guest file registry's critical section, so a thread stopped
 * at a random moment is very likely to stop inside it, owning the registry's
 * mutex. */
class LockingGuest final {
public:
	explicit LockingGuest(int disk) : disk_{disk}
	{
		require(::uml::runtime::kernel_pthread_create(&thread_, nullptr,
			&lockUntilStopped, this) == 0, "create the guest's thread");
		require(runsAgain(), "the guest's thread must run");
	}

	LockingGuest(const LockingGuest &) = delete;
	LockingGuest &operator=(const LockingGuest &) = delete;

	~LockingGuest()
	{
		stop_.store(true);
		require(::pthread_join(thread_, nullptr) == 0);
	}

	/* The guest gets its disk's lock again: a request is granted only while
	 * the host has given the locks back. */
	[[nodiscard]] bool runsAgain() const
	{
		const auto before = granted_.load();
		return uml::testing::waitUntil(
			[&] { return granted_.load() > before + 2U; },
			std::chrono::seconds {5}, std::chrono::milliseconds {2});
	}

private:
	static void *lockUntilStopped(void *argument)
	{
		auto *const guest = static_cast<LockingGuest *>(argument);
		while (!guest->stop_.load(std::memory_order_acquire)) {
			struct flock lock {};
			lock.l_type = F_WRLCK;
			lock.l_whence = SEEK_SET;
			if (darwin_uml_fcntl(guest->disk_, F_SETLK,
				    reinterpret_cast<long>(&lock)) == 0)
				guest->granted_.fetch_add(1, std::memory_order_relaxed);
		}
		return nullptr;
	}

	int disk_;
	pthread_t thread_ {};
	std::atomic<std::uint64_t> granted_ {0};
	std::atomic<bool> stop_ {false};
};

/* The hold stops the guest's threads wherever they are, and the host then
 * takes the registry's mutex to release the disk locks and, at the resume, to
 * restore them. A thread stopped inside the registry would own that mutex for
 * as long as it is stopped, and the release would wait for it forever with
 * the process asleep. The hold is therefore accepted only at a moment when no
 * stopped thread owns the registry. Every suspension below stops a guest that
 * is inside the registry nearly all the time; the watchdog turns a wedged one
 * into a failure. */
void aGuestInsideTheRegistryDoesNotWedgeTheSuspension()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	LockingGuest guest {disk.descriptor()};
	const uml::testing::Watchdog wedged {
		"a suspension with the guest inside the registry",
		std::chrono::seconds {60}};
	for (int round = 0; round < 25; ++round) {
		require(console.requests(3U, [&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		}) == Requests {"sync", "trim once", "suspend"});
		require(!disk.lockedByAnotherProcess(),
			"a suspended guest leaves its disk unlocked for the system");
		require(console.requests(1U, [&] {
			uml_emulator_resume(emulator.get());
		}) == Requests {"wake"});
		require(disk.lockedByAnotherProcess(),
			"the resume locks the disk again");
		require(guest.runsAgain(), "the guest runs once it is woken");
	}
}

/* With the RAM hibernated the one hold covers both: the locks leave once the
 * threads are held and before the write, which can take long enough for the
 * system to suspend the process. */
void theLocksLeaveBetweenTheHoldAndTheWrite()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	Steps *const steps = installSteps(emulator.get());
	bool lockedAtHold {false};
	bool lockedAtWrite {true};
	steps->atHold = [&] { lockedAtHold = disk.lockedByAnotherProcess(); };
	steps->atWrite = [&] { lockedAtWrite = disk.lockedByAnotherProcess(); };
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"});
	require(lockedAtHold, "the locks stay until the threads are held");
	require(!lockedAtWrite, "the locks are gone before the image is written");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess(), "the resume locks the disk again");
	(void)hibernationLog.take();
}

/* A hibernation that cannot hold the threads changes nothing, the locks
 * included. */
void aHibernationThatCannotHoldKeepsTheLocks()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	Steps *const steps = installSteps(emulator.get());
	steps->holdSucceeds = false;
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	require(hibernationLog.take() == Entries {"prepare", "hold"},
		"nothing is written where nothing is held");
	require(disk.lockedByAnotherProcess(),
		"the locks stay where the threads are not held");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess());
	(void)hibernationLog.take();
}

/* A suspension whose threads the host could not hold is no guarantee that the
 * guest stays put: it can wake itself and write. What a quiesce said of it is
 * then not repeated by a later call, which asks the guest again, and a quiesce
 * of the guest the host now holds is the one that is reported again. */
void aGuestTheHostDoesNotHoldIsAskedAgain()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	Steps *const steps = installSteps(emulator.get());
	steps->holdSucceeds = false;
	(void)hibernationLog.take();
	const auto first = settleAgainst(console, 3U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(first.heard == Requests {"sync", "trim once", "suspend"});
	requireReport(first, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED, "the first quiesce");
	/* The guest woke by itself and wrote. A sync is asked, not remembered. */
	const auto sync = settleAgainst(console, 1U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_sync(emulator.get(), &settle);
		});
	require(sync.heard == Requests {"sync"},
		"a guest the host does not hold is asked again");
	requireReport(sync, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_NOT_ASKED, "the sync's own answer");
	/* Another quiesce runs its whole sequence, and this time holds the guest. */
	steps->holdSucceeds = true;
	const auto second = settleAgainst(console, 3U,
		[&](uml_emulator_settle &settle) {
			return uml_emulator_quiesce(emulator.get(), &settle);
		});
	require(second.heard == Requests {"sync", "trim once", "suspend"},
		"a quiesce of a guest that is not held is a whole quiesce");
	requireReport(second, UML_EMULATOR_GUEST_STORAGE_CONFIRMED,
		UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED, "the second quiesce");
	/* Now it is held, and nothing it did is unseen. */
	require(console.requests(0U, [&] {
		uml_emulator_settle settle = blankSettle();
		require(uml_emulator_sync(emulator.get(), &settle) == 0);
		require(settle.guest_storage == UML_EMULATOR_GUEST_STORAGE_CONFIRMED &&
			settle.guest_suspension ==
			UML_EMULATOR_GUEST_SUSPENSION_SUSPENDED);
	}).empty(), "a held guest is reported again, not asked");
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	(void)hibernationLog.take();
}

/* ---- The guest's memory figures. ---- */

uml_emulator_guest_memory askedMemory()
{
	return {.struct_size = sizeof(uml_emulator_guest_memory)};
}

/* One datagram out, and the guest's own total and available memory back. */
void theGuestsMemoryIsRead(uml_emulator *emulator, FakeGuestConsole &console)
{
	auto memory = askedMemory();
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			UML_EMULATOR_OK);
	}) == Requests {"mem"});
	require(memory.struct_size == sizeof(memory) &&
		memory.total_bytes == 2147483648ULL &&
		memory.available_bytes == 1073741824ULL,
		"the guest's figures were not returned");
}

/* A kernel without the command and a reply that is not the figures are not
 * a reading, and a silent guest is given up on at the bound. */
void anAnswerThatIsNotTheFiguresIsNoReading(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	auto memory = askedMemory();
	memory.total_bytes = 7U;
	console.memory = FakeGuestConsole::Memory::unknown;
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			-UML_EMULATOR_UNSUPPORTED);
	}) == Requests {"mem"}, "a kernel without the command is unsupported");
	console.memory = FakeGuestConsole::Memory::malformed;
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			-UML_EMULATOR_RUNTIME_ERROR);
	}) == Requests {"mem"}, "a reply that is not the figures is an error");
	require(memory.total_bytes == 7U, "a refused reading changed the output");

	console.memory = FakeGuestConsole::Memory::silent;
	const auto started = std::chrono::steady_clock::now();
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			-UML_EMULATOR_RUNTIME_ERROR);
	}) == Requests {"mem"}, "a silent guest is asked once");
	require(std::chrono::steady_clock::now() - started >=
		std::chrono::milliseconds {450}, "the reply was not waited for");
	console.memory = FakeGuestConsole::Memory::figures;
}

/* A request would end a suspend, so a quiesced guest is not asked, and is
 * asked again once it has been resumed. */
void aQuiescedGuestIsNotAskedForItsMemory()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	auto memory = askedMemory();
	memory.total_bytes = 7U;
	require(console.requests(0U, [&] {
		require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
			-UML_EMULATOR_UNSUPPORTED);
	}).empty(), "a quiesced guest was woken to be asked");
	require(memory.total_bytes == 7U, "a refused reading changed the output");
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	theGuestsMemoryIsRead(emulator.get(), console);
	(void)hibernationLog.take();
}

/* A lifecycle call in progress is neither waited for nor waited on: the
 * request is refused at once and sends nothing. This holds through the whole
 * quiesce, so also while its suspend is starting: the guest has been asked
 * and has not yet replied that it sleeps. `held` is the request whose answer
 * the guest keeps back. */
void aLifecycleCallInProgressIsNotWaitedFor(const std::string &held)
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	std::mutex mutex;
	std::condition_variable changed;
	bool trimming {false};
	bool released {false};
	/* The guest holds its answer to `held`, and the quiesce its lifecycle
	 * mutex, until the test lets go or the guard does. */
	console.onRequest = [&](const std::string &command) {
		if (command != held)
			return;
		std::unique_lock lock {mutex};
		trimming = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {5},
			[&] { return released; });
	};
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(3U, [&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		});
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return trimming; }), "the quiesce never sent the request");
	}
	auto memory = askedMemory();
	const auto started = std::chrono::steady_clock::now();
	const int result = uml_emulator_get_guest_memory(emulator.get(), &memory);
	const auto waited = std::chrono::steady_clock::now() - started;
	/* The pressure response's trim is not waited for either, and sends
	 * nothing. */
	const auto trimStarted = std::chrono::steady_clock::now();
	const auto pressureTrim =
		uml::emulator::testing::trim_for_memory_pressure(emulator.get());
	const auto trimWaited = std::chrono::steady_clock::now() - trimStarted;
	{
		const std::lock_guard lock {mutex};
		released = true;
	}
	changed.notify_all();
	quiescing.join();
	require(result == -UML_EMULATOR_UNSUPPORTED,
		"a request ran beside a quiesce");
	require(waited < std::chrono::seconds {2},
		"a request waited for a quiesce");
	require(pressureTrim == ::uml::runtime::TrimOutcome::busy &&
		trimWaited < std::chrono::seconds {2},
		"the pressure trim ran beside a quiesce");
	require(heard == Requests {"sync", "trim once", "suspend"},
		"a request reached the guest beside a quiesce");
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	(void)hibernationLog.take();
}

/* The response's end of the trimmer that meets a lifecycle call longer than
 * its retries (a long quiesce) is busy each time, and must not be forgotten:
 * the resume settles what the response last wanted. */
void aPressureEndMetByALongLifecycleCallIsReplayedAtTheResume()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(),
			TrimKind::gradual_on) == TrimOutcome::sent);
	}) == Requests {"trim on"});
	std::mutex mutex;
	std::condition_variable changed;
	bool trimming {false};
	bool released {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "trim once")
			return;
		std::unique_lock lock {mutex};
		trimming = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {5},
			[&] { return released; });
	};
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(3U, [&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		});
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return trimming; }), "the quiesce never sent its trim");
	}
	/* The normal level, asked more often than the response retries. */
	for (int attempt = 0; attempt < 6; ++attempt)
		require(trim_for_memory_pressure(emulator.get(),
			TrimKind::gradual_off) == TrimOutcome::busy);
	{
		const std::lock_guard lock {mutex};
		released = true;
	}
	changed.notify_all();
	quiescing.join();
	require(heard == Requests {"sync", "trim once", "suspend"});
	console.onRequest = nullptr;
	require(console.requests(2U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake", "trim off"},
		"the end of the trimmer was lost behind the lifecycle call");
	/* Settled: nothing is owed, and asking again sends nothing. */
	require(console.requests(0U, [&] {
		require(trim_for_memory_pressure(emulator.get(),
			TrimKind::gradual_off) == TrimOutcome::done);
	}).empty());
	(void)hibernationLog.take();
}

/* The same end of the trimmer, met by a quiesce that leaves the guest running
 * (its suspend is refused, as a guest that cannot suspend does): no resume
 * follows, so the quiesce settles what the response last wanted when it ends,
 * and nothing is owed afterwards. */
void aPressureEndMetByAQuiesceThatLeavesTheGuestRunningIsSentWhenItEnds()
{
	using ::uml::runtime::TrimKind;
	using ::uml::runtime::TrimOutcome;
	using uml::emulator::testing::trim_for_memory_pressure;
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.refuseSuspend = true;
	require(console.requests(1U, [&] {
		require(trim_for_memory_pressure(emulator.get(),
			TrimKind::gradual_on) == TrimOutcome::sent);
	}) == Requests {"trim on"});
	std::mutex mutex;
	std::condition_variable changed;
	bool suspending {false};
	bool released {false};
	/* The guest holds its refusal of the suspend, as a freezer that is slow to
	 * give up does, and the quiesce its lifecycle mutex. */
	console.onRequest = [&](const std::string &command) {
		if (command != "suspend")
			return;
		std::unique_lock lock {mutex};
		suspending = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {5},
			[&] { return released; });
	};
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(5U, [&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		});
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return suspending; }), "the quiesce never sent its suspend");
	}
	/* The end of the trimmer, asked more often than the response retries. */
	for (int attempt = 0; attempt < 6; ++attempt)
		require(trim_for_memory_pressure(emulator.get(),
			TrimKind::gradual_off) == TrimOutcome::busy);
	{
		const std::lock_guard lock {mutex};
		released = true;
	}
	changed.notify_all();
	quiescing.join();
	require(heard == Requests {"sync", "trim once", "suspend", "sync",
		"trim off"}, "the end of the trimmer was lost behind the quiesce");
	console.onRequest = nullptr;
	/* Settled: nothing is owed, and asking again sends nothing. */
	require(console.requests(0U, [&] {
		require(trim_for_memory_pressure(emulator.get(),
			TrimKind::gradual_off) == TrimOutcome::done);
	}).empty());
}

/* A foreground transition's "trim off" holds the lifecycle mutex while it
 * waits for the guest. A pressure trim that meets it sends nothing and says it
 * is busy, which is what the response tries again; once the call has ended the
 * same trim is sent. */
void aPressureTrimBesideATrimChangeIsBusyThenSent()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	std::mutex mutex;
	std::condition_variable changed;
	bool changing {false};
	bool released {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "trim off")
			return;
		std::unique_lock lock {mutex};
		changing = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {5},
			[&] { return released; });
	};
	Requests heard;
	std::thread foreground {[&] {
		heard = console.requests(1U, [&] {
			require(uml_emulator_set_page_cache_trimming(
				emulator.get(), 0) == UML_EMULATOR_OK);
		});
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return changing; }), "the change never reached the guest");
	}
	const auto started = std::chrono::steady_clock::now();
	const auto outcome =
		uml::emulator::testing::trim_for_memory_pressure(emulator.get());
	const auto waited = std::chrono::steady_clock::now() - started;
	/* The gradual trimmer's request is no different. */
	const auto gradualOutcome = uml::emulator::testing::trim_for_memory_pressure(
		emulator.get(), ::uml::runtime::TrimKind::gradual_on);
	{
		const std::lock_guard lock {mutex};
		released = true;
	}
	changed.notify_all();
	foreground.join();
	require(outcome == ::uml::runtime::TrimOutcome::busy &&
		waited < std::chrono::seconds {2},
		"the pressure trim ran beside a trim change");
	require(gradualOutcome == ::uml::runtime::TrimOutcome::busy,
		"the gradual trimmer's request ran beside a trim change");
	require(heard == Requests {"trim off"},
		"the pressure trim reached the guest beside a trim change");
	console.onRequest = nullptr;
	require(console.requests(1U, [&] {
		require(uml::emulator::testing::trim_for_memory_pressure(
			emulator.get()) == ::uml::runtime::TrimOutcome::done);
	}) == Requests {"trim once"});
}

/* A clock the test sets by hand, for the lifecycle calls' waits, and counts
 * the reads of. */
std::atomic<std::int64_t> manualMilliseconds {0};
std::atomic<int> manualReads {0};

/* The time is captured before the read is published: the test moves the clock
 * once it sees a read, and that read must already have its time. */
std::chrono::steady_clock::time_point manualNow() noexcept
{
	const std::chrono::steady_clock::time_point now {
		std::chrono::milliseconds {manualMilliseconds.load()}};
	++manualReads;
	return now;
}

class ManualClock final {
public:
	ManualClock()
	{
		manualMilliseconds = 1'000'000;
		manualReads = 0;
		uml::emulator::testing::set_lifecycle_clock(&manualNow);
	}

	~ManualClock()
	{
		uml::emulator::testing::set_lifecycle_clock(nullptr);
	}

	ManualClock(const ManualClock &) = delete;
	ManualClock &operator=(const ManualClock &) = delete;
};

/* A pressure trim holds the lifecycle mutex while it waits for the guest, and
 * a quiesce called meanwhile queues behind it. `passes` is how long the
 * quiesce waited, by the clock the test sets: the pressure trim's reply is
 * held until the quiesce has noted its call, and then the clock moves. The
 * quiesce's requests are returned, with the time from its call to its end. */
struct QuiesceBesideTrim final {
	Requests heard;
	std::chrono::milliseconds took {};
	::uml::runtime::TrimOutcome pressure {};
};

QuiesceBesideTrim aQuiesceCalledDuringAPressureTrim(
	std::int64_t passes, int trimsAnswered, std::size_t requests)
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	const ManualClock clock;
	console.trimsAnsweredBeforeSilence = trimsAnswered;
	std::mutex mutex;
	std::condition_variable changed;
	bool trimming {false};
	bool released {false};
	/* Only the first "trim once", the pressure trim's, is held. */
	console.onRequest = [&](const std::string &command) {
		if (command != "trim once")
			return;
		std::unique_lock lock {mutex};
		if (trimming)
			return;
		trimming = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return released; });
	};
	QuiesceBesideTrim result;
	result.heard = console.requests(requests, [&] {
		std::thread pressure {[&] {
			result.pressure =
				uml::emulator::testing::trim_for_memory_pressure(
					emulator.get());
		}};
		{
			std::unique_lock lock {mutex};
			require(changed.wait_for(lock, std::chrono::seconds {10},
				[&] { return trimming; }),
				"the pressure trim never reached the guest");
		}
		const auto called = std::chrono::steady_clock::now();
		std::thread quiescing {[&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
			result.took =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - called);
		}};
		require(waitUntil([] { return manualReads.load() >= 1; },
			std::chrono::seconds {10}), "the quiesce never noted its call");
		manualMilliseconds += passes;
		{
			const std::lock_guard lock {mutex};
			released = true;
		}
		changed.notify_all();
		pressure.join();
		quiescing.join();
	});
	return result;
}

/* A pressure trim that held the lifecycle mutex for the whole of the trim's
 * wait has asked the guest for the same pass, so the quiesce behind it sends
 * none and its edge does not wait for another. Without the deduction it sent
 * a second "trim once" and waited its full wait again. */
void aQuiesceBesideAPressureTrimThatSpentTheWaitSendsNoSecondTrim()
{
	const auto result =
		aQuiesceCalledDuringAPressureTrim(3000, -1, 3U);
	require(result.pressure == ::uml::runtime::TrimOutcome::done);
	require(result.heard == Requests {"trim once", "sync", "suspend"},
		"a quiesce sent a second trim after the pressure trim's wait");
}

/* What the wait spent beside the pressure trim is taken from the quiesce's
 * own: it sends its trim, for what is left, and ends that long after its
 * call, not the full wait later. */
void aQuiesceBesideAPressureTrimWaitsForWhatIsLeft()
{
	const auto result =
		aQuiesceCalledDuringAPressureTrim(1000, 1, 4U);
	require(result.heard ==
		Requests {"trim once", "sync", "trim once", "suspend"},
		"the quiesce did not send its trim");
	require(result.took >= std::chrono::milliseconds {1900},
		"the quiesce did not wait for the trim");
	require(result.took < std::chrono::milliseconds {2900},
		"the quiesce waited its full wait after the pressure trim");
}

/* The mutex has no queue, so a pressure trim that tries it as a waiting
 * quiesce is woken could take it first, and every later response could do
 * the same: a warning then a critical level, each unanswered, held the
 * quiesce for 6 s. A lifecycle call is counted before it waits, and a
 * pressure trim that finds one gives way. Here the first pressure trim is
 * in the guest's hands and the quiesce waits; when it ends, pressure trims
 * come as fast as the thread can make them, and none may reach the guest
 * before the quiesce is done. */
void aPressureResponseCannotOvertakeAWaitingQuiesce()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	const ManualClock clock;
	std::mutex mutex;
	std::condition_variable changed;
	bool trimming {false};
	bool released {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "trim once")
			return;
		std::unique_lock lock {mutex};
		if (trimming)
			return;
		trimming = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return released; });
	};
	std::atomic<bool> quiesced {false};
	std::size_t reachedTheGuest {0};
	std::chrono::milliseconds took {};
	const auto heard = console.requests(3U, [&] {
		std::thread first {[&] {
			(void)uml::emulator::testing::trim_for_memory_pressure(
				emulator.get());
		}};
		{
			std::unique_lock lock {mutex};
			require(changed.wait_for(lock, std::chrono::seconds {10},
				[&] { return trimming; }),
				"the pressure trim never reached the guest");
		}
		const auto called = std::chrono::steady_clock::now();
		std::thread quiescing {[&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
			took = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - called);
			quiesced = true;
		}};
		require(waitUntil([&] {
			return uml::emulator::testing::lifecycle_calls(emulator.get()) >= 1U
				&& manualReads.load() >= 1;
		}, std::chrono::seconds {10}), "the quiesce never began to wait");
		manualMilliseconds += 3000;
		{
			const std::lock_guard lock {mutex};
			released = true;
		}
		changed.notify_all();
		/* Later responses, one after another, from this thread. */
		const auto until = std::chrono::steady_clock::now() +
			std::chrono::seconds {10};
		while (!quiesced && std::chrono::steady_clock::now() < until) {
			const auto outcome =
				uml::emulator::testing::trim_for_memory_pressure(
					emulator.get());
			if (outcome != ::uml::runtime::TrimOutcome::busy &&
			    outcome != ::uml::runtime::TrimOutcome::quiesced)
				++reachedTheGuest;
		}
		first.join();
		quiescing.join();
	});
	require(reachedTheGuest == 0,
		"a pressure trim took the mutex ahead of a waiting quiesce");
	require(heard == Requests {"trim once", "sync", "suspend"},
		"a pressure trim reached the guest ahead of a waiting quiesce");
	require(took < std::chrono::seconds {3},
		"the quiesce waited behind later pressure trims");
}

/* Time spent behind another lifecycle call is not a pressure trim: the guest
 * was not asked, so the quiesce still asks it, as the API promises, with its
 * reply wait spent: the request goes out and its reply is not waited for. The
 * resume here waits for the guest's replies to its wake and its trimming
 * change, and a quiesce called meanwhile queues behind it. */
void aQuiesceBesideAResumeStillSendsItsTrim()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	const ManualClock clock;
	/* The first quiesce is answered; the one beside the resume is not, as the
	 * reply to a request that is not waited for is not heard. */
	console.trimsAnsweredBeforeSilence = 1;
	require(console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
	}) == Requests {"sync", "trim once", "suspend"});
	/* A trimming change asked of a quiesced guest waits for the resume. */
	require(uml_emulator_set_page_cache_trimming(emulator.get(), 0) ==
		UML_EMULATOR_OK);
	std::mutex mutex;
	std::condition_variable changed;
	bool waking {false};
	bool released {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "wake")
			return;
		std::unique_lock lock {mutex};
		waking = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return released; });
	};
	manualReads = 0;
	const auto heard = console.requests(5U, [&] {
		std::thread resuming {[&] { uml_emulator_resume(emulator.get()); }};
		{
			std::unique_lock lock {mutex};
			require(changed.wait_for(lock, std::chrono::seconds {10},
				[&] { return waking; }), "the resume never woke the guest");
		}
		std::thread quiescing {[&] {
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		}};
		require(waitUntil([&] {
			return uml::emulator::testing::lifecycle_calls(emulator.get()) >= 2U
				&& manualReads.load() >= 1;
		}, std::chrono::seconds {10}), "the quiesce never began to wait");
		/* The resume's replies took the whole of the trim's wait. */
		manualMilliseconds += 3200;
		{
			const std::lock_guard lock {mutex};
			released = true;
		}
		changed.notify_all();
		resuming.join();
		quiescing.join();
	});
	require(heard == Requests {"wake", "trim off", "sync", "trim once",
		"suspend"},
		"the quiesce left the guest's page cache untrimmed");
}

/* With nothing ahead of it the quiesce's trim has the whole wait. */
void aQuiesceWithNothingAheadWaitsTheWholeTrimWait()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.trimsAnsweredBeforeSilence = 0;
	std::chrono::milliseconds took {};
	require(console.requests(3U, [&] {
		const auto called = std::chrono::steady_clock::now();
		require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		took = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - called);
	}) == Requests {"sync", "trim once", "suspend"});
	require(took >= std::chrono::milliseconds {2900},
		"the quiesce did not wait the trim's whole wait");
	require(took < std::chrono::seconds {8});
}

/* The wait for the reply is outside the lifecycle mutex, so a quiesce that
 * starts after the request was sent is heard while it waits. A request that
 * held the mutex would time out before the quiesce was heard. */
void aQuiesceDoesNotWaitForAMemoryRequest()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	console.memory = FakeGuestConsole::Memory::deferred;
	std::mutex mutex;
	std::condition_variable changed;
	bool asked {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "mem")
			return;
		const std::lock_guard lock {mutex};
		asked = true;
		changed.notify_all();
	};
	auto memory = askedMemory();
	const auto heard = console.requests(4U, [&] {
		std::thread quiescing {[&] {
			std::unique_lock lock {mutex};
			require(changed.wait_for(lock, std::chrono::seconds {10},
				[&] { return asked; }), "the request never arrived");
			lock.unlock();
			require(uml_emulator_quiesce(emulator.get(), nullptr) == 0);
		}};
		const int result =
			uml_emulator_get_guest_memory(emulator.get(), &memory);
		quiescing.join();
		require(result == UML_EMULATOR_OK,
			"the request was not answered while the quiesce was heard");
	});
	require(heard == Requests {"mem", "sync", "trim once", "suspend"});
	require(memory.total_bytes == 2147483648ULL &&
		memory.available_bytes == 1073741824ULL);
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	(void)hibernationLog.take();
}

} // namespace

int main(int argc, char **argv)
{
	const std::string_view selected = argc > 1 ? argv[1] : "";
	const uml::testing::PrivateTemporaryDirectory temporary {
		"uml-lifecycle-test.XXXXXX"};
	const ::uml::device::GuestConsoleRun run {true};
	if (selected == "suspend-timeout") {
		aSuspendThatTimedOutIsNotHibernated();
		return EXIT_SUCCESS;
	}
	if (selected == "suspend-silent") {
		aSuspendThatNeverAnsweredIsWokenAtTheResume();
		return EXIT_SUCCESS;
	}
	if (selected == "unheld-guest") {
		aGuestThatCannotBeHeldKeepsItsLocks();
		return EXIT_SUCCESS;
	}
	if (selected == "registry-owner") {
		aGuestInsideTheRegistryDoesNotWedgeTheSuspension();
		return EXIT_SUCCESS;
	}
	/* The quiesce's trim cases, each alone, for a change to the code they
	 * guard to show which of them it breaks. */
	/* The settle calls whose waits run out, each its own second or more. */
	if (selected == "settle-timeouts") {
		anOlderKernelIsOnlyAskedAndStaysUnconfirmed();
		aSyncWithoutAReplyIsUnconfirmedAfterItsBound();
		anOlderKernelIsNeverConfirmedBySuspend();
		aSuspendThatStatesItSyncedConfirmsAfterASilentSync();
		aQuiesceWhoseSyncGetsNoReplySkipsTheTrim();
		aFinalWritebackWithoutAReplyIsUnconfirmed();
		aSyncBesideALongLifecycleCallIsUnconfirmed();
		return EXIT_SUCCESS;
	}
	if (selected == "quiesce-spent-wait") {
		aQuiesceBesideAPressureTrimThatSpentTheWaitSendsNoSecondTrim();
		return EXIT_SUCCESS;
	}
	if (selected == "quiesce-remaining-wait") {
		aQuiesceBesideAPressureTrimWaitsForWhatIsLeft();
		return EXIT_SUCCESS;
	}
	if (selected == "quiesce-whole-wait") {
		aQuiesceWithNothingAheadWaitsTheWholeTrimWait();
		return EXIT_SUCCESS;
	}
	if (selected == "pressure-overtake") {
		aPressureResponseCannotOvertakeAWaitingQuiesce();
		return EXIT_SUCCESS;
	}
	if (selected == "quiesce-beside-resume") {
		aQuiesceBesideAResumeStillSendsItsTrim();
		return EXIT_SUCCESS;
	}
	if (!selected.empty())
		return EXIT_FAILURE;
	const auto emulator = makeEmulator();
	withoutAConsoleNothingWaits(emulator.get());
	FakeGuestConsole console;
	quiesceTrimsBeforeItSuspends(emulator.get(), console);
	aQuiescedGuestHearsOnlyTheResume(emulator.get(), console);
	aRunningGuestIsAskedAtOnce(emulator.get(), console);
	theMemoryPressureTrimAsksARunningGuest(emulator.get(), console);
	theGradualPressureTrimmerIsOneMessageAndKeepsWhatTheApplicationAsked(
		emulator.get(), console);
	aQuiescedGuestIsNotWokenForThePressureTrim(emulator.get(), console);
	aQuiescedGuestHearsTheGradualPressureTrimmerOnlyAtTheResume(
		emulator.get(), console);
	theGuestsMemoryIsRead(emulator.get(), console);
	anAnswerThatIsNotTheFiguresIsNoReading(emulator.get(), console);
	aHibernatedGuestIsWholeBeforeItIsWoken();
	aSecondQuiesceKeepsTheHibernation();
	aResidentQuiesceWritesNoRam();
	aQuiesceKeepsWhateverHoldsTheGuest();
	aGuestThatCouldNotSuspendIsNotHibernated();
	aGuestWithoutAConsoleIsNotHibernated();
	aFailedRestoreWakesNothing();
	aResumeDuringTheWriteStopsTheWrite();
	aStopBringsTheRamBack();
	aGuestThatWakesItselfCannotWriteDisksItDoesNotLock();
	theLocksLeaveBetweenTheHoldAndTheWrite();
	aHibernationThatCannotHoldKeepsTheLocks();
	aQuiescedGuestIsNotAskedForItsMemory();
	aGradualRequestThatWasNotSentIsOwedUntilAnyCallSettlesIt();
	aResumeThatCouldNotReplayTheTrimmerLeavesItOwed();
	aSilentConsoleHoldsNeitherThePressureTrimmerNorTheApplicationsRecord();
	theTrimmerEndsAsEitherOfThemLastWanted();
	aRestartedGuestStartsWithTheTrimmerOffAndUnwanted();
	aLifecycleCallInProgressIsNotWaitedFor("trim once");
	aLifecycleCallInProgressIsNotWaitedFor("suspend");
	aPressureEndMetByALongLifecycleCallIsReplayedAtTheResume();
	aPressureEndMetByAQuiesceThatLeavesTheGuestRunningIsSentWhenItEnds();
	aPressureTrimBesideATrimChangeIsBusyThenSent();
	aQuiesceDoesNotWaitForAMemoryRequest();
	aQuiesceBesideAPressureTrimThatSpentTheWaitSendsNoSecondTrim();
	aQuiesceBesideAPressureTrimWaitsForWhatIsLeft();
	aQuiesceWithNothingAheadWaitsTheWholeTrimWait();
	aPressureResponseCannotOvertakeAWaitingQuiesce();
	aQuiesceBesideAResumeStillSendsItsTrim();
	aSyncIsConfirmedByTheGuestsReply();
	aRefusedSyncIsNotSettled();
	withoutAGuestThereIsNothingToSettle();
	theReportIsChecked(emulator.get());
	aSuspendedGuestIsConfirmed();
	aRefusedSuspendIsWrittenBackAndNotSuspended();
	aRefusingGuestIsNotSettledByAQuiesce();
	aSuspendDoesNotSettleARefusedWriteback();
	aSuspendThatStatesItSyncedConfirmsTheStorage();
	aSuspendThatStatesItDidNotSyncConfirmsNothingMore();
	aGuestTheHostDoesNotHoldIsAskedAgain();
	/* Last: a run removes the console's directory when it returns. */
	aRunningGuestWithoutAConsoleIsNotSettled();
	return EXIT_SUCCESS;
}
