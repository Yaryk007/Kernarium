// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"
#include "guest_console_test_support.hpp"
#include "require.hpp"
#include "socket_paths.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
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

const ::uml::device::GuestConsoleLocation &processLocation()
{
	const auto &location = ::uml::device::guest_console_location();
	require(location.has_value());
	return *location;
}

std::string consolePath()
{
	return processLocation().console;
}

/* The host narrows the guest's console to its owner once it sends to it. */
void requireOwnerOnly(const std::string &path)
{
	struct stat details {};
	require(::lstat(path.c_str(), &details) == 0 &&
		(details.st_mode & 0777) == (S_IRUSR | S_IWUSR));
}

/* What the guest said, and when it arrived: the time since the first
 * request. */
struct Heard final {
	std::string command;
	std::chrono::milliseconds at;
};

using Reply = std::pair<std::uint32_t, std::string>;

/* What a suspend-capable guest replies when it gave a suspend up, busy or
 * for another reason, and to a writeback request (the "sync" command, or SysRq
 * 's' from a kernel that has no such command). */
const Reply busySuspend {1U, "Suspend failed: -16"};
const Reply failedSuspend {1U, "Suspend failed: -5"};
const Reply enteredSuspend {0U, "suspended"};
const Reply enteredSyncedSuspend {0U, "suspended, synced"};
const Reply enteredUnsyncedSuspend {0U, "suspended, not synced"};
const Reply sysrqWriteback {0U, "sysrq s ok"};
const Reply synced {0U, "synced in 12 ms"};
const Reply unknownCommand {1U, "Unknown command"};

/* Answers each request with what `policy` gives for it and its index, the
 * first one only after `firstDelay`, and keeps listening for a while after the
 * last, so that a request too many shows in the list. A reply whose request
 * gave up finds no socket. */
void answerAll(int descriptor, std::chrono::milliseconds firstDelay,
	std::function<Reply(const std::string &, std::size_t)> policy,
	std::vector<Heard> &heard)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	std::chrono::steady_clock::time_point first;
	for (int wait = 10000; awaitRequest(descriptor, wait); wait = 500) {
		const auto command = receiveRequest(descriptor, client, clientLength);
		const auto now = std::chrono::steady_clock::now();
		if (heard.empty())
			first = now;
		heard.push_back({command,
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now - first)});
		if (heard.size() == 1U)
			std::this_thread::sleep_for(firstDelay);
		const auto &[error, text] = policy(command, heard.size() - 1U);
		(void)sendReply(descriptor, client, clientLength, error, text);
	}
}

/* Runs suspend_guest against a console that answers by `policy` and returns
 * what the console heard. */
std::vector<Heard> suspendAgainst(
	std::function<Reply(const std::string &, std::size_t)> policy,
	std::chrono::milliseconds firstDelay,
	::uml::device::GuestSuspension &suspension,
	::uml::device::GuestSuspendSync *sync = nullptr)
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::vector<Heard> heard;
	std::thread observer {answerAll, server, firstDelay, std::move(policy),
		std::ref(heard)};
	suspension = ::uml::device::suspend_guest(sync);
	observer.join();
	requireOwnerOnly(consolePath());
	(void)::close(server);
	return heard;
}

std::vector<std::string> commandsOf(const std::vector<Heard> &heard)
{
	std::vector<std::string> commands;
	for (const auto &entry : heard)
		commands.push_back(entry.command);
	return commands;
}

/* A console whose suspend does not reply in time. It records what it hears,
 * with the time since the first request, remembers where each suspend came
 * from, and answers a wake with "running" and a writeback request as a guest
 * does. What it says to a suspend's address, and when, is the scenario's. */
struct HeldSuspend final {
	/* Sent to the first suspend's address once this long after the request
	 * arrived; nothing when it is empty. */
	std::optional<Reply> firstReply;
	std::chrono::milliseconds firstReplyAfter {0};
	/* Whether that reply is the first part of a reply whose end follows. */
	std::uint32_t firstReplyMore {0U};
	/* Sent to the latest suspend's address right after the first wake is
	 * answered: the final word of a suspend the wake aborted. */
	std::optional<Reply> replyToWake;
};

struct Held final {
	std::vector<Heard> heard;
	/* Whether each send to a suspend's address reached it: a client that
	 * gave up has removed its socket. */
	std::vector<bool> sent;
};

void serveHeldSuspend(int server, const HeldSuspend &scenario,
	const std::atomic_bool &done, Held &held)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	sockaddr_un suspendClient {};
	socklen_t suspendClientLength {};
	sockaddr_un firstClient {};
	socklen_t firstClientLength {};
	std::chrono::steady_clock::time_point first;
	std::chrono::steady_clock::time_point firstReplyAt;
	bool firstReplyPending = false;
	std::size_t suspends = 0U;
	bool wakeAnswered = false;
	while (!done.load()) {
		if (firstReplyPending &&
		    std::chrono::steady_clock::now() >= firstReplyAt) {
			firstReplyPending = false;
			held.sent.push_back(sendReply(server, firstClient,
				firstClientLength, scenario.firstReply->first,
				scenario.firstReply->second, scenario.firstReplyMore));
		}
		if (!awaitRequest(server, 20))
			continue;
		const auto command = receiveRequest(server, client, clientLength);
		const auto now = std::chrono::steady_clock::now();
		if (held.heard.empty())
			first = now;
		held.heard.push_back({command,
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now - first)});
		if (command == "suspend") {
			suspendClient = client;
			suspendClientLength = clientLength;
			if (++suspends == 1U && scenario.firstReply) {
				firstClient = client;
				firstClientLength = clientLength;
				firstReplyAt = now + scenario.firstReplyAfter;
				firstReplyPending = true;
			}
		} else if (command == "wake") {
			require(sendReply(server, client, clientLength, 0U, "running"));
			if (scenario.replyToWake && !wakeAnswered) {
				wakeAnswered = true;
				held.sent.push_back(sendReply(server, suspendClient,
					suspendClientLength, scenario.replyToWake->first,
					scenario.replyToWake->second));
			}
		}
	}
}

/* Runs suspend_guest and the resume that follows it against such a console
 * and returns what the console heard. A suspend asks for no writeback: the
 * caller does, once it knows that the guest runs. */
Held holdSuspend(const HeldSuspend &scenario,
	::uml::device::GuestSuspension &suspension)
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	Held held;
	std::atomic_bool done {false};
	std::thread console {serveHeldSuspend, server, std::cref(scenario),
		std::cref(done), std::ref(held)};
	suspension = ::uml::device::suspend_guest();
	::uml::device::resume_guest(suspension);
	/* A request too many shows too. */
	std::this_thread::sleep_for(std::chrono::milliseconds {300});
	done.store(true);
	console.join();
	(void)::close(server);
	return held;
}

const std::vector<std::string> aborted {"suspend", "wake"};
const std::vector<std::string> abortedAndWoken {"suspend", "wake", "wake"};

/* A suspend with no reply at its deadline is aborted with a wake, not taken
 * for a suspended guest, and the final word of the request is read from its
 * own socket, which stays open for it: a freezer that cannot finish refuses
 * the suspend long after the host's wait. A refusal is a guest known to run,
 * which the resume leaves alone. */
void aSuspendThatDoesNotReplyIsAborted()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.replyToWake = busySuspend}, suspension);
	require(suspension == ::uml::device::GuestSuspension::none);
	require(commandsOf(held.heard) == aborted);
	/* One deadline for the whole suspend, from the request. */
	require(held.heard[1].at >= std::chrono::milliseconds {9500} &&
		held.heard[1].at < std::chrono::milliseconds {11000});
	require(held.sent == std::vector<bool> {true},
		"the late refusal reached the request's socket");
}

/* A guest that says nothing after the wake may be asleep or running. */
void aSilentSuspendIsUnconfirmed()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend({}, suspension);
	require(suspension == ::uml::device::GuestSuspension::unconfirmed);
	require(commandsOf(held.heard) == abortedAndWoken,
		"the resume wakes an unconfirmed guest");
	require(held.heard[1].at >= std::chrono::milliseconds {9500} &&
		held.heard[1].at < std::chrono::milliseconds {11000});
}

/* The guest replies "suspended" before it checks for a wake, so the word does
 * not prove the sleep once the wake has been sent. */
void aSuspendedReplyAfterTheWakeIsUnconfirmed()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.replyToWake = enteredSuspend}, suspension);
	require(suspension == ::uml::device::GuestSuspension::unconfirmed);
	require(commandsOf(held.heard) == abortedAndWoken,
		"the resume wakes an unconfirmed guest");
	require(held.sent == std::vector<bool> {true});
}

/* A reply whose first part was a refusal is a refusal as a whole, though its
 * last part, which carries no error flag, arrives only after the host's
 * deadline and is read by itself: the guest is known to run and is not woken
 * again, and nothing the tail says, "suspended, synced" included, confirms
 * the suspend. */
void aReplyRefusedInItsFirstPartStaysRefused()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.firstReply = Reply {1U, "Suspend failed: "},
		 .firstReplyMore = 1U,
		 .replyToWake = enteredSyncedSuspend},
		suspension);
	require(suspension == ::uml::device::GuestSuspension::none,
		"the end of a refused reply confirmed the suspend");
	require(commandsOf(held.heard) == aborted);
	require(held.sent == std::vector<bool> {true, true},
		"the first part and the end reached the request's socket");
}

/* A busy refusal late in the wait does not start a wait of its own: the
 * retry's reply is awaited only until the one deadline. */
void aBusyRetryCannotOutlastTheDeadline()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.firstReply = busySuspend,
		 .firstReplyAfter = std::chrono::milliseconds {7900}},
		suspension);
	require(suspension == ::uml::device::GuestSuspension::unconfirmed);
	require(commandsOf(held.heard) == std::vector<std::string> {
		"suspend", "suspend", "wake", "wake"});
	require(held.heard[2].at >= std::chrono::milliseconds {9500} &&
		held.heard[2].at < std::chrono::milliseconds {10500},
		"the wake follows the deadline, not the retry's own wait");
}

/* Every guest is refused its first suspend busy: it is asked again every
 * 250 ms for two seconds after the first refusal, nothing stops it, and when
 * it stays busy it is left running. `refusedAt` is when the first refusal
 * reached the host. */
void requireRetriedUntilTheDeadline(const std::vector<Heard> &heard,
	std::chrono::milliseconds refusedAt)
{
	require(heard.size() >= 4U, "a busy guest is asked again");
	std::size_t suspends = 0U;
	for (const auto &entry : heard) {
		require(entry.command == "suspend",
			"nothing but suspends is sent, never a stop or a writeback");
		++suspends;
	}
	/* The first retry waits out the interval, and no retry follows the
	 * deadline: seven fit in two seconds, whatever the host's load adds. */
	require(heard[1].at >= refusedAt + std::chrono::milliseconds {200});
	const auto &lastSuspend = heard.back();
	require(lastSuspend.at < refusedAt + std::chrono::milliseconds {3000},
		"a busy guest is asked only within the deadline");
	require(suspends <= 8U);
}

/* A guest that stays busy is retried until the deadline and then left
 * running, never stopped. */
void aBusySuspendIsRetriedUntilTheDeadline()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto heard = suspendAgainst(
		[](const std::string &, std::size_t) { return busySuspend; },
		std::chrono::milliseconds {0}, suspension);
	require(suspension == ::uml::device::GuestSuspension::none);
	requireRetriedUntilTheDeadline(heard, std::chrono::milliseconds {0});
}

/* The guest replies to a suspend only after syncing its filesystems, so a
 * refusal can arrive seconds after the request. The two seconds start with
 * the refusal. */
void aLateBusyRefusalIsRetriedOnlyWithinTheDeadline()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto heard = suspendAgainst(
		[](const std::string &, std::size_t) { return busySuspend; },
		std::chrono::milliseconds {3000}, suspension);
	require(suspension == ::uml::device::GuestSuspension::none);
	requireRetriedUntilTheDeadline(heard, std::chrono::milliseconds {3000});
}

/* A guest whose busy state passes suspends: no stop. */
void aRetryThatSucceedsSuspendsTheGuest()
{
	auto suspension = ::uml::device::GuestSuspension::none;
	const auto heard = suspendAgainst(
		[](const std::string &, std::size_t index) {
			return index < 2U ? busySuspend : enteredSuspend;
		},
		std::chrono::milliseconds {0}, suspension);
	require(suspension == ::uml::device::GuestSuspension::system);
	require(commandsOf(heard) ==
		std::vector<std::string> {"suspend", "suspend", "suspend"});
}

/* A kernel that states whether its suspend synced says it in the reply, with
 * the word an older host looks for. Only the two whole texts state anything: a
 * kernel that replies "suspended" (sc1 and older) says nothing, and so does a
 * text that merely holds the statement's words, a refusal that mentions them,
 * and a refused suspend. The statement is reported with the suspension and
 * never without it. */
void aSuspendStatesWhetherItSynced()
{
	using ::uml::device::GuestSuspendSync;
	using ::uml::device::GuestSuspension;
	const struct {
		Reply reply;
		GuestSuspension suspension;
		GuestSuspendSync sync;
	} cases[] = {
		{enteredSyncedSuspend, GuestSuspension::system,
			GuestSuspendSync::synced},
		{enteredUnsyncedSuspend, GuestSuspension::system,
			GuestSuspendSync::skipped},
		{enteredSuspend, GuestSuspension::system,
			GuestSuspendSync::unstated},
		/* Not the statement: more text, another case, the words elsewhere. */
		{{0U, "suspended, synced in 12 ms"}, GuestSuspension::system,
			GuestSuspendSync::unstated},
		{{0U, "Suspended, synced"}, GuestSuspension::system,
			GuestSuspendSync::unstated},
		{{0U, "suspended, synced "}, GuestSuspension::system,
			GuestSuspendSync::unstated},
		{{0U, "suspended, not synced, synced"}, GuestSuspension::system,
			GuestSuspendSync::unstated},
		/* A refusal states nothing, whatever it says. */
		{{1U, "suspended, synced"}, GuestSuspension::none,
			GuestSuspendSync::unstated},
		{failedSuspend, GuestSuspension::none, GuestSuspendSync::unstated},
	};
	for (const auto &item : cases) {
		auto suspension = GuestSuspension::none;
		auto sync = GuestSuspendSync::skipped;
		const auto heard = suspendAgainst(
			[&item](const std::string &, std::size_t) { return item.reply; },
			std::chrono::milliseconds {0}, suspension, &sync);
		require(suspension == item.suspension, item.reply.second.c_str());
		require(sync == item.sync, item.reply.second.c_str());
		require(commandsOf(heard) == std::vector<std::string> {"suspend"},
			item.reply.second.c_str());
	}
	/* A retry that succeeds after a busy refusal states what the retry did. */
	auto suspension = GuestSuspension::none;
	auto sync = GuestSuspendSync::unstated;
	const auto heard = suspendAgainst(
		[](const std::string &, std::size_t index) {
			return index < 1U ? busySuspend : enteredSyncedSuspend;
		},
		std::chrono::milliseconds {0}, suspension, &sync);
	require(suspension == GuestSuspension::system &&
		sync == GuestSuspendSync::synced);
	require(commandsOf(heard) ==
		std::vector<std::string> {"suspend", "suspend"});
	/* A caller that does not ask for the statement is not given one. */
	suspension = GuestSuspension::none;
	(void)suspendAgainst(
		[](const std::string &, std::size_t) { return enteredSyncedSuspend; },
		std::chrono::milliseconds {0}, suspension);
	require(suspension == GuestSuspension::system);
}

/* Only a busy guest is asked again. A kernel without the suspend command, any
 * other refusal, and a retry that fails some other way leave the guest
 * running. */
void aRefusalThatIsNotBusyIsNotRetried()
{
	using ::uml::device::GuestSuspension;
	const std::vector<std::string> once {"suspend"};
	const Reply notConfigured {1U, "System suspend is not configured"};
	for (const auto &refusal : {notConfigured, failedSuspend}) {
		auto suspension = GuestSuspension::system;
		const auto heard = suspendAgainst(
			[&refusal](const std::string &, std::size_t) {
				return refusal;
			},
			std::chrono::milliseconds {0}, suspension);
		require(suspension == GuestSuspension::none);
		require(commandsOf(heard) == once, refusal.second.c_str());
	}
	auto suspension = GuestSuspension::system;
	const auto heard = suspendAgainst(
		[](const std::string &, std::size_t index) {
			return index == 0U ? busySuspend : failedSuspend;
		},
		std::chrono::milliseconds {0}, suspension);
	require(suspension == GuestSuspension::none);
	require(commandsOf(heard) ==
		std::vector<std::string> {"suspend", "suspend"});
}

/* Answers one request with `answer` and `error` after `delayMs`. A client
 * that stopped waiting has removed its socket, so that reply goes nowhere. */
void answerLater(int descriptor, std::string_view answer, std::uint32_t error,
	int delayMs, std::vector<std::string> &requests)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	if (!awaitRequest(descriptor, 10000))
		return;
	requests.push_back(receiveRequest(descriptor, client, clientLength));
	std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
	(void)sendReply(descriptor, client, clientLength, error, answer);
}

/* Runs `request` while the console answers its command, which must be
 * `expected`, and returns the result. */
template<typename Request>
auto answered(int server, Request &&request, std::string_view answer,
	std::uint32_t error, int delayMs, std::string_view expected)
{
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, answer, error, delayMs,
		std::ref(requests)};
	const auto result = request();
	observer.join();
	require(requests == std::vector<std::string> {std::string {expected}});
	return result;
}

/* The power key's wake is one request, whatever the host last asked for. */
void aWakeSendsOneWakeRequest()
{
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet: nothing answers and nothing waits. */
	require(!::uml::device::wake_guest());
	const int server = bindServer(path);
	require(answered(server, ::uml::device::wake_guest, "running", 0U, 0,
		"wake"));
	requireOwnerOnly(path);
	/* Only a guest that did it has woken. */
	require(!answered(server, ::uml::device::wake_guest, "Unknown command",
		1U, 0, "wake"));
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* Answers one request with `parts`, all but the last as console text that
 * is followed by more, and returns what the console heard. */
template<typename Request>
auto answeredInParts(int server, Request &&request,
	const std::vector<Reply> &parts, std::string_view expected)
{
	std::vector<std::string> requests;
	std::thread observer {[&] {
		sockaddr_un client {};
		socklen_t clientLength {};
		if (!awaitRequest(server, 10000))
			return;
		requests.push_back(
			receiveRequest(server, client, clientLength));
		for (std::size_t index = 0U; index < parts.size(); ++index)
			(void)sendReply(server, client, clientLength,
				parts[index].first, parts[index].second,
				index + 1U < parts.size() ? 1U : 0U);
	}};
	const auto result = request();
	observer.join();
	require(requests == std::vector<std::string> {std::string {expected}});
	return result;
}

/* What the console heard, and each request's reply socket's path, for a
 * console that answers each request by `policy` and keeps listening a little
 * after the last, so that a request too many shows. */
struct Writeback final {
	std::vector<std::string> heard;
	std::vector<std::string> clients;
};

/* Answers each request with what `policy` says for it, after `delay`. */
void serveWriteback(int server,
	std::function<std::pair<Reply, std::chrono::milliseconds>(
		const std::string &, std::size_t)> policy,
	Writeback &writeback)
{
	for (int wait = 10000; awaitRequest(server, wait); wait = 600) {
		sockaddr_un client {};
		socklen_t clientLength {};
		const auto command = receiveRequest(server, client, clientLength);
		writeback.heard.push_back(command);
		writeback.clients.emplace_back(client.sun_path);
		const auto [reply, delay] = policy(command, writeback.heard.size() - 1U);
		std::this_thread::sleep_for(delay);
		(void)sendReply(server, client, clientLength, reply.first,
			reply.second);
	}
}

using Policy = std::function<std::pair<Reply, std::chrono::milliseconds>(
	const std::string &, std::size_t)>;
const std::chrono::milliseconds noDelay {0};

/* Runs `request` against such a console, in a run of its own: the console is
 * narrowed to its owner once per run. */
template<typename Request>
auto writebackAgainst(Policy policy, Request &&request, Writeback &writeback)
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::thread console {serveWriteback, server, std::move(policy),
		std::ref(writeback)};
	const auto result = request();
	console.join();
	requireOwnerOnly(consolePath());
	(void)::close(server);
	return result;
}

/* A writeback is one "sync", which counts only if the guest's final reply says
 * that it returned. A kernel that does not know the command is asked for SysRq
 * 's' instead, which only starts the writeback. */
void aWritebackIsOneSync()
{
	using ::uml::device::GuestWriteback;
	using ::uml::device::request_guest_writeback;
	const std::chrono::milliseconds wait {2000};
	{
		/* No guest console yet: nothing is sent and nothing waits. */
		const ::uml::device::GuestConsoleRun run {true};
		require(request_guest_writeback(wait, 0U) ==
			GuestWriteback::unreachable);
	}
	const auto writeback = [&] { return request_guest_writeback(wait, 0U); };
	{
		/* The guest synced and said so; nothing else is sent. */
		Writeback heard;
		require(writebackAgainst(
			[](const std::string &, std::size_t) {
				return std::pair {synced, noDelay};
			}, writeback, heard) == GuestWriteback::done);
		require(heard.heard == std::vector<std::string> {"sync"});
	}
	{
		/* An older kernel: unknown command, then SysRq 's' accepted. The
		 * result is that it started, which is no confirmation. */
		Writeback heard;
		require(writebackAgainst(
			[](const std::string &command, std::size_t) {
				return std::pair {
					command == "sync" ? unknownCommand : sysrqWriteback,
					noDelay};
			}, writeback, heard) == GuestWriteback::started);
		require(heard.heard == std::vector<std::string> {"sync", "sysrq s"});
	}
	{
		/* An older kernel whose SysRq mask is off refuses. */
		Writeback heard;
		require(writebackAgainst(
			[](const std::string &command, std::size_t) {
				return std::pair {command == "sync" ? unknownCommand :
					Reply {1U, "sysrq s disabled"}, noDelay};
			}, writeback, heard) == GuestWriteback::refused);
		require(heard.heard == std::vector<std::string> {"sync", "sysrq s"});
	}
	{
		/* An older kernel that never answers SysRq 's' leaves it unconfirmed;
		 * its sync request got an answer, so there is a second. */
		Writeback heard;
		require(writebackAgainst(
			[](const std::string &command, std::size_t) {
				return std::pair {command == "sync" ? unknownCommand :
					sysrqWriteback, command == "sync" ? noDelay :
					std::chrono::milliseconds {2600}};
			}, [] {
				return request_guest_writeback(
					std::chrono::milliseconds {2000}, 0U);
			}, heard) == GuestWriteback::unanswered);
	}
	{
		/* Any other refusal is no confirmation and no fallback. */
		Writeback heard;
		require(writebackAgainst(
			[](const std::string &, std::size_t) {
				return std::pair {Reply {1U, "Out of memory"}, noDelay};
			}, writeback, heard) == GuestWriteback::refused);
		require(heard.heard == std::vector<std::string> {"sync"});
	}
	{
		/* An answer that is not "synced" is not a confirmation either. */
		Writeback heard;
		require(writebackAgainst(
			[](const std::string &, std::size_t) {
				return std::pair {Reply {0U, "running"}, noDelay};
			}, writeback, heard) == GuestWriteback::refused);
		require(heard.heard == std::vector<std::string> {"sync"});
	}
}

/* A guest that does not reply within the wait, as one whose sync is held up by
 * a full host disk, is unconfirmed after that wait and not asked again by the
 * request: no SysRq fallback follows silence. Its late reply goes to the first
 * request's socket, which the host has removed, so the next request is
 * answered by its own reply alone. */
void aSyncThatIsNotAnsweredInTimeIsUnconfirmed()
{
	using ::uml::device::GuestWriteback;
	using ::uml::device::request_guest_writeback;
	Writeback heard;
	std::vector<GuestWriteback> results;
	const auto started = std::chrono::steady_clock::now();
	writebackAgainst(
		[](const std::string &, std::size_t index) {
			/* The first reply comes after the host stopped waiting. */
			return std::pair {synced, index == 0U ?
				std::chrono::milliseconds {900} : noDelay};
		},
		[&] {
			results.push_back(request_guest_writeback(
				std::chrono::milliseconds {300}, 0U));
			const auto firstEnded = std::chrono::steady_clock::now();
			require(firstEnded - started < std::chrono::milliseconds {850},
				"the wait ends at its bound, not with the reply");
			/* The console is still busy with the first request. */
			std::this_thread::sleep_for(std::chrono::milliseconds {700});
			results.push_back(request_guest_writeback(
				std::chrono::milliseconds {2000}, 0U));
			return 0;
		}, heard);
	require(results == std::vector<GuestWriteback> {
		GuestWriteback::unanswered, GuestWriteback::done});
	require(heard.heard == std::vector<std::string> {"sync", "sync"},
		"silence is not followed by SysRq");
	require(heard.clients[0] != heard.clients[1],
		"each request has a reply socket of its own");
}

/* What a console that answers one "sync" did, and what came after. */
struct OneSync final {
	::uml::device::GuestWriteback result {};
	std::vector<std::string> heard;
	std::chrono::milliseconds took {};
};

/* Runs request_guest_writeback(`wait`, `settle`) against a console whose
 * answer to the first request is `serve`'s: it is handed the descriptor and the
 * client to answer, and the console then listens for another request for a
 * while, which is the fallback's, and answers it with `second`, if any. */
OneSync oneSyncAgainst(
	std::function<void(int, const sockaddr_un &, socklen_t)> serve,
	std::chrono::milliseconds wait, unsigned int settle = 0U,
	std::function<void(int, const sockaddr_un &, socklen_t)> second = {})
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	OneSync one;
	std::thread console {[&] {
		sockaddr_un client {};
		socklen_t clientLength {};
		if (!awaitRequest(server, 10000))
			return;
		one.heard.push_back(receiveRequest(server, client, clientLength));
		serve(server, client, clientLength);
		for (int more = 0; more < 2 && awaitRequest(server, 250); ++more) {
			one.heard.push_back(
				receiveRequest(server, client, clientLength));
			if (second)
				second(server, client, clientLength);
		}
	}};
	const auto started = std::chrono::steady_clock::now();
	one.result = ::uml::device::request_guest_writeback(wait, settle);
	one.took = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started);
	console.join();
	(void)::close(server);
	return one;
}

/* A datagram of `bytes` bytes of a reply whose header advertises `length`
 * bytes of text, as a console that sends less than it says does. */
void sendRawReply(int descriptor, const sockaddr_un &client,
	socklen_t clientLength, std::uint32_t error, std::uint32_t length,
	std::string_view text, std::size_t bytes)
{
	uml::testing::ConsoleReply reply {error, 0U, length, {}};
	std::memcpy(reply.data.data(), text.data(), text.size());
	require(::sendto(descriptor, &reply, bytes, 0,
		reinterpret_cast<const sockaddr *>(&client), clientLength) ==
		static_cast<ssize_t>(bytes));
}

/* A datagram longer than a reply can be, whose header advertises `text`: a
 * receive that cuts it to the size of a reply would find it whole. */
void sendOversizedReply(int descriptor, const sockaddr_un &client,
	socklen_t clientLength, std::string_view text, std::size_t extra)
{
	std::vector<char> datagram(sizeof(uml::testing::ConsoleReply) + extra, '\0');
	uml::testing::ConsoleReply reply {0U, 0U,
		static_cast<std::uint32_t>(text.size() + 1U), {}};
	std::memcpy(reply.data.data(), text.data(), text.size());
	std::memcpy(datagram.data(), &reply, sizeof(reply));
	require(::sendto(descriptor, datagram.data(), datagram.size(), 0,
		reinterpret_cast<const sockaddr *>(&client), clientLength) ==
		static_cast<ssize_t>(datagram.size()));
}

/* Only the guest's whole reply, "synced in <n> ms", is a confirmation: text
 * that merely starts alike, a count that is no number, a datagram that carries
 * less than it advertises, one shorter than a header, one longer than a reply
 * can be, and an error flagged in any part of the reply are not, and none of them starts the SysRq fallback,
 * which only a reply that says "Unknown command" and nothing more does. */
void aWritebackReplyIsValidatedStrictly()
{
	using ::uml::device::GuestWriteback;
	const std::chrono::milliseconds wait {1000};
	const auto text = [](std::uint32_t error, std::string_view reply) {
		return [error, reply](int fd, const sockaddr_un &to, socklen_t n) {
			(void)sendReply(fd, to, n, error, reply);
		};
	};
	const auto sync = [&](std::function<void(int, const sockaddr_un &,
		socklen_t)> serve) {
		return oneSyncAgainst(std::move(serve), wait);
	};
	const std::vector<std::string> onlySync {"sync"};
	for (const char *reply : {"synced", "synced in", "synced in  ms",
		     "synced in NaN ms", "synced in -1 ms", "synced in 3ms",
		     "synced in 3 ms and more", "Synced in 3 ms",
		     "synced in 99999999999999999999999 ms", "synced in 3 ms\n"}) {
		const auto one = sync(text(0U, reply));
		require(one.result == GuestWriteback::refused, reply);
		require(one.heard == onlySync, reply);
	}
	/* A reply the guest flags as an error is a refusal, whatever it says. */
	{
		const auto one = sync(text(1U, "synced in 3 ms"));
		require(one.result == GuestWriteback::refused);
		require(one.heard == onlySync);
	}
	/* A kernel that does not know the command says so, and says no more. */
	for (const char *reply : {"Unknown command: access denied",
		     "Unknown command.", "unknown command", "Unknown"}) {
		const auto one = sync(text(1U, reply));
		require(one.result == GuestWriteback::refused, reply);
		require(one.heard == onlySync,
			"no SysRq fallback for a refusal that is not exactly it");
	}
	{
		/* No error flag: not the kernel's refusal either. */
		const auto one = sync(text(0U, "Unknown command"));
		require(one.result == GuestWriteback::refused);
		require(one.heard == onlySync);
	}
	{
		/* The text it says, whole, with the NUL that the guest counts in its
		 * length, is the reply. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			constexpr std::string_view reply {"synced in 7 ms\0", 15U};
			sendRawReply(fd, to, n, 0U, 15U, reply,
				12U + reply.size());
		});
		require(one.result == GuestWriteback::done);
	}
	{
		/* A datagram that advertises 40 bytes of text and carries 6. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			sendRawReply(fd, to, n, 0U, 40U, "synced", 12U + 6U);
		});
		require(one.result == GuestWriteback::refused,
			"a truncated reply is no confirmation");
		require(one.heard == onlySync);
	}
	{
		/* One whose text is whole but whose last advertised byte, the NUL
		 * the guest counts, is not in the datagram: the zeros that fill a
		 * receive buffer would make it read as the reply. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			constexpr std::string_view reply {"synced in 7 ms"};
			sendRawReply(fd, to, n, 0U, 15U, reply, 12U + reply.size());
		});
		require(one.result == GuestWriteback::refused,
			"a datagram one byte short is no confirmation");
	}
	{
		/* One that advertises more than a reply can carry. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			sendRawReply(fd, to, n, 0U, 9999U, "synced in 3 ms",
				sizeof(uml::testing::ConsoleReply));
		});
		require(one.result == GuestWriteback::refused);
	}
	for (const std::size_t extra : {std::size_t {1U}, std::size_t {64U}}) {
		/* One longer than a reply can be, whose first 524 bytes are the
		 * guest's whole reply: only what fits would be read. */
		const auto one = sync([extra](int fd, const sockaddr_un &to,
					    socklen_t n) {
			sendOversizedReply(fd, to, n, "synced in 3 ms", extra);
		});
		require(one.result == GuestWriteback::refused,
			"an oversized datagram is no confirmation");
		require(one.heard == onlySync);
	}
	{
		/* One shorter than the header. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			sendRawReply(fd, to, n, 0U, 0U, {}, 4U);
		});
		require(one.result == GuestWriteback::refused);
		require(one.heard == onlySync);
	}
	{
		/* An error in the first part of a reply that goes on is kept, though
		 * its last part carries none. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			(void)sendReply(fd, to, n, 1U, "synced in ", 1U);
			(void)sendReply(fd, to, n, 0U, "3 ms");
		});
		require(one.result == GuestWriteback::refused);
	}
	{
		/* A reply in parts, none of them failed, is read whole. */
		const auto one = sync([](int fd, const sockaddr_un &to, socklen_t n) {
			(void)sendReply(fd, to, n, 0U, "synced in ", 1U);
			(void)sendReply(fd, to, n, 0U, "3 ms");
		});
		require(one.result == GuestWriteback::done);
	}
	{
		/* "Unknown command" in the kernel's own form falls back, and
		 * SysRq's acknowledgement is only a start. */
		const auto one = oneSyncAgainst(text(1U, "Unknown command"), wait, 0U,
			text(0U, "sysrq s ok"));
		require(one.result == GuestWriteback::started);
		require(one.heard == std::vector<std::string> {"sync", "sysrq s"});
	}
}

/* A writeback's wait is one budget from the call, shared by the wait for the
 * console, every part of the reply, the SysRq fallback of a kernel without the
 * command, and its settling period. */
void aWritebackWaitsOneBudgetForEverything()
{
	using ::uml::device::GuestWriteback;
	using ::uml::device::request_guest_writeback;
	using namespace std::chrono_literals;
	const auto later = [](std::chrono::milliseconds delay, std::uint32_t error,
		std::string_view text, std::uint32_t more = 0U) {
		return [=](int fd, const sockaddr_un &to, socklen_t n) {
			std::this_thread::sleep_for(delay);
			(void)sendReply(fd, to, n, error, text, more);
		};
	};
	{
		/* An older kernel, quick to answer both: the settling period is what
		 * is left of the budget, not its own 1.5 s. */
		const auto one = oneSyncAgainst(later(400ms, 1U, "Unknown command"),
			1000ms, 1500U, later(300ms, 0U, "sysrq s ok"));
		require(one.result == GuestWriteback::started);
		require(one.heard == std::vector<std::string> {"sync", "sysrq s"});
		require(one.took >= 950ms && one.took < 1400ms,
			"the settling period ends with the budget");
	}
	{
		/* An older kernel that is slow: the fallback waits only what is left
		 * (the probe's 3 s + 1.8 s + 1.5 s took 6.3 s for 3.5 s). */
		const auto one = oneSyncAgainst(later(700ms, 1U, "Unknown command"),
			1000ms, 1500U, later(600ms, 0U, "sysrq s ok"));
		require(one.result == GuestWriteback::unanswered);
		require(one.took >= 950ms && one.took < 1400ms,
			"the fallback's reply is waited for no longer than the budget");
	}
	{
		/* A budget that the first reply uses up leaves no fallback. */
		const auto one = oneSyncAgainst(later(1200ms, 1U, "Unknown command"),
			1000ms, 1500U, later(0ms, 0U, "sysrq s ok"));
		require(one.result == GuestWriteback::unanswered);
		require(one.took < 1400ms);
		require(one.heard == std::vector<std::string> {"sync"},
			"nothing is sent after the budget");
	}
	{
		/* A reply in many parts is waited for as one: five parts 150 ms apart
		 * with a 400 ms budget used to take 750 ms, each part restarting the
		 * wait. */
		const auto one = oneSyncAgainst(
			[](int fd, const sockaddr_un &to, socklen_t n) {
				for (int part = 0; part < 5; ++part) {
					std::this_thread::sleep_for(150ms);
					(void)sendReply(fd, to, n, 0U,
						part == 0 ? "synced in " : part == 4 ? "1 ms" : "",
						part == 4 ? 0U : 1U);
				}
			}, 400ms);
		require(one.result == GuestWriteback::unanswered);
		require(one.took >= 350ms && one.took < 650ms,
			"the parts of a reply share one wait");
	}
	{
		/* The console may be held by another request: the wait for it counts
		 * against the budget, and nothing is sent. */
		const ::uml::device::GuestConsoleRun run {true};
		const int server = bindServer(consolePath());
		std::vector<std::string> heard;
		std::thread console {[&] {
			sockaddr_un client {};
			socklen_t clientLength {};
			while (awaitRequest(server, 1500)) {
				heard.push_back(
					receiveRequest(server, client, clientLength));
				std::this_thread::sleep_for(900ms);
				(void)sendReply(server, client, clientLength, 0U, "trimmed");
			}
		}};
		std::thread trim {[] {
			(void)::uml::device::trim_guest_page_cache(3000U);
		}};
		std::this_thread::sleep_for(150ms);
		const auto started = std::chrono::steady_clock::now();
		const auto result = request_guest_writeback(300ms, 0U);
		const auto took =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started);
		trim.join();
		console.join();
		(void)::close(server);
		require(result == GuestWriteback::busy);
		require(took >= 250ms && took < 700ms,
			"the wait for the console counts against the budget");
		require(heard == std::vector<std::string> {"trim once"},
			"nothing is sent to a console that was not free");
	}
}

/* Turning trimming on or off is one request, which a kernel without the
 * command refuses. */
void trimmingIsSwitchedByOneRequest()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::set_guest_page_cache_trim;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet: nothing is sent and nothing waits. */
	require(set_guest_page_cache_trim(true) == GuestRequest::unreachable);
	const int server = bindServer(path);
	require(answered(server, [] { return set_guest_page_cache_trim(true); },
			"on", 0U, 0, "trim on") == GuestRequest::done);
	requireOwnerOnly(path);
	require(answered(server, [] { return set_guest_page_cache_trim(false); },
			"off", 0U, 0, "trim off") == GuestRequest::done);
	require(answered(server, [] { return set_guest_page_cache_trim(true); },
			"Unknown command", 1U, 0, "trim on") == GuestRequest::refused);
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* The request that does not wait returns at once with the guest silent or
 * gone, leaves a request another thread has in flight alone, and reaches the
 * guest as the ordinary one does. */
void aNoWaitTrimRequestReturnsAtOnce()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::set_guest_page_cache_trim;
	using ::uml::device::set_guest_page_cache_trim_nowait;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet. */
	require(set_guest_page_cache_trim_nowait(true) ==
		GuestRequest::unreachable);
	const int server = bindServer(path);
	const auto started = std::chrono::steady_clock::now();
	require(set_guest_page_cache_trim_nowait(true) == GuestRequest::sent);
	require(set_guest_page_cache_trim_nowait(false) == GuestRequest::sent);
	/* The guest never answers: nothing waited for it. */
	require(std::chrono::steady_clock::now() - started <
		std::chrono::milliseconds {1500});
	sockaddr_un client {};
	socklen_t clientLength {};
	require(awaitRequest(server, 1000));
	require(receiveRequest(server, client, clientLength) == "trim on");
	require(awaitRequest(server, 1000));
	require(receiveRequest(server, client, clientLength) == "trim off");
	requireOwnerOnly(path);

	/* A request in flight on another thread holds the console: the one that
	 * does not wait is busy and sends nothing. */
	std::atomic<bool> inFlight {false};
	std::thread holder {[&] {
		inFlight = true;
		(void)set_guest_page_cache_trim(true);
	}};
	require(awaitRequest(server, 5000));
	const auto command = receiveRequest(server, client, clientLength);
	require(command == "trim on");
	require(set_guest_page_cache_trim_nowait(true) == GuestRequest::busy);
	/* Let the holder's wait end with the reply. */
	require(sendReply(server, client, clientLength, 0U, "trim on"));
	holder.join();
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* A pass takes longer than an ordinary request may wait, and is waited for
 * up to its own bound and no longer. */
void aTrimPassIsWaitedForUpToItsBound()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::trim_guest_page_cache;
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	require(answered(server, [] { return trim_guest_page_cache(8000U); },
			"trimmed 4096 KiB in 3 ms, compacted in 40 ms, "
			"released 4096 KiB in 2 ms",
			0U, 2500,
			"trim once") == GuestRequest::done);
	/* The reply comes 2.5 seconds late; the request gives up long
	 * before, and before an ordinary request's two seconds. */
	std::chrono::steady_clock::duration waited {};
	require(answered(server,
			[&waited] {
				const auto start = std::chrono::steady_clock::now();
				const auto result = trim_guest_page_cache(200U);
				waited = std::chrono::steady_clock::now() - start;
				return result;
			},
			"trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			"released 0 KiB in 1 ms",
			0U, 2500,
			"trim once") == GuestRequest::unanswered);
	require(waited < std::chrono::milliseconds(1800));
	(void)::close(server);
}

/* Sends datagrams to the console, which nothing reads, until its queue has
 * no room: Darwin then refuses one (ENOBUFS) at once. Returns how many it
 * took. */
std::size_t fillTheConsoleQueue(const std::string &path)
{
	const int sender = ::socket(AF_UNIX, SOCK_DGRAM, 0);
	require(sender >= 0);
	sockaddr_un console {};
	console.sun_family = AF_UNIX;
	require(path.size() < sizeof(console.sun_path));
	std::memcpy(console.sun_path, path.c_str(), path.size() + 1U);
	uml::testing::ConsoleRequest filler {};
	filler.length = 4U;
	std::memcpy(filler.data.data(), "fill", 4U);
	for (std::size_t sent = 0; sent < 1000U; ++sent) {
		if (::sendto(sender, &filler, sizeof(filler), MSG_DONTWAIT,
			    reinterpret_cast<const sockaddr *>(&console),
			    sizeof(console)) < 0) {
			require(errno == ENOBUFS || errno == EAGAIN,
				"the console queue refused for another reason");
			(void)::close(sender);
			return sent;
		}
	}
	require(false, "the console queue never filled");
	return 0;
}

/* The console's queue holds a few requests and a full one refuses at once
 * (ENOBUFS, blocking or not), so a request that waits for its reply waits for
 * room too, within the same wait: it goes through once the guest has read
 * what is ahead of it, and is not sent when the wait ends first. */
void aRequestWaitsForRoomInAFullQueueWithinItsWait()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::trim_guest_page_cache;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	const int server = bindServer(path);

	/* The guest reads again after a while: the request goes through behind
	 * the full queue, in the wait it was given. */
	require(fillTheConsoleQueue(path) > 0U);
	std::thread guest {[&] {
		std::this_thread::sleep_for(std::chrono::milliseconds {300});
		sockaddr_un client {};
		socklen_t clientLength {};
		while (awaitRequest(server, 5000)) {
			if (receiveRequest(server, client, clientLength) ==
			    "trim once") {
				(void)sendReply(server, client, clientLength, 0U,
					"trimmed 0 KiB in 0 ms, compacted in 0 ms, "
					"released 0 KiB in 0 ms");
				return;
			}
		}
	}};
	const auto started = std::chrono::steady_clock::now();
	require(trim_guest_page_cache(5000U) == GuestRequest::done);
	const auto waited = std::chrono::steady_clock::now() - started;
	guest.join();
	require(waited >= std::chrono::milliseconds {250},
		"the request did not wait for room");
	require(!awaitRequest(server, 200));

	/* The guest never reads: the request is not sent when its wait ends, and
	 * the wait is not spent twice. */
	require(fillTheConsoleQueue(path) > 0U);
	const auto begun = std::chrono::steady_clock::now();
	require(trim_guest_page_cache(300U) == GuestRequest::unreachable);
	const auto spent = std::chrono::steady_clock::now() - begun;
	require(spent >= std::chrono::milliseconds {250},
		"the request gave up before its wait ended");
	require(spent < std::chrono::milliseconds {1500},
		"the request waited longer than its wait");
	sockaddr_un client {};
	socklen_t clientLength {};
	while (awaitRequest(server, 200))
		require(receiveRequest(server, client, clientLength) == "fill",
			"a request that was not sent reached the console");

	/* The guest reads after most of the wait and never answers: the time the
	 * request spent finding room is part of its wait, not added to it. */
	require(fillTheConsoleQueue(path) > 0U);
	std::thread silent {[&] {
		std::this_thread::sleep_for(std::chrono::milliseconds {600});
		sockaddr_un from {};
		socklen_t fromLength {};
		while (awaitRequest(server, 200))
			(void)receiveRequest(server, from, fromLength);
	}};
	const auto asked = std::chrono::steady_clock::now();
	require(trim_guest_page_cache(1000U) == GuestRequest::unanswered);
	const auto total = std::chrono::steady_clock::now() - asked;
	silent.join();
	require(total >= std::chrono::milliseconds {900},
		"the request did not wait for its reply");
	require(total < std::chrono::milliseconds {1400},
		"the time spent finding room was waited for again");
	(void)::close(server);
}

/* ---- The guest's memory figures: "mem". ---- */

using ::uml::device::GuestMemoryFigures;
using ::uml::device::GuestMemoryQuery;
using ::uml::device::GuestMemoryResult;
using ::uml::device::parse_guest_memory_reply;

/* The line a guest answers, and every way the host refuses to read one. */
void theMemoryReplyIsParsedStrictly()
{
	constexpr std::uint64_t gib = 1ULL << 30;
	const auto plain = parse_guest_memory_reply(
		"total=2147483648 available=1073741824 free=536870912");
	require(plain && plain->total_bytes == 2 * gib &&
		plain->available_bytes == gib && plain->free_bytes == gib / 2);
	/* The field order and a line end are the guest's to choose; a field it
	 * adds later is not ours to refuse. */
	const auto reordered = parse_guest_memory_reply(
		"free=7 available=5 total=9 swap=4\n");
	require(reordered && reordered->total_bytes == 9 &&
		reordered->available_bytes == 5 && reordered->free_bytes == 7);
	/* `free` is informational. */
	const auto withoutFree = parse_guest_memory_reply("total=9 available=5");
	require(withoutFree && withoutFree->free_bytes == 0);
	const auto largest = parse_guest_memory_reply(
		"total=18446744073709551615 available=0 free=0");
	require(largest && largest->total_bytes == UINT64_MAX);

	for (const std::string_view bad : {
		     "", " ", "total=1", "available=1", "total=1 free=1",
		     "total= available=1", "total=1 available=", "=1 total=1",
		     "total=-1 available=1", "total=+1 available=1",
		     "total=1.5 available=1", "total=0x10 available=1",
		     "total=1 available=1x", "total=1  available=1",
		     "total=1 available=1 ", " total=1 available=1",
		     "total=1 total=2 available=1",
		     "total=1 available=1 available=1",
		     "total=18446744073709551616 available=1",
		     "total 1 available 1", "total:1 available:1",
		     "Unknown command", "total=1 available=1 stray",
		     "total=1\navailable=1"})
		require(!parse_guest_memory_reply(bad),
			"a malformed memory reply was accepted");
}

/* Sends the request and waits for its reply with the console answering as
 * `policy` says. */
GuestMemoryResult askMemory(int server, std::string_view answer,
	std::uint32_t error, int delayMs, std::chrono::milliseconds wait,
	GuestMemoryFigures &figures, std::chrono::milliseconds &took)
{
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, answer, error, delayMs,
		std::ref(requests)};
	const auto began = std::chrono::steady_clock::now();
	auto query = GuestMemoryQuery::start();
	require(query.has_value(), "the request was not sent");
	const auto result = query->await(wait, figures);
	took = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - began);
	observer.join();
	require(requests == std::vector<std::string> {"mem"},
		"the guest must hear one \"mem\"");
	return result;
}

void aMemoryRequestReadsTheGuestsFigures()
{
	const ::uml::device::GuestConsoleRun run {true};
	/* No guest console yet: nothing is sent. */
	require(!GuestMemoryQuery::start().has_value());
	const int server = bindServer(consolePath());
	GuestMemoryFigures figures;
	std::chrono::milliseconds took {};
	constexpr std::chrono::milliseconds bound {500};
	require(askMemory(server,
		"total=1913519104 available=1056964608 free=196083712", 0U, 0,
		bound, figures, took) == GuestMemoryResult::done);
	require(figures.total_bytes == 1913519104ULL &&
		figures.available_bytes == 1056964608ULL &&
		figures.free_bytes == 196083712ULL);
	requireOwnerOnly(consolePath());
	/* The kernel's driver counts the NUL that ends a reply in its length. */
	constexpr char terminated[] = "total=2048 available=1024 free=512";
	require(askMemory(server, std::string_view {terminated, sizeof(terminated)},
		0U, 0, bound, figures, took) == GuestMemoryResult::done);
	require(figures.total_bytes == 2048ULL &&
		figures.available_bytes == 1024ULL && figures.free_bytes == 512ULL);
	/* A kernel without the command refuses it. */
	require(askMemory(server, "Unknown command", 1U, 0, bound, figures,
		took) == GuestMemoryResult::refused);
	/* A guest that answers something else did the command no good. */
	require(askMemory(server, "total=1", 0U, 0, bound, figures, took) ==
		GuestMemoryResult::malformed);
	require(askMemory(server, "hello", 0U, 0, bound, figures, took) ==
		GuestMemoryResult::malformed);
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* A guest that does not answer is waited for up to the bound and no longer,
 * and a reply that comes after the bound goes nowhere. The request can be
 * waited on again until it is destroyed. */
void aSilentGuestIsWaitedForUpToTheBound()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	GuestMemoryFigures figures;
	std::chrono::milliseconds took {};
	require(askMemory(server,
		"total=100 available=50 free=10", 0U, 1500,
		std::chrono::milliseconds {200}, figures, took) ==
		GuestMemoryResult::unanswered);
	require(took >= std::chrono::milliseconds {150} &&
		took < std::chrono::milliseconds {1200},
		"the wait must end at its bound, not at the reply");
	/* A zero wait takes only what has arrived. */
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, "total=8 available=4 free=2",
		0U, 300, std::ref(requests)};
	auto query = GuestMemoryQuery::start();
	require(query.has_value());
	require(query->await(std::chrono::milliseconds {0}, figures) ==
		GuestMemoryResult::unanswered);
	require(query->await(std::chrono::milliseconds {5000}, figures) ==
		GuestMemoryResult::done);
	require(figures.total_bytes == 8 && figures.available_bytes == 4);
	observer.join();
	(void)::close(server);
}

/* The memory request is sent under the console's lock, which a caller's
 * lifecycle lock may wait behind: it makes one attempt, so that a console whose
 * queue is full is answered at once and not after a wait for room. */
void aMemoryRequestDoesNotWaitForRoomInAFullQueue()
{
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	const int server = bindServer(path);
	require(fillTheConsoleQueue(path) > 0U);
	const auto started = std::chrono::steady_clock::now();
	auto query = GuestMemoryQuery::start();
	const auto spent = std::chrono::steady_clock::now() - started;
	require(!query.has_value(), "a request was sent to a full queue");
	require(spent < std::chrono::milliseconds {300},
		"the memory request waited for room in the console's queue");
	sockaddr_un client {};
	socklen_t clientLength {};
	while (awaitRequest(server, 200))
		require(receiveRequest(server, client, clientLength) == "fill",
			"a memory request that was not sent reached the console");
	(void)::close(server);
}

/* The reply is waited for without the console's lock: another request
 * reaches the guest, and is answered, while a "mem" waits for a slow one. */
void aWaitingMemoryRequestDoesNotHoldTheConsole()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::atomic_bool done {};
	std::mutex heardMutex;
	std::vector<std::string> heard;
	const auto heardCount = [&] {
		const std::lock_guard lock {heardMutex};
		return heard.size();
	};
	std::thread guest {[&] {
		sockaddr_un client {};
		socklen_t clientLength {};
		sockaddr_un slow {};
		socklen_t slowLength {};
		std::chrono::steady_clock::time_point slowAt;
		bool slowPending = false;
		while (!done.load()) {
			if (slowPending &&
			    std::chrono::steady_clock::now() >= slowAt) {
				slowPending = false;
				(void)sendReply(server, slow, slowLength, 0U,
					"total=64 available=32 free=16");
			}
			if (!awaitRequest(server, 20))
				continue;
			const auto command = receiveRequest(server, client,
				clientLength);
			{
				const std::lock_guard lock {heardMutex};
				heard.push_back(command);
			}
			if (command == "mem") {
				slow = client;
				slowLength = clientLength;
				slowAt = std::chrono::steady_clock::now() +
					std::chrono::milliseconds {800};
				slowPending = true;
			} else {
				(void)sendReply(server, client, clientLength, 0U,
					"running");
			}
		}
	}};
	auto query = GuestMemoryQuery::start();
	require(query.has_value());
	/* "mem" is out and unanswered; a wake must not queue behind it. */
	require(waitUntil([&] { return heardCount() == 1U; },
		std::chrono::seconds {10}), "the guest never heard the request");
	const auto began = std::chrono::steady_clock::now();
	require(::uml::device::wake_guest(), "the wake was not answered");
	require(std::chrono::steady_clock::now() - began <
		std::chrono::milliseconds {600},
		"a wake waited for the memory request's reply");
	GuestMemoryFigures figures;
	require(query->await(std::chrono::seconds {10}, figures) ==
		GuestMemoryResult::done);
	require(figures.total_bytes == 64);
	done.store(true);
	guest.join();
	require(heard == std::vector<std::string> {"mem", "wake"});
	(void)::close(server);
}

/* The widest reply socket a location may name. */
std::size_t widestClientPath(const ::uml::device::GuestConsoleLocation &location)
{
	return location.client_prefix.size() + 8U;
}

void aDirectoryHoldsShortNamesThatFit()
{
	/* $TMPDIR of an iOS application: 89 bytes with its trailing slash. */
	const std::string container =
		"/private/var/mobile/Containers/Data/Application/"
		"00000000-0000-4000-8000-000000000000/tmp";
	const auto location =
		::uml::device::compose_guest_console_location(container + "/");
	require(location.has_value());
	require(location->directory == container);
	require(location->console == container + "/mconsole");
	require(location->client_prefix == container + "/mc-");
	require(location->console.size() < sizeof(sockaddr_un {}.sun_path));
	require(widestClientPath(*location) < sizeof(sockaddr_un {}.sun_path));

	const auto slashes =
		::uml::device::compose_guest_console_location(container + "///");
	require(slashes.has_value() && slashes->directory == container);
}

void aDirectoryThatDoesNotFitIsRefused()
{
	/* The longest directory whose widest reply socket still fits. */
	constexpr std::size_t limit = sizeof(sockaddr_un {}.sun_path);
	const std::size_t longest = limit - 1U - std::string_view {"/mc-"}.size() -
		8U;
	const auto fitting = "/" + std::string(longest - 1U, 'd');
	const auto fits = ::uml::device::compose_guest_console_location(fitting);
	require(fits.has_value() && widestClientPath(*fits) == limit - 1U);

	/* One byte more is refused outright: never truncated. */
	require(!::uml::device::compose_guest_console_location(fitting + "d")
			 .has_value());
	require(!::uml::device::compose_guest_console_location(
		"/" + std::string(limit, 'd')).has_value());
}

void aDirectoryMustBeAbsolute()
{
	require(!::uml::device::compose_guest_console_location(
		std::string_view {}).has_value());
	require(!::uml::device::compose_guest_console_location("tmp/")
			 .has_value());
}

void aRunDirectoryGoesInTheFirstParentThatFits()
{
	const std::vector<std::string> temporary {
		"/var/folders/yb/lx9rftq56156_v7rkkngd8bc0000gn/T/"};
	const auto location =
		::uml::device::compose_run_console_location(temporary, "kern42");
	require(location.has_value());
	require(location->directory ==
		"/var/folders/yb/lx9rftq56156_v7rkkngd8bc0000gn/T/uml-kern42");
	require(location->console == location->directory + "/mconsole");
	require(location->client_prefix == location->directory + "/mc-");

	/* Unset, relative and too-long parents are passed over, as the
	 * Simulator's application directory is. */
	const std::vector<std::string> fallbacks {
		"", "relative", "/" + std::string(100U, 'd'), "/tmp//"};
	const auto fallback =
		::uml::device::compose_run_console_location(fallbacks, "kern42");
	require(fallback.has_value() &&
		fallback->directory == "/tmp/uml-kern42");
	const std::vector<std::string> root {"/"};
	require(::uml::device::compose_run_console_location(root, "kern42")
			->directory == "/uml-kern42");
	const std::vector<std::string> none {"", "/" + std::string(100U, 'd')};
	require(!::uml::device::compose_run_console_location(none, "kern42")
			 .has_value());
}

void theProcessNamesItsOwnDirectory()
{
	const auto &process = processLocation();
	const auto umid = "kern" + std::to_string(::getpid());
	require(std::filesystem::path {process.directory}.filename() ==
		"uml-" + umid);
	/* $TMPDIR is the first choice whenever it is set and short enough. */
	const char *temporary = std::getenv("TMPDIR");
	if (temporary != nullptr && temporary[0] == '/' &&
	    std::string_view {temporary}.size() < 70U) {
		require(std::filesystem::path {process.directory}.parent_path() ==
			std::filesystem::path {std::string {temporary} + "/"}
				.lexically_normal().parent_path());
	}
	const auto arguments =
		::uml::device::guest_management_console_arguments();
	require(arguments == std::vector<std::string> {
		"umid=" + umid, "uml_dir=" + process.directory});
}

bool directoryExists(const std::string &path)
{
	struct stat details {};
	return ::lstat(path.c_str(), &details) == 0 && S_ISDIR(details.st_mode);
}

bool socketExists(const std::string &path)
{
	struct stat details {};
	return ::lstat(path.c_str(), &details) == 0 &&
		S_ISSOCK(details.st_mode);
}

void aRunOwnsItsDirectoryUntilItReturns()
{
	const auto &location = processLocation();
	{
		const ::uml::device::GuestConsoleRun inactive {false};
		require(!directoryExists(location.directory));
	}
	int console = -1;
	{
		const ::uml::device::GuestConsoleRun run {true};
		struct stat details {};
		require(::lstat(location.directory.c_str(), &details) == 0 &&
			S_ISDIR(details.st_mode) && details.st_uid == ::geteuid() &&
			(details.st_mode & 07777) == S_IRWXU);
		/* The guest binds its console there, and a run killed before the
		 * guest shut down keeps it. */
		console = bindServer(location.console);
	}
	require(!directoryExists(location.directory));
	(void)::close(console);
}

void theTerminationPathRemovesTheRunDirectory()
{
	const auto &location = processLocation();
	const ::uml::device::GuestConsoleRun run {true};
	const int console = bindServer(location.console);
	/* What darwin_uml_exit, darwin_uml_abort and the host's signal
	 * handlers run; the guest's SIGINT and SIGTERM handler exits through
	 * darwin_uml_exit. */
	::uml::device::remove_registered_socket_paths();
	require(!directoryExists(location.directory));
	(void)::close(console);
}

void aDirectoryLeftByThisPidIsReplaced()
{
	const auto &location = processLocation();
	require(::mkdir(location.directory.c_str(), S_IRWXU) == 0);
	const int stale = bindServer(location.console);
	{
		const ::uml::device::GuestConsoleRun run {true};
		require(directoryExists(location.directory) &&
			!socketExists(location.console));
	}
	require(!directoryExists(location.directory));
	(void)::close(stale);
}

/* A pid whose process has exited and been reaped. */
pid_t exitedPid()
{
	const pid_t child = ::fork();
	require(child >= 0);
	if (child == 0)
		::_exit(0);
	int status = 0;
	require(::waitpid(child, &status, 0) == child);
	return child;
}

void createFile(const std::filesystem::path &path)
{
	require(::close(::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY,
		S_IRUSR | S_IWUSR)) == 0);
}

void aRunRemovesOnlyWhatExitedRunsLeft()
{
	const auto parent =
		std::filesystem::path {processLocation().directory}.parent_path();
	const auto dead = std::to_string(exitedPid());
	const auto live = std::to_string(::getppid());
	const auto abandoned = parent / ("uml-kern" + dead);
	const auto liveRun = parent / ("uml-kern" + live);
	const auto padded = parent / ("uml-kern0" + dead);
	const auto file = parent / ("uml-kern" + std::to_string(exitedPid()));
	/* A name that only begins like a run's. */
	const auto foreign = parent / ("uml-kern" + dead + "-mconsole");
	/* init is a live process that this user may not signal. */
	const auto unsignalable = parent / "uml-kern1";
	for (const auto &directory :
		{abandoned, liveRun, padded, foreign, unsignalable})
		require(std::filesystem::create_directory(directory));
	const int abandonedConsole = bindServer(abandoned / "mconsole");
	createFile(file);

	{
		const ::uml::device::GuestConsoleRun run {true};
	}
	/* Only an exited process's run directory goes. A live run's, one this
	 * user may not signal, another program's names and anything of another
	 * file type stay. */
	require(!std::filesystem::exists(abandoned));
	require(directoryExists(liveRun) && directoryExists(padded) &&
		directoryExists(unsignalable) && directoryExists(foreign) &&
		std::filesystem::is_regular_file(file));

	(void)::close(abandonedConsole);
	for (const auto &path : {liveRun, padded, file, foreign, unsignalable})
		std::filesystem::remove_all(path);
}

/* A writeback finds the console's queue full: the retries for room run inside
 * the request's one deadline, with the reply's wait, not before it. A guest
 * that reads in time is asked and answers within the budget; one that never
 * reads leaves nothing sent when the budget ends, and the budget is spent
 * once; one that reads late and then answers later still is unanswered at the
 * budget, not after room and a fresh wait. */
void aWritebackFindingNoRoomStaysInItsBudget()
{
	using ::uml::device::GuestWriteback;
	using ::uml::device::request_guest_writeback;
	using namespace std::chrono_literals;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	const int server = bindServer(path);
	const auto elapsed = [](std::chrono::steady_clock::time_point since) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - since);
	};

	/* Room comes back after 300 ms, and the guest then answers at once. */
	require(fillTheConsoleQueue(path) > 0U);
	std::thread reading {[&] {
		std::this_thread::sleep_for(300ms);
		sockaddr_un client {};
		socklen_t clientLength {};
		while (awaitRequest(server, 5000)) {
			if (receiveRequest(server, client, clientLength) == "sync") {
				(void)sendReply(server, client, clientLength, 0U,
					"synced in 5 ms");
				return;
			}
		}
	}};
	auto started = std::chrono::steady_clock::now();
	require(request_guest_writeback(2000ms, 0U) == GuestWriteback::done);
	auto took = elapsed(started);
	reading.join();
	require(took >= 250ms && took < 1500ms,
		"the writeback did not wait for room, or waited past its answer");
	require(!awaitRequest(server, 200));

	/* The guest never reads: nothing is sent, and the budget is spent once. */
	require(fillTheConsoleQueue(path) > 0U);
	started = std::chrono::steady_clock::now();
	require(request_guest_writeback(400ms, 0U) == GuestWriteback::unreachable);
	took = elapsed(started);
	require(took >= 350ms && took < 900ms,
		"a writeback that found no room did not end with its budget");
	sockaddr_un client {};
	socklen_t clientLength {};
	while (awaitRequest(server, 200))
		require(receiveRequest(server, client, clientLength) == "fill",
			"a writeback that was not sent reached the console");

	/* Room after 300 ms, the answer only after 900: the budget of 500 ms is
	 * all there is. */
	require(fillTheConsoleQueue(path) > 0U);
	std::thread late {[&] {
		std::this_thread::sleep_for(300ms);
		sockaddr_un from {};
		socklen_t fromLength {};
		while (awaitRequest(server, 3000)) {
			if (receiveRequest(server, from, fromLength) == "sync") {
				std::this_thread::sleep_for(600ms);
				(void)sendReply(server, from, fromLength, 0U,
					"synced in 5 ms");
				return;
			}
		}
	}};
	started = std::chrono::steady_clock::now();
	require(request_guest_writeback(500ms, 0U) == GuestWriteback::unanswered);
	took = elapsed(started);
	late.join();
	require(took >= 450ms && took < 800ms,
		"the time spent finding room was waited for again");
	(void)::close(server);
}

/* Runs a group under a private $TMPDIR, which must hold this process's run
 * directory. */
void runPrivately(void (*group)())
{
	/* The guest's console is not narrowed to its owner until the host
	 * sends to it, whatever the umask. */
	(void)::umask(0);
	const uml::testing::PrivateTemporaryDirectory temporary {
		"uml-console-test.XXXXXX"};
	require(std::filesystem::path {processLocation().directory}
			.parent_path() == temporary.path());
	group();
}

} // namespace

int main(int argc, char **argv)
{
	const std::string_view selected = argc > 1 ? argv[1] : "";
	if (selected == "location") {
		aDirectoryHoldsShortNamesThatFit();
		aDirectoryThatDoesNotFitIsRefused();
		aDirectoryMustBeAbsolute();
		aRunDirectoryGoesInTheFirstParentThatFits();
		theProcessNamesItsOwnDirectory();
	} else if (selected == "run") {
		runPrivately([] {
			aRunOwnsItsDirectoryUntilItReturns();
			theTerminationPathRemovesTheRunDirectory();
			aDirectoryLeftByThisPidIsReplaced();
			aRunRemovesOnlyWhatExitedRunsLeft();
		});
	} else if (selected == "wake") {
		runPrivately(aWakeSendsOneWakeRequest);
	} else if (selected == "writeback") {
		runPrivately(aWritebackIsOneSync);
	} else if (selected == "writeback-timeout") {
		runPrivately(aSyncThatIsNotAnsweredInTimeIsUnconfirmed);
	} else if (selected == "writeback-strict") {
		runPrivately(aWritebackReplyIsValidatedStrictly);
	} else if (selected == "writeback-budget") {
		runPrivately([] {
			aWritebackWaitsOneBudgetForEverything();
			aWritebackFindingNoRoomStaysInItsBudget();
		});
	} else if (selected == "suspend-failure") {
		runPrivately([] {
			aBusySuspendIsRetriedUntilTheDeadline();
			aLateBusyRefusalIsRetriedOnlyWithinTheDeadline();
			aRetryThatSucceedsSuspendsTheGuest();
			aRefusalThatIsNotBusyIsNotRetried();
		});
	} else if (selected == "trim") {
		runPrivately([] {
			trimmingIsSwitchedByOneRequest();
			aNoWaitTrimRequestReturnsAtOnce();
			aTrimPassIsWaitedForUpToItsBound();
			aRequestWaitsForRoomInAFullQueueWithinItsWait();
		});
	} else if (selected == "memory") {
		runPrivately([] {
			theMemoryReplyIsParsedStrictly();
			aMemoryRequestReadsTheGuestsFigures();
			aSilentGuestIsWaitedForUpToTheBound();
			aWaitingMemoryRequestDoesNotHoldTheConsole();
			aMemoryRequestDoesNotWaitForRoomInAFullQueue();
		});
	} else if (selected == "suspend-statement") {
		runPrivately(aSuspendStatesWhetherItSynced);
	} else if (selected == "suspend-timeout") {
		runPrivately(aSuspendThatDoesNotReplyIsAborted);
	} else if (selected == "suspend-silent") {
		runPrivately(aSilentSuspendIsUnconfirmed);
	} else if (selected == "suspend-suspended-after-wake") {
		runPrivately(aSuspendedReplyAfterTheWakeIsUnconfirmed);
	} else if (selected == "suspend-partial-refusal") {
		runPrivately(aReplyRefusedInItsFirstPartStaysRefused);
	} else if (selected == "suspend-retry-deadline") {
		runPrivately(aBusyRetryCannotOutlastTheDeadline);
	} else {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
