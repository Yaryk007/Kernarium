// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"

#include "sockets/socket_paths.hpp"

#include <TargetConditionals.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace uml::device {

namespace {

/* arch/um/drivers/mconsole.h. The guest refuses anything else. */
constexpr std::uint32_t consoleMagic = 0xcafebabeU;
constexpr std::uint32_t consoleVersion = 2U;
constexpr std::size_t maximumConsoleData = 512U;

struct Request final {
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

struct Reply final {
	std::uint32_t error;
	std::uint32_t more;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

enum class ExchangeResult {
	notSent,
	/* No complete reply arrived by the deadline. */
	pending,
	accepted,
	/* A part of the reply carried the error flag. */
	rejected,
	/* A datagram is no reply of the kind the guest's driver sends: shorter than
	 * its header, or advertising more text than it carries. What it said
	 * cannot be trusted either way. */
	malformed,
};

/* The final reply of a SysRq request names its key and whether the operation
 * ran (arch/um/drivers/mconsole_kern.c with_console()). The text the
 * operation prints is no answer: a console may print it after the final
 * reply, from another vCPU, or not at all. */
constexpr std::string_view writebackAcknowledgement = "sysrq s ok";
constexpr std::string_view writebackDisabled = "sysrq s disabled";
/* The "sync" command's reply is "synced in <n> ms", n in decimal, once sync(2)
 * has returned (arch/um/drivers/mconsole_kern.c mconsole_sync()). A kernel
 * without the command answers every unknown command with an error whose whole
 * text is this (mconsole_user.c); text that merely starts with it is another
 * refusal. */
constexpr std::string_view syncAcknowledgementHead = "synced in ";
constexpr std::string_view syncAcknowledgementTail = " ms";
constexpr std::string_view unknownCommand = "Unknown command";
/* The suspend's reply (arch/um/drivers/mconsole_kern.c
 * um_suspend_notify_entered()) says whether the suspend's own sync returned.
 * A kernel without the statement replies "suspended". Each text still holds
 * the word an older host looks for, and only these two whole texts state
 * anything: another that merely holds the word is no statement. */
constexpr std::string_view suspendAcknowledgement = "suspended";
constexpr std::string_view suspendSynced = "suspended, synced";
constexpr std::string_view suspendNotSynced = "suspended, not synced";
/* A guest reports a suspend it gave up with "Suspend failed: <errno>". EBUSY
 * (16) is the one a moment later can cure: a wake source fired while the guest
 * synced its filesystems, or another power transition held the lock. */
constexpr std::string_view suspendBusy = "Suspend failed: -16";
/* A busy suspend is asked for again at this interval, for this long after the
 * first refusal, and then given up. */
constexpr std::chrono::milliseconds suspendRetryInterval {250};
constexpr std::chrono::milliseconds suspendRetryWindow {
	std::chrono::seconds {2}};

/* The guest answers most requests at once, so one that has no reply within
 * replyWait is left pending. It answers a suspend only once it has synced its
 * filesystems and reached the sleep boundary, or given the suspend up, which
 * can take longer: a guest with much dirty data syncs for seconds, and one
 * whose own wake sources kept ending the suspend has entered it several times.
 * The whole suspend, the first request and the busy retries after it, has one
 * deadline from the first send. The wait ends with the reply, so only such a
 * guest spends it, and it leaves most of the time iOS grants an application it
 * backgrounds. A suspend with no reply by then is aborted, not trusted: a
 * guest whose freezer is held keeps trying for 80 s and then fails. */
constexpr std::chrono::milliseconds replyWait {std::chrono::seconds {2}};
constexpr std::chrono::milliseconds suspendDeadline {std::chrono::seconds {10}};
/* A busy retry whose reply could not arrive before the deadline is not sent. */
constexpr std::chrono::milliseconds suspendRetryGuard {
	std::chrono::seconds {1}};
/* A request that finds no room in the console's queue tries again at this
 * interval, within its own reply wait. */
constexpr std::chrono::milliseconds sendRetryPause {2};

/* A reply socket's identifier is this many hexadecimal digits, so the widest
 * reply path is known before any request is sent. */
constexpr std::size_t clientIdDigits = 8U;
constexpr std::size_t socketPathLimit = sizeof(sockaddr_un {}.sun_path);

/* arch/um/os-Darwin/umid.c names the console <uml_dir>/mconsole. */
constexpr std::string_view consoleName = "mconsole";
/* A run's directory is uml-<umid>, and the umid is kern<pid>. */
constexpr std::string_view runDirectoryPrefix = "uml-";
constexpr std::string_view umidPrefix = "kern";

/* An iOS device application owns its temporary directory and uses it
 * directly; everywhere else a run makes a directory of its own. */
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
constexpr bool ownRunDirectory = false;
#else
constexpr bool ownRunDirectory = true;
#endif

/* Timed, so that a request with a budget gives up on the console as well. */
std::timed_mutex consoleMutex;
bool consoleAdopted;
std::unique_ptr<SocketPath> consoleGuard;
std::unique_ptr<DirectoryPath> runDirectoryGuard;
std::atomic_uint32_t nextClientId;

/* One console per process. Two guests in one process cannot both own the
 * kernel's single console anyway, and embedded runs are serialized. */
[[nodiscard]] const std::string &consoleUmid()
{
	static const std::string umid =
		std::string {umidPrefix} + std::to_string(::getpid());
	return umid;
}

/* Where this process's console goes, and the directory named in a report
 * when no location fits. */
[[nodiscard]] std::optional<GuestConsoleLocation> composeProcessLocation(
	std::string &described)
{
	const char *temporary = std::getenv("TMPDIR");
	described = temporary == nullptr ? "" : temporary;
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
	return compose_guest_console_location(described);
#else
	const std::array<std::string, 3> parents {
		described, user_temporary_directory(), "/tmp"};
	described = "$TMPDIR, " + parents[1] + " or /tmp";
	return compose_run_console_location(parents, consoleUmid());
#endif
}

/* The pid a run's name, uml-kern<pid>, records, if `name` is one. */
[[nodiscard]] std::optional<pid_t> runDirectoryOwner(std::string_view name)
{
	if (!name.starts_with(runDirectoryPrefix))
		return std::nullopt;
	name.remove_prefix(runDirectoryPrefix.size());
	if (!name.starts_with(umidPrefix))
		return std::nullopt;
	name.remove_prefix(umidPrefix.size());
	pid_t owner = 0;
	const auto [end, error] =
		std::from_chars(name.data(), name.data() + name.size(), owner);
	if (name.starts_with('0') || error != std::errc {} ||
	    end != name.data() + name.size() || owner <= 0)
		return std::nullopt;
	return owner;
}

/*
 * A run killed outright leaves its directory behind, so each run removes what
 * runs that have exited left in `parent`: directories named uml-kern<pid> that
 * this user owns, whose process no longer exists. A live process, or one this
 * user may not signal, keeps its directory, and nothing else is touched.
 */
void removeAbandonedRunDirectories(const std::filesystem::path &parent)
{
	std::error_code error;
	for (std::filesystem::directory_iterator entry {parent, error};
	     !error && entry != std::filesystem::directory_iterator {};
	     entry.increment(error)) {
		const auto filename = entry->path().filename();
		const auto owner = runDirectoryOwner(filename.native());
		if (!owner || *owner == ::getpid())
			continue;
		struct stat details {};
		if (::lstat(entry->path().c_str(), &details) != 0 ||
		    !S_ISDIR(details.st_mode) || details.st_uid != ::geteuid())
			continue;
		if (::kill(*owner, 0) == 0 || errno != ESRCH)
			continue;
		std::error_code ignored;
		std::filesystem::remove_all(entry->path(), ignored);
	}
}

/*
 * Creates the run's directory, readable by this user alone because the console
 * accepts requests that stop the guest. One this user owns already at the name
 * was left by an earlier process with this pid, and is replaced.
 */
[[nodiscard]] bool createRunDirectory(const std::string &directory)
{
	if (::mkdir(directory.c_str(), S_IRWXU) == 0)
		return true;
	if (errno != EEXIST)
		return false;
	struct stat details {};
	if (::lstat(directory.c_str(), &details) != 0 ||
	    !S_ISDIR(details.st_mode) || details.st_uid != ::geteuid())
		return false;
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	return !error && ::mkdir(directory.c_str(), S_IRWXU) == 0;
}

void removeConsoleSocket(const GuestConsoleLocation &location)
{
	struct stat details {};
	if (::lstat(location.console.c_str(), &details) == 0 &&
	    S_ISSOCK(details.st_mode))
		(void)::unlink(location.console.c_str());
}

void releaseConsoleGuard()
{
	consoleGuard.reset();
	consoleAdopted = false;
}

[[nodiscard]] bool fillAddress(sockaddr_un &address, const std::string &path)
{
	if (path.size() >= sizeof(address.sun_path))
		return false;
	address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
	return true;
}

/*
 * The reply comes back to the sender's address, so the client has to be bound.
 * Darwin does not autobind a Unix datagram socket, so this binds next to the
 * guest's console. Each request binds a new name, so a reply that arrives
 * after its request gave up finds no socket instead of the next request's. A
 * reply that has not arrived within a receive's wait is left pending, and the
 * client may receive again: a request that matters past its wait keeps its
 * client, and with it the socket, for as long as it is read.
 */
class Client final {
public:
	explicit Client(const GuestConsoleLocation &location)
	{
		std::array<char, clientIdDigits + 1U> id {};
		(void)std::snprintf(id.data(), id.size(), "%08x",
			static_cast<unsigned int>(nextClientId.fetch_add(
				1U, std::memory_order_relaxed)));
		path_ = location.client_prefix + id.data();
		(void)::unlink(path_.c_str());
		if (!fillAddress(console_, location.console))
			return;
		descriptor_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
		if (descriptor_ < 0)
			return;
		sockaddr_un address {};
		if (!fillAddress(address, path_) ||
		    ::bind(descriptor_, reinterpret_cast<sockaddr *>(&address),
			    sizeof(address)) != 0) {
			::close(descriptor_);
			descriptor_ = -1;
			return;
		}
		/* A terminating signal while the request waits would otherwise
		 * leave the reply socket, and with it the run's directory. */
		try {
			auto guard = std::make_unique<SocketPath>(path_);
			guard->adopt();
			guard_ = std::move(guard);
		} catch (...) {
		}
	}

	~Client()
	{
		if (descriptor_ >= 0)
			::close(descriptor_);
		guard_.reset();
		(void)::unlink(path_.c_str());
	}

	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	[[nodiscard]] bool usable() const noexcept { return descriptor_ >= 0; }

	/* One attempt, which does not wait for room in the console's queue. The
	 * queue holds a few requests, and Darwin refuses a datagram that does not
	 * fit at once (ENOBUFS), whether the send blocks or not, instead of
	 * waiting for room as Linux does. False when nothing was sent; `full` then
	 * says whether the reason was the lack of room. */
	[[nodiscard]] bool trySend(std::string_view command,
		bool *full = nullptr) const noexcept
	{
		Request request {consoleMagic, consoleVersion,
			static_cast<std::uint32_t>(command.size()), {}};
		if (full != nullptr)
			*full = false;
		if (!usable() || command.size() >= request.data.size())
			return false;
		std::memcpy(request.data.data(), command.data(), command.size());
		const auto sent = ::sendto(descriptor_, &request, sizeof(request),
			MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&console_),
			sizeof(console_));
		if (sent == static_cast<ssize_t>(sizeof(request)))
			return true;
		if (full != nullptr && sent < 0)
			*full = errno == ENOBUFS || errno == EAGAIN;
		return false;
	}

	/* Sends the request, and while the console's queue has no room tries
	 * again a short while apart, until `deadline`, the request's own reply
	 * deadline. False when nothing was sent, which a lack of room still is
	 * at the deadline. */
	[[nodiscard]] bool send(std::string_view command,
		std::chrono::steady_clock::time_point deadline) const noexcept
	{
		for (;;) {
			bool full = false;
			if (trySend(command, &full))
				return true;
			const auto now = std::chrono::steady_clock::now();
			if (!full || now >= deadline)
				return false;
			std::this_thread::sleep_for(
				std::min<std::chrono::steady_clock::duration>(
					sendRetryPause, deadline - now));
		}
	}

	/* Collects the reply to the request, waiting until `deadline` for it as a
	 * whole, every part of a multipart reply included. A deadline that has
	 * passed only takes what has arrived. A reply that is not complete by then
	 * leaves the request pending, and the client may receive again.
	 *
	 * The parts that arrived before a deadline stay with the client, text and
	 * error flag both: the next receive continues the reply and delivers it
	 * whole, so the end of a reply that was refused at its start is still a
	 * refusal. `response` takes nothing until the reply is complete.
	 *
	 * The error flag of any part makes the reply rejected, though the guest
	 * sets it on the first part only. A datagram that is shorter than the
	 * reply header, longer than a reply can be, or advertises more text than
	 * it carries, is malformed, and ends the reply it was part of. */
	[[nodiscard]] ExchangeResult receive(std::string &response,
		Deadline deadline) const
	{
		constexpr auto header = static_cast<ssize_t>(sizeof(Reply) -
			maximumConsoleData);
		for (;;) {
			const auto remaining =
				std::chrono::duration_cast<std::chrono::microseconds>(
					deadline - Clock::now());
			int flags = 0;
			if (remaining.count() <= 0) {
				flags = MSG_DONTWAIT;
			} else {
				const timeval timeout {
					.tv_sec = static_cast<decltype(timeval::tv_sec)>(
						remaining.count() / 1000000),
					.tv_usec = static_cast<decltype(timeval::tv_usec)>(
						remaining.count() % 1000000)};
				(void)::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO,
					&timeout, sizeof(timeout));
			}
			/* One byte more than any reply is long, so that a datagram
			 * that is longer, which recv would cut to fit, shows. */
			std::array<char, sizeof(Reply) + 1U> datagram {};
			const auto received = ::recv(descriptor_, datagram.data(),
				datagram.size(), flags);
			if (received < 0) {
				if (errno == EINTR)
					continue;
				return ExchangeResult::pending;
			}
			if (received < header ||
			    received > static_cast<ssize_t>(sizeof(Reply))) {
				partial_.clear();
				partialRejected_ = false;
				return ExchangeResult::malformed;
			}
			Reply reply {};
			std::memcpy(&reply, datagram.data(),
				static_cast<std::size_t>(received));
			if (reply.length > maximumConsoleData ||
			    reply.length > static_cast<std::size_t>(received - header)) {
				partial_.clear();
				partialRejected_ = false;
				return ExchangeResult::malformed;
			}
			auto length = static_cast<std::size_t>(reply.length);
			/* The guest's driver counts the NUL that ends each reply in
			 * the reply's length. It ends the text and is no part of it. */
			if (length != 0U && reply.data[length - 1U] == '\0')
				--length;
			partial_.append(reply.data.data(), length);
			partialRejected_ = partialRejected_ || reply.error != 0U;
			if (reply.more == 0U) {
				const bool rejected = partialRejected_;
				response.append(partial_);
				partial_.clear();
				partialRejected_ = false;
				return rejected ? ExchangeResult::rejected :
					ExchangeResult::accepted;
			}
		}
	}

	/* The same, waiting up to `wait` from now. */
	[[nodiscard]] ExchangeResult receive(std::string &response,
		std::chrono::milliseconds wait) const
	{
		return receive(response, Clock::now() + wait);
	}

private:
	int descriptor_ {-1};
	std::string path_;
	sockaddr_un console_ {};
	std::unique_ptr<SocketPath> guard_;
	/* The parts of the reply that a receive took before its deadline, and
	 * whether any of them carried the error flag. One thread reads a client. */
	mutable std::string partial_;
	mutable bool partialRejected_ {false};
};

/*
 * The guest created this socket, so it can only be adopted once it exists.
 * The location belongs to this process, which makes anything at that path
 * ours; tightening the mode keeps the guest's console away from other local
 * users, because a console accepts requests that stop the guest.
 */
void adoptConsolePath(const std::string &path)
{
	if (consoleAdopted)
		return;
	struct stat details {};
	if (::lstat(path.c_str(), &details) != 0 || !S_ISSOCK(details.st_mode))
		return;
	(void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
	try {
		auto guard = std::make_unique<SocketPath>(path);
		guard->adopt();
		consoleGuard = std::move(guard);
		consoleAdopted = true;
	} catch (...) {
		/* Cleanup on a terminating signal is a convenience; failing to
		 * arrange it must not fail the writeback request itself. */
	}
}

/* Whether `text` is a count of decimal digits that fits 64 bits, and the
 * count. No sign, no padding. */
[[nodiscard]] bool parseByteCount(std::string_view text,
	std::uint64_t &count) noexcept
{
	if (text.empty())
		return false;
	const auto [end, error] =
		std::from_chars(text.data(), text.data() + text.size(), count);
	return error == std::errc {} && end == text.data() + text.size();
}

/* Sends one command to the location's console from a client of its own and
 * collects the reply. `deadline` is the request's whole: the console's queue
 * may have no room for the request, which is then tried again a short while
 * apart (Client::send), and the time that takes is not waited for again by the
 * reply. A queue that still has no room at the deadline is nothing sent. */
[[nodiscard]] ExchangeResult exchange(
	const GuestConsoleLocation &location,
	std::string_view command,
	std::string &response,
	Deadline deadline)
{
	const Client client {location};
	if (!client.send(command, deadline))
		return ExchangeResult::notSent;
	return client.receive(response, deadline);
}

/* The same, for a wait counted from now. A reply that was asked to be waited
 * for gets at least a millisecond after a send that used the wait up in
 * finding room, and a zero wait only takes what has arrived. */
[[nodiscard]] ExchangeResult exchange(
	const GuestConsoleLocation &location,
	std::string_view command,
	std::string &response,
	std::chrono::milliseconds wait = replyWait)
{
	const auto deadline = Clock::now() + std::max(wait, std::chrono::milliseconds {0});
	const Client client {location};
	if (!client.send(command, deadline))
		return ExchangeResult::notSent;
	return client.receive(response, wait.count() > 0 ?
		std::max(deadline, Clock::now() + std::chrono::milliseconds {1}) :
		deadline);
}

/* Whether `text` is the "sync" command's whole reply, "synced in <n> ms", n a
 * decimal count that fits 64 bits. Nothing else is a confirmation: a reply that
 * only starts alike, or whose count is no number, is something else. */
[[nodiscard]] bool isSyncAcknowledgement(std::string_view text) noexcept
{
	if (!text.starts_with(syncAcknowledgementHead) ||
	    !text.ends_with(syncAcknowledgementTail) ||
	    text.size() <= syncAcknowledgementHead.size() +
		    syncAcknowledgementTail.size())
		return false;
	const auto digits = text.substr(syncAcknowledgementHead.size(),
		text.size() - syncAcknowledgementHead.size() -
			syncAcknowledgementTail.size());
	std::uint64_t count = 0U;
	return parseByteCount(digits, count);
}

/* The fallback of a kernel without "sync": SysRq 's', which the guest runs
 * as an emergency sync on a workqueue and acknowledges once it is queued. The
 * request, its reply and the settling period that follows all end by
 * `deadline`: a request that cannot be answered in time is not sent. */
[[nodiscard]] GuestWriteback requestSysrqWriteback(
	const GuestConsoleLocation &location, Deadline deadline,
	unsigned int settleMs)
{
	if (Clock::now() >= deadline)
		return GuestWriteback::unanswered;
	std::string response;
	switch (exchange(location, "sysrq s", response, deadline)) {
	case ExchangeResult::notSent:
		return GuestWriteback::unreachable;
	case ExchangeResult::pending:
		return GuestWriteback::unanswered;
	case ExchangeResult::malformed:
		return GuestWriteback::refused;
	case ExchangeResult::rejected:
		/* The final reply comes last, after any console text. */
		if (response.ends_with(writebackDisabled)) {
			std::fputs(
				"Guest console refused the filesystem writeback; "
				"the guest has magic SysRq disabled\n",
				stderr);
		}
		return GuestWriteback::refused;
	case ExchangeResult::accepted:
		break;
	}
	if (!response.ends_with(writebackAcknowledgement))
		return GuestWriteback::refused;
	/* The guest reports that the writeback started, not that it finished, so
	 * give it a bounded chance to reach the block layer before the host flushes
	 * what it received, as far as the request's budget allows. */
	const auto settle = std::min(std::chrono::milliseconds {settleMs},
		std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - Clock::now()));
	if (settle.count() > 0)
		std::this_thread::sleep_for(settle);
	return GuestWriteback::started;
}

[[nodiscard]] GuestWriteback requestWriteback(
	const GuestConsoleLocation &location, Deadline deadline,
	std::chrono::milliseconds wait, unsigned int settleMs)
{
	std::string response;
	switch (exchange(location, "sync", response, deadline)) {
	case ExchangeResult::notSent:
		return GuestWriteback::unreachable;
	case ExchangeResult::pending:
		std::fprintf(stderr,
			"Guest console: the guest did not confirm its filesystem sync "
			"within %lld ms; it may still be writing\n",
			static_cast<long long>(wait.count()));
		return GuestWriteback::unanswered;
	case ExchangeResult::malformed:
		std::fputs("Guest console: the guest's filesystem sync reply is "
			"malformed\n", stderr);
		return GuestWriteback::refused;
	case ExchangeResult::rejected:
		if (response == unknownCommand)
			return requestSysrqWriteback(location, deadline, settleMs);
		std::fprintf(stderr,
			"Guest console: the guest refused its filesystem sync (%s)\n",
			response.c_str());
		return GuestWriteback::refused;
	case ExchangeResult::accepted:
		break;
	}
	if (!isSyncAcknowledgement(response)) {
		std::fprintf(stderr,
			"Guest console: the guest's filesystem sync reply is no "
			"confirmation (%s)\n", response.c_str());
		return GuestWriteback::refused;
	}
	return GuestWriteback::done;
}

} // namespace

std::optional<GuestConsoleLocation> compose_guest_console_location(
	std::string_view directory)
{
	/* arch/um/os-Darwin/umid.c composes <uml_dir>/<name> after dropping the
	 * same trailing slashes. */
	while (directory.size() > 1U && directory.ends_with('/'))
		directory.remove_suffix(1U);
	if (!directory.starts_with('/'))
		return std::nullopt;
	GuestConsoleLocation location;
	location.directory = directory;
	location.console =
		location.directory + "/" + std::string {consoleName};
	location.client_prefix = location.directory + "/mc-";
	if (location.console.size() >= socketPathLimit ||
	    location.client_prefix.size() + clientIdDigits >= socketPathLimit)
		return std::nullopt;
	return location;
}

std::optional<GuestConsoleLocation> compose_run_console_location(
	std::span<const std::string> parents, std::string_view umid)
{
	for (std::string_view parent : parents) {
		while (parent.size() > 1U && parent.ends_with('/'))
			parent.remove_suffix(1U);
		if (!parent.starts_with('/'))
			continue;
		auto directory = std::string {parent};
		if (directory != "/")
			directory += '/';
		directory += runDirectoryPrefix;
		directory += umid;
		if (auto location = compose_guest_console_location(directory))
			return location;
	}
	return std::nullopt;
}

const std::optional<GuestConsoleLocation> &guest_console_location()
{
	static const std::optional<GuestConsoleLocation> location = [] {
		std::string described;
		auto composed = composeProcessLocation(described);
		if (!composed) {
			std::fprintf(stderr,
				"Guest management console: no console under %s: "
				"it needs an absolute directory whose socket paths fit "
				"in %zu bytes; the host cannot reach the guest's console\n",
				described.c_str(), socketPathLimit);
		}
		return composed;
	}();
	return location;
}

std::vector<std::string> guest_management_console_arguments()
{
	std::vector<std::string> arguments {"umid=" + consoleUmid()};
	if (const auto &location = guest_console_location())
		arguments.push_back("uml_dir=" + location->directory);
	return arguments;
}

GuestWriteback request_guest_writeback(std::chrono::milliseconds wait,
	unsigned int settleMs) noexcept
{
	try {
		/* One deadline for the whole request: the console's lock, every
		 * reply, the fallback and its settling period. A zero wait would
		 * not wait at all. */
		wait = std::max(wait, std::chrono::milliseconds {1});
		const auto deadline = Clock::now() + wait;
		std::unique_lock lock(consoleMutex, std::defer_lock);
		if (!lock.try_lock_until(deadline)) {
			std::fprintf(stderr,
				"Guest console: another request kept the console for %lld "
				"ms; no filesystem sync was sent\n",
				static_cast<long long>(wait.count()));
			return GuestWriteback::busy;
		}
		const auto &location = guest_console_location();
		if (!location)
			return GuestWriteback::unreachable;
		adoptConsolePath(location->console);
		return requestWriteback(*location, deadline, wait, settleMs);
	} catch (...) {
		return GuestWriteback::unreachable;
	}
}

namespace {

/* A guest that accepted a suspend: it said it reached the sleep boundary,
 * and a kernel with the statement said whether its sync returned. */
[[nodiscard]] GuestSuspension suspendAccepted(const std::string &response,
	GuestSuspendSync &sync)
{
	if (response == suspendSynced) {
		sync = GuestSuspendSync::synced;
	} else if (response == suspendNotSynced) {
		sync = GuestSuspendSync::skipped;
	} else if (response.find(suspendAcknowledgement) == std::string::npos) {
		std::fputs("Guest console returned an unexpected suspend reply\n",
			stderr);
	}
	return GuestSuspension::system;
}

/* A guest that refused a suspend, or kept refusing. It runs on and the caller
 * does not treat it as asleep: the console's own stop would halt only the
 * vCPU that takes it, since the kernel blocks signals per vCPU, so it is no
 * substitute for a suspend. A suspend that holds syncs the filesystems itself;
 * one that was refused has not, and the caller asks for a writeback. */
[[nodiscard]] GuestSuspension suspendRefused(const std::string &response)
{
	std::fprintf(stderr,
		"Guest console: the guest could not suspend; it keeps running "
		"(%s)\n", response.c_str());
	return GuestSuspension::none;
}

/*
 * A suspend that has not replied by its deadline is aborted, not trusted. A
 * guest whose freezer cannot finish keeps trying for tens of seconds and then
 * fails, so a host that took the silence for a suspended guest would
 * hibernate one that runs. The wake edge is an interrupt command that the
 * guest answers while its suspend blocks, and it makes the PM core give an
 * attempt up before the devices are touched. The request's own socket, kept
 * for this, then carries the guest's final word.
 *
 * Only a reply that arrived before the wake proves the suspend: the guest
 * replies "suspended" before it checks for a wakeup, so one that follows the
 * wake does not say the guest slept. A refusal is a guest known to run;
 * anything else is unconfirmed, and its resume wakes it.
 */
[[nodiscard]] GuestSuspension abortSuspend(
	const GuestConsoleLocation &location, const Client &request,
	GuestSuspendSync &sync)
{
	std::string response;
	switch (request.receive(response, std::chrono::milliseconds {0})) {
	case ExchangeResult::accepted:
		return suspendAccepted(response, sync);
	case ExchangeResult::rejected:
		return suspendRefused(response);
	case ExchangeResult::notSent:
	case ExchangeResult::pending:
	case ExchangeResult::malformed:
		break;
	}
	std::string woken;
	(void)exchange(location, "wake", woken);
	response.clear();
	const auto result = request.receive(response, replyWait);
	const bool refused = result == ExchangeResult::rejected;
	std::fprintf(stderr,
		"Guest console: suspend was not confirmed within %lld s; woken, it "
		"keeps running (final reply: %s)\n",
		static_cast<long long>(
			std::chrono::duration_cast<std::chrono::seconds>(
				suspendDeadline).count()),
		result == ExchangeResult::pending ? "none" :
			result == ExchangeResult::malformed ? "malformed" :
			response.c_str());
	return refused ? GuestSuspension::none : GuestSuspension::unconfirmed;
}

} // namespace

GuestSuspension suspend_guest(GuestSuspendSync *sync) noexcept
{
	GuestSuspendSync stated = GuestSuspendSync::unstated;
	if (sync != nullptr)
		*sync = stated;
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return GuestSuspension::none;
		adoptConsolePath(location->console);
		const auto deadline =
			std::chrono::steady_clock::now() + suspendDeadline;
		/* A suspend-capable guest replies only after devices, secondary
		 * CPUs, interrupts, and timekeeping have reached the platform sleep
		 * boundary. The separate wake request remains queued on the console
		 * descriptor until the kernel has resumed it. The request that is
		 * waiting for its reply keeps its socket, which a timeout then
		 * reads again. */
		std::unique_ptr<Client> request;
		std::string response;
		auto result = ExchangeResult::notSent;
		const auto ask = [&] {
			response.clear();
			request = std::make_unique<Client>(*location);
			if (!request->send("suspend", deadline)) {
				result = ExchangeResult::notSent;
				return;
			}
			result = request->receive(response, deadline);
		};
		ask();
		/* A guest that was busy, as when a wake source fired while it synced,
		 * is asked again for a short while, if its answer could still arrive
		 * by the deadline. The refusal ended the guest's suspend, so no
		 * request of ours is still in flight. */
		std::chrono::steady_clock::time_point retryDeadline;
		bool retrying = false;
		while (result == ExchangeResult::rejected &&
		       response.starts_with(suspendBusy)) {
			if (!retrying) {
				retryDeadline = std::chrono::steady_clock::now() +
					suspendRetryWindow;
				retrying = true;
			}
			std::this_thread::sleep_for(suspendRetryInterval);
			const auto now = std::chrono::steady_clock::now();
			if (now >= retryDeadline || deadline - now <= suspendRetryGuard)
				break;
			ask();
		}
		GuestSuspension suspension = GuestSuspension::none;
		switch (result) {
		case ExchangeResult::accepted:
			suspension = suspendAccepted(response, stated);
			break;
		case ExchangeResult::notSent:
			break;
		case ExchangeResult::rejected:
			/* The guest refused, or kept refusing. */
			suspension = suspendRefused(response);
			break;
		case ExchangeResult::pending:
		case ExchangeResult::malformed:
			/* A reply that is no reply proves nothing: it is aborted like
			 * silence. */
			suspension = abortSuspend(*location, *request, stated);
			break;
		}
		if (sync != nullptr)
			*sync = stated;
		return suspension;
	} catch (...) {
		return GuestSuspension::none;
	}
}

void resume_guest(GuestSuspension suspension) noexcept
{
	if (suspension == GuestSuspension::none)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return;
		std::string response;
		(void)exchange(*location, "wake", response);
	} catch (...) {
	}
}

namespace {

[[nodiscard]] GuestRequest requestGuest(std::string_view command,
	std::chrono::milliseconds wait) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return GuestRequest::unreachable;
		adoptConsolePath(location->console);
		std::string response;
		switch (exchange(*location, command, response, wait)) {
		case ExchangeResult::notSent:
			return GuestRequest::unreachable;
		case ExchangeResult::pending:
		case ExchangeResult::malformed:
			return GuestRequest::unanswered;
		case ExchangeResult::rejected:
			return GuestRequest::refused;
		case ExchangeResult::accepted:
			return GuestRequest::done;
		}
		return GuestRequest::refused;
	} catch (...) {
		return GuestRequest::unreachable;
	}
}

} // namespace

bool wake_guest() noexcept
{
	return requestGuest("wake", replyWait) == GuestRequest::done;
}

GuestRequest set_guest_page_cache_trim(bool enabled) noexcept
{
	return requestGuest(enabled ? "trim on" : "trim off", replyWait);
}

GuestRequest set_guest_page_cache_trim_nowait(bool enabled) noexcept
{
	try {
		std::unique_lock lock(consoleMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return GuestRequest::busy;
		const auto &location = guest_console_location();
		if (!location)
			return GuestRequest::unreachable;
		adoptConsolePath(location->console);
		const Client client {*location};
		return client.trySend(enabled ? "trim on" : "trim off") ?
			GuestRequest::sent : GuestRequest::unreachable;
	} catch (...) {
		return GuestRequest::unreachable;
	}
}

GuestRequest trim_guest_page_cache(unsigned int waitMs) noexcept
{
	/* A zero receive timeout would wait for ever. */
	return requestGuest("trim once",
		std::chrono::milliseconds {std::max(waitMs, 1U)});
}

GuestConsoleRun::GuestConsoleRun(bool active) noexcept
	: active_(active)
{
	if (!active_)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return;
		releaseConsoleGuard();
		if constexpr (!ownRunDirectory) {
			removeConsoleSocket(*location);
			return;
		}
		runDirectoryGuard.reset();
		const std::filesystem::path directory {location->directory};
		removeAbandonedRunDirectories(directory.parent_path());
		if (!createRunDirectory(location->directory)) {
			std::fprintf(stderr,
				"Guest management console: cannot create %s: %s; "
				"the host cannot reach the guest's console\n",
				location->directory.c_str(), std::strerror(errno));
			return;
		}
		auto guard = std::make_unique<DirectoryPath>(directory);
		try {
			guard->adopt(std::string {consoleName});
		} catch (...) {
			/* Never a directory that a terminating signal would leave. */
			std::error_code ignored;
			std::filesystem::remove_all(directory, ignored);
			throw;
		}
		runDirectoryGuard = std::move(guard);
	} catch (...) {
	}
}

GuestConsoleRun::~GuestConsoleRun()
{
	if (!active_)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		releaseConsoleGuard();
		if (const auto &location = guest_console_location())
			removeConsoleSocket(*location);
		runDirectoryGuard.reset();
	} catch (...) {
	}
}



std::optional<GuestMemoryFigures> parse_guest_memory_reply(
	std::string_view text) noexcept
{
	if (text.ends_with('\n'))
		text.remove_suffix(1U);
	std::optional<std::uint64_t> total;
	std::optional<std::uint64_t> available;
	std::optional<std::uint64_t> free;
	for (;;) {
		const auto separator = text.find(' ');
		const auto field = text.substr(0U, separator);
		const auto equals = field.find('=');
		if (equals == std::string_view::npos || equals == 0U)
			return std::nullopt;
		const auto key = field.substr(0U, equals);
		std::uint64_t count = 0U;
		if (!parseByteCount(field.substr(equals + 1U), count))
			return std::nullopt;
		std::optional<std::uint64_t> *named = nullptr;
		if (key == "total")
			named = &total;
		else if (key == "available")
			named = &available;
		else if (key == "free")
			named = &free;
		if (named != nullptr) {
			if (named->has_value())
				return std::nullopt;
			*named = count;
		}
		if (separator == std::string_view::npos)
			break;
		text.remove_prefix(separator + 1U);
	}
	if (!total || !available)
		return std::nullopt;
	return GuestMemoryFigures {.total_bytes = *total,
		.available_bytes = *available,
		.free_bytes = free.value_or(0U)};
}

class GuestMemoryQuery::State final {
public:
	explicit State(const GuestConsoleLocation &location) : client(location) {}

	Client client;
};

GuestMemoryQuery::GuestMemoryQuery(std::unique_ptr<State> state) noexcept
	: state_(std::move(state))
{
}

GuestMemoryQuery::GuestMemoryQuery(GuestMemoryQuery &&other) noexcept = default;

GuestMemoryQuery::~GuestMemoryQuery() = default;

std::optional<GuestMemoryQuery> GuestMemoryQuery::start() noexcept
{
	try {
		/* The console's lock covers locating it and sending, not the wait
		 * for the reply, which the caller makes without it. */
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return std::nullopt;
		adoptConsolePath(location->console);
		auto state = std::make_unique<State>(*location);
		/* One attempt, whatever the queue: the console's lock is held, and a
		 * caller's lifecycle lock behind it. The query owns no reply
		 * deadline here, so it does not wait for room. */
		if (!state->client.trySend("mem"))
			return std::nullopt;
		return GuestMemoryQuery(std::move(state));
	} catch (...) {
		return std::nullopt;
	}
}

GuestMemoryResult GuestMemoryQuery::await(std::chrono::milliseconds wait,
	GuestMemoryFigures &figures) noexcept
{
	try {
		std::string response;
		switch (state_->client.receive(response, wait)) {
		case ExchangeResult::notSent:
			return GuestMemoryResult::unreachable;
		case ExchangeResult::pending:
			return GuestMemoryResult::unanswered;
		case ExchangeResult::rejected:
			return GuestMemoryResult::refused;
		case ExchangeResult::malformed:
			return GuestMemoryResult::malformed;
		case ExchangeResult::accepted:
			break;
		}
		const auto parsed = parse_guest_memory_reply(response);
		if (!parsed)
			return GuestMemoryResult::malformed;
		figures = *parsed;
		return GuestMemoryResult::done;
	} catch (...) {
		return GuestMemoryResult::unanswered;
	}
}

} // namespace uml::device
